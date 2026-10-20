// Phaser.hpp — the FX primitive (SPEC §4).
//
// A phaser evaluates a phase 0..1 to a value. Three generators answer that
// same question (§4.5): an analytic waveform, an explicit step list, or a
// hand-drawn bezier curve. Everything downstream is identical, which is why
// waveforms are NOT baked into step lists — a stepped sine is coarser and no
// cheaper than evaluating the sine.
#pragma once

#include "lumen/Attribute.hpp"
#include "lumen/Selection.hpp"
#include "lumen/Value.hpp"

#include <optional>
#include <string>
#include <vector>

namespace lumen {

// ---------------------------------------------------------------------------
// Generators (§4.5)
// ---------------------------------------------------------------------------
enum class Wave : uint8_t {
    Sine, Cosine, Ramp, SawDown, Triangle, Square, PWM, Noise, Count
};

const char* waveName(Wave w);
std::optional<Wave> waveFromName(const std::string& s);

// One step of a step list. Attack and decay are separate, not a symmetric
// ease: fast attack with slow decay is the difference between a flat strobe
// and one with a tail, and symmetric easing cannot express it (§4.5).
struct Step {
    float value = 1.f;      // target, 0..1 within the phaser's own range
    float width = 1.f;      // dwell, as a share of this step's slot
    float attack = 0.f;     // rise time, share of the slot
    float decay = 0.f;      // fall time, share of the slot
    float attackCurve = 0.f;  // -1 ease-out .. 0 linear .. +1 ease-in
    float decayCurve = 0.f;
};

// A bezier knot. Handles are offsets in phase/value space, so a knot with
// zero handles behaves linearly.
struct CurvePoint {
    float x = 0.f;    // phase 0..1
    float y = 0.f;    // value 0..1
    float inDx = 0.f, inDy = 0.f;
    float outDx = 0.f, outDy = 0.f;
};

enum class GenKind : uint8_t { Waveform, Steps, Curve };

struct Generator {
    GenKind kind = GenKind::Waveform;

    // Waveform
    Wave wave = Wave::Sine;
    float duty = 0.5f;   // PWM / pulse width — the "Width" live control (§4.17)

    // Steps
    std::vector<Step> steps;

    // Curve
    std::vector<CurvePoint> curve;

    // Evaluates to -1..1 for waveforms, or 0..1 mapped to -1..1 for steps and
    // curves, so all three are interchangeable downstream.
    float eval(float phase) const;
};

// ---------------------------------------------------------------------------
// Direction and bounce (§4.13)
//
// Bounce pivots on SPREAD POSITION, not on the value range: the three modes
// govern where the travelling wave reverses relative to the ends of the
// spread. That is why it lives here, in the travel term, rather than in the
// waveform.
// ---------------------------------------------------------------------------
enum class Direction : uint8_t { Forward, Reverse, Bounce, Random };

enum class BouncePivot : uint8_t {
    AtEnd,         // the leading edge reaches the last member
    CentreAtEnd,   // the wave's midpoint reaches the last member
    OverEnd,       // the whole wave travels past the last member
};

const char* directionName(Direction d);
const char* bouncePivotName(BouncePivot p);

// ---------------------------------------------------------------------------
// Gap (§4.15) — seven parameters, with named behaviours on top because seven
// orthogonal settings is powerful and unusable.
// ---------------------------------------------------------------------------
enum class GapHold : uint8_t { Base, HoldLast, First, FadeToBase };
enum class GapPlacement : uint8_t { After, Before, Split };
enum class GapResume : uint8_t { Reset, Continue, Reshuffle };
enum class GapScope : uint8_t { PerFixture, Group };

struct Gap {
    bool enabled = false;
    float length = 0.f;             // in cycles; 0 disables
    bool lengthIsFraction = false;  // true: share of the cycle, scales with speed
    std::vector<float> sequence;    // cycled lengths, e.g. {1, 1, 2}
    int everyNth = 1;               // gap after every N active loops
    GapHold hold = GapHold::Base;
    GapPlacement placement = GapPlacement::After;
    GapResume resume = GapResume::Reset;
    GapScope scope = GapScope::PerFixture;
    float transition = 0.f;         // ease in/out of the gap, share of the gap

