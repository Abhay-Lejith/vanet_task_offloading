#!/usr/bin/env python3
"""
Train and save an RL model (PPO) on the VeinsGym straight scenario.
- Registers the 'veins-straight-v1' env
- Trains PPO for a configurable number of timesteps
- Saves the model to examples/models

Requirements:
- Python packages: stable-baselines3, torch, gym, numpy, veins_gym
- OMNeT++ installed, and its setenv sourced (path configurable)

Quick install (Linux):
    pip install -U stable-baselines3 torch gym numpy

Note: If OMNeT++ isn't on PATH, set OMNETPP_SETENV or pass --omnetpp-setenv.
"""

import os
import sys
import shutil
import subprocess
import pathlib
import argparse
import gym
import numpy as np
from stable_baselines3 import PPO
from stable_baselines3.common.monitor import Monitor

# -----------------------------
# RL+PSO Hybrid Components
# -----------------------------
class PSOPlanner:
    """Discrete-action PSO planner with an online surrogate fitness model.

    - Searches over discrete actions (0..num_actions-1) using PSO where particle
      positions are real-valued but rounded to nearest discrete action for eval.
    - Fitness is predicted by a simple online linear model trained on
      (obs, one-hot(action)) -> observed reward from the environment.
    - RL agent outputs PSO hyperparameters (w, c1, c2) which adapt the search.
    """

    def __init__(self, num_actions: int = 4, max_samples: int = 2000, ridge_lambda: float = 1e-3):
        self.num_actions = int(num_actions)
        self.max_samples = int(max_samples)
        self.ridge_lambda = float(ridge_lambda)
        self.X = None  # shape (n, d)
        self.y = None  # shape (n,)
        self.w = None  # shape (d,)
        self._feature_dim = None

    def _featurize(self, obs: np.ndarray, action: int) -> np.ndarray:
        obs = np.asarray(obs, dtype=np.float32).ravel()
        if self._feature_dim is None:
            self._feature_dim = obs.shape[0] + self.num_actions
        a_oh = np.zeros((self.num_actions,), dtype=np.float32)
        a_oh[int(action) % self.num_actions] = 1.0
        return np.concatenate([obs, a_oh]).astype(np.float32)

    def predict(self, obs: np.ndarray, action: int) -> float:
        if self.w is None:
            # cold start: prefer middle actions slightly to avoid bias
            return 0.0
        x = self._featurize(obs, action)
        return float(np.dot(self.w, x))

    def update(self, obs: np.ndarray, action: int, reward: float):
        x = self._featurize(obs, action)
        if self.X is None:
            self.X = x[None, :]
            self.y = np.array([float(reward)], dtype=np.float32)
        else:
            if self.X.shape[0] >= self.max_samples:
                # FIFO buffer: drop oldest
                self.X = np.concatenate([self.X[1:], x[None, :]], axis=0)
                self.y = np.concatenate([self.y[1:], np.array([float(reward)], dtype=np.float32)], axis=0)
            else:
                self.X = np.concatenate([self.X, x[None, :]], axis=0)
                self.y = np.concatenate([self.y, np.array([float(reward)], dtype=np.float32)], axis=0)
        # Refit simple ridge each update (small d, cheap)
        try:
            n, d = self.X.shape
            A = self.X.T @ self.X + self.ridge_lambda * np.eye(d, dtype=np.float32)
            b = self.X.T @ self.y
            self.w = np.linalg.solve(A, b)
        except Exception:
            # Fallback to lstsq if solve fails
            self.w = np.linalg.lstsq(self.X, self.y, rcond=None)[0]

    def plan(self, obs: np.ndarray, w: float, c1: float, c2: float, num_particles: int = 16, num_iters: int = 10) -> int:
        # Initialize particles in continuous space [0, num_actions-1]
        low, high = 0.0, float(self.num_actions - 1)
        pos = np.random.uniform(low, high, size=(num_particles,)).astype(np.float32)
        vel = np.zeros_like(pos)

        pbest_pos = pos.copy()
        pbest_fit = np.array([self.predict(obs, int(np.round(p))) for p in pos], dtype=np.float32)
        gbest_idx = int(np.argmax(pbest_fit))
        gbest_pos = float(pbest_pos[gbest_idx])
        gbest_fit = float(pbest_fit[gbest_idx])

        for _ in range(max(1, int(num_iters))):
            # Velocity update
            r1 = np.random.uniform(0.0, 1.0, size=(num_particles,)).astype(np.float32)
            r2 = np.random.uniform(0.0, 1.0, size=(num_particles,)).astype(np.float32)
            vel = (
                w * vel
                + c1 * r1 * (pbest_pos - pos)
                + c2 * r2 * (gbest_pos - pos)
            ).astype(np.float32)
            # Position update with bounds
            pos = np.clip(pos + vel, low, high)
            # Evaluate
            fit = np.array([self.predict(obs, int(np.round(p))) for p in pos], dtype=np.float32)
            # Update personal bests
            improved = fit > pbest_fit
            pbest_pos[improved] = pos[improved]
            pbest_fit[improved] = fit[improved]
            # Update global best
            j = int(np.argmax(pbest_fit))
            if float(pbest_fit[j]) > gbest_fit:
                gbest_fit = float(pbest_fit[j])
                gbest_pos = float(pbest_pos[j])

        best_action = int(np.round(np.clip(gbest_pos, low, high)))
        return best_action


