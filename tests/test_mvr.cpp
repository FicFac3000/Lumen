// test_mvr.cpp — MVR import: matrices, addresses, scene walk, apply.
#include "Check.hpp"

#include "lumen/Command.hpp"
#include "lumen/Group.hpp"
#include "lumen/Mvr.hpp"
#include "lumen/Patch.hpp"
#include "lumen/Uid.hpp"
#include "lumen/Zip.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>

using namespace lumen;
namespace fs = std::filesystem;

namespace {

// A two-mode profile, so mode selection is actually exercised.
const char* kSpotXml = R"XML(<?xml version="1.0" encoding="UTF-8"?>
<GDTF DataVersion="1.1">
  <FixtureType Name="Test Spot" ShortName="TS" Manufacturer="Lumen">
    <AttributeDefinitions>
      <Attributes>
        <Attribute Name="Dimmer" Pretty="Dim"/>
        <Attribute Name="Pan" Pretty="P"/>
        <Attribute Name="Tilt" Pretty="T"/>
      </Attributes>
    </AttributeDefinitions>
    <DMXModes>
      <DMXMode Name="Basic" Geometry="Base">
        <DMXChannels>
          <DMXChannel DMXBreak="1" Offset="1" Geometry="Base">
            <LogicalChannel Attribute="Dimmer">
              <ChannelFunction Name="Dimmer" Attribute="Dimmer" Default="0/1"/>
            </LogicalChannel>
          </DMXChannel>
        </DMXChannels>
      </DMXMode>
      <DMXMode Name="Extended" Geometry="Base">
        <DMXChannels>
          <DMXChannel DMXBreak="1" Offset="1" Geometry="Base">
            <LogicalChannel Attribute="Dimmer">
              <ChannelFunction Name="Dimmer" Attribute="Dimmer" Default="0/1"/>
            </LogicalChannel>
          </DMXChannel>
          <DMXChannel DMXBreak="1" Offset="2,3" Geometry="Base">
            <LogicalChannel Attribute="Pan">
              <ChannelFunction Name="Pan" Attribute="Pan" Default="32768/2"/>
            </LogicalChannel>
          </DMXChannel>
          <DMXChannel DMXBreak="1" Offset="4,5" Geometry="Base">
            <LogicalChannel Attribute="Tilt">
              <ChannelFunction Name="Tilt" Attribute="Tilt" Default="32768/2"/>
            </LogicalChannel>
          </DMXChannel>
        </DMXChannels>
      </DMXMode>
    </DMXModes>
  </FixtureType>
</GDTF>
)XML";

// Two layers, a nested GroupObject, dotted and absolute addresses, and one
// fixture asking for a mode the profile does not have.
const char* kSceneXml = R"XML(<?xml version="1.0" encoding="UTF-8"?>
<GeneralSceneDescription verMajor="1" verMinor="5">
  <Scene>
    <Layers>
      <Layer name="Front Truss" uuid="L1">
        <ChildList>
          <Fixture name="Spot 1" uuid="F1">
            <GDTFSpec>TestSpot.gdtf</GDTFSpec>
            <GDTFMode>Extended</GDTFMode>
            <Matrix>{1,0,0}{0,1,0}{0,0,1}{1500,-2000,6000}</Matrix>
            <Addresses><Address break="0">1</Address></Addresses>
            <FixtureID>11</FixtureID>
          </Fixture>
          <GroupObject name="Inner" uuid="G1">
            <ChildList>
              <Fixture name="Spot 2" uuid="F2">
                <GDTFSpec>TestSpot.gdtf</GDTFSpec>
                <GDTFMode>Extended</GDTFMode>
                <Matrix>{1,0,0}{0,1,0}{0,0,1}{-1500,-2000,6000}</Matrix>
                <Addresses><Address break="0">2.100</Address></Addresses>
                <FixtureID>12</FixtureID>
              </Fixture>
            </ChildList>
          </GroupObject>
        </ChildList>
      </Layer>
      <Layer name="Floor" uuid="L2">
        <ChildList>
          <Fixture name="Spot 3" uuid="F3">
            <GDTFSpec>TestSpot.gdtf</GDTFSpec>
            <GDTFMode>NoSuchMode</GDTFMode>
            <Addresses><Address break="0">513</Address></Addresses>
            <FixtureID>13</FixtureID>
          </Fixture>
          <Fixture name="Mystery" uuid="F4">
            <GDTFSpec>Absent.gdtf</GDTFSpec>
            <GDTFMode>Basic</GDTFMode>
            <Addresses><Address break="0">100</Address></Addresses>
            <FixtureID>14</FixtureID>
          </Fixture>
        </ChildList>
      </Layer>
    </Layers>
  </Scene>
