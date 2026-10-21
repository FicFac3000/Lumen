// Tests for XML, ZIP and GDTF import — especially the taxonomy mapping.
#include "Check.hpp"

#include "lumen/Command.hpp"
#include "lumen/Engine.hpp"
#include "lumen/Gdtf.hpp"
#include "lumen/Preset.hpp"
#include "lumen/Xml.hpp"
#include "lumen/Zip.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>

using namespace lumen;
namespace fs = std::filesystem;

namespace {

// A GDTF description.xml exercising the shapes that matter: 16-bit channels,
// channel sets with an anchor, a subtractive colour mix, blades, an unmapped
// attribute, and geometry references for cells.
const char* kSpotXml = R"XML(<?xml version="1.0" encoding="UTF-8"?>
<GDTF DataVersion="1.2">
 <FixtureType Name="Test Spot 700" ShortName="TS700" Manufacturer="Lumen Test"
              FixtureTypeID="11111111-2222-3333-4444-555555555555"
              Description="A synthetic profile">
  <AttributeDefinitions>
   <Attributes>
    <Attribute Name="Pan" Pretty="P" PhysicalUnit="Angle"/>
    <Attribute Name="Tilt" Pretty="T" PhysicalUnit="Angle"/>
   </Attributes>
  </AttributeDefinitions>
  <Geometries>
   <Base Name="Base">
    <Axis Name="Yoke">
     <Beam Name="Beam"/>
    </Axis>
   </Base>
  </Geometries>
  <DMXModes>
   <DMXMode Name="Standard" Geometry="Base">
    <DMXChannels>
     <DMXChannel DMXBreak="1" Offset="1,2" Geometry="Yoke">
      <LogicalChannel Attribute="Pan">
       <ChannelFunction Name="Pan" Attribute="Pan" DMXFrom="0/1" Default="32768/2"
                        PhysicalFrom="-270" PhysicalTo="270"/>
      </LogicalChannel>
     </DMXChannel>
     <DMXChannel DMXBreak="1" Offset="3,4" Geometry="Yoke">
      <LogicalChannel Attribute="Tilt">
       <ChannelFunction Name="Tilt" Attribute="Tilt" DMXFrom="0/1" Default="32768/2"
                        PhysicalFrom="-135" PhysicalTo="135"/>
      </LogicalChannel>
     </DMXChannel>
     <DMXChannel DMXBreak="1" Offset="5" Geometry="Beam">
      <LogicalChannel Attribute="Dimmer">
       <ChannelFunction Name="Dimmer" Attribute="Dimmer" DMXFrom="0/1" Default="0/1"
                        PhysicalFrom="0" PhysicalTo="1"/>
      </LogicalChannel>
     </DMXChannel>
     <DMXChannel DMXBreak="1" Offset="6" Geometry="Beam">
      <LogicalChannel Attribute="Shutter1">
       <ChannelFunction Name="Shutter" Attribute="Shutter1" DMXFrom="0/1">
        <ChannelSet Name="Closed" DMXFrom="0/1" PhysicalFrom="0" PhysicalTo="0"/>
        <ChannelSet Name="Open" DMXFrom="32/1" PhysicalFrom="1" PhysicalTo="1"/>
        <ChannelSet Name="Strobe" DMXFrom="64/1" PhysicalFrom="1" PhysicalTo="25"/>
       </ChannelFunction>
      </LogicalChannel>
     </DMXChannel>
     <DMXChannel DMXBreak="1" Offset="7" Geometry="Beam">
      <LogicalChannel Attribute="ColorSub_C">
       <ChannelFunction Name="Cyan" Attribute="ColorSub_C" DMXFrom="0/1"/>
      </LogicalChannel>
     </DMXChannel>
     <DMXChannel DMXBreak="1" Offset="8" Geometry="Beam">
      <LogicalChannel Attribute="ColorSub_M">
       <ChannelFunction Name="Magenta" Attribute="ColorSub_M" DMXFrom="0/1"/>
      </LogicalChannel>
     </DMXChannel>
     <DMXChannel DMXBreak="1" Offset="9" Geometry="Beam">
      <LogicalChannel Attribute="Prism1PosRotate">
       <ChannelFunction Name="Prism Rotate" Attribute="Prism1PosRotate" DMXFrom="0/1">
        <ChannelSet Name="CCW fast" DMXFrom="0/1" PhysicalFrom="-30" PhysicalTo="-5"/>
        <ChannelSet Name="CCW slow" DMXFrom="16/1" PhysicalFrom="-5" PhysicalTo="-0.2"/>
        <ChannelSet Name="Stop" DMXFrom="121/1" PhysicalFrom="0" PhysicalTo="0"/>
        <ChannelSet Name="CW slow" DMXFrom="135/1" PhysicalFrom="0.2" PhysicalTo="5"/>
        <ChannelSet Name="CW fast" DMXFrom="240/1" PhysicalFrom="5" PhysicalTo="30"/>
       </ChannelFunction>
      </LogicalChannel>
     </DMXChannel>
     <DMXChannel DMXBreak="1" Offset="10" Geometry="Beam">
      <LogicalChannel Attribute="Blade1A">
       <ChannelFunction Name="Blade 1A" Attribute="Blade1A" DMXFrom="0/1"/>
      </LogicalChannel>
     </DMXChannel>
     <DMXChannel DMXBreak="1" Offset="11" Geometry="Beam">
      <LogicalChannel Attribute="Gobo3">
       <ChannelFunction Name="Gobo 3" Attribute="Gobo3" DMXFrom="0/1"/>
      </LogicalChannel>
     </DMXChannel>
     <DMXChannel DMXBreak="1" Offset="12" Geometry="Beam">
      <LogicalChannel Attribute="SparkleRate">
       <ChannelFunction Name="Sparkle" Attribute="SparkleRate" DMXFrom="0/1"/>
      </LogicalChannel>
     </DMXChannel>
    </DMXChannels>
   </DMXMode>
   <DMXMode Name="Basic" Geometry="Base">
    <DMXChannels>
     <DMXChannel DMXBreak="1" Offset="1" Geometry="Beam">
      <LogicalChannel Attribute="Dimmer">
       <ChannelFunction Name="Dimmer" Attribute="Dimmer" DMXFrom="0/1"/>
      </LogicalChannel>
     </DMXChannel>
    </DMXChannels>
   </DMXMode>
  </DMXModes>
 </FixtureType>
</GDTF>
)XML";

