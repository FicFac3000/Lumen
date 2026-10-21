// test_editor.cpp — the fixture authoring model, its two views, compile and
// validation (SPEC §1.2, §1.3).
#include "Check.hpp"

#include "lumen/FixtureEditor.hpp"
#include "lumen/Gdtf.hpp"

#include <cstdio>
#include <set>

using namespace lumen;

namespace {

const TableRow* rowAt(const std::vector<TableRow>& rows, int dmx) {
    for (const TableRow& r : rows)
        if (r.dmx == dmx) return &r;
    return nullptr;
}

const TreeRow* treeAt(const std::vector<TreeRow>& rows, const std::string& name) {
    for (const TreeRow& r : rows)
        if (r.name == name) return &r;
    return nullptr;
}

// GDTF writes a multi-instance fixture one channel per instance. Four
// uniform RGB cells behind a master, with a yoke so the geometry kinds are
// exercised too.
std::string uniformBarXml() {
    std::string x =
        "<GDTF DataVersion=\"1.2\"><FixtureType Name=\"Uniform Bar\" "
        "Manufacturer=\"Lumen Test\">"
        "<Geometries><Geometry Name=\"Base\">"
        "<Axis Name=\"Yoke\"><Beam Name=\"Pixel\"/></Axis>";
    for (int i = 1; i <= 4; ++i)
        x += "<GeometryReference Name=\"Cell" + std::to_string(i) +
             "\" Geometry=\"Pixel\"/>";
    x += "</Geometry></Geometries>"
         "<DMXModes><DMXMode Name=\"Pixel\" Geometry=\"Base\"><DMXChannels>"
         "<DMXChannel Offset=\"1\" Geometry=\"Base\"><LogicalChannel "
         "Attribute=\"Dimmer\"><ChannelFunction Name=\"Master\" "
         "Attribute=\"Dimmer\"/></LogicalChannel></DMXChannel>";
    const char* attrs[] = {"ColorAdd_R", "ColorAdd_G", "ColorAdd_B"};
    int offset = 2;
    for (int c = 1; c <= 4; ++c)
        for (const char* a : attrs) {
            x += "<DMXChannel Offset=\"" + std::to_string(offset++) +
                 "\" Geometry=\"Cell" + std::to_string(c) +
                 "\"><LogicalChannel Attribute=\"" + a +
                 "\"><ChannelFunction Name=\"x\" Attribute=\"" + a +
                 "\"/></LogicalChannel></DMXChannel>";
        }
    x += "</DMXChannels></DMXMode></DMXModes></FixtureType></GDTF>";
    return x;
}

// The same bar, but the last cell is missing its blue — a real fixture shape,
// and the one normalisation must refuse to average away.
std::string raggedBarXml() {
    std::string x = uniformBarXml();
    const std::string drop =
        "<DMXChannel Offset=\"13\" Geometry=\"Cell4\"><LogicalChannel "
        "Attribute=\"ColorAdd_B\"><ChannelFunction Name=\"x\" "
        "Attribute=\"ColorAdd_B\"/></LogicalChannel></DMXChannel>";
    const size_t at = x.find(drop);
    if (at != std::string::npos) x.erase(at, drop.size());
    return x;
}

int countIssues(const std::vector<Issue>& v, Issue::Level l) {
    int n = 0;
    for (const Issue& i : v)
        if (i.level == l) ++n;
    return n;
}

bool anyIssueMentions(const std::vector<Issue>& v, const std::string& text) {
    for (const Issue& i : v)
        if (i.what.find(text) != std::string::npos) return true;
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
static void testSimpleFixtureNeedsNoGeometry() {
    // §1.2: the table is the default surface for simple fixtures, so a
    // definition must work with no geometry names at all.
    FixtureDef d;
    d.name = "Plain Dimmer";
    FixtureEditor ed(d);
    const size_t m = ed.addMode("Default");
    CHECK(ed.addChannel(m, Attr::Dimmer));

    CompileReport c = compile(d, m);
    CHECK(c.ok);
    CHECK_EQ(c.channelsEmitted, 1);
    CHECK_EQ(c.type.footprint(), 1);
    CHECK_EQ(int(c.type.cellCount), 0);
    CHECK(c.type.name == "Plain Dimmer / Default");

    // And it is patchable, which is the only thing the compile is for.
    UidGen uids;
    Patch patch(uids);
    const int ti = patch.addType(c.type);
    Patch::PatchRequest req;
    req.id = 1;
    req.typeIndex = ti;
    CHECK(patch.add(req));
    CHECK(patch.typeOf(1)->hasAttr(Attr::Dimmer));
}

static void testOneDefinitionManyCells() {
    // §1.3's first want: one cell definition instantiates N times with
    // automatic DMX offset. Four declarations become 37 channels.
    FixtureDef d = starterPixelBar();

    CompileReport c = compile(d, 0);
    CHECK(c.ok);
    CHECK(c.errors.empty());
    CHECK_EQ(d.modes[0].channels.size(), size_t(4));   // what was authored
    CHECK_EQ(c.channelsEmitted, 37);                   // what it compiles to
    CHECK_EQ(c.declarationsExpanded, 3);               // red, green, blue
    CHECK_EQ(c.type.footprint(), 37);
    CHECK_EQ(int(c.type.cellCount), 12);

    // The master lives at the parent, the colour per cell (§1.3).
    CHECK(c.type.hasAttr(Attr::Dimmer, 0));
    CHECK(!c.type.hasAttr(Attr::Red, 0));
    for (CellId cell = 1; cell <= 12; ++cell) {
        CHECK(c.type.hasAttr(Attr::Red, cell));
        CHECK(c.type.hasAttr(Attr::Green, cell));
        CHECK(c.type.hasAttr(Attr::Blue, cell));
        CHECK(!c.type.hasAttr(Attr::Dimmer, cell));
    }

    // Offsets march in threes after the master, with no overlap anywhere.
    const ChannelDef* red1 = c.type.channel(Attr::Red, 1);
    const ChannelDef* red2 = c.type.channel(Attr::Red, 2);
    const ChannelDef* red12 = c.type.channel(Attr::Red, 12);
    CHECK(red1 && red2 && red12);
    CHECK_EQ(red1->coarse, 1);
    CHECK_EQ(red2->coarse, 4);
    CHECK_EQ(red12->coarse, 34);

    std::set<int> slots;
    for (const ChannelDef& ch : c.type.channels) CHECK(slots.insert(ch.coarse).second);
    CHECK_EQ(slots.size(), size_t(37));
}

static void testBothViewsAreLiveOnOneModel() {
    // §1.2: no import/export step between the views. An edit through the
    // editor has to show up in both.
    FixtureDef d = starterPixelBar();

    auto table = tableView(d, 0);
    auto tree = treeView(d, 0);
    CHECK_EQ(table.size(), size_t(37));

    // Table is in address order, one row per channel, with the cell visible.
    CHECK_EQ(table[0].dmx, 1);
    CHECK(table[0].attr == Attr::Dimmer);
    CHECK_EQ(int(table[0].cell), 0);
    CHECK(table[1].attr == Attr::Red);
    CHECK_EQ(int(table[1].cell), 1);
    CHECK(table[1].geometry == "Pixel1");
    for (size_t i = 1; i < table.size(); ++i) CHECK(table[i].dmx > table[i - 1].dmx);

    // Tree shows the body, twelve expanded references each with its cell, and
    // the shared definition flagged as the template.
    const TreeRow* body = treeAt(tree, "Body");
    CHECK(body != nullptr);
    CHECK_EQ(body->depth, 0);
    CHECK_EQ(body->channels.size(), size_t(1));

    const TreeRow* p1 = treeAt(tree, "Pixel1");
    const TreeRow* p12 = treeAt(tree, "Pixel12");
    CHECK(p1 && p12);
    CHECK(p1->expandedReference);
    CHECK_EQ(int(p1->cell), 1);
    CHECK_EQ(p1->dmxOffset, 1);
    CHECK_EQ(p1->channels.size(), size_t(3));
    CHECK_EQ(p1->channels[0].second, 2);     // red at address 2
    CHECK_EQ(int(p12->cell), 12);
    CHECK_EQ(p12->dmxOffset, 34);
    CHECK_EQ(p12->channels[0].second, 35);
    CHECK(p1->depth > body->depth);          // nested under the body

    const TreeRow* tmpl = treeAt(tree, "Pixel");
    CHECK(tmpl != nullptr);
    CHECK(tmpl->isTemplate);                 // the shared cell definition
    CHECK(!tmpl->expandedReference);
    CHECK_EQ(tmpl->channels.size(), size_t(3));

    // Now edit the cell definition once: every cell gains the attribute, in
    // both views, with no rebuild step.
    FixtureEditor ed(d);
    std::string err;
    CHECK(ed.addChannel(0, Attr::White, "Pixel", -1, -1, &err));
    CHECK(err.empty());

    auto table2 = tableView(d, 0);
    auto tree2 = treeView(d, 0);
    CHECK_EQ(table2.size(), size_t(49));     // 1 + 12 * 4
    CHECK_EQ(treeAt(tree2, "Pixel1")->channels.size(), size_t(4));
    CHECK_EQ(treeAt(tree2, "Pixel")->channels.size(), size_t(4));

    CompileReport c2 = compile(d, 0);
    CHECK(c2.ok);
    for (CellId cell = 1; cell <= 12; ++cell) CHECK(c2.type.hasAttr(Attr::White, cell));
    CHECK(!c2.type.hasAttr(Attr::White, 0));
}

static void testAppendRespectsWhatOffsetsAreRelativeTo() {
    // Appending inside a cell must land inside the cell; appending anywhere
    // else must land after every expanded instance, not in the middle of them.
    FixtureDef d = starterPixelBar();
    FixtureEditor ed(d);

    // Inside the cell: white goes to relative offset 3, so cell 1 gets
    // address 5 and the footprint grows by twelve.
    CHECK(ed.addChannel(0, Attr::White, "Pixel", -1));
    CompileReport c = compile(d, 0);
    CHECK(c.ok);
    CHECK_EQ(c.type.channel(Attr::White, 1)->coarse, 4);
    CHECK_EQ(c.type.footprint(), 49);

    // On the body: after everything, which is 49 channels in.
    CHECK(ed.addChannel(0, Attr::Shutter, "Body", -1));
    CompileReport c2 = compile(d, 0);
    CHECK(c2.ok);
    CHECK_EQ(c2.type.channel(Attr::Shutter, 0)->coarse, 49);
    CHECK_EQ(c2.type.footprint(), 50);
    CHECK(c2.errors.empty());
}

static void testAddInstancesInfersTheStride() {
    FixtureDef d;
    d.name = "Bar";
    GeometryNode body;
    body.name = "Body";
    GeometryNode cell;
    cell.name = "Cell";
    d.geometries.push_back(std::move(body));
    d.geometries.push_back(std::move(cell));

    FixtureEditor ed(d);
    const size_t m = ed.addMode("Mode", "Body");
    ed.addChannel(m, Attr::Red, "Cell", 0);
    ed.addChannel(m, Attr::Green, "Cell", 1);

    // Stride inferred from the cell's own channels: two.
    std::string err;
    CHECK_EQ(ed.addInstances(m, "Body", "Cell", 4, "Cell", 0, 0, &err), 4);
    CHECK(err.empty());
    CompileReport c = compile(d, m);
    CHECK(c.ok);
    CHECK_EQ(c.type.footprint(), 8);
    CHECK_EQ(c.type.channel(Attr::Red, 4)->coarse, 6);

    // A geometry with no channels in this mode cannot have a stride guessed,
    // and says so rather than inventing one.
    GeometryNode empty;
    empty.name = "Empty";
    CHECK(ed.addGeometry({}, empty));
    err.clear();
    CHECK_EQ(ed.addInstances(m, "Body", "Empty", 2, "E", 20, 0, &err), 0);
    CHECK(!err.empty());
    CHECK(err.find("stride") != std::string::npos);

    // Given one explicitly, it works.
    CHECK_EQ(ed.addInstances(m, "Body", "Empty", 2, "E", 20, 5, &err), 2);
}

static void testCellsNumberInDocumentOrder() {
    // Cell 1 must be the first reference in the plot, not whichever one the
    // container happened to yield first.
    FixtureDef d = starterPixelBar();
    auto tree = treeView(d, 0);
    CellId expect = 1;
    for (const TreeRow& r : tree) {
        if (!r.expandedReference) continue;
        CHECK_EQ(int(r.cell), int(expect));
        CHECK(r.name == "Pixel" + std::to_string(expect));
        ++expect;
    }
    CHECK_EQ(int(expect), 13);
}

static void testSixteenBitAndRangeLabels() {
    FixtureDef d = starterMovingSpot();

    CompileReport c = compile(d, 0);
    CHECK(c.ok);
    CHECK_EQ(c.channelsEmitted, 6);
    CHECK_EQ(c.type.footprint(), 8);   // two 16-bit pairs
    CHECK_EQ(int(c.type.cellCount), 0);

    const ChannelDef* pan = c.type.channel(Attr::Pan);
    CHECK(pan != nullptr);
    CHECK_EQ(pan->coarse, 0);
    CHECK_EQ(pan->fine, 1);

    // The table shows a 16-bit channel as one row with both slots, which is
    // what makes it fast to scan.
    auto table = tableView(d, 0);
    CHECK_EQ(table.size(), size_t(6));
    CHECK_EQ(table[0].dmx, 1);
    CHECK_EQ(table[0].fineDmx, 2);
    CHECK_EQ(table[2].fineDmx, 0);     // dimmer is 8-bit

    // Range labels survive the compile, so the auto-stepper can read them.
    auto sets = c.type.setsFor(Attr::Gobo1);
    CHECK_EQ(sets.size(), size_t(3));
    CHECK(sets[0].name == "Open");
    CHECK(sets[0].anchor);
    CHECK(!sets[1].anchor);
    CHECK_EQ(int(sets[2].dmxTo), 29);
    const TableRow* goboRow = rowAt(table, 8);
    CHECK(goboRow != nullptr);
    CHECK_EQ(goboRow->setCount, 3);

    // Sets replicate per cell on an instanced geometry, or a pixel bar's
    // labels would only exist on cell zero.
    FixtureDef bar = starterPixelBar();
    FixtureEditor ed(bar);
    CHECK(ed.addChannelSet(0, 1, {"Full", 255, 255, 100.f, 100.f, false}));
    CompileReport bc = compile(bar, 0);
    CHECK(bc.ok);
    CHECK_EQ(bc.type.setsFor(Attr::Red, 1).size(), size_t(1));
    CHECK_EQ(bc.type.setsFor(Attr::Red, 7).size(), size_t(1));
}

static void testOverlapIsAnError() {
    FixtureDef d;
    d.name = "Clash";
    FixtureEditor ed(d);
    const size_t m = ed.addMode("Default");
    ed.addChannel(m, Attr::Dimmer, {}, 0);
    ed.addChannel(m, Attr::Shutter, {}, 0);   // same slot

    CompileReport c = compile(d, m);
    CHECK(!c.ok);
    CHECK(!c.errors.empty());
    CHECK(c.errors[0].find("claimed by both") != std::string::npos);

    // A 16-bit fine half colliding counts too: it is still a channel the
    // fixture will read.
    FixtureDef e;
    FixtureEditor ed2(e);
    const size_t m2 = ed2.addMode("Default");
    ed2.addChannel(m2, Attr::Pan, {}, 0, 1);
    ed2.addChannel(m2, Attr::Dimmer, {}, 1);
    CHECK(!compile(e, m2).ok);

    // And an expansion that collides with a fixed channel, which is the one a
    // flat view would never show.
    FixtureDef f = starterPixelBar();
    FixtureEditor ed3(f);
    CHECK(ed3.setChannelOffset(0, 0, 5, -1));   // dimmer into cell 2's space
    CompileReport fc = compile(f, 0);
    CHECK(!fc.ok);
    CHECK(anyIssueMentions(validate(f, 0), "claimed by both"));
}

static void testValidationFindsStructuralMistakes() {
    FixtureDef ok = starterMovingSpot();
    const auto clean = validate(ok, 0);
    CHECK_EQ(countIssues(clean, Issue::Level::Error), 0);
    CHECK_EQ(countIssues(clean, Issue::Level::Warning), 0);

    // A reference pointing at nothing.
    FixtureDef dangling;
    dangling.name = "Dangling";
    GeometryNode body;
    body.name = "Body";
    GeometryNode ref;
    ref.name = "Cell1";
    ref.kind = GeometryKind::Reference;
    ref.referenced = "NotThere";
    body.children.push_back(std::move(ref));
    dangling.geometries.push_back(std::move(body));
    FixtureEditor de(dangling);
    de.addMode("Default", "Body");
    CHECK(anyIssueMentions(validate(dangling, 0), "points at missing geometry"));

    // A reference cycle: detected, not discovered as a hang.
    FixtureDef cyc;
    cyc.name = "Cycle";
    GeometryNode a;
    a.name = "A";
    a.kind = GeometryKind::Reference;
    a.referenced = "B";
    GeometryNode b;
    b.name = "B";
    b.kind = GeometryKind::Reference;
    b.referenced = "A";
    cyc.geometries.push_back(std::move(a));
    cyc.geometries.push_back(std::move(b));
    FixtureEditor ce(cyc);
    const size_t cm = ce.addMode("Default");
    cyc.modes[cm].channels.push_back(ChannelDecl{Attr::Dimmer, "A", 0, -1, 0, false, {}});
    const auto cycIssues = validate(cyc, cm);
    CHECK(anyIssueMentions(cycIssues, "cycle"));
    CHECK(!compile(cyc, cm).ok);   // and the compile refuses rather than looping

    // A mode root that does not exist.
    FixtureDef badRoot;
    badRoot.name = "BadRoot";
    FixtureEditor br(badRoot);
    br.addMode("Default", "Ghost");
    CHECK(anyIssueMentions(validate(badRoot, 0), "mode root geometry"));

    // The same attribute twice on one cell.
    FixtureDef dup;
    dup.name = "Dup";
    FixtureEditor du(dup);
    const size_t dm = du.addMode("Default");
    du.addChannel(dm, Attr::Dimmer, {}, 0);
    du.addChannel(dm, Attr::Dimmer, {}, 1);
    CHECK(anyIssueMentions(validate(dup, dm), "declared twice"));

    // Fine and coarse in one slot.
    FixtureDef same;
    FixtureEditor se(same);
    const size_t sm = se.addMode("Default");
    se.addChannel(sm, Attr::Pan, {}, 0);
    CHECK(!se.setChannelOffset(sm, 0, 3, 3));
    same.modes[sm].channels[0].fine = 3;
    same.modes[sm].channels[0].coarse = 3;
    CHECK(anyIssueMentions(validate(same, sm), "share one DMX slot"));

    // A duplicate geometry name, which would make every lookup ambiguous.
    FixtureDef twice;
    GeometryNode g1;
    g1.name = "Same";
    GeometryNode g2;
    g2.name = "Same";
    twice.geometries.push_back(std::move(g1));
    twice.geometries.push_back(std::move(g2));
    FixtureEditor te(twice);
    te.addMode("Default");
    CHECK(anyIssueMentions(validate(twice, 0), "duplicate geometry name"));

    // A gap is legal but surfaced, because it is almost always a mistake.
    FixtureDef gap;
    gap.name = "Gap";
    FixtureEditor ge(gap);
    const size_t gm = ge.addMode("Default");
    ge.addChannel(gm, Attr::Dimmer, {}, 0);
    ge.addChannel(gm, Attr::Shutter, {}, 4);
    const auto gapIssues = validate(gap, gm);
    CHECK_EQ(countIssues(gapIssues, Issue::Level::Error), 0);
    CHECK_EQ(countIssues(gapIssues, Issue::Level::Info), 3);  // slots 2, 3, 4
}

static void testEditOperationsRefuseWhatWouldCorrupt() {
    FixtureDef d = starterPixelBar();
    FixtureEditor ed(d);
    std::string err;

    // A channel against a geometry that does not exist.
    CHECK(!ed.addChannel(0, Attr::Red, "Nope", 0, -1, &err));
    CHECK(err.find("unknown geometry") != std::string::npos);

    // Removing the geometry twelve references still point at.
    err.clear();
    CHECK(!ed.removeGeometry("Pixel", &err));
    CHECK(err.find("still instantiated") != std::string::npos);

    // Removing one that still carries channels.
    err.clear();
    CHECK(!ed.removeGeometry("Body", &err));
    CHECK(err.find("still has channels") != std::string::npos);

    // A duplicate geometry name.
    GeometryNode dupe;
    dupe.name = "Body";
    err.clear();
    CHECK(!ed.addGeometry({}, dupe, &err));
    CHECK(err.find("already exists") != std::string::npos);

    // A reference to something missing.
    GeometryNode bad;
    bad.name = "Ghost1";
    bad.kind = GeometryKind::Reference;
    bad.referenced = "Ghost";
    err.clear();
    CHECK(!ed.addGeometry({}, bad, &err));
    CHECK(err.find("missing geometry") != std::string::npos);

    // Negative offsets.
    CHECK(!ed.setChannelOffset(0, 0, -1, -1, &err));

    // The last mode cannot be removed: a fixture with no mode patches nothing.
    err.clear();
    CHECK(!ed.removeMode(0, &err));
    CHECK(err.find("at least one mode") != std::string::npos);
    ed.addMode("Second");
    CHECK(ed.removeMode(1, &err));

    // Nothing above changed the definition.
    CompileReport c = compile(d, 0);
    CHECK(c.ok);
    CHECK_EQ(c.channelsEmitted, 37);
}

static void testRenameMovesEveryMention() {
    FixtureDef d = starterPixelBar();
    FixtureEditor ed(d);
    std::string err;

    CHECK(ed.renameGeometry("Pixel", "Cell", &err));
    CHECK(err.empty());

    // The twelve references now point at the new name, and the channels moved
    // with it — otherwise a rename silently empties the fixture.
    CHECK(d.findGeometry("Cell") != nullptr);
    CHECK(d.findGeometry("Pixel") == nullptr);
    CompileReport c = compile(d, 0);
    CHECK(c.ok);
    CHECK(c.errors.empty());
    CHECK_EQ(c.channelsEmitted, 37);
    CHECK_EQ(int(c.type.cellCount), 12);

    // The mode root moves too.
    CHECK(ed.renameGeometry("Body", "Chassis", &err));
    CHECK(d.modes[0].rootGeometry == "Chassis");
    CHECK_EQ(countIssues(validate(d, 0), Issue::Level::Error), 0);

    // Renaming onto an existing name is refused.
    CHECK(!ed.renameGeometry("Cell", "Chassis", &err));
    // Renaming to itself is a no-op, not a collision.
    CHECK(ed.renameGeometry("Cell", "Cell", &err));
}

static void testPackOffsetsClosesGapsButNotCells() {
    FixtureDef d;
    d.name = "Packed";
    FixtureEditor ed(d);
    const size_t m = ed.addMode("Default");
    ed.addChannel(m, Attr::Dimmer, {}, 0);
    ed.addChannel(m, Attr::Pan, {}, 5, 6);
    ed.addChannel(m, Attr::Shutter, {}, 10);

    CHECK(ed.packOffsets(m) > 0);
    CompileReport c = compile(d, m);
    CHECK(c.ok);
    CHECK_EQ(c.type.footprint(), 4);
    CHECK_EQ(c.type.channel(Attr::Dimmer)->coarse, 0);
    CHECK_EQ(c.type.channel(Attr::Pan)->coarse, 1);
    CHECK_EQ(c.type.channel(Attr::Pan)->fine, 2);
    CHECK_EQ(c.type.channel(Attr::Shutter)->coarse, 3);
    CHECK_EQ(ed.packOffsets(m), 0);   // idempotent

    // An already-tidy cell definition is left alone, and the master block
    // keeps its place: with cells present, whether the master sits before or
    // after them is the user's layout decision, not a mechanical one.
    FixtureDef bar = starterPixelBar();
    FixtureEditor bed(bar);
    const int before = compile(bar, 0).type.footprint();
    bed.packOffsets(0);
    CompileReport after = compile(bar, 0);
    CHECK(after.ok);
    CHECK_EQ(after.type.footprint(), before);
    CHECK_EQ(after.type.channel(Attr::Red, 1)->coarse, 1);
    CHECK_EQ(after.type.channel(Attr::Red, 12)->coarse, 34);
}

static void testGrowingAndShrinkingACellDefinition() {
    // Adding a channel to a cell definition must re-space the instances, or
    // all twelve land on top of each other. Removing one leaves a hole inside
    // every cell, which packing closes.
    FixtureDef d = starterPixelBar();
    FixtureEditor ed(d);

    CHECK(ed.addChannel(0, Attr::White, "Pixel", -1));
    CHECK(ed.addChannel(0, Attr::Amber, "Pixel", -1));
    CompileReport grown = compile(d, 0);
    CHECK(grown.ok);
    CHECK(grown.errors.empty());
    CHECK_EQ(grown.type.footprint(), 61);          // 1 + 12 * 5
    CHECK_EQ(grown.type.channel(Attr::Red, 2)->coarse, 6);
    CHECK_EQ(int(grown.type.cellCount), 12);       // still twelve, not more
    for (CellId cell = 1; cell <= 12; ++cell) CHECK(grown.type.hasAttr(Attr::Amber, cell));

    // Drop white from the middle of the cell.
    int whiteIndex = -1;
    for (size_t i = 0; i < d.modes[0].channels.size(); ++i)
        if (d.modes[0].channels[i].attr == Attr::White) whiteIndex = static_cast<int>(i);
    CHECK(whiteIndex >= 0);
    CHECK(ed.removeChannel(0, whiteIndex));

    // Still valid, but now with a hole in every cell — reported, because a
    // silent gap in twelve cells is a patch nobody can explain later.
    CompileReport holed = compile(d, 0);
    CHECK(holed.ok);
    CHECK_EQ(holed.type.footprint(), 61);
    CHECK_EQ(countIssues(validate(d, 0), Issue::Level::Info), 12);

    // Packing closes the hole inside the cell and re-spaces.
    CHECK(ed.packOffsets(0) > 0);
    CompileReport packed = compile(d, 0);
    CHECK(packed.ok);
    CHECK_EQ(packed.type.footprint(), 49);         // 1 + 12 * 4
    CHECK_EQ(packed.type.channel(Attr::Red, 2)->coarse, 5);
    CHECK_EQ(countIssues(validate(d, 0), Issue::Level::Info), 0);
    for (CellId cell = 1; cell <= 12; ++cell) {
        CHECK(packed.type.hasAttr(Attr::Amber, cell));
        CHECK(!packed.type.hasAttr(Attr::White, cell));
    }
}

static void testCompiledTypePatchesAndAddresses() {
    // The compile exists to produce something the engine can patch, so the
    // round trip through Patch is the real check.
    FixtureDef d = starterPixelBar();
    CompileReport c = compile(d, 0);
    CHECK(c.ok);

    UidGen uids;
    Patch patch(uids);
    const int ti = patch.addType(c.type);
    CHECK_EQ(patch.addRange(1, 4, ti, 1, 1), 4);

    // 37 channels each, so they sit at 1, 38, 75, 112.
    CHECK_EQ(patch.find(1)->address, 1);
    CHECK_EQ(patch.find(2)->address, 38);
    CHECK_EQ(patch.find(4)->address, 112);
    CHECK(patch.conflicts().empty());

    // Cells are addressable, which is what §1.3 wants for 12.3 selection.
    const auto cells = patch.cellsOf(1);
    CHECK_EQ(cells.size(), size_t(12));
    CHECK_EQ(int(cells.front()), 1);
    CHECK_EQ(int(cells.back()), 12);

    // Highlight derives from the taxonomy, not the geometry, so an imported
    // or authored fixture behaves the same.
    ValueLayer hl = c.type.highlightFor(1);
    CHECK(!hl.empty());
}

static void testEmptyAndDegenerateDefinitions() {
    FixtureDef d;
    // No modes at all: compile and validate both say so rather than crashing.
    CHECK(!compile(d, 0).ok);
    CHECK(anyIssueMentions(validate(d, 0), "no mode at index"));
    CHECK(tableView(d, 0).empty());
    CHECK(treeView(d, 0).empty());

    FixtureEditor ed(d);
    ed.addMode("Empty");
    CompileReport c = compile(d, 0);
    CHECK(c.ok);                        // an empty mode is legal
    CHECK(!c.warnings.empty());         // but worth saying
    CHECK(anyIssueMentions(validate(d, 0), "patches nothing"));
    CHECK(anyIssueMentions(validate(d, 0), "no name"));

    // An out-of-range mode index on every entry point.
    CHECK(tableView(d, 99).empty());
    CHECK(treeView(d, 99).empty());
    CHECK(!compile(d, 99).ok);
    CHECK(!ed.addChannel(99, Attr::Dimmer));
    CHECK(!ed.removeChannel(0, 0));
    CHECK(!ed.setChannelAttr(0, 7, Attr::Red));
    CHECK_EQ(ed.packOffsets(99), 0);

    // A geometry nobody references and nothing declares against still shows
    // in the tree, because it is where authoring starts.
    GeometryNode lone;
    lone.name = "Lone";
    CHECK(ed.addGeometry({}, lone));
    auto tree = treeView(d, 0);
    CHECK_EQ(tree.size(), size_t(1));
    CHECK(!tree[0].isTemplate);
    CHECK(tree[0].channels.empty());
}

static void testInstancesOfResolution() {
    FixtureDef d = starterPixelBar();

    // Nothing named: the fixture itself.
    auto root = instancesOf(d, {});
    CHECK_EQ(root.size(), size_t(1));
    CHECK_EQ(int(root[0].cell), 0);

    // A plain geometry: one instance, no cell.
    auto body = instancesOf(d, "Body");
    CHECK_EQ(body.size(), size_t(1));
    CHECK_EQ(int(body[0].cell), 0);
    CHECK_EQ(body[0].dmxOffset, 0);

    // The shared definition: twelve instances, in order.
    auto pixel = instancesOf(d, "Pixel");
    CHECK_EQ(pixel.size(), size_t(12));
    CHECK_EQ(int(pixel[0].cell), 1);
    CHECK_EQ(pixel[0].dmxOffset, 1);
    CHECK_EQ(int(pixel[11].cell), 12);
    CHECK_EQ(pixel[11].dmxOffset, 34);

    // One reference by name: just that instance.
    auto one = instancesOf(d, "Pixel5");
    CHECK_EQ(one.size(), size_t(1));
    CHECK_EQ(int(one[0].cell), 5);
    CHECK_EQ(one[0].dmxOffset, 13);

    // Unknown: empty, which is what compile reports as an error.
    CHECK(instancesOf(d, "Nope").empty());
}

// ---------------------------------------------------------------------------
// GDTF import produces an editable definition (§1.2)
// ---------------------------------------------------------------------------
static void testImportedProfileIsEditable() {
    GdtfReport r = parseGdtfXml(uniformBarXml());
    CHECK(r.ok);
    CHECK_EQ(r.info.modes.size(), size_t(1));

    const FixtureDef& d = r.info.def;
    CHECK(d.name == "Uniform Bar");
    CHECK(d.manufacturer == "Lumen Test");
    CHECK_EQ(d.modes.size(), size_t(1));

    // The geometry tree survived import, kinds and nesting included. A flat
    // channel list alone could not produce the tree view at all.
    const GeometryNode* yoke = d.findGeometry("Yoke");
    const GeometryNode* pixel = d.findGeometry("Pixel");
    CHECK(yoke != nullptr);
    CHECK(pixel != nullptr);
    CHECK(yoke->kind == GeometryKind::Axis);
    CHECK(pixel->kind == GeometryKind::Beam);
    CHECK(d.findGeometry("Cell1")->isReference());
    CHECK(d.findGeometry("Cell1")->referenced == "Pixel");

    // Twelve per-instance declarations collapsed into three against the
    // shared geometry, plus the master: four, not thirteen.
    CHECK_EQ(d.modes[0].channels.size(), size_t(4));
    int againstPixel = 0;
    for (const ChannelDecl& c : d.modes[0].channels)
        if (c.geometry == "Pixel") ++againstPixel;
    CHECK_EQ(againstPixel, 3);

    // And nothing about the patch changed: the definition compiles to exactly
    // what the direct parse produced. A divergence here is an editor that
    // silently rewrites fixtures.
    CompileReport c = compile(d, 0);
    const FixtureType& direct = r.info.modes[0].type;
    CHECK(c.ok);
    CHECK(c.errors.empty());
    CHECK_EQ(c.type.footprint(), direct.footprint());
    CHECK_EQ(c.type.footprint(), 13);
    CHECK_EQ(int(c.type.cellCount), int(direct.cellCount));
    CHECK_EQ(int(c.type.cellCount), 4);
    CHECK_EQ(c.type.channels.size(), direct.channels.size());
    for (CellId cell = 1; cell <= 4; ++cell)
        for (Attr a : {Attr::Red, Attr::Green, Attr::Blue}) {
            const ChannelDef* mine = c.type.channel(a, cell);
            const ChannelDef* theirs = direct.channel(a, cell);
            CHECK(mine && theirs);
            CHECK_EQ(mine->coarse, theirs->coarse);
        }

    // Now the point of all of it: one edit changes every cell.
    FixtureDef editable = d;
    FixtureEditor ed(editable);
    CHECK(ed.addChannel(0, Attr::White, "Pixel", -1));
    CompileReport c2 = compile(editable, 0);
    CHECK(c2.ok);
    CHECK_EQ(c2.type.footprint(), 17);   // 1 + 4 * 4
    for (CellId cell = 1; cell <= 4; ++cell) CHECK(c2.type.hasAttr(Attr::White, cell));
    CHECK(!c2.type.hasAttr(Attr::White, 0));
}

static void testRaggedCellsAreNotFlattened() {
    // A bar whose last cell differs must keep its per-instance declarations.
    // Averaging it into a uniform cell would hand the user a wrong patch.
    GdtfReport r = parseGdtfXml(raggedBarXml());
    CHECK(r.ok);

    const FixtureDef& d = r.info.def;
    CHECK(d.modes[0].channels.size() > size_t(4));   // not collapsed
    bool declaredPerCell = false;
    for (const ChannelDecl& c : d.modes[0].channels)
        if (c.geometry == "Cell1") declaredPerCell = true;
    CHECK(declaredPerCell);

    // And it says which instance stopped it, rather than failing silently.
    bool explained = false;
    for (const auto& w : r.warnings)
        if (w.find("not collapsed") != std::string::npos &&
            w.find("Cell4") != std::string::npos)
            explained = true;
    CHECK(explained);

    // The patch is still correct, which is what matters most.
    CompileReport c = compile(d, 0);
    const FixtureType& direct = r.info.modes[0].type;
    CHECK(c.ok);
    CHECK_EQ(c.type.footprint(), direct.footprint());
    CHECK_EQ(c.type.channels.size(), direct.channels.size());
    CHECK(c.type.hasAttr(Attr::Blue, 3));
    CHECK(!c.type.hasAttr(Attr::Blue, 4));
}

static void testImportSynthesisesAMissingTarget() {
    // A profile that references a geometry it never defines is invalid GDTF
    // and exists anyway. It should open in the editor rather than refuse.
    const std::string xml =
        "<GDTF><FixtureType Name=\"Sloppy\">"
        "<Geometries><Geometry Name=\"Base\">"
        "<GeometryReference Name=\"C1\" Geometry=\"Ghost\"/>"
        "<GeometryReference Name=\"C2\" Geometry=\"Ghost\"/>"
        "</Geometry></Geometries>"
        "<DMXModes><DMXMode Name=\"M\" Geometry=\"Base\"><DMXChannels>"
        "<DMXChannel Offset=\"1\" Geometry=\"C1\"><LogicalChannel "
        "Attribute=\"Dimmer\"><ChannelFunction Name=\"d\" "
        "Attribute=\"Dimmer\"/></LogicalChannel></DMXChannel>"
        "<DMXChannel Offset=\"2\" Geometry=\"C2\"><LogicalChannel "
        "Attribute=\"Dimmer\"><ChannelFunction Name=\"d\" "
        "Attribute=\"Dimmer\"/></LogicalChannel></DMXChannel>"
        "</DMXChannels></DMXMode></DMXModes></FixtureType></GDTF>";

    GdtfReport r = parseGdtfXml(xml);
    CHECK(r.ok);
    CHECK(r.info.def.findGeometry("Ghost") != nullptr);
    bool warned = false;
    for (const auto& w : r.warnings)
        if (w.find("without defining it") != std::string::npos) warned = true;
    CHECK(warned);

    CompileReport c = compile(r.info.def, 0);
    CHECK(c.ok);
    CHECK_EQ(int(c.type.cellCount), 2);
    CHECK_EQ(c.type.footprint(), 2);
    CHECK_EQ(countIssues(validate(r.info.def, 0), Issue::Level::Error), 0);
}

static void testNormaliseIsIdempotentAndSafe() {
    GdtfReport r = parseGdtfXml(uniformBarXml());
    CHECK(r.ok);
    FixtureDef d = r.info.def;

    // Already normalised on import, so a second pass changes nothing.
    const NormaliseReport again = normaliseInstances(d, 0);
    CHECK_EQ(again.collapsed, 0);
    CHECK_EQ(again.declarationsRemoved, 0);
    CHECK_EQ(d.modes[0].channels.size(), size_t(4));

    // Collapsing a hand-built definition reports what it did.
    FixtureDef hand;
    hand.name = "Hand";
    GeometryNode body;
    body.name = "Body";
    GeometryNode cellGeo;
    cellGeo.name = "Cell";
    hand.geometries.push_back(std::move(body));
    hand.geometries.push_back(std::move(cellGeo));
    FixtureEditor ed(hand);
    const size_t m = ed.addMode("Mode", "Body");
    for (int i = 1; i <= 3; ++i) {
        GeometryNode ref;
        ref.name = "C" + std::to_string(i);
        ref.kind = GeometryKind::Reference;
        ref.referenced = "Cell";
        ref.dmxOffset = (i - 1) * 2;
        CHECK(ed.addGeometry("Body", std::move(ref)));
    }
    for (int i = 1; i <= 3; ++i) {
        const std::string n = "C" + std::to_string(i);
        CHECK(ed.addChannel(m, Attr::Red, n, 0));
        CHECK(ed.addChannel(m, Attr::Green, n, 1));
    }
    const CompileReport before = compile(hand, m);
    CHECK(before.ok);

    const NormaliseReport nr = normaliseInstances(hand, m);
    CHECK_EQ(nr.collapsed, 1);
    CHECK_EQ(nr.declarationsRemoved, 4);     // six became two
    CHECK_EQ(hand.modes[m].channels.size(), size_t(2));
    CHECK(nr.kept.empty());

    // Same compiled result — collapsing is an editing convenience, not a
    // change to the fixture.
    const CompileReport after = compile(hand, m);
    CHECK(after.ok);
    CHECK_EQ(after.type.footprint(), before.type.footprint());
    CHECK_EQ(after.type.channels.size(), before.type.channels.size());
    CHECK_EQ(int(after.type.cellCount), int(before.type.cellCount));
    for (CellId cell = 1; cell <= 3; ++cell) {
        CHECK_EQ(after.type.channel(Attr::Red, cell)->coarse,
                 before.type.channel(Attr::Red, cell)->coarse);
        CHECK_EQ(after.type.channel(Attr::Green, cell)->coarse,
                 before.type.channel(Attr::Green, cell)->coarse);
    }
}

int main() {
    std::printf("lumen editor\n");
    testSimpleFixtureNeedsNoGeometry();
    testOneDefinitionManyCells();
    testBothViewsAreLiveOnOneModel();
    testAppendRespectsWhatOffsetsAreRelativeTo();
    testAddInstancesInfersTheStride();
    testCellsNumberInDocumentOrder();
    testSixteenBitAndRangeLabels();
    testOverlapIsAnError();
    testValidationFindsStructuralMistakes();
    testEditOperationsRefuseWhatWouldCorrupt();
    testRenameMovesEveryMention();
    testPackOffsetsClosesGapsButNotCells();
    testGrowingAndShrinkingACellDefinition();
    testCompiledTypePatchesAndAddresses();
    testEmptyAndDegenerateDefinitions();
    testInstancesOfResolution();
    testImportedProfileIsEditable();
    testRaggedCellsAreNotFlattened();
    testImportSynthesisesAMissingTarget();
    testNormaliseIsIdempotentAndSafe();
    return check::report("editor");
}
