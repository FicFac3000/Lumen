// Tests for the merge pipeline, priority, release, retention and DMX render.
#include "Check.hpp"

#include "lumen/Engine.hpp"
#include "lumen/TripleBuffer.hpp"

#include <cmath>
#include <thread>

using namespace lumen;

namespace {

// Two LED pars in universe 1, at addresses 1 and 7.
struct Rig {
    Engine eng;
    int par = -1;

    Rig() {
        installStarterTypes(eng.patch);
        par = eng.patch.findType("LED Par RGBW");
        std::string err;
        eng.patch.addRange(1, 4, par, 1, 1, 0, &err);
        eng.setClock(0.0);
        eng.syncUniverses();
    }

    // Builds a one-cue sequence driving fixture `id`.
    Sequence& seq(int sid, FixtureId id, Attr a, float v) {
        Sequence& s = eng.ensureSequence(sid);
        s.cues.resize(1);
        s.cues[0].number = 1.f;
        s.cues[0].timing.fadeIn = 0.0;  // snap, so tests read steady state
        s.cues[0].values().set(id, 0, a, Value::hard(v));
        return s;
    }

    Executor& exec(int eid, int sid) {
        Executor& e = eng.ensureExecutor(eid);
        e.sequenceId = sid;
        return e;
    }

    float out(FixtureId id, Attr a) const {
        const auto v = eng.merged().get(makeKey(id, 0, a));
        return v ? *v : -1.f;
    }
    int dmx(int uni, int ch) const {
        const Universe* u = eng.universe(uni);
        return u ? static_cast<int>(u->data[static_cast<size_t>(ch - 1)]) : -1;
    }
};

} // namespace

static void testTripleBuffer() {
    TripleBuffer<std::vector<int>> tb;

    tb.writeSlot() = {1, 2, 3};
    tb.publish();
    CHECK_EQ(tb.acquire().size(), size_t(3));
    CHECK_EQ(tb.acquire()[0], 1);

    // The writer never reuses the slot the reader holds, so the reader's
    // reference stays valid while the writer races ahead.
    const std::vector<int>& held = tb.acquire();
    for (int i = 0; i < 10; ++i) {
        tb.writeSlot() = {i, i, i};
        tb.publish();
    }
    CHECK_EQ(held.size(), size_t(3));
    CHECK_EQ(held[0], 1);        // still the version we acquired
    CHECK_EQ(tb.acquire()[0], 9);  // and the newest is there when we ask

    // Version advances on publish, so a consumer can skip redundant work.
    const uint64_t v = tb.version();
    CHECK(!tb.fresh(v));
    tb.writeSlot() = {42};
    tb.publish();
    CHECK(tb.fresh(v));
}

static void testProgrammerOverPlayback() {
    Rig r;
    r.seq(1, 1, Attr::Dimmer, 0.4f);
    Executor& e = r.exec(1, 1);
    e.go(*r.eng.sequence(1), r.eng.patch, 0.0);

    r.eng.tick();
    CHECK_NEAR(r.out(1, Attr::Dimmer), 0.4);

    // The programmer always wins over a playback, unconditionally (§11.4).
    r.eng.programmer.selection().set({{1, 0}});
    r.eng.programmer.setAttr(Attr::Dimmer, 0.1f);
    r.eng.tick();
    CHECK_NEAR(r.out(1, Attr::Dimmer), 0.1);  // lower, and still wins

    // Clearing it hands the attribute back to the playback. Intensity is HTP,
    // so it returns to the playback level rather than holding (§11.9).
    r.eng.programmer.clearAll();
    r.eng.tick();
    CHECK_NEAR(r.out(1, Attr::Dimmer), 0.4);
}

