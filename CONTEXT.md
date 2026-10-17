# kvstore: resume context

Reference sheet for describing this project on a resume and in interviews. Every
number below comes from one uninterrupted, sequential run on 2026-10-17 (17:44–17:54),
with nothing else using the test ports or competing for CPU. The commands in the
last section reproduce it.

## One-liner

A Redis-compatible in-memory key-value server written from scratch in C++20: a
single-threaded epoll event loop, the RESP2 protocol, TTL expiry, lists, pub/sub,
and append-only-file persistence. It works unchanged with `redis-cli`,
`redis-benchmark` and go-redis.

**Stack:** C++20, Linux (epoll, signalfd, fsync), CMake, AddressSanitizer +
UBSan, gprof, Python (stress testing), Bash, GitHub Actions, Docker.
**Size:** ~1,860 lines of C++ source, ~720 lines of tests.

## What it does

- **Networking:** non-blocking TCP server on one thread. It uses a level-triggered
  epoll loop, per-client read/write buffers, and full pipelining (it runs every
  complete command in a read, not just the first). Replies are batched so each
  client gets one `send()` per loop iteration. File descriptors are owned by an
  RAII wrapper.
- **Protocol:** an incremental RESP2 parser that never assumes a whole frame has
  arrived, plus the inline protocol (`nc` works). Error messages match Redis.
- **35 commands:** strings (`SET` with `EX/PX/EXAT/PXAT/NX/XX/GET/KEEPTTL`),
  `INCR`/`DECR` with overflow checks, key expiry (`EXPIRE`, `TTL`, `PERSIST`…),
  lists (`LPUSH/RPUSH/LPOP/RPOP/LRANGE`), pub/sub, `BGREWRITEAOF`, plus the
  `CONFIG`/`COMMAND`/`CLIENT` stubs that real clients send on connect.
- **Expiry:** lazy on lookup, plus an active cycle every 100 ms that samples keys
  with a TTL, the same algorithm Redis uses.
- **Persistence:** append-only log with `always` / `everysec` / `no` fsync policies.
  The log is written *before* replies are sent. Relative TTLs are logged as absolute
  timestamps. A truncated tail after a crash is recovered. Log compaction writes a
  temp file, fsyncs it, does an atomic `rename()`, then fsyncs the directory. The
  log is streamed in 4 MB chunks at startup.
- **Robustness:** a slow subscriber is disconnected at 8 MB of unsent output. A
  client that stops reading its replies is paused at 64 MB. Closes are deferred to
  the end of the loop iteration, which avoids fd-reuse and use-after-free bugs.
  SIGINT/SIGTERM shut the server down gracefully through `signalfd`.

## Verified metrics

Hardware: AMD Ryzen 7 7735HS (8 cores / 16 threads), NVMe SSD, loopback network,
Linux 7.0. Compared against Redis 7.0.15 with the same `redis-benchmark` flags
(`-t set,get -n 1000000 -c 50 -r 100000`). Both servers had AOF on with the same
fsync policy. Throughput numbers are the median of 3 runs; the p99 shown comes
from the median run.

### Throughput vs Redis (fsync = everysec)

| | pipeline | SET ops/s | SET p99 | GET ops/s | GET p99 |
|---|---|---|---|---|---|
| **kvstore** | 1 | 102,417 | 0.45 ms | 101,853 | 0.53 ms |
| Redis 7 | 1 | 102,176 | 0.56 ms | 100,786 | 0.40 ms |
| **kvstore** | 16 | **1,142,857** | 0.97 ms | **1,246,882** | 0.95 ms |
| Redis 7 | 16 | 735,835 | 1.61 ms | 1,157,407 | 1.05 ms |

Pipelined SET: kvstore **1.55×** Redis (1.52–1.57× in each of the 3 runs).
Unpipelined, the two are within 1% of each other.

### fsync = always (every write on disk before the reply; single run)

| | pipeline | SET ops/s | SET p99 |
|---|---|---|---|
| kvstore | 1 | 17,320 | 4.29 ms |
| Redis 7 | 1 | 16,225 | 6.33 ms |
| kvstore | 16 | 256,081 | 5.63 ms |
| Redis 7 | 16 | 119,047 | 26.99 ms |

These `always` numbers are noisy (disk-bound and only one run), so use them only to
show the cost of `fsync=always` (~6× slower than `everysec`). Don't use them as a
head-to-head comparison.

### Optimization (profile-driven)

gprof showed **77% of user CPU in the hash table's bucket-chain walk**: cache misses
caused by 96-byte entries (a `std::deque` stored inline in a `std::variant`). Server
CPU per operation was measured from `/proc` over 4 M pipelined ops, 3 runs each:

| change | user CPU / op | pipelined SET |
|---|---|---|
| baseline | 512 ns | ~0.96 M ops/s |
| entry 96 → 48 bytes (`unique_ptr` for lists) | 472 ns | ~1.0 M ops/s |
| AOF entries serialized in place (no per-write allocation) | **431 ns (−16%)** | **1.14 M ops/s (+19%)** |

The before/after rows were measured back to back in the same session on
2026-10-16. Absolute CPU/op drifts between sessions: the clean run on 2026-10-17
measured 480–522 ns/op user, ~380 ns/op sys, and 0.89–1.0 M SET/s, while Redis's
own numbers also dropped 5–10% that session. Quote the **relative** improvement
(−16% CPU/op), not the absolute ns.

### Correctness and reliability

