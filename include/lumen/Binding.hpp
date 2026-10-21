// Binding.hpp — Learn and remote control bindings (SPEC §12).
//
// MadMapper's flow: pick a target, press Learn, move the control, done. No
// dialog, no addresses typed. Applied across the whole app rather than one
// window.
//
// Two things here carry the real risk:
//
//   FADER TAKEOVER (§12.4). Software holds 50%, the physical fader sits at 0%.
//   Touching it must not snap the look. Three modes, per binding, with catch
//   as the default because it is the only one safe to touch during a show.
//
//   LAYERING (§12.6). Global bindings plus page-local ones, page-local
//   winning, so one Launchpad means different things per executor page without
//   remapping.
//
// A binding's TARGET can always be a command string (§12.2), which is the
// general escape hatch: any input can fire anything typeable, so a binding
// never needs a new target kind to reach a new feature.
#pragma once

#include "lumen/Attribute.hpp"
#include "lumen/Uid.hpp"

#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace lumen {

// ---------------------------------------------------------------------------
// Sources (§12.3)
// ---------------------------------------------------------------------------
enum class SourceKind : uint8_t {
    None,
    MidiNote,
    MidiCC,
    MidiCC14,      // two CCs, coarse and fine
    MidiProgram,
    MidiPitchBend,
    Osc,
    DmxIn,         // universe + channel, over Art-Net, sACN or an interface
    Key,
};

const char* sourceKindName(SourceKind k);

// Keyboard modifiers, as a bitmask (§2.8).
enum class Mods : uint8_t { None = 0, Shift = 1, Ctrl = 2, Alt = 4, Meta = 8 };
inline Mods operator|(Mods a, Mods b) {
    return static_cast<Mods>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}
inline bool has(Mods set, Mods flag) {
    return (static_cast<uint8_t>(set) & static_cast<uint8_t>(flag)) != 0;
}

struct InputSource {
    SourceKind kind = SourceKind::None;
    // MIDI channel 1..16, or the DMX universe. 0 means "any", which is what
    // Learn stores for a controller that does not pin its channel.
    int channel = 0;
    // Note or CC number, DMX channel, program number, or key code.
    int index = 0;
    std::string address;   // OSC address pattern
    int argIndex = 0;      // OSC argument position
    Mods mods = Mods::None;

    // What the Learn badge shows: "CC 7", "/lumen/exec/1", "U2.14" (§12.2).
    std::string describe() const;
    bool valid() const { return kind != SourceKind::None; }
    // Two sources collide when the same input would reach both.
    bool sameAs(const InputSource& other) const;
};

// What arrives from a transport. Normalised to 0..1 at the edge, so the
// mapping below never needs to know a CC is 7-bit and a 14-bit CC is not.
struct InputEvent {
    SourceKind kind = SourceKind::None;
    int channel = 0;
    int index = 0;
    std::string address;
    int argIndex = 0;
    Mods mods = Mods::None;

    float value = 0.f;     // 0..1
    bool pressed = false;  // note-on / button-down
    bool isNoteOff = false;
    double time = 0.0;

    // Endless encoders send a delta, not a position.
    bool isDelta = false;
    float delta = 0.f;
};

// ---------------------------------------------------------------------------
// Targets (§12.2) — anything
// ---------------------------------------------------------------------------
enum class TargetKind : uint8_t {
    Command,      // the escape hatch: any typeable command
    Fader,
    Encoder,
    Button,
    Master,       // grand master, a playback master
    Option,       // any setting in the app
    Executor,
    Preset,
    Group,
    Recipe,
    PanelSet,
    RadialSector,
};

const char* targetKindName(TargetKind k);

struct BindTarget {
    TargetKind kind = TargetKind::Command;
    Uid object{};          // the executor, preset, recipe it points at
    std::string command;   // for TargetKind::Command, and as a fallback action
    std::string label;     // what Learn shows on it
    int subIndex = 0;      // radial sector, encoder number, fader number

    bool sameAs(const BindTarget& other) const;
    std::string describe() const;
};

// ---------------------------------------------------------------------------
// Mapping (§12.4)
// ---------------------------------------------------------------------------
enum class BindCurve : uint8_t { Linear, Log, Exp, SCurve };
enum class BindMode : uint8_t {
    Absolute,    // the value follows the control
    Relative,    // endless encoders: add the delta
    Toggle,      // each press flips
    Momentary,   // on while held
    Trigger,     // fire once on press
};

