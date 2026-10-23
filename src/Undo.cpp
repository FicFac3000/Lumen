#include "lumen/Undo.hpp"

#include "lumen/Engine.hpp"

#include <algorithm>

namespace lumen {

const char* editScopeName(EditScope s) {
    switch (s) {
        case EditScope::Programmer: return "programmer";
        case EditScope::Patch: return "patch";
        case EditScope::Cue: return "cue";
        case EditScope::Preset: return "preset";
        case EditScope::Group: return "group";
        case EditScope::Recipe: return "recipe";
        case EditScope::Binding: return "binding";
        case EditScope::Layout: return "layout";
        case EditScope::Library: return "library";
        case EditScope::Mixed: return "mixed";
    }
    return "mixed";
}

std::optional<EditScope> editScopeFromName(const std::string& name) {
    for (EditScope s : {EditScope::Programmer, EditScope::Patch, EditScope::Cue,
                        EditScope::Preset, EditScope::Group, EditScope::Recipe,
                        EditScope::Binding, EditScope::Layout, EditScope::Library,
                        EditScope::Mixed})
        if (name == editScopeName(s)) return s;
    return std::nullopt;
}

const char* undoResultName(UndoReport::Result r) {
    switch (r) {
        case UndoReport::Result::Ok: return "ok";
        case UndoReport::Result::Nothing: return "nothing";
        case UndoReport::Result::Blocked: return "blocked";
        case UndoReport::Result::NoSuchEntry: return "no such entry";
    }
    return "nothing";
}

bool HistoryEntry::touchesAnyOf(const std::vector<Uid>& other) const {
    for (Uid a : touched)
        for (Uid b : other)
            if (a == b) return true;
    return false;
}

// ---------------------------------------------------------------------------
// History
// ---------------------------------------------------------------------------
double History::now() const { return clock_ ? clock_() : 0.0; }

void History::begin(const std::string& description, EditScope scope,
                    const std::string& source) {
    if (depth_++ > 0) return;   // nested: the outer transaction owns it
    HistoryEntry e;
    e.description = description;
    e.scope = scope;
    e.source = source;
    e.time = now();
    open_ = std::move(e);
}

Uid History::record(Operation op) {
    if (!op.valid()) return Uid{};

    if (open_) {
        // A transaction spanning scopes is Mixed, so a scoped undo cannot
        // pick it up as if it were purely one thing.
        if (open_->ops.empty() && open_->scope == EditScope::Mixed)
            open_->scope = op.scope;
        else if (open_->scope != op.scope)
            open_->scope = EditScope::Mixed;

        open_->bytes += op.bytes;
        for (Uid u : op.touched)
            if (std::find(open_->touched.begin(), open_->touched.end(), u) ==
                open_->touched.end())
                open_->touched.push_back(u);
        open_->ops.push_back(std::move(op));
        return open_->uid;
    }

    HistoryEntry e;
    e.description = op.description;
    e.scope = op.scope;
    e.time = now();
    e.bytes = op.bytes;
    e.touched = op.touched;
    e.ops.push_back(std::move(op));
    e.uid = uids_->next();
    e.id = nextId_++;
    const Uid uid = e.uid;
    push(std::move(e));
    return uid;
}

Uid History::commit() {
    if (depth_ == 0) return Uid{};
    if (--depth_ > 0) return open_ ? open_->uid : Uid{};
    if (!open_) return Uid{};

    HistoryEntry e = std::move(*open_);
    open_.reset();
    // An empty transaction is discarded: a history entry that undoes nothing
    // would make Undo look broken.
    if (e.ops.empty()) return Uid{};

    e.uid = uids_->next();
    e.id = nextId_++;
    const Uid uid = e.uid;
    push(std::move(e));
    return uid;
}

void History::abort(Engine& eng) {
    if (depth_ == 0) return;
    depth_ = 0;
    if (!open_) return;
    // Whatever the failed command already did is reverted, newest first, so a
    // command that fails halfway leaves no trace.
    HistoryEntry e = std::move(*open_);
    open_.reset();
    for (auto it = e.ops.rbegin(); it != e.ops.rend(); ++it)
        if (it->undo) it->undo(eng);
}

void History::push(HistoryEntry e) {
    dropRedoTail();
    entries_.push_back(std::move(e));
    cursor_ = static_cast<int>(entries_.size());
    enforceLimits();
}

void History::dropRedoTail() {
    // A new edit after an undo invalidates the redo tail, because the states
    // those entries would restore no longer follow from here.
    while (static_cast<int>(entries_.size()) > cursor_) entries_.pop_back();
}

void History::enforceLimits() {
    auto overBytes = [&]() {
        size_t total = 0;
        for (const HistoryEntry& e : entries_) total += e.bytes;
        return total > byteCap_;
    };
    // Oldest first, and never past the cursor: evicting an applied entry just
    // makes it un-undoable, which is the point of a cap.
    while ((static_cast<int>(entries_.size()) > depthLimit_ || overBytes()) &&
           entries_.size() > 1 && cursor_ > 0) {
        entries_.pop_front();
        --cursor_;
        ++evicted_;
    }
}

void History::setDepth(int entries) {
    depthLimit_ = std::max(1, entries);
    enforceLimits();
}

void History::setByteCap(size_t bytes) {
    byteCap_ = bytes;
    enforceLimits();
}

size_t History::bytes() const {
    size_t total = 0;
    for (const HistoryEntry& e : entries_) total += e.bytes;
    return total;
}

void History::clear() {
    entries_.clear();
    cursor_ = 0;
    depth_ = 0;
    open_.reset();
    evicted_ = 0;
}

const HistoryEntry* History::nextUndo() const {
    if (!canUndo()) return nullptr;
    return &entries_[static_cast<size_t>(cursor_ - 1)];
}

const HistoryEntry* History::nextRedo() const {
    if (!canRedo()) return nullptr;
    return &entries_[static_cast<size_t>(cursor_)];
}

UndoReport History::undo(Engine& eng) {
    UndoReport r;
    if (!canUndo()) {
        r.description = "nothing to undo";
        return r;
    }
    HistoryEntry& e = entries_[static_cast<size_t>(cursor_ - 1)];
    // Newest operation first: a transaction reverts in reverse order or an
    // operation can undo onto state a later one has not yet put back.
    for (auto it = e.ops.rbegin(); it != e.ops.rend(); ++it) it->undo(eng);
    --cursor_;
    r.result = UndoReport::Result::Ok;
    r.description = e.description;
    r.entriesAffected = 1;
    return r;
}

UndoReport History::redo(Engine& eng) {
    UndoReport r;
    if (!canRedo()) {
        r.description = "nothing to redo";
        return r;
    }
    HistoryEntry& e = entries_[static_cast<size_t>(cursor_)];
    for (Operation& op : e.ops) op.redo(eng);
    ++cursor_;
    r.result = UndoReport::Result::Ok;
    r.description = e.description;
    r.entriesAffected = 1;
    return r;
}

int History::findScoped(EditScope scope) const {
    for (int i = cursor_ - 1; i >= 0; --i)
        if (entries_[static_cast<size_t>(i)].scope == scope) return i;
    return -1;
}

UndoReport History::undoScoped(Engine& eng, EditScope scope) {
    UndoReport r;
    const int at = findScoped(scope);
    if (at < 0) {
        r.description = std::string("nothing to undo in the ") +
                        editScopeName(scope) + " scope";
        return r;
    }

    // §8.6's flagged problem. Reverting an older entry out of order is only
    // safe when nothing applied since then touched the same objects.
    HistoryEntry& target = entries_[static_cast<size_t>(at)];
    for (int i = at + 1; i < cursor_; ++i) {
        const HistoryEntry& later = entries_[static_cast<size_t>(i)];
        if (!later.touchesAnyOf(target.touched)) continue;
        r.result = UndoReport::Result::Blocked;
        r.description = target.description;
        r.blockedBy = later.description;
        return r;
    }

    // Read what we need BEFORE touching the deque: erasing invalidates the
    // reference, and reporting through it afterwards named whichever entry
    // slid into the vacated slot.
    const std::string description = target.description;
    for (auto it = target.ops.rbegin(); it != target.ops.rend(); ++it) it->undo(eng);

    // Out-of-order undo: the entry is removed rather than left behind the
    // cursor, because the cursor cannot express "all applied except this one"
    // and a redo of it would land on different state anyway.
    if (at == cursor_ - 1) {
        --cursor_;
    } else {
        entries_.erase(entries_.begin() + at);
        --cursor_;
    }
    r.result = UndoReport::Result::Ok;
    r.description = description;
    r.entriesAffected = 1;
    return r;
}

UndoReport History::redoScoped(Engine& eng, EditScope scope) {
    UndoReport r;
    for (int i = cursor_; i < static_cast<int>(entries_.size()); ++i) {
        HistoryEntry& e = entries_[static_cast<size_t>(i)];
        if (e.scope != scope) continue;
        // Redoing out of order has the same hazard in the other direction.
        for (int j = cursor_; j < i; ++j) {
            if (!entries_[static_cast<size_t>(j)].touchesAnyOf(e.touched)) continue;
            r.result = UndoReport::Result::Blocked;
            r.description = e.description;
            r.blockedBy = entries_[static_cast<size_t>(j)].description;
            return r;
        }
        // Same rule as undoScoped: take what the report needs before the
        // deque moves underneath us.
        const std::string description = e.description;
        for (Operation& op : e.ops) op.redo(eng);
        if (i == cursor_) {
            ++cursor_;
        } else {
            // Applied out of order, so it moves to just before the cursor to
            // keep "everything before the cursor is applied" true.
            HistoryEntry moved = std::move(e);
            entries_.erase(entries_.begin() + i);
            entries_.insert(entries_.begin() + cursor_, std::move(moved));
            ++cursor_;
        }
        r.result = UndoReport::Result::Ok;
        r.description = description;
        r.entriesAffected = 1;
        return r;
    }
    r.description = std::string("nothing to redo in the ") + editScopeName(scope) +
                    " scope";
    return r;
}

UndoReport History::undoThrough(Engine& eng, Uid entry) {
    UndoReport r;
    int at = -1;
    for (int i = 0; i < cursor_; ++i)
        if (entries_[static_cast<size_t>(i)].uid == entry) at = i;
    if (at < 0) {
        r.result = UndoReport::Result::NoSuchEntry;
        r.description = "no such history entry, or it is already undone";
        return r;
    }

    // Everything from the cursor back THROUGH the chosen entry, newest first.
    // In order, so no collision check is needed: this is exactly what plain
    // undo would do, repeated.
    r.description = entries_[static_cast<size_t>(at)].description;
    while (cursor_ > at) {
        const UndoReport step = undo(eng);
        if (!step.ok()) break;
        ++r.entriesAffected;
    }
    r.result = UndoReport::Result::Ok;
    return r;
}

// ---------------------------------------------------------------------------
// ActionLog
// ---------------------------------------------------------------------------
Uid ActionLog::add(const std::string& description, const std::string& scope,
                   const std::string& source, const std::string& prompt) {
    LogEntry e;
    e.uid = uids_->next();
    e.time = clock_ ? clock_() : 0.0;
    e.description = description;
    e.scope = scope;
    e.source = source;
    e.prompt = prompt;
    entries_.push_back(std::move(e));
    return entries_.back().uid;
}

LogEntry& ActionLog::restore(LogEntry e) {
    if (!e.uid.valid()) e.uid = uids_->next();
    uids_->observe(e.uid);
    entries_.push_back(std::move(e));
    return entries_.back();
}

void ActionLog::markUndone(const std::string& description) {
    // Newest matching entry, and marked rather than deleted: the log is an
    // audit record, so an action that happened and was reverted is two facts,
    // not zero.
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        if (it->description != description || it->undone) continue;
        it->undone = true;
        return;
    }
}

