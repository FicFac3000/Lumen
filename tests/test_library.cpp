// test_library.cpp — the ZIP writer, GDTF export, and the fixture library's
// promise that an imported package comes back out unchanged
// (SPEC §1.1, §1.4, §17.7).
#include "Check.hpp"

#include "lumen/Command.hpp"
#include "lumen/Library.hpp"
#include "lumen/Show.hpp"
#include "lumen/Zip.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace lumen;
namespace fs = std::filesystem;

namespace {

std::string tmpDir() {
    const auto d = fs::temp_directory_path() / "lumen_library_tests";
    fs::create_directories(d);
    return d.string();
}

bool haveUnzip() { return std::system("which unzip > /dev/null 2>&1") == 0; }

std::vector<uint8_t> slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
}

// A package with content Lumen's model cannot express, so "kept verbatim"
// means something: extra files, wheels, physical data.
std::string richProfileXml() {
    return R"XML(<?xml version="1.0" encoding="UTF-8"?>
<GDTF DataVersion="1.2">
 <FixtureType Name="Rich Spot" ShortName="RS" Manufacturer="Lumen Test"
              FixtureTypeID="1234" Description="has more than Lumen models">
  <AttributeDefinitions>
   <Attributes>
    <Attribute Name="Dimmer" Pretty="Dim"/>
    <Attribute Name="Pan" Pretty="P"/>
    <Attribute Name="Gobo1" Pretty="G1"/>
   </Attributes>
  </AttributeDefinitions>
  <Wheels>
   <Wheel Name="Gobo1">
    <Slot Name="Open" Color="0.3127,0.3290,100"/>
    <Slot Name="Dots" Color="0.3127,0.3290,100"/>
   </Wheel>
  </Wheels>
  <PhysicalDescriptions>
   <Emitters><Emitter Name="White" Color="0.3127,0.3290,100"/></Emitters>
  </PhysicalDescriptions>
  <Models><Model Name="Body" Length="0.4" Width="0.3" Height="0.5" File="body"/></Models>
  <Geometries>
   <Geometry Name="Base" Model="Body">
    <Axis Name="Yoke"><Beam Name="Head"/></Axis>
   </Geometry>
  </Geometries>
  <DMXModes>
   <DMXMode Name="Standard" Geometry="Base">
    <DMXChannels>
     <DMXChannel DMXBreak="1" Offset="1" Geometry="Head">
      <LogicalChannel Attribute="Dimmer">
       <ChannelFunction Name="Dimmer" Attribute="Dimmer" Default="0/1"/>
      </LogicalChannel>
     </DMXChannel>
     <DMXChannel DMXBreak="1" Offset="2,3" Geometry="Yoke">
      <LogicalChannel Attribute="Pan">
       <ChannelFunction Name="Pan" Attribute="Pan" Default="32768/2"/>
      </LogicalChannel>
     </DMXChannel>
     <DMXChannel DMXBreak="1" Offset="4" Geometry="Head">
      <LogicalChannel Attribute="Gobo1">
       <ChannelFunction Name="Gobo Wheel" Attribute="Gobo1" Default="0/1">
        <ChannelSet Name="Open" DMXFrom="0/1"/>
        <ChannelSet Name="Dots" DMXFrom="32/1"/>
        <ChannelSet Name="Breakup" DMXFrom="64/1"/>
       </ChannelFunction>
      </LogicalChannel>
     </DMXChannel>
    </DMXChannels>
   </DMXMode>
  </DMXModes>
  <Revisions><Revision Text="first" Date="2026-01-01T00:00:00"/></Revisions>
 </FixtureType>
</GDTF>
)XML";
}

// Built with our own writer, which is the thing under test — but then checked
// against the system unzip, so a reader that agrees with its own writer
// cannot hide a malformed archive.
std::vector<uint8_t> richPackage() {
    ZipWriter z;
    z.addText("description.xml", richProfileXml());
    // Files Lumen does not model at all. Their survival is the point.
    z.addText("models/gltf/body.glb", "pretend this is a 3D model");
    z.addText("wheels/gobo1.png", "pretend this is a gobo image");
    return z.bytes();
}

} // namespace

