#include "commands.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <climits>
#include <initializer_list>
#include <string_view>
#include <unordered_map>

#include "resp.h"
#include "server.h"
#include "store.h"

namespace {

using Argv = std::vector<std::string>;
using Handler = void (*)(CommandContext&, const Argv&);

enum Flags : uint32_t {
    kNone = 0,
    kPubSubOk = 1,  // allowed while the client is subscribed to channels
};

struct Command {
    std::string_view name;  // lower case, as shown in error messages
    Handler fn;
    int arity;  // > 0: exact argc; < 0: at least -arity
    uint32_t flags;
};

constexpr std::string_view kWrongType =
    "WRONGTYPE Operation against a key holding the wrong kind of value";
constexpr std::string_view kNotInt = "ERR value is not an integer or out of range";
constexpr std::string_view kSyntax = "ERR syntax error";
constexpr std::string_view kOverflow = "ERR increment or decrement would overflow";

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::toupper(static_cast<unsigned char>(x)) ==
                      std::toupper(static_cast<unsigned char>(y));
           });
}

bool parse_i64(std::string_view s, int64_t& out) {
    if (s.empty()) return false;
    auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && end == s.data() + s.size();
}

void reply_wrong_args(CommandContext& ctx, std::string_view name) {
    resp::write_error(ctx.out, "ERR wrong number of arguments for '" + std::string(name) +
                                   "' command");
}

template <class T>
T* value_as(Entry& e) {
    return std::get_if<T>(&e.value);
}

template <>
List* value_as<List>(Entry& e) {
    auto* p = std::get_if<ListPtr>(&e.value);
    return p ? p->get() : nullptr;
}

// Looks up key and checks that it holds a T. Returns false (after replying
// WRONGTYPE) if it holds something else; otherwise out is the value or
// nullptr when the key does not exist.
template <class T>
bool lookup(CommandContext& ctx, const std::string& key, T*& out) {
    out = nullptr;
    Entry* e = ctx.store.find(key, ctx.now_ms);
    if (!e) return true;
    out = value_as<T>(*e);
    if (!out) {
        resp::write_error(ctx.out, kWrongType);
        return false;
    }
    return true;
}

// Converts an expire argument to absolute unix ms. False on overflow.
bool to_abs_ms(int64_t v, bool seconds, bool relative, int64_t now_ms, int64_t& out) {
    if (seconds && __builtin_mul_overflow(v, 1000, &v)) return false;
    if (relative && __builtin_add_overflow(v, now_ms, &v)) return false;
    out = v;
    return true;
}

// Logs a write to the AOF. Pending expiry DELs go first: those keys expired
// during this command's lookups, before the command itself took effect.
void propagate(CommandContext& ctx, std::initializer_list<std::string_view> args) {
    if (!ctx.aof) return;
    propagate_expired(ctx.store, ctx.aof);
    resp::write_array_header(*ctx.aof, args.size());
    for (std::string_view a : args) resp::write_bulk(*ctx.aof, a);
}

void propagate(CommandContext& ctx, const Argv& argv) {
    if (!ctx.aof) return;
    propagate_expired(ctx.store, ctx.aof);
    resp::write_command(*ctx.aof, argv);
}

// ---- connection / server ----------------------------------------------------

void cmd_ping(CommandContext& ctx, const Argv& argv) {
    if (argv.size() > 2) return reply_wrong_args(ctx, "ping");
    if (ctx.client.subscribed()) {
        resp::write_array_header(ctx.out, 2);
        resp::write_bulk(ctx.out, "pong");
        resp::write_bulk(ctx.out, argv.size() == 2 ? argv[1] : "");
    } else if (argv.size() == 2) {
        resp::write_bulk(ctx.out, argv[1]);
    } else {
        resp::write_simple(ctx.out, "PONG");
    }
}

void cmd_echo(CommandContext& ctx, const Argv& argv) { resp::write_bulk(ctx.out, argv[1]); }

