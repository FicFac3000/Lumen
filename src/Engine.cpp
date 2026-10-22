#include "lumen/Engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>

namespace lumen {

namespace {

double monotonicSeconds() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point base = clock::now();
    return std::chrono::duration<double>(clock::now() - base).count();
}

// Monotonic trigger counter, so swap groups can resolve "most recently
// triggered" without relying on the clock.
std::atomic<uint64_t> gTriggerOrder{0};

uint8_t to8(float v) { return static_cast<uint8_t>(std::lround(clamp01(v) * 255.f)); }
uint16_t to16(float v) {
    return static_cast<uint16_t>(std::lround(static_cast<double>(clamp01(v)) * 65535.0));
}

} // namespace

const char* releaseModeName(ReleaseMode m) {
    switch (m) {
        case ReleaseMode::ToZero: return "ToZero";
        case ReleaseMode::ToBelow: return "ToBelow";
        case ReleaseMode::HoldLast: return "HoldLast";
    }
    return "?";
}

OutputLayer toOutput(const ValueLayer& in) {
    OutputLayer out;
    for (const auto& [key, val] : in.values()) out.set(key, val.v);
    return out;
}

// ---------------------------------------------------------------------------
// Executor
// ---------------------------------------------------------------------------
MergeRule Executor::mergeFor(Attr a) const {
    const auto& o = mergeByGroup[static_cast<size_t>(groupOf(a))];
    if (o) return *o;
    // Intensity is HTP by its kind unless the playback explicitly says
    // otherwise, so a default-configured executor behaves conventionally.
    if (isIntensity(a)) return MergeRule::HTP;
    return merge;
}

ReleaseMode Executor::releaseFor(Attr a) const {
    const auto& o = releaseByGroup[static_cast<size_t>(groupOf(a))];
    return o ? *o : release;
}

double Executor::fadeProgress(double now) const {
    if (fadeDur_ <= 0.0) return 1.0;
    const double p = (now - fadeStart_) / fadeDur_;
    return p < 0.0 ? 0.0 : (p > 1.0 ? 1.0 : p);
}

void Executor::beginFade(const OutputLayer& snap, double dur, double now) {
    from_ = snap;
    fadeStart_ = now;
    fadeDur_ = std::max(0.0, dur);
}

OutputLayer Executor::snapshot(const Sequence& seq, const Patch& patch, double now) const {
    OutputLayer out;
    if (!active_) return out;
    const Cue* c = seq.cueAt(cur_);
    if (!c) return out;

    // Tracking resolution, not the cue's own layer: a tracking cue stores only
    // changes, so its layer is not the stage state (§11.1).
    const Resolved resolved = resolveTracking(seq, cur_);
    OutputLayer target = toOutput(resolved.values);

    // Cue phasers run on the playback clock by default, so a chase starts on
    // the cue rather than wherever the global clock happens to be (§4.8).
    const double t = now - cueStart_;
    for (const Phaser& ph : c->phasers) {
        SpreadTable spread;
        spread.buildFromTargets(ph.targets, patch, ph.source, ph.variation.seed);
        ValueLayer base = resolved.values;
        ValueLayer applied;
        ph.apply(spread, ph.clock == ClockSource::Global ? now : t, base, applied, &patch);
        for (const auto& [key, val] : applied.values()) target.set(key, val.v);
    }

    return crossfade(from_, target, applyCurve(static_cast<float>(fadeProgress(now)), curve_));
}

void Executor::go(const Sequence& seq, const Patch& patch, double now) {
    if (seq.cues.empty()) return;
    const int next = active_ ? (cur_ + 1) % static_cast<int>(seq.cues.size()) : 0;
    gotoCue(seq, patch, next, now);
}

void Executor::back(const Sequence& seq, const Patch& patch, double now) {
    if (seq.cues.empty()) return;
    const int n = static_cast<int>(seq.cues.size());
    const int prev = active_ ? (cur_ - 1 + n) % n : n - 1;
    gotoCue(seq, patch, prev, now);
}

