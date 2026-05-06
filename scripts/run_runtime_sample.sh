#!/usr/bin/env bash
set -euo pipefail

PID="$(pgrep -af "(bin/main)" | awk '{print $1}' | head -n1 || true)"
DURATION="${1:-15}"

if [[ -z "${PID}" ]]; then
  echo "Server PID not found with pgrep '(bin/main)'." >&2
  exit 1
fi

if [[ "${DURATION}" -le 0 ]]; then
  echo "Duration must be > 0" >&2
  exit 1
fi

echo "Using PID=${PID}"

echo "Sampling top for ${DURATION}s..."
top -b -H -p "${PID}" -d 1 -n "${DURATION}" > /tmp/top_sample.txt

echo "Sampling /proc for ${DURATION}s..."
for i in $(seq 1 "${DURATION}"); do
  ts=$(date +%H:%M:%S)
  ps -p "${PID}" -o pid,comm,%cpu,%mem,rss,vsz | tail -n +2 | awk -v t="${ts}" '{print t, $0}'
  egrep 'VmRSS|VmSize|Threads' /proc/"${PID}"/status | xargs echo "${ts}"
  sleep 1
done > /tmp/proc_sample.txt

echo "Sampling syscalls for ${DURATION}s (requires ptrace permission)..."
if sudo sysctl -w kernel.yama.ptrace_scope=0 >/dev/null 2>&1; then
  sudo timeout "${DURATION}" strace -c -p "${PID}" -o /tmp/strace_sample.txt
else
  echo "Failed to relax ptrace_scope. Run manually: sudo sysctl -w kernel.yama.ptrace_scope=0" >&2
fi

echo "Outputs:"
echo "  /tmp/top_sample.txt"
echo "  /tmp/proc_sample.txt"
echo "  /tmp/strace_sample.txt"
