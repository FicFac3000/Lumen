// Input.hpp — DMX input and source arbitration (SPEC §18.3, §12.7).
//
// The receive counterpart to Output.hpp. §12.7 flags that the engine currently
// only transmits, and §18.3 flags arbitration as "the part that is easy to get
// wrong" — which is this file's whole reason to exist.
//
// THE PROBLEM. Two desks can send the same universe at once. That is not a
// fault to reject: it is how a tracking backup works (§18.8), how a remote
// console merges in, and how a house-light interlock reaches the desk. So the
// rules have to be explicit:
//
//   1. E1.31 carries a PRIORITY field. The highest priority present wins —
//      resolved by priority, never by arrival order. A backup desk sending at
//      priority 50 is invisible while the main desk sends at 100, and takes
//      over the moment the main desk stops, with no switchover logic.
//   2. Where priorities TIE, the universe's tie-break decides: HTP merge per
//      slot, or one source owns the universe outright.
//   3. A source that goes quiet releases its slots after a hold time. E1.31's
//      network data loss timeout is 2.5 seconds, and that is the default.
//   4. Every sender seen is tracked with its CID, priority and last-seen time,
//      because the first question when two desks fight over a universe is
//      "who is actually sending?".
//
// Art-Net carries neither a CID nor a priority, so an identity is synthesised
// from where the packet came from and a configurable default priority applies.
// Mixing the two protocols on one universe therefore works, but the Art-Net
// side cannot express priority — worth knowing rather than discovering.
#pragma once

#include "lumen/Output.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace lumen {

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------
struct SacnFrame {
    std::array<uint8_t, 16> cid{};
    std::string sourceName;
    uint8_t priority = 100;
    uint8_t sequence = 0;
    uint16_t universe = 0;
    uint16_t syncAddress = 0;
    bool preview = false;       // options bit 7: preview data, not live output
    bool terminated = false;    // options bit 6: stream terminated
    bool forceSync = false;
    std::array<uint8_t, 512> data{};
    size_t len = 0;
};

struct ArtDmxFrame {
    uint8_t sequence = 0;
    uint8_t physical = 0;
    uint16_t portAddress = 0;
    std::array<uint8_t, 512> data{};
    size_t len = 0;
};

// Both return nullopt on anything that is not a valid packet of that kind,
// rather than guessing — a malformed packet on a show network is more likely
// to be a different protocol than a corrupted one.
std::optional<SacnFrame> parseSacnPacket(const uint8_t* bytes, size_t len);
std::optional<ArtDmxFrame> parseArtDmxPacket(const uint8_t* bytes, size_t len);

// ---------------------------------------------------------------------------
// Sources
// ---------------------------------------------------------------------------
enum class InputProtocol : uint8_t { Sacn, ArtNet, Interface };

const char* inputProtocolName(InputProtocol p);

struct SourceInfo {
    std::array<uint8_t, 16> cid{};
    std::string name;          // the sACN source name, when it gave one
    std::string origin;        // where it came from: an IP, or a device
    InputProtocol protocol = InputProtocol::Sacn;
    int universe = 1;
    uint8_t priority = 100;

    // A separate flag rather than treating a zero timestamp as "never": zero
    // is a perfectly ordinary clock reading, and a source arriving at clock
    // zero was silently never activated.
    bool everSeen = false;
    double firstSeen = 0.0;
    double lastSeen = 0.0;
    uint64_t packets = 0;
    uint8_t lastSequence = 0;
    uint64_t outOfOrder = 0;   // sequence went backwards (E1.31 4.1)
    uint64_t rejected = 0;     // dropped for any reason
    bool terminated = false;
    bool preview = false;

    // Set while this source is contributing to the merged output. The answer
    // to "why is my desk not getting through".
    bool active = false;
    bool winning = false;

    std::string cidText() const;
    // The identity two packets are judged the same source by.
    bool sameAs(const SourceInfo& o) const;
};

