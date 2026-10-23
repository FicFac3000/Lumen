// Laser.hpp — point stream, optimiser and the safety gate (SPEC §14).
//
// §14.8 inverts the usual order deliberately: output and SAFETY come before
// content, because a laser pipeline that renders beautifully and blanks
// unreliably is worse than one that does nothing. So this file starts with the
// gate.
//
// THE GATE IS STRUCTURAL, NOT POLICY. §14.6 says nothing upstream can bypass
// it — not a cue, not an FX recipe, not a Learn binding, not a macro. Saying
// so in a comment is worth nothing, so it is enforced by the type system:
// a DAC accepts only a GatedFrame, and GatedFrame has no public constructor.
// SafetyGate is the only thing that can make one. Handing raw points to a
// projector is therefore a COMPILE ERROR, which is the only version of this
// rule that survives contact with future code.
//
// The other half of §14.6 is that the gate's own state is unreachable from the
// value system. `Arm` exists as a Control attribute for display (§14.2), but
// the armed state lives here and is only settable by an explicit hold-to-fire
// act. A cue storing Arm=1 arms nothing — tested, because that is exactly the
// mistake that would make every other precaution pointless.
#pragma once

#include "lumen/Attribute.hpp"
#include "lumen/Uid.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace lumen {

// ---------------------------------------------------------------------------
// Points (§14.1)
// ---------------------------------------------------------------------------
// Coordinates are -1..+1 in both axes, which is what every DAC protocol
// normalises to. Colour is 0..1 per channel. `blanked` is the beam being off,
// kept as a flag rather than implied by black so that a blanked move through
// a coloured region cannot be mistaken for visible content.
struct LaserPoint {
    float x = 0.f;
    float y = 0.f;
    float r = 0.f;
    float g = 0.f;
    float b = 0.f;
    bool blanked = true;

    static LaserPoint lit(float x, float y, float r, float g, float b) {
        return {x, y, r, g, b, false};
    }
    static LaserPoint dark(float x, float y) { return {x, y, 0.f, 0.f, 0.f, true}; }

    bool samePositionAs(const LaserPoint& o, float eps = 1e-5f) const;
};

using LaserPath = std::vector<LaserPoint>;

// A few shapes, enough to exercise the optimiser against figures with known
// corner counts rather than arbitrary data.
LaserPath makeSquare(float size = 0.8f);
LaserPath makeCircle(float radius = 0.8f, int segments = 64);
LaserPath makeLine(float x0, float y0, float x1, float y1);

// ---------------------------------------------------------------------------
// Faults (§14.6)
// ---------------------------------------------------------------------------
// A stationary beam at full power is the dangerous case, so every condition
// that could produce one blanks output.
enum class LaserFault : uint16_t {
    None = 0,
    DacDisconnected = 1 << 0,
    BufferUnderrun = 1 << 1,
    StreamStall = 1 << 2,   // no new frame within the stall timeout
    RenderStall = 1 << 3,
    GalvoFault = 1 << 4,    // where the DAC reports loss of motion
    PointRateExceeded = 1 << 5,
};

inline LaserFault operator|(LaserFault a, LaserFault b) {
    return static_cast<LaserFault>(static_cast<uint16_t>(a) | static_cast<uint16_t>(b));
}
inline LaserFault& operator|=(LaserFault& a, LaserFault b) {
    a = a | b;
    return a;
}
inline bool has(LaserFault set, LaserFault flag) {
    return (static_cast<uint16_t>(set) & static_cast<uint16_t>(flag)) != 0;
}
inline bool any(LaserFault set) { return static_cast<uint16_t>(set) != 0; }

std::string describeFaults(LaserFault f);

// ---------------------------------------------------------------------------
// Zone safety configuration (§14.6)
// ---------------------------------------------------------------------------
// A rectangular mask with hard geometric limits. A zone cannot emit outside
// it regardless of content or transform, so an FX driving a position past the
// edge produces blanking rather than a beam somewhere it must not go.
struct LaserMask {
    bool enabled = true;     // on by default: §14.6 wants compliance to be the easy path
    float minX = -1.f, maxX = 1.f;
    float minY = -1.f, maxY = 1.f;

