#include "lumen/Library.hpp"

#include "lumen/Zip.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace lumen {

namespace {

// A sensible .gdtf filename: what the fixture is called, with the characters
// a filesystem objects to replaced.
std::string fileNameFor(const LibraryEntry& e) {
    if (!e.sourceName.empty()) return e.sourceName;
    std::string base = e.name.empty() ? "fixture" : e.name;
    if (!e.def.manufacturer.empty()) base = e.def.manufacturer + "@" + base;
    std::string out;
    for (char c : base) {
        const bool bad = c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
                         c == '"' || c == '<' || c == '>' || c == '|';
        out += bad ? '_' : c;
    }
    return out + ".gdtf";
}

} // namespace

// ---------------------------------------------------------------------------
// Store
// ---------------------------------------------------------------------------
LibraryEntry& FixtureLibrary::add(LibraryEntry e) {
    if (!e.uid.valid()) e.uid = uids_->next();
    uids_->observe(e.uid);
    if (e.id == 0) {
        int maxId = 0;
        for (const auto& x : entries_) maxId = std::max(maxId, x.id);
        e.id = maxId + 1;
    }
    if (e.name.empty()) e.name = e.def.name.empty() ? "Fixture" : e.def.name;
    e.def.uid = e.uid;
    entries_.push_back(std::move(e));
    return entries_.back();
}

LibraryEntry& FixtureLibrary::restore(LibraryEntry e) {
    if (!e.uid.valid()) e.uid = uids_->next();
    uids_->observe(e.uid);
    entries_.push_back(std::move(e));
    return entries_.back();
}

LibraryEntry* FixtureLibrary::find(Uid uid) {
    for (auto& e : entries_)
        if (e.uid == uid) return &e;
    return nullptr;
}

const LibraryEntry* FixtureLibrary::find(Uid uid) const {
    for (const auto& e : entries_)
        if (e.uid == uid) return &e;
    return nullptr;
}

LibraryEntry* FixtureLibrary::byId(int id) {
    for (auto& e : entries_)
        if (e.id == id) return &e;
    return nullptr;
}

LibraryEntry* FixtureLibrary::byName(const std::string& name) {
    for (auto& e : entries_)
        if (e.name == name) return &e;
    return nullptr;
}

const LibraryEntry* FixtureLibrary::byName(const std::string& name) const {
    for (const auto& e : entries_)
        if (e.name == name) return &e;
    return nullptr;
}

bool FixtureLibrary::remove(Uid uid) {
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].uid != uid) continue;
        entries_.erase(entries_.begin() + static_cast<long>(i));
        return true;
    }
    return false;
}

bool FixtureLibrary::markEdited(Uid uid) {
    LibraryEntry* e = find(uid);
    if (!e) return false;
    e->edited = true;
    return true;
}

// ---------------------------------------------------------------------------
// Import
// ---------------------------------------------------------------------------
FixtureLibrary::ImportResult FixtureLibrary::importPackage(std::vector<uint8_t> bytes,
                                                           const std::string& sourceName,
                                                           const AttributeMap& map) {
    ImportResult res;
    if (bytes.empty()) {
        res.error = "empty package";
        return res;
    }

    ZipReader zip;
    std::string err;
    if (!zip.openBytes(bytes, &err)) {
        res.error = "not a gdtf package: " + err;
        return res;
    }
    const auto xml = zip.readText("description.xml", &err);
    if (!xml) {
        res.error = "no description.xml: " + err;
        return res;
    }
    GdtfReport g = parseGdtfXml(*xml, map);
    if (!g.ok) {
        res.error = "gdtf: " + g.error;
        return res;
    }

    LibraryEntry e;
    e.def = g.info.def;
    e.name = g.info.name.empty() ? sourceName : g.info.name;
    e.sourceName = sourceName;
    // The whole point: the package is kept, not reconstructed.
    e.gdtf = std::move(bytes);
    e.edited = false;

    res.warnings = g.warnings;
    for (const auto& u : g.unmapped)
        res.warnings.push_back("unmapped attribute " + u.gdtfAttribute + " in mode '" +
                               u.mode + "'");

    const LibraryEntry& stored = add(std::move(e));
    res.uid = stored.uid;
    res.modes = static_cast<int>(stored.def.modes.size());
    res.ok = true;
    return res;
}

