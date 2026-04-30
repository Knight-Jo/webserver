#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN_DIR="$ROOT_DIR/bin"

usage() {
  cat <<'EOF'
Usage: scripts/run_log_tests.sh [bench|stress|all]

Environment overrides:
  BENCH_THREADS, BENCH_MESSAGES, BENCH_BYTES, BENCH_FLUSH, BENCH_ROLL, BENCH_SAMPLE_RATE, BENCH_BASENAME
  STRESS_THREADS, STRESS_DURATION, STRESS_BYTES, STRESS_BURST, STRESS_PAUSE_US, STRESS_FLUSH, STRESS_ROLL, STRESS_BASENAME

Examples:
  scripts/run_log_tests.sh bench
  BENCH_THREADS=8 BENCH_MESSAGES=400000 scripts/run_log_tests.sh bench
  STRESS_DURATION=30 STRESS_BURST=5000 scripts/run_log_tests.sh stress
EOF
}

mode=${1:-all}
if [[ "$mode" != "bench" && "$mode" != "stress" && "$mode" != "all" ]]; then
  usage
  exit 1
fi

BENCH_THREADS=${BENCH_THREADS:-4}
BENCH_MESSAGES=${BENCH_MESSAGES:-200000}
BENCH_BYTES=${BENCH_BYTES:-256}
BENCH_FLUSH=${BENCH_FLUSH:-1}
BENCH_ROLL=${BENCH_ROLL:-268435456}
BENCH_SAMPLE_RATE=${BENCH_SAMPLE_RATE:-1000}
BENCH_BASENAME=${BENCH_BASENAME:-logs/bench}

STRESS_THREADS=${STRESS_THREADS:-8}
STRESS_DURATION=${STRESS_DURATION:-20}
STRESS_BYTES=${STRESS_BYTES:-512}
STRESS_BURST=${STRESS_BURST:-2000}
STRESS_PAUSE_US=${STRESS_PAUSE_US:-2000}
STRESS_FLUSH=${STRESS_FLUSH:-1}
STRESS_ROLL=${STRESS_ROLL:-67108864}
STRESS_BASENAME=${STRESS_BASENAME:-logs/stress}

run_bench() {
  "$BIN_DIR/log_benchmark" \
    --threads "$BENCH_THREADS" \
    --messages "$BENCH_MESSAGES" \
    --bytes "$BENCH_BYTES" \
    --flush "$BENCH_FLUSH" \
    --roll "$BENCH_ROLL" \
    --sample-rate "$BENCH_SAMPLE_RATE" \
    --basename "$BENCH_BASENAME"
}

run_stress() {
  "$BIN_DIR/log_stress" \
    --threads "$STRESS_THREADS" \
    --duration "$STRESS_DURATION" \
    --bytes "$STRESS_BYTES" \
    --burst "$STRESS_BURST" \
    --pause-us "$STRESS_PAUSE_US" \
    --flush "$STRESS_FLUSH" \
    --roll "$STRESS_ROLL" \
    --basename "$STRESS_BASENAME"
}

if [[ "$mode" == "bench" || "$mode" == "all" ]]; then
  run_bench
fi

if [[ "$mode" == "stress" || "$mode" == "all" ]]; then
  run_stress
fi