</GeneralSceneDescription>
)XML";

std::string tmpDir() {
    const auto d = fs::temp_directory_path() / "lumen_mvr_tests";
    fs::create_directories(d);
    return d.string();
}

bool haveZip() { return std::system("which zip > /dev/null 2>&1") == 0; }

// Builds a real .mvr: a ZIP holding the scene description plus a real .gdtf
// (itself a ZIP), which is what makes an MVR a complete handover.
bool makeMvrPackage(const std::string& outPath) {
    const std::string work = tmpDir() + "/pkg";
    fs::remove_all(work);
    fs::create_directories(work + "/inner");
    {
        std::ofstream o(work + "/inner/description.xml");
        o << kSpotXml;
    }
    if (std::system(("cd " + work + "/inner && zip -q -X ../TestSpot.gdtf description.xml"
                     " > /dev/null 2>&1").c_str()) != 0)
        return false;
    {
        std::ofstream o(work + "/GeneralSceneDescription.xml");
        o << kSceneXml;
    }
    fs::remove(outPath);
    const std::string cmd = "cd " + work + " && zip -q -X " + outPath +
                            " GeneralSceneDescription.xml TestSpot.gdtf > /dev/null 2>&1";
    return std::system(cmd.c_str()) == 0 && fs::exists(outPath);
}

} // namespace

// ---------------------------------------------------------------------------
static void testMatrixParsing() {
    MvrMatrix m = parseMvrMatrix("{1,0,0}{0,1,0}{0,0,1}{1000,2000,3000}");
    CHECK(m.parsed);
    CHECK_NEAR(m.tx, 1000.f);
    CHECK_NEAR(m.ty, 2000.f);
    CHECK_NEAR(m.tz, 3000.f);
    CHECK_NEAR(m.m[0][0], 1.f);
    CHECK_NEAR(m.m[1][1], 1.f);

    // Whitespace and negatives, as written by real exporters.
    MvrMatrix w = parseMvrMatrix("{1, 0, 0}{0, 1, 0}{0, 0, 1}{-500.5, 0, 2750.25}");
    CHECK(w.parsed);
    CHECK_NEAR(w.tx, -500.5f);
    CHECK_NEAR(w.tz, 2750.25f);

    // Too few groups is reported, not silently treated as identity-at-origin.
    CHECK(!parseMvrMatrix("{1,0,0}{0,1,0}").parsed);
    CHECK(!parseMvrMatrix("").parsed);
    CHECK(!parseMvrMatrix("garbage").parsed);

    // Vectors are COLUMNS. A 90 degree rotation about Z written column-wise is
    // {0,1,0}{-1,0,0}{0,0,1}; if columns and rows were swapped this would come
    // back as -90.
    MvrMatrix rz = parseMvrMatrix("{0,1,0}{-1,0,0}{0,0,1}{0,0,0}");
    CHECK(rz.parsed);
    CHECK_NEAR(rz.m[1][0], 1.f);
    CHECK_NEAR(rz.m[0][1], -1.f);
}

static void testEulerExtraction() {
    float rx = 0, ry = 0, rz = 0;

    eulerFromMatrix(parseMvrMatrix("{1,0,0}{0,1,0}{0,0,1}{0,0,0}"), rx, ry, rz);
    CHECK_NEAR(rx, 0.f);
    CHECK_NEAR(ry, 0.f);
    CHECK_NEAR(rz, 0.f);

    // 90 degrees about Z.
    eulerFromMatrix(parseMvrMatrix("{0,1,0}{-1,0,0}{0,0,1}{0,0,0}"), rx, ry, rz);
    CHECK_NEAR(rz, 90.f);
    CHECK_NEAR(ry, 0.f);

    // 180 degrees about X — a fixture hung upside down, which is the common
    // case in a real plot.
    eulerFromMatrix(parseMvrMatrix("{1,0,0}{0,-1,0}{0,0,-1}{0,0,0}"), rx, ry, rz);
    CHECK_NEAR(std::fabs(rx), 180.f);

    // The gimbal-lock case returns angles rather than NaN: pointing straight
    // down is exactly what a hung mover does.
    eulerFromMatrix(parseMvrMatrix("{0,0,-1}{0,1,0}{1,0,0}{0,0,0}"), rx, ry, rz);
    CHECK(!std::isnan(rx));
    CHECK(!std::isnan(ry));
    CHECK(!std::isnan(rz));
    CHECK_NEAR(ry, 90.f);
}

