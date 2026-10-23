// test_laser.cpp — the safety gate and the point optimiser (SPEC §14).
//
// §14.8 puts safety before content, so the safety tests come first and are the
// ones written hardest. The structural claim in §14.6 — that nothing upstream
// can bypass the gate — is enforced by the type system, which means part of
// this suite is a set of static_asserts: a GatedFrame cannot be constructed
// outside the gate, so a DAC cannot be handed raw points. Those checks fail at
// COMPILE time, which is the only version of that rule that survives contact
// with future code.
#include "Check.hpp"

#include <string>

#include "lumen/Command.hpp"
#include "lumen/Laser.hpp"

#include <cmath>
#include <cstdio>
#include <type_traits>

using namespace lumen;

// Every existing arming test is about the HOLD, the faults and the gate
// rather than about which surface armed, so they all speak for a panel key.
static constexpr SafetyGate::ArmSource Panel = SafetyGate::ArmSource::Panel;

// ---------------------------------------------------------------------------
// The structural guarantee (§14.6)
// ---------------------------------------------------------------------------
// If any of these ever becomes true, the gate has become bypassable and the
// rest of the safety work is decoration.
static_assert(!std::is_default_constructible_v<GatedFrame>,
              "a GatedFrame must not be constructible outside the safety gate");
static_assert(!std::is_constructible_v<GatedFrame, LaserPath>,
              "a GatedFrame must not be constructible from raw points");
static_assert(!std::is_convertible_v<LaserPath, GatedFrame>,
              "raw points must not convert to a gated frame");
// It does have to travel from the gate to a DAC, so copying and moving stay
// available.
static_assert(std::is_copy_constructible_v<GatedFrame>, "");
static_assert(std::is_move_constructible_v<GatedFrame>, "");

namespace {

double gNow = 0.0;

// The arm sequence, as an operator performs it: hold, wait, confirm.
bool armZone(SafetyGate& gate, int id) {
    if (!gate.beginArm(id, Panel)) return false;
    gNow += gate.armHold() + 0.01;
    return gate.confirmArm(id, Panel);
}

int litPoints(const LaserPath& p) {
    int n = 0;
    for (const LaserPoint& q : p)
        if (!q.blanked) ++n;
    return n;
}

int blankingTransitions(const LaserPath& p) {
    if (p.empty()) return 0;
    int n = 0;
    bool prev = p[0].blanked;
    for (const LaserPoint& q : p) {
        if (q.blanked != prev) ++n;
        prev = q.blanked;
    }
    return n;
}

float longestStep(const LaserPath& p) {
    float worst = 0.f;
    for (size_t i = 1; i < p.size(); ++i) {
        const float dx = p[i].x - p[i - 1].x;
        const float dy = p[i].y - p[i - 1].y;
        worst = std::max(worst, std::sqrt(dx * dx + dy * dy));
    }
    return worst;
}

} // namespace

// ---------------------------------------------------------------------------
// Fail-safe defaults (§14.6)
// ---------------------------------------------------------------------------
static void testAZoneStartsDarkAndStaysDarkUntilArmed() {
    gNow = 0.0;
    SafetyGate gate;
    gate.setClock([] { return gNow; });

    ZoneSafety& z = gate.addZone("Front");
    CHECK(!z.armed);          // beam off at startup
    CHECK(z.mask.enabled);    // masked by default: compliance is the easy path
    CHECK(!z.audienceScanning);

    // A full-power figure through a disarmed zone emits nothing.
    const GatedFrame f = gate.process(z.id, makeSquare());
    CHECK(f.blanked());
    CHECK_NEAR(f.appliedPower(), 0.f);
    CHECK_EQ(litPoints(f.points()), 0);

    const SafetyStatus s = gate.status(z.id);
    CHECK(s.blanking);
    CHECK(!s.armed);
    CHECK(s.reason == "disarmed");   // plain language, not "blanking: true"

    // An unknown zone emits nothing either: a typo in a zone id must not be a
    // route to unmasked output.
    const GatedFrame ghost = gate.process(99, makeSquare());
    CHECK(ghost.blanked());
    CHECK(gate.status(99).blanking);
    CHECK(gate.status(99).reason == "no such zone");

    // A blank frame is still a frame: a DAC starved of points reads as an
    // underrun, which is itself a fault.
    const GatedFrame blank = gate.blankFrame(z.id);
    CHECK(blank.blanked());
    CHECK_EQ(blank.zone(), z.id);
}

static void testArmingIsHoldToFire() {
    gNow = 0.0;
    SafetyGate gate;
    gate.setClock([] { return gNow; });
    gate.setArmHold(1.0);
    const int id = gate.addZone("Front").id;

    // Confirming without beginning does nothing.
    CHECK(!gate.confirmArm(id, Panel));
    CHECK(!gate.zone(id)->armed);

    // Confirming too early does nothing: a fumbled double-press must not arm
    // a laser.
    CHECK(gate.beginArm(id, Panel));
    gNow = 0.5;
    CHECK(!gate.confirmArm(id, Panel));
    CHECK(!gate.zone(id)->armed);

    // Held long enough, it arms.
    gNow = 1.1;
    CHECK(gate.confirmArm(id, Panel));
    CHECK(gate.zone(id)->armed);
    const GatedFrame f = gate.process(id, makeSquare());
    CHECK(!f.blanked());
    CHECK(litPoints(f.points()) > 0);
    CHECK(gate.status(id).reason == "emitting");

    // Cancelling mid-hold abandons it.
    CHECK(gate.disarm(id));
    CHECK(gate.beginArm(id, Panel));
    gate.cancelArm(id);
    gNow = 10.0;
    CHECK(!gate.confirmArm(id, Panel));
    CHECK(!gate.zone(id)->armed);

    CHECK(!gate.beginArm(99, Panel));
    CHECK(!gate.confirmArm(99, Panel));
    CHECK(!gate.disarm(99));
}

