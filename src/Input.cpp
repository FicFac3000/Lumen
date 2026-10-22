#include "lumen/Input.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace lumen {

const char* inputProtocolName(InputProtocol p) {
    switch (p) {
        case InputProtocol::Sacn: return "sACN";
        case InputProtocol::ArtNet: return "Art-Net";
        case InputProtocol::Interface: return "interface";
    }
    return "sACN";
}

const char* tieBreakName(TieBreak t) {
    switch (t) {
        case TieBreak::Htp: return "HTP";
        case TieBreak::Oldest: return "oldest";
        case TieBreak::Latest: return "latest";
    }
    return "HTP";
}

namespace {

uint16_t rd16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

uint32_t rd32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

} // namespace

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------
std::optional<SacnFrame> parseSacnPacket(const uint8_t* b, size_t len) {
    // Enough to reach the DMP layer's start code. A short packet is not a
    // malformed sACN packet, it is something else on port 5568.
    if (!b || len < kSacnDataOffset) return std::nullopt;

    if (rd16(b) != 0x0010 || rd16(b + 2) != 0x0000) return std::nullopt;
    static const char kAcnId[12] = {'A', 'S', 'C', '-', 'E', '1', '.', '1', '7',
                                    0,   0,   0};
    if (std::memcmp(b + 4, kAcnId, 12) != 0) return std::nullopt;
    if (rd32(b + 18) != 0x00000004) return std::nullopt;   // VECTOR_ROOT_E131_DATA
    if (rd32(b + 40) != 0x00000002) return std::nullopt;   // VECTOR_E131_DATA_PACKET
    if (b[117] != 0x02 || b[118] != 0xa1) return std::nullopt;
    // A non-zero start code is not DMX512 data — RDM and other start codes
    // share the transport and must not be merged in as levels.
    if (b[125] != 0x00) return std::nullopt;

    SacnFrame f;
    std::memcpy(f.cid.data(), b + 22, 16);
    // The source name is a 64-byte field, null-padded but not guaranteed
    // null-terminated.
    const char* name = reinterpret_cast<const char*>(b + 44);
    size_t nameLen = 0;
    while (nameLen < 64 && name[nameLen] != '\0') ++nameLen;
    f.sourceName.assign(name, nameLen);

    f.priority = b[108];
    f.syncAddress = rd16(b + 109);
    f.sequence = b[111];
    const uint8_t options = b[112];
    f.preview = (options & 0x80) != 0;
    f.terminated = (options & 0x40) != 0;
    f.forceSync = (options & 0x20) != 0;
    f.universe = rd16(b + 113);

    // The property value count includes the start code, so the DMX slot count
    // is one fewer. Trust it over the packet size: a sender may send fewer
    // than 512 slots.
    const uint16_t propCount = rd16(b + 123);
    size_t slots = propCount > 0 ? static_cast<size_t>(propCount - 1) : 0;
    slots = std::min<size_t>(slots, 512);
    slots = std::min<size_t>(slots, len - kSacnDataOffset);
    f.len = slots;
    if (slots) std::memcpy(f.data.data(), b + kSacnDataOffset, slots);
    return f;
}

std::optional<ArtDmxFrame> parseArtDmxPacket(const uint8_t* b, size_t len) {
    if (!b || len < kArtDmxHeaderSize) return std::nullopt;
    static const char kId[8] = {'A', 'r', 't', '-', 'N', 'e', 't', 0};
    if (std::memcmp(b, kId, 8) != 0) return std::nullopt;
    // OpCode is little-endian in Art-Net, unlike everything else in it.
    const uint16_t opcode = static_cast<uint16_t>(b[8] | (b[9] << 8));
    if (opcode != 0x5000) return std::nullopt;   // OpDmx

    ArtDmxFrame f;
    f.sequence = b[12];
    f.physical = b[13];
    f.portAddress = static_cast<uint16_t>(b[14] | (b[15] << 8));
    const uint16_t declared = rd16(b + 16);   // length IS big-endian here
    size_t slots = std::min<size_t>(declared, 512);
    slots = std::min<size_t>(slots, len - kArtDmxHeaderSize);
    f.len = slots;
    if (slots) std::memcpy(f.data.data(), b + kArtDmxHeaderSize, slots);
    return f;
}