// A multi-instance profile: a master dimmer plus four cells via geometry
// references, which is the §1.3 shape.
const char* kBarXml = R"XML(<?xml version="1.0"?>
<GDTF DataVersion="1.2">
 <FixtureType Name="Test Bar 4" ShortName="TB4" Manufacturer="Lumen Test">
  <Geometries>
   <Base Name="Base">
    <GeometryReference Name="Cell1" Geometry="Pixel"/>
    <GeometryReference Name="Cell2" Geometry="Pixel"/>
    <GeometryReference Name="Cell3" Geometry="Pixel"/>
    <GeometryReference Name="Cell4" Geometry="Pixel"/>
   </Base>
  </Geometries>
  <DMXModes>
   <DMXMode Name="Pixel" Geometry="Base">
    <DMXChannels>
     <DMXChannel Offset="1" Geometry="Base">
      <LogicalChannel Attribute="Dimmer">
       <ChannelFunction Name="Master" Attribute="Dimmer" DMXFrom="0/1"/>
      </LogicalChannel>
     </DMXChannel>
     <DMXChannel Offset="2" Geometry="Cell1">
      <LogicalChannel Attribute="ColorAdd_R">
       <ChannelFunction Name="R" Attribute="ColorAdd_R" DMXFrom="0/1"/>
      </LogicalChannel>
     </DMXChannel>
     <DMXChannel Offset="3" Geometry="Cell1">
      <LogicalChannel Attribute="ColorAdd_G">
       <ChannelFunction Name="G" Attribute="ColorAdd_G" DMXFrom="0/1"/>
      </LogicalChannel>
     </DMXChannel>
     <DMXChannel Offset="4" Geometry="Cell1">
      <LogicalChannel Attribute="ColorAdd_B">
       <ChannelFunction Name="B" Attribute="ColorAdd_B" DMXFrom="0/1"/>
      </LogicalChannel>
     </DMXChannel>
     <DMXChannel Offset="5" Geometry="Cell2">
      <LogicalChannel Attribute="ColorAdd_R">
       <ChannelFunction Name="R" Attribute="ColorAdd_R" DMXFrom="0/1"/>
      </LogicalChannel>
     </DMXChannel>
     <DMXChannel Offset="6" Geometry="Cell2">
      <LogicalChannel Attribute="ColorAdd_G">
       <ChannelFunction Name="G" Attribute="ColorAdd_G" DMXFrom="0/1"/>
      </LogicalChannel>
     </DMXChannel>
     <DMXChannel Offset="7" Geometry="Cell2">
      <LogicalChannel Attribute="ColorAdd_B">
       <ChannelFunction Name="B" Attribute="ColorAdd_B" DMXFrom="0/1"/>
      </LogicalChannel>
     </DMXChannel>
     <DMXChannel Offset="8" Geometry="Cell3">
      <LogicalChannel Attribute="ColorAdd_R">
       <ChannelFunction Name="R" Attribute="ColorAdd_R" DMXFrom="0/1"/>
      </LogicalChannel>
     </DMXChannel>
     <DMXChannel Offset="9" Geometry="Cell4">
      <LogicalChannel Attribute="ColorAdd_R">
       <ChannelFunction Name="R" Attribute="ColorAdd_R" DMXFrom="0/1"/>
      </LogicalChannel>
     </DMXChannel>
    </DMXChannels>
   </DMXMode>
  </DMXModes>
 </FixtureType>
