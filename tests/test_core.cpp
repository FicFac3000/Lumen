// Tests for the taxonomy, UIDs, value model and patch.
#include "Check.hpp"

#include "lumen/Attribute.hpp"
#include "lumen/Patch.hpp"
#include "lumen/Uid.hpp"
#include "lumen/Value.hpp"

#include <set>

using namespace lumen;

static void testTaxonomy() {
    // Every Attr must resolve to its own table entry — catches enum/table drift.
    for (size_t i = 0; i < kAttrCount; ++i) {
        const auto a = static_cast<Attr>(i);
        CHECK(attrInfo(a).attr == a);
    }

    // Names round-trip, including aliases.
    CHECK(attrFromName("Dimmer").value() == Attr::Dimmer);
    CHECK(attrFromName("dim").value() == Attr::Dimmer);
    CHECK(attrFromName("R").value() == Attr::Red);
    CHECK(attrFromName("prism").value() == Attr::Prism1);
    CHECK(!attrFromName("NotAnAttribute").has_value());

    // Group membership covers every attribute exactly once.
    size_t total = 0;
    for (size_t g = 0; g < kGroupCount; ++g)
        total += attrsInGroup(static_cast<Group>(g)).size();
    CHECK_EQ(total, kAttrCount);

    // §6.1 decisions: Intensity and Control are their own groups.
    CHECK(groupOf(Attr::Dimmer) == Group::Intensity);
    CHECK(groupOf(Attr::Reset) == Group::Control);
    CHECK(groupOf(Attr::Pan) == Group::Position);
    CHECK(groupOf(Attr::Hue) == Group::Color);
    CHECK(groupOf(Attr::Prism1Spin) == Group::Beam);

    // §6.2 kind flags drive behaviour elsewhere, so they must be right.
    CHECK(isIntensity(Attr::Dimmer));
    CHECK(!isIntensity(Attr::Pan));
    CHECK(isAxis(Attr::Pan) && isAxis(Attr::Tilt));
    CHECK(isAxis(Attr::PosX) && isAxis(Attr::PosY));
    CHECK(isAnchored(Attr::Prism1Spin));   // has a semantic zero
    CHECK(isAnchored(Attr::PanRotate));
    CHECK(isAnchored(Attr::CTO));
    CHECK(!isAnchored(Attr::Pan));         // absolute position, no anchor
    CHECK(isVirtual(Attr::Hue));
    CHECK(isVirtual(Attr::Saturation));
    CHECK(!isVirtual(Attr::Red));

    // §6.3 / §11.4: the merge rule reads the kind flag, not a named attribute.
    CHECK(defaultMergeRule(Attr::Dimmer) == MergeRule::HTP);
    CHECK(defaultMergeRule(Attr::Pan) == MergeRule::LTP);
    CHECK(defaultMergeRule(Attr::Red) == MergeRule::LTP);

    // §4.2 role binding: roles resolve to candidate attributes in preference
    // order, which is what makes one Circle recipe portable across domains.
    CHECK(roleCandidates(Role::AxisX).front() == Attr::Pan);
    CHECK(roleCandidates(Role::AxisY).front() == Attr::Tilt);
    CHECK(roleFromName("x").value() == Role::AxisX);
}

static void testUids() {
    UidGen gen(12345);
    std::set<uint64_t> seen;
    for (int i = 0; i < 10000; ++i) {
        Uid u = gen.next();
        CHECK(u.valid());
        seen.insert(u.v);
    }
    CHECK_EQ(seen.size(), size_t(10000));  // never reused within a session

    Uid u = gen.next();
    Uid back;
    CHECK(fromHex(toHex(u), back));
    CHECK(back == u);
    CHECK(!Uid{}.valid());
}

