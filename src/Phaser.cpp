#include "lumen/Phaser.hpp"

#include "lumen/Patch.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace lumen {

namespace {

constexpr double kTau = 6.283185307179586;

std::string lower(const std::string& s) {
    std::string o = s;
    std::transform(o.begin(), o.end(), o.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return o;
}

uint64_t mix64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return x;
}

float wrap01(float x) {
    x = std::fmod(x, 1.f);
    return x < 0.f ? x + 1.f : x;
}

// Shaping curve: -1 eases out, 0 is linear, +1 eases in.
float shape(float t, float curve) {
    t = std::clamp(t, 0.f, 1.f);
    if (std::fabs(curve) < 1e-4f) return t;
    const float k = std::clamp(curve, -1.f, 1.f);
    if (k > 0.f) {
        const float p = 1.f + k * 2.f;  // ease in
        return std::pow(t, p);
    }
    const float p = 1.f + (-k) * 2.f;   // ease out
    return 1.f - std::pow(1.f - t, p);
}

// Cubic bezier, solving for y at a given x. Same approach as CSS timing
// functions: Newton iterations with a bisection fallback.
float bezierYAtX(float x, float x0, float y0, float x1, float y1, float x2, float y2,
                 float x3, float y3) {
    auto cx = [&](float u) {
        const float v = 1.f - u;
        return v * v * v * x0 + 3.f * v * v * u * x1 + 3.f * v * u * u * x2 + u * u * u * x3;
    };
    auto cy = [&](float u) {
        const float v = 1.f - u;
        return v * v * v * y0 + 3.f * v * v * u * y1 + 3.f * v * u * u * y2 + u * u * u * y3;
    };
    auto dx = [&](float u) {
        const float v = 1.f - u;
        return 3.f * v * v * (x1 - x0) + 6.f * v * u * (x2 - x1) + 3.f * u * u * (x3 - x2);
    };

    float u = (x3 - x0) > 1e-6f ? (x - x0) / (x3 - x0) : 0.f;
    u = std::clamp(u, 0.f, 1.f);
    for (int i = 0; i < 8; ++i) {
        const float err = cx(u) - x;
        if (std::fabs(err) < 1e-5f) return cy(u);
        const float d = dx(u);
        if (std::fabs(d) < 1e-6f) break;
        u = std::clamp(u - err / d, 0.f, 1.f);
    }
    float lo = 0.f, hi = 1.f;
    for (int i = 0; i < 24; ++i) {
        u = 0.5f * (lo + hi);
        if (cx(u) < x) lo = u;
        else hi = u;
    }
    return cy(u);
}

} // namespace

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------
const char* waveName(Wave w) {
    switch (w) {
        case Wave::Sine: return "Sine";
        case Wave::Cosine: return "Cosine";
        case Wave::Ramp: return "Ramp";
        case Wave::SawDown: return "SawDown";
        case Wave::Triangle: return "Triangle";
        case Wave::Square: return "Square";
        case Wave::PWM: return "PWM";
        case Wave::Noise: return "Noise";
        case Wave::Count: break;
    }
    return "?";
}

std::optional<Wave> waveFromName(const std::string& s) {
    const std::string k = lower(s);
    for (size_t i = 0; i < static_cast<size_t>(Wave::Count); ++i) {
        const auto w = static_cast<Wave>(i);
        if (lower(waveName(w)) == k) return w;
    }
    if (k == "sin") return Wave::Sine;
    if (k == "cos") return Wave::Cosine;
    if (k == "saw" || k == "sawup") return Wave::Ramp;
    if (k == "tri") return Wave::Triangle;
    if (k == "sqr") return Wave::Square;
    if (k == "random" || k == "rnd") return Wave::Noise;
    return std::nullopt;
}

const char* directionName(Direction d) {
    switch (d) {
        case Direction::Forward: return "Forward";
        case Direction::Reverse: return "Reverse";
        case Direction::Bounce: return "Bounce";
        case Direction::Random: return "Random";
    }
    return "?";
}

const char* bouncePivotName(BouncePivot p) {
    switch (p) {
        case BouncePivot::AtEnd: return "AtEnd";
        case BouncePivot::CentreAtEnd: return "CentreAtEnd";
        case BouncePivot::OverEnd: return "OverEnd";
    }
    return "?";
}

