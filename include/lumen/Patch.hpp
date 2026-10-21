// Patch.hpp — fixture types, cells and patching (SPEC §1.3, §17.2, §17.3).
//
// Three numbers exist per fixture and must not be conflated:
//   Uid      — immutable, what every reference stores (§8.1)
//   id       — the user-facing number typed at the command line
//   address  — universe + start channel
// Because cues reference the Uid, re-addressing and renumbering cannot break a
// show (§17.2).
#pragma once

#include "lumen/Attribute.hpp"
#include "lumen/Uid.hpp"
#include "lumen/Value.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace lumen {

// ---------------------------------------------------------------------------
// Channel
// ---------------------------------------------------------------------------
struct ChannelDef {
    Attr attr = Attr::Dimmer;
    int coarse = 0;             // 0-based offset inside the footprint
    int fine = -1;              // -1 == 8-bit only
    CellId cell = 0;            // 0 = the fixture itself, 1..n = sub-fixture
    uint8_t defaultValue = 0;   // §11.9 "return to default"
    bool invert = false;
};

// A named DMX value range inside a channel — GDTF's ChannelSet. These names
// ARE the range labels (§7.2), and they drive encoder range-jumping (§2.6.1)
// and the auto-stepped preset generator (§7.3).
struct ChannelSet {
    Attr attr = Attr::Dimmer;
    CellId cell = 0;
    std::string name;
    uint8_t dmxFrom = 0;
    uint8_t dmxTo = 255;
    float physFrom = 0.f;
    float physTo = 0.f;
    bool anchor = false;  // a semantic zero: stop / open / off (§7.3)
};

struct FixtureType {
    Uid uid{};
    std::string name;
    std::vector<ChannelDef> channels;
    std::vector<ChannelSet> sets;
    CellId cellCount = 0;  // 0 = no sub-fixtures

    int footprint() const;
    bool hasAttr(Attr a, CellId cell = 0) const;
    const ChannelDef* channel(Attr a, CellId cell = 0) const;
    std::vector<ChannelSet> setsFor(Attr a, CellId cell = 0) const;

    // Highlight state is derived from the taxonomy (§20.7): drive the
    // intensity-kind attribute full, and where the type has none, drive colour
    // to white instead — which is what makes highlight work on an LED with no
    // dimmer channel.
    ValueLayer highlightFor(FixtureId id) const;
};

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------
struct Fixture {
    Uid uid{};
    FixtureId id = 0;
    std::string name;
    int typeIndex = -1;
    int universe = 1;    // 1-based
    int address = 1;     // 1-based DMX start address
    int order = 0;       // patch order
    std::vector<std::string> tags;

    // Stage position (§17.4). Needed by position presets, pixel mapping,
    // laser zone placement, the visualiser, and the stage-based spread
    // sources in §20.2.
    float x = 0.f, y = 0.f, z = 0.f;
    float rotX = 0.f, rotY = 0.f, rotZ = 0.f;
    bool positioned = false;
};

// ---------------------------------------------------------------------------
// Patch
// ---------------------------------------------------------------------------
class Patch {
public:
    explicit Patch(UidGen& uids) : uids_(&uids) {}

    // Types ----------------------------------------------------------------
    int addType(FixtureType t);
    int findType(const std::string& name) const;
    const FixtureType& type(int index) const;
    int typeCount() const { return static_cast<int>(types_.size()); }

    // Fixtures -------------------------------------------------------------
    struct PatchRequest {
        FixtureId id = 0;
        std::string name;
        int typeIndex = -1;
        int universe = 1;
        int address = 1;
        // A load restores the saved identity. Set it here rather than writing
        // the field afterwards, or the by-UID index keeps the generated one.
        Uid uid{};
    };

    bool add(const PatchRequest& req, std::string* err = nullptr);

    // Patches `count` fixtures from `firstId`, auto-addressing with an optional
    // gap between footprints (§17.2).
    int addRange(FixtureId firstId, int count, int typeIndex, int universe,
                 int firstAddress, int gap = 0, std::string* err = nullptr);

    // Next free address in a universe that fits `footprint` channels; 0 if none.
    int nextFreeAddress(int universe, int footprint, int gap = 0) const;

    const Fixture* find(FixtureId id) const;
    const Fixture* findByUid(Uid uid) const;
    const std::vector<Fixture>& fixtures() const { return fixtures_; }
    size_t size() const { return fixtures_.size(); }

    // Re-address and renumber. Safe by construction: references are UIDs, so
    // neither can break a show (§17.2, §17.3).
    bool readdress(FixtureId id, int universe, int address, std::string* err = nullptr);
    bool renumber(FixtureId from, FixtureId to, std::string* err = nullptr);

    // Stage placement (§17.4). A setter rather than a mutable accessor, so an
    // importer cannot reach in and change identity fields.
    bool setPosition(FixtureId id, float x, float y, float z,
                     float rx = 0.f, float ry = 0.f, float rz = 0.f);
    bool addTag(FixtureId id, const std::string& tag);
    bool rename(FixtureId id, const std::string& name);

    bool remove(FixtureId id);
    void clear();

    std::vector<int> universeList() const;
    const FixtureType* typeOf(FixtureId id) const;

    // Cells present on a fixture: {0} for a simple fixture, {1..n} for a
    // multi-instance one (§1.3).
    std::vector<CellId> cellsOf(FixtureId id) const;

    // Reports overlaps and over-capacity universes (§17.2).
    struct Conflict {
        FixtureId a = 0, b = 0;
        int universe = 0, address = 0;
        std::string detail;
    };
    std::vector<Conflict> conflicts() const;

private:
    bool footprintFree(int universe, int address, int fp, FixtureId ignore,
                       const Fixture** clash) const;

    UidGen* uids_;
    std::vector<FixtureType> types_;
    std::vector<Fixture> fixtures_;
    std::unordered_map<FixtureId, size_t> byId_;
    std::unordered_map<uint64_t, size_t> byUid_;
};

// A small starter library, enough to exercise the engine: a dimmer, an RGBW
// par, a moving spot with 16-bit pan/tilt, and a 12-cell pixel bar that
// exercises the sub-fixture path.
void installStarterTypes(Patch& patch);

} // namespace lumen
