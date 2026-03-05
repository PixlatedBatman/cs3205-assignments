#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RESULTS_DIR="${ROOT_DIR}/results"
OUT_DIR="${1:-${ROOT_DIR}/reports/plots}"

latest_dir() {
  local pattern="$1"
  ls -1dt ${pattern} 2>/dev/null | head -n1
}

load_thread="$(latest_dir "${RESULTS_DIR}/load_thread_*")"
load_fork="$(latest_dir "${RESULTS_DIR}/load_fork_*")"
load_select="$(latest_dir "${RESULTS_DIR}/load_select_*")"
stress_thread="$(latest_dir "${RESULTS_DIR}/stress_thread_*")"
stress_fork="$(latest_dir "${RESULTS_DIR}/stress_fork_*")"
stress_select="$(latest_dir "${RESULTS_DIR}/stress_select_*")"

missing=0
for p in \
  "${load_thread}/latencies_us.txt" \
  "${load_fork}/latencies_us.txt" \
  "${load_select}/latencies_us.txt" \
  "${stress_thread}/stress_results.csv" \
  "${stress_fork}/stress_results.csv" \
  "${stress_select}/stress_results.csv"; do
  if [[ ! -f "${p}" ]]; then
    echo "Missing required file: ${p}" >&2
    missing=1
  fi
done

if [[ "${missing}" -ne 0 ]]; then
  echo "Run load/stress tests for thread, fork, and select first." >&2
  exit 1
fi

echo "Using:"
echo "  ${load_thread}"
echo "  ${load_fork}"
echo "  ${load_select}"
echo "  ${stress_thread}"
echo "  ${stress_fork}"
echo "  ${stress_select}"

python3 "${ROOT_DIR}/scripts/plot_benchmarks.py" \
  --lat-thread "${load_thread}/latencies_us.txt" \
  --lat-fork "${load_fork}/latencies_us.txt" \
  --lat-select "${load_select}/latencies_us.txt" \
  --stress-thread "${stress_thread}/stress_results.csv" \
  --stress-fork "${stress_fork}/stress_results.csv" \
  --stress-select "${stress_select}/stress_results.csv" \
  --out-dir "${OUT_DIR}"

echo "Plots generated in: ${OUT_DIR}"