static void testHtpLtpMerge() {
    Rig r;
    r.seq(1, 1, Attr::Dimmer, 0.8f);
    r.seq(2, 1, Attr::Dimmer, 0.3f);
    Executor& a = r.exec(1, 1);
    Executor& b = r.exec(2, 2);
    a.go(*r.eng.sequence(1), r.eng.patch, 0.0);
    b.go(*r.eng.sequence(2), r.eng.patch, 0.0);

    // Intensity is HTP by kind, so the higher value wins regardless of order.
    r.eng.tick();
    CHECK_NEAR(r.out(1, Attr::Dimmer), 0.8);

    // Position is LTP, so the later executor wins.
    Rig p;
    p.seq(1, 1, Attr::Red, 0.8f);
    p.seq(2, 1, Attr::Red, 0.3f);
    Executor& pa = p.exec(1, 1);
    Executor& pb = p.exec(2, 2);
    pa.go(*p.eng.sequence(1), p.eng.patch, 0.0);
    pb.go(*p.eng.sequence(2), p.eng.patch, 0.0);
    p.eng.tick();
    CHECK_NEAR(p.out(1, Attr::Red), 0.3);  // executor 2 is later

    // A per-playback override flips that: making executor 2 HTP on Colour
    // means the higher value wins there too (§11.4).
    pb.mergeByGroup[static_cast<size_t>(Group::Color)] = MergeRule::HTP;
    p.eng.tick();
    CHECK_NEAR(p.out(1, Attr::Red), 0.8);
}

static void testPriority() {
    Rig r;
    r.seq(1, 1, Attr::Red, 0.2f);
    r.seq(2, 1, Attr::Red, 0.9f);
    Executor& a = r.exec(1, 1);
    Executor& b = r.exec(2, 2);
    a.go(*r.eng.sequence(1), r.eng.patch, 0.0);
    b.go(*r.eng.sequence(2), r.eng.patch, 0.0);

    // By id order, executor 2 is later and wins on LTP.
    r.eng.tick();
    CHECK_NEAR(r.out(1, Attr::Red), 0.9);

    // Priority breaks ties regardless of id: raising executor 1 above 2 makes
    // it merge last, so it wins (§11.4).
    a.priority = 10;
    r.eng.tick();
    CHECK_NEAR(r.out(1, Attr::Red), 0.2);

    // Assert forces a win even against higher priority.
    b.assert_ = true;
    r.eng.tick();
    CHECK_NEAR(r.out(1, Attr::Red), 0.9);
}

static void testSwapGroups() {
    Rig r;
    r.seq(1, 1, Attr::Pan, 0.2f);
    r.seq(2, 1, Attr::Pan, 0.8f);
    Executor& a = r.exec(1, 1);
    Executor& b = r.exec(2, 2);
    a.swapGroup = 1;
    b.swapGroup = 1;
    a.releaseTime = 0.0;

    a.go(*r.eng.sequence(1), r.eng.patch, 0.0);
    r.eng.tick();
    CHECK(a.active());

    // §11.4: playbacks in one swap group release each other, so only one
    // position look runs at a time.
    b.go(*r.eng.sequence(2), r.eng.patch, 0.0);
    r.eng.tick();
    CHECK(a.releasing());
    CHECK(!b.releasing());

    // A different swap group is unaffected.
    Rig s;
    s.seq(1, 1, Attr::Pan, 0.2f);
    s.seq(2, 1, Attr::Pan, 0.8f);
    Executor& sa = s.exec(1, 1);
    Executor& sb = s.exec(2, 2);
    sa.swapGroup = 1;
    sb.swapGroup = 2;
    sa.go(*s.eng.sequence(1), s.eng.patch, 0.0);
    sb.go(*s.eng.sequence(2), s.eng.patch, 0.0);
    s.eng.tick();
    CHECK(!sa.releasing());
}

