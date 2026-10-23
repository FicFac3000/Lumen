// lumen-server — the engine, with the interface in a browser.
//
// The engine is the real one, native and unmodified. The UI reaches it only
// through the command layer and the introspection queries (§16.2, §16.3) —
// the same discipline a native UI will have to keep, so nothing learned here
// has to be unlearned later.
//
// Everything that needs hardware is simulated (Simulated.hpp), behind the same
// interfaces the real devices will use. Swapping in an sACN socket or a Helios
// DAC changes which object is constructed and nothing else.
#include "Http.hpp"
#include "Json.hpp"

#include "lumen/Command.hpp"
#include "lumen/Laser.hpp"
#include "lumen/Show.hpp"
#include "lumen/Simulated.hpp"
#include "lumen/Surface.hpp"
#include "lumen/SurfaceView.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

using namespace lumen;
namespace fs = std::filesystem;

namespace {

std::atomic<bool> gRunning{true};
void onSignal(int) { gRunning = false; }

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

std::string mimeFor(const std::string& path) {
    auto ends = [&](const char* s) {
        const size_t n = std::strlen(s);
        return path.size() >= n && path.compare(path.size() - n, n, s) == 0;
    };
    if (ends(".html")) return "text/html; charset=utf-8";
    if (ends(".js")) return "text/javascript; charset=utf-8";
    if (ends(".css")) return "text/css; charset=utf-8";
    if (ends(".svg")) return "image/svg+xml";
    if (ends(".json")) return "application/json";
    return "text/plain; charset=utf-8";
}

// ---------------------------------------------------------------------------
// The session: engine, console, simulated devices, and the lock around them.
// ---------------------------------------------------------------------------
struct Session {
    Engine engine;
    Console console{engine};
    SimulatedOutput* dmx = nullptr;   // owned by the engine
    SimulatedLaser laser;
    // One lock. The HTTP thread and the tick thread both touch the engine,
    // and the engine is explicitly not thread-safe (§15.2) — it expects one
    // control thread.
    std::mutex lock;
    std::string showPath;
};

// A rig worth looking at on first run, so nothing starts empty.
void buildDemoShow(Session& s) {
    Engine& e = s.engine;
    Console& con = s.console;

    installStarterTypes(e.patch);
    con.executeAll(
        "Fixture 1 Thru 8 Patch \"LED Par RGBW\" 8 1 1;"
        "Fixture 101 Thru 104 Patch \"Moving Spot\" 4 2 1;"
        "Fixture 201 Patch \"Pixel Bar 12\" 1 3 1");

    // Stage positions, so the stage-based spread sources have something real
    // to read (§20.2).
    for (FixtureId id = 1; id <= 8; ++id)
        e.patch.setPosition(id, -3.5f + static_cast<float>(id - 1) * 1.0f, 3.0f, 5.0f);
    for (FixtureId id = 101; id <= 104; ++id)
        e.patch.setPosition(id, -3.0f + static_cast<float>(id - 101) * 2.0f, 6.0f,
                            7.0f);
    e.patch.setPosition(201, 0.f, 0.5f, 2.f);
    for (FixtureId id = 1; id <= 8; ++id) e.patch.addTag(id, "Front");
    for (FixtureId id = 101; id <= 104; ++id) e.patch.addTag(id, "Truss");

    // Groups.
    GroupDef front;
    front.name = "Front Wash";
    for (FixtureId id = 1; id <= 8; ++id) front.members.push_back({id, 0});
    e.groups.add(std::move(front));

    GroupDef movers;
    movers.name = "Movers";
    for (FixtureId id = 101; id <= 104; ++id) movers.members.push_back({id, 0});
    e.groups.add(std::move(movers));

    GroupDef odds;
    odds.name = "Odd Pars";
    for (FixtureId id = 1; id <= 8; id += 2) odds.members.push_back({id, 0});
    e.groups.add(std::move(odds));

    // Colour presets, generic so one applies to anything with the attribute.
    e.presets.ensurePool(1, "Colour", Group::Color);
    struct Swatch {
        const char* name;
        float r, g, b;
    };
    const Swatch swatches[] = {{"Red", 1, 0, 0},   {"Amber", 1, 0.6f, 0},
                               {"Green", 0, 1, 0}, {"Cyan", 0, 1, 1},
                               {"Blue", 0, 0, 1},  {"Magenta", 1, 0, 1},
                               {"White", 1, 1, 1}};
    for (const Swatch& sw : swatches) {
        Preset p;
        p.name = sw.name;
        p.group = Group::Color;
        p.values.set(0, 0, Attr::Red, Value::hard(sw.r));
        p.values.set(0, 0, Attr::Green, Value::hard(sw.g));
        p.values.set(0, 0, Attr::Blue, Value::hard(sw.b));
        e.presets.add(1, std::move(p));
    }

    e.presets.ensurePool(2, "Position", Group::Position);
    const std::pair<const char*, std::pair<float, float>> spots[] = {
        {"Home", {0.5f, 0.5f}},
        {"Downstage", {0.5f, 0.35f}},
        {"Band", {0.65f, 0.45f}},
        {"Drums", {0.35f, 0.45f}}};
    for (const auto& [name, pos] : spots) {
        Preset p;
        p.name = name;
        p.group = Group::Position;
        for (FixtureId id = 101; id <= 104; ++id) {
            p.values.set(id, 0, Attr::Pan, Value::hard(pos.first));
            p.values.set(id, 0, Attr::Tilt, Value::hard(pos.second));
        }
        e.presets.add(2, std::move(p));
    }

    // Several sequences of different shapes. One executor tells you nothing
    // about an executor SURFACE — the question is what a dozen look like
    // together, at different sizes and with different amounts of live state.
    con.executeAll(
        "Fixture 1 Thru 8 At 60;"
        "Fixture 1 Thru 8 At Red 100;"
        "Store Cue 1 1;"
        "ClearAll");
    con.executeAll(
        "Fixture 1 Thru 8 At 100;"
        "Fixture 1 Thru 8 At Blue 100;"
        "Fixture 101 Thru 104 At 80;"
        "Store Cue 2 1;"
        "ClearAll");
    con.executeAll(
        "Fixture 101 Thru 104 At 100;"
        "Fixture 201 At 100;"
        "Store Cue 3 1;"
        "ClearAll");

    // A four-step colour chase on the pars.
    for (int step = 0; step < 4; ++step) {
        const char* colour[] = {"Red", "Amber", "Green", "Blue"};
        con.executeAll(std::string("Fixture 1 Thru 8 At 100;Fixture 1 Thru 8 At ") +
                       colour[step] + " 100;Store Cue " + std::to_string(step + 1) +
                       " 2;ClearAll");
    }
    if (Sequence* chase = e.sequence(2)) {
        chase->chase = true;
        chase->chaseRateHz = 2.f;
    }

    // Mover positions, one cue per spot.
    {
        int n = 1;
        for (const char* spot : {"Home", "Band", "Drums", "Downstage"}) {
            con.executeAll(std::string("Fixture 101 Thru 104 At 100;"
                                       "Fixture 101 Thru 104 Preset \"") +
                           spot + "\";Store Cue " + std::to_string(n++) + " 3;ClearAll");
        }
    }

    // A one-cue blinder, the thing a surface needs as a big obvious button.
    con.executeAll("Fixture 1 Thru 8 At 100;Fixture 1 Thru 8 At White 100;"
                   "Store Cue 1 4;ClearAll");

    // Pixel bar looks.
    con.executeAll("Fixture 201 At 100;Store Cue 1 5;ClearAll");
    con.executeAll("Fixture 201 At 60;Store Cue 2 5;ClearAll");

    // Movers only, low, for a quiet state.
    con.executeAll("Fixture 101 Thru 104 At 35;Store Cue 1 6;ClearAll");

    const std::pair<int, const char*> names[] = {
        {1, "Main Looks"}, {2, "Colour Chase"}, {3, "Mover Spots"},
        {4, "Blinder"},    {5, "Pixel Bar"},    {6, "Low State"}};
    for (const auto& [id, name] : names)
        if (Sequence* seq = e.sequence(id)) seq->name = name;

    for (int id = 1; id <= 6; ++id)
        con.execute("Assign " + std::to_string(id) + " " + std::to_string(id));

    // Labels and fader positions differ, so a surface has something to show
    // besides "Exec N".
    const std::pair<int, const char*> labels[] = {
        {1, "Main Looks"}, {2, "Chase"},  {3, "Spots"},
        {4, "BLINDER"},    {5, "Pixels"}, {6, "Low"}};
    for (const auto& [id, label] : labels)
        if (Executor* x = e.executor(id)) x->label = label;
    if (Executor* x = e.executor(2)) x->master = 0.65f;
    if (Executor* x = e.executor(4)) x->master = 1.0f;
    if (Executor* x = e.executor(6)) x->master = 0.4f;

    // Two surface pages, one of each style (§9.6): keeping both means a show
    // owns pages of each, not a global preference.
    installStarterSurfaces(e.surfaces);
    for (int id = 1; id <= 6; ++id)
        if (const Executor* x = e.executor(id))
            e.surfaces.placeInSlot(1, id - 1, TileKind::Executor, x->uid);

    // The canvas page gets the same six, placed by size: a cue stack needs
    // room for its state, a blinder wants to be one big button (§9.2).
    {
        int col = 0, row = 0, rowH = 0;
        for (int id = 1; id <= 6; ++id) {
            const Executor* x = e.executor(id);
            if (!x) continue;
            const Sequence* seq = e.sequence(x->sequenceId);
            const int cues = seq ? static_cast<int>(seq->cues.size()) : 0;
            const int w = (cues >= 3 || x->label == "BLINDER") ? 3 : 2;
            const int h = (cues >= 3 || x->label == "BLINDER") ? 2 : 1;
            Surface* canvas = e.surfaces.byId(2);
            if (col + w > canvas->cols) { col = 0; row += rowH; rowH = 0; }
            e.surfaces.placeAt(2, Rect{col, row, w, h}, TileKind::Executor, x->uid);
            col += w;
            rowH = std::max(rowH, h);
        }
    }

    // A laser zone, disarmed as it must be (§14.6). Created before the
    // tiles, because a tile points at an object rather than conjuring one.
    e.laser.addZone("Centre");

    // §9.3's other tile types, so a surface is not just a row of executors.
    // The wing's second row is where a busking operator keeps the things
    // they reach for without looking.
    con.executeAll(
        "Surface 1;"
        "Tile 11 group 1;"
        "Tile 12 group 2;"
        "Tile 13 group 3;"
        "Tile 15 preset 1.1;"
        "Tile 16 preset 1.2;"
        "Tile 17 preset 1.4;"
        "Tile 18 preset 2.1;"
        "Tile 19 grandmaster;"
        "Tile 20 blackout;"
        "Tile 14 laserzone 1");

    // A binding set, so the Learn surface has something in it.
    con.execute("BindingSet \"Default\"");

    // A timecode slot.
    e.timecode.add("Show", TimecodeSource::Internal, FrameRate::Fps25);

    e.history.clear();   // the demo rig is the starting point, not an edit
}

// ---------------------------------------------------------------------------
// State snapshot
// ---------------------------------------------------------------------------
// Everything the UI draws, in one document. A snapshot rather than a stream,
// because a console UI at a few Hz does not need the complexity of a stream
// and this is trivially inspectable with curl.
std::string snapshot(Session& s) {
    Engine& e = s.engine;
    Json j;
    j.beginObject();

    j.member("time", e.now());
    j.member("grandMaster", e.grandMaster);
    j.member("blackout", e.blackout);
    j.member("page", e.bindings.page());
    j.member("showName", s.showPath);

    // --- programmer -------------------------------------------------------
    j.key("programmer").beginObject();
    j.member("count", e.programmer.values().size());
    j.member("hasActive", e.programmer.values().hasActive());
    j.member("highlight", e.programmer.highlight);
    j.key("selection").beginArray();
    for (const Target& t : e.programmer.selection().flatten()) {
        j.beginObject();
        j.member("fixture", static_cast<long long>(t.fixture));
        j.member("cell", static_cast<int>(t.cell));
        j.endObject();
    }
    j.endArray();
    j.endObject();

    // --- fixtures, with live output (§11.10's fixture sheet) --------------
    j.key("fixtures").beginArray();
    for (const Fixture& f : e.patch.fixtures()) {
        const FixtureType* type = e.patch.typeOf(f.id);
        j.beginObject();
        j.member("id", static_cast<long long>(f.id));
        j.member("name", f.name.empty() ? ("Fixture " + std::to_string(f.id)) : f.name);
        j.member("type", type ? type->name : std::string("?"));
        j.member("universe", f.universe);
        j.member("address", f.address);
        j.member("footprint", type ? type->footprint() : 0);
        j.member("cells", type ? static_cast<int>(type->cellCount) : 0);
        j.key("pos").beginArray().value(f.x).value(f.y).value(f.z).endArray();

        bool selected = false;
        for (const Target& t : e.programmer.selection().flatten())
            if (t.fixture == f.id) selected = true;
        j.member("selected", selected);

        // The attributes this fixture actually has, with their merged output
        // and where the value came from.
        j.key("attrs").beginArray();
        if (type) {
            for (const ChannelDef& c : type->channels) {
                if (c.cell != 0) continue;   // cells are shown separately
                const AttrKey k = makeKey(f.id, 0, c.attr);
                const auto merged = e.merged().get(k);
                const Value* prog = e.programmer.values().find(k);
                j.beginObject();
                j.member("attr", attrName(c.attr));
                j.member("group", groupName(groupOf(c.attr)));
                if (merged) j.member("value", *merged);
                else j.key("value").null();
                j.member("inProgrammer", prog != nullptr);
                j.member("active", prog ? prog->active : false);
                j.member("isRef", prog ? prog->isRef() : false);
                j.endObject();
            }
        }
        j.endArray();
        j.endObject();
    }
    j.endArray();

    // --- executors --------------------------------------------------------
    j.key("executors").beginArray();
    for (const Executor& x : e.executors) {
        const Sequence* seq = e.sequence(x.sequenceId);
        j.beginObject();
        j.member("id", x.id);
        j.member("label", x.label.empty() ? ("Exec " + std::to_string(x.id)) : x.label);
        j.member("sequence", x.sequenceId);
        j.member("sequenceName", seq ? seq->name : std::string());
        j.member("cues", seq ? static_cast<int>(seq->cues.size()) : 0);
        j.member("active", x.active());
        j.member("cue", x.active() ? x.currentIndex() + 1 : 0);
        j.member("master", x.master);
        j.member("priority", x.priority);

        // §9.2: a tile shows more as it gets bigger, so the surface needs the
        // detail available even when the current size does not display it.
        j.member("chase", seq ? seq->chase : false);
        j.member("rate", seq && seq->chase ? seq->chaseRateHz : 0.f);
        j.member("releasing", x.releasing());
        j.member("fade", x.active() ? x.fadeProgress(e.now()) : 0.0);
        const int cur = x.currentIndex();
        const bool haveCur = seq && cur >= 0 && cur < static_cast<int>(seq->cues.size());
        j.member("cueName", haveCur ? seq->cues[static_cast<size_t>(cur)].name
                                    : std::string());
        if (seq && !seq->cues.empty()) {
            const size_t next = static_cast<size_t>((cur + 1)) %
                                seq->cues.size();
            const Cue& n = seq->cues[next];
            j.member("nextCue", n.number);
            j.member("nextName", n.name);
            j.member("nextFade", n.timing.fadeIn);
        } else {
            j.key("nextCue").null();
            j.member("nextName", std::string());
            j.member("nextFade", 0.0);
        }
        j.endObject();
    }
    j.endArray();

    // --- executor surfaces (§9.6) ----------------------------------------
    // Both styles come from the same pages; what differs is how each page
    // says it wants to be laid out.
    j.member("surface", e.surfaces.current());
    j.key("surfaces").beginArray();
    for (const Surface& sf : e.surfaces.all()) {
        j.beginObject();
        j.member("id", sf.id);
        j.member("name", sf.name);
        j.member("style", surfaceStyleName(sf.style));
        j.member("cols", sf.cols);
        j.member("rows", sf.rows);
        j.member("editMode", sf.editMode);
        j.key("tiles").beginArray();
        for (const Tile& t : sf.tiles) {
            j.beginObject();
            j.member("kind", tileKindName(t.kind));
            j.member("slot", t.slot);
            j.member("x", t.rect.x);
            j.member("y", t.rect.y);
            j.member("w", t.rect.w);
            j.member("h", t.rect.h);
            bool sel = false;
            for (Uid u : e.surfaces.selection(sf.id))
                if (u == t.uid) sel = true;
            j.member("selected", sel);

            // The tile stores a UID, and the ENGINE resolves it to what that
            // object is called now and what a press should send (§9.2). The
            // interface draws the answer; it does not derive it, or a second
            // renderer would eventually derive it differently.
            const TileView v = resolveTile(e, t);
            j.member("legend", v.legend);
            j.member("detail", v.detail);
            j.member("symbol", v.symbol);
            j.member("colour", v.colour);
            j.member("hue", v.hue);
            j.member("command", v.command);
            j.member("altCommand", v.altCommand);
            j.member("live", v.live);
            j.member("level", v.level);
            j.member("levelCommand", v.levelCommand);
            j.member("missing", v.missing);
            j.member("unsupported", v.unsupported);
            j.endObject();
        }
        j.endArray();
        j.key("problems").beginArray();
        for (const std::string& w : e.surfaces.validate(sf.id)) j.value(w);
        j.endArray();
        j.endObject();
    }
    j.endArray();

    // --- sequences and their cues ----------------------------------------
    j.key("sequences").beginArray();
    for (const Sequence& seq : e.sequences) {
        j.beginObject();
        j.member("id", seq.id);
        j.member("name", seq.name);
        j.key("cues").beginArray();
        for (size_t i = 0; i < seq.cues.size(); ++i) {
            const Cue& c = seq.cues[i];
            j.beginObject();
            j.member("index", static_cast<int>(i) + 1);
            j.member("number", c.number);
            j.member("name", c.name);
            j.member("fadeIn", c.timing.fadeIn);
            j.member("values", c.allValues().size());
            j.endObject();
        }
        j.endArray();
        j.endObject();
    }
    j.endArray();

    // --- groups and presets ----------------------------------------------
    j.key("groups").beginArray();
    for (const GroupDef& g : e.groups.all()) {
        j.beginObject();
        j.member("id", g.id);
        j.member("name", g.name);
        j.member("members", e.groups.resolve(g.uid, e.patch).targets.size());
        j.member("dynamic", g.isDynamic());
        j.endObject();
    }
    j.endArray();

    j.key("presetPools").beginArray();
    for (const PresetPool& pool : e.presets.pools()) {
        j.beginObject();
        j.member("id", pool.id);
        j.member("name", pool.name);
        j.member("group", groupName(pool.group));
        j.key("presets").beginArray();
        for (const Preset& p : pool.presets) {
            j.beginObject();
            j.member("id", p.id);
            j.member("name", p.name);
            j.member("symbol", p.symbol());
            j.endObject();
        }
        j.endArray();
        j.endObject();
    }
    j.endArray();

    // --- DMX output, as it would leave the desk --------------------------
    j.key("universes").beginArray();
    if (s.dmx) {
        for (const SimulatedOutput::Frame& f : s.dmx->frames()) {
            j.beginObject();
            j.member("number", f.universe);
            j.member("frames", f.frames);
            const int top = std::max(1, s.dmx->highestChannel(f.universe));
            j.member("used", top);
            j.key("data").beginArray();
            for (int i = 0; i < top; ++i) j.value(static_cast<int>(f.data[static_cast<size_t>(i)]));
            j.endArray();
            j.endObject();
        }
    }
    j.endArray();

    // --- DMX input and its arbitration (§18.3) ---------------------------
    j.key("dmxIn").beginArray();
    for (int u : e.dmxIn.universes()) {
        const MergeReport m = e.dmxIn.report(u);
        j.beginObject();
        j.member("universe", u);
        j.member("priority", static_cast<int>(m.winningPriority));
        j.member("active", m.sourcesActive);
        j.member("seen", m.sourcesSeen);
        j.member("merged", m.merged);
        j.member("tie", tieBreakName(m.tie));
        j.endObject();
    }
    j.endArray();
    j.key("sources").beginArray();
    for (const SourceInfo& src : e.dmxIn.sources()) {
        j.beginObject();
        j.member("name", src.name.empty() ? src.cidText() : src.name);
        j.member("universe", src.universe);
        j.member("protocol", inputProtocolName(src.protocol));
        j.member("priority", static_cast<int>(src.priority));
        j.member("origin", src.origin);
        j.member("packets", src.packets);
        j.member("active", src.active);
        j.member("winning", src.winning);
        j.endObject();
    }
    j.endArray();

    // --- bindings ---------------------------------------------------------
    j.key("bindings").beginArray();
    for (const BindingSet& set : e.bindings.sets()) {
        for (const Binding& b : set.bindings) {
            j.beginObject();
            j.member("set", set.name);
            j.member("page", set.page);
            j.member("source", b.source.describe());
            j.member("target", b.target.describe());
            j.member("mode", bindModeName(b.map.mode));
            j.member("takeover", takeoverName(b.map.takeover));
            j.member("enabled", b.enabled);
            j.endObject();
        }
    }
    j.endArray();

    // --- timecode ---------------------------------------------------------
    j.key("timecode").beginArray();
    for (const TimecodeSlot& t : e.timecode.slots()) {
        j.beginObject();
        j.member("id", t.id);
        j.member("name", t.name);
        j.member("source", timecodeSourceName(t.source));
        j.member("rate", frameRateName(t.rate));
        j.member("position", t.at(e.now()).text(t.rate));
        j.member("running", t.running);
        j.member("stale", t.stale);
        j.endObject();
    }
    j.endArray();

    // --- laser safety (§14.6's status panel) ------------------------------
    j.key("laser").beginArray();
    for (const SafetyStatus& st : e.laser.statuses()) {
        j.beginObject();
        j.member("zone", st.zone);
        j.member("name", st.name);
        j.member("armed", st.armed);
        j.member("blanking", st.blanking);
        j.member("attenuation", st.attenuation);
        j.member("audience", st.audienceScanning);
        j.member("reason", st.reason);
        j.endObject();
    }
    j.endArray();
    j.member("emergencyStop", e.laser.emergencyStopped());

    // --- history and log --------------------------------------------------
    j.key("history").beginArray();
    {
        const int cursor = e.history.cursor();
        int i = 0;
        for (const HistoryEntry& h : e.history.entries()) {
            j.beginObject();
            j.member("id", h.id);
            j.member("description", h.description);
            j.member("scope", editScopeName(h.scope));
            j.member("source", h.source);
            j.member("undone", i >= cursor);
            j.endObject();
            ++i;
        }
    }
    j.endArray();
    j.member("canUndo", e.history.canUndo());
    j.member("canRedo", e.history.canRedo());

    // --- the command vocabulary, so the UI can offer it -------------------
    j.key("commands").beginArray();
    for (const CommandDef* d : s.console.registry().all()) {
        j.beginObject();
        j.member("name", d->name);
        j.member("help", d->help);
        j.member("privilege", privilegeName(d->privilege));
        j.endObject();
    }
    j.endArray();

    j.endObject();
    return j.str();
}

} // namespace

