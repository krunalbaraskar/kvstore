#include "resp.h"

#include <charconv>

namespace resp {

namespace {

constexpr size_t npos = std::string_view::npos;

// Position of the next "\r\n" at or after pos, or npos if not yet received.
size_t find_crlf(std::string_view buf, size_t pos) {
    for (size_t i = buf.find('\r', pos); i != npos; i = buf.find('\r', i + 1)) {
        if (i + 1 >= buf.size()) return npos;
        if (buf[i + 1] == '\n') return i;
    }
    return npos;
}

bool parse_int(std::string_view s, int64_t& out) {
    if (s.empty()) return false;
    auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && end == s.data() + s.size();
}

ParseResult incomplete() { return {}; }

ParseResult error(std::string msg) {
    ParseResult r;
    r.status = ParseStatus::Error;
    r.error = std::move(msg);
    return r;
}

ParseResult ok(size_t consumed) {
    ParseResult r;
    r.status = ParseStatus::Ok;
    r.consumed = consumed;
    return r;
}

// A header line ("*3\r\n", "$5\r\n") that has no CRLF yet. Real headers are a
// few bytes long, so a long run without CRLF means garbage, not a slow client.
ParseResult missing_crlf(std::string_view buf, size_t from) {
    if (buf.size() - from > kMaxInlineLen) return error("too big header line");
    return incomplete();
}

ParseResult parse_inline(std::string_view buf, std::vector<std::string>& argv) {
    size_t nl = buf.find('\n');
    if (nl == npos) {
        if (buf.size() > kMaxInlineLen) return error("too big inline request");
        return incomplete();
    }
    std::string_view line = buf.substr(0, nl);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

    argv.clear();
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
        if (i > start) argv.emplace_back(line.substr(start, i - start));
    }
    return ok(nl + 1);
}

}  // namespace

ParseResult parse_command(std::string_view buf, std::vector<std::string>& argv) {
    if (buf.empty()) return incomplete();
    if (buf[0] != '*') return parse_inline(buf, argv);

    size_t eol = find_crlf(buf, 1);
    if (eol == npos) return missing_crlf(buf, 0);
    int64_t count = 0;
    if (!parse_int(buf.substr(1, eol - 1), count) || count > kMaxArrayLen) {
        return error("invalid multibulk length");
    }
    size_t pos = eol + 2;
    argv.clear();
    if (count <= 0) return ok(pos);
    argv.reserve(static_cast<size_t>(count));

    for (int64_t i = 0; i < count; ++i) {
        if (pos >= buf.size()) return incomplete();
        if (buf[pos] != '$') {
            return error(std::string("expected '$', got '") + buf[pos] + "'");
        }
        eol = find_crlf(buf, pos + 1);
        if (eol == npos) return missing_crlf(buf, pos);
        int64_t len = 0;
        if (!parse_int(buf.substr(pos + 1, eol - pos - 1), len) || len < 0 || len > kMaxBulkLen) {
            return error("invalid bulk length");
        }
        size_t data = eol + 2;
        size_t ulen = static_cast<size_t>(len);
        // Length-prefixed, so the payload may itself contain "\r\n".
        if (buf.size() < data + ulen + 2) return incomplete();
        if (buf[data + ulen] != '\r' || buf[data + ulen + 1] != '\n') {
            return error("bulk string not terminated by CRLF");
        }
        argv.emplace_back(buf.substr(data, ulen));
        pos = data + ulen + 2;
    }
    return ok(pos);
}

namespace {

void append_int(std::string& out, int64_t v) {
    char tmp[24];
    auto [end, ec] = std::to_chars(tmp, tmp + sizeof tmp, v);
    out.append(tmp, end);
}

void append_prefixed(std::string& out, char prefix, int64_t v) {
    out.push_back(prefix);
    append_int(out, v);
    out.append("\r\n", 2);
}

}  // namespace

void write_simple(std::string& out, std::string_view s) {
    out.push_back('+');
    out.append(s);
    out.append("\r\n", 2);
}

void write_error(std::string& out, std::string_view msg) {
    out.push_back('-');
    out.append(msg);
    out.append("\r\n", 2);
}

void write_int(std::string& out, int64_t v) { append_prefixed(out, ':', v); }

void write_bulk(std::string& out, std::string_view s) {
    append_prefixed(out, '$', static_cast<int64_t>(s.size()));
    out.append(s);
    out.append("\r\n", 2);
}

void write_null(std::string& out) { out.append("$-1\r\n", 5); }

void write_array_header(std::string& out, size_t n) {
    append_prefixed(out, '*', static_cast<int64_t>(n));
}

void write_null_array(std::string& out) { out.append("*-1\r\n", 5); }

void write_command(std::string& out, const std::vector<std::string>& argv) {
    write_array_header(out, argv.size());
    for (const auto& a : argv) write_bulk(out, a);
}

}  // namespace resp
