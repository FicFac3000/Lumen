// Programmer.hpp — live selection and manual values (SPEC §11.8).
//
// Clear is driven by programmer STATE, not a press counter: repeated presses
// produce the 1-2-3 behaviour naturally and the key can never get out of sync
// with a hidden counter.
#pragma once

#include "lumen/Patch.hpp"
#include "lumen/Selection.hpp"
#include "lumen/Value.hpp"

#include <string>
#include <vector>

namespace lumen {

// What a Clear press actually did, so the command line can report it (§11.8).
enum class ClearResult : uint8_t {
    Nothing,
    Selection,   // ClearSelection — deselect; values stay and keep outputting
    Active,      // ClearActive — deactivate; values remain and still output
    All,         // ClearAll — empty the programmer
    Presets,     // ClearPresets (Shift+Clear)
    DefaultUnused,  // DefaultUnused (Shift+Alt+Clear)
    CommandLine, // empty programmer: the key doubles as a command-line escape
};

const char* clearResultName(ClearResult r);

// What happens to an LTP attribute when the programmer releases it (§11.9).
// HTP always returns to default and is not governed by this.
enum class UnusedMode : uint8_t {
    HoldLast,   // default: position stays, colour stays
    ToDefault,  // "unused channels return to default"
};

class Programmer {
public:
    explicit Programmer(const Patch& patch) : patch_(&patch) {}

    // Selection ------------------------------------------------------------
    Selection& selection() { return sel_; }
    const Selection& selection() const { return sel_; }

    // Values ---------------------------------------------------------------
    // Resolves cells per §1.3: writes the parent channel where the type has
    // one, and broadcasts to every cell where it does not. This is what makes
    // `Fixture 12 At Red 100` work on a 12-cell bar.
    void setAttr(Attr a, float v);
    void setAttrFor(const Target& t, Attr a, float v);
    void setRef(Attr a, Uid preset, Attr within, float resolved);

    const ValueLayer& values() const { return values_; }
    ValueLayer& values() { return values_; }

    // Clear ----------------------------------------------------------------
    ClearResult clear();            // state-driven staged clear
    ClearResult clearPresets();     // Shift+Clear
    ClearResult clearAll();         // hold Clear / third press
    ClearResult defaultUnused();    // Shift+Alt+Clear
    void flatten();                 // refs resolve to literals and stay

    bool empty() const { return values_.empty() && sel_.empty(); }

    // Store: only ACTIVE values store, and references store AS references so
    // editing the preset updates every cue using it (§11.8).
    ValueLayer storeLayer() const { return values_.activeOnly(); }

    // Highlight (§20.7) ----------------------------------------------------
    // Two independent axes, not three modes: what happens to selected
    // fixtures, and what happens to unselected ones.
    bool highlight = false;
    enum class Unselected : uint8_t { Unaffected, Lowlight, Blackout };
    Unselected unselected = Unselected::Unaffected;
    float lowlightLevel = 0.15f;

    // Built fresh rather than stored, so it can never be captured by Store.
    ValueLayer highlightLayer() const;

    // Attributes the programmer is NOT using, for §11.9's release behaviour.
    // Scoped to the programmer, deliberately: "unused" means not used by the
    // programmer, not absent from the whole merge.
    bool usesAttr(const Target& t, Attr a) const;

private:
    std::vector<Target> resolveTargets(Attr a) const;

    const Patch* patch_;
    Selection sel_;
    ValueLayer values_;
};

} // namespace lumen
