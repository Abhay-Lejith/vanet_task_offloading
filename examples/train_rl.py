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
    parser = argparse.ArgumentParser(description="Train PPO on VeinsGym StraightRoad")
    parser.add_argument("--timesteps", type=int, default=20000, help="Total training timesteps")
    parser.add_argument("--save-path", type=str, default="models/ppo_veins_straight", help="Model save path (without .zip)")
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
            low=-np.inf * np.ones((14,), dtype=np.float32),
            high=np.inf * np.ones((14,), dtype=np.float32),
            dtype=np.float32,
        )
        env.action_space = Discrete(4)
    except Exception as e:
        print("Warning: failed to patch env spaces:", e)
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
                low=-np.inf * np.ones((14,), dtype=np.float32),
                high=np.inf * np.ones((14,), dtype=np.float32),
                dtype=np.float32,
            )
            eval_env.action_space = Discrete(4)
        except Exception:
            pass
        obs, _ = eval_env.reset()
        rewards = []
        terminated, truncated = False, False
        steps = 0
        while not (terminated or truncated) and steps < 200:
            action, _ = model.predict(obs, deterministic=True)
            step_out = eval_env.step(int(action))
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
