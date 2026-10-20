// Output.hpp : sACN (ANSI E1.31) and Art-Net DMX transmitters.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace lumen {

// A rendered DMX universe. 1-based numbering, matching how consoles and nodes
// address them.
struct Universe {
    int number = 1;
    std::array<uint8_t, 512> data{};
};

// sACN packet geometry (ANSI E1.31-2018, DMP layer, 512 slots).
inline constexpr size_t kSacnRootOffset = 16;
inline constexpr size_t kSacnFramingOffset = 38;
inline constexpr size_t kSacnDmpOffset = 115;
inline constexpr size_t kSacnDataOffset = 126;
inline constexpr size_t kSacnPacketSize = 638;

// Art-Net ArtDmx geometry.
inline constexpr size_t kArtDmxHeaderSize = 18;
inline constexpr size_t kArtDmxPacketSize = kArtDmxHeaderSize + 512;

std::vector<uint8_t> buildSacnPacket(const std::array<uint8_t, 16>& cid,
                                     const std::string& sourceName,
                                     uint8_t priority,
                                     uint8_t sequence,
                                     uint16_t universe,
                                     const uint8_t* dmx,
                                     size_t len);

std::vector<uint8_t> buildArtDmxPacket(uint8_t sequence,
                                       uint8_t physical,
                                       uint16_t portAddress,
                                       const uint8_t* dmx,
                                       size_t len);

std::array<uint8_t, 16> makeCid(const std::string& seed);

class IOutput {
public:
    virtual ~IOutput() = default;
    virtual bool open(std::string* err) = 0;
    virtual void send(int universe, const uint8_t* dmx, size_t len) = 0;
    virtual const char* name() const = 0;
    virtual std::string describe() const = 0;
};

// Multicast sACN to 239.255.<uni-hi>.<uni-lo>:5568.
class SacnOutput : public IOutput {
public:
    explicit SacnOutput(std::string sourceName = "Lumen", uint8_t priority = 100);
    ~SacnOutput() override;

    bool open(std::string* err) override;
    void send(int universe, const uint8_t* dmx, size_t len) override;
    const char* name() const override { return "sACN"; }
    std::string describe() const override;

    void setUnicast(const std::string& ip) { unicast_ = ip; }

private:
    int fd_ = -1;
    std::string sourceName_;
    uint8_t priority_;
    std::array<uint8_t, 16> cid_{};
    std::unordered_map<int, uint8_t> seq_;
    std::string unicast_;
};

// Art-Net over UDP/6454, broadcast by default.
class ArtNetOutput : public IOutput {
public:
    explicit ArtNetOutput(std::string targetIp = "255.255.255.255");
    ~ArtNetOutput() override;

    bool open(std::string* err) override;
    void send(int universe, const uint8_t* dmx, size_t len) override;
    const char* name() const override { return "Art-Net"; }
    std::string describe() const override;

    // Maps engine universe N -> Art-Net port address (net/subnet/universe).
    void setPortAddressBase(uint16_t base) { base_ = base; }

private:
    int fd_ = -1;
    std::string target_;
    uint16_t base_ = 0;
    std::unordered_map<int, uint8_t> seq_;
};

} // namespace lumen