// §12.4's flagged problem. Catch is the default: it is the only one that
// cannot change the look the instant an operator brushes a fader.
enum class Takeover : uint8_t {
    Jump,    // follow immediately — predictable, visibly ugly
    Scale,   // remaining travel scales to the remaining range
    Catch,   // dead until it passes through the current value
};

// §12.3: what makes a DMX channel serve as a contact closure without a
// separate mechanism, now that GPI is deferred.
enum class ThresholdEdge : uint8_t { None, Rising, Falling, WhileAbove };

const char* bindModeName(BindMode m);
const char* takeoverName(Takeover t);
const char* curveName(BindCurve c);

struct Mapping {
    float inMin = 0.f, inMax = 1.f;
    bool invert = false;
    float outMin = 0.f, outMax = 1.f;
    BindCurve curve = BindCurve::Linear;
    BindMode mode = BindMode::Absolute;
    Takeover takeover = Takeover::Catch;

    float deadband = 0.f;    // ignore changes smaller than this
    float smoothing = 0.f;   // 0 = off, 1 = never arrives
    bool feedback = false;   // send the value back (§12.5)

    ThresholdEdge edge = ThresholdEdge::None;
    float threshold = 0.5f;
    float hysteresis = 0.02f;  // so a noisy channel does not chatter

    float relativeScale = 1.f;  // turns per full range on an endless encoder

    // Input normalised through range, invert and curve. Pure, so it is
    // testable without a binding.
    float shape(float raw) const;
};

// ---------------------------------------------------------------------------
// Binding
// ---------------------------------------------------------------------------
struct Binding {
    Uid uid{};
    int id = 0;
    std::string label;
    InputSource source;
    BindTarget target;
    Mapping map;
    bool enabled = true;

    // --- runtime state, not authored ------------------------------------
    // Takeover: whether this control has picked the value up yet. Reset when
    // a binding set activates, because a fader that was caught on the last
    // page has not been caught on this one.
    bool caught = false;
    float lastRaw = 0.f;
    bool hasLastRaw = false;
    float smoothed = 0.f;
    bool hasSmoothed = false;
    bool toggleState = false;
    bool aboveThreshold = false;
    // What the target's value was when this control was first touched, which
    // is what Scale needs as its anchor.
    float anchorValue = 0.f;
    bool hasAnchor = false;

    void resetRuntime();
};

// What applying an input produced.
struct Applied {
    bool hasValue = false;   // a continuous value to write
    float value = 0.f;
    bool fire = false;       // a discrete action: trigger, toggle, edge
    bool fireOn = false;     // for toggle/momentary: the new state
    bool swallowed = false;  // deadband, or catch not reached yet

    // §12.4: a control in catch mode must say which way to move, or the
    // operator is left guessing why the fader is dead.
    int catchDirection = 0;  // -1 move down, +1 move up, 0 nothing needed
    bool justCaught = false;
};

// `currentValue` is the target's value right now, which Catch and Scale both
// need and the binding cannot know.
Applied applyInput(Binding& b, const InputEvent& e, float currentValue);

// ---------------------------------------------------------------------------
// Binding sets and layering (§12.6)
// ---------------------------------------------------------------------------
struct BindingSet {
    Uid uid{};
    int id = 0;
    std::string name;
    bool enabled = true;
    // 0 = global. Anything else is page-local and wins over global on that
    // page, so one controller serves every executor page (§9.1).
    int page = 0;
    std::deque<Binding> bindings;

    Binding* find(Uid uid);
    Binding* findBySource(const InputSource& s);
};

struct Conflict {
    bool any = false;
    // Existing bindings the same input already reaches, so the warning can
    // name them rather than just saying "conflict".
    std::vector<std::string> existing;
};

class BindingStore {
public:
    explicit BindingStore(UidGen& uids) : uids_(&uids) {}

    BindingSet& addSet(BindingSet s);
    BindingSet& restoreSet(BindingSet s);
    BindingSet* set(Uid uid);
    BindingSet* setByName(const std::string& name);
    const std::deque<BindingSet>& sets() const { return sets_; }
    bool removeSet(Uid uid);

