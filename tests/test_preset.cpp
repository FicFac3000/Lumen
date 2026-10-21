// Tests for presets: live reference resolution, per-pool naming, and the
// auto-stepped generator — including the anchored-range case.
#include "Check.hpp"

#include "lumen/Engine.hpp"
#include "lumen/Preset.hpp"

#include <cmath>
#include <set>

using namespace lumen;

namespace {

struct Rig {
    Engine eng;
    PresetStore store{eng.uids};

    Rig() {
        installStarterTypes(eng.patch);
        std::string err;
        eng.patch.addRange(1, 4, eng.patch.findType("Moving Spot"), 1, 1, 0, &err);
        eng.setClock(0.0);
    }
    const FixtureType& spot() const { return *eng.patch.typeOf(1); }
};

int countSide(const std::vector<GeneratedStep>& v, int side) {
    int n = 0;
    for (const auto& s : v)
        if (s.side == side) ++n;
    return n;
}

} // namespace

static void testPoolNaming() {
    Rig r;
    PresetPool& pool = r.store.ensurePool(1, "Colour", Group::Color);
    CHECK_EQ(pool.id, 1);

    Preset a;
    a.name = "Warm";
    a.values.set(0, 0, Attr::Red, Value::hard(1.0f));
    const Uid ua = r.store.add(1, a).uid;

    // §8.2: names are unique WITHIN a pool, and a collision auto-suffixes
    // rather than being rejected — a bulk generation must never fail on one.
    Preset b;
    b.name = "Warm";
    b.values.set(0, 0, Attr::Red, Value::hard(0.8f));
    const Preset& added = r.store.add(1, b);
    CHECK(added.name == "Warm (2)");
    CHECK(added.uid != ua);
    CHECK_EQ(r.store.pool(1)->presets.size(), size_t(2));

    // Ids auto-increment within the pool.
    CHECK_EQ(r.store.pool(1)->presets[0].id, 1);
    CHECK_EQ(r.store.pool(1)->presets[1].id, 2);

    // The SAME name in a different pool is fine, and expected.
    r.store.ensurePool(2, "Position", Group::Position);
    Preset c;
    c.name = "Warm";
    c.values.set(0, 0, Attr::Pan, Value::hard(0.5f));
    CHECK(r.store.add(2, c).name == "Warm");
}

static void testNameLookupAmbiguity() {
    Rig r;
    r.store.ensurePool(1, "Colour", Group::Color);
    r.store.ensurePool(2, "ByType", Group::Color);

    Preset a;
    a.name = "Red";
    a.values.set(0, 0, Attr::Red, Value::hard(1.0f));
    r.store.add(1, a);

    // A unique bare name resolves.
    auto hit = r.store.findByName("Red");
    CHECK(hit.preset != nullptr);
    CHECK(!hit.ambiguous());

    // Two pools holding "Red" is the EXPECTED case, so a bare name must read
    // as a prompt rather than a guess (§8.2).
    Preset b;
    b.name = "Red";
    b.values.set(0, 0, Attr::Red, Value::hard(0.5f));
    r.store.add(2, b);

    auto amb = r.store.findByName("Red");
    CHECK(amb.preset == nullptr);
    CHECK(amb.ambiguous());
    CHECK_EQ(amb.ambiguousPools.size(), size_t(2));

    // The current pool context narrows it, which is what typing inside a pool
    // window does.
    auto ctx = r.store.findByName("Red", 2);
    CHECK(ctx.preset != nullptr);
    CHECK(!ctx.ambiguous());
    CHECK_NEAR(ctx.preset->values.value(0, 0, Attr::Red).value(), 0.5);

    CHECK(r.store.findByName("Nonexistent").preset == nullptr);
}

