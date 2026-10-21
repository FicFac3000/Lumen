// Layout.hpp — placement grid, windows, panel sets, navigation
// (SPEC §2.2, §2.5, §2.6, §2.7).
//
// This is the model the renderer draws. Keeping it separate is not only for
// testing: §16.2 requires the UI to be a client of the command layer, so the
// state it shows has to exist independently of how it is painted.
#pragma once

#include "lumen/Uid.hpp"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace lumen {

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------
struct Rect {
    int x = 0, y = 0, w = 0, h = 0;

    int right() const { return x + w; }
    int bottom() const { return y + h; }
    bool empty() const { return w <= 0 || h <= 0; }
    bool contains(int px, int py) const {
        return px >= x && px < right() && py >= y && py < bottom();
    }
    bool overlaps(const Rect& o) const {
        return x < o.right() && o.x < right() && y < o.bottom() && o.y < bottom();
    }
    bool operator==(const Rect& o) const {
        return x == o.x && y == o.y && w == o.w && h == o.h;
    }
};

// The placement grid from §2.2, with runtime-adjustable granularity. Windows
// snap to it; the operator changes the cell size while working.
class Grid {
public:
    explicit Grid(int cell = 16) { setCell(cell); }

    void setCell(int px);
    int cell() const { return cell_; }

    // Granularity steps the operator cycles through, rather than a free value
    // nobody can hit twice.
    void coarser();
    void finer();

    int snapTo(int v) const;
    Rect snap(const Rect& r) const;
    // Snapped and clamped inside `bounds`, never smaller than one cell.
    Rect fit(const Rect& r, const Rect& bounds) const;

private:
    int cell_ = 16;
};

// ---------------------------------------------------------------------------
// Windows (§2.1, §2.4, §2.7)
// ---------------------------------------------------------------------------
enum class WindowKind : uint8_t {
    FixtureSheet, Patch, CueSheet, PresetPool, GroupPool, RecipePool,
    ExecutorSurface, PhaserEditor, FixtureEditor, LayoutView, CommandLine,
    Settings, Count
};

const char* windowKindName(WindowKind k);
std::optional<WindowKind> windowKindFromName(const std::string& s);

// A grid cursor position inside a window. Restored when the window is raised
// again, which is what §2.7's hard-button behaviour promises.
struct Cursor {
    int row = 0;
    int col = 0;
    bool operator==(const Cursor& o) const { return row == o.row && col == o.col; }
};

struct WindowState {
    WindowKind kind = WindowKind::FixtureSheet;
    Rect rect;
    Cursor cursor;
    int screen = 0;
    bool open = false;
    // Whatever the header gear exposes (§2.4) — sort key, filter display mode,
    // view variant. Opaque here on purpose: each window type owns its meaning,
    // and this is exactly what persists as "the last configuration".
    std::vector<std::pair<std::string, std::string>> settings;

    std::string setting(const std::string& key) const;
    void setSetting(const std::string& key, const std::string& value);
};

// ---------------------------------------------------------------------------
// Bands and panel sets (§2.5, §2.6)
// ---------------------------------------------------------------------------
enum class Band : uint8_t {
    WindowBar, PoolBar, SoftButtons, EncoderBar, CommandLine, Masters, Count
};

enum class Edge : uint8_t { Top, Bottom, Left, Right };

const char* bandName(Band b);

struct BandConfig {
    bool visible = true;
    Edge edge = Edge::Top;
    int size = 48;       // px along the edge's normal
    bool perScreen = false;
};

struct PanelSet {
    std::string name;
    std::array<BandConfig, static_cast<size_t>(Band::Count)> bands{};

    const BandConfig& band(Band b) const { return bands[static_cast<size_t>(b)]; }
    BandConfig& band(Band b) { return bands[static_cast<size_t>(b)]; }
};

