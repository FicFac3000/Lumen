// Mvr.hpp — MVR import (SPEC §17.5).
//
// MVR (My Virtual Rig) is GDTF's companion format and carries fixtures,
// addresses, positions, rotations AND the GDTF profiles themselves in one
// file. Vectorworks, Depence and Capture all export it, so a designer's plot
// imports complete — patch, positions and profiles in one step rather than
// retyped. That is why §17.5 calls it a priority.
//
// It is a ZIP containing GeneralSceneDescription.xml plus the .gdtf files it
// references, so this sits on top of the Zip, Xml and Gdtf work.
#pragma once

#include "lumen/Gdtf.hpp"
#include "lumen/Group.hpp"
#include "lumen/Patch.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace lumen {

struct MvrFixture {
    std::string name;
    std::string uuid;
    std::string gdtfSpec;   // the .gdtf filename inside the archive
    std::string gdtfMode;
    std::string layer;

    FixtureId fixtureId = 0;
    int universe = 1;
    int address = 1;

    // MVR positions are in MILLIMETRES; these are converted to metres, which
    // is what the stage model uses.
    float x = 0.f, y = 0.f, z = 0.f;
    float rotX = 0.f, rotY = 0.f, rotZ = 0.f;  // degrees
    bool positioned = false;
};

struct MvrReport {
    bool ok = false;
    std::string error;

    std::vector<MvrFixture> fixtures;
    std::vector<std::string> layers;  // in document order

    // Parsed profiles, keyed by the GDTFSpec filename they were found under.
    std::map<std::string, GdtfInfo> profiles;

    std::vector<std::string> warnings;
};

MvrReport loadMvrFile(const std::string& path, const AttributeMap& map = AttributeMap{});
// Parses a scene description without an archive, for a caller that already
// has the XML (and therefore no embedded profiles to resolve).
MvrReport parseMvrXml(const std::string& xml);

// Applies a parsed MVR into a patch: registers the profiles as fixture types,
// patches the fixtures with their addresses and positions, and turns each
// layer into a group.
struct MvrApplyReport {
    int typesAdded = 0;
    int patched = 0;
    int skipped = 0;
    int groupsCreated = 0;
    std::vector<std::string> warnings;
};

MvrApplyReport applyMvr(Patch& patch, GroupStore* groups, const MvrReport& mvr);

// ---------------------------------------------------------------------------
// Pieces worth testing on their own
// ---------------------------------------------------------------------------
// An MVR matrix is four bracketed vectors: three rotation columns then the
// translation, e.g. "{1,0,0}{0,1,0}{0,0,1}{1000,2000,3000}".
struct MvrMatrix {
    float m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    float tx = 0.f, ty = 0.f, tz = 0.f;  // millimetres, as written
    bool parsed = false;
};

MvrMatrix parseMvrMatrix(const std::string& text);
// Extracts ZYX Euler angles in degrees from the rotation part.
void eulerFromMatrix(const MvrMatrix& m, float& rx, float& ry, float& rz);

// MVR writes an absolute DMX address, or occasionally "universe.channel".
// Returns 1-based universe and channel.
void decomposeAddress(const std::string& text, int& universe, int& channel);

} // namespace lumen
