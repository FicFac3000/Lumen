#include "lumen/Laser.hpp"

#include <algorithm>
#include <cmath>

namespace lumen {

bool LaserPoint::samePositionAs(const LaserPoint& o, float eps) const {
    return std::fabs(x - o.x) <= eps && std::fabs(y - o.y) <= eps;
}

LaserPath makeSquare(float size) {
    const float h = size * 0.5f;
    LaserPath p;
    p.push_back(LaserPoint::lit(-h, -h, 1.f, 1.f, 1.f));
    p.push_back(LaserPoint::lit(h, -h, 1.f, 1.f, 1.f));
    p.push_back(LaserPoint::lit(h, h, 1.f, 1.f, 1.f));
    p.push_back(LaserPoint::lit(-h, h, 1.f, 1.f, 1.f));
    p.push_back(LaserPoint::lit(-h, -h, 1.f, 1.f, 1.f));
    return p;
}

LaserPath makeCircle(float radius, int segments) {
    LaserPath p;
    segments = std::max(3, segments);
    for (int i = 0; i <= segments; ++i) {
        const float a = 6.283185307179586f * static_cast<float>(i) /
                        static_cast<float>(segments);
        p.push_back(LaserPoint::lit(radius * std::cos(a), radius * std::sin(a), 0.f,
                                    1.f, 0.f));
    }
    return p;
}

LaserPath makeLine(float x0, float y0, float x1, float y1) {
    LaserPath p;
    p.push_back(LaserPoint::lit(x0, y0, 1.f, 0.f, 0.f));
    p.push_back(LaserPoint::lit(x1, y1, 1.f, 0.f, 0.f));
    return p;
}

// ---------------------------------------------------------------------------
// Faults
// ---------------------------------------------------------------------------
std::string describeFaults(LaserFault f) {
    if (!any(f)) return "none";
    std::string out;
    auto add = [&](LaserFault flag, const char* text) {
        if (!has(f, flag)) return;
        if (!out.empty()) out += ", ";
        out += text;
    };
    add(LaserFault::DacDisconnected, "DAC disconnected");
    add(LaserFault::BufferUnderrun, "buffer underrun");
    add(LaserFault::StreamStall, "point stream stalled");
    add(LaserFault::RenderStall, "render stalled");
    add(LaserFault::GalvoFault, "galvo fault");
    add(LaserFault::PointRateExceeded, "point rate exceeded");
    return out;
}

bool LaserMask::contains(float x, float y) const {
    if (!enabled) return true;
    return x >= minX && x <= maxX && y >= minY && y <= maxY;
}

float ZoneSafety::effectivePower() const {
    float p = std::clamp(maxPower, 0.f, 1.f);
    if (audienceScanning) {
        // §14.6: audience attenuation is independent of the normal intensity
        // path and cannot be RAISED by it, so it multiplies in rather than
        // being compared against. And without the explicit opt-in an audience
        // zone emits nothing at all.
        if (!audienceOptIn) return 0.f;
        p *= std::clamp(audienceAttenuation, 0.f, 1.f);
    }
    return p;
}

// ---------------------------------------------------------------------------
// SafetyGate
// ---------------------------------------------------------------------------
ZoneSafety& SafetyGate::addZone(const std::string& name, Uid uid) {
    ZoneSafety z;
    z.uid = uid;
    z.id = nextId_++;
    z.name = name.empty() ? "Zone " + std::to_string(z.id) : name;
    // Fail-safe default: a zone starts disarmed and masked (§14.6).
    z.armed = false;
    zones_.push_back(std::move(z));
    return zones_.back();
}

ZoneSafety* SafetyGate::zone(int id) {
    for (auto& z : zones_)
        if (z.id == id) return &z;
    return nullptr;
}

const ZoneSafety* SafetyGate::zone(int id) const {
    for (const auto& z : zones_)
        if (z.id == id) return &z;
    return nullptr;
}

bool SafetyGate::removeZone(int id) {
    for (size_t i = 0; i < zones_.size(); ++i) {
        if (zones_[i].id != id) continue;
        zones_.erase(zones_.begin() + static_cast<long>(i));
        return true;
    }
    return false;
}

bool SafetyGate::beginArm(int zoneId) {
    ZoneSafety* z = zone(zoneId);
    if (!z) return false;
    // An emergency stop cannot be armed around. Reaching for arm during an
    // E-stop is exactly when refusing matters.
    if (estop_) return false;
    z->armHolding = true;
    z->armHoldStarted = now();
    return true;
}

bool SafetyGate::confirmArm(int zoneId) {
    ZoneSafety* z = zone(zoneId);
    if (!z) return false;
    if (estop_) return false;
    if (!z->armHolding) return false;
    // The hold has to have actually elapsed. A fumbled double-press must not
    // arm a laser.
    if (now() - z->armHoldStarted < armHold_) return false;
    // Nor may a zone with an unresolved fault arm.
    if (any(z->faults)) return false;
    z->armed = true;
    z->armHolding = false;
    return true;
}

void SafetyGate::cancelArm(int zoneId) {
    if (ZoneSafety* z = zone(zoneId)) z->armHolding = false;
}

bool SafetyGate::disarm(int zoneId) {
    ZoneSafety* z = zone(zoneId);
    if (!z) return false;
    z->armed = false;
    z->armHolding = false;
    return true;
}

void SafetyGate::disarmAll() {
    for (auto& z : zones_) {
        z.armed = false;
        z.armHolding = false;
    }
}

void SafetyGate::emergencyStop() {
    estop_ = true;
    // Disarm as well as latch, so clearing the latch alone cannot restore a
    // beam that was live when the button was hit.
    disarmAll();
}

void SafetyGate::resetEmergencyStop() {
    estop_ = false;
    disarmAll();
}

void SafetyGate::reportFault(int zoneId, LaserFault fault) {
    ZoneSafety* z = zone(zoneId);
    if (!z) return;
    z->faults |= fault;
    // A fault disarms. Recovering means clearing the fault AND arming again
    // deliberately, rather than the beam returning on its own the moment a
    // cable is reseated.
    if (any(z->faults)) z->armed = false;
}

void SafetyGate::clearFault(int zoneId, LaserFault fault) {
    ZoneSafety* z = zone(zoneId);
    if (!z) return;
    z->faults = static_cast<LaserFault>(static_cast<uint16_t>(z->faults) &
                                        ~static_cast<uint16_t>(fault));
}

void SafetyGate::reportOutputStatus(LaserFault fault) {
    for (auto& z : zones_) {
        z.faults |= fault;
        if (any(z.faults)) z.armed = false;
    }
}

GatedFrame SafetyGate::blankFrame(int zoneId) {
    GatedFrame f;
    f.zone_ = zoneId;
    f.blanked_ = true;
    f.appliedPower_ = 0.f;
    if (const ZoneSafety* z = zone(zoneId)) f.faults_ = z->faults;
    return f;
}

GatedFrame SafetyGate::process(int zoneId, const LaserPath& path) {
    GatedFrame out;
    out.zone_ = zoneId;
    out.blanked_ = true;
    out.appliedPower_ = 0.f;

    ZoneSafety* z = zone(zoneId);
    if (!z) {
        // An unknown zone emits nothing. A typo in a zone id must not be a
        // route to unmasked output.
        out.faults_ = LaserFault::DacDisconnected;
        return out;
    }

    // Record that the zone was fed, before any decision: the stall detector
    // measures whether frames ARRIVE, not whether they were allowed through.
    z->lastFrameAt = now();
    z->everFramed = true;
    out.faults_ = z->faults;

    // Order matters. Each of these is a veto, and the cheapest, most absolute
    // ones come first.
    if (estop_) return out;
    if (!z->armed) return out;
    if (any(z->faults)) return out;
    if (budget_ > 0 && static_cast<int>(path.size()) > budget_) {
        // Over the point budget is a fault, not a reason to truncate: a
        // half-drawn figure at the wrong rate is a flickering beam, and the
        // caller should have budgeted (§14.4). Routed through reportFault so
        // it DISARMS like every other fault — recovery is deliberate.
        reportFault(zoneId, LaserFault::PointRateExceeded);
        out.faults_ = z->faults;
        return out;
    }

    const float power = z->effectivePower();
    if (power <= 0.f) return out;

    // Passed. Now the geometry: a point outside the mask is BLANKED, not
    // clamped to the edge — clamping would draw a line along the mask
    // boundary, which is content the operator never asked for.
    out.points_ = path;
    for (LaserPoint& p : out.points_) {
        if (!z->mask.contains(p.x, p.y)) {
            p.blanked = true;
            p.r = p.g = p.b = 0.f;
            ++out.clipped_;
            continue;
        }
        if (p.blanked) {
            // A blanked point carries no colour, whatever the content said.
            p.r = p.g = p.b = 0.f;
            continue;
        }
        p.r *= power;
        p.g *= power;
        p.b *= power;
    }

    out.appliedPower_ = power;
    // Blanked only if nothing at all got through.
    out.blanked_ = true;
    for (const LaserPoint& p : out.points_)
        if (!p.blanked) {
            out.blanked_ = false;
            break;
        }
    return out;
}

int SafetyGate::sweep() {
    const double t = now();
    int stalled = 0;
    for (ZoneSafety& z : zones_) {
        if (!z.everFramed) continue;
        if (t - z.lastFrameAt <= z.stallTimeout) continue;
        if (has(z.faults, LaserFault::StreamStall)) continue;
        z.faults |= LaserFault::StreamStall;
        z.armed = false;
        ++stalled;
    }
    return stalled;
}

SafetyStatus SafetyGate::status(int zoneId) const {
    SafetyStatus s;
    const ZoneSafety* z = zone(zoneId);
    s.zone = zoneId;
    s.emergencyStopped = estop_;
    s.pointBudget = budget_;
    if (!z) {
        s.blanking = true;
        s.reason = "no such zone";
        return s;
    }
    s.name = z->name;
    s.armed = z->armed;
    s.maskActive = z->mask.enabled;
    s.attenuation = z->effectivePower();
    s.audienceScanning = z->audienceScanning;
    s.faults = z->faults;
    s.blanking = estop_ || !z->armed || any(z->faults) || s.attenuation <= 0.f;

    // Plain language, because §14.6 asks for visible state and "blanking:
    // true" answers nothing.
    if (estop_) s.reason = "emergency stop";
    else if (any(z->faults)) s.reason = describeFaults(z->faults);
    else if (!z->armed) s.reason = "disarmed";
    else if (z->audienceScanning && !z->audienceOptIn)
        s.reason = "audience zone without opt-in";
    else if (s.attenuation <= 0.f) s.reason = "attenuated to zero";
    else s.reason = "emitting";
    return s;
}

std::vector<SafetyStatus> SafetyGate::statuses() const {
    std::vector<SafetyStatus> out;
    for (const ZoneSafety& z : zones_) out.push_back(status(z.id));
    return out;
}

void SafetyGate::failSafe(const std::string& why) {
    disarmAll();
    lastFailSafe_ = why;
    ++failSafes_;
}

// ---------------------------------------------------------------------------
// Optimiser
// ---------------------------------------------------------------------------
bool isClosedPath(const LaserPath& path, float eps) {
    if (path.size() < 3) return false;
    return path.front().samePositionAs(path.back(), eps);
}

float turnAngleAt(const LaserPath& path, size_t i, bool closed) {
    if (path.size() < 3) return 0.f;
    size_t prev = 0, next = 0;
    if (closed) {
        // Wrap. The seam of a closed figure is a corner like any other, and
        // the galvo turning through it needs the same dwell.
        prev = (i == 0) ? path.size() - 1 : i - 1;
        next = (i + 1 >= path.size()) ? 0 : i + 1;
    } else {
        if (i == 0 || i + 1 >= path.size()) return 0.f;
        prev = i - 1;
        next = i + 1;
    }
    const float ax = path[i].x - path[prev].x;
    const float ay = path[i].y - path[prev].y;
    const float bx = path[next].x - path[i].x;
    const float by = path[next].y - path[i].y;
    const float la = std::sqrt(ax * ax + ay * ay);
    const float lb = std::sqrt(bx * bx + by * by);
    // A zero-length leg has no direction, so there is no turn to measure.
    if (la < 1e-9f || lb < 1e-9f) return 0.f;
    const float cosA = std::clamp((ax * bx + ay * by) / (la * lb), -1.f, 1.f);
    return std::acos(cosA) * 57.29577951308232f;
}

LaserPath interpolateTravel(const LaserPath& in, float maxStep, int* added) {
    LaserPath out;
    if (in.empty()) return out;
    if (maxStep <= 0.f) return in;

    int inserted = 0;
    out.push_back(in.front());
    for (size_t i = 1; i < in.size(); ++i) {
        const LaserPoint& a = in[i - 1];
        const LaserPoint& b = in[i];
        const float dx = b.x - a.x;
        const float dy = b.y - a.y;
        const float dist = std::sqrt(dx * dx + dy * dy);
        // A leg already at the limit measures a hair ABOVE it in floats, so a
        // strict comparison split every leg in half on a second pass and the
        // path grew without bound. §14.4 leaves per-zone versus per-projector
        // optimisation open, which means chained passes are plausible, so the
        // stage has to be stable under re-entry. A leg within rounding of the
        // limit is left alone; splitting it buys nothing anyway.
        const int steps = (dist <= maxStep * 1.0001f)
                              ? 1
                              : static_cast<int>(std::ceil(dist / maxStep));
        for (int s = 1; s < steps; ++s) {
            const float t = static_cast<float>(s) / static_cast<float>(steps);
            LaserPoint p = b;
            p.x = a.x + dx * t;
            p.y = a.y + dy * t;
            // The intermediate points belong to the move INTO b, so they take
            // b's beam state: a blanked move stays blanked the whole way, and
            // a lit move is lit the whole way.
            if (b.blanked) {
                p.blanked = true;
                p.r = p.g = p.b = 0.f;
            }
            out.push_back(p);
            ++inserted;
        }
        out.push_back(b);
    }
    if (added) *added = inserted;
    return out;
}

LaserPath addCornerDwell(const LaserPath& in, float angleDegrees, int dwell,
                         int* corners, bool closed) {
    LaserPath out;
    int found = 0;
    if (in.empty()) return out;
    if (dwell <= 0 || angleDegrees <= 0.f) {
        if (corners) *corners = 0;
        return in;
    }

    for (size_t i = 0; i < in.size(); ++i) {
        out.push_back(in[i]);
        // A blanked point needs no dwell: the beam is off, so the galvo can
        // take as long as it likes.
        if (in[i].blanked) continue;
        if (turnAngleAt(in, i, closed) < angleDegrees) continue;
        ++found;
        // Repeat the corner point so the galvo arrives before the beam moves
        // on. Repeated points, not interpolated ones: the beam must dwell AT
        // the corner, not approach it more slowly.
        for (int d = 0; d < dwell; ++d) out.push_back(in[i]);
    }
    if (corners) *corners = found;
    return out;
}

LaserPath addBlankingDwell(const LaserPath& in, int before, int after, int* added) {
    LaserPath out;
    int inserted = 0;
    if (in.empty()) return out;
    before = std::max(0, before);
    after = std::max(0, after);

    for (size_t i = 0; i < in.size(); ++i) {
        const bool blanked = in[i].blanked;
        const bool prevBlanked = i > 0 ? in[i - 1].blanked : true;

        // Already-present dwell is not repeated. Each pass would otherwise
        // add another layer at every transition, so a path optimised twice
        // grew a longer and longer dark lead.
        auto alreadyHeld = [&](const LaserPoint& at, int want) {
            if (want <= 0) return true;
            if (out.size() < static_cast<size_t>(want)) return false;
            for (int k = 1; k <= want; ++k) {
                const LaserPoint& q = out[out.size() - static_cast<size_t>(k)];
                if (!q.blanked || !q.samePositionAs(at)) return false;
            }
            return true;
        };

        // Beam going off: dwell at the PREVIOUS position with the beam
        // already off, so it is dark before any travel begins.
        if (blanked && !prevBlanked && i > 0) {
            LaserPoint hold = in[i - 1];
            hold.blanked = true;
            hold.r = hold.g = hold.b = 0.f;
            if (!alreadyHeld(hold, before)) {
                for (int d = 0; d < before; ++d) {
                    out.push_back(hold);
                    ++inserted;
                }
            }
        }

        // Beam coming on: dwell at the NEW position still dark, so the galvo
        // has arrived before the beam lights.
        if (!blanked && prevBlanked) {
            LaserPoint hold = in[i];
            hold.blanked = true;
            hold.r = hold.g = hold.b = 0.f;
            if (!alreadyHeld(hold, after)) {
                for (int d = 0; d < after; ++d) {
                    out.push_back(hold);
                    ++inserted;
                }
            }
        }

        out.push_back(in[i]);
    }
    if (added) *added = inserted;
    return out;
}

LaserPath applyColourShift(const LaserPath& in, int samples) {
    if (samples == 0 || in.empty()) return in;
    LaserPath out = in;
    const int n = static_cast<int>(in.size());
    for (int i = 0; i < n; ++i) {
        // Positive delays colour relative to position: the colour emitted at
        // sample i is the colour authored `samples` earlier.
        const int from = i - samples;
        LaserPoint& p = out[static_cast<size_t>(i)];
        if (from < 0 || from >= n) {
            // No authored colour belongs here. Clamping to the first sample
            // would light the leading samples with whatever the stroke starts
            // with, including over a position that should be dark — so the
            // gap emits NOTHING, which is the only safe default for a laser.
            p.r = p.g = p.b = 0.f;
            p.blanked = true;
            continue;
        }
        p.r = in[static_cast<size_t>(from)].r;
        p.g = in[static_cast<size_t>(from)].g;
        p.b = in[static_cast<size_t>(from)].b;
        // Blanking shifts WITH the colour. It is a property of the beam, not
        // of the position, and leaving it behind would light a travel move.
        p.blanked = in[static_cast<size_t>(from)].blanked;
    }
    return out;
}

OptimiseReport optimise(const LaserPath& in, const OptimiserSettings& s) {
    OptimiseReport rep;
    rep.generated = static_cast<int>(in.size());
    if (in.empty()) return rep;

    LaserPath work = in;

    // Closure is decided BEFORE dedupe, because dedupe is what removes the
    // evidence of it.
    const bool closed = isClosedPath(work) &&
                        work.front().blanked == work.back().blanked;

    // A closed figure whose last point repeats the first spends a sample
    // redrawing it.
    if (s.dedupe && closed && work.size() > 2) {
        work.pop_back();
        rep.deduped = 1;
    }

    // Corners first, measured on the ORIGINAL geometry: interpolating first
    // would turn every corner into a shallow sequence of turns and the dwell
    // would land in the wrong place or not at all. `closed` is what gives the
    // seam its dwell.
    work = addCornerDwell(work, s.cornerAngleDegrees, s.cornerDwell, &rep.corners,
                          closed);
    // Then blanking dwell, while the blanking transitions are still at the
    // path's own vertices.
    work = addBlankingDwell(work, s.blankBefore, s.blankAfter, &rep.blankingPoints);
    // Then split long moves, which cannot create or destroy either of the
    // above.
    work = interpolateTravel(work, s.maxStep, &rep.interpolated);
    // Colour shift last, because it compensates for the hardware at the far
    // end of the pipeline.
    work = applyColourShift(work, s.colourShift);

    rep.emitted = static_cast<int>(work.size());
    rep.path = std::move(work);
    return rep;
}

// ---------------------------------------------------------------------------
// Budgeting
// ---------------------------------------------------------------------------
BudgetReport budgetPoints(const std::vector<int>& pointsPerZone, int pointsPerSecond,
                          double requestedFps) {
    BudgetReport r;
    r.pointsPerSecond = std::max(0, pointsPerSecond);
    r.requestedFps = std::max(0.0, requestedFps);
    for (int n : pointsPerZone) r.totalPoints += std::max(0, n);

    if (r.requestedFps <= 0.0 || r.pointsPerSecond <= 0) {
        r.achievableFps = 0.0;
        r.overBudget = r.totalPoints > 0;
        return r;
    }

    r.budgetPerFrame =
        static_cast<int>(static_cast<double>(r.pointsPerSecond) / r.requestedFps);
    r.overBudget = r.totalPoints > r.budgetPerFrame;
    // What the figure will ACTUALLY run at, which is the number that explains
    // flicker. Reporting it beats silently dropping points.
    r.achievableFps = r.totalPoints > 0
                          ? static_cast<double>(r.pointsPerSecond) /
                                static_cast<double>(r.totalPoints)
                          : r.requestedFps;
    if (r.achievableFps > r.requestedFps) r.achievableFps = r.requestedFps;
    return r;
}

LaserPath decimate(const LaserPath& in, int maxPoints) {
    if (maxPoints <= 0) return {};
    if (static_cast<int>(in.size()) <= maxPoints) return in;
    if (maxPoints == 1) return {in.front()};

    // Every blanking transition is kept: dropping one would turn a blanked
    // move into a visible line, which is a safety matter rather than a
    // cosmetic one. Those points are mandatory; the rest are thinned.
    std::vector<size_t> mandatory;
    mandatory.push_back(0);
    for (size_t i = 1; i < in.size(); ++i)
        if (in[i].blanked != in[i - 1].blanked) mandatory.push_back(i);
    if (mandatory.back() != in.size() - 1) mandatory.push_back(in.size() - 1);

    std::vector<bool> keep(in.size(), false);
    for (size_t i : mandatory) keep[i] = true;

    int room = maxPoints - static_cast<int>(mandatory.size());
    if (room > 0) {
        // Spread what is left evenly over the points that are not already
        // mandatory.
        std::vector<size_t> spare;
        for (size_t i = 0; i < in.size(); ++i)
            if (!keep[i]) spare.push_back(i);
        if (!spare.empty()) {
            const double stride =
                static_cast<double>(spare.size()) / static_cast<double>(room);
            for (int k = 0; k < room; ++k) {
                const size_t idx = static_cast<size_t>(static_cast<double>(k) * stride);
                if (idx < spare.size()) keep[spare[idx]] = true;
            }
        }
    }

    LaserPath out;
    for (size_t i = 0; i < in.size(); ++i)
        if (keep[i]) out.push_back(in[i]);
    return out;
}

} // namespace lumen