// ---------------------------------------------------------------------------
// ProgrammerEdit
// ---------------------------------------------------------------------------
namespace {

size_t layerBytes(const ValueLayer& l) {
    return sizeof(ValueLayer) + l.size() * (sizeof(AttrKey) + sizeof(Value));
}

bool sameLayer(const ValueLayer& a, const ValueLayer& b) {
    if (a.size() != b.size()) return false;
    for (const auto& [k, v] : a.values()) {
        const Value* o = b.find(k);
        if (!o) return false;
        if (o->kind != v.kind || o->active != v.active || o->preset != v.preset ||
            o->presetAttr != v.presetAttr)
            return false;
        const float d = o->v - v.v;
        if (d > 1e-6f || d < -1e-6f) return false;
    }
    return true;
}

} // namespace

ProgrammerEdit::ProgrammerEdit(const Engine& eng, std::string description)
    : description_(std::move(description)), before_(eng.programmer.values()) {}

Uid ProgrammerEdit::commit(History& history, Engine& eng) {
    if (cancelled_) return Uid{};
    ValueLayer after = eng.programmer.values();
    // A command that changed nothing should leave nothing to undo.
    if (sameLayer(before_, after)) return Uid{};

    Operation op;
    op.description = description_;
    op.scope = EditScope::Programmer;
    op.bytes = layerBytes(before_) + layerBytes(after);
    // The programmer is one object, so the UID set is the fixtures it
    // touched: that is what a patch edit could collide with.
    std::vector<Uid> touched;
    auto noteFixtures = [&](const ValueLayer& l) {
        for (AttrKey k : l.keys()) {
            const Fixture* f = eng.patch.find(keyFixture(k));
            if (!f) continue;
            if (std::find(touched.begin(), touched.end(), f->uid) == touched.end())
                touched.push_back(f->uid);
        }
    };
    noteFixtures(before_);
    noteFixtures(after);
    op.touched = std::move(touched);

    ValueLayer b = before_;
    op.undo = [b](Engine& e) {
        e.programmer.values().clear();
        e.programmer.values().overwriteFrom(b);
    };
    op.redo = [after](Engine& e) {
        e.programmer.values().clear();
        e.programmer.values().overwriteFrom(after);
    };
    return history.record(std::move(op));
}

