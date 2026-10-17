#pragma once

#include <sys/epoll.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "aof.h"
#include "fd.h"
#include "store.h"

struct Config {
    std::string bind = "127.0.0.1";
    uint16_t port = 6380;
    bool appendonly = true;
    std::string aof_path = "appendonly.aof";
    FsyncPolicy fsync = FsyncPolicy::EverySec;
    // A subscriber whose unsent output passes this is disconnected.
    size_t pubsub_output_limit = 8 * 1024 * 1024;
};

struct Client {
    explicit Client(Fd f) : fd(std::move(f)) {}

    size_t pending_output() const { return wbuf.size() - wpos; }
    bool subscribed() const { return !channels.empty(); }

    Fd fd;
    std::string rbuf;
    std::string wbuf;
    size_t wpos = 0;           // bytes of wbuf already sent
    uint32_t events = 0;       // epoll interest currently registered
    bool closing = false;      // will be closed at the end of this loop iteration
    bool close_after_reply = false;
    bool write_queued = false;  // listed in Server::pending_writes_
    std::unordered_set<std::string> channels;
};

// Single-threaded epoll server. One loop iteration: wait for events, read and
// execute every complete command, then (before sleeping again) write the AOF,
// send all queued replies, and close clients marked for closing.
class Server {
public:
    explicit Server(Config cfg);
    int run();  // returns a process exit code

    // Used by command handlers.
    Store& store() { return store_; }
    const Config& config() const { return cfg_; }
    size_t subscribe(Client& c, const std::string& channel);
    size_t unsubscribe(Client& c, const std::string& channel);
    size_t publish(const std::string& channel, const std::string& message);
    bool rewrite_aof(std::string& err);

private:
    bool load_aof();
    bool listen_socket();
    void accept_clients();
    void on_client_event(Client& c, uint32_t events);
    void read_from(Client& c);
    void process_input(Client& c);
    void run_command(Client& c, std::vector<std::string>& argv);
    void write_to(Client& c);
    void queue_write(Client& c);
    void update_interest(Client& c);
    void mark_closing(Client& c);
    void cron();
    void before_sleep();
    void fatal(const std::string& msg);

    Config cfg_;
    Store store_;
    Aof aof_;
    Fd epoll_;
    Fd listener_;
    Fd signals_;
    bool stop_ = false;
    int exit_code_ = 0;

    std::unordered_map<int, std::unique_ptr<Client>> clients_;
    std::vector<Client*> pending_writes_;
    std::vector<int> closing_;
    std::unordered_map<std::string, std::unordered_set<Client*>> channels_;
    std::vector<std::string> argv_;  // reused across commands
};
