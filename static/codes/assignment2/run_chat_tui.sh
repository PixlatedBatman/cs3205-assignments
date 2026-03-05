#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

MODE="${MODE:-thread}"                 # thread|fork|select
CHAT_USERNAME="${CHAT_USERNAME:-}"
CHAT_PASSWORD="${CHAT_PASSWORD:-}"
CHAT_CLIENT_PORT="${CHAT_CLIENT_PORT:-}"
DISC_IP="${DISC_IP:-127.0.0.1}"
DISC_PORT="${DISC_PORT:-9000}"
CHAT_IP="${CHAT_IP:-127.0.0.1}"
CHAT_PORT="${CHAT_PORT:-9100}"
SKIP_BUILD="${SKIP_BUILD:-0}"
REGISTER="${REGISTER:-0}"

if [[ "${MODE}" != "thread" && "${MODE}" != "fork" && "${MODE}" != "select" ]]; then
  echo "Invalid MODE='${MODE}'. Use thread|fork|select" >&2
  exit 2
fi

if [[ "${SKIP_BUILD}" != "1" ]]; then
  make -C "${ROOT_DIR}" >/dev/null
fi

args=(
  --start-local
  --mode "${MODE}"
  --disc-ip "${DISC_IP}"
  --disc-port "${DISC_PORT}"
  --chat-ip "${CHAT_IP}"
  --chat-port "${CHAT_PORT}"
)

if [[ -n "${CHAT_USERNAME}" ]]; then
  args+=(--username "${CHAT_USERNAME}")
fi
if [[ -n "${CHAT_PASSWORD}" ]]; then
  args+=(--password "${CHAT_PASSWORD}")
fi
if [[ -n "${CHAT_CLIENT_PORT}" ]]; then
  args+=(--client-port "${CHAT_CLIENT_PORT}")
fi
if [[ "${REGISTER}" == "1" ]]; then
  args+=(--register)
fi

exec python3 "${ROOT_DIR}/scripts/chat_tui.py" "${args[@]}"
