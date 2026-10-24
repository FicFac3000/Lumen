// Mapping.hpp — pixel extraction, the one join between the two domains
// (SPEC §13.1–13.3).
//
// §13.1's rule, and the reason this file is small: RASTER DATA NEVER ENTERS
// THE ATTRIBUTE SYSTEM. Control drives raster parameters; the raster is
// sampled back into sparse colour values; those values join the merge like
// any other playback. Traffic runs one way across this boundary, and that is
// what keeps two million pixels out of a hash map that was built for twenty
// attributes per fixture.
//
//   cues / FX / programmer -> surface control attributes -> render
//                                                             |
//                                      sampled at sample points
//                                                             v
//                                     sparse colour values -> merge -> DMX
//
// What is deliberately NOT here: decoding video, running shaders, talking to
// a GPU. Those need a renderer and a machine with a screen. The renderer
// fills a RasterFrame; everything below works on one whatever produced it,
// so the GPU path slots in behind the same interface without this changing.
#pragma once

#include "lumen/Patch.hpp"
#include "lumen/Selection.hpp"
#include "lumen/Uid.hpp"
#include "lumen/Value.hpp"

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace lumen {

// ---------------------------------------------------------------------------
// The raster side
// ---------------------------------------------------------------------------
// A composited frame, as the renderer hands it over. RGB8 because that is
// what a DMX fixture can use and what a GPU readback gives cheaply; anything
// wider would be thrown away at the first sample.
//
// Rows are top-down, which matches every image format and every GPU readback
// worth having, so mapping space has y increasing DOWNWARD too (below). The
// alternative — flipping once, somewhere — is how content ends up upside down
// in exactly one of the preview and the output.
class RasterFrame {
public:
    RasterFrame() = default;
    RasterFrame(int width, int height);

    int width() const { return w_; }
    int height() const { return h_; }
    bool empty() const { return w_ <= 0 || h_ <= 0; }

    void resize(int width, int height);
    void fill(uint8_t r, uint8_t g, uint8_t b);

    struct Rgb {
        uint8_t r = 0, g = 0, b = 0;
    };

    void set(int x, int y, Rgb c);
    // Out of bounds returns black rather than wrapping or clamping: a sample
    // point that has drifted off the surface should go dark, not pick up the
    // edge pixel and look deliberate.
    Rgb at(int x, int y) const;

    const std::vector<uint8_t>& bytes() const { return px_; }
    std::vector<uint8_t>& bytes() { return px_; }

    // Frames are versioned rather than diffed: extraction only has to know
    // whether this is the same frame it sampled last time.
    uint64_t version() const { return version_; }
    void touch() { ++version_; }

private:
    int w_ = 0, h_ = 0;
    std::vector<uint8_t> px_;
    uint64_t version_ = 0;
};

// ---------------------------------------------------------------------------
// The control side
// ---------------------------------------------------------------------------
// §13.2: a mapped LED pixel is not a new object. It is an ordinary patched
// fixture that additionally carries a SAMPLE POINT in mapping space. Nothing
// about the fixture changes; it gains a position to be sampled at.
//
// Mapping space is normalised 0..1 across the surface, x right, y DOWN (see
// RasterFrame). Normalised so a sample layout survives a change of output
// resolution — the thing that otherwise breaks every mapping the first time
// a projector is swapped.
enum class SampleArea : uint8_t {
    Point,     // one pixel: sharp, and jumpy on detailed content
    Box,       // mean over a square: the usual choice
    Gaussian,  // weighted: softest, and the one that survives fine texture
};

const char* sampleAreaName(SampleArea a);
std::optional<SampleArea> sampleAreaFromName(const std::string& s);

struct SamplePoint {
    Target target;          // fixture, and cell for a multi-instance strip
    float x = 0.5f;
    float y = 0.5f;
    SampleArea area = SampleArea::Box;
    // Radius in mapping units. A box of 0.01 on a 1920-wide surface is ~19px
    // across, which is about right for an LED reading an area rather than a
    // texel.
    float radius = 0.01f;
    bool enabled = true;
};

// Samples one point out of a frame. Separate from the store so it can be
// tested on its own and so a renderer thread can call it without touching
// show state.
RasterFrame::Rgb samplePoint(const RasterFrame& frame, const SamplePoint& p);

// ---------------------------------------------------------------------------
// Layout tools (§13.2)
// ---------------------------------------------------------------------------
// Placing a hundred sample points by hand is how mappings do not get made.
// Each of these returns points in SELECTION ORDER, because that order is
// already the thing the rest of the console treats as information (§20.4) —
// a strip laid along a line fans and chases in the order it was laid.
std::vector<SamplePoint> layoutGrid(const std::vector<Target>& targets, int columns,
                                    float x0, float y0, float x1, float y1);
std::vector<SamplePoint> layoutLine(const std::vector<Target>& targets, float x0,
                                    float y0, float x1, float y1);
// A cubic bezier through two control points, which is enough for the curves
// an LED strip is actually bent into.
std::vector<SamplePoint> layoutBezier(const std::vector<Target>& targets, float x0,
                                      float y0, float cx1, float cy1, float cx2,
                                      float cy2, float x1, float y1);

// ---------------------------------------------------------------------------
// Extraction layer (§13.3)
// ---------------------------------------------------------------------------
// The thing neither reference tool can do: extraction enters the merge as a
// PLAYBACK LAYER with a priority and a merge rule, so a video look can HTP
// over a conventional wash, or a cue can take priority over mapped content,
// without leaving the console.
struct ExtractionReport {
    int sampled = 0;        // points that produced a value
    int offSurface = 0;     // points outside the frame, which go dark
    int disabled = 0;
    bool hadFrame = false;
};

class Extraction {
public:
    Uid uid{};
    // 0 means "not assigned yet": add() mints the next free id. A default of
    // 1 would make an unset id indistinguishable from a deliberate one, and
    // two layers would silently share it — the same trap Surface fell into.
    int id = 0;
    std::string name;

    // §11.4's three mechanisms, the same ones an executor has — which is the
    // point: mapped content is a playback, not a special case.
    int priority = 0;
    MergeRule merge = MergeRule::LTP;
    float master = 1.f;     // an intensity master, so mapped content fades
    bool enabled = true;

    std::vector<SamplePoint> points;

    SamplePoint* pointFor(const Target& t);
    const SamplePoint* pointFor(const Target& t) const;
    void setPoints(std::vector<SamplePoint> ps) { points = std::move(ps); }

    // Samples the frame and writes Red/Green/Blue for each point's target.
    // The master scales the result here rather than in the merge, so a faded
    // extraction contributes faded values to HTP instead of winning at full
    // and being scaled afterwards.
    ExtractionReport render(const RasterFrame& frame, OutputLayer& out) const;
};

class ExtractionStore {
public:
    explicit ExtractionStore(UidGen& uids) : uids_(&uids) {}

    Extraction& add(Extraction e);
    Extraction& restore(Extraction e);
    Extraction* byId(int id);
    const Extraction* byId(int id) const;
    Extraction* byName(const std::string& name);
    const std::deque<Extraction>& all() const { return extractions_; }
    size_t size() const { return extractions_.size(); }
    bool remove(int id);

    // Every layer, in priority order, merged into `out` — which already
    // holds the executors' contribution. Each layer applies ITS OWN merge
    // rule against whatever is underneath, whether that came from another
    // extraction or from a cue. That is §13.3's claim made real: mapped
    // content and conventional playback share one priority space and one set
    // of rules, so "let this cue win over the video" is the same operation
    // as "let this cue win over that cue".
    ExtractionReport mergeInto(const RasterFrame& frame, OutputLayer& out) const;

private:
    UidGen* uids_;
    std::deque<Extraction> extractions_;
};

} // namespace lumen
