// Tests for selection order, spread sources, modifiers and the Clear machine.
#include "Check.hpp"

#include "lumen/Patch.hpp"
#include "lumen/Programmer.hpp"
#include "lumen/Selection.hpp"

#include <cmath>
#include <set>

using namespace lumen;

namespace {

struct Rig {
    UidGen gen{42};
    Patch patch{gen};

    Rig(int count = 8) {
        installStarterTypes(patch);
        const int spot = patch.findType("Moving Spot");
        std::string err;
        patch.addRange(1, count, spot, 1, 1, 0, &err);
    }
    std::vector<Target> targets(std::initializer_list<FixtureId> ids) {
        std::vector<Target> out;
        for (FixtureId id : ids) out.push_back({id, 0});
        return out;
    }
};

} // namespace

static void testSelectionOrder() {
    Rig rig;
    Selection s;

    // §20.4: capture order is information. 1 + 5 + 3 must not re-sort.
    s.set(rig.targets({1, 5, 3}));
    auto flat = s.flatten();
    CHECK_EQ(flat.size(), size_t(3));
    CHECK_EQ(flat[0].fixture, FixtureId(1));
    CHECK_EQ(flat[1].fixture, FixtureId(5));
    CHECK_EQ(flat[2].fixture, FixtureId(3));

    // Duplicates are not added twice.
    s.add(rig.targets({5}));
    CHECK_EQ(s.flatten().size(), size_t(3));

    s.reverse();
    CHECK_EQ(s.flatten()[0].fixture, FixtureId(3));

    // Explicit sorts are available but never implicit.
    s.sortBy(rig.patch, "id");
    flat = s.flatten();
    CHECK_EQ(flat[0].fixture, FixtureId(1));
    CHECK_EQ(flat[1].fixture, FixtureId(3));
    CHECK_EQ(flat[2].fixture, FixtureId(5));

    // Odd/even and every-Nth.
    Selection t;
    t.set(rig.targets({1, 2, 3, 4, 5, 6, 7, 8}));
    Selection odd = t;
    odd.keepOdd();
    CHECK_EQ(odd.flatten().size(), size_t(4));
    CHECK_EQ(odd.flatten()[0].fixture, FixtureId(1));
    CHECK_EQ(odd.flatten()[1].fixture, FixtureId(3));

    Selection third = t;
    third.keepEveryNth(3);
    CHECK_EQ(third.flatten().size(), size_t(3));  // 1, 4, 7

    // Remove prunes, toggle round-trips.
    Selection u = t;
    u.remove({4, 0});
    CHECK_EQ(u.flatten().size(), size_t(7));
    CHECK(!u.contains({4, 0}));
    u.toggle({4, 0});
    CHECK(u.contains({4, 0}));
}

static void testHierarchicalSelection() {
    Rig rig(12);
    Selection s;

    // §20.8: selection is a TREE. Flattening on selection would lose the
    // structure that multi-level fan needs.
    s.addGroup(Uid{101}, "Truss A", rig.targets({1, 2, 3}));
    s.addGroup(Uid{102}, "Truss B", rig.targets({4, 5, 6}));
    s.add(rig.targets({7}));

    // Flattens in order, each group contributing its own order.
    const auto flat = s.flatten();
    CHECK_EQ(flat.size(), size_t(7));
    CHECK_EQ(flat[0].fixture, FixtureId(1));
    CHECK_EQ(flat[3].fixture, FixtureId(4));
    CHECK_EQ(flat[6].fixture, FixtureId(7));

    // But the hierarchy survives for multi-level fan.
    const auto groups = s.groupsAtTopLevel();
    CHECK_EQ(groups.size(), size_t(3));
    CHECK_EQ(groups[0].size(), size_t(3));
    CHECK_EQ(groups[1].size(), size_t(3));
    CHECK_EQ(groups[2].size(), size_t(1));
    CHECK_EQ(s.depth(), size_t(2));

    // Reversing reverses groups and their contents.
    s.reverse();
    CHECK_EQ(s.flatten()[0].fixture, FixtureId(7));
    CHECK_EQ(s.groupsAtTopLevel().size(), size_t(3));
}

