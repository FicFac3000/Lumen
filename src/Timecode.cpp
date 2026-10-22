#include "lumen/Timecode.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace lumen {

const char* frameRateName(FrameRate r) {
    switch (r) {
        case FrameRate::Fps24: return "24";
        case FrameRate::Fps25: return "25";
        case FrameRate::Fps2997Drop: return "29.97 drop";
        case FrameRate::Fps30: return "30";
    }
    return "25";
}

int framesPerSecond(FrameRate r) {
    switch (r) {
        case FrameRate::Fps24: return 24;
        case FrameRate::Fps25: return 25;
        // Drop-frame counts 0..29 like 30 fps; what differs is which numbers
        // exist at a minute boundary.
        case FrameRate::Fps2997Drop: return 30;
        case FrameRate::Fps30: return 30;
    }
    return 25;
}

double realFrameRate(FrameRate r) {
    if (r == FrameRate::Fps2997Drop) return 30000.0 / 1001.0;
    return static_cast<double>(framesPerSecond(r));
}

bool isDropFrame(FrameRate r) { return r == FrameRate::Fps2997Drop; }

const char* timecodeSourceName(TimecodeSource s) {
    switch (s) {
        case TimecodeSource::Internal: return "internal";
        case TimecodeSource::Ltc: return "LTC";
        case TimecodeSource::Mtc: return "MTC";
        case TimecodeSource::ArtNet: return "Art-Net";
    }
    return "internal";
}

std::string Timecode::text(FrameRate rate) const {
    char buf[32];
    // A semicolon before the frames is the conventional drop-frame marker, so
    // the two are distinguishable at a glance.
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d%c%02d", hours, minutes, seconds,
                  isDropFrame(rate) ? ';' : ':', frames);
    return buf;
}

bool isValidTimecode(const Timecode& tc, FrameRate rate) {
    const int fps = framesPerSecond(rate);
    if (tc.hours < 0 || tc.hours > 23) return false;
    if (tc.minutes < 0 || tc.minutes > 59) return false;
    if (tc.seconds < 0 || tc.seconds > 59) return false;
    if (tc.frames < 0 || tc.frames >= fps) return false;
    if (!isDropFrame(rate)) return true;
    // Frames 0 and 1 do not exist at the start of a minute, except every
    // tenth minute. A reading that names one is not a valid drop-frame code.
    if (tc.seconds == 0 && tc.frames < 2 && (tc.minutes % 10) != 0) return false;
    return true;
}

int64_t toFrames(const Timecode& tc, FrameRate rate) {
    const int fps = framesPerSecond(rate);
    const int64_t totalMinutes =
        static_cast<int64_t>(tc.hours) * 60 + static_cast<int64_t>(tc.minutes);
    int64_t frames = ((totalMinutes * 60) + tc.seconds) * fps + tc.frames;

    if (isDropFrame(rate)) {
        // Two numbers are skipped per minute, except every tenth minute, so
        // the count is short by 2 for each such minute already passed.
        const int64_t droppedMinutes = totalMinutes - (totalMinutes / 10);
        frames -= 2 * droppedMinutes;
    }
    return frames;
}

Timecode fromFrames(int64_t frames, FrameRate rate) {
    const int fps = framesPerSecond(rate);
    Timecode tc;
    if (frames < 0) frames = 0;

    if (isDropFrame(rate)) {
        // Work in ten-minute blocks: each holds 10 * 60 * 30 = 18000 nominal
        // frames minus 2 for each of the nine minutes that drop, so 17982.
        constexpr int64_t kPerTenMinutes = 17982;
        constexpr int64_t kPerMinute = 1798;   // a dropping minute: 1800 - 2
        const int64_t tenBlocks = frames / kPerTenMinutes;
        int64_t rest = frames % kPerTenMinutes;

        int64_t minutes = tenBlocks * 10;
        if (rest >= 1800) {
            // Past the first (non-dropping) minute of the block.
            rest -= 1800;
            minutes += 1 + rest / kPerMinute;
            rest %= kPerMinute;
            // Within a dropping minute the numbering starts at frame 2.
            rest += 2;
        }
        const int64_t totalSeconds = minutes * 60 + rest / fps;
        tc.frames = static_cast<int>(rest % fps);
        tc.seconds = static_cast<int>(totalSeconds % 60);
        tc.minutes = static_cast<int>((totalSeconds / 60) % 60);
        tc.hours = static_cast<int>((totalSeconds / 3600) % 24);
        return tc;
    }

    tc.frames = static_cast<int>(frames % fps);
    const int64_t totalSeconds = frames / fps;
    tc.seconds = static_cast<int>(totalSeconds % 60);
    tc.minutes = static_cast<int>((totalSeconds / 60) % 60);
    tc.hours = static_cast<int>((totalSeconds / 3600) % 24);
    return tc;
}