static void testAddressDecomposition() {
    int u = 0, c = 0;

    decomposeAddress("1", u, c);
    CHECK_EQ(u, 1);
    CHECK_EQ(c, 1);

    decomposeAddress("512", u, c);
    CHECK_EQ(u, 1);
    CHECK_EQ(c, 512);

    // The boundary is the part that is easy to get wrong by one.
    decomposeAddress("513", u, c);
    CHECK_EQ(u, 2);
    CHECK_EQ(c, 1);

    decomposeAddress("1024", u, c);
    CHECK_EQ(u, 2);
    CHECK_EQ(c, 512);

    decomposeAddress("1025", u, c);
    CHECK_EQ(u, 3);
    CHECK_EQ(c, 1);

    // Dotted form.
    decomposeAddress("4.100", u, c);
    CHECK_EQ(u, 4);
    CHECK_EQ(c, 100);

    decomposeAddress(" 2.1 ", u, c);
    CHECK_EQ(u, 2);
    CHECK_EQ(c, 1);

    // Nonsense lands on a patchable address rather than universe 0 channel 0.
    decomposeAddress("", u, c);
    CHECK_EQ(u, 1);
    CHECK_EQ(c, 1);
    decomposeAddress("0", u, c);
    CHECK_EQ(u, 1);
    CHECK_EQ(c, 1);
    decomposeAddress("-7", u, c);
    CHECK_EQ(u, 1);
    CHECK_EQ(c, 1);
}

static void testSceneWalk() {
    MvrReport r = parseMvrXml(kSceneXml);
    CHECK(r.ok);
    CHECK_EQ(r.layers.size(), size_t(2));
    CHECK(r.layers[0] == "Front Truss");
    CHECK(r.layers[1] == "Floor");

    // Four fixtures, including the one nested inside a GroupObject.
    CHECK_EQ(r.fixtures.size(), size_t(4));
    CHECK(r.fixtures[0].name == "Spot 1");
    CHECK(r.fixtures[1].name == "Spot 2");
    CHECK(r.fixtures[1].layer == "Front Truss");  // the layer, not the group
    CHECK(r.fixtures[2].layer == "Floor");

    CHECK(r.fixtures[0].uuid == "F1");
    CHECK(r.fixtures[0].gdtfSpec == "TestSpot.gdtf");
    CHECK(r.fixtures[0].gdtfMode == "Extended");
    CHECK_EQ(int(r.fixtures[0].fixtureId), 11);

    // Addresses: absolute on 1, dotted on 2, and absolute across the boundary.
    CHECK_EQ(r.fixtures[0].universe, 1);
    CHECK_EQ(r.fixtures[0].address, 1);
    CHECK_EQ(r.fixtures[1].universe, 2);
    CHECK_EQ(r.fixtures[1].address, 100);
    CHECK_EQ(r.fixtures[2].universe, 2);
    CHECK_EQ(r.fixtures[2].address, 1);

    // Millimetres become metres.
    CHECK(r.fixtures[0].positioned);
    CHECK_NEAR(r.fixtures[0].x, 1.5f);
    CHECK_NEAR(r.fixtures[0].y, -2.f);
    CHECK_NEAR(r.fixtures[0].z, 6.f);
    CHECK_NEAR(r.fixtures[1].x, -1.5f);

    // A fixture with no Matrix is marked unpositioned rather than parked at
    // the origin, so the visualiser can tell them apart.
    CHECK(!r.fixtures[2].positioned);

    // Parsing the XML alone resolves no profiles — they live in the archive.
    CHECK_EQ(r.profiles.size(), size_t(0));
}

static void testMalformedScene() {
    MvrReport bad = parseMvrXml("<not-a-scene/>");
    CHECK(!bad.ok);
    CHECK(!bad.error.empty());

    MvrReport broken = parseMvrXml("<Scene><Layers>");
    CHECK(!broken.ok);

    // A scene with no fixtures parses but says so.
    MvrReport empty = parseMvrXml("<Scene><Layers><Layer name=\"A\"/></Layers></Scene>");
    CHECK(empty.ok);
    CHECK_EQ(empty.fixtures.size(), size_t(0));
    CHECK_EQ(empty.layers.size(), size_t(1));
    CHECK(!empty.warnings.empty());
}