</GDTF>
)XML";

std::string tmpDir() {
    const auto d = fs::temp_directory_path() / "lumen_gdtf_tests";
    fs::create_directories(d);
    return d.string();
}

// Builds a real .gdtf (a ZIP) using the system zip tool, so the ZIP reader is
// tested against a file it did not write.
bool makeGdtfPackage(const std::string& outPath, const std::string& xml) {
    const std::string work = tmpDir() + "/pkg";
    fs::remove_all(work);
    fs::create_directories(work);
    {
        std::ofstream o(work + "/description.xml");
        o << xml;
    }
    fs::remove(outPath);
    const std::string cmd = "cd " + work + " && zip -q -X " + outPath +
                            " description.xml > /dev/null 2>&1";
    return std::system(cmd.c_str()) == 0 && fs::exists(outPath);
}

} // namespace

static void testXmlParsing() {
    XmlDoc d = parseXml("<a x=\"1\" y=\"two\"><b/><b z=\"3\">text</b></a>");
    CHECK(d.ok);
    CHECK(d.root.name == "a");
    CHECK_EQ(d.root.attrInt("x"), 1);
    CHECK(d.root.attrOr("y") == "two");
    CHECK_EQ(d.root.kids("b").size(), size_t(2));
    CHECK(d.root.kids("b")[1]->text == "text");
    CHECK_EQ(d.root.kids("b")[1]->attrInt("z"), 3);

    // Missing is distinguishable from present-but-empty, which matters because
    // an empty GDTF ChannelSet name is meaningful.
    CHECK(!d.root.hasAttr("nope"));
    CHECK(!d.root.attr("nope").has_value());
    XmlDoc e = parseXml("<a n=\"\"/>");
    CHECK(e.root.hasAttr("n"));
    CHECK(e.root.attr("n").value().empty());

    // Deep nesting must survive: the builder tracks indices, not pointers,
    // because appending to a children vector can reallocate it.
    std::string deep = "<r>";
    for (int i = 0; i < 200; ++i) deep += "<n i=\"" + std::to_string(i) + "\">";
    for (int i = 0; i < 200; ++i) deep += "</n>";
    deep += "</r>";
    XmlDoc dd = parseXml(deep);
    CHECK(dd.ok);
    const XmlNode* cur = &dd.root;
    int depth = 0;
    while (const XmlNode* next = cur->kid("n")) {
        CHECK_EQ(next->attrInt("i"), depth);
        cur = next;
        ++depth;
    }
    CHECK_EQ(depth, 200);

    // Many siblings, the case that actually triggers reallocation.
    std::string wide = "<r>";
    for (int i = 0; i < 500; ++i) wide += "<s i=\"" + std::to_string(i) + "\"/>";
    wide += "</r>";
    XmlDoc w = parseXml(wide);
    CHECK(w.ok);
    CHECK_EQ(w.root.kids("s").size(), size_t(500));
    CHECK_EQ(w.root.kids("s")[499]->attrInt("i"), 499);

    CHECK(!parseXml("<unclosed>").ok);
    CHECK(!parseXml("not xml at all <<<").ok);
}

