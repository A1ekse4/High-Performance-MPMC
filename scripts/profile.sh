#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
OUT_DIR="${BUILD_DIR}/profiling"
RUNS_DIR="${OUT_DIR}/runs"
RUN_ID="$(date +%Y%m%d_%H%M%S)"
RUN_DIR="${RUNS_DIR}/${RUN_ID}"

mkdir -p "${OUT_DIR}" "${RUN_DIR}"

if [[ ! -x "${BUILD_DIR}/bench_mpmc" ]]; then
  echo "bench_mpmc binary not found. Build first:"
  echo "  cmake -S . -B build -G Ninja && cmake --build build -j"
  exit 1
fi

PRODUCERS="${1:-4}"
CONSUMERS="${2:-4}"
CAPACITY="${3:-32768}"
DURATION_SECONDS="${4:-3}"

echo "run_id=${RUN_ID}" >"${RUN_DIR}/run_meta.txt"
echo "producers=${PRODUCERS}" >>"${RUN_DIR}/run_meta.txt"
echo "consumers=${CONSUMERS}" >>"${RUN_DIR}/run_meta.txt"
echo "capacity=${CAPACITY}" >>"${RUN_DIR}/run_meta.txt"
echo "duration_seconds=${DURATION_SECONDS}" >>"${RUN_DIR}/run_meta.txt"

echo "[1/4] Running benchmark scenario (P=${PRODUCERS}/C=${CONSUMERS}, cap=${CAPACITY}, ${DURATION_SECONDS}s)..."
"${BUILD_DIR}/bench_mpmc" "${PRODUCERS}" "${CONSUMERS}" "${CAPACITY}" "${DURATION_SECONDS}" | tee "${RUN_DIR}/bench.csv"
cp "${RUN_DIR}/bench.csv" "${OUT_DIR}/bench_4p4c.csv"

if ! command -v perf >/dev/null 2>&1; then
  echo "perf not found; install linux perf tools to continue."
  echo "On Fedora, try: sudo dnf install perf"
  exit 0
fi

echo "[2/4] Running perf stat..."
perf stat -e cycles,instructions,branches,branch-misses,cache-references,cache-misses \
  "${BUILD_DIR}/bench_mpmc" "${PRODUCERS}" "${CONSUMERS}" "${CAPACITY}" "${DURATION_SECONDS}" \
  2>"${RUN_DIR}/perf_stat.txt" 1>/dev/null
cp "${RUN_DIR}/perf_stat.txt" "${OUT_DIR}/perf_stat.txt"

echo "[3/4] Running perf record..."
perf record -F 999 -g --call-graph dwarf -o "${RUN_DIR}/perf.data" \
  "${BUILD_DIR}/bench_mpmc" "${PRODUCERS}" "${CONSUMERS}" "${CAPACITY}" "${DURATION_SECONDS}" \
  1>/dev/null 2>/dev/null
cp "${RUN_DIR}/perf.data" "${OUT_DIR}/perf.data"

if [[ "${PROFILE_SKIP_PLOTS:-0}" != "1" ]]; then
  echo "[4/4] Building cross-run profiling plots..."
  python3 "${ROOT_DIR}/scripts/plot_profiling_runs.py" --profiling-dir "${OUT_DIR}" --output-dir "${OUT_DIR}/plots" >/dev/null
else
  echo "[4/4] Skipping cross-run plots (PROFILE_SKIP_PLOTS=1)"
fi

echo "Profiling artifacts:"
echo "  run dir: ${RUN_DIR}"
echo "  latest bench: ${OUT_DIR}/bench_4p4c.csv"
echo "  latest perf stat: ${OUT_DIR}/perf_stat.txt"
echo "  latest perf data: ${OUT_DIR}/perf.data"
echo "  summary plots: ${OUT_DIR}/plots/"
echo
echo "Inspect call stacks:"
echo "  perf report -i ${OUT_DIR}/perf.data"