static void testPackageLoadAndApply() {
    if (!haveZip()) {
        std::printf("      (skipped: system zip unavailable)\n");
        return;
    }
    const std::string pkg = tmpDir() + "/plot.mvr";
    if (!makeMvrPackage(pkg)) {
        std::printf("      (skipped: could not build an mvr)\n");
        return;
    }

    MvrReport r = loadMvrFile(pkg);
    CHECK(r.ok);
    CHECK_EQ(r.fixtures.size(), size_t(4));

    // The embedded profile is resolved; the referenced-but-absent one is a
    // warning rather than a hard failure, because one bad reference must not
    // cost the designer the whole plot.
    CHECK_EQ(r.profiles.size(), size_t(1));
    CHECK(r.profiles.count("TestSpot.gdtf") == 1);
    CHECK_EQ(r.profiles["TestSpot.gdtf"].modes.size(), size_t(2));
    CHECK(r.profiles["TestSpot.gdtf"].name == "Test Spot");
    bool warnedAbsent = false;
    for (const auto& w : r.warnings)
        if (w.find("Absent.gdtf") != std::string::npos) warnedAbsent = true;
    CHECK(warnedAbsent);

    UidGen uids;
    Patch patch(uids);
    GroupStore groups(uids);
    MvrApplyReport ap = applyMvr(patch, &groups, r);

    // Both modes register as types; three fixtures patch, the one with no
    // profile is skipped.
    CHECK_EQ(ap.typesAdded, 2);
    CHECK_EQ(ap.patched, 3);
    CHECK_EQ(ap.skipped, 1);
    CHECK_EQ(patch.size(), size_t(3));

    // Ids come from the MVR, not from the import order.
    const Fixture* s1 = patch.find(11);
    CHECK(s1 != nullptr);
    CHECK(s1->name == "Spot 1");
    CHECK_EQ(s1->universe, 1);
    CHECK_EQ(s1->address, 1);

    // Position survived, in metres.
    CHECK(s1->positioned);
    CHECK_NEAR(s1->x, 1.5f);
    CHECK_NEAR(s1->z, 6.f);

    // The layer became a tag and the type is the 5-channel Extended mode.
    bool tagged = false;
    for (const auto& t : s1->tags)
        if (t == "Front Truss") tagged = true;
    CHECK(tagged);
    const FixtureType* ty = patch.typeOf(11);
    CHECK(ty != nullptr);
    CHECK_EQ(ty->footprint(), 5);
    CHECK(ty->hasAttr(Attr::Pan));

    // The fixture asking for a mode that does not exist fell back to the
    // profile's first mode and said so, rather than being dropped.
    const Fixture* s3 = patch.find(13);
    CHECK(s3 != nullptr);
    const FixtureType* ty3 = patch.typeOf(13);
    CHECK(ty3 != nullptr);
    CHECK_EQ(ty3->footprint(), 1);  // Basic
    bool warnedMode = false;
    for (const auto& w : ap.warnings)
        if (w.find("NoSuchMode") != std::string::npos) warnedMode = true;
    CHECK(warnedMode);

    // One group per layer, in document order, carrying only what patched.
    CHECK_EQ(ap.groupsCreated, 2);
    const GroupDef* front = groups.byName("Front Truss");
    CHECK(front != nullptr);
    CHECK_EQ(front->members.size(), size_t(2));
    CHECK_EQ(int(front->members[0].fixture), 11);
    CHECK_EQ(int(front->members[1].fixture), 12);
    const GroupDef* floor = groups.byName("Floor");
    CHECK(floor != nullptr);
    CHECK_EQ(floor->members.size(), size_t(1));  // the unpatched one is absent

    // The groups resolve against the patch, which is what the rest of the
    // engine actually consumes.
    GroupResolution res = groups.resolve(front->uid, patch);
    CHECK_EQ(res.targets.size(), size_t(2));
}

