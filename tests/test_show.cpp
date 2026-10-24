// Tests for show persistence: round-trip fidelity, atomicity, autosave.
#include "Check.hpp"

#include "lumen/Command.hpp"
#include "lumen/Mapping.hpp"
#include "lumen/Show.hpp"

#include <sqlite3.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>

using namespace lumen;
namespace fs = std::filesystem;

namespace {

std::string tmpDir() {
    const auto d = fs::temp_directory_path() / "lumen_show_tests";
    fs::create_directories(d);
    return d.string();
}

// Builds a show exercising every persisted structure.
void buildShow(Engine& eng) {
    installStarterTypes(eng.patch);
    Console con(eng);

    con.execute("Fixture 1 Thru 4 Patch \"Moving Spot\" 4 1 1");
    con.execute("Fixture 20 Patch \"Pixel Bar 12\" 1 2 1");

    // Positions, so the stage-based spread sources have something to read.
    for (FixtureId id = 1; id <= 4; ++id) {
        auto* f = const_cast<Fixture*>(eng.patch.find(id));
        f->x = static_cast<float>(id) * 1.5f;
        f->y = 2.0f;
        f->z = 4.0f;
        f->rotZ = 0.25f;
        f->positioned = true;
        f->tags.push_back("upstage");
    }

    Sequence& s = eng.ensureSequence(1);
    s.name = "Main";
    s.chase = true;
    s.chaseRateHz = 3.5f;
    s.cues.resize(3);
    for (size_t i = 0; i < 3; ++i) {
        s.cues[i].uid = eng.uids.next();
        s.cues[i].number = static_cast<float>(i + 1);
        s.cues[i].name = "Cue " + std::to_string(i + 1);
        s.cues[i].timing.fadeIn = 1.5 + static_cast<double>(i);
        s.cues[i].timing.curve = FadeCurve::SCurve;
    }
    s.cues[0].values().set(1, 0, Attr::Dimmer, Value::hard(1.0f));
    s.cues[0].values().set(1, 0, Attr::Red, Value::ref(Uid{987654}, Attr::Red, 0.75f));
    s.cues[1].values().set(2, 0, Attr::Pan, Value::hard(0.25f));
    s.cues[1].setTrackMode(Group::Color, TrackMode::Block);
    s.cues[1].values().set(1, 0, Attr::Red, Value::hard(0.9f));
    s.cues[2].shield = true;
    s.cues[2].values().set(20, 3, Attr::Green, Value::hard(0.5f));
    // An inactive value must survive, because active/inactive is a stored axis.
    Value inactive = Value::hard(0.33f);
    inactive.active = false;
    s.cues[2].values().set(3, 0, Attr::Blue, inactive);

    // A second part with its own timing.
    s.cues[0].parts.push_back(CuePart{});
    Timing pt;
    pt.fadeIn = 9.0;
    s.cues[0].parts[1].timing = pt;
    s.cues[0].parts[1].values.set(4, 0, Attr::Dimmer, Value::hard(0.6f));

    // A phaser of each generator kind, so steps and curves both round-trip.
    Phaser wave;
    wave.attr = Attr::Dimmer;
    wave.gen.wave = Wave::Triangle;
    wave.gen.duty = 0.3f;
    wave.speedHz = 2.25f;
    wave.size = 0.8f;
    wave.relative = false;
    wave.source = SpreadSource::StageAngle;
    wave.mods = {2, 3, 1.5f, 0.125f};
    wave.direction = Direction::Bounce;
    wave.pivot = BouncePivot::CentreAtEnd;
    wave.overshoot = 0.4f;
    wave.gap = gapFromBehaviour(GapBehaviour::Stutter, 0.5f);
    wave.variation = {0.1f, 0.2f, 0.3f, 4242};
    wave.stack = StackMode::Add;
    wave.clock = ClockSource::Playback;
    wave.targets = {{1, 0}, {2, 0}, {3, 0}};
    s.cues[0].phasers.push_back(wave);

    Phaser steps;
    steps.role = Role::AxisX;
    steps.gen.kind = GenKind::Steps;
    steps.gen.steps = {{0.2f, 0.5f, 0.1f, 0.2f, 0.5f, -0.5f},
                       {0.9f, 1.0f, 0.0f, 0.0f, 0.f, 0.f}};
    steps.targets = {{1, 0}, {2, 0}};
    s.cues[1].phasers.push_back(steps);

    Phaser curve;
    curve.attr = Attr::Zoom;
    curve.gen.kind = GenKind::Curve;
    curve.gen.curve = {{0.f, 0.f, 0.f, 0.f, 0.3f, 0.1f},
                       {0.5f, 1.f, -0.2f, 0.f, 0.2f, 0.f},
                       {1.f, 0.f, -0.3f, 0.1f, 0.f, 0.f}};
    curve.targets = {{20, 5}};
    s.cues[2].phasers.push_back(curve);

    Executor& e1 = eng.ensureExecutor(1);
    e1.sequenceId = 1;
    e1.label = "Main go";
    e1.priority = 7;
    e1.merge = MergeRule::HTP;
    e1.swapGroup = 2;
    e1.master = 0.65f;
    e1.release = ReleaseMode::HoldLast;
    e1.releaseTime = 4.5;
    e1.mergeByGroup[static_cast<size_t>(Group::Color)] = MergeRule::LTP;
    e1.releaseByGroup[static_cast<size_t>(Group::Intensity)] = ReleaseMode::ToZero;

    Executor& e2 = eng.ensureExecutor(5);
    e2.sequenceId = 1;
    e2.priority = -3;

    // --- groups: one of each kind, plus nesting and overrides (§20.8) -----
    GroupDef stat;
    stat.name = "Front Movers";
    stat.members = {{3, 0}, {1, 0}, {2, 0}};  // capture order is information
    stat.tags.push_back("truss");
    const Uid statUid = eng.groups.add(std::move(stat)).uid;

    GroupDef dyn;
    dyn.name = "All Spots";
    dyn.kind = GroupKind::Dynamic;
    dyn.query.typeName = "Moving Spot";
    dyn.query.universe = 1;   // where the Patch command put them
    dyn.query.addressRange = std::make_pair(1, 200);
    dyn.query.idRange = std::make_pair(FixtureId(1), FixtureId(99));
    dyn.query.hasAttr = Attr::Pan;
    dyn.query.hasGroup = Group::Position;
    dyn.query.hasKind = Kind::Continuous;
    dyn.query.anyTag = {"upstage", "spot"};
    dyn.query.sortKey = "x";
    dyn.allowOverrides = true;
    const Uid dynUid = eng.groups.add(std::move(dyn)).uid;
    eng.groups.exclude(dynUid, eng.patch.find(2)->uid);
    eng.groups.include(dynUid, eng.patch.find(20)->uid);

    GroupDef nested;
    nested.name = "Everything";
    const Uid nestUid = eng.groups.add(std::move(nested)).uid;
    eng.groups.nest(nestUid, statUid);
    eng.groups.nest(nestUid, dynUid);

    // --- presets (§7) -----------------------------------------------------
    eng.presets.ensurePool(1, "Colour", Group::Color);
    eng.presets.ensurePool(2, "Position", Group::Position);
    {
        Preset red;
        red.name = "Red";
        red.group = Group::Color;
        red.symbolOverride = "R";
        red.tags = {"primary", "warm"};
        // Fixture 0 is the wildcard, so one preset covers anything with the
        // attribute.
        red.values.set(0, 0, Attr::Red, Value::hard(1.0f));
        red.values.set(0, 0, Attr::Green, Value::hard(0.0f));
        eng.presets.add(1, std::move(red));

        Preset home;
        home.name = "Home";
        home.group = Group::Position;
        home.generated = true;
        home.indicatorOverride = "H";
        home.values.set(1, 0, Attr::Pan, Value::hard(0.5f));
        home.values.set(1, 0, Attr::Tilt, Value::hard(0.5f));
        Value inactiveTilt = Value::hard(0.25f);
        inactiveTilt.active = false;
        home.values.set(2, 0, Attr::Tilt, inactiveTilt);
        // A preset holding a reference to another preset must survive too.
        home.values.set(3, 0, Attr::Pan, Value::ref(Uid{424242}, Attr::Pan, 0.3f));
        eng.presets.add(2, std::move(home));
    }

    // --- FX recipes (§4) --------------------------------------------------
    {
        Recipe circle;
        circle.name = "Saved Circle";
        circle.speed = 1.75f;
        circle.size = 0.6f;
        circle.spread = 2.5f;
        circle.offset = 0.125f;
        circle.baseHz = 0.75f;
        circle.tags = {"movement"};

        Slot a;
        a.label = "X";
        a.role = Role::AxisX;
        a.gen.wave = Wave::Cosine;
        a.gen.duty = 0.4f;
        a.sizeMul = 0.5f;
        a.speedMul = 2.f;
        a.phaseOffset = 0.25f;
        a.centre = 0.4f;
        a.relative = false;
        a.mods = {2, 3, 1.5f, 0.0625f};
        a.source = SpreadSource::StageRadial;
        a.direction = Direction::Bounce;
        a.pivot = BouncePivot::OverEnd;
        a.gap = gapFromBehaviour(GapBehaviour::Stutter, 0.5f);
        a.gap.sequence = {1.f, 1.f, 2.f};
        a.variation = {0.1f, 0.2f, 0.3f, 777};
        a.layer = 1;
        a.stack = StackMode::Add;
        a.opacity = 0.75f;
        a.muted = true;
        circle.slots.push_back(std::move(a));

        Slot b;
        b.label = "Steps";
        b.attr = Attr::Dimmer;
        b.gen.kind = GenKind::Steps;
        b.gen.steps = {{0.1f, 0.5f, 0.2f, 0.3f, 0.4f, -0.4f}, {1.f, 1.f, 0.f, 0.f, 0.f, 0.f}};
        circle.slots.push_back(std::move(b));

        Slot c;
        c.label = "Curve";
        c.attr = Attr::Zoom;
        c.gen.kind = GenKind::Curve;
        c.gen.curve = {{0.f, 0.f, 0.f, 0.f, 0.2f, 0.1f}, {1.f, 1.f, -0.2f, 0.f, 0.f, 0.f}};
        circle.slots.push_back(std::move(c));

        eng.recipes.add(std::move(circle));
    }

    eng.grandMaster = 0.8f;
    eng.unusedMode = UnusedMode::ToDefault;
}

} // namespace

