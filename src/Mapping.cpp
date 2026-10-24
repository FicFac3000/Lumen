#include "lumen/Mapping.hpp"

#include <algorithm>
#include <cmath>

namespace lumen {

// ---------------------------------------------------------------------------
// RasterFrame
// ---------------------------------------------------------------------------
RasterFrame::RasterFrame(int width, int height) { resize(width, height); }

void RasterFrame::resize(int width, int height) {
    w_ = std::max(0, width);
    h_ = std::max(0, height);
    px_.assign(static_cast<size_t>(w_) * static_cast<size_t>(h_) * 3u, 0u);
    ++version_;
}

void RasterFrame::fill(uint8_t r, uint8_t g, uint8_t b) {
    for (size_t i = 0; i + 2 < px_.size(); i += 3) {
        px_[i] = r;
        px_[i + 1] = g;
        px_[i + 2] = b;
    }
    ++version_;
}

void RasterFrame::set(int x, int y, Rgb c) {
    if (x < 0 || y < 0 || x >= w_ || y >= h_) return;
    const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w_) +
                      static_cast<size_t>(x)) * 3u;
    px_[i] = c.r;
    px_[i + 1] = c.g;
    px_[i + 2] = c.b;
}

RasterFrame::Rgb RasterFrame::at(int x, int y) const {
    // Out of bounds is BLACK, not clamped. A sample point that has drifted
    // off the surface should go dark rather than pick up the edge pixel and
    // look like it was meant.
    if (x < 0 || y < 0 || x >= w_ || y >= h_) return Rgb{};
    const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w_) +
                      static_cast<size_t>(x)) * 3u;
    return Rgb{px_[i], px_[i + 1], px_[i + 2]};
}

// ---------------------------------------------------------------------------
// Sampling
// ---------------------------------------------------------------------------
const char* sampleAreaName(SampleArea a) {
    switch (a) {
        case SampleArea::Point: return "point";
        case SampleArea::Box: return "box";
        case SampleArea::Gaussian: return "gaussian";
    }
    return "?";
}

std::optional<SampleArea> sampleAreaFromName(const std::string& s) {
    if (s == "point") return SampleArea::Point;
    if (s == "box") return SampleArea::Box;
    if (s == "gaussian") return SampleArea::Gaussian;
    return std::nullopt;
}

namespace {

// Mapping space is normalised, so a radius in mapping units becomes a
// different number of pixels on every surface — which is the point: a layout
// survives a change of output resolution.
int radiusInPixels(float radius, int span) {
    return std::max(0, static_cast<int>(std::lround(radius * static_cast<float>(span))));
}

} // namespace

RasterFrame::Rgb samplePoint(const RasterFrame& frame, const SamplePoint& p) {
    if (frame.empty() || !p.enabled) return RasterFrame::Rgb{};

    const int cx = static_cast<int>(std::lround(p.x * static_cast<float>(frame.width() - 1)));
    const int cy = static_cast<int>(std::lround(p.y * static_cast<float>(frame.height() - 1)));

    if (p.area == SampleArea::Point) return frame.at(cx, cy);

    const int rx = radiusInPixels(p.radius, frame.width());
    const int ry = radiusInPixels(p.radius, frame.height());
    // A radius that rounds to nothing is a point sample, not an empty average.
    if (rx == 0 && ry == 0) return frame.at(cx, cy);

    double sr = 0, sg = 0, sb = 0, sw = 0;
    const double sigma = std::max(1.0, static_cast<double>(std::max(rx, ry)) / 2.0);
    const double twoSigmaSq = 2.0 * sigma * sigma;

    for (int dy = -ry; dy <= ry; ++dy) {
        for (int dx = -rx; dx <= rx; ++dx) {
            const int x = cx + dx;
            const int y = cy + dy;
            // Pixels outside the frame are EXCLUDED from the average, not
            // counted as black. The distinction matters and it is easy to get
            // backwards: a sample window hanging off the edge of the buffer
            // is an artifact of the buffer ending, not of the fixture being
            // off the content. Content does not stop at the edge — the
            // texture does. A fixture mapped to the very edge of a white
            // surface should read white, not grey.
            //
            // A point whose CENTRE is off the surface is the real mapping
            // problem, and that is handled separately: it goes dark and is
            // counted (Extraction::render).
            if (x < 0 || y < 0 || x >= frame.width() || y >= frame.height())
                continue;
            const RasterFrame::Rgb c = frame.at(x, y);
            double w = 1.0;
            if (p.area == SampleArea::Gaussian) {
                const double d2 = static_cast<double>(dx * dx + dy * dy);
                w = std::exp(-d2 / twoSigmaSq);
            }
            sr += c.r * w;
            sg += c.g * w;
            sb += c.b * w;
            sw += w;
        }
    }
    if (sw <= 0.0) return frame.at(cx, cy);

    auto byte = [](double v) {
        return static_cast<uint8_t>(std::clamp(std::lround(v), 0L, 255L));
    };
    return RasterFrame::Rgb{byte(sr / sw), byte(sg / sw), byte(sb / sw)};
}

