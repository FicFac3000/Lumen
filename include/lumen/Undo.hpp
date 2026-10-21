// Undo.hpp — undo, redo and the action log (SPEC §8.6).
//
// Three decisions from §8.6 shape this file:
//
//   PLAYBACK IS NOT UNDOABLE. Go, Off, Release, fader moves and flashes are
//   PERFORMANCE, not edits. If Undo could rewind a playback it would change
//   live output, so the system covers edits only. That is what makes Undo
//   safe to press during a show, which is worth more than un-Going a cue.
//   Enforced by what records: a playback command creates no entry at all.
//
//   ENTRIES ARE TRANSACTIONAL. One entry may hold many operations, applied
//   and reverted atomically. §7.4's renumber flow and §16.5's agent batches
//   both require it.
//
//   SCOPED UNDO IS GUARDED. With a history of [programmer A, patch B,
//   programmer C], Ctrl+Undo twice would revert C then A and leave B — fine
//   if B is independent, corrupting if B built on A. So every entry records
//   the object UIDs it touched, and a scoped undo is refused when a later
//   entry touched an overlapping set. It names the blocker rather than
//   silently doing the wrong thing.
//
// Deltas, not snapshots: an operation captures BY VALUE only the objects it
// touched, and its restore functions take the Engine as an argument rather
// than capturing it. That removes the dangling-reference hazard a closure
// over the model would otherwise carry, and keeps a 2000-fixture patch to
// 2000 small records rather than 2000 copies of the whole patch.
#pragma once

#include "lumen/Patch.hpp"
#include "lumen/Uid.hpp"
#include "lumen/Value.hpp"

#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace lumen {

class Engine;

// ---------------------------------------------------------------------------
// Scope
// ---------------------------------------------------------------------------
// There is deliberately NO Playback scope: a thing that cannot be undone has
// no scope in the undo system.
enum class EditScope : uint8_t {
    Programmer,
    Patch,
    Cue,
    Preset,
    Group,
    Recipe,
    Binding,
    Layout,
    Library,
    Mixed,     // a transaction spanning several
};

const char* editScopeName(EditScope s);
// The reverse, for the command layer's scope argument.
std::optional<EditScope> editScopeFromName(const std::string& name);

// ---------------------------------------------------------------------------
// Operation
// ---------------------------------------------------------------------------
// The restores take the Engine rather than capturing it, so an operation
// holds nothing but copied state and is safe to keep for as long as the
// history does.
using Restore = std::function<void(Engine&)>;

struct Operation {
    std::string description;
    EditScope scope = EditScope::Mixed;
    // The objects this touched, which is what makes scoped undo safe.
    std::vector<Uid> touched;
    Restore undo;
    Restore redo;
    // Roughly what this operation costs to keep, for the depth cap.
    size_t bytes = 0;

    bool valid() const { return undo != nullptr && redo != nullptr; }
};

// ---------------------------------------------------------------------------
// History entry
// ---------------------------------------------------------------------------
struct HistoryEntry {
    Uid uid{};
    int id = 0;
    std::string description;
    EditScope scope = EditScope::Mixed;
    double time = 0.0;
    // Who did it: the operator, a macro, a plugin, the agent (§16.5), or a
    // session member's name (§18.7).
    std::string source = "operator";
    std::vector<Operation> ops;
    std::vector<Uid> touched;   // the union over ops
    size_t bytes = 0;

    bool touchesAnyOf(const std::vector<Uid>& other) const;
};

// ---------------------------------------------------------------------------
// Result of an undo or redo
// ---------------------------------------------------------------------------
struct UndoReport {
    enum class Result : uint8_t {
        Ok,
        Nothing,       // nothing left in that direction
        Blocked,       // a later entry touched the same objects
        NoSuchEntry,
    };
    Result result = Result::Nothing;
    std::string description;
    // §8.6: say which action blocks it, so the refusal is actionable.
    std::string blockedBy;
    int entriesAffected = 0;
    bool ok() const { return result == Result::Ok; }
};

const char* undoResultName(UndoReport::Result r);

// ---------------------------------------------------------------------------
// History
// ---------------------------------------------------------------------------
class History {
public:
    explicit History(UidGen& uids) : uids_(&uids) {}

    // --- recording ---------------------------------------------------------
    // A transaction groups operations so they revert as a unit. Nesting is
    // counted rather than refused: a command that calls another must not have
    // the inner one close the outer one's transaction.
    void begin(const std::string& description, EditScope scope = EditScope::Mixed,
               const std::string& source = "operator");
    bool inTransaction() const { return depth_ > 0; }
    int transactionDepth() const { return depth_; }

    // Adds to the open transaction, or pushes a one-operation entry when
    // there is none.
    Uid record(Operation op);

    // Closes the outermost transaction. An empty one is discarded rather than
    // leaving a history entry that undoes nothing.
    Uid commit();
    // Reverts whatever the open transaction already did and discards it, so a
    // command that fails halfway leaves no trace.
    void abort(Engine& eng);