    bool contains(float x, float y) const;
};

struct ZoneSafety {
    Uid uid{};
    int id = 0;
    std::string name;

    // Armed is NOT a value. It is set only by the hold-to-fire path below,
    // and a zone starts disarmed (§14.6 fail-safe defaults).
    bool armed = false;
    // Which physical act armed it, kept for the status panel and the audit
    // record. Never persisted — see armed above.
    uint8_t armedFrom = 0;

    LaserMask mask;
    // Applied after all merging, so no priority can beat it (§11.4 cannot
    // reach here).
    float maxPower = 1.f;

    // §14.6: audience-scanning zones are flagged distinctly, with
    // independent attenuation that is separate from and CANNOT BE RAISED BY
    // the normal intensity path.
    bool audienceScanning = false;
    float audienceAttenuation = 0.1f;
    // Explicit opt-in, so audience scanning is never an ordinary parameter.
    bool audienceOptIn = false;

    // Scan-fail: how long without a frame before the zone blanks.
    double stallTimeout = 0.1;

    // --- runtime -----------------------------------------------------------
    double lastFrameAt = 0.0;
    bool everFramed = false;
    LaserFault faults = LaserFault::None;
    // Hold-to-fire progress.
    double armHoldStarted = 0.0;
    bool armHolding = false;
    uint8_t armHoldFrom = 0;   // which physical act began the hold

    // What the gate will actually multiply colour by, which is the number an
    // operator needs to see rather than infer.
    float effectivePower() const;
};

// ---------------------------------------------------------------------------
// GatedFrame — the only thing a DAC accepts
// ---------------------------------------------------------------------------
class SafetyGate;

class GatedFrame {
public:
    // Copyable and movable, because a frame legitimately travels from the
    // gate to a DAC; but not CONSTRUCTIBLE outside the gate.
    GatedFrame(const GatedFrame&) = default;
    GatedFrame(GatedFrame&&) = default;
    GatedFrame& operator=(const GatedFrame&) = default;
    GatedFrame& operator=(GatedFrame&&) = default;

    const LaserPath& points() const { return points_; }
    int zone() const { return zone_; }
    // True when the gate suppressed everything. The frame still exists and is
    // still sent, because a DAC needs points to keep its buffer fed — sending
    // nothing looks like an underrun, which is itself a fault.
    bool blanked() const { return blanked_; }
    LaserFault faults() const { return faults_; }
    float appliedPower() const { return appliedPower_; }
    int clipped() const { return clipped_; }

private:
    friend class SafetyGate;
    GatedFrame() = default;

    LaserPath points_;
    int zone_ = 0;
    bool blanked_ = true;
    LaserFault faults_ = LaserFault::None;
    float appliedPower_ = 0.f;
    int clipped_ = 0;
};

// Mirrors §3's IOutput pattern for DMX. It cannot be handed raw points.
class ILaserOutput {
public:
    virtual ~ILaserOutput() = default;
    virtual bool open(std::string* err) = 0;
    virtual void send(const GatedFrame& frame) = 0;
    virtual const char* name() const = 0;
    // The DAC's point rate, which the budget is computed against.
    virtual int pointsPerSecond() const = 0;
    // Faults the DAC itself reports: link loss, buffer level, galvo feedback.
    virtual LaserFault status() const = 0;
};

// ---------------------------------------------------------------------------
// Safety gate (§14.6)
// ---------------------------------------------------------------------------
struct SafetyStatus {
    int zone = 0;
    std::string name;
    bool armed = false;
    bool maskActive = false;
    float attenuation = 0.f;
    bool audienceScanning = false;
    LaserFault faults = LaserFault::None;
    bool emergencyStopped = false;
    bool blanking = false;
    int pointRate = 0;
    int pointBudget = 0;
    std::string reason;   // in plain language, for the status panel
};

