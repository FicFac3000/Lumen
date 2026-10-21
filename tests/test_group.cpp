// Tests for groups: order, nesting, dynamic queries, and the sticky
// override semantics.
#include "Check.hpp"

#include "lumen/Engine.hpp"
#include "lumen/Group.hpp"

#include <set>

using namespace lumen;

namespace {

struct Rig {
    Engine eng;
    GroupStore groups{eng.uids};

    Rig() {
        installStarterTypes(eng.patch);
        std::string err;
        // Four moving spots in universe 1, four pars in universe 2.
        eng.patch.addRange(1, 4, eng.patch.findType("Moving Spot"), 1, 1, 0, &err);
        eng.patch.addRange(11, 4, eng.patch.findType("LED Par RGBW"), 2, 1, 0, &err);

        // Tags and positions, so the facets have something to match.
        for (FixtureId id : {1u, 2u}) tag(id, "upstage");
        for (FixtureId id : {3u, 4u}) tag(id, "downstage");
        for (FixtureId id = 1; id <= 4; ++id) pos(id, static_cast<float>(id), 0.f);
        eng.setClock(0.0);
    }

    void tag(FixtureId id, const std::string& t) {
        const_cast<Fixture*>(eng.patch.find(id))->tags.push_back(t);
    }
    void pos(FixtureId id, float x, float y) {
        auto* f = const_cast<Fixture*>(eng.patch.find(id));
        f->x = x;
        f->y = y;
        f->positioned = true;
    }
    Uid uidOf(FixtureId id) const { return eng.patch.find(id)->uid; }
};

std::vector<FixtureId> ids(const std::vector<Target>& ts) {
    std::vector<FixtureId> out;
    for (const Target& t : ts) out.push_back(t.fixture);
    return out;
}

} // namespace

static void testStaticOrder() {
    Rig r;

    // §20.4: capture order is information, because it drives FX spread and fan
    // direction. A static group must preserve exactly what was given.
    GroupDef g;
    g.name = "Odd order";
    g.members = {{3, 0}, {1, 0}, {4, 0}};
    const Uid uid = r.groups.add(g).uid;

    const auto res = r.groups.resolve(uid, r.eng.patch);
    CHECK_EQ(res.targets.size(), size_t(3));
    CHECK_EQ(res.targets[0].fixture, FixtureId(3));
    CHECK_EQ(res.targets[1].fixture, FixtureId(1));
    CHECK_EQ(res.targets[2].fixture, FixtureId(4));

    // Ids and UIDs are assigned on add, so a rename cannot break a reference.
    CHECK(uid.valid());
    CHECK_EQ(r.groups.find(uid)->id, 1);
    r.groups.find(uid)->name = "Renamed";
    CHECK(r.groups.find(uid) != nullptr);
    CHECK(r.groups.byName("Odd order") == nullptr);
}

static void testNestingComposesOrder() {
    Rig r;

    GroupDef a;
    a.name = "Truss A";
    a.members = {{1, 0}, {2, 0}};
    const Uid ua = r.groups.add(a).uid;

    GroupDef b;
    b.name = "Truss B";
    b.members = {{4, 0}, {3, 0}};  // deliberately reversed
    const Uid ub = r.groups.add(b).uid;

    GroupDef all;
    all.name = "All Trusses";
    const Uid uall = r.groups.add(all).uid;

    std::string err;
    CHECK(r.groups.nest(uall, ua, &err));
    CHECK(r.groups.nest(uall, ub, &err));

    // Each nested group contributes its OWN order, so nesting composes ordered
    // sets rather than discarding them.
    const auto res = r.groups.resolve(uall, r.eng.patch);
    CHECK_EQ(ids(res.targets), std::vector<FixtureId>({1, 2, 4, 3}));

    // §20.8: the selection it builds is a TREE, because multi-level fan needs
    // the hierarchy at apply time.
    const Selection sel = r.groups.buildSelection(uall, r.eng.patch);
    CHECK_EQ(sel.flatten().size(), size_t(4));
    CHECK_EQ(sel.depth(), size_t(2));
    const auto tops = sel.groupsAtTopLevel();
    CHECK_EQ(tops.size(), size_t(2));
    CHECK_EQ(tops[0].size(), size_t(2));
    CHECK_EQ(tops[1].size(), size_t(2));
    CHECK_EQ(tops[1][0].fixture, FixtureId(4));

    // Editing a nested group propagates to its parent — the point of nesting
    // rather than copying.
    r.groups.find(ua)->members.push_back({11, 0});
    CHECK_EQ(r.groups.resolve(uall, r.eng.patch).targets.size(), size_t(5));

    // Duplicates across children appear once.
    r.groups.find(ub)->members.push_back({1, 0});
    CHECK_EQ(r.groups.resolve(uall, r.eng.patch).targets.size(), size_t(5));
}