class RLPSOWrapper(gym.Wrapper):
    """Gym wrapper that converts RL actions (PSO hyperparameters) into PSO-driven
    discrete offloading actions for the base environment.

    - RL action space: Box(3) in [0, 1]^3 mapped to (w, c1, c2)
      w in [0.2, 1.0], c1,c2 in [0.5, 2.5]
    - Base env action: Discrete(num_actions) where num_actions is inferred (default 4).
    - Reward to RL: the realized env reward achieved by PSO-selected action.
    """

    def __init__(self, env: gym.Env, num_actions: int = 4):
        super().__init__(env)
        from gym.spaces import Box
        self.action_space = Box(low=np.zeros((3,), dtype=np.float32), high=np.ones((3,), dtype=np.float32), dtype=np.float32)
        self.observation_space = env.observation_space
        self.num_actions = int(num_actions)
        self.planner = PSOPlanner(num_actions=self.num_actions)
        self._last_obs = None

    @staticmethod
    def _map_params(a: np.ndarray) -> tuple:
        a = np.asarray(a, dtype=np.float32).ravel()
        if a.shape[0] < 3:
            a = np.pad(a, (0, 3 - a.shape[0]))
        # Map [0,1] -> ranges
        w = 0.2 + float(np.clip(a[0], 0.0, 1.0)) * 0.8
        c1 = 0.5 + float(np.clip(a[1], 0.0, 1.0)) * 2.0
        c2 = 0.5 + float(np.clip(a[2], 0.0, 1.0)) * 2.0
        return w, c1, c2

    def reset(self, *args, **kwargs):
        res = self.env.reset(*args, **kwargs)
        if isinstance(res, tuple):
            obs, info = res
        else:
            obs, info = res, {}
        self._last_obs = obs
        return obs, info

    def step(self, action):
        w, c1, c2 = self._map_params(action)
        # Plan discrete offloading decision via PSO using current observation
        discrete_action = self.planner.plan(self._last_obs, w=w, c1=c1, c2=c2)
        out = self.env.step(int(discrete_action))
        if isinstance(out, tuple) and len(out) == 5:
            obs, reward, terminated, truncated, info = out
        else:
            # Backward compatibility
            obs, reward, done, info = out
            terminated, truncated = bool(done), False
        # Update surrogate with realized reward
        try:
            self.planner.update(self._last_obs, int(discrete_action), float(reward))
        except Exception:
            pass
        self._last_obs = obs
        return obs, float(reward), bool(terminated), bool(truncated), info

def source_bash_env(script_path: str):
    """Source a bash script in a subshell and import env vars back to Python."""
    cmd = f"bash -lc 'source {script_path} && env -0'"
    out = subprocess.check_output(cmd, shell=True)
    for chunk in out.split(b"\x00"):
        if not chunk:
            continue
        k, _, v = chunk.partition(b"=")
        if k:
            os.environ[k.decode()] = v.decode()

def register_env():
    import gym
    # env registration mirrors the notebook setup
    gym.register(
        id="veins-straight-v1",
        entry_point="veins_gym:VeinsEnv",
        kwargs={
            "scenario_dir": "../scenario",
            "timeout": 7.0,
            "print_veins_stdout": False,
            "user_interface": "Cmdenv",
            "config": "StraightRoad",
            'run_veins': True
        },
    )