    // --- undo / redo -------------------------------------------------------
    UndoReport undo(Engine& eng);
    UndoReport redo(Engine& eng);

    // Ctrl+Undo: the last entry of this scope, guarded against collisions.
    UndoReport undoScoped(Engine& eng, EditScope scope);
    UndoReport redoScoped(Engine& eng, EditScope scope);

    // Undo everything back THROUGH a chosen entry, as one operation (§8.6's
    // jump-to-point).
    UndoReport undoThrough(Engine& eng, Uid entry);

    bool canUndo() const { return cursor_ > 0; }
    bool canRedo() const { return cursor_ < static_cast<int>(entries_.size()); }
    const HistoryEntry* nextUndo() const;
    const HistoryEntry* nextRedo() const;

    // --- inspection --------------------------------------------------------
    const std::deque<HistoryEntry>& entries() const { return entries_; }
    // Entries before the cursor are applied; from the cursor on they are
    // undone and available to redo.
    int cursor() const { return cursor_; }
    size_t bytes() const;
    void clear();

    // --- limits ------------------------------------------------------------
    // §8.6: a bulk patch of 2000 fixtures must not blow memory.
    void setDepth(int entries);
    int depthLimit() const { return depthLimit_; }
    void setByteCap(size_t bytes);
    size_t byteCap() const { return byteCap_; }
    int evicted() const { return evicted_; }

    void setClock(std::function<double()> clock) { clock_ = std::move(clock); }

private:
    void push(HistoryEntry e);
    void dropRedoTail();
    void enforceLimits();
    double now() const;
    int findScoped(EditScope scope) const;

    UidGen* uids_;
    std::deque<HistoryEntry> entries_;
    int cursor_ = 0;
    int depth_ = 0;
    std::optional<HistoryEntry> open_;
    int depthLimit_ = 200;
    size_t byteCap_ = 64u * 1024u * 1024u;
    int evicted_ = 0;
    int nextId_ = 1;
    std::function<double()> clock_;
};

// ---------------------------------------------------------------------------
// Action log (§8.6, §16.5)
// ---------------------------------------------------------------------------
// The other record. Persisted in the show, full depth, no state — it exists
// to answer "what happened and who did it", not to revert anything. An agent
// action carries the prompt that caused it, so the two systems share one
// timeline.
struct LogEntry {
    Uid uid{};
    double time = 0.0;
    std::string description;
    std::string scope;
    std::string source;
    std::string prompt;   // §16.5: the prompt behind an agent action
    bool undone = false;  // marked when it is reverted, rather than deleted
};

class ActionLog {
public:
    explicit ActionLog(UidGen& uids) : uids_(&uids) {}

    Uid add(const std::string& description, const std::string& scope,
            const std::string& source = "operator", const std::string& prompt = {});
    LogEntry& restore(LogEntry e);
    void markUndone(const std::string& description);

    const std::deque<LogEntry>& all() const { return entries_; }
    size_t size() const { return entries_.size(); }
    void clear() { entries_.clear(); }

    void setClock(std::function<double()> clock) { clock_ = std::move(clock); }

private:
    UidGen* uids_;
    std::deque<LogEntry> entries_;
    std::function<double()> clock_;
};

// ---------------------------------------------------------------------------
// Capture helpers
// ---------------------------------------------------------------------------
// The call-site pattern: construct before mutating, commit after. Each guard
// captures only the object it names, by value.
//
//   auto u = ProgrammerEdit(eng, "At 100");
//   ... mutate the programmer ...
//   u.commit(eng.history);

class ProgrammerEdit {
public:
    ProgrammerEdit(const Engine& eng, std::string description);
    // Returns the entry, or an invalid Uid when nothing actually changed —
    // a command that was a no-op should not leave something to undo.
    Uid commit(History& history, Engine& eng);
    void cancel() { cancelled_ = true; }

private:
    std::string description_;
    ValueLayer before_;
    bool cancelled_ = false;
};

// Fixtures, as per-fixture deltas. A bulk patch records one small record per
// fixture rather than a copy of the whole patch, which is what keeps 2000 of
// them affordable.
class PatchEdit {
public:
    // One fixture's state, or its absence. `present` false records that the
    // fixture did not exist, which is how patching something new undoes.
    struct Snap {
        FixtureId id = 0;
        bool present = false;
        Fixture fixture;
        std::string typeName;   // types are shared, so the name is the link
    };

    PatchEdit(const Engine& eng, std::string description, std::vector<FixtureId> ids);
    // Every fixture in the patch, for an operation whose reach is not known
    // up front.
    static PatchEdit all(const Engine& eng, std::string description);
    Uid commit(History& history, Engine& eng);
    void cancel() { cancelled_ = true; }

private:
    std::string description_;
    std::vector<FixtureId> ids_;
    std::vector<Snap> before_;
    bool cancelled_ = false;
};

} // namespace lumen
