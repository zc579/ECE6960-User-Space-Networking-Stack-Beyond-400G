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

STAGES = [
    ("rx_cycles_per_pkt", "rx_cycles/pkt", "RX"),
    ("parse_cycles_per_pkt", "parse_cycles/pkt", "Parse"),
    ("rewrite_cycles_per_pkt", "rewrite_cycles/pkt", "Rewrite"),
    ("checksum_cycles_per_pkt", "checksum_cycles/pkt", "Checksum"),
    ("tx_cycles_per_pkt", "tx_cycles/pkt", "TX"),
    ("other_cycles_per_pkt", "other_cycles/pkt", "Other"),
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
        "tx_mpps": extract_value(line, "tx_mpps"),
        "total_cycles_per_pkt": extract_value(line, "total_cycles/pkt"),
    }
    for csv_name, profile_name, _label in STAGES:
        sample[csv_name] = extract_value(line, profile_name)

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


def weighted_average(samples, field: str, weight_field: str = "rx_pkts"):
    total_weight = sum(sample[weight_field] for sample in samples)
    if total_weight == 0:
        return 0.0
    return sum(sample[field] * sample[weight_field] for sample in samples) / total_weight


def summarize_log(mode: str, log_path: Path):
    samples = []
    for line in log_path.read_text().splitlines():
        sample = parse_profile_line(line)
        if sample:
            samples.append(sample)

    if not samples:
        raise ValueError(f"No valid [profile] lines found in {log_path}")

    cores = infer_core_count(log_path, samples)
    loaded_rounds = [
        round_samples
        for round_samples in collect_rounds(samples, cores)
        if sum(sample["rx_pkts"] for sample in round_samples) > 0
    ]
    if not loaded_rounds:
        raise ValueError(f"No loaded complete profile rounds found in {log_path}")

    best_round = max(
        loaded_rounds,
        key=lambda round_samples: (
            sum(sample["rx_pkts"] for sample in round_samples),
            sum(sample["rx_mpps"] for sample in round_samples),
        ),
    )

    summary = {
        "mode": mode,
        "cores": cores,
        "rx_mpps": sum(sample["rx_mpps"] for sample in best_round),
        "tx_mpps": sum(sample["tx_mpps"] for sample in best_round),
        "rx_pkts": sum(sample["rx_pkts"] for sample in best_round),
        "tx_pkts": sum(sample["tx_pkts"] for sample in best_round),
        "tx_drops": sum(sample["tx_drops"] for sample in best_round),
        "total_cycles_per_pkt": weighted_average(best_round, "total_cycles_per_pkt"),
        "queues_seen": "|".join(str(int(sample["queue_id"])) for sample in best_round),
        "log_path": str(log_path),
    }
    for csv_name, _profile_name, _label in STAGES:
        summary[csv_name] = weighted_average(best_round, csv_name)

    return summary


def find_logs(raw_root: Path, modes):
    logs = []
    for mode in modes:
        mode_dir = raw_root / mode
        mode_logs = sorted(mode_dir.glob("server_*c.log"))
        if not mode_logs:
            raise FileNotFoundError(f"No logs found under {mode_dir}")
        logs.extend((mode, log_path) for log_path in mode_logs)
    return logs


def write_csv(summaries, output_path: Path):
    fieldnames = [
        "mode",
        "cores",
        "rx_mpps",
        "tx_mpps",
        "rx_pkts",
        "tx_pkts",
        "tx_drops",
        "total_cycles_per_pkt",
        "rx_cycles_per_pkt",
        "parse_cycles_per_pkt",
        "rewrite_cycles_per_pkt",
        "checksum_cycles_per_pkt",
        "tx_cycles_per_pkt",
        "other_cycles_per_pkt",
        "queues_seen",
        "log_path",
    ]

    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for item in sorted(summaries, key=lambda row: (row["cores"], row["mode"])):
            row = dict(item)
            for key in fieldnames:
                if isinstance(row.get(key), float):
                    row[key] = f"{row[key]:.3f}"
            writer.writerow(row)

    print(f"Saved summary CSV to {output_path}")


def plot_comparison(summaries, output_path: Path, title: str):
    modes = [mode for mode in ["software", "offload", "preserve", "none"]
             if any(item["mode"] == mode for item in summaries)]
    cores = sorted({item["cores"] for item in summaries})
    by_key = {(item["mode"], item["cores"]): item for item in summaries}

    x = list(range(len(cores)))
    width = 0.24 if len(modes) == 3 else 0.75 / max(len(modes), 1)
    offsets = {
        mode: (idx - (len(modes) - 1) / 2.0) * width
        for idx, mode in enumerate(modes)
    }

    fig, ax = plt.subplots(figsize=(14, 7.5))
    for mode in modes:
        bottoms = [0.0] * len(cores)
        mode_x = [pos + offsets[mode] for pos in x]

        for csv_name, _profile_name, label in STAGES:
            heights = [
                by_key[(mode, core)][csv_name] if (mode, core) in by_key else 0.0
                for core in cores
            ]
            ax.bar(
                mode_x,
                heights,
                width=width,
                bottom=bottoms,
                color=STAGE_COLORS[label],
                edgecolor="black",
                linewidth=0.5,
                label=label if mode == modes[0] else None,
            )
            bottoms = [bottom + height for bottom, height in zip(bottoms, heights)]

        for xpos, core in zip(mode_x, cores):
            item = by_key.get((mode, core))
            if not item:
                continue
            ax.text(
                xpos,
                item["total_cycles_per_pkt"] + 2.0,
                f"{mode}\n{item['rx_mpps']:.1f} Mpps\n"
                f"{item['total_cycles_per_pkt']:.1f} cyc/pkt",
                ha="center",
                va="bottom",
                fontsize=7.5,
            )

    ax.set_xticks(x, [f"{core} cores" for core in cores])
    ax.set_ylabel("Cycles per Packet")
    ax.set_title(title)
    ax.grid(axis="y", linestyle="--", alpha=0.35)
    ax.legend(ncols=3, frameon=False)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    fig.tight_layout()
    fig.savefig(output_path, dpi=220)
    print(f"Saved comparison plot to {output_path}")


def main():
    parser = argparse.ArgumentParser(
        description="Compare software, offload, and preserve checksum modes."
    )
    parser.add_argument(
        "--raw-root",
        type=Path,
        default=Path("results/raw/checksum_modes"),
        help="Root containing software/offload/preserve server logs.",
    )
    parser.add_argument(
        "--modes",
        nargs="+",
        default=["software", "offload", "preserve"],
        help="Mode directories under --raw-root to compare.",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("results/processed/checksum_modes_stage_cycles.png"),
        help="Output plot path.",
    )
    parser.add_argument(
        "--summary-csv",
        type=Path,
        default=Path("results/processed/checksum_modes_summary.csv"),
        help="Output CSV path.",
    )
    parser.add_argument(
        "--title",
        default="Checksum Modes Stage Cycles",
        help="Plot title.",
    )
    args = parser.parse_args()

    summaries = [
        summarize_log(mode, path)
        for mode, path in find_logs(args.raw_root, args.modes)
    ]
    write_csv(summaries, args.summary_csv)
    plot_comparison(summaries, args.output, args.title)


if __name__ == "__main__":
    main()