static void testGdtfDmxValues() {
    // "value/byteCount" — the GDTF encoding.
    CHECK_NEAR(parseGdtfDmx("0/1"), 0.0);
    CHECK_NEAR(parseGdtfDmx("255/1"), 1.0);
    CHECK_NEAR(parseGdtfDmx("128/1"), 128.0 / 255.0);
    CHECK_NEAR(parseGdtfDmx("65535/2"), 1.0);
    CHECK_NEAR(parseGdtfDmx("32768/2"), 32768.0 / 65535.0);
    CHECK_NEAR(parseGdtfDmx("0/2"), 0.0);
    // A bare value is treated as one byte.
    CHECK_NEAR(parseGdtfDmx("255"), 1.0);
    // Nonsense degrades to zero rather than throwing.
    CHECK_NEAR(parseGdtfDmx(""), 0.0);
    CHECK_NEAR(parseGdtfDmx("abc"), 0.0);
}

static void testAttributeMapping() {
    AttributeMap m;
    CHECK(m.size() > 50);

    // §6.3: GDTF's vocabulary maps onto Lumen's own, which is what everything
    // downstream reads.
    CHECK(m.map("Pan").value() == Attr::Pan);
    CHECK(m.map("ColorAdd_R").value() == Attr::Red);
    CHECK(m.map("ColorSub_C").value() == Attr::CyanSub);
    CHECK(m.map("Shutter1").value() == Attr::Shutter);
    CHECK(m.map("Shutter1Strobe").value() == Attr::StrobeRate);
    CHECK(m.map("Prism1PosRotate").value() == Attr::Prism1Spin);
    CHECK(m.map("Gobo1PosRotate").value() == Attr::Gobo1Spin);
    CHECK(m.map("Blade2B").value() == Attr::Shaper2B);
    CHECK(m.map("XYZ_X").value() == Attr::PosX);

    // Subtractive cyan and additive cyan are DIFFERENT attributes, which the
    // native taxonomy distinguishes and GDTF's naming makes explicit.
    CHECK(m.map("ColorSub_C").value() != m.map("ColorAdd_C").value());

    // An unknown name is reported as unmapped rather than guessed.
    CHECK(!m.map("SparkleRate").has_value());

    // Index fallback: Gobo3 is recognised without its own table entry.
    CHECK(!m.map("Gobo3").has_value());
    CHECK(m.mapWithIndex("Gobo3").value() == Attr::Gobo1);
    CHECK(m.mapWithIndex("Prism2PosRotate").value() == Attr::Prism1Spin);
    CHECK(!m.mapWithIndex("SparkleRate").has_value());

    // The table is editable, so an unusual profile can be handled without
    // changing the shipped mapping.
    AttributeMap custom;
    custom.set("SparkleRate", Attr::StrobeRate);
    CHECK(custom.map("SparkleRate").value() == Attr::StrobeRate);
    custom.remove("Pan");
    CHECK(!custom.map("Pan").has_value());
}

