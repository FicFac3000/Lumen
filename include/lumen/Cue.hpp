// Cue.hpp — cues, tracking and timing (SPEC §11.1, §11.2).
//
// Tracking is NOT a sequence-wide mode. It is a property of each
// (cue, attribute group) pair, so one cue can track Position while blocking
// Colour. Three mechanisms are kept distinct because they are usually
// conflated:
//
//   Track    — cue stores only changes; unstored values inherit forward
//   Block    — cue stores a hard value even where unchanged, so an upstream
//              edit stops here
//   CueOnly  — changes apply to this cue alone; the next cue gets the
//              pre-change value back
#pragma once

#include "lumen/Phaser.hpp"
#include "lumen/Uid.hpp"
#include "lumen/Value.hpp"

#include <array>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace lumen {

// ---------------------------------------------------------------------------
// Timing (§11.2)
// ---------------------------------------------------------------------------
enum class FadeCurve : uint8_t { Linear, SCurve, EaseIn, EaseOut };

const char* fadeCurveName(FadeCurve c);
// Shapes a 0..1 crossfade position.
float applyCurve(float t, FadeCurve c);

struct Timing {
    double fadeIn = 3.0;
    double fadeOut = 3.0;
    double delayIn = 0.0;
    double delayOut = 0.0;
    FadeCurve curve = FadeCurve::Linear;
};

// How a cue treats one attribute group.
enum class TrackMode : uint8_t { Track, Block, CueOnly };

const char* trackModeName(TrackMode m);

// ---------------------------------------------------------------------------
// Cue
// ---------------------------------------------------------------------------
// A part is a named subset with its own timing. Part 0 is the main part
// (§11.2); further parts let one cue move different fixtures on different
// times without splitting it into several cues.
struct CuePart {
    std::string name;
    ValueLayer values;
    std::optional<Timing> timing;  // inherits the cue's when unset
};

struct Cue {
    Uid uid{};
    float number = 1.f;
    std::string name;

    Timing timing;
    std::unordered_map<uint8_t, Timing> groupTiming;  // key: Group
    std::unordered_map<uint16_t, Timing> attrTiming;  // key: Attr

    // Part 0 is the main part and always exists.
    std::vector<CuePart> parts{CuePart{}};
    std::vector<Phaser> phasers;

    // Per-group tracking state. The cue sheet shows this as a five-cell strip
    // rather than a single flag, so the state is readable at a glance (§11.1).
    std::array<TrackMode, kGroupCount> track{};

    // Tracking shield: no upstream edit may track into this cue, whatever the
    // group masks say. For the one cue that must look right (§11.1).
    bool shield = false;

    enum class Trigger : uint8_t { Go, Follow, Wait, Timecode, Link, Loop };
    Trigger trigger = Trigger::Go;
    double triggerTime = 0.0;

    ValueLayer& values() { return parts[0].values; }
    const ValueLayer& values() const { return parts[0].values; }

    TrackMode trackMode(Group g) const { return track[static_cast<size_t>(g)]; }
    void setTrackMode(Group g, TrackMode m) { track[static_cast<size_t>(g)] = m; }

    // Timing precedence: attribute override, then group override, then cue.
    Timing timingFor(Attr a) const;
    Timing timingForPart(size_t part) const;

    // Every stored value across all parts, flattened. Later parts win on a
    // key collision, which is the same rule as part ordering on stage.
    ValueLayer allValues() const;
};

// ---------------------------------------------------------------------------
// Sequence
// ---------------------------------------------------------------------------
struct Sequence {
    Uid uid{};
    int id = 1;
    std::string name;
    std::vector<Cue> cues;

    // Sequence-level default, overridden per cue (§11.1 precedence).
    std::array<TrackMode, kGroupCount> trackDefault{};

    // Any sequence can run as a chase — a mode switch, not a separate object
    // type (§11.5).
    bool chase = false;
    float chaseRateHz = 2.f;
    Direction chaseDirection = Direction::Forward;
    float chaseCrossfade = 0.f;

    int indexOfNumber(float n) const;
    const Cue* cueAt(int index) const;
    Cue* cueAt(int index);
    size_t size() const { return cues.size(); }
};

// ---------------------------------------------------------------------------
// Tracking resolution (§11.1)
// ---------------------------------------------------------------------------
// Walks the sequence accumulating tracked state and returns the effective
// values AT `index`. This is what replaces reading a single cue's values: a
// tracking cue stores only changes, so its own layer is not the stage state.
struct Resolved {
    ValueLayer values;
    // Where each value came from, for the fixture sheet's origin column
    // (§11.10). Populated only when `withOrigin` is set, because provenance is
    // computed on demand rather than every frame.
    std::unordered_map<AttrKey, int> originCue;
};

Resolved resolveTracking(const Sequence& seq, int index, bool withOrigin = false);

// Materialises a group's values into a cue so nothing upstream can track
// through it — this is what Block *does*, as opposed to how it resolves.
void blockGroup(Sequence& seq, int index, Group g);
void unblockGroup(Sequence& seq, int index, Group g);

} // namespace lumen