static void testReleaseModes() {
    Rig r;
    r.seq(1, 1, Attr::Dimmer, 1.0f);
    Executor& e = r.exec(1, 1);
    e.releaseTime = 2.0;
    e.go(*r.eng.sequence(1), r.eng.patch, 0.0);
    r.eng.tick();
    CHECK_NEAR(r.out(1, Attr::Dimmer), 1.0);

    // ToZero fades the contribution out over the release time.
    e.release = ReleaseMode::ToZero;
    e.releaseNow(0.0);
    r.eng.setClock(1.0);
    r.eng.tick();
    CHECK_NEAR(r.out(1, Attr::Dimmer), 0.5);  // halfway through a 2s release
    r.eng.setClock(2.0);
    r.eng.tick();
    CHECK_NEAR(r.out(1, Attr::Dimmer), 0.0);

    // HoldLast keeps the value where it is instead of fading.
    Rig h;
    h.seq(1, 1, Attr::Pan, 0.7f);
    Executor& he = h.exec(1, 1);
    he.release = ReleaseMode::HoldLast;
    he.releaseTime = 2.0;
    he.go(*h.eng.sequence(1), h.eng.patch, 0.0);
    h.eng.tick();
    he.releaseNow(0.0);
    h.eng.setClock(1.0);
    h.eng.tick();
    CHECK_NEAR(h.out(1, Attr::Pan), 0.7);

    // Per-group override: intensity to zero, position held — the common
    // busking want (§11.3).
    Rig m;
    Sequence& ms = m.eng.ensureSequence(1);
    ms.cues.resize(1);
    ms.cues[0].timing.fadeIn = 0.0;
    ms.cues[0].values().set(1, 0, Attr::Dimmer, Value::hard(1.0f));
    ms.cues[0].values().set(1, 0, Attr::Pan, Value::hard(0.7f));
    Executor& me = m.exec(1, 1);
    me.release = ReleaseMode::HoldLast;
    me.releaseByGroup[static_cast<size_t>(Group::Intensity)] = ReleaseMode::ToZero;
    me.releaseTime = 2.0;
    me.go(ms, m.eng.patch, 0.0);
    m.eng.tick();
    me.releaseNow(0.0);
    m.eng.setClock(2.0);
    m.eng.tick();
    CHECK_NEAR(m.out(1, Attr::Dimmer), 0.0);  // intensity gone
    CHECK_NEAR(m.out(1, Attr::Pan), 0.7);     // position held
}

static void testUnusedAttributeBehaviour() {
    // §11.9: "unused" means not used by the PROGRAMMER. HTP always returns to
    // default; LTP holds its last value unless the setting says otherwise.
    Rig r;
    r.eng.unusedMode = UnusedMode::HoldLast;
    r.eng.programmer.selection().set({{1, 0}});
    r.eng.programmer.setAttr(Attr::Dimmer, 1.0f);
    r.eng.programmer.setAttr(Attr::Red, 0.6f);
    r.eng.tick();
    CHECK_NEAR(r.out(1, Attr::Dimmer), 1.0);
    CHECK_NEAR(r.out(1, Attr::Red), 0.6);

    // Clearing the programmer: intensity drops out, colour stays. This is the
    // behaviour people expect, and the reason LTP means what it means.
    r.eng.programmer.clearAll();
    r.eng.tick();
    CHECK_NEAR(r.out(1, Attr::Dimmer), -1.0);  // released; falls to default
    CHECK_NEAR(r.out(1, Attr::Red), 0.6);      // held

    // It keeps holding across further ticks rather than decaying.
    r.eng.setClock(5.0);
    r.eng.tick();
    CHECK_NEAR(r.out(1, Attr::Red), 0.6);

    // With the setting on, LTP returns to default too.
    Rig d;
    d.eng.unusedMode = UnusedMode::ToDefault;
    d.eng.programmer.selection().set({{1, 0}});
    d.eng.programmer.setAttr(Attr::Red, 0.6f);
    d.eng.tick();
    CHECK_NEAR(d.out(1, Attr::Red), 0.6);
    d.eng.programmer.clearAll();
    d.eng.tick();
    CHECK_NEAR(d.out(1, Attr::Red), -1.0);

    // Per-group override, so "intensity to zero but colour holds" is possible
    // independently of the global setting.
    Rig g;
    g.eng.unusedMode = UnusedMode::ToDefault;
    g.eng.unusedByGroup[static_cast<size_t>(Group::Color)] = UnusedMode::HoldLast;
    g.eng.programmer.selection().set({{1, 0}});
    g.eng.programmer.setAttr(Attr::Red, 0.6f);
    g.eng.tick();
    g.eng.programmer.clearAll();
    g.eng.tick();
    CHECK_NEAR(g.out(1, Attr::Red), 0.6);

    // A playback claiming the attribute takes precedence over retention —
    // playback levels are unaffected by any of this.
    Rig p;
    p.seq(1, 1, Attr::Red, 0.25f);
    Executor& pe = p.exec(1, 1);
    pe.go(*p.eng.sequence(1), p.eng.patch, 0.0);
    p.eng.programmer.selection().set({{1, 0}});
    p.eng.programmer.setAttr(Attr::Red, 0.9f);
    p.eng.tick();
    CHECK_NEAR(p.out(1, Attr::Red), 0.9);
    p.eng.programmer.clearAll();
    p.eng.tick();
    CHECK_NEAR(p.out(1, Attr::Red), 0.25);  // the playback, not the held value
}

