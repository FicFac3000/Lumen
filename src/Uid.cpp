#include "lumen/Uid.hpp"

#include <chrono>
#include <cstdio>

namespace lumen {

namespace {
uint64_t splitmix64(uint64_t& x) {
    uint64_t z = (x += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}
} // namespace

UidGen::UidGen(uint64_t seed) {
    if (seed == 0) {
        seed = static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        seed ^= static_cast<uint64_t>(
            std::chrono::system_clock::now().time_since_epoch().count());
    }
    state_ = seed ? seed : 0x1234567890abcdefull;
}

Uid UidGen::next() {
    // Session-seeded random high bits plus a counter: unique within a show
    // without coordination, and never reused within a session.
    uint64_t s = state_ + (++counter_);
    uint64_t v = splitmix64(s);
    if (v == 0) v = 1;  // 0 is reserved for "invalid"
    return Uid{v};
}

void UidGen::observe(Uid existing) {
    // Mix a loaded UID into the state so subsequent generation cannot land on
    // the same sequence a previous session produced.
    if (!existing.valid()) return;
    state_ ^= existing.v;
}

std::string toHex(Uid u) {
    char buf[19];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(u.v));
    return std::string(buf);
}

bool fromHex(const std::string& s, Uid& out) {
    if (s.empty() || s.size() > 16) return false;
    uint64_t v = 0;
    for (char c : s) {
        v <<= 4;
        if (c >= '0' && c <= '9') v |= static_cast<uint64_t>(c - '0');
        else if (c >= 'a' && c <= 'f') v |= static_cast<uint64_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= static_cast<uint64_t>(c - 'A' + 10);
        else return false;
    }
    out = Uid{v};
    return true;
}

} // namespace lumen
