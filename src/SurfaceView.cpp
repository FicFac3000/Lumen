#include "lumen/SurfaceView.hpp"

#include <cstdio>

namespace lumen {

namespace {

std::string trimZero(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", v);
    std::string s = buf;
    if (s.size() > 2 && s.compare(s.size() - 2, 2, ".0") == 0) s.resize(s.size() - 2);
    return s;
}

// §9.2: colour derives from the object's main attribute group — or, for a
// colour preset, from the colour it actually produces. A palette of swatches
// that all render identically is a palette you have to read instead of see.
std::string swatchFor(const Engine& eng, const Preset& p) {
    const std::optional<float> r = eng.presets.resolve(p.uid, Attr::Red, 0, 0);
    const std::optional<float> g = eng.presets.resolve(p.uid, Attr::Green, 0, 0);
    const std::optional<float> b = eng.presets.resolve(p.uid, Attr::Blue, 0, 0);
    if (!r && !g && !b) return {};
    auto byte = [](const std::optional<float>& v) {
        return static_cast<int>((v ? *v : 0.f) * 255.f + 0.5f);
    };
    char buf[16];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", byte(r), byte(g), byte(b));
    return buf;
}

const char* hueForGroup(Group g) {
    switch (g) {
        case Group::Intensity: return "intensity";
        case Group::Position: return "position";
        case Group::Color: return "colour";
        case Group::Beam: return "beam";
        case Group::Control: return "control";
        case Group::Count: break;
    }
    return "default";
}

} // namespace

TileView resolveTile(const Engine& eng, const Tile& tile) {
    TileView v;
    v.symbol = tile.symbolOverride;
    v.colour = tile.colourOverride;
    v.hue = "default";

    switch (tile.kind) {
        case TileKind::Executor: {
            const Executor* x = nullptr;
            for (const Executor& e : eng.executors)
                if (e.uid == tile.target) x = &e;
            if (!x) {
                v.missing = true;
                v.legend = "missing executor";
                break;
            }
            const Sequence* seq = eng.sequence(x->sequenceId);
            v.legend = x->label.empty() ? ("Exec " + std::to_string(x->id)) : x->label;
            const int cues = seq ? static_cast<int>(seq->cues.size()) : 0;
            if (seq && seq->chase) {
                v.hue = "chase";
                v.detail = trimZero(seq->chaseRateHz) + " Hz";
                if (v.symbol.empty()) v.symbol = "⟳";
            } else {
                v.detail = x->active()
                               ? ("cue " + std::to_string(x->currentIndex() + 1) + "/" +
                                  std::to_string(cues))
                               : (std::to_string(cues) +
                                  (cues == 1 ? " cue" : " cues"));
                if (v.symbol.empty()) v.symbol = "▶";
            }
            v.live = x->active();
            v.level = x->master;
            v.command = "Go " + std::to_string(x->id);
            v.altCommand = "Off " + std::to_string(x->id);
            v.levelCommand = "ExecMaster " + std::to_string(x->id) + " %d";
            break;
        }

        case TileKind::Group: {
            const GroupDef* g = nullptr;
            for (const GroupDef& e : eng.groups.all())
                if (e.uid == tile.target) g = &e;
            if (!g) {
                v.missing = true;
                v.legend = "missing group";
                break;
            }
            v.legend = g->name;
            const GroupResolution res = eng.groups.resolve(g->uid, eng.patch);
            const size_t n = res.targets.size();
            v.detail = std::to_string(n) + (n == 1 ? " fixture" : " fixtures");
            if (v.symbol.empty()) v.symbol = "⬡";
            v.hue = "group";
            v.command = "Group " + std::to_string(g->id);
            // Selection is not a level, and a group tile with a fader would
            // imply it were one.
            break;
        }

        case TileKind::Preset: {
            const Preset* p = eng.presets.find(tile.target);
            if (!p) {
                v.missing = true;
                v.legend = "missing preset";
                break;
            }
            v.legend = p->name;
            const PresetPool* pool = eng.presets.pool(p->pool);
            v.detail = pool ? pool->name : std::string();
            if (v.symbol.empty()) v.symbol = p->symbol();
            v.hue = hueForGroup(p->group);
            if (v.colour.empty() && p->group == Group::Color)
                v.colour = swatchFor(eng, *p);
            v.command = "Preset " + std::to_string(p->pool) + "." + std::to_string(p->id);
            break;
        }

        case TileKind::Recipe: {
            const Recipe* r = eng.recipes.find(tile.target);
            if (!r) {
                v.missing = true;
                v.legend = "missing recipe";
                break;
            }
            v.legend = r->name;
            v.detail = std::to_string(r->slots.size()) +
                       (r->slots.size() == 1 ? " slot" : " slots");
            if (v.symbol.empty()) v.symbol = "∿";
            v.hue = "recipe";
            // Recipes are applied to a selection, and there is no command for
            // that yet — §16.2.1's pattern again, and saying so beats a tile
            // that looks pressable and is not.
            v.unsupported = true;
            break;
        }

        case TileKind::Timecode: {
            const TimecodeSlot* t = nullptr;
            for (const TimecodeSlot& e : eng.timecode.slots())
                if (e.uid == tile.target) t = &e;
            if (!t) {
                v.missing = true;
                v.legend = "missing timecode";
                break;
            }
            v.legend = t->name;
            v.detail = t->position.text(t->rate);
            if (v.symbol.empty()) v.symbol = "◷";
            v.hue = "control";
            v.live = t->running;
            v.command = "Timecode " + std::to_string(t->id) +
                        (t->running ? " stop" : " start");
            break;
        }

        case TileKind::GrandMaster:
            v.legend = tile.legendOverride.empty() ? "Grand" : tile.legendOverride;
            v.detail = std::to_string(static_cast<int>(eng.grandMaster * 100 + 0.5f)) + "%";
            if (v.symbol.empty()) v.symbol = "≡";
            v.hue = "intensity";
            v.level = eng.grandMaster;
            v.levelCommand = "GrandMaster %d";
            // Deliberately no press command: a grand master that could be
            // changed by a tap rather than a deliberate drag is a blackout
            // waiting to happen.
            break;

        case TileKind::Blackout:
            v.legend = tile.legendOverride.empty() ? "Blackout" : tile.legendOverride;
            v.detail = eng.blackout ? "ON" : "";
            if (v.symbol.empty()) v.symbol = "●";
            v.hue = "warn";
            v.live = eng.blackout;
            v.command = std::string("Blackout ") + (eng.blackout ? "off" : "on");
            break;

        case TileKind::LaserZone: {
            const ZoneSafety* z = nullptr;
            for (const ZoneSafety& e : eng.laser.zones())
                if (e.uid == tile.target) z = &e;
            if (!z) {
                v.missing = true;
                v.legend = "missing zone";
                break;
            }
            const SafetyStatus st = eng.laser.status(z->id);
            v.legend = z->name;
            v.detail = st.reason;
            v.hue = st.armed ? "armed" : "warn";
            v.live = st.armed;
            if (v.symbol.empty()) v.symbol = "\u26a0";

            // §14.6: a laser tile SHOWS state and can DISARM. It cannot arm —
            // arming is hold-to-fire from a physical act, and a tile that
            // armed on a tap would be exactly the single-press path the
            // whole design exists to prevent. Disarming, by contrast, should
            // be as easy as possible, so it is a plain press.
            if (st.armed)
                v.command = "Disarm " + std::to_string(z->id);
            break;
        }

        case TileKind::Macro:
            v.legend = tile.legendOverride.empty() ? "Macro" : tile.legendOverride;
            v.unsupported = true;   // no macro object exists yet
            break;

        case TileKind::Label:
            // Pure layout: no action, no target, nothing to resolve. A label
            // with no text is still a label, not a broken tile.
            v.legend = tile.legendOverride;
            v.hue = "label";
            break;

        case TileKind::Empty:
        case TileKind::Count:
            break;
    }

    // The override always wins, and is applied last so it beats whatever the
    // object implied (§9.2).
    if (!tile.legendOverride.empty() && tile.kind != TileKind::Label)
        v.legend = tile.legendOverride;
    return v;
}

} // namespace lumen