// ---------------------------------------------------------------------------
// PatchEdit
// ---------------------------------------------------------------------------
namespace {

PatchEdit::Snap snapFixture(const Engine& eng, FixtureId id) {
    PatchEdit::Snap s;
    s.id = id;
    const Fixture* f = eng.patch.find(id);
    if (!f) return s;
    s.present = true;
    s.fixture = *f;
    if (f->typeIndex >= 0 && f->typeIndex < eng.patch.typeCount())
        s.typeName = eng.patch.type(f->typeIndex).name;
    return s;
}

// Puts one fixture back the way the snapshot found it, including putting it
// back out of existence.
void restoreFixture(Engine& eng, const PatchEdit::Snap& s) {
    const Fixture* now = eng.patch.find(s.id);
    if (!s.present) {
        if (now) eng.patch.remove(s.id);
        return;
    }
    if (!now) {
        Patch::PatchRequest req;
        req.id = s.fixture.id;
        req.name = s.fixture.name;
        req.universe = s.fixture.universe;
        req.address = s.fixture.address;
        req.uid = s.fixture.uid;
        req.typeIndex = s.typeName.empty() ? s.fixture.typeIndex
                                           : eng.patch.findType(s.typeName);
        if (req.typeIndex < 0) req.typeIndex = s.fixture.typeIndex;
        if (!eng.patch.add(req)) return;
    } else {
        eng.patch.readdress(s.id, s.fixture.universe, s.fixture.address);
        eng.patch.rename(s.id, s.fixture.name);
    }
    eng.patch.setPosition(s.id, s.fixture.x, s.fixture.y, s.fixture.z, s.fixture.rotX,
                          s.fixture.rotY, s.fixture.rotZ);
    if (!s.fixture.positioned) {
        // setPosition marks it placed, so an unplaced fixture has to be put
        // back unplaced or the visualiser gains a fixture at the origin.
        if (Fixture* live = const_cast<Fixture*>(eng.patch.find(s.id)))
            live->positioned = false;
    }
    for (const std::string& tag : s.fixture.tags) eng.patch.addTag(s.id, tag);
}

bool sameSnap(const PatchEdit::Snap& a, const PatchEdit::Snap& b) {
    if (a.present != b.present) return false;
    if (!a.present) return true;
    return a.fixture.id == b.fixture.id && a.fixture.name == b.fixture.name &&
           a.fixture.universe == b.fixture.universe &&
           a.fixture.address == b.fixture.address &&
           a.fixture.typeIndex == b.fixture.typeIndex &&
           a.fixture.positioned == b.fixture.positioned &&
           a.fixture.tags == b.fixture.tags && a.typeName == b.typeName;
}

} // namespace

