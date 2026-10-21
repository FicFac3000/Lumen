#include "lumen/Radial.hpp"

#include <algorithm>
#include <cmath>

namespace lumen {

namespace {
constexpr float kPi = 3.14159265358979323846f;
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------
float RingGeometry::hubRadius(int depth) const {
    // The hub shrinks as breadcrumb arcs stack inside it, with a floor so it
    // stays a usable cancel/back target at depth.
    const float r = innerRadius - static_cast<float>(crumbInset) -
                    static_cast<float>(depth * crumbStep) - 8.f;
    return std::max(18.f, r);
}

float RingGeometry::sectorStep(int sectorCount) const {
    return sectorCount > 0 ? 360.f / static_cast<float>(sectorCount) : 360.f;
}

float RingGeometry::sectorCentre(int index, int sectorCount) const {
    // Sector 0 is centred at the top (-90 degrees in screen coordinates) and
    // they run clockwise.
    return -90.f + static_cast<float>(index) * sectorStep(sectorCount);
}

int sectorAtAngle(float angleDegrees, int sectorCount) {
    if (sectorCount <= 0) return -1;
    const float step = 360.f / static_cast<float>(sectorCount);
    // Shift by half a sector so the boundary falls between sectors rather than
    // through the middle of sector 0.
    float rel = angleDegrees + 90.f + step * 0.5f;
    rel = std::fmod(rel, 360.f);
    if (rel < 0.f) rel += 360.f;
    const int idx = static_cast<int>(rel / step);
    return idx % sectorCount;
}

// ---------------------------------------------------------------------------
// RadialMenu
// ---------------------------------------------------------------------------
void RadialMenu::open(const RadialNode& root, float cx, float cy) {
    root_ = &root;
    cx_ = cx;
    cy_ = cy;
    path_.clear();
    crumbs_.clear();
    open_ = true;
}

void RadialMenu::close() {
    open_ = false;
    root_ = nullptr;
    path_.clear();
    crumbs_.clear();
}

const RadialNode* RadialMenu::level() const {
    if (!open_ || !root_) return nullptr;
    return path_.empty() ? root_ : path_.back();
}

int RadialMenu::sectorCount() const {
    const RadialNode* l = level();
    return l ? static_cast<int>(l->children.size()) : 0;
}

Hit RadialMenu::hitTest(float px, float py) const {
    Hit h;
    if (!open_) return h;

    const float dx = px - cx_;
    const float dy = py - cy_;
    h.radius = std::sqrt(dx * dx + dy * dy);
    h.angle = std::atan2(dy, dx) * 180.f / kPi;

    const float hub = geometry.hubRadius(static_cast<int>(path_.size()));
    if (h.radius <= hub) {
        h.kind = Hit::Kind::Hub;
        return h;
    }
    if (h.radius < geometry.innerRadius) {
        // Between the hub and the ring: deliberately dead, so a small wobble
        // near the centre does not select a sector.
        h.kind = Hit::Kind::None;
        return h;
    }

    const int n = sectorCount();
    h.sector = sectorAtAngle(h.angle, n);

    if (h.radius > geometry.outerRadius * geometry.flickFactor) h.kind = Hit::Kind::Flick;
    else if (h.radius > geometry.outerRadius) h.kind = Hit::Kind::Outside;
    else h.kind = Hit::Kind::Sector;
    return h;
}

bool RadialMenu::drill(int sector) {
    const RadialNode* l = level();
    if (!l || sector < 0 || sector >= static_cast<int>(l->children.size())) return false;
    const RadialNode& child = l->children[static_cast<size_t>(sector)];
    if (!child.isBranch()) return false;
    path_.push_back(&child);
    crumbs_.push_back(sector);
    return true;
}

void RadialMenu::back() {
    if (path_.empty()) {
        close();
        return;
    }
    path_.pop_back();
    crumbs_.pop_back();
}

std::string RadialMenu::commandFor(int sector, const std::string& subject) const {
    const RadialNode* l = level();
    if (!l || sector < 0 || sector >= static_cast<int>(l->children.size())) return {};
    const RadialNode& child = l->children[static_cast<size_t>(sector)];
    if (child.isBranch()) return {};

    std::string out;
    out.reserve(child.command.size() + subject.size());
    for (char c : child.command) {
        if (c == '$') out += subject;
        else out += c;
    }
    return out;
}

std::string RadialMenu::trail(int sector) const {
    std::string out;
    const RadialNode* cur = root_;
    for (size_t i = 0; i < crumbs_.size() && cur; ++i) {
        const int idx = crumbs_[i];
        if (idx < 0 || idx >= static_cast<int>(cur->children.size())) break;
        const RadialNode& next = cur->children[static_cast<size_t>(idx)];
        if (!out.empty()) out += " › ";
        out += next.label;
        cur = &next;
    }
    const RadialNode* l = level();
    if (l && sector >= 0 && sector < static_cast<int>(l->children.size())) {
        if (!out.empty()) out += " › ";
        out += l->children[static_cast<size_t>(sector)].label;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Default menu (§5.2)
// ---------------------------------------------------------------------------
RadialNode defaultFixtureMenu() {
    RadialNode root;
    root.label = "Fixture";

    auto leaf = [](const char* label, const char* icon, const char* cmd) {
        RadialNode n;
        n.label = label;
        n.icon = icon;
        n.command = cmd;
        return n;
    };
    auto branch = [](const char* label, const char* icon,
                     std::vector<RadialNode> kids) {
        RadialNode n;
        n.label = label;
        n.icon = icon;
        n.children = std::move(kids);
        return n;
    };

    // Sector 0 — top
    root.children.push_back(branch("Intensity", "◐", {
        leaf("At Full", "", "Fixture $ At 100"),
        leaf("At 50", "", "Fixture $ At 50"),
        leaf("At 0", "", "Fixture $ At 0"),
        leaf("Highlight", "", "Fixture $ Highlight on"),
    }));
    // 1 — upper right
    root.children.push_back(branch("Color", "◉", {
        leaf("Open", "", "Fixture $ At White 100"),
        leaf("Red", "", "Fixture $ At Red 100"),
        leaf("Blue", "", "Fixture $ At Blue 100"),
        leaf("Clear", "", "ClearPresets"),
    }));
    // 2 — lower right
    root.children.push_back(branch("Position", "✛", {
        leaf("Home", "", "Fixture $ At Pan 50"),
        leaf("Centre", "", "Fixture $ At Tilt 50"),
    }));
    // 3 — BOTTOM: the branch that opens the second set of actions, each of
    // which branches again (§5.2).
    root.children.push_back(branch("More", "⋯", {
        branch("FX", "∿", {
            leaf("Stop FX", "", "ClearAll"),
        }),
        branch("Store", "▼", {
            leaf("Store Cue", "", "Store Cue"),
        }),
        branch("Patch", "⊞", {
            leaf("Unpatch", "", "Fixture $ Unpatch"),
        }),
        branch("Window", "⌸", {
            leaf("Fixtures", "", "Open FixtureSheet"),
            leaf("Patch", "", "Open Patch"),
        }),
    }));
    // 4 — lower left
    root.children.push_back(leaf("Clear", "⌫", "Clear"));
    // 5 — upper left
    root.children.push_back(leaf("Select", "▣", "Fixture $"));

    return root;
}

} // namespace lumen
