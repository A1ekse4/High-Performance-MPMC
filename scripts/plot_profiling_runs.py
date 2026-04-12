#!/usr/bin/env python3

from __future__ import annotations

import argparse
import csv
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


@dataclass
class ProfilingRun:
    run_id: str
    producers: int
    consumers: int
    capacity: int
    duration_seconds: int
    effective_ops_per_sec: float
    cycles: int
    instructions: int
    branches: int
    branch_misses: int
    cache_references: int
    cache_misses: int
    elapsed_seconds: float

    @property
    def branch_miss_rate(self) -> float:
        return 0.0 if self.branches == 0 else self.branch_misses / self.branches

    @property
    def cache_miss_rate(self) -> float:
        return 0.0 if self.cache_references == 0 else self.cache_misses / self.cache_references

    @property
    def ipc(self) -> float:
        return 0.0 if self.cycles == 0 else self.instructions / self.cycles


def _parse_int(text: str) -> int:
    cleaned = re.sub(r"[^0-9]", "", text)
    return int(cleaned) if cleaned else 0


def _parse_metric_counter(line: str, metric_name: str) -> int | None:
    if metric_name not in line:
        return None
    match = re.match(r"^\s*([0-9][0-9\s\u00A0\u202F,\.]*)\s+" + re.escape(metric_name), line)
    return _parse_int(match.group(1)) if match else None