// ---------------------------------------------------------------------------
// Layout tools
// ---------------------------------------------------------------------------
namespace {

SamplePoint makePoint(const Target& t, float x, float y) {
    SamplePoint p;
    p.target = t;
    p.x = std::clamp(x, 0.f, 1.f);
    p.y = std::clamp(y, 0.f, 1.f);
    return p;
}

// A single point gets the START of the span rather than its middle. Placing
// one fixture halfway along the line somebody drew is a surprise; placing it
// where they started drawing is not.
float lerpOver(float a, float b, size_t i, size_t n) {
    if (n <= 1) return a;
    return a + (b - a) * (static_cast<float>(i) / static_cast<float>(n - 1));
}

} // namespace

std::vector<SamplePoint> layoutGrid(const std::vector<Target>& targets, int columns,
                                    float x0, float y0, float x1, float y1) {
    std::vector<SamplePoint> out;
    if (targets.empty() || columns <= 0) return out;

    const int cols = std::min(columns, static_cast<int>(targets.size()));
    const int rows = static_cast<int>((targets.size() + cols - 1) / cols);

    for (size_t i = 0; i < targets.size(); ++i) {
        const int col = static_cast<int>(i) % cols;
        const int row = static_cast<int>(i) / cols;
        // Cell CENTRES, not corners: a grid of sample points sitting on the
        // boundary between two pixels is the one arrangement guaranteed to
        // disagree with itself when the surface resizes.
        const float fx = (static_cast<float>(col) + 0.5f) / static_cast<float>(cols);
        const float fy = (static_cast<float>(row) + 0.5f) / static_cast<float>(rows);
        out.push_back(makePoint(targets[i], x0 + (x1 - x0) * fx, y0 + (y1 - y0) * fy));
    }
    return out;
}

std::vector<SamplePoint> layoutLine(const std::vector<Target>& targets, float x0,
                                    float y0, float x1, float y1) {
    std::vector<SamplePoint> out;
    const size_t n = targets.size();
    for (size_t i = 0; i < n; ++i)
        out.push_back(makePoint(targets[i], lerpOver(x0, x1, i, n),
                                lerpOver(y0, y1, i, n)));
    return out;
}

