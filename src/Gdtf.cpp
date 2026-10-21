#include "lumen/Gdtf.hpp"

#include "lumen/Xml.hpp"
#include "lumen/Zip.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>

namespace lumen {

// ---------------------------------------------------------------------------
// AttributeMap — the §6.3 mapping
// ---------------------------------------------------------------------------
AttributeMap::AttributeMap() {
    // Intensity
    set("Dimmer", Attr::Dimmer);

    // Position
    set("Pan", Attr::Pan);
    set("Tilt", Attr::Tilt);
    set("PanRotate", Attr::PanRotate);
    set("TiltRotate", Attr::TiltRotate);
    set("PositionMSpeed", Attr::PanTiltSpeed);
    set("PanTiltSpeed", Attr::PanTiltSpeed);
    set("XYZ_X", Attr::PosX);
    set("XYZ_Y", Attr::PosY);
    set("XYZ_Z", Attr::PosZ);
    set("Rot_X", Attr::RotX);
    set("Rot_Y", Attr::RotY);
    set("Rot_Z", Attr::RotZ);

    // Colour — additive. GDTF spells these ColorAdd_*.
    set("ColorAdd_R", Attr::Red);
    set("ColorAdd_G", Attr::Green);
    set("ColorAdd_B", Attr::Blue);
    set("ColorAdd_A", Attr::Amber);
    set("ColorAdd_W", Attr::White);
    set("ColorAdd_WW", Attr::WarmWhite);
    set("ColorAdd_CW", Attr::CoolWhite);
    set("ColorAdd_UV", Attr::UV);
    set("ColorAdd_L", Attr::Lime);
    set("ColorAdd_C", Attr::Cyan);
    // Some profiles use the bare names instead.
    set("Red", Attr::Red);
    set("Green", Attr::Green);
    set("Blue", Attr::Blue);
    set("White", Attr::White);
    set("Amber", Attr::Amber);

    // Colour — subtractive
    set("ColorSub_C", Attr::CyanSub);
    set("ColorSub_M", Attr::Magenta);
    set("ColorSub_Y", Attr::Yellow);
    set("Cyan", Attr::CyanSub);
    set("Magenta", Attr::Magenta);
    set("Yellow", Attr::Yellow);

    // Colour — correction and model
    set("CTO", Attr::CTO);
    set("CTB", Attr::CTB);
    set("CTC", Attr::CTO);
    set("Tint", Attr::Tint);
    set("HSB_Hue", Attr::Hue);
    set("HSB_Saturation", Attr::Saturation);
    set("ColorRGB_Hue", Attr::Hue);
    set("ColorRGB_Saturation", Attr::Saturation);
    set("ColorMacro1", Attr::ColorMacro);
    set("Color1", Attr::ColorWheel1);
    set("Color1WheelSpin", Attr::ColorWheel1Spin);

    // Beam — shutter
    set("Shutter1", Attr::Shutter);
    set("Shutter1Strobe", Attr::StrobeRate);
    set("Shutter1StrobeRate", Attr::StrobeRate);
    set("StrobeFrequency", Attr::StrobeRate);

    // Beam — optics
    set("Zoom", Attr::Zoom);
    set("Focus1", Attr::Focus);
    set("Focus", Attr::Focus);
    set("Iris", Attr::Iris);
    set("Frost1", Attr::Frost);
    set("Frost", Attr::Frost);

    // Beam — gobo. GDTF uses GoboN / GoboNPos / GoboNPosRotate.
    set("Gobo1", Attr::Gobo1);
    set("Gobo1Pos", Attr::Gobo1Pos);
    set("Gobo1PosRotate", Attr::Gobo1Spin);
    set("Gobo1WheelSpin", Attr::Gobo1Spin);
    set("Gobo2", Attr::Gobo2);
    set("Gobo2Pos", Attr::Gobo2Pos);
    set("Gobo2PosRotate", Attr::Gobo2Spin);

    // Beam — prism
    set("Prism1", Attr::Prism1);
    set("Prism1Pos", Attr::Prism1Pos);
    set("Prism1PosRotate", Attr::Prism1Spin);

    // Beam — animation
    set("AnimationWheel1", Attr::AnimWheel);
    set("AnimationWheel1Pos", Attr::AnimWheelPos);
    set("AnimationWheel1PosRotate", Attr::AnimWheelSpin);

    // Beam — shapers. GDTF calls them Blades.
    set("Blade1A", Attr::Shaper1A);
    set("Blade1B", Attr::Shaper1B);
    set("Blade2A", Attr::Shaper2A);
    set("Blade2B", Attr::Shaper2B);
    set("Blade3A", Attr::Shaper3A);
    set("Blade3B", Attr::Shaper3B);
    set("Blade4A", Attr::Shaper4A);
    set("Blade4B", Attr::Shaper4B);
    set("ShaperRot", Attr::ShaperRot);
    set("Blade1Rot", Attr::ShaperRot);

    // Control
    set("Control1", Attr::Reset);
    set("LampControl", Attr::LampOn);
    set("Function", Attr::Reset);
    set("Fan1", Attr::FanSpeed);
    set("FanMode", Attr::FanMode);
    set("DisplayIntensity", Attr::DisplayIntensity);
}

std::optional<Attr> AttributeMap::map(const std::string& gdtfName) const {
    auto it = map_.find(gdtfName);
    if (it != map_.end()) return it->second;
    return std::nullopt;
}

void AttributeMap::set(const std::string& gdtfName, Attr a) { map_[gdtfName] = a; }
void AttributeMap::remove(const std::string& gdtfName) { map_.erase(gdtfName); }

std::optional<Attr> AttributeMap::mapWithIndex(const std::string& gdtfName) const {
    if (auto direct = map(gdtfName)) return direct;

    // GDTF numbers repeated features. Fall back to the index-1 form, so Gobo3
    // and Prism2 are recognised without needing their own table entries —
    // mapped to the attribute Lumen does model, with the index noted by the
    // caller if it matters.
    std::string generic;
    for (size_t i = 0; i < gdtfName.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(gdtfName[i]))) {
            // Replace the first run of digits with "1".
            generic = gdtfName.substr(0, i) + "1";
            size_t j = i;
            while (j < gdtfName.size() &&
                   std::isdigit(static_cast<unsigned char>(gdtfName[j])))
                ++j;
            generic += gdtfName.substr(j);
            break;
        }
    }
    if (!generic.empty() && generic != gdtfName) return map(generic);
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// DMX values
// ---------------------------------------------------------------------------
float parseGdtfDmx(const std::string& text) {
    if (text.empty()) return 0.f;
    const size_t slash = text.find('/');
    const std::string valuePart = slash == std::string::npos ? text : text.substr(0, slash);
    int bytes = 1;
    if (slash != std::string::npos) {
        try {
            bytes = std::stoi(text.substr(slash + 1));
        } catch (...) {
            bytes = 1;
        }
    }
    if (bytes < 1) bytes = 1;
    if (bytes > 4) bytes = 4;

    double value = 0.0;
    try {
        value = std::stod(valuePart);
    } catch (...) {
        return 0.f;
    }
    const double maxValue = std::pow(256.0, bytes) - 1.0;
    if (maxValue <= 0.0) return 0.f;
    return static_cast<float>(std::clamp(value / maxValue, 0.0, 1.0));
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------
namespace {

// "1,2" -> {0, 1}; GDTF offsets are 1-based, Lumen's are 0-based.
std::vector<int> parseOffsets(const std::string& text) {
    std::vector<int> out;
    std::string cur;
    for (char c : text + ",") {
        if (c == ',') {
            if (!cur.empty()) {
                try {
                    out.push_back(std::stoi(cur) - 1);
                } catch (...) {
                }
                cur.clear();
            }
        } else if (!std::isspace(static_cast<unsigned char>(c))) {
            cur += c;
        }
    }
    return out;
}

// GDTF writes an attribute as "Attribute" or "Feature.Attribute".
std::string lastSegment(const std::string& s) {
    const size_t dot = s.rfind('.');
    return dot == std::string::npos ? s : s.substr(dot + 1);
}

bool looksLikeAnchor(const std::string& setName, float physFrom, float physTo) {
    // §7.3's anchor detection, applied at import: a set whose physical value
    // is zero, or that is named for a stop/open state, is the semantic zero
    // the auto-stepper must step outward from.
    if (std::fabs(physFrom) < 1e-6f && std::fabs(physTo) < 1e-6f) return true;
    std::string n;
    for (char c : setName) n += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return n == "stop" || n == "open" || n == "off" || n == "no function" ||
           n == "closed" || n == "none" || n == "index";
}

// Mutable walk over every GeometryReference in a tree. Needed because the
// import has to fill in offsets the file left implicit.
void walkRefs(std::vector<GeometryNode>& nodes,
              const std::function<void(GeometryNode&)>& fn) {
    for (GeometryNode& n : nodes) {
        if (n.isReference()) fn(n);
        walkRefs(n.children, fn);
    }
}

const GeometryNode* findGeo(const std::vector<GeometryNode>& nodes,
                            const std::string& name) {
    for (const GeometryNode& n : nodes) {
        if (n.name == name) return &n;
        if (const GeometryNode* hit = findGeo(n.children, name)) return hit;
    }
    return nullptr;
}


} // namespace

GdtfReport parseGdtfXml(const std::string& xml, const AttributeMap& attrMap) {
    GdtfReport rep;

    XmlDoc doc = parseXml(xml);
    if (!doc.ok) {
        rep.error = "xml: " + doc.error;
        return rep;
    }

    const XmlNode* ft = doc.root.name == "FixtureType" ? &doc.root
                                                       : doc.root.descend("FixtureType");
    if (!ft) {
        rep.error = "no FixtureType element";
        return rep;
    }

    rep.info.name = ft->attrOr("Name");
    rep.info.shortName = ft->attrOr("ShortName");
    rep.info.manufacturer = ft->attrOr("Manufacturer");
    rep.info.fixtureTypeId = ft->attrOr("FixtureTypeID");
    rep.info.description = ft->attrOr("Description");

    rep.info.def.manufacturer = rep.info.manufacturer;
    rep.info.def.name = rep.info.name;
    rep.info.def.shortName = rep.info.shortName;
    rep.info.def.typeId = rep.info.fixtureTypeId;
    rep.info.def.description = rep.info.description;

    // Geometry references are what make a fixture multi-instance: each one is
    // an instance of a shared geometry, which becomes a Lumen cell (§1.3).
    std::vector<std::string> cellGeometries;
    if (const XmlNode* geos = ft->descend("Geometries")) {
        std::function<void(const XmlNode&)> walk = [&](const XmlNode& n) {
            for (const auto& c : n.children) {
                if (c.name == "GeometryReference") {
                    const std::string nm = c.attrOr("Name");
                    if (!nm.empty() &&
                        std::find(cellGeometries.begin(), cellGeometries.end(), nm) ==
                            cellGeometries.end())
                        cellGeometries.push_back(nm);
                }
                walk(c);
            }
        };
        walk(*geos);

        // The same tree again, this time as the editable model (§1.2). The
        // flat channel list above is the compiled artifact; this is the source
        // the editor's views read, so the hierarchy has to survive import.
        std::function<std::vector<GeometryNode>(const XmlNode&)> build =
            [&](const XmlNode& n) {
                std::vector<GeometryNode> out;
                for (const auto& c : n.children) {
                    GeometryNode g;
                    g.name = c.attrOr("Name");
                    if (g.name.empty()) continue;

                    if (c.name == "GeometryReference") {
                        g.kind = GeometryKind::Reference;
                        g.referenced = c.attrOr("Geometry");
                        // GDTF's Break gives the instance's absolute start,
                        // 1-based. Without one the offset is inferred from the
                        // channels declared against this instance, below.
                        if (const XmlNode* br = c.kid("Break"))
                            g.dmxOffset = std::max(0, br->attrInt("DMXOffset", 1) - 1);
                    } else if (c.name == "Axis") {
                        g.kind = GeometryKind::Axis;
                    } else if (c.name == "Beam") {
                        g.kind = GeometryKind::Beam;
                    } else {
                        g.kind = GeometryKind::Normal;
                    }
                    g.children = build(c);
                    out.push_back(std::move(g));
                }
                return out;
            };
        rep.info.def.geometries = build(*geos);
    }

    const XmlNode* modes = ft->descend("DMXModes");
    if (!modes) {
        rep.error = "no DMXModes";
        return rep;
    }

    for (const XmlNode* mode : modes->kids("DMXMode")) {
        GdtfMode gm;
        gm.name = mode->attrOr("Name");
        gm.geometry = mode->attrOr("Geometry");
        gm.type.name = rep.info.name.empty()
                           ? gm.name
                           : rep.info.name + (gm.name.empty() ? "" : " / " + gm.name);

        const XmlNode* channels = mode->kid("DMXChannels");
        if (!channels) {
            rep.warnings.push_back("mode '" + gm.name + "' has no DMXChannels");
            rep.info.modes.push_back(std::move(gm));
            continue;
        }

        std::set<std::string> seenCellGeoms;

        // Collected alongside the flat channel list, with GDTF's ABSOLUTE
        // offsets, and turned into the editable mode after the loop — the
        // model's offsets are relative to their instance, which cannot be
        // worked out until every channel of that instance has been seen.
        struct RawChannel {
            Attr attr = Attr::Dimmer;
            std::string geometry;
            int coarse = 0;
            int fine = -1;
            uint8_t defaultValue = 0;
            std::vector<ChannelSetDef> sets;
        };
        std::vector<RawChannel> raw;

        for (const XmlNode* ch : channels->kids("DMXChannel")) {
            const auto offsets = parseOffsets(ch->attrOr("Offset"));
            const std::string geometry = ch->attrOr("Geometry");

            // A channel whose geometry is a reference belongs to that cell.
            CellId cell = 0;
            const auto cellIt =
                std::find(cellGeometries.begin(), cellGeometries.end(), geometry);
            if (cellIt != cellGeometries.end()) {
                cell = static_cast<CellId>(
                    std::distance(cellGeometries.begin(), cellIt) + 1);
                seenCellGeoms.insert(geometry);
            }

            // The attribute lives on the LogicalChannel, or on its first
            // ChannelFunction when the logical channel omits it.
            const XmlNode* logical = ch->kid("LogicalChannel");
            std::string gdtfAttr = logical ? logical->attrOr("Attribute") : std::string();
            const XmlNode* fn = logical ? logical->kid("ChannelFunction") : nullptr;
            if (gdtfAttr.empty() && fn) gdtfAttr = fn->attrOr("Attribute");
            gdtfAttr = lastSegment(gdtfAttr);

            if (gdtfAttr.empty() || offsets.empty()) {
                // A channel with no offset is a virtual channel in GDTF; it
                // has no DMX footprint, so there is nothing to patch.
                continue;
            }

            const auto mapped = attrMap.mapWithIndex(gdtfAttr);
            if (!mapped) {
                // §6.3: visible, not dropped.
                rep.unmapped.push_back({gdtfAttr, gm.name, offsets.front() + 1, geometry});
                continue;
            }

            ChannelDef def;
            def.attr = *mapped;
            def.cell = cell;
            def.coarse = offsets[0];
            def.fine = offsets.size() > 1 ? offsets[1] : -1;

            if (fn && fn->hasAttr("Default"))
                def.defaultValue = static_cast<uint8_t>(
                    std::lround(parseGdtfDmx(fn->attrOr("Default")) * 255.f));

            gm.type.channels.push_back(def);
            ++rep.channels;

            RawChannel rc;
            rc.attr = *mapped;
            rc.geometry = geometry;
            rc.coarse = def.coarse;
            rc.fine = def.fine;
            rc.defaultValue = def.defaultValue;
            raw.push_back(std::move(rc));

            // Channel sets become Lumen's range labels (§7.2), which is what
            // gives an imported profile working labels for free.
            if (!logical) continue;
            for (const XmlNode* cf : logical->kids("ChannelFunction")) {
                const double pf = cf->attrDouble("PhysicalFrom", 0.0);
                const double pt = cf->attrDouble("PhysicalTo", 1.0);
                const auto sets = cf->kids("ChannelSet");

                if (sets.empty()) {
                    // A function with no sets is itself a named range.
                    ChannelSet cs;
                    cs.attr = *mapped;
                    cs.cell = cell;
                    cs.name = cf->attrOr("Name");
                    cs.dmxFrom = static_cast<uint8_t>(
                        std::lround(parseGdtfDmx(cf->attrOr("DMXFrom", "0/1")) * 255.f));
                    cs.dmxTo = 255;
                    cs.physFrom = static_cast<float>(pf);
                    cs.physTo = static_cast<float>(pt);
                    cs.anchor = looksLikeAnchor(cs.name, cs.physFrom, cs.physTo);
                    gm.type.sets.push_back(cs);
                    if (!raw.empty())
                        raw.back().sets.push_back({cs.name, cs.dmxFrom, cs.dmxTo,
                                                   cs.physFrom, cs.physTo, cs.anchor});
                    ++rep.channelSets;
                    continue;
                }

                for (size_t i = 0; i < sets.size(); ++i) {
                    const XmlNode* set = sets[i];
                    ChannelSet cs;
                    cs.attr = *mapped;
                    cs.cell = cell;
                    cs.name = set->attrOr("Name");
                    cs.dmxFrom = static_cast<uint8_t>(
                        std::lround(parseGdtfDmx(set->attrOr("DMXFrom", "0/1")) * 255.f));
                    // GDTF gives only the start of a set; its end is the next
                    // set's start minus one.
                    if (i + 1 < sets.size()) {
                        const int next = static_cast<int>(std::lround(
                            parseGdtfDmx(sets[i + 1]->attrOr("DMXFrom", "0/1")) * 255.f));
                        cs.dmxTo = static_cast<uint8_t>(std::max(0, next - 1));
                    } else {
                        cs.dmxTo = 255;
                    }
                    cs.physFrom = static_cast<float>(
                        set->attrDouble("PhysicalFrom", pf));
                    cs.physTo = static_cast<float>(set->attrDouble("PhysicalTo", pt));
                    cs.anchor = looksLikeAnchor(cs.name, cs.physFrom, cs.physTo);
                    gm.type.sets.push_back(cs);
                    if (!raw.empty())
                        raw.back().sets.push_back({cs.name, cs.dmxFrom, cs.dmxTo,
                                                   cs.physFrom, cs.physTo, cs.anchor});
                    ++rep.channelSets;
                }
            }
        }

        gm.type.cellCount = static_cast<CellId>(seenCellGeoms.size());
        rep.cells += static_cast<int>(seenCellGeoms.size());

        // --- the editable mode (§1.2) --------------------------------------
        {
            ModeDef md;
            md.name = gm.name;
            md.rootGeometry = gm.geometry;

            // A reference whose target is missing gets one synthesised, so a
            // profile that omits the shared geometry still opens in the editor
            // instead of refusing to compile. Flagged, not hidden.
            std::vector<std::string> missing;
            walkRefs(rep.info.def.geometries, [&](GeometryNode& r) {
                if (r.referenced.empty()) return;
                if (findGeo(rep.info.def.geometries, r.referenced)) return;
                if (std::find(missing.begin(), missing.end(), r.referenced) ==
                    missing.end())
                    missing.push_back(r.referenced);
            });
            for (const std::string& name : missing) {
                GeometryNode g;
                g.name = name;
                g.kind = GeometryKind::Beam;
                rep.info.def.geometries.push_back(std::move(g));
                rep.warnings.push_back("profile references geometry '" + name +
                                       "' without defining it; a placeholder was "
                                       "added so it can be edited");
            }

            // GDTF offsets are absolute within the mode; the model's are
            // relative to the owning instance. Where a reference carried no
            // Break, its base is the lowest offset declared against it.
            std::map<std::string, int> base;
            walkRefs(rep.info.def.geometries, [&](GeometryNode& r) {
                if (r.dmxOffset > 0) {
                    base[r.name] = r.dmxOffset;
                    return;
                }
                int lowest = -1;
                for (const RawChannel& rc : raw) {
                    if (rc.geometry != r.name) continue;
                    lowest = lowest < 0 ? rc.coarse : std::min(lowest, rc.coarse);
                }
                if (lowest > 0) r.dmxOffset = lowest;
                base[r.name] = r.dmxOffset;
            });

            for (const RawChannel& rc : raw) {
                ChannelDecl c;
                c.attr = rc.attr;
                c.geometry = rc.geometry;
                const auto b = base.find(rc.geometry);
                const int bias = b == base.end() ? 0 : b->second;
                c.coarse = rc.coarse - bias;
                c.fine = rc.fine < 0 ? -1 : rc.fine - bias;
                c.defaultValue = rc.defaultValue;
                c.sets = rc.sets;
                md.channels.push_back(std::move(c));
            }
            rep.info.def.modes.push_back(std::move(md));

            // Collapse the per-instance declarations GDTF writes out one by
            // one, so a pixel bar is edited once rather than cell by cell.
            const NormaliseReport nr =
                normaliseInstances(rep.info.def, rep.info.def.modes.size() - 1);
            for (const auto& [target, why] : nr.kept)
                rep.warnings.push_back("cells of '" + target +
                                       "' were not collapsed: " + why);
        }

        rep.info.modes.push_back(std::move(gm));
    }

    if (rep.info.modes.empty()) {
        rep.error = "no usable DMX modes";
        return rep;
    }
    rep.ok = true;
    return rep;
}

GdtfReport loadGdtfFile(const std::string& path, const AttributeMap& attrMap) {
    GdtfReport rep;
    ZipReader zip;
    std::string err;
    if (!zip.openFile(path, &err)) {
        rep.error = "zip: " + err;
        return rep;
    }
    const auto xml = zip.readText("description.xml", &err);
    if (!xml) {
        rep.error = "description.xml: " + err;
        return rep;
    }
    return parseGdtfXml(*xml, attrMap);
}

} // namespace lumen
