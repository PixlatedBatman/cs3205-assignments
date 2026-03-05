#!/usr/bin/env bash
set -euo pipefail

MODE="${1:-thread}"           # thread | fork | select
START_CLIENTS="${2:-5}"
STEP_CLIENTS="${3:-5}"
MAX_CLIENTS="${4:-60}"
MESSAGES_PER_CLIENT="${5:-10}"
DELAY_US="${6:-500}"
FAIL_RATIO="${7:-0.10}"       # stop when failed/total > FAIL_RATIO

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DISC_PORT="${DISC_PORT:-9000}"
CHAT_PORT="${CHAT_PORT:-9100}"
STAMP="$(date +%Y%m%d_%H%M%S)"
OUT_DIR="${ROOT_DIR}/results/stress_${MODE}_${STAMP}"

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

CSV="${OUT_DIR}/stress_results.csv"
echo "clients,success,failed,duration_ms,total_messages,throughput_msg_per_sec,latency_samples,latency_avg_us,latency_p95_us,avg_cpu_percent,peak_vmrss_kb,peak_pss_kb" > "${CSV}"

echo "Running stress test: mode=${MODE}, range=${START_CLIENTS}..${MAX_CLIENTS}, step=${STEP_CLIENTS}"
for clients in $(seq "${START_CLIENTS}" "${STEP_CLIENTS}" "${MAX_CLIENTS}"); do
  start_ms="$(date +%s%3N)"
  pids=()

  for i in $(seq 1 "${clients}"); do
    user="s${clients}_u${i}"
    pass="p${i}"
    cport="$((20000 + clients * 10 + i))"
    stdbuf -oL -eL ./bin/chat_load_client 127.0.0.1 "${DISC_PORT}" 127.0.0.1 "${CHAT_PORT}" \
      "${user}" "${pass}" "${cport}" "${MESSAGES_PER_CLIENT}" "stress-${MODE}" "${DELAY_US}" \
      > "${OUT_DIR}/clients_${clients}_u${i}.log" 2>&1 &
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
  total_messages="$((clients * MESSAGES_PER_CLIENT))"
  throughput="$(awk "BEGIN { if (${duration_ms} > 0) printf \"%.2f\", (${total_messages}*1000)/${duration_ms}; else print 0 }")"

  grep -h '^LATENCY_US ' "${OUT_DIR}"/clients_"${clients}"_u*.log 2>/dev/null | awk '{print $2}' > "${OUT_DIR}/latencies_clients_${clients}.txt" || true
  lat_count="$(wc -l < "${OUT_DIR}/latencies_clients_${clients}.txt" | tr -d ' ')"
  lat_avg="0"
  lat_p95="0"
  if [[ "${lat_count}" -gt 0 ]]; then
    lat_avg="$(awk '{s+=$1} END{printf "%.2f", s/NR}' "${OUT_DIR}/latencies_clients_${clients}.txt")"
    sorted_lat="${OUT_DIR}/latencies_clients_${clients}_sorted.txt"
    sort -n "${OUT_DIR}/latencies_clients_${clients}.txt" > "${sorted_lat}"
    p95_idx="$(awk -v n="${lat_count}" 'BEGIN{v=int((n+1)*0.95); if(v<1)v=1; print v}')"
    lat_p95="$(sed -n "${p95_idx}p" "${sorted_lat}")"
  fi

  avg_cpu="0"
  peak_vmrss="0"
  peak_pss="0"
  if [[ -f "${OUT_DIR}/chat_monitor.csv" ]]; then
    avg_cpu="$(awk -F, 'NR>1 {s+=$2; c++} END{if(c>0) printf "%.2f", s/c; else print 0}' "${OUT_DIR}/chat_monitor.csv")"
    peak_vmrss="$(awk -F, 'NR>1 {if($3>m)m=$3} END{print m+0}' "${OUT_DIR}/chat_monitor.csv")"
    peak_pss="$(awk -F, 'NR>1 {if($4>m)m=$4} END{print m+0}' "${OUT_DIR}/chat_monitor.csv")"
  fi

  echo "${clients},${success},${failed},${duration_ms},${total_messages},${throughput},${lat_count},${lat_avg},${lat_p95},${avg_cpu},${peak_vmrss},${peak_pss}" | tee -a "${CSV}"

  fail_ratio="$(awk "BEGIN { if (${clients} > 0) printf \"%.4f\", ${failed}/${clients}; else print 0 }")"
  echo "clients=${clients} success=${success} failed=${failed} fail_ratio=${fail_ratio}"

  if awk "BEGIN { exit !(${fail_ratio} > ${FAIL_RATIO}) }"; then
    echo "Stopping: fail ratio ${fail_ratio} exceeded threshold ${FAIL_RATIO}" | tee "${OUT_DIR}/stop_reason.txt"
    break
  fi
done

echo "Stress test logs: ${OUT_DIR}"
