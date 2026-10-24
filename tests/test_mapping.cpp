// Tests for pixel extraction (§13.1–13.3) — the one join between the sparse
// control domain and the dense raster one.
#include "Check.hpp"

#include "lumen/Mapping.hpp"

#include <cmath>
#include <cstdio>

using namespace lumen;

namespace {

RasterFrame gradient(int w, int h) {
    RasterFrame f(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint8_t v = static_cast<uint8_t>(x * 255 / std::max(1, w - 1));
            f.set(x, y, RasterFrame::Rgb{v, 0, static_cast<uint8_t>(255 - v)});
        }
    return f;
}

float red(const OutputLayer& l, FixtureId f, CellId c = 0) {
    const auto v = l.get(makeKey(f, c, Attr::Red));
    return v ? *v : -1.f;
}
float blue(const OutputLayer& l, FixtureId f, CellId c = 0) {
    const auto v = l.get(makeKey(f, c, Attr::Blue));
    return v ? *v : -1.f;
}

void testFrameBasics() {
    RasterFrame f(4, 3);
    CHECK(!f.empty());
    CHECK_EQ(f.width(), 4);
    CHECK_EQ(f.height(), 3);
    CHECK_EQ(f.bytes().size(), size_t(4 * 3 * 3));

    f.set(1, 1, RasterFrame::Rgb{10, 20, 30});
    CHECK_EQ(int(f.at(1, 1).r), 10);
    CHECK_EQ(int(f.at(1, 1).g), 20);
    CHECK_EQ(int(f.at(1, 1).b), 30);

    // Out of bounds is BLACK, not clamped. A sample point that has drifted
    // off the surface should go dark rather than pick up the edge pixel and
    // look like it was meant.
    CHECK_EQ(int(f.at(-1, 1).r), 0);
    CHECK_EQ(int(f.at(99, 1).r), 0);
    CHECK_EQ(int(f.at(1, -5).b), 0);

    f.fill(7, 8, 9);
    CHECK_EQ(int(f.at(0, 0).r), 7);
    CHECK_EQ(int(f.at(3, 2).b), 9);

    // A resize starts black rather than keeping stale content.
    const uint64_t before = f.version();
    f.resize(2, 2);
    CHECK_EQ(int(f.at(0, 0).r), 0);
    CHECK(f.version() > before);

    RasterFrame empty;
    CHECK(empty.empty());
    CHECK_EQ(int(empty.at(0, 0).r), 0);
}

void testSampleAreas() {
    const RasterFrame g = gradient(101, 11);

    SamplePoint p;
    p.x = 0.5f;
    p.y = 0.5f;

    // A point sample lands on one pixel: column 50 of 101, so ~127.
    p.area = SampleArea::Point;
    const RasterFrame::Rgb pt = samplePoint(g, p);
    CHECK(pt.r >= 125 && pt.r <= 130);
    CHECK_EQ(int(pt.g), 0);

    // A box average over a symmetric gradient gives the same centre value —
    // which is the point: averaging removes jitter without shifting colour.
    p.area = SampleArea::Box;
    p.radius = 0.05f;
    const RasterFrame::Rgb box = samplePoint(g, p);
    CHECK(std::abs(int(box.r) - int(pt.r)) <= 2);

    p.area = SampleArea::Gaussian;
    const RasterFrame::Rgb gauss = samplePoint(g, p);
    CHECK(std::abs(int(gauss.r) - int(pt.r)) <= 2);

    // On DETAILED content the areas separate, which is the whole reason they
    // exist: a point sample takes whichever pixel it landed on, an average
    // takes the neighbourhood.
    RasterFrame checker(64, 64);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            const uint8_t v = ((x + y) % 2) ? 255 : 0;
            checker.set(x, y, RasterFrame::Rgb{v, v, v});
        }
    SamplePoint c;
    c.x = 0.5f;
    c.y = 0.5f;
    c.area = SampleArea::Point;
    const int sharp = samplePoint(checker, c).r;
    CHECK(sharp == 0 || sharp == 255);
    c.area = SampleArea::Box;
    c.radius = 0.1f;
    const int soft = samplePoint(checker, c).r;
    CHECK(soft > 100 && soft < 160);   // averages towards mid grey

    // A radius that rounds to nothing is a point sample, not an empty average.
    c.radius = 0.0001f;
    c.area = SampleArea::Box;
    const int tiny = samplePoint(checker, c).r;
    CHECK(tiny == 0 || tiny == 255);

    // A disabled point samples nothing at all.
    c.enabled = false;
    CHECK_EQ(int(samplePoint(checker, c).r), 0);
}