static void testParseSpot() {
    GdtfReport r = parseGdtfXml(kSpotXml);
    CHECK(r.ok);
    CHECK(r.error.empty());
    CHECK(r.info.name == "Test Spot 700");
    CHECK(r.info.manufacturer == "Lumen Test");
    CHECK(r.info.shortName == "TS700");
    CHECK_EQ(r.info.modes.size(), size_t(2));

    const FixtureType& t = r.info.modes[0].type;
    CHECK(r.info.modes[0].name == "Standard");
    CHECK(t.name == "Test Spot 700 / Standard");

    // 16-bit channels keep both offsets, converted from GDTF's 1-based to
    // Lumen's 0-based.
    const ChannelDef* pan = t.channel(Attr::Pan);
    CHECK(pan != nullptr);
    CHECK_EQ(pan->coarse, 0);
    CHECK_EQ(pan->fine, 1);
    const ChannelDef* tilt = t.channel(Attr::Tilt);
    CHECK_EQ(tilt->coarse, 2);
    CHECK_EQ(tilt->fine, 3);

    // 8-bit channels have no fine offset.
    const ChannelDef* dim = t.channel(Attr::Dimmer);
    CHECK_EQ(dim->coarse, 4);
    CHECK_EQ(dim->fine, -1);

    // Defaults come across, converted to 8-bit.
    CHECK(pan->defaultValue > 120 && pan->defaultValue < 136);  // mid-travel

    // The mapped attributes are Lumen's, not GDTF's.
    CHECK(t.hasAttr(Attr::CyanSub));
    CHECK(t.hasAttr(Attr::Magenta));
    CHECK(t.hasAttr(Attr::Shaper1A));
    CHECK(t.hasAttr(Attr::Prism1Spin));
    CHECK(t.hasAttr(Attr::Gobo1));   // from Gobo3, via the index fallback
    CHECK(!t.hasAttr(Attr::Red));    // it is a subtractive fixture

    // §6.3: the unmapped attribute is VISIBLE rather than dropped.
    CHECK_EQ(r.unmapped.size(), size_t(1));
    CHECK(r.unmapped[0].gdtfAttribute == "SparkleRate");
    CHECK(r.unmapped[0].mode == "Standard");
    CHECK_EQ(r.unmapped[0].offset, 12);

    // Footprint reflects what was actually mapped, so the unmapped channel
    // does not silently shift addressing.
    CHECK_EQ(t.footprint(), 11);

    // The second mode is a separate type.
    CHECK(r.info.modes[1].name == "Basic");
    CHECK_EQ(r.info.modes[1].type.footprint(), 1);
    CHECK(r.info.modes[1].type.hasAttr(Attr::Dimmer));
    CHECK(!r.info.modes[1].type.hasAttr(Attr::Pan));
}

static void testChannelSetsBecomeRangeLabels() {
    GdtfReport r = parseGdtfXml(kSpotXml);
    CHECK(r.ok);
    const FixtureType& t = r.info.modes[0].type;

    // §7.2: GDTF ChannelSets ARE Lumen's range labels, so an imported profile
    // has working labels with no extra work.
    const auto shutter = t.setsFor(Attr::Shutter);
    CHECK_EQ(shutter.size(), size_t(3));
    CHECK(shutter[0].name == "Closed");
    CHECK(shutter[1].name == "Open");
    CHECK(shutter[2].name == "Strobe");

    // A set's end is the next set's start minus one, since GDTF gives only
    // the start.
    CHECK_EQ(int(shutter[0].dmxFrom), 0);
    CHECK_EQ(int(shutter[0].dmxTo), 31);
    CHECK_EQ(int(shutter[1].dmxFrom), 32);
    CHECK_EQ(int(shutter[1].dmxTo), 63);
    CHECK_EQ(int(shutter[2].dmxTo), 255);  // the last runs to the top

    // §7.3's anchor detection runs at import, so the auto-stepper works on an
    // imported profile immediately.
    const auto spin = t.setsFor(Attr::Prism1Spin);
    CHECK_EQ(spin.size(), size_t(5));
    CHECK(spin[2].name == "Stop");
    CHECK(spin[2].anchor);
    CHECK(!spin[0].anchor);
    CHECK(!spin[4].anchor);
    CHECK_NEAR(spin[0].physFrom, -30.0);
    CHECK_NEAR(spin[4].physTo, 30.0);

    // "Closed" is also recognised as a semantic zero.
    CHECK(shutter[0].anchor);
    CHECK(!shutter[2].anchor);
}

