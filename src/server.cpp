#include "server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "clock.h"
#include "commands.h"
#include "log.h"
#include "resp.h"

namespace {

constexpr int kCronIntervalMs = 100;
constexpr auto kActiveExpireBudget = std::chrono::microseconds(1000);
constexpr size_t kReadChunk = 64 * 1024;
constexpr int kMaxReadsPerEvent = 16;
// Stop reading from a client whose replies pile up faster than it reads them.
constexpr size_t kOutputPauseThreshold = 64 * 1024 * 1024;
constexpr size_t kMaxEvents = 1024;

std::string errno_str() { return std::strerror(errno); }

}  // namespace

Server::Server(Config cfg) : cfg_(std::move(cfg)) {}

int Server::run() {
    if (cfg_.appendonly) {
        if (!load_aof()) return 1;
        std::string err;
        if (!aof_.open(cfg_.aof_path, cfg_.fsync, err)) {
            log_error("Can't open the append-only file: ", err);
            return 1;
        }
    }

    epoll_.reset(::epoll_create1(EPOLL_CLOEXEC));
    if (!epoll_.valid()) {
        log_error("epoll_create1: ", errno_str());
        return 1;
    }
    if (!listen_socket()) return 1;

    // Deliver SIGINT/SIGTERM through an fd so shutdown happens inside the
    // loop, where flushing the AOF is safe.
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigprocmask(SIG_BLOCK, &mask, nullptr);
    signals_.reset(::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC));
    ::signal(SIGPIPE, SIG_IGN);

    for (int fd : {listener_.get(), signals_.get()}) {
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = fd;
        ::epoll_ctl(epoll_.get(), EPOLL_CTL_ADD, fd, &ev);
    }
    log_info("Ready to accept connections tcp on ", cfg_.bind, ":", cfg_.port);

    std::vector<epoll_event> events(kMaxEvents);
    int64_t next_cron = mono_ms() + kCronIntervalMs;
    while (!stop_) {
        int timeout = static_cast<int>(std::max<int64_t>(0, next_cron - mono_ms()));
        int n = ::epoll_wait(epoll_.get(), events.data(), static_cast<int>(events.size()), timeout);
        if (n < 0 && errno != EINTR) {
            fatal("epoll_wait: " + errno_str());
            break;
        }
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            if (fd == listener_.get()) {
                accept_clients();
            } else if (fd == signals_.get()) {
                signalfd_siginfo info;
                if (::read(fd, &info, sizeof info) == sizeof info) {
                    log_info("Received ", strsignal(static_cast<int>(info.ssi_signo)),
                             ", shutting down");
                    stop_ = true;
                }
            } else if (auto it = clients_.find(fd); it != clients_.end()) {
                on_client_event(*it->second, events[i].events);
            }
        }
        if (mono_ms() >= next_cron) {
            cron();
            next_cron = mono_ms() + kCronIntervalMs;
        }
        before_sleep();
    }

    if (!aof_.sync()) {
        log_error("Error syncing the AOF at shutdown: ", errno_str());
        return 1;
    }
    log_info("Bye");
    return exit_code_;
}

// Replays the AOF through the normal command path, streaming it in chunks so
// a large log does not have to fit in memory next to the dataset.
bool Server::load_aof() {
    Fd in(::open(cfg_.aof_path.c_str(), O_RDONLY | O_CLOEXEC));
    if (!in.valid()) {
        if (errno == ENOENT) return true;  // first start, nothing to load
        log_error("open ", cfg_.aof_path, ": ", errno_str());
        return false;
    }

    constexpr size_t kChunk = 4 * 1024 * 1024;
    int64_t start = mono_ms();
    Client loader{Fd{}};
    std::vector<std::string> argv;
    store_.set_expiry_enabled(false);
    std::string buf;
    size_t head = 0;       // start of the unparsed part of buf
    uint64_t offset = 0;   // file offset of buf[head]
    size_t commands = 0;
    for (bool eof = false;;) {
        while (head < buf.size()) {
            std::string_view rest = std::string_view(buf).substr(head);
            resp::ParseResult r = rest[0] == '*' ? resp::parse_command(rest, argv)
                                                 : resp::ParseResult{resp::ParseStatus::Error,
                                                                     0, "expected '*'"};
            if (r.status == resp::ParseStatus::Incomplete) break;
            if (r.status == resp::ParseStatus::Error) {
                log_error("Bad AOF format at offset ", offset, ": ", r.error);
                return false;
            }
            head += r.consumed;
            offset += r.consumed;
            if (argv.empty()) continue;

            CommandContext ctx{*this, store_, loader, loader.wbuf, unix_ms(), nullptr, true};
            execute_command(ctx, argv);
            if (!loader.wbuf.empty() && loader.wbuf[0] == '-') {
                log_warn("AOF command ", argv[0], " failed: ", loader.wbuf.substr(1));
            }
            loader.wbuf.clear();
            ++commands;
        }
        if (eof) break;

        buf.erase(0, head);
        head = 0;
        size_t old = buf.size();
        buf.resize(old + kChunk);
        ssize_t n;
        do {
            n = ::read(in.get(), buf.data() + old, kChunk);
        } while (n < 0 && errno == EINTR);
        if (n < 0) {
            log_error("read ", cfg_.aof_path, ": ", errno_str());
            return false;
        }
        buf.resize(old + static_cast<size_t>(n));
        eof = n == 0;
    }

    if (head < buf.size()) {
        // The server died halfway through writing the last command. The
        // client never got a reply for it, so dropping it is correct.
        log_warn("AOF ends with a truncated command (", buf.size() - head, " bytes at offset ",
                 offset, "); truncating the file");
        if (::truncate(cfg_.aof_path.c_str(), static_cast<off_t>(offset)) != 0) {
            log_error("truncate: ", errno_str());
            return false;
        }
    }
    store_.set_expiry_enabled(true);
    log_info("DB loaded from append only file: ", commands, " commands, ", store_.size(),
             " keys in ", mono_ms() - start, " ms");
    return true;
}

