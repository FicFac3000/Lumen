// Zip.hpp — a read-only ZIP reader (GDTF and MVR files are ZIP archives).
//
// Reads the central directory and inflates entries with zlib. Only what is
// needed: stored and deflated entries, no encryption, no spanning.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lumen {

struct ZipEntry {
    std::string name;
    uint32_t compressedSize = 0;
    uint32_t uncompressedSize = 0;
    uint16_t method = 0;       // 0 = stored, 8 = deflate
    uint32_t localOffset = 0;
    uint32_t crc32 = 0;
};

class ZipReader {
public:
    bool openFile(const std::string& path, std::string* err = nullptr);
    bool openBytes(std::vector<uint8_t> bytes, std::string* err = nullptr);

    const std::vector<ZipEntry>& entries() const { return entries_; }
    bool has(const std::string& name) const;
    // Case-insensitive, and tolerant of a leading "./" — GDTF files in the
    // wild are inconsistent about both.
    const ZipEntry* findLoose(const std::string& name) const;

    std::optional<std::string> readText(const std::string& name,
                                       std::string* err = nullptr) const;
    std::optional<std::vector<uint8_t>> read(const std::string& name,
                                             std::string* err = nullptr) const;

private:
    bool parseCentralDirectory(std::string* err);

    std::vector<uint8_t> data_;
    std::vector<ZipEntry> entries_;
};

// Inflates a raw deflate stream (no zlib header), which is what ZIP stores.
std::optional<std::vector<uint8_t>> inflateRaw(const uint8_t* src, size_t srcLen,
                                               size_t expectedOut);

// Deflates to a raw stream. Returns nullopt only on a zlib failure, never on
// incompressible input — the caller decides whether to store instead.
std::optional<std::vector<uint8_t>> deflateRaw(const uint8_t* src, size_t srcLen,
                                               int level = 6);

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------
// Enough to produce a .gdtf and a .mvr, which are both just ZIPs: stored and
// deflated entries, no encryption, no spanning, no ZIP64. Entries are written
// in the order they were added, because MVR readers in the wild expect
// GeneralSceneDescription.xml first.
class ZipWriter {
public:
    // An entry already compressed harder than deflate manages — an embedded
    // .gdtf inside a .mvr, say — is stored rather than re-deflated, so
    // `compress` is a request, not a promise.
    void add(const std::string& name, std::vector<uint8_t> bytes, bool compress = true);
    void addText(const std::string& name, const std::string& text, bool compress = true);

    size_t entryCount() const { return pending_.size(); }
    bool has(const std::string& name) const;

    std::vector<uint8_t> bytes() const;
    // Writes to a temp sibling and renames, so a failed write cannot destroy
    // a file that was already there.
    bool writeFile(const std::string& path, std::string* err = nullptr) const;

private:
    struct Pending {
        std::string name;
        std::vector<uint8_t> raw;
        bool compress = true;
    };
    std::vector<Pending> pending_;
};

} // namespace lumen
