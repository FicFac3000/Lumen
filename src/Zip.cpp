#include "lumen/Zip.hpp"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace lumen {

namespace {

uint16_t rd16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string normalise(std::string s) {
    if (s.rfind("./", 0) == 0) s = s.substr(2);
    std::replace(s.begin(), s.end(), '\\', '/');
    return lower(s);
}

constexpr uint32_t kCentralSig = 0x02014b50;
constexpr uint32_t kEocdSig = 0x06054b50;
constexpr uint32_t kLocalSig = 0x04034b50;

} // namespace

std::optional<std::vector<uint8_t>> inflateRaw(const uint8_t* src, size_t srcLen,
                                               size_t expectedOut) {
    std::vector<uint8_t> out(expectedOut ? expectedOut : srcLen * 4 + 64);

    z_stream zs{};
    // A negative window size selects raw deflate, which is what ZIP stores —
    // there is no zlib header on a ZIP member.
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return std::nullopt;

    zs.next_in = const_cast<Bytef*>(src);
    zs.avail_in = static_cast<uInt>(srcLen);
    zs.next_out = out.data();
    zs.avail_out = static_cast<uInt>(out.size());

    for (;;) {
        const int rc = inflate(&zs, Z_NO_FLUSH);
        if (rc == Z_STREAM_END) break;
        if (rc == Z_OK && zs.avail_out == 0) {
            // The declared size was wrong; grow and continue rather than fail.
            const size_t used = out.size();
            out.resize(used * 2);
            zs.next_out = out.data() + used;
            zs.avail_out = static_cast<uInt>(out.size() - used);
            continue;
        }
        if (rc != Z_OK) {
            inflateEnd(&zs);
            return std::nullopt;
        }
        if (zs.avail_in == 0) break;
    }

    out.resize(zs.total_out);
    inflateEnd(&zs);
    return out;
}

bool ZipReader::openFile(const std::string& path, std::string* err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (err) *err = "cannot open " + path;
        return false;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    return openBytes(std::move(bytes), err);
}

bool ZipReader::openBytes(std::vector<uint8_t> bytes, std::string* err) {
    data_ = std::move(bytes);
    entries_.clear();
    return parseCentralDirectory(err);
}

bool ZipReader::parseCentralDirectory(std::string* err) {
    if (data_.size() < 22) {
        if (err) *err = "too small to be a zip";
        return false;
    }

    // Find the end-of-central-directory record, scanning back over any
    // trailing comment.
    size_t eocd = 0;
    bool found = false;
    const size_t minPos = data_.size() > 65557 ? data_.size() - 65557 : 0;
    for (size_t i = data_.size() - 22 + 1; i-- > minPos;) {
        if (rd32(&data_[i]) == kEocdSig) {
            eocd = i;
            found = true;
            break;
        }
    }
    if (!found) {
        if (err) *err = "no end-of-central-directory record";
        return false;
    }

    const uint16_t count = rd16(&data_[eocd + 10]);
    const uint32_t cdOffset = rd32(&data_[eocd + 16]);
    if (cdOffset >= data_.size()) {
        if (err) *err = "central directory offset out of range";
        return false;
    }

    size_t p = cdOffset;
    for (uint16_t i = 0; i < count; ++i) {
        if (p + 46 > data_.size() || rd32(&data_[p]) != kCentralSig) {
            if (err) *err = "malformed central directory entry";
            return false;
        }
        ZipEntry e;
        e.method = rd16(&data_[p + 10]);
        e.crc32 = rd32(&data_[p + 16]);
        e.compressedSize = rd32(&data_[p + 20]);
        e.uncompressedSize = rd32(&data_[p + 24]);
        const uint16_t nameLen = rd16(&data_[p + 28]);
        const uint16_t extraLen = rd16(&data_[p + 30]);
        const uint16_t commentLen = rd16(&data_[p + 32]);
        e.localOffset = rd32(&data_[p + 42]);

        if (p + 46 + nameLen > data_.size()) {
            if (err) *err = "entry name out of range";
            return false;
        }
        e.name.assign(reinterpret_cast<const char*>(&data_[p + 46]), nameLen);
        entries_.push_back(std::move(e));
        p += 46u + nameLen + extraLen + commentLen;
    }
    return true;
}