// Groups, presets and recipes hang off the engine, so they have to survive a
// save: an imported MVR's layer groups would otherwise vanish on reopen.
static void testGroupsPresetsRecipesRoundTrip() {
    const std::string path = tmpDir() + "/stores.lumen";
    fs::remove(path);

    Engine a;
    buildShow(a);

    SaveReport sr = ShowFile::save(a, path, "Stores");
    CHECK(sr.ok);
    CHECK_EQ(sr.groups, 3);
    CHECK_EQ(sr.presets, 2);
    CHECK_EQ(sr.recipes, 1);

    Engine b;
    LoadReport lr = ShowFile::load(b, path);
    CHECK(lr.ok);
    CHECK_EQ(lr.groups, 3);
    CHECK_EQ(lr.presets, 2);
    CHECK_EQ(lr.recipes, 1);
    CHECK(lr.warnings.empty());

    // --- groups -----------------------------------------------------------
    const GroupDef* stat = b.groups.byName("Front Movers");
    CHECK(stat != nullptr);
    CHECK(stat->kind == GroupKind::Static);
    CHECK_EQ(stat->members.size(), size_t(3));
    // Capture order is information (it drives FX spread), so it must survive
    // in order rather than being sorted on the way through.
    CHECK_EQ(int(stat->members[0].fixture), 3);
    CHECK_EQ(int(stat->members[1].fixture), 1);
    CHECK_EQ(int(stat->members[2].fixture), 2);
    CHECK_EQ(stat->tags.size(), size_t(1));
    CHECK(stat->tags[0] == "truss");

    const GroupDef* dyn = b.groups.byName("All Spots");
    CHECK(dyn != nullptr);
    CHECK(dyn->kind == GroupKind::Dynamic);
    CHECK(dyn->allowOverrides);
    CHECK(dyn->query.typeName.has_value());
    CHECK(*dyn->query.typeName == "Moving Spot");
    CHECK(dyn->query.universe.has_value());
    CHECK_EQ(*dyn->query.universe, 1);
    CHECK(dyn->query.addressRange.has_value());
    CHECK_EQ(dyn->query.addressRange->second, 200);
    CHECK(dyn->query.idRange.has_value());
    CHECK_EQ(int(dyn->query.idRange->second), 99);
    CHECK(dyn->query.hasAttr.has_value());
    CHECK(*dyn->query.hasAttr == Attr::Pan);
    CHECK(dyn->query.hasGroup.has_value());
    CHECK(*dyn->query.hasGroup == Group::Position);
    CHECK(dyn->query.hasKind.has_value());
    CHECK(dyn->query.sortKey == "x");
    CHECK_EQ(dyn->query.anyTag.size(), size_t(2));
    // Overrides are sticky by fixture UID in both directions.
    CHECK_EQ(dyn->excludes.size(), size_t(1));
    CHECK_EQ(dyn->includes.size(), size_t(1));
    CHECK(dyn->excludes[0] == b.patch.find(2)->uid);
    CHECK(dyn->includes[0] == b.patch.find(20)->uid);

    // A dynamic group resolves against the reloaded patch, which is the real
    // test: a restored query that matches nothing would look the same as a
    // restored query that works.
    GroupResolution res = b.groups.resolve(dyn->uid, b.patch);
    // Four movers match the restored query, fixture 2 is the sticky exclude,
    // and the pixel bar is the sticky include. Asserting only "> 0" would
    // pass on a query that came back matching nothing but kept its include.
    CHECK_EQ(res.queryMatched, 4);
    CHECK_EQ(res.excluded, 1);
    CHECK_EQ(res.included, 1);
    CHECK_EQ(res.targets.size(), size_t(4));
    for (const Target& t : res.targets) CHECK(t.fixture != 2);

    const GroupDef* nest = b.groups.byName("Everything");
    CHECK(nest != nullptr);
    CHECK(nest->isNested());
    CHECK_EQ(nest->children.size(), size_t(2));
    CHECK(nest->children[0] == stat->uid);
    CHECK(nest->children[1] == dyn->uid);
    // Nesting survives as a tree, not a flattened list (§20.6).
    Selection sel = b.groups.buildSelection(nest->uid, b.patch);
    CHECK(sel.depth() >= 2);

    // --- presets ----------------------------------------------------------
    CHECK_EQ(b.presets.pools().size(), size_t(2));
    const PresetPool* colour = b.presets.pool(1);
    CHECK(colour != nullptr);
    CHECK(colour->name == "Colour");
    CHECK(colour->group == Group::Color);

    const Preset* red = colour->byName("Red");
    CHECK(red != nullptr);
    CHECK(red->symbol() == "R");
    CHECK_EQ(red->tags.size(), size_t(2));
    CHECK_EQ(red->values.size(), size_t(2));
    // The wildcard entry must stay fixture 0, or a generic palette silently
    // becomes fixture-specific.
    const auto rv = red->values.value(0, 0, Attr::Red);
    CHECK(rv.has_value());
    CHECK_NEAR(*rv, 1.0f);

    const PresetPool* pos = b.presets.pool(2);
    CHECK(pos != nullptr);
    const Preset* home = pos->byName("Home");
    CHECK(home != nullptr);
    CHECK(home->generated);
    CHECK(home->indicator() == "H");
    // The inactive value and the preset reference both survive.
    const Value* tilt = home->values.find(makeKey(2, 0, Attr::Tilt));
    CHECK(tilt != nullptr);
    CHECK(!tilt->active);
    const Value* ref = home->values.find(makeKey(3, 0, Attr::Pan));
    CHECK(ref != nullptr);
    CHECK(ref->isRef());
    CHECK(ref->preset == Uid{424242});
    CHECK(ref->presetAttr == Attr::Pan);

    // UIDs are preserved, because every reference in the show stores one.
    const Preset* byUid = b.presets.find(red->uid);
    CHECK(byUid == red);
    const Preset* srcRed = a.presets.pool(1)->byName("Red");
    CHECK(srcRed != nullptr);
    CHECK(red->uid == srcRed->uid);
    // And a restored name is not re-uniquified into "Red 2".
    CHECK(red->name == "Red");

    // --- recipes ----------------------------------------------------------
    CHECK_EQ(b.recipes.size(), size_t(1));
    const Recipe* rc = b.recipes.byName("Saved Circle");
    CHECK(rc != nullptr);
    CHECK_NEAR(rc->speed, 1.75f);
    CHECK_NEAR(rc->size, 0.6f);
    CHECK_NEAR(rc->spread, 2.5f);
    CHECK_NEAR(rc->offset, 0.125f);
    CHECK_NEAR(rc->baseHz, 0.75f);
    CHECK_EQ(rc->tags.size(), size_t(1));
    CHECK_EQ(rc->slots.size(), size_t(3));

    const Slot& s0 = rc->slots[0];
    CHECK(s0.label == "X");
    CHECK(s0.role == Role::AxisX);        // the role, not a resolved attribute
    CHECK(s0.gen.wave == Wave::Cosine);
    CHECK_NEAR(s0.gen.duty, 0.4f);
    CHECK(!s0.relative);
    CHECK_NEAR(s0.sizeMul, 0.5f);
    CHECK_NEAR(s0.speedMul, 2.f);
    CHECK_NEAR(s0.phaseOffset, 0.25f);
    CHECK_NEAR(s0.centre, 0.4f);
    CHECK_EQ(s0.mods.blocks, 2);
    CHECK_EQ(s0.mods.wings, 3);
    CHECK_NEAR(s0.mods.groups, 1.5f);
    CHECK_NEAR(s0.mods.offset, 0.0625f);
    CHECK(s0.source == SpreadSource::StageRadial);
    CHECK(s0.direction == Direction::Bounce);
    CHECK(s0.pivot == BouncePivot::OverEnd);
    CHECK(s0.gap.enabled);
    CHECK_EQ(s0.gap.sequence.size(), size_t(3));
    CHECK_NEAR(s0.gap.sequence[2], 2.f);
    CHECK_EQ(int(s0.variation.seed), 777);
    CHECK_EQ(s0.layer, 1);
    CHECK(s0.stack == StackMode::Add);
    CHECK_NEAR(s0.opacity, 0.75f);
    CHECK(s0.muted);
    CHECK(!s0.soloed);

    const Slot& s1 = rc->slots[1];
    CHECK(s1.gen.kind == GenKind::Steps);
    CHECK_EQ(s1.gen.steps.size(), size_t(2));
    CHECK_NEAR(s1.gen.steps[0].decayCurve, -0.4f);
    CHECK(!s1.soloed);

    const Slot& s2 = rc->slots[2];
    CHECK(s2.gen.kind == GenKind::Curve);
    CHECK_EQ(s2.gen.curve.size(), size_t(2));
    CHECK_NEAR(s2.gen.curve[1].inDx, -0.2f);

    // The restored recipe still instantiates: a reloaded FX that cannot be
    // run is no better than a lost one. Slot 0 is muted, so two of the three
    // compile — counted exactly, because "> 0" would pass on a recipe whose
    // role slot came back unresolvable.
    InstantiateReport ir = instantiate(*rc, {{1, 0}, {2, 0}, {3, 0}, {4, 0}}, b.patch);
    CHECK_EQ(ir.phasers.size(), size_t(2));
    CHECK_EQ(ir.slotsApplied, 2);
    CHECK_EQ(ir.slotsEmpty, 0);
    for (const Phaser& p : ir.phasers) CHECK_EQ(p.targets.size(), size_t(4));

    // Un-muting the role slot proves AxisX still resolves to Pan on a mover
    // after a reload — the keystone of §4.2 and the thing a lossy save would
    // quietly break.
    Recipe* live = b.recipes.byName("Saved Circle");
    CHECK(live != nullptr);
    live->slots[0].muted = false;
    InstantiateReport ir2 = instantiate(*live, {{1, 0}, {2, 0}}, b.patch);
    CHECK_EQ(ir2.phasers.size(), size_t(3));
    bool drivesPan = false;
    for (const Phaser& p : ir2.phasers)
        if (p.attr == Attr::Pan || p.role == Role::AxisX) drivesPan = true;
    CHECK(drivesPan);
}