static void testSpreadSources() {
    Rig rig(8);
    Selection s;
    s.set(rig.targets({1, 2, 3, 4, 5, 6, 7, 8}));

    // Index: even distribution over 0..1, exclusive of 1.
    SpreadTable idx;
    idx.build(s, rig.patch, SpreadSource::Index);
    CHECK_EQ(idx.size(), size_t(8));
    CHECK_NEAR(idx.raw({1, 0}), 0.0);
    CHECK_NEAR(idx.raw({3, 0}), 0.25);
    CHECK_NEAR(idx.raw({5, 0}), 0.5);
    CHECK_NEAR(idx.raw({8, 0}), 0.875);

    // Shuffle is a permutation: every bucket used exactly once (§4.12).
    SpreadTable sh;
    sh.build(s, rig.patch, SpreadSource::Shuffle, 1234);
    std::set<int> buckets;
    for (const Target& t : sh.order()) buckets.insert(static_cast<int>(sh.raw(t) * 8.f + 0.5f));
    CHECK_EQ(buckets.size(), size_t(8));

    // Seeded, so it replays identically.
    SpreadTable sh2;
    sh2.build(s, rig.patch, SpreadSource::Shuffle, 1234);
    for (const Target& t : sh.order()) CHECK_NEAR(sh.raw(t), sh2.raw(t));
}

static void testStageSpread() {
    UidGen gen{7};
    Patch patch{gen};
    installStarterTypes(patch);
    const int spot = patch.findType("Moving Spot");
    std::string err;
    patch.addRange(1, 4, spot, 1, 1, 0, &err);

    // Place four fixtures on a circle: east, north, west, south.
    struct P { FixtureId id; float x, y; };
    const P pts[] = {{1, 1.f, 0.f}, {2, 0.f, 1.f}, {3, -1.f, 0.f}, {4, 0.f, -1.f}};
    for (const auto& p : pts) {
        auto* f = const_cast<Fixture*>(patch.find(p.id));
        f->x = p.x;
        f->y = p.y;
        f->positioned = true;
    }

    Selection s;
    s.set({{1, 0}, {2, 0}, {3, 0}, {4, 0}});

    // §20.2: a circular chase across a physically circular rig needs
    // StageAngle, and no linear ordering produces it.
    SpreadTable ang;
    ang.build(s, patch, SpreadSource::StageAngle);
    CHECK_NEAR(ang.raw({1, 0}), 0.0);    // east  = 0
    CHECK_NEAR(ang.raw({2, 0}), 0.25);   // north = 90
    CHECK_NEAR(ang.raw({3, 0}), 0.5);    // west  = 180
    CHECK_NEAR(ang.raw({4, 0}), 0.75);   // south = 270

    // StageX normalises against the actual extent, so rig scale is irrelevant.
    SpreadTable sx;
    sx.build(s, patch, SpreadSource::StageX);
    CHECK_NEAR(sx.raw({3, 0}), 0.0);   // x = -1 is the minimum
    CHECK_NEAR(sx.raw({1, 0}), 1.0);   // x = +1 is the maximum
    CHECK_NEAR(sx.raw({2, 0}), 0.5);   // x = 0 sits halfway

    SpreadTable rad;
    rad.build(s, patch, SpreadSource::StageRadial, 0, 0.f, 0.f);
    for (FixtureId id = 1; id <= 4; ++id) CHECK_NEAR(rad.raw({id, 0}), 1.0);
}

