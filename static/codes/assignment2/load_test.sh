#!/usr/bin/env bash
set -euo pipefail

MODE="${1:-thread}"              # thread | fork | select
CLIENTS="${2:-10}"               # concurrent clients
MESSAGES_PER_CLIENT="${3:-20}"   # messages each client sends
DELAY_US="${4:-1000}"            # delay between sends per client

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DISC_PORT="${DISC_PORT:-9000}"
CHAT_PORT="${CHAT_PORT:-9100}"
STAMP="$(date +%Y%m%d_%H%M%S)"
OUT_DIR="${ROOT_DIR}/results/load_${MODE}_${STAMP}"

mkdir -p "${OUT_DIR}"

case "${MODE}" in
  thread) CHAT_BIN="${ROOT_DIR}/bin/chat_server_thread" ;;
  fork) CHAT_BIN="${ROOT_DIR}/bin/chat_server_fork" ;;
  select) CHAT_BIN="${ROOT_DIR}/bin/chat_server_select" ;;
  *) echo "Invalid mode: ${MODE} (use thread|fork|select)"; exit 2 ;;
esac

cleanup() {
  [[ -n "${DISC_PID:-}" ]] && kill "${DISC_PID}" >/dev/null 2>&1 || true
  [[ -n "${CHAT_PID:-}" ]] && kill "${CHAT_PID}" >/dev/null 2>&1 || true
}
trap cleanup EXIT

cd "${ROOT_DIR}"
make >/dev/null
rm -f data/registry.txt

stdbuf -oL -eL ./bin/discovery_server "${DISC_PORT}" > "${OUT_DIR}/discovery.log" 2>&1 &
DISC_PID=$!
SERVER_MONITOR_LOG="${OUT_DIR}/chat_monitor.csv" SERVER_MONITOR_INTERVAL=1 \
stdbuf -oL -eL "${CHAT_BIN}" "${CHAT_PORT}" > "${OUT_DIR}/chat_${MODE}.log" 2>&1 &
CHAT_PID=$!
sleep 1

if ! kill -0 "${DISC_PID}" 2>/dev/null; then
  echo "discovery_server failed to start. Check ${OUT_DIR}/discovery.log" >&2
  tail -n 40 "${OUT_DIR}/discovery.log" >&2 || true
  exit 1
fi
if ! kill -0 "${CHAT_PID}" 2>/dev/null; then
  echo "chat server failed to start. Check ${OUT_DIR}/chat_${MODE}.log" >&2
  tail -n 40 "${OUT_DIR}/chat_${MODE}.log" >&2 || true
  exit 1
fi

echo "Running load test: mode=${MODE}, clients=${CLIENTS}, messages=${MESSAGES_PER_CLIENT}"

start_ms="$(date +%s%3N)"
pids=()
for i in $(seq 1 "${CLIENTS}"); do
  user="u${i}"
  pass="p${i}"
  cport="$((12000 + i))"
  stdbuf -oL -eL ./bin/chat_load_client 127.0.0.1 "${DISC_PORT}" 127.0.0.1 "${CHAT_PORT}" \
    "${user}" "${pass}" "${cport}" "${MESSAGES_PER_CLIENT}" "load-${MODE}" "${DELAY_US}" \
    > "${OUT_DIR}/client_${i}.log" 2>&1 &
  pids+=("$!")
done

success=0
failed=0
for p in "${pids[@]}"; do
  if wait "${p}"; then
    success=$((success + 1))
  else
    failed=$((failed + 1))
  fi
done
end_ms="$(date +%s%3N)"
duration_ms="$((end_ms - start_ms))"
total_messages="$((CLIENTS * MESSAGES_PER_CLIENT))"

grep -h '^LATENCY_US ' "${OUT_DIR}"/client_*.log 2>/dev/null | awk '{print $2}' > "${OUT_DIR}/latencies_us.txt" || true
lat_count="$(wc -l < "${OUT_DIR}/latencies_us.txt" | tr -d ' ')"
lat_avg="0"
lat_min="0"
lat_max="0"
lat_p50="0"
lat_p95="0"
if [[ "${lat_count}" -gt 0 ]]; then
  lat_avg="$(awk '{s+=$1} END{printf "%.2f", s/NR}' "${OUT_DIR}/latencies_us.txt")"
  sorted_lat="${OUT_DIR}/latencies_sorted_us.txt"
  sort -n "${OUT_DIR}/latencies_us.txt" > "${sorted_lat}"
  lat_min="$(sed -n '1p' "${sorted_lat}")"
  lat_max="$(sed -n '$p' "${sorted_lat}")"
  p50_idx="$(awk -v n="${lat_count}" 'BEGIN{v=int((n+1)*0.50); if(v<1)v=1; print v}')"
  p95_idx="$(awk -v n="${lat_count}" 'BEGIN{v=int((n+1)*0.95); if(v<1)v=1; print v}')"
  lat_p50="$(sed -n "${p50_idx}p" "${sorted_lat}")"
  lat_p95="$(sed -n "${p95_idx}p" "${sorted_lat}")"
fi

mon_avg_cpu="0"
mon_peak_vmrss="0"
mon_peak_pss="0"
if [[ -f "${OUT_DIR}/chat_monitor.csv" ]]; then
  mon_avg_cpu="$(awk -F, 'NR>1 {s+=$2; c++} END{if(c>0) printf "%.2f", s/c; else print 0}' "${OUT_DIR}/chat_monitor.csv")"
  mon_peak_vmrss="$(awk -F, 'NR>1 {if($3>m)m=$3} END{print m+0}' "${OUT_DIR}/chat_monitor.csv")"
  mon_peak_pss="$(awk -F, 'NR>1 {if($4>m)m=$4} END{print m+0}' "${OUT_DIR}/chat_monitor.csv")"
fi

{
  echo "mode=${MODE}"
  echo "clients=${CLIENTS}"
  echo "messages_per_client=${MESSAGES_PER_CLIENT}"
  echo "delay_us=${DELAY_US}"
  echo "total_messages=${total_messages}"
  echo "successful_clients=${success}"
  echo "failed_clients=${failed}"
  echo "duration_ms=${duration_ms}"
  echo "throughput_msg_per_sec=$(awk "BEGIN { if (${duration_ms} > 0) printf \"%.2f\", (${total_messages}*1000)/${duration_ms}; else print 0 }")"
  echo "latency_samples=${lat_count}"
  echo "latency_avg_us=${lat_avg}"
  echo "latency_min_us=${lat_min}"
  echo "latency_p50_us=${lat_p50}"
  echo "latency_p95_us=${lat_p95}"
  echo "latency_max_us=${lat_max}"
  echo "monitor_avg_cpu_percent=${mon_avg_cpu}"
  echo "monitor_peak_vmrss_kb=${mon_peak_vmrss}"
  echo "monitor_peak_pss_kb=${mon_peak_pss}"
} | tee "${OUT_DIR}/summary.txt"

echo "Load test logs: ${OUT_DIR}"