// Saving and loading must be reachable through the command layer like
// everything else (§16.2), and loading must be gated: it replaces the show.
static void testShowFilesThroughTheConsole() {
    const std::string path = tmpDir() + "/console.lumen";
    fs::remove(path);

    Engine a;
    buildShow(a);
    Console con(a);

    Result sr = con.execute("SaveShow \"" + path + "\" \"Console Show\"");
    CHECK(sr.parsed);
    CHECK(sr.ok);
    CHECK(fs::exists(path));
    CHECK(sr.message.find("5 fixtures") != std::string::npos);

    Engine b;
    Console con2(b);

    // Protected: an operator has to be granted it, and an agent never gets it.
    Result denied = con2.execute("LoadShow \"" + path + "\"");
    CHECK(denied.parsed);
    CHECK(!denied.ok);
    CHECK_EQ(b.patch.size(), size_t(0));

    con2.setPrivilege(Privilege::Protected);
    Result lr = con2.execute("LoadShow \"" + path + "\"");
    CHECK(lr.ok);
    CHECK_EQ(b.patch.size(), size_t(5));
    CHECK_EQ(b.groups.size(), size_t(3));
    CHECK(lr.message.find("Console Show") != std::string::npos);

    // A failed load leaves the show that was already open alone.
    Result bad = con2.execute("LoadShow \"" + tmpDir() + "/not-here.lumen\"");
    CHECK(bad.parsed);
    CHECK(!bad.ok);
    CHECK_EQ(b.patch.size(), size_t(5));
    CHECK_EQ(b.groups.size(), size_t(3));

    // And the reloaded show runs.
    CHECK(con2.execute("Fixture 1 At 100").ok);
    b.tick();
    const auto dim = b.merged().get(makeKey(1, 0, Attr::Dimmer));
    CHECK(dim.has_value());
}

