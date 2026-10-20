// Tests for the FX primitive: generators, travel, bounce, gap, variation.
#include "Check.hpp"

#include "lumen/Patch.hpp"
#include "lumen/Phaser.hpp"
#include "lumen/Selection.hpp"

#include <cmath>
#include <set>

using namespace lumen;

namespace {

struct Rig {
    UidGen gen{99};
    Patch patch{gen};
    Selection sel;
    SpreadTable spread;

    explicit Rig(int count = 8) {
        installStarterTypes(patch);
        std::string err;
        patch.addRange(1, count, patch.findType("Moving Spot"), 1, 1, 0, &err);
        std::vector<Target> ts;
        for (int i = 0; i < count; ++i) ts.push_back({static_cast<FixtureId>(i + 1), 0});
        sel.set(ts);
        spread.build(sel, patch, SpreadSource::Index);
    }
};

} // namespace

static void testWaveforms() {
    Generator g;
    g.kind = GenKind::Waveform;

    g.wave = Wave::Sine;
    CHECK_NEAR(g.eval(0.00f), 0.0);
    CHECK_NEAR(g.eval(0.25f), 1.0);
    CHECK_NEAR(g.eval(0.50f), 0.0);
    CHECK_NEAR(g.eval(0.75f), -1.0);
    CHECK_NEAR(g.eval(1.00f), 0.0);   // cyclic

    g.wave = Wave::Cosine;
    CHECK_NEAR(g.eval(0.0f), 1.0);
    CHECK_NEAR(g.eval(0.5f), -1.0);

    g.wave = Wave::Ramp;
    CHECK_NEAR(g.eval(0.0f), -1.0);
    CHECK_NEAR(g.eval(0.5f), 0.0);

    g.wave = Wave::SawDown;
    CHECK_NEAR(g.eval(0.0f), 1.0);
    CHECK_NEAR(g.eval(0.5f), 0.0);

    g.wave = Wave::Triangle;
    CHECK_NEAR(g.eval(0.0f), -1.0);
    CHECK_NEAR(g.eval(0.5f), 1.0);
    CHECK_NEAR(g.eval(0.25f), 0.0);

    g.wave = Wave::Square;
    CHECK_NEAR(g.eval(0.25f), 1.0);
    CHECK_NEAR(g.eval(0.75f), -1.0);

    // PWM duty is the live "Width" control (§4.17).
    g.wave = Wave::PWM;
    g.duty = 0.25f;
    CHECK_NEAR(g.eval(0.1f), 1.0);
    CHECK_NEAR(g.eval(0.4f), -1.0);

    // Noise holds per cycle rather than per sample, so it reads as a step
    // sequence rather than hiss — and is deterministic.
    g.wave = Wave::Noise;
    CHECK_NEAR(g.eval(0.1f), g.eval(0.9f));          // same cycle, same value
    CHECK(std::fabs(g.eval(0.5f) - g.eval(1.5f)) > 1e-6);  // next cycle differs

    // Everything stays in range.
    for (size_t i = 0; i < static_cast<size_t>(Wave::Count); ++i) {
        g.wave = static_cast<Wave>(i);
        for (int k = 0; k <= 40; ++k) {
            const float v = g.eval(static_cast<float>(k) / 40.f);
            CHECK(v >= -1.0001f && v <= 1.0001f);
        }
    }
}

static void testStepGenerator() {
    Generator g;
    g.kind = GenKind::Steps;

    // A hard chase: three steps, full dwell, no transition.
    g.steps = {{0.f, 1.f}, {0.5f, 1.f}, {1.f, 1.f}};
    CHECK_NEAR(g.eval(0.10f), -1.0);   // 0.0 mapped to -1..1
    CHECK_NEAR(g.eval(0.45f), 0.0);    // 0.5
    CHECK_NEAR(g.eval(0.80f), 1.0);    // 1.0

    // Attack shapes the rise into a step. With attack 0.5 on the second step,
    // the first half of its slot interpolates from the previous value.
    Generator a;
    a.kind = GenKind::Steps;
    a.steps = {{0.f, 1.f}, {1.f, 1.f, 0.5f}};
    const float mid = a.eval(0.5f + 0.125f);  // quarter into step 2's attack
    CHECK(mid > -1.f && mid < 1.f);           // genuinely interpolating

    // §4.5: attack and decay are separate, not a symmetric ease. A fast
    // attack with a slow decay must not mirror.
    Generator s;
    s.kind = GenKind::Steps;
    s.steps = {{1.f, 0.2f, 0.05f, 0.7f}, {0.f, 1.f}};
    const float rising = s.eval(0.02f);
    const float falling = s.eval(0.35f);
    CHECK(std::fabs(rising - falling) > 1e-3);

    Generator empty;
    empty.kind = GenKind::Steps;
    CHECK_NEAR(empty.eval(0.5f), 0.0);  // no steps contributes nothing
}

