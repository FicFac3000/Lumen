// Library.hpp — the fixture library (SPEC §1.1, §1.4, §17.7).
//
// GDTF IS THE FIXTURE FILE FORMAT, AND AN IMPORTED PACKAGE KEEPS ITS ORIGINAL
// BYTES. That is the decision this file exists to enforce.
//
// Lumen's model covers geometries, modes, channels and channel sets. A real
// .gdtf package carries considerably more: 3D models, wheels and slots,
// emitters, filters, colour spaces, physical descriptions, DMX profiles,
// revision history. Regenerating a package from the model would silently
// discard all of it — so the original is kept verbatim, and an export
// re-emits exactly what came in.
//
// A profile is only regenerated when the user actually edited it, and the
// export says what regenerating cost. An untouched profile exports byte for
// byte, which is what makes Lumen safe to put in the middle of someone else's
// workflow: a plot that goes out is the plot that came in.
//
// The package is EMBEDDED in the show file, unlike media (§17.7). A profile is
// tens of kilobytes and a show has to be one file you can copy to a USB stick
// at 2am; a show that opens with its fixtures missing because a library folder
// moved is not a show.
#pragma once

#include "lumen/FixtureEditor.hpp"
#include "lumen/Gdtf.hpp"

#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace lumen {

struct LibraryEntry {
    Uid uid{};
    int id = 0;
    std::string name;        // usually def.name, but renameable
    FixtureDef def;          // the editable model (§1.2)

    // The original package, kept verbatim. Empty for a fixture authored in
    // Lumen, which therefore has nothing to preserve and is always written
    // from the model.
    std::vector<uint8_t> gdtf;
    std::string sourceName;  // the filename it arrived as

    // Set the moment the model diverges from `gdtf`. Until then an export is
    // a byte-for-byte copy.
    bool edited = false;

    std::vector<std::string> tags;

    bool hasOriginal() const { return !gdtf.empty(); }
    // True when exporting would lose what the model does not carry.
    bool exportIsLossy() const { return hasOriginal() && edited; }
};

// What an export produced and what it cost.
struct LibraryExport {
    bool ok = false;
    std::vector<uint8_t> bytes;
    std::string suggestedName;
    // True when these bytes are the original package, untouched.
    bool verbatim = false;
    // Named rather than silently lost, when a regeneration was needed.
    std::vector<std::string> dropped;
    std::string error;
};

class FixtureLibrary {
public:
    explicit FixtureLibrary(UidGen& uids) : uids_(&uids) {}

    LibraryEntry& add(LibraryEntry e);
    // Re-inserts an entry exactly as given, for a load: its own UID, id and
    // name, with no renaming or renumbering.
    LibraryEntry& restore(LibraryEntry e);

    LibraryEntry* find(Uid uid);
    const LibraryEntry* find(Uid uid) const;
    LibraryEntry* byId(int id);
    LibraryEntry* byName(const std::string& name);
    const LibraryEntry* byName(const std::string& name) const;
    const std::deque<LibraryEntry>& all() const { return entries_; }
    size_t size() const { return entries_.size(); }
    bool remove(Uid uid);

    // Imports a .gdtf package, keeping its bytes. The report's warnings carry
    // through, because an unmapped attribute matters as much on import into
    // the library as it did at the parse.
    struct ImportResult {
        bool ok = false;
        std::string error;
        Uid uid{};
        int modes = 0;
        std::vector<std::string> warnings;
    };
    ImportResult importPackage(std::vector<uint8_t> bytes, const std::string& sourceName,
                               const AttributeMap& map = AttributeMap{});
    ImportResult importFile(const std::string& path,
                            const AttributeMap& map = AttributeMap{});

    // Takes a definition authored in Lumen. It has no original, so it always
    // exports from the model.
    LibraryEntry& addAuthored(FixtureDef def);

    // Marks an entry edited, which is what makes a later export regenerate.
    // Call it from every edit path rather than inferring divergence by
    // comparison: a diff of the model against the package cannot see the
    // things the model never held.
    bool markEdited(Uid uid);

    LibraryExport exportEntry(Uid uid) const;
    bool exportToFile(Uid uid, const std::string& path, std::string* err = nullptr) const;

    // Compiles a mode into a runtime type and registers it in the patch, so
    // the library is what feeds patching (§1.1).
    struct PublishResult {
        bool ok = false;
        std::string error;
        int typeIndex = -1;
        std::string typeName;
        std::vector<std::string> warnings;
    };
    PublishResult publish(Uid uid, size_t modeIndex, Patch& patch) const;

private:
    UidGen* uids_;
    std::deque<LibraryEntry> entries_;  // deque: references stay valid
};

} // namespace lumen
