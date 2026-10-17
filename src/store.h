#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

using List = std::deque<std::string>;
// Lists live behind a pointer. A deque is 80 bytes; stored inline it would
// make every entry that big, strings included, and the extra cache misses on
// hash lookups were the top item in the profile (see README, Benchmarks).
using ListPtr = std::unique_ptr<List>;
using Value = std::variant<std::string, ListPtr>;

struct Entry {
    Value value;
    int64_t expire_at_ms = -1;  // absolute unix time in ms, -1 = no expiry
};

// The keyspace. Expiry is enforced two ways, like Redis: lazily, whenever a
// key is looked up, and actively, by sampling keys that carry a TTL so that
// keys nobody reads again still get freed.
class Store {
public:
    using Map = std::unordered_map<std::string, Entry>;

    // Returns the entry for key, or nullptr if it is missing or expired.
    // An expired entry is deleted on the way.
    Entry* find(const std::string& key, int64_t now_ms);

    // Inserts or replaces key with value and clears any expiry. The returned
    // reference stays valid until the key is erased.
    Entry& set(const std::string& key, Value value);

    bool erase(const std::string& key);
    void clear();

    void set_expire(const std::string& key, Entry& e, int64_t at_ms);
    void clear_expire(const std::string& key, Entry& e);

    // Samples keys with a TTL and deletes the expired ones, repeating while
    // more than a quarter of a sample was expired and the budget allows.
    // Returns how many keys were deleted.
    size_t active_expire(int64_t now_ms, std::chrono::microseconds budget);

    // Keys deleted by expiry since the last call to clear. The server logs
    // them to the AOF as DELs, so a replay never has to decide for itself
    // whether a key had expired at the time a later command ran.
    std::vector<std::string>& expired_keys() { return expired_; }

    // Turned off while the AOF is loading: the log already says exactly when
    // each key went away.
    void set_expiry_enabled(bool on) { expiry_enabled_ = on; }

    size_t size() const { return map_.size(); }
    size_t volatile_size() const { return volatile_.size(); }
    const Map& entries() const { return map_; }

private:
    Map map_;
    std::unordered_set<std::string> volatile_;  // keys that have an expiry
    std::vector<std::string> expired_;
    bool expiry_enabled_ = true;
    std::mt19937_64 rng_{0x6b7673746f7265ULL};
};