// The edge rule, which is easy to get backwards. A sample window hanging off
// the buffer is an artifact of the BUFFER ending, not of the fixture being
// off the content: a fixture mapped to the very edge of a white surface
// should read white. Counting the missing pixels as black would dim every
// strip that runs wall to wall, which is most of them.
void testEdgeSamplesReadTheContent() {
    RasterFrame white(32, 32);
    white.fill(255, 255, 255);

    SamplePoint middle;
    middle.x = 0.5f;
    middle.y = 0.5f;
    middle.area = SampleArea::Box;
    middle.radius = 0.2f;

    SamplePoint corner = middle;
    corner.x = 0.f;
    corner.y = 0.f;

    CHECK_EQ(int(samplePoint(white, middle).r), 255);
    CHECK_EQ(int(samplePoint(white, corner).r), 255);

    SamplePoint gauss = corner;
    gauss.area = SampleArea::Gaussian;
    CHECK_EQ(int(samplePoint(white, gauss).r), 255);

    // The real mapping problem — a point whose CENTRE is off the surface —
    // is still dark, and still counted. That is the case worth seeing.
    Extraction e;
    e.points = {SamplePoint{{1, 0}, 1.3f, 0.5f, SampleArea::Box, 0.2f, true}};
    OutputLayer out;
    const ExtractionReport rep = e.render(white, out);
    CHECK_EQ(rep.offSurface, 1);
    CHECK_NEAR(red(out, 1), 0.f);
}

void testLayouts() {
    std::vector<Target> strip;
    for (FixtureId i = 1; i <= 12; ++i) strip.push_back({i, 0});

    // A line spans end to end, first and last ON the ends.
    const auto line = layoutLine(strip, 0.f, 0.5f, 1.f, 0.5f);
    CHECK_EQ(line.size(), size_t(12));
    CHECK_NEAR(line.front().x, 0.f);
    CHECK_NEAR(line.back().x, 1.f);
    CHECK_NEAR(line.front().y, 0.5f);
    // Evenly spaced, and in SELECTION ORDER — that order is information
    // everywhere else in the console (§20.4) and a layout must not scramble
    // it, or a strip laid along a line would chase in the wrong direction.
    for (size_t i = 1; i < line.size(); ++i) {
        CHECK(line[i].x > line[i - 1].x);
        CHECK(line[i].target.fixture == strip[i].fixture);
    }

    // A grid fills rows, and points sit at cell CENTRES rather than corners.
    const auto grid = layoutGrid(strip, 4, 0.f, 0.f, 1.f, 1.f);
    CHECK_EQ(grid.size(), size_t(12));
    CHECK_NEAR(grid[0].x, 0.125f);        // first of four columns
    CHECK_NEAR(grid[0].y, 1.f / 6.f);     // first of three rows
    CHECK_NEAR(grid[3].x, 0.875f);        // last column
    CHECK_NEAR(grid[4].x, 0.125f);        // wrapped to the next row
    CHECK(grid[4].y > grid[0].y);

    // A ragged last row is fine: 10 across 4 columns is three rows.
    std::vector<Target> ten(strip.begin(), strip.begin() + 10);
    const auto ragged = layoutGrid(ten, 4, 0.f, 0.f, 1.f, 1.f);
    CHECK_EQ(ragged.size(), size_t(10));
    CHECK_NEAR(ragged[8].x, 0.125f);

    // More columns than fixtures does not produce empty columns.
    const auto wide = layoutGrid(ten, 99, 0.f, 0.f, 1.f, 1.f);
    CHECK_EQ(wide.size(), size_t(10));
    CHECK(wide.back().x <= 1.f);

    // A bezier starts and ends on its endpoints and stays inside the box.
    const auto curve = layoutBezier(strip, 0.f, 1.f, 0.f, 0.f, 1.f, 0.f, 1.f, 1.f);
    CHECK_EQ(curve.size(), size_t(12));
    CHECK_NEAR(curve.front().x, 0.f);
    CHECK_NEAR(curve.front().y, 1.f);
    CHECK_NEAR(curve.back().x, 1.f);
    CHECK_NEAR(curve.back().y, 1.f);
    for (const SamplePoint& p : curve) {
        CHECK(p.x >= 0.f && p.x <= 1.f);
        CHECK(p.y >= 0.f && p.y <= 1.f);
    }
    // It actually bows: the middle is above the endpoints.
    CHECK(curve[6].y < 0.9f);

    // Degenerate inputs give nothing rather than dividing by zero.
    CHECK(layoutGrid({}, 4, 0, 0, 1, 1).empty());
    CHECK(layoutGrid(strip, 0, 0, 0, 1, 1).empty());
    CHECK(layoutLine({}, 0, 0, 1, 1).empty());

    // A single fixture goes where the line STARTS, not halfway along it.
    const auto one = layoutLine({{7, 0}}, 0.2f, 0.3f, 0.9f, 0.9f);
    CHECK_EQ(one.size(), size_t(1));
    CHECK_NEAR(one[0].x, 0.2f);
}