    float lengthForLoop(int loopIndex) const;
};

enum class GapBehaviour : uint8_t {
    Pause, Freeze, Breathe, Burst, Stutter, Drift, Reshuffle, Count
};

const char* gapBehaviourName(GapBehaviour b);
// Sets the seven parameters from a named behaviour; the parameters stay
// editable underneath (§4.15).
Gap gapFromBehaviour(GapBehaviour b, float length = 1.f, int everyNth = 3);

// ---------------------------------------------------------------------------
// Per-fixture variation (§4.11). Seeded, so it replays identically.
// Distinct from the `random` spread source: that randomises ORDERING, this
// randomises each fixture's rendering.
// ---------------------------------------------------------------------------
struct Variation {
    float phase = 0.f;  // +/- share of a cycle
    float speed = 0.f;  // +/- share of the rate
    float size = 0.f;   // +/- share of the amplitude
    uint64_t seed = 1;
};

// ---------------------------------------------------------------------------
// Stacking (§4.7)
// ---------------------------------------------------------------------------
enum class StackMode : uint8_t { Replace, Add, Multiply, Max, Min };

// Which clock drives the phaser (§4.8). The engine's two existing behaviours
// were accidental; both are wanted, so it becomes an explicit choice.
enum class ClockSource : uint8_t { Global, Playback, Triggered };

// ---------------------------------------------------------------------------
// Phaser
// ---------------------------------------------------------------------------
struct Phaser {
    // Target: a concrete attribute, or a role resolved per fixture (§4.2).
    Attr attr = Attr::Dimmer;
    Role role = Role::None;

    Generator gen;
    float size = 1.f;       // peak-to-peak amount
    float centre = 0.5f;    // used when relative == false
    bool relative = true;   // §4.6: offset the underlying value, or drive absolutely
    float speedHz = 0.5f;

    SpreadSource source = SpreadSource::Index;
    SpreadModifiers mods;

    Direction direction = Direction::Forward;
    BouncePivot pivot = BouncePivot::AtEnd;
    float overshoot = 0.f;  // extra travel for OverEnd, in spread units

    Gap gap;
    Variation variation;
    StackMode stack = StackMode::Replace;
    ClockSource clock = ClockSource::Global;

    float fade = 1.f;  // 0..1 envelope on size, for FX fade in/out (§4.9)

    std::vector<Target> targets;

    // ---------------------------------------------------------------------
    // Evaluation
    // ---------------------------------------------------------------------
    struct Sample {
        float wave = 0.f;    // generator output, -1..1
        float delta = 0.f;   // contribution as an offset — §11.10's Relative
        bool inGap = false;
        bool silent = false; // no contribution this instant (gap at base)
    };

    // `t` is seconds on whichever clock this phaser follows.
    Sample sampleAt(const Target& target, const SpreadTable& spread, double t) const;

    // Applies to a layer, reading `base` for centring when relative.
    void apply(const SpreadTable& spread, double t, const ValueLayer& base,
               ValueLayer& out, const class Patch* patch = nullptr) const;

    // Resolves this phaser's target attribute for one fixture. Returns nullopt
    // where the fixture cannot serve the role, which SKIPS THE SLOT for that
    // fixture rather than dropping the fixture from the recipe (§4.2).
    std::optional<Attr> resolveAttr(const Patch& patch, const Target& target) const;
};

// Travel term: how far the wave has moved across the spread, given elapsed
// cycles. Forward ramps, Reverse ramps backwards, Bounce triangle-waves with
// an amplitude set by the pivot mode (§4.13).
float travelAt(double cycles, Direction dir, BouncePivot pivot,
               float waveWidth, float overshoot, uint64_t seed);

// Deterministic per-target noise in -1..1, so variation and Noise replay.
float targetNoise(const Target& t, uint64_t seed, uint32_t salt = 0);

} // namespace lumen
