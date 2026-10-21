// Tests for the FX three-tier model: role portability, masters, layers,
// and parameter presets.
#include "Check.hpp"

#include "lumen/Engine.hpp"
#include "lumen/Recipe.hpp"

#include <cmath>
#include <set>

using namespace lumen;

namespace {

// A rig with three different domains, so role resolution can be tested for
// what it actually claims: one recipe across all of them.
struct Rig {
    Engine eng;
    RecipeLibrary lib{eng.uids};

    Rig() {
        installStarterTypes(eng.patch);

        // A "laser zone" and a "video surface" patched as fixture types whose
        // axes are PosX/PosY rather than Pan/Tilt. §4.2's role candidates list
        // both, so the same slot resolves either way.
        FixtureType zone;
        zone.name = "Laser Zone";
        zone.channels = {{Attr::PosX, 0, 1}, {Attr::PosY, 2, 3},
                         {Attr::Dimmer, 4}, {Attr::Red, 5},
                         {Attr::Green, 6}, {Attr::Blue, 7}};
        eng.patch.addType(zone);

        FixtureType surface;
        surface.name = "Video Surface";
        surface.channels = {{Attr::PosX, 0}, {Attr::PosY, 1},
                            {Attr::Dimmer, 2}, {Attr::Zoom, 3}};
        eng.patch.addType(surface);

        std::string err;
        eng.patch.add({1, "", eng.patch.findType("Moving Spot"), 1, 1}, &err);
        eng.patch.add({2, "", eng.patch.findType("Laser Zone"), 1, 20}, &err);
        eng.patch.add({3, "", eng.patch.findType("Video Surface"), 1, 40}, &err);
        eng.patch.add({4, "", eng.patch.findType("Dimmer"), 1, 60}, &err);
        installStarterRecipes(lib);
        eng.setClock(0.0);
    }

    std::vector<Target> all() const { return {{1, 0}, {2, 0}, {3, 0}, {4, 0}}; }
};

const Phaser* phaserFor(const InstantiateReport& rep, Role role) {
    for (const auto& p : rep.phasers)
        if (p.role == role) return &p;
    return nullptr;
}

} // namespace

static void testCircleIsPortableAcrossDomains() {
    // The claim from §4.2, tested directly: ONE Circle recipe drives a moving
    // head, a laser zone and a video surface, because all three resolve the
    // same AxisX/AxisY roles.
    Rig r;
    const Recipe* circle = r.lib.byName("Circle");
    CHECK(circle != nullptr);
    CHECK_EQ(circle->slots.size(), size_t(2));
    CHECK(circle->slots[0].role == Role::AxisX);
    CHECK(circle->slots[1].role == Role::AxisY);
    // Neither slot names a concrete attribute — that is what makes it portable.
    CHECK(circle->slots[0].attr == Attr::Dimmer);  // the unused default

    const auto rep = instantiate(*circle, r.all(), r.eng.patch);
    CHECK_EQ(rep.slotsApplied, 2);

    const Phaser* x = phaserFor(rep, Role::AxisX);
    const Phaser* y = phaserFor(rep, Role::AxisY);
    CHECK(x != nullptr);
    CHECK(y != nullptr);

    // Each slot resolves per fixture to whatever that fixture provides.
    CHECK(x->resolveAttr(r.eng.patch, {1, 0}).value() == Attr::Pan);   // moving head
    CHECK(x->resolveAttr(r.eng.patch, {2, 0}).value() == Attr::PosX);  // laser zone
    CHECK(x->resolveAttr(r.eng.patch, {3, 0}).value() == Attr::PosX);  // video surface
    CHECK(y->resolveAttr(r.eng.patch, {1, 0}).value() == Attr::Tilt);
    CHECK(y->resolveAttr(r.eng.patch, {2, 0}).value() == Attr::PosY);

    // The plain dimmer has no axis, so it is dropped from THESE SLOTS only.
    CHECK(!x->resolveAttr(r.eng.patch, {4, 0}).has_value());
    CHECK_EQ(x->targets.size(), size_t(3));
    CHECK_EQ(rep.skipped.size(), size_t(2));  // one entry per slot
    CHECK_EQ(rep.skipped[0].second.front(), FixtureId(4));

    // And it genuinely draws a circle: cosine and sine a quarter cycle apart,
    // so x^2 + y^2 stays constant.
    SpreadTable spread;
    spread.buildFromTargets(x->targets, r.eng.patch, SpreadSource::Index);
    for (double t = 0.0; t < 2.0; t += 0.1) {
        const float vx = x->sampleAt({1, 0}, spread, t).wave;
        const float vy = y->sampleAt({1, 0}, spread, t).wave;
        CHECK_NEAR(vx * vx + vy * vy, 1.0);
    }
}