static void testRoundTrip() {
    const std::string path = tmpDir() + "/round.lumen";
    fs::remove(path);

    Engine a;
    buildShow(a);

    SaveReport sr = ShowFile::save(a, path, "Round Trip Show");
    CHECK(sr.ok);
    CHECK(sr.error.empty());
    CHECK_EQ(sr.fixtures, 5);
    CHECK_EQ(sr.sequences, 1);
    CHECK_EQ(sr.cues, 3);
    CHECK_EQ(sr.phasers, 3);
    CHECK_EQ(sr.executors, 2);
    CHECK(sr.values > 0);
    CHECK(fs::exists(path));
    CHECK(!fs::exists(path + ".tmp"));  // temp file cleaned up

    Engine b;
    LoadReport lr = ShowFile::load(b, path);
    CHECK(lr.ok);
    CHECK(lr.error.empty());
    CHECK_EQ(lr.schemaVersion, kShowSchemaVersion);
    CHECK(lr.showName == "Round Trip Show");
    CHECK_EQ(lr.warnings.size(), size_t(0));
    CHECK_EQ(lr.fixtures, 5);

    // --- patch ------------------------------------------------------------
    CHECK_EQ(b.patch.size(), size_t(5));
    for (FixtureId id = 1; id <= 4; ++id) {
        const Fixture* fa = a.patch.find(id);
        const Fixture* fb = b.patch.find(id);
        CHECK(fb != nullptr);
        // The UID must survive, or every stored reference breaks (§8.1).
        CHECK(fa->uid == fb->uid);
        CHECK(fa->name == fb->name);
        CHECK_EQ(fb->universe, fa->universe);
        CHECK_EQ(fb->address, fa->address);
        CHECK_NEAR(fb->x, fa->x);
        CHECK_NEAR(fb->y, fa->y);
        CHECK_NEAR(fb->z, fa->z);
        CHECK_NEAR(fb->rotZ, fa->rotZ);
        CHECK(fb->positioned);
        CHECK_EQ(fb->tags.size(), size_t(1));
        CHECK(fb->tags[0] == "upstage");
    }
    // Lookup by UID works after the round trip, which is what cues rely on.
    CHECK(b.patch.findByUid(a.patch.find(1)->uid) != nullptr);

    // --- types, including per-cell channels and range labels ---------------
    const FixtureType* bar = b.patch.typeOf(20);
    CHECK(bar != nullptr);
    CHECK_EQ(bar->cellCount, CellId(12));
    CHECK(bar->hasAttr(Attr::Red, 12));
    CHECK_EQ(bar->channel(Attr::Red, 2)->coarse, 4);

    const FixtureType* spot = b.patch.typeOf(1);
    CHECK_EQ(spot->footprint(), 15);
    CHECK_EQ(spot->channel(Attr::Pan, 0)->fine, 1);  // 16-bit survives
    const auto spin = spot->setsFor(Attr::Prism1Spin);
    CHECK_EQ(spin.size(), size_t(5));
    CHECK(spin[2].name == "Stop");
    CHECK(spin[2].anchor);  // the anchor flag the auto-stepper keys off
    CHECK_NEAR(spin[0].physFrom, -30.0);

    // --- sequence and cues ------------------------------------------------
    const Sequence* sb = b.sequence(1);
    CHECK(sb != nullptr);
    CHECK(sb->name == "Main");
    CHECK(sb->chase);
    CHECK_NEAR(sb->chaseRateHz, 3.5);
    CHECK_EQ(sb->cues.size(), size_t(3));
    CHECK(sb->uid == a.sequence(1)->uid);

    CHECK_NEAR(sb->cues[1].timing.fadeIn, 2.5);
    CHECK(sb->cues[0].timing.curve == FadeCurve::SCurve);
    CHECK(sb->cues[2].shield);
    CHECK(!sb->cues[0].shield);

    // Per-group tracking masks survive, which the five-cell cue-sheet strip
    // reads (§11.1).
    CHECK(sb->cues[1].trackMode(Group::Color) == TrackMode::Block);
    CHECK(sb->cues[1].trackMode(Group::Position) == TrackMode::Track);
    CHECK(sb->cues[0].trackMode(Group::Color) == TrackMode::Track);

    // --- values, including both axes --------------------------------------
    const Value* ref = sb->cues[0].values().find(makeKey(1, 0, Attr::Red));
    CHECK(ref != nullptr);
    CHECK(ref->isRef());  // a preset reference stays a reference (§11.8)
    CHECK(ref->preset == Uid{987654});
    CHECK(ref->presetAttr == Attr::Red);
    CHECK_NEAR(ref->v, 0.75);

    const Value* inact = sb->cues[2].values().find(makeKey(3, 0, Attr::Blue));
    CHECK(inact != nullptr);
    CHECK(!inact->active);  // the active axis is stored, not recomputed
    CHECK_NEAR(inact->v, 0.33);

    // A cell-addressed value keeps its cell.
    CHECK(sb->cues[2].values().has(20, 3, Attr::Green));

    // --- parts ------------------------------------------------------------
    CHECK_EQ(sb->cues[0].parts.size(), size_t(2));
    CHECK_NEAR(sb->cues[0].timingForPart(1).fadeIn, 9.0);
    CHECK(sb->cues[0].parts[1].values.has(4, 0, Attr::Dimmer));

    // --- phasers ----------------------------------------------------------
    CHECK_EQ(sb->cues[0].phasers.size(), size_t(1));
    const Phaser& w = sb->cues[0].phasers[0];
    CHECK(w.gen.kind == GenKind::Waveform);
    CHECK(w.gen.wave == Wave::Triangle);
    CHECK_NEAR(w.gen.duty, 0.3);
    CHECK_NEAR(w.speedHz, 2.25);
    CHECK(w.source == SpreadSource::StageAngle);
    CHECK_EQ(w.mods.blocks, 2);
    CHECK_EQ(w.mods.wings, 3);
    CHECK_NEAR(w.mods.groups, 1.5);
    CHECK_NEAR(w.mods.offset, 0.125);
    CHECK(w.direction == Direction::Bounce);
    CHECK(w.pivot == BouncePivot::CentreAtEnd);
    CHECK_NEAR(w.overshoot, 0.4);
    CHECK(w.gap.enabled);
    CHECK(w.gap.hold == GapHold::HoldLast);
    CHECK(w.gap.resume == GapResume::Continue);
    CHECK_EQ(w.gap.sequence.size(), size_t(4));  // Stutter's length sequence
    CHECK_NEAR(w.gap.sequence[1], 0.25);
    CHECK_EQ(w.variation.seed, uint64_t(4242));
    CHECK_NEAR(w.variation.size, 0.3);
    CHECK(w.stack == StackMode::Add);
    CHECK(w.clock == ClockSource::Playback);
    CHECK_EQ(w.targets.size(), size_t(3));
    CHECK_EQ(w.targets[2].fixture, FixtureId(3));

    const Phaser& st = sb->cues[1].phasers[0];
    CHECK(st.gen.kind == GenKind::Steps);
    CHECK(st.role == Role::AxisX);
    CHECK_EQ(st.gen.steps.size(), size_t(2));
    CHECK_NEAR(st.gen.steps[0].attack, 0.1);
    CHECK_NEAR(st.gen.steps[0].decayCurve, -0.5);

    const Phaser& cv = sb->cues[2].phasers[0];
    CHECK(cv.gen.kind == GenKind::Curve);
    CHECK_EQ(cv.gen.curve.size(), size_t(3));
    CHECK_NEAR(cv.gen.curve[1].y, 1.0);
    CHECK_NEAR(cv.gen.curve[1].inDx, -0.2);
    CHECK_EQ(cv.targets[0].cell, CellId(5));

    // --- executors --------------------------------------------------------
    const Executor* x1 = b.executor(1);
    CHECK(x1 != nullptr);
    CHECK(x1->label == "Main go");
    CHECK_EQ(x1->priority, 7);
    CHECK(x1->merge == MergeRule::HTP);
    CHECK_EQ(x1->swapGroup, 2);
    CHECK_NEAR(x1->master, 0.65);
    CHECK(x1->release == ReleaseMode::HoldLast);
    CHECK_NEAR(x1->releaseTime, 4.5);
    CHECK(x1->mergeByGroup[static_cast<size_t>(Group::Color)].has_value());
    CHECK(*x1->mergeByGroup[static_cast<size_t>(Group::Color)] == MergeRule::LTP);
    CHECK(!x1->mergeByGroup[static_cast<size_t>(Group::Beam)].has_value());
    CHECK(*x1->releaseByGroup[static_cast<size_t>(Group::Intensity)] == ReleaseMode::ToZero);
    CHECK(b.executor(5) != nullptr);
    CHECK_EQ(b.executor(5)->priority, -3);

    // --- engine settings --------------------------------------------------
    CHECK_NEAR(b.grandMaster, 0.8);
    CHECK(b.unusedMode == UnusedMode::ToDefault);
}