std::array<uint8_t, 16> synthesiseCid(const std::string& origin, int universe) {
    // Stable for a given origin and universe, so the same node is recognised
    // across packets without carrying a real CID.
    std::array<uint8_t, 16> out{};
    uint64_t h = 0xcbf29ce484222325ull;
    auto mix = [&h](uint8_t byte) {
        h ^= byte;
        h *= 0x100000001b3ull;
    };
    for (char c : origin) mix(static_cast<uint8_t>(c));
    mix(static_cast<uint8_t>(universe & 0xff));
    mix(static_cast<uint8_t>((universe >> 8) & 0xff));
    for (int i = 0; i < 8; ++i) {
        out[static_cast<size_t>(i)] = static_cast<uint8_t>(h >> (8 * i));
        // The top half repeats the hash with a different seed, so two origins
        // differing late in the string still differ here.
        out[static_cast<size_t>(8 + i)] = static_cast<uint8_t>((h * 0x9e3779b9ull) >>
                                                               (8 * i));
    }
    return out;
}

// ---------------------------------------------------------------------------
// SourceInfo
// ---------------------------------------------------------------------------
std::string SourceInfo::cidText() const {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%02x%02x%02x%02x-%02x%02x-%02x%02x", cid[0],
                  cid[1], cid[2], cid[3], cid[4], cid[5], cid[6], cid[7]);
    return buf;
}

bool SourceInfo::sameAs(const SourceInfo& o) const {
    return universe == o.universe && cid == o.cid;
}

// ---------------------------------------------------------------------------
// DmxInput
// ---------------------------------------------------------------------------
UniverseConfig& DmxInput::config(int universe) {
    for (auto& [u, c] : configs_)
        if (u == universe) return c;
    configs_.push_back({universe, defaults_});
    return configs_.back().second;
}

const UniverseConfig& DmxInput::config(int universe) const {
    for (const auto& [u, c] : configs_)
        if (u == universe) return c;
    return defaults_;
}

DmxInput::Stream* DmxInput::findStream(const std::array<uint8_t, 16>& cid,
                                       int universe) {
    for (Stream& s : streams_)
        if (s.info.universe == universe && s.info.cid == cid) return &s;
    return nullptr;
}

bool DmxInput::accept(Stream& s, const uint8_t* dmx, size_t len, uint8_t sequence,
                      bool hasSequence) {
    ++packets_;
    ++s.info.packets;
    const double t = now();
    if (!s.info.everSeen) {
        s.info.firstSeen = t;
        s.info.everSeen = true;
    }

    if (hasSequence && s.hasSequence) {
        // E1.31 4.1: discard a packet whose sequence is behind the last by up
        // to 20. Out-of-order UDP must not make levels jump backwards, but a
        // sender that restarts its counter has to be followed rather than
        // locked out forever.
        const int diff = static_cast<int>(
            static_cast<int8_t>(static_cast<uint8_t>(sequence - s.info.lastSequence)));
        if (diff <= 0 && diff > -20) {
            ++s.info.outOfOrder;
            ++s.info.rejected;
            ++rejected_;
            return false;
        }
    }
    s.info.lastSequence = sequence;
    if (hasSequence) s.hasSequence = true;

    s.info.lastSeen = t;
    const size_t n = std::min<size_t>(len, 512);
    // A sender sending fewer slots does not zero the rest: the slots it did
    // not send are simply not its business.
    if (dmx && n) std::memcpy(s.data.data(), dmx, n);
    s.len = std::max(s.len, n);
    return true;
}

bool DmxInput::receiveSacn(const SacnFrame& f, const std::string& origin) {
    const int universe = static_cast<int>(f.universe);
    if (universe <= 0) {
        ++rejected_;
        return false;
    }
    const UniverseConfig& cfg = config(universe);
    if (!cfg.enabled) {
        ++rejected_;
        return false;
    }

    Stream* s = findStream(f.cid, universe);
    if (!s) {
        Stream made;
        made.info.cid = f.cid;
        made.info.universe = universe;
        made.info.protocol = InputProtocol::Sacn;
        made.info.firstSeen = now();
        streams_.push_back(std::move(made));
        s = &streams_.back();
    }
    s->info.name = f.sourceName;
    s->info.origin = origin;
    s->info.priority = f.priority;
    s->info.preview = f.preview;

    // A terminate is a statement, not a packet to merge: the source is
    // releasing the universe deliberately, so it stops contributing at once
    // rather than after the hold time.
    if (f.terminated) {
        s->info.terminated = true;
        if (!s->info.everSeen) {
            s->info.firstSeen = now();
            s->info.everSeen = true;
        }
        s->info.lastSeen = now();
        ++s->info.packets;
        ++packets_;
        rebuild(universe);
        return true;
    }
    s->info.terminated = false;

    const bool took = accept(*s, f.data.data(), f.len, f.sequence, true);
    rebuild(universe);
    return took;
}

