#include "lumen/Selection.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>

namespace lumen {

namespace {

constexpr double kTau = 6.283185307179586;

uint64_t splitmix(uint64_t& x) {
    uint64_t z = (x += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

std::string lower(const std::string& s) {
    std::string o = s;
    std::transform(o.begin(), o.end(), o.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return o;
}

void flattenInto(const SelNode& n, std::vector<Target>& out) {
    if (n.type == SelNode::Type::Fixture) {
        out.push_back(n.target);
        return;
    }
    for (const auto& c : n.children) flattenInto(c, out);
}

size_t depthOf(const SelNode& n) {
    if (n.type == SelNode::Type::Fixture) return 1;
    size_t d = 0;
    for (const auto& c : n.children) d = std::max(d, depthOf(c));
    return d + 1;
}

} // namespace

// ---------------------------------------------------------------------------
// SelNode
// ---------------------------------------------------------------------------
SelNode SelNode::fixture(Target t) {
    SelNode n;
    n.type = Type::Fixture;
    n.target = t;
    return n;
}

SelNode SelNode::makeGroup(Uid uid, std::string label, std::vector<SelNode> members) {
    SelNode n;
    n.type = Type::Group;
    n.group = uid;
    n.label = std::move(label);
    n.children = std::move(members);
    return n;
}

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------
void Selection::set(std::vector<Target> targets) {
    roots_.clear();
    add(targets);
}

void Selection::add(const std::vector<Target>& targets) {
    for (const Target& t : targets) {
        if (contains(t)) continue;
        roots_.push_back(SelNode::fixture(t));
    }
}

void Selection::addGroup(Uid uid, const std::string& label, std::vector<Target> members) {
    std::vector<SelNode> kids;
    kids.reserve(members.size());
    for (const Target& t : members) kids.push_back(SelNode::fixture(t));
    roots_.push_back(SelNode::makeGroup(uid, label, std::move(kids)));
}

void Selection::remove(const Target& t) {
    // Removing a target prunes it wherever it sits, including inside a group,
    // then drops any group left empty.
    std::function<void(std::vector<SelNode>&)> prune = [&](std::vector<SelNode>& v) {
        for (auto it = v.begin(); it != v.end();) {
            if (it->type == SelNode::Type::Fixture) {
                if (it->target == t) { it = v.erase(it); continue; }
            } else {
                prune(it->children);
                if (it->children.empty()) { it = v.erase(it); continue; }
            }
            ++it;
        }
    };
    prune(roots_);
}

void Selection::toggle(const Target& t) {
    if (contains(t)) remove(t);
    else roots_.push_back(SelNode::fixture(t));
}

std::vector<Target> Selection::flatten() const {
    std::vector<Target> out;
    for (const auto& n : roots_) flattenInto(n, out);
    return out;
}

bool Selection::contains(const Target& t) const {
    const auto flat = flatten();
    return std::find(flat.begin(), flat.end(), t) != flat.end();
}

std::vector<std::vector<Target>> Selection::groupsAtTopLevel() const {
    std::vector<std::vector<Target>> out;
    for (const auto& n : roots_) {
        std::vector<Target> one;
        flattenInto(n, one);
        if (!one.empty()) out.push_back(std::move(one));
    }
    return out;
}

size_t Selection::depth() const {
    size_t d = 0;
    for (const auto& n : roots_) d = std::max(d, depthOf(n));
    return d;
}

void Selection::reverse() {
    std::reverse(roots_.begin(), roots_.end());
    for (auto& n : roots_)
        if (n.type == SelNode::Type::Group) std::reverse(n.children.begin(), n.children.end());
}

void Selection::shuffle(uint64_t seed) {
    const auto idx = shuffledIndices(roots_.size(), seed);
    std::vector<SelNode> next;
    next.reserve(roots_.size());
    for (size_t i : idx) next.push_back(roots_[i]);
    roots_ = std::move(next);
}

void Selection::rotate(int by) {
    if (roots_.empty()) return;
    const int n = static_cast<int>(roots_.size());
    int k = ((by % n) + n) % n;
    std::rotate(roots_.begin(), roots_.begin() + k, roots_.end());
}

void Selection::keepEveryNth(int n, int offset) {
    if (n < 1) return;
    std::vector<SelNode> next;
    for (size_t i = 0; i < roots_.size(); ++i)
        if ((static_cast<int>(i) - offset) % n == 0 && static_cast<int>(i) >= offset)
            next.push_back(roots_[i]);
    roots_ = std::move(next);
}

void Selection::keepOdd() { keepEveryNth(2, 0); }
void Selection::keepEven() { keepEveryNth(2, 1); }

void Selection::sortBy(const Patch& patch, const std::string& key) {
    const std::string k = lower(key);
    auto value = [&](const SelNode& n) -> double {
        std::vector<Target> flat;
        flattenInto(n, flat);
        if (flat.empty()) return 0.0;
        const Fixture* f = patch.find(flat.front().fixture);
        if (!f) return 0.0;
        if (k == "id") return static_cast<double>(f->id);
        if (k == "patch") return f->universe * 1000.0 + f->address;
        if (k == "x") return f->x;
        if (k == "y") return f->y;
        if (k == "z") return f->z;
        return static_cast<double>(f->order);
    };

    if (k == "name") {
        std::stable_sort(roots_.begin(), roots_.end(),
                         [&](const SelNode& a, const SelNode& b) {
                             std::vector<Target> fa, fb;
                             flattenInto(a, fa);
                             flattenInto(b, fb);
                             const Fixture* pa = fa.empty() ? nullptr : patch.find(fa.front().fixture);
                             const Fixture* pb = fb.empty() ? nullptr : patch.find(fb.front().fixture);
                             return (pa ? pa->name : "") < (pb ? pb->name : "");
                         });
        return;
    }
    std::stable_sort(roots_.begin(), roots_.end(),
                     [&](const SelNode& a, const SelNode& b) { return value(a) < value(b); });
}

// ---------------------------------------------------------------------------
// Spread
// ---------------------------------------------------------------------------
const char* spreadSourceName(SpreadSource s) {
    switch (s) {
        case SpreadSource::Index: return "Index";
        case SpreadSource::PatchOrder: return "PatchOrder";
        case SpreadSource::StageX: return "StageX";
        case SpreadSource::StageY: return "StageY";
        case SpreadSource::StageZ: return "StageZ";
        case SpreadSource::StageRadial: return "StageRadial";
        case SpreadSource::StageAngle: return "StageAngle";
        case SpreadSource::Shuffle: return "Shuffle";
        case SpreadSource::Random: return "Random";
        case SpreadSource::Count: break;
    }
    return "?";
}

std::optional<SpreadSource> spreadSourceFromName(const std::string& s) {
    const std::string k = lower(s);
    for (size_t i = 0; i < static_cast<size_t>(SpreadSource::Count); ++i) {
        const auto src = static_cast<SpreadSource>(i);
        if (lower(spreadSourceName(src)) == k) return src;
    }
    if (k == "linear" || k == "order") return SpreadSource::Index;
    if (k == "angle") return SpreadSource::StageAngle;
    if (k == "radial") return SpreadSource::StageRadial;
    return std::nullopt;
}

std::vector<size_t> shuffledIndices(size_t n, uint64_t seed,
                                    std::optional<size_t> avoidFirst) {
    std::vector<size_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = i;
    if (n < 2) return v;

    uint64_t s = seed ? seed : 0x9e3779b97f4a7c15ull;
    // Up to a few attempts to avoid opening on the value the previous cycle
    // closed on — the one visible repeat a permutation is meant to prevent,
    // which otherwise lands exactly at the seam between cycles (§4.12).
    for (int attempt = 0; attempt < 8; ++attempt) {
        for (size_t i = n - 1; i > 0; --i) {
            const size_t j = static_cast<size_t>(splitmix(s) % (i + 1));
            std::swap(v[i], v[j]);
        }
        if (!avoidFirst || v.front() != *avoidFirst) break;
    }
    return v;
}

float applyModifiers(float spread, const SpreadModifiers& m, size_t n) {
    float s = spread;

    // Blocks: N adjacent members share one phase. With `b` members per block
    // there are ceil(n/b) distinct phases, so quantising the normalised value
    // into that many buckets reproduces MA3's Block — and because it quantises
    // the value rather than an index, it works identically for continuous
    // sources such as stage angle.
    if (m.blocks > 1 && n > 1) {
        const float nb = std::ceil(static_cast<float>(n) / static_cast<float>(m.blocks));
        if (nb >= 1.f) s = std::floor(s * nb) / nb;
    }

    // Wings: fold into N symmetric wings, mirroring alternate ones.
    if (m.wings > 1) {
        const float w = static_cast<float>(m.wings);
        float x = s * w;
        const float wing = std::floor(x);
        float frac = x - wing;
        if (static_cast<int>(wing) % 2 == 1) frac = 1.f - frac;
        s = frac;
    }

    s = s * m.groups + m.offset;
    s = std::fmod(s, 1.f);
    if (s < 0.f) s += 1.f;
    return s;
}

void SpreadTable::buildFromTargets(const std::vector<Target>& targets, const Patch& patch,
                                   SpreadSource source, uint64_t seed,
                                   float centreX, float centreY) {
    Selection s;
    s.set(targets);
    build(s, patch, source, seed, centreX, centreY);
}

void SpreadTable::build(const Selection& sel, const Patch& patch, SpreadSource source,
                        uint64_t seed, float centreX, float centreY) {
    order_ = sel.flatten();
    spread_.assign(order_.size(), 0.f);
    const size_t n = order_.size();
    if (n == 0) return;
    if (n == 1) {
        spread_[0] = 0.f;
        return;
    }

    auto fx = [&](size_t i) { return patch.find(order_[i].fixture); };

    switch (source) {
        case SpreadSource::Index: {
            for (size_t i = 0; i < n; ++i)
                spread_[i] = static_cast<float>(i) / static_cast<float>(n);
            break;
        }
        case SpreadSource::PatchOrder: {
            std::vector<std::pair<double, size_t>> keyed;
            keyed.reserve(n);
            for (size_t i = 0; i < n; ++i) {
                const Fixture* f = fx(i);
                const double k = f ? f->universe * 1000.0 + f->address : 0.0;
                keyed.push_back({k, i});
            }
            std::sort(keyed.begin(), keyed.end());
            for (size_t r = 0; r < n; ++r)
                spread_[keyed[r].second] = static_cast<float>(r) / static_cast<float>(n);
            break;
        }
        case SpreadSource::StageX:
        case SpreadSource::StageY:
        case SpreadSource::StageZ: {
            // Normalise against the actual extent, so a rig three metres wide
            // and one thirty metres wide both spread across a full cycle.
            double lo = 1e30, hi = -1e30;
            std::vector<double> raw(n, 0.0);
            for (size_t i = 0; i < n; ++i) {
                const Fixture* f = fx(i);
                double v = 0.0;
                if (f) v = source == SpreadSource::StageX ? f->x
                         : source == SpreadSource::StageY ? f->y : f->z;
                raw[i] = v;
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
            const double span = hi - lo;
            for (size_t i = 0; i < n; ++i)
                spread_[i] = span > 1e-9 ? static_cast<float>((raw[i] - lo) / span) : 0.f;
            break;
        }
        case SpreadSource::StageRadial: {
            double hi = 0.0;
            std::vector<double> raw(n, 0.0);
            for (size_t i = 0; i < n; ++i) {
                const Fixture* f = fx(i);
                const double dx = (f ? f->x : 0.0) - centreX;
                const double dy = (f ? f->y : 0.0) - centreY;
                raw[i] = std::sqrt(dx * dx + dy * dy);
                hi = std::max(hi, raw[i]);
            }
            for (size_t i = 0; i < n; ++i)
                spread_[i] = hi > 1e-9 ? static_cast<float>(raw[i] / hi) : 0.f;
            break;
        }
        case SpreadSource::StageAngle: {
            // This is what a circular chase across a physically circular rig
            // needs, and no amount of linear ordering produces it (§20.2).
            for (size_t i = 0; i < n; ++i) {
                const Fixture* f = fx(i);
                const double dx = (f ? f->x : 0.0) - centreX;
                const double dy = (f ? f->y : 0.0) - centreY;
                double ang = std::atan2(dy, dx) / kTau;
                if (ang < 0.0) ang += 1.0;
                spread_[i] = static_cast<float>(ang);
            }
            break;
        }
        case SpreadSource::Shuffle: {
            const auto idx = shuffledIndices(n, seed);
            for (size_t r = 0; r < n; ++r)
                spread_[idx[r]] = static_cast<float>(r) / static_cast<float>(n);
            break;
        }
        case SpreadSource::Random: {
            uint64_t s = seed ? seed : 1;
            for (size_t i = 0; i < n; ++i)
                spread_[i] = static_cast<float>(splitmix(s) % 100000u) / 100000.f;
            break;
        }
        case SpreadSource::Count: break;
    }
}

float SpreadTable::raw(const Target& t) const {
    for (size_t i = 0; i < order_.size(); ++i)
        if (order_[i] == t) return spread_[i];
    return 0.f;
}

float SpreadTable::phase(const Target& t, const SpreadModifiers& m) const {
    return applyModifiers(raw(t), m, order_.size());
}

} // namespace lumen
