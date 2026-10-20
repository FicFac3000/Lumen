// Selection.hpp — ordered, hierarchical selection and spread (SPEC §20).
//
// Selection order is not cosmetic: it IS the FX phase distribution and the fan
// direction. And selection is a TREE, not a list (§20.8) — flattening on
// selection would lose the structure that multi-level fan needs.
#pragma once

#include "lumen/Patch.hpp"
#include "lumen/Uid.hpp"
#include "lumen/Value.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lumen {

// A selected thing: a fixture, or one cell of a multi-instance fixture.
// cell == 0 means "the fixture itself", resolved per attribute at set time
// (parent channel where the type has one, broadcast to cells otherwise).
struct Target {
    FixtureId fixture = 0;
    CellId cell = 0;

    bool operator==(const Target& o) const {
        return fixture == o.fixture && cell == o.cell;
    }
};

// ---------------------------------------------------------------------------
// Selection tree
// ---------------------------------------------------------------------------
struct SelNode {
    enum class Type : uint8_t { Fixture, Group };

    Type type = Type::Fixture;
    Target target;          // Type::Fixture
    Uid group{};            // Type::Group
    std::string label;      // for display and for fan level naming
    std::vector<SelNode> children;  // Type::Group — resolved members, in order

    static SelNode fixture(Target t);
    static SelNode makeGroup(Uid uid, std::string label, std::vector<SelNode> members);
};

class Selection {
public:
    // Order is capture order: `Fixture 1 + 5 + 3` selects 1, 5, 3 — the typing
    // order is information and is never re-sorted implicitly (§20.4).
    void clear() { roots_.clear(); }
    void set(std::vector<Target> targets);
    void add(const std::vector<Target>& targets);
    void addGroup(Uid uid, const std::string& label, std::vector<Target> members);
    void remove(const Target& t);
    void toggle(const Target& t);

    bool empty() const { return roots_.empty(); }
    const std::vector<SelNode>& roots() const { return roots_; }

    // Depth-first ordered flatten. Each group contributes its own stored order,
    // so nesting composes ordered sets rather than discarding them.
    std::vector<Target> flatten() const;
    size_t size() const { return flatten().size(); }
    bool contains(const Target& t) const;

    // Hierarchy preserved for multi-level fan (§20.6): "fan across four
    // trusses, then within each truss" needs the grouping at apply time.
    std::vector<std::vector<Target>> groupsAtTopLevel() const;
    size_t depth() const;

    // Reorder operations (§20.4) ------------------------------------------
    void reverse();
    void shuffle(uint64_t seed);
    void rotate(int by);
    void keepEveryNth(int n, int offset = 0);
    void keepOdd();
    void keepEven();
    void sortBy(const Patch& patch, const std::string& key);  // id, patch, name, x, y, z

private:
    std::vector<SelNode> roots_;
};

// ---------------------------------------------------------------------------
// Spread sources (§20.2)
//
// One abstraction serves FX phase, fan, and any sequential operation: a
// function over the selection yielding a normalised 0..1 per target.
// ---------------------------------------------------------------------------
enum class SpreadSource : uint8_t {
    Index,        // linear selection order
    PatchOrder,   // universe + address
    StageX, StageY, StageZ,
    StageRadial,  // distance from a stage point
    StageAngle,   // angle around a stage point
    Shuffle,      // permutation — no immediate repeats (§4.12)
    Random,       // independent draws — can repeat
    Count
};

const char* spreadSourceName(SpreadSource s);
std::optional<SpreadSource> spreadSourceFromName(const std::string& s);

// Phase modifiers (§20.3), applied as transforms on the spread value:
//   phase = fold( quantise( source, blocks ), wings ) * groups + offset
struct SpreadModifiers {
    int blocks = 1;      // N adjacent members share one phase
    int wings = 1;       // mirror the spread into N symmetric wings
    float groups = 1.f;  // how many full phase cycles across the selection
    float offset = 0.f;  // rotate the whole spread, in turns
};

// `n` is the member count, needed because Blocks is defined as members-per-
// block (matching MA3) rather than as a bucket count.
float applyModifiers(float spread, const SpreadModifiers& m, size_t n);

// Resolved once per selection change, not per frame. The phaser then reads a
// spread value rather than computing idx/total, which is what lets grid and
// stage sources work identically to linear order (§20.9).
class SpreadTable {
public:
    void build(const Selection& sel, const Patch& patch, SpreadSource source,
               uint64_t seed = 0, float centreX = 0.f, float centreY = 0.f);

    // Raw normalised spread for a target, before modifiers.
    float raw(const Target& t) const;
    // Spread with modifiers applied — what a phaser actually uses.
    float phase(const Target& t, const SpreadModifiers& m) const;

    const std::vector<Target>& order() const { return order_; }
    size_t size() const { return order_.size(); }
    bool empty() const { return order_.empty(); }

private:
    std::vector<Target> order_;
    std::vector<float> spread_;
};

// Seeded permutation (§4.12): every value appears exactly once per cycle, and
// a reshuffle never opens with the value the previous cycle closed on.
std::vector<size_t> shuffledIndices(size_t n, uint64_t seed,
                                    std::optional<size_t> avoidFirst = std::nullopt);

} // namespace lumen
