#include "lumen/Programmer.hpp"

namespace lumen {

const char* clearResultName(ClearResult r) {
    switch (r) {
        case ClearResult::Nothing: return "Nothing";
        case ClearResult::Selection: return "ClearSelection";
        case ClearResult::Active: return "ClearActive";
        case ClearResult::All: return "ClearAll";
        case ClearResult::Presets: return "ClearPresets";
        case ClearResult::DefaultUnused: return "DefaultUnused";
        case ClearResult::CommandLine: return "ClearCommandLine";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Cell resolution (§1.3)
// ---------------------------------------------------------------------------
std::vector<Target> Programmer::resolveTargets(Attr a) const {
    std::vector<Target> out;
    for (const Target& t : sel_.flatten()) {
        const FixtureType* ft = patch_->typeOf(t.fixture);
        if (!ft) continue;

        // An explicitly selected cell is addressed directly.
        if (t.cell != 0) {
            out.push_back(t);
            continue;
        }
        // The type carries this attribute on the parent: write there.
        if (ft->hasAttr(a, 0)) {
            out.push_back({t.fixture, 0});
            continue;
        }
        // Otherwise broadcast to every cell that has it.
        bool any = false;
        for (CellId c : patch_->cellsOf(t.fixture)) {
            if (c != 0 && ft->hasAttr(a, c)) {
                out.push_back({t.fixture, c});
                any = true;
            }
        }
        // The fixture does not have this attribute at all. Nothing is written,
        // which is the per-fixture skip the FX role rules rely on (§4.2).
        (void)any;
    }
    return out;
}

void Programmer::setAttr(Attr a, float v) {
    for (const Target& t : resolveTargets(a)) values_.set(t.fixture, t.cell, a, Value::hard(v));
}

void Programmer::setAttrFor(const Target& t, Attr a, float v) {
    values_.set(t.fixture, t.cell, a, Value::hard(v));
}

void Programmer::setRef(Attr a, Uid preset, Attr within, float resolved) {
    for (const Target& t : resolveTargets(a))
        values_.set(t.fixture, t.cell, a, Value::ref(preset, within, resolved));
}

// ---------------------------------------------------------------------------
// Clear — state-driven (§11.8)
// ---------------------------------------------------------------------------
ClearResult Programmer::clear() {
    if (!sel_.empty()) {
        sel_.clear();
        return ClearResult::Selection;
    }
    if (values_.hasActive()) {
        values_.deactivateAll();
        return ClearResult::Active;
    }
    if (!values_.empty()) {
        values_.clear();
        return ClearResult::All;
    }
    // Empty programmer: the key usefully doubles as a command-line escape.
    return ClearResult::CommandLine;
}

ClearResult Programmer::clearPresets() {
    if (values_.empty()) return ClearResult::Nothing;
    values_.removeRefs();
    return ClearResult::Presets;
}

ClearResult Programmer::clearAll() {
    const bool had = !values_.empty() || !sel_.empty();
    values_.clear();
    sel_.clear();
    return had ? ClearResult::All : ClearResult::Nothing;
}

ClearResult Programmer::defaultUnused() {
    // Releases residue; it does NOT write defaults into the programmer. The
    // alternative would make them storable, so the next Store would bake every
    // default into the cue (§11.8).
    return ClearResult::DefaultUnused;
}

void Programmer::flatten() { values_.flattenRefs(); }

bool Programmer::usesAttr(const Target& t, Attr a) const {
    return values_.has(t.fixture, t.cell, a);
}

// ---------------------------------------------------------------------------
// Highlight (§20.7)
// ---------------------------------------------------------------------------
ValueLayer Programmer::highlightLayer() const {
    ValueLayer out;
    if (!highlight && unselected == Unselected::Unaffected) return out;

    const auto selected = sel_.flatten();
    auto isSelected = [&](FixtureId id) {
        for (const Target& t : selected)
            if (t.fixture == id) return true;
        return false;
    };

    for (const Fixture& f : patch_->fixtures()) {
        const FixtureType* ft = patch_->typeOf(f.id);
        if (!ft) continue;

        if (isSelected(f.id)) {
            if (highlight) out.overwriteFrom(ft->highlightFor(f.id));
            continue;
        }
        switch (unselected) {
            case Unselected::Unaffected:
                break;
            case Unselected::Lowlight:
            case Unselected::Blackout: {
                const float lvl = unselected == Unselected::Lowlight ? lowlightLevel : 0.f;
                for (const auto& ch : ft->channels)
                    if (isIntensity(ch.attr)) out.set(f.id, ch.cell, ch.attr, Value::hard(lvl));
                break;
            }
        }
    }
    return out;
}

} // namespace lumen
