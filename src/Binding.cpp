#include "lumen/Binding.hpp"

#include <algorithm>
#include <cmath>

namespace lumen {

const char* sourceKindName(SourceKind k) {
    switch (k) {
        case SourceKind::None: return "None";
        case SourceKind::MidiNote: return "Note";
        case SourceKind::MidiCC: return "CC";
        case SourceKind::MidiCC14: return "CC14";
        case SourceKind::MidiProgram: return "Program";
        case SourceKind::MidiPitchBend: return "PitchBend";
        case SourceKind::Osc: return "OSC";
        case SourceKind::DmxIn: return "DMX";
        case SourceKind::Key: return "Key";
    }
    return "None";
}

const char* targetKindName(TargetKind k) {
    switch (k) {
        case TargetKind::Command: return "Command";
        case TargetKind::Fader: return "Fader";
        case TargetKind::Encoder: return "Encoder";
        case TargetKind::Button: return "Button";
        case TargetKind::Master: return "Master";
        case TargetKind::Option: return "Option";
        case TargetKind::Executor: return "Executor";
        case TargetKind::Preset: return "Preset";
        case TargetKind::Group: return "Group";
        case TargetKind::Recipe: return "Recipe";
        case TargetKind::PanelSet: return "PanelSet";
        case TargetKind::RadialSector: return "RadialSector";
    }
    return "Command";
}

const char* bindModeName(BindMode m) {
    switch (m) {
        case BindMode::Absolute: return "Absolute";
        case BindMode::Relative: return "Relative";
        case BindMode::Toggle: return "Toggle";
        case BindMode::Momentary: return "Momentary";
        case BindMode::Trigger: return "Trigger";
    }
    return "Absolute";
}

const char* takeoverName(Takeover t) {
    switch (t) {
        case Takeover::Jump: return "Jump";
        case Takeover::Scale: return "Scale";
        case Takeover::Catch: return "Catch";
    }
    return "Catch";
}

const char* curveName(BindCurve c) {
    switch (c) {
        case BindCurve::Linear: return "Linear";
        case BindCurve::Log: return "Log";
        case BindCurve::Exp: return "Exp";
        case BindCurve::SCurve: return "SCurve";
    }
    return "Linear";
}

// ---------------------------------------------------------------------------
// Source
// ---------------------------------------------------------------------------
std::string InputSource::describe() const {
    switch (kind) {
        case SourceKind::None:
            return {};
        case SourceKind::Osc:
            return address.empty() ? std::string("OSC") : address;
        case SourceKind::DmxIn:
            // The spec's own badge format: "U2.14".
            return "U" + std::to_string(channel) + "." + std::to_string(index);
        case SourceKind::Key: {
            std::string out;
            if (has(mods, Mods::Ctrl)) out += "Ctrl+";
            if (has(mods, Mods::Alt)) out += "Alt+";
            if (has(mods, Mods::Shift)) out += "Shift+";
            if (has(mods, Mods::Meta)) out += "Meta+";
            return out + "Key " + std::to_string(index);
        }
        default:
            break;
    }
    std::string out = std::string(sourceKindName(kind)) + " " + std::to_string(index);
    // The channel appears only when it distinguishes something. Learn always
    // pins the channel it saw — it has to, or a two-channel controller's CC 7
    // collides with itself — but channel 1 is the overwhelmingly common case,
    // and "ch1" on every badge is noise. §12.1's own example badge is "CC 7".
    if (channel > 1) out += " ch" + std::to_string(channel);
    return out;
}

bool InputSource::sameAs(const InputSource& o) const {
    if (kind != o.kind) return false;
    if (kind == SourceKind::Osc) return address == o.address && argIndex == o.argIndex;
    if (kind == SourceKind::Key) return index == o.index && mods == o.mods;
    if (index != o.index) return false;
    // Channel 0 is "any", so it collides with every channel rather than none.
    if (channel == 0 || o.channel == 0) return true;
    return channel == o.channel;
}

namespace {

bool sourceMatches(const InputSource& s, const InputEvent& e) {
    if (s.kind != e.kind) return false;
    switch (s.kind) {
        case SourceKind::Osc:
            return s.address == e.address && s.argIndex == e.argIndex;
        case SourceKind::Key:
            return s.index == e.index && s.mods == e.mods;
        case SourceKind::None:
            return false;
        default:
            break;
    }
    if (s.index != e.index) return false;
    if (s.channel == 0) return true;   // any channel
    return s.channel == e.channel;
}

float applyCurveShape(float t, BindCurve c) {
    t = std::clamp(t, 0.f, 1.f);
    switch (c) {
        case BindCurve::Linear:
            return t;
        case BindCurve::Log:
            // Fast at the bottom, which is what a dimmer fader wants.
            return std::log1p(t * 9.f) / std::log(10.f);
        case BindCurve::Exp:
            return (std::pow(10.f, t) - 1.f) / 9.f;
        case BindCurve::SCurve:
            return t * t * (3.f - 2.f * t);
    }
    return t;
}

} // namespace

// ---------------------------------------------------------------------------
// Mapping
// ---------------------------------------------------------------------------
float Mapping::shape(float raw) const {
    const float lo = std::min(inMin, inMax);
    const float hi = std::max(inMin, inMax);
    float t = (hi - lo) < 1e-9f ? 0.f : (std::clamp(raw, lo, hi) - lo) / (hi - lo);
    // An inverted input range is a legitimate way to write it, so honour both
    // that and the explicit invert flag.
    if (inMin > inMax) t = 1.f - t;
    if (invert) t = 1.f - t;
    t = applyCurveShape(t, curve);
    return outMin + t * (outMax - outMin);
}

void Binding::resetRuntime() {
    caught = false;
    lastRaw = 0.f;
    hasLastRaw = false;
    smoothed = 0.f;
    hasSmoothed = false;
    aboveThreshold = false;
    anchorValue = 0.f;
    hasAnchor = false;
    // toggleState deliberately survives: it is the state of the thing being
    // controlled, not of the control.
}

// ---------------------------------------------------------------------------
// Applying an input
// ---------------------------------------------------------------------------
namespace {

// Did a press happen? A note-off is not a press, and a continuous source
// counts as pressed when it crosses the halfway mark.
bool isPress(const InputEvent& e) {
    if (e.isNoteOff) return false;
    if (e.kind == SourceKind::MidiNote) return e.pressed || e.value > 0.f;
    if (e.kind == SourceKind::Key) return e.pressed;
    return e.value >= 0.5f;
}

bool isRelease(const InputEvent& e) {
    if (e.isNoteOff) return true;
    if (e.kind == SourceKind::MidiNote || e.kind == SourceKind::Key)
        return !e.pressed && e.value <= 0.f;
    return e.value < 0.5f;
}

} // namespace

Applied applyInput(Binding& b, const InputEvent& e, float currentValue) {
    Applied out;
    if (!b.enabled) {
        out.swallowed = true;
        return out;
    }

    // --- threshold edges: the GPI substitute (§12.3) ----------------------
    if (b.map.edge != ThresholdEdge::None) {
        const float v = e.value;
        const float hi = b.map.threshold;
        // Hysteresis, so a noisy DMX channel sitting on the threshold does
        // not chatter the binding on and off every frame.
        const float lo = std::max(0.f, b.map.threshold - b.map.hysteresis);
        const bool was = b.aboveThreshold;
        if (!was && v >= hi) b.aboveThreshold = true;
        else if (was && v <= lo) b.aboveThreshold = false;

        const bool now = b.aboveThreshold;
        switch (b.map.edge) {
            case ThresholdEdge::Rising:
                out.fire = !was && now;
                out.fireOn = true;
                break;
            case ThresholdEdge::Falling:
                out.fire = was && !now;
                out.fireOn = false;
                break;
            case ThresholdEdge::WhileAbove:
                // A state, not an edge: report it whenever it changes.
                out.fire = was != now;
                out.fireOn = now;
                break;
            case ThresholdEdge::None:
                break;
        }
        out.swallowed = !out.fire;
        return out;
    }

    switch (b.map.mode) {
        case BindMode::Trigger: {
            out.fire = isPress(e);
            out.fireOn = true;
            out.swallowed = !out.fire;
            return out;
        }
        case BindMode::Momentary: {
            if (isPress(e)) {
                out.fire = true;
                out.fireOn = true;
            } else if (isRelease(e)) {
                out.fire = true;
                out.fireOn = false;
            }
            out.swallowed = !out.fire;
            return out;
        }
        case BindMode::Toggle: {
            // Only a press flips it; the release must not flip it back.
            if (!isPress(e)) {
                out.swallowed = true;
                return out;
            }
            b.toggleState = !b.toggleState;
            out.fire = true;
            out.fireOn = b.toggleState;
            out.hasValue = true;
            out.value = b.toggleState ? b.map.outMax : b.map.outMin;
            return out;
        }
        case BindMode::Relative: {
            const float d = e.isDelta ? e.delta : (e.value - 0.5f) * 2.f;
            if (std::fabs(d) < 1e-6f) {
                out.swallowed = true;
                return out;
            }
            const float span = b.map.outMax - b.map.outMin;
            float v = currentValue + d * b.map.relativeScale * span;
            v = std::clamp(v, std::min(b.map.outMin, b.map.outMax),
                           std::max(b.map.outMin, b.map.outMax));
            out.hasValue = true;
            out.value = v;
            // An endless encoder has no position, so there is nothing to
            // catch up to: it is always already in control.
            b.caught = true;
            return out;
        }
        case BindMode::Absolute:
            break;
    }

    // --- absolute: smoothing, deadband, then takeover ---------------------
    float raw = e.value;
    if (b.map.smoothing > 0.f) {
        const float a = std::clamp(1.f - b.map.smoothing, 0.01f, 1.f);
        raw = b.hasSmoothed ? b.smoothed + (raw - b.smoothed) * a : raw;
        b.smoothed = raw;
        b.hasSmoothed = true;
    }

    if (b.map.deadband > 0.f && b.hasLastRaw &&
        std::fabs(raw - b.lastRaw) < b.map.deadband) {
        out.swallowed = true;
        return out;
    }

    const float mapped = b.map.shape(raw);
    const float loOut = std::min(b.map.outMin, b.map.outMax);
    const float hiOut = std::max(b.map.outMin, b.map.outMax);

    switch (b.map.takeover) {
        case Takeover::Jump: {
            b.lastRaw = raw;
            b.hasLastRaw = true;
            b.caught = true;
            out.hasValue = true;
            out.value = std::clamp(mapped, loOut, hiOut);
            return out;
        }

        case Takeover::Scale: {
            // The first touch anchors on where the value already was; from
            // there the control's remaining travel covers the remaining
            // range, in whichever direction it moves.
            if (!b.hasAnchor) {
                b.anchorValue = std::clamp(currentValue, loOut, hiOut);
                b.lastRaw = raw;
                b.hasLastRaw = true;
                b.hasAnchor = true;
                // Touching a fader must not move anything by itself.
                out.swallowed = true;
                return out;
            }
            const float anchorRaw = b.map.shape(b.lastRaw);
            float v;
            if (mapped >= anchorRaw) {
                const float head = hiOut - anchorRaw;
                const float travel = hiOut - anchorRaw;
                v = travel < 1e-9f
                        ? b.anchorValue
                        : b.anchorValue +
                              (mapped - anchorRaw) / travel * (hiOut - b.anchorValue);
                (void)head;
            } else {
                const float travel = anchorRaw - loOut;
                v = travel < 1e-9f
                        ? b.anchorValue
                        : b.anchorValue -
                              (anchorRaw - mapped) / travel * (b.anchorValue - loOut);
            }
            b.caught = true;
            out.hasValue = true;
            out.value = std::clamp(v, loOut, hiOut);
            return out;
        }

        case Takeover::Catch: {
            const float target = std::clamp(currentValue, loOut, hiOut);
            if (b.caught) {
                b.lastRaw = raw;
                b.hasLastRaw = true;
                out.hasValue = true;
                out.value = std::clamp(mapped, loOut, hiOut);
                return out;
            }

            // Crossing counts in either direction, and an exact match counts
            // too: a fader parked on the value is already in control.
            const bool crossed =
                !b.hasLastRaw
                    ? std::fabs(mapped - target) < 1e-6f
                    : (b.map.shape(b.lastRaw) - target) * (mapped - target) <= 0.f;

            b.lastRaw = raw;
            b.hasLastRaw = true;

            if (crossed) {
                b.caught = true;
                out.hasValue = true;
                out.value = std::clamp(mapped, loOut, hiOut);
                out.justCaught = true;
                return out;
            }

            // Dead, but it says which way to move rather than leaving the
            // operator guessing why the fader does nothing (§12.4).
            out.swallowed = true;
            out.catchDirection = mapped < target ? +1 : -1;
            return out;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// BindTarget
// ---------------------------------------------------------------------------
bool BindTarget::sameAs(const BindTarget& o) const {
    if (kind != o.kind || subIndex != o.subIndex) return false;
    if (kind == TargetKind::Command) return command == o.command;
    if (object.valid() || o.object.valid()) return object == o.object;
    return label == o.label;
}

std::string BindTarget::describe() const {
    if (kind == TargetKind::Command)
        return command.empty() ? std::string("Command") : command;
    std::string out = targetKindName(kind);
    if (!label.empty()) out += " " + label;
    else if (subIndex) out += " " + std::to_string(subIndex);
    return out;
}

// ---------------------------------------------------------------------------
// BindingSet
// ---------------------------------------------------------------------------
Binding* BindingSet::find(Uid uid) {
    for (auto& b : bindings)
        if (b.uid == uid) return &b;
    return nullptr;
}

Binding* BindingSet::findBySource(const InputSource& s) {
    for (auto& b : bindings)
        if (b.source.sameAs(s)) return &b;
    return nullptr;
}

// ---------------------------------------------------------------------------
// BindingStore
// ---------------------------------------------------------------------------
BindingSet& BindingStore::addSet(BindingSet s) {
    if (!s.uid.valid()) s.uid = uids_->next();
    uids_->observe(s.uid);
    if (s.id == 0) {
        int maxId = 0;
        for (const auto& x : sets_) maxId = std::max(maxId, x.id);
        s.id = maxId + 1;
    }
    if (s.name.empty()) s.name = "Set " + std::to_string(s.id);
    sets_.push_back(std::move(s));
    return sets_.back();
}

BindingSet& BindingStore::restoreSet(BindingSet s) {
    if (!s.uid.valid()) s.uid = uids_->next();
    uids_->observe(s.uid);
    for (Binding& b : s.bindings) uids_->observe(b.uid);
    sets_.push_back(std::move(s));
    return sets_.back();
}

BindingSet* BindingStore::set(Uid uid) {
    for (auto& s : sets_)
        if (s.uid == uid) return &s;
    return nullptr;
}

BindingSet* BindingStore::setByName(const std::string& name) {
    for (auto& s : sets_)
        if (s.name == name) return &s;
    return nullptr;
}

bool BindingStore::removeSet(Uid uid) {
    for (size_t i = 0; i < sets_.size(); ++i) {
        if (sets_[i].uid != uid) continue;
        sets_.erase(sets_.begin() + static_cast<long>(i));
        return true;
    }
    return false;
}

Binding* BindingStore::add(Uid setUid, Binding b) {
    BindingSet* s = set(setUid);
    if (!s) return nullptr;
    if (!b.uid.valid()) b.uid = uids_->next();
    uids_->observe(b.uid);
    if (b.id == 0) {
        int maxId = 0;
        for (const auto& x : s->bindings) maxId = std::max(maxId, x.id);
        b.id = maxId + 1;
    }
    s->bindings.push_back(std::move(b));
    return &s->bindings.back();
}

bool BindingStore::remove(Uid setUid, Uid bindingUid) {
    BindingSet* s = set(setUid);
    if (!s) return false;
    for (size_t i = 0; i < s->bindings.size(); ++i) {
        if (s->bindings[i].uid != bindingUid) continue;
        s->bindings.erase(s->bindings.begin() + static_cast<long>(i));
        return true;
    }
    return false;
}

Binding* BindingStore::find(Uid bindingUid) {
    for (auto& s : sets_)
        if (Binding* b = s.find(bindingUid)) return b;
    return nullptr;
}

void BindingStore::setPage(int page) {
    if (page == page_) return;
    page_ = page;
    // A fader caught on the outgoing page has not been caught on the incoming
    // one, so the takeover state goes with the page (§12.4, §12.6).
    resetTakeover();
}

std::vector<Binding*> BindingStore::resolveAll(const InputEvent& e) {
    std::vector<Binding*> out;
    // Page-local first, then global: layering means the page wins (§12.6).
    for (int pass = 0; pass < 2; ++pass) {
        const bool wantPage = pass == 0;
        for (BindingSet& s : sets_) {
            if (!s.enabled) continue;
            const bool isPage = s.page != 0;
            if (isPage != wantPage) continue;
            if (isPage && s.page != page_) continue;
            for (Binding& b : s.bindings) {
                if (!b.enabled) continue;
                if (sourceMatches(b.source, e)) out.push_back(&b);
            }
        }
    }
    return out;
}

std::vector<Binding*> BindingStore::resolve(const InputEvent& e) {
    std::vector<Binding*> all = resolveAll(e);
    if (all.empty()) return all;

    // The first match's layer wins. Several bindings in the SAME layer are
    // deliberate (§12.6 allows one input driving several things), but a
    // page-local binding shadows the global one entirely rather than firing
    // both.
    std::vector<Binding*> out;
    bool pageLocalFound = false;
    for (Binding* b : all) {
        bool isPage = false;
        for (const BindingSet& s : sets_)
            for (const Binding& x : s.bindings)
                if (&x == b) isPage = s.page != 0;
        if (out.empty()) {
            pageLocalFound = isPage;
            out.push_back(b);
            continue;
        }
        if (isPage == pageLocalFound) out.push_back(b);
    }
    return out;
}

Conflict BindingStore::wouldConflict(const InputSource& s, int page) const {
    Conflict c;
    for (const BindingSet& bs : sets_) {
        if (!bs.enabled) continue;
        // A binding on another page is not a conflict: that is what pages are
        // for.
        if (bs.page != 0 && bs.page != page) continue;
        for (const Binding& b : bs.bindings) {
            if (!b.source.sameAs(s)) continue;
            c.any = true;
            c.existing.push_back(bs.name + ": " + b.source.describe() + " -> " +
                                 b.target.describe());
        }
    }
    return c;
}

void BindingStore::activate(Uid setUid, bool on) {
    BindingSet* s = set(setUid);
    if (!s) return;
    s->enabled = on;
    // §12.5: on activation the controller's state is unknown, so nothing is
    // caught until it is touched.
    for (Binding& b : s->bindings) b.resetRuntime();
}

void BindingStore::resetTakeover() {
    for (BindingSet& s : sets_)
        for (Binding& b : s.bindings) b.resetRuntime();
}

size_t BindingStore::bindingCount() const {
    size_t n = 0;
    for (const BindingSet& s : sets_) n += s.bindings.size();
    return n;
}

// ---------------------------------------------------------------------------
// Learn
// ---------------------------------------------------------------------------
void Learn::begin() {
    active_ = true;
    if (focus_ < 0 && !targets_.empty()) focus_ = 0;
}

void Learn::end() { active_ = false; }

void Learn::setTargets(std::vector<BindTarget> targets) {
    targets_ = std::move(targets);
    if (focus_ >= static_cast<int>(targets_.size())) focus_ = -1;
    if (focus_ < 0 && !targets_.empty() && active_) focus_ = 0;
}

bool Learn::focus(int index) {
    if (index < 0 || index >= static_cast<int>(targets_.size())) return false;
    focus_ = index;
    return true;
}

bool Learn::focusTarget(const BindTarget& t) {
    for (size_t i = 0; i < targets_.size(); ++i)
        if (targets_[i].sameAs(t)) {
            focus_ = static_cast<int>(i);
            return true;
        }
    return false;
}

const BindTarget* Learn::focused() const {
    if (focus_ < 0 || focus_ >= static_cast<int>(targets_.size())) return nullptr;
    return &targets_[static_cast<size_t>(focus_)];
}

LearnResult Learn::bind(const InputEvent& e, bool force) {
    LearnResult r;
    r.focusIndex = focus_;
    if (!active_) {
        r.message = "not in learn";
        return r;
    }
    const BindTarget* t = focused();
    if (!t) {
        r.message = "nothing focused";
        return r;
    }
    if (!targetSet_.valid() || !store_->set(targetSet_)) {
        r.message = "no binding set selected";
        return r;
    }

    InputSource s;
    s.kind = e.kind;
    s.channel = e.channel;
    s.index = e.index;
    s.address = e.address;
    s.argIndex = e.argIndex;
    s.mods = e.mods;
    if (!s.valid()) {
        r.message = "input carried no source";
        return r;
    }

    BindingSet* bs = store_->set(targetSet_);

    if (!force) {
        const Conflict c = store_->wouldConflict(s, bs->page);
        if (c.any) {
            // Re-learning the SAME target is not a conflict, it is a change.
            bool sameTargetOnly = true;
            for (Binding& b : bs->bindings)
                if (b.source.sameAs(s) && !b.target.sameAs(*t)) sameTargetOnly = false;
            // Only safe to treat as a change when the existing bindings are
            // all in this set and all point at this target.
            size_t hereCount = 0;
            for (Binding& b : bs->bindings)
                if (b.source.sameAs(s)) ++hereCount;
            if (!sameTargetOnly || hereCount != c.existing.size()) {
                r.outcome = LearnResult::Outcome::Conflict;
                r.conflict = c;
                r.message = "already bound";
                return r;
            }
        }
    }

    // Replace any binding in this set that already drives this target, so
    // learning twice moves the binding rather than stacking two.
    for (size_t i = 0; i < bs->bindings.size();) {
        if (bs->bindings[i].target.sameAs(*t))
            bs->bindings.erase(bs->bindings.begin() + static_cast<long>(i));
        else
            ++i;
    }

    Binding b;
    b.source = s;
    b.target = *t;
    b.label = t->describe();
    // A sensible mapping per target kind, so a learned fader is usable before
    // anyone opens its settings.
    switch (t->kind) {
        case TargetKind::Button:
        case TargetKind::Command:
        case TargetKind::Preset:
        case TargetKind::Group:
        case TargetKind::PanelSet:
        case TargetKind::RadialSector:
            b.map.mode = BindMode::Trigger;
            break;
        case TargetKind::Executor:
            b.map.mode = (e.kind == SourceKind::MidiNote || e.kind == SourceKind::Key)
                             ? BindMode::Trigger
                             : BindMode::Absolute;
            break;
        case TargetKind::Encoder:
            b.map.mode = BindMode::Relative;
            break;
        default:
            b.map.mode = BindMode::Absolute;
            break;
    }
    Binding* made = store_->add(targetSet_, std::move(b));
    if (!made) {
        r.message = "could not add the binding";
        return r;
    }

    r.binding = made->uid;
    r.outcome = LearnResult::Outcome::Bound;
    r.message = made->source.describe() + " -> " + made->target.describe();

    // Learn next: eight faders become eight fader moves (§12.1).
    if (autoAdvance_ && focus_ + 1 < static_cast<int>(targets_.size())) {
        ++focus_;
        r.outcome = LearnResult::Outcome::Advanced;
        r.focusIndex = focus_;
    }
    return r;
}

LearnResult Learn::receive(const InputEvent& e) { return bind(e, false); }
LearnResult Learn::confirm(const InputEvent& e) { return bind(e, true); }

LearnResult Learn::unlearnFocused() {
    LearnResult r;
    r.focusIndex = focus_;
    const BindTarget* t = focused();
    if (!t) {
        r.message = "nothing focused";
        return r;
    }
    BindingSet* bs = store_->set(targetSet_);
    if (!bs) {
        r.message = "no binding set selected";
        return r;
    }
    int removed = 0;
    for (size_t i = 0; i < bs->bindings.size();) {
        if (bs->bindings[i].target.sameAs(*t)) {
            bs->bindings.erase(bs->bindings.begin() + static_cast<long>(i));
            ++removed;
        } else {
            ++i;
        }
    }
    if (removed == 0) {
        r.message = "not bound";
        return r;
    }
    r.outcome = LearnResult::Outcome::Unlearned;
    r.message = t->describe() + " unlearned";
    return r;
}

int Learn::clearSet() {
    BindingSet* bs = store_->set(targetSet_);
    if (!bs) return 0;
    const int n = static_cast<int>(bs->bindings.size());
    bs->bindings.clear();
    return n;
}

std::string Learn::badgeFor(const BindTarget& t) const {
    // Every enabled set, because the badge should show what will actually
    // reach this target, not just what this set holds.
    for (const BindingSet& s : store_->sets()) {
        if (!s.enabled) continue;
        if (s.page != 0 && s.page != store_->page()) continue;
        for (const Binding& b : s.bindings)
            if (b.target.sameAs(t)) return b.source.describe();
    }
    return {};
}

// ---------------------------------------------------------------------------
// InputQueue
// ---------------------------------------------------------------------------
bool InputQueue::push(const InputEvent& e) {
    std::lock_guard<std::mutex> lock(m_);
    if (q_.size() >= capacity_) {
        // Drop the newest rather than the oldest: a stuck controller flooding
        // the queue must not push out the note the operator just played.
        ++dropped_;
        return false;
    }
    q_.push_back(e);
    return true;
}

std::vector<InputEvent> InputQueue::drain() {
    std::vector<InputEvent> out;
    std::lock_guard<std::mutex> lock(m_);
    out.swap(q_);
    return out;
}

size_t InputQueue::size() const {
    std::lock_guard<std::mutex> lock(m_);
    return q_.size();
}

size_t InputQueue::dropped() const {
    std::lock_guard<std::mutex> lock(m_);
    return dropped_;
}

void InputQueue::clearDropped() {
    std::lock_guard<std::mutex> lock(m_);
    dropped_ = 0;
}

} // namespace lumen