PatchEdit::PatchEdit(const Engine& eng, std::string description,
                     std::vector<FixtureId> ids)
    : description_(std::move(description)), ids_(std::move(ids)) {
    before_.reserve(ids_.size());
    for (FixtureId id : ids_) before_.push_back(snapFixture(eng, id));
}

PatchEdit PatchEdit::all(const Engine& eng, std::string description) {
    std::vector<FixtureId> ids;
    ids.reserve(eng.patch.size());
    for (const Fixture& f : eng.patch.fixtures()) ids.push_back(f.id);
    return PatchEdit(eng, std::move(description), std::move(ids));
}

Uid PatchEdit::commit(History& history, Engine& eng) {
    if (cancelled_ || ids_.empty()) return Uid{};

    std::vector<Snap> after;
    after.reserve(ids_.size());
    for (FixtureId id : ids_) after.push_back(snapFixture(eng, id));

    bool changed = false;
    for (size_t i = 0; i < after.size(); ++i)
        if (!sameSnap(before_[i], after[i])) changed = true;
    if (!changed) return Uid{};

    Operation op;
    op.description = description_;
    op.scope = EditScope::Patch;
    // Per-fixture records, not a copy of the whole patch: that is what keeps
    // a 2000-fixture operation affordable.
    op.bytes = (before_.size() + after.size()) * sizeof(Snap);
    for (const Snap& s : before_)
        if (s.present) op.touched.push_back(s.fixture.uid);
    for (const Snap& s : after)
        if (s.present &&
            std::find(op.touched.begin(), op.touched.end(), s.fixture.uid) ==
                op.touched.end())
            op.touched.push_back(s.fixture.uid);

    std::vector<Snap> b = before_;
    op.undo = [b](Engine& e) {
        for (const Snap& s : b) restoreFixture(e, s);
        e.syncUniverses();
    };
    op.redo = [after](Engine& e) {
        for (const Snap& s : after) restoreFixture(e, s);
        e.syncUniverses();
    };
    return history.record(std::move(op));
}