static void testCurveGenerator() {
    Generator g;
    g.kind = GenKind::Curve;

    // A straight line from 0 to 1 with no handles must read linearly. Phase is
    // cyclic, so 1.0 wraps to 0.0 — a curve that does not return to its start
    // is intentionally discontinuous at the seam.
    g.curve = {{0.f, 0.f}, {1.f, 1.f}};
    CHECK_NEAR(g.eval(0.0f), -1.0);
    CHECK_NEAR(g.eval(0.5f), 0.0);
    CHECK_NEAR(g.eval(0.999f), 0.998);
    CHECK_NEAR(g.eval(1.0f), -1.0);  // wrapped back to the start

    // Handles bend it: pulling the out-handle right delays the rise.
    Generator b;
    b.kind = GenKind::Curve;
    b.curve = {{0.f, 0.f, 0.f, 0.f, 0.6f, 0.f}, {1.f, 1.f, -0.6f, 0.f, 0.f, 0.f}};
    CHECK(b.eval(0.5f) < 0.05f);        // still near the midpoint
    CHECK(b.eval(0.25f) < g.eval(0.25f));  // and behind the linear curve

    // A three-knot peak: up then down.
    Generator p;
    p.kind = GenKind::Curve;
    p.curve = {{0.f, 0.f}, {0.5f, 1.f}, {1.f, 0.f}};
    CHECK_NEAR(p.eval(0.5f), 1.0);
    CHECK(p.eval(0.25f) > p.eval(0.0f));
    CHECK(p.eval(0.75f) < p.eval(0.5f));

    // Stays in range everywhere.
    for (int k = 0; k <= 40; ++k) {
        const float v = p.eval(static_cast<float>(k) / 40.f);
        CHECK(v >= -1.0001f && v <= 1.0001f);
    }
}

static void testTravelAndBounce() {
    // Forward ramps, reverse ramps backwards.
    CHECK_NEAR(travelAt(0.0, Direction::Forward, BouncePivot::AtEnd, 1.f, 0.f, 1), 0.0);
    CHECK_NEAR(travelAt(2.5, Direction::Forward, BouncePivot::AtEnd, 1.f, 0.f, 1), 2.5);
    CHECK_NEAR(travelAt(2.5, Direction::Reverse, BouncePivot::AtEnd, 1.f, 0.f, 1), -2.5);

    // §4.13: bounce triangle-waves, and the PIVOT sets the amplitude — where
    // the wave reverses relative to the ends of the spread.
    const float atEnd = travelAt(1.0, Direction::Bounce, BouncePivot::AtEnd, 1.f, 0.f, 1);
    const float centre = travelAt(1.0, Direction::Bounce, BouncePivot::CentreAtEnd, 1.f, 0.f, 1);
    const float over = travelAt(1.0, Direction::Bounce, BouncePivot::OverEnd, 1.f, 0.25f, 1);

    CHECK_NEAR(atEnd, 1.0);    // leading edge reaches the last member
    CHECK_NEAR(centre, 1.5);   // midpoint reaches it: half the wave overshoots
    CHECK_NEAR(over, 2.25);    // whole wave passes, plus the overshoot
    CHECK(centre > atEnd);
    CHECK(over > centre);

    // It returns: a full out-and-back is two cycles of travel.
    CHECK_NEAR(travelAt(0.0, Direction::Bounce, BouncePivot::AtEnd, 1.f, 0.f, 1), 0.0);
    CHECK_NEAR(travelAt(2.0, Direction::Bounce, BouncePivot::AtEnd, 1.f, 0.f, 1), 0.0);
    CHECK_NEAR(travelAt(0.5, Direction::Bounce, BouncePivot::AtEnd, 1.f, 0.f, 1), 0.5);
    CHECK_NEAR(travelAt(1.5, Direction::Bounce, BouncePivot::AtEnd, 1.f, 0.f, 1), 0.5);

    // Narrower waves overshoot less, because the amplitude is derived from the
    // wave's own width.
    const float narrow = travelAt(1.0, Direction::Bounce, BouncePivot::CentreAtEnd,
                                  0.25f, 0.f, 1);
    CHECK_NEAR(narrow, 1.125);
    CHECK(narrow < centre);
}

