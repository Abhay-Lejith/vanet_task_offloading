#!/usr/bin/env python3
"""
Simple contextual bandit training on the VeinsGym straight scenario.
- Avoids Stable-Baselines3/Gymnasium compatibility layers.
- One action per pending task; step blocks until completion; immediate reward.
- Saves a small PyTorch policy to examples/models/bandit_policy.pt

Quick start:
    pip install torch gym numpy veins_gym
    python examples/train_bandit.py --steps 200 --omnetpp-setenv /home/abhay/omnetpp-5.7.1/setenv
"""

import os
import subprocess
import argparse
import pathlib
import numpy as np
import torch
import torch.nn as nn
import torch.optim as optim


def source_bash_env(script_path: str):
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
    # Register env similarly to the notebook setup
    gym.register(
        id="veins-straight-v1",
        entry_point="veins_gym:VeinsEnv",
        kwargs={
            "scenario_dir": "../scenario",
            "timeout": 7.0,            
            "print_veins_stdout": False,
            "user_interface": "Cmdenv",
            "config": "StraightRoad",
            "run_veins": True,
        },
    )

class Policy(nn.Module):
    def __init__(self, obs_dim=14, n_actions=4):
        super().__init__()
        self.net = nn.Sequential(
            nn.Linear(obs_dim, 64), nn.ReLU(),
            nn.Linear(64, 64), nn.ReLU(),
            nn.Linear(64, n_actions),
        )

    def forward(self, x):
        return self.net(x)

def preprocess_obs(obs):
    # obs = [speed, d0, d1, d2, taskMB, busy0, busy1, busy2, ul0, ul1, ul2, battery, energyLocal, energyOffload]
    x = np.asarray(obs, dtype=np.float32).copy()
    # Simple scaling to reasonable ranges
    x[0] /= 30.0       # speed m/s -> ~0..1
    x[1:4] /= 1500.0   # distances -> 0..1
    x[4] /= 20.0       # task MB -> 0..1
    # busy flags are already 0/1
    x[8:11] /= 50.0    # UL Mbps -> 0..~1
    # battery already normalized 0-1
    # energy estimates already normalized (divided by 100 in C++)
    return x


def main():
    ap = argparse.ArgumentParser(description="Simple contextual bandit training on Veins env")
    ap.add_argument("--omnetpp-setenv", default=os.environ.get("OMNETPP_SETENV", "/home/abhay/omnetpp-5.7.1/setenv"))
    ap.add_argument("--steps", type=int, default=2000, help="Number of decisions (tasks) to collect")
    ap.add_argument("--lr", type=float, default=1e-3)
    ap.add_argument("--batch", type=int, default=64, help="Update every N decisions")
    ap.add_argument("--save", default="models/bandit_policy.pt")
    args = ap.parse_args()

    if args.omnetpp_setenv and os.path.exists(args.omnetpp_setenv):
        source_bash_env(args.omnetpp_setenv)
        print("Sourced:", args.omnetpp_setenv)
    else:
        print("Warning: OMNeT++ setenv not found:", args.omnetpp_setenv)

    import gym  # noqa: F401
    import veins_gym  # noqa: F401
    register_env()

    env = gym.make("veins-straight-v1")

    device = torch.device("cpu")
    policy = Policy(obs_dim=14, n_actions=4).to(device)
    opt = optim.Adam(policy.parameters(), lr=args.lr)

    obs = env.reset()
    decisions = 0
    logps, rewards = [], []
    reward_baseline = 0.0
    alpha = 0.01  # moving baseline

    try:
        while decisions < args.steps:
            x = preprocess_obs(obs)
            x_t = torch.tensor(x, dtype=torch.float32, device=device).unsqueeze(0)
            logits = policy(x_t)
            probs = torch.softmax(logits, dim=-1)
            dist = torch.distributions.Categorical(probs=probs)
            action = int(dist.sample().item())
            logp = dist.log_prob(torch.tensor(action, device=device))

            next_obs, reward, done, info = env.step(action)
            reward = float(reward)  # immediate reward for this action
            reward_baseline = (1 - alpha) * reward_baseline + alpha * reward
            adv = reward - reward_baseline

            logps.append(logp)
            rewards.append(adv)
            decisions += 1

            if len(logps) >= args.batch:
                loss = -torch.stack(logps).mul(torch.tensor(rewards, dtype=torch.float32, device=device)).mean()
                opt.zero_grad()
                loss.backward()
                opt.step()
                logps.clear()
                rewards.clear()

            obs = next_obs
            if done:
                obs = env.reset()
    finally:
        try:
            env.close()
        except Exception:
            pass

    save_path = pathlib.Path(__file__).parent / args.save
    save_path.parent.mkdir(parents=True, exist_ok=True)
    torch.save({"model_state": policy.state_dict()}, str(save_path))
    print("Saved policy to:", str(save_path))
    print("Decisions collected:", decisions)
    print("Done.")

if __name__ == "__main__":
    if not hasattr(np, 'bool8'):
        np.bool8 = np.bool_
    main()
