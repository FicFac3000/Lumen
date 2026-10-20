#include "lumen/Value.hpp"

#include <algorithm>

namespace lumen {

float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

// ---------------------------------------------------------------------------
// ValueLayer
// ---------------------------------------------------------------------------
void ValueLayer::set(FixtureId f, CellId cell, Attr a, const Value& val) {
    v_[makeKey(f, cell, a)] = val;
}

void ValueLayer::remove(FixtureId f, CellId cell, Attr a) { v_.erase(makeKey(f, cell, a)); }

bool ValueLayer::has(FixtureId f, CellId cell, Attr a) const {
    return v_.count(makeKey(f, cell, a)) != 0;
}

const Value* ValueLayer::find(AttrKey k) const {
    auto it = v_.find(k);
    return it == v_.end() ? nullptr : &it->second;
}

Value* ValueLayer::findMut(AttrKey k) {
    auto it = v_.find(k);
    return it == v_.end() ? nullptr : &it->second;
}

std::optional<float> ValueLayer::value(FixtureId f, CellId cell, Attr a) const {
    if (const Value* x = find(makeKey(f, cell, a))) return x->v;
    return std::nullopt;
}

bool ValueLayer::hasActive() const {
    for (const auto& [k, val] : v_) {
        (void)k;
        if (val.active) return true;
    }
    return false;
}

void ValueLayer::deactivateAll() {
    for (auto& [k, val] : v_) {
        (void)k;
        val.active = false;
    }
}

void ValueLayer::removeRefs() {
    for (auto it = v_.begin(); it != v_.end();) {
        if (it->second.isRef()) it = v_.erase(it);
        else ++it;
    }
}

void ValueLayer::flattenRefs() {
    for (auto& [k, val] : v_) {
        (void)k;
        if (!val.isRef()) continue;
        val.kind = Value::Kind::Hard;
        val.preset = Uid{};
        // v already holds the last resolved value, which is what flattening
        // means: freeze the preset's current contribution as a literal.
    }
}

ValueLayer ValueLayer::activeOnly() const {
    ValueLayer out;
    for (const auto& [k, val] : v_)
        if (val.active) out.v_[k] = val;
    return out;
}

std::vector<AttrKey> ValueLayer::keys(std::optional<Group> group) const {
    std::vector<AttrKey> out;
    out.reserve(v_.size());
    for (const auto& [k, val] : v_) {
        (void)val;
        if (group && groupOf(keyAttr(k)) != *group) continue;
        out.push_back(k);
    }
    std::sort(out.begin(), out.end());
    return out;
}

void ValueLayer::overwriteFrom(const ValueLayer& other) {
    for (const auto& [k, val] : other.v_) v_[k] = val;
}

// ---------------------------------------------------------------------------
// OutputLayer
// ---------------------------------------------------------------------------
std::optional<float> OutputLayer::get(AttrKey k) const {
    auto it = v_.find(k);
    if (it == v_.end()) return std::nullopt;
    return it->second;
}

void OutputLayer::mergeOver(const OutputLayer& other) {
    for (const auto& [k, val] : other.v_) {
        auto it = v_.find(k);
        if (it == v_.end()) {
            v_[k] = val;
            continue;
        }
        if (defaultMergeRule(keyAttr(k)) == MergeRule::HTP)
            it->second = std::max(it->second, val);
        else
            it->second = val;
    }
}

void OutputLayer::overwriteFrom(const OutputLayer& other) {
    for (const auto& [k, val] : other.v_) v_[k] = val;
}

OutputLayer crossfade(const OutputLayer& from, const OutputLayer& to, float t) {
    const float a = clamp01(t);
    OutputLayer out;
    for (const auto& [k, toVal] : to.values()) {
        const auto f = from.get(k);
        const float fromVal = f ? *f : 0.f;
        out.set(k, fromVal + (toVal - fromVal) * a);
    }
    for (const auto& [k, fromVal] : from.values()) {
        if (to.has(k)) continue;
        out.set(k, fromVal * (1.f - a));
    }
    return out;
}

} // namespace lumen
