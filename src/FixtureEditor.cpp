#include "lumen/FixtureEditor.hpp"

#include <algorithm>
#include <functional>
#include <set>

namespace lumen {

const char* geometryKindName(GeometryKind k) {
    switch (k) {
        case GeometryKind::Normal: return "Normal";
        case GeometryKind::Axis: return "Axis";
        case GeometryKind::Beam: return "Beam";
        case GeometryKind::Reference: return "Reference";
    }
    return "Normal";
}

const char* issueLevelName(Issue::Level l) {
    switch (l) {
        case Issue::Level::Error: return "error";
        case Issue::Level::Warning: return "warning";
        case Issue::Level::Info: return "info";
    }
    return "error";
}

namespace {

// Depth-first walk over every node in the tree, parent before children.
void walkNodes(const std::vector<GeometryNode>& nodes,
               const std::function<void(const GeometryNode&, int)>& fn, int depth = 0) {
    for (const GeometryNode& n : nodes) {
        fn(n, depth);
        walkNodes(n.children, fn, depth + 1);
    }
}

const GeometryNode* findIn(const std::vector<GeometryNode>& nodes,
                           const std::string& name) {
    for (const GeometryNode& n : nodes) {
        if (n.name == name) return &n;
        if (const GeometryNode* hit = findIn(n.children, name)) return hit;
    }
    return nullptr;
}

GeometryNode* findInMut(std::vector<GeometryNode>& nodes, const std::string& name) {
    for (GeometryNode& n : nodes) {
        if (n.name == name) return &n;
        if (GeometryNode* hit = findInMut(n.children, name)) return hit;
    }
    return nullptr;
}

// References pointing at `target`, in document order. Document order is what
// makes cell numbering stable and predictable.
std::vector<const GeometryNode*> referencesTo(const FixtureDef& def,
                                              const std::string& target) {
    std::vector<const GeometryNode*> out;
    walkNodes(def.geometries, [&](const GeometryNode& n, int) {
        if (n.isReference() && n.referenced == target) out.push_back(&n);
    });
    return out;
}

// Every reference in the definition, in document order. The index into this
// list plus one is the cell id, so a reference's cell does not depend on which
// geometry it points at.
std::vector<const GeometryNode*> allReferences(const FixtureDef& def) {
    std::vector<const GeometryNode*> out;
    walkNodes(def.geometries, [&](const GeometryNode& n, int) {
        if (n.isReference()) out.push_back(&n);
    });
    return out;
}

CellId cellOfReference(const FixtureDef& def, const GeometryNode* ref) {
    const auto all = allReferences(def);
    for (size_t i = 0; i < all.size(); ++i)
        if (all[i] == ref) return static_cast<CellId>(i + 1);
    return 0;
}

// A reference chain that loops would hang the expansion, so it is detected
// rather than discovered as a freeze.
bool referenceCycle(const FixtureDef& def, const std::string& start) {
    std::set<std::string> seen;
    std::string at = start;
    while (!at.empty()) {
        if (!seen.insert(at).second) return true;
        const GeometryNode* n = def.findGeometry(at);
        if (!n || !n->isReference()) return false;
        at = n->referenced;
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// FixtureDef
// ---------------------------------------------------------------------------
std::string FixtureDef::typeName(size_t modeIndex) const {
    if (modeIndex >= modes.size()) return name;
    const std::string& m = modes[modeIndex].name;
    if (name.empty()) return m;
    return m.empty() ? name : name + " / " + m;
}

const ModeDef* FixtureDef::mode(const std::string& modeName) const {
    for (const ModeDef& m : modes)
        if (m.name == modeName) return &m;
    return nullptr;
}

int FixtureDef::modeIndex(const std::string& modeName) const {
    for (size_t i = 0; i < modes.size(); ++i)
        if (modes[i].name == modeName) return static_cast<int>(i);
    return -1;
}

const GeometryNode* FixtureDef::findGeometry(const std::string& geoName) const {
    return findIn(geometries, geoName);
}

GeometryNode* FixtureDef::findGeometry(const std::string& geoName) {
    return findInMut(geometries, geoName);
}

// ---------------------------------------------------------------------------
// Instances
// ---------------------------------------------------------------------------
std::vector<GeometryInstance> instancesOf(const FixtureDef& def,
                                          const std::string& geoName) {
    std::vector<GeometryInstance> out;

    // The mode root, or a channel with no geometry at all: one instance, no
    // cell. A simple fixture never needs to name a geometry.
    if (geoName.empty()) {
        out.push_back({std::string(), 0, 0});
        return out;
    }

    const GeometryNode* node = def.findGeometry(geoName);
    if (!node) return out;  // unresolved; validate() reports it

    // A channel declared directly against a reference belongs to that one
    // instance.
    if (node->isReference()) {
        out.push_back({node->name, cellOfReference(def, node), node->dmxOffset});
        return out;
    }

    // A shared geometry: one instance per reference pointing at it. This is
    // §1.3's "one cell definition instantiates N times".
    const auto refs = referencesTo(def, geoName);
    if (refs.empty()) {
        // Nothing instantiates it, so it is a plain body geometry.
        out.push_back({node->name, 0, 0});
        return out;
    }
    for (const GeometryNode* r : refs)
        out.push_back({r->name, cellOfReference(def, r), r->dmxOffset});
    return out;
}

// ---------------------------------------------------------------------------
// Compile
// ---------------------------------------------------------------------------
CompileReport compile(const FixtureDef& def, size_t modeIndex) {
    CompileReport rep;
    if (modeIndex >= def.modes.size()) {
        rep.errors.push_back("no mode at index " + std::to_string(modeIndex));
        return rep;
    }
    const ModeDef& mode = def.modes[modeIndex];

    rep.type.uid = def.uid;
    rep.type.name = def.typeName(modeIndex);

    CellId maxCell = 0;
    for (const ChannelDecl& c : mode.channels) {
        if (c.coarse < 0) {
            rep.errors.push_back("channel " + std::string(attrName(c.attr)) +
                                 " has a negative offset");
            continue;
        }
        if (!c.geometry.empty() && referenceCycle(def, c.geometry)) {
            rep.errors.push_back("geometry '" + c.geometry + "' references itself");
            continue;
        }

        const auto instances = instancesOf(def, c.geometry);
        if (instances.empty()) {
            rep.errors.push_back("channel " + std::string(attrName(c.attr)) +
                                 " names unknown geometry '" + c.geometry + "'");
            continue;
        }
        if (instances.size() > 1) ++rep.declarationsExpanded;

        for (const GeometryInstance& in : instances) {
            ChannelDef out;
            out.attr = c.attr;
            out.cell = in.cell;
            out.coarse = c.coarse + in.dmxOffset;
            out.fine = c.fine < 0 ? -1 : c.fine + in.dmxOffset;
            out.defaultValue = c.defaultValue;
            out.invert = c.invert;
            rep.type.channels.push_back(out);
            ++rep.channelsEmitted;
            maxCell = std::max(maxCell, in.cell);

            for (const ChannelSetDef& s : c.sets) {
                ChannelSet cs;
                cs.attr = c.attr;
                cs.cell = in.cell;
                cs.name = s.name;
                cs.dmxFrom = s.dmxFrom;
                cs.dmxTo = s.dmxTo;
                cs.physFrom = s.physFrom;
                cs.physTo = s.physTo;
                cs.anchor = s.anchor;
                rep.type.sets.push_back(cs);
            }
        }
    }

    rep.type.cellCount = maxCell;

    // Overlaps are an error, not a warning: two attributes sharing a DMX slot
    // means one of them never reaches the fixture.
    std::vector<std::pair<int, size_t>> used;  // offset -> channel index
    for (size_t i = 0; i < rep.type.channels.size(); ++i) {
        const ChannelDef& c = rep.type.channels[i];
        for (int off : {c.coarse, c.fine}) {
            if (off < 0) continue;
            for (const auto& [prev, pi] : used) {
                if (prev != off) continue;
                rep.errors.push_back(
                    "DMX offset " + std::to_string(off + 1) + " is claimed by both " +
                    attrName(rep.type.channels[pi].attr) + " and " + attrName(c.attr));
            }
            used.push_back({off, i});
        }
    }

    if (rep.errors.empty()) rep.ok = true;
    if (rep.type.channels.empty())
        rep.warnings.push_back("mode has no channels, so it patches nothing");
    return rep;
}

// ---------------------------------------------------------------------------
// Validate
// ---------------------------------------------------------------------------
std::vector<Issue> validate(const FixtureDef& def, size_t modeIndex) {
    std::vector<Issue> out;
    auto add = [&](Issue::Level l, std::string what, int ci = -1, std::string geo = {}) {
        out.push_back({l, std::move(what), ci, std::move(geo)});
    };

    if (def.name.empty()) add(Issue::Level::Warning, "fixture has no name");
    if (modeIndex >= def.modes.size()) {
        add(Issue::Level::Error, "no mode at index " + std::to_string(modeIndex));
        return out;
    }
    const ModeDef& mode = def.modes[modeIndex];
    if (mode.name.empty()) add(Issue::Level::Warning, "mode has no name");

    if (!mode.rootGeometry.empty() && !def.findGeometry(mode.rootGeometry))
        add(Issue::Level::Error, "mode root geometry '" + mode.rootGeometry +
                                     "' does not exist", -1, mode.rootGeometry);

    // Geometry tree ---------------------------------------------------------
    std::set<std::string> names;
    walkNodes(def.geometries, [&](const GeometryNode& n, int) {
        if (n.name.empty()) {
            add(Issue::Level::Error, "a geometry has no name");
            return;
        }
        if (!names.insert(n.name).second)
            add(Issue::Level::Error, "duplicate geometry name '" + n.name + "'", -1,
                n.name);
        if (!n.isReference()) {
            if (!n.referenced.empty())
                add(Issue::Level::Warning,
                    "'" + n.name + "' is not a reference but names a referenced "
                    "geometry; it will be ignored", -1, n.name);
            return;
        }
        if (n.referenced.empty())
            add(Issue::Level::Error, "reference '" + n.name + "' points at nothing", -1,
                n.name);
        else if (!def.findGeometry(n.referenced))
            add(Issue::Level::Error, "reference '" + n.name + "' points at missing "
                                     "geometry '" + n.referenced + "'", -1, n.name);
        else if (referenceCycle(def, n.name))
            add(Issue::Level::Error, "reference '" + n.name + "' forms a cycle", -1,
                n.name);
        if (n.dmxOffset < 0)
            add(Issue::Level::Error, "reference '" + n.name + "' has a negative DMX "
                                     "offset", -1, n.name);
    });

    // Channels --------------------------------------------------------------
    std::set<std::pair<int, int>> attrCell;  // (attr, cell)
    for (size_t i = 0; i < mode.channels.size(); ++i) {
        const ChannelDecl& c = mode.channels[i];
        const int ci = static_cast<int>(i);

        if (c.coarse < 0)
            add(Issue::Level::Error, "negative coarse offset", ci, c.geometry);
        if (c.fine >= 0 && c.fine == c.coarse)
            add(Issue::Level::Error, "fine and coarse share one DMX slot", ci,
                c.geometry);
        if (!c.geometry.empty() && !def.findGeometry(c.geometry))
            add(Issue::Level::Error, "unknown geometry '" + c.geometry + "'", ci,
                c.geometry);

        const auto instances = instancesOf(def, c.geometry);
        for (const GeometryInstance& in : instances) {
            const auto key = std::make_pair(static_cast<int>(c.attr),
                                            static_cast<int>(in.cell));
            if (!attrCell.insert(key).second)
                add(Issue::Level::Error,
                    std::string(attrName(c.attr)) + " is declared twice on " +
                        (in.cell ? "cell " + std::to_string(in.cell)
                                 : std::string("the fixture")),
                    ci, c.geometry);
        }

        for (const ChannelSetDef& s : c.sets) {
            if (s.dmxFrom > s.dmxTo)
                add(Issue::Level::Error, "range '" + s.name + "' runs backwards", ci,
                    c.geometry);
            if (s.name.empty())
                add(Issue::Level::Warning, "an unnamed range on " +
                                               std::string(attrName(c.attr)), ci,
                    c.geometry);
        }
    }

    // The compile is the real arbiter of overlaps, because only it knows what
    // the instances expand to.
    const CompileReport cr = compile(def, modeIndex);
    for (const auto& e : cr.errors) add(Issue::Level::Error, e);
    for (const auto& w : cr.warnings) add(Issue::Level::Warning, w);

    // A gap in the footprint is legal GDTF but almost always a mistake, so it
    // is surfaced as information rather than silently accepted.
    if (cr.ok && !cr.type.channels.empty()) {
        std::set<int> slots;
        for (const ChannelDef& c : cr.type.channels) {
            slots.insert(c.coarse);
            if (c.fine >= 0) slots.insert(c.fine);
        }
        const int top = *slots.rbegin();
        for (int s = 0; s <= top; ++s)
            if (!slots.count(s))
                add(Issue::Level::Info,
                    "DMX offset " + std::to_string(s + 1) + " is unused");
    }

    return out;
}

// ---------------------------------------------------------------------------
// Table view
// ---------------------------------------------------------------------------
std::vector<TableRow> tableView(const FixtureDef& def, size_t modeIndex) {
    std::vector<TableRow> rows;
    if (modeIndex >= def.modes.size()) return rows;
    const ModeDef& mode = def.modes[modeIndex];

    for (size_t i = 0; i < mode.channels.size(); ++i) {
        const ChannelDecl& c = mode.channels[i];
        for (const GeometryInstance& in : instancesOf(def, c.geometry)) {
            TableRow r;
            r.dmx = c.coarse + in.dmxOffset + 1;
            r.fineDmx = c.fine < 0 ? 0 : c.fine + in.dmxOffset + 1;
            r.attr = c.attr;
            r.attrName = attrName(c.attr);
            r.geometry = in.instanceName;
            r.cell = in.cell;
            r.defaultValue = c.defaultValue;
            r.invert = c.invert;
            r.setCount = static_cast<int>(c.sets.size());
            r.channelIndex = static_cast<int>(i);
            rows.push_back(std::move(r));
        }
    }

    // Address order, because that is the order the table is read in. A stable
    // tie-break on the declaration index keeps two channels sharing an offset
    // (an error the compile reports) from shuffling between redraws.
    std::stable_sort(rows.begin(), rows.end(), [](const TableRow& a, const TableRow& b) {
        if (a.dmx != b.dmx) return a.dmx < b.dmx;
        return a.channelIndex < b.channelIndex;
    });
    return rows;
}

// ---------------------------------------------------------------------------
// Tree view
// ---------------------------------------------------------------------------
std::vector<TreeRow> treeView(const FixtureDef& def, size_t modeIndex) {
    std::vector<TreeRow> rows;
    if (modeIndex >= def.modes.size()) return rows;
    const ModeDef& mode = def.modes[modeIndex];

    // Channels declared at a given geometry, as offsets within this instance.
    auto channelsAt = [&](const std::string& geoName, int dmxOffset) {
        std::vector<std::pair<Attr, int>> out;
        for (const ChannelDecl& c : mode.channels)
            if (c.geometry == geoName) out.push_back({c.attr, c.coarse + dmxOffset + 1});
        std::sort(out.begin(), out.end(),
                  [](const auto& a, const auto& b) { return a.second < b.second; });
        return out;
    };

    std::function<void(const std::vector<GeometryNode>&, int, int, std::set<std::string>&)>
        emit = [&](const std::vector<GeometryNode>& nodes, int depth, int offsetBias,
                   std::set<std::string>& onPath) {
            for (const GeometryNode& n : nodes) {
                TreeRow r;
                r.depth = depth;
                r.name = n.name;
                r.kind = n.kind;
                r.dmxOffset = n.isReference() ? n.dmxOffset + offsetBias : offsetBias;
                r.referenced = n.referenced;

                if (n.isReference()) {
                    r.cell = cellOfReference(def, &n);
                    r.expandedReference = true;
                    // The reference itself carries no channels; the geometry it
                    // points at does, so they are listed against the instance.
                    r.channels = channelsAt(n.referenced, r.dmxOffset);
                    // Channels declared directly on the reference too, which a
                    // hand-authored definition is allowed to do.
                    for (const auto& ch : channelsAt(n.name, r.dmxOffset))
                        r.channels.push_back(ch);
                    std::sort(r.channels.begin(), r.channels.end(),
                              [](const auto& a, const auto& b) {
                                  return a.second < b.second;
                              });
                    rows.push_back(std::move(r));

                    // Expand what it points at, unless that would loop.
                    const GeometryNode* target = def.findGeometry(n.referenced);
                    if (target && !onPath.count(n.referenced)) {
                        onPath.insert(n.referenced);
                        emit(target->children, depth + 1, r.dmxOffset, onPath);
                        onPath.erase(n.referenced);
                    }
                    continue;
                }

                // A geometry that exists only to be instantiated is the shared
                // cell definition. Its channel numbers are offsets within a
                // cell, so it is flagged rather than presented as if those
                // were real addresses.
                // The numbers stay 1-based as everywhere else; the flag is
                // what tells the view they are offsets WITHIN a cell.
                r.isTemplate = !referencesTo(def, n.name).empty();
                r.channels = channelsAt(n.name, offsetBias);
                rows.push_back(std::move(r));
                emit(n.children, depth + 1, offsetBias, onPath);
            }
        };

    std::set<std::string> onPath;
    emit(def.geometries, 0, 0, onPath);
    return rows;
}

// ---------------------------------------------------------------------------
// Normalisation
// ---------------------------------------------------------------------------
namespace {

// A declaration's identity for comparison purposes: everything that would make
// two cells' channels differ.
std::string declSignature(const ChannelDecl& c) {
    std::string s = std::string(attrName(c.attr)) + "@" + std::to_string(c.coarse) + ":" +
                    std::to_string(c.fine) + ":" + std::to_string(c.defaultValue) + ":" +
                    (c.invert ? "i" : "-");
    for (const ChannelSetDef& st : c.sets)
        s += "|" + st.name + "/" + std::to_string(st.dmxFrom) + "-" +
             std::to_string(st.dmxTo) + "/" + std::to_string(st.anchor);
    return s;
}

std::string groupSignature(const ModeDef& m, const std::string& geoName) {
    std::vector<std::string> sigs;
    for (const ChannelDecl& c : m.channels)
        if (c.geometry == geoName) sigs.push_back(declSignature(c));
    std::sort(sigs.begin(), sigs.end());
    std::string out;
    for (const std::string& s : sigs) out += s + ";";
    return out;
}

} // namespace

NormaliseReport normaliseInstances(FixtureDef& def, size_t modeIndex) {
    NormaliseReport rep;
    if (modeIndex >= def.modes.size()) return rep;
    ModeDef& mode = def.modes[modeIndex];

    // Reference groups, keyed by what they instantiate, in document order.
    std::vector<std::string> targets;
    for (const GeometryNode* r : allReferences(def)) {
        if (r->referenced.empty()) continue;
        if (std::find(targets.begin(), targets.end(), r->referenced) == targets.end())
            targets.push_back(r->referenced);
    }

    for (const std::string& target : targets) {
        const auto refs = referencesTo(def, target);
        if (refs.size() < 2) continue;

        // Already normalised, or the shared geometry carries channels of its
        // own that these would conflict with.
        if (!groupSignature(mode, target).empty()) {
            rep.kept.push_back({target, "the shared geometry already declares channels"});
            continue;
        }

        const std::string first = groupSignature(mode, refs.front()->name);
        if (first.empty()) continue;  // nothing declared per instance

        bool uniform = true;
        for (const GeometryNode* r : refs) {
            if (groupSignature(mode, r->name) == first) continue;
            uniform = false;
            rep.kept.push_back({target, "instance '" + r->name +
                                            "' declares different channels"});
            break;
        }
        if (!uniform) continue;

        // Keep one instance's declarations, moved onto the shared geometry.
        std::vector<ChannelDecl> keep;
        for (const ChannelDecl& c : mode.channels)
            if (c.geometry == refs.front()->name) {
                ChannelDecl moved = c;
                moved.geometry = target;
                keep.push_back(std::move(moved));
            }

        std::vector<std::string> instanceNames;
        for (const GeometryNode* r : refs) instanceNames.push_back(r->name);

        const size_t before = mode.channels.size();
        mode.channels.erase(
            std::remove_if(mode.channels.begin(), mode.channels.end(),
                           [&](const ChannelDecl& c) {
                               return std::find(instanceNames.begin(),
                                                instanceNames.end(),
                                                c.geometry) != instanceNames.end();
                           }),
            mode.channels.end());
        for (ChannelDecl& c : keep) mode.channels.push_back(std::move(c));

        rep.declarationsRemoved +=
            static_cast<int>(before - mode.channels.size());
        ++rep.collapsed;
    }
    return rep;
}

// ---------------------------------------------------------------------------
// FixtureEditor
// ---------------------------------------------------------------------------
ModeDef* FixtureEditor::modeAt(size_t i) {
    return i < def_->modes.size() ? &def_->modes[i] : nullptr;
}

bool FixtureEditor::addChannel(size_t modeIndex, Attr a, const std::string& geometry,
                               int coarse, int fine, std::string* err) {
    ModeDef* m = modeAt(modeIndex);
    if (!m) {
        if (err) *err = "no such mode";
        return false;
    }
    if (!geometry.empty() && !def_->findGeometry(geometry)) {
        if (err) *err = "unknown geometry: " + geometry;
        return false;
    }

    ChannelDecl c;
    c.attr = a;
    c.geometry = geometry;
    c.fine = fine;

    if (coarse < 0) {
        // Appending has to respect what the offset is relative to. On an
        // instanced geometry it appends inside the cell; anywhere else it
        // appends after everything the mode claims once expanded, so a new
        // channel cannot land in the middle of a pixel bar.
        int top = -1;
        const bool instanced =
            !geometry.empty() && instancesOf(*def_, geometry).size() > 1;
        if (instanced) {
            for (const ChannelDecl& ch : m->channels) {
                if (ch.geometry != geometry) continue;
                top = std::max(top, ch.coarse);
                if (ch.fine >= 0) top = std::max(top, ch.fine);
            }
        } else {
            const CompileReport cr = compile(*def_, modeIndex);
            for (const ChannelDef& ch : cr.type.channels) {
                top = std::max(top, ch.coarse);
                if (ch.fine >= 0) top = std::max(top, ch.fine);
            }
        }
        c.coarse = top + 1;
        if (fine >= 0) c.fine = c.coarse + 1;
    } else {
        c.coarse = coarse;
    }

    const bool instancedGeo =
        !geometry.empty() && instancesOf(*def_, geometry).size() > 1;
    m->channels.push_back(std::move(c));
    // Growing a cell definition has to re-space its instances, or every cell
    // lands on top of the next one.
    if (instancedGeo) respaceInstances(modeIndex, geometry);
    return true;
}

bool FixtureEditor::removeChannel(size_t modeIndex, int channelIndex, std::string* err) {
    ModeDef* m = modeAt(modeIndex);
    if (!m || channelIndex < 0 ||
        channelIndex >= static_cast<int>(m->channels.size())) {
        if (err) *err = "no such channel";
        return false;
    }
    const std::string geo = m->channels[static_cast<size_t>(channelIndex)].geometry;
    m->channels.erase(m->channels.begin() + channelIndex);
    if (!geo.empty() && instancesOf(*def_, geo).size() > 1)
        respaceInstances(modeIndex, geo);
    return true;
}

bool FixtureEditor::setChannelAttr(size_t modeIndex, int channelIndex, Attr a,
                                   std::string* err) {
    ModeDef* m = modeAt(modeIndex);
    if (!m || channelIndex < 0 ||
        channelIndex >= static_cast<int>(m->channels.size())) {
        if (err) *err = "no such channel";
        return false;
    }
    m->channels[static_cast<size_t>(channelIndex)].attr = a;
    return true;
}

bool FixtureEditor::setChannelOffset(size_t modeIndex, int channelIndex, int coarse,
                                     int fine, std::string* err) {
    ModeDef* m = modeAt(modeIndex);
    if (!m || channelIndex < 0 ||
        channelIndex >= static_cast<int>(m->channels.size())) {
        if (err) *err = "no such channel";
        return false;
    }
    if (coarse < 0) {
        if (err) *err = "offset cannot be negative";
        return false;
    }
    if (fine >= 0 && fine == coarse) {
        if (err) *err = "fine and coarse cannot share one slot";
        return false;
    }
    ChannelDecl& c = m->channels[static_cast<size_t>(channelIndex)];
    const std::string geo = c.geometry;
    c.coarse = coarse;
    c.fine = fine;
    if (!geo.empty() && instancesOf(*def_, geo).size() > 1)
        respaceInstances(modeIndex, geo);
    return true;
}

bool FixtureEditor::addChannelSet(size_t modeIndex, int channelIndex, ChannelSetDef set,
                                  std::string* err) {
    ModeDef* m = modeAt(modeIndex);
    if (!m || channelIndex < 0 ||
        channelIndex >= static_cast<int>(m->channels.size())) {
        if (err) *err = "no such channel";
        return false;
    }
    if (set.dmxFrom > set.dmxTo) {
        if (err) *err = "range runs backwards";
        return false;
    }
    m->channels[static_cast<size_t>(channelIndex)].sets.push_back(std::move(set));
    return true;
}

int FixtureEditor::respaceInstances(size_t modeIndex, const std::string& referenced) {
    ModeDef* m = modeAt(modeIndex);
    if (!m || referenced.empty()) return 0;

    int footprint = 0;
    for (const ChannelDecl& c : m->channels) {
        if (c.geometry != referenced) continue;
        footprint = std::max(footprint, c.coarse + 1);
        if (c.fine >= 0) footprint = std::max(footprint, c.fine + 1);
    }
    if (footprint <= 0) return 0;

    // Walk the tree in document order so the cell numbering and the spacing
    // agree, and keep the first instance where the user put it.
    std::vector<GeometryNode*> refs;
    std::function<void(std::vector<GeometryNode>&)> collect =
        [&](std::vector<GeometryNode>& nodes) {
            for (GeometryNode& n : nodes) {
                if (n.isReference() && n.referenced == referenced) refs.push_back(&n);
                collect(n.children);
            }
        };
    collect(def_->geometries);
    if (refs.size() < 2) return 0;

    const int base = refs.front()->dmxOffset;
    int moved = 0;
    for (size_t i = 0; i < refs.size(); ++i) {
        const int want = base + static_cast<int>(i) * footprint;
        if (refs[i]->dmxOffset != want) ++moved;
        refs[i]->dmxOffset = want;
    }
    return moved;
}

int FixtureEditor::packOffsets(size_t modeIndex) {
    ModeDef* m = modeAt(modeIndex);
    if (!m) return 0;

    // Packing happens within one address space at a time, because a channel's
    // offset is relative to its own instance. So:
    //
    //   - each instanced geometry's channels pack from the cell's own zero,
    //     then the instances re-space to the new cell footprint;
    //   - the mode's own channels pack from zero ONLY when the mode has no
    //     instances. With cells present, whether the master block sits before
    //     or after the cell block is a layout decision, not a mechanical one,
    //     so it is left to the user rather than guessed at.
    auto packGroup = [&](const std::string& geo) {
        std::vector<size_t> order;
        for (size_t i = 0; i < m->channels.size(); ++i)
            if (m->channels[i].geometry == geo) order.push_back(i);
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return m->channels[a].coarse < m->channels[b].coarse;
        });
        int next = 0, moved = 0;
        for (size_t i : order) {
            ChannelDecl& c = m->channels[i];
            const int wantCoarse = next++;
            const int wantFine = c.fine >= 0 ? next++ : -1;
            if (c.coarse != wantCoarse || c.fine != wantFine) ++moved;
            c.coarse = wantCoarse;
            c.fine = wantFine;
        }
        return moved;
    };

    std::vector<std::string> instanced;
    bool anyInstances = false;
    for (const ChannelDecl& c : m->channels) {
        if (c.geometry.empty()) continue;
        if (instancesOf(*def_, c.geometry).size() <= 1) continue;
        anyInstances = true;
        if (std::find(instanced.begin(), instanced.end(), c.geometry) == instanced.end())
            instanced.push_back(c.geometry);
    }

    int moved = 0;
    for (const std::string& geo : instanced) {
        moved += packGroup(geo);
        respaceInstances(modeIndex, geo);
    }

    if (!anyInstances) {
        // No cells, so the whole mode is one address space: order by current
        // offset and hand out consecutive slots.
        std::vector<size_t> order;
        for (size_t i = 0; i < m->channels.size(); ++i) order.push_back(i);
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return m->channels[a].coarse < m->channels[b].coarse;
        });
        int next = 0;
        for (size_t i : order) {
            ChannelDecl& c = m->channels[i];
            const int wantCoarse = next++;
            const int wantFine = c.fine >= 0 ? next++ : -1;
            if (c.coarse != wantCoarse || c.fine != wantFine) ++moved;
            c.coarse = wantCoarse;
            c.fine = wantFine;
        }
    }
    return moved;
}

