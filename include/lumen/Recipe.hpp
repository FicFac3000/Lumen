// Recipe.hpp — the FX three-tier model (SPEC §4.1–4.4, §4.17).
//
//   Recipe  --instantiates-->  Slots  --compile to-->  Phasers  --> Attributes
//   (named FX)                 (rebindable targets)    (step-based)
//
// ChamSys gives the recipe: one named thing covering several attributes,
// busked with a handful of masters. MA3 gives the phaser: per-attribute,
// step-based, stored in the cue. Neither alone is enough, so they stack rather
// than one being chosen.
//
// The keystone is §4.2: a slot's target is a VARIABLE. Bind two slots to the
// AxisX/AxisY roles and one Circle recipe drives a moving head, a laser zone
// and a video surface, because all three resolve the same roles.
#pragma once

#include "lumen/Phaser.hpp"
#include "lumen/Preset.hpp"

#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace lumen {

// ---------------------------------------------------------------------------
// Slot — a recipe member (§4.1 tier 2)
// ---------------------------------------------------------------------------
struct Slot {
    std::string label;

    // Target: a concrete attribute, or a role resolved per fixture. Role wins
    // when set, which is what makes a recipe portable.
    Attr attr = Attr::Dimmer;
    Role role = Role::None;

    Generator gen;
    bool relative = true;      // §4.6, per slot not per recipe

    // Multipliers against the recipe's masters, so a slot can run at half the
    // recipe's speed without detaching from it.
    float sizeMul = 1.f;
    float speedMul = 1.f;
    float phaseOffset = 0.f;   // turns
    float centre = 0.5f;       // used when relative == false

    SpreadModifiers mods;
    SpreadSource source = SpreadSource::Index;
    Direction direction = Direction::Forward;
    BouncePivot pivot = BouncePivot::AtEnd;
    Gap gap;
    Variation variation;

    // §4.7: layers, so a slow drift and a fast flicker stay separate objects
    // rather than merging into one unreadable phaser.
    int layer = 0;
    StackMode stack = StackMode::Replace;
    float opacity = 1.f;
    bool muted = false;
    bool soloed = false;
};

// ---------------------------------------------------------------------------
// Recipe — tier 3
// ---------------------------------------------------------------------------
struct Recipe {
    Uid uid{};
    int id = 0;
    std::string name;
    std::vector<Slot> slots;

    // The masters, which are what the ChamSys busking feel comes from: four
    // controls that re-scale every slot at once.
    float speed = 1.f;
    float size = 1.f;
    float spread = 1.f;
    float offset = 0.f;

    // Base rate the speed master multiplies.
    float baseHz = 0.5f;

    std::vector<std::string> tags;
};

// ---------------------------------------------------------------------------
// Parameter preset (§4.17)
//
// Distinct from a recipe, and the distinction matters: a recipe holds
// STRUCTURE (which slots, which roles), a parameter preset holds STATE (a
// snapshot of the nine live controls). That is what lets one "fast and wide"
// preset apply across different recipes.
// ---------------------------------------------------------------------------
struct FxParams {
    float size = 1.f;
    float width = 0.5f;       // generator duty
    float spread = 1.f;
    float offset = 0.f;
    float speed = 1.f;
    float rate = 1.f;         // multiplier on top of speed
    Direction direction = Direction::Forward;
    SpreadSource selection = SpreadSource::Index;
    std::optional<Wave> form;  // only applies where the target uses a waveform
    Gap gap;

    // Numeric controls interpolate; discrete ones switch at the midpoint
    // (§4.17), because there is no halfway between Forward and Reverse.
    static FxParams morph(const FxParams& a, const FxParams& b, float t);
};

struct ParamPreset {
    Uid uid{};
    int id = 0;
    std::string name;
    FxParams params;
    double fadeTime = 0.0;   // recall fades rather than snaps
    std::vector<std::string> tags;
};

// Reads the live parameters off a recipe, and writes them back.
FxParams paramsOf(const Recipe& r);
// `skipped` collects controls that could not apply to this recipe, so a recall
// can say what it could not do rather than failing silently.
void applyParams(Recipe& r, const FxParams& p, std::vector<std::string>* skipped = nullptr);

// ---------------------------------------------------------------------------
// Instantiation (§4.1)
// ---------------------------------------------------------------------------
struct InstantiateReport {
    std::vector<Phaser> phasers;
    // Per slot, the fixtures that could not serve its target. The slot is
    // skipped for THOSE FIXTURES, not the fixture dropped from the recipe
    // (§4.2) — so a Circle on a mixed selection still runs its intensity slot
    // on a plain dimmer.
    std::vector<std::pair<std::string, std::vector<FixtureId>>> skipped;
    int slotsApplied = 0;
    int slotsEmpty = 0;   // slots no fixture in the selection could serve
};

InstantiateReport instantiate(const Recipe& r, const std::vector<Target>& targets,
                              const Patch& patch);

// ---------------------------------------------------------------------------
// Library
// ---------------------------------------------------------------------------
class RecipeLibrary {
public:
    explicit RecipeLibrary(UidGen& uids) : uids_(&uids) {}

    Recipe& add(Recipe r);
    Recipe* find(Uid uid);
    const Recipe* find(Uid uid) const;
    Recipe* byName(const std::string& name);
    const std::deque<Recipe>& all() const { return recipes_; }
    size_t size() const { return recipes_.size(); }

    ParamPreset& addParams(ParamPreset p);
    const std::deque<ParamPreset>& paramPresets() const { return params_; }
    const ParamPreset* findParams(Uid uid) const;

private:
    UidGen* uids_;
    std::deque<Recipe> recipes_;
    std::deque<ParamPreset> params_;
};

// The shipped recipes. Circle is the one that proves the role model: two
// slots on AxisX/AxisY, portable across every domain.
Recipe recipeCircle();
Recipe recipeSineDim();
Recipe recipeRainbow();
Recipe recipeFly();        // a figure-of-eight: AxisX at half the AxisY rate
void installStarterRecipes(RecipeLibrary& lib);

} // namespace lumen
