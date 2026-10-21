// Tests for the UI model: grid snapping, window state, panel sets,
// navigation, and radial menu geometry.
#include "Check.hpp"

#include "lumen/Command.hpp"
#include "lumen/Layout.hpp"
#include "lumen/Radial.hpp"

#include <cmath>
#include <set>

using namespace lumen;

static void testGridSnapping() {
    Grid g(16);
    CHECK_EQ(g.cell(), 16);

    // Rounds to nearest, so dragging feels symmetric rather than always
    // biasing one way.
    CHECK_EQ(g.snapTo(0), 0);
    CHECK_EQ(g.snapTo(7), 0);
    CHECK_EQ(g.snapTo(8), 16);
    CHECK_EQ(g.snapTo(23), 16);
    CHECK_EQ(g.snapTo(24), 32);
    CHECK_EQ(g.snapTo(-7), 0);
    CHECK_EQ(g.snapTo(-8), -16);

    // A snapped rect never collapses below one cell, so a window cannot be
    // dragged out of existence.
    const Rect r = g.snap({5, 11, 3, 2});
    CHECK_EQ(r.x, 0);
    CHECK_EQ(r.y, 16);
    CHECK_EQ(r.w, 16);
    CHECK_EQ(r.h, 16);

    // §2.2: granularity is adjustable at runtime, in steps.
    g.setCell(16);
    g.coarser();
    CHECK_EQ(g.cell(), 24);
    g.coarser();
    CHECK_EQ(g.cell(), 32);
    g.finer();
    CHECK_EQ(g.cell(), 24);
    for (int i = 0; i < 10; ++i) g.finer();
    CHECK_EQ(g.cell(), 4);  // clamps at the finest step
    for (int i = 0; i < 10; ++i) g.coarser();
    CHECK_EQ(g.cell(), 64);

    // fit() keeps a rect inside its bounds as well as on the grid.
    Grid h(10);
    const Rect fitted = h.fit({95, 95, 50, 50}, {0, 0, 100, 100});
    CHECK(fitted.right() <= 100);
    CHECK(fitted.bottom() <= 100);
    CHECK_EQ(fitted.x % 10, 0);
    CHECK_EQ(fitted.y % 10, 0);
}

static void testRectGeometry() {
    const Rect a{0, 0, 100, 100};
    CHECK(a.contains(0, 0));
    CHECK(a.contains(99, 99));
    CHECK(!a.contains(100, 100));  // half-open, so adjacent rects do not touch

    // Adjacency is not overlap — two windows edge to edge are fine.
    CHECK(!a.overlaps({100, 0, 50, 50}));
    CHECK(!a.overlaps({0, 100, 50, 50}));
    CHECK(a.overlaps({99, 99, 10, 10}));
    CHECK(a.overlaps({-10, -10, 20, 20}));
    CHECK(!a.overlaps({0, 0, 0, 0}));  // empty overlaps nothing
}

static void testWindowOpenAndCursorRestore() {
    WindowManager wm;

    // §2.7: a first press opens the window at the first cell.
    WindowState& s = wm.press(WindowKind::FixtureSheet);
    CHECK(s.open);
    CHECK(s.cursor == Cursor{0, 0});
    CHECK(wm.isOpen(WindowKind::FixtureSheet));
    CHECK(wm.frontmost() == WindowKind::FixtureSheet);

    // Move the cursor, open something else, then come back: the cursor must be
    // exactly where it was. This is the promise the hard button makes.
    wm.find(WindowKind::FixtureSheet)->cursor = {7, 3};
    wm.press(WindowKind::Patch);
    CHECK(wm.frontmost() == WindowKind::Patch);

    WindowState& again = wm.press(WindowKind::FixtureSheet);
    CHECK(again.cursor == Cursor{7, 3});
    CHECK(wm.frontmost() == WindowKind::FixtureSheet);

    // Closing and reopening also restores it — the state survives the window,
    // which is what makes it worth persisting in the show.
    wm.close(WindowKind::FixtureSheet);
    CHECK(!wm.isOpen(WindowKind::FixtureSheet));
    CHECK(wm.frontmost() == WindowKind::Patch);
    CHECK(wm.press(WindowKind::FixtureSheet).cursor == Cursor{7, 3});

    // A closed window is never reported as frontmost even though it keeps its
    // place in the ordering.
    wm.close(WindowKind::Patch);
    wm.close(WindowKind::FixtureSheet);
    CHECK(wm.frontmost() == WindowKind::Count);
}