static void testLiveResolution() {
    Rig r;
    r.store.ensurePool(1, "Colour", Group::Color);

    Preset warm;
    warm.name = "Warm";
    warm.values.set(0, 0, Attr::Red, Value::hard(1.0f));     // wildcard
    warm.values.set(0, 0, Attr::Green, Value::hard(0.6f));
    const Uid uid = r.store.add(1, warm).uid;

    // A wildcard preset applies to ANY fixture that has the attribute, which
    // is what makes a generic palette work.
    CHECK_NEAR(r.store.resolve(uid, Attr::Red, 1, 0).value(), 1.0);
    CHECK_NEAR(r.store.resolve(uid, Attr::Red, 99, 0).value(), 1.0);
    CHECK(!r.store.resolve(uid, Attr::Blue, 1, 0).has_value());
    CHECK(!r.store.resolve(Uid{1234}, Attr::Red, 1, 0).has_value());

    // A fixture-specific entry wins over the wildcard.
    r.store.findMut(uid)->values.set(2, 0, Attr::Red, Value::hard(0.25f));
    CHECK_NEAR(r.store.resolve(uid, Attr::Red, 2, 0).value(), 0.25);
    CHECK_NEAR(r.store.resolve(uid, Attr::Red, 1, 0).value(), 1.0);

    // A cell falls back to the parent entry before the wildcard.
    r.store.findMut(uid)->values.set(3, 0, Attr::Red, Value::hard(0.4f));
    CHECK_NEAR(r.store.resolve(uid, Attr::Red, 3, 7).value(), 0.4);
}

static void testEditingPresetUpdatesCues() {
    // §11.8: this is the feature that falls out of storing a REFERENCE rather
    // than a copy — edit the Warm preset and every cue using it follows.
    Rig r;
    r.store.ensurePool(1, "Colour", Group::Color);

    Preset warm;
    warm.name = "Warm";
    warm.values.set(0, 0, Attr::Red, Value::hard(1.0f));
    const Uid uid = r.store.add(1, warm).uid;

    // Two cues both reference it.
    ValueLayer cueA, cueB;
    cueA.set(1, 0, Attr::Red, Value::ref(uid, Attr::Red, 1.0f));
    cueB.set(2, 0, Attr::Red, Value::ref(uid, Attr::Red, 1.0f));

    r.store.resolveLayer(cueA);
    r.store.resolveLayer(cueB);
    CHECK_NEAR(cueA.find(makeKey(1, 0, Attr::Red))->v, 1.0);
    CHECK_NEAR(cueB.find(makeKey(2, 0, Attr::Red))->v, 1.0);

    // Edit the preset once...
    r.store.findMut(uid)->values.set(0, 0, Attr::Red, Value::hard(0.3f));

    // ...and both cues follow, without either being touched.
    r.store.resolveLayer(cueA);
    r.store.resolveLayer(cueB);
    CHECK_NEAR(cueA.find(makeKey(1, 0, Attr::Red))->v, 0.3);
    CHECK_NEAR(cueB.find(makeKey(2, 0, Attr::Red))->v, 0.3);

    // They are still references, not flattened by resolution.
    CHECK(cueA.find(makeKey(1, 0, Attr::Red))->isRef());

    // A dangling reference keeps its last value rather than snapping to zero,
    // so deleting a preset cannot black out a show.
    ValueLayer orphan;
    orphan.set(1, 0, Attr::Red, Value::ref(Uid{999999}, Attr::Red, 0.77f));
    r.store.resolveLayer(orphan);
    CHECK_NEAR(orphan.find(makeKey(1, 0, Attr::Red))->v, 0.77);
}