void testExtractionWritesColour() {
    const RasterFrame g = gradient(101, 11);

    Extraction e;
    e.id = 1;
    e.setPoints(layoutLine({{1, 0}, {2, 0}, {3, 0}}, 0.f, 0.5f, 1.f, 0.5f));

    OutputLayer out;
    const ExtractionReport rep = e.render(g, out);
    CHECK(rep.hadFrame);
    CHECK_EQ(rep.sampled, 3);
    CHECK_EQ(rep.offSurface, 0);

    // The gradient runs red-left to blue-right, so the strip reads across it.
    CHECK(red(out, 1) < red(out, 2));
    CHECK(red(out, 2) < red(out, 3));
    CHECK(blue(out, 1) > blue(out, 3));
    // The default area is a box, so the ends read the AVERAGE of the few
    // pixels around them rather than exactly the end pixel. Within a frame
    // of full scale either way, which is what an LED can show anyway.
    CHECK(red(out, 1) < 0.01f);
    CHECK(red(out, 3) > 0.99f);

    // The master scales what the layer CONTRIBUTES, not what it wins with.
    // Scaling afterwards would let a faded extraction win an HTP comparison
    // at full and then be turned down.
    e.master = 0.5f;
    OutputLayer half;
    e.render(g, half);
    CHECK(std::abs(red(half, 3) - 0.5f) < 0.01f);

    // A disabled layer writes nothing at all.
    e.enabled = false;
    OutputLayer none;
    const ExtractionReport off = e.render(g, none);
    CHECK_EQ(none.size(), size_t(0));
    CHECK_EQ(off.sampled, 0);

    // With no frame there is nothing to sample, and it says so rather than
    // writing black over a rig that is working.
    e.enabled = true;
    RasterFrame nothing;
    OutputLayer blank;
    const ExtractionReport nf = e.render(nothing, blank);
    CHECK(!nf.hadFrame);
    CHECK_EQ(blank.size(), size_t(0));
}

// A point that has drifted off the surface goes DARK and is COUNTED. A
// mapping that has slipped should be visible as a number, not as fixtures
// that mysteriously stopped responding.
void testOffSurfacePointsAreCounted() {
    RasterFrame white(16, 16);
    white.fill(255, 255, 255);

    Extraction e;
    e.points = {
        SamplePoint{{1, 0}, 0.5f, 0.5f, SampleArea::Point, 0.01f, true},
        SamplePoint{{2, 0}, 1.4f, 0.5f, SampleArea::Point, 0.01f, true},
        SamplePoint{{3, 0}, 0.5f, -0.2f, SampleArea::Point, 0.01f, true},
        SamplePoint{{4, 0}, 0.5f, 0.5f, SampleArea::Point, 0.01f, false},
    };

    OutputLayer out;
    const ExtractionReport rep = e.render(white, out);
    CHECK_EQ(rep.sampled, 1);
    CHECK_EQ(rep.offSurface, 2);
    CHECK_EQ(rep.disabled, 1);

    CHECK_NEAR(red(out, 1), 1.f);
    CHECK_NEAR(red(out, 2), 0.f);
    CHECK_NEAR(red(out, 3), 0.f);
    // A disabled point writes nothing — not black. Nothing is how another
    // layer gets to own that fixture.
    CHECK(red(out, 4) < 0.f);
}