static void testACueCannotArmAZone() {
    // The whole point of §14.6. `Arm` exists as a Control attribute so the
    // state is visible and storable like anything else — but the gate's armed
    // state is not a value, and there is no path from the value system into
    // it. If a cue could arm a laser, every other precaution here would be
    // decoration.
    Engine eng;
    Console con(eng);
    eng.setClock(0.0);
    gNow = 0.0;

    SafetyGate gate;
    gate.setClock([] { return gNow; });
    const int id = gate.addZone("Front").id;

    installStarterTypes(eng.patch);
    CHECK(con.execute("Fixture 1 Patch \"Dimmer\"").ok);

    // Drive Arm as hard as the value system allows. Written straight into the
    // programmer layer rather than through `At`, because the patch filters
    // attributes a fixture does not have — and the threat being tested is a
    // value that DOES reach the merged output, not one the patch rejects.
    eng.programmer.values().set(1, 0, Attr::Arm, Value::hard(1.f));
    CHECK_NEAR(eng.programmer.values().value(1, 0, Attr::Arm).value_or(-1.f), 1.f);
    CHECK(con.execute("Store Cue 1 1").ok);
    CHECK(con.execute("Assign 1 1").ok);
    CHECK(con.execute("Go 1").ok);
    eng.tick();
    // It is in the merged output at full value, asserted by a playback.
    const auto merged = eng.merged().get(makeKey(1, 0, Attr::Arm));
    CHECK(merged.has_value());
    CHECK_NEAR(merged.value_or(-1.f), 1.f);

    // And the zone is still disarmed, and still emits nothing.
    CHECK(!gate.zone(id)->armed);
    CHECK(gate.process(id, makeSquare()).blanked());
    CHECK(gate.status(id).reason == "disarmed");

    // Only the deliberate act arms it.
    CHECK(armZone(gate, id));
    CHECK(gate.zone(id)->armed);
}

static void testEmergencyStopIsItsOwnPath() {
    // §14.6: one always-reachable control that blanks all laser output
    // immediately, independent of playback, blackout and grand master.
    gNow = 0.0;
    SafetyGate gate;
    gate.setClock([] { return gNow; });
    const int a = gate.addZone("Front").id;
    const int b = gate.addZone("Rear").id;
    CHECK(armZone(gate, a));
    CHECK(armZone(gate, b));
    CHECK(!gate.process(a, makeSquare()).blanked());
    CHECK(!gate.process(b, makeSquare()).blanked());

    gate.emergencyStop();
    CHECK(gate.emergencyStopped());
    // Every zone, at once.
    CHECK(gate.process(a, makeSquare()).blanked());
    CHECK(gate.process(b, makeSquare()).blanked());
    // And it disarms, so clearing the latch alone cannot restore a beam that
    // was live when the button was hit.
    CHECK(!gate.zone(a)->armed);
    CHECK(!gate.zone(b)->armed);
    CHECK(gate.status(a).reason == "emergency stop");
    CHECK(gate.status(a).emergencyStopped);

    // It is a LATCH: it cannot be armed around.
    CHECK(!gate.beginArm(a, Panel));
    CHECK(!gate.confirmArm(a, Panel));
    CHECK(!armZone(gate, a));
    CHECK(gate.process(a, makeSquare()).blanked());

    // Resetting clears the latch and leaves everything disarmed, so recovery
    // is two deliberate acts rather than one.
    gate.resetEmergencyStop();
    CHECK(!gate.emergencyStopped());
    CHECK(!gate.zone(a)->armed);
    CHECK(gate.process(a, makeSquare()).blanked());
    CHECK(armZone(gate, a));
    CHECK(!gate.process(a, makeSquare()).blanked());
}

static void testFaultsBlankAndDisarm() {
    gNow = 0.0;
    SafetyGate gate;
    gate.setClock([] { return gNow; });
    const int id = gate.addZone("Front").id;
    CHECK(armZone(gate, id));
    CHECK(!gate.process(id, makeSquare()).blanked());

    // A stationary beam at full power is the dangerous case, so every
    // condition that could produce one blanks.
    for (LaserFault fault : {LaserFault::DacDisconnected, LaserFault::BufferUnderrun,
                             LaserFault::StreamStall, LaserFault::RenderStall,
                             LaserFault::GalvoFault}) {
        CHECK(armZone(gate, id));
        CHECK(!gate.process(id, makeSquare()).blanked());
        gate.reportFault(id, fault);
        const GatedFrame f = gate.process(id, makeSquare());
        CHECK(f.blanked());
        CHECK(has(f.faults(), fault));
        // A fault disarms, so recovery needs the fault cleared AND a
        // deliberate re-arm. A beam returning on its own the moment a cable
        // is reseated is exactly what must not happen.
        CHECK(!gate.zone(id)->armed);
        CHECK(!gate.confirmArm(id, Panel));
        gate.clearFault(id, fault);
        CHECK(!gate.zone(id)->armed);   // clearing is not arming
    }

    // Arming is refused while a fault stands.
    gate.reportFault(id, LaserFault::GalvoFault);
    CHECK(gate.beginArm(id, Panel));
    gNow += gate.armHold() + 0.01;
    CHECK(!gate.confirmArm(id, Panel));
    gate.clearFault(id, LaserFault::GalvoFault);
    CHECK(armZone(gate, id));

    // A DAC-reported fault reaches every zone it drives.
    const int other = gate.addZone("Rear").id;
    CHECK(armZone(gate, other));
    gate.reportOutputStatus(LaserFault::DacDisconnected);
    CHECK(!gate.zone(id)->armed);
    CHECK(!gate.zone(other)->armed);
    CHECK(gate.status(id).reason.find("DAC disconnected") != std::string::npos);

    CHECK(describeFaults(LaserFault::None) == "none");
    CHECK(describeFaults(LaserFault::BufferUnderrun | LaserFault::GalvoFault)
              .find("underrun") != std::string::npos);
}

