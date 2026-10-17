#include "store.h"

#include <vector>

Entry* Store::find(const std::string& key, int64_t now_ms) {
    auto it = map_.find(key);
    if (it == map_.end()) return nullptr;
    int64_t at = it->second.expire_at_ms;
    if (expiry_enabled_ && at >= 0 && at <= now_ms) {
        expired_.push_back(key);
        volatile_.erase(key);
        map_.erase(it);
        return nullptr;
    }
    return &it->second;
}

Entry& Store::set(const std::string& key, Value value) {
    auto [it, inserted] = map_.try_emplace(key);
    Entry& e = it->second;
    if (!inserted && e.expire_at_ms >= 0) volatile_.erase(key);
    e.value = std::move(value);
    e.expire_at_ms = -1;
    return e;
}

bool Store::erase(const std::string& key) {
    auto it = map_.find(key);
    if (it == map_.end()) return false;
    if (it->second.expire_at_ms >= 0) volatile_.erase(key);
    map_.erase(it);
    return true;
}

void Store::clear() {
    map_.clear();
    volatile_.clear();
}

void Store::set_expire(const std::string& key, Entry& e, int64_t at_ms) {
    if (e.expire_at_ms < 0) volatile_.insert(key);
    e.expire_at_ms = at_ms;
}

void Store::clear_expire(const std::string& key, Entry& e) {
    if (e.expire_at_ms >= 0) volatile_.erase(key);
    e.expire_at_ms = -1;
}

size_t Store::active_expire(int64_t now_ms, std::chrono::microseconds budget) {
    constexpr size_t kSample = 20;
    constexpr size_t kMaxBucketProbes = 400;
    auto start = std::chrono::steady_clock::now();
    size_t deleted = 0;
    std::vector<std::string> sample;

    while (expiry_enabled_ && !volatile_.empty()) {
        // unordered_set has no random access, but its buckets do: pick random
        // buckets and take whatever keys live in them.
        sample.clear();
        std::uniform_int_distribution<size_t> pick(0, volatile_.bucket_count() - 1);
        for (size_t probes = 0; probes < kMaxBucketProbes && sample.size() < kSample; ++probes) {
            size_t b = pick(rng_);
            for (auto it = volatile_.begin(b); it != volatile_.end(b) && sample.size() < kSample; ++it) {
                sample.push_back(*it);
            }
        }
        if (sample.empty()) break;

        size_t expired = 0;
        for (const auto& key : sample) {
            auto it = map_.find(key);
            if (it != map_.end() && it->second.expire_at_ms <= now_ms) {
                expired_.push_back(key);
                volatile_.erase(key);
                map_.erase(it);
                ++expired;
            }
        }
        deleted += expired;

        if (expired * 4 <= sample.size()) break;
        if (std::chrono::steady_clock::now() - start > budget) break;
    }
    return deleted;
}