bool DmxInput::receiveArtNet(const ArtDmxFrame& f, int universe,
                             const std::string& origin) {
    if (universe <= 0) {
        ++rejected_;
        return false;
    }
    const UniverseConfig& cfg = config(universe);
    if (!cfg.enabled) {
        ++rejected_;
        return false;
    }

    const auto cid = synthesiseCid("artnet:" + origin, universe);
    Stream* s = findStream(cid, universe);
    if (!s) {
        Stream made;
        made.info.cid = cid;
        made.info.universe = universe;
        made.info.protocol = InputProtocol::ArtNet;
        made.info.firstSeen = now();
        streams_.push_back(std::move(made));
        s = &streams_.back();
    }
    s->info.origin = origin;
    if (s->info.name.empty())
        s->info.name = "Art-Net " + std::to_string(f.portAddress);
    // Art-Net has no priority field, so the universe's configured value
    // stands in. Without this an Art-Net source could never be arbitrated
    // against an sACN one at all.
    s->info.priority = cfg.artNetPriority;
    s->info.terminated = false;
    s->info.preview = false;

    const bool took = accept(*s, f.data.data(), f.len, f.sequence, true);
    rebuild(universe);
    return took;
}

bool DmxInput::receiveRaw(int universe, const uint8_t* dmx, size_t len,
                          const std::string& origin, uint8_t priority) {
    if (universe <= 0) {
        ++rejected_;
        return false;
    }
    if (!config(universe).enabled) {
        ++rejected_;
        return false;
    }

    const auto cid = synthesiseCid("iface:" + origin, universe);
    Stream* s = findStream(cid, universe);
    if (!s) {
        Stream made;
        made.info.cid = cid;
        made.info.universe = universe;
        made.info.protocol = InputProtocol::Interface;
        made.info.name = origin;
        made.info.firstSeen = now();
        streams_.push_back(std::move(made));
        s = &streams_.back();
    }
    s->info.origin = origin;
    s->info.priority = priority;
    s->info.terminated = false;

    // No sequence numbers on a direct interface: there is no network to
    // reorder them.
    const bool took = accept(*s, dmx, len, 0, false);
    rebuild(universe);
    return took;
}

void DmxInput::rebuild(int universe) {
    const UniverseConfig& cfg = config(universe);
    const double t = now();

    // Which streams are live right now.
    std::vector<Stream*> live;
    uint8_t top = 0;
    for (Stream& s : streams_) {
        s.info.active = false;
        s.info.winning = false;
        if (s.info.universe != universe) continue;
        if (s.info.terminated) continue;
        if (cfg.ignorePreview && s.info.preview) continue;
        if (!s.info.everSeen) continue;
        if (t - s.info.lastSeen > cfg.hold) continue;
        s.info.active = true;
        live.push_back(&s);
        top = std::max(top, s.info.priority);
    }

    Universe* out = nullptr;
    for (Universe& u : merged_)
        if (u.number == universe) out = &u;

    if (live.empty()) {
        // Nothing live: the universe stops existing rather than freezing on
        // its last values. A stale universe that looks live is how a dead
        // backup goes unnoticed.
        if (out) {
            for (size_t i = 0; i < merged_.size(); ++i)
                if (merged_[i].number == universe) {
                    merged_.erase(merged_.begin() + static_cast<long>(i));
                    break;
                }
        }
        return;
    }

    if (!out) {
        Universe made;
        made.number = universe;
        merged_.push_back(made);
        out = &merged_.back();
    }
    out->data.fill(0);

    // Only the top priority contributes. This is the rule that makes a
    // backup desk at a lower priority invisible until the main one stops.
    std::vector<Stream*> winners;
    for (Stream* s : live)
        if (s->info.priority == top) winners.push_back(s);

    if (winners.size() > 1 && cfg.tie != TieBreak::Htp) {
        // One source owns the universe. Deterministic either way, so the
        // winner does not flip frame to frame with packet arrival order.
        std::sort(winners.begin(), winners.end(), [&](Stream* a, Stream* b) {
            if (cfg.tie == TieBreak::Oldest) {
                if (a->info.firstSeen != b->info.firstSeen)
                    return a->info.firstSeen < b->info.firstSeen;
            } else {
                if (a->info.firstSeen != b->info.firstSeen)
                    return a->info.firstSeen > b->info.firstSeen;
            }
            // Equal timestamps happen under a driven clock in tests and on a
            // coarse clock in the field; the CID breaks it so the result is
            // still deterministic.
            return a->info.cid < b->info.cid;
        });
        winners.resize(1);
    }

    for (Stream* s : winners) {
        s->info.winning = true;
        for (size_t i = 0; i < s->len; ++i)
            out->data[i] = std::max(out->data[i], s->data[i]);
    }
}

