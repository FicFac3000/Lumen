// Tests for the executor surface model (§9), and especially for the one
// operation that "keep both" forces into existence: converting a page from
// one style to the other.
#include "Check.hpp"

#include "lumen/Command.hpp"
#include "lumen/Surface.hpp"
#include "lumen/SurfaceView.hpp"

#include <algorithm>
#include <cstdio>

using namespace lumen;

namespace {

struct Fix {
    UidGen uids;
    SurfaceStore store{uids};
    Uid a = uids.next(), b = uids.next(), c = uids.next(), d = uids.next();

    Surface& wing() {
        Surface s;
        s.id = 1;
        s.name = "Wing";
        s.style = SurfaceStyle::Wing;
        s.cols = 10;
        s.rows = 2;
        return store.add(std::move(s));
    }
    Surface& canvas(int cols = 12, int rows = 8) {
        Surface s;
        s.id = 2;
        s.name = "Show";
        s.style = SurfaceStyle::Canvas;
        s.cols = cols;
        s.rows = rows;
        return store.add(std::move(s));
    }
};

void testNames() {
    CHECK(std::string(surfaceStyleName(SurfaceStyle::Wing)) == "wing");
    CHECK(std::string(surfaceStyleName(SurfaceStyle::Canvas)) == "canvas");
    CHECK(surfaceStyleFromName("wing").value() == SurfaceStyle::Wing);
    CHECK(surfaceStyleFromName("canvas").value() == SurfaceStyle::Canvas);
    // The reference consoles' own names work, because that is what people say.
    CHECK(surfaceStyleFromName("chamsys").value() == SurfaceStyle::Wing);
    CHECK(surfaceStyleFromName("ma3").value() == SurfaceStyle::Canvas);
    CHECK(!surfaceStyleFromName("wings").has_value());

    // Every tile kind round-trips through its name, so the show file and the
    // command layer cannot disagree about what a tile is.
    for (int i = 0; i < static_cast<int>(TileKind::Count); ++i) {
        const TileKind k = static_cast<TileKind>(i);
        CHECK(tileKindFromName(tileKindName(k)).value() == k);
    }
    CHECK(!tileKindFromName("nonsense").has_value());
}

void testWingPlacement() {
    Fix f;
    f.wing();

    CHECK(f.store.placeInSlot(1, 0, TileKind::Executor, f.a).ok);
    CHECK(f.store.placeInSlot(1, 5, TileKind::Executor, f.b).ok);
    CHECK_EQ(f.store.byId(1)->tiles.size(), size_t(2));

    // A wing tile carries a rect as well, so converting later is a pure
    // reinterpretation rather than a reconstruction.
    const Tile* t = f.store.byId(1)->atSlot(5);
    CHECK(t != nullptr);
    CHECK_EQ(t->rect.x, 5);
    CHECK_EQ(t->rect.y, 0);
    CHECK_EQ(t->rect.w, 1);

    // Slot 15 is row 1, column 5 in a 10-wide matrix.
    CHECK(f.store.placeInSlot(1, 15, TileKind::Group, f.c).ok);
    CHECK_EQ(f.store.byId(1)->atSlot(15)->rect.y, 1);

    // On a fixed matrix "put it here" has exactly one meaning, so an occupied
    // slot is replaced — and the caller is told what went.
    const PlaceResult over = f.store.placeInSlot(1, 5, TileKind::Preset, f.d);
    CHECK(over.ok);
    CHECK(over.replaced == f.b);
    CHECK_EQ(f.store.byId(1)->tiles.size(), size_t(3));
    CHECK(f.store.byId(1)->atSlot(5)->target == f.d);

    // Outside the matrix is refused, with the size said out loud.
    const PlaceResult out = f.store.placeInSlot(1, 20, TileKind::Executor, f.a);
    CHECK(!out.ok);
    CHECK(out.error.find("20 slots") != std::string::npos);
    CHECK(!f.store.placeInSlot(1, -1, TileKind::Executor, f.a).ok);
    CHECK(!f.store.placeInSlot(99, 0, TileKind::Executor, f.a).ok);
}

void testCanvasPlacement() {
    Fix f;
    f.canvas();

    CHECK(f.store.placeAt(2, Rect{0, 0, 3, 2}, TileKind::Executor, f.a).ok);
    CHECK(f.store.placeAt(2, Rect{3, 0, 2, 1}, TileKind::Executor, f.b).ok);

    // §9.1: overlap is REFUSED, not nudged. A tile that quietly moved is worse
    // than one that did not appear, because the operator presses where they
    // put it.
    const PlaceResult clash = f.store.placeAt(2, Rect{2, 1, 2, 2}, TileKind::Group, f.c);
    CHECK(!clash.ok);
    CHECK(clash.error.find("overlaps") != std::string::npos);
    CHECK(clash.error.find("9.1") != std::string::npos);
    CHECK_EQ(f.store.byId(2)->tiles.size(), size_t(2));

    // Touching edges is not overlapping.
    CHECK(f.store.placeAt(2, Rect{0, 2, 3, 1}, TileKind::Group, f.c).ok);

    // Off the right-hand edge is refused.
    CHECK(!f.store.placeAt(2, Rect{11, 0, 3, 1}, TileKind::Executor, f.d).ok);
    CHECK(!f.store.placeAt(2, Rect{0, 0, 0, 0}, TileKind::Executor, f.d).ok);

    // Moving obeys the same rule, and a tile may be moved onto itself.
    const Uid first = f.store.byId(2)->tiles[0].uid;
    CHECK(f.store.moveTile(2, first, Rect{0, 0, 3, 2}).ok);
    CHECK(!f.store.moveTile(2, first, Rect{3, 0, 2, 1}).ok);
    CHECK(f.store.moveTile(2, first, Rect{6, 4, 4, 3}).ok);
    CHECK_EQ(f.store.byId(2)->tiles[0].rect.x, 6);

    CHECK(f.store.clearTile(2, first));
    CHECK(!f.store.clearTile(2, first));
    CHECK_EQ(f.store.byId(2)->tiles.size(), size_t(2));
}

// The asymmetry is the whole point: one direction is free, the other costs
// something, and the one that costs has to say so first.
void testConversionIsAsymmetric() {
    Fix f;
    f.wing();
    f.store.placeInSlot(1, 0, TileKind::Executor, f.a);
    f.store.placeInSlot(1, 7, TileKind::Executor, f.b);
    f.store.placeInSlot(1, 13, TileKind::Group, f.c);

    // Wing -> Canvas is lossless: every slot was already a 1x1 rect in the
    // right place, so nothing moves and nothing shrinks.
    const ConvertReport up = f.store.previewStyle(1, SurfaceStyle::Canvas);
    CHECK(up.lossless);
    CHECK_EQ(up.resized, 0);
    CHECK_EQ(up.displaced, 0);

    std::string err;
    const ConvertReport done = f.store.setStyle(1, SurfaceStyle::Canvas, false, &err);
    CHECK(done.lossless);
    CHECK(err.empty());
    CHECK(f.store.byId(1)->style == SurfaceStyle::Canvas);
    CHECK_EQ(f.store.byId(1)->tiles.size(), size_t(3));
    // Position preserved exactly: slot 13 is row 1, column 3.
    for (const Tile& t : f.store.byId(1)->tiles)
        if (t.target == f.c) {
            CHECK_EQ(t.rect.x, 3);
            CHECK_EQ(t.rect.y, 1);
        }

    // And straight back, still lossless, because nothing was ever resized.
    const ConvertReport back = f.store.setStyle(1, SurfaceStyle::Wing, false, &err);
    CHECK(back.lossless);
    CHECK_EQ(f.store.byId(1)->tiles.size(), size_t(3));
    CHECK(f.store.byId(1)->atSlot(13) != nullptr);

    // Converting to the style it already has is a no-op, not an error.
    const ConvertReport same = f.store.setStyle(1, SurfaceStyle::Wing, false, &err);
    CHECK(same.lossless);
    CHECK_EQ(f.store.byId(1)->tiles.size(), size_t(3));
}

void testCanvasToWingReportsItsLosses() {
    Fix f;
    f.canvas(10, 2);   // same matrix a wing would have

    // A big tile: survives, but cannot keep its size.
    f.store.placeAt(2, Rect{0, 0, 3, 2}, TileKind::Executor, f.a);
    // A tile below another in the same column: in a 10-wide matrix it lands in
    // a different slot, so it survives.
    f.store.placeAt(2, Rect{4, 1, 1, 1}, TileKind::Executor, f.b);
    // A tile outside the two rows a wing has: nowhere to go.
    Surface* s = f.store.byId(2);
    s->rows = 8;
    f.store.placeAt(2, Rect{0, 5, 1, 1}, TileKind::Group, f.c);
    s->rows = 2;

    const ConvertReport rep = f.store.previewStyle(2, SurfaceStyle::Wing);
    CHECK(!rep.lossless);
    CHECK_EQ(rep.resized, 1);        // the 3x2
    CHECK_EQ(rep.displaced, 1);      // the one at row 5
    CHECK(rep.wouldDropTiles());
    CHECK(rep.notes.size() >= 2);

    // Previewing changed nothing.
    CHECK(f.store.byId(2)->style == SurfaceStyle::Canvas);
    CHECK_EQ(f.store.byId(2)->tiles.size(), size_t(3));

    // Dropping tiles needs an explicit decision: without force the conversion
    // is refused and the page is untouched.
    std::string err;
    f.store.setStyle(2, SurfaceStyle::Wing, false, &err);
    CHECK(!err.empty());
    CHECK(err.find("nowhere to go") != std::string::npos);
    CHECK(f.store.byId(2)->style == SurfaceStyle::Canvas);
    CHECK_EQ(f.store.byId(2)->tiles.size(), size_t(3));

    // With force it happens, and only the reachable tiles survive.
    err.clear();
    const ConvertReport forced = f.store.setStyle(2, SurfaceStyle::Wing, true, &err);
    CHECK(!forced.lossless);
    CHECK(f.store.byId(2)->style == SurfaceStyle::Wing);
    CHECK_EQ(f.store.byId(2)->tiles.size(), size_t(2));
    // The survivor that was 3x2 is now one slot.
    for (const Tile& t : f.store.byId(2)->tiles) {
        CHECK_EQ(t.rect.w, 1);
        CHECK_EQ(t.rect.h, 1);
    }
}

// Resizing alone is a loss, but not one that loses a tile — so it does not
// need forcing. Getting this backwards would either nag constantly or discard
// layouts silently.
void testResizeAloneDoesNotNeedForcing() {
    Fix f;
    f.canvas(10, 2);
    f.store.placeAt(2, Rect{0, 0, 3, 2}, TileKind::Executor, f.a);
    f.store.placeAt(2, Rect{5, 0, 2, 1}, TileKind::Executor, f.b);

    const ConvertReport rep = f.store.previewStyle(2, SurfaceStyle::Wing);
    CHECK(!rep.lossless);
    CHECK_EQ(rep.resized, 2);
    CHECK_EQ(rep.displaced, 0);
    CHECK(!rep.wouldDropTiles());

    std::string err;
    f.store.setStyle(2, SurfaceStyle::Wing, false, &err);
    CHECK(err.empty());
    CHECK(f.store.byId(2)->style == SurfaceStyle::Wing);
    CHECK_EQ(f.store.byId(2)->tiles.size(), size_t(2));
}

void testValidation() {
    Fix f;
    f.canvas();
    f.store.placeAt(2, Rect{0, 0, 2, 2}, TileKind::Executor, f.a);
    f.store.placeAt(2, Rect{4, 0, 2, 2}, TileKind::Executor, f.b);
    CHECK(f.store.validate(2).empty());

    // Reach past the placement API to construct the broken state a bad load
    // could produce, because validate() exists for exactly that case.
    Surface* s = f.store.byId(2);
    s->tiles[1].rect = Rect{1, 1, 2, 2};
    CHECK_EQ(f.store.validate(2).size(), size_t(1));
    CHECK(f.store.validate(2)[0].find("overlaps") != std::string::npos);

    f.wing();
    f.store.placeInSlot(1, 3, TileKind::Executor, f.a);
    f.store.placeInSlot(1, 4, TileKind::Executor, f.b);
    CHECK(f.store.validate(1).empty());
    f.store.byId(1)->tiles[1].slot = 3;
    CHECK_EQ(f.store.validate(1).size(), size_t(1));
    CHECK(f.store.validate(1)[0].find("slot 4") != std::string::npos);

    f.store.byId(1)->tiles[1].slot = 99;
    CHECK(f.store.validate(1)[0].find("outside") != std::string::npos);

    CHECK_EQ(f.store.validate(77).size(), size_t(1));
}

// Keeping both styles means a show owns pages of each, not one global answer.
void testStoreKeepsBoth() {
    UidGen uids;
    SurfaceStore store{uids};
    installStarterSurfaces(store);

    CHECK_EQ(store.size(), size_t(2));
    CHECK(store.byId(1)->style == SurfaceStyle::Wing);
    CHECK(store.byId(2)->style == SurfaceStyle::Canvas);
    CHECK_EQ(store.current(), 1);

    CHECK(store.setCurrent(2));
    CHECK_EQ(store.current(), 2);
    CHECK(!store.setCurrent(9));
    CHECK_EQ(store.current(), 2);

    CHECK(store.byName("Show") != nullptr);
    CHECK(store.byName("Wing")->id == 1);
    CHECK(store.byName("nope") == nullptr);

    // Installing twice does not duplicate: a show that already has surfaces
    // keeps the ones it has.
    installStarterSurfaces(store);
    CHECK_EQ(store.size(), size_t(2));

    // Removing the current page moves the cursor rather than leaving it
    // pointing at nothing.
    CHECK(store.remove(2));
    CHECK_EQ(store.current(), 1);
    CHECK(!store.remove(2));

    // Added without an id, a page gets the next one.
    Surface extra;
    extra.style = SurfaceStyle::Canvas;
    const int id = store.add(std::move(extra)).id;
    CHECK_EQ(id, 2);
    CHECK(store.byId(2)->name == "Surface 2");
    CHECK(store.byId(2)->uid.valid());
}

// Alignment is the operation that makes a free canvas usable instead of
// merely possible. The thing it must never do is half-happen.
void testAlignmentIsAtomic() {
    Fix f;
    f.canvas(12, 8);
    f.store.placeAt(2, Rect{0, 0, 2, 1}, TileKind::Executor, f.a);
    f.store.placeAt(2, Rect{4, 2, 3, 1}, TileKind::Executor, f.b);
    f.store.placeAt(2, Rect{8, 4, 2, 1}, TileKind::Executor, f.c);
    Surface* s = f.store.byId(2);
    const Uid t0 = s->tiles[0].uid, t1 = s->tiles[1].uid, t2 = s->tiles[2].uid;

    // Nothing selected, or one thing: there is no "align" of one tile.
    CHECK(!f.store.align(2, SurfaceStore::Align::Left).ok);
    f.store.select(2, {t0});
    const SurfaceStore::AlignResult one = f.store.align(2, SurfaceStore::Align::Left);
    CHECK(!one.ok);
    CHECK(one.error.find("at least two") != std::string::npos);

    // Align left: every selected tile takes the leftmost x. These are in
    // different rows, so nothing collides.
    f.store.select(2, {t0, t1, t2});
    const SurfaceStore::AlignResult left = f.store.align(2, SurfaceStore::Align::Left);
    CHECK(left.ok);
    CHECK_EQ(left.moved, 2);
    for (const Tile& t : f.store.byId(2)->tiles) CHECK_EQ(t.rect.x, 0);

    // Match width takes its width from the FIRST selected, not the widest:
    // "make these like that one" needs `that one` to be a choice.
    f.store.select(2, {t1, t0, t2});       // t1 is 3 wide
    CHECK(f.store.align(2, SurfaceStore::Align::MatchWidth).ok);
    for (const Tile& t : f.store.byId(2)->tiles) CHECK_EQ(t.rect.w, 3);

    // Align top would stack all three in one row at x=0 — three 3-wide tiles
    // in the same place. Refused, and NOTHING moves: a half-aligned page is
    // worse than one that did not align, because its shape is one nobody
    // chose.
    std::vector<Rect> before;
    for (const Tile& t : f.store.byId(2)->tiles) before.push_back(t.rect);

    const SurfaceStore::AlignResult bad = f.store.align(2, SurfaceStore::Align::Top);
    CHECK(!bad.ok);
    CHECK(bad.error.find("overlap") != std::string::npos);
    CHECK_EQ(bad.moved, 0);
    for (size_t i = 0; i < before.size(); ++i)
        CHECK(f.store.byId(2)->tiles[i].rect == before[i]);
    CHECK(f.store.validate(2).empty());
}

void testSpreadAndFill() {
    Fix f;
    f.canvas(12, 8);
    f.store.placeAt(2, Rect{0, 0, 2, 1}, TileKind::Executor, f.a);
    f.store.placeAt(2, Rect{3, 0, 2, 1}, TileKind::Executor, f.b);
    f.store.placeAt(2, Rect{10, 0, 2, 1}, TileKind::Executor, f.c);
    Surface* s = f.store.byId(2);
    f.store.select(2, {s->tiles[0].uid, s->tiles[1].uid, s->tiles[2].uid});

    // Spread keeps the outermost two where they are and evens the gaps.
    CHECK(f.store.align(2, SurfaceStore::Align::SpreadH).ok);
    s = f.store.byId(2);
    CHECK_EQ(s->tiles[0].rect.x, 0);
    CHECK_EQ(s->tiles[2].rect.right(), 12);
    const int gap1 = s->tiles[1].rect.x - s->tiles[0].rect.right();
    const int gap2 = s->tiles[2].rect.x - s->tiles[1].rect.right();
    CHECK_EQ(gap1, gap2);
    CHECK(f.store.validate(2).empty());

    // Fill row shares the full width, remainder to the left, ending flush.
    CHECK(f.store.align(2, SurfaceStore::Align::FillRow).ok);
    s = f.store.byId(2);
    int total = 0;
    for (const Tile& t : s->tiles) total += t.rect.w;
    CHECK_EQ(total, 12);
    CHECK_EQ(s->tiles[0].rect.x, 0);
    // Sorted left to right, the row is contiguous with no gaps.
    std::vector<Rect> row;
    for (const Tile& t : s->tiles) row.push_back(t.rect);
    std::sort(row.begin(), row.end(), [](const Rect& a, const Rect& b) { return a.x < b.x; });
    for (size_t i = 1; i < row.size(); ++i) CHECK_EQ(row[i].x, row[i - 1].right());
    CHECK_EQ(row.back().right(), 12);
    CHECK(f.store.validate(2).empty());
}

void testAlignmentRefusedOnAWing() {
    Fix f;
    f.wing();
    f.store.placeInSlot(1, 0, TileKind::Executor, f.a);
    f.store.placeInSlot(1, 1, TileKind::Executor, f.b);
    Surface* s = f.store.byId(1);
    f.store.select(1, {s->tiles[0].uid, s->tiles[1].uid});

    // A wing's slots do not move. Pretending to align them would suggest it
    // is arrangeable when not being arrangeable is the entire point.
    const SurfaceStore::AlignResult r = f.store.align(1, SurfaceStore::Align::Left);
    CHECK(!r.ok);
    CHECK(r.error.find("wing") != std::string::npos);
}

void testSelectionIsPerPageAndOrdered() {
    Fix f;
    f.wing();
    f.canvas();
    f.store.placeInSlot(1, 0, TileKind::Executor, f.a);
    f.store.placeAt(2, Rect{0, 0, 1, 1}, TileKind::Executor, f.b);
    const Uid wingTile = f.store.byId(1)->tiles[0].uid;
    const Uid canvasTile = f.store.byId(2)->tiles[0].uid;

    f.store.select(1, {wingTile});
    f.store.select(2, {canvasTile});
    CHECK_EQ(f.store.selection(1).size(), size_t(1));
    CHECK(f.store.selection(1)[0] == wingTile);
    CHECK(f.store.selection(2)[0] == canvasTile);
    // A page nobody has selected on has an empty selection, not a missing one.
    CHECK(f.store.selection(99).empty());

    // Re-picking an already-selected tile must not reorder: match-width reads
    // the first one, so order is information.
    f.store.selectAlso(2, canvasTile);
    CHECK_EQ(f.store.selection(2).size(), size_t(1));
    f.store.selectAlso(2, wingTile);
    CHECK_EQ(f.store.selection(2).size(), size_t(2));
    CHECK(f.store.selection(2)[0] == canvasTile);

    f.store.deselect(2);
    CHECK(f.store.selection(2).empty());
    CHECK_EQ(f.store.selection(1).size(), size_t(1));
}

// §9.2: a tile's legend, colour and action are a VIEW of the object it points
// at. The thing that must hold is that NOTHING is stored on the tile — rename
// the object and every tile showing it follows, with no bookkeeping.
void testTileViewIsAView() {
    Engine eng;
    Console con(eng);
    installStarterTypes(eng.patch);
    installStarterSurfaces(eng.surfaces);
    con.executeAll("Fixture 1 Thru 4 Patch \"LED Par RGBW\" 4 1 1;"
                   "Fixture 1 At 100;Store Cue 1 1;ClearAll;Assign 1 1");

    GroupDef g;
    g.id = 1;
    g.name = "Front Wash";
    g.members = {{1, 0}, {2, 0}, {3, 0}};
    eng.groups.add(g);

    eng.presets.ensurePool(1, "Colour", Group::Color);
    Preset red;
    red.id = 1;
    red.name = "Red";
    red.group = Group::Color;
    red.values.set(0, 0, Attr::Red, Value::hard(1.f));
    red.values.set(0, 0, Attr::Green, Value::hard(0.f));
    red.values.set(0, 0, Attr::Blue, Value::hard(0.f));
    eng.presets.add(1, red);

    CHECK(con.executeAll("Surface 1;Tile 1 executor 1;Tile 2 group 1;"
                         "Tile 3 preset 1.1;Tile 4 blackout;Tile 5 grandmaster")
              .ok);
    const Surface* sf = eng.surfaces.byId(1);
    CHECK(sf != nullptr);

    // Executor: legend, live state, a fader, and both press commands.
    const TileView x = resolveTile(eng, *sf->atSlot(0));
    CHECK(x.legend == "Exec 1" || !x.legend.empty());
    CHECK(x.command == "Go 1");
    CHECK(x.altCommand == "Off 1");
    CHECK(x.levelCommand == "ExecMaster 1 %d");
    CHECK(x.level >= 0.f);
    CHECK(!x.missing);

    // Group: a count, and NO fader — selection is not a level, and a fader
    // would imply it were one.
    const TileView gv = resolveTile(eng, *sf->atSlot(1));
    CHECK(gv.legend == "Front Wash");
    CHECK(gv.detail == "3 fixtures");
    CHECK(gv.command == "Group 1");
    CHECK(gv.level < 0.f);
    CHECK(gv.levelCommand.empty());

    // A colour preset carries the colour it actually produces. A palette of
    // swatches that all render identically is one you read instead of see.
    const TileView pv = resolveTile(eng, *sf->atSlot(2));
    CHECK(pv.legend == "Red");
    CHECK(pv.colour == "#ff0000");
    CHECK(pv.hue == "colour");
    CHECK(pv.command == "Preset 1.1");

    // Blackout reflects state and offers the opposite action.
    const TileView bv = resolveTile(eng, *sf->atSlot(3));
    CHECK(!bv.live);
    CHECK(bv.command == "Blackout on");
    eng.blackout = true;
    const TileView bv2 = resolveTile(eng, *sf->atSlot(3));
    CHECK(bv2.live);
    CHECK(bv2.command == "Blackout off");

    // The grand master has a fader but NO press command: one that could be
    // changed by a tap rather than a deliberate drag is a blackout waiting
    // to happen.
    const TileView mv = resolveTile(eng, *sf->atSlot(4));
    CHECK(mv.level >= 0.f);
    CHECK(mv.command.empty());
    CHECK(mv.levelCommand == "GrandMaster %d");

    // THE POINT: rename the object, and the tile follows with no write to
    // the tile at all.
    eng.groups.byId(1)->name = "Backlight";
    CHECK(resolveTile(eng, *sf->atSlot(1)).legend == "Backlight");
    CHECK(sf->atSlot(1)->legendOverride.empty());

    // An override beats what the object implies, and survives the rename.
    Tile overridden = *sf->atSlot(1);
    overridden.legendOverride = "BACK";
    CHECK(resolveTile(eng, overridden).legend == "BACK");
}

// A tile whose object is gone must be shown BROKEN, not omitted. Dropping it
// would take its position with it, and on a wing position is the whole
// bargain (§9.6).
void testMissingTargetsAreVisible() {
    Engine eng;
    installStarterSurfaces(eng.surfaces);

    Tile orphan;
    orphan.kind = TileKind::Executor;
    orphan.target = eng.uids.next();   // a UID nothing answers to
    const TileView v = resolveTile(eng, orphan);
    CHECK(v.missing);
    CHECK(!v.legend.empty());
    CHECK(v.command.empty());

    for (TileKind k : {TileKind::Group, TileKind::Preset, TileKind::Recipe,
                       TileKind::Timecode}) {
        Tile t;
        t.kind = k;
        t.target = eng.uids.next();
        const TileView tv = resolveTile(eng, t);
        CHECK(tv.missing);
        CHECK(tv.command.empty());
    }

    // A kind nothing drives yet says so, rather than drawing a tile that
    // looks pressable and is not.
    Tile laser;
    laser.kind = TileKind::LaserZone;
    CHECK(resolveTile(eng, laser).unsupported);

    // A label points at nothing by design, so it is neither missing nor
    // unsupported — it is just layout.
    Tile label;
    label.kind = TileKind::Label;
    label.legendOverride = "FRONT OF HOUSE";
    const TileView lv = resolveTile(eng, label);
    CHECK(!lv.missing);
    CHECK(!lv.unsupported);
    CHECK(lv.legend == "FRONT OF HOUSE");
    CHECK(lv.command.empty());
}

} // namespace

int main() {
    std::printf("lumen surface\n");
    testNames();
    testWingPlacement();
    testCanvasPlacement();
    testConversionIsAsymmetric();
    testCanvasToWingReportsItsLosses();
    testResizeAloneDoesNotNeedForcing();
    testAlignmentIsAtomic();
    testSpreadAndFill();
    testAlignmentRefusedOnAWing();
    testSelectionIsPerPageAndOrdered();
    testTileViewIsAView();
    testMissingTargetsAreVisible();
    testValidation();
    testStoreKeepsBoth();
    return check::report("surface");
}