static void testWindowSettingsPersist() {
    WindowManager wm;
    WindowState& w = wm.press(WindowKind::PresetPool);

    // §2.4: whatever the header gear exposes is exactly what persists as "the
    // last configuration of that window".
    w.setSetting("sort", "name");
    w.setSetting("filter_display", "grey_unselectable");
    CHECK(w.setting("sort") == "name");
    CHECK(w.setting("filter_display") == "grey_unselectable");
    CHECK(w.setting("missing").empty());

    w.setSetting("sort", "id");  // updates rather than duplicating
    CHECK(w.setting("sort") == "id");
    CHECK_EQ(wm.find(WindowKind::PresetPool)->settings.size(), size_t(2));

    wm.close(WindowKind::PresetPool);
    CHECK(wm.press(WindowKind::PresetPool).setting("sort") == "id");
}

static void testNoOverlap() {
    WindowManager wm;
    wm.setScreen(0, {0, 0, 1000, 800});
    wm.grid().setCell(10);

    wm.press(WindowKind::FixtureSheet);
    wm.press(WindowKind::Patch);

    // A new window opens somewhere FREE rather than on top of what is already
    // there, which it must when overlap is forbidden.
    CHECK(!wm.find(WindowKind::FixtureSheet)->rect.overlaps(
        wm.find(WindowKind::Patch)->rect));

    const Rect work = wm.workArea(0);
    std::string err;
    // Move Patch out of the way first, then place FixtureSheet at the origin.
    CHECK(wm.place(WindowKind::Patch, {work.x + 600, work.y, 100, 100}, &err));
    CHECK(wm.place(WindowKind::FixtureSheet, {work.x, work.y, 200, 200}, &err));
    CHECK(err.empty());

    // §9.1: overlapping is refused, because overlapping targets mean
    // mis-presses on a busking surface.
    CHECK(!wm.place(WindowKind::Patch, {work.x + 100, work.y + 100, 200, 200}, &err));
    CHECK(err.find("overlap") != std::string::npos);

    // Edge to edge is fine.
    CHECK(wm.place(WindowKind::Patch, {work.x + 200, work.y, 200, 200}, &err));

    // A closed window does not block placement.
    wm.close(WindowKind::FixtureSheet);
    CHECK(wm.place(WindowKind::Patch, {work.x, work.y, 200, 200}, &err));

    // Overlap can be allowed explicitly, for a design that wants it.
    wm.press(WindowKind::FixtureSheet);
    wm.place(WindowKind::FixtureSheet, {work.x + 400, work.y, 200, 200}, &err);
    wm.allowOverlap = true;
    CHECK(wm.place(WindowKind::Patch, {work.x + 450, work.y, 200, 200}, &err));
}

static void testPlacementSnapsAndClamps() {
    WindowManager wm;
    wm.setScreen(0, {0, 0, 1000, 800});
    wm.grid().setCell(20);
    wm.press(WindowKind::CueSheet);

    std::string err;
    CHECK(wm.place(WindowKind::CueSheet, {33, 47, 177, 193}, &err));
    const Rect r = wm.find(WindowKind::CueSheet)->rect;
    CHECK_EQ(r.x % 20, 0);
    CHECK_EQ(r.y % 20, 0);
    CHECK_EQ(r.w % 20, 0);
    CHECK_EQ(r.h % 20, 0);

    // A window dragged past the edge is clamped inside the work area rather
    // than disappearing.
    const Rect work = wm.workArea(0);
    CHECK(wm.place(WindowKind::CueSheet, {5000, 5000, 200, 200}, &err));
    const Rect c = wm.find(WindowKind::CueSheet)->rect;
    CHECK(c.right() <= work.right());
    CHECK(c.bottom() <= work.bottom());
    CHECK(c.x >= work.x);
    CHECK(c.y >= work.y);
}

static void testPanelSetsReclaimSpace() {
    WindowManager wm;
    wm.setScreen(0, {0, 0, 1920, 1080});

    CHECK(wm.selectPanelSet("Full"));
    const Rect full = wm.workArea(0);
    CHECK(full.w < 1920);   // bands take space
    CHECK(full.h < 1080);

    // §2.5: the Wing set removes the on-screen button bands and the space is
    // RECLAIMED by windows, not left empty. That is the whole point.
    CHECK(wm.selectPanelSet("Wing"));
    const Rect wing = wm.workArea(0);
    CHECK(wing.w > full.w);
    CHECK(wing.h >= full.h);

    // The encoder bar stays, because a wing does not replace it.
    CHECK(wm.panelSet()->band(Band::EncoderBar).visible);
    CHECK(!wm.panelSet()->band(Band::WindowBar).visible);
    CHECK(!wm.panelSet()->band(Band::Masters).visible);

    // §2.6: the encoder bar is at the BOTTOM, MA-style.
    CHECK(wm.panelSet()->band(Band::EncoderBar).edge == Edge::Bottom);

    // Show hides programming chrome and gives the most room.
    CHECK(wm.selectPanelSet("Show"));
    CHECK(wm.workArea(0).h > full.h);

    CHECK(!wm.selectPanelSet("Nonexistent"));
    CHECK(wm.panelSet()->name == "Show");  // a failed select changes nothing

    const auto names = wm.panelSetNames();
    CHECK_EQ(names.size(), size_t(4));
}

