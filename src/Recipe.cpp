#include "lumen/Recipe.hpp"

#include <algorithm>
#include <cmath>

namespace lumen {

// ---------------------------------------------------------------------------
// FxParams
// ---------------------------------------------------------------------------
FxParams FxParams::morph(const FxParams& a, const FxParams& b, float t) {
    const float u = std::clamp(t, 0.f, 1.f);
    auto lerp = [&](float x, float y) { return x + (y - x) * u; };

    FxParams o;
    o.size = lerp(a.size, b.size);
    o.width = lerp(a.width, b.width);
    o.spread = lerp(a.spread, b.spread);
    o.offset = lerp(a.offset, b.offset);
    o.speed = lerp(a.speed, b.speed);
    o.rate = lerp(a.rate, b.rate);

    // Discrete controls switch at the midpoint: there is no halfway between
    // Forward and Reverse, so interpolating them would be meaningless (§4.17).
    const bool second = u >= 0.5f;
    o.direction = second ? b.direction : a.direction;
    o.selection = second ? b.selection : a.selection;
    o.form = second ? b.form : a.form;
    o.gap = second ? b.gap : a.gap;
    return o;
}

FxParams paramsOf(const Recipe& r) {
    FxParams p;
    p.size = r.size;
    p.spread = r.spread;
    p.offset = r.offset;
    p.speed = r.speed;
    p.rate = 1.f;
    if (!r.slots.empty()) {
        const Slot& s = r.slots.front();
        p.width = s.gen.duty;
        p.direction = s.direction;
        p.selection = s.source;
        if (s.gen.kind == GenKind::Waveform) p.form = s.gen.wave;
        p.gap = s.gap;
    }
    return p;
}

void applyParams(Recipe& r, const FxParams& p, std::vector<std::string>* skipped) {
    r.size = p.size;
    r.spread = p.spread;
    r.offset = p.offset;
    r.speed = p.speed * p.rate;

    for (Slot& s : r.slots) {
        s.gen.duty = p.width;
        s.direction = p.direction;
        s.source = p.selection;
        s.gap = p.gap;

        // §4.17: Form applies only where the slot's generator matches. A
        // parameter preset says what it could not do rather than silently
        // rewriting a hand-drawn curve into a sine.
        if (p.form) {
            if (s.gen.kind == GenKind::Waveform) s.gen.wave = *p.form;
            else if (skipped) {
                skipped->push_back(s.label.empty() ? std::string("slot") : s.label);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Instantiation
// ---------------------------------------------------------------------------
InstantiateReport instantiate(const Recipe& r, const std::vector<Target>& targets,
                              const Patch& patch) {
    InstantiateReport rep;
    if (targets.empty()) return rep;

    // Solo on any slot mutes the rest, which is the only practical way to work
    // on one layer of a four-layer effect (§4.7).
    const bool anySolo = std::any_of(r.slots.begin(), r.slots.end(),
                                     [](const Slot& s) { return s.soloed; });

    for (const Slot& s : r.slots) {
        if (s.muted && !s.soloed) continue;
        if (anySolo && !s.soloed) continue;

        Phaser p;
        p.gen = s.gen;
        p.relative = s.relative;
        p.centre = s.centre;

        // Masters re-scale every slot at once; the slot multipliers sit on
        // top, so a slot can run at half the recipe's speed without detaching.
        p.speedHz = r.baseHz * r.speed * s.speedMul;
        p.size = r.size * s.sizeMul * s.opacity;

        p.mods = s.mods;
        p.mods.groups = s.mods.groups * r.spread;
        p.mods.offset = s.mods.offset + s.phaseOffset + r.offset;

        p.source = s.source;
        p.direction = s.direction;
        p.pivot = s.pivot;
        p.gap = s.gap;
        p.variation = s.variation;
        p.stack = s.stack;
        p.attr = s.attr;
        p.role = s.role;

        // Resolve the target per fixture. Where a fixture cannot serve this
        // slot, it is dropped from THIS SLOT only.
        std::vector<FixtureId> unserved;
        for (const Target& t : targets) {
            if (p.resolveAttr(patch, t)) p.targets.push_back(t);
            else unserved.push_back(t.fixture);
        }

        if (!unserved.empty())
            rep.skipped.emplace_back(s.label.empty() ? std::string("slot") : s.label,
                                     std::move(unserved));

        if (p.targets.empty()) {
            ++rep.slotsEmpty;
            continue;
        }
        ++rep.slotsApplied;
        rep.phasers.push_back(std::move(p));
    }
    return rep;
}

// ---------------------------------------------------------------------------
// Library
// ---------------------------------------------------------------------------
Recipe& RecipeLibrary::add(Recipe r) {
    if (!r.uid.valid()) r.uid = uids_->next();
    if (r.id == 0) {
        int maxId = 0;
        for (const auto& e : recipes_) maxId = std::max(maxId, e.id);
        r.id = maxId + 1;
    }
    recipes_.push_back(std::move(r));
    return recipes_.back();
}

Recipe* RecipeLibrary::find(Uid uid) {
    for (auto& r : recipes_)
        if (r.uid == uid) return &r;
    return nullptr;
}

const Recipe* RecipeLibrary::find(Uid uid) const {
    for (const auto& r : recipes_)
        if (r.uid == uid) return &r;
    return nullptr;
}

Recipe* RecipeLibrary::byName(const std::string& name) {
    for (auto& r : recipes_)
        if (r.name == name) return &r;
    return nullptr;
}

ParamPreset& RecipeLibrary::addParams(ParamPreset p) {
    if (!p.uid.valid()) p.uid = uids_->next();
    if (p.id == 0) {
        int maxId = 0;
        for (const auto& e : params_) maxId = std::max(maxId, e.id);
        p.id = maxId + 1;
    }
    params_.push_back(std::move(p));
    return params_.back();
}

const ParamPreset* RecipeLibrary::findParams(Uid uid) const {
    for (const auto& p : params_)
        if (p.uid == uid) return &p;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Starter recipes
// ---------------------------------------------------------------------------
Recipe recipeCircle() {
    Recipe r;
    r.name = "Circle";
    r.baseHz = 0.5f;
    r.tags = {"position", "portable"};

    // THE demonstration of §4.2. Neither slot names a concrete attribute, so
    // the same recipe orbits a moving head's pan/tilt, a laser zone's XY and a
    // video surface's position — whichever the selected fixture provides.
    Slot x;
    x.label = "X";
    x.role = Role::AxisX;
    x.gen.wave = Wave::Cosine;
    x.relative = true;
    x.sizeMul = 1.f;

    Slot y;
    y.label = "Y";
    y.role = Role::AxisY;
    y.gen.wave = Wave::Sine;  // a quarter cycle behind cosine: a circle
    y.relative = true;
    y.sizeMul = 1.f;

    r.slots = {x, y};
    return r;
}

Recipe recipeSineDim() {
    Recipe r;
    r.name = "Sine Dim";
    r.baseHz = 1.f;
    r.tags = {"intensity"};

    Slot s;
    s.label = "Dim";
    s.role = Role::Intensity;
    s.gen.wave = Wave::Sine;
    s.relative = true;
    r.slots = {s};
    return r;
}

Recipe recipeRainbow() {
    Recipe r;
    r.name = "Rainbow";
    r.baseHz = 0.25f;
    r.tags = {"colour"};

    // §4.10: an FX on R/G/B independently crosses through grey. Driving Hue
    // instead goes round the wheel, and Hue is a virtual-kind attribute so no
    // new mechanism is needed.
    Slot h;
    h.label = "Hue";
    h.attr = Attr::Hue;
    h.gen.wave = Wave::Ramp;
    h.relative = false;
    h.centre = 0.5f;
    h.sizeMul = 2.f;  // a full sweep of the wheel
    r.slots = {h};
    return r;
}

Recipe recipeFly() {
    Recipe r;
    r.name = "Fly";
    r.baseHz = 0.4f;
    r.tags = {"position"};

    // A figure-of-eight: the X axis runs at half the Y rate. Expressing it
    // with a slot multiplier rather than two speeds is what keeps the recipe's
    // speed master meaningful.
    Slot x;
    x.label = "X";
    x.role = Role::AxisX;
    x.gen.wave = Wave::Sine;
    x.speedMul = 0.5f;

    Slot y;
    y.label = "Y";
    y.role = Role::AxisY;
    y.gen.wave = Wave::Sine;
    y.speedMul = 1.f;
    y.sizeMul = 0.6f;

    r.slots = {x, y};
    return r;
}

void installStarterRecipes(RecipeLibrary& lib) {
    lib.add(recipeCircle());
    lib.add(recipeSineDim());
    lib.add(recipeRainbow());
    lib.add(recipeFly());
}

} // namespace lumen
