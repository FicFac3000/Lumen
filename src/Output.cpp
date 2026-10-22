#include "lumen/Output.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

namespace lumen {

namespace {
void put16(std::vector<uint8_t>& b, size_t at, uint16_t v) {
    b[at] = static_cast<uint8_t>(v >> 8);
    b[at + 1] = static_cast<uint8_t>(v & 0xff);
}
void put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    b[at] = static_cast<uint8_t>(v >> 24);
    b[at + 1] = static_cast<uint8_t>((v >> 16) & 0xff);
    b[at + 2] = static_cast<uint8_t>((v >> 8) & 0xff);
    b[at + 3] = static_cast<uint8_t>(v & 0xff);
}
uint16_t flagsAndLength(size_t pduLength) {
    return static_cast<uint16_t>(0x7000u | (pduLength & 0x0fffu));
}
} // namespace

std::array<uint8_t, 16> makeCid(const std::string& seed) {
    // Deterministic RFC-4122-shaped CID so a given install keeps its identity.
    std::array<uint8_t, 16> cid{};
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char c : seed) {
        h ^= c;
        h *= 0x100000001b3ull;
    }
    for (size_t i = 0; i < 16; ++i) {
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdull;
        cid[i] = static_cast<uint8_t>(h >> 56);
    }
    cid[6] = static_cast<uint8_t>((cid[6] & 0x0f) | 0x40); // version 4
    cid[8] = static_cast<uint8_t>((cid[8] & 0x3f) | 0x80); // variant 1
    return cid;
}

std::vector<uint8_t> buildSacnPacket(const std::array<uint8_t, 16>& cid,
                                     const std::string& sourceName,
                                     uint8_t priority,
                                     uint8_t sequence,
                                     uint16_t universe,
                                     const uint8_t* dmx,
                                     size_t len) {
    // The packet is sized to the slots it was actually given. Declaring 513
    // properties while being handed 24 slots would send 488 zeros the caller
    // never asked for — which on a universe two senders share by range would
    // black out the other one's channels.
    const size_t slots = std::min<size_t>(len, 512);
    const size_t packetSize = kSacnDataOffset + slots;
    std::vector<uint8_t> b(packetSize, 0);

    // --- Root layer ---------------------------------------------------------
    put16(b, 0, 0x0010);                       // preamble size
    put16(b, 2, 0x0000);                       // postamble size
    static const char kAcnId[12] = {'A', 'S', 'C', '-', 'E', '1', '.', '1', '7', 0, 0, 0};
    std::memcpy(b.data() + 4, kAcnId, 12);
    put16(b, 16, flagsAndLength(packetSize - kSacnRootOffset));
    put32(b, 18, 0x00000004);                  // VECTOR_ROOT_E131_DATA
    std::memcpy(b.data() + 22, cid.data(), 16);

    // --- Framing layer ------------------------------------------------------
    put16(b, 38, flagsAndLength(packetSize - kSacnFramingOffset));
    put32(b, 40, 0x00000002);                  // VECTOR_E131_DATA_PACKET
    const size_t nameLen = std::min<size_t>(sourceName.size(), 63);
    std::memcpy(b.data() + 44, sourceName.data(), nameLen);
    b[108] = priority;
    put16(b, 109, 0);                          // synchronization address
    b[111] = sequence;
    b[112] = 0;                                // options
    put16(b, 113, universe);

    // --- DMP layer ----------------------------------------------------------
    put16(b, 115, flagsAndLength(packetSize - kSacnDmpOffset));
    b[117] = 0x02;                             // VECTOR_DMP_SET_PROPERTY
    b[118] = 0xa1;                             // address & data type
    put16(b, 119, 0x0000);                     // first property address
    put16(b, 121, 0x0001);                     // address increment
    // The count includes the start code, so it is slots + 1.
    put16(b, 123, static_cast<uint16_t>(slots + 1));
    b[125] = 0x00;                             // DMX start code

    if (dmx && slots) std::memcpy(b.data() + kSacnDataOffset, dmx, slots);
    return b;
}