bool FixtureEditor::addGeometry(const std::string& parent, GeometryNode node,
                                std::string* err) {
    if (node.name.empty()) {
        if (err) *err = "a geometry needs a name";
        return false;
    }
    if (def_->findGeometry(node.name)) {
        if (err) *err = "geometry already exists: " + node.name;
        return false;
    }
    if (node.isReference() && !node.referenced.empty() &&
        !def_->findGeometry(node.referenced)) {
        if (err) *err = "cannot reference missing geometry: " + node.referenced;
        return false;
    }
    if (parent.empty()) {
        def_->geometries.push_back(std::move(node));
        return true;
    }
    GeometryNode* p = def_->findGeometry(parent);
    if (!p) {
        if (err) *err = "unknown parent geometry: " + parent;
        return false;
    }
    p->children.push_back(std::move(node));
    return true;
}

bool FixtureEditor::removeGeometry(const std::string& geoName, std::string* err) {
    // Refuse while anything still points at it, rather than leaving dangling
    // references for the compile to trip over later.
    if (!referencesTo(*def_, geoName).empty()) {
        if (err) *err = geoName + " is still instantiated by a reference";
        return false;
    }
    for (const ModeDef& m : def_->modes)
        for (const ChannelDecl& c : m.channels)
            if (c.geometry == geoName) {
                if (err) *err = geoName + " still has channels declared against it";
                return false;
            }

    std::function<bool(std::vector<GeometryNode>&)> drop =
        [&](std::vector<GeometryNode>& nodes) {
            for (size_t i = 0; i < nodes.size(); ++i) {
                if (nodes[i].name == geoName) {
                    nodes.erase(nodes.begin() + static_cast<long>(i));
                    return true;
                }
                if (drop(nodes[i].children)) return true;
            }
            return false;
        };
    if (!drop(def_->geometries)) {
        if (err) *err = "unknown geometry: " + geoName;
        return false;
    }
    return true;
}

