#!/usr/bin/env bash
set -euo pipefail

cd /app
mkdir -p out art

start_server() {
  echo "Starting server.py"
  python server.py &
  server_pid=$!
  echo "server.py pid=${server_pid}"
}

start_main() {
  echo "Starting main.py"
  python main.py &
  main_pid=$!
  echo "main.py pid=${main_pid}"
}

cleanup() {
  echo "Stopping host processes..."
  kill -TERM "${server_pid:-}" "${main_pid:-}" 2>/dev/null || true
  wait "${server_pid:-}" "${main_pid:-}" 2>/dev/null || true
}

trap 'echo "Signal received, shutting down..."; cleanup; exit 0' INT TERM

# Start both services
start_server
start_main

# Monitor loop: check PIDs and restart whichever stopped
while true; do
  sleep 5

  if ! kill -0 "${server_pid:-}" 2>/dev/null; then
    echo "server.py (pid ${server_pid:-}) not running; restarting..."
    start_server
  fi

  if ! kill -0 "${main_pid:-}" 2>/dev/null; then
    echo "main.py (pid ${main_pid:-}) not running; restarting..."
    start_main
  fi
done