void cmd_quit(CommandContext& ctx, const Argv&) {
    resp::write_simple(ctx.out, "OK");
    ctx.client.close_after_reply = true;
}

void cmd_select(CommandContext& ctx, const Argv& argv) {
    if (argv[1] == "0") {
        resp::write_simple(ctx.out, "OK");
    } else {
        resp::write_error(ctx.out, "ERR DB index is out of range");
    }
}

// redis-cli sends COMMAND DOCS on start-up; an empty answer just means no
// command hints.
void cmd_command(CommandContext& ctx, const Argv&) { resp::write_array_header(ctx.out, 0); }

// CONFIG GET for the few parameters clients ask about (redis-benchmark reads
// "save" and "appendonly"). Replies with name/value pairs, like Redis.
void cmd_config(CommandContext& ctx, const Argv& argv) {
    if (!iequals(argv[1], "GET") || argv.size() < 3) {
        return resp::write_error(ctx.out, "ERR only CONFIG GET <parameter> is supported");
    }
    const Config& cfg = ctx.server.config();
    const char* fsync = cfg.fsync == FsyncPolicy::Always     ? "always"
                        : cfg.fsync == FsyncPolicy::EverySec ? "everysec"
                                                             : "no";
    const std::pair<std::string_view, std::string> params[] = {
        {"save", ""},
        {"appendonly", cfg.appendonly ? "yes" : "no"},
        {"appendfsync", fsync},
        {"port", std::to_string(cfg.port)},
        {"bind", cfg.bind},
    };
    std::vector<const std::pair<std::string_view, std::string>*> found;
    for (size_t i = 2; i < argv.size(); ++i) {
        for (const auto& p : params) {
            if (iequals(argv[i], p.first) &&
                std::find(found.begin(), found.end(), &p) == found.end()) {
                found.push_back(&p);
            }
        }
    }
    resp::write_array_header(ctx.out, found.size() * 2);
    for (const auto* p : found) {
        resp::write_bulk(ctx.out, p->first);
        resp::write_bulk(ctx.out, p->second);
    }
}

// go-redis sends CLIENT SETINFO on connect.
void cmd_client(CommandContext& ctx, const Argv&) { resp::write_simple(ctx.out, "OK"); }

void cmd_dbsize(CommandContext& ctx, const Argv&) {
    resp::write_int(ctx.out, static_cast<int64_t>(ctx.store.size()));
}

void cmd_flushall(CommandContext& ctx, const Argv& argv) {
    ctx.store.clear();
    resp::write_simple(ctx.out, "OK");
    propagate(ctx, argv);
}

void cmd_bgrewriteaof(CommandContext& ctx, const Argv&) {
    std::string err;
    if (ctx.server.rewrite_aof(err)) {
        resp::write_simple(ctx.out, "Background append only file rewriting started");
    } else {
        resp::write_error(ctx.out, err);
    }
}

// ---- keys and strings -------------------------------------------------------

void cmd_get(CommandContext& ctx, const Argv& argv) {
    std::string* s;
    if (!lookup(ctx, argv[1], s)) return;
    if (s) {
        resp::write_bulk(ctx.out, *s);
    } else {
        resp::write_null(ctx.out);
    }
}

