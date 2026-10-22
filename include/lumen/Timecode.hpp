// Timecode.hpp — timecode sources and slots (SPEC §18.5).
//
// Feeds §11.2's cue triggers. Four sources, several independent slots, and one
// trap worth naming: DROP-FRAME.
//
// 29.97 fps drop-frame does not drop frames. It drops frame NUMBERS — two of
// them at the start of every minute except every tenth minute — so that a
// timecode reading tracks wall-clock time on a 30/1.001 system. Treating it as
// plain 30 fps drifts by 3.6 seconds an hour, which over a show is the
// difference between a cue landing on the downbeat and landing visibly late.
// So the conversion is implemented properly and tested against known values
// rather than assumed.
//
// A slot also reports DRIFT: the difference between where the incoming
// timecode says we are and where free-running from the last frame would put
// us. §18.5 asks for it to be visible, because a drifting source is the thing
// that explains cues firing slightly wrong all evening.
#pragma once

#include "lumen/Uid.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace lumen {

// ---------------------------------------------------------------------------
// Frame rates
// ---------------------------------------------------------------------------
enum class FrameRate : uint8_t {
    Fps24,
    Fps25,
    Fps2997Drop,   // 30/1.001 with dropped frame NUMBERS
    Fps30,
};

const char* frameRateName(FrameRate r);
// Nominal frames per labelled second — 30 for drop-frame, because its frame
// NUMBERS run 0..29.
int framesPerSecond(FrameRate r);
// Actual frames per real second, which is where 29.97 differs.
double realFrameRate(FrameRate r);
bool isDropFrame(FrameRate r);

// ---------------------------------------------------------------------------
// A timecode reading
// ---------------------------------------------------------------------------
struct Timecode {
    int hours = 0;
    int minutes = 0;
    int seconds = 0;
    int frames = 0;

    bool operator==(const Timecode& o) const {
        return hours == o.hours && minutes == o.minutes && seconds == o.seconds &&
               frames == o.frames;
    }
    bool operator!=(const Timecode& o) const { return !(*this == o); }

    // "01:02:03:04", or "01:02:03;04" for drop-frame, which is the
    // conventional way to show at a glance which one you are looking at.
    std::string text(FrameRate rate = FrameRate::Fps25) const;
};

// A drop-frame reading that names a dropped frame number cannot exist. These
// say so rather than silently accepting it.
bool isValidTimecode(const Timecode& tc, FrameRate rate);

// Total frame count from 00:00:00:00, which is the form arithmetic is easy in.
int64_t toFrames(const Timecode& tc, FrameRate rate);
Timecode fromFrames(int64_t frames, FrameRate rate);

double toSeconds(const Timecode& tc, FrameRate rate);
Timecode fromSeconds(double seconds, FrameRate rate);

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------
enum class TimecodeSource : uint8_t {
    Internal,   // free-running, for programming without an external source
    Ltc,        // audio-embedded, needs a decoder
    Mtc,        // MIDI timecode
    ArtNet,     // network
};

const char* timecodeSourceName(TimecodeSource s);

struct TimecodeSlot {
    Uid uid{};
    int id = 0;
    std::string name;
    TimecodeSource source = TimecodeSource::Internal;
    FrameRate rate = FrameRate::Fps25;

    bool running = false;
    Timecode position;

    // --- state §18.5 asks to be visible ----------------------------------
    double lastFrameAt = 0.0;   // wall time of the last external frame
    bool everFed = false;
    // Positive means the incoming code is ahead of free-running from the last
    // frame; negative means behind. In frames, because that is the unit an
    // operator reasons about.
    double driftFrames = 0.0;
    // No frames for longer than the timeout. A stale source keeps its last
    // position rather than jumping to zero, but says it is stale — a frozen
    // reading that claims to be live is how a dead feed goes unnoticed.
    bool stale = false;
    // A reading that went backwards or jumped: the source was scrubbed, or
    // the decoder lost lock.
    uint64_t jumps = 0;
    uint64_t frames = 0;

    // Where the slot is now, free-running from the last external frame when
    // running. For Internal this is the whole mechanism.
    Timecode at(double now) const;
    double secondsAt(double now) const;
};

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------
class TimecodeEngine {
public:
    explicit TimecodeEngine(UidGen& uids) : uids_(&uids) {}

    void setClock(std::function<double()> clock) { clock_ = std::move(clock); }
    double now() const { return clock_ ? clock_() : 0.0; }

    // §18.5: several timelines run independently.
    TimecodeSlot& add(const std::string& name, TimecodeSource source,
                      FrameRate rate = FrameRate::Fps25);
    TimecodeSlot& restore(TimecodeSlot s);
    TimecodeSlot* slot(int id);
    const TimecodeSlot* slot(int id) const;
    TimecodeSlot* byName(const std::string& name);
    TimecodeSlot* byUid(Uid uid);
    const std::deque<TimecodeSlot>& slots() const { return slots_; }
    bool remove(int id);
    size_t size() const { return slots_.size(); }

    // An external frame arrived. Returns false for a reading the rate cannot
    // express, which is a decoder fault worth surfacing rather than smoothing.
    bool feed(int id, const Timecode& tc);

    // Internal transport, and manual control of an external slot for
    // programming without the source running.
    bool start(int id);
    bool stop(int id);
    bool setPosition(int id, const Timecode& tc);

    // How long without a frame before a slot is called stale. Two frames is
    // too tight for a network source; a quarter second is the usual tolerance.
    void setTimeout(double seconds) { timeout_ = std::max(0.0, seconds); }
    double timeout() const { return timeout_; }

    // Marks stale slots. Called from the engine's tick.
    int tick();

    // --- cue triggers (§11.2) ---------------------------------------------
    // Did a slot pass a given timecode since the last call? That is the whole
    // question a timecode-triggered cue asks, and asking it as "passed"
    // rather than "equals" is what makes it survive a dropped frame.
    struct Crossing {
        int slot = 0;
        Timecode at;
        bool forward = true;
    };
    // Registers a trigger point. Returns the crossings observed on this pass.
    void watch(int slot, const Timecode& tc);
    void unwatch(int slot, const Timecode& tc);
    std::vector<Crossing> crossings(double now);

private:
    struct Watch {
        int slot = 0;
        Timecode at;
        int64_t frames = 0;
        bool primed = false;
        int64_t lastPosition = 0;
    };

    UidGen* uids_;
    std::deque<TimecodeSlot> slots_;
    std::vector<Watch> watches_;
    std::function<double()> clock_;
    double timeout_ = 0.25;
    int nextId_ = 1;
};

} // namespace lumen