static void testThreeLayerIdentity() {
    Rig r;
    r.store.ensurePool(1, "Beam", Group::Beam);

    Preset p;
    p.name = "Prism Stop";
    p.values.set(0, 0, Attr::Prism1Spin, Value::hard(0.5f));
    const Preset& added = r.store.add(1, p);

    // §7.1: symbol and indicator derive from the attribute unless overridden.
    CHECK(added.symbol() == attrInfo(Attr::Prism1Spin).symbol);
    CHECK(!added.indicator().empty());

    Preset q;
    q.name = "Custom";
    q.symbolOverride = "✦";
    q.indicatorOverride = "fast";
    q.values.set(0, 0, Attr::Zoom, Value::hard(1.0f));
    const Preset& added2 = r.store.add(1, q);
    CHECK(added2.symbol() == "✦");
    CHECK(added2.indicator() == "fast");

    // A multi-attribute preset says how many it drives rather than one value.
    Preset multi;
    multi.name = "Look";
    multi.values.set(0, 0, Attr::Red, Value::hard(1.f));
    multi.values.set(0, 0, Attr::Green, Value::hard(0.5f));
    multi.values.set(0, 0, Attr::Blue, Value::hard(0.f));
    const Preset& m = r.store.add(1, multi);
    CHECK_EQ(m.attributes().size(), size_t(3));
    CHECK(m.indicator() == "3 attrs");
}

static void testFilteringFacets() {
    Rig r;
    r.store.ensurePool(1, "Beam", Group::Beam);

    Preset spin;
    spin.name = "Rotate";
    spin.values.set(0, 0, Attr::Prism1Spin, Value::hard(0.8f));
    const Preset& s = r.store.add(1, spin);

    Preset shaper;
    shaper.name = "Frame";
    shaper.values.set(0, 0, Attr::Shaper2A, Value::hard(0.3f));
    const Preset& sh = r.store.add(1, shaper);

    Preset zoom;
    zoom.name = "Wide";
    zoom.values.set(0, 0, Attr::Zoom, Value::hard(1.0f));
    const Preset& z = r.store.add(1, zoom);

    // §7.5: the two examples resolve to derived facets, with no hand-tagging.
    CHECK(s.isAnchoredKind());    // "only presets with rotations"
    CHECK(!z.isAnchoredKind());
    CHECK(sh.isShaper());         // "only presets with the frame/shaper attribute"
    CHECK(!z.isShaper());
    CHECK(!s.isShaper());

    CHECK(s.hasAttr(Attr::Prism1Spin));
    CHECK(!s.hasAttr(Attr::Zoom));
}

static void testAnchoredSymmetricStepping() {
    // THE case flagged in §7.3. Prism1Spin runs
    //   CCW fast | CCW slow | STOP | CW slow | CW fast
    // Dividing that into N equal steps produces presets straddling the stop
    // point, which are useless. Steps must walk OUTWARD from the anchor.
    Rig r;
    const FixtureType& spot = r.spot();

    const auto sets = spot.setsFor(Attr::Prism1Spin);
    CHECK_EQ(sets.size(), size_t(5));
    CHECK(sets[2].name == "Stop");
    CHECK(sets[2].anchor);

    StepRequest req;
    req.attr = Attr::Prism1Spin;
    req.steps = 3;
    req.mode = StepMode::PerSet;
    req.autoSymmetric = true;  // the §7.3 default

    const auto steps = planSteps(spot, req);

    // The anchor is detected, so the mode is promoted to Symmetric: one anchor
    // preset plus three per side.
    CHECK_EQ(steps.size(), size_t(7));
    CHECK_EQ(countSide(steps, 0), 1);
    CHECK_EQ(countSide(steps, -1), 3);
    CHECK_EQ(countSide(steps, +1), 3);

    // The anchor preset sits at the stop point, physically zero.
    const GeneratedStep* anchor = nullptr;
    for (const auto& s : steps)
        if (s.isAnchor) anchor = &s;
    CHECK(anchor != nullptr);
    CHECK(anchor->setName == "Stop");
    CHECK_NEAR(anchor->physical, 0.0);

    // CRITICAL: no generated step may land inside the stop range other than
    // the anchor itself. A step straddling the stop point is the bug this
    // whole mode exists to prevent.
    int inStop = 0;
    for (const auto& s : steps)
        if (s.setName == "Stop") ++inStop;
    CHECK_EQ(inStop, 1);

    // Every negative-side step must actually be counter-clockwise, and every
    // positive-side step clockwise — the sign is what makes the direction
    // glyph meaningful.
    for (const auto& s : steps) {
        if (s.side < 0) CHECK(s.physical < 0.f);
        if (s.side > 0) CHECK(s.physical > 0.f);
    }

    // Steps walk outward: index 0 is the SLOWEST, the last is the fastest.
    float prevSpeed = -1.f;
    for (const auto& s : steps) {
        if (s.side != +1) continue;
        CHECK(std::fabs(s.physical) > prevSpeed);
        prevSpeed = std::fabs(s.physical);
    }
    prevSpeed = -1.f;
    for (const auto& s : steps) {
        if (s.side != -1) continue;
        CHECK(std::fabs(s.physical) > prevSpeed);
        prevSpeed = std::fabs(s.physical);
    }

    // Changing the step count rebuilds proportionally.
    req.steps = 5;
    CHECK_EQ(planSteps(spot, req).size(), size_t(11));  // anchor + 5 + 5

    // Dropping the anchor preset leaves only the sides.
    req.steps = 2;
    req.keepAnchor = false;
    CHECK_EQ(planSteps(spot, req).size(), size_t(4));
}

