// test_binding.cpp — Learn, bindings, mapping and fader takeover (SPEC §12).
#include "Check.hpp"

#include "lumen/Binding.hpp"
#include "lumen/Command.hpp"
#include "lumen/Show.hpp"

#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace lumen;

namespace {

InputEvent cc(int number, float value, int channel = 1) {
    InputEvent e;
    e.kind = SourceKind::MidiCC;
    e.channel = channel;
    e.index = number;
    e.value = value;
    return e;
}

InputEvent note(int number, bool down, int channel = 1) {
    InputEvent e;
    e.kind = SourceKind::MidiNote;
    e.channel = channel;
    e.index = number;
    e.pressed = down;
    e.isNoteOff = !down;
    e.value = down ? 1.f : 0.f;
    return e;
}

InputEvent dmx(int universe, int channel, float value) {
    InputEvent e;
    e.kind = SourceKind::DmxIn;
    e.channel = universe;
    e.index = channel;
    e.value = value;
    return e;
}

InputEvent osc(const std::string& address, float value, int arg = 0) {
    InputEvent e;
    e.kind = SourceKind::Osc;
    e.address = address;
    e.argIndex = arg;
    e.value = value;
    return e;
}

BindTarget fader(int n, const std::string& label) {
    BindTarget t;
    t.kind = TargetKind::Fader;
    t.subIndex = n;
    t.label = label;
    return t;
}

BindTarget command(const std::string& c) {
    BindTarget t;
    t.kind = TargetKind::Command;
    t.command = c;
    return t;
}

// Runs a fader sweep through a binding, tracking the value the way the engine
// would, and reports what happened at each step.
struct SweepStep {
    float raw = 0.f;
    bool set = false;
    float value = 0.f;
    int direction = 0;
    bool justCaught = false;
};

std::vector<SweepStep> sweep(Binding& b, const std::vector<float>& raws, float startValue) {
    std::vector<SweepStep> out;
    float value = startValue;
    for (float raw : raws) {
        const Applied a = applyInput(b, cc(7, raw), value);
        if (a.hasValue) value = a.value;
        out.push_back({raw, a.hasValue, value, a.catchDirection, a.justCaught});
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
static void testSourceDescriptionsMatchTheSpecBadges() {
    // §12.1 says the badge shows "CC 7", "/lumen/exec/1", "U2.14". Those are
    // the strings, not an approximation of them.
    InputSource c;
    c.kind = SourceKind::MidiCC;
    c.index = 7;
    CHECK(c.describe() == "CC 7");
    c.channel = 1;
    CHECK(c.describe() == "CC 7");   // channel 1 adds nothing, so it is hidden
    c.channel = 3;
    CHECK(c.describe() == "CC 7 ch3");

    InputSource o;
    o.kind = SourceKind::Osc;
    o.address = "/lumen/exec/1";
    CHECK(o.describe() == "/lumen/exec/1");

    InputSource d;
    d.kind = SourceKind::DmxIn;
    d.channel = 2;
    d.index = 14;
    CHECK(d.describe() == "U2.14");

    InputSource k;
    k.kind = SourceKind::Key;
    k.index = 65;
    k.mods = Mods::Ctrl | Mods::Shift;
    CHECK(k.describe() == "Ctrl+Shift+Key 65");

    CHECK(InputSource{}.describe().empty());
    CHECK(!InputSource{}.valid());

    // Channel 0 means "any", so it collides with a pinned channel rather than
    // sitting beside it. A controller that does not pin its channel would
    // otherwise silently double-bind.
    InputSource any;
    any.kind = SourceKind::MidiCC;
    any.index = 7;
    InputSource ch5 = any;
    ch5.channel = 5;
    CHECK(any.sameAs(ch5));
    CHECK(ch5.sameAs(any));
    InputSource ch6 = any;
    ch6.channel = 6;
    CHECK(!ch5.sameAs(ch6));

    // Different kinds on the same number are different sources.
    InputSource n = any;
    n.kind = SourceKind::MidiNote;
    CHECK(!any.sameAs(n));
}

static void testMappingShape() {
    Mapping m;
    CHECK_NEAR(m.shape(0.f), 0.f);
    CHECK_NEAR(m.shape(0.5f), 0.5f);
    CHECK_NEAR(m.shape(1.f), 1.f);

    // Output range.
    m.outMin = 0.2f;
    m.outMax = 0.8f;
    CHECK_NEAR(m.shape(0.f), 0.2f);
    CHECK_NEAR(m.shape(1.f), 0.8f);
    CHECK_NEAR(m.shape(0.5f), 0.5f);

    // Invert.
    m = Mapping{};
    m.invert = true;
    CHECK_NEAR(m.shape(0.f), 1.f);
    CHECK_NEAR(m.shape(1.f), 0.f);

    // Input range: a fader that only uses the top half of its travel.
    m = Mapping{};
    m.inMin = 0.5f;
    m.inMax = 1.f;
    CHECK_NEAR(m.shape(0.5f), 0.f);
    CHECK_NEAR(m.shape(0.75f), 0.5f);
    CHECK_NEAR(m.shape(1.f), 1.f);
    CHECK_NEAR(m.shape(0.f), 0.f);   // below the range clamps, not wraps

    // An input range written backwards inverts, which is a legitimate way to
    // write it and must not silently produce a dead control.
    m = Mapping{};
    m.inMin = 1.f;
    m.inMax = 0.f;
    CHECK_NEAR(m.shape(0.f), 1.f);
    CHECK_NEAR(m.shape(1.f), 0.f);

    // Curves: all pass through the ends, and bend the right way in between.
    for (BindCurve c : {BindCurve::Linear, BindCurve::Log, BindCurve::Exp,
                        BindCurve::SCurve}) {
        Mapping cm;
        cm.curve = c;
        CHECK_NEAR(cm.shape(0.f), 0.f);
        CHECK_NEAR(cm.shape(1.f), 1.f);
        // Monotonic, or a fader would reverse somewhere in its travel.
        float prev = -1.f;
        for (int i = 0; i <= 20; ++i) {
            const float v = cm.shape(static_cast<float>(i) / 20.f);
            CHECK(v >= prev - 1e-5f);
            prev = v;
        }
    }
    Mapping log;
    log.curve = BindCurve::Log;
    CHECK(log.shape(0.5f) > 0.5f);   // fast at the bottom
    Mapping exp;
    exp.curve = BindCurve::Exp;
    CHECK(exp.shape(0.5f) < 0.5f);
    Mapping s;
    s.curve = BindCurve::SCurve;
    CHECK_NEAR(s.shape(0.5f), 0.5f);
    CHECK(s.shape(0.25f) < 0.25f);
    CHECK(s.shape(0.75f) > 0.75f);
}

// ---------------------------------------------------------------------------
// §12.4's flagged problem
// ---------------------------------------------------------------------------
static void testJumpTakeoverSnaps() {
    Binding b;
    b.map.takeover = Takeover::Jump;
    const auto steps = sweep(b, {0.f, 0.25f, 0.5f, 1.f}, 0.5f);
    // Ugly, but predictable: the value follows from the first event.
    CHECK(steps[0].set);
    CHECK_NEAR(steps[0].value, 0.f);
    CHECK_NEAR(steps[3].value, 1.f);
    for (const SweepStep& s : steps) CHECK_EQ(s.direction, 0);
}

static void testCatchTakeoverIsDeadUntilItCrosses() {
    // Software holds 0.5, the fader sits at 0. Nothing happens until it
    // passes through 0.5 — the only behaviour safe to touch during a show.
    Binding b;
    b.map.takeover = Takeover::Catch;
    const auto steps = sweep(b, {0.f, 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 1.f}, 0.5f);

    for (size_t i = 0; i < 5; ++i) {
        CHECK(!steps[i].set);
        CHECK_NEAR(steps[i].value, 0.5f);
        // And it says which way to move, or the operator is left guessing
        // why the fader is dead.
        CHECK_EQ(steps[i].direction, +1);
    }
    CHECK(steps[5].set);
    CHECK(steps[5].justCaught);
    CHECK_NEAR(steps[5].value, 0.5f);
    CHECK_EQ(steps[5].direction, 0);
    CHECK(steps[6].set);
    CHECK_NEAR(steps[6].value, 0.6f);
    CHECK_NEAR(steps[7].value, 1.f);
    CHECK(b.caught);

    // From above, which is the other half of the problem.
    Binding d;
    d.map.takeover = Takeover::Catch;
    const auto down = sweep(d, {1.f, 0.8f, 0.6f, 0.4f, 0.3f, 0.2f}, 0.3f);
    for (size_t i = 0; i < 3; ++i) {
        CHECK(!down[i].set);
        CHECK_EQ(down[i].direction, -1);
    }
    // 0.4 -> 0.3 crosses the held value, so it catches there.
    CHECK(down[4].set);
    CHECK(down[4].justCaught);
    CHECK_NEAR(down[5].value, 0.2f);

    // Crossing in one jump still catches: a fader flicked past the value must
    // not stay dead because no event landed exactly on it.
    Binding j;
    j.map.takeover = Takeover::Catch;
    const auto jump = sweep(j, {0.1f, 0.9f}, 0.5f);
    CHECK(!jump[0].set);
    CHECK(jump[1].set);
    CHECK(jump[1].justCaught);
    CHECK_NEAR(jump[1].value, 0.9f);

    // A fader already parked on the value is already in control.
    Binding p;
    p.map.takeover = Takeover::Catch;
    const auto parked = sweep(p, {0.5f, 0.7f}, 0.5f);
    CHECK(parked[0].set);
    CHECK(parked[0].justCaught);
    CHECK_NEAR(parked[1].value, 0.7f);

    // Once caught it stays caught, even if the value then moves elsewhere.
    Binding c;
    c.map.takeover = Takeover::Catch;
    CHECK(applyInput(c, cc(7, 0.5f), 0.5f).justCaught);
    const Applied after = applyInput(c, cc(7, 0.2f), 0.9f);
    CHECK(after.hasValue);
    CHECK_NEAR(after.value, 0.2f);
}

static void testScaleTakeoverUsesRemainingTravel() {
    // Software holds 0.5, the fader sits at 0. The first touch anchors and
    // changes nothing; after that the fader's remaining travel covers the
    // remaining range.
    Binding b;
    b.map.takeover = Takeover::Scale;
    const auto up = sweep(b, {0.f, 0.5f, 1.f}, 0.5f);
    CHECK(!up[0].set);                 // touching it moves nothing
    CHECK_NEAR(up[0].value, 0.5f);
    CHECK(up[1].set);
    CHECK_NEAR(up[1].value, 0.75f);    // halfway up the remaining travel
    CHECK_NEAR(up[2].value, 1.f);      // and it reaches the top exactly

    // Downward travel scales against the room below, and returning to the
    // anchor returns the value — the mapping is reversible, not a ratchet.
    Binding d;
    d.map.takeover = Takeover::Scale;
    const auto both = sweep(d, {0.2f, 1.f, 0.6f, 0.2f, 0.f}, 0.8f);
    CHECK(!both[0].set);
    CHECK_NEAR(both[1].value, 1.f);
    CHECK_NEAR(both[3].value, 0.8f);   // back to the anchor
    CHECK_NEAR(both[4].value, 0.f);    // and down to the bottom exactly

    // Anchored at the top: there is no travel left upward, so it cannot
    // overshoot.
    Binding t;
    t.map.takeover = Takeover::Scale;
    const auto capped = sweep(t, {0.5f, 1.f}, 1.f);
    CHECK(!capped[0].set);
    CHECK_NEAR(capped[1].value, 1.f);
}

static void testTakeoverResetsWithThePage() {
    // A fader caught on the outgoing page has not been caught on the incoming
    // one. Carrying the caught flag across would make a page change able to
    // snap the look on the first touch.
    UidGen uids;
    BindingStore store(uids);
    BindingSet s;
    s.name = "Global";
    const Uid setUid = store.addSet(std::move(s)).uid;

    Binding b;
    b.source.kind = SourceKind::MidiCC;
    b.source.index = 7;
    b.target = fader(1, "Master");
    b.map.takeover = Takeover::Catch;
    Binding* live = store.add(setUid, std::move(b));
    CHECK(live != nullptr);

    CHECK(applyInput(*live, cc(7, 0.5f), 0.5f).justCaught);
    CHECK(live->caught);

    store.setPage(2);
    CHECK(!live->caught);
    CHECK(!live->hasLastRaw);
    // Dead again, and pointing the way to the new value rather than snapping.
    const Applied a = applyInput(*live, cc(7, 0.5f), 0.9f);
    CHECK(!a.hasValue);
    CHECK(a.swallowed);
    CHECK_EQ(a.catchDirection, +1);
}

static void testModes() {
    // Trigger: fires on press, ignores the release.
    Binding t;
    t.map.mode = BindMode::Trigger;
    CHECK(applyInput(t, note(60, true), 0.f).fire);
    CHECK(!applyInput(t, note(60, false), 0.f).fire);

    // Momentary: on while held, off on release, and it reports which.
    Binding m;
    m.map.mode = BindMode::Momentary;
    const Applied down = applyInput(m, note(60, true), 0.f);
    CHECK(down.fire);
    CHECK(down.fireOn);
    const Applied up = applyInput(m, note(60, false), 0.f);
    CHECK(up.fire);
    CHECK(!up.fireOn);

    // Toggle: each PRESS flips; the release must not flip it back.
    Binding g;
    g.map.mode = BindMode::Toggle;
    const Applied a = applyInput(g, note(60, true), 0.f);
    CHECK(a.fire);
    CHECK(a.fireOn);
    CHECK(a.hasValue);
    CHECK_NEAR(a.value, 1.f);
    const Applied rel = applyInput(g, note(60, false), 0.f);
    CHECK(!rel.fire);
    CHECK(rel.swallowed);
    const Applied b2 = applyInput(g, note(60, true), 0.f);
    CHECK(b2.fire);
    CHECK(!b2.fireOn);
    CHECK_NEAR(b2.value, 0.f);

    // Toggle state is the state of the thing being controlled, so it survives
    // a runtime reset; takeover state does not.
    g.resetRuntime();
    const Applied b3 = applyInput(g, note(60, true), 0.f);
    CHECK(b3.fireOn);

    // Relative: an endless encoder adds its delta and is always in control,
    // because it has no position to catch up to.
    Binding r;
    r.map.mode = BindMode::Relative;
    r.map.relativeScale = 0.1f;
    InputEvent d;
    d.kind = SourceKind::MidiCC;
    d.isDelta = true;
    d.delta = 1.f;
    const Applied step = applyInput(r, d, 0.5f);
    CHECK(step.hasValue);
    CHECK_NEAR(step.value, 0.6f);
    CHECK(r.caught);
    d.delta = -1.f;
    CHECK_NEAR(applyInput(r, d, 0.6f).value, 0.5f);
    // And it clamps rather than running past the ends.
    d.delta = 100.f;
    CHECK_NEAR(applyInput(r, d, 0.9f).value, 1.f);
    d.delta = -100.f;
    CHECK_NEAR(applyInput(r, d, 0.1f).value, 0.f);
    // A zero delta is not a change.
    d.delta = 0.f;
    CHECK(applyInput(r, d, 0.5f).swallowed);
}

static void testDeadbandAndSmoothing() {
    // Noisy DMX: a deadband means small wobbles do not reach the look.
    Binding b;
    b.map.takeover = Takeover::Jump;
    b.map.deadband = 0.05f;
    CHECK(applyInput(b, cc(7, 0.5f), 0.f).hasValue);
    CHECK(applyInput(b, cc(7, 0.52f), 0.5f).swallowed);
    CHECK(applyInput(b, cc(7, 0.53f), 0.5f).swallowed);
    CHECK(applyInput(b, cc(7, 0.6f), 0.5f).hasValue);

    // Smoothing approaches the target rather than snapping. The FIRST sample
    // seeds the filter at its own value — smoothing up from zero every time
    // input starts would mean a ramp nobody asked for — so a step needs some
    // history behind it to smooth against.
    Binding s;
    s.map.takeover = Takeover::Jump;
    s.map.smoothing = 0.8f;
    const Applied seed = applyInput(s, cc(7, 0.f), 0.f);
    CHECK(seed.hasValue);
    CHECK_NEAR(seed.value, 0.f);

    const Applied step = applyInput(s, cc(7, 1.f), 0.f);
    CHECK(step.hasValue);
    CHECK(step.value > 0.f);
    CHECK(step.value < 1.f);        // it did not snap

    float last = step.value;
    for (int i = 0; i < 80; ++i) {
        const Applied a = applyInput(s, cc(7, 1.f), last);
        CHECK(a.value >= last - 1e-6f);   // monotonic, never overshoots
        last = a.value;
    }
    CHECK_NEAR(last, 1.f);           // and it does arrive

    // Smoothing off means the value is whatever arrived.
    Binding n;
    n.map.takeover = Takeover::Jump;
    CHECK_NEAR(applyInput(n, cc(7, 0.f), 0.f).value, 0.f);
    CHECK_NEAR(applyInput(n, cc(7, 1.f), 0.f).value, 1.f);
}

static void testThresholdEdgesStandInForGpi() {
    // §12.3: GPI is deferred, so a DMX channel crossing a threshold is the
    // contact closure. Rising edge first.
    Binding r;
    r.source.kind = SourceKind::DmxIn;
    r.map.edge = ThresholdEdge::Rising;
    r.map.threshold = 0.5f;
    CHECK(!applyInput(r, dmx(1, 10, 0.f), 0.f).fire);
    const Applied fired = applyInput(r, dmx(1, 10, 0.6f), 0.f);
    CHECK(fired.fire);
    CHECK(fired.fireOn);
    // Staying high does not fire again.
    CHECK(!applyInput(r, dmx(1, 10, 0.9f), 0.f).fire);
    // Dropping below, then rising again, does.
    CHECK(!applyInput(r, dmx(1, 10, 0.1f), 0.f).fire);
    CHECK(applyInput(r, dmx(1, 10, 0.7f), 0.f).fire);

    // Hysteresis: a channel sitting on the threshold must not chatter the
    // binding every frame.
    Binding h;
    h.map.edge = ThresholdEdge::Rising;
    h.map.threshold = 0.5f;
    h.map.hysteresis = 0.1f;
    CHECK(applyInput(h, dmx(1, 1, 0.51f), 0.f).fire);
    int fires = 0;
    const float jitter[] = {0.49f, 0.51f, 0.48f, 0.52f, 0.45f, 0.5f};
    for (float v : jitter)
        if (applyInput(h, dmx(1, 1, v), 0.f).fire) ++fires;
    CHECK_EQ(fires, 0);
    // Only a real drop below the lower bound re-arms it.
    CHECK(!applyInput(h, dmx(1, 1, 0.3f), 0.f).fire);
    CHECK(applyInput(h, dmx(1, 1, 0.8f), 0.f).fire);

    // Falling edge.
    Binding f;
    f.map.edge = ThresholdEdge::Falling;
    f.map.threshold = 0.5f;
    f.map.hysteresis = 0.f;
    CHECK(!applyInput(f, dmx(1, 1, 0.9f), 0.f).fire);
    const Applied dropped = applyInput(f, dmx(1, 1, 0.1f), 0.f);
    CHECK(dropped.fire);
    CHECK(!dropped.fireOn);

    // While-above is a state, not an edge: it reports both transitions.
    Binding w;
    w.map.edge = ThresholdEdge::WhileAbove;
    w.map.threshold = 0.5f;
    w.map.hysteresis = 0.f;
    const Applied on = applyInput(w, dmx(1, 1, 0.8f), 0.f);
    CHECK(on.fire);
    CHECK(on.fireOn);
    CHECK(!applyInput(w, dmx(1, 1, 0.9f), 0.f).fire);   // no change
    const Applied off = applyInput(w, dmx(1, 1, 0.2f), 0.f);
    CHECK(off.fire);
    CHECK(!off.fireOn);
}

static void testDisabledBindingDoesNothing() {
    Binding b;
    b.enabled = false;
    b.map.takeover = Takeover::Jump;
    const Applied a = applyInput(b, cc(7, 1.f), 0.f);
    CHECK(!a.hasValue);
    CHECK(!a.fire);
    CHECK(a.swallowed);
}

// ---------------------------------------------------------------------------
// Layering (§12.6)
// ---------------------------------------------------------------------------
static void testPageLocalBindingsWinOverGlobal() {
    UidGen uids;
    BindingStore store(uids);

    BindingSet g;
    g.name = "Global";
    g.page = 0;
    const Uid gu = store.addSet(std::move(g)).uid;

    BindingSet p1;
    p1.name = "Page 1";
    p1.page = 1;
    const Uid p1u = store.addSet(std::move(p1)).uid;

    BindingSet p2;
    p2.name = "Page 2";
    p2.page = 2;
    const Uid p2u = store.addSet(std::move(p2)).uid;

    auto bind = [&](Uid setUid, int ccNumber, const std::string& cmd) {
        Binding b;
        b.source.kind = SourceKind::MidiCC;
        b.source.index = ccNumber;
        b.target = command(cmd);
        store.add(setUid, std::move(b));
    };
    bind(gu, 7, "Global 7");
    bind(p1u, 7, "Page1 7");
    bind(p2u, 7, "Page2 7");
    bind(gu, 8, "Global 8");

    // On page 0 only the global one exists.
    store.setPage(0);
    auto hit = store.resolve(cc(7, 1.f));
    CHECK_EQ(hit.size(), size_t(1));
    CHECK(hit[0]->target.command == "Global 7");

    // On page 1 the page-local one SHADOWS the global rather than both firing.
    store.setPage(1);
    hit = store.resolve(cc(7, 1.f));
    CHECK_EQ(hit.size(), size_t(1));
    CHECK(hit[0]->target.command == "Page1 7");
    // And the other page's binding is not reachable from here.
    for (Binding* b : store.resolveAll(cc(7, 1.f)))
        CHECK(b->target.command != "Page2 7");

    store.setPage(2);
    hit = store.resolve(cc(7, 1.f));
    CHECK_EQ(hit.size(), size_t(1));
    CHECK(hit[0]->target.command == "Page2 7");

    // An input with no page-local binding still falls through to global —
    // which is what makes a global master work on every page.
    hit = store.resolve(cc(8, 1.f));
    CHECK_EQ(hit.size(), size_t(1));
    CHECK(hit[0]->target.command == "Global 8");

    // Disabling a set takes it out of resolution without deleting it.
    store.activate(p2u, false);
    hit = store.resolve(cc(7, 1.f));
    CHECK_EQ(hit.size(), size_t(1));
    CHECK(hit[0]->target.command == "Global 7");
    store.activate(p2u, true);
    CHECK(store.resolve(cc(7, 1.f))[0]->target.command == "Page2 7");

    // A disabled individual binding is skipped too.
    store.set(p2u)->bindings.front().enabled = false;
    CHECK(store.resolve(cc(7, 1.f))[0]->target.command == "Global 7");
}

static void testSeveralTargetsInOneLayerAllFire() {
    // §12.6: one input driving several things is sometimes the point, so
    // matches within a layer are not deduplicated.
    UidGen uids;
    BindingStore store(uids);
    BindingSet g;
    g.name = "Global";
    const Uid gu = store.addSet(std::move(g)).uid;

    for (const char* cmd : {"Go 1", "Go 2", "Go 3"}) {
        Binding b;
        b.source.kind = SourceKind::MidiNote;
        b.source.index = 36;
        b.target = command(cmd);
        store.add(gu, std::move(b));
    }
    const auto hit = store.resolve(note(36, true));
    CHECK_EQ(hit.size(), size_t(3));
    CHECK_EQ(store.bindingCount(), size_t(3));
}

static void testConflictDetection() {
    UidGen uids;
    BindingStore store(uids);
    BindingSet g;
    g.name = "Global";
    const Uid gu = store.addSet(std::move(g)).uid;
    BindingSet p;
    p.name = "Page 3";
    p.page = 3;
    const Uid pu = store.addSet(std::move(p)).uid;

    InputSource s;
    s.kind = SourceKind::MidiCC;
    s.index = 7;

    CHECK(!store.wouldConflict(s, 0).any);

    Binding b;
    b.source = s;
    b.target = fader(1, "Master");
    store.add(gu, std::move(b));

    const Conflict c = store.wouldConflict(s, 0);
    CHECK(c.any);
    CHECK_EQ(c.existing.size(), size_t(1));
    // It names what it would join rather than just saying "conflict".
    CHECK(c.existing[0].find("Global") != std::string::npos);
    CHECK(c.existing[0].find("CC 7") != std::string::npos);
    CHECK(c.existing[0].find("Master") != std::string::npos);

    // A binding on ANOTHER page is not a conflict: that is what pages are for.
    Binding pb;
    pb.source = s;
    pb.target = fader(2, "Other");
    store.add(pu, std::move(pb));
    const Conflict onPage4 = store.wouldConflict(s, 4);
    CHECK_EQ(onPage4.existing.size(), size_t(1));   // just the global one
    const Conflict onPage3 = store.wouldConflict(s, 3);
    CHECK_EQ(onPage3.existing.size(), size_t(2));

    // A disabled set is not in the way.
    store.activate(gu, false);
    CHECK_EQ(store.wouldConflict(s, 4).existing.size(), size_t(0));
}

// ---------------------------------------------------------------------------
// Learn (§12.1)
// ---------------------------------------------------------------------------
static void testLearnFlow() {
    UidGen uids;
    BindingStore store(uids);
    BindingSet s;
    s.name = "APC40 busking";
    const Uid setUid = store.addSet(std::move(s)).uid;

    Learn learn(store);
    learn.setTargetSet(setUid);
    learn.setTargets({fader(1, "Exec 1"), fader(2, "Exec 2"), fader(3, "Exec 3"),
                      fader(4, "Exec 4")});

    // Nothing happens until learn is on.
    CHECK(!learn.active());
    CHECK(learn.receive(cc(7, 1.f)).outcome == LearnResult::Outcome::Ignored);
    CHECK_EQ(store.bindingCount(), size_t(0));

    learn.begin();
    CHECK(learn.active());
    CHECK_EQ(learn.focusIndex(), 0);

    // Four fader moves map four faders, because learn auto-advances (§12.1).
    for (int i = 0; i < 4; ++i) {
        const LearnResult r = learn.receive(cc(7 + i, 0.5f));
        CHECK(r.outcome == (i < 3 ? LearnResult::Outcome::Advanced
                                  : LearnResult::Outcome::Bound));
        CHECK(r.binding.valid());
        CHECK(r.message.find("CC " + std::to_string(7 + i)) != std::string::npos);
    }
    CHECK_EQ(store.bindingCount(), size_t(4));
    CHECK_EQ(learn.focusIndex(), 3);   // stops at the end rather than wrapping

    // A learned fader is usable before anyone opens its settings.
    const Binding* first = store.set(setUid)->findBySource([] {
        InputSource s2;
        s2.kind = SourceKind::MidiCC;
        s2.index = 7;
        return s2;
    }());
    CHECK(first != nullptr);
    if (first) {
        CHECK(first->map.mode == BindMode::Absolute);
        CHECK(first->map.takeover == Takeover::Catch);   // the safe default
        CHECK(first->target.sameAs(fader(1, "Exec 1")));
    }

    // Badges show what reaches each target.
    CHECK(learn.badgeFor(fader(1, "Exec 1")) == "CC 7");
    CHECK(learn.badgeFor(fader(4, "Exec 4")) == "CC 10");
    CHECK(learn.badgeFor(fader(9, "Unbound")).empty());

    // Auto-advance off: the focus stays put, so one target can be re-learned.
    learn.setAutoAdvance(false);
    CHECK(learn.focus(0));
    const LearnResult again = learn.receive(cc(20, 0.5f));
    CHECK(again.outcome == LearnResult::Outcome::Bound);
    CHECK_EQ(learn.focusIndex(), 0);
    // Re-learning MOVED the binding rather than stacking a second one.
    CHECK_EQ(store.bindingCount(), size_t(4));
    CHECK(learn.badgeFor(fader(1, "Exec 1")) == "CC 20");

    // Unlearn, and clearing the set.
    const LearnResult un = learn.unlearnFocused();
    CHECK(un.outcome == LearnResult::Outcome::Unlearned);
    CHECK_EQ(store.bindingCount(), size_t(3));
    CHECK(learn.badgeFor(fader(1, "Exec 1")).empty());
    CHECK(learn.unlearnFocused().outcome == LearnResult::Outcome::Ignored);

    CHECK_EQ(learn.clearSet(), 3);
    CHECK_EQ(store.bindingCount(), size_t(0));

    learn.end();
    CHECK(!learn.active());
    CHECK(learn.receive(cc(7, 1.f)).outcome == LearnResult::Outcome::Ignored);
}

static void testLearnConflictsNeedConfirming() {
    UidGen uids;
    BindingStore store(uids);
    BindingSet s;
    s.name = "Set";
    const Uid setUid = store.addSet(std::move(s)).uid;

    Learn learn(store);
    learn.setTargetSet(setUid);
    learn.setTargets({fader(1, "A"), fader(2, "B")});
    learn.setAutoAdvance(false);
    learn.begin();

    CHECK(learn.focus(0));
    CHECK(learn.receive(cc(7, 0.5f)).outcome == LearnResult::Outcome::Bound);

    // The same input on a DIFFERENT target warns instead of silently stacking.
    CHECK(learn.focus(1));
    const LearnResult conflict = learn.receive(cc(7, 0.5f));
    CHECK(conflict.outcome == LearnResult::Outcome::Conflict);
    CHECK(conflict.conflict.any);
    CHECK_EQ(conflict.conflict.existing.size(), size_t(1));
    CHECK_EQ(store.bindingCount(), size_t(1));   // nothing was changed

    // Allowed on confirm, because one input driving two things is sometimes
    // the point.
    const LearnResult forced = learn.confirm(cc(7, 0.5f));
    CHECK(forced.outcome == LearnResult::Outcome::Bound);
    CHECK_EQ(store.bindingCount(), size_t(2));
    CHECK_EQ(store.resolve(cc(7, 1.f)).size(), size_t(2));

    // Re-learning the SAME target with the input it already has is a change,
    // not a conflict — otherwise correcting a mapping needs a confirmation
    // every time.
    CHECK(learn.focus(1));
    CHECK(learn.receive(cc(7, 0.5f)).outcome == LearnResult::Outcome::Conflict);
}

static void testLearnRefusesWithoutASet() {
    UidGen uids;
    BindingStore store(uids);
    Learn learn(store);
    learn.setTargets({fader(1, "A")});
    learn.begin();
    // No set selected: refused rather than inventing one the user did not ask
    // for.
    const LearnResult r = learn.receive(cc(7, 1.f));
    CHECK(r.outcome == LearnResult::Outcome::Ignored);
    CHECK(r.message.find("binding set") != std::string::npos);

    // No targets at all.
    Learn l2(store);
    BindingSet s;
    s.name = "S";
    l2.setTargetSet(store.addSet(std::move(s)).uid);
    l2.begin();
    CHECK_EQ(l2.focusIndex(), -1);
    CHECK(l2.focused() == nullptr);
    CHECK(l2.receive(cc(7, 1.f)).outcome == LearnResult::Outcome::Ignored);
    CHECK(!l2.focus(0));
    CHECK(!l2.focus(-1));

    // An event with no source cannot be learned.
    Learn l3(store);
    BindingSet s3;
    s3.name = "S3";
    l3.setTargetSet(store.addSet(std::move(s3)).uid);
    l3.setTargets({fader(1, "A")});
    l3.begin();
    InputEvent blank;
    CHECK(l3.receive(blank).outcome == LearnResult::Outcome::Ignored);
}

static void testLearnBindsEveryKindOfSource() {
    UidGen uids;
    BindingStore store(uids);
    BindingSet s;
    s.name = "Mixed";
    const Uid setUid = store.addSet(std::move(s)).uid;
    Learn learn(store);
    learn.setTargetSet(setUid);
    learn.setTargets({command("Go 1"), command("Go 2"), command("Go 3"),
                      fader(1, "Master")});
    learn.begin();

    // A command target is the escape hatch (§12.2), and it defaults to
    // Trigger because firing a command on a fader sweep would be unusable.
    CHECK(learn.receive(note(36, true)).outcome == LearnResult::Outcome::Advanced);
    CHECK(learn.receive(osc("/lumen/go", 1.f)).outcome ==
          LearnResult::Outcome::Advanced);
    CHECK(learn.receive(dmx(2, 14, 1.f)).outcome == LearnResult::Outcome::Advanced);
    CHECK(learn.receive(cc(7, 0.5f)).outcome == LearnResult::Outcome::Bound);

    CHECK(learn.badgeFor(command("Go 1")) == "Note 36");
    CHECK(learn.badgeFor(command("Go 2")) == "/lumen/go");
    CHECK(learn.badgeFor(command("Go 3")) == "U2.14");
    CHECK(learn.badgeFor(fader(1, "Master")) == "CC 7");

    for (const Binding& b : store.set(setUid)->bindings) {
        if (b.target.kind == TargetKind::Command)
            CHECK(b.map.mode == BindMode::Trigger);
        else
            CHECK(b.map.mode == BindMode::Absolute);
    }

    // Each of them resolves back to its own binding.
    CHECK_EQ(store.resolve(note(36, true)).size(), size_t(1));
    CHECK_EQ(store.resolve(osc("/lumen/go", 1.f)).size(), size_t(1));
    CHECK_EQ(store.resolve(dmx(2, 14, 1.f)).size(), size_t(1));
    // A different OSC address, universe or channel does not.
    CHECK(store.resolve(osc("/lumen/other", 1.f)).empty());
    CHECK(store.resolve(dmx(2, 15, 1.f)).empty());
    CHECK(store.resolve(dmx(3, 14, 1.f)).empty());
}

// ---------------------------------------------------------------------------
// Input queue (§12.7)
// ---------------------------------------------------------------------------
static void testInputQueue() {
    InputQueue q(8);
    CHECK_EQ(q.size(), size_t(0));
    CHECK_EQ(q.dropped(), size_t(0));

    for (int i = 0; i < 8; ++i) CHECK(q.push(cc(i, 0.5f)));
    CHECK_EQ(q.size(), size_t(8));

    // Bounded: a stuck controller flooding the queue drops events and SAYS
    // so, rather than growing until the show crashes hours in.
    CHECK(!q.push(cc(99, 1.f)));
    CHECK_EQ(q.dropped(), size_t(1));
    CHECK_EQ(q.size(), size_t(8));

    const auto drained = q.drain();
    CHECK_EQ(drained.size(), size_t(8));
    CHECK_EQ(q.size(), size_t(0));
    // Order is preserved, because a note-on arriving after a note-off is a
    // different thing than the other way round.
    for (int i = 0; i < 8; ++i) CHECK_EQ(drained[static_cast<size_t>(i)].index, i);
    CHECK(q.drain().empty());

    q.clearDropped();
    CHECK_EQ(q.dropped(), size_t(0));
}

static void testInputQueueUnderConcurrentProducers() {
    // The point of the queue is that several input threads can push while the
    // control thread drains, without tearing or losing count.
    InputQueue q(100000);
    constexpr int kProducers = 4;
    constexpr int kEach = 2000;

    std::vector<std::thread> threads;
    for (int p = 0; p < kProducers; ++p) {
        threads.emplace_back([&q, p] {
            for (int i = 0; i < kEach; ++i) q.push(cc(p, static_cast<float>(i) / kEach));
        });
    }

    size_t seen = 0;
    bool joined = false;
    // Drain while they run, then once more after.
    while (!joined) {
        seen += q.drain().size();
        if (seen >= kProducers * kEach) joined = true;
        else {
            bool allDone = true;
            for (auto& t : threads)
                if (t.joinable()) allDone = false;
            if (allDone) joined = true;
        }
        if (!joined && seen > 0) continue;
        if (seen < kProducers * kEach) {
            for (auto& t : threads)
                if (t.joinable()) t.join();
            seen += q.drain().size();
            joined = true;
        }
    }
    for (auto& t : threads)
        if (t.joinable()) t.join();
    seen += q.drain().size();

    CHECK_EQ(seen, size_t(kProducers * kEach));
    CHECK_EQ(q.dropped(), size_t(0));
}

// ---------------------------------------------------------------------------
// Through the engine and the command layer (§12.7, §12.2)
// ---------------------------------------------------------------------------
static void testInputReachesTheShowThroughTheQueue() {
    Engine eng;
    Console con(eng);
    installStarterTypes(eng.patch);
    CHECK(con.execute("Fixture 1 Thru 4 Patch \"LED Par RGBW\" 4 1 1").ok);
    CHECK(con.execute("Fixture 1 Thru 4 At 100").ok);
    CHECK(con.execute("Store Cue 1 1").ok);
    CHECK(con.execute("Assign 1 1").ok);
    CHECK(con.execute("Clear").ok);
    CHECK(con.execute("Clear").ok);
    CHECK(con.execute("Clear").ok);

    // Learn a note onto a command and a fader onto the grand master.
    CHECK(con.execute("BindingSet \"Show\"").ok);
    CHECK(con.execute("Learn on \"Show\"").ok);
    CHECK(con.learn().active());

    BindTarget go;
    go.kind = TargetKind::Command;
    go.command = "Go 1";
    go.label = "Go";
    BindTarget gm;
    gm.kind = TargetKind::Master;
    gm.subIndex = 0;
    gm.label = "Grand";
    con.learn().setTargets({go, gm});
    CHECK(con.learn().focus(0));

    CHECK(con.learn().receive(note(36, true)).binding.valid());
    CHECK(con.learn().receive(cc(7, 0.5f)).binding.valid());
    con.learn().end();
    CHECK(con.learn().badgeFor(go) == "Note 36");
    CHECK(con.learn().badgeFor(gm) == "CC 7");

    // Play the note the way a MIDI thread would: onto the queue, drained at
    // the top of tick(), dispatched by the command layer.
    eng.setClock(0.0);
    CHECK(eng.executor(1) != nullptr);
    CHECK(!eng.executor(1)->active());

    CHECK(eng.inputs.push(note(36, true)));
    eng.tick();
    const Console::Pumped p = con.pumpBindings();
    CHECK_EQ(p.actions, 1);
    CHECK_EQ(p.executed, 1);
    CHECK_EQ(p.failed, 0);
    CHECK(p.messages.empty());
    CHECK(eng.executor(1)->active());
    CHECK_EQ(eng.executor(1)->currentIndex(), 0);

    // The release of a note bound as a Trigger is not a second press.
    CHECK(eng.inputs.push(note(36, false)));
    eng.tick();
    CHECK_EQ(con.pumpBindings().actions, 0);

    // The grand master is a target the ENGINE owns, so it is applied during
    // the drain rather than handed up as a command.
    CHECK_NEAR(eng.grandMaster, 1.f);
    for (float v : {0.5f, 0.7f, 0.9f}) {
        CHECK(eng.inputs.push(cc(7, v)));
        eng.tick();
        CHECK_EQ(con.pumpBindings().actions, 0);   // nothing for the console
        CHECK_NEAR(eng.grandMaster, 1.f);          // catch: still dead
    }
    CHECK(eng.inputs.push(cc(7, 1.f)));
    eng.tick();
    CHECK_NEAR(eng.grandMaster, 1.f);              // caught, at the same value
    CHECK(eng.inputs.push(cc(7, 0.4f)));
    eng.tick();
    CHECK_NEAR(eng.grandMaster, 0.4f);             // and now it follows

    // A command that fails is reported rather than swallowed.
    BindingSet* s = eng.bindings.setByName("Show");
    CHECK(s != nullptr);
    Binding broken;
    broken.source.kind = SourceKind::MidiNote;
    broken.source.channel = 1;
    broken.source.index = 99;
    broken.target = command("ThisIsNotACommand");
    broken.map.mode = BindMode::Trigger;
    CHECK(eng.bindings.add(s->uid, std::move(broken)) != nullptr);
    CHECK(eng.inputs.push(note(99, true)));
    eng.tick();
    const Console::Pumped bad = con.pumpBindings();
    CHECK_EQ(bad.actions, 1);
    CHECK_EQ(bad.failed, 1);
    CHECK(!bad.messages.empty());
}

static void testAFloodCannotGrowWithoutBound() {
    // §12.7's reason for a queue at all: a stuck controller sending at full
    // rate must not become a slow memory leak that crashes the show hours in.
    Engine eng;
    Console con(eng);
    BindingSet s;
    s.name = "Flood";
    const Uid setUid = eng.bindings.addSet(std::move(s)).uid;
    Binding b;
    b.source.kind = SourceKind::MidiCC;
    b.source.channel = 1;
    b.source.index = 7;
    b.target.kind = TargetKind::Master;
    b.target.subIndex = 0;
    b.map.takeover = Takeover::Jump;
    CHECK(eng.bindings.add(setUid, std::move(b)) != nullptr);

    eng.setClock(0.0);
    for (int i = 0; i < 100000; ++i)
        eng.inputs.push(cc(7, static_cast<float>(i % 100) / 100.f));

    // Capped, and it counted what it threw away.
    CHECK(eng.inputs.size() <= size_t(4096));
    CHECK(eng.inputs.dropped() > size_t(90000));

    // One tick drains the whole backlog; the queue does not carry over.
    eng.tick();
    CHECK_EQ(eng.inputs.size(), size_t(0));
    CHECK(con.pumpBindings().actions == 0);   // the master is the engine's own
}

static void testPageCommandSwitchesLayerAndResetsTakeover() {
    Engine eng;
    Console con(eng);

    CHECK(con.execute("BindingSet \"Global\"").ok);
    CHECK(con.execute("BindingSet \"Page 2\"").ok);
    CHECK(con.execute("BindingSet \"Page 2\" page 2").ok);
    CHECK(eng.bindings.setByName("Page 2")->page == 2);

    auto bindTo = [&](const char* setName, const char* cmd) {
        BindingSet* s = eng.bindings.setByName(setName);
        CHECK(s != nullptr);
        Binding b;
        b.source.kind = SourceKind::MidiNote;
        b.source.channel = 1;
        b.source.index = 36;
        b.target = command(cmd);
        b.map.mode = BindMode::Trigger;
        eng.bindings.add(s->uid, std::move(b));
    };
    bindTo("Global", "GrandMaster 100");
    bindTo("Page 2", "Blackout on");

    // On page 0 the global binding runs.
    CHECK(con.execute("Page 0").ok);
    CHECK(eng.inputs.push(note(36, true)));
    eng.tick();
    CHECK_EQ(con.pumpBindings().executed, 1);
    CHECK(!eng.blackout);

    // On page 2 the page-local one shadows it entirely.
    CHECK(con.execute("Page 2").ok);
    CHECK(eng.inputs.push(note(36, true)));
    eng.tick();
    CHECK_EQ(con.pumpBindings().executed, 1);
    CHECK(eng.blackout);

    CHECK(!con.execute("Page -1").ok);

    // Disabling a set takes it out without deleting it.
    CHECK(con.execute("BindingSet \"Page 2\" off").ok);
    CHECK(eng.bindings.setByName("Page 2") != nullptr);
    CHECK(!eng.bindings.setByName("Page 2")->enabled);
    CHECK(con.execute("Blackout off").ok);
    CHECK(eng.inputs.push(note(36, true)));
    eng.tick();
    CHECK_EQ(con.pumpBindings().executed, 1);
    CHECK(!eng.blackout);   // the global one ran instead

    CHECK(!con.execute("BindingSet \"Nope\" off").ok);
}

static void testLearnCommands() {
    Engine eng;
    Console con(eng);

    // Learn with no set makes one rather than refusing, and says so.
    Result on = con.execute("Learn on");
    CHECK(on.ok);
    CHECK(on.message.find("Default") != std::string::npos);
    CHECK(con.learn().active());
    CHECK(con.learn().targetSet().valid());

    con.learn().setTargets({command("Go 1"), command("Go 2"), command("Go 3")});
    CHECK(con.learn().focus(0));

    // Learn next walks the targets.
    CHECK(con.execute("Learn next").ok);
    CHECK_EQ(con.learn().focusIndex(), 1);
    CHECK(con.execute("Learn next").ok);
    CHECK_EQ(con.learn().focusIndex(), 2);
    CHECK(!con.execute("Learn next").ok);   // no further target

    CHECK(con.learn().focus(0));
    CHECK(con.learn().receive(note(36, true)).binding.valid());
    CHECK_EQ(eng.bindings.bindingCount(), size_t(1));

    // Unlearn needs something focused and bound.
    CHECK(con.learn().focus(0));
    CHECK(con.execute("Unlearn").ok);
    CHECK_EQ(eng.bindings.bindingCount(), size_t(0));
    CHECK(!con.execute("Unlearn").ok);

    CHECK(con.learn().receive(note(36, true)).binding.valid());
    Result cleared = con.execute("Learn clear");
    CHECK(cleared.ok);
    CHECK_EQ(eng.bindings.bindingCount(), size_t(0));

    CHECK(con.execute("Learn off").ok);
    CHECK(!con.learn().active());

    CHECK(!con.execute("Learn on \"Nonexistent\"").ok);
}

static void testBindingSetsTravelWithTheShow() {
    // §12.6: binding sets are stored in the show, so a controller map travels
    // with the show rather than with the machine.
    const std::string path = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR")
                                                               : "/tmp") +
                             "/lumen_binding_show.lumen";
    std::remove(path.c_str());

    Engine a;
    Console con(a);

    BindingSet global;
    global.name = "Global";
    const Uid gu = a.bindings.addSet(std::move(global)).uid;
    BindingSet page;
    page.name = "Launchpad page 2";
    page.page = 2;
    page.enabled = false;   // disabled, and it must stay that way
    const Uid pu = a.bindings.addSet(std::move(page)).uid;

    Binding master;
    master.source.kind = SourceKind::MidiCC;
    master.source.channel = 5;
    master.source.index = 7;
    master.target.kind = TargetKind::Master;
    master.target.subIndex = 0;
    master.target.label = "Grand";
    master.label = "Grand master";
    master.map.takeover = Takeover::Scale;
    master.map.curve = BindCurve::SCurve;
    master.map.inMin = 0.1f;
    master.map.inMax = 0.9f;
    master.map.outMin = 0.2f;
    master.map.outMax = 0.8f;
    master.map.invert = true;
    master.map.deadband = 0.02f;
    master.map.smoothing = 0.3f;
    master.map.feedback = true;
    a.bindings.add(gu, std::move(master));

    Binding houseLights;
    houseLights.source.kind = SourceKind::DmxIn;
    houseLights.source.channel = 2;
    houseLights.source.index = 14;
    houseLights.target = command("Blackout on");
    houseLights.map.edge = ThresholdEdge::Rising;
    houseLights.map.threshold = 0.75f;
    houseLights.map.hysteresis = 0.05f;
    a.bindings.add(gu, std::move(houseLights));

    Binding toggled;
    toggled.source.kind = SourceKind::Osc;
    toggled.source.address = "/lumen/exec/1";
    toggled.source.argIndex = 1;
    toggled.target = command("Go 1");
    toggled.map.mode = BindMode::Toggle;
    toggled.toggleState = true;   // the state of the thing, so it persists
    toggled.enabled = false;
    a.bindings.add(gu, std::move(toggled));

    Binding onPage;
    onPage.source.kind = SourceKind::Key;
    onPage.source.index = 65;
    onPage.source.mods = Mods::Ctrl | Mods::Alt;
    onPage.target = command("Release 1");
    onPage.map.mode = BindMode::Momentary;
    onPage.map.relativeScale = 0.25f;
    a.bindings.add(pu, std::move(onPage));

    // Catch something, so the test can check takeover state does NOT persist.
    Binding* live = a.bindings.set(gu)->findBySource([] {
        InputSource s;
        s.kind = SourceKind::MidiCC;
        s.channel = 5;
        s.index = 7;
        return s;
    }());
    CHECK(live != nullptr);
    if (live) {
        applyInput(*live, cc(7, 0.5f, 5), 0.5f);
        applyInput(*live, cc(7, 0.6f, 5), 0.5f);
        CHECK(live->caught);
    }

    SaveReport sr = ShowFile::save(a, path, "Bindings");
    CHECK(sr.ok);
    CHECK(sr.error.empty());
    CHECK_EQ(sr.bindingSets, 2);
    CHECK_EQ(sr.bindings, 4);

    Engine b;
    LoadReport lr = ShowFile::load(b, path);
    CHECK(lr.ok);
    CHECK(lr.error.empty());
    CHECK(lr.warnings.empty());
    CHECK_EQ(lr.bindingSets, 2);
    CHECK_EQ(lr.bindings, 4);

    const BindingSet* lg = b.bindings.setByName("Global");
    const BindingSet* lp = b.bindings.setByName("Launchpad page 2");
    CHECK(lg != nullptr);
    CHECK(lp != nullptr);
    if (!lg || !lp) return;
    CHECK_EQ(lg->page, 0);
    CHECK(lg->enabled);
    CHECK_EQ(lp->page, 2);
    CHECK(!lp->enabled);              // a disabled set stays disabled
    CHECK_EQ(lg->bindings.size(), size_t(3));
    CHECK_EQ(lp->bindings.size(), size_t(1));

    // Order within a set is preserved, because resolution order depends on it.
    const Binding& m = lg->bindings[0];
    CHECK(m.label == "Grand master");
    CHECK(m.source.kind == SourceKind::MidiCC);
    CHECK_EQ(m.source.channel, 5);
    CHECK_EQ(m.source.index, 7);
    CHECK(m.source.describe() == "CC 7 ch5");
    CHECK(m.target.kind == TargetKind::Master);
    CHECK_EQ(m.target.subIndex, 0);
    CHECK(m.map.takeover == Takeover::Scale);
    CHECK(m.map.curve == BindCurve::SCurve);
    CHECK_NEAR(m.map.inMin, 0.1f);
    CHECK_NEAR(m.map.inMax, 0.9f);
    CHECK_NEAR(m.map.outMin, 0.2f);
    CHECK_NEAR(m.map.outMax, 0.8f);
    CHECK(m.map.invert);
    CHECK_NEAR(m.map.deadband, 0.02f);
    CHECK_NEAR(m.map.smoothing, 0.3f);
    CHECK(m.map.feedback);
    // Takeover state is NOT restored: a reloaded show has not had its faders
    // touched, so nothing is caught until it is.
    CHECK(!m.caught);
    CHECK(!m.hasAnchor);

    const Binding& house = lg->bindings[1];
    CHECK(house.source.kind == SourceKind::DmxIn);
    CHECK(house.source.describe() == "U2.14");
    CHECK(house.map.edge == ThresholdEdge::Rising);
    CHECK_NEAR(house.map.threshold, 0.75f);
    CHECK_NEAR(house.map.hysteresis, 0.05f);
    CHECK(house.target.command == "Blackout on");

    const Binding& tog = lg->bindings[2];
    CHECK(tog.source.kind == SourceKind::Osc);
    CHECK(tog.source.address == "/lumen/exec/1");
    CHECK_EQ(tog.source.argIndex, 1);
    CHECK(tog.map.mode == BindMode::Toggle);
    CHECK(tog.toggleState);        // the controlled thing's state persists
    CHECK(!tog.enabled);           // and so does a disabled binding

    const Binding& key = lp->bindings[0];
    CHECK(key.source.kind == SourceKind::Key);
    CHECK(key.source.describe() == "Ctrl+Alt+Key 65");
    CHECK(key.map.mode == BindMode::Momentary);
    CHECK_NEAR(key.map.relativeScale, 0.25f);

    // And the reloaded bindings resolve and work.
    b.bindings.setPage(0);
    CHECK_EQ(b.bindings.resolve(cc(7, 0.5f, 5)).size(), size_t(1));
    CHECK(b.bindings.resolve(cc(7, 0.5f, 6)).empty());   // the channel is pinned
    CHECK_EQ(b.bindings.resolve(dmx(2, 14, 1.f)).size(), size_t(1));

    Console con2(b);
    con2.setPrivilege(Privilege::Operator);
    CHECK(!b.blackout);
    CHECK(b.inputs.push(dmx(2, 14, 1.f)));
    b.setClock(0.0);
    b.tick();
    const Console::Pumped p = con2.pumpBindings();
    CHECK_EQ(p.executed, 1);
    CHECK(b.blackout);

    std::remove(path.c_str());
}

int main() {
    std::printf("lumen binding\n");
    testSourceDescriptionsMatchTheSpecBadges();
    testMappingShape();
    testJumpTakeoverSnaps();
    testCatchTakeoverIsDeadUntilItCrosses();
    testScaleTakeoverUsesRemainingTravel();
    testTakeoverResetsWithThePage();
    testModes();
    testDeadbandAndSmoothing();
    testThresholdEdgesStandInForGpi();
    testDisabledBindingDoesNothing();
    testPageLocalBindingsWinOverGlobal();
    testSeveralTargetsInOneLayerAllFire();
    testConflictDetection();
    testLearnFlow();
    testLearnConflictsNeedConfirming();
    testLearnRefusesWithoutASet();
    testLearnBindsEveryKindOfSource();
    testInputQueue();
    testInputQueueUnderConcurrentProducers();
    testInputReachesTheShowThroughTheQueue();
    testAFloodCannotGrowWithoutBound();
    testPageCommandSwitchesLayerAndResetsTakeover();
    testLearnCommands();
    testBindingSetsTravelWithTheShow();
    return check::report("binding");
}