double toSeconds(const Timecode& tc, FrameRate rate) {
    return static_cast<double>(toFrames(tc, rate)) / realFrameRate(rate);
}

Timecode fromSeconds(double seconds, FrameRate rate) {
    if (seconds < 0.0) seconds = 0.0;
    // Round rather than truncate: a position derived from a float that lands
    // a hair under a frame boundary should be that frame, not the one before.
    const int64_t frames =
        static_cast<int64_t>(std::llround(seconds * realFrameRate(rate)));
    return fromFrames(frames, rate);
}

// ---------------------------------------------------------------------------
// TimecodeSlot
// ---------------------------------------------------------------------------
double TimecodeSlot::secondsAt(double now) const {
    const double base = toSeconds(position, rate);
    if (!running) return base;
    // Free-run from the last external frame. For an Internal slot that frame
    // is whenever it was started or set, which is the same mechanism.
    const double since = everFed ? std::max(0.0, now - lastFrameAt) : 0.0;
    return base + since;
}

Timecode TimecodeSlot::at(double now) const {
    return fromSeconds(secondsAt(now), rate);
}

// ---------------------------------------------------------------------------
// TimecodeEngine
// ---------------------------------------------------------------------------
TimecodeSlot& TimecodeEngine::add(const std::string& name, TimecodeSource source,
                                  FrameRate rate) {
    TimecodeSlot s;
    s.uid = uids_->next();
    s.id = nextId_++;
    s.name = name.empty() ? "Timecode " + std::to_string(s.id) : name;
    s.source = source;
    s.rate = rate;
    slots_.push_back(std::move(s));
    return slots_.back();
}

TimecodeSlot& TimecodeEngine::restore(TimecodeSlot s) {
    if (!s.uid.valid()) s.uid = uids_->next();
    uids_->observe(s.uid);
    nextId_ = std::max(nextId_, s.id + 1);
    slots_.push_back(std::move(s));
    return slots_.back();
}

TimecodeSlot* TimecodeEngine::slot(int id) {
    for (auto& s : slots_)
        if (s.id == id) return &s;
    return nullptr;
}

const TimecodeSlot* TimecodeEngine::slot(int id) const {
    for (const auto& s : slots_)
        if (s.id == id) return &s;
    return nullptr;
}

TimecodeSlot* TimecodeEngine::byName(const std::string& name) {
    for (auto& s : slots_)
        if (s.name == name) return &s;
    return nullptr;
}

TimecodeSlot* TimecodeEngine::byUid(Uid uid) {
    for (auto& s : slots_)
        if (s.uid == uid) return &s;
    return nullptr;
}

bool TimecodeEngine::remove(int id) {
    for (size_t i = 0; i < slots_.size(); ++i) {
        if (slots_[i].id != id) continue;
        slots_.erase(slots_.begin() + static_cast<long>(i));
        watches_.erase(std::remove_if(watches_.begin(), watches_.end(),
                                      [id](const Watch& w) { return w.slot == id; }),
                       watches_.end());
        return true;
    }
    return false;
}