static void testImportedProfileFeedsTheAutoStepper() {
    // The payoff: an imported profile drives the §7.3 generator with no
    // hand-authoring, because the anchor came across with the channel sets.
    GdtfReport r = parseGdtfXml(kSpotXml);
    CHECK(r.ok);
    const FixtureType& t = r.info.modes[0].type;

    StepRequest req;
    req.attr = Attr::Prism1Spin;
    req.steps = 3;
    const auto steps = planSteps(t, req);

    CHECK_EQ(steps.size(), size_t(7));  // anchor + 3 per side
    int inStop = 0;
    for (const auto& s : steps)
        if (s.setName == "Stop") ++inStop;
    CHECK_EQ(inStop, 1);  // nothing straddles the stop point
    for (const auto& s : steps) {
        if (s.side < 0) CHECK(s.physical < 0.f);
        if (s.side > 0) CHECK(s.physical > 0.f);
    }
}

static void testGeometryReferencesBecomeCells() {
    GdtfReport r = parseGdtfXml(kBarXml);
    CHECK(r.ok);
    CHECK_EQ(r.info.modes.size(), size_t(1));
    const FixtureType& t = r.info.modes[0].type;

    // §1.3: geometry references become cells, so a multi-instance fixture is
    // ONE patch entry with addressable cells.
    CHECK_EQ(t.cellCount, CellId(4));

    // The master dimmer sits on the parent, cell 0.
    CHECK(t.hasAttr(Attr::Dimmer, 0));
    CHECK_EQ(t.channel(Attr::Dimmer, 0)->coarse, 0);

    // Colour lives on the cells, numbered in geometry-reference order.
    CHECK(t.hasAttr(Attr::Red, 1));
    CHECK(t.hasAttr(Attr::Green, 1));
    CHECK(t.hasAttr(Attr::Blue, 1));
    CHECK(t.hasAttr(Attr::Red, 2));
    CHECK(t.hasAttr(Attr::Red, 3));
    CHECK(t.hasAttr(Attr::Red, 4));
    CHECK(!t.hasAttr(Attr::Red, 0));   // not on the parent
    CHECK(!t.hasAttr(Attr::Red, 5));   // and no phantom fifth cell

    CHECK_EQ(t.channel(Attr::Red, 1)->coarse, 1);
    CHECK_EQ(t.channel(Attr::Red, 2)->coarse, 4);
    CHECK_EQ(t.channel(Attr::Green, 2)->coarse, 5);
    CHECK_EQ(t.footprint(), 9);
}

static void testImportedTypeIsPatchable() {
    // End to end: an imported type patches, addresses and outputs DMX.
    Engine eng;
    GdtfReport r = parseGdtfXml(kSpotXml);
    CHECK(r.ok);

    const int ti = eng.patch.addType(r.info.modes[0].type);
    std::string err;
    CHECK(eng.patch.add({1, "", ti, 1, 1}, &err));
    eng.setClock(0.0);
    eng.syncUniverses();

    Console con(eng);
    CHECK(con.execute("Fixture 1 At 100").ok);
    eng.tick();
    // The dimmer was at GDTF offset 5, so DMX channel 5.
    CHECK_EQ(int(eng.universe(1)->data[4]), 255);

    // 16-bit pan splits across channels 1 and 2.
    CHECK(con.execute("Fixture 1 At Pan 100").ok);
    eng.tick();
    CHECK_EQ(int(eng.universe(1)->data[0]), 255);
    CHECK_EQ(int(eng.universe(1)->data[1]), 255);

    // A subtractive colour attribute addresses the right channel.
    CHECK(con.execute("Fixture 1 At Magenta 50").ok);
    eng.tick();
    CHECK_EQ(int(eng.universe(1)->data[7]), 128);

    // And the multi-cell bar patches as one entry with four cells.
    GdtfReport b = parseGdtfXml(kBarXml);
    const int bi = eng.patch.addType(b.info.modes[0].type);
    CHECK(eng.patch.add({10, "", bi, 1, 100}, &err));
    eng.syncUniverses();
    CHECK_EQ(eng.patch.cellsOf(10).size(), size_t(4));
    CHECK(con.execute("Fixture 10 At Red 100").ok);
    eng.tick();
    // Red broadcast to all four cells: offsets 2, 5, 8, 9 from address 100.
    CHECK_EQ(int(eng.universe(1)->data[100]), 255);
    CHECK_EQ(int(eng.universe(1)->data[103]), 255);
    CHECK_EQ(int(eng.universe(1)->data[106]), 255);
    CHECK_EQ(int(eng.universe(1)->data[107]), 255);
}