def main():
    parser = argparse.ArgumentParser(description="Train RL_PSO on VeinsGym StraightRoad")
    parser.add_argument("--timesteps", type=int, default=20000, help="Total training timesteps")
    parser.add_argument("--save-path", type=str, default="models/rl_pso", help="Model save path (without .zip)")
    parser.add_argument("--omnetpp-setenv", type=str, default=os.environ.get("OMNETPP_SETENV", "/home/abhay/omnetpp-5.7.1/setenv"), help="Path to OMNeT++ setenv script")
    args = parser.parse_args()

    # Source OMNeT++ environment if available
    if args.omnetpp_setenv and os.path.exists(args.omnetpp_setenv):
        source_bash_env(args.omnetpp_setenv)
        print("Sourced:", args.omnetpp_setenv)
    else:
        print("Warning: OMNeT++ setenv not found:", args.omnetpp_setenv)
        print("If Veins/opp_run aren't in PATH, training may fail.")

    # Register and make env
    register_env()
    # Prefer Gym spaces; fall back to Gymnasium if needed
    try:
        from gym.spaces import Box, Discrete
    except Exception:
        from gymnasium.spaces import Box, Discrete

    # Monkeypatch: VeinsGym calls gym.utils.seeding.create_seed which is missing in newer Gym.
    try:
        import secrets
        if not hasattr(gym.utils, "seeding"):
            import types
            gym.utils.seeding = types.SimpleNamespace()
        if not hasattr(gym.utils.seeding, "create_seed"):
            def _compat_create_seed(max_bytes: int = 4) -> int:
                return int.from_bytes(secrets.token_bytes(max_bytes), "big")
            gym.utils.seeding.create_seed = _compat_create_seed
    except Exception as e:
        print("Warning: failed to monkeypatch gym.utils.seeding.create_seed:", e)

    # Create base and monitor wrappers
    env = gym.make("veins-straight-v1")

    # Ensure numeric seed on reset to avoid OMNeT++ seed-set=None
    class _SeedFix(gym.Wrapper):
        def reset(self, seed=None, **kwargs):
            import random
            # SB3 may pass None or a string placeholder
            if seed is None or (isinstance(seed, str) and seed.upper().startswith("NO SEED")):
                try:
                    import secrets
                    seed_val = int.from_bytes(secrets.token_bytes(4), "big")
                except Exception:
                    seed_val = random.randrange(0, 2**32 - 1)
            else:
                try:
                    seed_val = int(seed)
                except Exception:
                    seed_val = random.randrange(0, 2**32 - 1)
            # OMNeT++/SUMO seed must fit target integer type; clamp to 31-bit
            seed_val = int(seed_val % (2**31 - 1))
            res = self.env.reset(seed=seed_val, **kwargs)
            # Normalize to (obs, info) tuple for SB3/shimmy compat
            if isinstance(res, tuple):
                if len(res) == 2:
                    return res
                elif len(res) >= 1:
                    return res[0], (res[1] if len(res) > 1 else {})
            return res, {}

    class _APICompat(gym.Wrapper):
        """Ensure step/reset signatures are compatible with SB3/Monitor.
        - reset -> (obs, info)
        - step -> 5-tuple (obs, reward, terminated, truncated, info)
        """
        def reset(self, *args, **kwargs):
            res = self.env.reset(*args, **kwargs)
            if isinstance(res, tuple):
                if len(res) == 2:
                    return res
                elif len(res) >= 1:
                    return res[0], (res[1] if len(res) > 1 else {})
            return res, {}

        def step(self, action):
            res = self.env.step(action)
            # If env uses old API (4-tuple), convert to new API (5-tuple)
            if isinstance(res, tuple) and len(res) == 4:
                obs, reward, done, info = res
                terminated = bool(done)
                truncated = False
                return obs, reward, terminated, truncated, info
            return res

    env = _SeedFix(env)
    env = _APICompat(env)
    # Patch spaces to standard types so SB3/shimmy can convert them
    try:
        env.observation_space = Box(
            low=-np.inf * np.ones((11,), dtype=np.float32),
            high=np.inf * np.ones((11,), dtype=np.float32),
            dtype=np.float32,
        )
        env.action_space = Discrete(4)
    except Exception as e:
        print("Warning: failed to patch env spaces:", e)

    # Wrap env with RL+PSO adapter: RL outputs (w,c1,c2), PSO picks discrete action
    env = RLPSOWrapper(env, num_actions=4)
    # env = Monitor(env)

    # Build and train model
    model = PPO(
        policy="MlpPolicy",
        env=env,
        verbose=1,
        seed=12345,
        device="cpu",
        n_steps=128,
        n_epochs=5,
        batch_size=32,
        gamma=0.5,
        # Small net for quick runs; adjust as needed
        policy_kwargs={"net_arch": [64, 64]},
    )

    try:
        model.learn(total_timesteps=args.timesteps)
    finally:
        try:
            env.close()
        except Exception:
            pass

    # Ensure save directory exists
    save_path = pathlib.Path(__file__).parent / args.save_path
    save_path.parent.mkdir(parents=True, exist_ok=True)

    # Save model
    model.save(str(save_path))
    print("Saved model to:", str(save_path) + ".zip")

    # Quick evaluation: run a short episode deterministically
    try:
        eval_env = gym.make("veins-straight-v1")
        eval_env = _SeedFix(eval_env)
        eval_env = _APICompat(eval_env)
        try:
            eval_env.observation_space = Box(
                low=-np.inf * np.ones((11,), dtype=np.float32),
                high=np.inf * np.ones((11,), dtype=np.float32),
                dtype=np.float32,
            )
            eval_env.action_space = Discrete(4)
        except Exception:
            pass
        # RL+PSO wrapper for evaluation
        eval_env = RLPSOWrapper(eval_env, num_actions=4)
        obs, _ = eval_env.reset()
        rewards = []
        terminated, truncated = False, False
        steps = 0
        while not (terminated or truncated) and steps < 200:
            action, _ = model.predict(obs, deterministic=True)
            step_out = eval_env.step(action)
            if isinstance(step_out, tuple) and len(step_out) == 5:
                obs, reward, terminated, truncated, info = step_out
            else:
                obs, reward, done, info = step_out
                terminated, truncated = bool(done), False
            if reward != 0.0:
                rewards.append(float(reward))
            steps += 1
        eval_env.close()
        if rewards:
            print("Eval mean reward:", float(np.mean(rewards)))
        else:
            print("Eval: no rewards observed in short run")
    except Exception as e:
        print("Eval run failed:", e)


if __name__ == "__main__":
    if not hasattr(np, 'bool8'):
        np.bool8 = np.bool_
    main()
