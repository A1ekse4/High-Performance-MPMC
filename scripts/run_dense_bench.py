#!/usr/bin/env python3

from __future__ import annotations

import argparse
import csv
import subprocess
from pathlib import Path


def run_case(bench_bin: Path, producers: int, consumers: int, capacity: int, seconds: int) -> dict[str, str]:
    cmd = [str(bench_bin), str(producers), str(consumers), str(capacity), str(seconds)]
    completed = subprocess.run(cmd, check=True, capture_output=True, text=True)
    lines = [line.strip() for line in completed.stdout.splitlines() if line.strip()]
    if len(lines) < 2:
        raise RuntimeError(f"Unexpected bench output for P={producers}, C={consumers}, cap={capacity}: {completed.stdout}")

    headers = [x.strip() for x in lines[0].split(",")]
    values = [x.strip() for x in lines[1].split(",")]
    return dict(zip(headers, values, strict=True))


def main() -> None:
    parser = argparse.ArgumentParser(description="Run dense producer/consumer benchmark matrix.")
    parser.add_argument("--bench-bin", default="build/bench_mpmc", help="Path to bench binary")
    parser.add_argument("--min-producers", type=int, default=1)
    parser.add_argument("--max-producers", type=int, default=12)
    parser.add_argument("--min-consumers", type=int, default=1)
    parser.add_argument("--max-consumers", type=int, default=12)
    parser.add_argument("--capacity", type=int, default=32768, help="Queue capacity (power of two)")
    parser.add_argument(
        "--capacities",
        default="",
        help="Comma-separated capacities (example: 4096,32768,262144). Overrides --capacity when set.",
    )
    parser.add_argument("--seconds", type=int, default=1, help="Duration for each scenario")
    parser.add_argument("--output", default="build/bench_dense_1_12.csv", help="Output CSV path")
    args = parser.parse_args()

    bench_bin = Path(args.bench_bin)
    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    if not bench_bin.exists():
        raise FileNotFoundError(f"Bench binary not found: {bench_bin}")

    if args.capacities.strip():
        capacities = [int(x.strip()) for x in args.capacities.split(",") if x.strip()]
    else:
        capacities = [args.capacity]

    rows: list[dict[str, str]] = []
    total = (
        (args.max_producers - args.min_producers + 1)
        * (args.max_consumers - args.min_consumers + 1)
        * len(capacities)
    )
    completed_idx = 0

    for capacity in capacities:
        for producers in range(args.min_producers, args.max_producers + 1):
            for consumers in range(args.min_consumers, args.max_consumers + 1):
                completed_idx += 1
                row = run_case(bench_bin, producers, consumers, capacity, args.seconds)
                rows.append(row)
                print(
                    f"[{completed_idx}/{total}] "
                    f"P={producers} C={consumers} cap={capacity} "
                    f"eff={float(row['effective_ops_per_sec']) / 1_000_000.0:.2f} Mops/s"
                )

    headers = ["producers", "consumers", "capacity", "enq_ops", "deq_ops", "effective_ops_per_sec"]
    with output_path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=headers)
        writer.writeheader()
        writer.writerows(rows)

    print(f"\nDense benchmark CSV saved to: {output_path}")


if __name__ == "__main__":
    main()
