#include "lumen/Simulated.hpp"

#include <algorithm>
#include <cstring>

namespace lumen {

// ---------------------------------------------------------------------------
// SimulatedOutput
// ---------------------------------------------------------------------------
bool SimulatedOutput::open(std::string*) { return true; }

void SimulatedOutput::send(int universe, const uint8_t* dmx, size_t len) {
    // Called from the DMX thread, read from the UI thread, so it locks. A
    // real socket output does not need this; a simulation that is also a
    // display surface does.
    std::lock_guard<std::mutex> lock(m_);
    ++total_;
    for (Frame& f : frames_) {
        if (f.universe != universe) continue;
        ++f.frames;
        if (dmx) std::memcpy(f.data.data(), dmx, std::min<size_t>(len, 512));
        return;
    }
    Frame f;
    f.universe = universe;
    f.frames = 1;
    if (dmx) std::memcpy(f.data.data(), dmx, std::min<size_t>(len, 512));
    frames_.push_back(f);
    std::sort(frames_.begin(), frames_.end(),
              [](const Frame& a, const Frame& b) { return a.universe < b.universe; });
}

std::string SimulatedOutput::describe() const {
    std::lock_guard<std::mutex> lock(m_);
    return "simulated, " + std::to_string(frames_.size()) + " universe(s), " +
           std::to_string(total_) + " frames";
}

std::vector<SimulatedOutput::Frame> SimulatedOutput::frames() const {
    std::lock_guard<std::mutex> lock(m_);
    return frames_;
}

std::optional<SimulatedOutput::Frame> SimulatedOutput::frame(int universe) const {
    std::lock_guard<std::mutex> lock(m_);
    for (const Frame& f : frames_)
        if (f.universe == universe) return f;
    return std::nullopt;
}

uint64_t SimulatedOutput::totalFrames() const {
    std::lock_guard<std::mutex> lock(m_);
    return total_;
}

int SimulatedOutput::highestChannel(int universe) const {
    std::lock_guard<std::mutex> lock(m_);
    for (const Frame& f : frames_) {
        if (f.universe != universe) continue;
        for (int i = 511; i >= 0; --i)
            if (f.data[static_cast<size_t>(i)] != 0) return i + 1;
        return 0;
    }
    return 0;
}

void SimulatedOutput::reset() {
    std::lock_guard<std::mutex> lock(m_);
    frames_.clear();
    total_ = 0;
}

// ---------------------------------------------------------------------------
// SimulatedLaser
// ---------------------------------------------------------------------------
bool SimulatedLaser::open(std::string*) { return true; }

void SimulatedLaser::send(const GatedFrame& frame) {
    ++total_;
    if (frame.blanked()) ++blanked_;
    for (auto& [zone, stored] : last_) {
        if (zone != frame.zone()) continue;
        stored = frame;
        return;
    }
    last_.push_back({frame.zone(), frame});
}

LaserFault SimulatedLaser::status() const { return injected_; }

std::optional<GatedFrame> SimulatedLaser::lastFrame(int zone) const {
    for (const auto& [z, f] : last_)
        if (z == zone) return f;
    return std::nullopt;
}

std::vector<int> SimulatedLaser::zones() const {
    std::vector<int> out;
    for (const auto& [z, f] : last_) out.push_back(z);
    std::sort(out.begin(), out.end());
    return out;
}

// ---------------------------------------------------------------------------
// SimulatedInput
// ---------------------------------------------------------------------------
InputEvent SimulatedInput::midiNote(int note, bool down, int channel) {
    InputEvent e;
    e.kind = SourceKind::MidiNote;
    e.channel = channel;
    e.index = note;
    e.pressed = down;
    e.isNoteOff = !down;
    e.value = down ? 1.f : 0.f;
    return e;
}

InputEvent SimulatedInput::midiCC(int cc, float value, int channel) {
    InputEvent e;
    e.kind = SourceKind::MidiCC;
    e.channel = channel;
    e.index = cc;
    e.value = std::clamp(value, 0.f, 1.f);
    return e;
}

InputEvent SimulatedInput::osc(const std::string& address, float value) {
    InputEvent e;
    e.kind = SourceKind::Osc;
    e.address = address;
    e.value = value;
    return e;
}

InputEvent SimulatedInput::key(int code, Mods mods) {
    InputEvent e;
    e.kind = SourceKind::Key;
    e.index = code;
    e.mods = mods;
    e.pressed = true;
    e.value = 1.f;
    return e;
}

std::vector<uint8_t> SimulatedInput::sacnFrame(const std::string& sourceName,
                                               uint8_t priority, uint8_t sequence,
                                               int universe,
                                               const std::array<uint8_t, 512>& data) {
    return buildSacnPacket(makeCid(sourceName), sourceName, priority, sequence,
                           static_cast<uint16_t>(universe), data.data(), 512);
}

} // namespace lumen
