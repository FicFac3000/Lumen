// Tests for tracking resolution, Block, Cue Only, shield and timing.
#include "Check.hpp"

#include "lumen/Cue.hpp"

#include <cmath>

using namespace lumen;

namespace {

// Builds a four-cue sequence on one fixture:
//   cue 1: dimmer 100%, red 100%
//   cue 2: pan 50%                     (dimmer and red must TRACK through)
//   cue 3: dimmer 50%
//   cue 4: (empty)                     (everything tracks through)
Sequence buildSeq() {
    Sequence s;
    s.id = 1;
    s.cues.resize(4);
    for (int i = 0; i < 4; ++i) s.cues[static_cast<size_t>(i)].number = static_cast<float>(i + 1);

    s.cues[0].values().set(1, 0, Attr::Dimmer, Value::hard(1.0f));
    s.cues[0].values().set(1, 0, Attr::Red, Value::hard(1.0f));
    s.cues[1].values().set(1, 0, Attr::Pan, Value::hard(0.5f));
    s.cues[2].values().set(1, 0, Attr::Dimmer, Value::hard(0.5f));
    return s;
}

float at(const Resolved& r, Attr a) {
    const Value* v = r.values.find(makeKey(1, 0, a));
    return v ? v->v : -1.f;
}

} // namespace

static void testTrackingForward() {
    Sequence s = buildSeq();

    // Cue 1 holds what it stored.
    Resolved r0 = resolveTracking(s, 0);
    CHECK_NEAR(at(r0, Attr::Dimmer), 1.0);
    CHECK_NEAR(at(r0, Attr::Red), 1.0);
    CHECK_EQ(r0.values.size(), size_t(2));

    // Cue 2 stores only Pan, so Dimmer and Red TRACK through. This is the
    // whole point: a tracking cue's own layer is not the stage state.
    Resolved r1 = resolveTracking(s, 1);
    CHECK_NEAR(at(r1, Attr::Dimmer), 1.0);
    CHECK_NEAR(at(r1, Attr::Red), 1.0);
    CHECK_NEAR(at(r1, Attr::Pan), 0.5);
    CHECK_EQ(r1.values.size(), size_t(3));
    CHECK_EQ(s.cues[1].values().size(), size_t(1));  // but it stored only one

    // Cue 3 changes Dimmer; Red and Pan keep tracking.
    Resolved r2 = resolveTracking(s, 2);
    CHECK_NEAR(at(r2, Attr::Dimmer), 0.5);
    CHECK_NEAR(at(r2, Attr::Red), 1.0);
    CHECK_NEAR(at(r2, Attr::Pan), 0.5);

    // Cue 4 stores nothing at all, so the stage is unchanged from cue 3.
    Resolved r3 = resolveTracking(s, 3);
    CHECK_NEAR(at(r3, Attr::Dimmer), 0.5);
    CHECK_NEAR(at(r3, Attr::Red), 1.0);
    CHECK_NEAR(at(r3, Attr::Pan), 0.5);
    CHECK_EQ(s.cues[3].values().size(), size_t(0));
}

static void testUpstreamEditTracks() {
    Sequence s = buildSeq();

    // Editing cue 1's colour must reach cue 4, because nothing blocks it.
    s.cues[0].values().set(1, 0, Attr::Red, Value::hard(0.25f));
    CHECK_NEAR(at(resolveTracking(s, 3), Attr::Red), 0.25);

    // Blocking Colour at cue 2 stops it there and leaves everything after it
    // on the pre-edit value.
    Sequence b = buildSeq();
    blockGroup(b, 1, Group::Color);
    CHECK(b.cues[1].trackMode(Group::Color) == TrackMode::Block);
    CHECK(b.cues[1].values().has(1, 0, Attr::Red));  // materialised a hard value

    b.cues[0].values().set(1, 0, Attr::Red, Value::hard(0.25f));
    CHECK_NEAR(at(resolveTracking(b, 0), Attr::Red), 0.25);  // the edit itself
    CHECK_NEAR(at(resolveTracking(b, 1), Attr::Red), 1.0);   // blocked here
    CHECK_NEAR(at(resolveTracking(b, 3), Attr::Red), 1.0);   // and downstream

    // Blocking only Colour must leave other groups tracking freely.
    b.cues[0].values().set(1, 0, Attr::Dimmer, Value::hard(0.1f));
    CHECK_NEAR(at(resolveTracking(b, 1), Attr::Dimmer), 0.1);
}