bool FixtureEditor::renameGeometry(const std::string& from, const std::string& to,
                                   std::string* err) {
    if (to.empty()) {
        if (err) *err = "a geometry needs a name";
        return false;
    }
    GeometryNode* n = def_->findGeometry(from);
    if (!n) {
        if (err) *err = "unknown geometry: " + from;
        return false;
    }
    if (from != to && def_->findGeometry(to)) {
        if (err) *err = "geometry already exists: " + to;
        return false;
    }
    n->name = to;

    // Rename is safe by construction only if every mention moves with it.
    walkNodes(def_->geometries, [](const GeometryNode&, int) {});
    std::function<void(std::vector<GeometryNode>&)> fix =
        [&](std::vector<GeometryNode>& nodes) {
            for (GeometryNode& g : nodes) {
                if (g.referenced == from) g.referenced = to;
                fix(g.children);
            }
        };
    fix(def_->geometries);
    for (ModeDef& m : def_->modes) {
        if (m.rootGeometry == from) m.rootGeometry = to;
        for (ChannelDecl& c : m.channels)
            if (c.geometry == from) c.geometry = to;
    }
    return true;
}

int FixtureEditor::addInstances(size_t modeIndex, const std::string& parent,
                                const std::string& referenced, int count,
                                const std::string& namePrefix, int firstOffset,
                                int stride, std::string* err) {
    if (count <= 0) {
        if (err) *err = "count must be positive";
        return 0;
    }
    const GeometryNode* target = def_->findGeometry(referenced);
    if (!target) {
        if (err) *err = "unknown geometry: " + referenced;
        return 0;
    }
    if (!parent.empty() && !def_->findGeometry(parent)) {
        if (err) *err = "unknown parent geometry: " + parent;
        return 0;
    }
    if (firstOffset < 0) {
        if (err) *err = "offset cannot be negative";
        return 0;
    }

    // The stride defaults to the footprint of the channels declared against
    // the referenced geometry, which is what "automatic DMX offset" means:
    // the user says twelve pixels, not twelve offsets.
    if (stride <= 0) {
        int top = -1;
        const ModeDef* m = modeIndex < def_->modes.size() ? &def_->modes[modeIndex]
                                                          : nullptr;
        if (m) {
            for (const ChannelDecl& c : m->channels) {
                if (c.geometry != referenced) continue;
                top = std::max(top, c.coarse);
                if (c.fine >= 0) top = std::max(top, c.fine);
            }
        }
        if (top < 0) {
            if (err)
                *err = "cannot infer a stride: " + referenced +
                       " has no channels in this mode, so pass one";
            return 0;
        }
        stride = top + 1;
    }

    int made = 0;
    for (int i = 0; i < count; ++i) {
        GeometryNode ref;
        ref.kind = GeometryKind::Reference;
        ref.name = namePrefix + std::to_string(i + 1);
        ref.referenced = referenced;
        ref.dmxOffset = firstOffset + i * stride;
        std::string e;
        if (!addGeometry(parent, std::move(ref), &e)) {
            if (err) *err = e;
            break;
        }
        ++made;
    }
    return made;
}