static void testApplyWithoutGroupsAndIdClashes() {
    if (!haveZip()) return;
    const std::string pkg = tmpDir() + "/plot.mvr";
    if (!fs::exists(pkg) && !makeMvrPackage(pkg)) return;

    MvrReport r = loadMvrFile(pkg);
    CHECK(r.ok);

    UidGen uids;
    Patch patch(uids);

    // A show that already owns fixture 11. The import must not overwrite it or
    // refuse; it renumbers and reports.
    installStarterTypes(patch);
    Patch::PatchRequest sitting;
    sitting.id = 11;
    sitting.name = "Already here";
    sitting.typeIndex = 0;
    sitting.universe = 7;
    sitting.address = 1;
    CHECK(patch.add(sitting));

    MvrApplyReport ap = applyMvr(patch, nullptr, r);  // no group store
    CHECK_EQ(ap.groupsCreated, 0);
    CHECK_EQ(ap.patched, 3);
    CHECK(patch.find(11)->name == "Already here");

    bool warnedClash = false;
    for (const auto& w : ap.warnings)
        if (w.find("already in use") != std::string::npos) warnedClash = true;
    CHECK(warnedClash);

    // The renumbered import is still there under some other id, with its name.
    int found = 0;
    for (const Fixture& f : patch.fixtures())
        if (f.name == "Spot 1" || f.name == "Spot 2" || f.name == "Spot 3") ++found;
    CHECK_EQ(found, 3);
}

static void testMissingAndBrokenPackages() {
    MvrReport gone = loadMvrFile(tmpDir() + "/does-not-exist.mvr");
    CHECK(!gone.ok);
    CHECK(!gone.error.empty());

    const std::string junk = tmpDir() + "/junk.mvr";
    {
        std::ofstream o(junk, std::ios::binary);
        o << "not an archive at all";
    }
    MvrReport bad = loadMvrFile(junk);
    CHECK(!bad.ok);

    // A ZIP with no scene description fails with a message naming what is
    // missing, rather than reporting an empty plot.
    if (haveZip()) {
        const std::string work = tmpDir() + "/noscene";
        fs::remove_all(work);
        fs::create_directories(work);
        {
            std::ofstream o(work + "/readme.txt");
            o << "hello";
        }
        const std::string out = tmpDir() + "/noscene.mvr";
        fs::remove(out);
        if (std::system(("cd " + work + " && zip -q -X " + out +
                         " readme.txt > /dev/null 2>&1").c_str()) == 0) {
            MvrReport r = loadMvrFile(out);
            CHECK(!r.ok);
            CHECK(r.error.find("GeneralSceneDescription") != std::string::npos);
        }
    }
}

// The command layer is the only mutation path (§16.2), so the import has to
// be reachable through it, not just as a library call.
static void testImportThroughTheConsole() {
    if (!haveZip()) return;
    const std::string pkg = tmpDir() + "/plot.mvr";
    if (!fs::exists(pkg) && !makeMvrPackage(pkg)) return;

    Engine eng;
    Console con(eng);

    Result r = con.execute("ImportMvr \"" + pkg + "\"");
    CHECK(r.parsed);
    CHECK(r.ok);
    CHECK_EQ(eng.patch.size(), size_t(3));
    CHECK_EQ(eng.groups.size(), size_t(2));
    CHECK(r.message.find("3 patched") != std::string::npos);
    // The skipped fixture is reported rather than hidden.
    CHECK(!r.rows.empty());

    // The imported groups are visible to introspection.
    Result ls = con.execute("List groups");
    CHECK(ls.ok);
    CHECK_EQ(ls.rows.size(), size_t(2));

    // The patch is addressable from the command line straight after import.
    CHECK(con.execute("Fixture 11 At 100").ok);
    eng.tick();
    const Fixture* f = eng.patch.find(11);
    CHECK(f != nullptr);
    const auto dim = eng.merged().get(makeKey(11, 0, Attr::Dimmer));
    CHECK(dim.has_value());
    CHECK_NEAR(*dim, 1.0f);

    // A bad path fails with a message and changes nothing.
    Result bad = con.execute("ImportMvr \"/nope/missing.mvr\"");
    CHECK(bad.parsed);
    CHECK(!bad.ok);
    CHECK_EQ(eng.patch.size(), size_t(3));

    // An agent cannot import: it adds to the live patch (§16.5).
    con.setPrivilege(Privilege::Any);
    Result denied = con.execute("ImportMvr \"" + pkg + "\"");
    CHECK(!denied.ok);
    con.setPrivilege(Privilege::Operator);
}