int main(int argc, char** argv) {
    int port = 7878;
    std::string openShow;
    bool demo = true;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--port" && i + 1 < argc) port = std::atoi(argv[++i]);
        else if (a == "--show" && i + 1 < argc) { openShow = argv[++i]; demo = false; }
        else if (a == "--empty") demo = false;
        else if (a == "--help") {
            std::printf(
                "lumen-server [--port N] [--show file.lumen] [--empty]\n"
                "  Serves the console UI on http://127.0.0.1:<port>\n");
            return 0;
        }
    }

    Session session;
    {
        auto out = std::make_unique<SimulatedOutput>();
        session.dmx = out.get();
        session.engine.addOutput(std::move(out));
    }

    if (!openShow.empty()) {
        const LoadReport r = ShowFile::load(session.engine, openShow);
        if (!r.ok) {
            std::fprintf(stderr, "could not open %s: %s\n", openShow.c_str(),
                         r.error.c_str());
            return 1;
        }
        session.showPath = openShow;
        std::printf("opened %s: %d fixtures, %d cues\n", openShow.c_str(), r.fixtures,
                    r.cues);
    } else if (demo) {
        buildDemoShow(session);
        session.showPath = "(demo rig)";
        std::printf("built the demo rig: %zu fixtures\n", session.engine.patch.size());
    }

    // Where the UI's files live. Next to the binary when installed, or in the
    // source tree when run from a build directory.
    fs::path webRoot;
    for (const fs::path& candidate :
         {fs::path("web"), fs::path("../web"), fs::path("../../web"),
          fs::path(LUMEN_WEB_ROOT)}) {
        if (fs::exists(candidate / "index.html")) {
            webRoot = candidate;
            break;
        }
    }
    if (webRoot.empty()) {
        std::fprintf(stderr, "cannot find the web UI (looked for web/index.html)\n");
        return 1;
    }

    HttpServer http;
    std::string err;
    if (!http.listen(port, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }

    // --- routes -----------------------------------------------------------
    http.route("GET", "/state", [&session](const HttpRequest&) {
        std::lock_guard<std::mutex> lock(session.lock);
        HttpResponse r;
        r.body = snapshot(session);
        return r;
    });

    http.route("POST", "/cmd", [&session](const HttpRequest& req) {
        // The ONLY way in. Every mutation the UI makes is a command string,
        // exactly as §16.2 requires — so anything the UI can do is also
        // typeable, scriptable and bindable.
        std::lock_guard<std::mutex> lock(session.lock);
        const std::string line = req.body;
        const Result res = session.console.executeAll(line);
        session.console.pumpBindings();

        Json j;
        j.beginObject();
        j.member("ok", res.ok);
        j.member("parsed", res.parsed);
        j.member("message", res.message);
        j.member("command", line);
        j.key("rows").beginArray();
        for (const std::string& row : res.rows) j.value(row);
        j.endArray();
        j.endObject();

        HttpResponse out;
        out.body = j.str();
        return out;
    });

    // Simulated input, so bindings and arbitration can be exercised with no
    // hardware present.
    http.route("POST", "/input", [&session](const HttpRequest& req) {
        std::lock_guard<std::mutex> lock(session.lock);
        const std::string kind = queryParam(req.query, "kind");
        const int index = std::atoi(queryParam(req.query, "index").c_str());
        const float value =
            static_cast<float>(std::atof(queryParam(req.query, "value").c_str()));

        InputEvent e;
        if (kind == "note") e = SimulatedInput::midiNote(index, value > 0.5f);
        else if (kind == "cc") e = SimulatedInput::midiCC(index, value);
        else if (kind == "osc") e = SimulatedInput::osc(queryParam(req.query, "address"), value);
        else if (kind == "key") e = SimulatedInput::key(index);
        else {
            HttpResponse bad;
            bad.status = 400;
            bad.body = "{\"ok\":false,\"message\":\"unknown input kind\"}";
            return bad;
        }
        session.engine.inputs.push(e);

        HttpResponse out;
        out.body = "{\"ok\":true}";
        return out;
    });

    http.fallback([&webRoot](const HttpRequest& req) {
        HttpResponse r;
        std::string rel = req.path == "/" ? "/index.html" : req.path;
        // No traversal out of the web root: this is a local UI, but a path
        // that escapes its root is a bug whatever the context.
        if (rel.find("..") != std::string::npos) {
            r.status = 400;
            r.contentType = "text/plain";
            r.body = "bad path";
            return r;
        }
        const fs::path file = webRoot / rel.substr(1);
        const std::string body = readFile(file);
        if (body.empty() && !fs::exists(file)) {
            r.status = 404;
            r.contentType = "text/plain";
            r.body = "not found: " + rel;
            return r;
        }
        r.contentType = mimeFor(rel);
        r.body = body;
        return r;
    });

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    // The output thread is the real one from §15.2: it reads published frames
    // from the triple buffer and never takes the model lock, so a slow HTTP
    // request cannot stall the DMX stream. Here it feeds the simulation
    // instead of a wire, which is the only difference.
    session.engine.startOutput(40.0);

    // The engine ticks on its own thread at a console-ish rate. The HTTP
    // thread takes the same lock, so the two never touch the model at once.
    std::thread ticker([&session] {
        while (gRunning) {
            {
                std::lock_guard<std::mutex> lock(session.lock);
                session.engine.tick();
                session.console.pumpBindings();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    });

    std::printf("\n  Lumen is running.\n\n    http://127.0.0.1:%d\n\n", http.port());
    std::printf("  Simulated output: no hardware required.\n");
    std::printf("  Ctrl-C to stop.\n\n");
    std::fflush(stdout);

    while (gRunning) http.poll(100);

    gRunning = false;
    ticker.join();
    session.engine.stopOutput();
    std::printf("stopped after %llu requests\n",
                static_cast<unsigned long long>(http.served()));
    return 0;
}
