// Attribute.hpp — Lumen's own attribute taxonomy (SPEC §6).
//
// This model is native. GDTF, ChamSys .hed, laser zones and pixel-mapper cells
// are all IMPORT MAPPINGS onto it. Sorting, filtering, preset organisation, FX
// role binding and UI grouping read this, never a source profile's vocabulary.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lumen {

// ---------------------------------------------------------------------------
// Main groups (§6.1). Five, not three: Intensity and Control are separate
// because the engine treats them specially — Intensity is HTP-merged and
// master-scaled, Control is kept out of the encoder rotation.
// ---------------------------------------------------------------------------
enum class Group : uint8_t {
    Intensity = 0,
    Position,
    Color,
    Beam,
    Control,
    Count
};

inline constexpr size_t kGroupCount = static_cast<size_t>(Group::Count);

const char* groupName(Group g);
std::optional<Group> groupFromName(const std::string& s);

// ---------------------------------------------------------------------------
// Kind flags (§6.2). Declared once here; read everywhere. These are flags, not
// an enum: Pan is {Continuous, Axis}, PanRotate is {Anchored, Axis}.
// ---------------------------------------------------------------------------
enum class Kind : uint16_t {
    None       = 0,
    Continuous = 1u << 0,  // plain analogue range
    Intensity  = 1u << 1,  // HTP-merged, scaled by grand master and blackout
    Anchored   = 1u << 2,  // range has a semantic zero (stop/centre)
    Indexed    = 1u << 3,  // discrete slots, not continuous
    Virtual    = 1u << 4,  // derived; no DMX channel of its own
    Axis       = 1u << 5,  // spatial; bindable to the AxisX/AxisY FX roles
};

constexpr Kind operator|(Kind a, Kind b) {
    return static_cast<Kind>(static_cast<uint16_t>(a) | static_cast<uint16_t>(b));
}
constexpr bool has(Kind mask, Kind flag) {
    return (static_cast<uint16_t>(mask) & static_cast<uint16_t>(flag)) != 0;
}

// ---------------------------------------------------------------------------
// Sub-attributes (§6.1).
// ---------------------------------------------------------------------------
enum class Attr : uint16_t {
    // Intensity
    Dimmer = 0,
    DimmerFine,

    // Position
    Pan,
    Tilt,
    PanRotate,       // continuous rotation: direction + speed, anchored
    TiltRotate,
    PanTiltSpeed,
    PosX, PosY, PosZ,
    RotX, RotY, RotZ,

    // Color — additive
    Red, Green, Blue, Amber, White, Lime, Cyan, UV, WarmWhite, CoolWhite,
    // Color — subtractive
    CyanSub, Magenta, Yellow,
    // Color — correction
    CTO, CTB, Tint,
    // Color — model (virtual: derived, no channel of their own)
    Hue, Saturation, ColorTemp,
    // Color — discrete
    ColorWheel1, ColorWheel1Spin, ColorMacro,

    // Beam — shutter
    Shutter, StrobeRate,
    // Beam — optics
    Zoom, Focus, Iris, Frost,
    // Beam — gobo
    Gobo1, Gobo1Pos, Gobo1Spin,
    Gobo2, Gobo2Pos, Gobo2Spin,
    // Beam — prism
    Prism1, Prism1Pos, Prism1Spin,
    // Beam — animation
    AnimWheel, AnimWheelPos, AnimWheelSpin,
    // Beam — shapers / framing
    Shaper1A, Shaper1B, Shaper2A, Shaper2B,
    Shaper3A, Shaper3B, Shaper4A, Shaper4B,
    ShaperRot,

    // Control — never in the encoder rotation; press-and-confirm only
    LampOn, LampOff, Reset, ResetPanTilt, FanMode, FanSpeed, DisplayIntensity,

    Count
};

inline constexpr size_t kAttrCount = static_cast<size_t>(Attr::Count);

struct AttrInfo {
    Attr attr;
    Group group;
    Kind kind;
    const char* name;    // canonical, stable; used by the command layer
    const char* pretty;  // display label (§7.1's label layer)
    const char* symbol;  // display glyph (§7.1's symbol layer)
};

const AttrInfo& attrInfo(Attr a);
const char* attrName(Attr a);
std::optional<Attr> attrFromName(const std::string& s);  // accepts aliases

inline Group groupOf(Attr a) { return attrInfo(a).group; }
inline Kind kindOf(Attr a) { return attrInfo(a).kind; }
inline bool isIntensity(Attr a) { return has(kindOf(a), Kind::Intensity); }
inline bool isVirtual(Attr a) { return has(kindOf(a), Kind::Virtual); }
inline bool isAxis(Attr a) { return has(kindOf(a), Kind::Axis); }
inline bool isAnchored(Attr a) { return has(kindOf(a), Kind::Anchored); }

// Every attribute in a group, in declaration order. Used by the encoder bar
// (§2.6) and by the per-group tracking masks (§11.1).
const std::vector<Attr>& attrsInGroup(Group g);

// ---------------------------------------------------------------------------
// Merge rule (§11.4). Reads the `intensity` kind flag rather than testing for
// a specific attribute, so a user-configured HTP attribute behaves correctly.
// ---------------------------------------------------------------------------
enum class MergeRule : uint8_t { HTP, LTP };

MergeRule defaultMergeRule(Attr a);

// ---------------------------------------------------------------------------
// FX roles (§4.2). A recipe slot may target a concrete attribute or a role;
// roles resolve per fixture at apply time, which is what lets one Circle
// recipe drive a moving head, a laser zone and a video surface.
// ---------------------------------------------------------------------------
enum class Role : uint8_t {
    None = 0,
    AxisX,
    AxisY,
    AxisZ,
    Intensity,
    ColorComponent1,
    ColorComponent2,
    ColorComponent3,
    Size,
    Count
};

const char* roleName(Role r);
std::optional<Role> roleFromName(const std::string& s);

// Candidate attributes for a role, in preference order. Resolution picks the
// first the fixture actually has; where it has none, the SLOT is skipped for
// that fixture, not the fixture for the whole recipe (§4.2).
const std::vector<Attr>& roleCandidates(Role r);

} // namespace lumen
