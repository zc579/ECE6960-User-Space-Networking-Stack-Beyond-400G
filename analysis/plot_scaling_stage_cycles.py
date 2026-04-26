#!/usr/bin/env python3
import argparse
import csv
import os
import re
from pathlib import Path

os.environ.setdefault("MPLCONFIGDIR", str(Path("/tmp/matplotlib-cache")))

import matplotlib.pyplot as plt


PROFILE_PATTERN = re.compile(r"\[profile\]")
CORE_COUNT_PATTERN = re.compile(r"(\d+)c")

STAGE_FIELDS = [
    ("RX", "rx_cycles/pkt"),
    ("Parse", "parse_cycles/pkt"),
    ("Rewrite", "rewrite_cycles/pkt"),
    ("Checksum", "checksum_cycles/pkt"),
    ("TX", "tx_cycles/pkt"),
    ("Other", "other_cycles/pkt"),
]

STAGE_COLORS = {
    "RX": "#355C7D",
    "Parse": "#6C8EBF",
    "Rewrite": "#99B898",
    "Checksum": "#E9C46A",
    "TX": "#E76F51",
    "Other": "#7F8C8D",
}


def extract_value(line: str, name: str):
    match = re.search(rf"{re.escape(name)}=([0-9.]+)", line)
    if not match:
        return None
    value = match.group(1)
    if "." in value:
        return float(value)
    return int(value)


def parse_profile_line(line: str):
    if not PROFILE_PATTERN.search(line):
        return None

    sample = {
        "queue_id": extract_value(line, "queue_id"),
        "rx_pkts": extract_value(line, "rx_pkts"),
        "tx_pkts": extract_value(line, "tx_pkts"),
        "tx_drops": extract_value(line, "tx_drops"),
        "rx_mpps": extract_value(line, "rx_mpps"),
        "total_cycles/pkt": extract_value(line, "total_cycles/pkt"),
    }
    for _, field in STAGE_FIELDS:
        sample[field] = extract_value(line, field)

    if any(value is None for value in sample.values()):
        return None
    return sample


def infer_core_count(path: Path, samples):
    match = CORE_COUNT_PATTERN.search(path.stem)
    if match:
        return int(match.group(1))
    return max(int(sample["queue_id"]) for sample in samples) + 1


def collect_rounds(samples, expected_queues: int):
    rounds = []
    current = []
    seen = set()
    expected = set(range(expected_queues))

    for sample in samples:
        queue_id = int(sample["queue_id"])
        if queue_id in seen:
            if seen == expected:
                rounds.append(current)
            current = [sample]
            seen = {queue_id}
            continue

        current.append(sample)
        seen.add(queue_id)
        if seen == expected:
            rounds.append(current)
            current = []
            seen = set()

    return rounds


def choose_best_round(rounds):
    loaded_rounds = [r for r in rounds if sum(sample["rx_pkts"] for sample in r) > 0]
    if not loaded_rounds:
        raise ValueError("No non-idle complete profile rounds were found.")

    return max(
        loaded_rounds,
        key=lambda r: (
            sum(sample["rx_pkts"] for sample in r),
            sum(sample["rx_mpps"] for sample in r),
        ),
    )


def weighted_average(round_samples, field: str, weight_field: str = "rx_pkts"):
    total_weight = sum(sample[weight_field] for sample in round_samples)
    if total_weight == 0:
        return 0.0
    return sum(sample[field] * sample[weight_field] for sample in round_samples) / total_weight


def summarize_log(log_path: Path):
    samples = []
    for line in log_path.read_text().splitlines():
        sample = parse_profile_line(line)
        if sample:
            samples.append(sample)

    if not samples:
        raise ValueError(f"No valid [profile] lines found in {log_path}")

    cores = infer_core_count(log_path, samples)
    rounds = collect_rounds(samples, cores)
    if not rounds:
        raise ValueError(f"No complete queue rounds found in {log_path}")

    best_round = choose_best_round(rounds)
    total_rx_pkts = sum(sample["rx_pkts"] for sample in best_round)
    summary = {
        "cores": cores,
        "log_path": str(log_path),
        "queues_seen": "|".join(str(int(sample["queue_id"])) for sample in best_round),
        "total_rx_pkts": total_rx_pkts,
        "total_tx_pkts": sum(sample["tx_pkts"] for sample in best_round),
        "total_tx_drops": sum(sample["tx_drops"] for sample in best_round),
        "total_rx_mpps": sum(sample["rx_mpps"] for sample in best_round),
        "total_cycles/pkt": weighted_average(best_round, "total_cycles/pkt"),
    }

    for stage, field in STAGE_FIELDS:
        summary[stage] = weighted_average(best_round, field)

    return summary