size_t FixtureEditor::addMode(const std::string& name, const std::string& rootGeometry) {
    ModeDef m;
    m.name = name;
    m.rootGeometry = rootGeometry;
    def_->modes.push_back(std::move(m));
    return def_->modes.size() - 1;
}

bool FixtureEditor::removeMode(size_t modeIndex, std::string* err) {
    if (modeIndex >= def_->modes.size()) {
        if (err) *err = "no such mode";
        return false;
    }
    if (def_->modes.size() == 1) {
        if (err) *err = "a fixture needs at least one mode";
        return false;
    }
    def_->modes.erase(def_->modes.begin() + static_cast<long>(modeIndex));
    return true;
}

// ---------------------------------------------------------------------------
// Starter definitions
// ---------------------------------------------------------------------------
FixtureDef starterPixelBar() {
    FixtureDef d;
    d.manufacturer = "Lumen";
    d.name = "Pixel Bar 12";
    d.shortName = "PB12";
    d.description = "Twelve RGB cells behind a master dimmer";

    GeometryNode body;
    body.name = "Body";
    body.kind = GeometryKind::Normal;

    // One cell definition. Twelve references to it come from addInstances
    // below, which is the point: the channels are declared once.
    GeometryNode pixel;
    pixel.name = "Pixel";
    pixel.kind = GeometryKind::Beam;

    d.geometries.push_back(std::move(body));
    d.geometries.push_back(std::move(pixel));

    FixtureEditor ed(d);
    const size_t m = ed.addMode("Pixel", "Body");

    // Master dimmer at the parent, colour per cell (§1.3). The pixel's own
    // channels are numbered from the pixel's start, so one declaration serves
    // all twelve; the stride is inferred from them.
    ed.addChannel(m, Attr::Dimmer, "Body", 0);
    ed.addChannel(m, Attr::Red, "Pixel", 0);
    ed.addChannel(m, Attr::Green, "Pixel", 1);
    ed.addChannel(m, Attr::Blue, "Pixel", 2);
    ed.addInstances(m, "Body", "Pixel", 12, "Pixel", 1);
    return d;
}