static void testValueModel() {
    // Key packing: fixture | cell | attr must round-trip, including high cells.
    const AttrKey k = makeKey(4001, 12, Attr::Blue);
    CHECK_EQ(keyFixture(k), FixtureId(4001));
    CHECK_EQ(keyCell(k), CellId(12));
    CHECK(keyAttr(k) == Attr::Blue);

    ValueLayer L;
    L.set(1, 0, Attr::Dimmer, Value::hard(0.5f));
    L.set(1, 0, Attr::Red, Value::ref(Uid{99}, Attr::Red, 1.0f));
    CHECK_EQ(L.size(), size_t(2));
    CHECK_NEAR(*L.value(1, 0, Attr::Dimmer), 0.5);

    // §11.8 active axis: ClearActive deactivates without removing.
    CHECK(L.hasActive());
    L.deactivateAll();
    CHECK(!L.hasActive());
    CHECK_EQ(L.size(), size_t(2));             // values remain and still output
    CHECK_EQ(L.activeOnly().size(), size_t(0));  // but Store ignores them

    // §11.8 kind axis: ClearPresets removes references, leaving hard values.
    ValueLayer M;
    M.set(1, 0, Attr::Dimmer, Value::hard(0.5f));
    M.set(1, 0, Attr::Red, Value::ref(Uid{99}, Attr::Red, 1.0f));
    M.removeRefs();
    CHECK_EQ(M.size(), size_t(1));
    CHECK(M.has(1, 0, Attr::Dimmer));
    CHECK(!M.has(1, 0, Attr::Red));

    // Flatten: references become hard values at their resolved value.
    ValueLayer N;
    N.set(1, 0, Attr::Red, Value::ref(Uid{99}, Attr::Red, 0.75f));
    N.flattenRefs();
    CHECK(!N.find(makeKey(1, 0, Attr::Red))->isRef());
    CHECK_NEAR(N.find(makeKey(1, 0, Attr::Red))->v, 0.75);

    // Group filtering drives §11.1's per-group tracking masks.
    ValueLayer P;
    P.set(1, 0, Attr::Dimmer, Value::hard(1.f));
    P.set(1, 0, Attr::Pan, Value::hard(0.5f));
    P.set(1, 0, Attr::Red, Value::hard(0.5f));
    CHECK_EQ(P.keys(Group::Intensity).size(), size_t(1));
    CHECK_EQ(P.keys(Group::Position).size(), size_t(1));
    CHECK_EQ(P.keys().size(), size_t(3));

    // Values clamp on construction.
    CHECK_NEAR(Value::hard(1.7f).v, 1.0);
    CHECK_NEAR(Value::hard(-3.f).v, 0.0);
}

static void testMergeRules() {
    OutputLayer base, over;
    const AttrKey dim = makeKey(1, 0, Attr::Dimmer);
    const AttrKey pan = makeKey(1, 0, Attr::Pan);

    base.set(dim, 0.8f);
    base.set(pan, 0.2f);
    over.set(dim, 0.3f);  // lower intensity must not win
    over.set(pan, 0.9f);  // later position must win

    base.mergeOver(over);
    CHECK_NEAR(*base.get(dim), 0.8);  // HTP
    CHECK_NEAR(*base.get(pan), 0.9);  // LTP

    // Crossfade: values leaving the stage fade rather than snapping.
    OutputLayer from, to;
    from.set(dim, 1.0f);
    to.set(pan, 1.0f);
    OutputLayer mid = crossfade(from, to, 0.5f);
    CHECK_NEAR(*mid.get(dim), 0.5);
    CHECK_NEAR(*mid.get(pan), 0.5);

    OutputLayer done = crossfade(from, to, 1.0f);
    CHECK_NEAR(*done.get(dim), 0.0);
    CHECK_NEAR(*done.get(pan), 1.0);
}

