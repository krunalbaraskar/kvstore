#!/usr/bin/env bash
# End-to-end tests with the real redis-cli: commands, expiry, pub/sub, inline
# protocol, AOF persistence across kill -9, truncated-tail recovery, rewrite.
#
# Usage: tests/integration.sh [path/to/kvstore]   (default build/release/kvstore)
set -uo pipefail

BIN=${1:-build/release/kvstore}
PORT=${PORT:-6391}
WORK=$(mktemp -d)
AOF=$WORK/appendonly.aof
PID=
FAILS=0
cli() { redis-cli -p "$PORT" "$@"; }

cleanup() { [[ -n $PID ]] && kill -9 "$PID" 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

start() {
    "$BIN" --port "$PORT" --aof-file "$AOF" "$@" >>"$WORK/server.log" 2>&1 &
    PID=$!
    for _ in $(seq 50); do cli ping >/dev/null 2>&1 && return; sleep 0.1; done
    echo "server did not start"; cat "$WORK/server.log"; exit 1
}
stop() { kill -TERM "$PID"; wait "$PID"; local rc=$?; PID=; return $rc; }
crash() { kill -9 "$PID"; wait "$PID" 2>/dev/null; PID=; }

expect() {  # expect <expected> <command...>
    local want=$1; shift
    local got; got=$(cli "$@" 2>&1)
    if [[ "$got" != "$want" ]]; then
        echo "FAIL: $* => '$got' (want '$want')"; FAILS=$((FAILS + 1))
    fi
}

start
expect PONG ping
expect OK set greeting hello
expect hello get greeting
expect "" get missing
expect 1 exists greeting
expect 11 incrby counter 11
expect 3 rpush list a b c
expect $'b\nc' lrange list -2 -1
expect "" lrange list 2 1
expect "" set greeting x NX
expect OK set short 1 PX 300
expect 1 expire greeting 100
expect 100 ttl greeting
expect list type list
expect "ERR wrong number of arguments for 'get' command" get
expect "WRONGTYPE Operation against a key holding the wrong kind of value" get list
sleep 0.5
expect "" get short

# Inline protocol, as typed into nc.
got=$(printf 'PING\r\nSET inline 1\r\nGET inline\r\n' | nc -q1 localhost "$PORT" | tr -d '\r')
[[ $got == $'+PONG\n+OK\n$1\n1' ]] || { echo "FAIL: inline protocol => $got"; FAILS=$((FAILS + 1)); }

# Pub/sub across two connections.
redis-cli -p "$PORT" subscribe news >"$WORK/sub.out" &
SUB=$!
sleep 0.3
expect 1 publish news hello
expect 0 publish other hello
sleep 0.2
kill "$SUB"; wait "$SUB" 2>/dev/null
grep -q '^hello$' "$WORK/sub.out" || { echo "FAIL: subscriber did not get the message"; FAILS=$((FAILS + 1)); }

# Persistence: kill -9 must not lose acknowledged writes (fsync=everysec loses
# at most the last second on power loss, but a process crash loses nothing
# that reached write()).
for i in $(seq 1 1000); do echo "SET key:$i $i"; done | cli >/dev/null
expect OK set expiring v PX 600
crash
sleep 0.7
start
expect hello get greeting
expect 11 get counter
expect 1000 get key:1000
expect "" get expiring   # stayed expired across the restart
expect $'a\nb\nc' lrange list 0 -1

# Rewrite, then make sure the compacted log still restores the same data.
before=$(stat -c %s "$AOF")
expect "Background append only file rewriting started" bgrewriteaof
after=$(stat -c %s "$AOF")
(( after < before )) || { echo "FAIL: rewrite did not shrink the AOF ($before -> $after)"; FAILS=$((FAILS + 1)); }
cli set after-rewrite yes >/dev/null
stop
start
expect 1000 get key:1000
expect yes get after-rewrite
ttl=$(cli ttl greeting)
(( ttl > 80 && ttl <= 100 )) || { echo "FAIL: ttl after rewrite+restart = $ttl"; FAILS=$((FAILS + 1)); }
stop

# A crash halfway through writing a command leaves a truncated tail.
printf '*3\r\n$3\r\nSET\r\n$4\r\nhalf\r\n$5\r\nwri' >>"$AOF"
start
expect "" get half
expect yes get after-rewrite
grep -q "truncated command" "$WORK/server.log" || { echo "FAIL: no truncation warning"; FAILS=$((FAILS + 1)); }
stop

if (( FAILS > 0 )); then echo "integration: $FAILS failure(s)"; exit 1; fi
echo "integration: all checks passed"