static void testScanFailBlanksOnStall() {
    // §14.7: the gate runs last on the laser thread, so a stall anywhere
    // upstream still results in blanking rather than a held beam. Nothing
    // will arrive to trigger it, so the sweep is what notices.
    gNow = 0.0;
    SafetyGate gate;
    gate.setClock([] { return gNow; });
    ZoneSafety& z = gate.addZone("Front");
    z.stallTimeout = 0.1;
    const int id = z.id;
    CHECK(armZone(gate, id));

    CHECK(!gate.process(id, makeSquare()).blanked());
    CHECK_EQ(gate.sweep(), 0);

    gNow += 0.05;
    CHECK_EQ(gate.sweep(), 0);   // within the timeout

    gNow += 0.2;
    CHECK_EQ(gate.sweep(), 1);
    CHECK(has(gate.zone(id)->faults, LaserFault::StreamStall));
    CHECK(!gate.zone(id)->armed);
    CHECK(gate.process(id, makeSquare()).blanked());
    CHECK_EQ(gate.sweep(), 0);   // reported once, not every pass

    // A zone never fed cannot stall: there is nothing to have stopped.
    const int fresh = gate.addZone("Never used").id;
    gNow += 100.0;
    CHECK_EQ(gate.sweep(), 0);
    CHECK(!any(gate.zone(fresh)->faults));

    // The stall is measured on frames ARRIVING, not on frames being allowed
    // through: a disarmed zone being fed is not stalled.
    const int disarmed = gate.addZone("Disarmed").id;
    gate.zone(disarmed)->stallTimeout = 0.1;
    gate.process(disarmed, makeSquare());
    gNow += 0.05;
    CHECK_EQ(gate.sweep(), 0);
    CHECK(!any(gate.zone(disarmed)->faults));
}

static void testMaskBlanksRatherThanClamps() {
    gNow = 0.0;
    SafetyGate gate;
    gate.setClock([] { return gNow; });
    ZoneSafety& z = gate.addZone("Front");
    z.mask.minX = -0.5f;
    z.mask.maxX = 0.5f;
    z.mask.minY = -0.5f;
    z.mask.maxY = 0.5f;
    const int id = z.id;
    CHECK(armZone(gate, id));

    // A figure wider than the mask: the outside is BLANKED, not clamped to
    // the edge — clamping would draw a line along the mask boundary, which is
    // content nobody asked for.
    LaserPath wide = makeLine(-0.9f, 0.f, 0.9f, 0.f);
    wide = interpolateTravel(wide, 0.05f);
    const GatedFrame f = gate.process(id, wide);
    CHECK(!f.blanked());
    CHECK(f.clipped() > 0);
    for (const LaserPoint& p : f.points()) {
        if (std::fabs(p.x) > 0.5f + 1e-5f) {
            CHECK(p.blanked);
            CHECK_NEAR(p.r, 0.f);
        }
    }
    // Nothing moved: the geometry is untouched, only the beam state.
    CHECK_NEAR(f.points().front().x, -0.9f);
    CHECK_NEAR(f.points().back().x, 0.9f);

    // A figure entirely outside the mask emits nothing at all.
    const GatedFrame out = gate.process(id, makeLine(0.8f, 0.8f, 0.9f, 0.9f));
    CHECK(out.blanked());
    CHECK_EQ(out.clipped(), 2);

    // A disabled mask passes everything.
    gate.zone(id)->mask.enabled = false;
    const GatedFrame open = gate.process(id, wide);
    CHECK_EQ(open.clipped(), 0);
    CHECK(!gate.status(id).maskActive);

    CHECK(LaserMask{}.contains(0.f, 0.f));
    LaserMask off;
    off.enabled = false;
    off.maxX = 0.f;
    CHECK(off.contains(5.f, 5.f));   // disabled means no limit at all
}

static void testPowerIsAppliedAfterEverything() {
    gNow = 0.0;
    SafetyGate gate;
    gate.setClock([] { return gNow; });
    ZoneSafety& z = gate.addZone("Front");
    z.maxPower = 0.25f;
    const int id = z.id;
    CHECK(armZone(gate, id));

    // Content at full power comes out attenuated. Applied after all merging,
    // so no priority (§11.4) can beat it.
    const GatedFrame f = gate.process(id, makeSquare());
    CHECK(!f.blanked());
    CHECK_NEAR(f.appliedPower(), 0.25f);
    for (const LaserPoint& p : f.points())
        if (!p.blanked) CHECK_NEAR(p.r, 0.25f);

    // Zero power is blanking, not a very dim beam.
    gate.zone(id)->maxPower = 0.f;
    CHECK(gate.process(id, makeSquare()).blanked());
    CHECK(gate.status(id).reason == "attenuated to zero");

    // Out-of-range values are clamped rather than trusted.
    gate.zone(id)->maxPower = 5.f;
    const GatedFrame over = gate.process(id, makeSquare());
    CHECK_NEAR(over.appliedPower(), 1.f);
    for (const LaserPoint& p : over.points())
        if (!p.blanked) CHECK(p.r <= 1.f);
}