static void testCyclePrevention() {
    Rig r;
    GroupDef a;
    a.name = "A";
    const Uid ua = r.groups.add(a).uid;
    GroupDef b;
    b.name = "B";
    const Uid ub = r.groups.add(b).uid;
    GroupDef c;
    c.name = "C";
    const Uid uc = r.groups.add(c).uid;

    std::string err;
    CHECK(r.groups.nest(ua, ub, &err));
    CHECK(r.groups.nest(ub, uc, &err));

    // A containing B containing A is rejected at assignment, not discovered
    // as a hang (§20.8).
    CHECK(!r.groups.nest(uc, ua, &err));
    CHECK(err.find("cycle") != std::string::npos);
    CHECK(!r.groups.nest(ub, ua, &err));
    CHECK(!r.groups.nest(ua, ua, &err));  // self-nesting too

    CHECK(r.groups.wouldCycle(ua, ua));
    CHECK(r.groups.wouldCycle(uc, ua));
    CHECK(!r.groups.wouldCycle(uc, ub) == false);  // C->B would also cycle

    // Removing a group drops references from anything that nested it, so
    // nothing is left pointing at something gone.
    CHECK(r.groups.remove(uc));
    CHECK_EQ(r.groups.find(ub)->children.size(), size_t(0));
}

static void testDynamicQueries() {
    Rig r;

    // By type.
    GroupDef spots;
    spots.name = "All Spots";
    spots.kind = GroupKind::Dynamic;
    spots.query.typeName = "Moving Spot";
    const Uid us = r.groups.add(spots).uid;
    CHECK_EQ(ids(r.groups.resolve(us, r.eng.patch).targets),
             std::vector<FixtureId>({1, 2, 3, 4}));

    // By universe.
    GroupDef u2;
    u2.name = "Universe 2";
    u2.kind = GroupKind::Dynamic;
    u2.query.universe = 2;
    const Uid uu = r.groups.add(u2).uid;
    CHECK_EQ(r.groups.resolve(uu, r.eng.patch).targets.size(), size_t(4));

    // By tag.
    GroupDef up;
    up.name = "Upstage";
    up.kind = GroupKind::Dynamic;
    up.query.anyTag = {"upstage"};
    const Uid uup = r.groups.add(up).uid;
    CHECK_EQ(ids(r.groups.resolve(uup, r.eng.patch).targets),
             std::vector<FixtureId>({1, 2}));

    // §20.8's headline facet: "everything with a Prism". Derived from Lumen's
    // own taxonomy, so no hand-tagging.
    GroupDef prisms;
    prisms.name = "Has Prism";
    prisms.kind = GroupKind::Dynamic;
    prisms.query.hasAttr = Attr::Prism1;
    const Uid upr = r.groups.add(prisms).uid;
    CHECK_EQ(r.groups.resolve(upr, r.eng.patch).targets.size(), size_t(4));  // spots only

    // By kind: anything with an anchored attribute.
    GroupDef anchored;
    anchored.name = "Has Rotation";
    anchored.kind = GroupKind::Dynamic;
    anchored.query.hasKind = Kind::Anchored;
    const Uid uan = r.groups.add(anchored).uid;
    CHECK_EQ(r.groups.resolve(uan, r.eng.patch).targets.size(), size_t(4));

    // An id range, and an address range.
    GroupDef range;
    range.name = "Range";
    range.kind = GroupKind::Dynamic;
    range.query.idRange = {std::make_pair(FixtureId(2), FixtureId(3))};
    const Uid ur = r.groups.add(range).uid;
    CHECK_EQ(ids(r.groups.resolve(ur, r.eng.patch).targets),
             std::vector<FixtureId>({2, 3}));
}