// ---------------------------------------------------------------------------
// SurfaceEdit (§9.8)
// ---------------------------------------------------------------------------
namespace {

bool sameTiles(const std::vector<Tile>& a, const std::vector<Tile>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (!(a[i].uid == b[i].uid) || a[i].kind != b[i].kind ||
            !(a[i].target == b[i].target) || a[i].slot != b[i].slot ||
            !(a[i].rect == b[i].rect) || a[i].legendOverride != b[i].legendOverride ||
            a[i].colourOverride != b[i].colourOverride ||
            a[i].symbolOverride != b[i].symbolOverride)
            return false;
    }
    return true;
}

} // namespace

SurfaceEdit::SurfaceEdit(const Engine& eng, std::string description, int surfaceId)
    : description_(std::move(description)), surfaceId_(surfaceId) {
    if (const Surface* s = eng.surfaces.byId(surfaceId)) {
        existed_ = true;
        before_ = s->tiles;
        beforeStyle_ = s->style;
    }
}

Uid SurfaceEdit::commit(History& history, Engine& eng) {
    if (cancelled_ || !existed_) return Uid{};
    const Surface* s = eng.surfaces.byId(surfaceId_);
    if (!s) return Uid{};
    if (s->style == beforeStyle_ && sameTiles(before_, s->tiles)) return Uid{};

    Operation op;
    op.description = description_;
    op.scope = EditScope::Layout;
    op.bytes = (before_.size() + s->tiles.size()) * sizeof(Tile);

    // §8.6's scoped undo needs to know what this touched. A surface edit
    // touches the page and every tile on either side of it — a tile that was
    // dropped by a conversion is as much "touched" as one that moved.
    std::vector<Uid> touched{s->uid};
    auto note = [&](const std::vector<Tile>& tiles) {
        for (const Tile& t : tiles)
            if (std::find(touched.begin(), touched.end(), t.uid) == touched.end())
                touched.push_back(t.uid);
    };
    note(before_);
    note(s->tiles);
    op.touched = std::move(touched);

    const int id = surfaceId_;
    const std::vector<Tile> before = before_;
    const SurfaceStyle beforeStyle = beforeStyle_;
    const std::vector<Tile> after = s->tiles;
    const SurfaceStyle afterStyle = s->style;

    op.undo = [id, before, beforeStyle](Engine& e) {
        if (Surface* sf = e.surfaces.byId(id)) {
            sf->tiles = before;
            sf->style = beforeStyle;
        }
    };
    op.redo = [id, after, afterStyle](Engine& e) {
        if (Surface* sf = e.surfaces.byId(id)) {
            sf->tiles = after;
            sf->style = afterStyle;
        }
    };
    return history.record(std::move(op));
}

} // namespace lumen