// SET key value [NX | XX] [GET] [EX s | PX ms | EXAT s | PXAT ms | KEEPTTL]
void cmd_set(CommandContext& ctx, const Argv& argv) {
    bool nx = false, xx = false, get = false, keepttl = false;
    int64_t expire_at = -1;
    for (size_t i = 3; i < argv.size(); ++i) {
        const std::string& opt = argv[i];
        bool ex = iequals(opt, "EX"), px = iequals(opt, "PX");
        bool exat = iequals(opt, "EXAT"), pxat = iequals(opt, "PXAT");
        if (iequals(opt, "NX") && !xx) {
            nx = true;
        } else if (iequals(opt, "XX") && !nx) {
            xx = true;
        } else if (iequals(opt, "GET")) {
            get = true;
        } else if (iequals(opt, "KEEPTTL") && expire_at < 0) {
            keepttl = true;
        } else if ((ex || px || exat || pxat) && !keepttl && expire_at < 0 &&
                   i + 1 < argv.size()) {
            int64_t v;
            if (!parse_i64(argv[++i], v)) return resp::write_error(ctx.out, kNotInt);
            if (v <= 0 || !to_abs_ms(v, ex || exat, ex || px, ctx.now_ms, expire_at)) {
                return resp::write_error(ctx.out, "ERR invalid expire time in 'set' command");
            }
        } else {
            return resp::write_error(ctx.out, kSyntax);
        }
    }

    const std::string& key = argv[1];
    Entry* e = ctx.store.find(key, ctx.now_ms);
    const std::string* old = e ? std::get_if<std::string>(&e->value) : nullptr;
    if (get && e && !old) return resp::write_error(ctx.out, kWrongType);

    bool skip = (nx && e) || (xx && !e);
    if (get) {
        // Reply before overwriting, since old points into the entry.
        if (old) {
            resp::write_bulk(ctx.out, *old);
        } else {
            resp::write_null(ctx.out);
        }
    } else if (skip) {
        resp::write_null(ctx.out);
    } else {
        resp::write_simple(ctx.out, "OK");
    }
    if (skip) return;

    if (keepttl && e) {
        e->value = argv[2];
    } else {
        Entry& fresh = ctx.store.set(key, argv[2]);
        if (expire_at >= 0) ctx.store.set_expire(key, fresh, expire_at);
    }

    if (expire_at >= 0) {
        propagate(ctx, {"SET", key, argv[2], "PXAT", std::to_string(expire_at)});
    } else if (keepttl) {
        propagate(ctx, {"SET", key, argv[2], "KEEPTTL"});
    } else {
        propagate(ctx, {"SET", key, argv[2]});
    }
}

void cmd_del(CommandContext& ctx, const Argv& argv) {
    int64_t n = 0;
    for (size_t i = 1; i < argv.size(); ++i) {
        // find() first so an already-expired key does not count as deleted.
        if (ctx.store.find(argv[i], ctx.now_ms) && ctx.store.erase(argv[i])) ++n;
    }
    resp::write_int(ctx.out, n);
    if (n > 0) propagate(ctx, argv);
}

void cmd_exists(CommandContext& ctx, const Argv& argv) {
    int64_t n = 0;
    for (size_t i = 1; i < argv.size(); ++i) {
        if (ctx.store.find(argv[i], ctx.now_ms)) ++n;
    }
    resp::write_int(ctx.out, n);
}

void cmd_type(CommandContext& ctx, const Argv& argv) {
    Entry* e = ctx.store.find(argv[1], ctx.now_ms);
    if (!e) return resp::write_simple(ctx.out, "none");
    resp::write_simple(ctx.out, std::holds_alternative<std::string>(e->value) ? "string" : "list");
}

void expire_generic(CommandContext& ctx, const Argv& argv, bool seconds, bool relative) {
    int64_t v, at;
    if (!parse_i64(argv[2], v)) return resp::write_error(ctx.out, kNotInt);
    if (!to_abs_ms(v, seconds, relative, ctx.now_ms, at)) {
        std::string name = argv[0];
        std::transform(name.begin(), name.end(), name.begin(), ::tolower);
        return resp::write_error(ctx.out, "ERR invalid expire time in '" + name + "' command");
    }
    const std::string& key = argv[1];
    Entry* e = ctx.store.find(key, ctx.now_ms);
    if (!e) return resp::write_int(ctx.out, 0);
    // While loading, keep the key: if it really expired before the next
    // command touched it, the log has a DEL for it.
    if (at <= ctx.now_ms && !ctx.loading) {
        ctx.store.erase(key);
        propagate(ctx, {"DEL", key});
    } else {
        ctx.store.set_expire(key, *e, at);
        propagate(ctx, {"PEXPIREAT", key, std::to_string(at)});
    }
    resp::write_int(ctx.out, 1);
}