def format_summary(summary):
    formatted = dict(summary)
    for key in [
        "total_rx_mpps",
        "total_cycles/pkt",
        "RX",
        "Parse",
        "Rewrite",
        "Checksum",
        "TX",
        "Other",
    ]:
        formatted[key] = f"{formatted[key]:.3f}"
    return formatted


def plot_summaries(summaries, output_path: Path, title: str):
    summaries = sorted(summaries, key=lambda item: item["cores"])
    x_labels = [f"{item['cores']} cores" for item in summaries]
    x = list(range(len(summaries)))

    fig, ax = plt.subplots(figsize=(12, 6.5))
    bottoms = [0.0] * len(summaries)

    for stage, _field in STAGE_FIELDS:
        heights = [item[stage] for item in summaries]
        ax.bar(
            x,
            heights,
            bottom=bottoms,
            color=STAGE_COLORS[stage],
            edgecolor="black",
            linewidth=0.7,
            label=stage,
        )
        bottoms = [bottom + height for bottom, height in zip(bottoms, heights)]

    for idx, item in enumerate(summaries):
        ax.text(
            idx,
            item["total_cycles/pkt"] + 2.5,
            f"{item['total_rx_mpps']:.1f} Mpps\n{item['total_cycles/pkt']:.1f} cyc/pkt",
            ha="center",
            va="bottom",
            fontsize=9,
        )

    ax.set_xticks(x, x_labels)
    ax.set_ylabel("Cycles per Packet")
    ax.set_title(title)
    ax.grid(axis="y", linestyle="--", alpha=0.35)
    ax.legend(ncols=3, frameon=False)

    fig.tight_layout()
    output_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output_path, dpi=220)
    print(f"Saved plot to {output_path}")


def write_summary_csv(summaries, output_path: Path):
    output_path.parent.mkdir(parents=True, exist_ok=True)
    fieldnames = [
        "cores",
        "total_rx_mpps",
        "total_rx_pkts",
        "total_tx_pkts",
        "total_tx_drops",
        "total_cycles/pkt",
        "RX",
        "Parse",
        "Rewrite",
        "Checksum",
        "TX",
        "Other",
        "queues_seen",
        "log_path",
    ]

    with output_path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for summary in sorted(summaries, key=lambda item: item["cores"]):
            writer.writerow(format_summary(summary))

    print(f"Saved summary CSV to {output_path}")


def main():
    parser = argparse.ArgumentParser(
        description=(
            "Plot multi-core echo_server stage cycles from complete [profile] rounds "
            "and save a summary CSV."
        )
    )
    parser.add_argument(
        "logs",
        nargs="*",
        type=Path,
        default=sorted(Path("results/raw/scaling").glob("server_*c.log")),
        help="Server log files to analyze. Defaults to results/raw/scaling/server_*c.log.",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("results/processed/100g_scaling_stage_cycles.png"),
        help="Output image path.",
    )
    parser.add_argument(
        "--summary-csv",
        type=Path,
        default=Path("results/processed/100g_scaling_stage_cycles_summary.csv"),
        help="Output CSV path.",
    )
    parser.add_argument(
        "--title",
        default="100G Echo Server Stage Cycles Across Core Counts",
        help="Plot title.",
    )
    args = parser.parse_args()

    if not args.logs:
        raise SystemExit("No input logs were provided and no default logs were found.")

    summaries = [summarize_log(log_path) for log_path in args.logs]
    plot_summaries(summaries, args.output, args.title)
    write_summary_csv(summaries, args.summary_csv)


if __name__ == "__main__":
    main()