// ---------------------------------------------------------------------------
static void testZipWriterRoundTrips() {
    ZipWriter z;
    std::string repetitive;
    for (int i = 0; i < 2000; ++i) repetitive += "deflate me ";
    z.addText("description.xml", "<GDTF/>");
    z.addText("big.txt", repetitive);
    z.addText("empty.txt", "");
    z.addText("nested/dir/file.txt", "nested");

    // Incompressible input: deflate would grow it, so it must be stored.
    std::vector<uint8_t> noise(4096);
    uint32_t x = 12345;
    for (auto& b : noise) {
        x = x * 1103515245u + 12345u;
        b = static_cast<uint8_t>(x >> 16);
    }
    z.add("noise.bin", noise);

    CHECK_EQ(z.entryCount(), size_t(5));
    CHECK(z.has("big.txt"));
    CHECK(!z.has("absent"));

    ZipReader r;
    std::string err;
    CHECK(r.openBytes(z.bytes(), &err));
    CHECK_EQ(r.entries().size(), size_t(5));

    const auto d = r.readText("description.xml", &err);
    CHECK(d.has_value());
    CHECK(*d == "<GDTF/>");
    const auto big = r.readText("big.txt", &err);
    CHECK(big.has_value());
    CHECK(*big == repetitive);
    const auto e0 = r.readText("empty.txt", &err);
    CHECK(e0.has_value());
    CHECK(e0->empty());
    const auto n = r.read("noise.bin", &err);
    CHECK(n.has_value());
    CHECK(*n == noise);
    CHECK(r.readText("nested/dir/file.txt", &err).value_or("") == "nested");

    // Compression was actually applied where it helps and skipped where it
    // does not: storing everything would work but bloat a show file.
    for (const ZipEntry& e : r.entries()) {
        if (e.name == "big.txt") {
            CHECK_EQ(int(e.method), 8);
            CHECK(e.compressedSize < e.uncompressedSize / 4);
        }
        if (e.name == "noise.bin") CHECK_EQ(int(e.method), 0);
    }

    // Adding the same name twice replaces rather than duplicating.
    z.addText("description.xml", "<GDTF DataVersion=\"1.2\"/>");
    CHECK_EQ(z.entryCount(), size_t(5));
    ZipReader r2;
    CHECK(r2.openBytes(z.bytes(), &err));
    CHECK(r2.readText("description.xml", &err).value_or("").find("1.2") !=
          std::string::npos);

    // The same input gives the same bytes, so an export is reproducible.
    CHECK(z.bytes() == z.bytes());
}

static void testSystemUnzipAcceptsIt() {
    if (!haveUnzip()) {
        std::printf("      (skipped: system unzip unavailable)\n");
        return;
    }
    const std::string path = tmpDir() + "/written.zip";
    fs::remove(path);

    ZipWriter z;
    z.addText("description.xml", richProfileXml());
    z.addText("models/gltf/body.glb", "model bytes");
    std::string err;
    CHECK(z.writeFile(path, &err));
    CHECK(err.empty());
    CHECK(fs::exists(path));
    CHECK(!fs::exists(path + ".tmp"));   // temp file cleaned up

    // A reader that agrees with its own writer proves nothing; the system
    // tool is the independent check.
    CHECK_EQ(std::system(("unzip -t -qq " + path + " > /dev/null 2>&1").c_str()), 0);

    const std::string out = tmpDir() + "/extracted";
    fs::remove_all(out);
    CHECK_EQ(std::system(("unzip -qq " + path + " -d " + out + " > /dev/null 2>&1").c_str()),
             0);
    CHECK(fs::exists(out + "/description.xml"));
    CHECK(fs::exists(out + "/models/gltf/body.glb"));
    std::ifstream in(out + "/description.xml");
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    CHECK(text == richProfileXml());
}

