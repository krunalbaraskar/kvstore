#pragma once

#include <sstream>
#include <string_view>

enum class LogLevel { Info, Warn, Error };

void log_line(LogLevel level, std::string_view msg);

template <class... Args>
void log_at(LogLevel level, const Args&... args) {
    std::ostringstream os;
    (os << ... << args);
    log_line(level, os.str());
}

template <class... Args>
void log_info(const Args&... args) { log_at(LogLevel::Info, args...); }
template <class... Args>
void log_warn(const Args&... args) { log_at(LogLevel::Warn, args...); }
template <class... Args>
void log_error(const Args&... args) { log_at(LogLevel::Error, args...); }
