#!/usr/bin/env python3
"""Hammer a running kvstore with random traffic to shake out memory bugs.

50 clients send random commands, pipelined and split into random-sized
fragments; some send protocol garbage; a few subscribe and never read, so the
server has to cut them off at the output-buffer limit. Run it against the
ASan/UBSan build and check the server log afterwards.

Usage: tests/stress.py [--port 6380] [--seconds 120] [--clients 50]
"""
import argparse
import random
import socket
import threading
import time

KEYS = [f"k{i}" for i in range(200)]
CHANNELS = ["c1", "c2", "c3"]


def encode(*args):
    out = [f"*{len(args)}\r\n".encode()]
    for a in args:
        b = a if isinstance(a, bytes) else str(a).encode()
        out.append(b"$%d\r\n%s\r\n" % (len(b), b))
    return b"".join(out)


def random_command(rng):
    k, v = rng.choice(KEYS), rng.randbytes(rng.choice([0, 1, 10, 100, 5000]))
    n = lambda: rng.randint(-5, 5)
    return rng.choice([
        lambda: ("SET", k, v),
        lambda: ("SET", k, v, "PX", rng.randint(1, 500)),
        lambda: ("SET", k, v, rng.choice(["NX", "XX"]), "GET"),
        lambda: ("GET", k),
        lambda: ("DEL", k, rng.choice(KEYS)),
        lambda: ("EXISTS", k),
        lambda: ("INCR", k),
        lambda: ("INCRBY", k, rng.randint(-2**63, 2**63 - 1)),
        lambda: ("EXPIRE", k, n()),
        lambda: ("TTL", k),
        lambda: ("LPUSH", k, v, v),
        lambda: ("RPUSH", k, v),
        lambda: ("LPOP", k),
        lambda: ("RPOP", k, rng.randint(0, 3)),
        lambda: ("LRANGE", k, n(), n()),
        lambda: ("TYPE", k),
        lambda: ("PUBLISH", rng.choice(CHANNELS), v),
        lambda: ("PING",),
        lambda: ("NOSUCHCMD", k),
        lambda: ("GET",),
    ])()


def read_replies(sock, count, buf):
    """Reads `count` complete RESP replies; returns leftover bytes."""
    def need(data, pos):
        # Returns the position after one reply, or None if incomplete.
        if pos >= len(data):
            return None
        eol = data.find(b"\r\n", pos)
        if eol < 0:
            return None
        t, body = data[pos:pos + 1], data[pos + 1:eol]
        if t in (b"+", b"-", b":"):
            return eol + 2
        n = int(body)
        if t == b"$":
            if n < 0:
                return eol + 2
            end = eol + 2 + n + 2
            return end if end <= len(data) else None
        if t == b"*":
            p = eol + 2
            for _ in range(max(n, 0)):
                p = need(data, p)
                if p is None:
                    return None
            return p
        raise ValueError(f"bad reply type {t!r}")

    pos = 0
    while count > 0:
        end = need(buf, pos)
        if end is None:
            chunk = sock.recv(65536)
            if not chunk:
                raise ConnectionError("server closed the connection")
            buf += chunk
            continue
        pos, count = end, count - 1
    return buf[pos:]


def worker(port, deadline, seed, stats):
    rng = random.Random(seed)
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=10) as s:
                buf = b""
                for _ in range(rng.randint(1, 200)):
                    if time.time() >= deadline:
                        break
                    if rng.random() < 0.01:
                        # Garbage: the server should reply with an error and close.
                        s.sendall(rng.choice([b"*abc\r\n", b"*1\r\n$-9\r\n", b"*1\r\n:1\r\n"]))
                        stats["garbage"] += 1
                        break
                    cmds = [random_command(rng) for _ in range(rng.randint(1, 50))]
                    data = b"".join(encode(*c) for c in cmds)
                    # Send in random fragments so frames straddle reads.
                    i = 0
                    while i < len(data):
                        j = i + rng.choice([1, 7, 64, 1500, len(data)])
                        s.sendall(data[i:j])
                        i = j
                    buf = read_replies(s, len(cmds), buf)
                    stats["commands"] += len(cmds)
        except (ConnectionError, socket.timeout, OSError) as e:
            stats["errors"] += 1
            stats["last_error"] = repr(e)


def slow_subscriber(port, deadline):
    """Subscribes and never reads, until the server disconnects it."""
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port)) as s:
                s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
                s.sendall(encode("SUBSCRIBE", *CHANNELS))
                while time.time() < deadline:
                    time.sleep(0.5)
                    s.sendall(encode("PING"))  # fails once the server hangs up
        except OSError:
            pass


def flooder(port, deadline):
    payload = b"x" * 64 * 1024
    with socket.create_connection(("127.0.0.1", port)) as s:
        while time.time() < deadline:
            s.sendall(encode("PUBLISH", "c1", payload) * 16)
            read_replies(s, 16, b"")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=6380)
    ap.add_argument("--seconds", type=float, default=120)
    ap.add_argument("--clients", type=int, default=50)
    args = ap.parse_args()

    deadline = time.time() + args.seconds
    stats = {"commands": 0, "errors": 0, "garbage": 0, "last_error": None}
    threads = [threading.Thread(target=worker, args=(args.port, deadline, i, stats))
               for i in range(args.clients)]
    threads += [threading.Thread(target=slow_subscriber, args=(args.port, deadline)) for _ in range(3)]
    threads.append(threading.Thread(target=flooder, args=(args.port, deadline)))
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    print(f"commands={stats['commands']} garbage_frames={stats['garbage']} "
          f"connection_errors={stats['errors']} last_error={stats['last_error']}")


if __name__ == "__main__":
    main()