static void testSpreadIntegration() {
    Rig rig(8);
    Phaser p;
    p.attr = Attr::Dimmer;
    p.gen.wave = Wave::Sine;
    p.speedHz = 1.f;
    p.size = 1.f;
    p.relative = false;
    p.centre = 0.5f;
    p.mods.groups = 1.f;

    for (int i = 0; i < 8; ++i) p.targets.push_back({static_cast<FixtureId>(i + 1), 0});

    // §4.8: adding a fixture to a running FX must not restart it. Phase comes
    // from the fixture's position in the spread, not from when it joined — so
    // at t=0 the eight fixtures sit at eight evenly spaced phases.
    // Eight evenly spread phases through a sine give five distinct outputs,
    // because a sine is symmetric: 0, +.707, +1, +.707, 0, -.707, -1, -.707.
    std::set<int> phases;
    for (int i = 0; i < 8; ++i) {
        const auto s = p.sampleAt({static_cast<FixtureId>(i + 1), 0}, rig.spread, 0.0);
        phases.insert(static_cast<int>(s.wave * 1000.f));
    }
    CHECK_EQ(phases.size(), size_t(5));

    // A ramp is monotonic in phase, so it exposes all eight distinct phases.
    Phaser ramp = p;
    ramp.gen.wave = Wave::Ramp;
    std::set<int> rampPhases;
    for (int i = 0; i < 8; ++i)
        rampPhases.insert(static_cast<int>(
            ramp.sampleAt({static_cast<FixtureId>(i + 1), 0}, rig.spread, 0.0).wave * 1000.f));
    CHECK_EQ(rampPhases.size(), size_t(8));

    // Fixture 1 sits at spread 0, so a sine starts at 0 and peaks a quarter
    // cycle later.
    const Target f1{1, 0};
    CHECK_NEAR(p.sampleAt(f1, rig.spread, 0.0).wave, 0.0);
    CHECK_NEAR(p.sampleAt(f1, rig.spread, 0.25).wave, 1.0);
    CHECK_NEAR(p.sampleAt(f1, rig.spread, 0.5).wave, 0.0);

    // Fixture 3 is a quarter of the way through the spread, so it leads by 90.
    CHECK_NEAR(p.sampleAt({3, 0}, rig.spread, 0.0).wave, 1.0);

    // Absolute mode drives the centre, ignoring the base.
    ValueLayer base, out;
    base.set(1, 0, Attr::Dimmer, Value::hard(0.9f));
    p.apply(rig.spread, 0.25, base, out, &rig.patch);
    CHECK_NEAR(*out.value(1, 0, Attr::Dimmer), 1.0);  // 0.5 centre + half of size 1

    // §4.6 relative mode offsets whatever the fixture is already doing.
    ValueLayer mid;
    mid.set(1, 0, Attr::Dimmer, Value::hard(0.5f));
    Phaser rel = p;
    rel.relative = true;
    rel.size = 0.4f;
    ValueLayer out2;
    rel.apply(rig.spread, 0.25, mid, out2, &rig.patch);
    CHECK_NEAR(*out2.value(1, 0, Attr::Dimmer), 0.7);  // 0.5 base + half of 0.4

    // Relative still clamps at the top of the range.
    ValueLayer out3;
    rel.apply(rig.spread, 0.25, base, out3, &rig.patch);
    CHECK_NEAR(*out3.value(1, 0, Attr::Dimmer), 1.0);  // 0.9 + 0.2 clipped
}