void Executor::gotoCue(const Sequence& seq, const Patch& patch, int index, double now) {
    const Cue* c = seq.cueAt(index);
    if (!c) return;

    // Snapshot where we actually are before switching, so the crossfade starts
    // from the live state — mid-fade included — rather than from the previous
    // cue's stored values.
    const OutputLayer snap = snapshot(seq, patch, now);

    active_ = true;
    releasing_ = false;
    cur_ = index;
    cueStart_ = now + c->timing.delayIn;
    curve_ = c->timing.curve;
    startOrder_ = gTriggerOrder.fetch_add(1) + 1;
    beginFade(snap, c->timing.fadeIn + c->timing.delayIn, now);
}

void Executor::off(double now) {
    active_ = false;
    releasing_ = false;
    cur_ = -1;
    from_.clear();
    fadeStart_ = now;
    fadeDur_ = 0.0;
}

void Executor::releaseNow(double now) {
    if (!active_) return;
    releasing_ = true;
    releaseStart_ = now;
    releaseDur_ = releaseTime;
}

OutputLayer Executor::render(const Sequence& seq, const Patch& patch, double now) const {
    OutputLayer out;
    if (!active_ && !releasing_) return out;
    if (master <= 0.f && !assert_) return out;

    OutputLayer live = snapshot(seq, patch, now);

    // A timed release fades the whole contribution out, per attribute group
    // where an override says so (§11.3).
    float releaseScale = 1.f;
    if (releasing_) {
        const double p = releaseDur_ > 0.0 ? (now - releaseStart_) / releaseDur_ : 1.0;
        releaseScale = 1.f - static_cast<float>(std::clamp(p, 0.0, 1.0));
    }

    for (const auto& [key, v] : live.values()) {
        const Attr a = keyAttr(key);
        float value = v;

        // The fader scales intensity; other attributes pass through, which is
        // how a console behaves when a fader is parked low.
        if (isIntensity(a)) value *= master;

        if (releasing_) {
            switch (releaseFor(a)) {
                case ReleaseMode::ToZero:
                    value *= releaseScale;
                    break;
                case ReleaseMode::HoldLast:
                    // Values stay where they are; nothing to scale.
                    break;
                case ReleaseMode::ToBelow:
                    // Fall through to lower-priority playbacks: stop
                    // contributing rather than fading to zero.
                    if (releaseScale <= 0.f) continue;
                    value *= releaseScale;
                    break;
            }
        }
        out.set(key, value);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------
Engine::Engine() {
    startTime_ = monotonicSeconds();
    // Everything that measures time measures the SAME time. A subsystem with
    // its own clock would drift against the one driving cues, and under a
    // driven clock in a test it would not move at all.
    dmxIn.setClock([this] { return now(); });
    timecode.setClock([this] { return now(); });
    history.setClock([this] { return now(); });
    actionLog.setClock([this] { return now(); });
}
Engine::~Engine() { stopOutput(); }

double Engine::now() const {
    if (clockOverride_) return clockValue_;
    return monotonicSeconds() - startTime_;
}

void Engine::setClock(double t) {
    clockOverride_ = true;
    clockValue_ = t;
}
void Engine::clearClock() { clockOverride_ = false; }

Sequence* Engine::sequence(int id) {
    for (auto& s : sequences)
        if (s.id == id) return &s;
    return nullptr;
}

const Sequence* Engine::sequence(int id) const {
    for (const auto& s : sequences)
        if (s.id == id) return &s;
    return nullptr;
}

Sequence& Engine::ensureSequence(int id) {
    if (Sequence* s = sequence(id)) return *s;
    Sequence s;
    s.uid = uids.next();
    s.id = id;
    s.name = "Sequence " + std::to_string(id);
    sequences.push_back(std::move(s));
    return sequences.back();
}

Executor* Engine::executor(int id) {
    for (auto& e : executors)
        if (e.id == id) return &e;
    return nullptr;
}

Executor& Engine::ensureExecutor(int id) {
    if (Executor* e = executor(id)) return *e;
    Executor e;
    e.uid = uids.next();
    e.id = id;
    executors.push_back(std::move(e));
    return *executor(id);
}

UnusedMode Engine::unusedFor(Attr a) const {
    const auto& o = unusedByGroup[static_cast<size_t>(groupOf(a))];
    return o ? *o : unusedMode;
}

void Engine::syncUniverses() {
    const auto wanted = patch.universeList();
    std::vector<Universe> next;
    next.reserve(wanted.size());
    for (int n : wanted) {
        Universe u;
        u.number = n;
        if (const Universe* prev = universe(n)) u.data = prev->data;
        next.push_back(u);
    }
    scratch_ = next;
    published_ = std::move(next);
}

const Universe* Engine::universe(int number) const {
    for (const auto& u : published_)
        if (u.number == number) return &u;
    return nullptr;
}

void Engine::applySwapGroups(double t) {
    // §11.4: playbacks in the same swap group release each other, so only one
    // position look runs at a time. The most recently started one wins.
    std::set<int> groups;
    for (const auto& e : executors)
        if (e.swapGroup != 0 && e.active()) groups.insert(e.swapGroup);

    for (int g : groups) {
        // The most recently started playback wins the group.
        Executor* newest = nullptr;
        for (auto& e : executors) {
            if (e.swapGroup != g || !e.active()) continue;
            if (!newest || e.startOrder() > newest->startOrder()) newest = &e;
        }
        for (auto& e : executors) {
            if (e.swapGroup != g || &e == newest) continue;
            if (e.active() && !e.releasing()) e.releaseNow(t);
        }
    }
}

OutputLayer Engine::mergePlaybacks(double t) {
    // Resolution order: priority number first, then executor id as the final
    // tiebreak (§11.4).
    std::vector<const Executor*> order;
    order.reserve(executors.size());
    for (const auto& e : executors) order.push_back(&e);
    std::sort(order.begin(), order.end(), [](const Executor* a, const Executor* b) {
        if (a->priority != b->priority) return a->priority < b->priority;
        return a->id < b->id;
    });

    OutputLayer acc;
    OutputLayer asserted;

    for (const Executor* e : order) {
        const Sequence* seq = sequence(e->sequenceId);
        if (!seq) continue;
        const OutputLayer layer = e->render(*seq, patch, t);

        for (const auto& [key, v] : layer.values()) {
            const Attr a = keyAttr(key);
            const auto existing = acc.get(key);
            if (!existing) {
                acc.set(key, v);
            } else if (e->mergeFor(a) == MergeRule::HTP) {
                acc.set(key, std::max(*existing, v));
            } else {
                acc.set(key, v);
            }
        }
        // Assert forces a playback's values to win even against higher
        // priority, for the duration (§11.4).
        if (e->assert_)
            for (const auto& [key, v] : layer.values()) asserted.set(key, v);
    }

    acc.overwriteFrom(asserted);
    return acc;
}

void Engine::applyRetention(const OutputLayer& playbacks, const ValueLayer& prog) {
    // §11.9: "unused" means not used by the PROGRAMMER. HTP attributes always
    // return to default on release; LTP attributes hold their last value
    // unless the setting says otherwise. Playback levels are unaffected.
    std::vector<AttrKey> stillHeld;

    for (AttrKey key : lastProgrammerKeys_) {
        const Attr a = keyAttr(key);
        if (prog.find(key)) continue;           // programmer still uses it
        if (playbacks.has(key)) continue;       // a playback claims it
        if (isIntensity(a)) continue;           // HTP always drops to default
        if (unusedFor(a) == UnusedMode::ToDefault) continue;

        if (const auto held = retained_.get(key)) {
            merged_.set(key, *held);
            stillHeld.push_back(key);
        }
    }

    // Track which keys the programmer currently drives, so the next tick knows
    // what it just released.
    lastProgrammerKeys_.clear();
    for (const auto& [key, val] : prog.values()) {
        (void)val;
        lastProgrammerKeys_.push_back(key);
    }
    for (AttrKey k : stillHeld) lastProgrammerKeys_.push_back(k);
}

float Engine::bindingValue(const BindTarget& t) const {
    // The engine answers for what it actually owns; anything else belongs to
    // the layer above, which can supply it.
    switch (t.kind) {
        case TargetKind::Master:
            // subIndex 0 is the grand master; anything else is a playback's.
            if (t.subIndex == 0) return grandMaster;
            for (const Executor& e : executors)
                if (e.id == t.subIndex) return e.master;
            break;
        case TargetKind::Executor:
            for (const Executor& e : executors)
                if (e.id == t.subIndex) return e.master;
            break;
        default:
            break;
    }
    if (externalValue) return externalValue(t);
    return 0.f;
}

int Engine::pollDmxBindings() {
    // §12.3: a DMX-in channel crossing a threshold is the contact closure now
    // that GPI is deferred. Arriving DMX is a LEVEL, not an event, so the
    // events bindings consume have to be generated from it — and only for the
    // channels something is actually bound to, so an idle universe costs
    // nothing.
    int pushed = 0;
    for (BindingSet& set : bindings.setsMutable()) {
        if (!set.enabled) continue;
        if (set.page != 0 && set.page != bindings.page()) continue;
        for (Binding& b : set.bindings) {
            if (!b.enabled || b.source.kind != SourceKind::DmxIn) continue;
            const auto level = dmxIn.slot(b.source.channel, b.source.index);
            if (!level) continue;
            const float v = static_cast<float>(*level) / 255.f;
            // Only on change: re-firing an unchanged level every tick would
            // make a threshold binding chatter at the frame rate.
            if (b.hasLastRaw && std::fabs(v - b.lastRaw) < 1e-6f) continue;

            InputEvent e;
            e.kind = SourceKind::DmxIn;
            e.channel = b.source.channel;
            e.index = b.source.index;
            e.value = v;
            e.time = now();
            inputs.push(e);
            ++pushed;
        }
    }
    return pushed;
}

int Engine::drainInputs() {
    pollDmxBindings();
    const std::vector<InputEvent> events = inputs.drain();
    int acted = 0;
    for (const InputEvent& e : events) {
        for (Binding* b : bindings.resolve(e)) {
            const Applied a = applyInput(*b, e, bindingValue(b->target));
            if (!a.hasValue && !a.fire) continue;

            // What the engine owns, it applies itself; everything else is
            // handed up, because a Command target needs the command layer.
            bool handled = false;
            if (b->target.kind == TargetKind::Master && a.hasValue) {
                if (b->target.subIndex == 0) {
                    grandMaster = clamp01(a.value);
                    handled = true;
                } else {
                    for (Executor& x : executors)
                        if (x.id == b->target.subIndex) {
                            x.master = clamp01(a.value);
                            handled = true;
                        }
                }
            }
            if (!handled) pendingActions.push_back({b->uid, b->target, a});
            ++acted;
        }
    }
    return acted;
}

void Engine::tick() {
    const double t = now();

    // §12.7: inputs are drained HERE, at the top of the control-thread pass,
    // not on the output thread.
    drainInputs();
    // A DMX source that went quiet releases its slots, and a timecode feed
    // that stopped is marked stale. Both are time-driven, so they belong on
    // the same pass rather than waiting for the next packet that may never
    // come.
    dmxIn.expire();
    timecode.tick();

    applySwapGroups(t);

    // 1. Playbacks, lowest priority first.
    const OutputLayer playbacks = mergePlaybacks(t);
    merged_ = playbacks;

    // 2. Programmer on top, unconditionally.
    const ValueLayer& prog = programmer.values();
    merged_.overwriteFrom(toOutput(prog));

    // 3. Programmer phasers, centred on whatever is underneath. These run on
    //    the global clock, so a busked effect does not restart when a cue
    //    changes (§4.8).
    for (const Phaser& ph : programmer.phasers()) {
        SpreadTable spread;
        spread.buildFromTargets(ph.targets, patch, ph.source, ph.variation.seed);
        ValueLayer base;
        for (const auto& [key, v] : merged_.values())
            base.setKey(key, Value::hard(v));
        ValueLayer applied;
        ph.apply(spread, t, base, applied, &patch);
        merged_.overwriteFrom(toOutput(applied));
    }

    // 4. LTP hold-last for values the programmer has just released.
    applyRetention(playbacks, prog);

    // Retention must be captured BEFORE highlight, or a highlight value would
    // stick after the highlight is switched off.
    retained_ = merged_;

    // 5. Highlight above everything except the output stage (§20.7). Built
    //    fresh each tick and never stored, so Store cannot capture it.
    const ValueLayer hi = programmer.highlightLayer();
    if (!hi.empty()) merged_.overwriteFrom(toOutput(hi));

    renderDmx();
    frames_.fetch_add(1, std::memory_order_relaxed);
}

void Engine::renderDmx() {
    if (scratch_.size() != patch.universeList().size()) syncUniverses();
    for (auto& u : scratch_) u.data.fill(0);

    for (const auto& fx : patch.fixtures()) {
        Universe* u = nullptr;
        for (auto& cand : scratch_)
            if (cand.number == fx.universe) {
                u = &cand;
                break;
            }
        if (!u) continue;

        const FixtureType& ft = patch.type(fx.typeIndex);
        for (const auto& ch : ft.channels) {
            const AttrKey key = makeKey(fx.id, ch.cell, ch.attr);
            float v;
            if (const auto got = merged_.get(key)) v = *got;
            else v = static_cast<float>(ch.defaultValue) / 255.f;

            if (isIntensity(ch.attr)) {
                v *= grandMaster;
                if (blackout) v = 0.f;
            }
            if (ch.invert) v = 1.f - v;
            v = clamp01(v);

            const int base = fx.address - 1;
            if (ch.fine >= 0) {
                const uint16_t w = to16(v);
                const int hi = base + ch.coarse;
                const int lo = base + ch.fine;
                if (hi >= 0 && hi < 512) u->data[static_cast<size_t>(hi)] =
                    static_cast<uint8_t>(w >> 8);
                if (lo >= 0 && lo < 512) u->data[static_cast<size_t>(lo)] =
                    static_cast<uint8_t>(w & 0xff);
            } else {
                const int idx = base + ch.coarse;
                if (idx >= 0 && idx < 512) u->data[static_cast<size_t>(idx)] = to8(v);
            }
        }
    }

    published_ = scratch_;
    frame_.writeSlot() = scratch_;
    frame_.publish();
}

void Engine::addOutput(std::unique_ptr<IOutput> out) { outputs_.push_back(std::move(out)); }

void Engine::startOutput(double hz) {
    if (running_.exchange(true)) return;
    const auto period = std::chrono::duration<double>(1.0 / std::max(1.0, hz));

    // The output thread reads published frames from the triple buffer and
    // never blocks the control thread (§15.2).
    thread_ = std::thread([this, period]() {
        auto nextWake = std::chrono::steady_clock::now();
        uint64_t seen = 0;
        while (running_.load(std::memory_order_acquire)) {
            const std::vector<Universe>& frame = frame_.acquire();
            seen = frame_.version();
            for (auto& out : outputs_)
                for (const auto& u : frame) out->send(u.number, u.data.data(), u.data.size());

            nextWake += std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);
            std::this_thread::sleep_until(nextWake);
        }
        (void)seen;
    });
}

void Engine::stopOutput() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
}

} // namespace lumen