static void testLoadedShowRuns() {
    // The real test of a round trip is that the loaded show BEHAVES the same.
    const std::string path = tmpDir() + "/runs.lumen";
    fs::remove(path);

    Engine a;
    buildShow(a);
    CHECK(ShowFile::save(a, path).ok);

    Engine b;
    CHECK(ShowFile::load(b, path).ok);
    b.setClock(0.0);

    Sequence* s = b.sequence(1);
    for (auto& c : s->cues) c.timing.fadeIn = 0.0;
    Executor* e = b.executor(1);
    e->master = 1.0f;

    // Cue 1 carries a Dimmer phaser, so the dimmer must be MOVING rather than
    // parked at the stored value — proof the phaser survived the round trip
    // and is running, not just stored.
    e->gotoCue(*s, b.patch, 0, 0.0);
    b.setClock(0.0);
    b.tick();
    const float d0 = *b.merged().get(makeKey(1, 0, Attr::Dimmer));
    b.setClock(0.17);
    b.tick();
    const float d1 = *b.merged().get(makeKey(1, 0, Attr::Dimmer));
    CHECK(std::fabs(d0 - d1) > 1e-4);
    CHECK(d0 >= 0.f && d0 <= 1.f);

    // Cue 2's phaser targets Role::AxisX, which resolves to Pan on a moving
    // spot — so role binding also survived (SPEC 4.2).
    e->gotoCue(*s, b.patch, 1, 0.0);
    b.tick();
    CHECK(b.merged().get(makeKey(2, 0, Attr::Pan)).has_value());

    // Now strip the phasers and the stored values show through, which is what
    // verifies tracking after a load.
    for (auto& c : s->cues) c.phasers.clear();
    e->gotoCue(*s, b.patch, 0, 0.0);
    b.tick();
    CHECK_NEAR(*b.merged().get(makeKey(1, 0, Attr::Dimmer)), 1.0);

    e->gotoCue(*s, b.patch, 1, 0.0);
    b.tick();
    CHECK_NEAR(*b.merged().get(makeKey(2, 0, Attr::Pan)), 0.25);
    // Cue 1's dimmer tracks into cue 2, which stored only Pan and Red.
    CHECK_NEAR(*b.merged().get(makeKey(1, 0, Attr::Dimmer)), 1.0);
    // Cue 2 blocks Colour, so its own red wins over cue 1's preset reference.
    CHECK_NEAR(*b.merged().get(makeKey(1, 0, Attr::Red)), 0.9);

    // And the command layer drives the loaded show.
    Console con(b);
    CHECK(con.execute("Fixture 1 At 50").ok);
    b.tick();
    CHECK_NEAR(*b.merged().get(makeKey(1, 0, Attr::Dimmer)), 0.5);
}

