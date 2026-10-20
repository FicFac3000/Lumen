// Value.hpp — the programmer / cue value model (SPEC §11.8).
//
// A value carries two orthogonal axes:
//   active  — will it be stored? Inactive values stay in the programmer and
//             still output, but Store ignores them.
//   kind    — a hard literal, or a live reference to a preset. A stored
//             reference is what makes "edit the Warm preset, every cue using
//             it follows" work, and it is safe because the reference is a UID.
#pragma once

#include "lumen/Attribute.hpp"
#include "lumen/Uid.hpp"

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace lumen {

using FixtureId = uint32_t;
using CellId = uint16_t;  // 0 = the fixture itself; 1..n = sub-fixture cells

// ---------------------------------------------------------------------------
// Addressing key: fixture (32) | cell (16) | attribute (16).
// Cells are first-class in the key so a Pixel Curve 12 is one patch entry with
// twelve addressable cells (§1.3, §17.3) rather than twelve fixtures.
// ---------------------------------------------------------------------------
using AttrKey = uint64_t;

inline constexpr AttrKey makeKey(FixtureId f, CellId cell, Attr a) {
    return (static_cast<AttrKey>(f) << 32) | (static_cast<AttrKey>(cell) << 16) |
           static_cast<AttrKey>(a);
}
inline constexpr FixtureId keyFixture(AttrKey k) { return static_cast<FixtureId>(k >> 32); }
inline constexpr CellId keyCell(AttrKey k) { return static_cast<CellId>((k >> 16) & 0xffffu); }
inline constexpr Attr keyAttr(AttrKey k) { return static_cast<Attr>(k & 0xffffu); }

float clamp01(float v);

// ---------------------------------------------------------------------------
// Value
// ---------------------------------------------------------------------------
struct Value {
    enum class Kind : uint8_t { Hard, PresetRef };

    Kind kind = Kind::Hard;
    float v = 0.f;         // hard value, or the last resolved value for a ref
    bool active = true;    // §11.8 active axis
    Uid preset{};          // when kind == PresetRef
    Attr presetAttr = Attr::Dimmer;  // which attribute within that preset

    static Value hard(float value, bool isActive = true) {
        Value x;
        x.kind = Kind::Hard;
        x.v = clamp01(value);
        x.active = isActive;
        return x;
    }
    static Value ref(Uid presetUid, Attr within, float resolved, bool isActive = true) {
        Value x;
        x.kind = Kind::PresetRef;
        x.preset = presetUid;
        x.presetAttr = within;
        x.v = clamp01(resolved);
        x.active = isActive;
        return x;
    }

    bool isRef() const { return kind == Kind::PresetRef; }
};

// ---------------------------------------------------------------------------
// ValueLayer — authoring and storage. Holds Values, not floats.
// ---------------------------------------------------------------------------
class ValueLayer {
public:
    void set(FixtureId f, CellId cell, Attr a, const Value& val);
    void set(FixtureId f, Attr a, float v) { set(f, 0, a, Value::hard(v)); }
    void setKey(AttrKey k, const Value& val) { v_[k] = val; }

    void remove(FixtureId f, CellId cell, Attr a);
    void removeKey(AttrKey k) { v_.erase(k); }
    bool has(FixtureId f, CellId cell, Attr a) const;

    const Value* find(AttrKey k) const;
    Value* findMut(AttrKey k);
    std::optional<float> value(FixtureId f, CellId cell, Attr a) const;

    void clear() { v_.clear(); }
    bool empty() const { return v_.empty(); }
    size_t size() const { return v_.size(); }
    const std::unordered_map<AttrKey, Value>& values() const { return v_; }

    // §11.8 Clear axes -----------------------------------------------------
    bool hasActive() const;           // drives the state-based Clear machine
    void deactivateAll();             // ClearActive
    void removeRefs();                // ClearPresets (Shift+Clear)
    void flattenRefs();               // Flatten: refs become hard values

    // Only active values are stored (§11.7, §11.8).
    ValueLayer activeOnly() const;

    // Keys present, optionally filtered to one attribute group (§11.1 masks).
    std::vector<AttrKey> keys(std::optional<Group> group = std::nullopt) const;

    // Unconditional overwrite — used where a layer always wins (programmer,
    // highlight). Merge rules live in the merge pipeline, not here.
    void overwriteFrom(const ValueLayer& other);

private:
    std::unordered_map<AttrKey, Value> v_;
};

// ---------------------------------------------------------------------------
// OutputLayer — a merge result. Plain floats: merging resolves references and
// discards the authoring distinction, because the output stage only needs
// numbers. Provenance for §11.10's inspector is computed separately and on
// demand for a fixture subset, never for every attribute every frame.
// ---------------------------------------------------------------------------
class OutputLayer {
public:
    void set(AttrKey k, float v) { v_[k] = clamp01(v); }
    std::optional<float> get(AttrKey k) const;
    bool has(AttrKey k) const { return v_.count(k) != 0; }
    void clear() { v_.clear(); }
    size_t size() const { return v_.size(); }
    const std::unordered_map<AttrKey, float>& values() const { return v_; }

    // Merge `other` on top, honouring each attribute's rule: HTP takes the
    // maximum, LTP takes the newer value (§11.4).
    void mergeOver(const OutputLayer& other);

    // Unconditional overwrite, for layers that always win.
    void overwriteFrom(const OutputLayer& other);

private:
    std::unordered_map<AttrKey, float> v_;
};

// Linear crossfade between two output layers. Keys present on only one side
// fade from or to zero, so a value leaving the stage fades rather than snapping.
OutputLayer crossfade(const OutputLayer& from, const OutputLayer& to, float t);

} // namespace lumen