// The shipped sets. `Wing` removes the on-screen button bands and the space is
// RECLAIMED by windows rather than left empty (§2.5).
PanelSet panelSetFull();
PanelSet panelSetWing();
PanelSet panelSetProgramming();
PanelSet panelSetShow();

// ---------------------------------------------------------------------------
// Navigation (§2.7)
// ---------------------------------------------------------------------------
// Jumping to a source needs a way back, or it is a one-way trip. Separate from
// undo: undo reverts changes, this reverts LOCATION.
struct Location {
    WindowKind window = WindowKind::FixtureSheet;
    Cursor cursor;
    Uid object{};   // what was focused, when the jump targeted an object
};

class NavStack {
public:
    void push(const Location& from);
    bool canBack() const { return !back_.empty(); }
    bool canForward() const { return !fwd_.empty(); }
    std::optional<Location> back(const Location& current);
    std::optional<Location> forward(const Location& current);
    void clear();
    size_t depth() const { return back_.size(); }

private:
    std::vector<Location> back_;
    std::vector<Location> fwd_;
};

// ---------------------------------------------------------------------------
// WindowManager
// ---------------------------------------------------------------------------
class WindowManager {
public:
    WindowManager();

    Grid& grid() { return grid_; }
    const Grid& grid() const { return grid_; }

    void setScreen(int index, const Rect& bounds);
    Rect screen(int index) const;
    int screenCount() const { return static_cast<int>(screens_.size()); }

    // §2.7: a window button press opens the window if closed, raises it if
    // open, and restores the cursor to where it was. A window never seen
    // before starts at the first cell.
    WindowState& press(WindowKind kind);

    bool isOpen(WindowKind kind) const;
    void close(WindowKind kind);
    WindowState* find(WindowKind kind);
    const WindowState* find(WindowKind kind) const;

    // Z-order, front last. Raising is what the hard button does.
    const std::vector<WindowKind>& order() const { return order_; }
    WindowKind frontmost() const;
    void raise(WindowKind kind);

    // Placement snaps to the grid and refuses to overlap another open window —
    // the one piece of ChamSys rigidity kept, because overlapping targets mean
    // mis-presses (§9.1).
    bool place(WindowKind kind, const Rect& want, std::string* err = nullptr);
    bool allowOverlap = false;

    // Cursor movement. Arrow keys cross between windows at an edge rather than
    // stopping dead (§2.8), unless wrapping is preferred.
    enum class Dir : uint8_t { Up, Down, Left, Right };
    bool moveCursor(Dir d, int rows, int cols);
    bool wrapCursor = false;

    // Clamps a restored cursor to still-valid content, so a deleted row does
    // not leave the cursor pointing nowhere (§2.7).
    void clampCursor(WindowKind kind, int rows, int cols);

    // Panel sets ------------------------------------------------------------
    void addPanelSet(PanelSet set);
    bool selectPanelSet(const std::string& name);
    const PanelSet* panelSet() const { return active_ ? &sets_[*active_] : nullptr; }
    std::vector<std::string> panelSetNames() const;

    // Connecting a wing selects `Wing` and reclaims the band space; the manual
    // override sticks so an explicit choice is not undone by a reconnect.
    void setWingConnected(bool connected);
    bool wingConnected() const { return wing_; }
    bool manualOverride() const { return manual_; }

    // The area left for windows after the active set's bands are subtracted.
    Rect workArea(int screenIndex = 0) const;

    NavStack& nav() { return nav_; }

private:
    // Finds a free slot for a window that would otherwise open on top of
    // another one. Cascades in grid steps.
    Rect firstFree(const Rect& want, const Rect& work, WindowKind ignore,
                   int screenIndex) const;

    Grid grid_;
    std::vector<Rect> screens_;
    std::vector<WindowState> windows_;
    std::vector<WindowKind> order_;
    std::vector<PanelSet> sets_;
    std::optional<size_t> active_;
    std::string beforeWing_;
    bool wing_ = false;
    bool manual_ = false;
    NavStack nav_;
};

} // namespace lumen