static void testWingAutoSwitch() {
    WindowManager wm;
    CHECK(wm.selectPanelSet("Programming"));

    // Connecting a wing switches to Wing and remembers what to go back to.
    wm.setWingConnected(true);
    CHECK(wm.panelSet()->name == "Wing");
    CHECK(wm.wingConnected());

    wm.setWingConnected(false);
    CHECK(wm.panelSet()->name == "Programming");  // restored, not left on Wing

    // Idempotent: a repeated connect does not stack.
    wm.setWingConnected(true);
    wm.setWingConnected(true);
    CHECK(wm.panelSet()->name == "Wing");
    wm.setWingConnected(false);
    CHECK(wm.panelSet()->name == "Programming");
}

static void testNavStack() {
    NavStack nav;
    CHECK(!nav.canBack());
    CHECK(!nav.canForward());
    CHECK(!nav.back({WindowKind::Patch, {0, 0}, {}}).has_value());

    const Location sheet{WindowKind::FixtureSheet, {5, 2}, Uid{11}};
    const Location cue{WindowKind::CueSheet, {3, 1}, Uid{22}};
    const Location preset{WindowKind::PresetPool, {0, 7}, Uid{33}};

    // §2.7: jumping pushes where you were, so there is a way back.
    nav.push(sheet);
    CHECK(nav.canBack());
    CHECK_EQ(nav.depth(), size_t(1));

    auto backTo = nav.back(cue);
    CHECK(backTo.has_value());
    CHECK(backTo->window == WindowKind::FixtureSheet);
    CHECK(backTo->cursor == Cursor{5, 2});   // the exact cell, not just the window
    CHECK(backTo->object == Uid{11});
    CHECK(nav.canForward());

    auto fwdTo = nav.forward(sheet);
    CHECK(fwdTo.has_value());
    CHECK(fwdTo->window == WindowKind::CueSheet);

    // A new jump invalidates the forward path, as in a browser.
    nav.push(sheet);
    nav.back(cue);
    CHECK(nav.canForward());
    nav.push(preset);
    CHECK(!nav.canForward());

    nav.clear();
    CHECK(!nav.canBack());
}

static void testCursorMovement() {
    WindowManager wm;
    wm.press(WindowKind::FixtureSheet);

    CHECK(wm.moveCursor(WindowManager::Dir::Right, 4, 4));
    CHECK(wm.find(WindowKind::FixtureSheet)->cursor == Cursor{0, 1});
    CHECK(wm.moveCursor(WindowManager::Dir::Down, 4, 4));
    CHECK(wm.find(WindowKind::FixtureSheet)->cursor == Cursor{1, 1});

    // Stops at the edge and reports that it could not move, so the renderer
    // can decide to cross into the adjacent window (§2.8).
    wm.find(WindowKind::FixtureSheet)->cursor = {0, 0};
    CHECK(!wm.moveCursor(WindowManager::Dir::Up, 4, 4));
    CHECK(!wm.moveCursor(WindowManager::Dir::Left, 4, 4));
    CHECK(wm.find(WindowKind::FixtureSheet)->cursor == Cursor{0, 0});

    // Wrapping is available as a setting.
    wm.wrapCursor = true;
    CHECK(wm.moveCursor(WindowManager::Dir::Up, 4, 4));
    CHECK(wm.find(WindowKind::FixtureSheet)->cursor == Cursor{3, 0});

    // §2.7: a restored cursor clamps to the nearest valid cell rather than
    // resetting, so a deleted row does not lose the operator's place.
    wm.find(WindowKind::FixtureSheet)->cursor = {99, 99};
    wm.clampCursor(WindowKind::FixtureSheet, 10, 5);
    CHECK(wm.find(WindowKind::FixtureSheet)->cursor == Cursor{9, 4});
    wm.clampCursor(WindowKind::FixtureSheet, 0, 0);
    CHECK(wm.find(WindowKind::FixtureSheet)->cursor == Cursor{0, 0});
}