static void testDynamicLiveUpdates() {
    Rig r;
    GroupDef spots;
    spots.name = "All Spots";
    spots.kind = GroupKind::Dynamic;
    spots.query.typeName = "Moving Spot";
    const Uid us = r.groups.add(spots).uid;

    CHECK_EQ(r.groups.resolve(us, r.eng.patch).targets.size(), size_t(4));

    // Patching a new matching fixture joins the group, with no edit to it.
    std::string err;
    CHECK(r.eng.patch.add({5, "", r.eng.patch.findType("Moving Spot"), 1, 100}, &err));
    CHECK_EQ(r.groups.resolve(us, r.eng.patch).targets.size(), size_t(5));

    // And unpatching one removes it.
    CHECK(r.eng.patch.remove(5));
    CHECK_EQ(r.groups.resolve(us, r.eng.patch).targets.size(), size_t(4));

    // A non-matching fixture does not join.
    CHECK(r.eng.patch.add({20, "", r.eng.patch.findType("Dimmer"), 3, 1}, &err));
    CHECK_EQ(r.groups.resolve(us, r.eng.patch).targets.size(), size_t(4));
}

static void testDynamicSortRule() {
    Rig r;

    // A dynamic group has no manual order to store, so the query carries a
    // sort rule (§20.8).
    GroupDef g;
    g.name = "Spots";
    g.kind = GroupKind::Dynamic;
    g.query.typeName = "Moving Spot";
    g.query.sortKey = "id";
    const Uid uid = r.groups.add(g).uid;
    CHECK_EQ(ids(r.groups.resolve(uid, r.eng.patch).targets),
             std::vector<FixtureId>({1, 2, 3, 4}));

    // Sorting by position gives a spatial order, which is what the stage-based
    // spread sources want.
    r.pos(1, 10.f, 0.f);
    r.pos(2, 5.f, 0.f);
    r.pos(3, 1.f, 0.f);
    r.pos(4, 7.f, 0.f);
    r.groups.find(uid)->query.sortKey = "x";
    CHECK_EQ(ids(r.groups.resolve(uid, r.eng.patch).targets),
             std::vector<FixtureId>({3, 2, 4, 1}));

    r.groups.find(uid)->query.sortKey = "patch";
    CHECK_EQ(ids(r.groups.resolve(uid, r.eng.patch).targets),
             std::vector<FixtureId>({1, 2, 3, 4}));
}

static void testOverridesOffByDefault() {
    Rig r;
    GroupDef g;
    g.name = "Spots";
    g.kind = GroupKind::Dynamic;
    g.query.typeName = "Moving Spot";
    const Uid uid = r.groups.add(g).uid;

    // §20.8: overrides are a per-group TOGGLE, off by default, so a dynamic
    // group stays a pure query unless the user opts in.
    CHECK(!r.groups.find(uid)->allowOverrides);

    std::string err;
    CHECK(!r.groups.exclude(uid, r.uidOf(2), &err));
    CHECK(err.find("not enabled") != std::string::npos);
    CHECK(!r.groups.include(uid, r.uidOf(11), &err));
    CHECK_EQ(r.groups.resolve(uid, r.eng.patch).targets.size(), size_t(4));

    // Overrides do not apply to a static group — edit it directly instead.
    GroupDef s;
    s.name = "Static";
    s.members = {{1, 0}};
    const Uid su = r.groups.add(s).uid;
    r.groups.find(su)->allowOverrides = true;
    CHECK(!r.groups.exclude(su, r.uidOf(1), &err));
    CHECK(err.find("static") != std::string::npos);
}