std::vector<uint8_t> buildArtDmxPacket(uint8_t sequence,
                                       uint8_t physical,
                                       uint16_t portAddress,
                                       const uint8_t* dmx,
                                       size_t len) {
    std::vector<uint8_t> b(kArtDmxPacketSize, 0);
    static const char kId[8] = {'A', 'r', 't', '-', 'N', 'e', 't', 0};
    std::memcpy(b.data(), kId, 8);
    b[8] = 0x00;                               // OpDmx low byte
    b[9] = 0x50;                               // OpDmx high byte (0x5000, LE)
    b[10] = 0;                                 // ProtVerHi
    b[11] = 14;                                // ProtVerLo
    b[12] = sequence;
    b[13] = physical;
    b[14] = static_cast<uint8_t>(portAddress & 0xff);   // SubUni
    b[15] = static_cast<uint8_t>((portAddress >> 8) & 0x7f); // Net
    b[16] = static_cast<uint8_t>(512 >> 8);    // LengthHi
    b[17] = static_cast<uint8_t>(512 & 0xff);  // LengthLo

    const size_t n = std::min<size_t>(len, 512);
    if (dmx && n) std::memcpy(b.data() + kArtDmxHeaderSize, dmx, n);
    return b;
}

// ---------------------------------------------------------------------------
// SacnOutput
// ---------------------------------------------------------------------------
SacnOutput::SacnOutput(std::string sourceName, uint8_t priority)
    : sourceName_(std::move(sourceName)), priority_(priority) {
    cid_ = makeCid(sourceName_ + ":lumen");
}

SacnOutput::~SacnOutput() {
    if (fd_ >= 0) ::close(fd_);
}

bool SacnOutput::open(std::string* err) {
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) {
        if (err) *err = "sACN: socket() failed";
        return false;
    }
    const unsigned char ttl = 16;
    ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    const int loop = 1;
    ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
    return true;
}

void SacnOutput::send(int universe, const uint8_t* dmx, size_t len) {
    if (fd_ < 0 || universe < 1 || universe > 63999) return;
    const uint8_t seq = seq_[universe]++;
    const auto pkt = buildSacnPacket(cid_, sourceName_, priority_, seq,
                                     static_cast<uint16_t>(universe), dmx, len);

    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(5568);
    if (!unicast_.empty()) {
        ::inet_pton(AF_INET, unicast_.c_str(), &to.sin_addr);
    } else {
        const uint32_t addr = (239u << 24) | (255u << 16) |
                              (static_cast<uint32_t>((universe >> 8) & 0xff) << 8) |
                              static_cast<uint32_t>(universe & 0xff);
        to.sin_addr.s_addr = htonl(addr);
    }
    ::sendto(fd_, pkt.data(), pkt.size(), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
}

std::string SacnOutput::describe() const {
    return unicast_.empty() ? std::string("multicast 239.255.x.y:5568")
                            : ("unicast " + unicast_ + ":5568");
}

// ---------------------------------------------------------------------------
// ArtNetOutput
// ---------------------------------------------------------------------------
ArtNetOutput::ArtNetOutput(std::string targetIp) : target_(std::move(targetIp)) {}

ArtNetOutput::~ArtNetOutput() {
    if (fd_ >= 0) ::close(fd_);
}

bool ArtNetOutput::open(std::string* err) {
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) {
        if (err) *err = "Art-Net: socket() failed";
        return false;
    }
    const int yes = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    return true;
}

void ArtNetOutput::send(int universe, const uint8_t* dmx, size_t len) {
    if (fd_ < 0 || universe < 1) return;
    // Art-Net sequence runs 1..255; 0 means "sequencing disabled".
    uint8_t& s = seq_[universe];
    s = (s >= 255) ? 1 : static_cast<uint8_t>(s + 1);
    const uint8_t seq = s;
    const auto port = static_cast<uint16_t>(base_ + (universe - 1));
    const auto pkt = buildArtDmxPacket(seq, 0, port, dmx, len);

    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(6454);
    ::inet_pton(AF_INET, target_.c_str(), &to.sin_addr);
    ::sendto(fd_, pkt.data(), pkt.size(), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
}

std::string ArtNetOutput::describe() const { return target_ + ":6454"; }

} // namespace lumen