static void testModifiers() {
    const size_t n = 8;

    // Groups: how many full phase cycles across the selection.
    SpreadModifiers g;
    g.groups = 2.f;
    CHECK_NEAR(applyModifiers(0.00f, g, n), 0.0);
    CHECK_NEAR(applyModifiers(0.25f, g, n), 0.5);
    CHECK_NEAR(applyModifiers(0.50f, g, n), 0.0);  // wrapped

    // Offset rotates the whole spread.
    SpreadModifiers o;
    o.offset = 0.25f;
    CHECK_NEAR(applyModifiers(0.0f, o, n), 0.25);
    CHECK_NEAR(applyModifiers(0.8f, o, n), 0.05);  // wrapped

    // Blocks: N adjacent members share one phase. 8 members, blocks of 2 ->
    // four distinct phases, adjacent pairs matching.
    SpreadModifiers b;
    b.blocks = 2;
    const float p0 = applyModifiers(0.000f, b, n);
    const float p1 = applyModifiers(0.125f, b, n);
    const float p2 = applyModifiers(0.250f, b, n);
    CHECK_NEAR(p0, p1);          // members 0 and 1 share
    CHECK(std::fabs(p2 - p1) > 1e-6);  // member 2 starts a new block
    CHECK_NEAR(p2, 0.25);

    std::set<int> distinct;
    for (size_t i = 0; i < n; ++i)
        distinct.insert(static_cast<int>(
            applyModifiers(static_cast<float>(i) / n, b, n) * 1000.f + 0.5f));
    CHECK_EQ(distinct.size(), size_t(4));

    // Wings: mirror the spread into N symmetric wings. The property that
    // matters is symmetry about the fold, so assert that rather than raw
    // numbers — phase is cyclic, so the fold point lands on 1.0 which
    // normalises to 0.0, and any periodic waveform evaluates both identically.
    SpreadModifiers w;
    w.wings = 2;
    CHECK_NEAR(applyModifiers(0.00f, w, n), 0.0);
    CHECK_NEAR(applyModifiers(0.25f, w, n), 0.5);
    CHECK_NEAR(applyModifiers(0.75f, w, n), 0.5);  // folded back — mirrored

    // Mirror symmetry: s and (1 - s) must land on the same phase.
    for (int i = 1; i < 8; ++i) {
        const float s = static_cast<float>(i) / 16.f;
        CHECK_NEAR(applyModifiers(s, w, n), applyModifiers(1.f - s, w, n));
    }

    // Four wings fold twice as often, so quarter-points coincide.
    SpreadModifiers w4;
    w4.wings = 4;
    CHECK_NEAR(applyModifiers(0.125f, w4, n), 0.5);
    CHECK_NEAR(applyModifiers(0.375f, w4, n), 0.5);

    // Everything off is the identity, so an unmodified phaser is unchanged.
    SpreadModifiers none;
    for (size_t i = 0; i < n; ++i) {
        const float s = static_cast<float>(i) / n;
        CHECK_NEAR(applyModifiers(s, none, n), s);
    }
}

static void testCellResolution() {
    UidGen gen{3};
    Patch patch{gen};
    installStarterTypes(patch);
    std::string err;
    CHECK(patch.add({1, "", patch.findType("Pixel Bar 12"), 1, 1}, &err));

    Programmer prog(patch);
    prog.selection().set({{1, 0}});

    // §1.3: the master dimmer lives on the parent, so Dimmer writes to cell 0.
    prog.setAttr(Attr::Dimmer, 1.0f);
    CHECK(prog.values().has(1, 0, Attr::Dimmer));
    CHECK_EQ(prog.values().size(), size_t(1));

    // Colour exists only on cells, so it broadcasts to all twelve.
    prog.setAttr(Attr::Red, 0.5f);
    CHECK_EQ(prog.values().keys(Group::Color).size(), size_t(12));
    CHECK(prog.values().has(1, 1, Attr::Red));
    CHECK(prog.values().has(1, 12, Attr::Red));
    CHECK(!prog.values().has(1, 0, Attr::Red));

    // Selecting one cell addresses only that cell.
    Programmer p2(patch);
    p2.selection().set({{1, 3}});
    p2.setAttr(Attr::Red, 1.0f);
    CHECK_EQ(p2.values().size(), size_t(1));
    CHECK(p2.values().has(1, 3, Attr::Red));

    // An attribute the type does not have writes nothing — the per-fixture
    // skip the FX role rules depend on (§4.2).
    Programmer p3(patch);
    p3.selection().set({{1, 0}});
    p3.setAttr(Attr::Prism1, 1.0f);
    CHECK_EQ(p3.values().size(), size_t(0));
}