static void testSlotSkippedNotFixture() {
    // §4.2's decision: skip the SLOT for a fixture that cannot serve it, not
    // the fixture for the whole recipe. So an intensity slot on the same mixed
    // selection still reaches the plain dimmer.
    Rig r;

    Recipe mixed;
    mixed.name = "Mixed";
    Slot pos;
    pos.label = "X";
    pos.role = Role::AxisX;
    Slot dim;
    dim.label = "Dim";
    dim.role = Role::Intensity;
    mixed.slots = {pos, dim};

    const auto rep = instantiate(mixed, r.all(), r.eng.patch);
    CHECK_EQ(rep.slotsApplied, 2);

    const Phaser* x = phaserFor(rep, Role::AxisX);
    const Phaser* d = phaserFor(rep, Role::Intensity);
    CHECK_EQ(x->targets.size(), size_t(3));  // the dimmer dropped out
    CHECK_EQ(d->targets.size(), size_t(4));  // but it is in the intensity slot

    // A slot NO fixture can serve is reported rather than silently vanishing.
    Recipe impossible;
    impossible.name = "Impossible";
    Slot shaper;
    shaper.label = "Shaper";
    shaper.attr = Attr::Shaper3A;
    impossible.slots = {shaper};
    const auto rep2 = instantiate(impossible, r.all(), r.eng.patch);
    CHECK_EQ(rep2.slotsApplied, 0);
    CHECK_EQ(rep2.slotsEmpty, 1);
    CHECK_EQ(rep2.phasers.size(), size_t(0));
}

static void testMastersScaleEverySlot() {
    Rig r;
    Recipe fly = *r.lib.byName("Fly");

    const auto base = instantiate(fly, {{1, 0}}, r.eng.patch);
    CHECK_EQ(base.phasers.size(), size_t(2));
    const float baseX = base.phasers[0].speedHz;
    const float baseY = base.phasers[1].speedHz;

    // The X slot runs at half the Y rate, expressed as a slot multiplier so
    // the recipe's speed master stays meaningful.
    CHECK_NEAR(baseX, baseY * 0.5);

    // §4.4: one master re-scales every slot at once, preserving their ratio.
    fly.speed = 3.f;
    const auto fast = instantiate(fly, {{1, 0}}, r.eng.patch);
    CHECK_NEAR(fast.phasers[0].speedHz, baseX * 3.0);
    CHECK_NEAR(fast.phasers[1].speedHz, baseY * 3.0);
    CHECK_NEAR(fast.phasers[0].speedHz, fast.phasers[1].speedHz * 0.5);

    // Size likewise, keeping each slot's own proportion.
    fly.size = 0.5f;
    const auto small = instantiate(fly, {{1, 0}}, r.eng.patch);
    CHECK_NEAR(small.phasers[0].size, 0.5);          // sizeMul 1.0
    CHECK_NEAR(small.phasers[1].size, 0.5 * 0.6);    // sizeMul 0.6

    // Spread scales the phase distribution; offset rotates the whole thing.
    fly.spread = 2.f;
    fly.offset = 0.25f;
    const auto spread = instantiate(fly, {{1, 0}}, r.eng.patch);
    CHECK_NEAR(spread.phasers[0].mods.groups, 2.0);
    CHECK_NEAR(spread.phasers[0].mods.offset, 0.25);
}

static void testLayersMuteAndSolo() {
    Rig r;

    Recipe stacked;
    stacked.name = "Stacked";
    Slot drift;
    drift.label = "Drift";
    drift.role = Role::Intensity;
    drift.speedMul = 0.2f;
    drift.layer = 0;
    Slot flicker;
    flicker.label = "Flicker";
    flicker.role = Role::Intensity;
    flicker.speedMul = 8.f;
    flicker.layer = 1;
    flicker.stack = StackMode::Add;
    stacked.slots = {drift, flicker};

    // §4.7: both target the same attribute and both instantiate — they stay
    // separate objects rather than merging into one unreadable phaser.
    auto rep = instantiate(stacked, {{1, 0}}, r.eng.patch);
    CHECK_EQ(rep.slotsApplied, 2);
    CHECK(rep.phasers[1].stack == StackMode::Add);

    // Muting a layer drops it.
    stacked.slots[1].muted = true;
    rep = instantiate(stacked, {{1, 0}}, r.eng.patch);
    CHECK_EQ(rep.slotsApplied, 1);
    CHECK_NEAR(rep.phasers[0].speedHz, stacked.baseHz * 0.2);

    // Solo on one layer mutes the others, which is the only practical way to
    // work on one layer of a multi-layer effect.
    stacked.slots[1].muted = false;
    stacked.slots[1].soloed = true;
    rep = instantiate(stacked, {{1, 0}}, r.eng.patch);
    CHECK_EQ(rep.slotsApplied, 1);
    CHECK_NEAR(rep.phasers[0].speedHz, stacked.baseHz * 8.0);

    // Opacity scales a layer's contribution without changing the recipe size.
    stacked.slots[1].opacity = 0.25f;
    rep = instantiate(stacked, {{1, 0}}, r.eng.patch);
    CHECK_NEAR(rep.phasers[0].size, 0.25);
}

