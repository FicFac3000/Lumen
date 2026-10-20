#include "lumen/Cue.hpp"

#include <algorithm>
#include <cmath>

namespace lumen {

const char* fadeCurveName(FadeCurve c) {
    switch (c) {
        case FadeCurve::Linear: return "Linear";
        case FadeCurve::SCurve: return "SCurve";
        case FadeCurve::EaseIn: return "EaseIn";
        case FadeCurve::EaseOut: return "EaseOut";
    }
    return "?";
}

float applyCurve(float t, FadeCurve c) {
    const float x = std::clamp(t, 0.f, 1.f);
    switch (c) {
        case FadeCurve::Linear: return x;
        case FadeCurve::SCurve: return x * x * (3.f - 2.f * x);  // smoothstep
        case FadeCurve::EaseIn: return x * x;
        case FadeCurve::EaseOut: return 1.f - (1.f - x) * (1.f - x);
    }
    return x;
}

const char* trackModeName(TrackMode m) {
    switch (m) {
        case TrackMode::Track: return "Track";
        case TrackMode::Block: return "Block";
        case TrackMode::CueOnly: return "CueOnly";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Cue
// ---------------------------------------------------------------------------
Timing Cue::timingFor(Attr a) const {
    auto ai = attrTiming.find(static_cast<uint16_t>(a));
    if (ai != attrTiming.end()) return ai->second;
    auto gi = groupTiming.find(static_cast<uint8_t>(groupOf(a)));
    if (gi != groupTiming.end()) return gi->second;
    return timing;
}

Timing Cue::timingForPart(size_t part) const {
    if (part < parts.size() && parts[part].timing) return *parts[part].timing;
    return timing;
}

ValueLayer Cue::allValues() const {
    ValueLayer out;
    for (const auto& p : parts) out.overwriteFrom(p.values);
    return out;
}

// ---------------------------------------------------------------------------
// Sequence
// ---------------------------------------------------------------------------
int Sequence::indexOfNumber(float n) const {
    for (size_t i = 0; i < cues.size(); ++i)
        if (std::fabs(cues[i].number - n) < 1e-4f) return static_cast<int>(i);
    return -1;
}

const Cue* Sequence::cueAt(int index) const {
    if (index < 0 || index >= static_cast<int>(cues.size())) return nullptr;
    return &cues[static_cast<size_t>(index)];
}

Cue* Sequence::cueAt(int index) {
    if (index < 0 || index >= static_cast<int>(cues.size())) return nullptr;
    return &cues[static_cast<size_t>(index)];
}

// ---------------------------------------------------------------------------
// Tracking resolution
// ---------------------------------------------------------------------------
Resolved resolveTracking(const Sequence& seq, int index, bool withOrigin) {
    Resolved out;
    if (index < 0 || index >= static_cast<int>(seq.cues.size())) return out;

    // `tracked` carries forward; `applied` is what the cue being resolved
    // actually puts on stage. The two differ for CueOnly, whose changes apply
    // here but are not inherited by the next cue.
    ValueLayer tracked;
    std::unordered_map<AttrKey, int> trackedOrigin;

    for (int j = 0; j <= index; ++j) {
        const Cue& cue = seq.cues[static_cast<size_t>(j)];

        // A shielded cue starts from nothing upstream, so no edit earlier in
        // the sequence can reach it (§11.1).
        if (cue.shield) {
            tracked.clear();
            trackedOrigin.clear();
        }

        ValueLayer applied = tracked;
        std::unordered_map<AttrKey, int> appliedOrigin = trackedOrigin;

        const ValueLayer own = cue.allValues();
        for (const auto& [key, val] : own.values()) {
            const Group g = groupOf(keyAttr(key));
            const TrackMode mode = cue.trackMode(g);

            applied.setKey(key, val);
            if (withOrigin) appliedOrigin[key] = j;

            // CueOnly applies here but is not inherited.
            if (mode != TrackMode::CueOnly) {
                tracked.setKey(key, val);
                if (withOrigin) trackedOrigin[key] = j;
            }
        }

        if (j == index) {
            out.values = std::move(applied);
            if (withOrigin) out.originCue = std::move(appliedOrigin);
            return out;
        }
    }
    return out;
}

void blockGroup(Sequence& seq, int index, Group g) {
    Cue* cue = seq.cueAt(index);
    if (!cue) return;

    // Block means the cue holds hard values of its own, so an upstream edit
    // cannot track through. Materialise the resolved state for this group.
    const Resolved r = resolveTracking(seq, index);
    for (const auto& [key, val] : r.values.values()) {
        if (groupOf(keyAttr(key)) != g) continue;
        if (cue->values().find(key)) continue;  // already stored here
        cue->values().setKey(key, val);
    }
    cue->setTrackMode(g, TrackMode::Block);
}

void unblockGroup(Sequence& seq, int index, Group g) {
    Cue* cue = seq.cueAt(index);
    if (!cue) return;

    // Remove values that merely duplicate what would track in anyway, so the
    // cue returns to storing only changes.
    if (index > 0) {
        const Resolved upstream = resolveTracking(seq, index - 1);
        std::vector<AttrKey> redundant;
        for (const auto& [key, val] : cue->values().values()) {
            if (groupOf(keyAttr(key)) != g) continue;
            if (const Value* up = upstream.values.find(key)) {
                if (!up->isRef() && !val.isRef() && std::fabs(up->v - val.v) < 1e-6f)
                    redundant.push_back(key);
            }
        }
        for (AttrKey k : redundant) cue->values().removeKey(k);
    }
    cue->setTrackMode(g, TrackMode::Track);
}

} // namespace lumen