static void testMalformedInput() {
    CHECK(!parseGdtfXml("<GDTF/>").ok);
    CHECK(parseGdtfXml("<GDTF/>").error.find("FixtureType") != std::string::npos);

    CHECK(!parseGdtfXml("<GDTF><FixtureType Name=\"x\"/></GDTF>").ok);
    CHECK(parseGdtfXml("<GDTF><FixtureType Name=\"x\"/></GDTF>").error.find("DMXModes") !=
          std::string::npos);

    CHECK(!parseGdtfXml("not xml").ok);

    // A mode with no channels is a warning, not a failure: the rest of the
    // profile is still usable.
    GdtfReport r = parseGdtfXml(
        "<GDTF><FixtureType Name=\"x\"><DMXModes><DMXMode Name=\"Empty\"/>"
        "</DMXModes></FixtureType></GDTF>");
    CHECK(r.ok);
    CHECK_EQ(r.warnings.size(), size_t(1));
    CHECK_EQ(r.info.modes.size(), size_t(1));
    CHECK_EQ(r.info.modes[0].type.footprint(), 0);
}

static void testZipAndPackageLoad() {
    const std::string pkg = tmpDir() + "/test-spot.gdtf";
    if (!makeGdtfPackage(pkg, kSpotXml)) {
        std::printf("      (skipped: system zip unavailable)\n");
        return;
    }

    // The ZIP reader is tested against a file it did not write.
    ZipReader zip;
    std::string err;
    CHECK(zip.openFile(pkg, &err));
    CHECK(err.empty());
    CHECK(zip.entries().size() >= 1);
    CHECK(zip.has("description.xml"));
    CHECK(zip.has("DESCRIPTION.XML"));   // case-insensitive lookup
    CHECK(!zip.has("nothing.xml"));

    const auto text = zip.readText("description.xml", &err);
    CHECK(text.has_value());
    CHECK(text->find("Test Spot 700") != std::string::npos);
    CHECK_EQ(text->size(), std::string(kSpotXml).size());

    // And the whole package path works.
    GdtfReport r = loadGdtfFile(pkg);
    CHECK(r.ok);
    CHECK(r.info.name == "Test Spot 700");
    CHECK_EQ(r.info.modes.size(), size_t(2));

    // A missing or non-zip file fails cleanly.
    CHECK(!loadGdtfFile(tmpDir() + "/does-not-exist.gdtf").ok);
    const std::string junk = tmpDir() + "/junk.gdtf";
    {
        std::ofstream o(junk, std::ios::binary);
        o << "this is definitely not a zip archive";
    }
    GdtfReport bad = loadGdtfFile(junk);
    CHECK(!bad.ok);
    CHECK(!bad.error.empty());
}

int main() {
    std::printf("lumen gdtf\n");
    testXmlParsing();
    testGdtfDmxValues();
    testAttributeMapping();
    testParseSpot();
    testChannelSetsBecomeRangeLabels();
    testImportedProfileFeedsTheAutoStepper();
    testGeometryReferencesBecomeCells();
    testImportedTypeIsPatchable();
    testMalformedInput();
    testZipAndPackageLoad();
    return check::report("gdtf");
}