static void testColourRecipeUsesHue() {
    // §4.10: an FX on R/G/B independently crosses through grey. Rainbow drives
    // Hue instead, which goes round the wheel.
    Rig r;
    const Recipe* rainbow = r.lib.byName("Rainbow");
    CHECK(rainbow != nullptr);
    CHECK_EQ(rainbow->slots.size(), size_t(1));
    CHECK(rainbow->slots[0].attr == Attr::Hue);
    CHECK(isVirtual(Attr::Hue));  // no new mechanism needed
    CHECK(!rainbow->slots[0].relative);  // absolute: it sweeps the wheel

    // A virtual attribute resolves even though no fixture has a Hue channel,
    // which is what makes this work at all.
    const auto rep = instantiate(*rainbow, r.all(), r.eng.patch);
    CHECK_EQ(rep.slotsApplied, 1);
    CHECK_EQ(rep.phasers[0].targets.size(), size_t(4));
}

static void testParamsRoundTrip() {
    Rig r;
    Recipe circle = *r.lib.byName("Circle");

    // §4.17: a parameter preset holds STATE, not structure.
    FxParams p = paramsOf(circle);
    CHECK_NEAR(p.size, 1.0);
    CHECK_NEAR(p.speed, 1.0);

    p.size = 0.4f;
    p.speed = 2.f;
    p.rate = 1.5f;
    p.spread = 3.f;
    p.direction = Direction::Bounce;
    p.selection = SpreadSource::StageAngle;
    p.width = 0.3f;

    std::vector<std::string> skipped;
    applyParams(circle, p, &skipped);
    CHECK(skipped.empty());  // both slots use waveforms

    CHECK_NEAR(circle.size, 0.4);
    CHECK_NEAR(circle.speed, 3.0);  // speed * rate
    CHECK_NEAR(circle.spread, 3.0);
    for (const Slot& s : circle.slots) {
        CHECK(s.direction == Direction::Bounce);
        CHECK(s.source == SpreadSource::StageAngle);
        CHECK_NEAR(s.gen.duty, 0.3);
    }

    // The same parameter preset applies to a DIFFERENT recipe — which is the
    // point of keeping state separate from structure.
    Recipe sine = *r.lib.byName("Sine Dim");
    applyParams(sine, p, &skipped);
    CHECK_NEAR(sine.size, 0.4);
    CHECK_NEAR(sine.speed, 3.0);
}

static void testParamsSkipsWhatItCannotApply() {
    Rig r;

    // A hand-drawn curve must not be silently rewritten into a sine by a
    // parameter recall (§4.17).
    Recipe drawn;
    drawn.name = "Drawn";
    Slot s;
    s.label = "Curve";
    s.role = Role::Intensity;
    s.gen.kind = GenKind::Curve;
    s.gen.curve = {{0.f, 0.f}, {0.5f, 1.f}, {1.f, 0.f}};
    drawn.slots = {s};

    FxParams p;
    p.form = Wave::Square;

    std::vector<std::string> skipped;
    applyParams(drawn, p, &skipped);

    CHECK_EQ(skipped.size(), size_t(1));
    CHECK(skipped[0] == "Curve");
    CHECK(drawn.slots[0].gen.kind == GenKind::Curve);          // untouched
    CHECK_EQ(drawn.slots[0].gen.curve.size(), size_t(3));

    // Everything else still applied.
    p.size = 0.2f;
    applyParams(drawn, p, nullptr);
    CHECK_NEAR(drawn.size, 0.2);
}