void cmd_expire(CommandContext& ctx, const Argv& argv) { expire_generic(ctx, argv, true, true); }
void cmd_pexpire(CommandContext& ctx, const Argv& argv) { expire_generic(ctx, argv, false, true); }
void cmd_expireat(CommandContext& ctx, const Argv& argv) { expire_generic(ctx, argv, true, false); }
void cmd_pexpireat(CommandContext& ctx, const Argv& argv) { expire_generic(ctx, argv, false, false); }

void ttl_generic(CommandContext& ctx, const Argv& argv, bool ms) {
    Entry* e = ctx.store.find(argv[1], ctx.now_ms);
    if (!e) return resp::write_int(ctx.out, -2);
    if (e->expire_at_ms < 0) return resp::write_int(ctx.out, -1);
    int64_t left = e->expire_at_ms - ctx.now_ms;
    resp::write_int(ctx.out, ms ? left : (left + 500) / 1000);
}

void cmd_ttl(CommandContext& ctx, const Argv& argv) { ttl_generic(ctx, argv, false); }
void cmd_pttl(CommandContext& ctx, const Argv& argv) { ttl_generic(ctx, argv, true); }

void cmd_persist(CommandContext& ctx, const Argv& argv) {
    Entry* e = ctx.store.find(argv[1], ctx.now_ms);
    if (!e || e->expire_at_ms < 0) return resp::write_int(ctx.out, 0);
    ctx.store.clear_expire(argv[1], *e);
    resp::write_int(ctx.out, 1);
    propagate(ctx, argv);
}

void incr_generic(CommandContext& ctx, const Argv& argv, int64_t delta) {
    const std::string& key = argv[1];
    std::string* s;
    if (!lookup(ctx, key, s)) return;
    int64_t cur = 0, next;
    if (s && !parse_i64(*s, cur)) return resp::write_error(ctx.out, kNotInt);
    if (__builtin_add_overflow(cur, delta, &next)) return resp::write_error(ctx.out, kOverflow);
    if (s) {
        *s = std::to_string(next);  // in place, so the TTL survives
    } else {
        ctx.store.set(key, std::to_string(next));
    }
    resp::write_int(ctx.out, next);
    propagate(ctx, argv);
}

void cmd_incr(CommandContext& ctx, const Argv& argv) { incr_generic(ctx, argv, 1); }
void cmd_decr(CommandContext& ctx, const Argv& argv) { incr_generic(ctx, argv, -1); }

void cmd_incrby(CommandContext& ctx, const Argv& argv) {
    int64_t delta;
    if (!parse_i64(argv[2], delta)) return resp::write_error(ctx.out, kNotInt);
    incr_generic(ctx, argv, delta);
}

void cmd_decrby(CommandContext& ctx, const Argv& argv) {
    int64_t delta;
    if (!parse_i64(argv[2], delta)) return resp::write_error(ctx.out, kNotInt);
    if (delta == INT64_MIN) return resp::write_error(ctx.out, "ERR decrement would overflow");
    incr_generic(ctx, argv, -delta);
}

// ---- lists ------------------------------------------------------------------

void push_generic(CommandContext& ctx, const Argv& argv, bool left) {
    List* list;
    if (!lookup(ctx, argv[1], list)) return;
    if (!list) list = std::get<ListPtr>(ctx.store.set(argv[1], std::make_unique<List>()).value).get();
    for (size_t i = 2; i < argv.size(); ++i) {
        if (left) {
            list->push_front(argv[i]);
        } else {
            list->push_back(argv[i]);
        }
    }
    resp::write_int(ctx.out, static_cast<int64_t>(list->size()));
    propagate(ctx, argv);
}

void cmd_lpush(CommandContext& ctx, const Argv& argv) { push_generic(ctx, argv, true); }
void cmd_rpush(CommandContext& ctx, const Argv& argv) { push_generic(ctx, argv, false); }