// ---------------------------------------------------------------------------
// Arbitration
// ---------------------------------------------------------------------------
// What happens when two sources tie on priority.
enum class TieBreak : uint8_t {
    Htp,        // merge per slot, highest level wins — the safe default
    Oldest,     // the source that has been sending longest owns the universe
    Latest,     // the most recent arrival owns it — for remote control
};

const char* tieBreakName(TieBreak t);

struct UniverseConfig {
    // E1.31's network data loss timeout. A source going quiet holds its slots
    // for this long before releasing them, which is what makes failover
    // automatic rather than a feature.
    double hold = 2.5;
    TieBreak tie = TieBreak::Htp;
    // Preview data is explicitly not live output, so it is ignored by default.
    bool ignorePreview = true;
    // Art-Net cannot express priority, so it is given one.
    uint8_t artNetPriority = 100;
    bool enabled = true;
};

// Why a universe's output looks the way it does, for the diagnostic page
// §18.3 asks for.
struct MergeReport {
    int universe = 1;
    int sourcesSeen = 0;
    int sourcesActive = 0;      // within the hold time, not terminated
    uint8_t winningPriority = 0;
    TieBreak tie = TieBreak::Htp;
    bool merged = false;        // more than one source contributed
    std::vector<std::string> contributors;
};

class DmxInput {
public:
    DmxInput() = default;

    void setClock(std::function<double()> clock) { clock_ = std::move(clock); }
    double now() const { return clock_ ? clock_() : 0.0; }

    UniverseConfig& config(int universe);
    const UniverseConfig& config(int universe) const;
    void setDefaults(const UniverseConfig& c) { defaults_ = c; }

    // --- feeding -----------------------------------------------------------
    // Each returns false when the packet was rejected, with the reason
    // counted against the source so a diagnostic page can show it.
    bool receiveSacn(const SacnFrame& f, const std::string& origin);
    bool receiveArtNet(const ArtDmxFrame& f, int universe, const std::string& origin);
    // A USB interface or any other source with no protocol identity of its
    // own, which therefore cannot express priority either.
    bool receiveRaw(int universe, const uint8_t* dmx, size_t len,
                    const std::string& origin, uint8_t priority = 100);

    // --- results -----------------------------------------------------------
    // The merged universe, or null when nothing live is arriving for it.
    const Universe* universe(int number) const;
    std::vector<int> universes() const;
    // One slot, 1-based, or nullopt when nothing is driving it.
    std::optional<uint8_t> slot(int universe, int channel) const;

    MergeReport report(int universe) const;
    // Every sender seen, newest activity first. Universe 0 means all of them.
    std::vector<SourceInfo> sources(int universe = 0) const;

    // Releases slots held by sources that have gone quiet. Called from the
    // engine's tick; returns how many sources expired.
    int expire();

    // Forgets a source entirely, for a diagnostic page's "clear" action. A
    // terminated source is kept until this is called, because knowing a desk
    // sent a terminate is itself diagnostic.
    bool forget(const std::array<uint8_t, 16>& cid, int universe);
    void clear();

    uint64_t packets() const { return packets_; }
    uint64_t rejected() const { return rejected_; }

private:
    struct Stream {
        SourceInfo info;
        std::array<uint8_t, 512> data{};
        size_t len = 0;
        bool hasSequence = false;
    };

    Stream* findStream(const std::array<uint8_t, 16>& cid, int universe);
    bool accept(Stream& s, const uint8_t* dmx, size_t len, uint8_t sequence,
                bool hasSequence);
    void rebuild(int universe);

    std::function<double()> clock_;
    std::deque<Stream> streams_;
    std::vector<Universe> merged_;
    UniverseConfig defaults_;
    std::vector<std::pair<int, UniverseConfig>> configs_;
    uint64_t packets_ = 0;
    uint64_t rejected_ = 0;
};

// Synthesises a stable 16-byte identity for a source that has none of its own,
// so Art-Net and interface inputs can be tracked like sACN ones.
std::array<uint8_t, 16> synthesiseCid(const std::string& origin, int universe);

} // namespace lumen