static void testAudienceScanningNeedsOptInAndCannotBeRaised() {
    // §14.6: audience zones are flagged distinctly, with independent
    // attenuation that is separate from and cannot be raised by the normal
    // intensity path. And audience scanning must never be an ordinary
    // parameter, so it needs an explicit opt-in.
    gNow = 0.0;
    SafetyGate gate;
    gate.setClock([] { return gNow; });
    ZoneSafety& z = gate.addZone("Audience");
    z.audienceScanning = true;
    z.audienceAttenuation = 0.1f;
    const int id = z.id;
    CHECK(!z.audienceOptIn);
    CHECK(armZone(gate, id));

    // Armed, masked, full power — and still nothing, because the opt-in is
    // missing.
    CHECK(gate.process(id, makeSquare()).blanked());
    CHECK(gate.status(id).reason == "audience zone without opt-in");
    CHECK(gate.status(id).audienceScanning);

    gate.zone(id)->audienceOptIn = true;
    const GatedFrame f = gate.process(id, makeSquare());
    CHECK(!f.blanked());
    CHECK_NEAR(f.appliedPower(), 0.1f);

    // The normal intensity path CANNOT raise it: maxPower at full still gives
    // the audience attenuation, because the two multiply rather than compete.
    gate.zone(id)->maxPower = 1.f;
    CHECK_NEAR(gate.process(id, makeSquare()).appliedPower(), 0.1f);
    // And turning maxPower down still lowers it — attenuation only ever
    // reduces.
    gate.zone(id)->maxPower = 0.5f;
    CHECK_NEAR(gate.process(id, makeSquare()).appliedPower(), 0.05f);

    // A non-audience zone at the same settings is unaffected by the audience
    // attenuation, so the flag is what carries the restriction.
    ZoneSafety& normal = gate.addZone("Front");
    normal.audienceAttenuation = 0.1f;
    normal.maxPower = 1.f;
    CHECK(armZone(gate, normal.id));
    CHECK_NEAR(gate.process(normal.id, makeSquare()).appliedPower(), 1.f);
}

static void testPointBudgetIsAFaultNotATruncation() {
    gNow = 0.0;
    SafetyGate gate;
    gate.setClock([] { return gNow; });
    const int id = gate.addZone("Front").id;
    CHECK(armZone(gate, id));
    gate.setPointBudget(100);

    CHECK(!gate.process(id, makeCircle(0.8f, 20)).blanked());

    // Over budget blanks and faults rather than truncating: a half-drawn
    // figure at the wrong rate is a flickering beam, and the caller should
    // have budgeted.
    const GatedFrame over = gate.process(id, makeCircle(0.8f, 400));
    CHECK(over.blanked());
    CHECK(has(over.faults(), LaserFault::PointRateExceeded));
    CHECK(!gate.zone(id)->armed);

    // No budget set means no limit.
    gate.clearFault(id, LaserFault::PointRateExceeded);
    gate.setPointBudget(0);
    CHECK(armZone(gate, id));
    CHECK(!gate.process(id, makeCircle(0.8f, 400)).blanked());
}

static void testFailSafeCoversEveryEntryPoint() {
    // §14.6 lists five occasions for beam-off: startup, show load, error,
    // panel-set change, unhandled exception. One call for all of them, so a
    // new call site cannot implement half of it.
    gNow = 0.0;
    SafetyGate gate;
    gate.setClock([] { return gNow; });
    const int a = gate.addZone("Front").id;
    const int b = gate.addZone("Rear").id;
    CHECK(armZone(gate, a));
    CHECK(armZone(gate, b));

    gate.failSafe("show loaded");
    CHECK(!gate.zone(a)->armed);
    CHECK(!gate.zone(b)->armed);
    CHECK(gate.process(a, makeSquare()).blanked());
    CHECK(gate.lastFailSafe() == "show loaded");
    CHECK_EQ(gate.failSafeCount(), 1);

    gate.failSafe("panel set changed");
    CHECK_EQ(gate.failSafeCount(), 2);
    CHECK(gate.lastFailSafe() == "panel set changed");

    gate.disarmAll();
    CHECK(gate.removeZone(a));
    CHECK(!gate.removeZone(a));
    CHECK_EQ(gate.zones().size(), size_t(1));
    CHECK_EQ(gate.statuses().size(), size_t(1));
}

static void testStatusPanelShowsTheRealReason() {
    // §14.6 asks for visible state, not buried. The reason has to name the
    // actual cause, because "blanking" on its own answers nothing.
    gNow = 0.0;
    SafetyGate gate;
    gate.setClock([] { return gNow; });
    const int id = gate.addZone("Front").id;

    CHECK(gate.status(id).reason == "disarmed");
    CHECK(armZone(gate, id));
    CHECK(gate.status(id).reason == "emitting");
    CHECK(!gate.status(id).blanking);

    gate.reportFault(id, LaserFault::BufferUnderrun);
    CHECK(gate.status(id).reason == "buffer underrun");
    CHECK(gate.status(id).blanking);

    // Emergency stop outranks every other explanation, because it is the one
    // the operator just did.
    gate.emergencyStop();
    CHECK(gate.status(id).reason == "emergency stop");

    gate.resetEmergencyStop();
    gate.clearFault(id, LaserFault::BufferUnderrun);
    CHECK(armZone(gate, id));
    const SafetyStatus s = gate.status(id);
    CHECK(s.name == "Front");
    CHECK(s.armed);
    CHECK(s.maskActive);
    CHECK_NEAR(s.attenuation, 1.f);
    CHECK(!s.audienceScanning);
}

