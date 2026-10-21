// Show.hpp — show file persistence (SPEC §17.7, §17.8).
//
// SQLite, single file. The deciding reason is TRANSACTIONAL WRITES: a console
// crashing mid-save must not destroy the show. JSON and a directory-of-files
// both fail that test. It is also queryable, which §16.3's introspection wants
// anyway, and it is one file to copy to a USB stick at 2am.
//
// Media and content are REFERENCED, not embedded (§17.7), and sampled pixel
// values are never stored (§13.3).
#pragma once

#include "lumen/Engine.hpp"

#include <string>
#include <vector>

namespace lumen {

struct SaveReport {
    bool ok = false;
    std::string error;
    int fixtures = 0;
    int types = 0;
    int sequences = 0;
    int cues = 0;
    int values = 0;
    int phasers = 0;
    int executors = 0;
    int groups = 0;
    int presets = 0;
    int recipes = 0;
    int library = 0;
};

struct LoadReport {
    bool ok = false;
    std::string error;
    int schemaVersion = 0;
    std::string showName;
    int fixtures = 0;
    int sequences = 0;
    int cues = 0;
    int executors = 0;
    int groups = 0;
    int presets = 0;
    int recipes = 0;
    int library = 0;
    // Things the file referenced that this build could not restore, rather
    // than failing the whole load. A show must open even if something in it
    // is unknown to this version.
    std::vector<std::string> warnings;
};

// The schema version written into every file. A load refuses a NEWER file
// rather than silently misreading it.
inline constexpr int kShowSchemaVersion = 1;

class ShowFile {
public:
    // Writes the whole show inside one transaction. On failure nothing is
    // committed and any existing file is untouched.
    static SaveReport save(const Engine& eng, const std::string& path,
                           const std::string& showName = {});

    // Replaces the engine's contents. On failure the engine is left as it was.
    static LoadReport load(Engine& eng, const std::string& path);

    // Reads only the header, for a recovery prompt that must not load a
    // possibly-broken show to describe it.
    static LoadReport inspect(const std::string& path);
};

// ---------------------------------------------------------------------------
// Autosave (§17.8)
//
// For live use these are not conveniences. Rolling versions rather than
// overwriting one file, so a bad save cannot take the show with it.
// ---------------------------------------------------------------------------
class Autosave {
public:
    Autosave(std::string directory, std::string baseName, int keep = 5)
        : dir_(std::move(directory)), base_(std::move(baseName)), keep_(keep) {}

    // Writes the next rolling version. Call before any destructive operation
    // as well as on a timer.
    SaveReport write(const Engine& eng, const std::string& reason = {});

    // Newest first, so crash recovery can offer the most recent.
    std::vector<std::string> versions() const;
    std::string newest() const;

    // A snapshot the operator takes deliberately before a risky change; kept
    // outside the rolling set so rotation cannot discard it.
    SaveReport snapshot(const Engine& eng, const std::string& name);

    int keep() const { return keep_; }

private:
    std::string pathFor(int index) const;
    void rotate();

    std::string dir_;
    std::string base_;
    int keep_ = 5;
    int counter_ = 0;
};

} // namespace lumen