// §13.3's claim: mapped content is a playback. It carries a priority and a
// merge rule, and resolves against what is underneath by the same §11.4
// rules an executor does.
void testExtractionMergesLikeAPlayback() {
    RasterFrame grey(8, 8);
    grey.fill(128, 128, 128);

    UidGen uids;
    ExtractionStore store{uids};

    Extraction& low = store.add([] {
        Extraction e;
        e.name = "Low";
        e.priority = 0;
        e.merge = MergeRule::LTP;
        e.points = {SamplePoint{{1, 0}, 0.5f, 0.5f, SampleArea::Point, 0.01f, true}};
        return e;
    }());
    CHECK(low.uid.valid());
    CHECK_EQ(low.id, 1);

    // A higher-priority LTP layer replaces what is underneath.
    Extraction high;
    high.name = "High";
    high.priority = 10;
    high.merge = MergeRule::LTP;
    high.master = 0.25f;
    high.points = {SamplePoint{{1, 0}, 0.5f, 0.5f, SampleArea::Point, 0.01f, true}};
    store.add(high);

    OutputLayer out;
    store.mergeInto(grey, out);
    CHECK_NEAR(red(out, 1), 128.f / 255.f * 0.25f);

    // Switch the top layer to HTP and the brighter one underneath wins, even
    // though it has lower priority. That is the rule, not a special case.
    store.byId(2)->merge = MergeRule::HTP;
    OutputLayer htp;
    store.mergeInto(grey, htp);
    CHECK_NEAR(htp.get(makeKey(1, 0, Attr::Red)).value(), 128.f / 255.f);

    // And against a value that was already there — a cue's contribution —
    // the same rule applies, which is the point of §13.3. Both layers have
    // to be HTP: the lower one resolves FIRST, so leaving it on LTP would
    // let it replace the cue before the upper one ever saw it.
    store.byId(1)->merge = MergeRule::HTP;
    OutputLayer withCue;
    withCue.set(makeKey(1, 0, Attr::Red), 0.9f);
    store.mergeInto(grey, withCue);
    CHECK_NEAR(withCue.get(makeKey(1, 0, Attr::Red)).value(), 0.9f);  // HTP: cue wins

    store.byId(1)->merge = MergeRule::LTP;
    store.byId(2)->merge = MergeRule::LTP;
    OutputLayer overCue;
    overCue.set(makeKey(1, 0, Attr::Red), 0.9f);
    store.mergeInto(grey, overCue);
    // LTP: the extraction takes it, because it resolved later by priority.
    CHECK(overCue.get(makeKey(1, 0, Attr::Red)).value() < 0.9f);
}

void testStore() {
    UidGen uids;
    ExtractionStore store{uids};
    CHECK_EQ(store.size(), size_t(0));

    Extraction a;
    a.name = "Wall";
    const int ida = store.add(a).id;
    Extraction b;
    const int idb = store.add(b).id;
    CHECK_EQ(ida, 1);
    CHECK_EQ(idb, 2);
    CHECK(store.byId(2)->name == "Extraction 2");
    CHECK(store.byName("Wall") != nullptr);
    CHECK(store.byName("nope") == nullptr);
    CHECK(store.byId(9) == nullptr);

    CHECK(store.remove(1));
    CHECK(!store.remove(1));
    CHECK_EQ(store.size(), size_t(1));

    // A restored layer keeps its own identity — a load must not renumber.
    Extraction loaded;
    loaded.uid = uids.next();
    loaded.id = 42;
    loaded.name = "Restored";
    store.restore(loaded);
    CHECK(store.byId(42) != nullptr);
    CHECK(store.byId(42)->name == "Restored");

    // Per-cell points, for a multi-instance strip patched as one fixture
    // (§1.3): twelve sample points out of one patch entry.
    Extraction bar;
    std::vector<Target> cells;
    for (CellId c = 1; c <= 12; ++c) cells.push_back({201, c});
    bar.setPoints(layoutLine(cells, 0.1f, 0.2f, 0.9f, 0.2f));
    CHECK_EQ(bar.points.size(), size_t(12));
    CHECK(bar.pointFor({201, 1}) != nullptr);
    CHECK(bar.pointFor({201, 12}) != nullptr);
    CHECK(bar.pointFor({201, 13}) == nullptr);
    CHECK_NEAR(bar.pointFor({201, 1})->x, 0.1f);
}

void testNames() {
    CHECK(std::string(sampleAreaName(SampleArea::Box)) == "box");
    CHECK(sampleAreaFromName("point").value() == SampleArea::Point);
    CHECK(sampleAreaFromName("gaussian").value() == SampleArea::Gaussian);
    CHECK(!sampleAreaFromName("blurry").has_value());
    for (SampleArea a : {SampleArea::Point, SampleArea::Box, SampleArea::Gaussian})
        CHECK(sampleAreaFromName(sampleAreaName(a)).value() == a);
}

} // namespace

int main() {
    std::printf("lumen mapping\n");
    testFrameBasics();
    testSampleAreas();
    testEdgeSamplesReadTheContent();
    testLayouts();
    testExtractionWritesColour();
    testOffSurfacePointsAreCounted();
    testExtractionMergesLikeAPlayback();
    testStore();
    testNames();
    return check::report("mapping");
}
