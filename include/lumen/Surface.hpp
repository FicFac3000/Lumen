// Surface.hpp — the executor surface as a model (SPEC §9).
//
// §9 split the MA3/ChamSys question as "flexibility in the canvas, immediacy
// in the tile", and §9.6 kept BOTH styles. Keeping both is only coherent if
// the style is a property of a PAGE rather than a global preference: a show
// has a busking wing and a programmed canvas, and moves between them, rather
// than having one answer imposed on every surface it owns.
//
// So a Surface is a named page with a style and a set of tiles, and the two
// styles are two ways of *placing* the same tiles:
//
//   Wing     a fixed matrix. A tile owns a numbered SLOT. Nothing reflows,
//            ever, because position being permanent is the entire bargain —
//            an empty slot is still a button under a thumb.
//   Canvas   free placement on a snapping grid. A tile owns a RECT, and its
//            size decides how much detail it renders (§9.2).
//
// Both forbid overlap (§9.1): on a busking surface, overlapping targets mean
// mis-presses, and that is not recoverable mid-show.
#pragma once

#include "lumen/Layout.hpp"
#include "lumen/Uid.hpp"

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace lumen {

enum class SurfaceStyle : uint8_t { Wing, Canvas };

const char* surfaceStyleName(SurfaceStyle s);
std::optional<SurfaceStyle> surfaceStyleFromName(const std::string& s);

// §9.3's tile types. The kind decides what a press does; the target UID
// decides what it does it to.
enum class TileKind : uint8_t {
    Executor, Group, Preset, Recipe, Macro,
    GrandMaster, Blackout, Timecode, LaserZone,
    Label, Empty, Count
};

const char* tileKindName(TileKind k);
std::optional<TileKind> tileKindFromName(const std::string& s);

// A tile holds the assigned object's UID, never a copy of its name or colour
// (§8.1, §9.2). Legend and colour are therefore a VIEW: rename the preset and
// every tile showing it updates, with no bookkeeping anywhere.
struct Tile {
    Uid uid{};                   // the tile's own identity
    TileKind kind = TileKind::Empty;
    Uid target{};                // the assigned object

    // Wing placement: a slot index, 0-based, row-major within the matrix.
    int slot = 0;
    // Canvas placement: a grid rectangle. Ignored in Wing style.
    Rect rect;

    // All three remain overridable per tile (§9.2), empty meaning "derive it".
    std::string legendOverride;
    std::string colourOverride;
    std::string symbolOverride;

    bool isEmpty() const { return kind == TileKind::Empty; }
};

// A page. `cols`/`rows` are the wing matrix; the canvas uses cols as its
// width in grid cells and grows downward.
struct Surface {
    Uid uid{};
    // 0 means "not assigned yet": add() then mints the next free id. A default
    // of 1 would make an unset id indistinguishable from a deliberate one, and
    // two pages would quietly share it.
    int id = 0;
    std::string name;
    SurfaceStyle style = SurfaceStyle::Wing;
    int cols = 10;
    int rows = 2;
    std::vector<Tile> tiles;

    int slotCount() const { return cols * rows; }
    const Tile* atSlot(int slot) const;
    Tile* atSlot(int slot);
    const Tile* find(Uid tile) const;
};

// What a placement refused, and why. A bool would make the caller invent the
// explanation, and "it did not go there" is exactly what the operator needs
// told.
struct PlaceResult {
    bool ok = false;
    std::string error;
    Uid placed{};
    Uid replaced{};              // wing style: what was in that slot before
};

// ---------------------------------------------------------------------------
// Converting a page between styles
// ---------------------------------------------------------------------------
// This is the operation "keep both" forces into existence, and it is not
// symmetric:
//
//   Wing -> Canvas   lossless. Every slot becomes a 1x1 rect at the same
//                    visual position, so nothing moves and nothing shrinks.
//   Canvas -> Wing   LOSSY. A 3x2 tile cannot stay 3x2 in a fixed matrix, and
//                    two tiles whose rects start in different cells can land
//                    in the same slot.
//
// So the lossy direction is reported BEFORE it is performed. Silently
// discarding a layout somebody arranged by hand is the kind of thing that is
// discovered during a show.
struct ConvertReport {
    bool lossless = true;
    int resized = 0;                      // tiles that lose their size
    int displaced = 0;                    // tiles that cannot keep a slot
    std::vector<std::string> notes;       // one line per affected tile
    bool wouldDropTiles() const { return displaced > 0; }
};

class SurfaceStore {
public:
    explicit SurfaceStore(UidGen& uids) : uids_(&uids) {}

    Surface& add(Surface s);
    // Re-inserts a page exactly as given — its own UID, id and name. A load
    // must not re-identify what it restores.
    Surface& restore(Surface s);

    Surface* byId(int id);
    const Surface* byId(int id) const;
    Surface* find(Uid uid);
    const Surface* find(Uid uid) const;
    Surface* byName(const std::string& name);
    const std::deque<Surface>& all() const { return surfaces_; }
    size_t size() const { return surfaces_.size(); }
    bool remove(int id);

    // The page the operator is looking at. Separate from the window system:
    // two windows can show different pages of the same show.
    int current() const { return current_; }
    bool setCurrent(int id);

    // Placement ------------------------------------------------------------
    // Wing: `slot` is honoured, and an occupied slot is REPLACED, because on a
    // fixed matrix "put it here" has exactly one meaning.
    PlaceResult placeInSlot(int surfaceId, int slot, TileKind kind, Uid target);
    // Canvas: `rect` is honoured and overlap is REFUSED, because on a free
    // canvas "put it here" might mean on top of something (§9.1).
    PlaceResult placeAt(int surfaceId, const Rect& rect, TileKind kind, Uid target);
    bool clearTile(int surfaceId, Uid tile);
    // Canvas: move or resize an existing tile, with the same overlap rule.
    PlaceResult moveTile(int surfaceId, Uid tile, const Rect& rect);

    // Conversion -----------------------------------------------------------
    // Asks what converting would cost, changing nothing.
    ConvertReport previewStyle(int surfaceId, SurfaceStyle to) const;
    // Performs it. Returns the same report; `force` is required when the
    // conversion would drop tiles, so losing a layout is always a decision
    // somebody made rather than one that happened to them.
    ConvertReport setStyle(int surfaceId, SurfaceStyle to, bool force,
                           std::string* err = nullptr);

    // Validation (§9.1) ----------------------------------------------------
    // Overlapping tiles on a canvas, or two tiles in one wing slot. Returns
    // the offending pairs as text, empty when the page is sound.
    std::vector<std::string> validate(int surfaceId) const;

private:
    Surface* mutableById(int id);

    UidGen* uids_;
    std::deque<Surface> surfaces_;   // deque: references survive growth
    int current_ = 0;
};

// A sensible starting pair, which is what "keep both" means in practice: a
// wing for busking and a canvas for a show you built. Neither is the default
// in the sense of being the only one.
void installStarterSurfaces(SurfaceStore& store);

} // namespace lumen