static void testGdtfExportRoundTrips() {
    // A fixture authored in Lumen leaves as a valid .gdtf, which is what makes
    // it usable in MA3, ChamSys or a plot.
    for (const FixtureDef& d : {starterMovingSpot(), starterPixelBar()}) {
        const GdtfWriteReport w = writeGdtfXml(d);
        CHECK(w.ok);
        CHECK(!w.xml.empty());
        // The writer says what it cannot express rather than implying the
        // package is complete.
        CHECK(!w.dropped.empty());

        GdtfReport back = parseGdtfXml(w.xml);
        CHECK(back.ok);
        CHECK(back.error.empty());
        CHECK(back.unmapped.empty());   // every attribute wrote a name we read
        CHECK_EQ(back.info.modes.size(), d.modes.size());
        CHECK(back.info.name == d.name);
        CHECK(back.info.manufacturer == d.manufacturer);

        for (size_t m = 0; m < d.modes.size(); ++m) {
            const CompileReport before = compile(d, m);
            const CompileReport after = compile(back.info.def, m);
            CHECK(before.ok);
            CHECK(after.ok);
            CHECK_EQ(after.type.footprint(), before.type.footprint());
            CHECK_EQ(int(after.type.cellCount), int(before.type.cellCount));
            CHECK_EQ(after.type.channels.size(), before.type.channels.size());
            // Range labels survive, which is what the preset generator reads.
            CHECK_EQ(after.type.sets.size(), before.type.sets.size());
            // And the channels land on the same addresses.
            for (const ChannelDef& c : before.type.channels) {
                const ChannelDef* mine = after.type.channel(c.attr, c.cell);
                CHECK(mine != nullptr);
                if (mine) {
                    CHECK_EQ(mine->coarse, c.coarse);
                    CHECK_EQ(mine->fine, c.fine);
                }
            }
        }
    }

    // A pixel bar comes back normalised, so exporting and re-importing does
    // not turn one declaration into twelve for good.
    GdtfReport bar = parseGdtfXml(writeGdtfXml(starterPixelBar()).xml);
    CHECK(bar.ok);
    CHECK_EQ(bar.info.def.modes[0].channels.size(), size_t(4));

    // A definition with no modes cannot be a package, and says so.
    FixtureDef empty;
    empty.name = "Nothing";
    CHECK(!writeGdtfPackage(empty).ok);
    CHECK(!writeGdtfPackage(empty).error.empty());

    // The package is a real ZIP with a description.xml in it.
    const GdtfPackage pkg = writeGdtfPackage(starterMovingSpot());
    CHECK(pkg.ok);
    ZipReader zr;
    std::string err;
    CHECK(zr.openBytes(pkg.bytes, &err));
    CHECK(zr.has("description.xml"));
}

static void testAttributeNamesRoundTripThroughGdtf() {
    // Export writes the import mapping backwards. If the canonical name is
    // wrong, a round trip silently loses the attribute.
    const AttributeMap map;
    for (Attr a : {Attr::Dimmer, Attr::Pan, Attr::Tilt, Attr::Red, Attr::Green,
                   Attr::Blue, Attr::White, Attr::Amber, Attr::Zoom, Attr::Shutter,
                   Attr::Gobo1}) {
        const std::string name = gdtfNameFor(a);
        CHECK(!name.empty());
        const auto back = map.mapWithIndex(name);
        CHECK(back.has_value());
        if (back) CHECK(*back == a);
    }

    // Several GDTF names mean the same thing; the canonical one is the first
    // the table registers, not whichever sorts last.
    CHECK(gdtfNameFor(Attr::Red) == "ColorAdd_R");

    // Removing the canonical spelling promotes a surviving alias rather than
    // leaving the attribute unwritable.
    AttributeMap edited;
    CHECK(edited.nameFor(Attr::Red).value_or("") == "ColorAdd_R");
    edited.remove("ColorAdd_R");
    const auto promoted = edited.nameFor(Attr::Red);
    CHECK(promoted.has_value());
    CHECK(promoted.value_or("") != "ColorAdd_R");
    CHECK(edited.map(*promoted).value_or(Attr::Dimmer) == Attr::Red);
}

