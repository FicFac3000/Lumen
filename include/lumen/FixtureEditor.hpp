// FixtureEditor.hpp — the fixture authoring model and its two views
// (SPEC §1.2, §1.3, §1.4).
//
// ONE MODEL, TWO VIEWS. §1.2 is explicit that the internal model is the
// geometry tree and that the table and tree are both live on it — no
// import/export step between them. So this file holds the authoring truth:
//
//   FixtureDef  --compile-->  FixtureType   (what the engine patches)
//                 |
//                 +--tableView()--> flat channel rows   (ChamSys style)
//                 +--treeView()---> geometry hierarchy  (MA3 style)
//
// FixtureType is deliberately the COMPILED artifact, not the source. It is
// flat because the DMX renderer wants it flat, and nothing in the engine
// should carry a geometry tree it never walks. Editing a flat list is what
// loses multi-instance structure, which is the whole reason §1.2 asks for the
// tree in the first place.
//
// The keystone is §1.3's first want: one cell definition instantiates N times
// with automatic DMX offset. That falls out of GDTF's own semantics — a
// GeometryReference is an instance of a shared geometry — so a channel
// declared once against the shared geometry compiles to N channels, one per
// reference, each with its own cell and offset.
#pragma once

#include "lumen/Patch.hpp"

#include <optional>
#include <string>
#include <vector>

namespace lumen {

// ---------------------------------------------------------------------------
// Geometry tree
// ---------------------------------------------------------------------------
enum class GeometryKind : uint8_t {
    Normal,     // a plain body geometry
    Axis,       // a moving part (pan/tilt yoke)
    Beam,       // the emitter
    Reference,  // an INSTANCE of another geometry — this is what makes cells
};

const char* geometryKindName(GeometryKind k);

struct GeometryNode {
    std::string name;
    GeometryKind kind = GeometryKind::Normal;

    // Reference only: which geometry this instantiates, and the DMX offset
    // added to every channel inside it. The offset is what lets twelve
    // identical pixels share one channel declaration.
    std::string referenced;
    int dmxOffset = 0;

    std::vector<GeometryNode> children;

    bool isReference() const { return kind == GeometryKind::Reference; }
};

// ---------------------------------------------------------------------------
// Channels
// ---------------------------------------------------------------------------
// A named DMX range inside a channel. Same meaning as ChannelSet, but held in
// the authoring model so the editor can add and rename ranges without
// recompiling to find them again.
struct ChannelSetDef {
    std::string name;
    uint8_t dmxFrom = 0;
    uint8_t dmxTo = 255;
    float physFrom = 0.f;
    float physTo = 0.f;
    bool anchor = false;   // a semantic zero: stop / open / off (§7.3)
};

// OFFSETS ARE RELATIVE TO THE OWNING INSTANCE. On a plain geometry that is
// the same as the mode, because its instance offset is zero. On an instanced
// one it is relative to the cell's own start: a pixel's red is offset 0, and
// each GeometryReference's dmxOffset places the whole cell. That is what lets
// one declaration serve twelve pixels — the alternative, offsets relative to
// the mode, makes the first cell's numbering special and every other cell a
// correction to it.
struct ChannelDecl {
    Attr attr = Attr::Dimmer;
    // Which geometry declares this channel. Empty means the mode's root, so a
    // simple fixture needs no geometry names at all.
    std::string geometry;

    int coarse = 0;          // 0-based, relative to the owning instance
    int fine = -1;           // -1 == 8-bit only
    uint8_t defaultValue = 0;
    bool invert = false;

    std::vector<ChannelSetDef> sets;
};

struct ModeDef {
    std::string name;
    std::string rootGeometry;
    std::vector<ChannelDecl> channels;
};

// ---------------------------------------------------------------------------
// FixtureDef — the authoring source
// ---------------------------------------------------------------------------
struct FixtureDef {
    Uid uid{};
    std::string manufacturer;
    std::string name;
    std::string shortName;
    std::string typeId;      // GDTF FixtureTypeID
    std::string description;

    std::vector<GeometryNode> geometries;  // roots
    std::vector<ModeDef> modes;

    // §1.4 asks for revision history. One revision entry per saved edit, kept
    // with the definition so a borrowed profile says where it came from.
    struct Revision {
        std::string text;
        std::string date;
    };
    std::vector<Revision> revisions;

