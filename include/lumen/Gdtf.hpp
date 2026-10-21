// Gdtf.hpp — GDTF import (SPEC §1.1, §6.3, §7.2).
//
// GDTF is an IMPORT MAPPING onto Lumen's own taxonomy, not the native model.
// The mapping table is the architecturally important part: everything
// downstream — sorting, filtering, preset organisation, FX role binding — reads
// Lumen's attributes, never GDTF's names (§6.3).
//
// Unmapped source attributes land in a visible bucket rather than being
// dropped, so a profile using something unusual can be fixed rather than
// silently losing a channel.
#pragma once

#include "lumen/FixtureEditor.hpp"
#include "lumen/Patch.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace lumen {

// ---------------------------------------------------------------------------
// The mapping table (§6.3)
// ---------------------------------------------------------------------------
// Maps a GDTF attribute name to a Lumen attribute. Editable, because a profile
// in the wild will eventually use a name this table has not seen.
class AttributeMap {
public:
    AttributeMap();  // installs the standard GDTF mapping

    std::optional<Attr> map(const std::string& gdtfName) const;
    void set(const std::string& gdtfName, Attr a);
    void remove(const std::string& gdtfName);
    size_t size() const { return map_.size(); }

    // GDTF numbers repeated features (Gobo1, Gobo2, Prism1...). A name that
    // differs only by its index maps to the indexed Lumen attribute where one
    // exists, so Gobo3 does not need its own table entry to be recognised.
    std::optional<Attr> mapWithIndex(const std::string& gdtfName) const;

private:
    std::map<std::string, Attr> map_;
};

// ---------------------------------------------------------------------------
// Parse results
// ---------------------------------------------------------------------------
struct GdtfMode {
    std::string name;
    std::string geometry;
    FixtureType type;   // the Lumen type this mode becomes
};

struct GdtfInfo {
    std::string name;
    std::string shortName;
    std::string manufacturer;
    std::string fixtureTypeId;
    std::string description;
    std::vector<GdtfMode> modes;

    // The editable source (§1.2). `modes[i].type` is what the engine patches;
    // this is what the fixture editor's table and tree views work on, with the
    // geometry tree intact and per-instance declarations collapsed so a pixel
    // bar is edited once rather than cell by cell. The two must agree: a
    // definition that compiles to a different footprint than the one patched
    // would be an editor that silently rewrites fixtures.
    FixtureDef def;
};

struct GdtfReport {
    bool ok = false;
    std::string error;
    GdtfInfo info;

    // §6.3: unmapped source attributes are visible rather than dropped.
    struct Unmapped {
        std::string gdtfAttribute;
        std::string mode;
        int offset = 0;
        std::string geometry;
    };
    std::vector<Unmapped> unmapped;
    std::vector<std::string> warnings;

    int channels = 0;
    int channelSets = 0;
    int cells = 0;
};

// Parses a description.xml. `map` may be customised to handle an unusual
// profile without editing the shipped table.
GdtfReport parseGdtfXml(const std::string& xml, const AttributeMap& map = AttributeMap{});

// Opens a .gdtf package and parses the description.xml inside it.
GdtfReport loadGdtfFile(const std::string& path, const AttributeMap& map = AttributeMap{});

// A GDTF DMX value is written "value/byteCount", e.g. "32768/2". Returns the
// value normalised to 0..1 so it can be stored as a default.
float parseGdtfDmx(const std::string& text);

} // namespace lumen