static void testPerGroupMasks() {
    // §11.1: tracking is per (cue, group) — one cue can track Position while
    // blocking Colour. That combination is the point of the design.
    Sequence s = buildSeq();
    blockGroup(s, 1, Group::Color);
    s.cues[1].setTrackMode(Group::Position, TrackMode::Track);

    CHECK(s.cues[1].trackMode(Group::Color) == TrackMode::Block);
    CHECK(s.cues[1].trackMode(Group::Position) == TrackMode::Track);
    CHECK(s.cues[1].trackMode(Group::Intensity) == TrackMode::Track);

    // Colour is pinned, position still flows.
    s.cues[0].values().set(1, 0, Attr::Red, Value::hard(0.f));
    CHECK_NEAR(at(resolveTracking(s, 2), Attr::Red), 1.0);
    CHECK_NEAR(at(resolveTracking(s, 2), Attr::Pan), 0.5);
}

static void testCueOnly() {
    Sequence s = buildSeq();

    // A CueOnly change applies at its own cue and the NEXT cue gets the
    // pre-change value back — distinct from Block, which pins it forward.
    s.cues[1].setTrackMode(Group::Intensity, TrackMode::CueOnly);
    s.cues[1].values().set(1, 0, Attr::Dimmer, Value::hard(0.2f));

    CHECK_NEAR(at(resolveTracking(s, 1), Attr::Dimmer), 0.2);  // applies here
    // Cue 3 stores its own dimmer, so check cue 4 for the inheritance.
    Sequence t = buildSeq();
    t.cues[2].values().removeKey(makeKey(1, 0, Attr::Dimmer));  // let it track
    t.cues[1].setTrackMode(Group::Intensity, TrackMode::CueOnly);
    t.cues[1].values().set(1, 0, Attr::Dimmer, Value::hard(0.2f));

    CHECK_NEAR(at(resolveTracking(t, 1), Attr::Dimmer), 0.2);  // applies
    CHECK_NEAR(at(resolveTracking(t, 2), Attr::Dimmer), 1.0);  // reverted
    CHECK_NEAR(at(resolveTracking(t, 3), Attr::Dimmer), 1.0);  // stays reverted

    // CueOnly on one group does not affect another.
    CHECK_NEAR(at(resolveTracking(t, 2), Attr::Red), 1.0);
}

static void testShield() {
    Sequence s = buildSeq();

    // §11.1 tracking shield: no upstream edit reaches a shielded cue, whatever
    // the group masks say. For the one cue that must look right.
    s.cues[2].shield = true;
    s.cues[2].values().set(1, 0, Attr::Red, Value::hard(0.75f));
    s.cues[2].values().set(1, 0, Attr::Pan, Value::hard(0.9f));

    Resolved r = resolveTracking(s, 2);
    CHECK_NEAR(at(r, Attr::Dimmer), 0.5);  // its own stored value
    CHECK_NEAR(at(r, Attr::Red), 0.75);
    CHECK_NEAR(at(r, Attr::Pan), 0.9);
    CHECK_EQ(r.values.size(), size_t(3));  // nothing inherited from upstream

    // Changing cue 1 cannot reach it.
    s.cues[0].values().set(1, 0, Attr::Red, Value::hard(0.f));
    CHECK_NEAR(at(resolveTracking(s, 2), Attr::Red), 0.75);

    // A shield also stops anything upstream leaking past it to later cues.
    Resolved r3 = resolveTracking(s, 3);
    CHECK_NEAR(at(r3, Attr::Red), 0.75);
}

static void testUnblock() {
    Sequence s = buildSeq();
    blockGroup(s, 1, Group::Color);
    CHECK(s.cues[1].values().has(1, 0, Attr::Red));

    // Unblock removes values that merely duplicate what would track in anyway,
    // returning the cue to storing only changes.
    unblockGroup(s, 1, Group::Color);
    CHECK(s.cues[1].trackMode(Group::Color) == TrackMode::Track);
    CHECK(!s.cues[1].values().has(1, 0, Attr::Red));

    // And the upstream edit flows again.
    s.cues[0].values().set(1, 0, Attr::Red, Value::hard(0.3f));
    CHECK_NEAR(at(resolveTracking(s, 3), Attr::Red), 0.3);

    // Unblock must not discard a genuine change that happens to be in the
    // blocked group.
    Sequence t = buildSeq();
    t.cues[1].values().set(1, 0, Attr::Red, Value::hard(0.42f));
    blockGroup(t, 1, Group::Color);
    unblockGroup(t, 1, Group::Color);
    CHECK_NEAR(at(resolveTracking(t, 1), Attr::Red), 0.42);
}