static void testGdtfImportCommand() {
    if (!haveZip()) return;
    const std::string work = tmpDir() + "/gdtfcmd";
    fs::remove_all(work);
    fs::create_directories(work);
    {
        std::ofstream o(work + "/description.xml");
        o << kSpotXml;
    }
    const std::string pkg = tmpDir() + "/spot.gdtf";
    fs::remove(pkg);
    if (std::system(("cd " + work + " && zip -q -X " + pkg +
                     " description.xml > /dev/null 2>&1").c_str()) != 0)
        return;

    Engine eng;
    Console con(eng);

    // Both modes by default.
    Result all = con.execute("ImportGdtf \"" + pkg + "\"");
    CHECK(all.ok);
    CHECK_EQ(eng.patch.typeCount(), 2);

    // Naming one mode imports only that one — and re-importing replaces
    // rather than duplicating, because addType keys on the name.
    Engine eng2;
    Console con2(eng2);
    Result one = con2.execute("ImportGdtf \"" + pkg + "\" \"Basic\"");
    CHECK(one.ok);
    CHECK_EQ(eng2.patch.typeCount(), 1);
    CHECK(con2.execute("ImportGdtf \"" + pkg + "\" \"Basic\"").ok);
    CHECK_EQ(eng2.patch.typeCount(), 1);

    // A mode that is not there is refused, not silently ignored.
    Result nope = con2.execute("ImportGdtf \"" + pkg + "\" \"Nonexistent\"");
    CHECK(nope.parsed);
    CHECK(!nope.ok);

    // The imported type is immediately patchable through the command line.
    CHECK(con2.execute("Fixture 1 Thru 4 Patch \"Test Spot / Basic\"").ok);
    CHECK_EQ(eng2.patch.size(), size_t(4));
}

// ---------------------------------------------------------------------------
// Export (§17.5)
// ---------------------------------------------------------------------------
static void testPlotRoundTripsBackOut() {
    if (!haveZip()) return;
    const std::string pkg = tmpDir() + "/plot.mvr";
    if (!fs::exists(pkg) && !makeMvrPackage(pkg)) return;

    // Import the plot, keeping the profiles in the library — which is what
    // makes an export faithful rather than a reconstruction.
    Engine a;
    MvrReport m = loadMvrFile(pkg);
    CHECK(m.ok);

    ZipReader zip;
    std::string err;
    CHECK(zip.openFile(pkg, &err));
    std::vector<uint8_t> originalProfile;
    for (const auto& [spec, info] : m.profiles) {
        const auto bytes = zip.read(spec, &err);
        CHECK(bytes.has_value());
        if (!bytes) continue;
        originalProfile = *bytes;
        CHECK(a.library.importPackage(*bytes, spec).ok);
    }
    CHECK(!originalProfile.empty());

    MvrApplyReport ap = applyMvr(a.patch, &a.groups, m);
    CHECK_EQ(ap.patched, 3);

    // Give one fixture a real orientation, so the matrix is not just identity.
    CHECK(a.patch.setPosition(11, 1.5f, -2.f, 6.f, -30.f, 15.f, 45.f));

    const std::string out = tmpDir() + "/exported.mvr";
    fs::remove(out);
    MvrWriteReport w = writeMvrFile(a.patch, a.library, out);
    CHECK(w.ok);
    CHECK(w.error.empty());
    CHECK_EQ(w.fixtures, 3);
    CHECK_EQ(w.skipped, 0);
    CHECK_EQ(w.layers, 2);
    CHECK_EQ(w.profiles, 1);
    CHECK(w.regenerated.empty());   // nothing was edited, so nothing was rebuilt
    CHECK(fs::exists(out));

    // The embedded profile is the designer's own file, byte for byte.
    ZipReader back;
    CHECK(back.openFile(out, &err));
    CHECK(back.has("GeneralSceneDescription.xml"));
    CHECK(back.has("TestSpot.gdtf"));
    const auto embedded = back.read("TestSpot.gdtf", &err);
    CHECK(embedded.has_value());
    CHECK(embedded.value_or(std::vector<uint8_t>()) == originalProfile);

    // And the whole thing re-imports into an identical patch.
    MvrReport again = loadMvrFile(out);
    CHECK(again.ok);
    CHECK_EQ(again.fixtures.size(), size_t(3));
    CHECK_EQ(again.layers.size(), size_t(2));
    CHECK_EQ(again.profiles.size(), size_t(1));
    CHECK(again.warnings.empty());

    Engine b;
    MvrApplyReport ap2 = applyMvr(b.patch, &b.groups, again);
    CHECK_EQ(ap2.patched, 3);
    CHECK_EQ(ap2.skipped, 0);
    CHECK_EQ(ap2.groupsCreated, 2);
    CHECK_EQ(b.patch.size(), a.patch.size());

    for (const Fixture& f : a.patch.fixtures()) {
        const Fixture* g = b.patch.find(f.id);
        CHECK(g != nullptr);
        if (!g) continue;
        CHECK(g->name == f.name);
        CHECK_EQ(g->universe, f.universe);
        CHECK_EQ(g->address, f.address);
        CHECK_NEAR(g->x, f.x);
        CHECK_NEAR(g->y, f.y);
        CHECK_NEAR(g->z, f.z);
        // The mode came back too, so the footprint matches rather than
        // defaulting to the profile's first mode.
        CHECK_EQ(b.patch.type(g->typeIndex).footprint(),
                 a.patch.type(f.typeIndex).footprint());
    }

    // Layers survived as groups, which is where the tags came from.
    CHECK(b.groups.byName("Front Truss") != nullptr);
    CHECK(b.groups.byName("Floor") != nullptr);

    // Rotation survived the matrix. 180 degrees comes back as -180 — the same
    // orientation written the other way round — so this checks the general
    // case rather than that one.
    const Fixture* rotated = b.patch.find(11);
    CHECK(rotated != nullptr);
    if (rotated) {
        CHECK_NEAR(rotated->rotX, -30.f);
        CHECK_NEAR(rotated->rotY, 15.f);
        CHECK_NEAR(rotated->rotZ, 45.f);
    }
}