bool ZipReader::has(const std::string& name) const { return findLoose(name) != nullptr; }

const ZipEntry* ZipReader::findLoose(const std::string& name) const {
    const std::string want = normalise(name);
    for (const auto& e : entries_)
        if (normalise(e.name) == want) return &e;
    // Fall back to a basename match: some writers nest description.xml inside
    // a folder, which the spec does not allow but which exists in the wild.
    for (const auto& e : entries_) {
        const std::string n = normalise(e.name);
        const size_t slash = n.rfind('/');
        if (slash != std::string::npos && n.substr(slash + 1) == want) return &e;
    }
    return nullptr;
}

std::optional<std::vector<uint8_t>> ZipReader::read(const std::string& name,
                                                    std::string* err) const {
    const ZipEntry* e = findLoose(name);
    if (!e) {
        if (err) *err = "no such entry: " + name;
        return std::nullopt;
    }
    if (e->localOffset + 30 > data_.size() || rd32(&data_[e->localOffset]) != kLocalSig) {
        if (err) *err = "bad local header for " + e->name;
        return std::nullopt;
    }

    const uint16_t nameLen = rd16(&data_[e->localOffset + 26]);
    const uint16_t extraLen = rd16(&data_[e->localOffset + 28]);
    const size_t dataAt = e->localOffset + 30u + nameLen + extraLen;
    if (dataAt + e->compressedSize > data_.size()) {
        if (err) *err = "entry data out of range: " + e->name;
        return std::nullopt;
    }

    if (e->method == 0) {
        return std::vector<uint8_t>(data_.begin() + static_cast<long>(dataAt),
                                    data_.begin() +
                                        static_cast<long>(dataAt + e->compressedSize));
    }
    if (e->method != 8) {
        if (err) *err = "unsupported compression method " + std::to_string(e->method);
        return std::nullopt;
    }

    auto out = inflateRaw(&data_[dataAt], e->compressedSize, e->uncompressedSize);
    if (!out && err) *err = "inflate failed for " + e->name;
    return out;
}

std::optional<std::string> ZipReader::readText(const std::string& name,
                                               std::string* err) const {
    auto bytes = read(name, err);
    if (!bytes) return std::nullopt;
    return std::string(bytes->begin(), bytes->end());
}

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------
std::optional<std::vector<uint8_t>> deflateRaw(const uint8_t* src, size_t srcLen,
                                               int level) {
    z_stream zs{};
    // Negative window bits: a raw stream with no zlib header, which is what
    // ZIP stores.
    if (deflateInit2(&zs, level, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return std::nullopt;

    std::vector<uint8_t> out(deflateBound(&zs, static_cast<uLong>(srcLen)) + 16);
    zs.next_in = const_cast<Bytef*>(src);
    zs.avail_in = static_cast<uInt>(srcLen);
    zs.next_out = out.data();
    zs.avail_out = static_cast<uInt>(out.size());

    const int rc = deflate(&zs, Z_FINISH);
    const size_t produced = out.size() - zs.avail_out;
    deflateEnd(&zs);
    if (rc != Z_STREAM_END) return std::nullopt;
    out.resize(produced);
    return out;
}

namespace {

void put16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xff));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xff));
}

void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xff));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xff));
    v.push_back(static_cast<uint8_t>((x >> 16) & 0xff));
    v.push_back(static_cast<uint8_t>((x >> 24) & 0xff));
}

void putBytes(std::vector<uint8_t>& v, const std::string& s) {
    v.insert(v.end(), s.begin(), s.end());
}

} // namespace

void ZipWriter::add(const std::string& name, std::vector<uint8_t> bytes, bool compress) {
    for (Pending& p : pending_) {
        if (p.name != name) continue;
        p.raw = std::move(bytes);
        p.compress = compress;
        return;
    }
    pending_.push_back({name, std::move(bytes), compress});
}

void ZipWriter::addText(const std::string& name, const std::string& text, bool compress) {
    add(name, std::vector<uint8_t>(text.begin(), text.end()), compress);
}

