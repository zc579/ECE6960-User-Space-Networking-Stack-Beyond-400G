#!/usr/bin/env python3
import argparse
import re
from pathlib import Path

import matplotlib.pyplot as plt


PROFILE_PATTERN = re.compile(
    r"\[profile\].*?"
    r"rx_cycles/pkt=(?P<rx>[0-9.]+)\s+"
    r"parse_cycles/pkt=(?P<parse>[0-9.]+)\s+"
    r"rewrite_cycles/pkt=(?P<rewrite>[0-9.]+)\s+"
    r"checksum_cycles/pkt=(?P<checksum>[0-9.]+)\s+"
    r"tx_cycles/pkt=(?P<tx>[0-9.]+)"
)


def parse_profile_line(line: str):
    match = PROFILE_PATTERN.search(line)
    if not match:
        return None
    return {
        "RX": float(match.group("rx")),
        "Parse": float(match.group("parse")),
        "Rewrite": float(match.group("rewrite")),
        "Checksum": float(match.group("checksum")),
        "TX": float(match.group("tx")),
    }


def choose_profile(lines, mode: str):
    profiles = [p for line in lines if (p := parse_profile_line(line))]
    if not profiles:
        raise ValueError("No valid [profile] lines were found in the input.")

    if mode == "last":
        return profiles[-1]

    if mode == "max-throughput":
        best = None
        best_mpps = -1.0
        mpps_pattern = re.compile(r"rx_mpps=([0-9.]+)")
        for line in lines:
            profile = parse_profile_line(line)
            if not profile:
                continue
            m = mpps_pattern.search(line)
            mpps = float(m.group(1)) if m else -1.0
            if mpps > best_mpps:
                best_mpps = mpps
                best = profile
        return best

    raise ValueError(f"Unsupported mode: {mode}")


def plot_breakdown(profile, output: Path, title: str):
    labels = list(profile.keys())
    values = list(profile.values())
    total = sum(values)
    percentages = [v / total * 100.0 if total else 0.0 for v in values]

    colors = ["#355C7D", "#6C8EBF", "#99B898", "#E9C46A", "#E76F51"]

    fig, ax = plt.subplots(figsize=(9, 5))
    bars = ax.bar(labels, percentages, color=colors, edgecolor="black", linewidth=0.8)

    ax.set_ylabel("Stage Share (%)")
    ax.set_ylim(0, max(percentages) * 1.25 if total else 1)
    ax.set_title(title)
    ax.grid(axis="y", linestyle="--", alpha=0.35)

    for bar, pct, cycles in zip(bars, percentages, values):
        ax.text(
            bar.get_x() + bar.get_width() / 2.0,
            bar.get_height() + 0.8,
            f"{pct:.1f}%\n{cycles:.1f} cyc/pkt",
            ha="center",
            va="bottom",
            fontsize=9,
        )

    fig.tight_layout()
    output.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output, dpi=200)
    print(f"Saved plot to {output}")


def main():
    parser = argparse.ArgumentParser(
        description="Plot single-core echo_server stage breakdown from [profile] logs."
    )
    parser.add_argument(
        "logfile",
        type=Path,
        help="Path to a log file containing echo_server [profile] output.",
    )
    parser.add_argument(
        "--mode",
        choices=["last", "max-throughput"],
        default="max-throughput",
        help="Choose which [profile] sample to plot.",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("results/processed/single_core_stage_breakdown.png"),
        help="Output image path.",
    )
    parser.add_argument(
        "--title",
        default="Single-Core Echo Server Stage Breakdown",
        help="Plot title.",
    )
    args = parser.parse_args()

    lines = args.logfile.read_text().splitlines()
    profile = choose_profile(lines, args.mode)
    plot_breakdown(profile, args.output, args.title)


if __name__ == "__main__":
    main()
