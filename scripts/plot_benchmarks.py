#!/usr/bin/env python3

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
from pathlib import Path

import matplotlib

matplotlib.use("Agg")

import matplotlib.pyplot as plt


@dataclass(frozen=True)
class BenchRow:
    producers: int
    consumers: int
    capacity: int
    enq_ops: int
    deq_ops: int
    effective_ops_per_sec: float


def read_rows(path: Path) -> list[BenchRow]:
    rows: list[BenchRow] = []
    with path.open("r", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        for item in reader:
            rows.append(
                BenchRow(
                    producers=int(item["producers"]),
                    consumers=int(item["consumers"]),
                    capacity=int(item["capacity"]),
                    enq_ops=int(item["enq_ops"]),
                    deq_ops=int(item["deq_ops"]),
                    effective_ops_per_sec=float(item["effective_ops_per_sec"]),
                )
            )
    return rows


def plot_heatmaps(rows: list[BenchRow], output_dir: Path) -> None:
    capacities = sorted({r.capacity for r in rows})
    producers = sorted({r.producers for r in rows})
    consumers = sorted({r.consumers for r in rows})

    fig, axes = plt.subplots(1, len(capacities), figsize=(6 * len(capacities), 5), squeeze=False)
    axes_list = axes[0]

    for idx, capacity in enumerate(capacities):
        ax = axes_list[idx]
        grid: list[list[float]] = []
        for p in producers:
            row_vals: list[float] = []
            for c in consumers:
                match = next(
                    (r for r in rows if r.capacity == capacity and r.producers == p and r.consumers == c),
                    None,
                )
                row_vals.append((match.effective_ops_per_sec / 1_000_000.0) if match else 0.0)
            grid.append(row_vals)

        image = ax.imshow(grid, aspect="auto", origin="lower")
        ax.set_title(f"Capacity={capacity}")
        ax.set_xlabel("Consumers")
        ax.set_ylabel("Producers")
        ax.set_xticks(range(len(consumers)))
        ax.set_xticklabels(consumers)
        ax.set_yticks(range(len(producers)))
        ax.set_yticklabels(producers)

        for y, p in enumerate(producers):
            for x, c in enumerate(consumers):
                value = grid[y][x]
                ax.text(x, y, f"{value:.1f}", ha="center", va="center", fontsize=8)

        fig.colorbar(image, ax=ax, fraction=0.046, pad=0.04, label="M ops/sec")

    fig.suptitle("MPMC effective throughput heatmaps", fontsize=14)
    fig.tight_layout()
    fig.savefig(output_dir / "throughput_heatmaps.png", dpi=180)
    plt.close(fig)


def plot_top_scenarios(rows: list[BenchRow], output_dir: Path, top_n: int = 10) -> None:
    top = sorted(rows, key=lambda r: r.effective_ops_per_sec, reverse=True)[:top_n]
    labels = [f"P{r.producers}/C{r.consumers}/K{r.capacity}" for r in top]
    values = [r.effective_ops_per_sec / 1_000_000.0 for r in top]

    fig, ax = plt.subplots(figsize=(12, 6))
    bars = ax.bar(range(len(top)), values)
    ax.set_xticks(range(len(top)))
    ax.set_xticklabels(labels, rotation=35, ha="right")
    ax.set_ylabel("Effective throughput (M ops/sec)")
    ax.set_title(f"Top {top_n} benchmark scenarios")

    for bar, value in zip(bars, values):
        ax.text(bar.get_x() + bar.get_width() / 2, bar.get_height(), f"{value:.1f}", ha="center", va="bottom", fontsize=8)

    fig.tight_layout()
    fig.savefig(output_dir / "top_scenarios.png", dpi=180)
    plt.close(fig)


def plot_capacity_trends(rows: list[BenchRow], output_dir: Path) -> None:
    capacities = sorted({r.capacity for r in rows})
    producers = sorted({r.producers for r in rows})

    fig, ax = plt.subplots(figsize=(10, 6))
    for p in producers:
        p_rows = [r for r in rows if r.producers == p and r.consumers == p]
        if not p_rows:
            continue
        p_rows = sorted(p_rows, key=lambda r: capacities.index(r.capacity))
        x = [str(r.capacity) for r in p_rows]
        y = [r.effective_ops_per_sec / 1_000_000.0 for r in p_rows]
        ax.plot(x, y, marker="o", label=f"P=C={p}")

    ax.set_title("Throughput trend by capacity (balanced P=C)")
    ax.set_xlabel("Capacity")
    ax.set_ylabel("Effective throughput (M ops/sec)")
    ax.grid(alpha=0.3)
    ax.legend()
    fig.tight_layout()
    fig.savefig(output_dir / "capacity_trends_balanced.png", dpi=180)
    plt.close(fig)


def main() -> None:
    parser = argparse.ArgumentParser(description="Build plots from bench_mpmc CSV output.")
    parser.add_argument("--input", default="build/bench_results.csv", help="Input CSV path")
    parser.add_argument("--output-dir", default="build/plots", help="Directory for generated plots")
    args = parser.parse_args()

    input_path = Path(args.input)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    rows = read_rows(input_path)
    if not rows:
        raise ValueError(f"No rows parsed from {input_path}")

    plot_heatmaps(rows, output_dir)
    plot_top_scenarios(rows, output_dir)
    plot_capacity_trends(rows, output_dir)

    print(f"Plots generated in: {output_dir}")
    print("- throughput_heatmaps.png")
    print("- top_scenarios.png")
    print("- capacity_trends_balanced.png")


if __name__ == "__main__":
    main()
