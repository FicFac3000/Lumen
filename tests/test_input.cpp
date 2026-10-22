// test_input.cpp — DMX input arbitration and timecode (SPEC §18.3, §18.5).
#include "Check.hpp"

#include "lumen/Command.hpp"
#include "lumen/Input.hpp"
#include "lumen/Timecode.hpp"

#include <cstdio>

using namespace lumen;

namespace {

double gNow = 0.0;

std::vector<uint8_t> sacn(const std::string& name, uint8_t priority, uint8_t sequence,
                          uint16_t universe, uint8_t level, uint8_t options = 0,
                          size_t slots = 512) {
    std::array<uint8_t, 512> d{};
    d[0] = level;
    d[1] = static_cast<uint8_t>(level / 2);
    if (slots > 2) d[slots - 1] = level;
    auto pkt = buildSacnPacket(makeCid(name), name, priority, sequence, universe,
                               d.data(), slots);
    if (options) pkt[112] = static_cast<uint8_t>(pkt[112] | options);
    return pkt;
}

bool feed(DmxInput& in, const std::string& name, uint8_t priority, uint8_t sequence,
          uint16_t universe, uint8_t level, uint8_t options = 0) {
    const auto pkt = sacn(name, priority, sequence, universe, level, options);
    const auto f = parseSacnPacket(pkt.data(), pkt.size());
    if (!f) return false;
    return in.receiveSacn(*f, name);
}

int levelAt(const DmxInput& in, int universe, int channel) {
    const auto v = in.slot(universe, channel);
    return v ? static_cast<int>(*v) : -1;
}

const SourceInfo* findSource(const std::vector<SourceInfo>& all,
                             const std::string& name) {
    for (const SourceInfo& s : all)
        if (s.name == name) return &s;
    return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------
static void testSacnParsingMirrorsBuilding() {
    const auto pkt = sacn("Desk A", 123, 42, 7, 200);
    const auto f = parseSacnPacket(pkt.data(), pkt.size());
    CHECK(f.has_value());
    if (!f) return;

    CHECK(f->sourceName == "Desk A");
    CHECK_EQ(int(f->priority), 123);
    CHECK_EQ(int(f->sequence), 42);
    CHECK_EQ(int(f->universe), 7);
    CHECK(!f->preview);
    CHECK(!f->terminated);
    CHECK_EQ(f->len, size_t(512));
    CHECK_EQ(int(f->data[0]), 200);
    CHECK_EQ(int(f->data[1]), 100);
    CHECK(f->cid == makeCid("Desk A"));

    // Option flags.
    const auto prev = sacn("P", 100, 1, 1, 10, 0x80);
    CHECK(parseSacnPacket(prev.data(), prev.size())->preview);
    const auto term = sacn("T", 100, 1, 1, 10, 0x40);
    CHECK(parseSacnPacket(term.data(), term.size())->terminated);

    // Rejections. A short packet or a different protocol on the same port is
    // not a corrupted sACN packet — it is something else, and guessing would
    // merge nonsense into a universe.
    CHECK(!parseSacnPacket(nullptr, 0).has_value());
    CHECK(!parseSacnPacket(pkt.data(), 10).has_value());
    auto bad = pkt;
    bad[4] = 'X';   // ACN identifier
    CHECK(!parseSacnPacket(bad.data(), bad.size()).has_value());
    bad = pkt;
    bad[21] = 0x09;   // root vector
    CHECK(!parseSacnPacket(bad.data(), bad.size()).has_value());
    bad = pkt;
    bad[43] = 0x09;   // framing vector
    CHECK(!parseSacnPacket(bad.data(), bad.size()).has_value());
    bad = pkt;
    bad[125] = 0xcc;   // a non-zero START CODE is not DMX512 data
    CHECK(!parseSacnPacket(bad.data(), bad.size()).has_value());

    // A sender sending fewer than 512 slots is honoured, not padded.
    const auto shortPkt = sacn("Short", 100, 1, 1, 90, 0, 24);
    const auto sf = parseSacnPacket(shortPkt.data(), shortPkt.size());
    CHECK(sf.has_value());
    if (sf) CHECK_EQ(sf->len, size_t(24));
}

static void testArtDmxParsing() {
    std::array<uint8_t, 512> d{};
    d[0] = 77;
    d[511] = 88;
    const auto pkt = buildArtDmxPacket(9, 0, 3, d.data(), 512);
    const auto f = parseArtDmxPacket(pkt.data(), pkt.size());
    CHECK(f.has_value());
    if (!f) return;
    CHECK_EQ(int(f->sequence), 9);
    CHECK_EQ(int(f->portAddress), 3);
    CHECK_EQ(f->len, size_t(512));
    CHECK_EQ(int(f->data[0]), 77);
    CHECK_EQ(int(f->data[511]), 88);

    CHECK(!parseArtDmxPacket(pkt.data(), 4).has_value());
    auto bad = pkt;
    bad[0] = 'B';
    CHECK(!parseArtDmxPacket(bad.data(), bad.size()).has_value());
    bad = pkt;
    bad[8] = 0x00;   // not OpDmx
    bad[9] = 0x20;
    CHECK(!parseArtDmxPacket(bad.data(), bad.size()).has_value());

    // An sACN packet is not an ArtDmx packet and vice versa, which matters
    // because a show network carries both.
    const auto s = sacn("X", 100, 1, 1, 10);
    CHECK(!parseArtDmxPacket(s.data(), s.size()).has_value());
    CHECK(!parseSacnPacket(pkt.data(), pkt.size()).has_value());
}

// ---------------------------------------------------------------------------
// Arbitration (§18.3)
// ---------------------------------------------------------------------------
static void testPriorityDecidesNotArrivalOrder() {
    gNow = 0.0;
    DmxInput in;
    in.setClock([] { return gNow; });

    CHECK(feed(in, "Main", 100, 1, 1, 200));
    CHECK_EQ(levelAt(in, 1, 1), 200);

    // A backup desk at a lower priority is INVISIBLE while the main one
    // sends — resolved by priority, never by arrival order.
    CHECK(feed(in, "Backup", 50, 1, 1, 80));
    CHECK_EQ(levelAt(in, 1, 1), 200);
    MergeReport r = in.report(1);
    CHECK_EQ(r.sourcesSeen, 2);
    CHECK_EQ(r.sourcesActive, 2);
    CHECK_EQ(int(r.winningPriority), 100);
    CHECK(!r.merged);
    CHECK_EQ(r.contributors.size(), size_t(1));
    CHECK(r.contributors[0] == "Main");

    // Arriving later at a lower priority changes nothing, and arriving later
    // at a HIGHER priority takes over at once.
    gNow = 0.5;
    CHECK(feed(in, "Backup", 50, 2, 1, 255));
    CHECK_EQ(levelAt(in, 1, 1), 200);
    CHECK(feed(in, "Override", 200, 1, 1, 33));
    CHECK_EQ(levelAt(in, 1, 1), 33);
    CHECK_EQ(int(in.report(1).winningPriority), 200);
}

static void testQuietSourceFailsOverAfterTheHold() {
    gNow = 0.0;
    DmxInput in;
    in.setClock([] { return gNow; });
    CHECK_NEAR(in.config(1).hold, 2.5);   // E1.31's network data loss timeout

    CHECK(feed(in, "Main", 100, 1, 1, 200));
    CHECK(feed(in, "Backup", 50, 1, 1, 80));
    CHECK_EQ(levelAt(in, 1, 1), 200);

    // The backup keeps sending; the main desk has gone quiet.
    uint8_t seq = 2;
    for (double t : {1.0, 2.0, 2.4}) {
        gNow = t;
        CHECK(feed(in, "Backup", 50, seq++, 1, 80));
        // Still the main desk: its hold has not elapsed.
        CHECK_EQ(levelAt(in, 1, 1), 200);
    }

    gNow = 2.6;
    CHECK(feed(in, "Backup", 50, seq++, 1, 80));
    // Failover, with no switchover logic anywhere: the main desk simply
    // stopped being a candidate.
    CHECK_EQ(levelAt(in, 1, 1), 80);
    CHECK_EQ(int(in.report(1).winningPriority), 50);
    CHECK_EQ(in.report(1).sourcesActive, 1);

    // And the main desk takes it straight back when it returns.
    gNow = 3.0;
    CHECK(feed(in, "Main", 100, 50, 1, 200));
    CHECK_EQ(levelAt(in, 1, 1), 200);

    // When EVERY source goes quiet the universe stops existing rather than
    // freezing on its last values: a stale universe that looks live is how a
    // dead feed goes unnoticed.
    gNow = 10.0;
    CHECK(in.expire() > 0);
    CHECK(in.universe(1) == nullptr);
    CHECK_EQ(levelAt(in, 1, 1), -1);
    CHECK(in.universes().empty());
    // The sources are still listed, because "who was sending" is exactly what
    // someone diagnosing a dead universe needs.
    CHECK_EQ(in.sources(1).size(), size_t(2));
    for (const SourceInfo& s : in.sources(1)) CHECK(!s.active);
}

static void testTieBreaks() {
    // §18.3: where priorities tie, HTP merge or one source owns the universe,
    // configurable per universe.
    gNow = 0.0;
    DmxInput in;
    in.setClock([] { return gNow; });

    in.config(1).tie = TieBreak::Htp;
    CHECK(feed(in, "A", 100, 1, 1, 120));
    CHECK(feed(in, "B", 100, 1, 1, 200));
    CHECK_EQ(levelAt(in, 1, 1), 200);   // highest level per slot
    CHECK_EQ(levelAt(in, 1, 2), 100);   // B's slot 2 is 100, A's is 60
    MergeReport r = in.report(1);
    CHECK(r.merged);
    CHECK_EQ(r.contributors.size(), size_t(2));

    // Oldest wins: the source that has been sending longest owns it, so the
    // winner cannot flip frame to frame with packet arrival order.
    DmxInput oldest;
    oldest.setClock([] { return gNow; });
    oldest.config(1).tie = TieBreak::Oldest;
    gNow = 10.0;
    CHECK(feed(oldest, "A", 100, 1, 1, 120));
    gNow = 10.5;
    CHECK(feed(oldest, "B", 100, 1, 1, 200));
    CHECK_EQ(levelAt(oldest, 1, 1), 120);
    CHECK(!oldest.report(1).merged);
    // Repeated packets do not change the answer.
    gNow = 11.0;
    CHECK(feed(oldest, "B", 100, 2, 1, 255));
    CHECK_EQ(levelAt(oldest, 1, 1), 120);

    DmxInput latest;
    latest.setClock([] { return gNow; });
    latest.config(1).tie = TieBreak::Latest;
    gNow = 20.0;
    CHECK(feed(latest, "A", 100, 1, 1, 120));
    gNow = 20.5;
    CHECK(feed(latest, "B", 100, 1, 1, 200));
    CHECK_EQ(levelAt(latest, 1, 1), 200);

    // Tie-break only applies among the TOP priority: a third source below it
    // is still invisible.
    CHECK(feed(latest, "C", 10, 1, 1, 255));
    CHECK_EQ(levelAt(latest, 1, 1), 200);
}

static void testTerminateReleasesImmediately() {
    // A stream-terminated packet is a statement, not data: the source is
    // releasing the universe deliberately, so it must stop contributing at
    // once rather than after the hold time.
    gNow = 0.0;
    DmxInput in;
    in.setClock([] { return gNow; });
    CHECK(feed(in, "Main", 100, 1, 1, 200));
    CHECK(feed(in, "Backup", 50, 1, 1, 80));
    CHECK_EQ(levelAt(in, 1, 1), 200);

    CHECK(feed(in, "Main", 100, 2, 1, 200, 0x40));
    CHECK_EQ(levelAt(in, 1, 1), 80);   // the same instant, not 2.5s later

    const SourceInfo* main = findSource(in.sources(1), "Main");
    CHECK(main != nullptr);
    if (main) {
        CHECK(main->terminated);
        CHECK(!main->active);
    }
    // It is kept in the list, because knowing a desk sent a terminate is
    // itself diagnostic.
    CHECK_EQ(in.sources(1).size(), size_t(2));

    // Sending data again un-terminates it.
    CHECK(feed(in, "Main", 100, 3, 1, 150));
    CHECK_EQ(levelAt(in, 1, 1), 150);

    // And forget() removes it outright.
    CHECK(in.forget(makeCid("Main"), 1));
    CHECK_EQ(in.sources(1).size(), size_t(1));
    CHECK_EQ(levelAt(in, 1, 1), 80);
    CHECK(!in.forget(makeCid("Main"), 1));
}

static void testOutOfOrderPacketsAreDropped() {
    // E1.31 4.1: a packet whose sequence is behind the last by up to 20 is
    // discarded. Out-of-order UDP must not make levels jump backwards.
    gNow = 0.0;
    DmxInput in;
    in.setClock([] { return gNow; });

    CHECK(feed(in, "Desk", 100, 10, 1, 100));
    CHECK_EQ(levelAt(in, 1, 1), 100);

    CHECK(!feed(in, "Desk", 100, 9, 1, 255));   // late
    CHECK_EQ(levelAt(in, 1, 1), 100);           // unchanged
    CHECK(!feed(in, "Desk", 100, 10, 1, 255));  // duplicate
    CHECK_EQ(levelAt(in, 1, 1), 100);

    CHECK(feed(in, "Desk", 100, 11, 1, 50));
    CHECK_EQ(levelAt(in, 1, 1), 50);

    const SourceInfo* d = findSource(in.sources(1), "Desk");
    CHECK(d != nullptr);
    if (d) {
        CHECK_EQ(int(d->outOfOrder), 2);
        CHECK_EQ(int(d->packets), 4);
        CHECK(d->winning);
    }
    CHECK_EQ(int(in.rejected()), 2);

    // A sender that restarts its counter must be followed, not locked out
    // forever — which is what a strict "must increase" rule would do.
    CHECK(feed(in, "Desk", 100, 200, 1, 77));
    CHECK_EQ(levelAt(in, 1, 1), 77);

    // Wrap-around: 255 -> 0 is forward, not backwards.
    CHECK(feed(in, "Desk", 100, 255, 1, 88));
    CHECK_EQ(levelAt(in, 1, 1), 88);
    CHECK(feed(in, "Desk", 100, 0, 1, 99));
    CHECK_EQ(levelAt(in, 1, 1), 99);
}

static void testPreviewDataIsNotOutput() {
    // Preview data is explicitly not live output, so it is ignored by
    // default — even at a crushing priority.
    gNow = 0.0;
    DmxInput in;
    in.setClock([] { return gNow; });
    CHECK(in.config(1).ignorePreview);

    CHECK(feed(in, "Previewer", 200, 1, 1, 199, 0x80));
    CHECK(in.universe(1) == nullptr);
    CHECK_EQ(in.report(1).sourcesActive, 0);
    CHECK_EQ(in.report(1).sourcesSeen, 1);

    CHECK(feed(in, "Main", 100, 1, 1, 77));
    CHECK_EQ(levelAt(in, 1, 1), 77);

    // Turned off, it merges like anything else.
    in.config(1).ignorePreview = false;
    CHECK(feed(in, "Previewer", 200, 2, 1, 199, 0x80));
    CHECK_EQ(levelAt(in, 1, 1), 199);
}

static void testUniversesAreIndependentAndConfigurable() {
    gNow = 0.0;
    DmxInput in;
    in.setClock([] { return gNow; });

    CHECK(feed(in, "A", 100, 1, 1, 10));
    CHECK(feed(in, "A", 100, 1, 2, 20));
    CHECK(feed(in, "A", 100, 1, 5, 50));
    CHECK_EQ(in.universes().size(), size_t(3));
    CHECK_EQ(levelAt(in, 1, 1), 10);
    CHECK_EQ(levelAt(in, 2, 1), 20);
    CHECK_EQ(levelAt(in, 5, 1), 50);
    CHECK_EQ(levelAt(in, 3, 1), -1);
    // The same CID on two universes is two streams, since a desk sending
    // several universes is the normal case.
    CHECK_EQ(in.sources(0).size(), size_t(3));
    CHECK_EQ(in.sources(1).size(), size_t(1));

    // Per-universe config: a different hold and tie-break on each.
    in.config(2).hold = 10.0;
    in.config(2).tie = TieBreak::Latest;
    CHECK_NEAR(in.config(1).hold, 2.5);
    CHECK_NEAR(in.config(2).hold, 10.0);
    CHECK(in.config(1).tie == TieBreak::Htp);

    gNow = 5.0;
    in.expire();
    CHECK(in.universe(1) == nullptr);        // 2.5s hold elapsed
    CHECK(in.universe(2) != nullptr);        // 10s hold did not

    // A disabled universe rejects everything.
    in.config(4).enabled = false;
    CHECK(!feed(in, "A", 100, 1, 4, 99));
    CHECK(in.universe(4) == nullptr);

    // Out-of-range slots and universes answer safely.
    CHECK_EQ(levelAt(in, 2, 0), -1);
    CHECK_EQ(levelAt(in, 2, 513), -1);
    CHECK_EQ(levelAt(in, 999, 1), -1);

    in.clear();
    CHECK(in.universes().empty());
    CHECK(in.sources(0).empty());
    CHECK_EQ(int(in.packets()), 0);
}

static void testArtNetAndInterfaceSources() {
    gNow = 0.0;
    DmxInput in;
    in.setClock([] { return gNow; });

    std::array<uint8_t, 512> d{};
    d[0] = 111;
    const auto pkt = buildArtDmxPacket(1, 0, 0, d.data(), 512);
    const auto f = parseArtDmxPacket(pkt.data(), pkt.size());
    CHECK(f.has_value());
    if (!f) return;

    // Art-Net carries no priority, so the universe's configured value stands
    // in — without which an Art-Net source could not be arbitrated against an
    // sACN one at all.
    in.config(1).artNetPriority = 150;
    CHECK(in.receiveArtNet(*f, 1, "192.168.1.5"));
    CHECK_EQ(levelAt(in, 1, 1), 111);

    const auto sources = in.sources(1);
    CHECK_EQ(sources.size(), size_t(1));
    CHECK(sources[0].protocol == InputProtocol::ArtNet);
    CHECK_EQ(int(sources[0].priority), 150);
    CHECK(sources[0].origin == "192.168.1.5");

    // It beats an sACN source at 100, because that is what the config said.
    CHECK(feed(in, "Desk", 100, 1, 1, 222));
    CHECK_EQ(levelAt(in, 1, 1), 111);

    // Two Art-Net nodes on one universe are two sources, told apart by where
    // they came from since neither has a CID.
    CHECK(in.receiveArtNet(*f, 1, "192.168.1.6"));
    CHECK_EQ(in.sources(1).size(), size_t(3));
    CHECK(synthesiseCid("artnet:192.168.1.5", 1) !=
          synthesiseCid("artnet:192.168.1.6", 1));
    CHECK(synthesiseCid("artnet:192.168.1.5", 1) !=
          synthesiseCid("artnet:192.168.1.5", 2));
    CHECK(synthesiseCid("artnet:192.168.1.5", 1) ==
          synthesiseCid("artnet:192.168.1.5", 1));

    // A direct interface has no sequence numbers — there is no network to
    // reorder them — so repeated frames all apply.
    DmxInput iface;
    iface.setClock([] { return gNow; });
    std::array<uint8_t, 512> raw{};
    raw[0] = 10;
    CHECK(iface.receiveRaw(1, raw.data(), 512, "Enttec Pro", 90));
    CHECK_EQ(levelAt(iface, 1, 1), 10);
    raw[0] = 20;
    CHECK(iface.receiveRaw(1, raw.data(), 512, "Enttec Pro", 90));
    CHECK_EQ(levelAt(iface, 1, 1), 20);
    raw[0] = 15;
    CHECK(iface.receiveRaw(1, raw.data(), 512, "Enttec Pro", 90));
    CHECK_EQ(levelAt(iface, 1, 1), 15);
    CHECK(iface.sources(1)[0].protocol == InputProtocol::Interface);
    CHECK_EQ(int(iface.sources(1)[0].outOfOrder), 0);

    CHECK(!iface.receiveRaw(0, raw.data(), 512, "bad"));
    CHECK(!iface.receiveArtNet(*f, 0, "bad"));
}

static void testShortPacketsDoNotZeroWhatTheyDidNotSend() {
    // A sender sending 24 slots is not saying the other 488 are zero. Zeroing
    // them would black out a universe that two senders share by range.
    gNow = 0.0;
    DmxInput in;
    in.setClock([] { return gNow; });

    const auto full = sacn("Full", 100, 1, 1, 200, 0, 512);
    CHECK(in.receiveSacn(*parseSacnPacket(full.data(), full.size()), "full"));
    CHECK_EQ(levelAt(in, 1, 512), 200);

    const auto shortPkt = sacn("Full", 100, 2, 1, 90, 0, 24);
    CHECK(in.receiveSacn(*parseSacnPacket(shortPkt.data(), shortPkt.size()), "full"));
    CHECK_EQ(levelAt(in, 1, 1), 90);
    CHECK_EQ(levelAt(in, 1, 512), 200);   // not touched by the short packet
}

// ---------------------------------------------------------------------------
// Timecode (§18.5)
// ---------------------------------------------------------------------------
static void testFrameRateFacts() {
    CHECK_EQ(framesPerSecond(FrameRate::Fps24), 24);
    CHECK_EQ(framesPerSecond(FrameRate::Fps25), 25);
    CHECK_EQ(framesPerSecond(FrameRate::Fps30), 30);
    // Drop-frame counts 0..29 like 30 fps; what differs is which NUMBERS
    // exist at a minute boundary.
    CHECK_EQ(framesPerSecond(FrameRate::Fps2997Drop), 30);
    CHECK_NEAR(realFrameRate(FrameRate::Fps30), 30.0);
    CHECK(realFrameRate(FrameRate::Fps2997Drop) < 30.0);
    CHECK_NEAR(realFrameRate(FrameRate::Fps2997Drop), 30000.0 / 1001.0);
    CHECK(isDropFrame(FrameRate::Fps2997Drop));
    CHECK(!isDropFrame(FrameRate::Fps30));

    // The semicolon is the conventional drop-frame marker, so the two are
    // distinguishable at a glance.
    const Timecode tc{1, 2, 3, 4};
    CHECK(tc.text(FrameRate::Fps25) == "01:02:03:04");
    CHECK(tc.text(FrameRate::Fps2997Drop) == "01:02:03;04");
}

static void testPlainFrameRatesRoundTrip() {
    for (FrameRate r : {FrameRate::Fps24, FrameRate::Fps25, FrameRate::Fps30}) {
        const int fps = framesPerSecond(r);
        CHECK_EQ(int(toFrames({0, 0, 0, 0}, r)), 0);
        CHECK_EQ(int(toFrames({0, 0, 1, 0}, r)), fps);
        CHECK_EQ(int(toFrames({0, 1, 0, 0}, r)), 60 * fps);
        CHECK_EQ(int(toFrames({1, 0, 0, 0}, r)), 3600 * fps);
        CHECK_NEAR(toSeconds({1, 0, 0, 0}, r), 3600.0);

        for (const Timecode& tc : {Timecode{0, 0, 0, 0}, Timecode{0, 0, 0, 1},
                                   Timecode{1, 2, 3, 4}, Timecode{23, 59, 59, 0},
                                   Timecode{10, 30, 15, 7}}) {
            if (!isValidTimecode(tc, r)) continue;
            CHECK(fromFrames(toFrames(tc, r), r) == tc);
            CHECK(fromSeconds(toSeconds(tc, r), r) == tc);
        }
        // The last frame of a second.
        const Timecode last{0, 0, 0, fps - 1};
        CHECK(isValidTimecode(last, r));
        CHECK(fromFrames(toFrames(last, r), r) == last);
        CHECK(!isValidTimecode({0, 0, 0, fps}, r));
        CHECK(!isValidTimecode({0, 0, 60, 0}, r));
        CHECK(!isValidTimecode({0, 60, 0, 0}, r));
        CHECK(!isValidTimecode({24, 0, 0, 0}, r));
        CHECK(!isValidTimecode({0, 0, 0, -1}, r));
    }
}

static void testDropFrameIsExact() {
    // 29.97 drop-frame drops frame NUMBERS, not frames: two at the start of
    // every minute except every tenth. Getting this wrong drifts 3.6 seconds
    // an hour, which over a show is a cue landing visibly late.
    const FrameRate r = FrameRate::Fps2997Drop;

    // The textbook values.
    CHECK_EQ(int(toFrames({0, 0, 59, 29}, r)), 1799);
    CHECK_EQ(int(toFrames({0, 1, 0, 2}, r)), 1800);    // 00 and 01 do not exist
    CHECK_EQ(int(toFrames({0, 9, 59, 29}, r)), 17981);
    CHECK_EQ(int(toFrames({0, 10, 0, 0}, r)), 17982);  // the tenth minute keeps them
    CHECK_EQ(int(toFrames({1, 0, 0, 0}, r)), 107892);  // one hour

    // One hour of drop-frame is 3.6ms SHORT of an hour of real time, which is
    // the 0.1% the format exists to correct.
    const double hour = toSeconds({1, 0, 0, 0}, r);
    CHECK(hour < 3600.0);
    CHECK(hour > 3599.99);

    // The dropped numbers do not exist.
    CHECK(!isValidTimecode({0, 1, 0, 0}, r));
    CHECK(!isValidTimecode({0, 1, 0, 1}, r));
    CHECK(isValidTimecode({0, 1, 0, 2}, r));
    CHECK(isValidTimecode({0, 10, 0, 0}, r));   // except every tenth minute
    CHECK(isValidTimecode({0, 10, 0, 1}, r));
    CHECK(isValidTimecode({0, 20, 0, 0}, r));
    CHECK(!isValidTimecode({0, 11, 0, 0}, r));
    CHECK(!isValidTimecode({0, 59, 0, 1}, r));
    CHECK(isValidTimecode({1, 0, 0, 0}, r));    // minute 0 of an hour

    // Exhaustive over the first eleven minutes: every valid reading round
    // trips, frame numbering is consecutive with no gaps or repeats, and
    // exactly eighteen readings are invalid (two per dropping minute).
    int invalid = 0;
    int64_t previous = -1;
    for (int m = 0; m < 11; ++m)
        for (int s = 0; s < 60; ++s)
            for (int f = 0; f < 30; ++f) {
                const Timecode tc{0, m, s, f};
                if (!isValidTimecode(tc, r)) {
                    ++invalid;
                    continue;
                }
                const int64_t n = toFrames(tc, r);
                CHECK(fromFrames(n, r) == tc);
                if (previous >= 0) CHECK_EQ(int(n), int(previous + 1));
                previous = n;
            }
    CHECK_EQ(invalid, 18);   // 2 each for minutes 1..9

    // And a second pass well past an hour, where the ten-minute block
    // arithmetic has had room to go wrong.
    for (int64_t n = 107880; n < 107920; ++n) {
        const Timecode tc = fromFrames(n, r);
        CHECK(isValidTimecode(tc, r));
        CHECK_EQ(int(toFrames(tc, r)), int(n));
    }
}

static void testSlotsRunIndependently() {
    UidGen uids;
    TimecodeEngine tc(uids);
    gNow = 100.0;
    tc.setClock([] { return gNow; });

    // §18.5: several timelines run independently.
    TimecodeSlot& a = tc.add("Act 1", TimecodeSource::Ltc, FrameRate::Fps25);
    TimecodeSlot& b = tc.add("Video", TimecodeSource::ArtNet, FrameRate::Fps2997Drop);
    TimecodeSlot& c = tc.add("Rehearsal", TimecodeSource::Internal, FrameRate::Fps30);
    CHECK_EQ(tc.size(), size_t(3));
    CHECK_EQ(a.id, 1);
    CHECK(a.uid != b.uid);
    CHECK(tc.byName("Video") == &b);
    CHECK(tc.slot(3) == &c);
    CHECK(tc.byUid(c.uid) == &c);
    CHECK(tc.slot(99) == nullptr);

    CHECK(tc.feed(1, {0, 1, 0, 0}));
    CHECK(tc.feed(2, {0, 10, 0, 0}));
    CHECK(tc.slot(1)->position == Timecode{0, 1, 0, 0});
    CHECK(tc.slot(2)->position == Timecode{0, 10, 0, 0});
    CHECK(tc.slot(3)->position == Timecode{0, 0, 0, 0});

    // A reading the rate cannot express is a decoder fault worth surfacing:
    // the usual cause is the wrong frame rate configured.
    CHECK(!tc.feed(2, {0, 1, 0, 0}));        // a dropped number
    CHECK(!tc.feed(1, {0, 0, 0, 25}));       // no frame 25 at 25 fps
    CHECK(!tc.feed(99, {0, 0, 0, 0}));
    CHECK(tc.slot(2)->position == Timecode{0, 10, 0, 0});   // unchanged

    // Feeding frames means it is running, and it free-runs between them.
    CHECK(tc.slot(1)->running);
    gNow = 102.0;
    CHECK(tc.slot(1)->at(gNow) == Timecode{0, 1, 2, 0});
    // The other slots are untouched by that.
    CHECK(tc.slot(2)->at(gNow) == Timecode{0, 10, 2, 0});
    CHECK(tc.slot(3)->at(gNow) == Timecode{0, 0, 0, 0});   // not started

    CHECK(tc.remove(2));
    CHECK_EQ(tc.size(), size_t(2));
    CHECK(!tc.remove(2));
}

static void testInternalTransportAndStaleness() {
    UidGen uids;
    TimecodeEngine tc(uids);
    gNow = 0.0;
    tc.setClock([] { return gNow; });

    const int internal = tc.add("Free", TimecodeSource::Internal, FrameRate::Fps25).id;
    CHECK(!tc.slot(internal)->running);
    CHECK(tc.start(internal));
    CHECK(tc.slot(internal)->running);

    gNow = 10.0;
    CHECK(tc.slot(internal)->at(gNow) == Timecode{0, 0, 10, 0});
    CHECK_NEAR(tc.slot(internal)->secondsAt(gNow), 10.0);

    // Stopping freezes where it actually is, rather than losing the free-run.
    CHECK(tc.stop(internal));
    CHECK(tc.slot(internal)->position == Timecode{0, 0, 10, 0});
    gNow = 20.0;
    CHECK(tc.slot(internal)->at(gNow) == Timecode{0, 0, 10, 0});
    CHECK(tc.start(internal));
    gNow = 25.0;
    CHECK(tc.slot(internal)->at(gNow) == Timecode{0, 0, 15, 0});

    CHECK(tc.setPosition(internal, {1, 0, 0, 0}));
    CHECK(tc.slot(internal)->at(gNow) == Timecode{1, 0, 0, 0});
    CHECK(!tc.setPosition(internal, {0, 0, 0, 99}));
    CHECK(!tc.start(99));
    CHECK(!tc.stop(99));

    // An external slot that stops receiving goes STALE. It keeps its last
    // position — jumping to zero would be worse — but says so, because a
    // frozen reading that claims to be live is how a dead feed goes unnoticed.
    const int ltc = tc.add("LTC", TimecodeSource::Ltc, FrameRate::Fps25).id;
    gNow = 30.0;
    CHECK(tc.feed(ltc, {0, 5, 0, 0}));
    CHECK(!tc.slot(ltc)->stale);
    CHECK_EQ(tc.tick(), 0);

    gNow = 30.1;
    CHECK_EQ(tc.tick(), 0);   // within the timeout
    gNow = 30.5;
    CHECK_EQ(tc.tick(), 1);
    CHECK(tc.slot(ltc)->stale);
    CHECK(tc.slot(ltc)->position == Timecode{0, 5, 0, 0});
    CHECK_EQ(tc.tick(), 0);   // reported once, not every pass

    CHECK(tc.feed(ltc, {0, 5, 1, 0}));
    CHECK(!tc.slot(ltc)->stale);

    // An internal slot cannot go stale: it is its own source.
    gNow = 1000.0;
    tc.tick();
    CHECK(!tc.slot(internal)->stale);
}

static void testDriftAndJumpsAreVisible() {
    UidGen uids;
    TimecodeEngine tc(uids);
    gNow = 0.0;
    tc.setClock([] { return gNow; });
    const int id = tc.add("LTC", TimecodeSource::Ltc, FrameRate::Fps25).id;

    CHECK(tc.feed(id, {0, 0, 10, 0}));
    CHECK_EQ(int(tc.slot(id)->frames), 1);

    // A frame arriving exactly where free-running expected it: no drift.
    gNow = 1.0;
    CHECK(tc.feed(id, {0, 0, 11, 0}));
    CHECK_NEAR(tc.slot(id)->driftFrames, 0.0);
    CHECK_EQ(int(tc.slot(id)->jumps), 0);

    // A source running slow: the code is behind where free-running put us.
    gNow = 2.0;
    CHECK(tc.feed(id, {0, 0, 11, 20}));   // 0.8s of code for 1.0s of wall time
    CHECK(tc.slot(id)->driftFrames < 0.0);
    CHECK_NEAR(tc.slot(id)->driftFrames, -5.0);

    // A source running fast.
    gNow = 3.0;
    CHECK(tc.feed(id, {0, 0, 13, 20}));
    CHECK(tc.slot(id)->driftFrames > 0.0);

    // A scrub backwards is a jump, not drift — a different fault with a
    // different cause.
    const uint64_t before = tc.slot(id)->jumps;
    gNow = 4.0;
    CHECK(tc.feed(id, {0, 0, 2, 0}));
    CHECK_EQ(int(tc.slot(id)->jumps), int(before + 1));
    CHECK(tc.slot(id)->position == Timecode{0, 0, 2, 0});

    // A large forward jump counts too.
    gNow = 5.0;
    CHECK(tc.feed(id, {0, 5, 0, 0}));
    CHECK_EQ(int(tc.slot(id)->jumps), int(before + 2));
}

static void testTriggerCrossings() {
    // §11.2's cue triggers ask one question: has the timeline PASSED this
    // point? Asking "passed" rather than "equals" is what makes a trigger
    // survive a dropped frame or a coarse tick.
    UidGen uids;
    TimecodeEngine tc(uids);
    gNow = 0.0;
    tc.setClock([] { return gNow; });
    const int id = tc.add("Show", TimecodeSource::Internal, FrameRate::Fps25).id;
    CHECK(tc.start(id));

    tc.watch(id, {0, 0, 5, 0});
    tc.watch(id, {0, 0, 10, 0});
    // Registering a trigger does not fire it, even though the first
    // observation is below both points.
    CHECK(tc.crossings(gNow).empty());

    gNow = 1.0;
    CHECK(tc.crossings(gNow).empty());

    // A coarse jump straight past the point still fires: this is the case an
    // equality test would miss.
    gNow = 7.0;
    auto fired = tc.crossings(gNow);
    CHECK_EQ(fired.size(), size_t(1));
    if (!fired.empty()) {
        CHECK(fired[0].at == Timecode{0, 0, 5, 0});
        CHECK(fired[0].forward);
        CHECK_EQ(fired[0].slot, id);
    }
    // And it does not fire again while sitting past it.
    CHECK(tc.crossings(gNow).empty());

    gNow = 12.0;
    fired = tc.crossings(gNow);
    CHECK_EQ(fired.size(), size_t(1));
    if (!fired.empty()) CHECK(fired[0].at == Timecode{0, 0, 10, 0});

    // Scrubbing backwards reports a backward crossing, so a cue list can
    // decide for itself whether to run in reverse.
    CHECK(tc.setPosition(id, {0, 0, 0, 0}));
    fired = tc.crossings(gNow);
    CHECK_EQ(fired.size(), size_t(2));
    for (const auto& c : fired) CHECK(!c.forward);

    // Then forward again fires both.
    CHECK(tc.setPosition(id, {0, 0, 20, 0}));
    fired = tc.crossings(gNow);
    CHECK_EQ(fired.size(), size_t(2));
    for (const auto& c : fired) CHECK(c.forward);

    tc.unwatch(id, {0, 0, 5, 0});
    CHECK(tc.setPosition(id, {0, 0, 0, 0}));
    tc.crossings(gNow);
    CHECK(tc.setPosition(id, {0, 0, 20, 0}));
    CHECK_EQ(tc.crossings(gNow).size(), size_t(1));

    // Watching the same point twice is one watch, not two.
    tc.watch(id, {0, 0, 10, 0});
    CHECK(tc.setPosition(id, {0, 0, 0, 0}));
    tc.crossings(gNow);
    CHECK(tc.setPosition(id, {0, 0, 20, 0}));
    CHECK_EQ(tc.crossings(gNow).size(), size_t(1));

    // A watch on a removed slot is forgotten rather than firing into nothing.
    CHECK(tc.remove(id));
    CHECK(tc.crossings(gNow).empty());
}

// ---------------------------------------------------------------------------
// Through the engine (§12.3, §18.3)
// ---------------------------------------------------------------------------
static void testArrivingDmxFiresABoundCommand() {
    // The whole chain: a real sACN packet is arbitrated, merged, turned into a
    // binding event and fires a command. This is §12.3's house-light
    // interlock, which is what deferring GPI rests on.
    Engine eng;
    Console con(eng);
    eng.setClock(0.0);

    CHECK(con.execute("BindingSet \"House\"").ok);
    BindingSet* set = eng.bindings.setByName("House");
    CHECK(set != nullptr);
    if (!set) return;

    Binding b;
    b.source.kind = SourceKind::DmxIn;
    b.source.channel = 2;
    b.source.index = 14;
    b.target.kind = TargetKind::Command;
    b.target.command = "Blackout on";
    b.map.edge = ThresholdEdge::Rising;
    b.map.threshold = 0.5f;
    b.map.hysteresis = 0.05f;
    CHECK(eng.bindings.add(set->uid, std::move(b)) != nullptr);

    auto sendLevel = [&](double t, uint8_t level, uint8_t seq) {
        eng.setClock(t);
        std::array<uint8_t, 512> d{};
        d[13] = level;   // channel 14, 1-based
        const auto pkt = buildSacnPacket(makeCid("Panel"), "Panel", 100, seq, 2,
                                         d.data(), 512);
        const auto f = parseSacnPacket(pkt.data(), pkt.size());
        CHECK(f.has_value());
        if (f) CHECK(eng.dmxIn.receiveSacn(*f, "10.0.0.9"));
        eng.tick();
        return con.pumpBindings();
    };

    CHECK(!eng.blackout);
    CHECK_EQ(sendLevel(0.1, 0, 1).executed, 0);
    CHECK(!eng.blackout);
    CHECK_EQ(sendLevel(0.2, 40, 2).executed, 0);   // below the threshold
    CHECK(!eng.blackout);

    CHECK_EQ(sendLevel(0.3, 200, 3).executed, 1);
    CHECK(eng.blackout);
    CHECK(con.execute("Blackout off").ok);

    // Staying high does not fire again.
    CHECK_EQ(sendLevel(0.4, 255, 4).executed, 0);
    CHECK(!eng.blackout);
    // Dropping below and rising again does.
    CHECK_EQ(sendLevel(0.5, 10, 5).executed, 0);
    CHECK_EQ(sendLevel(0.6, 220, 6).executed, 1);
    CHECK(eng.blackout);

    // The source is visible on the diagnostic page §18.3 asks for.
    const Result rows = con.execute("List sources");
    CHECK(rows.ok);
    CHECK(!rows.rows.empty());
    bool namesPanel = false, saysWinning = false;
    for (const auto& row : rows.rows) {
        if (row.find("Panel") != std::string::npos) namesPanel = true;
        if (row.find("winning") != std::string::npos) saysWinning = true;
    }
    CHECK(namesPanel);
    CHECK(saysWinning);

    // A universe nothing is bound to costs nothing: no events are generated
    // for channels no binding names.
    std::array<uint8_t, 512> d{};
    d[0] = 255;
    const auto pkt = buildSacnPacket(makeCid("Other"), "Other", 100, 1, 9, d.data(),
                                     512);
    eng.dmxIn.receiveSacn(*parseSacnPacket(pkt.data(), pkt.size()), "10.0.0.10");
    eng.setClock(0.7);
    eng.tick();
    CHECK_EQ(con.pumpBindings().actions, 0);
}

static void testEngineSweepsInputAndTimecode() {
    // Both are time-driven: a source that went quiet must release its slots
    // even though no packet will ever arrive to trigger it, and a timecode
    // feed that stopped must be called stale on the same basis.
    Engine eng;
    Console con(eng);
    eng.setClock(0.0);

    std::array<uint8_t, 512> d{};
    d[0] = 200;
    const auto pkt = buildSacnPacket(makeCid("Desk"), "Desk", 100, 1, 1, d.data(), 512);
    CHECK(eng.dmxIn.receiveSacn(*parseSacnPacket(pkt.data(), pkt.size()), "desk"));
    eng.tick();
    CHECK(eng.dmxIn.universe(1) != nullptr);

    // Nothing arrives, and the tick alone releases it.
    eng.setClock(5.0);
    eng.tick();
    CHECK(eng.dmxIn.universe(1) == nullptr);

    // Every subsystem measures the same time, so a driven clock moves all of
    // them — a subsystem with its own clock would not move at all here.
    const int id = eng.timecode.add("LTC", TimecodeSource::Ltc, FrameRate::Fps25).id;
    CHECK(eng.timecode.feed(id, {0, 1, 0, 0}));
    CHECK(!eng.timecode.slot(id)->stale);
    eng.setClock(10.0);
    eng.tick();
    CHECK(eng.timecode.slot(id)->stale);

    // And the introspection reflects it.
    const Result rows = con.execute("List timecode");
    CHECK(rows.ok);
    CHECK_EQ(rows.rows.size(), size_t(1));
    if (!rows.rows.empty()) {
        CHECK(rows.rows[0].find("LTC") != std::string::npos);
        CHECK(rows.rows[0].find("stale") != std::string::npos);
    }
}

static void testTimecodeCommands() {
    Engine eng;
    Console con(eng);
    eng.setClock(0.0);

    Result added = con.execute("AddTimecode \"Show\" internal 2997");
    CHECK(added.ok);
    CHECK(added.message.find("29.97") != std::string::npos);
    CHECK_EQ(eng.timecode.size(), size_t(1));
    const int id = eng.timecode.slots().front().id;

    CHECK(con.execute("Timecode " + std::to_string(id) + " start").ok);
    CHECK(eng.timecode.slot(id)->running);
    eng.setClock(2.0);
    CHECK(con.execute("Timecode " + std::to_string(id) + " stop").ok);
    CHECK(!eng.timecode.slot(id)->running);

    CHECK(con.execute("Timecode " + std::to_string(id) + " set 1 2 3 4").ok);
    CHECK(eng.timecode.slot(id)->position == Timecode{1, 2, 3, 4});

    // A drop-frame slot refuses a reading that names a dropped number, rather
    // than accepting a code that cannot exist.
    Result bad = con.execute("Timecode " + std::to_string(id) + " set 0 1 0 0");
    CHECK(bad.parsed);
    CHECK(!bad.ok);
    CHECK(bad.message.find("not a valid reading") != std::string::npos);

    CHECK(!con.execute("Timecode 99 start").ok);
    CHECK(!con.execute("AddTimecode \"\"").ok);
}

int main() {
    std::printf("lumen input\n");
    testSacnParsingMirrorsBuilding();
    testArtDmxParsing();
    testPriorityDecidesNotArrivalOrder();
    testQuietSourceFailsOverAfterTheHold();
    testTieBreaks();
    testTerminateReleasesImmediately();
    testOutOfOrderPacketsAreDropped();
    testPreviewDataIsNotOutput();
    testUniversesAreIndependentAndConfigurable();
    testArtNetAndInterfaceSources();
    testShortPacketsDoNotZeroWhatTheyDidNotSend();
    testFrameRateFacts();
    testPlainFrameRatesRoundTrip();
    testDropFrameIsExact();
    testSlotsRunIndependently();
    testInternalTransportAndStaleness();
    testDriftAndJumpsAreVisible();
    testTriggerCrossings();
    testArrivingDmxFiresABoundCommand();
    testEngineSweepsInputAndTimecode();
    testTimecodeCommands();
    return check::report("input");
}
