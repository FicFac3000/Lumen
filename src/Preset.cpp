#include "lumen/Preset.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>

namespace lumen {

namespace {

std::string trimNum(double v) {
    std::ostringstream o;
    if (std::fabs(v - std::round(v)) < 1e-6) o << static_cast<long long>(std::round(v));
    else {
        o.setf(std::ios::fixed);
        o.precision(1);
        o << v;
    }
    return o.str();
}

// A physical unit label for the common cases, so a generated name reads as a
// real-world value rather than a bare number.
const char* unitFor(Attr a) {
    if (has(kindOf(a), Kind::Anchored) &&
        (a == Attr::Prism1Spin || a == Attr::Gobo1Spin || a == Attr::Gobo2Spin ||
         a == Attr::ColorWheel1Spin || a == Attr::AnimWheelSpin || a == Attr::PanRotate ||
         a == Attr::TiltRotate || a == Attr::ShaperRot))
        return "rpm";
    if (a == Attr::Pan || a == Attr::Tilt || a == Attr::Gobo1Pos || a == Attr::Gobo2Pos ||
        a == Attr::Prism1Pos || a == Attr::AnimWheelPos || a == Attr::RotX ||
        a == Attr::RotY || a == Attr::RotZ)
        return "°";
    if (a == Attr::StrobeRate) return "Hz";
    return "";
}

std::string dirGlyph(float physical) {
    if (physical > 1e-6f) return "↻";   // clockwise
    if (physical < -1e-6f) return "↺";  // counter-clockwise
    return "■";                          // stopped
}

} // namespace

// ---------------------------------------------------------------------------
// Preset
// ---------------------------------------------------------------------------
std::string Preset::symbol() const {
    if (!symbolOverride.empty()) return symbolOverride;
    const auto attrs = attributes();
    if (attrs.empty()) return "?";
    return attrInfo(attrs.front()).symbol;
}

std::string Preset::indicator() const {
    if (!indicatorOverride.empty()) return indicatorOverride;
    const auto attrs = attributes();
    if (attrs.size() > 1) return std::to_string(attrs.size()) + " attrs";
    if (attrs.empty()) return {};
    // A single-attribute preset shows its value, which is the useful case.
    for (const auto& [key, val] : values.values()) {
        (void)key;
        return trimNum(static_cast<double>(val.v) * 100.0) + "%";
    }
    return {};
}

std::vector<Attr> Preset::attributes() const {
    std::set<uint16_t> seen;
    for (const auto& [key, val] : values.values()) {
        (void)val;
        seen.insert(static_cast<uint16_t>(keyAttr(key)));
    }
    std::vector<Attr> out;
    out.reserve(seen.size());
    for (uint16_t a : seen) out.push_back(static_cast<Attr>(a));
    return out;
}

bool Preset::hasAttr(Attr a) const {
    for (const auto& [key, val] : values.values()) {
        (void)val;
        if (keyAttr(key) == a) return true;
    }
    return false;
}

bool Preset::isAnchoredKind() const {
    for (Attr a : attributes())
        if (isAnchored(a)) return true;
    return false;
}

bool Preset::isShaper() const {
    for (Attr a : attributes()) {
        const std::string n = attrName(a);
        if (n.rfind("Shaper", 0) == 0) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// PresetPool
// ---------------------------------------------------------------------------
const Preset* PresetPool::byName(const std::string& n) const {
    for (const auto& p : presets)
        if (p.name == n) return &p;
    return nullptr;
}

const Preset* PresetPool::byId(int presetId) const {
    for (const auto& p : presets)
        if (p.id == presetId) return &p;
    return nullptr;
}

Preset* PresetPool::byId(int presetId) {
    for (auto& p : presets)
        if (p.id == presetId) return &p;
    return nullptr;
}

std::string PresetPool::uniqueName(const std::string& wanted) const {
    if (!byName(wanted)) return wanted;
    // Auto-suffix rather than reject, so a bulk generation never fails on a
    // name clash (§8.2).
    for (int n = 2; n < 10000; ++n) {
        const std::string candidate = wanted + " (" + std::to_string(n) + ")";
        if (!byName(candidate)) return candidate;
    }
    return wanted;
}

// ---------------------------------------------------------------------------
// PresetStore
// ---------------------------------------------------------------------------
PresetPool& PresetStore::ensurePool(int id, const std::string& name, Group g) {
    if (PresetPool* p = pool(id)) return *p;
    PresetPool p;
    p.id = id;
    p.name = name;
    p.group = g;
    pools_.push_back(std::move(p));
    return pools_.back();
}

PresetPool* PresetStore::pool(int id) {
    for (auto& p : pools_)
        if (p.id == id) return &p;
    return nullptr;
}

const PresetPool* PresetStore::pool(int id) const {
    for (const auto& p : pools_)
        if (p.id == id) return &p;
    return nullptr;
}

PresetPool* PresetStore::poolByName(const std::string& name) {
    for (auto& p : pools_)
        if (p.name == name) return &p;
    return nullptr;
}

Preset& PresetStore::add(int poolId, Preset p) {
    PresetPool* existing = pool(poolId);
    PresetPool& target = existing
                             ? *existing
                             : ensurePool(poolId, "Pool " + std::to_string(poolId), p.group);
    if (!p.uid.valid()) p.uid = uids_->next();
    p.pool = poolId;
    p.name = target.uniqueName(p.name.empty() ? "Preset" : p.name);
    if (p.id == 0) {
        int maxId = 0;
        for (const auto& e : target.presets) maxId = std::max(maxId, e.id);
        p.id = maxId + 1;
    }
    target.presets.push_back(std::move(p));
    return target.presets.back();
}

Preset& PresetStore::restore(Preset p) {
    PresetPool* existing = pool(p.pool);
    PresetPool& target =
        existing ? *existing
                 : ensurePool(p.pool, "Pool " + std::to_string(p.pool), p.group);
    if (!p.uid.valid()) p.uid = uids_->next();
    uids_->observe(p.uid);
    target.presets.push_back(std::move(p));
    return target.presets.back();
}

const Preset* PresetStore::find(Uid uid) const {
    for (const auto& pl : pools_)
        for (const auto& p : pl.presets)
            if (p.uid == uid) return &p;
    return nullptr;
}

Preset* PresetStore::findMut(Uid uid) {
    for (auto& pl : pools_)
        for (auto& p : pl.presets)
            if (p.uid == uid) return &p;
    return nullptr;
}

PresetStore::NameLookup PresetStore::findByName(const std::string& name,
                                                std::optional<int> poolContext) const {
    NameLookup out;

    // The current pool narrows a bare name first — typing inside a pool window
    // resolves against that pool before searching others (§8.2).
    if (poolContext) {
        if (const PresetPool* p = pool(*poolContext)) {
            if (const Preset* hit = p->byName(name)) {
                out.preset = hit;
                return out;
            }
        }
    }

    std::vector<const Preset*> hits;
    for (const auto& pl : pools_)
        if (const Preset* hit = pl.byName(name)) hits.push_back(hit);

    if (hits.size() == 1) {
        out.preset = hits.front();
    } else if (hits.size() > 1) {
        // Report the candidates rather than guessing. Same-name presets in
        // different pools are expected, so this must read as a prompt.
        for (const Preset* h : hits) out.ambiguousPools.push_back(h->pool);
    }
    return out;
}

std::optional<float> PresetStore::resolve(Uid preset, Attr a, FixtureId f,
                                          CellId cell) const {
    const Preset* p = find(preset);
    if (!p) return std::nullopt;

    // Exact fixture and cell first.
    if (const auto v = p->values.value(f, cell, a)) return v;
    // Then the fixture with cell 0, for a preset stored against the parent.
    if (cell != 0) {
        if (const auto v = p->values.value(f, 0, a)) return v;
    }
    // Then the wildcard entry, which is what makes a generic palette apply to
    // any fixture that has the attribute.
    if (const auto v = p->values.value(0, 0, a)) return v;
    return std::nullopt;
}

void PresetStore::resolveLayer(ValueLayer& layer) const {
    for (const auto& [key, val] : layer.values()) {
        if (!val.isRef()) continue;
        const auto fresh = resolve(val.preset, val.presetAttr, keyFixture(key), keyCell(key));
        if (!fresh) continue;  // a dangling reference keeps its last value
        Value updated = val;
        updated.v = *fresh;
        layer.setKey(key, updated);
    }
}

size_t PresetStore::size() const {
    size_t n = 0;
    for (const auto& p : pools_) n += p.presets.size();
    return n;
}

// ---------------------------------------------------------------------------
// Auto-stepper
// ---------------------------------------------------------------------------
const char* stepModeName(StepMode m) {
    switch (m) {
        case StepMode::Dmx: return "Dmx";
        case StepMode::Physical: return "Physical";
        case StepMode::PerSet: return "PerSet";
        case StepMode::Symmetric: return "Symmetric";
    }
    return "?";
}

std::string expandTemplate(const std::string& tmpl, const GeneratedStep& s, Attr a,
                           const std::string& fixtureType) {
    std::string out;
    size_t i = 0;
    while (i < tmpl.size()) {
        if (tmpl[i] != '{') {
            out += tmpl[i++];
            continue;
        }
        const size_t end = tmpl.find('}', i);
        if (end == std::string::npos) {
            out += tmpl.substr(i);
            break;
        }
        const std::string tok = tmpl.substr(i + 1, end - i - 1);
        if (tok == "attr") out += attrInfo(a).pretty;
        else if (tok == "set") out += s.setName;
        else if (tok == "n") out += std::to_string(s.indexInSide + 1);
        else if (tok == "N") out += std::to_string(s.sideCount);
        else if (tok == "phys") out += trimNum(static_cast<double>(s.physical));
        else if (tok == "unit") out += unitFor(a);
        else if (tok == "dir") out += dirGlyph(s.physical);
        else if (tok == "dmx") out += std::to_string(static_cast<int>(s.dmx));
        else if (tok == "fixtype") out += fixtureType;
        i = end + 1;
    }

    // Tidy the double spaces a skipped token leaves behind.
    std::string tidy;
    bool prevSpace = false;
    for (char c : out) {
        const bool sp = (c == ' ');
        if (sp && prevSpace) continue;
        tidy += c;
        prevSpace = sp;
    }
    while (!tidy.empty() && tidy.back() == ' ') tidy.pop_back();
    size_t start = 0;
    while (start < tidy.size() && tidy[start] == ' ') ++start;
    return tidy.substr(start);
}

namespace {

float physAt(const ChannelSet& s, uint8_t dmx) {
    if (s.dmxTo <= s.dmxFrom) return s.physFrom;
    const float t = static_cast<float>(dmx - s.dmxFrom) /
                    static_cast<float>(s.dmxTo - s.dmxFrom);
    return s.physFrom + (s.physTo - s.physFrom) * t;
}

const ChannelSet* setForDmx(const std::vector<ChannelSet>& sets, uint8_t dmx) {
    for (const auto& s : sets)
        if (dmx >= s.dmxFrom && dmx <= s.dmxTo) return &s;
    return sets.empty() ? nullptr : &sets.front();
}

GeneratedStep makeStep(const std::vector<ChannelSet>& sets, int dmx, int idx, int count,
                       int side) {
    GeneratedStep g;
    g.dmx = static_cast<uint8_t>(std::clamp(dmx, 0, 255));
    g.normalised = static_cast<float>(g.dmx) / 255.f;
    g.indexInSide = idx;
    g.sideCount = count;
    g.side = side;
    if (const ChannelSet* s = setForDmx(sets, g.dmx)) {
        g.setName = s->name;
        g.physical = physAt(*s, g.dmx);
        g.isAnchor = s->anchor && s->dmxFrom == g.dmx;
    }
    return g;
}

} // namespace

std::vector<GeneratedStep> planSteps(const FixtureType& type, const StepRequest& req) {
    std::vector<GeneratedStep> out;
    const auto sets = type.setsFor(req.attr, req.cell);
    const int n = std::max(1, req.steps);

    // Find an anchor: a set flagged as one, or one whose physical value is 0.
    int anchorIdx = -1;
    for (size_t i = 0; i < sets.size(); ++i) {
        const auto& s = sets[i];
        if (s.anchor || (std::fabs(s.physFrom) < 1e-6f && std::fabs(s.physTo) < 1e-6f)) {
            anchorIdx = static_cast<int>(i);
            break;
        }
    }

    StepMode mode = req.mode;
    // §7.3 default: PerSet, with Symmetric applied automatically where an
    // anchor is found. Dividing an anchored range into equal steps would
    // straddle the stop point.
    //
    // But NOT for an `indexed` attribute. A gobo wheel's "Open" slot is a
    // semantic zero and so looks like an anchor, yet symmetric stepping there
    // produces presets that land between gobo slots instead of on them. An
    // indexed attribute always wants one preset per slot.
    const bool indexedAttr = has(kindOf(req.attr), Kind::Indexed);
    if (req.autoSymmetric && anchorIdx >= 0 && mode == StepMode::PerSet && !indexedAttr)
        mode = StepMode::Symmetric;

    if (sets.empty()) {
        // No range labels at all: fall back to an even spread of the channel.
        for (int i = 0; i < n; ++i) {
            const int dmx = n == 1 ? 128 : (i * 255) / (n - 1);
            GeneratedStep g = makeStep(sets, dmx, i, n, 0);
            g.setName.clear();
            out.push_back(g);
        }
        return out;
    }

    switch (mode) {
        case StepMode::Dmx: {
            for (int i = 0; i < n; ++i) {
                const int dmx = n == 1 ? 128 : (i * 255) / (n - 1);
                out.push_back(makeStep(sets, dmx, i, n, 0));
            }
            break;
        }

        case StepMode::Physical: {
            // Equal steps in PHYSICAL space, which is not the same as equal
            // DMX when the profile maps non-linearly.
            float lo = sets.front().physFrom, hi = sets.front().physTo;
            for (const auto& s : sets) {
                lo = std::min({lo, s.physFrom, s.physTo});
                hi = std::max({hi, s.physFrom, s.physTo});
            }
            for (int i = 0; i < n; ++i) {
                const float want = n == 1 ? (lo + hi) * 0.5f
                                          : lo + (hi - lo) * static_cast<float>(i) /
                                                     static_cast<float>(n - 1);
                // Walk the channel for the closest DMX value to that physical.
                int best = 0;
                float bestErr = 1e30f;
                for (int d = 0; d <= 255; ++d) {
                    if (const ChannelSet* s = setForDmx(sets, static_cast<uint8_t>(d))) {
                        const float err = std::fabs(physAt(*s, static_cast<uint8_t>(d)) - want);
                        if (err < bestErr) {
                            bestErr = err;
                            best = d;
                        }
                    }
                }
                out.push_back(makeStep(sets, best, i, n, 0));
            }
            break;
        }

        case StepMode::PerSet: {
            // N steps inside each named range — what a gobo wheel or a prism
            // facet list wants.
            for (const auto& s : sets) {
                const int span = s.dmxTo - s.dmxFrom;
                const bool indexed = span <= 0 || has(kindOf(req.attr), Kind::Indexed);
                const int count = indexed ? 1 : n;
                for (int i = 0; i < count; ++i) {
                    int dmx;
                    if (count == 1) {
                        // An indexed slot lands in the MIDDLE of its range, so
                        // it is robust to a profile being slightly off.
                        dmx = s.dmxFrom + span / 2;
                    } else {
                        dmx = s.dmxFrom + (span * i) / (count - 1);
                    }
                    out.push_back(makeStep(sets, dmx, i, count, 0));
                }
            }
            break;
        }

        case StepMode::Symmetric: {
            if (anchorIdx < 0) {
                StepRequest alt = req;
                alt.mode = StepMode::PerSet;
                alt.autoSymmetric = false;
                return planSteps(type, alt);
            }
            const ChannelSet& anchor = sets[static_cast<size_t>(anchorIdx)];

            // The anchor keeps a preset of its own — the stop point is a real
            // thing an operator wants to hit.
            if (req.keepAnchor) {
                GeneratedStep g = makeStep(sets, anchor.dmxFrom + (anchor.dmxTo -
                                                                   anchor.dmxFrom) / 2,
                                           0, 1, 0);
                g.isAnchor = true;
                g.setName = anchor.name;
                out.push_back(g);
            }

            // Below the anchor: step OUTWARD from it, so step 1 is the slowest
            // and step N the fastest. That ordering is what makes the generated
            // names mean something.
            if (anchorIdx > 0) {
                const int farEdge = sets.front().dmxFrom;
                const int nearEdge = anchor.dmxFrom - 1;
                for (int i = 0; i < n; ++i) {
                    const float t = n == 1 ? 0.5f
                                           : static_cast<float>(i) / static_cast<float>(n - 1);
                    const int dmx = nearEdge - static_cast<int>(
                                                   std::round(t * (nearEdge - farEdge)));
                    out.push_back(makeStep(sets, dmx, i, n, -1));
                }
            }

            // Above the anchor, same outward ordering.
            if (anchorIdx < static_cast<int>(sets.size()) - 1) {
                const int nearEdge = anchor.dmxTo + 1;
                const int farEdge = sets.back().dmxTo;
                for (int i = 0; i < n; ++i) {
                    const float t = n == 1 ? 0.5f
                                           : static_cast<float>(i) / static_cast<float>(n - 1);
                    const int dmx = nearEdge + static_cast<int>(
                                                   std::round(t * (farEdge - nearEdge)));
                    out.push_back(makeStep(sets, dmx, i, n, +1));
                }
            }
            break;
        }
    }
    return out;
}

GenerateReport generatePresets(PresetStore& store, int poolId, const Patch& patch,
                               const FixtureType& type, const std::vector<Target>& targets,
                               const StepRequest& req) {
    GenerateReport rep;
    (void)patch;

    const auto steps = planSteps(type, req);
    if (steps.empty()) {
        rep.note = "no steps planned; the type has no range for that attribute";
        return rep;
    }

    for (const GeneratedStep& s : steps) {
        Preset p;
        p.group = groupOf(req.attr);
        p.generated = true;
        p.name = expandTemplate(req.nameTemplate, s, req.attr, type.name);
        p.tags.push_back("generated");
        if (s.isAnchor) p.tags.push_back("anchor");

        if (targets.empty()) {
            // A generic preset: wildcard fixture, so it applies to anything
            // with the attribute.
            p.values.set(0, 0, req.attr, Value::hard(s.normalised));
        } else {
            for (const Target& t : targets)
                p.values.set(t.fixture, t.cell, req.attr, Value::hard(s.normalised));
        }

        store.add(poolId, std::move(p));
        ++rep.created;
        rep.names.push_back(store.pool(poolId)->presets.back().name);
    }

    rep.note = std::string("mode ") + stepModeName(req.mode);
    return rep;
}

} // namespace lumen