bool Server::listen_socket() {
    listener_.reset(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!listener_.valid()) {
        log_error("socket: ", errno_str());
        return false;
    }
    int one = 1;
    ::setsockopt(listener_.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg_.port);
    if (::inet_pton(AF_INET, cfg_.bind.c_str(), &addr.sin_addr) != 1) {
        log_error("Invalid bind address: ", cfg_.bind);
        return false;
    }
    if (::bind(listener_.get(), reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        log_error("bind ", cfg_.bind, ":", cfg_.port, ": ", errno_str());
        return false;
    }
    if (::listen(listener_.get(), 511) != 0) {
        log_error("listen: ", errno_str());
        return false;
    }
    return true;
}

void Server::accept_clients() {
    for (;;) {
        int fd = ::accept4(listener_.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) log_warn("accept: ", errno_str());
            return;
        }
        auto client = std::make_unique<Client>(Fd(fd));
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = fd;
        if (::epoll_ctl(epoll_.get(), EPOLL_CTL_ADD, fd, &ev) != 0) {
            log_warn("epoll_ctl add: ", errno_str());
            continue;  // client's destructor closes fd
        }
        client->events = EPOLLIN;
        clients_.emplace(fd, std::move(client));
    }
}

void Server::on_client_event(Client& c, uint32_t events) {
    if (c.closing) return;
    if (events & EPOLLERR) return mark_closing(c);
    if (events & (EPOLLIN | EPOLLHUP)) read_from(c);
    if (!c.closing && (events & EPOLLOUT)) {
        write_to(c);
        // Input left behind while output was paused can now be served.
        if (!c.closing && !c.rbuf.empty() && c.pending_output() < kOutputPauseThreshold) {
            process_input(c);
        }
    }
}

void Server::read_from(Client& c) {
    char buf[kReadChunk];
    for (int i = 0; i < kMaxReadsPerEvent; ++i) {
        ssize_t n = ::read(c.fd.get(), buf, sizeof buf);
        if (n > 0) {
            c.rbuf.append(buf, static_cast<size_t>(n));
            if (static_cast<size_t>(n) < sizeof buf) break;  // drained the socket
            continue;
        }
        if (n == 0) return mark_closing(c);  // peer closed
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        return mark_closing(c);
    }
    process_input(c);
}

void Server::process_input(Client& c) {
    size_t pos = 0;
    while (!c.closing && !c.close_after_reply && pos < c.rbuf.size()) {
        if (!c.subscribed() && c.pending_output() >= kOutputPauseThreshold) break;
        resp::ParseResult r =
            resp::parse_command(std::string_view(c.rbuf).substr(pos), argv_);
        if (r.status == resp::ParseStatus::Incomplete) break;
        if (r.status == resp::ParseStatus::Error) {
            resp::write_error(c.wbuf, "ERR Protocol error: " + r.error);
            c.close_after_reply = true;
            pos = c.rbuf.size();
            break;
        }
        pos += r.consumed;
        if (!argv_.empty()) run_command(c, argv_);
    }
    c.rbuf.erase(0, pos);
    if (c.pending_output() > 0 || c.close_after_reply) queue_write(c);
    update_interest(c);
}

void Server::run_command(Client& c, std::vector<std::string>& argv) {
    CommandContext ctx{*this, store_, c, c.wbuf, unix_ms(), aof_.buffer()};
    execute_command(ctx, argv);
    // A read command can expire keys too; log those.
    propagate_expired(store_, aof_.buffer());
}

void Server::queue_write(Client& c) {
    if (c.write_queued) return;
    c.write_queued = true;
    pending_writes_.push_back(&c);
}

