// Preset.hpp — presets, pools and the auto-stepped generator
// (SPEC §7.1–7.5, §8.2, §11.8).
//
// Two things here matter beyond storage:
//
// 1. References RESOLVE LIVE. A cue stores a preset UID, not a copy of its
//    value, so editing the preset updates every cue using it. That is how
//    designers actually work, and it is safe because the reference is a UID
//    (§8.1).
//
// 2. The auto-stepper handles ANCHORED ranges. A rotation attribute runs
//    CCW-fast -> CCW-slow -> stop -> CW-slow -> CW-fast; dividing that into N
//    equal steps produces presets straddling the stop point, which are
//    useless. Steps walk outward from the anchor instead (§7.3).
#pragma once

#include "lumen/Patch.hpp"
#include "lumen/Selection.hpp"
#include "lumen/Value.hpp"

#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace lumen {

// ---------------------------------------------------------------------------
// Preset
// ---------------------------------------------------------------------------
struct Preset {
    Uid uid{};
    int id = 0;              // position within its pool, user-facing
    std::string name;        // unique WITHIN the pool (§8.2)
    int pool = 0;
    Group group = Group::Beam;
    bool generated = false;  // auto-stepped rather than hand-stored
    std::vector<std::string> tags;

    // Values may be fixture-specific, or generic with fixture 0 as a wildcard
    // so one "Red" applies to anything that has the attribute.
    ValueLayer values;

    // §7.1's three-layer identity. The symbol and indicator are derived from
    // the attribute unless overridden.
    std::string symbolOverride;
    std::string indicatorOverride;

    std::string symbol() const;
    std::string indicator() const;

    // The attributes this preset drives, for filtering (§7.5).
    std::vector<Attr> attributes() const;
    bool hasAttr(Attr a) const;
    bool isAnchoredKind() const;   // "only presets with rotations"
    bool isShaper() const;         // "only presets with the frame/shaper attribute"
};

struct PresetPool {
    int id = 0;
    std::string name;
    Group group = Group::Beam;
    std::deque<Preset> presets;  // deque: references stay valid as it grows

    const Preset* byName(const std::string& n) const;
    const Preset* byId(int presetId) const;
    Preset* byId(int presetId);
    // Per-pool uniqueness: a collision auto-suffixes rather than being
    // rejected, so a bulk generation never fails on a name clash (§8.2).
    std::string uniqueName(const std::string& wanted) const;
};

// ---------------------------------------------------------------------------
// Store
// ---------------------------------------------------------------------------
class PresetStore {
public:
    explicit PresetStore(UidGen& uids) : uids_(&uids) {}

    PresetPool& ensurePool(int id, const std::string& name, Group g);
    PresetPool* pool(int id);
    const PresetPool* pool(int id) const;
    PresetPool* poolByName(const std::string& name);
    const std::deque<PresetPool>& pools() const { return pools_; }

    // Adds to a pool, assigning a UID and a unique name.
    Preset& add(int poolId, Preset p);

    // Re-inserts a preset exactly as given — its own UID, id and name. A load
    // must not rename or re-identify what it is restoring, which is why this
    // is separate from add(). The pool is taken from the preset itself.
    Preset& restore(Preset p);

    const Preset* find(Uid uid) const;
    Preset* findMut(Uid uid);

    // §8.2: a bare name resolves only when exactly one pool holds it; on a
    // collision the caller is told which pools matched rather than being given
    // a guess. Same-name presets in different pools are the expected case.
    struct NameLookup {
        const Preset* preset = nullptr;
        std::vector<int> ambiguousPools;
        bool ambiguous() const { return !ambiguousPools.empty(); }
    };
    NameLookup findByName(const std::string& name,
                          std::optional<int> poolContext = std::nullopt) const;

    // Live resolution (§11.8). Exact (fixture, cell) match first, then the
    // wildcard entry, so a generic palette applies to any fixture.
    std::optional<float> resolve(Uid preset, Attr a, FixtureId f, CellId cell) const;

    // Resolves every reference in a layer to current preset values. This is
    // what makes editing a preset update every cue that references it.
    void resolveLayer(ValueLayer& layer) const;

    size_t size() const;

private:
    UidGen* uids_;
    std::deque<PresetPool> pools_;
};

// ---------------------------------------------------------------------------
// Auto-stepped generation (§7.3)
// ---------------------------------------------------------------------------
enum class StepMode : uint8_t {
    Dmx,        // equal steps in DMX value — even spread across a channel
    Physical,   // equal steps in physical value — real-world numbers
    PerSet,     // N steps inside each ChannelSet — gobo wheels, prism facets
    Symmetric,  // outward from a detected anchor — rotation, CTO, pan/tilt
};

const char* stepModeName(StepMode m);

struct StepRequest {
    Attr attr = Attr::Dimmer;
    CellId cell = 0;
    int steps = 5;                 // per side, when Symmetric
    StepMode mode = StepMode::PerSet;
    // Default behaviour: PerSet, with Symmetric applied automatically where an
    // anchor is found. This is the §7.3 default.
    bool autoSymmetric = true;
    std::string nameTemplate = "{attr} {set} {n}/{N}";
    bool keepAnchor = true;        // the anchor becomes a preset of its own
};

struct GeneratedStep {
    std::string name;
    uint8_t dmx = 0;
    float physical = 0.f;
    float normalised = 0.f;   // 0..1, what the preset actually stores
    std::string setName;
    int indexInSide = 0;
    int sideCount = 0;
    int side = 0;             // -1 below the anchor, 0 none, +1 above
    bool isAnchor = false;
};

// Expands a naming template. Tokens: {attr} {set} {n} {N} {phys} {unit}
// {dir} {dmx} {fixtype}
std::string expandTemplate(const std::string& tmpl, const GeneratedStep& s,
                           Attr a, const std::string& fixtureType);

// Plans the steps without creating anything, so the UI can preview and edit
// before committing (§7.3).
std::vector<GeneratedStep> planSteps(const FixtureType& type, const StepRequest& req);

// Creates the planned presets in a pool.
struct GenerateReport {
    int created = 0;
    int skipped = 0;
    std::vector<std::string> names;
    std::string note;
};

GenerateReport generatePresets(PresetStore& store, int poolId, const Patch& patch,
                               const FixtureType& type, const std::vector<Target>& targets,
                               const StepRequest& req);

} // namespace lumen
