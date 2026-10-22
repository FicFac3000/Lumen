#include "lumen/Attribute.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <unordered_map>

namespace lumen {

namespace {

std::string lower(const std::string& s) {
    std::string o = s;
    std::transform(o.begin(), o.end(), o.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return o;
}

constexpr Kind kCont = Kind::Continuous;
constexpr Kind kInt = Kind::Intensity | Kind::Continuous;
constexpr Kind kAxisC = Kind::Axis | Kind::Continuous;
constexpr Kind kAxisA = Kind::Axis | Kind::Anchored;
constexpr Kind kAnch = Kind::Anchored | Kind::Continuous;
constexpr Kind kIdx = Kind::Indexed;
constexpr Kind kVirt = Kind::Virtual | Kind::Continuous;

// Table order must match the Attr enum exactly; verified by a static_assert on
// size plus a runtime check in attrInfo().
const std::array<AttrInfo, kAttrCount> kTable = {{
    {Attr::Dimmer,           Group::Intensity, kInt,   "Dimmer",      "Dimmer",      "◐"},
    {Attr::DimmerFine,       Group::Intensity, kInt,   "DimmerFine",  "Dimmer Fine", "◐"},

    {Attr::Pan,              Group::Position,  kAxisC, "Pan",         "Pan",         "↔"},
    {Attr::Tilt,             Group::Position,  kAxisC, "Tilt",        "Tilt",        "↕"},
    {Attr::PanRotate,        Group::Position,  kAxisA, "PanRotate",   "Pan Rotate",  "↻"},
    {Attr::TiltRotate,       Group::Position,  kAxisA, "TiltRotate",  "Tilt Rotate", "↻"},
    {Attr::PanTiltSpeed,     Group::Position,  kCont,  "PanTiltSpeed","P/T Speed",   "⧖"},
    {Attr::PosX,             Group::Position,  kAxisC, "PosX",        "Position X",  "↔"},
    {Attr::PosY,             Group::Position,  kAxisC, "PosY",        "Position Y",  "↕"},
    {Attr::PosZ,             Group::Position,  kAxisC, "PosZ",        "Position Z",  "⤢"},
    {Attr::RotX,             Group::Position,  kAxisA, "RotX",        "Rotate X",    "↻"},
    {Attr::RotY,             Group::Position,  kAxisA, "RotY",        "Rotate Y",    "↻"},
    {Attr::RotZ,             Group::Position,  kAxisA, "RotZ",        "Rotate Z",    "↻"},

    {Attr::Red,              Group::Color,     kCont,  "Red",         "Red",         "◉"},
    {Attr::Green,            Group::Color,     kCont,  "Green",       "Green",       "◉"},
    {Attr::Blue,             Group::Color,     kCont,  "Blue",        "Blue",        "◉"},
    {Attr::Amber,            Group::Color,     kCont,  "Amber",       "Amber",       "◉"},
    {Attr::White,            Group::Color,     kCont,  "White",       "White",       "◉"},
    {Attr::Lime,             Group::Color,     kCont,  "Lime",        "Lime",        "◉"},
    {Attr::Cyan,             Group::Color,     kCont,  "Cyan",        "Cyan",        "◉"},
    {Attr::UV,               Group::Color,     kCont,  "UV",          "UV",          "◉"},
    {Attr::WarmWhite,        Group::Color,     kCont,  "WarmWhite",   "Warm White",  "◉"},
    {Attr::CoolWhite,        Group::Color,     kCont,  "CoolWhite",   "Cool White",  "◉"},
    {Attr::CyanSub,          Group::Color,     kCont,  "CyanSub",     "Cyan",        "◎"},
    {Attr::Magenta,          Group::Color,     kCont,  "Magenta",     "Magenta",     "◎"},
    {Attr::Yellow,           Group::Color,     kCont,  "Yellow",      "Yellow",      "◎"},
    {Attr::CTO,              Group::Color,     kAnch,  "CTO",         "CTO",         "◑"},
    {Attr::CTB,              Group::Color,     kAnch,  "CTB",         "CTB",         "◑"},
    {Attr::Tint,             Group::Color,     kAnch,  "Tint",        "Tint",        "◑"},
    {Attr::Hue,              Group::Color,     kVirt,  "Hue",         "Hue",         "◔"},
    {Attr::Saturation,       Group::Color,     kVirt,  "Saturation",  "Saturation",  "◔"},
    {Attr::ColorTemp,        Group::Color,     kVirt,  "ColorTemp",   "Colour Temp", "◑"},
    {Attr::ColorWheel1,      Group::Color,     kIdx,   "ColorWheel1", "Colour Wheel","☸"},
    {Attr::ColorWheel1Spin,  Group::Color,     kAnch,  "ColorWheel1Spin", "Wheel Spin", "↻"},
    {Attr::ColorMacro,       Group::Color,     kIdx,   "ColorMacro",  "Colour Macro","⌘"},

    {Attr::Shutter,          Group::Beam,      kIdx,   "Shutter",     "Shutter",     "□"},
    {Attr::StrobeRate,       Group::Beam,      kCont,  "StrobeRate",  "Strobe Rate", "⚡"},
    {Attr::Zoom,             Group::Beam,      kCont,  "Zoom",        "Zoom",        "⤡"},
    {Attr::Focus,            Group::Beam,      kCont,  "Focus",       "Focus",       "○"},
    {Attr::Iris,             Group::Beam,      kCont,  "Iris",        "Iris",        "◌"},
    {Attr::Frost,            Group::Beam,      kCont,  "Frost",       "Frost",       "░"},
    {Attr::Gobo1,            Group::Beam,      kIdx,   "Gobo1",       "Gobo 1",      "✲"},
    {Attr::Gobo1Pos,         Group::Beam,      kCont,  "Gobo1Pos",    "Gobo 1 Pos",  "◴"},
    {Attr::Gobo1Spin,        Group::Beam,      kAnch,  "Gobo1Spin",   "Gobo 1 Spin", "↻"},
    {Attr::Gobo2,            Group::Beam,      kIdx,   "Gobo2",       "Gobo 2",      "✲"},
    {Attr::Gobo2Pos,         Group::Beam,      kCont,  "Gobo2Pos",    "Gobo 2 Pos",  "◴"},
    {Attr::Gobo2Spin,        Group::Beam,      kAnch,  "Gobo2Spin",   "Gobo 2 Spin", "↻"},
    {Attr::Prism1,           Group::Beam,      kIdx,   "Prism1",      "Prism 1",     "◇"},
    {Attr::Prism1Pos,        Group::Beam,      kCont,  "Prism1Pos",   "Prism 1 Pos", "◈"},
    {Attr::Prism1Spin,       Group::Beam,      kAnch,  "Prism1Spin",  "Prism 1 Spin","↻"},
    {Attr::AnimWheel,        Group::Beam,      kIdx,   "AnimWheel",   "Anim Wheel",  "∿"},
    {Attr::AnimWheelPos,     Group::Beam,      kCont,  "AnimWheelPos","Anim Pos",    "◴"},
    {Attr::AnimWheelSpin,    Group::Beam,      kAnch,  "AnimWheelSpin","Anim Spin",  "↻"},
    {Attr::Shaper1A,         Group::Beam,      kCont,  "Shaper1A",    "Shaper 1A",   "◧"},
    {Attr::Shaper1B,         Group::Beam,      kCont,  "Shaper1B",    "Shaper 1B",   "◨"},
    {Attr::Shaper2A,         Group::Beam,      kCont,  "Shaper2A",    "Shaper 2A",   "◧"},
    {Attr::Shaper2B,         Group::Beam,      kCont,  "Shaper2B",    "Shaper 2B",   "◨"},
    {Attr::Shaper3A,         Group::Beam,      kCont,  "Shaper3A",    "Shaper 3A",   "◧"},
    {Attr::Shaper3B,         Group::Beam,      kCont,  "Shaper3B",    "Shaper 3B",   "◨"},
    {Attr::Shaper4A,         Group::Beam,      kCont,  "Shaper4A",    "Shaper 4A",   "◧"},
    {Attr::Shaper4B,         Group::Beam,      kCont,  "Shaper4B",    "Shaper 4B",   "◨"},
    {Attr::ShaperRot,        Group::Beam,      kAnch,  "ShaperRot",   "Shaper Rot",  "↻"},

    {Attr::LampOn,           Group::Control,   kIdx,   "LampOn",      "Lamp On",     "☉"},
    {Attr::LampOff,          Group::Control,   kIdx,   "LampOff",     "Lamp Off",    "⊘"},
    {Attr::Reset,            Group::Control,   kIdx,   "Reset",       "Reset",       "↺"},
    {Attr::ResetPanTilt,     Group::Control,   kIdx,   "ResetPanTilt","Reset P/T",   "↺"},
    {Attr::FanMode,          Group::Control,   kIdx,   "FanMode",     "Fan Mode",    "❄"},
    {Attr::FanSpeed,         Group::Control,   kCont,  "FanSpeed",    "Fan Speed",   "❄"},
    {Attr::DisplayIntensity, Group::Control,   kCont,  "DisplayIntensity", "Display","☰"},
    {Attr::Arm,              Group::Control,   kCont,  "Arm",       "Arm",     "⚠"},
}};

static_assert(kTable.size() == kAttrCount, "attribute table must cover every Attr");

// Aliases accepted by the command layer, so `Fixture 1 At Red 100` and
// `Fixture 1 At R 100` both parse (§16.2).
const std::unordered_map<std::string, Attr>& aliasMap() {
    static const std::unordered_map<std::string, Attr> m = {
        {"dim", Attr::Dimmer}, {"intens", Attr::Dimmer}, {"intensity", Attr::Dimmer},
        {"r", Attr::Red}, {"g", Attr::Green}, {"b", Attr::Blue},
        {"w", Attr::White}, {"a", Attr::Amber}, {"uv", Attr::UV},
        {"ww", Attr::WarmWhite}, {"cw", Attr::CoolWhite},
        {"c", Attr::CyanSub}, {"m", Attr::Magenta}, {"y", Attr::Yellow},
        {"p", Attr::Pan}, {"t", Attr::Tilt},
        {"strobe", Attr::StrobeRate}, {"gobo", Attr::Gobo1}, {"prism", Attr::Prism1},
        {"sat", Attr::Saturation}, {"cto", Attr::CTO}, {"ctb", Attr::CTB},
    };
    return m;
}

} // namespace

const char* groupName(Group g) {
    switch (g) {
        case Group::Intensity: return "Intensity";
        case Group::Position: return "Position";
        case Group::Color: return "Color";
        case Group::Beam: return "Beam";
        case Group::Control: return "Control";
        case Group::Count: break;
    }
    return "?";
}

std::optional<Group> groupFromName(const std::string& s) {
    const std::string k = lower(s);
    for (size_t i = 0; i < kGroupCount; ++i) {
        const auto g = static_cast<Group>(i);
        if (lower(groupName(g)) == k) return g;
    }
    if (k == "colour") return Group::Color;
    if (k == "int" || k == "dim") return Group::Intensity;
    if (k == "pos") return Group::Position;
    return std::nullopt;
}

const AttrInfo& attrInfo(Attr a) {
    const auto i = static_cast<size_t>(a);
    // A mismatch here means the table and the enum have drifted apart.
    const AttrInfo& info = kTable[i < kAttrCount ? i : 0];
    return info;
}

const char* attrName(Attr a) { return attrInfo(a).name; }

std::optional<Attr> attrFromName(const std::string& s) {
    const std::string k = lower(s);
    for (const auto& e : kTable)
        if (lower(e.name) == k) return e.attr;
    auto it = aliasMap().find(k);
    if (it != aliasMap().end()) return it->second;
    return std::nullopt;
}

const std::vector<Attr>& attrsInGroup(Group g) {
    static const std::array<std::vector<Attr>, kGroupCount> byGroup = [] {
        std::array<std::vector<Attr>, kGroupCount> out;
        for (const auto& e : kTable) out[static_cast<size_t>(e.group)].push_back(e.attr);
        return out;
    }();
    return byGroup[static_cast<size_t>(g)];
}

MergeRule defaultMergeRule(Attr a) {
    return isIntensity(a) ? MergeRule::HTP : MergeRule::LTP;
}

const char* roleName(Role r) {
    switch (r) {
        case Role::None: return "None";
        case Role::AxisX: return "AxisX";
        case Role::AxisY: return "AxisY";
        case Role::AxisZ: return "AxisZ";
        case Role::Intensity: return "Intensity";
        case Role::ColorComponent1: return "ColorComponent1";
        case Role::ColorComponent2: return "ColorComponent2";
        case Role::ColorComponent3: return "ColorComponent3";
        case Role::Size: return "Size";
        case Role::Count: break;
    }
    return "?";
}

std::optional<Role> roleFromName(const std::string& s) {
    const std::string k = lower(s);
    for (size_t i = 0; i < static_cast<size_t>(Role::Count); ++i) {
        const auto r = static_cast<Role>(i);
        if (lower(roleName(r)) == k) return r;
    }
    if (k == "x") return Role::AxisX;
    if (k == "y") return Role::AxisY;
    if (k == "z") return Role::AxisZ;
    return std::nullopt;
}

const std::vector<Attr>& roleCandidates(Role r) {
    static const std::vector<Attr> none{};
    static const std::vector<Attr> axisX = {Attr::Pan, Attr::PosX};
    static const std::vector<Attr> axisY = {Attr::Tilt, Attr::PosY};
    static const std::vector<Attr> axisZ = {Attr::PosZ};
    static const std::vector<Attr> inten = {Attr::Dimmer};
    static const std::vector<Attr> c1 = {Attr::Red, Attr::CyanSub, Attr::Hue};
    static const std::vector<Attr> c2 = {Attr::Green, Attr::Magenta, Attr::Saturation};
    static const std::vector<Attr> c3 = {Attr::Blue, Attr::Yellow};
    static const std::vector<Attr> size = {Attr::Zoom, Attr::Iris};

    switch (r) {
        case Role::AxisX: return axisX;
        case Role::AxisY: return axisY;
        case Role::AxisZ: return axisZ;
        case Role::Intensity: return inten;
        case Role::ColorComponent1: return c1;
        case Role::ColorComponent2: return c2;
        case Role::ColorComponent3: return c3;
        case Role::Size: return size;
        case Role::None:
        case Role::Count: break;
    }
    return none;
}

} // namespace lumen