static void testUidsNotReused() {
    // §8.1: a session that opens an existing show must not re-issue one of its
    // UIDs, or a new object could collide with a stored reference.
    const std::string path = tmpDir() + "/uids.lumen";
    fs::remove(path);

    Engine a;
    buildShow(a);
    CHECK(ShowFile::save(a, path).ok);

    Engine b;
    CHECK(ShowFile::load(b, path).ok);

    std::set<uint64_t> loaded;
    for (const auto& f : b.patch.fixtures()) loaded.insert(f.uid.v);
    for (const auto& s : b.sequences) {
        loaded.insert(s.uid.v);
        for (const auto& c : s.cues) loaded.insert(c.uid.v);
    }
    for (const auto& e : b.executors) loaded.insert(e.uid.v);
    CHECK(loaded.size() > 8);

    for (int i = 0; i < 5000; ++i) {
        const Uid fresh = b.uids.next();
        CHECK(loaded.count(fresh.v) == 0);
    }
}

static void testAtomicity() {
    // §17.7: the deciding reason for SQLite is that a crash mid-save must not
    // destroy the show. A save that fails must leave the previous file intact.
    const std::string path = tmpDir() + "/atomic.lumen";
    fs::remove(path);

    Engine a;
    buildShow(a);
    CHECK(ShowFile::save(a, path, "First").ok);
    const auto firstSize = fs::file_size(path);
    CHECK(firstSize > 0);

    // A save to an unwritable location must fail cleanly and leave the good
    // file untouched.
    SaveReport bad = ShowFile::save(a, "/proc/lumen-cannot-write-here.lumen");
    CHECK(!bad.ok);
    CHECK(!bad.error.empty());
    CHECK(fs::exists(path));
    CHECK_EQ(static_cast<int>(fs::file_size(path)), static_cast<int>(firstSize));

    // The original still loads and still says what it said.
    Engine b;
    LoadReport lr = ShowFile::load(b, path);
    CHECK(lr.ok);
    CHECK(lr.showName == "First");

    // A truncated file is rejected rather than half-loaded.
    const std::string broken = tmpDir() + "/broken.lumen";
    {
        std::ofstream o(broken, std::ios::binary);
        o << "SQLite format 3\0this is not a database";
    }
    Engine c;
    installStarterTypes(c.patch);
    const size_t before = c.patch.typeCount();
    LoadReport br = ShowFile::load(c, broken);
    CHECK(!br.ok);
    CHECK(!br.error.empty());
    // The engine is left as it was, not half-replaced.
    CHECK_EQ(c.patch.typeCount(), static_cast<int>(before));
}

static void testSchemaVersionGuard() {
    const std::string path = tmpDir() + "/future.lumen";
    fs::remove(path);

    Engine a;
    buildShow(a);
    CHECK(ShowFile::save(a, path).ok);

    // Forge a newer schema version. A build must refuse a file it cannot
    // reliably read rather than silently misreading it.
    {
        Engine tmp;
        (void)tmp;
    }
    sqlite3* raw = nullptr;
    CHECK_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
    const std::string sql = "UPDATE meta SET value='" +
                            std::to_string(kShowSchemaVersion + 1) +
                            "' WHERE key='schema_version'";
    sqlite3_exec(raw, sql.c_str(), nullptr, nullptr, nullptr);
    sqlite3_close(raw);

    Engine b;
    LoadReport lr = ShowFile::load(b, path);
    CHECK(!lr.ok);
    CHECK(lr.error.find("newer version") != std::string::npos);

    // But inspect() still reads the header, so a recovery prompt can describe
    // a file it cannot open.
    LoadReport ins = ShowFile::inspect(path);
    CHECK(ins.ok);
    CHECK_EQ(ins.schemaVersion, kShowSchemaVersion + 1);
    CHECK_EQ(ins.fixtures, 5);
}

static void testInspect() {
    const std::string path = tmpDir() + "/inspect.lumen";
    fs::remove(path);

    Engine a;
    buildShow(a);
    CHECK(ShowFile::save(a, path, "Describe Me").ok);

    // Crash recovery needs to describe a file without loading it.
    LoadReport r = ShowFile::inspect(path);
    CHECK(r.ok);
    CHECK(r.showName == "Describe Me");
    CHECK_EQ(r.fixtures, 5);
    CHECK_EQ(r.sequences, 1);
    CHECK_EQ(r.cues, 3);
    CHECK_EQ(r.executors, 2);

    CHECK(!ShowFile::inspect(tmpDir() + "/does-not-exist.lumen").ok);
}