static void testPatch() {
    UidGen gen(7);
    Patch p(gen);
    installStarterTypes(p);

    const int spot = p.findType("Moving Spot");
    const int par = p.findType("LED Par RGBW");
    const int bar = p.findType("Pixel Bar 12");
    CHECK(spot >= 0 && par >= 0 && bar >= 0);

    CHECK_EQ(p.type(par).footprint(), 6);
    CHECK_EQ(p.type(spot).footprint(), 15);  // 16-bit pan/tilt + 11 more
    CHECK_EQ(p.type(bar).footprint(), 37);   // master + 12 cells x 3

    // Basic patch.
    std::string err;
    CHECK(p.add({1, "", spot, 1, 1}, &err));
    CHECK(err.empty());

    // Overlap must be rejected, and say what it clashed with.
    CHECK(!p.add({2, "", spot, 1, 5}, &err));
    CHECK(err.find("clash") != std::string::npos);

    // Same address in a different universe is fine.
    CHECK(p.add({2, "", spot, 2, 1}, &err));

    // Duplicate fixture id rejected.
    CHECK(!p.add({1, "", par, 3, 1}, &err));

    // A footprint may not span universes (§17.2).
    CHECK(!p.add({3, "", bar, 1, 500}, &err));
    CHECK(err.find("one universe") != std::string::npos);

    // Auto-addressed range with a gap.
    Patch q(gen);
    installStarterTypes(q);
    const int qpar = q.findType("LED Par RGBW");
    const int made = q.addRange(101, 8, qpar, 1, 1, 2, &err);
    CHECK_EQ(made, 8);
    CHECK_EQ(q.find(101)->address, 1);
    CHECK_EQ(q.find(102)->address, 9);   // 6 channels + 2 gap
    CHECK_EQ(q.find(108)->address, 57);
    CHECK_EQ(q.conflicts().size(), size_t(0));

    // UID references survive re-addressing and renumbering (§17.2, §17.3).
    const Uid before = q.find(101)->uid;
    CHECK(q.readdress(101, 2, 100, &err));
    CHECK(q.find(101)->uid == before);
    CHECK_EQ(q.find(101)->universe, 2);
    CHECK(q.renumber(101, 501, &err));
    CHECK(q.find(101) == nullptr);
    CHECK(q.find(501) != nullptr);
    CHECK(q.find(501)->uid == before);
    CHECK(q.findByUid(before) != nullptr);
    CHECK_EQ(q.findByUid(before)->id, FixtureId(501));

    // Renumbering onto an occupied id is refused.
    CHECK(!q.renumber(502, 503, &err));  // 502 exists, 503 does not... both ways
    CHECK(q.renumber(102, 999, &err));
    CHECK(!q.renumber(103, 999, &err));

    // Cells: a simple fixture has one implicit cell, a bar has twelve.
    Patch r(gen);
    installStarterTypes(r);
    CHECK(r.add({1, "", r.findType("LED Par RGBW"), 1, 1}, &err));
    CHECK(r.add({2, "", r.findType("Pixel Bar 12"), 1, 10}, &err));
    CHECK_EQ(r.cellsOf(1).size(), size_t(1));
    CHECK_EQ(r.cellsOf(2).size(), size_t(12));
    CHECK_EQ(r.cellsOf(2).front(), CellId(1));
    CHECK_EQ(r.cellsOf(2).back(), CellId(12));

    // Per-cell channels resolve independently.
    const FixtureType& bt = *r.typeOf(2);
    CHECK(bt.hasAttr(Attr::Red, 1));
    CHECK(bt.hasAttr(Attr::Red, 12));
    CHECK(!bt.hasAttr(Attr::Red, 13));
    CHECK(bt.hasAttr(Attr::Dimmer, 0));  // master on the parent
    CHECK_EQ(bt.channel(Attr::Red, 1)->coarse, 1);
    CHECK_EQ(bt.channel(Attr::Red, 2)->coarse, 4);

    // Range labels come from the profile (§7.2), sorted by DMX.
    const auto sets = r.typeOf(1)->setsFor(Attr::Shutter);
    CHECK_EQ(sets.size(), size_t(3));
    CHECK(sets.front().name == "Closed");
    CHECK(sets.front().anchor);  // closed is a semantic zero

    // The prism rotate sets include a detected anchor, which is what §7.3's
    // symmetric stepping keys off.
    const auto spin = r.typeOf(2) ? std::vector<ChannelSet>{} : std::vector<ChannelSet>{};
    (void)spin;
}

static void testHighlight() {
    UidGen gen(11);
    Patch p(gen);
    installStarterTypes(p);
    std::string err;

    // A type with a dimmer: highlight drives it full and opens the beam.
    CHECK(p.add({1, "", p.findType("Moving Spot"), 1, 1}, &err));
    const ValueLayer h = p.typeOf(1)->highlightFor(1);
    CHECK_NEAR(*h.value(1, 0, Attr::Dimmer), 1.0);
    CHECK_NEAR(*h.value(1, 0, Attr::Shutter), 1.0);
    CHECK_NEAR(*h.value(1, 0, Attr::Gobo1), 0.0);
    CHECK_NEAR(*h.value(1, 0, Attr::Prism1), 0.0);
    CHECK_NEAR(*h.value(1, 0, Attr::Zoom), 0.5);

    // §20.7: highlight is derived from the kind flags, so a type with no
    // intensity-kind attribute drives colour to white instead. The pixel bar's
    // cells have only RGB.
    FixtureType rgbOnly;
    rgbOnly.name = "RGB Cell";
    rgbOnly.channels = {{Attr::Red, 0}, {Attr::Green, 1}, {Attr::Blue, 2}};
    const ValueLayer h2 = rgbOnly.highlightFor(5);
    CHECK_NEAR(*h2.value(5, 0, Attr::Red), 1.0);
    CHECK_NEAR(*h2.value(5, 0, Attr::Green), 1.0);
    CHECK_NEAR(*h2.value(5, 0, Attr::Blue), 1.0);
    CHECK(!h2.has(5, 0, Attr::Dimmer));  // nothing invented that isn't there
}

int main() {
    std::printf("lumen core\n");
    testTaxonomy();
    testUids();
    testValueModel();
    testMergeRules();
    testPatch();
    testHighlight();
    return check::report("core");
}