static void testHighlightNotRetained() {
    Rig r;
    r.eng.programmer.selection().set({{1, 0}});
    r.eng.programmer.highlight = true;
    r.eng.tick();
    CHECK_NEAR(r.out(1, Attr::Dimmer), 1.0);

    // Highlight must not leak into the retained layer, or switching it off
    // would leave the highlight stuck on the rig.
    r.eng.programmer.highlight = false;
    r.eng.tick();
    CHECK_NEAR(r.out(1, Attr::Dimmer), -1.0);
    CHECK_NEAR(r.out(1, Attr::Red), -1.0);

    // And it never reaches the programmer, so Store cannot capture it.
    CHECK_EQ(r.eng.programmer.values().size(), size_t(0));
    CHECK_EQ(r.eng.programmer.storeLayer().size(), size_t(0));
}

static void testGrandMasterAndBlackout() {
    Rig r;
    r.eng.programmer.selection().set({{1, 0}});
    r.eng.programmer.setAttr(Attr::Dimmer, 1.0f);
    r.eng.programmer.setAttr(Attr::Red, 1.0f);
    r.eng.tick();
    CHECK_EQ(r.dmx(1, 1), 255);  // dimmer
    CHECK_EQ(r.dmx(1, 2), 255);  // red

    // The grand master scales intensity only — colour is untouched.
    r.eng.grandMaster = 0.5f;
    r.eng.tick();
    CHECK_EQ(r.dmx(1, 1), 128);
    CHECK_EQ(r.dmx(1, 2), 255);

    // Blackout zeroes intensity, again leaving colour alone.
    r.eng.blackout = true;
    r.eng.tick();
    CHECK_EQ(r.dmx(1, 1), 0);
    CHECK_EQ(r.dmx(1, 2), 255);

    r.eng.blackout = false;
    r.eng.grandMaster = 1.0f;
    r.eng.tick();
    CHECK_EQ(r.dmx(1, 1), 255);
}