static void testRoleResolution() {
    UidGen gen{5};
    Patch patch{gen};
    installStarterTypes(patch);
    std::string err;
    CHECK(patch.add({1, "", patch.findType("Moving Spot"), 1, 1}, &err));
    CHECK(patch.add({2, "", patch.findType("Dimmer"), 2, 1}, &err));

    Selection sel;
    sel.set({{1, 0}, {2, 0}});
    SpreadTable spread;
    spread.build(sel, patch, SpreadSource::Index);

    // §4.2: a Circle recipe's AxisX slot resolves to Pan on the moving head.
    Phaser circleX;
    circleX.role = Role::AxisX;
    circleX.targets = {{1, 0}, {2, 0}};
    CHECK(circleX.resolveAttr(patch, {1, 0}).value() == Attr::Pan);

    // The plain dimmer has no axis attribute, so the SLOT is skipped for that
    // fixture — the fixture is not dropped from the recipe.
    CHECK(!circleX.resolveAttr(patch, {2, 0}).has_value());

    ValueLayer base, out;
    circleX.relative = false;
    circleX.apply(spread, 0.0, base, out, &patch);
    CHECK(out.has(1, 0, Attr::Pan));      // moving head got its pan
    CHECK_EQ(out.size(), size_t(1));      // the dimmer contributed nothing

    // An intensity slot on the same mixed selection still reaches both, which
    // is the point of skipping the slot rather than the fixture.
    Phaser inten;
    inten.role = Role::Intensity;
    inten.targets = {{1, 0}, {2, 0}};
    inten.relative = false;
    ValueLayer out2;
    inten.apply(spread, 0.0, base, out2, &patch);
    CHECK_EQ(out2.size(), size_t(2));
}

static void testGap() {
    Rig rig(4);
    Phaser p;
    p.attr = Attr::Dimmer;
    p.gen.wave = Wave::Sine;
    p.speedHz = 1.f;
    p.relative = false;
    p.targets = {{1, 0}};
    const Target f1{1, 0};

    // Pause: one cycle active, one cycle of gap, value falls to base.
    p.gap = gapFromBehaviour(GapBehaviour::Pause, 1.f);
    CHECK(p.gap.enabled);
    CHECK(!p.sampleAt(f1, rig.spread, 0.25).inGap);   // inside the active loop
    CHECK(p.sampleAt(f1, rig.spread, 1.25).inGap);    // inside the gap
    CHECK(p.sampleAt(f1, rig.spread, 1.25).silent);   // and contributing nothing
    CHECK(!p.sampleAt(f1, rig.spread, 2.25).inGap);   // active again

    // Freeze holds the cycle-end value instead of falling to base.
    p.gap = gapFromBehaviour(GapBehaviour::Freeze, 1.f);
    const auto frozen = p.sampleAt(f1, rig.spread, 1.5);
    CHECK(frozen.inGap);
    CHECK(!frozen.silent);

    // Burst: three active loops, then a gap (§4.15 every-Nth).
    p.gap = gapFromBehaviour(GapBehaviour::Burst, 1.f, 3);
    CHECK_EQ(p.gap.everyNth, 3);
    CHECK(!p.sampleAt(f1, rig.spread, 0.5).inGap);
    CHECK(!p.sampleAt(f1, rig.spread, 1.5).inGap);
    CHECK(!p.sampleAt(f1, rig.spread, 2.5).inGap);
    CHECK(p.sampleAt(f1, rig.spread, 3.5).inGap);     // the gap
    CHECK(!p.sampleAt(f1, rig.spread, 4.5).inGap);    // next burst

    // A sequence of lengths stops the repeat being metronomic.
    Gap seq;
    seq.enabled = true;
    seq.sequence = {1.f, 1.f, 2.f};
    CHECK_NEAR(seq.lengthForLoop(0), 1.0);
    CHECK_NEAR(seq.lengthForLoop(2), 2.0);
    CHECK_NEAR(seq.lengthForLoop(3), 1.0);   // cycles

    // Placement Before puts the gap at the start of the super-cycle.
    Phaser before = p;
    before.gap = gapFromBehaviour(GapBehaviour::Pause, 1.f);
    before.gap.placement = GapPlacement::Before;
    CHECK(before.sampleAt(f1, rig.spread, 0.25).inGap);
    CHECK(!before.sampleAt(f1, rig.spread, 1.25).inGap);

    // Drift continues the phase through the gap, so it never realigns; Pause
    // resets, so every loop restarts in the same place.
    //
    // This only diverges when the gap length is NOT a whole number of cycles:
    // with an integer gap the two differ by an integer number of cycles, which
    // is the same phase. A ramp is used because it is monotonic in phase —
    // a sine's symmetry would mask the difference.
    Phaser drift = p;
    drift.gen.wave = Wave::Ramp;
    drift.gap = gapFromBehaviour(GapBehaviour::Drift, 0.5f);
    Phaser reset = drift;
    reset.gap = gapFromBehaviour(GapBehaviour::Pause, 0.5f);

    const float driftW = drift.sampleAt(f1, rig.spread, 2.0).wave;
    const float resetW = reset.sampleAt(f1, rig.spread, 2.0).wave;
    CHECK(!drift.sampleAt(f1, rig.spread, 2.0).inGap);
    CHECK_NEAR(driftW, -1.0);  // phase ran through the gap: 2.0 cycles
    CHECK_NEAR(resetW, 0.0);   // phase paused during it: 1.5 cycles
    CHECK(std::fabs(driftW - resetW) > 1e-4);

    // With a whole-cycle gap the two coincide, which is correct rather than a
    // bug: the phases differ by exactly one cycle.
    Phaser driftInt = drift;
    driftInt.gap = gapFromBehaviour(GapBehaviour::Drift, 1.f);
    Phaser resetInt = drift;
    resetInt.gap = gapFromBehaviour(GapBehaviour::Pause, 1.f);
    CHECK_NEAR(driftInt.sampleAt(f1, rig.spread, 4.25).wave,
               resetInt.sampleAt(f1, rig.spread, 4.25).wave);

    // Disabled gap never reports a gap.
    Phaser none = p;
    none.gap = Gap{};
    for (int k = 0; k < 20; ++k)
        CHECK(!none.sampleAt(f1, rig.spread, k * 0.25).inGap);
}