static void testUnanchoredStepping() {
    Rig r;
    const FixtureType& spot = r.spot();

    // Gobo1's "Open" slot is flagged as an anchor, but an INDEXED attribute
    // must not be promoted to symmetric stepping: that would put presets
    // between gobo slots rather than on them. One preset per slot, centred.
    StepRequest gobo;
    gobo.attr = Attr::Gobo1;
    gobo.steps = 2;
    gobo.autoSymmetric = true;
    const auto goboSteps = planSteps(spot, gobo);
    CHECK_EQ(goboSteps.size(), size_t(4));  // Open + 3 gobos, not symmetric sides

    // Every step lands in the MIDDLE of its range.
    const int wantMid[] = {4, 14, 24, 34};
    const char* wantName[] = {"Open", "Gobo 1", "Gobo 2", "Gobo 3"};
    for (size_t i = 0; i < goboSteps.size(); ++i) {
        CHECK_EQ(int(goboSteps[i].dmx), wantMid[i]);
        CHECK(goboSteps[i].setName == wantName[i]);
        CHECK_EQ(goboSteps[i].side, 0);  // no sides: this is per-set
    }

    // Each step must sit strictly inside its own range, which is the property
    // that makes a slightly-off profile still hit the right gobo.
    for (const auto& st : goboSteps) {
        const auto sets = spot.setsFor(Attr::Gobo1);
        bool inside = false;
        for (const auto& cs : sets)
            if (cs.name == st.setName && st.dmx > cs.dmxFrom && st.dmx < cs.dmxTo)
                inside = true;
        CHECK(inside);
    }

    // An attribute with no ranges at all falls back to an even spread of the
    // channel rather than producing nothing.
    StepRequest zoom;
    zoom.attr = Attr::Zoom;
    zoom.steps = 5;
    const auto zoomSteps = planSteps(spot, zoom);
    CHECK_EQ(zoomSteps.size(), size_t(5));
    CHECK_EQ(int(zoomSteps.front().dmx), 0);
    CHECK_EQ(int(zoomSteps.back().dmx), 255);
    for (const auto& s : zoomSteps) CHECK(s.setName.empty());
}

