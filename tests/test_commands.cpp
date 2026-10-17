// Runs commands against a Server that is constructed but not started, which
// exercises the handlers without any networking.

#include <string>
#include <vector>

#include "check.h"
#include "clock.h"
#include "commands.h"
#include "resp.h"
#include "server.h"

namespace {

struct Harness {
    Server server{Config{}};
    Client client{Fd{}};
    std::vector<std::vector<std::string>> last_propagated;

    std::string run(std::vector<std::string> argv) {
        client.wbuf.clear();
        std::string aof;
        CommandContext ctx{server, server.store(), client, client.wbuf, unix_ms(), &aof};
        execute_command(ctx, argv);
        // Decode what the command logged.
        last_propagated.clear();
        std::string_view rest = aof;
        std::vector<std::string> cmd;
        while (!rest.empty()) {
            auto r = resp::parse_command(rest, cmd);
            if (r.status != resp::ParseStatus::Ok) break;
            last_propagated.push_back(cmd);
            rest.remove_prefix(r.consumed);
        }
        return client.wbuf;
    }
};

void test_strings() {
    Harness h;
    CHECK_EQ(h.run({"PING"}), "+PONG\r\n");
    CHECK_EQ(h.run({"ping", "hi"}), "$2\r\nhi\r\n");
    CHECK_EQ(h.run({"GET", "a"}), "$-1\r\n");
    CHECK_EQ(h.run({"SET", "a", "1"}), "+OK\r\n");
    CHECK_EQ(h.run({"GET", "a"}), "$1\r\n1\r\n");
    CHECK_EQ(h.run({"EXISTS", "a", "a", "b"}), ":2\r\n");
    CHECK_EQ(h.run({"DEL", "a", "b"}), ":1\r\n");
    CHECK_EQ(h.run({"DEL", "a"}), ":0\r\n");
    CHECK(h.last_propagated.empty());
    CHECK_EQ(h.run({"GET"}), "-ERR wrong number of arguments for 'get' command\r\n");
    CHECK_EQ(h.run({"NOPE", "x"}).rfind("-ERR unknown command 'NOPE'", 0), 0u);
    CHECK_EQ(h.run({"CONFIG", "GET", "appendonly"}), "*2\r\n$10\r\nappendonly\r\n$3\r\nyes\r\n");
    CHECK_EQ(h.run({"CONFIG", "GET", "nope"}), "*0\r\n");
}

void test_set_options() {
    Harness h;
    CHECK_EQ(h.run({"SET", "k", "v", "NX"}), "+OK\r\n");
    CHECK_EQ(h.run({"SET", "k", "w", "NX"}), "$-1\r\n");
    CHECK(h.last_propagated.empty());
    CHECK_EQ(h.run({"SET", "x", "w", "XX"}), "$-1\r\n");
    CHECK_EQ(h.run({"SET", "k", "w", "XX", "GET"}), "$1\r\nv\r\n");
    CHECK_EQ(h.run({"SET", "k", "v", "NX", "XX"}), "-ERR syntax error\r\n");
    CHECK_EQ(h.run({"SET", "k", "v", "EX", "0"}), "-ERR invalid expire time in 'set' command\r\n");
    CHECK_EQ(h.run({"SET", "k", "v", "EX", "abc"}), "-ERR value is not an integer or out of range\r\n");

    CHECK_EQ(h.run({"SET", "k", "v", "EX", "100"}), "+OK\r\n");
    CHECK_EQ(h.run({"TTL", "k"}), ":100\r\n");
    // EX is logged as an absolute PXAT so replay does not extend the TTL.
    CHECK_EQ(h.last_propagated.size(), 0u);  // TTL is read-only
    h.run({"SET", "k", "v", "PX", "5000"});
    CHECK_EQ(h.last_propagated.size(), 1u);
    CHECK_EQ(h.last_propagated[0].size(), 5u);
    CHECK_EQ(h.last_propagated[0][3], "PXAT");

    CHECK_EQ(h.run({"SET", "k", "z", "KEEPTTL"}), "+OK\r\n");
    CHECK(h.run({"PTTL", "k"}) != ":-1\r\n");
    CHECK_EQ(h.run({"SET", "k", "z"}), "+OK\r\n");
    CHECK_EQ(h.run({"TTL", "k"}), ":-1\r\n");
    CHECK_EQ(h.run({"TTL", "missing"}), ":-2\r\n");
}

void test_expire() {
    Harness h;
    CHECK_EQ(h.run({"EXPIRE", "k", "10"}), ":0\r\n");
    h.run({"SET", "k", "v"});
    CHECK_EQ(h.run({"EXPIRE", "k", "10"}), ":1\r\n");
    CHECK_EQ(h.last_propagated[0][0], "PEXPIREAT");
    CHECK_EQ(h.run({"PERSIST", "k"}), ":1\r\n");
    CHECK_EQ(h.run({"PERSIST", "k"}), ":0\r\n");
    CHECK_EQ(h.run({"EXPIRE", "k", "-1"}), ":1\r\n");
    CHECK_EQ(h.last_propagated[0][0], "DEL");
    CHECK_EQ(h.run({"EXISTS", "k"}), ":0\r\n");
    h.run({"SET", "k", "v"});
    CHECK_EQ(h.run({"PEXPIREAT", "k", "1"}), ":1\r\n");
    CHECK_EQ(h.run({"GET", "k"}), "$-1\r\n");
}

void test_counters() {
    Harness h;
    CHECK_EQ(h.run({"INCR", "n"}), ":1\r\n");
    CHECK_EQ(h.run({"INCRBY", "n", "41"}), ":42\r\n");
    CHECK_EQ(h.run({"DECR", "n"}), ":41\r\n");
    CHECK_EQ(h.run({"DECRBY", "n", "-9"}), ":50\r\n");
    h.run({"SET", "s", "abc"});
    CHECK_EQ(h.run({"INCR", "s"}), "-ERR value is not an integer or out of range\r\n");
    h.run({"SET", "big", "9223372036854775807"});
    CHECK_EQ(h.run({"INCR", "big"}), "-ERR increment or decrement would overflow\r\n");
    CHECK_EQ(h.run({"DECRBY", "n", "-9223372036854775808"}), "-ERR decrement would overflow\r\n");
    // INCR keeps an existing TTL.
    h.run({"SET", "t", "1", "EX", "100"});
    h.run({"INCR", "t"});
    CHECK_EQ(h.run({"TTL", "t"}), ":100\r\n");
}

void test_lists() {
    Harness h;
    CHECK_EQ(h.run({"RPUSH", "l", "a", "b", "c"}), ":3\r\n");
    CHECK_EQ(h.run({"LPUSH", "l", "z"}), ":4\r\n");
    CHECK_EQ(h.run({"LRANGE", "l", "0", "-1"}), "*4\r\n$1\r\nz\r\n$1\r\na\r\n$1\r\nb\r\n$1\r\nc\r\n");
    CHECK_EQ(h.run({"LRANGE", "l", "-2", "100"}), "*2\r\n$1\r\nb\r\n$1\r\nc\r\n");
    CHECK_EQ(h.run({"LRANGE", "l", "3", "1"}), "*0\r\n");
    CHECK_EQ(h.run({"LRANGE", "nope", "0", "-1"}), "*0\r\n");
    CHECK_EQ(h.run({"LPOP", "l"}), "$1\r\nz\r\n");
    CHECK_EQ(h.run({"RPOP", "l", "2"}), "*2\r\n$1\r\nc\r\n$1\r\nb\r\n");
    CHECK_EQ(h.run({"LLEN", "l"}), ":1\r\n");
    CHECK_EQ(h.run({"RPOP", "l"}), "$1\r\na\r\n");
    CHECK_EQ(h.run({"EXISTS", "l"}), ":0\r\n");  // empty lists are deleted
    CHECK_EQ(h.run({"LPOP", "l"}), "$-1\r\n");
    CHECK_EQ(h.run({"LPOP", "l", "1"}), "*-1\r\n");
    CHECK_EQ(h.run({"TYPE", "l"}), "+none\r\n");
}

void test_wrongtype() {
    Harness h;
    const std::string wrongtype =
        "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n";
    h.run({"SET", "s", "v"});
    h.run({"RPUSH", "l", "a"});
    CHECK_EQ(h.run({"LPUSH", "s", "x"}), wrongtype);
    CHECK_EQ(h.run({"GET", "l"}), wrongtype);
    CHECK_EQ(h.run({"INCR", "l"}), wrongtype);
    CHECK_EQ(h.run({"SET", "l", "x", "GET"}), wrongtype);
    CHECK_EQ(h.run({"TYPE", "l"}), "+list\r\n");
    CHECK_EQ(h.run({"SET", "l", "x"}), "+OK\r\n");  // plain SET overwrites any type
    CHECK_EQ(h.run({"TYPE", "l"}), "+string\r\n");
}

void test_subscribed_mode() {
    Harness h;
    CHECK_EQ(h.run({"SUBSCRIBE", "news"}), "*3\r\n$9\r\nsubscribe\r\n$4\r\nnews\r\n:1\r\n");
    CHECK_EQ(h.run({"GET", "a"}).rfind("-ERR Can't execute 'get'", 0), 0u);
    CHECK_EQ(h.run({"PING"}), "*2\r\n$4\r\npong\r\n$0\r\n\r\n");
    CHECK_EQ(h.run({"UNSUBSCRIBE"}), "*3\r\n$11\r\nunsubscribe\r\n$4\r\nnews\r\n:0\r\n");
    CHECK_EQ(h.run({"UNSUBSCRIBE"}), "*3\r\n$11\r\nunsubscribe\r\n$-1\r\n:0\r\n");
    CHECK_EQ(h.run({"GET", "a"}), "$-1\r\n");
}

}  // namespace

int main() {
    test_strings();
    test_set_options();
    test_expire();
    test_counters();
    test_lists();
    test_wrongtype();
    test_subscribed_mode();
    return finish("test_commands");
}
