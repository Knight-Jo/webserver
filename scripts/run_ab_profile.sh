#!/usr/bin/env bash
set -euo pipefail

HOST="${1:-127.0.0.1}"
PORT="${2:-8080}"
REQS="${3:-10000}"
CONC="${4:-100}"

if ! command -v ab >/dev/null 2>&1; then
  echo "ab not found. Please install apache2-utils (or httpd-tools)." >&2
  exit 1
fi

PID="$(pgrep -af "(bin/main)" | awk '{print $1}' | head -n1 || true)"

if [[ -z "${PID}" ]]; then
  echo "Server PID not found with pgrep '(bin/main)'." >&2
  exit 1
fi

echo "Using PID=${PID}"
echo "Running ab: -n ${REQS} -c ${CONC} http://${HOST}:${PORT}/"
ab -n "${REQS}" -c "${CONC}" "http://${HOST}:${PORT}/" | tee /tmp/ab_result.txt

echo "Sampling CPU/memory for 15 seconds..."
for i in $(seq 1 15); do
  ts=$(date +%H:%M:%S)
  ps -p "${PID}" -o pid,comm,%cpu,%mem,rss,vsz | tail -n +2 | awk -v t="${ts}" '{print t, $0}'
  egrep 'VmRSS|VmSize|Threads' /proc/"${PID}"/status | xargs echo "${ts}"
  sleep 1
done | tee /tmp/cpu_mem_sample.txt

echo "Sampling syscalls for 15 seconds (requires ptrace permission)..."
if sudo sysctl -w kernel.yama.ptrace_scope=0 >/dev/null 2>&1; then
  sudo timeout 15 strace -c -p "${PID}" -o /tmp/kama_strace.txt
  cat /tmp/kama_strace.txt
else
  echo "Failed to relax ptrace_scope. Run manually: sudo sysctl -w kernel.yama.ptrace_scope=0" >&2
fi

echo "Outputs:"
echo "  /tmp/ab_result.txt"
echo "  /tmp/cpu_mem_sample.txt"
echo "  /tmp/kama_strace.txt"