static void testStepModes() {
    Rig r;
    const FixtureType& spot = r.spot();

    // Dmx: an even spread across the channel, ignoring range boundaries.
    StepRequest d;
    d.attr = Attr::Prism1Spin;
    d.steps = 5;
    d.mode = StepMode::Dmx;
    d.autoSymmetric = false;
    const auto dmx = planSteps(spot, d);
    CHECK_EQ(dmx.size(), size_t(5));
    CHECK_EQ(int(dmx[0].dmx), 0);
    CHECK_EQ(int(dmx[4].dmx), 255);
    CHECK_EQ(int(dmx[2].dmx), 127);

    // Physical: equal steps in rpm, which is NOT the same as equal DMX when
    // the profile maps non-linearly.
    StepRequest p = d;
    p.mode = StepMode::Physical;
    const auto phys = planSteps(spot, p);
    CHECK_EQ(phys.size(), size_t(5));
    CHECK(phys.front().physical < -20.f);   // the CCW extreme
    CHECK(phys.back().physical > 20.f);     // the CW extreme
    // Physical values are evenly spaced; DMX values are not.
    const float gap1 = phys[1].physical - phys[0].physical;
    const float gap2 = phys[2].physical - phys[1].physical;
    CHECK(std::fabs(gap1 - gap2) < 2.0f);
    const int dgap1 = phys[1].dmx - phys[0].dmx;
    const int dgap2 = phys[2].dmx - phys[1].dmx;
    CHECK(dgap1 != dgap2);  // proof the two modes differ

    // PerSet on an indexed attribute gives one preset per slot, landing in the
    // MIDDLE of each range so a slightly-off profile still hits the gobo.
    StepRequest g;
    g.attr = Attr::Gobo1;
    g.steps = 4;
    g.mode = StepMode::PerSet;
    g.autoSymmetric = false;
    const auto perSet = planSteps(spot, g);
    CHECK_EQ(perSet.size(), size_t(4));  // Open + 3 gobos
    std::set<std::string> names;
    for (const auto& s : perSet) names.insert(s.setName);
    CHECK_EQ(names.size(), size_t(4));
    CHECK_EQ(int(perSet[0].dmx), 4);   // middle of 0-9
    CHECK_EQ(int(perSet[1].dmx), 14);  // middle of 10-19
}

static void testNamingTemplates() {
    Rig r;
    const FixtureType& spot = r.spot();

    StepRequest req;
    req.attr = Attr::Prism1Spin;
    req.steps = 3;
    const auto steps = planSteps(spot, req);

    // Tokens expand from the step and the attribute (§7.3).
    const GeneratedStep& cw = *std::find_if(steps.begin(), steps.end(),
                                            [](const GeneratedStep& s) {
                                                return s.side == +1 && s.indexInSide == 0;
                                            });

    const std::string a = expandTemplate("{attr} {set} {n}/{N}", cw,
                                         Attr::Prism1Spin, "Moving Spot");
    CHECK(a == "Prism 1 Spin CW slow 1/3");

    const std::string b = expandTemplate("{attr} {dir} {phys}{unit}", cw,
                                         Attr::Prism1Spin, "Moving Spot");
    CHECK(b.find("↻") != std::string::npos);  // clockwise glyph
    CHECK(b.find("rpm") != std::string::npos);

    const std::string c = expandTemplate("{set}", cw, Attr::Prism1Spin, "x");
    CHECK(c == "CW slow");

    const std::string d = expandTemplate("{fixtype} {dmx}", cw, Attr::Prism1Spin,
                                         "Moving Spot");
    CHECK(d.find("Moving Spot") != std::string::npos);

    // The anchor reads as stopped, not as a direction.
    const GeneratedStep& anchor = *std::find_if(steps.begin(), steps.end(),
                                                [](const GeneratedStep& s) {
                                                    return s.isAnchor;
                                                });
    const std::string e = expandTemplate("{dir} {set}", anchor, Attr::Prism1Spin, "x");
    CHECK(e.find("■") != std::string::npos);

    // An unknown token is dropped without leaving double spaces behind.
    const std::string f = expandTemplate("{attr} {bogus} {set}", cw,
                                         Attr::Prism1Spin, "x");
    CHECK(f.find("  ") == std::string::npos);
}