static void testAutosaveRotation() {
    const std::string dir = tmpDir() + "/auto";
    fs::remove_all(dir);

    Engine a;
    buildShow(a);
    Autosave au(dir, "show", 3);

    // §17.8: rolling versions, not one overwritten file, so a bad save cannot
    // take the show with it.
    CHECK(au.write(a, "first").ok);
    CHECK_EQ(au.versions().size(), size_t(1));
    CHECK(au.write(a, "second").ok);
    CHECK_EQ(au.versions().size(), size_t(2));
    CHECK(au.write(a, "third").ok);
    CHECK_EQ(au.versions().size(), size_t(3));

    // Rotation discards only the oldest, keeping the set at `keep`.
    CHECK(au.write(a, "fourth").ok);
    CHECK_EQ(au.versions().size(), size_t(3));

    // The newest is loadable, which is what crash recovery offers first.
    const std::string newest = au.newest();
    CHECK(!newest.empty());
    Engine b;
    CHECK(ShowFile::load(b, newest).ok);
    CHECK_EQ(b.patch.size(), size_t(5));

    // A deliberate snapshot sits outside the rolling set, so rotation cannot
    // discard it.
    CHECK(au.snapshot(a, "before-rig-change").ok);
    for (int i = 0; i < 6; ++i) CHECK(au.write(a, "churn").ok);
    CHECK(fs::exists(dir + "/show.snap-before-rig-change.lumen"));
    CHECK_EQ(au.versions().size(), size_t(3));
}

static void testEmptyShow() {
    // An empty show must save and load without special-casing.
    const std::string path = tmpDir() + "/empty.lumen";
    fs::remove(path);

    Engine a;
    SaveReport sr = ShowFile::save(a, path, "Empty");
    CHECK(sr.ok);
    CHECK_EQ(sr.fixtures, 0);
    CHECK_EQ(sr.types, 0);

    Engine b;
    installStarterTypes(b.patch);
    LoadReport lr = ShowFile::load(b, path);
    CHECK(lr.ok);
    CHECK_EQ(b.patch.size(), size_t(0));
    CHECK_EQ(b.patch.typeCount(), 0);  // the loaded show replaces, not merges
}

// Keeping both styles is only real if a page's style survives a save: a show
// that forgets which of its surfaces was a wing has not kept both, it has
// kept one and guessed.
static void testSurfacesRoundTrip() {
    const std::string path = tmpDir() + "/surfaces.lumen";
    fs::remove(path);

    Engine a;
    buildShow(a);
    installStarterSurfaces(a.surfaces);

    // A wing page with a gap in it: the empty slot between two tiles is part
    // of the layout, not an absence, so it has to come back the same.
    a.surfaces.placeInSlot(1, 0, TileKind::Executor, a.uids.next());
    a.surfaces.placeInSlot(1, 4, TileKind::Group, a.uids.next());
    a.surfaces.byId(1)->tiles[1].legendOverride = "BLINDER";

    // A canvas page with tiles of different sizes, which is the whole claim
    // of that style.
    a.surfaces.placeAt(2, Rect{0, 0, 3, 2}, TileKind::Executor, a.uids.next());
    a.surfaces.placeAt(2, Rect{3, 0, 2, 1}, TileKind::Executor, a.uids.next());
    a.surfaces.setCurrent(2);

    CHECK(ShowFile::save(a, path, "Surfaces").ok);

    Engine b;
    const LoadReport lr = ShowFile::load(b, path);
    CHECK(lr.ok);
    CHECK(lr.warnings.empty());
    CHECK_EQ(b.surfaces.size(), size_t(2));

    const Surface* wing = b.surfaces.byId(1);
    CHECK(wing != nullptr);
    CHECK(wing->style == SurfaceStyle::Wing);
    CHECK(wing->name == "Wing");
    CHECK_EQ(wing->cols, 10);
    CHECK_EQ(wing->tiles.size(), size_t(2));
    CHECK(wing->atSlot(0) != nullptr);
    CHECK(wing->atSlot(4) != nullptr);
    // The gap is still a gap.
    CHECK(wing->atSlot(1) == nullptr);
    CHECK(wing->atSlot(4)->legendOverride == "BLINDER");
    CHECK(wing->atSlot(4)->kind == TileKind::Group);

    const Surface* canvas = b.surfaces.byId(2);
    CHECK(canvas != nullptr);
    CHECK(canvas->style == SurfaceStyle::Canvas);
    CHECK_EQ(canvas->tiles.size(), size_t(2));
    CHECK_EQ(canvas->tiles[0].rect.w, 3);
    CHECK_EQ(canvas->tiles[0].rect.h, 2);
    CHECK_EQ(canvas->tiles[1].rect.x, 3);

    // Targets are UIDs, so they survive identically — the legend is a view
    // of the object, not a copy of its name (§8.1, §9.2).
    CHECK(canvas->tiles[0].target == a.surfaces.byId(2)->tiles[0].target);
    CHECK(wing->atSlot(0)->uid == a.surfaces.byId(1)->atSlot(0)->uid);

    // A reloaded page still passes §9.1.
    CHECK(b.surfaces.validate(1).empty());
    CHECK(b.surfaces.validate(2).empty());

    // And converting after a load behaves the same as converting before one.
    std::string err;
    const ConvertReport rep = b.surfaces.setStyle(2, SurfaceStyle::Wing, false, &err);
    CHECK(!rep.lossless);
    CHECK_EQ(rep.resized, 2);
    CHECK(err.empty());
    CHECK(b.surfaces.byId(2)->style == SurfaceStyle::Wing);

    fs::remove(path);
}