static void testExportSkipsWhatItCannotDescribe() {
    // A fixture patched from a type with no library profile behind it cannot
    // be written: a dangling GDTFSpec makes the whole archive unusable, so it
    // is skipped and named instead.
    Engine eng;
    installStarterTypes(eng.patch);
    Patch::PatchRequest req;
    req.id = 1;
    req.name = "Orphan";
    req.typeIndex = eng.patch.findType("LED Par RGBW");
    CHECK(eng.patch.add(req));

    MvrWriteReport rep;
    const std::string xml = writeMvrXml(eng.patch, eng.library, "Lumen", &rep);
    CHECK(rep.ok);
    CHECK_EQ(rep.fixtures, 0);
    CHECK_EQ(rep.skipped, 1);
    CHECK(!rep.warnings.empty());
    CHECK(rep.warnings[0].find("no profile in the library") != std::string::npos);
    CHECK(xml.find("Orphan") == std::string::npos);

    // With the profile in the library, the same fixture writes fine.
    LibraryEntry& e = eng.library.addAuthored(starterMovingSpot());
    const auto pub = eng.library.publish(e.uid, 0, eng.patch);
    CHECK(pub.ok);
    Patch::PatchRequest req2;
    req2.id = 2;
    req2.name = "Known";
    req2.typeIndex = pub.typeIndex;
    req2.address = 100;
    CHECK(eng.patch.add(req2));

    MvrWriteReport rep2;
    const std::string xml2 = writeMvrXml(eng.patch, eng.library, "Lumen", &rep2);
    CHECK_EQ(rep2.fixtures, 1);
    CHECK_EQ(rep2.skipped, 1);   // the orphan, still
    CHECK(xml2.find("Known") != std::string::npos);
    CHECK(xml2.find("<GDTFMode>Standard</GDTFMode>") != std::string::npos);

    // An authored profile has no original, so the export says it was written
    // from the model rather than copied.
    const std::string out = tmpDir() + "/authored.mvr";
    fs::remove(out);
    MvrWriteReport rep3 = writeMvrFile(eng.patch, eng.library, out);
    CHECK(rep3.ok);
    CHECK_EQ(rep3.profiles, 1);
    CHECK_EQ(rep3.regenerated.size(), size_t(1));

    // It still re-imports, which is the test that matters.
    MvrReport back = loadMvrFile(out);
    CHECK(back.ok);
    CHECK_EQ(back.fixtures.size(), size_t(1));
    CHECK_EQ(back.profiles.size(), size_t(1));
    Engine b;
    CHECK_EQ(applyMvr(b.patch, nullptr, back).patched, 1);
    const Fixture* k = b.patch.find(2);
    CHECK(k != nullptr);
    if (k) {
        CHECK(k->name == "Known");
        CHECK_EQ(k->address, 100);
        CHECK_EQ(b.patch.type(k->typeIndex).footprint(), 8);
    }
}