// LPOP/RPOP key [count]
void pop_generic(CommandContext& ctx, const Argv& argv, bool left) {
    if (argv.size() > 3) return reply_wrong_args(ctx, left ? "lpop" : "rpop");
    bool with_count = argv.size() == 3;
    int64_t count = 1;
    if (with_count && (!parse_i64(argv[2], count) || count < 0)) {
        return resp::write_error(ctx.out, "ERR value is out of range, must be positive");
    }
    List* list;
    if (!lookup(ctx, argv[1], list)) return;
    if (!list) return with_count ? resp::write_null_array(ctx.out) : resp::write_null(ctx.out);

    size_t n = std::min(static_cast<size_t>(count), list->size());
    if (with_count) resp::write_array_header(ctx.out, n);
    for (size_t i = 0; i < n; ++i) {
        if (left) {
            resp::write_bulk(ctx.out, list->front());
            list->pop_front();
        } else {
            resp::write_bulk(ctx.out, list->back());
            list->pop_back();
        }
    }
    if (list->empty()) ctx.store.erase(argv[1]);
    if (n > 0) propagate(ctx, argv);
}

void cmd_lpop(CommandContext& ctx, const Argv& argv) { pop_generic(ctx, argv, true); }
void cmd_rpop(CommandContext& ctx, const Argv& argv) { pop_generic(ctx, argv, false); }

void cmd_llen(CommandContext& ctx, const Argv& argv) {
    List* list;
    if (!lookup(ctx, argv[1], list)) return;
    resp::write_int(ctx.out, list ? static_cast<int64_t>(list->size()) : 0);
}

void cmd_lrange(CommandContext& ctx, const Argv& argv) {
    int64_t start, stop;
    if (!parse_i64(argv[2], start) || !parse_i64(argv[3], stop)) {
        return resp::write_error(ctx.out, kNotInt);
    }
    List* list;
    if (!lookup(ctx, argv[1], list)) return;
    int64_t len = list ? static_cast<int64_t>(list->size()) : 0;
    if (start < 0) start += len;
    if (stop < 0) stop += len;
    if (start < 0) start = 0;
    if (stop >= len) stop = len - 1;
    if (start > stop) return resp::write_array_header(ctx.out, 0);

    resp::write_array_header(ctx.out, static_cast<size_t>(stop - start + 1));
    for (int64_t i = start; i <= stop; ++i) resp::write_bulk(ctx.out, (*list)[i]);
}

// ---- pub/sub ----------------------------------------------------------------

void reply_subscription(CommandContext& ctx, std::string_view kind, const std::string* channel,
                        size_t count) {
    resp::write_array_header(ctx.out, 3);
    resp::write_bulk(ctx.out, kind);
    if (channel) {
        resp::write_bulk(ctx.out, *channel);
    } else {
        resp::write_null(ctx.out);
    }
    resp::write_int(ctx.out, static_cast<int64_t>(count));
}

void cmd_subscribe(CommandContext& ctx, const Argv& argv) {
    for (size_t i = 1; i < argv.size(); ++i) {
        size_t n = ctx.server.subscribe(ctx.client, argv[i]);
        reply_subscription(ctx, "subscribe", &argv[i], n);
    }
}

void cmd_unsubscribe(CommandContext& ctx, const Argv& argv) {
    Argv channels(argv.begin() + 1, argv.end());
    if (channels.empty()) {
        channels.assign(ctx.client.channels.begin(), ctx.client.channels.end());
        std::sort(channels.begin(), channels.end());
    }
    if (channels.empty()) return reply_subscription(ctx, "unsubscribe", nullptr, 0);
    for (const auto& ch : channels) {
        size_t n = ctx.server.unsubscribe(ctx.client, ch);
        reply_subscription(ctx, "unsubscribe", &ch, n);
    }
}

void cmd_publish(CommandContext& ctx, const Argv& argv) {
    resp::write_int(ctx.out, static_cast<int64_t>(ctx.server.publish(argv[1], argv[2])));
}