static void testOrigin() {
    Sequence s = buildSeq();

    // §11.10 wants a tracked value's origin: "72% tracked from Cue 12". The
    // resolution pass already walks back to find it, so it costs nothing.
    Resolved r = resolveTracking(s, 3, true);
    CHECK_EQ(r.originCue[makeKey(1, 0, Attr::Red)], 0);     // set in cue 1
    CHECK_EQ(r.originCue[makeKey(1, 0, Attr::Pan)], 1);     // cue 2
    CHECK_EQ(r.originCue[makeKey(1, 0, Attr::Dimmer)], 2);  // cue 3

    // Origin is only populated when asked for.
    Resolved plain = resolveTracking(s, 3, false);
    CHECK_EQ(plain.originCue.size(), size_t(0));
}

static void testTiming() {
    Cue c;
    c.timing.fadeIn = 3.0;

    // Precedence: attribute override, then group override, then cue (§11.2).
    CHECK_NEAR(c.timingFor(Attr::Pan).fadeIn, 3.0);

    Timing posT;
    posT.fadeIn = 8.0;
    c.groupTiming[static_cast<uint8_t>(Group::Position)] = posT;
    CHECK_NEAR(c.timingFor(Attr::Pan).fadeIn, 8.0);   // group override
    CHECK_NEAR(c.timingFor(Attr::Red).fadeIn, 3.0);   // other groups unaffected

    Timing tiltT;
    tiltT.fadeIn = 1.5;
    c.attrTiming[static_cast<uint16_t>(Attr::Tilt)] = tiltT;
    CHECK_NEAR(c.timingFor(Attr::Tilt).fadeIn, 1.5);  // attribute wins
    CHECK_NEAR(c.timingFor(Attr::Pan).fadeIn, 8.0);   // sibling still group

    // Parts carry their own timing, inheriting the cue's when unset.
    c.parts.push_back(CuePart{});
    CHECK_NEAR(c.timingForPart(1).fadeIn, 3.0);
    Timing partT;
    partT.fadeIn = 12.0;
    c.parts[1].timing = partT;
    CHECK_NEAR(c.timingForPart(1).fadeIn, 12.0);
    CHECK_NEAR(c.timingForPart(0).fadeIn, 3.0);

    // Later parts win on a key collision, matching part order on stage.
    c.parts[0].values.set(1, 0, Attr::Dimmer, Value::hard(0.2f));
    c.parts[1].values.set(1, 0, Attr::Dimmer, Value::hard(0.8f));
    c.parts[1].values.set(2, 0, Attr::Dimmer, Value::hard(0.4f));
    const ValueLayer all = c.allValues();
    CHECK_EQ(all.size(), size_t(2));
    CHECK_NEAR(all.find(makeKey(1, 0, Attr::Dimmer))->v, 0.8);
}

static void testFadeCurves() {
    // Endpoints are fixed for every curve, so a fade always completes.
    for (auto c : {FadeCurve::Linear, FadeCurve::SCurve, FadeCurve::EaseIn,
                   FadeCurve::EaseOut}) {
        CHECK_NEAR(applyCurve(0.f, c), 0.0);
        CHECK_NEAR(applyCurve(1.f, c), 1.0);
        // Monotonic, which a fade must be.
        float prev = -1.f;
        for (int k = 0; k <= 20; ++k) {
            const float v = applyCurve(static_cast<float>(k) / 20.f, c);
            CHECK(v >= prev - 1e-5f);
            prev = v;
        }
    }
    CHECK_NEAR(applyCurve(0.5f, FadeCurve::Linear), 0.5);
    CHECK_NEAR(applyCurve(0.5f, FadeCurve::SCurve), 0.5);
    CHECK(applyCurve(0.25f, FadeCurve::EaseIn) < 0.25f);
    CHECK(applyCurve(0.25f, FadeCurve::EaseOut) > 0.25f);
    // S-curve is slow at both ends, fast in the middle.
    CHECK(applyCurve(0.1f, FadeCurve::SCurve) < 0.1f);
    CHECK(applyCurve(0.9f, FadeCurve::SCurve) > 0.9f);
}

static void testPresetRefsTrack() {
    // A preset reference tracks like any other value, and stays a reference —
    // which is what makes "edit the Warm preset, every cue follows" work.
    Sequence s;
    s.cues.resize(3);
    s.cues[0].values().set(1, 0, Attr::Red, Value::ref(Uid{4242}, Attr::Red, 0.8f));

    Resolved r = resolveTracking(s, 2);
    const Value* v = r.values.find(makeKey(1, 0, Attr::Red));
    CHECK(v != nullptr);
    CHECK(v->isRef());
    CHECK(v->preset == Uid{4242});
    CHECK_NEAR(v->v, 0.8);
}

int main() {
    std::printf("lumen cue\n");
    testTrackingForward();
    testUpstreamEditTracks();
    testPerGroupMasks();
    testCueOnly();
    testShield();
    testUnblock();
    testOrigin();
    testTiming();
    testFadeCurves();
    testPresetRefsTrack();
    return check::report("cue");
}