// ---------------------------------------------------------------------------
// Optimiser (§14.4)
// ---------------------------------------------------------------------------
static void testTurnAngles() {
    const LaserPath sq = makeSquare(1.f);
    CHECK_NEAR(turnAngleAt(sq, 0), 0.f);          // an open end has no turn
    CHECK_NEAR(turnAngleAt(sq, 1), 90.f);
    CHECK_NEAR(turnAngleAt(sq, 2), 90.f);
    CHECK_NEAR(turnAngleAt(sq, 3), 90.f);
    CHECK_NEAR(turnAngleAt(sq, 4), 0.f);

    // A straight line turns nowhere.
    LaserPath straight = makeLine(-1.f, 0.f, 1.f, 0.f);
    straight = interpolateTravel(straight, 0.1f);
    for (size_t i = 0; i < straight.size(); ++i) CHECK_NEAR(turnAngleAt(straight, i), 0.f);

    // A reversal is 180 degrees.
    LaserPath back;
    back.push_back(LaserPoint::lit(0.f, 0.f, 1, 1, 1));
    back.push_back(LaserPoint::lit(0.5f, 0.f, 1, 1, 1));
    back.push_back(LaserPoint::lit(0.f, 0.f, 1, 1, 1));
    CHECK_NEAR(turnAngleAt(back, 1), 180.f);

    // A repeated point has no direction, so it reports no turn rather than a
    // NaN from a zero-length leg.
    LaserPath dup;
    dup.push_back(LaserPoint::lit(0.f, 0.f, 1, 1, 1));
    dup.push_back(LaserPoint::lit(0.f, 0.f, 1, 1, 1));
    dup.push_back(LaserPoint::lit(1.f, 0.f, 1, 1, 1));
    CHECK_NEAR(turnAngleAt(dup, 1), 0.f);
    CHECK(!std::isnan(turnAngleAt(dup, 1)));

    // Closure makes the seam a corner like any other.
    CHECK(isClosedPath(sq));
    CHECK(!isClosedPath(makeLine(0.f, 0.f, 1.f, 1.f)));
    LaserPath open = sq;
    open.pop_back();
    CHECK(!isClosedPath(open));
    CHECK_NEAR(turnAngleAt(open, 0, true), 90.f);   // wrapped
    CHECK_NEAR(turnAngleAt(open, 0, false), 0.f);
    CHECK_NEAR(turnAngleAt(open, 3, true), 90.f);
}

static void testCornerDwellIncludingTheSeam() {
    // A square has FOUR corners. Dedupe removes the closing point, so without
    // closure handling the seam gets no dwell — three treated corners and one
    // visibly distorted vertex, which is the defect §14.4 warns about.
    OptimiserSettings s;
    const OptimiseReport r = optimise(makeSquare(0.8f), s);
    CHECK_EQ(r.corners, 4);
    CHECK_EQ(r.deduped, 1);

    // A smooth circle has none.
    CHECK_EQ(optimise(makeCircle(0.8f, 64), s).corners, 0);
    // A coarse one is all corners: eight 45-degree turns, seam included.
    CHECK_EQ(optimise(makeCircle(0.8f, 8), s).corners, 8);

    // The dwell is REPEATED points at the corner, not a slower approach: the
    // beam must wait AT the vertex for the galvo to arrive.
    int corners = 0;
    LaserPath open = makeSquare(0.8f);
    open.pop_back();
    const LaserPath dwelt = addCornerDwell(open, 35.f, 3, &corners, true);
    CHECK_EQ(corners, 4);
    CHECK_EQ(dwelt.size(), open.size() + size_t(4 * 3));
    int repeats = 0;
    for (size_t i = 1; i < dwelt.size(); ++i)
        if (dwelt[i].samePositionAs(dwelt[i - 1])) ++repeats;
    CHECK_EQ(repeats, 12);

    // A blanked corner needs no dwell: the beam is off, so the galvo can take
    // as long as it likes.
    LaserPath blankCorner;
    blankCorner.push_back(LaserPoint::dark(-0.5f, 0.f));
    blankCorner.push_back(LaserPoint::dark(0.f, 0.f));
    blankCorner.push_back(LaserPoint::dark(0.f, 0.5f));
    int none = 0;
    addCornerDwell(blankCorner, 35.f, 3, &none);
    CHECK_EQ(none, 0);

    // Turned off, it is a pass-through.
    int off = 0;
    CHECK_EQ(addCornerDwell(open, 35.f, 0, &off).size(), open.size());
    CHECK_EQ(off, 0);
    CHECK(addCornerDwell({}, 35.f, 3).empty());
}

static void testBlankingDwellIsDarkBeforeItTravels() {
    // §14.4: points at the start and end of a blanked move, so the beam is
    // off BEFORE travel and on only AFTER arrival. Getting this wrong draws a
    // visible line between two figures.
    LaserPath fig;
    fig.push_back(LaserPoint::lit(-0.5f, -0.5f, 1, 1, 1));
    fig.push_back(LaserPoint::lit(-0.1f, -0.5f, 1, 1, 1));
    fig.push_back(LaserPoint::dark(0.5f, 0.5f));
    fig.push_back(LaserPoint::lit(0.9f, 0.5f, 1, 1, 1));

    int added = 0;
    const LaserPath out = addBlankingDwell(fig, 2, 2, &added);
    CHECK_EQ(added, 6);   // 2 at the start, 2 before travel, 2 after arrival

    // The dwell before travel sits at the PREVIOUS position, already dark.
    bool foundPreTravel = false;
    for (size_t i = 0; i + 1 < out.size(); ++i) {
        if (!out[i].blanked) continue;
        if (!out[i].samePositionAs(fig[1])) continue;
        foundPreTravel = true;
    }
    CHECK(foundPreTravel);

    // The dwell after arrival sits at the NEW position, still dark.
    bool foundPostArrival = false;
    for (size_t i = 0; i < out.size(); ++i)
        if (out[i].blanked && out[i].samePositionAs(fig[3])) foundPostArrival = true;
    CHECK(foundPostArrival);

    // Every blanked point carries no colour, whatever the content said.
    for (const LaserPoint& p : out)
        if (p.blanked) {
            CHECK_NEAR(p.r, 0.f);
            CHECK_NEAR(p.g, 0.f);
            CHECK_NEAR(p.b, 0.f);
        }

    // The travel itself stays dark the whole way once interpolated — a lit
    // intermediate point would be a visible line between the two figures.
    const OptimiseReport r = optimise(fig, OptimiserSettings{});
    for (const LaserPoint& p : r.path) {
        const bool inTravel = p.x > -0.05f && p.x < 0.45f && p.y > -0.45f && p.y < 0.45f;
        if (inTravel) CHECK(p.blanked);
    }
    CHECK_EQ(blankingTransitions(r.path), 3);

    CHECK(addBlankingDwell({}, 2, 2).empty());
    CHECK_EQ(addBlankingDwell(fig, 0, 0).size(), fig.size());
}

