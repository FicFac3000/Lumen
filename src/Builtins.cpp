// Builtins.cpp — the M1 command set (SPEC §16.2).
//
// Every one of these is the ONLY route to the operation it performs. Nothing
// in the UI, a macro, a plugin or an agent gets a privileged path into the
// model that bypasses this file.
#include "lumen/Command.hpp"
#include "lumen/Gdtf.hpp"
#include "lumen/Library.hpp"
#include "lumen/Mapping.hpp"
#include "lumen/Undo.hpp"
#include "lumen/Mvr.hpp"
#include "lumen/Show.hpp"
#include "lumen/Surface.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace lumen {

namespace {

// Console values are percentages; the engine works in 0..1.
float pct(double v) { return clamp01(static_cast<float>(v / 100.0)); }

std::vector<Target> effectiveTargets(Context& ctx) {
    if (!ctx.targets.empty()) {
        ctx.engine.programmer.selection().set(ctx.targets);
        return ctx.targets;
    }
    return ctx.engine.programmer.selection().flatten();
}

std::string plural(size_t n, const char* one, const char* many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

// A canvas tile's slot is derived from its position, so it changes whenever
// the tile moves. An error that just says "not there" sends the operator
// hunting; one that lists where the tiles are is the next command.
std::string occupiedSlots(const Surface& s) {
    if (s.tiles.empty()) return " (this page has no tiles)";
    std::vector<int> slots;
    for (const Tile& t : s.tiles) slots.push_back(t.slot + 1);
    std::sort(slots.begin(), slots.end());
    std::string out = " (tiles are at ";
    for (size_t i = 0; i < slots.size(); ++i) {
        if (i) out += ", ";
        if (i == 8) { out += "…"; break; }
        out += std::to_string(slots[i]);
    }
    return out + ")";
}

// Cue numbers are decimals but almost always whole ones. `std::to_string`
// would report cue 4 as "4.000000", which looks like a bug to the operator
// reading the command line.
std::string cueNumber(double v) {
    std::ostringstream o;
    const double r = std::round(v * 1000.0) / 1000.0;
    if (r == std::floor(r)) o << static_cast<long long>(r);
    else {
        o.setf(std::ios::fixed);
        o.precision(3);
        o << r;
        std::string s = o.str();
        while (s.size() > 1 && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
        return s;
    }
    return o.str();
}

} // namespace

void Console::installBuiltins() {
    Engine& eng = engine_;

    // ---------------------------------------------------------------- values
    registry_.add({
        "At", {"@"},
        {{"attribute", ParamType::Attribute, false, "which attribute; Dimmer if omitted"},
         {"value", ParamType::Percent, true, "0-100"}},
        Privilege::Operator, false, true,
        "Sets an attribute on the selection. `Fixture 1 Thru 4 At 50`, "
        "`Fixture 1 At Red 100`.",
        [](Context& ctx) -> Result {
            const auto targets = effectiveTargets(ctx);
            if (targets.empty()) return Result::bad("nothing selected");

            Attr attr = Attr::Dimmer;
            size_t vi = 0;
            if (ctx.arg(0) && ctx.arg(0)->attr) {
                attr = *ctx.arg(0)->attr;
                vi = 1;
            } else if (ctx.arg(0) && !ctx.arg(0)->isNumber) {
                return Result::bad("unknown attribute: " + ctx.arg(0)->raw);
            }
            const Arg* v = ctx.arg(vi);
            if (!v || !v->isNumber) return Result::bad("expected a value");

            auto& prog = ctx.engine.programmer;
            const std::string what = std::string(attrName(attr)) + " at " +
                                     std::to_string(static_cast<int>(v->number)) + "%";
            ProgrammerEdit undo(ctx.engine, what);
            prog.selection().set(targets);
            prog.setAttr(attr, pct(v->number));
            undo.commit(ctx.engine.history, ctx.engine);
            ctx.engine.actionLog.add(what, "programmer", ctx.source, ctx.prompt);
            return Result::good(std::string(attrName(attr)) + " at " +
                                std::to_string(static_cast<int>(v->number)) + "% on " +
                                plural(targets.size(), "fixture", "fixtures"));
        }});

    // --------------------------------------------------------------- preset
    registry_.add({
        "Preset", {"pre"},
        {{"which", ParamType::String, true, "pool.id, or a preset name"}},
        Privilege::Operator, false, true,
        "Applies a preset to the selection as a live reference (SPEC 11.8). "
        "`Preset 1.3`, `Preset Red`, `Fixture 1 Thru 4 Preset \"Warm\"`.",
        [](Context& ctx) -> Result {
            const auto targets = effectiveTargets(ctx);
            if (targets.empty()) return Result::bad("nothing selected");

            const Arg* a = ctx.arg(0);
            if (!a) return Result::bad("expected a preset");

            auto& store = ctx.engine.presets;
            const Preset* found = nullptr;

            // `1.3` is pool 1, preset 3 — the same dotted form consoles use,
            // and the same one the tokeniser already reads for fixture cells.
            const size_t dot = a->raw.find('.');
            if (a->isNumber && dot != std::string::npos) {
                const int poolId = std::atoi(a->raw.substr(0, dot).c_str());
                const int presetId = std::atoi(a->raw.substr(dot + 1).c_str());
                const PresetPool* pool = store.pool(poolId);
                if (!pool) return Result::bad("no preset pool " + std::to_string(poolId));
                found = pool->byId(presetId);
                if (!found)
                    return Result::bad("pool " + std::to_string(poolId) + " (" + pool->name +
                                       ") has no preset " + std::to_string(presetId));
            } else if (a->isNumber) {
                return Result::bad("which pool? use pool.id, as in `Preset 1." + a->raw + "`");
            } else {
                // By name. §8.2: a bare name resolves only when exactly one
                // pool holds it, and a collision says which pools matched
                // rather than picking one.
                const PresetStore::NameLookup look = store.findByName(a->raw);
                if (look.ambiguous()) {
                    Result r = Result::bad("\"" + a->raw + "\" exists in " +
                                           std::to_string(look.ambiguousPools.size()) +
                                           " pools — say which");
                    for (int p : look.ambiguousPools) {
                        const PresetPool* pool = store.pool(p);
                        r.rows.push_back(std::to_string(p) + " " +
                                         (pool ? pool->name : std::string("?")));
                    }
                    return r;
                }
                found = look.preset;
                if (!found) return Result::bad("no preset named \"" + a->raw + "\"");
            }

            auto& prog = ctx.engine.programmer;
            const std::string what = "Preset " + found->name;
            ProgrammerEdit undo(ctx.engine, what);
            prog.selection().set(targets);
            const Programmer::PresetApply applied = prog.applyPreset(store, *found);
            undo.commit(ctx.engine.history, ctx.engine);
            ctx.engine.actionLog.add(what, "programmer", ctx.source, ctx.prompt);

            if (applied.written == 0)
                return Result::bad("\"" + found->name +
                                   "\" has nothing for the selected fixture(s)");

            std::ostringstream msg;
            msg << found->name << " on " << plural(targets.size(), "fixture", "fixtures");
            if (!applied.attrs.empty()) {
                msg << " (";
                for (size_t k = 0; k < applied.attrs.size(); ++k)
                    msg << (k ? ", " : "") << attrName(applied.attrs[k]);
                msg << ")";
            }
            Result r = Result::good(msg.str());
            // A partial apply is reported, not hidden: "it half worked" is the
            // answer most likely to be a patching mistake.
            if (applied.skipped > 0)
                r.rows.push_back(std::to_string(applied.skipped) +
                                 " value(s) skipped — the preset says nothing for those");
            return r;
        }});

    // ---------------------------------------------------------------- clear
    registry_.add({
        "Clear", {"cl"}, {},
        Privilege::Operator, false, false,
        "Staged clear, resolved against programmer state: selection, then "
        "active values, then everything (SPEC 11.8).",
        [](Context& ctx) -> Result {
            ProgrammerEdit undo(ctx.engine, "Clear");
            const ClearResult r = ctx.engine.programmer.clear();
            if (undo.commit(ctx.engine.history, ctx.engine).valid())
                ctx.engine.actionLog.add(std::string("Clear: ") + clearResultName(r),
                                         "programmer", ctx.source, ctx.prompt);
            return Result::good(clearResultName(r));
        }});

    registry_.add({
        "ClearAll", {}, {},
        Privilege::Operator, false, false,
        "Empties the programmer outright — hold Clear.",
        [](Context& ctx) -> Result {
            ProgrammerEdit undo(ctx.engine, "Clear all");
            const ClearResult r = ctx.engine.programmer.clearAll();
            if (undo.commit(ctx.engine.history, ctx.engine).valid())
                ctx.engine.actionLog.add("Clear all", "programmer", ctx.source, ctx.prompt);
            return Result::good(clearResultName(r));
        }});

    registry_.add({
        "ClearPresets", {}, {},
        Privilege::Operator, false, false,
        "Removes preset-referenced values, leaving hard values — Shift+Clear.",
        [](Context& ctx) -> Result {
            ProgrammerEdit undo(ctx.engine, "Clear presets");
            const ClearResult r = ctx.engine.programmer.clearPresets();
            if (undo.commit(ctx.engine.history, ctx.engine).valid())
                ctx.engine.actionLog.add("Clear presets", "programmer", ctx.source, ctx.prompt);
            return Result::good(clearResultName(r));
        }});

    registry_.add({
        "Flatten", {}, {},
        Privilege::Operator, false, false,
        "Resolves preset references to literal values and keeps them.",
        [](Context& ctx) -> Result {
            ProgrammerEdit undo(ctx.engine, "Flatten");
            ctx.engine.programmer.flatten();
            if (undo.commit(ctx.engine.history, ctx.engine).valid())
                ctx.engine.actionLog.add("Flatten", "programmer", ctx.source, ctx.prompt);
            return Result::good("flattened");
        }});

    // ------------------------------------------------------------ selection
    registry_.add({
        "All", {}, {},
        Privilege::Operator, false, false,
        "Selects every patched fixture, in patch order.",
        [](Context& ctx) -> Result {
            std::vector<Target> all;
            for (const auto& f : ctx.engine.patch.fixtures()) all.push_back({f.id, 0});
            ctx.engine.programmer.selection().set(all);
            return Result::good("selected " + plural(all.size(), "fixture", "fixtures"));
        }});

    registry_.add({
        "Reverse", {}, {},
        Privilege::Operator, false, true,
        "Reverses selection order, which also reverses FX spread and fan "
        "direction (SPEC 20.4).",
        [](Context& ctx) -> Result {
            ctx.engine.programmer.selection().reverse();
            return Result::good("reversed");
        }});

    registry_.add({
        "Shuffle", {},
        {{"seed", ParamType::Int, false, "seed, so the order is reproducible"}},
        Privilege::Operator, false, true,
        "Shuffles selection order using a seeded permutation, so it replays "
        "identically (SPEC 4.12).",
        [](Context& ctx) -> Result {
            const auto seed = static_cast<uint64_t>(ctx.number(0, 1));
            ctx.engine.programmer.selection().shuffle(seed);
            return Result::good("shuffled");
        }});

    // ------------------------------------------------------------ highlight
    registry_.add({
        "Highlight", {"hi"},
        {{"state", ParamType::Bool, false, "on or off; toggles if omitted"}},
        Privilege::Operator, false, true,
        "Forces the selection to a visible state. Never stored (SPEC 20.7).",
        [](Context& ctx) -> Result {
            auto& p = ctx.engine.programmer;
            const std::string s = ctx.text(0);
            if (s.empty()) p.highlight = !p.highlight;
            else p.highlight = (s == "on" || s == "On" || s == "1");
            return Result::good(p.highlight ? "highlight on" : "highlight off");
        }});

    registry_.add({
        "Unselected", {},
        {{"mode", ParamType::Enum, true, "what happens to unselected fixtures",
          {"unaffected", "lowlight", "blackout"}}},
        Privilege::Operator, false, false,
        "The second highlight axis: unaffected, lowlight, or blackout (solo).",
        [](Context& ctx) -> Result {
            const std::string m = ctx.text(0);
            auto& p = ctx.engine.programmer;
            if (m == "unaffected") p.unselected = Programmer::Unselected::Unaffected;
            else if (m == "lowlight") p.unselected = Programmer::Unselected::Lowlight;
            else if (m == "blackout") p.unselected = Programmer::Unselected::Blackout;
            else return Result::bad("expected unaffected, lowlight or blackout");
            return Result::good("unselected: " + m);
        }});

    // ---------------------------------------------------------------- store
    registry_.add({
        "Store", {"rec"},
        {{"object", ParamType::Enum, true, "what to store", {"cue"}},
         {"number", ParamType::Float, false, "cue number; appends if omitted"},
         {"sequence", ParamType::Int, false, "target sequence; 1 if omitted"}},
        Privilege::Operator, false, false,
        "Stores the programmer's ACTIVE values into a cue. Preset references "
        "store as references (SPEC 11.8).",
        [](Context& ctx) -> Result {
            if (ctx.text(0) != "Cue" && ctx.text(0) != "cue")
                return Result::bad("only `Store Cue` is implemented");

            const int seqId = static_cast<int>(ctx.number(2, 1));
            Sequence& seq = ctx.engine.ensureSequence(seqId);

            const ValueLayer layer = ctx.engine.programmer.storeLayer();
            if (layer.empty()) return Result::bad("nothing active in the programmer");

            float number = static_cast<float>(ctx.number(1, 0.0));
            if (number <= 0.f)
                number = seq.cues.empty() ? 1.f : seq.cues.back().number + 1.f;

            const int existing = seq.indexOfNumber(number);
            if (existing >= 0) {
                seq.cues[static_cast<size_t>(existing)].values().overwriteFrom(layer);
                return Result::good("merged into cue " + cueNumber(number));
            }
            Cue c;
            c.uid = ctx.engine.uids.next();
            c.number = number;
            c.values() = layer;
            for (const Phaser& ph : ctx.engine.programmer.phasers()) c.phasers.push_back(ph);
            seq.cues.push_back(std::move(c));
            return Result::good("stored cue " + cueNumber(number) + " in sequence " +
                                std::to_string(seqId) + " (" +
                                plural(layer.size(), "value", "values") + ")");
        }});

    // ------------------------------------------------------------- playback
    registry_.add({
        "Assign", {},
        {{"sequence", ParamType::Int, true, "sequence id"},
         {"executor", ParamType::Int, true, "executor id"}},
        Privilege::Operator, false, false,
        "Binds a sequence to an executor.",
        [](Context& ctx) -> Result {
            const int seqId = static_cast<int>(ctx.number(0, 0));
            const int exId = static_cast<int>(ctx.number(1, 0));
            if (!ctx.engine.sequence(seqId))
                return Result::bad("no sequence " + std::to_string(seqId));
            ctx.engine.ensureExecutor(exId).sequenceId = seqId;
            return Result::good("sequence " + std::to_string(seqId) + " on executor " +
                                std::to_string(exId));
        }});

    registry_.add({
        "Go", {},
        {{"executor", ParamType::Int, false, "executor id; 1 if omitted"}},
        Privilege::Operator, false, false,
        "Advances an executor to its next cue.",
        [](Context& ctx) -> Result {
            const int id = static_cast<int>(ctx.number(0, 1));
            Executor* e = ctx.engine.executor(id);
            if (!e) return Result::bad("no executor " + std::to_string(id));
            const Sequence* s = ctx.engine.sequence(e->sequenceId);
            if (!s) return Result::bad("executor " + std::to_string(id) + " has no sequence");
            e->go(*s, ctx.engine.patch, ctx.engine.now());
            return Result::good("exec " + std::to_string(id) + " cue " +
                                std::to_string(e->currentIndex() + 1));
        }});

    registry_.add({
        "Back", {},
        {{"executor", ParamType::Int, false, "executor id; 1 if omitted"}},
        Privilege::Operator, false, false,
        "Steps an executor back one cue.",
        [](Context& ctx) -> Result {
            const int id = static_cast<int>(ctx.number(0, 1));
            Executor* e = ctx.engine.executor(id);
            if (!e) return Result::bad("no executor " + std::to_string(id));
            const Sequence* s = ctx.engine.sequence(e->sequenceId);
            if (!s) return Result::bad("executor has no sequence");
            e->back(*s, ctx.engine.patch, ctx.engine.now());
            return Result::good("exec " + std::to_string(id) + " cue " +
                                std::to_string(e->currentIndex() + 1));
        }});

    registry_.add({
        "Goto", {},
        {{"cue", ParamType::Float, true, "cue number"},
         {"executor", ParamType::Int, false, "executor id; 1 if omitted"}},
        Privilege::Operator, false, false,
        "Jumps an executor to a cue by number.",
        [](Context& ctx) -> Result {
            const auto num = static_cast<float>(ctx.number(0, 1));
            const int id = static_cast<int>(ctx.number(1, 1));
            Executor* e = ctx.engine.executor(id);
            if (!e) return Result::bad("no executor " + std::to_string(id));
            const Sequence* s = ctx.engine.sequence(e->sequenceId);
            if (!s) return Result::bad("executor has no sequence");
            const int idx = s->indexOfNumber(num);
            if (idx < 0) return Result::bad("no cue " + cueNumber(num));
            e->gotoCue(*s, ctx.engine.patch, idx, ctx.engine.now());
            return Result::good("exec " + std::to_string(id) + " at cue " + cueNumber(num));
        }});

    registry_.add({
        "Off", {},
        {{"executor", ParamType::Int, false, "executor id; all if omitted"}},
        Privilege::Operator, false, false,
        "Stops an executor immediately, with no release time.",
        [](Context& ctx) -> Result {
            const double t = ctx.engine.now();
            if (!ctx.arg(0)) {
                for (auto& e : ctx.engine.executors) e.off(t);
                return Result::good("all executors off");
            }
            const int id = static_cast<int>(ctx.number(0, 1));
            Executor* e = ctx.engine.executor(id);
            if (!e) return Result::bad("no executor " + std::to_string(id));
            e->off(t);
            return Result::good("exec " + std::to_string(id) + " off");
        }});

    registry_.add({
        "Release", {"rel"},
        {{"executor", ParamType::Int, false, "executor id; all if omitted — Shift+Release"}},
        Privilege::Operator, false, false,
        "Releases an executor over its release time, honouring its release "
        "mode. Never touches the programmer (SPEC 11.3).",
        [](Context& ctx) -> Result {
            const double t = ctx.engine.now();
            if (!ctx.arg(0)) {
                int n = 0;
                for (auto& e : ctx.engine.executors)
                    if (e.active()) { e.releaseNow(t); ++n; }
                return Result::good("released " + plural(static_cast<size_t>(n),
                                                         "executor", "executors"));
            }
            const int id = static_cast<int>(ctx.number(0, 1));
            Executor* e = ctx.engine.executor(id);
            if (!e) return Result::bad("no executor " + std::to_string(id));
            e->releaseNow(t);
            return Result::good("exec " + std::to_string(id) + " releasing");
        }});

    // --------------------------------------------------------------- masters
    registry_.add({
        "GrandMaster", {"gm"},
        {{"value", ParamType::Percent, true, "0-100"}},
        Privilege::Operator, false, false,
        "Sets the grand master, which scales intensity only.",
        [](Context& ctx) -> Result {
            if (!ctx.arg(0) || !ctx.arg(0)->isNumber) return Result::bad("expected a value");
            ctx.engine.grandMaster = pct(ctx.number(0, 100));
            return Result::good("grand master " +
                                std::to_string(static_cast<int>(ctx.number(0, 100))) + "%");
        }});

    // ------------------------------------------------------- surfaces (§9)
    // Written BEFORE the UI needed them, for once. Three rounds of finding a
    // missing command by building a surface is enough of a lesson.
    registry_.add({
        "Surface", {"surf"},
        {{"page", ParamType::Int, false, "page id; lists them if omitted"}},
        Privilege::Operator, false, false,
        "Selects an executor surface page, or lists them (SPEC 9.6).",
        [](Context& ctx) -> Result {
            auto& store = ctx.engine.surfaces;
            if (!ctx.arg(0)) {
                Result r = Result::good(plural(store.size(), "page", "pages"));
                for (const Surface& s : store.all())
                    r.rows.push_back(std::to_string(s.id) + " " + s.name + "  " +
                                     surfaceStyleName(s.style) + "  " +
                                     std::to_string(s.tiles.size()) + " tile(s)" +
                                     (s.id == store.current() ? "  <- current" : ""));
                return r;
            }
            const int id = static_cast<int>(ctx.number(0, 0));
            if (!store.setCurrent(id))
                return Result::bad("no surface page " + std::to_string(id));
            const Surface* s = store.byId(id);
            return Result::good("page " + std::to_string(id) + " (" + s->name + ", " +
                                surfaceStyleName(s->style) + ")");
        }});

    registry_.add({
        "SurfaceStyle", {},
        {{"style", ParamType::Enum, true, "wing or canvas", {"wing", "canvas"}},
         {"page", ParamType::Int, false, "page id; the current one if omitted"},
         {"force", ParamType::String, false, "`force` to accept dropped tiles"}},
        Privilege::Operator, false, false,
        "Converts a surface page between the fixed wing and the free canvas. "
        "Reports what the change costs before it costs it (SPEC 9.6).",
        [](Context& ctx) -> Result {
            auto& store = ctx.engine.surfaces;
            const std::optional<SurfaceStyle> want = surfaceStyleFromName(ctx.text(0));
            if (!want) return Result::bad("expected wing or canvas");
            const int id = ctx.arg(1) ? static_cast<int>(ctx.number(1, 0))
                                      : store.current();
            if (!store.byId(id))
                return Result::bad("no surface page " + std::to_string(id));
            const std::string third = ctx.text(2);
            const bool force = (third == "force" || third == "Force");

            std::string err;
            SurfaceEdit undo(ctx.engine, std::string("Make page ") +
                                             std::to_string(id) + " a " +
                                             surfaceStyleName(*want), id);
            const ConvertReport rep = store.setStyle(id, *want, force, &err);
            if (!err.empty()) {
                undo.cancel();
                // Refused, with the reason and the full list: a layout
                // somebody arranged by hand is not discarded on a shrug.
                Result r = Result::bad(err);
                for (const std::string& n : rep.notes) r.rows.push_back(n);
                return r;
            }
            undo.commit(ctx.engine.history, ctx.engine);
            ctx.engine.actionLog.add("Page " + std::to_string(id) + " -> " +
                                         surfaceStyleName(*want),
                                     "layout", ctx.source, ctx.prompt);
            Result r = Result::good("page " + std::to_string(id) + " is now a " +
                                    surfaceStyleName(*want));
            if (!rep.lossless)
                for (const std::string& n : rep.notes) r.rows.push_back(n);
            return r;
        }});

    registry_.add({
        "Tile", {},
        {{"slot", ParamType::Int, true, "slot number on the page, 1-based"},
         {"kind", ParamType::Enum, true, "what the tile holds",
          {"executor", "group", "preset", "recipe", "timecode", "grandmaster",
           "blackout", "laserzone", "label", "empty"}},
         {"target", ParamType::String, false,
          "the object's id; pool.id for a preset, the text for a label"}},
        Privilege::Operator, false, false,
        "Assigns a tile on the current surface page. `Tile 3 executor 2`, "
        "`Tile 4 preset 1.3`, `Tile 5 blackout` (SPEC 9.2, 9.3).",
        [](Context& ctx) -> Result {
            auto& store = ctx.engine.surfaces;
            const int page = store.current();
            Surface* s = store.byId(page);
            if (!s) return Result::bad("no surface page selected");

            const int slot = static_cast<int>(ctx.number(0, 0)) - 1;
            const std::optional<TileKind> kind = tileKindFromName(ctx.text(1));
            if (!kind) return Result::bad("unknown tile kind: " + ctx.text(1));

            if (*kind == TileKind::Empty) {
                if (const Tile* t = s->atSlot(slot)) {
                    const Uid uid = t->uid;
                    SurfaceEdit undo(ctx.engine,
                                     "Clear slot " + std::to_string(slot + 1), page);
                    store.clearTile(page, uid);
                    undo.commit(ctx.engine.history, ctx.engine);
                    ctx.engine.actionLog.add("Clear slot " + std::to_string(slot + 1),
                                             "layout", ctx.source, ctx.prompt);
                    return Result::good("slot " + std::to_string(slot + 1) + " cleared");
                }
                return Result::good("slot " + std::to_string(slot + 1) +
                                    " was already empty");
            }

            // The tile stores the object's UID, never its name (§8.1, §9.2),
            // so the legend follows a rename with no bookkeeping anywhere.
            const std::string ref = ctx.text(2);
            const int targetId = ctx.arg(2) ? static_cast<int>(ctx.number(2, 0)) : 0;
            Uid target{};
            std::string legend;

            switch (*kind) {
                case TileKind::Executor: {
                    const Executor* x = ctx.engine.executor(targetId);
                    if (!x) return Result::bad("no executor " + std::to_string(targetId));
                    target = x->uid;
                    break;
                }
                case TileKind::Group: {
                    const GroupDef* g = ctx.engine.groups.byId(targetId);
                    if (!g) return Result::bad("no group " + std::to_string(targetId));
                    target = g->uid;
                    break;
                }
                case TileKind::Preset: {
                    // `pool.id`, the same dotted form the Preset command takes.
                    // One grammar for addressing a preset, not two.
                    const size_t dot = ref.find('.');
                    if (dot == std::string::npos)
                        return Result::bad("a preset tile needs pool.id, as in "
                                           "`Tile " + std::to_string(slot + 1) +
                                           " preset 1.3`");
                    const PresetPool* pool =
                        ctx.engine.presets.pool(std::atoi(ref.substr(0, dot).c_str()));
                    if (!pool) return Result::bad("no preset pool in " + ref);
                    const Preset* pre =
                        pool->byId(std::atoi(ref.substr(dot + 1).c_str()));
                    if (!pre) return Result::bad("no preset " + ref);
                    target = pre->uid;
                    break;
                }
                case TileKind::Recipe: {
                    const Recipe* r = nullptr;
                    for (const Recipe& e : ctx.engine.recipes.all())
                        if (e.id == targetId) r = &e;
                    if (!r) return Result::bad("no recipe " + std::to_string(targetId));
                    target = r->uid;
                    break;
                }
                case TileKind::Timecode: {
                    const TimecodeSlot* t = ctx.engine.timecode.slot(targetId);
                    if (!t)
                        return Result::bad("no timecode slot " + std::to_string(targetId));
                    target = t->uid;
                    break;
                }
                case TileKind::Label:
                    // A label carries its text rather than pointing at
                    // anything, which is the one case where the override IS
                    // the content.
                    legend = ref;
                    break;
                case TileKind::LaserZone: {
                    const ZoneSafety* z = ctx.engine.laser.zone(targetId);
                    if (!z)
                        return Result::bad("no laser zone " + std::to_string(targetId));
                    target = z->uid;
                    break;
                }
                case TileKind::GrandMaster:
                case TileKind::Blackout:
                case TileKind::Macro:
                    // These address a thing the console has exactly one of,
                    // or none yet. Nothing to point at.
                    if (!ref.empty()) legend = ref;
                    break;
                case TileKind::Empty:
                case TileKind::Count:
                    break;
            }

            const std::string what = std::string(tileKindName(*kind)) +
                                     (ref.empty() ? "" : " " + ref) + " on slot " +
                                     std::to_string(slot + 1);
            SurfaceEdit undo(ctx.engine, what, page);
            PlaceResult r = (s->style == SurfaceStyle::Wing)
                                ? store.placeInSlot(page, slot, *kind, target)
                                : store.placeAt(page,
                                                Rect{slot % s->cols, slot / s->cols, 1, 1},
                                                *kind, target);
            if (!r.ok) {
                undo.cancel();
                return Result::bad(r.error);
            }
            if (!legend.empty())
                for (Tile& t : s->tiles)
                    if (t.uid == r.placed) t.legendOverride = legend;
            undo.commit(ctx.engine.history, ctx.engine);
            ctx.engine.actionLog.add(what, "layout", ctx.source, ctx.prompt);
            return Result::good(what);
        }});

    // ------------------------------------------- pixel extraction (§13.3)
    registry_.add({
        "Extraction", {"extract"},
        {{"id", ParamType::Int, false, "layer id; lists them if omitted"},
         {"name", ParamType::String, false, "name it, creating it if new"}},
        Privilege::Operator, false, false,
        "Creates or lists pixel-extraction layers. Mapped content is a "
        "playback, so a layer has a priority and a merge rule (SPEC 13.3).",
        [](Context& ctx) -> Result {
            auto& store = ctx.engine.extractions;
            if (!ctx.arg(0)) {
                Result r = Result::good(plural(store.size(), "layer", "layers"));
                for (const Extraction& e : store.all())
                    r.rows.push_back(
                        std::to_string(e.id) + " " + e.name + "  " +
                        plural(e.points.size(), "point", "points") + "  prio " +
                        std::to_string(e.priority) + "  " +
                        (e.merge == MergeRule::HTP ? "HTP" : "LTP") +
                        (e.enabled ? "" : "  [off]"));
                return r;
            }
            const int id = static_cast<int>(ctx.number(0, 0));
            Extraction* e = store.byId(id);
            if (!e) {
                Extraction made;
                made.id = id;
                made.name = ctx.text(1);
                e = &store.add(made);
                return Result::good("created extraction layer " +
                                    std::to_string(e->id) + " (" + e->name + ")");
            }
            if (ctx.arg(1)) e->name = ctx.text(1);
            return Result::good("layer " + std::to_string(e->id) + " (" + e->name +
                                "), " + plural(e->points.size(), "point", "points"));
        }});

    registry_.add({
        "MapLayout", {},
        {{"layer", ParamType::Int, true, "extraction layer id"},
         {"shape", ParamType::Enum, true, "how to place them",
          {"grid", "line", "bezier"}},
         {"a", ParamType::Float, false, "grid: columns · line/bezier: x0 as a percent"},
         {"b", ParamType::Float, false, "y0"},
         {"c", ParamType::Float, false, "x1"},
         {"d", ParamType::Float, false, "y1"}},
        Privilege::Operator, false, true,
        "Places the selection's sample points. Mapping space is 0-100 across "
        "the surface, y downward (SPEC 13.2).",
        [](Context& ctx) -> Result {
            auto& store = ctx.engine.extractions;
            const int id = static_cast<int>(ctx.number(0, 0));
            Extraction* e = store.byId(id);
            if (!e) return Result::bad("no extraction layer " + std::to_string(id));

            const auto targets = effectiveTargets(ctx);
            if (targets.empty()) return Result::bad("nothing selected");

            // Percentages at the command line, normalised inside: 0-100 is
            // what every other spatial figure in this console uses, and one
            // command taking 0-1 would be the odd one out.
            auto frac = [&](size_t i, double fallback) {
                return static_cast<float>(ctx.number(i, fallback) / 100.0);
            };

            const std::string shape = ctx.text(1);
            std::vector<SamplePoint> points;
            if (shape == "grid") {
                const int cols = static_cast<int>(ctx.number(2, 8));
                if (cols <= 0) return Result::bad("a grid needs at least one column");
                points = layoutGrid(targets, cols, frac(3, 0), frac(4, 0),
                                    frac(5, 100), frac(6, 100));
            } else if (shape == "line") {
                points = layoutLine(targets, frac(2, 0), frac(3, 50), frac(4, 100),
                                    frac(5, 50));
            } else {
                // A bow between the endpoints, which is the curve an LED
                // strip is usually bent into and saves typing four handles.
                const float x0 = frac(2, 0), y0 = frac(3, 50);
                const float x1 = frac(4, 100), y1 = frac(5, 50);
                const float bow = 0.25f;
                points = layoutBezier(targets, x0, y0, x0 + (x1 - x0) * 0.33f,
                                      y0 - bow, x0 + (x1 - x0) * 0.66f, y1 - bow, x1,
                                      y1);
            }
            e->setPoints(std::move(points));
            ctx.engine.actionLog.add("Map " + shape + " on layer " +
                                         std::to_string(id),
                                     "mapping", ctx.source, ctx.prompt);
            return Result::good(plural(e->points.size(), "point", "points") +
                                " placed in a " + shape);
        }});

    registry_.add({
        "MapArea", {},
        {{"layer", ParamType::Int, true, "extraction layer id"},
         {"area", ParamType::Enum, true, "how each point samples",
          {"point", "box", "gaussian"}},
         {"radius", ParamType::Float, false, "as a percent of the surface"}},
        Privilege::Operator, false, false,
        "Sets how every point on a layer samples. Averaging is what stops "
        "LED output flickering on detailed content (SPEC 13.2).",
        [](Context& ctx) -> Result {
            auto& store = ctx.engine.extractions;
            const int id = static_cast<int>(ctx.number(0, 0));
            Extraction* e = store.byId(id);
            if (!e) return Result::bad("no extraction layer " + std::to_string(id));
            const std::optional<SampleArea> area = sampleAreaFromName(ctx.text(1));
            if (!area) return Result::bad("expected point, box or gaussian");
            const float radius = ctx.arg(2)
                                     ? static_cast<float>(ctx.number(2, 1) / 100.0)
                                     : -1.f;
            for (SamplePoint& p : e->points) {
                p.area = *area;
                if (radius >= 0.f) p.radius = radius;
            }
            return Result::good(plural(e->points.size(), "point", "points") +
                                " sampling " + sampleAreaName(*area));
        }});

    registry_.add({
        "MapMaster", {},
        {{"layer", ParamType::Int, true, "extraction layer id"},
         {"value", ParamType::Percent, true, "0-100"}},
        Privilege::Operator, false, false,
        "Sets an extraction layer's intensity master, so mapped content "
        "fades like any playback (SPEC 13.3).",
        [](Context& ctx) -> Result {
            const int id = static_cast<int>(ctx.number(0, 0));
            Extraction* e = ctx.engine.extractions.byId(id);
            if (!e) return Result::bad("no extraction layer " + std::to_string(id));
            e->master = pct(ctx.number(1, 100));
            return Result::good("layer " + std::to_string(id) + " master " +
                                std::to_string(static_cast<int>(ctx.number(1, 100))) +
                                "%");
        }});

    registry_.add({
        "MapMerge", {},
        {{"layer", ParamType::Int, true, "extraction layer id"},
         {"rule", ParamType::Enum, true, "htp or ltp", {"htp", "ltp"}},
         {"priority", ParamType::Int, false, "higher resolves later"}},
        Privilege::Operator, false, false,
        "Sets how a layer resolves against cues and other layers — the same "
        "mechanisms an executor has (SPEC 11.4, 13.3).",
        [](Context& ctx) -> Result {
            const int id = static_cast<int>(ctx.number(0, 0));
            Extraction* e = ctx.engine.extractions.byId(id);
            if (!e) return Result::bad("no extraction layer " + std::to_string(id));
            const std::string rule = ctx.text(1);
            if (rule == "htp") e->merge = MergeRule::HTP;
            else if (rule == "ltp") e->merge = MergeRule::LTP;
            else return Result::bad("expected htp or ltp");
            if (ctx.arg(2)) e->priority = static_cast<int>(ctx.number(2, 0));
            return Result::good("layer " + std::to_string(id) + " " + rule +
                                " at priority " + std::to_string(e->priority));
        }});

    // ------------------------------------------------------- laser (§14.6)
    // Note what is NOT here: there is no `Arm`. Arming is hold-to-fire from
    // a physical act (SafetyGate::ArmSource), and a command for it would be
    // reachable from a cue, a macro, a binding and the agent — exactly the
    // single-press path §14.6 exists to prevent. Disarming and stopping are
    // commands, because the safe direction should be as reachable as the
    // console can make it.
    registry_.add({
        "Disarm", {},
        {{"zone", ParamType::Int, false, "zone id; every zone if omitted"}},
        Privilege::Operator, false, false,
        "Disarms a laser zone, or all of them. There is deliberately no Arm "
        "command (SPEC 14.6).",
        [](Context& ctx) -> Result {
            auto& gate = ctx.engine.laser;
            if (!ctx.arg(0)) {
                gate.disarmAll();
                ctx.engine.actionLog.add("Disarm all zones", "laser", ctx.source,
                                         ctx.prompt);
                return Result::good("every zone disarmed");
            }
            const int id = static_cast<int>(ctx.number(0, 0));
            if (!gate.zone(id)) return Result::bad("no laser zone " + std::to_string(id));
            gate.disarm(id);
            ctx.engine.actionLog.add("Disarm zone " + std::to_string(id), "laser",
                                     ctx.source, ctx.prompt);
            return Result::good("zone " + std::to_string(id) + " disarmed");
        }});

    registry_.add({
        "EmergencyStop", {"estop"},
        {{"action", ParamType::Enum, false, "stop or reset; stop if omitted",
          {"stop", "reset"}}},
        Privilege::Operator, false, false,
        "Latches every laser off, or clears the latch. Resetting also "
        "disarms, so recovery cannot silently restore a beam (SPEC 14.6).",
        [](Context& ctx) -> Result {
            auto& gate = ctx.engine.laser;
            const std::string what = ctx.text(0);
            if (what == "reset") {
                gate.resetEmergencyStop();
                ctx.engine.actionLog.add("Emergency stop reset", "laser", ctx.source,
                                         ctx.prompt);
                return Result::good("emergency stop cleared; every zone disarmed");
            }
            gate.emergencyStop();
            ctx.engine.actionLog.add("EMERGENCY STOP", "laser", ctx.source, ctx.prompt);
            return Result::good("emergency stop latched");
        }});

    registry_.add({
        "EditMode", {},
        {{"state", ParamType::Bool, false, "on or off; toggles if omitted"},
         {"page", ParamType::Int, false, "page id; the current one if omitted"}},
        Privilege::Operator, false, false,
        "Unlocks a surface page for arranging. Run mode is the default and "
        "tiles cannot be moved in it (SPEC 9.4).",
        [](Context& ctx) -> Result {
            auto& store = ctx.engine.surfaces;
            const int id = ctx.arg(1) ? static_cast<int>(ctx.number(1, 0))
                                      : store.current();
            Surface* sf = store.byId(id);
            if (!sf) return Result::bad("no surface page " + std::to_string(id));
            const std::string st = ctx.text(0);
            if (st.empty()) sf->editMode = !sf->editMode;
            else sf->editMode = (st == "on" || st == "On" || st == "1");
            return Result::good("page " + std::to_string(id) + " is in " +
                                (sf->editMode ? "edit" : "run") + " mode");
        }});

    registry_.add({
        "TileMove", {},
        {{"slot", ParamType::Int, true, "the tile's current slot, 1-based"},
         {"column", ParamType::Int, true, "destination column, 1-based"},
         {"row", ParamType::Int, true, "destination row, 1-based"},
         {"width", ParamType::Int, false, "new width in cells; unchanged if omitted"},
         {"height", ParamType::Int, false, "new height; unchanged if omitted"}},
        Privilege::Operator, false, false,
        "Moves or resizes a tile on a canvas page. Refused if it would "
        "overlap another tile (SPEC 9.1).",
        [](Context& ctx) -> Result {
            auto& store = ctx.engine.surfaces;
            const int page = store.current();
            Surface* sf = store.byId(page);
            if (!sf) return Result::bad("no surface page selected");
            if (sf->style != SurfaceStyle::Canvas)
                return Result::bad("a wing's slots do not move; use `Tile` to "
                                   "assign one");
            // §9.4 enforced here rather than in the UI, because a lock only
            // the interface respects is not a lock.
            if (!sf->editMode)
                return Result::bad("page " + std::to_string(page) +
                                   " is in run mode — `EditMode on` first");

            const int slot = static_cast<int>(ctx.number(0, 0)) - 1;
            const Tile* t = sf->atSlot(slot);
            // A canvas tile's slot is its top-left cell, so moving one changes
            // its slot. Saying where the tiles actually are turns a dead end
            // into the next command.
            if (!t)
                return Result::bad("no tile at slot " + std::to_string(slot + 1) +
                                   occupiedSlots(*sf));

            Rect want = t->rect;
            want.x = static_cast<int>(ctx.number(1, 1)) - 1;
            want.y = static_cast<int>(ctx.number(2, 1)) - 1;
            if (ctx.arg(3)) want.w = static_cast<int>(ctx.number(3, want.w));
            if (ctx.arg(4)) want.h = static_cast<int>(ctx.number(4, want.h));

            const std::string what = "Move tile to " + std::to_string(want.x + 1) +
                                     "," + std::to_string(want.y + 1) + " at " +
                                     std::to_string(want.w) + "x" +
                                     std::to_string(want.h);
            SurfaceEdit undo(ctx.engine, what, page);
            const PlaceResult r = store.moveTile(page, t->uid, want);
            if (!r.ok) {
                undo.cancel();
                return Result::bad(r.error);
            }
            undo.commit(ctx.engine.history, ctx.engine);
            ctx.engine.actionLog.add(what, "layout", ctx.source, ctx.prompt);
            return Result::good("tile moved to " + std::to_string(want.x + 1) + "," +
                                std::to_string(want.y + 1) + " at " +
                                std::to_string(want.w) + "x" + std::to_string(want.h));
        }});

    registry_.add({
        "TileSelect", {"tsel"},
        {{"slot", ParamType::Targets, false, "slots to select; clears if omitted"}},
        Privilege::Operator, false, true,
        "Selects tiles on the current page for alignment. Selection ORDER "
        "matters: match-width takes its width from the first (SPEC 9.1).",
        [](Context& ctx) -> Result {
            auto& store = ctx.engine.surfaces;
            const int page = store.current();
            Surface* sf = store.byId(page);
            if (!sf) return Result::bad("no surface page selected");

            // Slots arrive as plain arguments: `TileSelect 1 4 7`, or with
            // the `+` the rest of the grammar uses, which the tokeniser drops
            // between numbers. Either reads naturally at a console.
            if (ctx.args.empty()) {
                store.deselect(page);
                return Result::good("tile selection cleared");
            }
            std::vector<Uid> picked;
            for (const Arg& a : ctx.args) {
                if (!a.isNumber) continue;
                const int slot = static_cast<int>(a.number) - 1;
                const Tile* tile = sf->atSlot(slot);
                if (!tile)
                    return Result::bad("no tile at slot " + std::to_string(slot + 1) +
                                       occupiedSlots(*sf));
                picked.push_back(tile->uid);
            }
            if (picked.empty()) {
                store.deselect(page);
                return Result::good("tile selection cleared");
            }
            store.select(page, picked);
            return Result::good(plural(picked.size(), "tile", "tiles") + " selected");
        }});

    registry_.add({
        "Align", {},
        {{"how", ParamType::Enum, true, "how to arrange them",
          {"left", "right", "top", "bottom", "matchwidth", "matchheight",
           "spreadh", "spreadv", "fillrow"}}},
        Privilege::Operator, false, false,
        "Arranges the selected tiles. Applied all or nothing: a refused "
        "alignment leaves the page untouched (SPEC 9.1).",
        [](Context& ctx) -> Result {
            auto& store = ctx.engine.surfaces;
            const int page = store.current();
            Surface* sf = store.byId(page);
            if (!sf) return Result::bad("no surface page selected");
            if (!sf->editMode)
                return Result::bad("page " + std::to_string(page) +
                                   " is in run mode — `EditMode on` first");

            const std::optional<SurfaceStore::Align> how =
                SurfaceStore::alignFromName(ctx.text(0));
            if (!how) return Result::bad("unknown alignment: " + ctx.text(0));

            const std::string what = std::string("Align ") + ctx.text(0);
            SurfaceEdit undo(ctx.engine, what, page);
            const SurfaceStore::AlignResult r = store.align(page, *how);
            if (!r.ok) {
                // A refused alignment changed nothing, so it must not leave
                // an entry: an undo that does nothing is worse than no undo,
                // because it consumes the press that would have worked.
                undo.cancel();
                return Result::bad(r.error);
            }
            undo.commit(ctx.engine.history, ctx.engine);
            ctx.engine.actionLog.add(what, "layout", ctx.source, ctx.prompt);
            return Result::good(plural(static_cast<size_t>(r.moved), "tile", "tiles") +
                                " moved");
        }});

    // The executor fader. The single most-used control on a console, and it
    // had no command form until the surface needed one — the same §16.2.1
    // gap as Group and Preset, found the same way.
    registry_.add({
        "ExecMaster", {"xm"},
        {{"executor", ParamType::Int, true, "executor id"},
         {"value", ParamType::Percent, true, "0-100"}},
        Privilege::Operator, false, false,
        "Sets an executor's master fader. Not undoable: it is playback, not "
        "an edit (SPEC 8.6).",
        [](Context& ctx) -> Result {
            const int id = static_cast<int>(ctx.number(0, 0));
            Executor* x = ctx.engine.executor(id);
            if (!x) return Result::bad("no executor " + std::to_string(id));
            if (!ctx.arg(1) || !ctx.arg(1)->isNumber)
                return Result::bad("expected a value");
            x->master = pct(ctx.number(1, 100));
            return Result::good("executor " + std::to_string(id) + " master " +
                                std::to_string(static_cast<int>(ctx.number(1, 100))) + "%");
        }});

    registry_.add({
        "Blackout", {"bo"},
        {{"state", ParamType::Bool, false, "on or off; toggles if omitted"}},
        Privilege::Operator, false, false,
        "Zeroes intensity without changing anything else.",
        [](Context& ctx) -> Result {
            const std::string s = ctx.text(0);
            if (s.empty()) ctx.engine.blackout = !ctx.engine.blackout;
            else ctx.engine.blackout = (s == "on" || s == "On" || s == "1");
            return Result::good(ctx.engine.blackout ? "blackout on" : "blackout off");
        }});

    registry_.add({
        "UnusedChannels", {},
        {{"mode", ParamType::Enum, true, "what LTP attributes do on release",
          {"hold", "default"}}},
        Privilege::Operator, false, false,
        "LTP attributes hold their last value or return to default when the "
        "programmer releases them. HTP always returns to default (SPEC 11.9).",
        [](Context& ctx) -> Result {
            const std::string m = ctx.text(0);
            if (m == "hold") ctx.engine.unusedMode = UnusedMode::HoldLast;
            else if (m == "default") ctx.engine.unusedMode = UnusedMode::ToDefault;
            else return Result::bad("expected hold or default");
            return Result::good("unused channels: " + m);
        }});

    // ---------------------------------------------------------------- patch
    registry_.add({
        "Patch", {},
        {{"type", ParamType::String, true, "fixture type name, in quotes"},
         {"count", ParamType::Int, false, "how many; 1 if omitted"},
         {"universe", ParamType::Int, false, "universe; 1 if omitted"},
         {"address", ParamType::Int, false, "start address; next free if omitted"}},
        Privilege::Operator, false, true,
        "Patches fixtures at the selected ids. "
        "`Fixture 1 Thru 8 Patch \"LED Par RGBW\"`",
        [](Context& ctx) -> Result {
            const auto targets = ctx.targets.empty()
                                     ? ctx.engine.programmer.selection().flatten()
                                     : ctx.targets;
            if (targets.empty()) return Result::bad("no fixture ids given");

            const std::string typeName = ctx.text(0);
            const int ti = ctx.engine.patch.findType(typeName);
            if (ti < 0) return Result::bad("unknown fixture type: " + typeName);

            // `count` only means anything when one id was given: with a range
            // the targets already carry the count. Declared metadata that the
            // handler ignores is exactly the drift this layer exists to stop.
            std::vector<Target> expanded = targets;
            const int count = static_cast<int>(ctx.number(1, 1));
            if (targets.size() == 1 && count > 1) {
                expanded.clear();
                for (int i = 0; i < count; ++i)
                    expanded.push_back({static_cast<FixtureId>(targets[0].fixture +
                                                               static_cast<FixtureId>(i)),
                                        0});
            }

            const int universe = static_cast<int>(ctx.number(2, 1));
            int address = static_cast<int>(ctx.number(3, 0));
            const int fp = ctx.engine.patch.type(ti).footprint();
            if (address <= 0) address = ctx.engine.patch.nextFreeAddress(universe, fp);

            // One transaction over the whole range, so a bulk patch undoes as
            // a unit rather than one fixture at a time (SPEC 8.6).
            std::vector<FixtureId> ids;
            for (const Target& t : expanded) ids.push_back(t.fixture);
            PatchEdit undo(ctx.engine, "Patch " + typeName, ids);

            int made = 0;
            std::string err;
            for (const Target& t : expanded) {
                Patch::PatchRequest req;
                req.id = t.fixture;
                req.typeIndex = ti;
                req.universe = universe;
                req.address = address;
                if (!ctx.engine.patch.add(req, &err)) break;
                ++made;
                address += fp;
            }
            ctx.engine.syncUniverses();
            if (made > 0) {
                undo.commit(ctx.engine.history, ctx.engine);
                ctx.engine.actionLog.add(
                    "Patch " + std::to_string(made) + " x " + typeName, "patch",
                    ctx.source, ctx.prompt);
            }
            if (made == 0) return Result::bad(err.empty() ? "nothing patched" : err);
            return Result::good("patched " + plural(static_cast<size_t>(made),
                                                    "fixture", "fixtures") +
                                (err.empty() ? "" : " (stopped: " + err + ")"));
        }});

    registry_.add({
        "Unpatch", {},
        {{"", ParamType::Targets, false, "uses the selection"}},
        Privilege::Protected, true, true,
        "Removes fixtures from the patch. Programming referencing them is "
        "kept, because cues hold UIDs (SPEC 8.1).",
        [](Context& ctx) -> Result {
            const auto targets = ctx.targets.empty()
                                     ? ctx.engine.programmer.selection().flatten()
                                     : ctx.targets;
            if (targets.empty()) return Result::bad("nothing selected");

            std::vector<FixtureId> ids;
            for (const Target& t : targets) ids.push_back(t.fixture);
            PatchEdit undo(ctx.engine, "Unpatch", ids);

            int n = 0;
            for (const Target& t : targets)
                if (ctx.engine.patch.remove(t.fixture)) ++n;
            ctx.engine.syncUniverses();
            if (n > 0) {
                undo.commit(ctx.engine.history, ctx.engine);
                ctx.engine.actionLog.add(
                    "Unpatch " + std::to_string(n) + " fixture(s)", "patch",
                    ctx.source, ctx.prompt);
            }
            return Result::good("unpatched " + plural(static_cast<size_t>(n),
                                                      "fixture", "fixtures"));
        }});

    // ----------------------------------------------------------- show files
    registry_.add({
        "SaveShow", {},
        {{"path", ParamType::String, true, "where to write the .lumen file"},
         {"name", ParamType::String, false, "show name stored in the file"}},
        Privilege::Operator, false, false,
        "Writes the whole show in one transaction (SPEC 17.7). The previous "
        "file is replaced only once a complete one exists.",
        [](Context& ctx) -> Result {
            const std::string path = ctx.text(0);
            if (path.empty()) return Result::bad("no path given");
            SaveReport s = ShowFile::save(ctx.engine, path, ctx.text(1));
            if (!s.ok) return Result::bad("save failed: " + s.error);
            std::ostringstream o;
            o << s.fixtures << " fixtures, " << s.cues << " cues, " << s.values
              << " values, " << s.groups << " groups, " << s.presets << " presets, "
              << s.recipes << " recipes, " << s.library << " library fixtures";
            return Result::good(o.str());
        }});

    registry_.add({
        "LoadShow", {},
        {{"path", ParamType::String, true, "a .lumen file"}},
        Privilege::Protected, true, false,
        "Replaces the current show. Protected and destructive: on failure the "
        "engine is left exactly as it was.",
        [](Context& ctx) -> Result {
            const std::string path = ctx.text(0);
            if (path.empty()) return Result::bad("no path given");
            LoadReport l = ShowFile::load(ctx.engine, path);
            if (!l.ok) return Result::bad("load failed: " + l.error);
            Result r = Result::good();
            for (const auto& w : l.warnings) r.rows.push_back("warning: " + w);
            std::ostringstream o;
            o << (l.showName.empty() ? std::string("untitled") : l.showName) << ": "
              << l.fixtures << " fixtures, " << l.cues << " cues, " << l.executors
              << " executors";
            r.message = o.str();
            return r;
        }});

    // --------------------------------------------------------------- import
    registry_.add({
        "ImportGdtf", {},
        {{"path", ParamType::String, true, "path to a .gdtf package"},
         {"mode", ParamType::String, false, "DMX mode name; all modes if omitted"}},
        Privilege::Operator, false, false,
        "Imports a GDTF profile into the fixture library and publishes its "
        "modes as patchable types. The package is kept verbatim, so exporting "
        "it again returns the original file (SPEC 1.4).",
        [](Context& ctx) -> Result {
            const std::string path = ctx.text(0);
            if (path.empty()) return Result::bad("no path given");

            auto imp = ctx.engine.library.importFile(path);
            if (!imp.ok) return Result::bad("gdtf: " + imp.error);

            const LibraryEntry* e = ctx.engine.library.find(imp.uid);
            if (!e) return Result::bad("import produced no library entry");

            const std::string want = ctx.text(1);
            int added = 0;
            Result r = Result::good();
            for (size_t i = 0; i < e->def.modes.size(); ++i) {
                if (!want.empty() && e->def.modes[i].name != want) continue;
                const auto pub = ctx.engine.library.publish(imp.uid, i, ctx.engine.patch);
                if (!pub.ok) {
                    r.rows.push_back("mode '" + e->def.modes[i].name +
                                     "' did not compile: " + pub.error);
                    continue;
                }
                ++added;
                r.rows.push_back(pub.typeName + "  " +
                                 std::to_string(
                                     ctx.engine.patch.type(pub.typeIndex).footprint()) +
                                 "ch");
            }
            if (added == 0)
                return Result::bad(want.empty() ? "no mode in the profile compiled"
                                                : "no mode named " + want);
            for (const auto& w : imp.warnings) r.rows.push_back("warning: " + w);
            r.message = e->name + ": " + plural(static_cast<size_t>(added),
                                                "type", "types") +
                        ", package kept";
            return r;
        }});

    registry_.add({
        "ExportGdtf", {},
        {{"fixture", ParamType::String, true, "a library fixture name"},
         {"path", ParamType::String, true, "where to write the .gdtf package"}},
        Privilege::Operator, false, false,
        "Writes a library fixture back out as a .gdtf. An untouched profile "
        "is copied byte for byte; an edited one is regenerated from Lumen's "
        "model, and the reply says what regenerating cost (SPEC 1.4).",
        [](Context& ctx) -> Result {
            const std::string name = ctx.text(0);
            const std::string path = ctx.text(1);
            if (name.empty() || path.empty())
                return Result::bad("needs a fixture name and a path");

            const LibraryEntry* e = ctx.engine.library.byName(name);
            if (!e) return Result::bad("no fixture in the library named " + name);

            const LibraryExport x = ctx.engine.library.exportEntry(e->uid);
            if (!x.ok) return Result::bad(x.error);

            std::string err;
            if (!ctx.engine.library.exportToFile(e->uid, path, &err))
                return Result::bad("could not write: " + err);

            Result r = Result::good();
            for (const auto& d : x.dropped) r.rows.push_back("not written: " + d);
            r.message = e->name + " -> " + path + " (" +
                        (x.verbatim ? "the original package, unchanged"
                                    : "regenerated from the model") +
                        ", " + std::to_string(x.bytes.size()) + " bytes)";
            return r;
        }});

    registry_.add({
        "PublishFixture", {},
        {{"fixture", ParamType::String, true, "a library fixture name"},
         {"mode", ParamType::String, false, "DMX mode; the first if omitted"}},
        Privilege::Operator, false, false,
        "Compiles a library fixture's mode into a patchable type. This is the "
        "step between editing a definition and patching it (SPEC 1.2).",
        [](Context& ctx) -> Result {
            const LibraryEntry* e = ctx.engine.library.byName(ctx.text(0));
            if (!e) return Result::bad("no fixture in the library named " + ctx.text(0));

            size_t mi = 0;
            const std::string want = ctx.text(1);
            if (!want.empty()) {
                const int found = e->def.modeIndex(want);
                if (found < 0) return Result::bad("no mode named " + want);
                mi = static_cast<size_t>(found);
            }

            const auto pub = ctx.engine.library.publish(e->uid, mi, ctx.engine.patch);
            if (!pub.ok) return Result::bad(pub.error);

            Result r = Result::good();
            for (const auto& w : pub.warnings) r.rows.push_back("warning: " + w);
            r.message = pub.typeName + "  " +
                        std::to_string(
                            ctx.engine.patch.type(pub.typeIndex).footprint()) +
                        "ch";
            return r;
        }});

    registry_.add({
        "ImportMvr", {},
        {{"path", ParamType::String, true, "path to a .mvr file"}},
        Privilege::Operator, false, false,
        "Imports an MVR: profiles, patch, stage positions and one group per "
        "layer (SPEC 17.5). Additive: an id already in use is renumbered and "
        "reported, never overwritten, which is why this is not Protected.",
        [](Context& ctx) -> Result {
            const std::string path = ctx.text(0);
            if (path.empty()) return Result::bad("no path given");
            MvrReport m = loadMvrFile(path);
            if (!m.ok) return Result::bad("mvr: " + m.error);

            MvrApplyReport ap =
                applyMvr(ctx.engine.patch, &ctx.engine.groups, m);
            ctx.engine.syncUniverses();

            Result r = Result::good();
            for (const auto& w : m.warnings) r.rows.push_back("file: " + w);
            for (const auto& w : ap.warnings) r.rows.push_back("patch: " + w);
            std::ostringstream o;
            o << ap.patched << " patched, " << ap.typesAdded << " type(s), "
              << ap.groupsCreated << " group(s)";
            if (ap.skipped) o << ", " << ap.skipped << " skipped";
            r.message = o.str();
            // Nothing landing at all is a failed import, not a quiet success.
            if (ap.patched == 0) return Result::bad(o.str());
            return r;
        }});

    registry_.add({
        "ExportMvr", {},
        {{"path", ParamType::String, true, "where to write the .mvr"},
         {"layer", ParamType::String, false, "layer for untagged fixtures"}},
        Privilege::Operator, false, false,
        "Writes the patch out as an MVR: positions, addresses, layers from "
        "tags, and the GDTF profiles the library kept (SPEC 17.5).",
        [](Context& ctx) -> Result {
            const std::string path = ctx.text(0);
            if (path.empty()) return Result::bad("no path given");
            const std::string layer = ctx.text(1, "Lumen");

            MvrWriteReport w =
                writeMvrFile(ctx.engine.patch, ctx.engine.library, path, layer);
            if (!w.ok) return Result::bad("mvr: " + w.error);

            Result r = Result::good();
            for (const auto& g : w.regenerated)
                r.rows.push_back("profile written from Lumen's model, not copied: " + g);
            for (const auto& warn : w.warnings) r.rows.push_back("warning: " + warn);
            std::ostringstream o;
            o << w.fixtures << " fixtures, " << w.layers << " layer(s), "
              << w.profiles << " profile(s)";
            if (w.skipped) o << ", " << w.skipped << " skipped";
            r.message = o.str();
            // Writing an archive with no fixtures in it is not a success.
            if (w.fixtures == 0) return Result::bad(o.str());
            return r;
        }});

    // -------------------------------------------------------- timecode (18.5)
    registry_.add({
        "Timecode", {"tc"},
        {{"slot", ParamType::Int, true, "slot id, from `List timecode`"},
         {"action", ParamType::Enum, true, "start · stop · set",
          {"start", "stop", "set"}},
         {"hours", ParamType::Int, false, "with set"},
         {"minutes", ParamType::Int, false, "with set"},
         {"seconds", ParamType::Int, false, "with set"},
         {"frames", ParamType::Int, false, "with set"}},
        Privilege::Operator, false, false,
        "Controls a timecode slot. An external slot can be driven manually "
        "for programming without its source running (SPEC 18.5).",
        [](Context& ctx) -> Result {
            const int id = static_cast<int>(ctx.number(0, 0));
            TimecodeSlot* s = ctx.engine.timecode.slot(id);
            if (!s) return Result::bad("no timecode slot " + std::to_string(id));
            const std::string action = ctx.text(1);

            if (action == "start") {
                ctx.engine.timecode.start(id);
                return Result::good(s->name + " running at " +
                                    s->at(ctx.engine.now()).text(s->rate));
            }
            if (action == "stop") {
                ctx.engine.timecode.stop(id);
                return Result::good(s->name + " stopped at " +
                                    s->position.text(s->rate));
            }
            if (action == "set") {
                Timecode tc;
                tc.hours = static_cast<int>(ctx.number(2, 0));
                tc.minutes = static_cast<int>(ctx.number(3, 0));
                tc.seconds = static_cast<int>(ctx.number(4, 0));
                tc.frames = static_cast<int>(ctx.number(5, 0));
                if (!ctx.engine.timecode.setPosition(id, tc))
                    return Result::bad(tc.text(s->rate) +
                                       " is not a valid reading at " +
                                       frameRateName(s->rate));
                return Result::good(s->name + " at " + tc.text(s->rate));
            }
            return Result::bad("expected start, stop or set");
        }});

    registry_.add({
        "AddTimecode", {},
        {{"name", ParamType::String, true, "slot name"},
         {"source", ParamType::Enum, false, "internal · ltc · mtc · artnet",
          {"internal", "ltc", "mtc", "artnet"}},
         {"rate", ParamType::Enum, false, "24 · 25 · 2997 · 30",
          {"24", "25", "2997", "30"}}},
        Privilege::Operator, false, false,
        "Adds a timecode slot. Several run independently (SPEC 18.5).",
        [](Context& ctx) -> Result {
            const std::string name = ctx.text(0);
            if (name.empty()) return Result::bad("no name given");
            const std::string src = ctx.text(1, "internal");
            TimecodeSource source = TimecodeSource::Internal;
            if (src == "ltc") source = TimecodeSource::Ltc;
            else if (src == "mtc") source = TimecodeSource::Mtc;
            else if (src == "artnet") source = TimecodeSource::ArtNet;

            const std::string rateName = ctx.text(2, "25");
            FrameRate rate = FrameRate::Fps25;
            if (rateName == "24") rate = FrameRate::Fps24;
            else if (rateName == "2997") rate = FrameRate::Fps2997Drop;
            else if (rateName == "30") rate = FrameRate::Fps30;

            const TimecodeSlot& s = ctx.engine.timecode.add(name, source, rate);
            return Result::good("slot " + std::to_string(s.id) + " '" + s.name +
                                "' " + timecodeSourceName(source) + " @ " +
                                frameRateName(rate));
        }});

    // ------------------------------------------------------------- undo (8.6)
    registry_.add({
        "Undo", {},
        {{"scope", ParamType::Enum, false,
          "programmer · patch · cue · preset · group · recipe · binding",
          {"programmer", "patch", "cue", "preset", "group", "recipe", "binding"}}},
        Privilege::Operator, false, false,
        "Reverts the last edit. With a scope, reverts the last edit of that "
        "kind — refused when a later edit touched the same objects, naming "
        "what blocks it. Playback is never undone, so this is safe to press "
        "during a show (SPEC 8.6).",
        [](Context& ctx) -> Result {
            const std::string scopeName = ctx.text(0);
            UndoReport r;
            if (scopeName.empty()) {
                r = ctx.engine.history.undo(ctx.engine);
            } else {
                const auto scope = editScopeFromName(scopeName);
                if (!scope) return Result::bad("unknown scope: " + scopeName);
                r = ctx.engine.history.undoScoped(ctx.engine, *scope);
            }
            if (r.result == UndoReport::Result::Blocked)
                return Result::bad("cannot undo '" + r.description +
                                   "': a later action touched the same objects — " +
                                   r.blockedBy);
            if (!r.ok()) return Result::bad(r.description);
            ctx.engine.actionLog.markUndone(r.description);
            ctx.engine.syncUniverses();
            return Result::good("undid " + r.description);
        }});

    registry_.add({
        "Redo", {},
        {{"scope", ParamType::Enum, false, "same scopes as Undo",
          {"programmer", "patch", "cue", "preset", "group", "recipe", "binding"}}},
        Privilege::Operator, false, false,
        "Reapplies the last undone edit (SPEC 8.6).",
        [](Context& ctx) -> Result {
            const std::string scopeName = ctx.text(0);
            UndoReport r;
            if (scopeName.empty()) {
                r = ctx.engine.history.redo(ctx.engine);
            } else {
                const auto scope = editScopeFromName(scopeName);
                if (!scope) return Result::bad("unknown scope: " + scopeName);
                r = ctx.engine.history.redoScoped(ctx.engine, *scope);
            }
            if (r.result == UndoReport::Result::Blocked)
                return Result::bad("cannot redo '" + r.description + "': " +
                                   r.blockedBy + " is in the way");
            if (!r.ok()) return Result::bad(r.description);
            ctx.engine.syncUniverses();
            return Result::good("redid " + r.description);
        }});

    registry_.add({
        "UndoThrough", {},
        {{"id", ParamType::Int, true, "a history entry id, from `List history`"}},
        Privilege::Protected, true, false,
        "Undoes everything back through a chosen entry, as one operation "
        "(SPEC 8.6's jump-to-point). Protected: it can revert a lot at once.",
        [](Context& ctx) -> Result {
            const int id = static_cast<int>(ctx.number(0, 0));
            Uid target{};
            for (const HistoryEntry& e : ctx.engine.history.entries())
                if (e.id == id) target = e.uid;
            if (!target.valid()) return Result::bad("no history entry " +
                                                    std::to_string(id));
            const UndoReport r = ctx.engine.history.undoThrough(ctx.engine, target);
            if (!r.ok()) return Result::bad(r.description);
            ctx.engine.syncUniverses();
            return Result::good("undid " +
                                plural(static_cast<size_t>(r.entriesAffected),
                                       "entry", "entries") +
                                ", back through " + r.description);
        }});

    // ------------------------------------------------------------ learn (12)
    registry_.add({
        "Learn", {},
        {{"state", ParamType::Enum, false, "on · off · next · clear",
          {"on", "off", "next", "clear"}},
         {"set", ParamType::String, false, "binding set to learn into"}},
        Privilege::Operator, false, false,
        "Enters or leaves Learn. Every bindable target highlights; send an "
        "input to bind it to the focused one (SPEC 12.1).",
        [](Context& ctx) -> Result {
            if (!ctx.console) return Result::bad("learn needs a console");
            Learn& learn = ctx.console->learn();
            const std::string state = ctx.text(0, "on");
            const std::string setName = ctx.text(1);

            if (!setName.empty()) {
                BindingSet* s = ctx.engine.bindings.setByName(setName);
                if (!s) return Result::bad("no binding set named " + setName);
                learn.setTargetSet(s->uid);
            }

            if (state == "off") {
                learn.end();
                return Result::good("learn off");
            }
            if (state == "clear") {
                const int n = learn.clearSet();
                return Result::good("cleared " +
                                    plural(static_cast<size_t>(n), "binding",
                                           "bindings"));
            }
            if (state == "next") {
                if (!learn.focus(learn.focusIndex() + 1))
                    return Result::bad("no further target");
                const BindTarget* t = learn.focused();
                return Result::good("learning " + (t ? t->describe() : std::string()));
            }

            if (!learn.targetSet().valid()) {
                // Without a set, learning has nowhere to put anything. Make
                // one rather than refusing, but say so.
                BindingSet s;
                s.name = "Default";
                learn.setTargetSet(ctx.engine.bindings.addSet(std::move(s)).uid);
                learn.begin();
                return Result::good("learn on, into a new set 'Default'");
            }
            learn.begin();
            const BindingSet* s = ctx.engine.bindings.set(learn.targetSet());
            return Result::good("learn on, into '" + (s ? s->name : std::string()) +
                                "'");
        }});

    registry_.add({
        "Unlearn", {},
        {}, Privilege::Operator, false, false,
        "Removes the binding on the focused Learn target (SPEC 12.1).",
        [](Context& ctx) -> Result {
            if (!ctx.console) return Result::bad("unlearn needs a console");
            const LearnResult r = ctx.console->learn().unlearnFocused();
            if (r.outcome != LearnResult::Outcome::Unlearned)
                return Result::bad(r.message.empty() ? "nothing to unlearn"
                                                     : r.message);
            return Result::good(r.message);
        }});

    registry_.add({
        "BindingSet", {},
        {{"name", ParamType::String, true, "the set"},
         {"state", ParamType::Enum, false, "on · off · page",
          {"on", "off", "page"}},
         {"page", ParamType::Int, false, "page number, with state=page"}},
        Privilege::Operator, false, false,
        "Creates, enables or disables a named binding set, or makes it "
        "page-local. Page-local bindings win over global ones (SPEC 12.6).",
        [](Context& ctx) -> Result {
            const std::string name = ctx.text(0);
            if (name.empty()) return Result::bad("no set name given");
            BindingSet* s = ctx.engine.bindings.setByName(name);
            const std::string state = ctx.text(1, "on");

            if (!s) {
                if (state == "off") return Result::bad("no binding set named " + name);
                BindingSet made;
                made.name = name;
                s = &ctx.engine.bindings.addSet(std::move(made));
                return Result::good("created binding set '" + name + "'");
            }
            if (state == "page") {
                s->page = static_cast<int>(ctx.number(2, 0));
                return Result::good(name + " is now " +
                                    (s->page ? "local to page " +
                                                   std::to_string(s->page)
                                             : std::string("global")));
            }
            ctx.engine.bindings.activate(s->uid, state != "off");
            return Result::good(name + (state == "off" ? " disabled" : " enabled"));
        }});

    registry_.add({
        "Page", {},
        {{"page", ParamType::Int, true, "the active page"}},
        Privilege::Operator, false, false,
        "Switches the active executor page, which also switches which "
        "page-local bindings resolve and resets fader takeover (SPEC 12.6).",
        [](Context& ctx) -> Result {
            const int page = static_cast<int>(ctx.number(0, 0));
            if (page < 0) return Result::bad("page cannot be negative");
            ctx.engine.bindings.setPage(page);
            return Result::good("page " + std::to_string(page));
        }});

    // -------------------------------------------------- introspection (16.3)
    registry_.add({
        "List", {"ls"},
        {{"what", ParamType::Enum, true, "what to list",
          {"patch", "types", "sequences", "executors", "programmer", "selection",
           "groups", "presets", "recipes", "library", "bindings", "history", "log",
           "sources", "timecode", "commands"}}},
        Privilege::Any, false, false,
        "Structured query over the show model. Paginated by the caller, never "
        "a whole-show dump (SPEC 16.3).",
        [](Context& ctx) -> Result {
            const std::string what = ctx.text(0);
            Result r = Result::good();
            Engine& e = ctx.engine;

            if (what == "patch") {
                for (const auto& f : e.patch.fixtures()) {
                    std::ostringstream o;
                    o << f.id << "  " << f.name << "  " << f.universe << "." << f.address
                      << "  uid=" << toHex(f.uid);
                    r.rows.push_back(o.str());
                }
            } else if (what == "types") {
                for (int i = 0; i < e.patch.typeCount(); ++i) {
                    const auto& t = e.patch.type(i);
                    std::ostringstream o;
                    o << t.name << "  " << t.footprint() << "ch";
                    if (t.cellCount) o << "  " << t.cellCount << " cells";
                    r.rows.push_back(o.str());
                }
            } else if (what == "sequences") {
                for (const auto& s : e.sequences) {
                    std::ostringstream o;
                    o << s.id << "  " << s.name << "  " << s.cues.size() << " cue(s)";
                    r.rows.push_back(o.str());
                }
            } else if (what == "executors") {
                for (const auto& x : e.executors) {
                    std::ostringstream o;
                    o << x.id << "  seq=" << x.sequenceId << "  prio=" << x.priority
                      << "  " << (x.active() ? "active" : "idle");
                    if (x.active()) o << " cue " << (x.currentIndex() + 1);
                    r.rows.push_back(o.str());
                }
            } else if (what == "programmer") {
                for (AttrKey k : e.programmer.values().keys()) {
                    const Value* v = e.programmer.values().find(k);
                    std::ostringstream o;
                    o << keyFixture(k);
                    if (keyCell(k)) o << "." << keyCell(k);
                    o << "  " << attrName(keyAttr(k)) << "  "
                      << static_cast<int>(v->v * 100.f) << "%"
                      << (v->active ? "" : "  [inactive]")
                      << (v->isRef() ? "  [preset]" : "");
                    r.rows.push_back(o.str());
                }
            } else if (what == "selection") {
                for (const Target& t : e.programmer.selection().flatten()) {
                    std::ostringstream o;
                    o << t.fixture;
                    if (t.cell) o << "." << t.cell;
                    r.rows.push_back(o.str());
                }
            } else if (what == "groups") {
                for (const auto& g : e.groups.all()) {
                    std::ostringstream o;
                    o << g.id << "  " << g.name << "  " << groupKindName(g.kind);
                    if (g.isDynamic())
                        o << "  " << e.groups.resolve(g.uid, e.patch).targets.size()
                          << " matched";
                    else
                        o << "  " << g.members.size() << " member(s)";
                    if (g.isNested()) o << "  +" << g.children.size() << " nested";
                    r.rows.push_back(o.str());
                }
            } else if (what == "presets") {
                for (const auto& p : e.presets.pools()) {
                    std::ostringstream o;
                    o << p.id << "  " << p.name << "  " << groupName(p.group) << "  "
                      << p.presets.size() << " preset(s)";
                    r.rows.push_back(o.str());
                }
            } else if (what == "recipes") {
                for (const auto& rc : e.recipes.all()) {
                    std::ostringstream o;
                    o << rc.name << "  " << rc.slots.size() << " slot(s)";
                    r.rows.push_back(o.str());
                }
            } else if (what == "library") {
                for (const auto& e : ctx.engine.library.all()) {
                    std::ostringstream o;
                    o << e.id << "  " << e.name << "  " << e.def.modes.size()
                      << " mode(s)";
                    if (e.hasOriginal())
                        o << "  [" << e.sourceName << ", " << e.gdtf.size() << " bytes"
                          << (e.edited ? ", edited" : "") << "]";
                    else
                        o << "  [authored here]";
                    r.rows.push_back(o.str());
                }
            } else if (what == "bindings") {
                for (const auto& set : e.bindings.sets()) {
                    for (const auto& b : set.bindings) {
                        std::ostringstream o;
                        o << set.name << (set.page ? " p" + std::to_string(set.page)
                                                   : std::string())
                          << "  " << b.source.describe() << " -> "
                          << b.target.describe() << "  " << bindModeName(b.map.mode)
                          << "/" << takeoverName(b.map.takeover)
                          << (b.enabled ? "" : "  [off]");
                        r.rows.push_back(o.str());
                    }
                }
            } else if (what == "history") {
                const int cursor = e.history.cursor();
                int i = 0;
                for (const auto& h : e.history.entries()) {
                    std::ostringstream o;
                    // The cursor marker is what makes a history view readable:
                    // entries below it are applied, above it are undone.
                    o << (i < cursor ? "  " : "~ ") << h.id << "  "
                      << editScopeName(h.scope) << "  " << h.description << "  ["
                      << h.source << "]";
                    r.rows.push_back(o.str());
                    ++i;
                }
            } else if (what == "log") {
                for (const auto& l : e.actionLog.all()) {
                    std::ostringstream o;
                    o << l.scope << "  " << l.description << "  [" << l.source << "]"
                      << (l.undone ? "  (undone)" : "");
                    if (!l.prompt.empty()) o << "  prompt: " << l.prompt;
                    r.rows.push_back(o.str());
                }
            } else if (what == "sources") {
                // §18.3's diagnostic page: the first question when two desks
                // fight over a universe is who is actually sending.
                for (int u : e.dmxIn.universes().empty()
                                 ? std::vector<int>{}
                                 : e.dmxIn.universes()) {
                    const MergeReport m = e.dmxIn.report(u);
                    std::ostringstream o;
                    o << "universe " << u << "  prio " << int(m.winningPriority)
                      << "  " << m.sourcesActive << "/" << m.sourcesSeen
                      << " active  " << tieBreakName(m.tie)
                      << (m.merged ? "  merged" : "");
                    r.rows.push_back(o.str());
                }
                for (const SourceInfo& s : e.dmxIn.sources()) {
                    std::ostringstream o;
                    o << "  U" << s.universe << "  "
                      << (s.name.empty() ? s.cidText() : s.name) << "  "
                      << inputProtocolName(s.protocol) << "  prio " << int(s.priority)
                      << "  " << s.origin << "  " << s.packets << " pkt";
                    if (s.outOfOrder) o << ", " << s.outOfOrder << " late";
                    if (s.terminated) o << "  [terminated]";
                    else if (!s.active) o << "  [quiet]";
                    else if (s.winning) o << "  [winning]";
                    else o << "  [held back]";
                    r.rows.push_back(o.str());
                }
            } else if (what == "timecode") {
                for (const TimecodeSlot& s : e.timecode.slots()) {
                    std::ostringstream o;
                    o << s.id << "  " << s.name << "  "
                      << timecodeSourceName(s.source) << " @ "
                      << frameRateName(s.rate) << "  "
                      << s.at(e.now()).text(s.rate) << "  "
                      << (s.running ? "running" : "stopped");
                    if (s.stale) o << "  [stale]";
                    if (s.driftFrames > 0.5 || s.driftFrames < -0.5) {
                        char buf[32];
                        std::snprintf(buf, sizeof(buf), "  drift %+.1f frames",
                                      s.driftFrames);
                        o << buf;
                    }
                    if (s.jumps) o << "  " << s.jumps << " jump(s)";
                    r.rows.push_back(o.str());
                }
            } else if (what == "commands") {
                for (const auto* d : ctx.registry.all()) r.rows.push_back(d->name);
            } else {
                return Result::bad("cannot list: " + what);
            }
            r.message = plural(r.rows.size(), "row", "rows");
            return r;
        }});

    registry_.add({
        "Help", {"?"},
        {{"command", ParamType::String, false, "a command name; lists all if omitted"}},
        Privilege::Any, false, false,
        "Help, generated from the same metadata the parser uses.",
        [](Context& ctx) -> Result {
            Result r = Result::good();
            const std::string name = ctx.text(0);
            std::istringstream in(name.empty() ? ctx.registry.helpText()
                                               : ctx.registry.helpFor(name));
            std::string line;
            while (std::getline(in, line)) r.rows.push_back(line);
            r.message = name.empty() ? "all commands" : name;
            return r;
        }});

    registry_.add({
        "Schema", {}, {},
        Privilege::Any, false, false,
        "Agent tool schema as JSON, generated from the command metadata "
        "(SPEC 16.2).",
        [](Context& ctx) -> Result {
            Result r = Result::good("schema");
            r.rows.push_back(ctx.registry.agentSchema());
            return r;
        }});

    (void)eng;
}

} // namespace lumen
