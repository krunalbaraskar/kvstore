#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// RESP2 parsing and serialization.
// https://redis.io/docs/latest/develop/reference/protocol-spec/
namespace resp {

enum class ParseStatus { Ok, Incomplete, Error };

struct ParseResult {
    ParseStatus status = ParseStatus::Incomplete;
    size_t consumed = 0;  // frame length in bytes, set when status == Ok
    std::string error;    // set when status == Error
};

constexpr int64_t kMaxBulkLen = 512LL * 1024 * 1024;
constexpr int64_t kMaxArrayLen = 1024 * 1024;
constexpr size_t kMaxInlineLen = 64 * 1024;

// Parses one command from the start of buf: either a RESP array of bulk
// strings (what real clients send) or an inline command line (what you type
// into nc). Never assumes a whole frame is present: a partial frame returns
// Incomplete and leaves argv unspecified. An empty array or blank line
// returns Ok with an empty argv.
ParseResult parse_command(std::string_view buf, std::vector<std::string>& argv);

void write_simple(std::string& out, std::string_view s);
void write_error(std::string& out, std::string_view msg);
void write_int(std::string& out, int64_t v);
void write_bulk(std::string& out, std::string_view s);
void write_null(std::string& out);
void write_array_header(std::string& out, size_t n);
void write_null_array(std::string& out);
// Encodes argv as an array of bulk strings, the format commands travel in.
void write_command(std::string& out, const std::vector<std::string>& argv);

}  // namespace resp
