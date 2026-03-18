#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
SOCK="/tmp/aicam_wifi.sock"
DAEMON="$ROOT_DIR/bin/wifi-daemon"

"$DAEMON" >/tmp/wifi-daemon.log 2>&1 &
PID=$!
trap 'kill $PID >/dev/null 2>&1 || true; rm -f "$SOCK"' EXIT

for _ in $(seq 1 50); do
  [[ -S "$SOCK" ]] && break
  sleep 0.1
done

[[ -S "$SOCK" ]]

printf 'SCAN_GET\n' | socat - UNIX-CONNECT:"$SOCK" | grep -q "OK"
printf 'CONNECT\tHome-WiFi\n' | socat - UNIX-CONNECT:"$SOCK" | grep -q "OK"
printf 'DISCONNECT\n' | socat - UNIX-CONNECT:"$SOCK" | grep -q "OK"