static void testTravelInterpolationRespectsTheStepLimit() {
    // A step longer than the galvo can traverse in one sample is what
    // produces distortion, so no step may exceed the limit.
    const OptimiserSettings s;
    for (const LaserPath& figure : {makeSquare(1.6f), makeCircle(0.9f, 12),
                                    makeLine(-1.f, -1.f, 1.f, 1.f)}) {
        const OptimiseReport r = optimise(figure, s);
        CHECK(longestStep(r.path) <= s.maxStep + 1e-5f);
        CHECK(r.emitted > r.generated);
    }

    int added = 0;
    const LaserPath line = interpolateTravel(makeLine(0.f, 0.f, 1.f, 0.f), 0.25f,
                                             &added);
    CHECK_EQ(line.size(), size_t(5));   // 0, .25, .5, .75, 1
    CHECK_EQ(added, 3);
    CHECK_NEAR(line[1].x, 0.25f);
    CHECK_NEAR(line[2].x, 0.5f);

    // Intermediate points take the beam state of the move they belong to, so
    // a blanked move stays blanked the whole way.
    LaserPath jump;
    jump.push_back(LaserPoint::lit(0.f, 0.f, 1, 1, 1));
    jump.push_back(LaserPoint::dark(1.f, 0.f));
    const LaserPath split = interpolateTravel(jump, 0.25f);
    CHECK_EQ(litPoints(split), 1);
    for (size_t i = 1; i < split.size(); ++i) CHECK(split[i].blanked);

    // A step already short enough is left alone, and a zero limit is a
    // pass-through rather than an infinite loop.
    CHECK_EQ(interpolateTravel(makeLine(0.f, 0.f, 0.1f, 0.f), 0.5f).size(), size_t(2));
    CHECK_EQ(interpolateTravel(jump, 0.f).size(), jump.size());
    CHECK(interpolateTravel({}, 0.1f).empty());
}

static void testColourShiftLeadsWithDarkness() {
    // Colour channels lag the galvos, so a sample offset compensates. The
    // leading gap has no authored colour — and emitting the first sample's
    // colour there would light a position that should be dark, so it emits
    // nothing instead.
    LaserPath p;
    for (int i = 0; i < 6; ++i)
        p.push_back(LaserPoint::lit(static_cast<float>(i) * 0.1f, 0.f,
                                    static_cast<float>(i) / 5.f, 0.f, 0.f));

    const LaserPath shifted = applyColourShift(p, 2);
    CHECK_EQ(shifted.size(), p.size());
    // The leading two samples emit nothing.
    CHECK(shifted[0].blanked);
    CHECK(shifted[1].blanked);
    CHECK_NEAR(shifted[0].r, 0.f);
    // From there the colour is what was authored two samples earlier.
    CHECK(!shifted[2].blanked);
    CHECK_NEAR(shifted[2].r, p[0].r);
    CHECK_NEAR(shifted[3].r, p[1].r);
    CHECK_NEAR(shifted[5].r, p[3].r);
    // Positions are untouched: only the beam moves in time, not the galvo.
    for (size_t i = 0; i < p.size(); ++i) CHECK_NEAR(shifted[i].x, p[i].x);

    // Blanking shifts WITH the colour: it is a property of the beam, and
    // leaving it behind would light a travel move.
    LaserPath mixed;
    mixed.push_back(LaserPoint::lit(0.f, 0.f, 1, 1, 1));
    mixed.push_back(LaserPoint::dark(0.5f, 0.f));
    mixed.push_back(LaserPoint::lit(1.f, 0.f, 1, 1, 1));
    const LaserPath ms = applyColourShift(mixed, 1);
    CHECK(ms[0].blanked);
    CHECK(!ms[1].blanked);   // sample 0's lit beam, one sample later
    CHECK(ms[2].blanked);    // sample 1's blanking, one sample later

    CHECK_EQ(applyColourShift(p, 0).size(), p.size());
    CHECK_NEAR(applyColourShift(p, 0)[3].r, p[3].r);
    CHECK(applyColourShift({}, 2).empty());
}