| check | result |
|---|---|
| Compiler warnings (`-Wall -Wextra -Wpedantic`), release and ASan builds | **0** |
| Unit tests (parser, store, all command handlers) | 3 suites, **124 assertions**, all pass in release and ASan+UBSan builds |
| Integration tests with the real `redis-cli` | **37 checks** pass (commands, TTLs, pub/sub, inline protocol, `kill -9` recovery, truncated-log recovery, rewrite) |
| 2-minute fuzz-style stress test, ASan + UBSan build: 50 clients, random commands in random-sized fragments, malformed frames, slow subscribers | **6.38 M commands, 2,526 malformed frames, 360 slow subscribers cut off, 0 sanitizer reports, 0 client errors, clean exit** |
| `kill -9` durability: 10,000 acknowledged SETs, crash, restart | **10,000 / 10,000 recovered** (fsync `always` and `everysec`) |
| AOF replay equivalence: random traffic → dump → restart → dump | **identical**, both from the raw log (141,488 commands) and after compaction (94 commands) |
| AOF compaction | 95 MB log → 94 commands, rewritten in 22 ms, replays to the same keyspace |

## Resume bullets

Pick 2–3. All numbers are backed by the tables above.

- Built a **Redis-compatible key-value server in C++20** (epoll event loop, RESP2,
  35 commands, TTL expiry, pub/sub, append-only persistence) that works unchanged
  with redis-cli, redis-benchmark and go-redis.
- Reached **1.1–1.2 M ops/s** pipelined (**1.5× Redis 7** on SET under the same
  AOF settings) and matched Redis at ~100 K ops/s unpipelined, with sub-millisecond
  p99 latency.
- **Profiled with gprof and cut CPU per operation by 16%** (+19% throughput) by
  halving the hash-table entry size to fix cache misses and removing a per-write
  heap allocation on the persistence path.
- Built crash-safe persistence (write-ahead AOF, fsync policies, atomic
  `rename()` compaction, torn-write recovery): **10,000 / 10,000 acknowledged writes
  survive `kill -9`**, and log replay is verified byte-for-byte equivalent.
- Hardened with **AddressSanitizer/UBSan fuzz-style stress testing**: 6.4 M random
  and malformed commands from 50 concurrent clients with **zero memory errors**.
  This testing found and fixed a TTL replay-divergence bug.

Short version (one line): *"kvstore: Redis-compatible C++20 server (epoll, RESP2,
AOF persistence). 1.1 M+ ops/s pipelined, 1.5× Redis on SET, crash-safe, 0 ASan
errors over 6.4 M fuzzed commands."*

## Interview talking points

1. **Why one thread?** Commands never run concurrently, so there are no locks and
   every command is atomic. That is why `INCR` is safe from many clients. The cost
   of a command is tiny compared to the syscalls around it, so a second thread
   would mostly add contention.
2. **TCP is a byte stream.** One `read()` can return half a command or three
   commands. The parser returns *Incomplete* instead of guessing, and a unit test
   feeds a command one byte at a time.
3. **The replay bug (best story).** A random-traffic test compared the keyspace
   before and after a restart and found one key that came back to life. Cause:
   `INCR` on a key that was still alive (with a TTL) at runtime; on replay, the TTL
   had already passed, so `INCR` created a fresh key with no TTL. The fix, which is
   also Redis's design: log every expiry as an explicit `DEL`, and disable expiry
   while loading, so replay never has to decide what had expired.
4. **The optimization story.** Profile → hypothesis (cache misses from 96-byte
   nodes) → verify with `sizeof` → fix → measure CPU per op (a better metric than
   ops/s, because the benchmark client can be the bottleneck).
5. **Durability trade-offs.** `fsync=always` costs ~6× throughput. Batching every
   write in a loop iteration into one `fdatasync` (group commit) is what keeps it at
   21 K ops/s instead of a few thousand. `everysec` can lose up to ~1 s on power
   loss, but nothing on a process crash.
6. **Deferred close.** Closing a socket mid-iteration lets `accept()` hand out the
   same fd number while stale events for it are still in the batch, and it frees
   objects that `PUBLISH` may be iterating over.
7. **Why `rename()`?** It replaces a directory entry atomically, so a crash during
   compaction leaves the old log or the new one, never half of each.

## What NOT to claim

- Not "faster than Redis" in general. kvstore wins on pipelined SET (and GET by
  ~8%) because it does less (no memory accounting or eviction, no replication, no encodings). Unpipelined,
  the two are equal, because the network round trip dominates.
- No replication, clustering, transactions, hashes, sets or sorted sets.
- Compaction is synchronous (Redis forks a child for it), and `everysec` fsync runs
  on the main thread.
- The profile came from gprof (user-space only). perf was blocked on this machine,
  so there is no flame graph.
- Docker image and CI workflow are written but haven't been run yet (Docker isn't
  installed on the dev machine, and the repo isn't pushed).

## Reproduce

```bash
cmake --preset release && cmake --build --preset release
cmake --preset asan && cmake --build --preset asan
ctest --preset asan                              # unit tests
tests/integration.sh build/asan/kvstore          # redis-cli integration
tests/replay_check.sh build/asan/kvstore 20      # AOF replay equivalence
build/asan/kvstore --port 6395 & python3 tests/stress.py --port 6395 --seconds 120
bench/bench.sh build/release/kvstore everysec    # vs Redis
bench/cpu.sh build/release/kvstore 3             # CPU per op
```
