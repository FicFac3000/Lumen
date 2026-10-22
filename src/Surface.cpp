#include "lumen/Surface.hpp"

#include <algorithm>
#include <string>

namespace lumen {

const char* surfaceStyleName(SurfaceStyle s) {
    switch (s) {
        case SurfaceStyle::Wing: return "wing";
        case SurfaceStyle::Canvas: return "canvas";
    }
    return "?";
}

std::optional<SurfaceStyle> surfaceStyleFromName(const std::string& s) {
    if (s == "wing" || s == "Wing" || s == "chamsys") return SurfaceStyle::Wing;
    if (s == "canvas" || s == "Canvas" || s == "ma3") return SurfaceStyle::Canvas;
    return std::nullopt;
}

const char* tileKindName(TileKind k) {
    switch (k) {
        case TileKind::Executor: return "executor";
        case TileKind::Group: return "group";
        case TileKind::Preset: return "preset";
        case TileKind::Recipe: return "recipe";
        case TileKind::Macro: return "macro";
        case TileKind::GrandMaster: return "grandmaster";
        case TileKind::Blackout: return "blackout";
        case TileKind::Timecode: return "timecode";
        case TileKind::LaserZone: return "laserzone";
        case TileKind::Label: return "label";
        case TileKind::Empty: return "empty";
        case TileKind::Count: break;
    }
    return "?";
}

std::optional<TileKind> tileKindFromName(const std::string& s) {
    for (int i = 0; i < static_cast<int>(TileKind::Count); ++i) {
        const TileKind k = static_cast<TileKind>(i);
        if (s == tileKindName(k)) return k;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Surface
// ---------------------------------------------------------------------------
const Tile* Surface::atSlot(int s) const {
    for (const Tile& t : tiles)
        if (t.slot == s) return &t;
    return nullptr;
}

Tile* Surface::atSlot(int s) {
    for (Tile& t : tiles)
        if (t.slot == s) return &t;
    return nullptr;
}

const Tile* Surface::find(Uid tile) const {
    for (const Tile& t : tiles)
        if (t.uid == tile) return &t;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Store
// ---------------------------------------------------------------------------
Surface& SurfaceStore::add(Surface s) {
    if (!s.uid.valid()) s.uid = uids_->next();
    if (s.id <= 0) {
        int top = 0;
        for (const Surface& e : surfaces_) top = std::max(top, e.id);
        s.id = top + 1;
    }
    if (s.name.empty()) s.name = "Surface " + std::to_string(s.id);
    for (Tile& t : s.tiles)
        if (!t.uid.valid()) t.uid = uids_->next();
    surfaces_.push_back(std::move(s));
    if (current_ == 0) current_ = surfaces_.back().id;
    return surfaces_.back();
}

Surface& SurfaceStore::restore(Surface s) {
    surfaces_.push_back(std::move(s));
    if (current_ == 0) current_ = surfaces_.back().id;
    return surfaces_.back();
}

Surface* SurfaceStore::mutableById(int id) {
    for (Surface& s : surfaces_)
        if (s.id == id) return &s;
    return nullptr;
}

Surface* SurfaceStore::byId(int id) { return mutableById(id); }

const Surface* SurfaceStore::byId(int id) const {
    for (const Surface& s : surfaces_)
        if (s.id == id) return &s;
    return nullptr;
}

Surface* SurfaceStore::find(Uid uid) {
    for (Surface& s : surfaces_)
        if (s.uid == uid) return &s;
    return nullptr;
}

const Surface* SurfaceStore::find(Uid uid) const {
    for (const Surface& s : surfaces_)
        if (s.uid == uid) return &s;
    return nullptr;
}

Surface* SurfaceStore::byName(const std::string& name) {
    for (Surface& s : surfaces_)
        if (s.name == name) return &s;
    return nullptr;
}

bool SurfaceStore::remove(int id) {
    for (auto it = surfaces_.begin(); it != surfaces_.end(); ++it) {
        if (it->id != id) continue;
        surfaces_.erase(it);
        if (current_ == id) current_ = surfaces_.empty() ? 0 : surfaces_.front().id;
        return true;
    }
    return false;
}

bool SurfaceStore::setCurrent(int id) {
    if (!byId(id)) return false;
    current_ = id;
    return true;
}

// ---------------------------------------------------------------------------
// Placement
// ---------------------------------------------------------------------------
PlaceResult SurfaceStore::placeInSlot(int surfaceId, int slot, TileKind kind,
                                      Uid target) {
    PlaceResult r;
    Surface* s = mutableById(surfaceId);
    if (!s) {
        r.error = "no surface " + std::to_string(surfaceId);
        return r;
    }
    if (slot < 0 || slot >= s->slotCount()) {
        r.error = "slot " + std::to_string(slot + 1) + " is outside this page (" +
                  std::to_string(s->slotCount()) + " slots)";
        return r;
    }

    // On a fixed matrix, "put it here" has exactly one meaning, so an occupied
    // slot is replaced rather than refused. The caller is told what went.
    if (Tile* existing = s->atSlot(slot)) {
        r.replaced = existing->target;
        existing->kind = kind;
        existing->target = target;
        existing->legendOverride.clear();
        existing->colourOverride.clear();
        existing->symbolOverride.clear();
        existing->rect = Rect{slot % s->cols, slot / s->cols, 1, 1};
        r.ok = true;
        r.placed = existing->uid;
        return r;
    }

    Tile t;
    t.uid = uids_->next();
    t.kind = kind;
    t.target = target;
    t.slot = slot;
    // A wing tile still carries a rect, so converting to a canvas is a pure
    // reinterpretation rather than a reconstruction.
    t.rect = Rect{slot % s->cols, slot / s->cols, 1, 1};
    s->tiles.push_back(t);
    r.ok = true;
    r.placed = t.uid;
    return r;
}

namespace {

// §9.1 in one place, so the canvas and the move path cannot disagree.
const Tile* firstOverlap(const Surface& s, const Rect& want, Uid ignore) {
    for (const Tile& t : s.tiles) {
        if (t.uid == ignore) continue;
        if (t.rect.overlaps(want)) return &t;
    }
    return nullptr;
}

std::string describe(const Tile& t) {
    if (!t.legendOverride.empty()) return t.legendOverride;
    return std::string(tileKindName(t.kind)) + " tile";
}

} // namespace

PlaceResult SurfaceStore::placeAt(int surfaceId, const Rect& rect, TileKind kind,
                                  Uid target) {
    PlaceResult r;
    Surface* s = mutableById(surfaceId);
    if (!s) {
        r.error = "no surface " + std::to_string(surfaceId);
        return r;
    }
    if (rect.empty()) {
        r.error = "a tile needs a size";
        return r;
    }
    if (rect.x < 0 || rect.y < 0 || rect.right() > s->cols) {
        r.error = "outside the page (" + std::to_string(s->cols) + " columns wide)";
        return r;
    }
    if (const Tile* clash = firstOverlap(*s, rect, Uid{})) {
        // Refused, not nudged: a tile that quietly moved somewhere else is
        // worse than one that did not appear, because the operator presses
        // where they put it.
        r.error = "overlaps " + describe(*clash) +
                  " — overlapping targets mean mis-presses (SPEC 9.1)";
        return r;
    }

    Tile t;
    t.uid = uids_->next();
    t.kind = kind;
    t.target = target;
    t.rect = rect;
    t.slot = rect.y * s->cols + rect.x;
    s->tiles.push_back(t);
    r.ok = true;
    r.placed = t.uid;
    return r;
}

bool SurfaceStore::clearTile(int surfaceId, Uid tile) {
    Surface* s = mutableById(surfaceId);
    if (!s) return false;
    const size_t before = s->tiles.size();
    s->tiles.erase(std::remove_if(s->tiles.begin(), s->tiles.end(),
                                  [&](const Tile& t) { return t.uid == tile; }),
                   s->tiles.end());
    return s->tiles.size() != before;
}

PlaceResult SurfaceStore::moveTile(int surfaceId, Uid tile, const Rect& rect) {
    PlaceResult r;
    Surface* s = mutableById(surfaceId);
    if (!s) {
        r.error = "no surface " + std::to_string(surfaceId);
        return r;
    }
    Tile* t = nullptr;
    for (Tile& e : s->tiles)
        if (e.uid == tile) t = &e;
    if (!t) {
        r.error = "no such tile on this page";
        return r;
    }
    if (rect.empty()) {
        r.error = "a tile needs a size";
        return r;
    }
    if (rect.x < 0 || rect.y < 0 || rect.right() > s->cols) {
        r.error = "outside the page (" + std::to_string(s->cols) + " columns wide)";
        return r;
    }
    if (const Tile* clash = firstOverlap(*s, rect, tile)) {
        r.error = "overlaps " + describe(*clash) + " (SPEC 9.1)";
        return r;
    }
    t->rect = rect;
    t->slot = rect.y * s->cols + rect.x;
    r.ok = true;
    r.placed = tile;
    return r;
}

// ---------------------------------------------------------------------------
// Conversion
// ---------------------------------------------------------------------------
ConvertReport SurfaceStore::previewStyle(int surfaceId, SurfaceStyle to) const {
    ConvertReport rep;
    const Surface* s = byId(surfaceId);
    if (!s) {
        rep.lossless = false;
        rep.notes.push_back("no surface " + std::to_string(surfaceId));
        return rep;
    }
    if (s->style == to) return rep;

    if (to == SurfaceStyle::Canvas) {
        // Every slot is already a 1x1 rect at the same position, so there is
        // nothing to lose. The canvas simply stops ignoring the rect.
        return rep;
    }

    // Canvas -> Wing. Two separate losses, counted separately because they
    // are different kinds of bad: one changes how a tile looks, the other
    // means a tile has nowhere to be.
    std::vector<int> taken;
    for (const Tile& t : s->tiles) {
        if (t.rect.w > 1 || t.rect.h > 1) {
            ++rep.resized;
            rep.notes.push_back(describe(t) + " shrinks from " +
                                std::to_string(t.rect.w) + "x" +
                                std::to_string(t.rect.h) + " to one slot");
        }
        const int slot = t.rect.y * s->cols + t.rect.x;
        if (slot < 0 || slot >= s->slotCount()) {
            ++rep.displaced;
            rep.notes.push_back(describe(t) + " falls outside the " +
                                std::to_string(s->cols) + "x" + std::to_string(s->rows) +
                                " matrix");
            continue;
        }
        if (std::find(taken.begin(), taken.end(), slot) != taken.end()) {
            ++rep.displaced;
            rep.notes.push_back(describe(t) + " collides with another tile in slot " +
                                std::to_string(slot + 1));
            continue;
        }
        taken.push_back(slot);
    }
    rep.lossless = (rep.resized == 0 && rep.displaced == 0);
    return rep;
}

ConvertReport SurfaceStore::setStyle(int surfaceId, SurfaceStyle to, bool force,
                                     std::string* err) {
    ConvertReport rep = previewStyle(surfaceId, to);
    Surface* s = mutableById(surfaceId);
    if (!s) {
        if (err) *err = "no surface " + std::to_string(surfaceId);
        return rep;
    }
    if (s->style == to) return rep;

    // Losing a hand-made layout is always a decision somebody made, never one
    // that happened to them.
    if (rep.wouldDropTiles() && !force) {
        if (err)
            *err = std::to_string(rep.displaced) +
                   " tile(s) have nowhere to go in a fixed matrix; "
                   "convert anyway to drop them";
        return rep;
    }

    if (to == SurfaceStyle::Canvas) {
        for (Tile& t : s->tiles)
            t.rect = Rect{t.slot % s->cols, t.slot / s->cols, 1, 1};
        s->style = to;
        return rep;
    }

    std::vector<Tile> kept;
    std::vector<int> taken;
    for (Tile& t : s->tiles) {
        const int slot = t.rect.y * s->cols + t.rect.x;
        if (slot < 0 || slot >= s->slotCount()) continue;
        if (std::find(taken.begin(), taken.end(), slot) != taken.end()) continue;
        taken.push_back(slot);
        t.slot = slot;
        t.rect = Rect{slot % s->cols, slot / s->cols, 1, 1};
        kept.push_back(t);
    }
    s->tiles = std::move(kept);
    s->style = to;
    return rep;
}

// ---------------------------------------------------------------------------
// Selection and alignment (§9.1)
// ---------------------------------------------------------------------------
void SurfaceStore::select(int surfaceId, const std::vector<Uid>& tiles) {
    for (auto& e : selected_)
        if (e.first == surfaceId) {
            e.second = tiles;
            return;
        }
    selected_.push_back({surfaceId, tiles});
}

void SurfaceStore::selectAlso(int surfaceId, Uid tile) {
    for (auto& e : selected_)
        if (e.first == surfaceId) {
            // Selection order is information: "match width" takes its width
            // from the FIRST one picked, so re-picking must not reorder.
            for (Uid u : e.second)
                if (u == tile) return;
            e.second.push_back(tile);
            return;
        }
    selected_.push_back({surfaceId, {tile}});
}

void SurfaceStore::deselect(int surfaceId) {
    for (auto& e : selected_)
        if (e.first == surfaceId) e.second.clear();
}

const std::vector<Uid>& SurfaceStore::selection(int surfaceId) const {
    static const std::vector<Uid> none;
    for (const auto& e : selected_)
        if (e.first == surfaceId) return e.second;
    return none;
}

const char* SurfaceStore::alignName(Align a) {
    switch (a) {
        case Align::Left: return "left";
        case Align::Right: return "right";
        case Align::Top: return "top";
        case Align::Bottom: return "bottom";
        case Align::MatchWidth: return "matchwidth";
        case Align::MatchHeight: return "matchheight";
        case Align::SpreadH: return "spreadh";
        case Align::SpreadV: return "spreadv";
        case Align::FillRow: return "fillrow";
    }
    return "?";
}

std::optional<SurfaceStore::Align> SurfaceStore::alignFromName(const std::string& s) {
    const Align all[] = {Align::Left,       Align::Right,       Align::Top,
                         Align::Bottom,     Align::MatchWidth,  Align::MatchHeight,
                         Align::SpreadH,    Align::SpreadV,     Align::FillRow};
    for (Align a : all)
        if (s == alignName(a)) return a;
    return std::nullopt;
}

SurfaceStore::AlignResult SurfaceStore::commitArrangement(
    Surface& s, const std::vector<std::pair<Uid, Rect>>& want) {
    AlignResult r;

    // Build the page as it WOULD be, and check the whole of it. Checking each
    // move as it happens would accept the first three and fail the fourth,
    // leaving a shape nobody asked for.
    std::vector<std::pair<Uid, Rect>> after;
    for (const Tile& t : s.tiles) {
        Rect rect = t.rect;
        for (const auto& w : want)
            if (w.first == t.uid) rect = w.second;
        after.push_back({t.uid, rect});
    }

    for (const auto& a : after) {
        if (a.second.empty() || a.second.x < 0 || a.second.y < 0 ||
            a.second.right() > s.cols) {
            r.error = "that would push a tile off the page";
            return r;
        }
    }
    for (size_t i = 0; i < after.size(); ++i)
        for (size_t k = i + 1; k < after.size(); ++k)
            if (after[i].second.overlaps(after[k].second)) {
                r.error = "that would overlap two tiles (SPEC 9.1)";
                return r;
            }

    for (Tile& t : s.tiles)
        for (const auto& w : want)
            if (w.first == t.uid && !(t.rect == w.second)) {
                t.rect = w.second;
                t.slot = w.second.y * s.cols + w.second.x;
                ++r.moved;
            }
    r.ok = true;
    return r;
}

SurfaceStore::AlignResult SurfaceStore::align(int surfaceId, Align how) {
    AlignResult r;
    Surface* s = mutableById(surfaceId);
    if (!s) {
        r.error = "no surface " + std::to_string(surfaceId);
        return r;
    }
    if (s->style != SurfaceStyle::Canvas) {
        // Nothing to align on a fixed matrix, and pretending otherwise would
        // suggest the wing is arrangeable when its whole point is that it
        // is not.
        r.error = "alignment is for the canvas; a wing's slots do not move";
        return r;
    }

    std::vector<Tile*> sel;
    for (Uid u : selection(surfaceId))
        for (Tile& t : s->tiles)
            if (t.uid == u) sel.push_back(&t);
    if (sel.size() < 2) {
        r.error = "select at least two tiles to align";
        return r;
    }

    std::vector<std::pair<Uid, Rect>> want;
    const Rect first = sel.front()->rect;

    switch (how) {
        case Align::Left: {
            int x = sel.front()->rect.x;
            for (const Tile* t : sel) x = std::min(x, t->rect.x);
            for (Tile* t : sel) want.push_back({t->uid, Rect{x, t->rect.y, t->rect.w, t->rect.h}});
            break;
        }
        case Align::Right: {
            int edge = sel.front()->rect.right();
            for (const Tile* t : sel) edge = std::max(edge, t->rect.right());
            for (Tile* t : sel)
                want.push_back({t->uid, Rect{edge - t->rect.w, t->rect.y, t->rect.w, t->rect.h}});
            break;
        }
        case Align::Top: {
            int y = sel.front()->rect.y;
            for (const Tile* t : sel) y = std::min(y, t->rect.y);
            for (Tile* t : sel) want.push_back({t->uid, Rect{t->rect.x, y, t->rect.w, t->rect.h}});
            break;
        }
        case Align::Bottom: {
            int edge = sel.front()->rect.bottom();
            for (const Tile* t : sel) edge = std::max(edge, t->rect.bottom());
            for (Tile* t : sel)
                want.push_back({t->uid, Rect{t->rect.x, edge - t->rect.h, t->rect.w, t->rect.h}});
            break;
        }
        case Align::MatchWidth:
            // From the FIRST selected, not the widest: "make these like that
            // one" is what the gesture means, and which one is `that one` has
            // to be something the operator chose rather than something the
            // geometry decided.
            for (Tile* t : sel)
                want.push_back({t->uid, Rect{t->rect.x, t->rect.y, first.w, t->rect.h}});
            break;
        case Align::MatchHeight:
            for (Tile* t : sel)
                want.push_back({t->uid, Rect{t->rect.x, t->rect.y, t->rect.w, first.h}});
            break;
        case Align::SpreadH: {
            std::vector<Tile*> row = sel;
            std::sort(row.begin(), row.end(),
                      [](const Tile* a, const Tile* b) { return a->rect.x < b->rect.x; });
            const int lo = row.front()->rect.x;
            const int hi = row.back()->rect.right();
            int used = 0;
            for (const Tile* t : row) used += t->rect.w;
            const int span = hi - lo;
            const int gaps = static_cast<int>(row.size()) - 1;
            if (span - used < 0 || gaps <= 0) {
                r.error = "no room to spread those out";
                return r;
            }
            const int gap = (span - used) / gaps;
            int x = lo;
            for (Tile* t : row) {
                want.push_back({t->uid, Rect{x, t->rect.y, t->rect.w, t->rect.h}});
                x += t->rect.w + gap;
            }
            break;
        }
        case Align::SpreadV: {
            std::vector<Tile*> col = sel;
            std::sort(col.begin(), col.end(),
                      [](const Tile* a, const Tile* b) { return a->rect.y < b->rect.y; });
            const int lo = col.front()->rect.y;
            const int hi = col.back()->rect.bottom();
            int used = 0;
            for (const Tile* t : col) used += t->rect.h;
            const int gaps = static_cast<int>(col.size()) - 1;
            if (hi - lo - used < 0 || gaps <= 0) {
                r.error = "no room to spread those out";
                return r;
            }
            const int gap = (hi - lo - used) / gaps;
            int y = lo;
            for (Tile* t : col) {
                want.push_back({t->uid, Rect{t->rect.x, y, t->rect.w, t->rect.h}});
                y += t->rect.h + gap;
            }
            break;
        }
        case Align::FillRow: {
            // Share one row's full width equally. The remainder goes to the
            // leftmost tiles one cell each, so the row ends flush rather than
            // leaving a ragged cell at the end.
            std::vector<Tile*> row = sel;
            std::sort(row.begin(), row.end(),
                      [](const Tile* a, const Tile* b) { return a->rect.x < b->rect.x; });
            const int n = static_cast<int>(row.size());
            if (n > s->cols) {
                r.error = "more tiles than columns";
                return r;
            }
            const int each = s->cols / n;
            int extra = s->cols % n;
            const int y = row.front()->rect.y;
            int x = 0;
            for (Tile* t : row) {
                const int w = each + (extra > 0 ? 1 : 0);
                if (extra > 0) --extra;
                want.push_back({t->uid, Rect{x, y, w, t->rect.h}});
                x += w;
            }
            break;
        }
    }

    return commitArrangement(*s, want);
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------
std::vector<std::string> SurfaceStore::validate(int surfaceId) const {
    std::vector<std::string> out;
    const Surface* s = byId(surfaceId);
    if (!s) {
        out.push_back("no surface " + std::to_string(surfaceId));
        return out;
    }

    if (s->style == SurfaceStyle::Wing) {
        for (size_t i = 0; i < s->tiles.size(); ++i)
            for (size_t k = i + 1; k < s->tiles.size(); ++k)
                if (s->tiles[i].slot == s->tiles[k].slot)
                    out.push_back(describe(s->tiles[i]) + " and " + describe(s->tiles[k]) +
                                  " both claim slot " + std::to_string(s->tiles[i].slot + 1));
        for (const Tile& t : s->tiles)
            if (t.slot < 0 || t.slot >= s->slotCount())
                out.push_back(describe(t) + " is in slot " + std::to_string(t.slot + 1) +
                              ", outside this page");
        return out;
    }

    for (size_t i = 0; i < s->tiles.size(); ++i)
        for (size_t k = i + 1; k < s->tiles.size(); ++k)
            if (s->tiles[i].rect.overlaps(s->tiles[k].rect))
                out.push_back(describe(s->tiles[i]) + " overlaps " +
                              describe(s->tiles[k]) + " (SPEC 9.1)");
    return out;
}

// ---------------------------------------------------------------------------
// Starter pair
// ---------------------------------------------------------------------------
void installStarterSurfaces(SurfaceStore& store) {
    if (!store.all().empty()) return;

    Surface wing;
    wing.id = 1;
    wing.name = "Wing";
    wing.style = SurfaceStyle::Wing;
    wing.cols = 10;
    wing.rows = 2;
    store.add(std::move(wing));

    Surface canvas;
    canvas.id = 2;
    canvas.name = "Show";
    canvas.style = SurfaceStyle::Canvas;
    canvas.cols = 12;
    canvas.rows = 8;
    store.add(std::move(canvas));

    store.setCurrent(1);
}

} // namespace lumen