static void testImportedPackageComesBackUnchanged() {
    // The promise this file exists for: GDTF is the fixture file format, and
    // an imported package is kept, not reconstructed.
    const std::vector<uint8_t> original = richPackage();

    UidGen uids;
    FixtureLibrary lib(uids);
    const auto imp = lib.importPackage(original, "Lumen@Rich_Spot.gdtf");
    CHECK(imp.ok);
    CHECK(imp.error.empty());
    CHECK_EQ(imp.modes, 1);

    const LibraryEntry* e = lib.find(imp.uid);
    CHECK(e != nullptr);
    CHECK(e->name == "Rich Spot");
    CHECK(e->sourceName == "Lumen@Rich_Spot.gdtf");
    CHECK(e->hasOriginal());
    CHECK(!e->edited);
    CHECK(!e->exportIsLossy());
    CHECK(e->gdtf == original);

    // Export: byte for byte, with nothing dropped.
    const LibraryExport x = lib.exportEntry(imp.uid);
    CHECK(x.ok);
    CHECK(x.verbatim);
    CHECK(x.bytes == original);
    CHECK(x.dropped.empty());
    CHECK(x.suggestedName == "Lumen@Rich_Spot.gdtf");

    // Which means the files Lumen does not model survive — the whole reason
    // not to regenerate.
    ZipReader zr;
    std::string err;
    CHECK(zr.openBytes(x.bytes, &err));
    CHECK(zr.has("models/gltf/body.glb"));
    CHECK(zr.has("wheels/gobo1.png"));
    const auto desc = zr.readText("description.xml", &err);
    CHECK(desc.has_value());
    CHECK(desc->find("<Wheels>") != std::string::npos);
    CHECK(desc->find("PhysicalDescriptions") != std::string::npos);

    // The model still parsed, so the fixture is editable and patchable.
    CHECK_EQ(e->def.modes.size(), size_t(1));
    CHECK(e->def.findGeometry("Yoke") != nullptr);
    CHECK(e->def.findGeometry("Yoke")->kind == GeometryKind::Axis);
    const CompileReport c = compile(e->def, 0);
    CHECK(c.ok);
    CHECK_EQ(c.type.footprint(), 4);
    CHECK_EQ(c.type.setsFor(Attr::Gobo1).size(), size_t(3));
}

static void testEditingMakesTheExportLossyAndSaysSo() {
    UidGen uids;
    FixtureLibrary lib(uids);
    const std::vector<uint8_t> original = richPackage();
    const auto imp = lib.importPackage(original, "rich.gdtf");
    CHECK(imp.ok);

    LibraryEntry* e = lib.find(imp.uid);
    CHECK(e != nullptr);
    FixtureEditor ed(e->def);
    CHECK(ed.addChannel(0, Attr::Zoom, "Head", -1));
    CHECK(lib.markEdited(imp.uid));
    CHECK(e->edited);
    CHECK(e->exportIsLossy());

    const LibraryExport x = lib.exportEntry(imp.uid);
    CHECK(x.ok);
    CHECK(!x.verbatim);
    CHECK(x.bytes != original);
    // It names the cost rather than quietly shipping a thinner profile, and
    // the first line is the one that matters.
    CHECK(!x.dropped.empty());
    CHECK(!x.dropped.empty());
    CHECK(x.dropped[0].find("regenerated") != std::string::npos);
    CHECK(!x.dropped.empty());
    CHECK(x.dropped[0].find("rich.gdtf") != std::string::npos);
    bool mentionsModels = false, mentionsWheels = false;
    for (const auto& d : x.dropped) {
        if (d.find("3D models") != std::string::npos) mentionsModels = true;
        if (d.find("wheels") != std::string::npos) mentionsWheels = true;
    }
    CHECK(mentionsModels);
    CHECK(mentionsWheels);

    // The regenerated package is still valid and carries the edit.
    ZipReader zr;
    std::string err;
    CHECK(zr.openBytes(x.bytes, &err));
    CHECK(zr.has("description.xml"));
    CHECK(!zr.has("models/gltf/body.glb"));   // honestly gone, as reported
    GdtfReport back = parseGdtfXml(*zr.readText("description.xml", &err));
    CHECK(back.ok);
    CHECK_EQ(compile(back.info.def, 0).type.footprint(), 5);

    // A fixture authored in Lumen has no original, so it is never "lossy" —
    // there was never anything else to lose.
    LibraryEntry& authored = lib.addAuthored(starterMovingSpot());
    CHECK(!authored.hasOriginal());
    CHECK(!authored.exportIsLossy());
    const LibraryExport ax = lib.exportEntry(authored.uid);
    CHECK(ax.ok);
    CHECK(!ax.verbatim);
    CHECK(ax.suggestedName == "Lumen@Moving Spot.gdtf");
    for (const auto& d : ax.dropped)
        CHECK(d.find("regenerated") == std::string::npos);
}