def parse_meta(path: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    if not path.exists():
        return result
    for line in path.read_text(encoding="utf-8").splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            result[key.strip()] = value.strip()
    return result


def parse_bench(path: Path) -> dict[str, str]:
    with path.open("r", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        for row in reader:
            return row
    raise ValueError(f"No rows in bench csv: {path}")


def parse_perf_stat(path: Path) -> dict[str, float]:
    metrics: dict[str, float] = {
        "cycles": 0,
        "instructions": 0,
        "branches": 0,
        "branch_misses": 0,
        "cache_references": 0,
        "cache_misses": 0,
        "elapsed_seconds": 0.0,
    }
    for line in path.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        for key in ("cycles", "instructions", "branches", "branch-misses", "cache-references", "cache-misses"):
            value = _parse_metric_counter(line, key)
            if value is not None:
                metrics[key.replace("-", "_")] = value
        if "seconds time elapsed" in stripped:
            match = re.search(r"([0-9]+[.,][0-9]+|[0-9]+)", stripped)
            if match:
                metrics["elapsed_seconds"] = float(match.group(1).replace(",", "."))
    return metrics


def load_runs(runs_dir: Path) -> list[ProfilingRun]:
    runs: list[ProfilingRun] = []
    for run_dir in sorted([p for p in runs_dir.glob("*") if p.is_dir()]):
        bench_path = run_dir / "bench.csv"
        perf_path = run_dir / "perf_stat.txt"
        if not bench_path.exists() or not perf_path.exists():
            continue
        meta = parse_meta(run_dir / "run_meta.txt")
        bench = parse_bench(bench_path)
        perf = parse_perf_stat(perf_path)
        runs.append(
            ProfilingRun(
                run_id=meta.get("run_id", run_dir.name),
                producers=int(meta.get("producers", bench["producers"])),
                consumers=int(meta.get("consumers", bench["consumers"])),
                capacity=int(meta.get("capacity", bench["capacity"])),
                duration_seconds=int(meta.get("duration_seconds", 0)),
                effective_ops_per_sec=float(bench["effective_ops_per_sec"]),
                cycles=int(perf["cycles"]),
                instructions=int(perf["instructions"]),
                branches=int(perf["branches"]),
                branch_misses=int(perf["branch_misses"]),
                cache_references=int(perf["cache_references"]),
                cache_misses=int(perf["cache_misses"]),
                elapsed_seconds=float(perf["elapsed_seconds"]),
            )
        )
    return runs


def classify_symbol(symbol: str) -> str:
    s = symbol.lower()
    if "try_dequeue" in s:
        return "dequeue_path"
    if "emplace_impl" in s or "try_enqueue" in s:
        return "enqueue_path"
    if "cpu_relax" in s or "backoff::pause" in s:
        return "backoff_spin"
    if "atomic" in s or "memory_order" in s or "cmpexch" in s:
        return "atomics_control"
    if "vector<" in s and "operator[]" in s:
        return "buffer_access"
    return "other_code"


def parse_top_symbols(perf_data_path: Path, top_n: int = 6) -> list[tuple[str, float]]:
    cmd = ["perf", "report", "--stdio", "--no-children", "-n", "-i", str(perf_data_path)]
    completed = subprocess.run(cmd, check=True, capture_output=True, text=True)
    buckets: dict[str, float] = {}
    for line in completed.stdout.splitlines():
        if "bench_mpmc" not in line or "[.]" not in line:
            continue
        match = re.match(
            r"^\s*([0-9]+(?:\.[0-9]+)?)%\s+[0-9][0-9\s\u00A0\u202F,\.]*\s+bench_mpmc\s+.*\[\.\]\s+(.+)$",
            line,
        )
        if not match:
            continue
        pct = float(match.group(1))
        bucket = classify_symbol(match.group(2).strip())
        buckets[bucket] = buckets.get(bucket, 0.0) + pct
    return sorted(buckets.items(), key=lambda x: x[1], reverse=True)[:top_n]


def latest_runs_by_pair(runs: list[ProfilingRun]) -> tuple[dict[tuple[int, int], ProfilingRun], int, int]:
    by_pair: dict[tuple[int, int], ProfilingRun] = {}
    max_p, max_c = 0, 0
    for run in runs:
        by_pair[(run.producers, run.consumers)] = run
        max_p = max(max_p, run.producers)
        max_c = max(max_c, run.consumers)
    return by_pair, max(max_p, 12), max(max_c, 12)


def collect_symbol_maps_by_pair(
    runs_dir: Path, top_n: int = 6
) -> tuple[dict[tuple[int, int], dict[str, float]], list[str], int, int]:
    by_pair: dict[tuple[int, int], dict[str, float]] = {}
    all_symbols: list[str] = []
    max_p, max_c = 0, 0
    for run_dir in sorted([p for p in runs_dir.glob("*") if p.is_dir()]):
        meta = parse_meta(run_dir / "run_meta.txt")
        producers = int(meta.get("producers", "0"))
        consumers = int(meta.get("consumers", "0"))
        if producers == 0 or consumers == 0:
            continue
        max_p = max(max_p, producers)
        max_c = max(max_c, consumers)

        perf_data = run_dir / "perf.data"
        if not perf_data.exists():
            continue
        try:
            symbols = parse_top_symbols(perf_data, top_n=top_n)
        except Exception:
            continue
        if not symbols:
            continue

        symbol_map = {name: pct for name, pct in symbols}
        symbol_map["Other"] = max(0.0, 100.0 - sum(symbol_map.values()))
        by_pair[(producers, consumers)] = symbol_map
        for key in symbol_map.keys():
            if key != "Other" and key not in all_symbols:
                all_symbols.append(key)
    all_symbols = all_symbols[:top_n] + ["Other"]
    return by_pair, all_symbols, max(max_p, 12), max(max_c, 12)


def export_table(runs: list[ProfilingRun], out_csv: Path) -> None:
    with out_csv.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(
            [
                "run_id",
                "producers",
                "consumers",
                "capacity",
                "duration_seconds",
                "effective_ops_per_sec",
                "cycles",
                "instructions",
                "ipc",
                "branches",
                "branch_misses",
                "branch_miss_rate",
                "cache_references",
                "cache_misses",
                "cache_miss_rate",
                "elapsed_seconds",
            ]
        )
        for r in runs:
            writer.writerow(
                [
                    r.run_id,
                    r.producers,
                    r.consumers,
                    r.capacity,
                    r.duration_seconds,
                    f"{r.effective_ops_per_sec:.2f}",
                    r.cycles,
                    r.instructions,
                    f"{r.ipc:.4f}",
                    r.branches,
                    r.branch_misses,
                    f"{r.branch_miss_rate:.6f}",
                    r.cache_references,
                    r.cache_misses,
                    f"{r.cache_miss_rate:.6f}",
                    f"{r.elapsed_seconds:.6f}",
                ]
            )


def export_symbol_distribution_table(runs_dir: Path, out_csv: Path, top_n: int = 6) -> None:
    by_pair, all_symbols, max_p, max_c = collect_symbol_maps_by_pair(runs_dir, top_n=top_n)
    with out_csv.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["producer", "consumer"] + all_symbols)
        for p in range(1, max_p + 1):
            for c in range(1, max_c + 1):
                values = by_pair.get((p, c), {})
                writer.writerow([p, c] + [f"{values.get(sym, 0.0):.4f}" for sym in all_symbols])


def plot_summary_pc_grid(runs: list[ProfilingRun], out_path: Path) -> None:
    by_pair, max_p, max_c = latest_runs_by_pair(runs)
    fig, axes = plt.subplots(max_p, max_c, figsize=(max(18, max_c * 1.8), max(14, max_p * 1.2)))
    names = ["thr", "ipc", "cache%", "branch%"]
    colors = ["tab:blue", "tab:green", "tab:red", "tab:orange"]
    x = list(range(len(names)))

    for p in range(1, max_p + 1):
        for c in range(1, max_c + 1):
            row_idx = max_p - p
            ax = axes[row_idx][c - 1]
            run = by_pair.get((p, c))
            if not run:
                ax.text(0.5, 0.5, "no run", transform=ax.transAxes, ha="center", va="center", fontsize=6)
                ax.set_title(f"P{p}/C{c}", fontsize=7)
                ax.set_xticks([])
                ax.grid(axis="y", alpha=0.15)
                continue

            vals = [
                run.effective_ops_per_sec / 1_000_000.0,
                run.ipc,
                run.cache_miss_rate * 100.0,
                run.branch_miss_rate * 100.0,
            ]
            bars = ax.bar(x, vals, color=colors)
            for i, val in enumerate(vals):
                ax.text(bars[i].get_x() + bars[i].get_width() / 2.0, val, f"{val:.1f}", ha="center", va="bottom", fontsize=5)
            ax.set_title(f"P{p}/C{c}", fontsize=7)
            if p == 1:
                ax.set_xticks(x)
                ax.set_xticklabels(names, rotation=20, ha="right", fontsize=6)
            else:
                ax.set_xticks([])
            ax.grid(axis="y", alpha=0.15)

    handles = [plt.Rectangle((0, 0), 1, 1, color=colors[i]) for i in range(len(names))]
    labels = [
        "thr = throughput (M ops/sec)",
        "ipc = instructions/cycle",
        "cache% = cache miss rate",
        "branch% = branch miss rate",
    ]
    fig.legend(handles, labels, loc="upper right", bbox_to_anchor=(0.995, 0.995), ncol=1, fontsize=8)
    fig.suptitle("Profiling summary grid (rows: producers, cols: consumers)")
    fig.subplots_adjust(left=0.03, right=0.995, bottom=0.03, top=0.93, wspace=0.15, hspace=0.35)
    fig.savefig(out_path, dpi=180)
    plt.close(fig)


def plot_symbol_distribution_pc_grid_subplots(runs_dir: Path, out_path: Path, top_n: int = 6) -> None:
    by_pair, all_symbols, max_p, max_c = collect_symbol_maps_by_pair(runs_dir, top_n=top_n)
    if not all_symbols:
        return

    fig, axes = plt.subplots(max_p, max_c, figsize=(max(18, max_c * 1.8), max(14, max_p * 1.2)), sharey=True)
    palette = list(plt.cm.tab10.colors) + list(plt.cm.Set3.colors)
    category_colors = {sym: palette[idx % len(palette)] for idx, sym in enumerate(all_symbols)}
    x = list(range(len(all_symbols)))

    for p in range(1, max_p + 1):
        for c in range(1, max_c + 1):
            row_idx = max_p - p
            ax = axes[row_idx][c - 1]
            values_map = by_pair.get((p, c))
            if not values_map:
                ax.text(0.5, 0.5, "no run", transform=ax.transAxes, ha="center", va="center", fontsize=6)
                ax.set_title(f"P{p}/C{c}", fontsize=7)
                ax.set_xticks([])
                ax.set_ylim(0, 100)
                ax.grid(axis="y", alpha=0.15)
                continue

            vals = [values_map.get(sym, 0.0) for sym in all_symbols]
            colors = [category_colors[sym] for sym in all_symbols]
            bars = ax.bar(x, vals, color=colors)
            for i, val in enumerate(vals):
                ax.text(
                    bars[i].get_x() + bars[i].get_width() / 2.0,
                    val,
                    f"{val:.0f}%",
                    ha="center",
                    va="bottom",
                    fontsize=5,
                )
            ax.set_title(f"P{p}/C{c}", fontsize=7)
            if p == 1:
                ax.set_xticks(x)
                ax.set_xticklabels(all_symbols, rotation=35, ha="right", fontsize=6)
            else:
                ax.set_xticks([])
            ax.set_ylim(0, 100)
            ax.grid(axis="y", alpha=0.15)

    handles = [plt.Rectangle((0, 0), 1, 1, color=category_colors[s]) for s in all_symbols]
    fig.legend(
        handles,
        all_symbols,
        loc="upper right",
        bbox_to_anchor=(0.995, 0.995),
        ncol=1,
        fontsize=8,
        title="Categories",
    )
    fig.suptitle("CPU samples by category grid (rows: producers, cols: consumers)")
    fig.subplots_adjust(left=0.03, right=0.995, bottom=0.03, top=0.90, wspace=0.15, hspace=0.35)
    fig.savefig(out_path, dpi=180)
    plt.close(fig)


def main() -> None:
    parser = argparse.ArgumentParser(description="Build profiling summary plots across all runs.")
    parser.add_argument("--profiling-dir", default="build/profiling", help="Profiling root directory")
    parser.add_argument("--output-dir", default="build/profiling/plots", help="Output directory for summary artifacts")
    args = parser.parse_args()

    profiling_dir = Path(args.profiling_dir)
    runs_dir = profiling_dir / "runs"
    out_dir = Path(args.output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    if not runs_dir.exists():
        print(f"No runs directory: {runs_dir}")
        return

    runs = load_runs(runs_dir)
    if not runs:
        print("No complete profiling runs found.")
        return

    export_table(runs, out_dir / "profiling_runs_summary.csv")
    plot_summary_pc_grid(runs, out_dir / "profiling_runs_summary.png")
    plot_symbol_distribution_pc_grid_subplots(runs_dir, out_dir / "profiling_symbol_distribution_pc_grid_subplots.png")
    export_symbol_distribution_table(runs_dir, out_dir / "profiling_symbol_distribution_table.csv")

    print(f"Profiling summary generated for {len(runs)} run(s): {out_dir}")
    print("- profiling_runs_summary.csv")
    print("- profiling_runs_summary.png")
    print("- profiling_symbol_distribution_pc_grid_subplots.png")
    print("- profiling_symbol_distribution_table.csv")


if __name__ == "__main__":
    main()