bool TimecodeEngine::feed(int id, const Timecode& tc) {
    TimecodeSlot* s = slot(id);
    if (!s) return false;
    // A reading the rate cannot express is a decoder fault. Reporting it beats
    // smoothing it over, because the usual cause is the wrong rate configured.
    if (!isValidTimecode(tc, s->rate)) return false;

    const double t = now();
    const int64_t incoming = toFrames(tc, s->rate);

    if (s->everFed) {
        // Where free-running from the last frame would have put us.
        const double expected = toSeconds(s->position, s->rate) +
                                (s->running ? std::max(0.0, t - s->lastFrameAt) : 0.0);
        const double actual = toSeconds(tc, s->rate);
        s->driftFrames = (actual - expected) * realFrameRate(s->rate);

        const int64_t previous = toFrames(s->position, s->rate);
        // A jump is a scrub or a lost lock, not drift. The threshold is
        // deliberately loose: a second of slop is normal on a network source.
        if (incoming < previous ||
            incoming - previous > static_cast<int64_t>(realFrameRate(s->rate)))
            ++s->jumps;
    }

    s->position = tc;
    s->lastFrameAt = t;
    s->everFed = true;
    s->stale = false;
    s->running = true;   // frames arriving means it is running
    ++s->frames;
    return true;
}

bool TimecodeEngine::start(int id) {
    TimecodeSlot* s = slot(id);
    if (!s) return false;
    s->running = true;
    s->lastFrameAt = now();
    // An internal slot has no external frames, so starting it is what sets
    // the reference the free-run counts from.
    s->everFed = true;
    s->stale = false;
    return true;
}

bool TimecodeEngine::stop(int id) {
    TimecodeSlot* s = slot(id);
    if (!s) return false;
    // Freeze where it actually is, so a stop does not lose the position the
    // free-run had reached.
    s->position = s->at(now());
    s->running = false;
    s->lastFrameAt = now();
    return true;
}

bool TimecodeEngine::setPosition(int id, const Timecode& tc) {
    TimecodeSlot* s = slot(id);
    if (!s) return false;
    if (!isValidTimecode(tc, s->rate)) return false;
    s->position = tc;
    s->lastFrameAt = now();
    s->everFed = true;
    s->driftFrames = 0.0;
    return true;
}

int TimecodeEngine::tick() {
    const double t = now();
    int stale = 0;
    for (TimecodeSlot& s : slots_) {
        // An internal slot cannot go stale: it is its own source.
        if (s.source == TimecodeSource::Internal) continue;
        if (!s.everFed || !s.running) continue;
        const bool now_stale = (t - s.lastFrameAt) > timeout_;
        if (now_stale && !s.stale) ++stale;
        s.stale = now_stale;
    }
    return stale;
}

// ---------------------------------------------------------------------------
// Trigger crossings
// ---------------------------------------------------------------------------
void TimecodeEngine::watch(int slotId, const Timecode& tc) {
    const TimecodeSlot* s = slot(slotId);
    if (!s) return;
    for (const Watch& w : watches_)
        if (w.slot == slotId && w.at == tc) return;
    Watch w;
    w.slot = slotId;
    w.at = tc;
    w.frames = toFrames(tc, s->rate);
    w.primed = false;
    watches_.push_back(w);
}

void TimecodeEngine::unwatch(int slotId, const Timecode& tc) {
    watches_.erase(std::remove_if(watches_.begin(), watches_.end(),
                                  [&](const Watch& w) {
                                      return w.slot == slotId && w.at == tc;
                                  }),
                   watches_.end());
}

std::vector<TimecodeEngine::Crossing> TimecodeEngine::crossings(double now) {
    std::vector<Crossing> out;
    for (Watch& w : watches_) {
        const TimecodeSlot* s = slot(w.slot);
        if (!s) continue;
        const int64_t position = toFrames(s->at(now), s->rate);

        if (!w.primed) {
            // The first observation establishes where we are; it does not
            // fire. Otherwise registering a trigger below the current
            // position would fire it immediately.
            w.primed = true;
            w.lastPosition = position;
            continue;
        }

        // "Passed", not "equals": a dropped frame or a coarse tick must not
        // make a cue miss its trigger, which is exactly what an equality test
        // would do.
        if (w.lastPosition < w.frames && position >= w.frames)
            out.push_back({w.slot, w.at, true});
        else if (w.lastPosition > w.frames && position <= w.frames)
            out.push_back({w.slot, w.at, false});
        w.lastPosition = position;
    }
    return out;
}

} // namespace lumen
