#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "fd.h"

class Store;

enum class FsyncPolicy { Always, EverySec, No };

// Append-only file. Write commands are buffered during an event-loop
// iteration and written out before any reply is sent, so a client never sees
// "+OK" for a write that is not at least in the page cache (or, with
// FsyncPolicy::Always, on disk).
class Aof {
public:
    bool open(const std::string& path, FsyncPolicy policy, std::string& err);
    bool enabled() const { return fd_.valid(); }

    // Where commands to log are appended; nullptr while the AOF is off.
    std::string* buffer() { return enabled() ? &buf_ : nullptr; }

    // write()s buffered commands; with Always also fsyncs. False on I/O error.
    bool flush(int64_t now_mono_ms);
    // With EverySec, fsyncs if a second has passed since the last fsync.
    bool tick(int64_t now_mono_ms);
    // Flushes and fsyncs regardless of policy (used at shutdown).
    bool sync();

    // Replaces the log with the smallest set of commands that rebuilds the
    // current keyspace: write a temp file, fsync it, rename() it over the old
    // one. Runs synchronously; Redis forks a child for this instead.
    bool rewrite(const Store& store, int64_t now_unix_ms, std::string& err);

private:
    std::string path_;
    Fd fd_;
    FsyncPolicy policy_ = FsyncPolicy::EverySec;
    std::string buf_;
    bool unsynced_ = false;
    int64_t last_fsync_ms_ = 0;
};