    // Compiled type name, matching what GDTF import produces, so an edited
    // profile replaces rather than duplicates its imported original.
    std::string typeName(size_t modeIndex) const;

    const ModeDef* mode(const std::string& modeName) const;
    int modeIndex(const std::string& modeName) const;

    // Depth-first search through the whole tree.
    const GeometryNode* findGeometry(const std::string& geoName) const;
    GeometryNode* findGeometry(const std::string& geoName);
};

// ---------------------------------------------------------------------------
// Instances (§1.3)
// ---------------------------------------------------------------------------
// What a geometry name resolves to at compile time. A plain geometry is one
// instance with no cell; a geometry that K references point at is K instances,
// each its own cell.
struct GeometryInstance {
    std::string instanceName;  // the reference's name, or the geometry's own
    CellId cell = 0;           // 0 == the fixture itself
    int dmxOffset = 0;
};

// Cells are numbered in document order of the references, so a pixel bar's
// cell 1 is the first pixel in the plot rather than whichever one the hash
// landed on first.
std::vector<GeometryInstance> instancesOf(const FixtureDef& def,
                                          const std::string& geoName);

// ---------------------------------------------------------------------------
// Compile (§1.2)
// ---------------------------------------------------------------------------
struct CompileReport {
    bool ok = false;
    FixtureType type;
    int channelsEmitted = 0;
    // Declarations that served more than one instance — the count of "one
    // definition, N cells" expansions, not the channels they produced.
    int declarationsExpanded = 0;
    std::vector<std::string> errors;    // compile refused
    std::vector<std::string> warnings;  // compiled, but look at this
};

CompileReport compile(const FixtureDef& def, size_t modeIndex);

// ---------------------------------------------------------------------------
// Validation (§1.4)
// ---------------------------------------------------------------------------
struct Issue {
    enum class Level : uint8_t { Error, Warning, Info };
    Level level = Level::Error;
    std::string what;
    // Where, so the editor can put the cursor on it rather than just saying
    // something is wrong somewhere.
    int channelIndex = -1;
    std::string geometry;
};

const char* issueLevelName(Issue::Level l);

std::vector<Issue> validate(const FixtureDef& def, size_t modeIndex);

// ---------------------------------------------------------------------------
// Table view — ChamSys style (§1.2)
// ---------------------------------------------------------------------------
// One row per compiled DMX channel, in address order. This is the flattened
// mode, which is what makes it fast to scan: a 16-bit channel is one row with
// both offsets, not two rows.
struct TableRow {
    int dmx = 1;             // 1-based coarse address within the footprint
    int fineDmx = 0;         // 0 == none
    Attr attr = Attr::Dimmer;
    std::string attrName;
    std::string geometry;    // the instance that owns it
    CellId cell = 0;
    uint8_t defaultValue = 0;
    bool invert = false;
    int setCount = 0;
    // Which ChannelDecl this came from, so an edit in the table reaches the
    // model rather than the compiled copy.
    int channelIndex = -1;
};

std::vector<TableRow> tableView(const FixtureDef& def, size_t modeIndex);

// ---------------------------------------------------------------------------
// Tree view — MA3 style (§1.2, §1.3)
// ---------------------------------------------------------------------------
// The geometry hierarchy with references EXPANDED, because an unexpanded
// reference is exactly the thing the tree exists to show. Each row carries its
// cell, so the master/cell relationship (§1.3) is visible: parent rows hold
// the dimmer and shutter, cell rows hold the colour.
struct TreeRow {
    int depth = 0;
    std::string name;
    GeometryKind kind = GeometryKind::Normal;
    CellId cell = 0;
    int dmxOffset = 0;
    // Channels declared at this node, as 1-based addresses within the mode.
    std::vector<std::pair<Attr, int>> channels;
    bool expandedReference = false;  // this row stands in for a reference
    // A geometry that only ever appears through references: the shared cell
    // DEFINITION. Its channel numbers are offsets within a cell, not
    // addresses, and it is the row to edit to change every cell at once —
    // which is why it is shown rather than hidden behind its instances.
    bool isTemplate = false;
    std::string referenced;
};

std::vector<TreeRow> treeView(const FixtureDef& def, size_t modeIndex);

// ---------------------------------------------------------------------------
// Normalisation (§1.2, §1.3)
// ---------------------------------------------------------------------------
// GDTF files in the wild declare a multi-instance fixture one channel per
// instance: four cells means twelve ColorAdd declarations, not three. That is
// faithful to the file and useless to edit — changing a pixel bar's colour
// layout would mean the same edit twelve times, which is exactly what §1.2's
// tree view is supposed to spare the user.
//
// So an imported definition is normalised: where every reference to a shared
// geometry declares the same channels at the same offsets within its own
// instance, those declarations collapse into ONE against the shared geometry.
// Nothing about the compiled result changes — it is the same channels at the
// same addresses — only what there is to edit.
//
// A group whose instances differ is left exactly as it is. An LED bar whose
// last cell has an extra channel is a real fixture, and silently averaging it
// into a uniform cell would corrupt the patch.
struct NormaliseReport {
    int collapsed = 0;             // reference groups collapsed to one
    int declarationsRemoved = 0;
    // Groups left alone, with the reason, so the editor can say why a fixture
    // still has to be edited cell by cell.
    std::vector<std::pair<std::string, std::string>> kept;
};

NormaliseReport normaliseInstances(FixtureDef& def, size_t modeIndex);

// ---------------------------------------------------------------------------
// Edit operations
// ---------------------------------------------------------------------------
// Everything that changes a definition goes through these, so the two views
// cannot drift and so the command layer has one surface to expose (§16.2).
class FixtureEditor {
public:
    explicit FixtureEditor(FixtureDef& def) : def_(&def) {}

