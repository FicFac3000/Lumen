// SurfaceView.hpp — resolving a tile to what it shows and what it does
// (SPEC §9.2, §9.3).
//
// §9.2 makes a tile's legend and colour a VIEW of the assigned object rather
// than typed text: the tile stores a UID (§8.1), so renaming the object
// updates every tile showing it with no bookkeeping anywhere. That resolution
// has to happen somewhere, and it must happen ONCE — in the engine, not in
// each interface that draws a surface. A second renderer that re-derived the
// legend would eventually derive it differently.
//
// It lives in its own translation unit because it needs both the Surface and
// the Engine, and the Engine already owns the SurfaceStore.
#pragma once

#include "lumen/Engine.hpp"
#include "lumen/Surface.hpp"

#include <string>

namespace lumen {

// Everything an interface needs to draw one tile and act on a press. A
// renderer that needs more than this is a renderer deciding something the
// model should have decided.
struct TileView {
    std::string legend;      // the object's name now, or the tile's override
    std::string detail;      // second line: "3 cues", "8 fixtures", "2 Hz"
    std::string symbol;      // §7.1's three-layer identity, where there is one
    std::string colour;      // "#rrggbb" when the object implies one, else ""
    std::string hue;         // a coarse category, for a renderer with a palette

    // What a press sends. Everything a tile does is a command (§16.2), so a
    // tile IS its command and no interface needs a per-kind switch to act.
    std::string command;
    std::string altCommand;  // right-press / long-press; empty when there is none

    bool live = false;       // active, armed, running, on
    float level = -1.f;      // a fader position 0..1, or < 0 for "no fader"
    std::string levelCommand;  // what setting that fader sends, with a %d slot

    // The target is gone — deleted, or from a show whose object no longer
    // exists. Shown as broken rather than omitted: a tile that silently
    // vanishes takes its position with it, and on a wing position is the
    // whole bargain (§9.6).
    bool missing = false;
    // The kind is known but nothing resolves it yet, and saying so is better
    // than drawing a tile that does nothing when pressed.
    bool unsupported = false;
};

TileView resolveTile(const Engine& eng, const Tile& tile);

} // namespace lumen
