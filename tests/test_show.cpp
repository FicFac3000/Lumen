// Tests for show persistence: round-trip fidelity, atomicity, autosave.
#include "Check.hpp"

#include "lumen/Command.hpp"
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

    eng.grandMaster = 0.8f;
    eng.unusedMode = UnusedMode::ToDefault;
}

} // namespace

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

int main() {
    std::printf("lumen show\n");
    testRoundTrip();
    testLoadedShowRuns();
    testUidsNotReused();
    testAtomicity();
    testSchemaVersionGuard();
    testInspect();
    testAutosaveRotation();
    testEmptyShow();
    return check::report("show");
}