bool ZipWriter::has(const std::string& name) const {
    for (const Pending& p : pending_)
        if (p.name == name) return true;
    return false;
}

std::vector<uint8_t> ZipWriter::bytes() const {
    struct Written {
        std::string name;
        uint32_t crc = 0;
        uint32_t compressed = 0;
        uint32_t uncompressed = 0;
        uint16_t method = 0;
        uint32_t localOffset = 0;
    };

    std::vector<uint8_t> out;
    std::vector<Written> written;
    written.reserve(pending_.size());

    for (const Pending& p : pending_) {
        Written w;
        w.name = p.name;
        w.uncompressed = static_cast<uint32_t>(p.raw.size());
        w.crc = static_cast<uint32_t>(
            crc32(0L, p.raw.empty() ? nullptr : p.raw.data(),
                  static_cast<uInt>(p.raw.size())));
        w.localOffset = static_cast<uint32_t>(out.size());

        std::vector<uint8_t> payload;
        if (p.compress && !p.raw.empty()) {
            if (auto z = deflateRaw(p.raw.data(), p.raw.size())) {
                // Deflate on already-compressed data (an embedded .gdtf, a
                // PNG) grows it. Storing is both smaller and faster to read.
                if (z->size() < p.raw.size()) {
                    payload = std::move(*z);
                    w.method = 8;
                }
            }
        }
        if (w.method == 0) payload = p.raw;
        w.compressed = static_cast<uint32_t>(payload.size());

        put32(out, 0x04034b50);                             // local header
        put16(out, 20);                                     // version needed
        put16(out, 0);                                      // flags
        put16(out, w.method);
        put16(out, 0);                                      // mod time
        // A fixed 1980-01-01, so the same input always produces the same
        // archive. A real timestamp would make every export differ.
        put16(out, 0x0021);                                 // mod date
        put32(out, w.crc);
        put32(out, w.compressed);
        put32(out, w.uncompressed);
        put16(out, static_cast<uint16_t>(w.name.size()));
        put16(out, 0);                                      // extra length
        putBytes(out, w.name);
        out.insert(out.end(), payload.begin(), payload.end());

        written.push_back(std::move(w));
    }

    const uint32_t cdStart = static_cast<uint32_t>(out.size());
    for (const Written& w : written) {
        put32(out, 0x02014b50);                             // central directory
        put16(out, 20);                                     // version made by
        put16(out, 20);                                     // version needed
        put16(out, 0);                                      // flags
        put16(out, w.method);
        put16(out, 0);
        put16(out, 0x21);
        put32(out, w.crc);
        put32(out, w.compressed);
        put32(out, w.uncompressed);
        put16(out, static_cast<uint16_t>(w.name.size()));
        put16(out, 0);                                      // extra
        put16(out, 0);                                      // comment
        put16(out, 0);                                      // disk number
        put16(out, 0);                                      // internal attrs
        put32(out, 0);                                      // external attrs
        put32(out, w.localOffset);
        putBytes(out, w.name);
    }
    const uint32_t cdSize = static_cast<uint32_t>(out.size()) - cdStart;

    put32(out, 0x06054b50);                                 // end of central dir
    put16(out, 0);
    put16(out, 0);
    put16(out, static_cast<uint16_t>(written.size()));
    put16(out, static_cast<uint16_t>(written.size()));
    put32(out, cdSize);
    put32(out, cdStart);
    put16(out, 0);                                          // comment length
    return out;
}

bool ZipWriter::writeFile(const std::string& path, std::string* err) const {
    const std::vector<uint8_t> data = bytes();
    const std::string tmp = path + ".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) {
            if (err) *err = "cannot open " + tmp;
            return false;
        }
        if (!data.empty())
            o.write(reinterpret_cast<const char*>(data.data()),
                    static_cast<std::streamsize>(data.size()));
        o.flush();
        if (!o) {
            if (err) *err = "write failed: " + tmp;
            return false;
        }
    }
    // Replace the previous file only once a complete one exists.
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        if (err) *err = "rename failed: " + ec.message();
        return false;
    }
    return true;
}

} // namespace lumen