static void testPublishFeedsThePatch() {
    UidGen uids;
    FixtureLibrary lib(uids);
    LibraryEntry& e = lib.addAuthored(starterPixelBar());

    Patch patch(uids);
    const auto pub = lib.publish(e.uid, 0, patch);
    CHECK(pub.ok);
    CHECK(pub.typeIndex >= 0);
    CHECK(pub.typeName == "Pixel Bar 12 / Pixel");
    CHECK_EQ(patch.type(pub.typeIndex).footprint(), 37);
    CHECK_EQ(int(patch.type(pub.typeIndex).cellCount), 12);

    // Publishing again after an edit REPLACES the type rather than adding a
    // second one, because the name is the identity.
    FixtureEditor ed(e.def);
    CHECK(ed.addChannel(0, Attr::White, "Pixel", -1));
    lib.markEdited(e.uid);
    const auto pub2 = lib.publish(e.uid, 0, patch);
    CHECK(pub2.ok);
    CHECK_EQ(patch.typeCount(), 1);
    CHECK_EQ(patch.type(pub2.typeIndex).footprint(), 49);

    // Each mode of one fixture is its OWN type with its own identity. Sharing
    // the definition's uid between them collides in the patch index and in
    // the show file, so they must differ.
    FixtureDef twoModes = starterMovingSpot();
    FixtureEditor ed2(twoModes);
    const size_t second = ed2.addMode("Basic", "Base");
    CHECK(ed2.addChannel(second, Attr::Dimmer, "Head", 0));
    LibraryEntry& multi = lib.addAuthored(twoModes);
    const auto a = lib.publish(multi.uid, 0, patch);
    const auto b = lib.publish(multi.uid, 1, patch);
    CHECK(a.ok);
    CHECK(b.ok);
    CHECK(a.typeName != b.typeName);
    CHECK(patch.type(a.typeIndex).uid != patch.type(b.typeIndex).uid);
    CHECK(patch.type(a.typeIndex).uid.valid());
    CHECK(patch.type(b.typeIndex).uid.valid());

    // A mode index that does not exist, and a fixture that is not there.
    CHECK(!lib.publish(e.uid, 99, patch).ok);
    CHECK(!lib.publish(Uid{999999}, 0, patch).ok);
    CHECK(!lib.exportEntry(Uid{999999}).ok);

    // A definition that does not compile is refused with the reason, not
    // published as something broken.
    FixtureDef clash;
    clash.name = "Clash";
    FixtureEditor ce(clash);
    const size_t cm = ce.addMode("Default");
    ce.addChannel(cm, Attr::Dimmer, {}, 0);
    ce.addChannel(cm, Attr::Shutter, {}, 0);
    LibraryEntry& bad = lib.addAuthored(clash);
    const auto failed = lib.publish(bad.uid, 0, patch);
    CHECK(!failed.ok);
    CHECK(failed.error.find("claimed by both") != std::string::npos);
}

static void testLibraryStoreBasics() {
    UidGen uids;
    FixtureLibrary lib(uids);
    CHECK_EQ(lib.size(), size_t(0));

    LibraryEntry& a = lib.addAuthored(starterMovingSpot());
    LibraryEntry& b = lib.addAuthored(starterPixelBar());
    CHECK_EQ(lib.size(), size_t(2));
    CHECK_EQ(a.id, 1);
    CHECK_EQ(b.id, 2);
    CHECK(a.uid != b.uid);
    CHECK(lib.byName("Moving Spot") == &a);
    CHECK(lib.byId(2) == &b);
    CHECK(lib.find(a.uid) == &a);
    CHECK(lib.byName("nope") == nullptr);
    // The definition's uid is kept in step with the entry's, so a reference
    // stored against either finds the same fixture.
    CHECK(a.def.uid == a.uid);

    // Capture the UID before removing. `a` is a reference INTO the deque,
    // and erasing from the middle of a deque invalidates every reference to
    // it — the stores use deque so references survive GROWTH, which is not
    // the same promise. Reading `a.uid` after the erase is a use-after-free
    // that happens to return the right number most of the time.
    const Uid removed = a.uid;
    CHECK(lib.remove(removed));
    CHECK_EQ(lib.size(), size_t(1));
    CHECK(!lib.remove(removed));
    CHECK(!lib.markEdited(removed));

    // An empty or malformed package is refused with a reason.
    CHECK(!lib.importPackage({}, "x.gdtf").ok);
    std::vector<uint8_t> junk{'n', 'o', 'p', 'e'};
    const auto bad = lib.importPackage(junk, "x.gdtf");
    CHECK(!bad.ok);
    CHECK(bad.error.find("not a gdtf") != std::string::npos);

    // A ZIP with no description.xml.
    ZipWriter z;
    z.addText("readme.txt", "hello");
    const auto noDesc = lib.importPackage(z.bytes(), "x.gdtf");
    CHECK(!noDesc.ok);
    CHECK(noDesc.error.find("description.xml") != std::string::npos);

    CHECK(!lib.importFile(tmpDir() + "/not-here.gdtf").ok);
}