class SafetyGate {
public:
    SafetyGate() = default;
    // Zones are show objects, so they need §8.1 identities like everything
    // else: a tile, a binding or a cue that referenced a zone by its mutable
    // id would break the moment the id changed. The generator is handed in
    // rather than owned, so every UID in a show comes from one sequence.
    void setUids(UidGen& uids) { uids_ = &uids; }

    void setClock(std::function<double()> clock) { clock_ = std::move(clock); }
    double now() const { return clock_ ? clock_() : 0.0; }

    // --- zones -------------------------------------------------------------
    ZoneSafety& addZone(const std::string& name, Uid uid = Uid{});
    // Re-inserts a zone exactly as saved — its own UID, id and settings. A
    // load must not re-identify what it restores. Arming is NOT restored,
    // whatever the record says: §14.6's fail-safe default is that a show
    // opens with every beam off.
    ZoneSafety& restoreZone(ZoneSafety z);
    ZoneSafety* zone(int id);
    const ZoneSafety* zone(int id) const;
    const std::deque<ZoneSafety>& zones() const { return zones_; }
    bool removeZone(int id);
    // Drops every zone, for a show load replacing them wholesale.
    void clearZones();

    // --- arming (§14.6, §6.1.1) -------------------------------------------
    // Hold-to-fire with confirmation. Two deliberate acts separated by a
    // hold: nothing in the value system can perform either.
    //
    // Once the engine owns this gate (§14.6.1), anything holding an Engine&
    // can reach these — so the arm path carries its own guard. An arm names
    // the PHYSICAL ACT that caused it, and the enum below has no value a
    // cue, macro, binding or agent could honestly pass: there is no
    // `Command`, no `Playback`, no `Script`. A caller that wanted to arm
    // from a cue would have to write a lie, in one place, in full view.
    // That is the §14.6 line — structural, not policy — held at the one
    // boundary where the code can still tell the difference.
    enum class ArmSource : uint8_t {
        Panel,     // a physical arm key on a console or wing
        Pointer,   // a hold on an on-screen arm control, by a present operator
        KeySwitch, // a keyed hardware interlock
    };
    static const char* armSourceName(ArmSource s);

    void setArmHold(double seconds) { armHold_ = std::max(0.0, seconds); }
    double armHold() const { return armHold_; }

    bool beginArm(int zoneId, ArmSource from);
    // Completes only once the hold has elapsed. Returns false and leaves the
    // zone disarmed otherwise — a fumbled press must not arm a laser. The
    // source must MATCH the one that began the hold: starting a hold on a
    // panel key and finishing it from somewhere else is not one act.
    bool confirmArm(int zoneId, ArmSource from);
    void cancelArm(int zoneId);
    // What began the hold, for the action log and the status panel.
    std::optional<ArmSource> armingFrom(int zoneId) const;
    bool disarm(int zoneId);
    void disarmAll();

    // --- emergency stop ----------------------------------------------------
    // Its own path, independent of playback, blackout and grand master. A
    // latch: it clears only by an explicit reset, so a cue cannot release it.
    void emergencyStop();
    bool emergencyStopped() const { return estop_; }
    // Resetting also disarms every zone, so recovering from an E-stop cannot
    // silently restore a beam.
    void resetEmergencyStop();

    // --- faults ------------------------------------------------------------
    void reportFault(int zoneId, LaserFault fault);
    void clearFault(int zoneId, LaserFault fault);
    // Faults the DAC reports, applied to every zone it drives.
    void reportOutputStatus(LaserFault fault);

    // --- the gate itself ---------------------------------------------------
    // The last stage. Everything upstream is a suggestion; this decides.
    GatedFrame process(int zoneId, const LaserPath& path);
    // A zone with nothing to send still needs a frame, or the DAC underruns.
    GatedFrame blankFrame(int zoneId);

    // Marks zones that have not been fed within their stall timeout. Runs on
    // the laser thread, after everything else, so a stall anywhere upstream
    // still results in blanking rather than a held beam (§14.7).
    int sweep();

    SafetyStatus status(int zoneId) const;
    std::vector<SafetyStatus> statuses() const;

