#!/usr/bin/env python3
"""Plot aim_sim CSV output: true yaw error and commanded yaw step per scenario.

usage: plot_sim.py [outdir]   (default /home/user/aim_sim_out)
writes <outdir>/aim_sim.png
"""
import csv
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

SCENARIOS = ["static", "strafe", "lowfps", "mismatch", "nocam", "approach"]
MODES = [("memory", "memory (camera error)"), ("memacc", "memory (accumulator error)"), ("touch", "touch")]


def load(path):
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    t = [float(r["t"]) for r in rows]
    err = [float(r["err_yaw"]) for r in rows]
    step = [float(r["step_yaw"]) for r in rows]
    return t, err, step


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "/home/user/aim_sim_out"
    fig, axes = plt.subplots(len(SCENARIOS), 2, figsize=(14, 3.0 * len(SCENARIOS)), squeeze=False)
    for row, name in enumerate(SCENARIOS):
        ax_err, ax_step = axes[row][0], axes[row][1]
        for mode, label in MODES:
            path = os.path.join(outdir, f"{name}_{mode}.csv")
            if not os.path.exists(path):
                continue
            t, err, step = load(path)
            ax_err.plot(t, err, lw=1.0, label=label)
            ax_step.plot(t, step, lw=0.8, label=label)
        ax_err.axhline(0.0, color="k", lw=0.5)
        ax_err.set_title(f"{name}: true yaw error (target - camera), deg")
        ax_step.set_title(f"{name}: commanded yaw step per tick, deg")
        ax_err.grid(alpha=0.3)
        ax_step.grid(alpha=0.3)
        if row == 0:
            ax_err.legend(fontsize=8)
    for ax in axes[-1]:
        ax.set_xlabel("t, s")
    fig.tight_layout()
    out = os.path.join(outdir, "aim_sim.png")
    fig.savefig(out, dpi=110)
    print(out)


if __name__ == "__main__":
    main()