void Server::write_to(Client& c) {
    while (c.wpos < c.wbuf.size()) {
        ssize_t n = ::send(c.fd.get(), c.wbuf.data() + c.wpos, c.wbuf.size() - c.wpos,
                           MSG_NOSIGNAL);
        if (n > 0) {
            c.wpos += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        return mark_closing(c);
    }
    if (c.wpos == c.wbuf.size()) {
        c.wbuf.clear();
        c.wpos = 0;
        if (c.close_after_reply) return mark_closing(c);
    }
    update_interest(c);
}

// Level-triggered interest: EPOLLOUT only while there is something to send,
// otherwise epoll_wait would report the socket writable on every call.
void Server::update_interest(Client& c) {
    if (c.closing) return;
    uint32_t want = 0;
    if (!c.close_after_reply && (c.subscribed() || c.pending_output() < kOutputPauseThreshold)) {
        want |= EPOLLIN;
    }
    if (c.pending_output() > 0) want |= EPOLLOUT;
    if (want == c.events) return;

    epoll_event ev{};
    ev.events = want;
    ev.data.fd = c.fd.get();
    if (::epoll_ctl(epoll_.get(), EPOLL_CTL_MOD, c.fd.get(), &ev) != 0) return mark_closing(c);
    c.events = want;
}

// Closing is deferred to the end of the loop iteration. Closing right away
// would let accept() reuse the fd number while later events in the same
// epoll_wait batch still refer to the old client, and would free Client
// objects that PUBLISH may be iterating over.
void Server::mark_closing(Client& c) {
    if (c.closing) return;
    c.closing = true;
    closing_.push_back(c.fd.get());
}

size_t Server::subscribe(Client& c, const std::string& channel) {
    if (c.channels.insert(channel).second) channels_[channel].insert(&c);
    return c.channels.size();
}

size_t Server::unsubscribe(Client& c, const std::string& channel) {
    if (c.channels.erase(channel) > 0) {
        auto it = channels_.find(channel);
        if (it != channels_.end()) {
            it->second.erase(&c);
            if (it->second.empty()) channels_.erase(it);
        }
    }
    return c.channels.size();
}

size_t Server::publish(const std::string& channel, const std::string& message) {
    auto it = channels_.find(channel);
    if (it == channels_.end()) return 0;
    size_t receivers = 0;
    for (Client* sub : it->second) {
        if (sub->closing) continue;
        resp::write_array_header(sub->wbuf, 3);
        resp::write_bulk(sub->wbuf, "message");
        resp::write_bulk(sub->wbuf, channel);
        resp::write_bulk(sub->wbuf, message);
        ++receivers;
        if (sub->pending_output() > cfg_.pubsub_output_limit) {
            log_warn("Client fd=", sub->fd.get(), " exceeded the pub/sub output limit (",
                     sub->pending_output(), " bytes); disconnecting");
            mark_closing(*sub);
        } else {
            queue_write(*sub);
        }
    }
    return receivers;
}

bool Server::rewrite_aof(std::string& err) {
    if (!aof_.enabled()) {
        err = "ERR append only file is disabled";
        return false;
    }
    int64_t start = mono_ms();
    if (!aof_.rewrite(store_, unix_ms(), err)) {
        err = "ERR AOF rewrite failed: " + err;
        log_error(err);
        return false;
    }
    log_info("AOF rewritten: ", store_.size(), " keys in ", mono_ms() - start, " ms");
    return true;
}

void Server::cron() {
    store_.active_expire(unix_ms(), kActiveExpireBudget);
    propagate_expired(store_, aof_.buffer());
    if (aof_.enabled() && !(aof_.flush(mono_ms()) && aof_.tick(mono_ms()))) {
        fatal("Error writing the AOF: " + errno_str());
    }
}

void Server::before_sleep() {
    // AOF first: a client must not see a reply for a write that was not logged.
    if (aof_.enabled() && !aof_.flush(mono_ms())) {
        fatal("Error writing the AOF: " + errno_str());
        return;
    }
    for (Client* c : pending_writes_) {
        c->write_queued = false;
        if (!c->closing) write_to(*c);
    }
    pending_writes_.clear();

    for (int fd : closing_) {
        auto it = clients_.find(fd);
        if (it == clients_.end()) continue;
        Client& c = *it->second;
        for (const auto& ch : std::vector<std::string>(c.channels.begin(), c.channels.end())) {
            unsubscribe(c, ch);
        }
        ::epoll_ctl(epoll_.get(), EPOLL_CTL_DEL, fd, nullptr);
        clients_.erase(it);  // Fd destructor closes the socket
    }
    closing_.clear();
}

void Server::fatal(const std::string& msg) {
    log_error(msg);
    exit_code_ = 1;
    stop_ = true;
}