static void testVariation() {
    Rig rig(8);
    Phaser p;
    p.attr = Attr::Dimmer;
    p.gen.wave = Wave::Sine;
    p.speedHz = 1.f;
    p.relative = false;
    p.mods.groups = 0.f;  // no spread, so only variation separates fixtures
    for (int i = 0; i < 8; ++i) p.targets.push_back({static_cast<FixtureId>(i + 1), 0});

    // With no spread and no variation, every fixture is in lockstep.
    std::set<int> same;
    for (int i = 0; i < 8; ++i)
        same.insert(static_cast<int>(
            p.sampleAt({static_cast<FixtureId>(i + 1), 0}, rig.spread, 0.3).wave * 10000.f));
    CHECK_EQ(same.size(), size_t(1));

    // §4.11: variation breaks the lockstep per fixture, which is what makes an
    // effect read as organic rather than mechanical.
    p.variation.phase = 0.2f;
    p.variation.seed = 777;
    std::set<int> varied;
    for (int i = 0; i < 8; ++i)
        varied.insert(static_cast<int>(
            p.sampleAt({static_cast<FixtureId>(i + 1), 0}, rig.spread, 0.3).wave * 10000.f));
    CHECK(varied.size() >= 6);

    // Seeded, so it replays identically — the show looks the same every night.
    for (int i = 0; i < 8; ++i) {
        const Target t{static_cast<FixtureId>(i + 1), 0};
        CHECK_NEAR(p.sampleAt(t, rig.spread, 0.3).wave,
                   p.sampleAt(t, rig.spread, 0.3).wave);
        CHECK_NEAR(targetNoise(t, 777, 1), targetNoise(t, 777, 1));
    }
    // Different seeds give different variation.
    CHECK(std::fabs(targetNoise({1, 0}, 777, 1) - targetNoise({1, 0}, 778, 1)) > 1e-6);
    // Different salts decorrelate phase, speed and size.
    CHECK(std::fabs(targetNoise({1, 0}, 777, 1) - targetNoise({1, 0}, 777, 2)) > 1e-6);
}

static void testFadeEnvelope() {
    Rig rig(4);
    Phaser p;
    p.attr = Attr::Dimmer;
    p.gen.wave = Wave::Sine;
    p.speedHz = 1.f;
    p.size = 1.f;
    p.relative = false;
    p.targets = {{1, 0}};

    // §4.9: fade scales size, so an effect arrives with its cue rather than
    // snapping in at full amplitude.
    p.fade = 1.f;
    const float full = p.sampleAt({1, 0}, rig.spread, 0.25).delta;
    p.fade = 0.5f;
    const float half = p.sampleAt({1, 0}, rig.spread, 0.25).delta;
    p.fade = 0.f;
    const float none = p.sampleAt({1, 0}, rig.spread, 0.25).delta;

    CHECK_NEAR(half, full * 0.5);
    CHECK_NEAR(none, 0.0);
}

int main() {
    std::printf("lumen phaser\n");
    testWaveforms();
    testStepGenerator();
    testCurveGenerator();
    testTravelAndBounce();
    testSpreadIntegration();
    testRoleResolution();
    testGap();
    testVariation();
    testFadeEnvelope();
    return check::report("phaser");
}