FixtureLibrary::ImportResult FixtureLibrary::importFile(const std::string& path,
                                                        const AttributeMap& map) {
    ImportResult res;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        res.error = "cannot open " + path;
        return res;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        res.error = "empty file: " + path;
        return res;
    }

    // Keep just the filename, since that is what an MVR's GDTFSpec names.
    std::string base = path;
    const size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);

    return importPackage(std::move(bytes), base, map);
}

LibraryEntry& FixtureLibrary::addAuthored(FixtureDef def) {
    LibraryEntry e;
    e.name = def.name;
    e.def = std::move(def);
    // No original to preserve, so an export always writes from the model and
    // loses nothing that was ever there.
    e.edited = false;
    return add(std::move(e));
}

// ---------------------------------------------------------------------------
// Export
// ---------------------------------------------------------------------------
LibraryExport FixtureLibrary::exportEntry(Uid uid) const {
    LibraryExport out;
    const LibraryEntry* e = find(uid);
    if (!e) {
        out.error = "no such fixture in the library";
        return out;
    }
    out.suggestedName = fileNameFor(*e);

    // Untouched: hand back exactly what came in. Nothing is reinterpreted, so
    // nothing can be lost.
    if (e->hasOriginal() && !e->edited) {
        out.bytes = e->gdtf;
        out.verbatim = true;
        out.ok = true;
        return out;
    }

    const GdtfPackage pkg = writeGdtfPackage(e->def);
    if (!pkg.ok) {
        out.error = pkg.error.empty() ? "could not write the profile" : pkg.error;
        return out;
    }
    out.bytes = pkg.bytes;
    out.dropped = pkg.dropped;
    if (e->hasOriginal())
        out.dropped.insert(out.dropped.begin(),
                           "this profile was edited, so it is regenerated from "
                           "Lumen's model rather than copied from '" + e->sourceName +
                           "'");
    out.ok = true;
    return out;
}

bool FixtureLibrary::exportToFile(Uid uid, const std::string& path,
                                  std::string* err) const {
    const LibraryExport x = exportEntry(uid);
    if (!x.ok) {
        if (err) *err = x.error;
        return false;
    }
    const std::string tmp = path + ".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) {
            if (err) *err = "cannot open " + tmp;
            return false;
        }
        o.write(reinterpret_cast<const char*>(x.bytes.data()),
                static_cast<std::streamsize>(x.bytes.size()));
        o.flush();
        if (!o) {
            if (err) *err = "write failed: " + tmp;
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        if (err) *err = "rename failed: " + ec.message();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Publish
// ---------------------------------------------------------------------------
FixtureLibrary::PublishResult FixtureLibrary::publish(Uid uid, size_t modeIndex,
                                                      Patch& patch) const {
    PublishResult res;
    const LibraryEntry* e = find(uid);
    if (!e) {
        res.error = "no such fixture in the library";
        return res;
    }
    if (modeIndex >= e->def.modes.size()) {
        res.error = "no mode " + std::to_string(modeIndex) + " in " + e->name;
        return res;
    }

    const CompileReport c = compile(e->def, modeIndex);
    if (!c.ok) {
        res.error = c.errors.empty() ? "the definition does not compile" : c.errors[0];
        for (const auto& err : c.errors) res.warnings.push_back(err);
        return res;
    }
    res.warnings = c.warnings;
    res.typeName = c.type.name;
    res.typeIndex = patch.addType(c.type);
    res.ok = true;
    return res;
}

} // namespace lumen
