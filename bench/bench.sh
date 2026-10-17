#!/usr/bin/env bash
# Benchmarks kvstore against redis-server with identical redis-benchmark flags.
# Both servers log every write to an AOF with the same fsync policy.
#
# Usage: bench/bench.sh [path/to/kvstore] [fsync policy]
# Prints CSV: server,pipeline,test,rps,p50_ms,p99_ms
set -euo pipefail

BIN=${1:-build/release/kvstore}
FSYNC=${2:-everysec}
N=${N:-1000000}
CLIENTS=${CLIENTS:-50}
WORK=$(mktemp -d)
PIDS=()
trap 'kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; rm -rf "$WORK"' EXIT

"$BIN" --port 6501 --aof-file "$WORK/kv.aof" --fsync "$FSYNC" 2>"$WORK/kv.log" &
PIDS+=($!)
redis-server --port 6502 --dir "$WORK" --save "" --appendonly yes --appendfsync "$FSYNC" \
    --daemonize no >"$WORK/redis.log" 2>&1 &
PIDS+=($!)
for p in 6501 6502; do until redis-cli -p $p ping >/dev/null 2>&1; do sleep 0.1; done; done

echo "server,pipeline,test,rps,p50_ms,p99_ms"
for pipeline in 1 16; do
    for target in kvstore:6501 redis:6502; do
        name=${target%%:*} port=${target##*:}
        redis-cli -p "$port" flushall >/dev/null
        redis-benchmark -p "$port" -t set,get -n "$N" -c "$CLIENTS" -P "$pipeline" -r 100000 --csv |
            tail -n +2 | tr -d '"' |
            awk -F, -v s="$name" -v p="$pipeline" '{printf "%s,%s,%s,%d,%s,%s\n", s, p, $1, $2, $5, $7}'
    done
done
