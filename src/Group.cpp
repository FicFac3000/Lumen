#include "lumen/Group.hpp"

#include <algorithm>
#include <set>

namespace lumen {

const char* groupKindName(GroupKind k) {
    switch (k) {
        case GroupKind::Static: return "Static";
        case GroupKind::Dynamic: return "Dynamic";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// GroupQuery
// ---------------------------------------------------------------------------
bool GroupQuery::empty() const {
    return !typeName && !universe && !addressRange && !idRange && anyTag.empty() &&
           !hasAttr && !hasGroup && !hasKind;
}

bool GroupQuery::matches(const Patch& patch, const Fixture& f) const {
    if (universe && f.universe != *universe) return false;

    if (addressRange) {
        if (f.address < addressRange->first || f.address > addressRange->second) return false;
    }
    if (idRange) {
        if (f.id < idRange->first || f.id > idRange->second) return false;
    }

    const FixtureType* ft = patch.typeOf(f.id);
    if (typeName) {
        if (!ft || ft->name != *typeName) return false;
    }

    if (!anyTag.empty()) {
        bool any = false;
        for (const auto& want : anyTag)
            if (std::find(f.tags.begin(), f.tags.end(), want) != f.tags.end()) any = true;
        if (!any) return false;
    }

    // The attribute facets read Lumen's own taxonomy, so they work the same
    // whatever the fixture was imported from (§6.3).
    if (hasAttr) {
        if (!ft) return false;
        bool found = false;
        for (const auto& c : ft->channels)
            if (c.attr == *hasAttr) found = true;
        if (!found) return false;
    }
    if (hasGroup) {
        if (!ft) return false;
        bool found = false;
        for (const auto& c : ft->channels)
            if (groupOf(c.attr) == *hasGroup) found = true;
        if (!found) return false;
    }
    if (hasKind) {
        if (!ft) return false;
        bool found = false;
        for (const auto& c : ft->channels)
            if (has(kindOf(c.attr), *hasKind)) found = true;
        if (!found) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Sorting
// ---------------------------------------------------------------------------
void sortTargets(std::vector<Target>& targets, const Patch& patch,
                 const std::string& sortKey) {
    if (sortKey == "none") return;

    auto key = [&](const Target& t) -> double {
        const Fixture* f = patch.find(t.fixture);
        if (!f) return 0.0;
        if (sortKey == "id") return static_cast<double>(f->id);
        if (sortKey == "x") return f->x;
        if (sortKey == "y") return f->y;
        if (sortKey == "z") return f->z;
        if (sortKey == "patch") return f->universe * 1000.0 + f->address;
        return static_cast<double>(f->order);
    };

    if (sortKey == "name") {
        std::stable_sort(targets.begin(), targets.end(), [&](const Target& a, const Target& b) {
            const Fixture* fa = patch.find(a.fixture);
            const Fixture* fb = patch.find(b.fixture);
            return (fa ? fa->name : "") < (fb ? fb->name : "");
        });
        return;
    }
    std::stable_sort(targets.begin(), targets.end(),
                     [&](const Target& a, const Target& b) {
                         const double ka = key(a), kb = key(b);
                         if (ka != kb) return ka < kb;
                         return a.cell < b.cell;
                     });
}

// ---------------------------------------------------------------------------
// GroupStore
// ---------------------------------------------------------------------------
GroupDef& GroupStore::add(GroupDef g) {
    if (!g.uid.valid()) g.uid = uids_->next();
    if (g.id == 0) {
        int maxId = 0;
        for (const auto& e : groups_) maxId = std::max(maxId, e.id);
        g.id = maxId + 1;
    }
    groups_.push_back(std::move(g));
    return groups_.back();
}

GroupDef* GroupStore::find(Uid uid) {
    for (auto& g : groups_)
        if (g.uid == uid) return &g;
    return nullptr;
}

const GroupDef* GroupStore::find(Uid uid) const {
    for (const auto& g : groups_)
        if (g.uid == uid) return &g;
    return nullptr;
}

GroupDef* GroupStore::byId(int id) {
    for (auto& g : groups_)
        if (g.id == id) return &g;
    return nullptr;
}

GroupDef* GroupStore::byName(const std::string& name) {
    for (auto& g : groups_)
        if (g.name == name) return &g;
    return nullptr;
}

const GroupDef* GroupStore::byName(const std::string& name) const {
    for (const auto& g : groups_)
        if (g.name == name) return &g;
    return nullptr;
}

bool GroupStore::remove(Uid uid) {
    for (auto it = groups_.begin(); it != groups_.end(); ++it) {
        if (it->uid != uid) continue;
        groups_.erase(it);
        // Drop references from anything that nested it, so no group is left
        // pointing at something gone.
        for (auto& g : groups_)
            g.children.erase(std::remove(g.children.begin(), g.children.end(), uid),
                             g.children.end());
        return true;
    }
    return false;
}

bool GroupStore::wouldCycle(Uid parent, Uid child) const {
    if (parent == child) return true;
    // Walk down from the child: if the parent appears, nesting would close a
    // loop. Depth-first with a visited set, so an existing cycle in the data
    // cannot hang this check either.
    std::vector<Uid> stack{child};
    std::set<uint64_t> seen;
    while (!stack.empty()) {
        const Uid cur = stack.back();
        stack.pop_back();
        if (!seen.insert(cur.v).second) continue;
        if (cur == parent) return true;
        if (const GroupDef* g = find(cur))
            for (Uid c : g->children) stack.push_back(c);
    }
    return false;
}

bool GroupStore::nest(Uid parent, Uid child, std::string* err) {
    GroupDef* p = find(parent);
    const GroupDef* c = find(child);
    if (!p || !c) {
        if (err) *err = "no such group";
        return false;
    }
    if (wouldCycle(parent, child)) {
        // Rejected at assignment rather than discovered as a hang.
        if (err) *err = "would create a cycle";
        return false;
    }
    if (std::find(p->children.begin(), p->children.end(), child) == p->children.end())
        p->children.push_back(child);
    return true;
}

void GroupStore::resolveInto(Uid uid, const Patch& patch, std::vector<Target>& out,
                             std::vector<Uid>& visiting, GroupResolution& rep) const {
    const GroupDef* g = find(uid);
    if (!g) return;

    if (std::find(visiting.begin(), visiting.end(), uid) != visiting.end()) {
        // Defensive: data loaded from elsewhere could contain a cycle even
        // though nest() refuses to create one.
        rep.cycleDetected = true;
        return;
    }
    visiting.push_back(uid);

    // Nested children first, each contributing its own stored order.
    for (Uid child : g->children) resolveInto(child, patch, out, visiting, rep);

    std::vector<Target> own;

    if (g->isDynamic()) {
        for (const Fixture& f : patch.fixtures())
            if (g->query.matches(patch, f)) own.push_back({f.id, 0});
        rep.queryMatched += static_cast<int>(own.size());
        sortTargets(own, patch, g->query.sortKey);

        if (g->allowOverrides) {
            // Excludes first, so an include can re-add something the query
            // never matched without being cancelled by a stale exclusion.
            std::set<uint64_t> excl;
            for (Uid u : g->excludes) excl.insert(u.v);

            std::vector<Target> kept;
            std::set<uint64_t> matchedUids;
            for (const Target& t : own) {
                const Fixture* f = patch.find(t.fixture);
                if (!f) continue;
                matchedUids.insert(f->uid.v);
                if (excl.count(f->uid.v)) {
                    ++rep.excluded;
                    continue;
                }
                kept.push_back(t);
            }
            own = std::move(kept);

            // An exclusion for something the query no longer matches is
            // marked, not removed: the list is tidied deliberately (§20.8).
            for (Uid u : g->excludes)
                if (!matchedUids.count(u.v)) rep.staleExcludes.push_back(u);

            for (Uid u : g->includes) {
                const Fixture* f = patch.findByUid(u);
                if (!f) continue;
                const Target t{f->id, 0};
                if (std::find(own.begin(), own.end(), t) != own.end()) continue;
                own.push_back(t);
                ++rep.included;
            }
        }
    } else {
        own = g->members;
    }

    for (const Target& t : own)
        if (std::find(out.begin(), out.end(), t) == out.end()) out.push_back(t);

    visiting.pop_back();
}

GroupResolution GroupStore::resolve(Uid uid, const Patch& patch) const {
    GroupResolution rep;
    std::vector<Uid> visiting;
    resolveInto(uid, patch, rep.targets, visiting, rep);
    return rep;
}

Selection GroupStore::buildSelection(Uid uid, const Patch& patch) const {
    Selection sel;
    const GroupDef* g = find(uid);
    if (!g) return sel;

    // §20.8: the hierarchy survives, because multi-level fan needs it. Each
    // nested child becomes its own node rather than being flattened away.
    if (g->isNested()) {
        for (Uid child : g->children) {
            const GroupDef* c = find(child);
            if (!c) continue;
            const GroupResolution r = resolve(child, patch);
            if (!r.targets.empty()) sel.addGroup(c->uid, c->name, r.targets);
        }
        // Any direct members of the parent sit alongside its children.
        std::vector<Target> direct;
        if (!g->isDynamic()) {
            direct = g->members;
        } else {
            direct = resolve(uid, patch).targets;
            // Strip anything the children already contributed.
            std::vector<Target> childTargets;
            for (Uid child : g->children) {
                const auto r = resolve(child, patch);
                childTargets.insert(childTargets.end(), r.targets.begin(), r.targets.end());
            }
            direct.erase(std::remove_if(direct.begin(), direct.end(),
                                        [&](const Target& t) {
                                            return std::find(childTargets.begin(),
                                                             childTargets.end(),
                                                             t) != childTargets.end();
                                        }),
                         direct.end());
        }
        if (!direct.empty()) sel.add(direct);
        return sel;
    }

    sel.set(resolve(uid, patch).targets);
    return sel;
}

bool GroupStore::freeze(Uid uid, const Patch& patch) {
    GroupDef* g = find(uid);
    if (!g) return false;

    const GroupResolution r = resolve(uid, patch);
    g->members = r.targets;
    g->kind = GroupKind::Static;
    g->children.clear();
    g->query = GroupQuery{};
    g->includes.clear();
    g->excludes.clear();
    g->allowOverrides = false;
    return true;
}

bool GroupStore::exclude(Uid group, Uid fixture, std::string* err) {
    GroupDef* g = find(group);
    if (!g) {
        if (err) *err = "no such group";
        return false;
    }
    if (!g->isDynamic()) {
        if (err) *err = "overrides apply to dynamic groups; edit a static group directly";
        return false;
    }
    if (!g->allowOverrides) {
        // Off by default, so a dynamic group stays a pure query unless the
        // user opts in (§20.8).
        if (err) *err = "manual overrides are not enabled for this group";
        return false;
    }
    if (std::find(g->excludes.begin(), g->excludes.end(), fixture) == g->excludes.end())
        g->excludes.push_back(fixture);
    g->includes.erase(std::remove(g->includes.begin(), g->includes.end(), fixture),
                      g->includes.end());
    return true;
}

bool GroupStore::include(Uid group, Uid fixture, std::string* err) {
    GroupDef* g = find(group);
    if (!g) {
        if (err) *err = "no such group";
        return false;
    }
    if (!g->isDynamic()) {
        if (err) *err = "overrides apply to dynamic groups; edit a static group directly";
        return false;
    }
    if (!g->allowOverrides) {
        if (err) *err = "manual overrides are not enabled for this group";
        return false;
    }
    if (std::find(g->includes.begin(), g->includes.end(), fixture) == g->includes.end())
        g->includes.push_back(fixture);
    g->excludes.erase(std::remove(g->excludes.begin(), g->excludes.end(), fixture),
                      g->excludes.end());
    return true;
}

void GroupStore::resetOverrides(Uid group) {
    if (GroupDef* g = find(group)) {
        g->includes.clear();
        g->excludes.clear();
    }
}

} // namespace lumen