std::vector<SamplePoint> layoutBezier(const std::vector<Target>& targets, float x0,
                                      float y0, float cx1, float cy1, float cx2,
                                      float cy2, float x1, float y1) {
    std::vector<SamplePoint> out;
    const size_t n = targets.size();
    for (size_t i = 0; i < n; ++i) {
        const float t = (n <= 1) ? 0.f
                                 : static_cast<float>(i) / static_cast<float>(n - 1);
        const float u = 1.f - t;
        // Spaced by PARAMETER, not by arc length. Honest rather than ideal:
        // points bunch where the curve is tight. Arc-length spacing is the
        // better answer and wants its own pass with a length table.
        const float bx = u * u * u * x0 + 3 * u * u * t * cx1 + 3 * u * t * t * cx2 +
                         t * t * t * x1;
        const float by = u * u * u * y0 + 3 * u * u * t * cy1 + 3 * u * t * t * cy2 +
                         t * t * t * y1;
        out.push_back(makePoint(targets[i], bx, by));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Extraction
// ---------------------------------------------------------------------------
SamplePoint* Extraction::pointFor(const Target& t) {
    for (SamplePoint& p : points)
        if (p.target == t) return &p;
    return nullptr;
}

const SamplePoint* Extraction::pointFor(const Target& t) const {
    for (const SamplePoint& p : points)
        if (p.target == t) return &p;
    return nullptr;
}

ExtractionReport Extraction::render(const RasterFrame& frame, OutputLayer& out) const {
    ExtractionReport rep;
    rep.hadFrame = !frame.empty();
    if (!enabled || !rep.hadFrame) return rep;

    for (const SamplePoint& p : points) {
        if (!p.enabled) {
            ++rep.disabled;
            continue;
        }
        if (p.x < 0.f || p.x > 1.f || p.y < 0.f || p.y > 1.f) {
            // Off the surface is dark, and counted. A mapping that has
            // drifted should be visible as a number, not as fixtures that
            // mysteriously stopped responding.
            ++rep.offSurface;
            out.set(makeKey(p.target.fixture, p.target.cell, Attr::Red), 0.f);
            out.set(makeKey(p.target.fixture, p.target.cell, Attr::Green), 0.f);
            out.set(makeKey(p.target.fixture, p.target.cell, Attr::Blue), 0.f);
            continue;
        }

        const RasterFrame::Rgb c = samplePoint(frame, p);
        // The master scales HERE, not in the merge. A faded extraction has to
        // contribute faded values to an HTP comparison; scaling afterwards
        // would let it win the comparison at full and then be turned down.
        const float m = clamp01(master);
        out.set(makeKey(p.target.fixture, p.target.cell, Attr::Red),
                static_cast<float>(c.r) / 255.f * m);
        out.set(makeKey(p.target.fixture, p.target.cell, Attr::Green),
                static_cast<float>(c.g) / 255.f * m);
        out.set(makeKey(p.target.fixture, p.target.cell, Attr::Blue),
                static_cast<float>(c.b) / 255.f * m);
        ++rep.sampled;
    }
    return rep;
}

// ---------------------------------------------------------------------------
// Store
// ---------------------------------------------------------------------------
Extraction& ExtractionStore::add(Extraction e) {
    if (!e.uid.valid()) e.uid = uids_->next();
    if (e.id <= 0) {
        int top = 0;
        for (const Extraction& x : extractions_) top = std::max(top, x.id);
        e.id = top + 1;
    }
    if (e.name.empty()) e.name = "Extraction " + std::to_string(e.id);
    extractions_.push_back(std::move(e));
    return extractions_.back();
}

Extraction& ExtractionStore::restore(Extraction e) {
    extractions_.push_back(std::move(e));
    return extractions_.back();
}

Extraction* ExtractionStore::byId(int id) {
    for (Extraction& e : extractions_)
        if (e.id == id) return &e;
    return nullptr;
}

const Extraction* ExtractionStore::byId(int id) const {
    for (const Extraction& e : extractions_)
        if (e.id == id) return &e;
    return nullptr;
}

Extraction* ExtractionStore::byName(const std::string& name) {
    for (Extraction& e : extractions_)
        if (e.name == name) return &e;
    return nullptr;
}

bool ExtractionStore::remove(int id) {
    for (auto it = extractions_.begin(); it != extractions_.end(); ++it) {
        if (it->id != id) continue;
        extractions_.erase(it);
        return true;
    }
    return false;
}

ExtractionReport ExtractionStore::mergeInto(const RasterFrame& frame,
                                            OutputLayer& out) const {
    ExtractionReport total;
    total.hadFrame = !frame.empty();

    // Priority first, then id as the final tiebreak — the same resolution
    // order mergePlaybacks uses, because an extraction layer is a playback
    // and resolving it differently would be a second set of rules to learn.
    std::vector<const Extraction*> order;
    order.reserve(extractions_.size());
    for (const Extraction& e : extractions_) order.push_back(&e);
    std::sort(order.begin(), order.end(), [](const Extraction* a, const Extraction* b) {
        if (a->priority != b->priority) return a->priority < b->priority;
        return a->id < b->id;
    });

    for (const Extraction* e : order) {
        OutputLayer layer;
        const ExtractionReport rep = e->render(frame, layer);
        total.sampled += rep.sampled;
        total.offSurface += rep.offSurface;
        total.disabled += rep.disabled;

        for (const auto& [key, v] : layer.values()) {
            const auto existing = out.get(key);
            if (!existing) out.set(key, v);
            else if (e->merge == MergeRule::HTP) out.set(key, std::max(*existing, v));
            else out.set(key, v);
        }
    }
    return total;
}

} // namespace lumen