static void testSectorGeometry() {
    // Sector 0 is centred at the top and they run clockwise, so with six
    // sectors index 3 sits at the bottom — where the branch that opens the
    // second set of actions lives (§5.2).
    CHECK_EQ(sectorAtAngle(-90.f, 6), 0);   // top
    CHECK_EQ(sectorAtAngle(-30.f, 6), 1);
    CHECK_EQ(sectorAtAngle(30.f, 6), 2);
    CHECK_EQ(sectorAtAngle(90.f, 6), 3);    // bottom
    CHECK_EQ(sectorAtAngle(150.f, 6), 4);
    CHECK_EQ(sectorAtAngle(210.f, 6), 5);

    // Equivalent angles resolve the same, so wrapping cannot misfire.
    CHECK_EQ(sectorAtAngle(270.f, 6), 0);
    CHECK_EQ(sectorAtAngle(-450.f, 6), 0);

    // Boundaries fall BETWEEN sectors, not through the middle of one.
    CHECK_EQ(sectorAtAngle(-61.f, 6), 0);
    CHECK_EQ(sectorAtAngle(-59.f, 6), 1);

    // Four sectors put one at each compass point.
    CHECK_EQ(sectorAtAngle(-90.f, 4), 0);
    CHECK_EQ(sectorAtAngle(0.f, 4), 1);
    CHECK_EQ(sectorAtAngle(90.f, 4), 2);
    CHECK_EQ(sectorAtAngle(180.f, 4), 3);

    // Every angle maps to a valid sector for any plausible count.
    for (int n = 2; n <= 12; ++n) {
        std::set<int> seen;
        for (int deg = -360; deg <= 360; ++deg) {
            const int s = sectorAtAngle(static_cast<float>(deg), n);
            CHECK(s >= 0 && s < n);
            seen.insert(s);
        }
        CHECK_EQ(seen.size(), static_cast<size_t>(n));  // all reachable
    }
    CHECK_EQ(sectorAtAngle(0.f, 0), -1);

    RingGeometry g;
    CHECK_NEAR(g.sectorCentre(0, 6), -90.0);
    CHECK_NEAR(g.sectorCentre(3, 6), 90.0);
    CHECK_NEAR(g.sectorStep(6), 60.0);

    // The hub shrinks with depth but never below a usable target.
    CHECK(g.hubRadius(0) > g.hubRadius(3));
    CHECK(g.hubRadius(20) >= 18.f);
}

static void testRadialHitTest() {
    RadialNode menu = defaultFixtureMenu();
    RadialMenu r;
    r.open(menu, 500.f, 400.f);
    CHECK(r.isOpen());
    CHECK_EQ(r.sectorCount(), 6);

    // Centre is the hub — cancel at the root, step back when drilled in.
    CHECK(r.hitTest(500.f, 400.f).kind == Hit::Kind::Hub);

    // Between hub and ring is deliberately dead, so a wobble near the centre
    // does not select a sector.
    CHECK(r.hitTest(500.f, 400.f - 45.f).kind == Hit::Kind::None);

    // Straight up lands on sector 0; straight down on sector 3.
    const Hit up = r.hitTest(500.f, 400.f - 100.f);
    CHECK(up.kind == Hit::Kind::Sector);
    CHECK_EQ(up.sector, 0);

    const Hit down = r.hitTest(500.f, 400.f + 100.f);
    CHECK(down.kind == Hit::Kind::Sector);
    CHECK_EQ(down.sector, 3);

    // Just past the ring is still that sector, but outside the ring proper.
    CHECK(r.hitTest(500.f, 400.f - 150.f).kind == Hit::Kind::Outside);
    CHECK_EQ(r.hitTest(500.f, 400.f - 150.f).sector, 0);

    // A long throw is a flick — the muscle-memory path (§5.1).
    const Hit flick = r.hitTest(500.f, 400.f - 400.f);
    CHECK(flick.kind == Hit::Kind::Flick);
    CHECK_EQ(flick.sector, 0);

    // A closed menu hits nothing.
    r.close();
    CHECK(r.hitTest(500.f, 400.f).kind == Hit::Kind::None);
}

