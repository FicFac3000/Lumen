// Builtins.cpp — the M1 command set (SPEC §16.2).
//
// Every one of these is the ONLY route to the operation it performs. Nothing
// in the UI, a macro, a plugin or an agent gets a privileged path into the
// model that bypasses this file.
#include "lumen/Command.hpp"
#include "lumen/Gdtf.hpp"
#include "lumen/Library.hpp"
#include "lumen/Mvr.hpp"
#include "lumen/Show.hpp"

#include <algorithm>
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
            prog.selection().set(targets);
            prog.setAttr(attr, pct(v->number));
            return Result::good(std::string(attrName(attr)) + " at " +
                                std::to_string(static_cast<int>(v->number)) + "% on " +
                                plural(targets.size(), "fixture", "fixtures"));
        }});

    // ---------------------------------------------------------------- clear
    registry_.add({
        "Clear", {"cl"}, {},
        Privilege::Operator, false, false,
        "Staged clear, resolved against programmer state: selection, then "
        "active values, then everything (SPEC 11.8).",
        [](Context& ctx) -> Result {
            const ClearResult r = ctx.engine.programmer.clear();
            return Result::good(clearResultName(r));
        }});

    registry_.add({
        "ClearAll", {}, {},
        Privilege::Operator, false, false,
        "Empties the programmer outright — hold Clear.",
        [](Context& ctx) -> Result {
            return Result::good(clearResultName(ctx.engine.programmer.clearAll()));
        }});

    registry_.add({
        "ClearPresets", {}, {},
        Privilege::Operator, false, false,
        "Removes preset-referenced values, leaving hard values — Shift+Clear.",
        [](Context& ctx) -> Result {
            return Result::good(clearResultName(ctx.engine.programmer.clearPresets()));
        }});

    registry_.add({
        "Flatten", {}, {},
        Privilege::Operator, false, false,
        "Resolves preset references to literal values and keeps them.",
        [](Context& ctx) -> Result {
            ctx.engine.programmer.flatten();
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
                return Result::good("merged into cue " + std::to_string(number));
            }
            Cue c;
            c.uid = ctx.engine.uids.next();
            c.number = number;
            c.values() = layer;
            for (const Phaser& ph : ctx.engine.programmer.phasers()) c.phasers.push_back(ph);
            seq.cues.push_back(std::move(c));
            return Result::good("stored cue " + std::to_string(number) + " in sequence " +
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
            if (idx < 0) return Result::bad("no cue " + std::to_string(num));
            e->gotoCue(*s, ctx.engine.patch, idx, ctx.engine.now());
            return Result::good("exec " + std::to_string(id) + " at cue " + std::to_string(num));
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
            int n = 0;
            for (const Target& t : targets)
                if (ctx.engine.patch.remove(t.fixture)) ++n;
            ctx.engine.syncUniverses();
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
           "groups", "presets", "recipes", "library", "commands"}}},
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
