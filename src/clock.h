#pragma once

#include <chrono>
#include <cstdint>

// Wall-clock time in ms. Used for key expiry because expiry times are
// persisted in the AOF and must mean the same thing after a restart.
inline int64_t unix_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// Monotonic time in ms. Used for timers inside the event loop, which must not
// jump when someone changes the system clock.
inline int64_t mono_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