const char* gapBehaviourName(GapBehaviour b) {
    switch (b) {
        case GapBehaviour::Pause: return "Pause";
        case GapBehaviour::Freeze: return "Freeze";
        case GapBehaviour::Breathe: return "Breathe";
        case GapBehaviour::Burst: return "Burst";
        case GapBehaviour::Stutter: return "Stutter";
        case GapBehaviour::Drift: return "Drift";
        case GapBehaviour::Reshuffle: return "Reshuffle";
        case GapBehaviour::Count: break;
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Generator
// ---------------------------------------------------------------------------
float Generator::eval(float phase) const {
    const float p = wrap01(phase);

    switch (kind) {
        case GenKind::Waveform: {
            switch (wave) {
                case Wave::Sine: return static_cast<float>(std::sin(kTau * p));
                case Wave::Cosine: return static_cast<float>(std::cos(kTau * p));
                case Wave::Ramp: return 2.f * p - 1.f;
                case Wave::SawDown: return 1.f - 2.f * p;
                case Wave::Triangle: return 1.f - 4.f * std::fabs(p - 0.5f);
                case Wave::Square: return p < 0.5f ? 1.f : -1.f;
                case Wave::PWM: return p < std::clamp(duty, 0.f, 1.f) ? 1.f : -1.f;
                case Wave::Noise: {
                    // Held value per cycle rather than per sample, so it reads
                    // as a random step sequence instead of white noise.
                    const auto cyc = static_cast<uint64_t>(std::floor(phase));
                    const float n = static_cast<float>(mix64(cyc * 0x9e3779b9ull) & 0xffffffu) /
                                    8388607.5f - 1.f;
                    return std::clamp(n, -1.f, 1.f);
                }
                case Wave::Count: break;
            }
            return 0.f;
        }

        case GenKind::Steps: {
            if (steps.empty()) return 0.f;
            const size_t n = steps.size();
            const float slot = 1.f / static_cast<float>(n);
            const size_t i = std::min(n - 1, static_cast<size_t>(p / slot));
            const Step& s = steps[i];
            const Step& nextStep = steps[(i + 1) % n];

            const float local = (p - static_cast<float>(i) * slot) / slot;  // 0..1 in slot
            const float dwell = std::clamp(s.width, 0.f, 1.f);
            const float atk = std::clamp(s.attack, 0.f, 1.f);
            const float dec = std::clamp(s.decay, 0.f, 1.f);

            float v;
            if (local < atk && atk > 1e-5f) {
                // Rising into this step from the previous one.
                const Step& prev = steps[(i + n - 1) % n];
                v = prev.value + (s.value - prev.value) * shape(local / atk, s.attackCurve);
            } else if (local > dwell && dec > 1e-5f) {
                // Falling out of this step toward the next.
                const float f = std::min(1.f, (local - dwell) / dec);
                v = s.value + (nextStep.value - s.value) * shape(f, s.decayCurve);
            } else if (local > dwell) {
                v = nextStep.value;
            } else {
                v = s.value;
            }
            return std::clamp(v, 0.f, 1.f) * 2.f - 1.f;
        }

        case GenKind::Curve: {
            if (curve.empty()) return 0.f;
            if (curve.size() == 1) return std::clamp(curve[0].y, 0.f, 1.f) * 2.f - 1.f;

            // Find the segment containing p, treating the curve as cyclic.
            size_t i = 0;
            for (size_t k = 0; k + 1 < curve.size(); ++k) {
                if (p >= curve[k].x && p <= curve[k + 1].x) {
                    i = k;
                    break;
                }
                if (p > curve[k + 1].x) i = k + 1;
            }
            if (i + 1 >= curve.size()) i = curve.size() - 2;

            const CurvePoint& a = curve[i];
            const CurvePoint& b = curve[i + 1];
            const float y = bezierYAtX(p, a.x, a.y, a.x + a.outDx, a.y + a.outDy,
                                       b.x + b.inDx, b.y + b.inDy, b.x, b.y);
            return std::clamp(y, 0.f, 1.f) * 2.f - 1.f;
        }
    }
    return 0.f;
}

// ---------------------------------------------------------------------------
// Gap
// ---------------------------------------------------------------------------
float Gap::lengthForLoop(int loopIndex) const {
    if (!enabled) return 0.f;
    if (!sequence.empty()) {
        const size_t i = static_cast<size_t>(
            ((loopIndex % static_cast<int>(sequence.size())) + sequence.size()) %
            sequence.size());
        return std::max(0.f, sequence[i]);
    }
    return std::max(0.f, length);
}

Gap gapFromBehaviour(GapBehaviour b, float length, int everyNth) {
    Gap g;
    g.enabled = true;
    g.length = length;
    switch (b) {
        case GapBehaviour::Pause:
            g.hold = GapHold::Base;
            g.resume = GapResume::Reset;
            break;
        case GapBehaviour::Freeze:
            g.hold = GapHold::HoldLast;
            g.resume = GapResume::Reset;
            break;
        case GapBehaviour::Breathe:
            g.hold = GapHold::FadeToBase;
            g.placement = GapPlacement::Split;
            g.transition = 0.5f;
            break;
        case GapBehaviour::Burst:
            g.hold = GapHold::Base;
            g.everyNth = std::max(1, everyNth);
            g.resume = GapResume::Reset;
            break;
        case GapBehaviour::Stutter:
            g.hold = GapHold::HoldLast;
            g.resume = GapResume::Continue;
            g.sequence = {0.1f, 0.25f, 0.15f, 0.4f};
            break;
        case GapBehaviour::Drift:
            g.hold = GapHold::Base;
            g.resume = GapResume::Continue;
            break;
        case GapBehaviour::Reshuffle:
            g.hold = GapHold::Base;
            g.resume = GapResume::Reshuffle;
            break;
        case GapBehaviour::Count:
            g.enabled = false;
            break;
    }
    return g;
}

// ---------------------------------------------------------------------------
// Travel and noise
// ---------------------------------------------------------------------------
float targetNoise(const Target& t, uint64_t seed, uint32_t salt) {
    uint64_t h = seed;
    h = mix64(h ^ (static_cast<uint64_t>(t.fixture) << 20));
    h = mix64(h ^ (static_cast<uint64_t>(t.cell) << 4));
    h = mix64(h ^ static_cast<uint64_t>(salt));
    return static_cast<float>(h & 0xffffffu) / 8388607.5f - 1.f;
}

float travelAt(double cycles, Direction dir, BouncePivot pivot,
               float waveWidth, float overshoot, uint64_t seed) {
    switch (dir) {
        case Direction::Forward:
            return static_cast<float>(cycles);

        case Direction::Reverse:
            return static_cast<float>(-cycles);

        case Direction::Bounce: {
            // The pivot modes change the triangle's amplitude — where the wave
            // reverses relative to the ends of the spread (§4.13).
            float amp = 1.f;
            if (pivot == BouncePivot::CentreAtEnd) amp = 1.f + waveWidth * 0.5f;
            else if (pivot == BouncePivot::OverEnd) amp = 1.f + waveWidth + overshoot;

            // Triangle over 2 amplitudes: out and back.
            double x = std::fmod(cycles, 2.0);
            if (x < 0.0) x += 2.0;
            const double tri = x < 1.0 ? x : 2.0 - x;
            return static_cast<float>(tri * amp);
        }

        case Direction::Random: {
            // Shuffle, not dice (§4.12): direction is drawn from a permutation
            // so it cannot repeat immediately.
            const auto cyc = static_cast<uint64_t>(std::floor(cycles));
            const uint64_t h = mix64(seed ^ (cyc * 0x9e3779b97f4a7c15ull));
            const float sign = (h & 1ull) ? 1.f : -1.f;
            const double frac = cycles - std::floor(cycles);
            return static_cast<float>(static_cast<double>(cyc) + frac * sign);
        }
    }
    return static_cast<float>(cycles);
}

// ---------------------------------------------------------------------------
// Phaser
// ---------------------------------------------------------------------------
std::optional<Attr> Phaser::resolveAttr(const Patch& patch, const Target& target) const {
    const FixtureType* ft = patch.typeOf(target.fixture);
    if (!ft) return std::nullopt;

    if (role == Role::None) {
        if (ft->hasAttr(attr, target.cell) || ft->hasAttr(attr, 0) || isVirtual(attr))
            return attr;
        return std::nullopt;
    }
    // Roles resolve per fixture, in preference order. Where the fixture has
    // none of the candidates, the SLOT is skipped for that fixture.
    for (Attr cand : roleCandidates(role)) {
        if (ft->hasAttr(cand, target.cell) || ft->hasAttr(cand, 0)) return cand;
    }
    return std::nullopt;
}

Phaser::Sample Phaser::sampleAt(const Target& target, const SpreadTable& spread,
                                double t) const {
    Sample out;

    // Per-fixture variation (§4.11), seeded so it replays identically.
    const float vPhase = variation.phase * targetNoise(target, variation.seed, 1);
    const float vSpeed = variation.speed * targetNoise(target, variation.seed, 2);
    const float vSize = variation.size * targetNoise(target, variation.seed, 3);

    const float effSpeed = speedHz * (1.f + vSpeed);
    const float waveWidth = mods.groups > 0.f ? 1.f / mods.groups : 1.f;

    // Raw elapsed cycles on this phaser's clock.
    const double rawCycles = t * static_cast<double>(effSpeed);

    // Gap: N active loops plus the gap form a super-cycle, so "every Nth loop"
    // falls out of the super-cycle being N loops long (§4.15).
    double cycles = rawCycles;
    bool inGap = false;
    float gapProgress = 0.f;

    if (gap.enabled) {
        const int nth = std::max(1, gap.everyNth);
        float gapLen = gap.lengthForLoop(
            static_cast<int>(std::floor(rawCycles / std::max(1, nth))));
        if (gap.lengthIsFraction) gapLen *= static_cast<float>(nth);

        const double super = static_cast<double>(nth) + gapLen;
        if (super > 1e-9 && gapLen > 0.f) {
            double pos = std::fmod(rawCycles, super);
            if (pos < 0.0) pos += super;

            // Placement decides where the gap sits inside the super-cycle.
            double activeStart = 0.0, leadGap = 0.0;
            switch (gap.placement) {
                case GapPlacement::After:  activeStart = 0.0; leadGap = 0.0; break;
                case GapPlacement::Before: activeStart = gapLen; leadGap = gapLen; break;
                case GapPlacement::Split:  activeStart = gapLen * 0.5;
                                           leadGap = gapLen * 0.5; break;
            }
            const double activeEnd = activeStart + nth;

            if (pos < activeStart) {
                inGap = true;
                gapProgress = leadGap > 1e-9 ? static_cast<float>(pos / leadGap) : 0.f;
            } else if (pos >= activeEnd) {
                inGap = true;
                const double trailGap = super - activeEnd;
                gapProgress = trailGap > 1e-9
                                  ? static_cast<float>((pos - activeEnd) / trailGap)
                                  : 1.f;
            }

            // Reset: phase advances only during active spans, so each loop
            // restarts where the last one ended. Continue: phase keeps running
            // through the gap, so the effect never realigns with itself —
            // which is what the Drift behaviour is for.
            if (gap.resume != GapResume::Continue) {
                const double completedSupers = std::floor(rawCycles / super);
                const double intoActive =
                    std::clamp(pos - activeStart, 0.0, static_cast<double>(nth));
                cycles = completedSupers * nth + intoActive;
            }
        }
    }

    const float spreadPhase = spread.phase(target, mods);
    const float travel = travelAt(cycles, direction, pivot, waveWidth, overshoot,
                                  variation.seed);
    const float amp = size * (1.f + vSize) * std::clamp(fade, 0.f, 1.f);

    float w;
    if (!inGap) {
        w = gen.eval(travel + spreadPhase + vPhase);
    } else {
        out.inGap = true;
        // The cycle-end phase. For an integer active span this equals the
        // cycle-start phase, because a full cycle returns to where it began —
        // so HoldLast and First coincide unless the gap interrupts a partial
        // cycle, which only `lengthIsFraction` can produce.
        const float endPhase = spreadPhase + vPhase;
        switch (gap.hold) {
            case GapHold::Base:
                out.silent = true;
                return out;
            case GapHold::HoldLast:
                w = gen.eval(endPhase);
                break;
            case GapHold::First:
                w = gen.eval(vPhase + spreadPhase);
                break;
            case GapHold::FadeToBase:
                w = gen.eval(endPhase) * (1.f - std::clamp(gapProgress, 0.f, 1.f));
                break;
            default:
                w = 0.f;
                break;
        }
    }

    out.wave = w;
    out.delta = w * amp * 0.5f;
    return out;
}

void Phaser::apply(const SpreadTable& spread, double t, const ValueLayer& base,
                   ValueLayer& out, const Patch* patch) const {
    for (const Target& target : targets) {
        Attr a = attr;
        if (patch) {
            const auto resolved = resolveAttr(*patch, target);
            if (!resolved) continue;  // skip the slot for this fixture (§4.2)
            a = *resolved;
        } else if (role != Role::None) {
            const auto& cands = roleCandidates(role);
            if (cands.empty()) continue;
            a = cands.front();
        }

        const Sample s = sampleAt(target, spread, t);
        if (s.silent && relative) continue;  // gap at base: no contribution

        float centreValue = centre;
        if (relative) {
            if (auto b = base.value(target.fixture, target.cell, a)) centreValue = *b;
            else if (auto b2 = out.value(target.fixture, target.cell, a)) centreValue = *b2;
            else centreValue = 0.5f;
        }

        const float v = clamp01(centreValue + s.delta);
        out.set(target.fixture, target.cell, a, Value::hard(v));
    }
}

} // namespace lumen
