#!/usr/bin/env python3
"""
Local baseline agent: always computes locally (action=0).
Registers the VeinsGym straight scenario and runs one episode, printing rewards.
"""
import os
import sys
import time
import argparse
import numpy as np

import gym
import veins_gym  # ensure this is installed in your Python env


def register_env(scenario_dir: str, config: str = "StraightRoad", run_veins: bool = True, timeout: float = 7.0):
    gym.register(
        id='veins-straight-v1',
        entry_point='veins_gym:VeinsEnv',
        kwargs={
            'scenario_dir': scenario_dir,
            'config': config,
            'timeout': timeout,
            'print_veins_stdout': False,
            'user_interface': 'Cmdenv',
            'run_veins': run_veins,
        },
    )


def run_episode(args):
    # Register and make the environment
    register_env(args.scenario_dir, args.config, args.run_veins, args.timeout)
    env = gym.make('veins-straight-v1')

    obs = env.reset()
    done = False
    rewards = []
    steps = 0
    print("Policy: always local (action=0)")
    while not done:
        action = 0  # local processing
        obs, reward, done, info = env.step(action)
        if reward != 0.0:
            print(f"Received reward: {float(reward):.4f} at step {steps}")
            rewards.append(float(reward))
        steps += 1

    print(f"Episode steps: {steps}")
    print(f"Mean reward: {np.mean(rewards) if rewards else 0.0:.6f}")
    env.close()


if __name__ == "__main__":
    if not hasattr(np, 'bool8'):
        np.bool8 = np.bool_

    parser = argparse.ArgumentParser(description="Local baseline agent for Veins straight scenario")
    parser.add_argument('--scenario-dir', default='../scenario', help='Path to scenario directory relative to this script')
    parser.add_argument('--config', default='StraightRoad', help='OMNeT++ config name in omnetpp.ini')
    parser.add_argument('--timeout', type=float, default=7.0, help='RPC timeout seconds for Veins-Gym')
    parser.add_argument('--run-veins', action='store_true', default=True, help='Start the simulation via Veins-Gym')
    args = parser.parse_args()

    run_episode(args)