static void testLibrarySurvivesTheShowFile() {
    const std::string pkgPath = tmpDir() + "/rich.gdtf";
    {
        std::ofstream o(pkgPath, std::ios::binary);
        const auto bytes = richPackage();
        o.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }
    const std::vector<uint8_t> original = slurp(pkgPath);
    CHECK(!original.empty());

    const std::string showPath = tmpDir() + "/library.lumen";
    fs::remove(showPath);

    Engine a;
    Console con(a);
    Result imp = con.execute("ImportGdtf \"" + pkgPath + "\"");
    CHECK(imp.parsed);
    CHECK(imp.ok);
    CHECK_EQ(a.library.size(), size_t(1));
    CHECK_EQ(a.patch.typeCount(), 1);
    CHECK(imp.message.find("package kept") != std::string::npos);

    // An authored fixture too, so both kinds persist.
    a.library.addAuthored(starterPixelBar());
    // And an edited one, so the flag persists rather than resetting.
    LibraryEntry* rich = a.library.byName("Rich Spot");
    CHECK(rich != nullptr);
    rich->tags.push_back("borrowed");
    rich->def.revisions.push_back({"edited in Lumen", "2026-10-21"});

    SaveReport sr = ShowFile::save(a, showPath, "Library Show");
    CHECK(sr.ok);
    CHECK(sr.error.empty());
    CHECK_EQ(sr.library, 2);

    Engine b;
    LoadReport lr = ShowFile::load(b, showPath);
    CHECK(lr.ok);
    CHECK(lr.error.empty());
    CHECK_EQ(lr.library, 2);
    CHECK(lr.warnings.empty());
    CHECK_EQ(b.library.size(), size_t(2));

    const LibraryEntry* loaded = b.library.byName("Rich Spot");
    CHECK(loaded != nullptr);
    CHECK(loaded->sourceName == "rich.gdtf");
    CHECK_EQ(loaded->tags.size(), size_t(1));
    if (!loaded->tags.empty()) CHECK(loaded->tags[0] == "borrowed");
    CHECK_EQ(loaded->def.revisions.size(), size_t(1));
    if (!loaded->def.revisions.empty())
        CHECK(!loaded->def.revisions.empty());
        CHECK(loaded->def.revisions[0].date == "2026-10-21");

    // The package survived the show file BYTE FOR BYTE. Anything less and a
    // show that travels loses the designer's profiles.
    CHECK(loaded->hasOriginal());
    CHECK(loaded->gdtf == original);
    const LibraryExport x = b.library.exportEntry(loaded->uid);
    CHECK(x.ok);
    CHECK(x.verbatim);
    CHECK(x.bytes == original);

    // The parsed model survived too, tree and all.
    CHECK(loaded->def.manufacturer == "Lumen Test");
    CHECK(loaded->def.typeId == "1234");
    CHECK(loaded->def.findGeometry("Head") != nullptr);
    CHECK(loaded->def.findGeometry("Head")->kind == GeometryKind::Beam);
    CHECK(loaded->def.findGeometry("Yoke")->children.size() == size_t(1));
    const CompileReport c = compile(loaded->def, 0);
    CHECK(c.ok);
    CHECK_EQ(c.type.footprint(), 4);
    const auto gobo = c.type.setsFor(Attr::Gobo1);
    CHECK_EQ(gobo.size(), size_t(3));
    if (!gobo.empty()) CHECK(gobo[0].anchor);   // "Open" is still an anchor

    // The multi-instance one kept its cells, which means its references and
    // their offsets round-tripped.
    const LibraryEntry* bar = b.library.byName("Pixel Bar 12");
    CHECK(bar != nullptr);
    CHECK(!bar->hasOriginal());
    CHECK_EQ(bar->def.modes[0].channels.size(), size_t(4));
    const CompileReport bc = compile(bar->def, 0);
    CHECK(bc.ok);
    CHECK_EQ(bc.type.footprint(), 37);
    CHECK_EQ(int(bc.type.cellCount), 12);
}