static void testExportThroughTheConsole() {
    Engine eng;
    Console con(eng);
    LibraryEntry& e = eng.library.addAuthored(starterMovingSpot());
    const auto pub = eng.library.publish(e.uid, 0, eng.patch);
    CHECK(pub.ok);
    CHECK(con.execute("Fixture 1 Thru 4 Patch \"Moving Spot / Standard\"").ok);

    const std::string out = tmpDir() + "/console.mvr";
    fs::remove(out);
    Result r = con.execute("ExportMvr \"" + out + "\"");
    CHECK(r.parsed);
    CHECK(r.ok);
    CHECK(fs::exists(out));
    CHECK(r.message.find("4 fixtures") != std::string::npos);

    MvrReport back = loadMvrFile(out);
    CHECK(back.ok);
    CHECK_EQ(back.fixtures.size(), size_t(4));

    // Nothing patched is a failed export, not a quiet success: an archive with
    // no fixtures in it is not a plot.
    Engine blank;
    Console con2(blank);
    Result bad = con2.execute("ExportMvr \"" + tmpDir() + "/blank.mvr\"");
    CHECK(bad.parsed);
    CHECK(!bad.ok);

    CHECK(!con.execute("ExportMvr \"\"").ok);
}

static void testExportDetails() {
    Engine eng;
    LibraryEntry& e = eng.library.addAuthored(starterMovingSpot());
    const auto pub = eng.library.publish(e.uid, 0, eng.patch);
    CHECK(pub.ok);

    // Three fixtures across two layers, by tag, plus one untagged.
    for (FixtureId id = 1; id <= 3; ++id) {
        Patch::PatchRequest r;
        r.id = id;
        r.name = "F" + std::to_string(id);
        r.typeIndex = pub.typeIndex;
        r.universe = 2;
        r.address = 1 + static_cast<int>(id - 1) * 8;
        CHECK(eng.patch.add(r));
    }
    CHECK(eng.patch.addTag(1, "Truss"));
    CHECK(eng.patch.addTag(2, "Truss"));

    MvrWriteReport rep;
    const std::string xml = writeMvrXml(eng.patch, eng.library, "Floor", &rep);
    CHECK_EQ(rep.fixtures, 3);
    CHECK_EQ(rep.layers, 2);
    CHECK(xml.find("name=\"Truss\"") != std::string::npos);
    CHECK(xml.find("name=\"Floor\"") != std::string::npos);

    // Addresses go out ABSOLUTE: universe 2 channel 1 is 513.
    CHECK(xml.find("<Address break=\"0\">513</Address>") != std::string::npos);
    CHECK(xml.find("<Address break=\"0\">521</Address>") != std::string::npos);

    // Reading it back puts them in the right universe again.
    MvrReport back = parseMvrXml(xml);
    CHECK(back.ok);
    CHECK_EQ(back.fixtures.size(), size_t(3));
    for (const MvrFixture& f : back.fixtures) CHECK_EQ(f.universe, 2);

    // UUIDs are derived from the fixture UID, not drawn at random, so a
    // second export of the same show gives the same UUIDs and a plot updates
    // instead of filling with duplicates.
    const std::string again = writeMvrXml(eng.patch, eng.library, "Floor", nullptr);
    CHECK(again == xml);
    MvrReport parsed = parseMvrXml(xml);
    std::set<std::string> uuids;
    for (const MvrFixture& f : parsed.fixtures) {
        CHECK(!f.uuid.empty());
        CHECK_EQ(f.uuid.size(), size_t(36));
        CHECK(uuids.insert(f.uuid).second);   // distinct per fixture
    }

    // An empty patch is a valid, empty scene rather than an error.
    Engine blank;
    MvrWriteReport br;
    const std::string bx = writeMvrXml(blank.patch, blank.library, "Lumen", &br);
    CHECK(br.ok);
    CHECK_EQ(br.fixtures, 0);
    CHECK(parseMvrXml(bx).ok);
}

int main() {
    std::printf("lumen mvr\n");
    testMatrixParsing();
    testEulerExtraction();
    testAddressDecomposition();
    testSceneWalk();
    testMalformedScene();
    testPackageLoadAndApply();
    testApplyWithoutGroupsAndIdClashes();
    testMissingAndBrokenPackages();
    testImportThroughTheConsole();
    testGdtfImportCommand();
    testPlotRoundTripsBackOut();
    testExportSkipsWhatItCannotDescribe();
    testExportThroughTheConsole();
    testExportDetails();
    return check::report("mvr");
}
