#include <string>
#include <vector>

#include "check.h"
#include "resp.h"

using resp::ParseStatus;
using Argv = std::vector<std::string>;

namespace {

void test_full_command() {
    std::string buf = "*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n";
    Argv argv;
    auto r = resp::parse_command(buf, argv);
    CHECK(r.status == ParseStatus::Ok);
    CHECK_EQ(r.consumed, buf.size());
    CHECK((argv == Argv{"SET", "key", "value"}));
}

// Feeding one byte at a time must give Incomplete for every prefix and the
// same result as the whole buffer once the last byte arrives.
void test_one_byte_chunks() {
    std::string buf = "*3\r\n$3\r\nSET\r\n$4\r\nk\r\nk\r\n$0\r\n\r\n";
    Argv expected{"SET", "k\r\nk", ""};
    Argv argv;
    for (size_t len = 0; len < buf.size(); ++len) {
        auto r = resp::parse_command(std::string_view(buf).substr(0, len), argv);
        CHECK(r.status == ParseStatus::Incomplete);
    }
    auto r = resp::parse_command(buf, argv);
    CHECK(r.status == ParseStatus::Ok);
    CHECK_EQ(r.consumed, buf.size());
    CHECK(argv == expected);
}

void test_pipelined() {
    std::string one = "*1\r\n$4\r\nPING\r\n";
    std::string two = "*2\r\n$3\r\nGET\r\n$1\r\na\r\n";
    std::string buf = one + two + "*2\r\n$3\r\nGE";
    Argv argv;
    auto r1 = resp::parse_command(buf, argv);
    CHECK(r1.status == ParseStatus::Ok);
    CHECK_EQ(r1.consumed, one.size());
    CHECK((argv == Argv{"PING"}));

    auto r2 = resp::parse_command(std::string_view(buf).substr(r1.consumed), argv);
    CHECK(r2.status == ParseStatus::Ok);
    CHECK_EQ(r2.consumed, two.size());
    CHECK((argv == Argv{"GET", "a"}));

    auto r3 = resp::parse_command(std::string_view(buf).substr(r1.consumed + r2.consumed), argv);
    CHECK(r3.status == ParseStatus::Incomplete);
}

void test_inline() {
    Argv argv;
    auto r = resp::parse_command("  set  a   1\r\nGET a\n", argv);
    CHECK(r.status == ParseStatus::Ok);
    CHECK_EQ(r.consumed, 14u);
    CHECK((argv == Argv{"set", "a", "1"}));

    r = resp::parse_command("PING", argv);
    CHECK(r.status == ParseStatus::Incomplete);

    r = resp::parse_command("\r\n", argv);
    CHECK(r.status == ParseStatus::Ok);
    CHECK(argv.empty());
}

void test_errors() {
    Argv argv;
    CHECK(resp::parse_command("*1\r\n:5\r\n", argv).status == ParseStatus::Error);
    CHECK(resp::parse_command("*1\r\n$-5\r\n", argv).status == ParseStatus::Error);
    CHECK(resp::parse_command("*x\r\n", argv).status == ParseStatus::Error);
    CHECK(resp::parse_command("*1\r\n$3\r\nabcde\r\n", argv).status == ParseStatus::Error);
    CHECK(resp::parse_command("*99999999\r\n", argv).status == ParseStatus::Error);
    CHECK(resp::parse_command(std::string(70000, 'a'), argv).status == ParseStatus::Error);
}

void test_empty_array() {
    Argv argv{"stale"};
    auto r = resp::parse_command("*0\r\n", argv);
    CHECK(r.status == ParseStatus::Ok);
    CHECK_EQ(r.consumed, 4u);
    CHECK(argv.empty());
}

void test_writers() {
    std::string out;
    resp::write_simple(out, "OK");
    resp::write_error(out, "ERR x");
    resp::write_int(out, -42);
    resp::write_bulk(out, "hi");
    resp::write_bulk(out, "");
    resp::write_null(out);
    resp::write_array_header(out, 2);
    resp::write_null_array(out);
    CHECK_EQ(out, "+OK\r\n-ERR x\r\n:-42\r\n$2\r\nhi\r\n$0\r\n\r\n$-1\r\n*2\r\n*-1\r\n");

    out.clear();
    resp::write_command(out, {"DEL", "k"});
    CHECK_EQ(out, "*2\r\n$3\r\nDEL\r\n$1\r\nk\r\n");

    // Round trip through the parser.
    Argv argv;
    auto r = resp::parse_command(out, argv);
    CHECK(r.status == ParseStatus::Ok);
    CHECK((argv == Argv{"DEL", "k"}));
}

}  // namespace

int main() {
    test_full_command();
    test_one_byte_chunks();
    test_pipelined();
    test_inline();
    test_errors();
    test_empty_array();
    test_writers();
    return finish("test_resp");
}
