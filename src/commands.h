#pragma once

#include <cstdint>
#include <string>
#include <vector>

class Server;
class Store;
struct Client;

struct CommandContext {
    Server& server;
    Store& store;
    Client& client;
    std::string& out;  // replies are appended here
    int64_t now_ms;    // unix time in ms, fixed for the whole command
    // AOF buffer that write commands append to, or nullptr when nothing is
    // logged (AOF off, or replaying it). Handlers log relative times as
    // absolute ones (EXPIRE k 10 -> PEXPIREAT k <ms>) so that replaying the
    // log later gives the same result.
    std::string* aof;
    bool loading = false;  // replaying the AOF
};

// Executes a non-empty argv, appending the reply to ctx.out.
void execute_command(CommandContext& ctx, const std::vector<std::string>& argv);

// Logs keys the store deleted by expiry as DELs, then forgets them. aof may be
// nullptr.
void propagate_expired(Store& store, std::string* aof);
