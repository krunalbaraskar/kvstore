#include "aof.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <variant>

#include "resp.h"
#include "store.h"

namespace {

bool write_all(int fd, std::string_view data) {
    while (!data.empty()) {
        ssize_t n = ::write(fd, data.data(), data.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        data.remove_prefix(static_cast<size_t>(n));
    }
    return true;
}

std::string errno_msg(const std::string& what) { return what + ": " + std::strerror(errno); }

// After rename() the new directory entry is durable only once the directory
// itself is fsynced.
bool fsync_dir_of(const std::string& path) {
    std::string dir = std::filesystem::path(path).parent_path().string();
    if (dir.empty()) dir = ".";
    Fd d(::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    return d.valid() && ::fsync(d.get()) == 0;
}

}  // namespace

bool Aof::open(const std::string& path, FsyncPolicy policy, std::string& err) {
    path_ = path;
    policy_ = policy;
    fd_.reset(::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644));
    if (!fd_.valid()) {
        err = errno_msg("open " + path);
        return false;
    }
    return true;
}

bool Aof::flush(int64_t now_mono_ms) {
    if (buf_.empty()) return true;
    if (!write_all(fd_.get(), buf_)) return false;
    buf_.clear();
    if (policy_ == FsyncPolicy::Always) {
        if (::fdatasync(fd_.get()) != 0) return false;
        last_fsync_ms_ = now_mono_ms;
    } else {
        unsynced_ = true;
    }
    return true;
}

bool Aof::tick(int64_t now_mono_ms) {
    if (policy_ != FsyncPolicy::EverySec || !unsynced_) return true;
    if (now_mono_ms - last_fsync_ms_ < 1000) return true;
    if (::fdatasync(fd_.get()) != 0) return false;
    unsynced_ = false;
    last_fsync_ms_ = now_mono_ms;
    return true;
}

bool Aof::sync() {
    if (!enabled()) return true;
    if (!write_all(fd_.get(), buf_)) return false;
    buf_.clear();
    unsynced_ = false;
    return ::fsync(fd_.get()) == 0;
}

bool Aof::rewrite(const Store& store, int64_t now_unix_ms, std::string& err) {
    if (!sync()) {
        err = errno_msg("flush before rewrite");
        return false;
    }
    std::string tmp = path_ + ".rewrite.tmp";
    Fd out(::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));
    if (!out.valid()) {
        err = errno_msg("open " + tmp);
        return false;
    }

    constexpr size_t kSpill = 1 << 20;
    constexpr size_t kListChunk = 64;
    std::string buf;
    buf.reserve(kSpill * 2);
    bool ok = true;
    auto spill = [&] {
        ok = ok && write_all(out.get(), buf);
        buf.clear();
    };

    for (const auto& [key, e] : store.entries()) {
        if (e.expire_at_ms >= 0 && e.expire_at_ms <= now_unix_ms) continue;
        if (const auto* s = std::get_if<std::string>(&e.value)) {
            resp::write_array_header(buf, e.expire_at_ms >= 0 ? 5 : 3);
            resp::write_bulk(buf, "SET");
            resp::write_bulk(buf, key);
            resp::write_bulk(buf, *s);
            if (e.expire_at_ms >= 0) {
                resp::write_bulk(buf, "PXAT");
                resp::write_bulk(buf, std::to_string(e.expire_at_ms));
            }
        } else {
            const List& list = *std::get<ListPtr>(e.value);
            for (size_t i = 0; i < list.size(); i += kListChunk) {
                size_t n = std::min(kListChunk, list.size() - i);
                resp::write_array_header(buf, n + 2);
                resp::write_bulk(buf, "RPUSH");
                resp::write_bulk(buf, key);
                for (size_t j = i; j < i + n; ++j) resp::write_bulk(buf, list[j]);
            }
            if (e.expire_at_ms >= 0) {
                resp::write_array_header(buf, 3);
                resp::write_bulk(buf, "PEXPIREAT");
                resp::write_bulk(buf, key);
                resp::write_bulk(buf, std::to_string(e.expire_at_ms));
            }
        }
        if (buf.size() >= kSpill) spill();
    }
    spill();

    if (!ok || ::fsync(out.get()) != 0) {
        err = errno_msg("write " + tmp);
        ::unlink(tmp.c_str());
        return false;
    }
    out.reset();
    // rename() atomically replaces the directory entry: a crash leaves either
    // the complete old file or the complete new one, never a mix.
    if (::rename(tmp.c_str(), path_.c_str()) != 0) {
        err = errno_msg("rename " + tmp);
        ::unlink(tmp.c_str());
        return false;
    }
    fsync_dir_of(path_);
    return open(path_, policy_, err);
}
