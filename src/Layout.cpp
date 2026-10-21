#include "lumen/Layout.hpp"

#include <algorithm>
#include <cctype>

namespace lumen {

namespace {

std::string lower(const std::string& s) {
    std::string o = s;
    std::transform(o.begin(), o.end(), o.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return o;
}

// Granularity steps, rather than a free value nobody can hit twice.
const int kCellSteps[] = {4, 8, 16, 24, 32, 48, 64};

} // namespace

// ---------------------------------------------------------------------------
// Grid
// ---------------------------------------------------------------------------
void Grid::setCell(int px) { cell_ = std::max(1, px); }

void Grid::coarser() {
    for (int s : kCellSteps)
        if (s > cell_) { cell_ = s; return; }
    cell_ = kCellSteps[sizeof(kCellSteps) / sizeof(int) - 1];
}

void Grid::finer() {
    for (int i = static_cast<int>(sizeof(kCellSteps) / sizeof(int)) - 1; i >= 0; --i)
        if (kCellSteps[i] < cell_) { cell_ = kCellSteps[i]; return; }
    cell_ = kCellSteps[0];
}

int Grid::snapTo(int v) const {
    const int c = cell_;
    // Round to nearest, symmetric about zero so negatives behave.
    const int q = (v >= 0) ? ((v + c / 2) / c) : -((-v + c / 2) / c);
    return q * c;
}

Rect Grid::snap(const Rect& r) const {
    Rect o;
    o.x = snapTo(r.x);
    o.y = snapTo(r.y);
    o.w = std::max(cell_, snapTo(r.w));
    o.h = std::max(cell_, snapTo(r.h));
    return o;
}

Rect Grid::fit(const Rect& r, const Rect& bounds) const {
    Rect o = snap(r);
    o.w = std::min(o.w, std::max(cell_, snapTo(bounds.w)));
    o.h = std::min(o.h, std::max(cell_, snapTo(bounds.h)));

    // Snap the position by FLOORING rather than rounding to nearest: rounding
    // after the clamp can push the rect back outside the bounds it was just
    // clamped into.
    auto floorTo = [&](int v) {
        const int q = (v >= 0) ? (v / cell_) : -(((-v) + cell_ - 1) / cell_);
        return q * cell_;
    };
    o.x = std::clamp(o.x, bounds.x, std::max(bounds.x, bounds.right() - o.w));
    o.y = std::clamp(o.y, bounds.y, std::max(bounds.y, bounds.bottom() - o.h));
    o.x = std::max(bounds.x, floorTo(o.x));
    o.y = std::max(bounds.y, floorTo(o.y));
    return o;
}

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------
const char* windowKindName(WindowKind k) {
    switch (k) {
        case WindowKind::FixtureSheet: return "FixtureSheet";
        case WindowKind::Patch: return "Patch";
        case WindowKind::CueSheet: return "CueSheet";
        case WindowKind::PresetPool: return "PresetPool";
        case WindowKind::GroupPool: return "GroupPool";
        case WindowKind::RecipePool: return "RecipePool";
        case WindowKind::ExecutorSurface: return "ExecutorSurface";
        case WindowKind::PhaserEditor: return "PhaserEditor";
        case WindowKind::FixtureEditor: return "FixtureEditor";
        case WindowKind::LayoutView: return "LayoutView";
        case WindowKind::CommandLine: return "CommandLine";
        case WindowKind::Settings: return "Settings";
        case WindowKind::Count: break;
    }
    return "?";
}

std::optional<WindowKind> windowKindFromName(const std::string& s) {
    const std::string k = lower(s);
    for (size_t i = 0; i < static_cast<size_t>(WindowKind::Count); ++i) {
        const auto w = static_cast<WindowKind>(i);
        if (lower(windowKindName(w)) == k) return w;
    }
    if (k == "fixtures" || k == "sheet") return WindowKind::FixtureSheet;
    if (k == "cues") return WindowKind::CueSheet;
    if (k == "presets") return WindowKind::PresetPool;
    if (k == "exec" || k == "surface") return WindowKind::ExecutorSurface;
    return std::nullopt;
}

const char* bandName(Band b) {
    switch (b) {
        case Band::WindowBar: return "WindowBar";
        case Band::PoolBar: return "PoolBar";
        case Band::SoftButtons: return "SoftButtons";
        case Band::EncoderBar: return "EncoderBar";
        case Band::CommandLine: return "CommandLine";
        case Band::Masters: return "Masters";
        case Band::Count: break;
    }
    return "?";
}

// ---------------------------------------------------------------------------
// WindowState
// ---------------------------------------------------------------------------
std::string WindowState::setting(const std::string& key) const {
    for (const auto& [k, v] : settings)
        if (k == key) return v;
    return {};
}

void WindowState::setSetting(const std::string& key, const std::string& value) {
    for (auto& [k, v] : settings) {
        if (k == key) {
            v = value;
            return;
        }
    }
    settings.emplace_back(key, value);
}

// ---------------------------------------------------------------------------
// Panel sets
// ---------------------------------------------------------------------------
namespace {

PanelSet basePanelSet(const char* name) {
    PanelSet s;
    s.name = name;
    s.band(Band::WindowBar) = {true, Edge::Top, 40, false};
    s.band(Band::PoolBar) = {true, Edge::Top, 40, false};
    s.band(Band::SoftButtons) = {true, Edge::Right, 120, false};
    // §2.6: the encoder bar is at the BOTTOM, MA-style.
    s.band(Band::EncoderBar) = {true, Edge::Bottom, 96, true};
    s.band(Band::CommandLine) = {true, Edge::Bottom, 32, false};
    s.band(Band::Masters) = {true, Edge::Right, 80, false};
    return s;
}

} // namespace

PanelSet panelSetFull() { return basePanelSet("Full"); }

PanelSet panelSetWing() {
    PanelSet s = basePanelSet("Wing");
    // The wing supplies the physical buttons, so the on-screen bands go and
    // the space is reclaimed by windows rather than left empty (§2.5).
    s.band(Band::WindowBar).visible = false;
    s.band(Band::PoolBar).visible = false;
    s.band(Band::SoftButtons).visible = false;
    s.band(Band::Masters).visible = false;
    return s;
}

PanelSet panelSetProgramming() {
    PanelSet s = basePanelSet("Programming");
    s.band(Band::PoolBar).size = 56;
    s.band(Band::EncoderBar).size = 128;
    s.band(Band::Masters).visible = false;
    return s;
}

PanelSet panelSetShow() {
    PanelSet s = basePanelSet("Show");
    s.band(Band::PoolBar).visible = false;
    s.band(Band::SoftButtons).visible = false;
    s.band(Band::EncoderBar).visible = false;
    return s;
}

// ---------------------------------------------------------------------------
// NavStack
// ---------------------------------------------------------------------------
void NavStack::push(const Location& from) {
    back_.push_back(from);
    // A new jump invalidates the forward path, as in a browser.
    fwd_.clear();
}

std::optional<Location> NavStack::back(const Location& current) {
    if (back_.empty()) return std::nullopt;
    const Location to = back_.back();
    back_.pop_back();
    fwd_.push_back(current);
    return to;
}

std::optional<Location> NavStack::forward(const Location& current) {
    if (fwd_.empty()) return std::nullopt;
    const Location to = fwd_.back();
    fwd_.pop_back();
    back_.push_back(current);
    return to;
}

void NavStack::clear() {
    back_.clear();
    fwd_.clear();
}

// ---------------------------------------------------------------------------
// WindowManager
// ---------------------------------------------------------------------------
WindowManager::WindowManager() {
    screens_.push_back({0, 0, 1920, 1080});
    addPanelSet(panelSetFull());
    addPanelSet(panelSetWing());
    addPanelSet(panelSetProgramming());
    addPanelSet(panelSetShow());
    selectPanelSet("Full");
}

void WindowManager::setScreen(int index, const Rect& bounds) {
    if (index < 0) return;
    while (static_cast<int>(screens_.size()) <= index) screens_.push_back({0, 0, 0, 0});
    screens_[static_cast<size_t>(index)] = bounds;
}

Rect WindowManager::screen(int index) const {
    if (index < 0 || index >= static_cast<int>(screens_.size())) return {};
    return screens_[static_cast<size_t>(index)];
}

WindowState* WindowManager::find(WindowKind kind) {
    for (auto& w : windows_)
        if (w.kind == kind) return &w;
    return nullptr;
}

const WindowState* WindowManager::find(WindowKind kind) const {
    for (const auto& w : windows_)
        if (w.kind == kind) return &w;
    return nullptr;
}

bool WindowManager::isOpen(WindowKind kind) const {
    const WindowState* w = find(kind);
    return w && w->open;
}

WindowState& WindowManager::press(WindowKind kind) {
    WindowState* w = find(kind);

    if (!w) {
        // Never opened before: start at the first cell, which is the §2.7
        // fallback.
        WindowState s;
        s.kind = kind;
        s.cursor = {0, 0};
        const Rect work = workArea(0);
        s.rect = grid_.fit({work.x, work.y, grid_.cell() * 20, grid_.cell() * 14}, work);
        s.rect = firstFree(s.rect, work, kind, 0);
        s.open = true;
        windows_.push_back(std::move(s));
        order_.push_back(kind);
        return windows_.back();
    }

    // Already known: reopen if closed, and raise either way. The cursor is
    // left exactly as it was, which is the promise the hard button makes.
    w->open = true;
    raise(kind);
    return *w;
}

void WindowManager::close(WindowKind kind) {
    if (WindowState* w = find(kind)) w->open = false;
    order_.erase(std::remove(order_.begin(), order_.end(), kind), order_.end());
}

void WindowManager::raise(WindowKind kind) {
    order_.erase(std::remove(order_.begin(), order_.end(), kind), order_.end());
    order_.push_back(kind);
}

WindowKind WindowManager::frontmost() const {
    // Walk back through the z-order so a closed window is never reported as
    // frontmost.
    for (auto it = order_.rbegin(); it != order_.rend(); ++it)
        if (isOpen(*it)) return *it;
    return WindowKind::Count;
}

bool WindowManager::place(WindowKind kind, const Rect& want, std::string* err) {
    WindowState* w = find(kind);
    if (!w) {
        if (err) *err = "window not open";
        return false;
    }
    const Rect work = workArea(w->screen);
    const Rect snapped = grid_.fit(want, work);

    if (!allowOverlap) {
        for (const auto& other : windows_) {
            if (other.kind == kind || !other.open || other.screen != w->screen) continue;
            if (snapped.overlaps(other.rect)) {
                if (err)
                    *err = std::string("would overlap ") + windowKindName(other.kind);
                return false;
            }
        }
    }
    w->rect = snapped;
    return true;
}

bool WindowManager::moveCursor(Dir d, int rows, int cols) {
    const WindowKind front = frontmost();
    if (front == WindowKind::Count) return false;
    WindowState* w = find(front);
    if (!w || rows <= 0 || cols <= 0) return false;

    int r = w->cursor.row;
    int c = w->cursor.col;
    switch (d) {
        case Dir::Up: --r; break;
        case Dir::Down: ++r; break;
        case Dir::Left: --c; break;
        case Dir::Right: ++c; break;
    }

    if (wrapCursor) {
        r = ((r % rows) + rows) % rows;
        c = ((c % cols) + cols) % cols;
        w->cursor = {r, c};
        return true;
    }

    // Stop at the edge. Crossing into the adjacent window is the renderer's
    // job, since only it knows the spatial arrangement; the model reports that
    // the cursor could not move.
    if (r < 0 || r >= rows || c < 0 || c >= cols) return false;
    w->cursor = {r, c};
    return true;
}

void WindowManager::clampCursor(WindowKind kind, int rows, int cols) {
    WindowState* w = find(kind);
    if (!w) return;
    // Clamp to the nearest valid cell rather than resetting to the origin: a
    // deleted row should not lose the operator's place entirely (§2.7).
    w->cursor.row = rows > 0 ? std::clamp(w->cursor.row, 0, rows - 1) : 0;
    w->cursor.col = cols > 0 ? std::clamp(w->cursor.col, 0, cols - 1) : 0;
}

Rect WindowManager::firstFree(const Rect& want, const Rect& work, WindowKind ignore,
                              int screenIndex) const {
    if (allowOverlap) return want;

    auto clear = [&](const Rect& r) {
        for (const auto& w : windows_) {
            if (w.kind == ignore || !w.open || w.screen != screenIndex) continue;
            if (r.overlaps(w.rect)) return false;
        }
        return true;
    };
    if (clear(want)) return want;

    // Cascade across then down, in grid steps, so a new window lands beside
    // what is already open rather than on top of it.
    const int step = grid_.cell();
    for (int y = work.y; y + want.h <= work.bottom(); y += step) {
        for (int x = work.x; x + want.w <= work.right(); x += step) {
            const Rect probe{x, y, want.h > 0 ? want.w : 0, want.h};
            if (clear(probe)) return probe;
        }
    }
    return want;  // nowhere free: let it overlap rather than refuse to open
}

void WindowManager::addPanelSet(PanelSet set) {
    for (auto& s : sets_) {
        if (s.name == set.name) {
            s = std::move(set);
            return;
        }
    }
    sets_.push_back(std::move(set));
}

bool WindowManager::selectPanelSet(const std::string& name) {
    for (size_t i = 0; i < sets_.size(); ++i) {
        if (sets_[i].name == name) {
            active_ = i;
            return true;
        }
    }
    return false;
}

std::vector<std::string> WindowManager::panelSetNames() const {
    std::vector<std::string> out;
    out.reserve(sets_.size());
    for (const auto& s : sets_) out.push_back(s.name);
    return out;
}

void WindowManager::setWingConnected(bool connected) {
    if (connected == wing_) return;
    wing_ = connected;

    // §2.5: auto-switch on hardware detect, with a manual override that
    // sticks — an explicit choice is not undone by a reconnect.
    if (manual_) return;

    if (connected) {
        beforeWing_ = panelSet() ? panelSet()->name : std::string("Full");
        selectPanelSet("Wing");
    } else if (!beforeWing_.empty()) {
        selectPanelSet(beforeWing_);
        beforeWing_.clear();
    }
}

Rect WindowManager::workArea(int screenIndex) const {
    Rect r = screen(screenIndex);
    const PanelSet* set = panelSet();
    if (!set) return r;

    for (size_t i = 0; i < static_cast<size_t>(Band::Count); ++i) {
        const BandConfig& b = set->bands[i];
        if (!b.visible) continue;  // hidden bands reclaim their space
        switch (b.edge) {
            case Edge::Top:
                r.y += b.size;
                r.h -= b.size;
                break;
            case Edge::Bottom:
                r.h -= b.size;
                break;
            case Edge::Left:
                r.x += b.size;
                r.w -= b.size;
                break;
            case Edge::Right:
                r.w -= b.size;
                break;
        }
    }
    r.w = std::max(0, r.w);
    r.h = std::max(0, r.h);
    return r;
}

} // namespace lumen
