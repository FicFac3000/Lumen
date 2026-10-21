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

} // namespace lumen
