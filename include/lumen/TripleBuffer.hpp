// TripleBuffer.hpp — lock-free single-producer / single-consumer handoff
// (SPEC §15.2).
//
// This replaces the mutex-plus-copy pattern the earlier draft used. That
// worked with two threads and would not survive four: the DMX, render and
// laser threads all need to read published state without ever blocking the
// control thread, and the laser thread must not block on anything at all.
//
// Three slots: the writer always has a free one to fill, the reader always has
// a complete one to read, and neither waits for the other. The reader may miss
// intermediate versions, which is correct for state — only the newest frame
// matters.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <utility>

namespace lumen {

template <typename T>
class TripleBuffer {
public:
    TripleBuffer() = default;

    // Producer side -------------------------------------------------------
    // The slot currently safe to write into.
    T& writeSlot() { return slot_[writing_]; }

    // Publishes the written slot and takes a new one. Lock-free.
    void publish() {
        const unsigned published = writing_;
        // Pick a slot that is neither the one just published nor the one the
        // reader has acquired.
        const unsigned held = held_.load(std::memory_order_acquire);
        unsigned next = 0;
        for (unsigned i = 0; i < 3; ++i) {
            if (i != published && i != held) {
                next = i;
                break;
            }
        }
        writing_ = next;
        latest_.store(published, std::memory_order_release);
        version_.fetch_add(1, std::memory_order_release);
    }

    // Consumer side -------------------------------------------------------
    // Acquires the newest published slot. Valid until the next acquire().
    const T& acquire() {
        const unsigned idx = latest_.load(std::memory_order_acquire);
        held_.store(idx, std::memory_order_release);
        return slot_[idx];
    }

    uint64_t version() const { return version_.load(std::memory_order_acquire); }

    // True if a newer version has been published since `seen`.
    bool fresh(uint64_t seen) const { return version() != seen; }

private:
    std::array<T, 3> slot_{};
    unsigned writing_ = 0;
    std::atomic<unsigned> latest_{0};
    std::atomic<unsigned> held_{2};
    std::atomic<uint64_t> version_{0};
};

} // namespace lumen
