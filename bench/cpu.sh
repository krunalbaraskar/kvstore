#!/usr/bin/env bash
# Measures server CPU time per operation (user and system, from /proc) for a
# fixed pipelined SET/GET workload. More stable than ops/sec for comparing two
# builds, because it does not depend on how fast the benchmark client is.
#
# Usage: bench/cpu.sh [path/to/kvstore] [runs]
set -euo pipefail

BIN=${1:-build/release/kvstore}
RUNS=${2:-3}
N=${N:-2000000}
TICK_NS=$((1000000000 / $(getconf CLK_TCK)))

for run in $(seq "$RUNS"); do
    WORK=$(mktemp -d)
    "$BIN" --port 6505 --aof-file "$WORK/a.aof" 2>/dev/null &
    PID=$!
    until redis-cli -p 6505 ping >/dev/null 2>&1; do sleep 0.1; done
    read -r u0 s0 < <(awk '{print $14, $15}' /proc/$PID/stat)
    rps=$(redis-benchmark -p 6505 -t set,get -n "$N" -c 50 -P 16 -r 100000 --threads 4 --csv 2>/dev/null |
        tail -n +2 | tr -d '"' | awk -F, '{printf "%s=%d ", $1, $2}')
    read -r u1 s1 < <(awk '{print $14, $15}' /proc/$PID/stat)
    kill -TERM $PID; wait $PID
    rm -rf "$WORK"
    ops=$((2 * N))
    echo "run $run: user $(((u1 - u0) * TICK_NS / ops)) ns/op, sys $(((s1 - s0) * TICK_NS / ops)) ns/op, $rps"
done