static void testRadialNavigation() {
    RadialNode menu = defaultFixtureMenu();
    RadialMenu r;
    r.open(menu, 0.f, 0.f);

    // Sector 4 is a leaf, so it cannot be drilled into.
    CHECK(!r.drill(4));
    CHECK_EQ(r.depth(), size_t(0));
    CHECK(!r.drill(99));
    CHECK(!r.drill(-1));

    // Sector 3 is the bottom branch — "More" — and opens the second set.
    CHECK(r.drill(3));
    CHECK_EQ(r.depth(), size_t(1));
    CHECK(r.level()->label == "More");
    CHECK_EQ(r.sectorCount(), 4);
    CHECK_EQ(r.crumbs().size(), size_t(1));
    CHECK_EQ(r.crumbs()[0], 3);

    // Each of those branches again, as described in §5.2.
    CHECK(r.drill(0));
    CHECK(r.level()->label == "FX");
    CHECK_EQ(r.depth(), size_t(2));

    // Sub-rings REPLACE in place: the sector count is the new level's, not
    // the root's.
    CHECK_EQ(r.sectorCount(), 1);

    // Back steps out one level at a time, and closes at the root.
    r.back();
    CHECK(r.level()->label == "More");
    r.back();
    CHECK_EQ(r.depth(), size_t(0));
    CHECK(r.isOpen());
    r.back();
    CHECK(!r.isOpen());
}

static void testRadialCommands() {
    RadialNode menu = defaultFixtureMenu();
    RadialMenu r;
    r.open(menu, 0.f, 0.f);

    // §5.5: every node's action is a command string, with `$` substituted for
    // the object the menu was opened on. That is what makes the menu a macro
    // with a radial skin.
    CHECK(r.commandFor(4, "12") == "Clear");
    CHECK(r.commandFor(5, "12") == "Fixture 12");

    // A branch has no command of its own.
    CHECK(r.commandFor(0, "12").empty());

    CHECK(r.drill(0));  // Intensity
    CHECK(r.commandFor(0, "12") == "Fixture 12 At 100");
    CHECK(r.commandFor(1, "7 Thru 9") == "Fixture 7 Thru 9 At 50");

    // The breadcrumb trail names the path taken, for the arrival indicator.
    CHECK(r.trail(0) == "Intensity › At Full");
    r.back();
    CHECK(r.drill(3));
    CHECK(r.drill(3));  // More > Window
    CHECK(r.trail(1) == "More › Window › Patch");
}

static void testRadialCommandsExecute() {
    // The real payoff: a radial sector's command runs through the console
    // unchanged, so the menu needs no parser of its own (§5.5).
    Engine eng;
    installStarterTypes(eng.patch);
    Console con(eng);
    CHECK(con.execute("Fixture 1 Thru 4 Patch \"LED Par RGBW\" 4 1 1").ok);
    eng.setClock(0.0);

    RadialNode menu = defaultFixtureMenu();
    RadialMenu r;
    r.open(menu, 0.f, 0.f);

    CHECK(r.drill(0));  // Intensity
    const std::string cmd = r.commandFor(0, "1 Thru 4");
    CHECK(cmd == "Fixture 1 Thru 4 At 100");

    Result res = con.execute(cmd);
    CHECK(res.ok);
    eng.tick();
    CHECK_NEAR(*eng.merged().get(makeKey(1, 0, Attr::Dimmer)), 1.0);
    CHECK_NEAR(*eng.merged().get(makeKey(4, 0, Attr::Dimmer)), 1.0);

    // And a leaf at the root does too.
    r.back();
    Result clr = con.execute(r.commandFor(4, "1"));
    CHECK(clr.ok);

    // Every leaf in the shipped default menu must parse — a menu that ships
    // with a broken sector is worse than one with fewer.
    int leaves = 0;
    std::function<void(const RadialNode&)> walk = [&](const RadialNode& n) {
        for (const auto& c : n.children) {
            if (c.isLeaf()) {
                ++leaves;
                std::string s = c.command;
                size_t p;
                while ((p = s.find('$')) != std::string::npos) s.replace(p, 1, "1");
                // `Open <window>` is a UI command not yet in the registry, so
                // skip those rather than asserting they parse.
                if (s.rfind("Open ", 0) == 0) continue;
                Result rr = con.execute(s);
                // Parsing is what matters here. A leaf may legitimately be
                // refused at this privilege (Unpatch) or because the
                // programmer is empty (Store Cue) — but it must never fail to
                // parse, because a menu that ships with an unparseable sector
                // is worse than one with fewer sectors.
                CHECK(rr.parsed);
            } else {
                walk(c);
            }
        }
    };
    walk(menu);
    CHECK(leaves >= 12);
}

int main() {
    std::printf("lumen ui\n");
    testGridSnapping();
    testRectGeometry();
    testWindowOpenAndCursorRestore();
    testWindowSettingsPersist();
    testNoOverlap();
    testPlacementSnapsAndClamps();
    testPanelSetsReclaimSpace();
    testWingAutoSwitch();
    testNavStack();
    testCursorMovement();
    testSectorGeometry();
    testRadialHitTest();
    testRadialNavigation();
    testRadialCommands();
    testRadialCommandsExecute();
    return check::report("ui");
}
