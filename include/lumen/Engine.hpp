// Engine.hpp — executors, the merge pipeline and DMX rendering
// (SPEC §11.3, §11.4, §11.9, §15.2).
#pragma once

#include "lumen/Binding.hpp"
#include "lumen/Cue.hpp"
#include "lumen/Group.hpp"
#include "lumen/Library.hpp"
#include "lumen/Output.hpp"
#include "lumen/Patch.hpp"
#include "lumen/Preset.hpp"
#include "lumen/Programmer.hpp"
#include "lumen/Recipe.hpp"
#include "lumen/TripleBuffer.hpp"

#include <array>
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace lumen {

// ---------------------------------------------------------------------------
// Release (§11.3) — per playback, overridable per attribute group, because
// "intensity to zero but hold position" is the common busking want.
// ---------------------------------------------------------------------------
enum class ReleaseMode : uint8_t { ToZero, ToBelow, HoldLast };

const char* releaseModeName(ReleaseMode m);

// ---------------------------------------------------------------------------
// Executor
// ---------------------------------------------------------------------------
class Executor {
public:
    Uid uid{};
    int id = 1;
    int sequenceId = -1;
    std::string label;

    // §11.4: all three mechanisms, configurable per executor.
    int priority = 0;                                 // explicit, breaks ties
    MergeRule merge = MergeRule::LTP;                 // per-playback rule
    std::array<std::optional<MergeRule>, kGroupCount> mergeByGroup{};
    int swapGroup = 0;                                // 0 = none; same group releases

    float master = 1.0f;                              // fader
    ReleaseMode release = ReleaseMode::ToZero;
    std::array<std::optional<ReleaseMode>, kGroupCount> releaseByGroup{};
    double releaseTime = 2.0;
    bool assert_ = false;                             // force a win this duration

    // Transport ------------------------------------------------------------
    // The patch is needed to snapshot the live state before switching, so the
    // crossfade starts from what is actually on stage rather than from the
    // previous cue's stored values.
    void go(const Sequence& seq, const Patch& patch, double now);
    void back(const Sequence& seq, const Patch& patch, double now);
    void gotoCue(const Sequence& seq, const Patch& patch, int index, double now);
    void off(double now);                              // immediate
    void releaseNow(double now);                       // timed

    bool active() const { return active_; }
    int currentIndex() const { return cur_; }
    double fadeProgress(double now) const;
    uint64_t startOrder() const { return startOrder_; }
    bool releasing() const { return releasing_; }

    MergeRule mergeFor(Attr a) const;
    ReleaseMode releaseFor(Attr a) const;

    // Renders this executor's contribution. Uses tracking resolution, so a
    // cue that stores only changes still puts the full tracked state on stage.
    OutputLayer render(const Sequence& seq, const Patch& patch, double now) const;

private:
    void beginFade(const OutputLayer& snapshot, double dur, double now);
    OutputLayer snapshot(const Sequence& seq, const Patch& patch, double now) const;

    bool active_ = false;
    bool releasing_ = false;
    int cur_ = -1;
    OutputLayer from_;
    double cueStart_ = 0.0;
    double fadeStart_ = 0.0;
    double fadeDur_ = 0.0;
    // Separate from the cue crossfade above: sharing the fields made the
    // release scale the already-crossfading contribution, squaring the fade.
    double releaseStart_ = 0.0;
    double releaseDur_ = 0.0;
    // Swap groups resolve on TRIGGER ORDER, not on clock time: two playbacks
    // started in the same tick carry the same timestamp, and the tie broke the
    // wrong way. A monotonic counter makes "most recently triggered wins"
    // exact regardless of the clock.
    uint64_t startOrder_ = 0;
    FadeCurve curve_ = FadeCurve::Linear;
};

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------
class Engine {
public:
    Engine();
    ~Engine();

    UidGen uids;
    Patch patch{uids};
    Programmer programmer{patch};

    // Groups, presets and FX recipes belong to the show, so the engine owns
    // them: anything the command layer can reach must hang off one root, or
    // saving and loading quietly lose half the show.
    GroupStore groups{uids};
    PresetStore presets{uids};
    RecipeLibrary recipes{uids};
    // Fixture definitions, each keeping the .gdtf package it came from
    // (Library.hpp). The patch holds compiled types; this holds the sources.
    FixtureLibrary library{uids};

    // Remote control (§12). The queue is filled by the MIDI, OSC and DMX-in
    // threads and drained at the TOP OF tick() — §12.7 is explicit that input
    // handling must not run on the output thread, so a flood of MIDI cannot
    // stall DMX.
    BindingStore bindings{uids};
    InputQueue inputs;

    // What a drained input turned into, for the layer above to act on. The
    // engine resolves and maps; it does not dispatch, because a Command
    // target needs the command layer and the engine must not depend on it.
    struct BindingAction {
        Uid binding{};
        BindTarget target;
        Applied applied;
    };
    std::vector<BindingAction> pendingActions;

    // The target's value right now, which catch and scale takeover both need
    // (§12.4). The engine answers for what it owns and returns 0 otherwise;
    // the layer above can supply the rest.
    float bindingValue(const BindTarget& t) const;
    std::function<float(const BindTarget&)> externalValue;

    // Drains the input queue, resolves each event through the binding store
    // and fills pendingActions. Called at the top of tick(); exposed so a
    // test can drive it without running a whole frame.
    int drainInputs();

    // deque, not vector: ensureSequence()/ensureExecutor() return references,
    // and a vector push_back would invalidate every outstanding one. This bit
    // during testing, where holding two executor references crossed a reallocation.
    std::deque<Sequence> sequences;
    std::deque<Executor> executors;

    float grandMaster = 1.0f;
    bool blackout = false;
    UnusedMode unusedMode = UnusedMode::HoldLast;  // §11.9
    std::array<std::optional<UnusedMode>, kGroupCount> unusedByGroup{};

    Sequence* sequence(int id);
    const Sequence* sequence(int id) const;
    Sequence& ensureSequence(int id);
    Executor* executor(int id);
    Executor& ensureExecutor(int id);

    double now() const;
    void setClock(double t);     // tests drive time explicitly
    void clearClock();

    // One pass: merge every layer, resolve release behaviour, render DMX,
    // publish to the triple buffer.
    void tick();

    const OutputLayer& merged() const { return merged_; }
    const std::vector<Universe>& universes() const { return published_; }
    const Universe* universe(int number) const;
    void syncUniverses();

    // Output ---------------------------------------------------------------
    void addOutput(std::unique_ptr<IOutput> out);
    void startOutput(double hz = 44.0);
    void stopOutput();
    bool outputRunning() const { return running_.load(); }
    uint64_t frameCount() const { return frames_.load(); }

    UnusedMode unusedFor(Attr a) const;

private:
    void applySwapGroups(double t);
    OutputLayer mergePlaybacks(double t);
    void applyRetention(const OutputLayer& playbacks, const ValueLayer& prog);
    void renderDmx();

    OutputLayer merged_;
    OutputLayer retained_;                  // §11.9 LTP hold-last
    std::vector<AttrKey> lastProgrammerKeys_;
    std::vector<Universe> scratch_;
    std::vector<Universe> published_;

    TripleBuffer<std::vector<Universe>> frame_;
    std::vector<std::unique_ptr<IOutput>> outputs_;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> frames_{0};

    double startTime_ = 0.0;
    bool clockOverride_ = false;
    double clockValue_ = 0.0;
};

// Converts an authoring layer to floats for merging. A preset reference
// resolves to its cached value, which is what the merge stage needs — the
// authoring distinction only matters for storing and for Clear.
OutputLayer toOutput(const ValueLayer& in);

} // namespace lumen
