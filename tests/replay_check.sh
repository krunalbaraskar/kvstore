#!/usr/bin/env bash
# Checks that the AOF reproduces the keyspace exactly: random traffic from
# stress.py, dump every key, restart from the log, dump again, compare. Then the
# same after BGREWRITEAOF.
#
# Usage: tests/replay_check.sh [path/to/kvstore] [stress seconds]
set -euo pipefail

BIN=${1:-build/release/kvstore}
SECONDS_OF_TRAFFIC=${2:-20}
PORT=${PORT:-6396}
WORK=$(mktemp -d)
PID=
trap '[[ -n $PID ]] && kill -9 $PID 2>/dev/null; rm -rf "$WORK"' EXIT
cd "$(dirname "$0")/.."

start() {
    "$BIN" --port "$PORT" --aof-file "$WORK/a.aof" >>"$WORK/log" 2>&1 &
    PID=$!
    until redis-cli -p "$PORT" ping >/dev/null 2>&1; do
        kill -0 "$PID" || { cat "$WORK/log"; exit 1; }
        sleep 0.2
    done
}
stop() { kill -TERM "$PID"; wait "$PID"; PID=; }

dump() {
    for i in $(seq 0 199); do
        local k=k$i t
        t=$(redis-cli -p "$PORT" type "$k")
        case $t in
            string) echo "$k string $(redis-cli -p "$PORT" --no-raw get "$k" | sha1sum)" ;;
            list) echo "$k list $(redis-cli -p "$PORT" --no-raw lrange "$k" 0 -1 | sha1sum)" ;;
            *) echo "$k $t" ;;
        esac
        echo "$k ttl $(redis-cli -p "$PORT" ttl "$k" | sed 's/^[0-9][0-9]*$/positive/')"
    done
}

start
python3 tests/stress.py --port "$PORT" --seconds "$SECONDS_OF_TRAFFIC" --clients 20
sleep 6  # the stress traffic sets TTLs of at most 5 s; let them all lapse
dump >"$WORK/before"
echo "keys: $(redis-cli -p "$PORT" dbsize), AOF bytes: $(stat -c %s "$WORK/a.aof")"
stop

start
dump >"$WORK/after-replay"
diff "$WORK/before" "$WORK/after-replay" && echo "replay: identical"

redis-cli -p "$PORT" bgrewriteaof >/dev/null
echo "AOF bytes after rewrite: $(stat -c %s "$WORK/a.aof")"
stop
start
dump >"$WORK/after-rewrite"
diff "$WORK/before" "$WORK/after-rewrite" && echo "rewrite: identical"
stop
grep -E 'loaded|rewritten|truncat' "$WORK/log"
