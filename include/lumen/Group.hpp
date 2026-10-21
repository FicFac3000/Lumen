// Group.hpp — static, nested and dynamic groups (SPEC §20.8).
//
// Three kinds that COMPOSE, rather than three separate features:
//
//   Static   an ordered list. Order is capture order and is information,
//            because it drives FX spread and fan direction (§20.4).
//   Nested   a group containing groups. The selection it produces is a TREE,
//            not a flat list, because multi-level fan needs the hierarchy at
//            apply time.
//   Dynamic  defined by a query over the same facets as preset filtering
//            (§7.5), so it live-updates as the rig changes.
//
// Dynamic groups are safe for cues: a cue stores per-fixture values, not a
// group reference, so changing a group cannot alter an existing cue. A TILE
// bound to a group is a live reference, which is what you want there.
#pragma once

#include "lumen/Patch.hpp"
#include "lumen/Selection.hpp"

#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace lumen {

enum class GroupKind : uint8_t { Static, Dynamic };

const char* groupKindName(GroupKind k);

// A dynamic group's definition. Every facet is derived from Lumen's own
// taxonomy (§6), so it works identically for GDTF-imported fixtures, .hed
// imports, laser zones and mapped pixels.
struct GroupQuery {
    std::optional<std::string> typeName;
    std::optional<int> universe;
    std::optional<std::pair<int, int>> addressRange;  // inclusive
    std::optional<std::pair<FixtureId, FixtureId>> idRange;
    std::vector<std::string> anyTag;        // matches if the fixture has any
    std::optional<Attr> hasAttr;            // "everything with a Prism"
    std::optional<Group> hasGroup;          // has any attribute in this group
    std::optional<Kind> hasKind;            // e.g. anything anchored

    // A dynamic group has no manual order to store, so the query carries a
    // sort rule (§20.8).
    std::string sortKey = "patch";          // patch · id · name · x · y · z

    bool matches(const Patch& patch, const Fixture& f) const;
    bool empty() const;
};

struct GroupDef {
    Uid uid{};
    int id = 0;
    std::string name;
    GroupKind kind = GroupKind::Static;

    // Static membership, in capture order.
    std::vector<Target> members;

    // Nested children, by group UID. Composes ordered sets rather than
    // discarding their order.
    std::vector<Uid> children;

    GroupQuery query;

    // §20.8: manual overrides are a per-group TOGGLE, not a fourth kind. Off
    // by default, so a dynamic group stays a pure query unless opted in.
    bool allowOverrides = false;
    // Sticky by fixture UID, in BOTH directions: a fixture the user removed
    // stays removed even if it stops and then starts matching again. The
    // alternative lets an explicit removal silently reappear after a re-patch.
    std::vector<Uid> includes;
    std::vector<Uid> excludes;

    std::vector<std::string> tags;

    bool isDynamic() const { return kind == GroupKind::Dynamic; }
    bool isNested() const { return !children.empty(); }
};

// What a resolution produced, including why it is what it is.
struct GroupResolution {
    std::vector<Target> targets;
    int queryMatched = 0;
    int excluded = 0;
    int included = 0;
    // An exclusion for a fixture the query no longer matches anyway. Marked
    // rather than auto-removed, so the list is tidied deliberately (§20.8).
    std::vector<Uid> staleExcludes;
    bool cycleDetected = false;
};

class GroupStore {
public:
    explicit GroupStore(UidGen& uids) : uids_(&uids) {}

    GroupDef& add(GroupDef g);
    GroupDef* find(Uid uid);
    const GroupDef* find(Uid uid) const;
    GroupDef* byId(int id);
    GroupDef* byName(const std::string& name);
    const GroupDef* byName(const std::string& name) const;
    const std::deque<GroupDef>& all() const { return groups_; }
    size_t size() const { return groups_.size(); }
    bool remove(Uid uid);

    // Nesting a group inside itself, directly or transitively, is rejected at
    // assignment rather than discovered as a hang.
    bool nest(Uid parent, Uid child, std::string* err = nullptr);
    bool wouldCycle(Uid parent, Uid child) const;

    // Flat ordered membership.
    GroupResolution resolve(Uid uid, const Patch& patch) const;

    // Hierarchical, for multi-level fan (§20.6): "fan across four trusses,
    // then within each truss" needs the structure, not a flattened list.
    Selection buildSelection(Uid uid, const Patch& patch) const;

    // §20.8: converts a dynamic group to static, capturing current membership
    // and order. Commonly wanted before a show, when the rig should stop
    // moving underneath the programming.
    bool freeze(Uid uid, const Patch& patch);

    // Override helpers that respect the toggle.
    bool exclude(Uid group, Uid fixture, std::string* err = nullptr);
    bool include(Uid group, Uid fixture, std::string* err = nullptr);
    void resetOverrides(Uid group);

private:
    void resolveInto(Uid uid, const Patch& patch, std::vector<Target>& out,
                     std::vector<Uid>& visiting, GroupResolution& rep) const;

    UidGen* uids_;
    std::deque<GroupDef> groups_;
};

// Sorts targets by a named rule. Exposed because a dynamic group's order and
// a selection sort must agree.
void sortTargets(std::vector<Target>& targets, const Patch& patch,
                 const std::string& sortKey);

} // namespace lumen