    // §14.6 fail-safe defaults: beam off at startup, on show load, on error,
    // on panel-set change, on any unhandled exception. One call for all of
    // them, so a new call site cannot forget half of it.
    void failSafe(const std::string& why);
    const std::string& lastFailSafe() const { return lastFailSafe_; }
    int failSafeCount() const { return failSafes_; }

    void setPointBudget(int pointsPerFrame) { budget_ = std::max(0, pointsPerFrame); }
    int pointBudget() const { return budget_; }

private:
    UidGen* uids_ = nullptr;
    std::function<double()> clock_;
    std::deque<ZoneSafety> zones_;
    bool estop_ = false;
    double armHold_ = 1.0;
    int budget_ = 0;
    std::string lastFailSafe_;
    int failSafes_ = 0;
    int nextId_ = 1;
};

// ---------------------------------------------------------------------------
// Optimiser (§14.4)
// ---------------------------------------------------------------------------
// §14.4 flags this as the genuinely hard part with no analogue in DMX or
// video: galvos have finite acceleration, and feeding them an unoptimised
// path produces distorted figures and can damage the scanners.
struct OptimiserSettings {
    // Longest move, in normalised units, before it is split. A step longer
    // than the galvo can traverse in one sample is what produces distortion.
    float maxStep = 0.05f;
    // A turn sharper than this gets dwell points so the galvo arrives before
    // the beam moves on.
    float cornerAngleDegrees = 35.f;
    int cornerDwell = 3;
    // Blanked travel: points with the beam already off before moving, and
    // after arriving before turning it on.
    int blankBefore = 2;
    int blankAfter = 2;
    // Colour channels lag the galvos; a per-projector sample offset
    // compensates. Positive delays colour relative to position.
    int colourShift = 0;
    // Drops the last point when it duplicates the first, so a closed figure
    // does not spend a sample on a point already drawn.
    bool dedupe = true;
};

struct OptimiseReport {
    LaserPath path;
    int generated = 0;      // points in
    int emitted = 0;        // points out
    int corners = 0;
    int interpolated = 0;
    int blankingPoints = 0;
    int deduped = 0;
};

OptimiseReport optimise(const LaserPath& in, const OptimiserSettings& s);

// The individual stages, exposed because each is worth testing on its own and
// because §14.4 leaves per-zone versus per-projector tuning open.
LaserPath interpolateTravel(const LaserPath& in, float maxStep, int* added = nullptr);
// `closed` makes the path wrap, so the corner at a closed figure's SEAM is
// treated like any other. Without it a square gets dwell on three corners and
// not the fourth, which is a visibly distorted vertex — exactly the class of
// defect §14.4 warns about.
LaserPath addCornerDwell(const LaserPath& in, float angleDegrees, int dwell,
                         int* corners = nullptr, bool closed = false);
LaserPath addBlankingDwell(const LaserPath& in, int before, int after,
                           int* added = nullptr);
LaserPath applyColourShift(const LaserPath& in, int samples);

// The turn angle in degrees at point i. 0 at the ends of an open path; on a
// closed one the ends wrap into each other.
float turnAngleAt(const LaserPath& path, size_t i, bool closed = false);

// First and last points at the same place: the figure comes back to where it
// started, so its seam is a corner rather than two loose ends.
bool isClosedPath(const LaserPath& path, float eps = 1e-5f);

// --- point-rate budgeting --------------------------------------------------
// §14.4: total points across all zones must fit the DAC's pps, or the frame
// rate drops and the figure flickers. Reporting the achievable rate is more
// useful than silently dropping points, so both are available.
struct BudgetReport {
    int totalPoints = 0;
    int pointsPerSecond = 0;
    double requestedFps = 0.0;
    double achievableFps = 0.0;
    bool overBudget = false;
    int budgetPerFrame = 0;
};

BudgetReport budgetPoints(const std::vector<int>& pointsPerZone, int pointsPerSecond,
                          double requestedFps);

// Decimates a path to at most `maxPoints`, keeping the first and last points
// and preserving every blanking transition — dropping one would turn a
// blanked move into a visible line, which is a safety matter and not a
// cosmetic one.
LaserPath decimate(const LaserPath& in, int maxPoints);

} // namespace lumen