    // Adds to a set, assigning a UID and an id.
    Binding* add(Uid setUid, Binding b);
    bool remove(Uid setUid, Uid bindingUid);
    Binding* find(Uid bindingUid);

    // The active page. Page-local bindings only resolve on their own page.
    void setPage(int page);
    int page() const { return page_; }

    // Resolution order is the whole point of layering: page-local first, then
    // global, and the first match wins unless the caller wants them all.
    std::vector<Binding*> resolve(const InputEvent& e);
    // Every match, for the case where one input deliberately drives several
    // things (§12.6 allows it on confirm).
    std::vector<Binding*> resolveAll(const InputEvent& e);

    // §12.6: binding an input that is already bound warns and names what it
    // would join, rather than silently stacking.
    Conflict wouldConflict(const InputSource& s, int page) const;

    // Activating a set resets takeover state, because a fader caught on the
    // outgoing set has not been caught on the incoming one (§12.5 feedback
    // sends on activation for the same reason).
    void activate(Uid setUid, bool on);
    void resetTakeover();

    size_t bindingCount() const;

private:
    UidGen* uids_;
    std::deque<BindingSet> sets_;
    int page_ = 0;
};

// ---------------------------------------------------------------------------
// Learn (§12.1)
// ---------------------------------------------------------------------------
struct LearnResult {
    enum class Outcome : uint8_t {
        Ignored,     // not in learn, or nothing focused
        Bound,
        Conflict,    // needs confirming
        Unlearned,
        Advanced,    // bound and moved on to the next target
    };
    Outcome outcome = Outcome::Ignored;
    Uid binding{};
    Conflict conflict;
    std::string message;
    int focusIndex = -1;
};

class Learn {
public:
    Learn(BindingStore& store) : store_(&store) {}

    // The set new bindings land in. Without one, Learn refuses rather than
    // inventing a set the user did not ask for.
    void setTargetSet(Uid setUid) { targetSet_ = setUid; }
    Uid targetSet() const { return targetSet_; }

    void begin();
    void end();
    bool active() const { return active_; }

    // The bindable targets on screen, in the order Learn advances through
    // them — which is what makes mapping eight faders eight fader moves
    // rather than eight separate operations (§12.1).
    void setTargets(std::vector<BindTarget> targets);
    const std::vector<BindTarget>& targets() const { return targets_; }

    bool focus(int index);
    bool focusTarget(const BindTarget& t);
    int focusIndex() const { return focus_; }
    const BindTarget* focused() const;

    void setAutoAdvance(bool on) { autoAdvance_ = on; }
    bool autoAdvance() const { return autoAdvance_; }

    // Feeding an input while learning binds it to the focused target.
    LearnResult receive(const InputEvent& e);
    // Binding over an existing one, after the warning was accepted.
    LearnResult confirm(const InputEvent& e);

    LearnResult unlearnFocused();
    // Clears every binding in the target set (§12.1's `Learn Clear`).
    int clearSet();

    // What the badge on each target shows: its source, or empty.
    std::string badgeFor(const BindTarget& t) const;

private:
    LearnResult bind(const InputEvent& e, bool force);

    BindingStore* store_;
    Uid targetSet_{};
    bool active_ = false;
    bool autoAdvance_ = true;
    int focus_ = -1;
    std::vector<BindTarget> targets_;
};

// ---------------------------------------------------------------------------
// Input queue (§12.7)
// ---------------------------------------------------------------------------
// FLAGGED IN THE SPEC: input handling must not run on the 44 Hz output thread.
// Inputs land here from the MIDI, OSC and DMX-in threads; the engine drains
// the queue at the top of tick(). A mutex is right rather than lock-free: the
// producers are several, the rate is low, and the requirement is that the
// OUTPUT thread never waits on input — which it does not, because draining
// happens on the control thread under the lock it already holds.
//
// Bounded, and it counts what it dropped. An unbounded queue under a stuck
// controller sending at full rate is a slow memory leak that shows up as a
// crash hours into a show.
class InputQueue {
public:
    explicit InputQueue(size_t capacity = 4096) : capacity_(capacity) {}

    bool push(const InputEvent& e);
    std::vector<InputEvent> drain();

    size_t size() const;
    size_t dropped() const;
    void clearDropped();

private:
    mutable std::mutex m_;
    std::vector<InputEvent> q_;
    size_t capacity_;
    size_t dropped_ = 0;
};

} // namespace lumen
