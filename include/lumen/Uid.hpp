// Uid.hpp — immutable object identity (SPEC §8.1).
//
// Every show object carries three identities: a UID that never changes and is
// what every reference stores, a user-facing ID, and a name. Both of the latter
// are mutable; only the UID is a reference target.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace lumen {

struct Uid {
    uint64_t v = 0;

    constexpr bool valid() const { return v != 0; }
    constexpr bool operator==(const Uid& o) const { return v == o.v; }
    constexpr bool operator!=(const Uid& o) const { return v != o.v; }
    constexpr bool operator<(const Uid& o) const { return v < o.v; }
};

// Monotonic + session-seeded generator. UIDs are never reused, including after
// deletion, so a dangling reference stays detectably dangling rather than
// silently resolving to a different object.
class UidGen {
public:
    explicit UidGen(uint64_t seed = 0);
    Uid next();

    // Restores the generator past a loaded show's highest UID so a session that
    // opens an existing show cannot re-issue one of its UIDs.
    void observe(Uid existing);

private:
    uint64_t state_;
    uint64_t counter_ = 0;
};

std::string toHex(Uid u);
bool fromHex(const std::string& s, Uid& out);

} // namespace lumen

template <>
struct std::hash<lumen::Uid> {
    size_t operator()(const lumen::Uid& u) const noexcept {
        // UIDs are already well-mixed; identity hash is fine and cheap.
        return static_cast<size_t>(u.v);
    }
};