static void testClearMachine() {
    Rig rig(4);
    Programmer prog(rig.patch);

    // §11.8: Clear resolves against programmer STATE, not a press counter.
    CHECK(prog.clear() == ClearResult::CommandLine);  // nothing at all

    prog.selection().set(rig.targets({1, 2}));
    prog.setAttr(Attr::Dimmer, 1.0f);
    prog.setAttr(Attr::Pan, 0.5f);
    CHECK_EQ(prog.values().size(), size_t(4));

    // 1st: deselect. Values stay and keep outputting.
    CHECK(prog.clear() == ClearResult::Selection);
    CHECK(prog.selection().empty());
    CHECK_EQ(prog.values().size(), size_t(4));

    // 2nd: deactivate. Values REMAIN in the programmer and still output.
    CHECK(prog.clear() == ClearResult::Active);
    CHECK_EQ(prog.values().size(), size_t(4));
    CHECK(!prog.values().hasActive());
    CHECK_EQ(prog.storeLayer().size(), size_t(0));  // but Store ignores them

    // 3rd: empty the programmer.
    CHECK(prog.clear() == ClearResult::All);
    CHECK_EQ(prog.values().size(), size_t(0));

    // 4th on an empty programmer: command-line escape.
    CHECK(prog.clear() == ClearResult::CommandLine);

    // Repeated presses reproduce 1-2-3 without a counter, so re-selecting
    // mid-sequence correctly restarts at the selection stage.
    prog.selection().set(rig.targets({1}));
    prog.setAttr(Attr::Dimmer, 1.0f);
    CHECK(prog.clear() == ClearResult::Selection);
    prog.selection().set(rig.targets({2}));           // select again
    CHECK(prog.clear() == ClearResult::Selection);     // not Active
    CHECK(prog.clear() == ClearResult::Active);

    // Shift+Clear removes references, leaving hard values.
    Programmer p2(rig.patch);
    p2.selection().set(rig.targets({1}));
    p2.setAttr(Attr::Dimmer, 1.0f);
    p2.setRef(Attr::Red, Uid{55}, Attr::Red, 1.0f);
    CHECK_EQ(p2.values().size(), size_t(2));
    CHECK(p2.clearPresets() == ClearResult::Presets);
    CHECK_EQ(p2.values().size(), size_t(1));
    CHECK(p2.values().has(1, 0, Attr::Dimmer));

    // Hold Clear empties everything at once.
    Programmer p3(rig.patch);
    p3.selection().set(rig.targets({1}));
    p3.setAttr(Attr::Dimmer, 1.0f);
    CHECK(p3.clearAll() == ClearResult::All);
    CHECK(p3.empty());
}

static void testHighlightAxes() {
    Rig rig(4);
    Programmer prog(rig.patch);
    prog.selection().set(rig.targets({1, 2}));

    // §20.7: two independent axes, not three modes.
    // highlight + unaffected == MA3 Highlight.
    prog.highlight = true;
    prog.unselected = Programmer::Unselected::Unaffected;
    ValueLayer h = prog.highlightLayer();
    CHECK_NEAR(*h.value(1, 0, Attr::Dimmer), 1.0);
    CHECK_NEAR(*h.value(2, 0, Attr::Dimmer), 1.0);
    CHECK(!h.has(3, 0, Attr::Dimmer));  // unselected untouched

    // highlight + blackout == ChamSys Solo.
    prog.unselected = Programmer::Unselected::Blackout;
    h = prog.highlightLayer();
    CHECK_NEAR(*h.value(1, 0, Attr::Dimmer), 1.0);
    CHECK_NEAR(*h.value(3, 0, Attr::Dimmer), 0.0);
    CHECK_NEAR(*h.value(4, 0, Attr::Dimmer), 0.0);

    // highlight + lowlight == focusing with context still visible.
    prog.unselected = Programmer::Unselected::Lowlight;
    prog.lowlightLevel = 0.2f;
    h = prog.highlightLayer();
    CHECK_NEAR(*h.value(1, 0, Attr::Dimmer), 1.0);
    CHECK_NEAR(*h.value(3, 0, Attr::Dimmer), 0.2);

    // Highlight is built fresh and never enters the programmer, so Store can
    // not capture it — the structural guarantee from §20.7.
    CHECK_EQ(prog.values().size(), size_t(0));
    CHECK_EQ(prog.storeLayer().size(), size_t(0));
}

int main() {
    std::printf("lumen programmer\n");
    testSelectionOrder();
    testHierarchicalSelection();
    testSpreadSources();
    testStageSpread();
    testModifiers();
    testCellResolution();
    testClearMachine();
    testHighlightAxes();
    return check::report("programmer");
}
