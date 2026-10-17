#include <string>

#include "check.h"
#include "store.h"

namespace {

void test_set_find_erase() {
    Store s;
    s.set("a", std::string("1"));
    Entry* e = s.find("a", 0);
    CHECK(e != nullptr);
    CHECK_EQ(std::get<std::string>(e->value), "1");
    CHECK(s.erase("a"));
    CHECK(!s.erase("a"));
    CHECK(s.find("a", 0) == nullptr);
}

void test_lazy_expiry() {
    Store s;
    Entry& e = s.set("k", std::string("v"));
    s.set_expire("k", e, 1000);
    CHECK_EQ(s.volatile_size(), 1u);
    CHECK(s.find("k", 999) != nullptr);
    CHECK(s.find("k", 1000) == nullptr);
    CHECK_EQ(s.size(), 0u);
    CHECK_EQ(s.volatile_size(), 0u);
}

void test_set_clears_expiry() {
    Store s;
    Entry& e = s.set("k", std::string("v"));
    s.set_expire("k", e, 1000);
    s.set("k", std::string("w"));
    CHECK_EQ(s.volatile_size(), 0u);
    CHECK(s.find("k", 5000) != nullptr);
}

// Keys that are never read again must still be freed by the active cycle.
void test_active_expiry() {
    Store s;
    for (int i = 0; i < 10000; ++i) {
        std::string key = "key:" + std::to_string(i);
        Entry& e = s.set(key, std::string("v"));
        s.set_expire(key, e, i < 9000 ? 100 : 1'000'000);
    }
    for (int round = 0; round < 1000 && s.size() > 9100; ++round) {
        s.active_expire(200, std::chrono::milliseconds(50));
    }
    // The cycle stops once fewer than 25% of a sample are expired, so a few
    // stragglers may remain, but the bulk must be gone and live keys kept.
    CHECK(s.size() <= 9100);
    CHECK(s.size() >= 1000);
    for (int i = 9000; i < 10000; ++i) CHECK(s.find("key:" + std::to_string(i), 200) != nullptr);
}

}  // namespace

int main() {
    test_set_find_erase();
    test_lazy_expiry();
    test_set_clears_expiry();
    test_active_expiry();
    return finish("test_store");
}