    FixtureDef& def() { return *def_; }
    const FixtureDef& def() const { return *def_; }

    // Channels -------------------------------------------------------------
    // Appends at the next free offset when `coarse` is negative, which is what
    // the table's "add row" does.
    bool addChannel(size_t modeIndex, Attr a, const std::string& geometry = {},
                    int coarse = -1, int fine = -1, std::string* err = nullptr);
    bool removeChannel(size_t modeIndex, int channelIndex, std::string* err = nullptr);
    bool setChannelAttr(size_t modeIndex, int channelIndex, Attr a,
                        std::string* err = nullptr);
    bool setChannelOffset(size_t modeIndex, int channelIndex, int coarse, int fine,
                          std::string* err = nullptr);
    bool addChannelSet(size_t modeIndex, int channelIndex, ChannelSetDef set,
                       std::string* err = nullptr);

    // Packs offsets to close gaps left by deletions, keeping the current
    // order. Returns how many channels moved.
    int packOffsets(size_t modeIndex);

    // Re-spaces the references to `referenced` so each instance clears the
    // cell's current footprint, keeping the first one's offset as the base.
    // Growing a cell definition otherwise leaves every instance overlapping
    // the next, so addChannel and setChannelOffset call this themselves for an
    // instanced geometry. Returns how many references moved.
    //
    // Channels belonging to OTHER geometries are deliberately left where they
    // are: moving a fixture's master channels because a cell grew would be a
    // surprise. The compile reports any collision that re-spacing pushed into,
    // which is the editor saying what still needs a decision.
    int respaceInstances(size_t modeIndex, const std::string& referenced);

    // Geometry -------------------------------------------------------------
    bool addGeometry(const std::string& parent, GeometryNode node,
                     std::string* err = nullptr);
    bool removeGeometry(const std::string& geoName, std::string* err = nullptr);
    bool renameGeometry(const std::string& from, const std::string& to,
                        std::string* err = nullptr);

    // §1.3's first want: instantiate one cell definition N times with
    // automatic DMX offset. `stride` is the footprint of the referenced
    // geometry's channels; passing 0 computes it.
    int addInstances(size_t modeIndex, const std::string& parent,
                     const std::string& referenced, int count,
                     const std::string& namePrefix, int firstOffset,
                     int stride = 0, std::string* err = nullptr);

    // Modes ----------------------------------------------------------------
    size_t addMode(const std::string& name, const std::string& rootGeometry = {});
    bool removeMode(size_t modeIndex, std::string* err = nullptr);

private:
    ModeDef* modeAt(size_t i);
    FixtureDef* def_;
};

// ---------------------------------------------------------------------------
// A starter definition, matching the Pixel Bar 12 in the starter library, so
// the editor has something multi-instance to work on from a cold start.
// ---------------------------------------------------------------------------
FixtureDef starterPixelBar();
FixtureDef starterMovingSpot();

} // namespace lumen
