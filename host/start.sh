#!/usr/bin/env bash
set -euo pipefail

cd /app
mkdir -p out art

cleanup() {
  echo "Stopping host processes..."
  kill -TERM "${server_pid:-}" "${main_pid:-}" 2>/dev/null || true
  wait "${server_pid:-}" "${main_pid:-}" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

while true; do
  echo "Starting server.py and main.py"
  python server.py &
  server_pid=$!

  python main.py &
  main_pid=$!

  wait -n "$server_pid" "$main_pid"
  status=$?
  echo "One process exited with status $status"

  cleanup
  echo "Restarting processes in 2 seconds..."
  sleep 2
 done