static void testDmxRender() {
    Rig r;
    // Fixture 2 sits at address 7, so its dimmer is channel 7.
    CHECK_EQ(r.eng.patch.find(2)->address, 7);

    r.eng.programmer.selection().set({{2, 0}});
    r.eng.programmer.setAttr(Attr::Dimmer, 1.0f);
    r.eng.programmer.setAttr(Attr::Blue, 0.5f);
    r.eng.tick();
    CHECK_EQ(r.dmx(1, 7), 255);   // dimmer
    CHECK_EQ(r.dmx(1, 10), 128);  // blue is offset 3
    CHECK_EQ(r.dmx(1, 1), 0);     // fixture 1 untouched

    // 16-bit channels split across coarse and fine.
    Engine e16;
    installStarterTypes(e16.patch);
    std::string err;
    CHECK(e16.patch.add({1, "", e16.patch.findType("Moving Spot"), 1, 1}, &err));
    e16.setClock(0.0);
    e16.syncUniverses();
    e16.programmer.selection().set({{1, 0}});
    e16.programmer.setAttr(Attr::Pan, 0.5f);
    e16.tick();
    const Universe* u = e16.universe(1);
    const int coarse = u->data[0];
    const int fine = u->data[1];
    // 0.5 * 65535 = 32767.5, which rounds to 32768 = 0x8000.
    CHECK_EQ(coarse, 128);
    CHECK_EQ(fine, 0);
    const int combined = coarse * 256 + fine;
    CHECK(std::abs(combined - 32768) <= 1);

    // Full and zero must hit the rails exactly, which is what a 16-bit
    // attribute is for.
    e16.programmer.setAttr(Attr::Pan, 1.0f);
    e16.tick();
    CHECK_EQ(int(e16.universe(1)->data[0]), 255);
    CHECK_EQ(int(e16.universe(1)->data[1]), 255);
    e16.programmer.setAttr(Attr::Pan, 0.0f);
    e16.tick();
    CHECK_EQ(int(e16.universe(1)->data[0]), 0);
    CHECK_EQ(int(e16.universe(1)->data[1]), 0);
}

static void testTrackingThroughExecutor() {
    // An executor must render the TRACKED state, not the cue's own layer.
    Engine e;
    installStarterTypes(e.patch);
    std::string err;
    e.patch.addRange(1, 2, e.patch.findType("LED Par RGBW"), 1, 1, 0, &err);
    e.setClock(0.0);
    e.syncUniverses();

    Sequence& s = e.ensureSequence(1);
    s.cues.resize(2);
    for (auto& c : s.cues) c.timing.fadeIn = 0.0;
    s.cues[0].values().set(1, 0, Attr::Dimmer, Value::hard(1.0f));
    s.cues[0].values().set(1, 0, Attr::Red, Value::hard(1.0f));
    s.cues[1].values().set(1, 0, Attr::Dimmer, Value::hard(0.5f));

    Executor& ex = e.ensureExecutor(1);
    ex.sequenceId = 1;
    ex.gotoCue(s, e.patch, 1, 0.0);
    e.tick();

    // Cue 2 stored only the dimmer, but Red must still be on stage.
    CHECK_NEAR(*e.merged().get(makeKey(1, 0, Attr::Dimmer)), 0.5);
    CHECK_NEAR(*e.merged().get(makeKey(1, 0, Attr::Red)), 1.0);
    CHECK_EQ(s.cues[1].values().size(), size_t(1));
}

static void testOutputThread() {
    Rig r;
    r.eng.clearClock();
    r.eng.programmer.selection().set({{1, 0}});
    r.eng.programmer.setAttr(Attr::Dimmer, 1.0f);

    // The output thread reads published frames and never blocks the control
    // thread, so ticking while it runs must be safe (§15.2).
    r.eng.startOutput(200.0);
    CHECK(r.eng.outputRunning());
    for (int i = 0; i < 200; ++i) {
        r.eng.tick();
        std::this_thread::yield();
    }
    r.eng.stopOutput();
    CHECK(!r.eng.outputRunning());
    CHECK(r.eng.frameCount() >= 200);
    CHECK_EQ(r.dmx(1, 1), 255);
}

int main() {
    std::printf("lumen engine\n");
    testTripleBuffer();
    testProgrammerOverPlayback();
    testHtpLtpMerge();
    testPriority();
    testSwapGroups();
    testReleaseModes();
    testUnusedAttributeBehaviour();
    testHighlightNotRetained();
    testGrandMasterAndBlackout();
    testDmxRender();
    testTrackingThroughExecutor();
    testOutputThread();
    return check::report("engine");
}
