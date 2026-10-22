// Tests for the executor surface model (§9), and especially for the one
// operation that "keep both" forces into existence: converting a page from
// one style to the other.
#include "Check.hpp"

#include "lumen/Surface.hpp"

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

} // namespace

int main() {
    std::printf("lumen surface\n");
    testNames();
    testWingPlacement();
    testCanvasPlacement();
    testConversionIsAsymmetric();
    testCanvasToWingReportsItsLosses();
    testResizeAloneDoesNotNeedForcing();
    testValidation();
    testStoreKeepsBoth();
    return check::report("surface");
}