static void testGenerateIntoPool() {
    Rig r;
    r.store.ensurePool(5, "Prism", Group::Beam);

    StepRequest req;
    req.attr = Attr::Prism1Spin;
    req.steps = 3;
    req.nameTemplate = "{attr} {set} {n}/{N}";

    // Generic: no targets, so the presets are wildcard and apply to anything
    // with the attribute.
    GenerateReport rep = generatePresets(r.store, 5, r.eng.patch, r.spot(), {}, req);
    CHECK_EQ(rep.created, 7);
    CHECK_EQ(r.store.pool(5)->presets.size(), size_t(7));

    for (const auto& p : r.store.pool(5)->presets) {
        CHECK(p.generated);
        CHECK(p.group == Group::Beam);
        CHECK(p.uid.valid());
        CHECK(!p.name.empty());
        // The wildcard entry is what makes it generic.
        CHECK(p.values.has(0, 0, Attr::Prism1Spin));
    }

    // Names are unique within the pool even where the template would repeat.
    std::set<std::string> names;
    for (const auto& p : r.store.pool(5)->presets) names.insert(p.name);
    CHECK_EQ(names.size(), size_t(7));

    // The anchor preset is tagged, so a UI can mark it.
    int anchors = 0;
    for (const auto& p : r.store.pool(5)->presets)
        for (const auto& t : p.tags)
            if (t == "anchor") ++anchors;
    CHECK_EQ(anchors, 1);

    // Fixture-specific generation writes per target instead.
    r.store.ensurePool(6, "Prism Specific", Group::Beam);
    GenerateReport rep2 = generatePresets(r.store, 6, r.eng.patch, r.spot(),
                                          {{1, 0}, {2, 0}}, req);
    CHECK_EQ(rep2.created, 7);
    const Preset& first = r.store.pool(6)->presets.front();
    CHECK(first.values.has(1, 0, Attr::Prism1Spin));
    CHECK(first.values.has(2, 0, Attr::Prism1Spin));
    CHECK(!first.values.has(0, 0, Attr::Prism1Spin));
}

static void testGeneratedPresetsDriveOutput() {
    // End to end: generate a bank, reference one from the programmer, and
    // check it reaches the output through resolution.
    Rig r;
    r.store.ensurePool(1, "Prism", Group::Beam);

    StepRequest req;
    req.attr = Attr::Prism1Spin;
    req.steps = 2;
    generatePresets(r.store, 1, r.eng.patch, r.spot(), {}, req);

    // Pick the fastest clockwise step.
    const Preset* fastest = nullptr;
    float best = -1.f;
    for (const auto& p : r.store.pool(1)->presets) {
        const auto v = p.values.value(0, 0, Attr::Prism1Spin);
        if (v && *v > best) {
            best = *v;
            fastest = &p;
        }
    }
    CHECK(fastest != nullptr);

    r.eng.programmer.selection().set({{1, 0}});
    r.eng.programmer.setRef(Attr::Prism1Spin, fastest->uid, Attr::Prism1Spin, 0.f);

    // The cached value starts wrong on purpose; resolution fixes it.
    r.store.resolveLayer(r.eng.programmer.values());
    r.eng.tick();
    CHECK_NEAR(*r.eng.merged().get(makeKey(1, 0, Attr::Prism1Spin)), best);

    // Editing the preset moves the live output, with nothing else touched.
    r.store.findMut(fastest->uid)->values.set(0, 0, Attr::Prism1Spin, Value::hard(0.1f));
    r.store.resolveLayer(r.eng.programmer.values());
    r.eng.tick();
    CHECK_NEAR(*r.eng.merged().get(makeKey(1, 0, Attr::Prism1Spin)), 0.1);
}

int main() {
    std::printf("lumen preset\n");
    testPoolNaming();
    testNameLookupAmbiguity();
    testLiveResolution();
    testEditingPresetUpdatesCues();
    testThreeLayerIdentity();
    testFilteringFacets();
    testAnchoredSymmetricStepping();
    testUnanchoredStepping();
    testStepModes();
    testNamingTemplates();
    testGenerateIntoPool();
    testGeneratedPresetsDriveOutput();
    return check::report("preset");
}
