#include "lumen/Mvr.hpp"

#include "lumen/Xml.hpp"
#include "lumen/Zip.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>

namespace lumen {

namespace {

constexpr float kRadToDeg = 57.29577951308232f;

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::vector<float> parseFloats(const std::string& s) {
    std::vector<float> out;
    std::string cur;
    for (char c : s + ",") {
        if (c == ',' || c == ' ') {
            const std::string t = trim(cur);
            if (!t.empty()) {
                try {
                    out.push_back(std::stof(t));
                } catch (...) {
                }
            }
            cur.clear();
        } else {
            cur += c;
        }
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Matrix and address
// ---------------------------------------------------------------------------
MvrMatrix parseMvrMatrix(const std::string& text) {
    MvrMatrix out;

    // Four bracketed vectors: three rotation columns then the translation.
    std::vector<std::vector<float>> groups;
    size_t i = 0;
    while (i < text.size()) {
        const size_t open = text.find('{', i);
        if (open == std::string::npos) break;
        const size_t close = text.find('}', open);
        if (close == std::string::npos) break;
        groups.push_back(parseFloats(text.substr(open + 1, close - open - 1)));
        i = close + 1;
    }
    if (groups.size() < 4) return out;

    for (int c = 0; c < 3; ++c) {
        if (groups[static_cast<size_t>(c)].size() < 3) return out;
        for (int r = 0; r < 3; ++r) out.m[r][c] = groups[static_cast<size_t>(c)][static_cast<size_t>(r)];
    }
    if (groups[3].size() >= 3) {
        out.tx = groups[3][0];
        out.ty = groups[3][1];
        out.tz = groups[3][2];
    }
    out.parsed = true;
    return out;
}

namespace {
// asin/atan2 on an exact axis-aligned matrix still return -0.0 and float
// noise. A plot that is square should read as 0, not as -0.0 or 0.000003.
void tidy(float& v) {
    if (std::fabs(v) < 1e-4f) v = 0.f;
}
} // namespace

void eulerFromMatrix(const MvrMatrix& m, float& rx, float& ry, float& rz) {
    // ZYX extraction. Lighting plots mostly carry a yaw with a little tilt, so
    // the degenerate case (looking straight up or down) is handled rather than
    // producing NaNs.
    const float sy = -m.m[2][0];
    if (std::fabs(sy) > 0.99999f) {
        ry = (sy > 0.f ? 90.f : -90.f);
        rx = std::atan2(-m.m[1][2], m.m[1][1]) * kRadToDeg;
        rz = 0.f;
        tidy(rx);
        return;
    }
    ry = std::asin(std::clamp(sy, -1.f, 1.f)) * kRadToDeg;
    rx = std::atan2(m.m[2][1], m.m[2][2]) * kRadToDeg;
    rz = std::atan2(m.m[1][0], m.m[0][0]) * kRadToDeg;
    tidy(rx);
    tidy(ry);
    tidy(rz);
}

void decomposeAddress(const std::string& text, int& universe, int& channel) {
    const std::string t = trim(text);
    universe = 1;
    channel = 1;
    if (t.empty()) return;

    // Some writers use "universe.channel" rather than an absolute value.
    const size_t dot = t.find('.');
    if (dot != std::string::npos) {
        try {
            universe = std::max(1, std::stoi(t.substr(0, dot)));
            channel = std::max(1, std::stoi(t.substr(dot + 1)));
        } catch (...) {
        }
        return;
    }

    long abs = 1;
    try {
        abs = std::stol(t);
    } catch (...) {
        return;
    }
    if (abs < 1) abs = 1;
    // An absolute address counts through universes of 512.
    universe = static_cast<int>((abs - 1) / 512) + 1;
    channel = static_cast<int>((abs - 1) % 512) + 1;
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------
MvrReport parseMvrXml(const std::string& xml) {
    MvrReport rep;

    XmlDoc doc = parseXml(xml);
    if (!doc.ok) {
        rep.error = "xml: " + doc.error;
        return rep;
    }

    const XmlNode* scene = doc.root.name == "Scene" ? &doc.root : doc.root.descend("Scene");
    if (!scene) {
        rep.error = "no Scene element";
        return rep;
    }
    const XmlNode* layers = scene->descend("Layers");
    if (!layers) {
        rep.error = "no Layers element";
        return rep;
    }

    // A fixture can sit inside nested GroupObjects, so the walk is recursive
    // and carries the owning layer down with it.
    std::function<void(const XmlNode&, const std::string&)> walk =
        [&](const XmlNode& node, const std::string& layerName) {
            for (const auto& child : node.children) {
                if (child.name == "Fixture") {
                    MvrFixture f;
                    f.name = child.attrOr("name");
                    f.uuid = child.attrOr("uuid");
                    f.layer = layerName;

                    if (const XmlNode* spec = child.kid("GDTFSpec"))
                        f.gdtfSpec = trim(spec->text);
                    if (const XmlNode* mode = child.kid("GDTFMode"))
                        f.gdtfMode = trim(mode->text);

                    if (const XmlNode* fid = child.kid("FixtureID")) {
                        try {
                            f.fixtureId = static_cast<FixtureId>(std::stoul(trim(fid->text)));
                        } catch (...) {
                        }
                    }
                    // Some writers put the usable number in UnitNumber instead.
                    if (f.fixtureId == 0) {
                        if (const XmlNode* un = child.kid("UnitNumber")) {
                            try {
                                f.fixtureId =
                                    static_cast<FixtureId>(std::stoul(trim(un->text)));
                            } catch (...) {
                            }
                        }
                    }

                    if (const XmlNode* addrs = child.kid("Addresses")) {
                        if (const XmlNode* a = addrs->kid("Address"))
                            decomposeAddress(a->text, f.universe, f.address);
                    }

                    if (const XmlNode* mx = child.kid("Matrix")) {
                        const MvrMatrix m = parseMvrMatrix(mx->text);
                        if (m.parsed) {
                            // MVR is in millimetres; the stage model is metres.
                            f.x = m.tx / 1000.f;
                            f.y = m.ty / 1000.f;
                            f.z = m.tz / 1000.f;
                            eulerFromMatrix(m, f.rotX, f.rotY, f.rotZ);
                            f.positioned = true;
                        }
                    }

                    rep.fixtures.push_back(std::move(f));
                    continue;
                }
                // GroupObject and ChildList just nest; keep the layer name.
                walk(child, layerName);
            }
        };

    for (const XmlNode* layer : layers->kids("Layer")) {
        const std::string name = layer->attrOr("name", "Layer");
        rep.layers.push_back(name);
        walk(*layer, name);
    }

    if (rep.fixtures.empty()) rep.warnings.push_back("no fixtures found in any layer");
    rep.ok = true;
    return rep;
}

MvrReport loadMvrFile(const std::string& path, const AttributeMap& attrMap) {
    MvrReport rep;

    ZipReader zip;
    std::string err;
    if (!zip.openFile(path, &err)) {
        rep.error = "zip: " + err;
        return rep;
    }
    const auto xml = zip.readText("GeneralSceneDescription.xml", &err);
    if (!xml) {
        rep.error = "GeneralSceneDescription.xml: " + err;
        return rep;
    }

    rep = parseMvrXml(*xml);
    if (!rep.ok) return rep;

    // Resolve the GDTF profiles the scene references. They are embedded in the
    // same archive, which is what makes an MVR a complete handover.
    std::vector<std::string> wanted;
    for (const auto& f : rep.fixtures) {
        if (f.gdtfSpec.empty()) continue;
        if (std::find(wanted.begin(), wanted.end(), f.gdtfSpec) == wanted.end())
            wanted.push_back(f.gdtfSpec);
    }

    for (const std::string& spec : wanted) {
        const auto bytes = zip.read(spec, &err);
        if (!bytes) {
            rep.warnings.push_back("missing profile in archive: " + spec);
            continue;
        }
        ZipReader inner;
        if (!inner.openBytes(*bytes, &err)) {
            rep.warnings.push_back("profile is not a valid gdtf: " + spec);
            continue;
        }
        const auto desc = inner.readText("description.xml", &err);
        if (!desc) {
            rep.warnings.push_back("profile has no description.xml: " + spec);
            continue;
        }
        GdtfReport g = parseGdtfXml(*desc, attrMap);
        if (!g.ok) {
            rep.warnings.push_back("profile failed to parse: " + spec + " (" + g.error + ")");
            continue;
        }
        for (const auto& u : g.unmapped)
            rep.warnings.push_back("unmapped attribute " + u.gdtfAttribute + " in " + spec);
        rep.profiles[spec] = std::move(g.info);
    }
    return rep;
}

// ---------------------------------------------------------------------------
// Applying
// ---------------------------------------------------------------------------
MvrApplyReport applyMvr(Patch& patch, GroupStore* groups, const MvrReport& mvr) {
    MvrApplyReport rep;

    // Register each referenced mode as a fixture type, keyed by spec + mode so
    // two modes of the same profile stay distinct.
    std::map<std::string, int> typeIndex;
    for (const auto& [spec, info] : mvr.profiles) {
        for (const auto& mode : info.modes) {
            const std::string key = spec + "|" + mode.name;
            // addType replaces a same-named type, so only count the new ones.
            const bool isNew = patch.findType(mode.type.name) < 0;
            typeIndex[key] = patch.addType(mode.type);
            if (isNew) ++rep.typesAdded;
        }
    }

    // Which patched fixture each MVR entry became, so layer groups are built
    // from real ids rather than by matching names back up.
    std::vector<FixtureId> placed(mvr.fixtures.size(), 0);

    FixtureId nextFree = 1;
    for (size_t fi = 0; fi < mvr.fixtures.size(); ++fi) {
        const MvrFixture& f = mvr.fixtures[fi];
        std::string key = f.gdtfSpec + "|" + f.gdtfMode;
        auto it = typeIndex.find(key);

        if (it == typeIndex.end()) {
            // The named mode is missing: fall back to the profile's first
            // mode rather than dropping the fixture, and say so.
            const auto profile = mvr.profiles.find(f.gdtfSpec);
            if (profile != mvr.profiles.end() && !profile->second.modes.empty()) {
                key = f.gdtfSpec + "|" + profile->second.modes.front().name;
                it = typeIndex.find(key);
                rep.warnings.push_back("fixture '" + f.name + "' asks for mode '" +
                                       f.gdtfMode + "' which is not in " + f.gdtfSpec +
                                       "; used '" + profile->second.modes.front().name +
                                       "'");
            }
        }
        if (it == typeIndex.end()) {
            rep.warnings.push_back("no profile for fixture '" + f.name + "' (" +
                                   f.gdtfSpec + "); skipped");
            ++rep.skipped;
            continue;
        }

        Patch::PatchRequest req;
        req.id = f.fixtureId;
        req.name = f.name;
        req.typeIndex = it->second;
        req.universe = f.universe;
        req.address = f.address;

        // An MVR without usable fixture ids still has to patch, so fall back
        // to the next free number rather than refusing the import.
        if (req.id == 0 || patch.find(req.id)) {
            while (patch.find(nextFree)) ++nextFree;
            if (req.id != 0)
                rep.warnings.push_back("fixture id " + std::to_string(f.fixtureId) +
                                       " already in use; patched '" + f.name + "' as " +
                                       std::to_string(nextFree));
            req.id = nextFree;
        }

        std::string err;
        if (!patch.add(req, &err)) {
            rep.warnings.push_back("could not patch '" + f.name + "': " + err);
            ++rep.skipped;
            continue;
        }
        ++rep.patched;
        placed[fi] = req.id;

        if (f.positioned) patch.setPosition(req.id, f.x, f.y, f.z, f.rotX, f.rotY, f.rotZ);
        if (!f.layer.empty()) patch.addTag(req.id, f.layer);
    }

    // Each layer becomes a group, which is most of what a designer's plot
    // organisation is for.
    if (groups) {
        for (const std::string& layer : mvr.layers) {
            std::vector<Target> members;
            for (size_t fi = 0; fi < mvr.fixtures.size(); ++fi) {
                if (mvr.fixtures[fi].layer != layer || placed[fi] == 0) continue;
                members.push_back({placed[fi], 0});
            }
            if (members.empty()) continue;
            GroupDef g;
            g.name = layer;
            g.members = std::move(members);
            g.tags.push_back("mvr");
            groups->add(std::move(g));
            ++rep.groupsCreated;
        }
    }
    return rep;
}

} // namespace lumen