// §14.6.1: a laser ZONE is show data and travels with the show. What must
// never travel with it is arming — a file that could restore a live beam
// would make opening one a thing you had to be ready for.
static void testLaserZonesRoundTripButNeverArmed() {
    const std::string path = tmpDir() + "/laser.lumen";
    fs::remove(path);

    Engine a;
    buildShow(a);
    ZoneSafety& house = a.laser.addZone("House");
    house.mask.enabled = true;
    house.mask.minX = -0.8f;
    house.mask.maxX = 0.8f;
    house.mask.minY = 0.2f;        // nothing below head height
    house.mask.maxY = 1.0f;
    house.maxPower = 0.45f;
    house.stallTimeout = 0.08;

    ZoneSafety& aud = a.laser.addZone("Audience");
    aud.audienceScanning = true;
    aud.audienceOptIn = true;
    aud.audienceAttenuation = 0.04f;

    // Arm one of them, so the file is written while a beam is live.
    double clock = 0.0;
    a.laser.setClock([&clock] { return clock; });
    a.laser.setArmHold(1.0);
    CHECK(a.laser.beginArm(house.id, SafetyGate::ArmSource::Panel));
    clock = 2.0;
    CHECK(a.laser.confirmArm(house.id, SafetyGate::ArmSource::Panel));
    CHECK(a.laser.zone(house.id)->armed);

    CHECK(ShowFile::save(a, path, "Laser").ok);

    Engine b;
    const LoadReport lr = ShowFile::load(b, path);
    CHECK(lr.ok);
    CHECK_EQ(b.laser.zones().size(), size_t(2));

    const ZoneSafety* h = b.laser.zone(house.id);
    CHECK(h != nullptr);
    CHECK(h && h->name == "House");
    CHECK(h && h->mask.enabled);
    CHECK_NEAR(h ? h->mask.minY : 0.f, 0.2f);
    CHECK_NEAR(h ? h->maxPower : 0.f, 0.45f);
    CHECK_NEAR(static_cast<float>(h ? h->stallTimeout : 0.0), 0.08f);

    // THE POINT: it was armed when saved, and it is not armed now.
    CHECK(h && !h->armed);
    for (const ZoneSafety& z : b.laser.zones()) CHECK(!z.armed);
    CHECK(b.laser.failSafeCount() > 0);
    CHECK(b.laser.lastFailSafe() == "show loaded");

    // The audience flags survive, because they are the ones a venue signed
    // off on and re-entering them from memory is how they get entered wrong.
    const ZoneSafety* au = b.laser.zone(aud.id);
    CHECK(au && au->audienceScanning);
    CHECK(au && au->audienceOptIn);
    CHECK_NEAR(au ? au->audienceAttenuation : 0.f, 0.04f);

    // And a loaded zone keeps its id, so a show's own numbering survives.
    CHECK(b.laser.zone(aud.id) != nullptr);

    fs::remove(path);
}


// §13.3: a mapping is hours of work and belongs to the rig, so the layer and
// its sample points travel with the show. What must NOT travel with it is
// any trace of the raster — §13.1's boundary runs one way, and baking two
// million sampled values into a file is exactly what that rule prevents.
static void testExtractionRoundTrip() {
    const std::string path = tmpDir() + "/mapping.lumen";
    fs::remove(path);

    Engine a;
    buildShow(a);

    Extraction wall;
    wall.name = "Back Wall";
    wall.priority = 7;
    wall.merge = MergeRule::HTP;
    wall.master = 0.6f;
    std::vector<Target> cells;
    for (CellId c = 1; c <= 12; ++c) cells.push_back({201, c});
    wall.setPoints(layoutLine(cells, 0.05f, 0.5f, 0.95f, 0.5f));
    for (SamplePoint& p : wall.points) {
        p.area = SampleArea::Gaussian;
        p.radius = 0.03f;
    }
    wall.points[3].enabled = false;     // one point deliberately off
    a.extractions.add(wall);

    Extraction off;
    off.name = "Spare";
    off.enabled = false;
    a.extractions.add(off);

    // Give the engine a frame, so the save has a raster in front of it and
    // the absence of raster columns is a real result rather than a vacuous
    // one.
    a.frame.resize(64, 64);
    a.frame.fill(255, 128, 0);

    CHECK(ShowFile::save(a, path, "Mapping").ok);

    Engine b;
    const LoadReport lr = ShowFile::load(b, path);
    CHECK(lr.ok);
    CHECK(lr.warnings.empty());
    CHECK_EQ(b.extractions.size(), size_t(2));

    const Extraction* w = b.extractions.byName("Back Wall");
    CHECK(w != nullptr);
    CHECK(w && w->priority == 7);
    CHECK(w && w->merge == MergeRule::HTP);
    CHECK_NEAR(w ? w->master : 0.f, 0.6f);
    CHECK(w && w->enabled);
    CHECK_EQ(w ? w->points.size() : 0u, size_t(12));

    // Per-cell targets survive, which is what makes one patch entry carry
    // twelve sample points (§1.3).
    CHECK(w && w->pointFor({201, 1}) != nullptr);
    CHECK(w && w->pointFor({201, 12}) != nullptr);
    CHECK_NEAR(w && w->pointFor({201, 1}) ? w->pointFor({201, 1})->x : 0.f, 0.05f);
    CHECK(w && w->pointFor({201, 1})->area == SampleArea::Gaussian);
    CHECK_NEAR(w && w->pointFor({201, 1}) ? w->pointFor({201, 1})->radius : 0.f, 0.03f);

    // Point ORDER survives, because it is selection order and selection
    // order is information everywhere else (§20.4).
    for (size_t i = 1; i < w->points.size(); ++i)
        CHECK(w->points[i].x > w->points[i - 1].x);

    // A disabled point stays disabled, and a disabled LAYER stays disabled.
    CHECK(w && !w->points[3].enabled);
    const Extraction* sp = b.extractions.byName("Spare");
    CHECK(sp && !sp->enabled);

    // THE POINT: no raster came back. A loaded show has a mapping and no
    // pixels, which is the §13.1 boundary holding across a file.
    CHECK(b.frame.empty());

    // And the restored mapping works: give the new engine a frame and the
    // same fixtures light.
    b.frame.resize(64, 64);
    b.frame.fill(255, 128, 0);
    OutputLayer out;
    const ExtractionReport rep = b.extractions.mergeInto(b.frame, out);
    CHECK_EQ(rep.sampled, 11);       // twelve points, one disabled
    CHECK_EQ(rep.disabled, 1);
    CHECK(out.get(makeKey(201, 1, Attr::Red)).has_value());

    fs::remove(path);
}

int main() {
    std::printf("lumen show\n");
    testRoundTrip();
    testGroupsPresetsRecipesRoundTrip();
    testSurfacesRoundTrip();
    testLaserZonesRoundTripButNeverArmed();
    testExtractionRoundTrip();
    testShowFilesThroughTheConsole();
    testLoadedShowRuns();
    testUidsNotReused();
    testAtomicity();
    testSchemaVersionGuard();
    testInspect();
    testAutosaveRotation();
    testEmptyShow();
    return check::report("show");
}
