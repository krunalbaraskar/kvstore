#include "log.h"

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <ctime>

void log_line(LogLevel level, std::string_view msg) {
    using namespace std::chrono;
    auto now = system_clock::now();
    std::time_t secs = system_clock::to_time_t(now);
    int ms = static_cast<int>(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
    std::tm tm{};
    localtime_r(&secs, &tm);
    char ts[32];
    std::strftime(ts, sizeof ts, "%d %b %Y %H:%M:%S", &tm);

    char tag = level == LogLevel::Info ? '*' : level == LogLevel::Warn ? '#' : '!';
    std::fprintf(stderr, "%d:M %s.%03d %c %.*s\n", static_cast<int>(getpid()), ts, ms, tag,
                 static_cast<int>(msg.size()), msg.data());
}