static void testLibraryThroughTheConsole() {
    const std::string pkgPath = tmpDir() + "/console.gdtf";
    {
        std::ofstream o(pkgPath, std::ios::binary);
        const auto bytes = richPackage();
        o.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }
    const std::vector<uint8_t> original = slurp(pkgPath);

    Engine eng;
    Console con(eng);

    CHECK(con.execute("ImportGdtf \"" + pkgPath + "\"").ok);
    Result ls = con.execute("List library");
    CHECK(ls.ok);
    CHECK_EQ(ls.rows.size(), size_t(1));
    CHECK(!ls.rows.empty());
    CHECK(ls.rows[0].find("Rich Spot") != std::string::npos);
    CHECK(!ls.rows.empty());
    CHECK(ls.rows[0].find("console.gdtf") != std::string::npos);

    // Patch it and run it, which is the only thing importing is for.
    CHECK(con.execute("Fixture 1 Thru 3 Patch \"Rich Spot / Standard\"").ok);
    CHECK_EQ(eng.patch.size(), size_t(3));
    CHECK(con.execute("Fixture 1 At 100").ok);
    eng.tick();
    CHECK(eng.merged().get(makeKey(1, 0, Attr::Dimmer)).has_value());

    // Export it back out: unchanged, byte for byte.
    const std::string out = tmpDir() + "/exported.gdtf";
    fs::remove(out);
    Result ex = con.execute("ExportGdtf \"Rich Spot\" \"" + out + "\"");
    CHECK(ex.parsed);
    CHECK(ex.ok);
    CHECK(ex.message.find("unchanged") != std::string::npos);
    CHECK(fs::exists(out));
    CHECK(slurp(out) == original);
    CHECK(ex.rows.empty());   // nothing was dropped, so nothing is reported

    // Edit it, then export again: regenerated, and the reply says what went.
    LibraryEntry* e = eng.library.byName("Rich Spot");
    CHECK(e != nullptr);
    FixtureEditor ed(e->def);
    CHECK(ed.addChannel(0, Attr::Zoom, "Head", -1));
    CHECK(eng.library.markEdited(e->uid));
    Result ex2 = con.execute("ExportGdtf \"Rich Spot\" \"" + out + "\"");
    CHECK(ex2.ok);
    CHECK(ex2.message.find("regenerated") != std::string::npos);
    CHECK(!ex2.rows.empty());
    CHECK(slurp(out) != original);

    // Republishing picks up the edit.
    Result pub = con.execute("PublishFixture \"Rich Spot\" \"Standard\"");
    CHECK(pub.ok);
    CHECK(pub.message.find("5ch") != std::string::npos);

    // Unknown fixture, unknown mode, missing arguments.
    CHECK(!con.execute("ExportGdtf \"Ghost\" \"" + out + "\"").ok);
    CHECK(!con.execute("PublishFixture \"Ghost\"").ok);
    CHECK(!con.execute("PublishFixture \"Rich Spot\" \"Ghost\"").ok);
    CHECK(!con.execute("ImportGdtf \"" + tmpDir() + "/absent.gdtf\"").ok);
}

int main() {
    std::printf("lumen library\n");
    testZipWriterRoundTrips();
    testSystemUnzipAcceptsIt();
    testGdtfExportRoundTrips();
    testAttributeNamesRoundTripThroughGdtf();
    testImportedPackageComesBackUnchanged();
    testEditingMakesTheExportLossyAndSaysSo();
    testPublishFeedsThePatch();
    testLibraryStoreBasics();
    testLibrarySurvivesTheShowFile();
    testLibraryThroughTheConsole();
    return check::report("library");
}