static void testStickyOverrides() {
    Rig r;
    GroupDef g;
    g.name = "Spots";
    g.kind = GroupKind::Dynamic;
    g.query.typeName = "Moving Spot";
    g.allowOverrides = true;
    const Uid uid = r.groups.add(g).uid;

    const Uid f2 = r.uidOf(2);
    std::string err;

    // Exclude one, and add something the query never matched.
    CHECK(r.groups.exclude(uid, f2, &err));
    CHECK(r.groups.include(uid, r.uidOf(11), &err));

    auto res = r.groups.resolve(uid, r.eng.patch);
    CHECK_EQ(ids(res.targets), std::vector<FixtureId>({1, 3, 4, 11}));
    CHECK_EQ(res.excluded, 1);
    CHECK_EQ(res.included, 1);
    CHECK_EQ(res.queryMatched, 4);

    // THE sticky rule (§20.8). Change fixture 2 so it no longer matches...
    CHECK(r.eng.patch.remove(2));
    res = r.groups.resolve(uid, r.eng.patch);
    CHECK_EQ(ids(res.targets), std::vector<FixtureId>({1, 3, 4, 11}));
    // ...and the now-redundant exclusion is MARKED, not silently dropped.
    CHECK_EQ(res.staleExcludes.size(), size_t(1));
    CHECK(res.staleExcludes[0] == f2);

    // ...then re-patch a Moving Spot at id 2. A naive implementation would
    // clean up the redundant exclusion and let the fixture silently reappear.
    // Sticky-by-UID means the NEW fixture 2 is a different object, so it DOES
    // join — which is correct: the user excluded a specific fixture, not an id.
    CHECK(r.eng.patch.add({2, "", r.eng.patch.findType("Moving Spot"), 1, 200}, &err));
    res = r.groups.resolve(uid, r.eng.patch);
    CHECK_EQ(res.targets.size(), size_t(5));
    CHECK_EQ(res.staleExcludes.size(), size_t(1));  // the old UID is still stale

    // Excluding and including the same fixture cancels rather than stacking.
    const Uid f3 = r.uidOf(3);
    CHECK(r.groups.exclude(uid, f3, &err));
    CHECK_EQ(r.groups.find(uid)->excludes.size(), size_t(2));
    CHECK(r.groups.include(uid, f3, &err));
    CHECK_EQ(r.groups.find(uid)->excludes.size(), size_t(1));
    CHECK_EQ(r.groups.find(uid)->includes.size(), size_t(2));

    // Reset clears both lists, which is the "back to the pure query" action.
    // The manually included LED par drops out with them, leaving exactly what
    // the query matches: four Moving Spots.
    r.groups.resetOverrides(uid);
    const auto pure = r.groups.resolve(uid, r.eng.patch);
    CHECK_EQ(ids(pure.targets), std::vector<FixtureId>({1, 3, 4, 2}));
    CHECK_EQ(pure.staleExcludes.size(), size_t(0));
    CHECK_EQ(pure.excluded, 0);
    CHECK_EQ(pure.included, 0);
}

static void testFreeze() {
    Rig r;
    GroupDef g;
    g.name = "Spots";
    g.kind = GroupKind::Dynamic;
    g.query.typeName = "Moving Spot";
    g.query.sortKey = "id";
    g.allowOverrides = true;
    const Uid uid = r.groups.add(g).uid;
    std::string err;
    r.groups.exclude(uid, r.uidOf(4), &err);

    CHECK_EQ(ids(r.groups.resolve(uid, r.eng.patch).targets),
             std::vector<FixtureId>({1, 2, 3}));

    // §20.8: freeze captures current membership AND order, so the rig stops
    // moving underneath the programming.
    CHECK(r.groups.freeze(uid, r.eng.patch));
    const GroupDef* frozen = r.groups.find(uid);
    CHECK(frozen->kind == GroupKind::Static);
    CHECK_EQ(ids(frozen->members), std::vector<FixtureId>({1, 2, 3}));
    CHECK(frozen->query.empty());
    CHECK_EQ(frozen->excludes.size(), size_t(0));
    CHECK(!frozen->allowOverrides);

    // Patching another matching fixture no longer changes it — the point.
    CHECK(r.eng.patch.add({6, "", r.eng.patch.findType("Moving Spot"), 1, 300}, &err));
    CHECK_EQ(r.groups.resolve(uid, r.eng.patch).targets.size(), size_t(3));
}