FixtureDef starterMovingSpot() {
    FixtureDef d;
    d.manufacturer = "Lumen";
    d.name = "Moving Spot";
    d.shortName = "MSpot";

    GeometryNode base;
    base.name = "Base";
    GeometryNode yoke;
    yoke.name = "Yoke";
    yoke.kind = GeometryKind::Axis;
    GeometryNode head;
    head.name = "Head";
    head.kind = GeometryKind::Beam;
    yoke.children.push_back(std::move(head));
    base.children.push_back(std::move(yoke));
    d.geometries.push_back(std::move(base));

    FixtureDef& def = d;
    FixtureEditor ed(def);
    const size_t m = ed.addMode("Standard", "Base");

    ed.addChannel(m, Attr::Pan, "Yoke", 0, 1);
    ed.addChannel(m, Attr::Tilt, "Yoke", 2, 3);
    ed.addChannel(m, Attr::Dimmer, "Head", 4);
    ed.addChannel(m, Attr::Shutter, "Head", 5);
    ed.addChannel(m, Attr::Zoom, "Head", 6);

    // A gobo wheel with range labels, so the auto-stepper (§7.3) has
    // something to read on a hand-authored fixture.
    ed.addChannel(m, Attr::Gobo1, "Head", 7);
    const int gobo = static_cast<int>(def.modes[m].channels.size()) - 1;
    ed.addChannelSet(m, gobo, {"Open", 0, 9, 0.f, 0.f, true});
    ed.addChannelSet(m, gobo, {"Dots", 10, 19, 0.f, 0.f, false});
    ed.addChannelSet(m, gobo, {"Breakup", 20, 29, 0.f, 0.f, false});
    return d;
}

} // namespace lumen