static void testBudgetReportsWhatWillActuallyHappen() {
    // §14.4: total points across all zones must fit the DAC's pps, or the
    // frame rate drops and the figure flickers. Reporting the achievable rate
    // is more useful than silently dropping points.
    BudgetReport r = budgetPoints({1200, 800, 400}, 30000, 30.0);
    CHECK_EQ(r.totalPoints, 2400);
    CHECK_EQ(r.budgetPerFrame, 1000);
    CHECK(r.overBudget);
    CHECK_NEAR(r.achievableFps, 12.5);

    // Within budget, the requested rate is achievable and nothing is flagged.
    r = budgetPoints({300, 200}, 30000, 30.0);
    CHECK_EQ(r.totalPoints, 500);
    CHECK(!r.overBudget);
    CHECK_NEAR(r.achievableFps, 30.0);

    // Degenerate inputs answer rather than dividing by zero.
    r = budgetPoints({}, 30000, 30.0);
    CHECK_EQ(r.totalPoints, 0);
    CHECK(!r.overBudget);
    r = budgetPoints({100}, 0, 30.0);
    CHECK(r.overBudget);
    CHECK_NEAR(r.achievableFps, 0.0);
    r = budgetPoints({100}, 30000, 0.0);
    CHECK(r.overBudget);
    r = budgetPoints({-5, 10}, 30000, 30.0);
    CHECK_EQ(r.totalPoints, 10);
}

static void testDecimateNeverLosesABlankingTransition() {
    // Dropping a blanking transition turns a blanked move into a visible
    // line, which is a safety matter rather than a cosmetic one.
    LaserPath fig;
    fig.push_back(LaserPoint::lit(-0.5f, -0.5f, 1, 1, 1));
    fig.push_back(LaserPoint::lit(-0.1f, -0.5f, 1, 1, 1));
    fig.push_back(LaserPoint::dark(0.5f, 0.5f));
    fig.push_back(LaserPoint::lit(0.9f, 0.5f, 1, 1, 1));
    const LaserPath full = optimise(fig, OptimiserSettings{}).path;
    const int transitions = blankingTransitions(full);
    CHECK_EQ(transitions, 3);

    for (int target : {60, 40, 20, 10, 6, 5}) {
        const LaserPath d = decimate(full, target);
        CHECK(static_cast<int>(d.size()) <= std::max(target,
                                                     blankingTransitions(full) + 2));
        CHECK_EQ(blankingTransitions(d), transitions);
        // The ends are kept, so the figure still starts and finishes where it
        // should.
        CHECK(d.front().samePositionAs(full.front()));
        CHECK(d.back().samePositionAs(full.back()));
    }

    // Nothing to do, and degenerate targets.
    CHECK_EQ(decimate(full, 10000).size(), full.size());
    CHECK_EQ(decimate(full, 1).size(), size_t(1));
    CHECK(decimate(full, 0).empty());
    CHECK(decimate({}, 10).empty());
}

static void testOptimiseIsIdempotentOnItsOwnOutput() {
    // Running the optimiser twice must not keep growing the path: a pipeline
    // that is not stable under re-entry drifts over a long show.
    const OptimiserSettings s;
    const OptimiseReport first = optimise(makeSquare(0.8f), s);
    const OptimiseReport second = optimise(first.path, s);

    // The step limit still holds, and the second pass adds no corners —
    // dwell points are repeats, which have no direction and therefore no turn.
    CHECK(longestStep(second.path) <= s.maxStep + 1e-5f);
    CHECK_EQ(second.corners, 0);
    // EXACTLY stable, not merely bounded. A leg sitting on the step limit
    // measures a hair above it in floats, and blanking dwell can be re-added
    // at a transition that already has it; either one makes a chained
    // optimiser grow the path on every pass.
    CHECK_EQ(second.emitted, first.emitted);
    CHECK_EQ(second.blankingPoints, 0);
    CHECK_EQ(second.interpolated, 0);
    const OptimiseReport third = optimise(second.path, s);
    CHECK_EQ(third.emitted, first.emitted);

    const OptimiseReport empty = optimise({}, s);
    CHECK_EQ(empty.emitted, 0);
    CHECK(empty.path.empty());

    // A single lit point is a legal, if pointless, figure. It comes out as
    // three: a figure that STARTS lit still needs the galvo parked before the
    // beam lights, so the leading dark dwell is correct rather than noise.
    LaserPath one{LaserPoint::lit(0.f, 0.f, 1, 1, 1)};
    const OptimiseReport single = optimise(one, s);
    CHECK_EQ(single.emitted, 3);
    CHECK_EQ(single.corners, 0);
    CHECK(single.path[0].blanked);
    CHECK(single.path[1].blanked);
    CHECK(!single.path[2].blanked);
}

static void testTheWholePipelineEndsAtTheGate() {
    // Generate, optimise, gate. The gate is last, and what it says is final.
    gNow = 0.0;
    SafetyGate gate;
    gate.setClock([] { return gNow; });
    ZoneSafety& z = gate.addZone("Front");
    z.maxPower = 0.5f;
    z.mask.minX = -0.5f;
    z.mask.maxX = 0.5f;
    z.mask.minY = -0.5f;
    z.mask.maxY = 0.5f;
    const int id = z.id;

    // A figure that STRADDLES the mask: a square of side 1.6 runs along
    // x=+/-0.8 and would be entirely outside, which tests nothing.
    LaserPath figure = makeLine(-0.9f, 0.f, 0.9f, 0.f);
    for (const LaserPoint& p : makeLine(0.f, -0.9f, 0.f, 0.9f)) figure.push_back(p);
    const OptimiseReport opt = optimise(figure, OptimiserSettings{});
    CHECK(opt.emitted > 0);

    // Disarmed: the optimiser's work reaches nothing.
    CHECK(gate.process(id, opt.path).blanked());

    CHECK(armZone(gate, id));
    const GatedFrame f = gate.process(id, opt.path);
    CHECK(!f.blanked());
    CHECK_NEAR(f.appliedPower(), 0.5f);
    CHECK(f.clipped() > 0);          // the figure runs past the mask
    CHECK_EQ(f.zone(), id);

    // Every lit point is inside the mask and at the attenuated level. This is
    // the invariant the whole file exists to protect.
    for (const LaserPoint& p : f.points()) {
        if (p.blanked) continue;
        CHECK(std::fabs(p.x) <= 0.5f + 1e-5f);
        CHECK(std::fabs(p.y) <= 0.5f + 1e-5f);
        CHECK(p.r <= 0.5f + 1e-5f);
    }

    // And one emergency stop takes all of it away.
    gate.emergencyStop();
    const GatedFrame stopped = gate.process(id, opt.path);
    CHECK(stopped.blanked());
    CHECK_EQ(litPoints(stopped.points()), 0);
}


