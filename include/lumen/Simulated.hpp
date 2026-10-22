// Simulated.hpp — hardware stand-ins, so the whole console runs with nothing
// plugged in.
//
// Every one of these implements the SAME interface the real device will, so
// swapping in a real sACN socket, a Helios DAC or a MIDI port later changes
// which object is constructed and nothing else. That is the point: the parts
// that need hardware are absent, not stubbed out with special cases scattered
// through the engine.
//
// The laser one is worth noting: SimulatedLaser accepts a GatedFrame like any
// other DAC, which means the safety gate (§14.6) applies to the simulation
// exactly as it does to a projector. A simulated run that bypassed the gate
// would be worse than no simulation, because it would train the operator on
// behaviour the real thing does not have.
#pragma once

#include "lumen/Binding.hpp"
#include "lumen/Input.hpp"
#include "lumen/Laser.hpp"
#include "lumen/Output.hpp"

#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace lumen {

// ---------------------------------------------------------------------------
// DMX output
// ---------------------------------------------------------------------------
// Keeps the last frame per universe and counts what it was sent, so the UI can
// show exactly what would have gone down the wire.
class SimulatedOutput : public IOutput {
public:
    bool open(std::string* err) override;
    void send(int universe, const uint8_t* dmx, size_t len) override;
    const char* name() const override { return "Simulated"; }
    std::string describe() const override;

    struct Frame {
        int universe = 1;
        std::array<uint8_t, 512> data{};
        uint64_t frames = 0;
    };

    std::vector<Frame> frames() const;
    std::optional<Frame> frame(int universe) const;
    uint64_t totalFrames() const;
    // Highest channel in use anywhere, so a view can size itself to the rig
    // rather than always drawing 512 cells.
    int highestChannel(int universe) const;
    void reset();

private:
    mutable std::mutex m_;
    std::vector<Frame> frames_;
    uint64_t total_ = 0;
};

// ---------------------------------------------------------------------------
// Laser output
// ---------------------------------------------------------------------------
class SimulatedLaser : public ILaserOutput {
public:
    explicit SimulatedLaser(int pps = 30000) : pps_(pps) {}

    bool open(std::string* err) override;
    void send(const GatedFrame& frame) override;
    const char* name() const override { return "Simulated DAC"; }
    int pointsPerSecond() const override { return pps_; }
    LaserFault status() const override;

    // The last frame per zone, for drawing the figure on screen.
    std::optional<GatedFrame> lastFrame(int zone) const;
    std::vector<int> zones() const;
    uint64_t totalFrames() const { return total_; }
    uint64_t blankedFrames() const { return blanked_; }

    // Faults can be injected, which is the only way to exercise scan-fail
    // handling without unplugging a real projector mid-show.
    void injectFault(LaserFault f) { injected_ = injected_ | f; }
    void clearInjected() { injected_ = LaserFault::None; }

private:
    int pps_;
    std::vector<std::pair<int, GatedFrame>> last_;
    uint64_t total_ = 0;
    uint64_t blanked_ = 0;
    LaserFault injected_ = LaserFault::None;
};

// ---------------------------------------------------------------------------
// Input injection
// ---------------------------------------------------------------------------
// Stands in for a MIDI port, an OSC socket and an sACN listener. The events it
// produces are the same normalised ones a real transport pushes, so a binding
// learned from a simulated control works unchanged against the hardware.
class SimulatedInput {
public:
    // Pushes straight onto an engine's queue, exactly as a transport thread
    // would.
    static InputEvent midiNote(int note, bool down, int channel = 1);
    static InputEvent midiCC(int cc, float value, int channel = 1);
    static InputEvent osc(const std::string& address, float value);
    static InputEvent key(int code, Mods mods = Mods::None);

    // A simulated desk sending one universe, for exercising §18.3's
    // arbitration without a second console in the room.
    static std::vector<uint8_t> sacnFrame(const std::string& sourceName,
                                          uint8_t priority, uint8_t sequence,
                                          int universe,
                                          const std::array<uint8_t, 512>& data);
};

} // namespace lumen