// -----------------------------------------------------------------------------

const std::unordered_map<std::string, Command>& command_table() {
    static const auto table = [] {
        const Command commands[] = {
            {"ping", cmd_ping, -1, kPubSubOk},
            {"echo", cmd_echo, 2, kNone},
            {"quit", cmd_quit, 1, kPubSubOk},
            {"select", cmd_select, 2, kNone},
            {"command", cmd_command, -1, kNone},
            {"config", cmd_config, -2, kNone},
            {"client", cmd_client, -2, kNone},
            {"dbsize", cmd_dbsize, 1, kNone},
            {"flushall", cmd_flushall, -1, kNone},
            {"bgrewriteaof", cmd_bgrewriteaof, 1, kNone},
            {"get", cmd_get, 2, kNone},
            {"set", cmd_set, -3, kNone},
            {"del", cmd_del, -2, kNone},
            {"exists", cmd_exists, -2, kNone},
            {"type", cmd_type, 2, kNone},
            {"expire", cmd_expire, 3, kNone},
            {"pexpire", cmd_pexpire, 3, kNone},
            {"expireat", cmd_expireat, 3, kNone},
            {"pexpireat", cmd_pexpireat, 3, kNone},
            {"ttl", cmd_ttl, 2, kNone},
            {"pttl", cmd_pttl, 2, kNone},
            {"persist", cmd_persist, 2, kNone},
            {"incr", cmd_incr, 2, kNone},
            {"decr", cmd_decr, 2, kNone},
            {"incrby", cmd_incrby, 3, kNone},
            {"decrby", cmd_decrby, 3, kNone},
            {"lpush", cmd_lpush, -3, kNone},
            {"rpush", cmd_rpush, -3, kNone},
            {"lpop", cmd_lpop, -2, kNone},
            {"rpop", cmd_rpop, -2, kNone},
            {"llen", cmd_llen, 2, kNone},
            {"lrange", cmd_lrange, 4, kNone},
            {"subscribe", cmd_subscribe, -2, kPubSubOk},
            {"unsubscribe", cmd_unsubscribe, -1, kPubSubOk},
            {"publish", cmd_publish, 3, kNone},
        };
        std::unordered_map<std::string, Command> t;
        for (const auto& c : commands) t.emplace(std::string(c.name), c);
        return t;
    }();
    return table;
}

}  // namespace

void propagate_expired(Store& store, std::string* aof) {
    auto& keys = store.expired_keys();
    if (keys.empty()) return;
    if (aof) {
        for (const auto& key : keys) {
            resp::write_array_header(*aof, 2);
            resp::write_bulk(*aof, "DEL");
            resp::write_bulk(*aof, key);
        }
    }
    keys.clear();
}

void execute_command(CommandContext& ctx, const Argv& argv) {
    std::string name = argv[0];
    std::transform(name.begin(), name.end(), name.begin(), ::tolower);

    const auto& table = command_table();
    auto it = table.find(name);
    if (it == table.end()) {
        std::string msg = "ERR unknown command '" + argv[0] + "', with args beginning with:";
        for (size_t i = 1; i < argv.size() && i <= 3; ++i) msg += " '" + argv[i] + "'";
        return resp::write_error(ctx.out, msg);
    }
    const Command& cmd = it->second;
    int argc = static_cast<int>(argv.size());
    if ((cmd.arity > 0 && argc != cmd.arity) || (cmd.arity < 0 && argc < -cmd.arity)) {
        return reply_wrong_args(ctx, cmd.name);
    }
    if (ctx.client.subscribed() && !(cmd.flags & kPubSubOk)) {
        return resp::write_error(
            ctx.out, "ERR Can't execute '" + std::string(cmd.name) +
                         "': only (P|S)SUBSCRIBE / (P|S)UNSUBSCRIBE / PING / QUIT / RESET are "
                         "allowed in this context");
    }
    cmd.fn(ctx, argv);
}