// §14.6.1: once the engine owns the gate, anything holding an Engine& can
// reach the arm path, so the arm path carries its own guard. The enum has no
// value a cue, macro or agent could honestly pass — and a hold is ONE act
// from start to finish.
// A zone is a show object and needs a §8.1 identity like everything else: a
// tile, a binding or a cue referencing it by its mutable id would break the
// moment that id changed — and without one, two zones collide on save.
static void testZonesHaveIdentities() {
    UidGen uids;
    SafetyGate gate;
    gate.setUids(uids);

    const ZoneSafety& a = gate.addZone("House");
    const ZoneSafety& b = gate.addZone("Audience");
    CHECK(a.uid.valid());
    CHECK(b.uid.valid());
    CHECK(!(a.uid == b.uid));
    CHECK(a.id != b.id);

    // An explicit UID is honoured, which is what a load needs.
    const Uid given = uids.next();
    const ZoneSafety& c = gate.addZone("Given", given);
    CHECK(c.uid == given);

    // A restored zone keeps everything but its arming (§14.6).
    ZoneSafety saved;
    saved.uid = uids.next();
    saved.id = 42;
    saved.name = "Restored";
    saved.armed = true;
    saved.armHolding = true;
    saved.maxPower = 0.3f;
    const ZoneSafety& back = gate.restoreZone(saved);
    CHECK(back.id == 42);
    CHECK(back.name == "Restored");
    CHECK_NEAR(back.maxPower, 0.3f);
    CHECK(!back.armed);
    CHECK(!back.armHolding);
    // A later zone must not reuse the restored id.
    CHECK(gate.addZone("After").id > 42);
}

static void testArmingNamesItsSource() {
    SafetyGate gate;
    double t = 0.0;
    gate.setClock([&t] { return t; });
    gate.setArmHold(1.0);
    const int z = gate.addZone("House").id;

    using Src = SafetyGate::ArmSource;

    // A hold begun on a panel key cannot be finished from an on-screen
    // control. Two halves of two different gestures is not one gesture, and
    // accepting it would let either half be synthesised.
    CHECK(gate.beginArm(z, Src::Panel));
    CHECK(gate.armingFrom(z).value() == Src::Panel);
    t = 2.0;
    CHECK(!gate.confirmArm(z, Src::Pointer));
    CHECK(!gate.zone(z)->armed);
    // The same act completes.
    CHECK(gate.confirmArm(z, Src::Panel));
    CHECK(gate.zone(z)->armed);
    CHECK(gate.zone(z)->armedFrom == static_cast<uint8_t>(Src::Panel));

    // The source is recorded for the audit trail, so "who armed this" is
    // answerable after the fact.
    gate.disarm(z);
    t = 10.0;
    CHECK(gate.beginArm(z, Src::KeySwitch));
    t = 12.0;
    CHECK(gate.confirmArm(z, Src::KeySwitch));
    CHECK(gate.zone(z)->armedFrom == static_cast<uint8_t>(Src::KeySwitch));

    // Nothing about the source weakens the rules that were already there:
    // the hold still has to elapse, and an E-stop still cannot be armed
    // around.
    gate.disarm(z);
    t = 20.0;
    CHECK(gate.beginArm(z, Src::Pointer));
    t = 20.5;
    CHECK(!gate.confirmArm(z, Src::Pointer));   // hold not elapsed

    gate.emergencyStop();
    t = 30.0;
    CHECK(!gate.beginArm(z, Src::Panel));
    CHECK(!gate.confirmArm(z, Src::Panel));

    // And every name round-trips, so an audit record cannot say "?".
    for (Src srcv : {Src::Panel, Src::Pointer, Src::KeySwitch})
        CHECK(std::string(SafetyGate::armSourceName(srcv)) != "?");
}

int main() {
    std::printf("lumen laser\n");
    testZonesHaveIdentities();
    testArmingNamesItsSource();
    testAZoneStartsDarkAndStaysDarkUntilArmed();
    testArmingIsHoldToFire();
    testACueCannotArmAZone();
    testEmergencyStopIsItsOwnPath();
    testFaultsBlankAndDisarm();
    testScanFailBlanksOnStall();
    testMaskBlanksRatherThanClamps();
    testPowerIsAppliedAfterEverything();
    testAudienceScanningNeedsOptInAndCannotBeRaised();
    testPointBudgetIsAFaultNotATruncation();
    testFailSafeCoversEveryEntryPoint();
    testStatusPanelShowsTheRealReason();
    testTurnAngles();
    testCornerDwellIncludingTheSeam();
    testBlankingDwellIsDarkBeforeItTravels();
    testTravelInterpolationRespectsTheStepLimit();
    testColourShiftLeadsWithDarkness();
    testBudgetReportsWhatWillActuallyHappen();
    testDecimateNeverLosesABlankingTransition();
    testOptimiseIsIdempotentOnItsOwnOutput();
    testTheWholePipelineEndsAtTheGate();
    return check::report("laser");
}