static void testCuesAreSafeFromDynamicGroups() {
    // §20.8: a cue stores PER-FIXTURE VALUES, not a group reference, so
    // changing a dynamic group cannot alter an existing cue. Worth asserting
    // because the opposite would be a serious trap.
    Rig r;
    GroupDef g;
    g.name = "Spots";
    g.kind = GroupKind::Dynamic;
    g.query.typeName = "Moving Spot";
    const Uid uid = r.groups.add(g).uid;

    r.eng.programmer.selection().set(r.groups.resolve(uid, r.eng.patch).targets);
    r.eng.programmer.setAttr(Attr::Dimmer, 1.0f);

    Sequence& seq = r.eng.ensureSequence(1);
    seq.cues.resize(1);
    seq.cues[0].timing.fadeIn = 0.0;
    seq.cues[0].values() = r.eng.programmer.storeLayer();
    const size_t stored = seq.cues[0].values().size();
    CHECK_EQ(stored, size_t(4));

    // Grow the group by patching another matching fixture.
    std::string err;
    CHECK(r.eng.patch.add({7, "", r.eng.patch.findType("Moving Spot"), 1, 300}, &err));
    CHECK_EQ(r.groups.resolve(uid, r.eng.patch).targets.size(), size_t(5));

    // The cue is unchanged: it captured what matched at store time.
    CHECK_EQ(seq.cues[0].values().size(), stored);
    CHECK(!seq.cues[0].values().has(7, 0, Attr::Dimmer));
}

static void testGroupSelectionDrivesFx() {
    // End to end: a nested group's hierarchy survives into a Selection, which
    // is what multi-level fan and spread read.
    Rig r;

    GroupDef a;
    a.name = "A";
    a.members = {{1, 0}, {2, 0}};
    const Uid ua = r.groups.add(a).uid;
    GroupDef b;
    b.name = "B";
    b.members = {{3, 0}, {4, 0}};
    const Uid ub = r.groups.add(b).uid;
    GroupDef all;
    all.name = "All";
    const Uid uall = r.groups.add(all).uid;
    std::string err;
    r.groups.nest(uall, ua, &err);
    r.groups.nest(uall, ub, &err);

    const Selection sel = r.groups.buildSelection(uall, r.eng.patch);
    SpreadTable spread;
    spread.build(sel, r.eng.patch, SpreadSource::Index);

    // Four members spread evenly, in the order the nesting produced.
    CHECK_EQ(spread.size(), size_t(4));
    CHECK_NEAR(spread.raw({1, 0}), 0.0);
    CHECK_NEAR(spread.raw({2, 0}), 0.25);
    CHECK_NEAR(spread.raw({3, 0}), 0.5);
    CHECK_NEAR(spread.raw({4, 0}), 0.75);

    // And the per-group structure is still available for multi-level fan.
    CHECK_EQ(sel.groupsAtTopLevel().size(), size_t(2));
}

int main() {
    std::printf("lumen group\n");
    testStaticOrder();
    testNestingComposesOrder();
    testCyclePrevention();
    testDynamicQueries();
    testDynamicLiveUpdates();
    testDynamicSortRule();
    testOverridesOffByDefault();
    testStickyOverrides();
    testFreeze();
    testCuesAreSafeFromDynamicGroups();
    testGroupSelectionDrivesFx();
    return check::report("group");
}