static void testParamMorph() {
    FxParams a;
    a.size = 0.f;
    a.speed = 1.f;
    a.direction = Direction::Forward;
    a.selection = SpreadSource::Index;

    FxParams b;
    b.size = 1.f;
    b.speed = 5.f;
    b.direction = Direction::Reverse;
    b.selection = SpreadSource::StageAngle;

    // Numeric controls interpolate continuously.
    const FxParams mid = FxParams::morph(a, b, 0.5f);
    CHECK_NEAR(mid.size, 0.5);
    CHECK_NEAR(mid.speed, 3.0);

    const FxParams quarter = FxParams::morph(a, b, 0.25f);
    CHECK_NEAR(quarter.size, 0.25);
    CHECK_NEAR(quarter.speed, 2.0);

    // Discrete ones switch at the midpoint, because there is no halfway
    // between Forward and Reverse (§4.17).
    CHECK(FxParams::morph(a, b, 0.49f).direction == Direction::Forward);
    CHECK(FxParams::morph(a, b, 0.50f).direction == Direction::Reverse);
    CHECK(FxParams::morph(a, b, 0.49f).selection == SpreadSource::Index);
    CHECK(FxParams::morph(a, b, 0.51f).selection == SpreadSource::StageAngle);

    // Endpoints are exact, so a morph fader parks cleanly at either end.
    CHECK_NEAR(FxParams::morph(a, b, 0.f).size, 0.0);
    CHECK_NEAR(FxParams::morph(a, b, 1.f).size, 1.0);
    CHECK_NEAR(FxParams::morph(a, b, -5.f).size, 0.0);  // clamped
    CHECK_NEAR(FxParams::morph(a, b, 9.f).size, 1.0);
}

static void testLibrary() {
    Rig r;
    CHECK_EQ(r.lib.size(), size_t(4));
    CHECK(r.lib.byName("Circle") != nullptr);
    CHECK(r.lib.byName("Nope") == nullptr);

    // UIDs and ids are assigned on add, so references survive renaming (§8.1).
    const Uid uid = r.lib.byName("Circle")->uid;
    CHECK(uid.valid());
    CHECK(r.lib.find(uid) != nullptr);
    r.lib.byName("Circle")->name = "Orbit";
    CHECK(r.lib.find(uid) != nullptr);          // reference still resolves
    CHECK(r.lib.find(uid)->name == "Orbit");
    CHECK(r.lib.byName("Circle") == nullptr);

    std::set<int> ids;
    for (const auto& rec : r.lib.all()) ids.insert(rec.id);
    CHECK_EQ(ids.size(), size_t(4));

    ParamPreset fast;
    fast.name = "Fast and wide";
    fast.params.speed = 4.f;
    fast.params.size = 1.f;
    fast.fadeTime = 2.0;
    const Uid pu = r.lib.addParams(fast).uid;
    CHECK(r.lib.findParams(pu) != nullptr);
    CHECK_NEAR(r.lib.findParams(pu)->params.speed, 4.0);
    CHECK_NEAR(r.lib.findParams(pu)->fadeTime, 2.0);
}

static void testRecipeDrivesOutput() {
    // End to end: instantiate a recipe into the programmer and check it
    // reaches the output through the merge pipeline.
    Rig r;
    Recipe sine = *r.lib.byName("Sine Dim");
    sine.size = 1.f;

    r.eng.programmer.selection().set({{1, 0}, {4, 0}});
    r.eng.programmer.setAttr(Attr::Dimmer, 0.5f);

    const auto rep = instantiate(sine, r.eng.programmer.selection().flatten(),
                                 r.eng.patch);
    CHECK_EQ(rep.slotsApplied, 1);
    for (const Phaser& p : rep.phasers) r.eng.programmer.addPhaser(p);

    // At a quarter cycle a relative sine is at its positive peak, so the
    // dimmer sits above the 50% base it is centred on.
    r.eng.setClock(0.25);
    r.eng.tick();
    const float v = *r.eng.merged().get(makeKey(1, 0, Attr::Dimmer));
    CHECK(v > 0.5f);

    // Three quarters of the way round it is below the base.
    r.eng.setClock(0.75);
    r.eng.tick();
    CHECK(*r.eng.merged().get(makeKey(1, 0, Attr::Dimmer)) < 0.5f);

    // Both fixtures are driven, since Intensity resolves on both.
    CHECK(r.eng.merged().get(makeKey(4, 0, Attr::Dimmer)).has_value());

    // Clear removes the phasers along with the values.
    r.eng.programmer.clearAll();
    CHECK_EQ(r.eng.programmer.phasers().size(), size_t(0));
}

int main() {
    std::printf("lumen recipe\n");
    testCircleIsPortableAcrossDomains();
    testSlotSkippedNotFixture();
    testMastersScaleEverySlot();
    testLayersMuteAndSolo();
    testColourRecipeUsesHue();
    testParamsRoundTrip();
    testParamsSkipsWhatItCannotApply();
    testParamMorph();
    testLibrary();
    testRecipeDrivesOutput();
    return check::report("recipe");
}