const Universe* DmxInput::universe(int number) const {
    for (const Universe& u : merged_)
        if (u.number == number) return &u;
    return nullptr;
}

std::vector<int> DmxInput::universes() const {
    std::vector<int> out;
    for (const Universe& u : merged_) out.push_back(u.number);
    std::sort(out.begin(), out.end());
    return out;
}

std::optional<uint8_t> DmxInput::slot(int universe, int channel) const {
    if (channel < 1 || channel > 512) return std::nullopt;
    const Universe* u = this->universe(universe);
    if (!u) return std::nullopt;
    return u->data[static_cast<size_t>(channel - 1)];
}

MergeReport DmxInput::report(int universe) const {
    MergeReport r;
    r.universe = universe;
    const UniverseConfig& cfg = config(universe);
    r.tie = cfg.tie;
    int contributing = 0;
    for (const Stream& s : streams_) {
        if (s.info.universe != universe) continue;
        ++r.sourcesSeen;
        if (s.info.active) ++r.sourcesActive;
        if (!s.info.winning) continue;
        ++contributing;
        r.winningPriority = s.info.priority;
        r.contributors.push_back(s.info.name.empty() ? s.info.cidText() : s.info.name);
    }
    r.merged = contributing > 1;
    return r;
}

std::vector<SourceInfo> DmxInput::sources(int universe) const {
    std::vector<SourceInfo> out;
    for (const Stream& s : streams_)
        if (universe == 0 || s.info.universe == universe) out.push_back(s.info);
    // Newest activity first: the thing someone is diagnosing is usually the
    // source that just appeared.
    std::sort(out.begin(), out.end(), [](const SourceInfo& a, const SourceInfo& b) {
        if (a.lastSeen != b.lastSeen) return a.lastSeen > b.lastSeen;
        return a.cid < b.cid;
    });
    return out;
}

int DmxInput::expire() {
    const double t = now();
    int expired = 0;
    std::vector<int> touched;
    for (Stream& s : streams_) {
        if (!s.info.everSeen || s.info.terminated) continue;
        if (t - s.info.lastSeen <= config(s.info.universe).hold) continue;
        if (!s.info.active) continue;
        ++expired;
        if (std::find(touched.begin(), touched.end(), s.info.universe) == touched.end())
            touched.push_back(s.info.universe);
    }
    // Rebuild whether or not anything expired on this pass, because a
    // universe whose only source went quiet has to lose its output.
    for (const Stream& s : streams_)
        if (std::find(touched.begin(), touched.end(), s.info.universe) == touched.end())
            touched.push_back(s.info.universe);
    for (int u : touched) rebuild(u);
    return expired;
}

bool DmxInput::forget(const std::array<uint8_t, 16>& cid, int universe) {
    for (size_t i = 0; i < streams_.size(); ++i) {
        if (streams_[i].info.universe != universe || streams_[i].info.cid != cid)
            continue;
        streams_.erase(streams_.begin() + static_cast<long>(i));
        rebuild(universe);
        return true;
    }
    return false;
}

void DmxInput::clear() {
    streams_.clear();
    merged_.clear();
    packets_ = 0;
    rejected_ = 0;
}

} // namespace lumen
