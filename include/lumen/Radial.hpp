// Radial.hpp — the radial action menu (SPEC §5).
//
// A sector is a LEAF (fires a command string) or a BRANCH (opens the next
// ring). Because every node's action is a command string, the menu is a macro
// with a radial skin: anything typeable is bindable, and the parser is reused
// rather than duplicated.
//
// Sub-rings REPLACE in place with a breadcrumb, rather than growing as
// concentric rings — that keeps the radius readable at depth.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lumen {

struct RadialNode {
    std::string label;
    std::string icon;
    std::string command;          // a console command string; empty on a branch
    std::vector<RadialNode> children;

    bool isBranch() const { return !children.empty(); }
    bool isLeaf() const { return children.empty(); }
};

// Ring geometry. Sector 0 is centred at the TOP and they run clockwise, so
// with six sectors index 3 sits at the bottom — which is where the reference
// interaction puts the branch that opens the second set of actions.
struct RingGeometry {
    float innerRadius = 62.f;
    float outerRadius = 136.f;
    float gapDegrees = 2.4f;
    float flickFactor = 1.42f;   // beyond this multiple of the outer radius, a
                                 // drag counts as a flick
    int crumbInset = 11;         // breadcrumb arc inset per level
    int crumbStep = 7;

    float hubRadius(int depth) const;
    float sectorStep(int sectorCount) const;
    float sectorCentre(int index, int sectorCount) const;  // degrees
};

// What a pointer position resolves to.
struct Hit {
    enum class Kind : uint8_t { None, Hub, Sector, Outside, Flick };
    Kind kind = Kind::None;
    int sector = -1;
    float radius = 0.f;
    float angle = 0.f;  // degrees, 0 = right, +90 = down (screen coords)
};

// Live state of an open menu.
class RadialMenu {
public:
    void open(const RadialNode& root, float cx, float cy);
    void close();
    bool isOpen() const { return open_; }

    // The ring currently shown — the root, or the branch drilled into.
    const RadialNode* level() const;
    size_t depth() const { return path_.size(); }
    int sectorCount() const;

    // Breadcrumb: which sector was taken at each level above the current one.
    const std::vector<int>& crumbs() const { return crumbs_; }

    float centreX() const { return cx_; }
    float centreY() const { return cy_; }

    Hit hitTest(float px, float py) const;

    // Drilling in; returns false if that sector is a leaf or does not exist.
    bool drill(int sector);
    // Stepping back out. At the root this closes the menu.
    void back();

    // Resolves a sector to the command it would fire, after substituting `$`
    // for the object the menu was opened on. Empty for a branch.
    std::string commandFor(int sector, const std::string& subject) const;

    // The breadcrumb text a UI shows on arrival, e.g. "More > FX > Circle".
    std::string trail(int sector) const;

    RingGeometry geometry;

    // §10.2 trigger settings that the model needs to resolve.
    int holdMs = 180;        // before a branch auto-opens while held
    bool flickToExecute = true;

private:
    bool open_ = false;
    float cx_ = 0.f, cy_ = 0.f;
    const RadialNode* root_ = nullptr;
    std::vector<const RadialNode*> path_;
    std::vector<int> crumbs_;
};

// Maps a pointer angle to a sector. Exposed because it is the piece most
// worth testing directly.
int sectorAtAngle(float angleDegrees, int sectorCount);

// The default fixture menu from §5.2: six sectors with the branch that opens
// the second set of actions at the bottom.
RadialNode defaultFixtureMenu();

} // namespace lumen
