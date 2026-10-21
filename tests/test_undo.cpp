// test_undo.cpp — undo, redo, scoped undo and the action log (SPEC §8.6).
#include "Check.hpp"

#include "lumen/Command.hpp"
#include "lumen/Show.hpp"
#include "lumen/Undo.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>

using namespace lumen;
namespace fs = std::filesystem;

namespace {

std::string tmpDir() {
    const auto d = fs::temp_directory_path() / "lumen_undo_tests";
    fs::create_directories(d);
    return d.string();
}

// A trivial undoable operation over a counter the engine does not own, for
// testing the history mechanism itself without dragging the model in.
Operation counterOp(int& counter, int before, int after, std::string desc,
                    EditScope scope, std::vector<Uid> touched) {
    Operation op;
    op.description = std::move(desc);
    op.scope = scope;
    op.touched = std::move(touched);
    op.bytes = 64;
    int* c = &counter;
    op.undo = [c, before](Engine&) { *c = before; };
    op.redo = [c, after](Engine&) { *c = after; };
    return op;
}

std::optional<float> dimmerOf(const Engine& e, FixtureId id) {
    return e.programmer.values().value(id, 0, Attr::Dimmer);
}

} // namespace

// ---------------------------------------------------------------------------
static void testUndoRedoBasics() {
    UidGen uids;
    History h(uids);
    Engine eng;   // only needed to satisfy the restore signature
    int counter = 0;

    CHECK(!h.canUndo());
    CHECK(!h.canRedo());
    CHECK(h.undo(eng).result == UndoReport::Result::Nothing);
    CHECK(h.redo(eng).result == UndoReport::Result::Nothing);
    CHECK(h.nextUndo() == nullptr);
    CHECK(h.nextRedo() == nullptr);

    counter = 1;
    CHECK(h.record(counterOp(counter, 0, 1, "set 1", EditScope::Programmer, {})).valid());
    counter = 2;
    CHECK(h.record(counterOp(counter, 1, 2, "set 2", EditScope::Programmer, {})).valid());
    CHECK_EQ(h.entries().size(), size_t(2));
    CHECK_EQ(h.cursor(), 2);
    CHECK(h.canUndo());
    CHECK(!h.canRedo());
    CHECK(h.nextUndo()->description == "set 2");

    UndoReport r = h.undo(eng);
    CHECK(r.ok());
    CHECK(r.description == "set 2");
    CHECK_EQ(r.entriesAffected, 1);
    CHECK_EQ(counter, 1);
    CHECK_EQ(h.cursor(), 1);
    CHECK(h.canRedo());
    CHECK(h.nextRedo()->description == "set 2");

    CHECK(h.undo(eng).ok());
    CHECK_EQ(counter, 0);
    CHECK(!h.canUndo());

    CHECK(h.redo(eng).ok());
    CHECK_EQ(counter, 1);
    CHECK(h.redo(eng).ok());
    CHECK_EQ(counter, 2);
    CHECK(!h.canRedo());

    // A new edit after an undo invalidates the redo tail: the states those
    // entries would restore no longer follow from here.
    CHECK(h.undo(eng).ok());
    CHECK_EQ(counter, 1);
    CHECK(h.canRedo());
    counter = 9;
    h.record(counterOp(counter, 1, 9, "set 9", EditScope::Programmer, {}));
    CHECK(!h.canRedo());
    CHECK_EQ(h.entries().size(), size_t(2));
    CHECK(h.nextUndo()->description == "set 9");

    // An operation with no restore functions is not recorded, rather than
    // sitting in the history doing nothing when undone.
    Operation broken;
    broken.description = "broken";
    CHECK(!h.record(std::move(broken)).valid());
    CHECK_EQ(h.entries().size(), size_t(2));

    h.clear();
    CHECK_EQ(h.entries().size(), size_t(0));
    CHECK_EQ(h.cursor(), 0);
}

static void testTransactionsRevertAtomically() {
    // §8.6: one entry may hold many operations, applied and reverted
    // atomically. §7.4's renumber and §16.5's agent batches both need it.
    UidGen uids;
    History h(uids);
    Engine eng;
    int a = 0, b = 0, c = 0;

    h.begin("Renumber", EditScope::Preset, "operator");
    CHECK(h.inTransaction());
    a = 1;
    h.record(counterOp(a, 0, 1, "a", EditScope::Preset, {Uid{1}}));
    b = 1;
    h.record(counterOp(b, 0, 1, "b", EditScope::Preset, {Uid{2}}));
    c = 1;
    h.record(counterOp(c, 0, 1, "c", EditScope::Preset, {Uid{3}}));
    CHECK_EQ(h.entries().size(), size_t(0));   // nothing visible until commit
    const Uid entry = h.commit();
    CHECK(entry.valid());
    CHECK(!h.inTransaction());

    CHECK_EQ(h.entries().size(), size_t(1));
    const HistoryEntry& e = h.entries().front();
    CHECK_EQ(e.ops.size(), size_t(3));
    CHECK(e.description == "Renumber");
    CHECK(e.scope == EditScope::Preset);
    CHECK_EQ(e.touched.size(), size_t(3));

    // All three revert together.
    CHECK(h.undo(eng).ok());
    CHECK_EQ(a, 0);
    CHECK_EQ(b, 0);
    CHECK_EQ(c, 0);
    CHECK(h.redo(eng).ok());
    CHECK_EQ(a, 1);
    CHECK_EQ(b, 1);
    CHECK_EQ(c, 1);

    // A transaction spanning scopes is Mixed, so a scoped undo cannot pick it
    // up as if it were purely one thing.
    h.begin("Mixed batch");
    h.record(counterOp(a, 1, 2, "a", EditScope::Programmer, {}));
    h.record(counterOp(b, 1, 2, "b", EditScope::Patch, {}));
    h.commit();
    CHECK(h.entries().back().scope == EditScope::Mixed);

    // An empty transaction is discarded: an entry that undoes nothing makes
    // Undo look broken.
    const size_t before = h.entries().size();
    h.begin("Did nothing");
    CHECK(!h.commit().valid());
    CHECK_EQ(h.entries().size(), before);

    // Nesting is counted, so an inner command cannot close the outer one's
    // transaction out from under it.
    h.begin("Outer");
    h.begin("Inner");
    CHECK_EQ(h.transactionDepth(), 2);
    c = 5;
    h.record(counterOp(c, 1, 5, "c", EditScope::Cue, {}));
    CHECK(!h.commit().valid());         // the inner commit does not close it
    CHECK(h.inTransaction());
    CHECK(h.commit().valid());
    CHECK(h.entries().back().description == "Outer");

    // Aborting reverts what the failed command already did, leaving no trace.
    const size_t count = h.entries().size();
    h.begin("Will fail", EditScope::Cue);
    a = 100;
    h.record(counterOp(a, 2, 100, "a", EditScope::Cue, {}));
    h.abort(eng);
    CHECK_EQ(a, 2);
    CHECK_EQ(h.entries().size(), count);
    CHECK(!h.inTransaction());
}

static void testScopedUndoGuardsAgainstCollisions() {
    // §8.6's flagged problem, with the spec's own example:
    // [programmer A, patch B, programmer C].
    UidGen uids;
    History h(uids);
    Engine eng;
    int a = 0, b = 0, c = 0;

    // B is INDEPENDENT of A, so scoped undo should reach past it.
    a = 1;
    h.record(counterOp(a, 0, 1, "programmer A", EditScope::Programmer, {Uid{1}}));
    b = 1;
    h.record(counterOp(b, 0, 1, "patch B", EditScope::Patch, {Uid{99}}));
    c = 1;
    h.record(counterOp(c, 0, 1, "programmer C", EditScope::Programmer, {Uid{2}}));

    UndoReport r1 = h.undoScoped(eng, EditScope::Programmer);
    CHECK(r1.ok());
    CHECK(r1.description == "programmer C");
    CHECK_EQ(c, 0);

    UndoReport r2 = h.undoScoped(eng, EditScope::Programmer);
    CHECK(r2.ok());
    CHECK(r2.description == "programmer A");
    CHECK_EQ(a, 0);
    CHECK_EQ(b, 1);   // the patch edit is untouched, which is the point

    CHECK(h.undoScoped(eng, EditScope::Programmer).result ==
          UndoReport::Result::Nothing);

    // Now B DEPENDS on A: it touched the same object.
    History h2(uids);
    a = 1;
    h2.record(counterOp(a, 0, 1, "programmer A", EditScope::Programmer, {Uid{1}}));
    b = 1;
    h2.record(counterOp(b, 0, 1, "patch B", EditScope::Patch, {Uid{1}}));
    c = 1;
    h2.record(counterOp(c, 0, 1, "programmer C", EditScope::Programmer, {Uid{2}}));

    CHECK(h2.undoScoped(eng, EditScope::Programmer).ok());   // C is fine
    const UndoReport blocked = h2.undoScoped(eng, EditScope::Programmer);
    CHECK(blocked.result == UndoReport::Result::Blocked);
    CHECK(blocked.description == "programmer A");
    // It names what blocks it, so the refusal is actionable rather than just
    // a refusal.
    CHECK(blocked.blockedBy == "patch B");
    CHECK_EQ(a, 1);   // and nothing was changed
    CHECK_EQ(b, 1);

    // Undoing back through the blocker clears the way.
    CHECK(h2.undo(eng).ok());
    CHECK_EQ(b, 0);
    CHECK(h2.undoScoped(eng, EditScope::Programmer).ok());
    CHECK_EQ(a, 0);

    // A scope with nothing in it says so rather than undoing something else.
    History h3(uids);
    a = 1;
    h3.record(counterOp(a, 0, 1, "patch only", EditScope::Patch, {}));
    const UndoReport none = h3.undoScoped(eng, EditScope::Programmer);
    CHECK(none.result == UndoReport::Result::Nothing);
    CHECK(none.description.find("programmer") != std::string::npos);
    CHECK_EQ(a, 1);
}

static void testScopedRedo() {
    UidGen uids;
    History h(uids);
    Engine eng;
    int a = 0, b = 0;

    a = 1;
    h.record(counterOp(a, 0, 1, "programmer A", EditScope::Programmer, {Uid{1}}));
    b = 1;
    h.record(counterOp(b, 0, 1, "patch B", EditScope::Patch, {Uid{99}}));
    CHECK(h.undo(eng).ok());   // B undone
    CHECK(h.undo(eng).ok());   // A undone
    CHECK_EQ(a, 0);
    CHECK_EQ(b, 0);

    // Redo only the programmer one, reaching past the undone patch edit.
    const UndoReport r = h.redoScoped(eng, EditScope::Programmer);
    CHECK(r.ok());
    CHECK(r.description == "programmer A");
    CHECK_EQ(a, 1);
    CHECK_EQ(b, 0);
    CHECK(h.canRedo());   // B is still there to redo
    CHECK(h.redo(eng).ok());
    CHECK_EQ(b, 1);

    // Out-of-order redo is guarded in the same way.
    History h2(uids);
    a = 1;
    h2.record(counterOp(a, 0, 1, "patch first", EditScope::Patch, {Uid{1}}));
    b = 1;
    h2.record(counterOp(b, 0, 1, "programmer second", EditScope::Programmer, {Uid{1}}));
    CHECK(h2.undo(eng).ok());
    CHECK(h2.undo(eng).ok());
    const UndoReport blocked = h2.redoScoped(eng, EditScope::Programmer);
    CHECK(blocked.result == UndoReport::Result::Blocked);
    CHECK(blocked.blockedBy == "patch first");

    CHECK(h.redoScoped(eng, EditScope::Preset).result == UndoReport::Result::Nothing);
}

static void testJumpToPoint() {
    UidGen uids;
    History h(uids);
    Engine eng;
    int v = 0;
    std::vector<Uid> ids;
    for (int i = 1; i <= 5; ++i) {
        v = i;
        ids.push_back(h.record(counterOp(v, i - 1, i, "step " + std::to_string(i),
                                         EditScope::Cue, {})));
    }
    CHECK_EQ(v, 5);
    CHECK_EQ(h.cursor(), 5);

    // Back through step 2: undoes 5, 4, 3 and 2, as one operation.
    const UndoReport r = h.undoThrough(eng, ids[1]);
    CHECK(r.ok());
    CHECK(r.description == "step 2");
    CHECK_EQ(r.entriesAffected, 4);
    CHECK_EQ(v, 1);
    CHECK_EQ(h.cursor(), 1);
    // Everything undone is still available to redo.
    CHECK(h.canRedo());
    CHECK(h.redo(eng).ok());
    CHECK_EQ(v, 2);

    // An entry that is already undone, or never existed.
    CHECK(h.undoThrough(eng, Uid{987654}).result == UndoReport::Result::NoSuchEntry);
    CHECK(h.undoThrough(eng, ids[4]).result == UndoReport::Result::NoSuchEntry);
}

static void testDepthAndByteCaps() {
    // §8.6: a bulk patch of 2000 fixtures must not blow memory.
    UidGen uids;
    History h(uids);
    Engine eng;
    int v = 0;

    h.setDepth(5);
    for (int i = 1; i <= 20; ++i) {
        v = i;
        h.record(counterOp(v, i - 1, i, "step " + std::to_string(i),
                           EditScope::Cue, {}));
    }
    CHECK_EQ(h.entries().size(), size_t(5));
    CHECK_EQ(h.cursor(), 5);
    CHECK_EQ(h.evicted(), 15);
    // The oldest went, so the newest is still undoable.
    CHECK(h.nextUndo()->description == "step 20");
    CHECK(h.entries().front().description == "step 16");

    // Undo still works across the remaining window, and then stops rather
    // than reaching for entries that were evicted.
    for (int i = 0; i < 5; ++i) CHECK(h.undo(eng).ok());
    CHECK(!h.canUndo());
    CHECK_EQ(v, 15);

    // A byte cap evicts too.
    History hb(uids);
    hb.setByteCap(200);          // each op above costs 64
    for (int i = 1; i <= 10; ++i) {
        v = i;
        hb.record(counterOp(v, i - 1, i, "s", EditScope::Cue, {}));
    }
    CHECK(hb.bytes() <= size_t(200));
    CHECK(hb.entries().size() < size_t(10));
    CHECK(hb.evicted() > 0);

    // A single operation larger than the cap is kept rather than evicted into
    // nothing: a history that cannot hold one entry is useless.
    History hs(uids);
    hs.setByteCap(10);
    Operation big = counterOp(v, 1, 2, "huge", EditScope::Patch, {});
    big.bytes = 1000;
    CHECK(hs.record(std::move(big)).valid());
    CHECK_EQ(hs.entries().size(), size_t(1));
    CHECK(hs.canUndo());
}

// ---------------------------------------------------------------------------
// Against the real model
// ---------------------------------------------------------------------------
static void testProgrammerEditsUndo() {
    Engine eng;
    Console con(eng);
    installStarterTypes(eng.patch);
    CHECK(con.execute("Fixture 1 Thru 4 Patch \"Dimmer\" 4 1 1").ok);

    const size_t afterPatch = eng.history.entries().size();
    CHECK(con.execute("Fixture 1 Thru 4 At 50").ok);
    CHECK_EQ(eng.history.entries().size(), afterPatch + 1);
    CHECK_NEAR(dimmerOf(eng, 1).value_or(-1.f), 0.5f);

    CHECK(con.execute("Fixture 1 At 100").ok);
    CHECK_NEAR(dimmerOf(eng, 1).value_or(-1.f), 1.f);

    Result u = con.execute("Undo");
    CHECK(u.ok);
    CHECK(u.message.find("Dimmer at 100%") != std::string::npos);
    CHECK_NEAR(dimmerOf(eng, 1).value_or(-1.f), 0.5f);

    CHECK(con.execute("Redo").ok);
    CHECK_NEAR(dimmerOf(eng, 1).value_or(-1.f), 1.f);

    // Clear is an edit, so it undoes — which matters, because clearing the
    // programmer by accident is exactly when you reach for Undo.
    CHECK(con.execute("ClearAll").ok);
    CHECK(eng.programmer.values().empty());
    CHECK(con.execute("Undo").ok);
    CHECK_EQ(eng.programmer.values().size(), size_t(4));
    CHECK_NEAR(dimmerOf(eng, 1).value_or(-1.f), 1.f);

    // A command that changes nothing leaves nothing to undo.
    const size_t before = eng.history.entries().size();
    CHECK(con.execute("Fixture 1 At 100").ok);   // already there
    CHECK_EQ(eng.history.entries().size(), before);

    // Nothing left: Undo says so rather than failing silently.
    while (eng.history.canUndo()) CHECK(con.execute("Undo").ok);
    CHECK(!con.execute("Undo").ok);
}

static void testPatchEditsUndoAsAUnit() {
    Engine eng;
    Console con(eng);
    installStarterTypes(eng.patch);

    CHECK(con.execute("Fixture 1 Thru 8 Patch \"LED Par RGBW\" 8 1 1").ok);
    CHECK_EQ(eng.patch.size(), size_t(8));
    // A bulk patch is ONE entry, so it undoes as a unit rather than eight
    // presses (§8.6).
    CHECK_EQ(eng.history.entries().size(), size_t(1));
    CHECK_EQ(eng.history.entries().front().ops.size(), size_t(1));

    const Uid uidOfThree = eng.patch.find(3)->uid;

    CHECK(con.execute("Undo").ok);
    CHECK_EQ(eng.patch.size(), size_t(0));
    CHECK(con.execute("Redo").ok);
    CHECK_EQ(eng.patch.size(), size_t(8));
    // The UID comes back, which is what every reference in the show stores.
    CHECK(eng.patch.find(3) != nullptr);
    CHECK(eng.patch.find(3)->uid == uidOfThree);
    const int footprint = eng.patch.typeOf(3)->footprint();
    const int thirdAddress = 1 + 2 * footprint;
    CHECK_EQ(eng.patch.find(3)->address, thirdAddress);
    CHECK(eng.patch.conflicts().empty());

    // Unpatch undoes too, restoring addresses and positions.
    CHECK(eng.patch.setPosition(3, 1.5f, 2.f, 4.f, 10.f, 20.f, 30.f));
    CHECK(eng.patch.addTag(3, "truss"));
    con.setPrivilege(Privilege::Protected);
    CHECK(con.execute("Fixture 3 Unpatch").ok);
    CHECK(eng.patch.find(3) == nullptr);
    CHECK_EQ(eng.patch.size(), size_t(7));
    CHECK(con.execute("Undo").ok);
    con.setPrivilege(Privilege::Operator);

    const Fixture* back = eng.patch.find(3);
    CHECK(back != nullptr);
    if (back) {
        CHECK(back->uid == uidOfThree);
        CHECK_EQ(back->address, thirdAddress);
        CHECK(back->positioned);
        CHECK_NEAR(back->x, 1.5f);
        CHECK_NEAR(back->rotZ, 30.f);
        CHECK_EQ(back->tags.size(), size_t(1));
        CHECK(back->tags[0] == "truss");
    }

    // An unplaced fixture comes back unplaced, rather than appearing at the
    // origin in the visualiser.
    CHECK(!eng.patch.find(5)->positioned);
    con.setPrivilege(Privilege::Protected);
    CHECK(con.execute("Fixture 5 Unpatch").ok);
    CHECK(con.execute("Undo").ok);
    con.setPrivilege(Privilege::Operator);
    CHECK(eng.patch.find(5) != nullptr);
    CHECK(!eng.patch.find(5)->positioned);
}

static void testPlaybackIsNotUndoable() {
    // §8.6: Go, Off, Release and fader moves are PERFORMANCE, not edits. If
    // Undo could rewind a playback it would change live output, so it must
    // leave no trace — which is what makes Undo safe to press during a show.
    Engine eng;
    Console con(eng);
    installStarterTypes(eng.patch);
    CHECK(con.execute("Fixture 1 Thru 4 Patch \"Dimmer\" 4 1 1").ok);
    CHECK(con.execute("Fixture 1 Thru 4 At 100").ok);
    CHECK(con.execute("Store Cue 1 1").ok);
    CHECK(con.execute("Assign 1 1").ok);
    CHECK(con.execute("ClearAll").ok);

    eng.setClock(0.0);
    const size_t entries = eng.history.entries().size();
    const int cursor = eng.history.cursor();

    for (const char* cmd : {"Go 1", "Go 1", "Back 1", "Goto 1 1", "Off 1",
                            "Release 1", "GrandMaster 50", "Blackout on",
                            "Blackout off", "Highlight on", "Highlight off"}) {
        con.execute(cmd);
        // Not one of them may add to the history.
        CHECK_EQ(eng.history.entries().size(), entries);
        CHECK_EQ(eng.history.cursor(), cursor);
    }

    // And Undo still reaches the last EDIT, past all that performance.
    Result u = con.execute("Undo");
    CHECK(u.ok);
    CHECK(u.message.find("Clear all") != std::string::npos);
}

static void testHistoryAndLogAreSeparateRecords() {
    Engine eng;
    Console con(eng);
    installStarterTypes(eng.patch);

    CHECK(con.execute("Fixture 1 Thru 2 Patch \"Dimmer\" 2 1 1").ok);
    CHECK(con.execute("Fixture 1 At 60").ok);
    CHECK(con.execute("ClearAll").ok);

    // The log holds a description per action, with attribution.
    CHECK_EQ(eng.actionLog.size(), size_t(3));
    const auto& log = eng.actionLog.all();
    CHECK(log[0].scope == "patch");
    CHECK(log[1].scope == "programmer");
    CHECK(log[1].description.find("60%") != std::string::npos);
    for (const LogEntry& l : log) {
        CHECK(l.source == "operator");
        CHECK(l.uid.valid());
        CHECK(!l.undone);
    }

    // Undoing MARKS the log entry rather than deleting it: an action that
    // happened and was reverted is two facts, not zero.
    CHECK(con.execute("Undo").ok);
    CHECK_EQ(eng.actionLog.size(), size_t(3));
    bool anyUndone = false;
    for (const LogEntry& l : eng.actionLog.all())
        if (l.undone) anyUndone = true;
    CHECK(anyUndone);

    // The history view marks undone entries, so they are distinguishable.
    // Checked HERE, before the next edit: a new edit drops the redo tail, so
    // by then there is nothing undone left to show.
    Result histNow = con.execute("List history");
    CHECK(histNow.ok);
    bool markedUndone = false;
    for (const auto& row : histNow.rows)
        if (row.rfind("~", 0) == 0) markedUndone = true;
    CHECK(markedUndone);

    // The agent's actions are attributed to the agent, with the prompt that
    // caused them (§16.5).
    Console agent(eng);
    agent.setPrivilege(Privilege::Any);
    agent.setSource("agent");
    agent.setPrompt("make the front wash warmer");
    // An agent cannot reach an Operator command at all, which is the point of
    // the privilege tier — so it is the attribution that is under test here.
    CHECK(!agent.execute("Fixture 1 At 80").ok);
    agent.setPrivilege(Privilege::Operator);
    CHECK(agent.execute("Fixture 1 At 80").ok);
    CHECK(eng.actionLog.all().back().source == "agent");

    Result hist = con.execute("List history");
    CHECK(hist.ok);
    CHECK(!hist.rows.empty());

    Result logRows = con.execute("List log");
    CHECK(logRows.ok);
    CHECK_EQ(logRows.rows.size(), eng.actionLog.size());
    bool mentionsPrompt = false;
    for (const auto& row : logRows.rows)
        if (row.find("make the front wash warmer") != std::string::npos)
            mentionsPrompt = true;
    CHECK(mentionsPrompt);
}

static void testScopedUndoThroughTheConsole() {
    // The spec's scenario, end to end: [programmer A, patch B, programmer C]
    // with B independent of A.
    Engine eng;
    Console con(eng);
    installStarterTypes(eng.patch);
    CHECK(con.execute("Fixture 1 Thru 2 Patch \"Dimmer\" 2 1 1").ok);

    CHECK(con.execute("Fixture 1 At 50").ok);                       // A
    CHECK(con.execute("Fixture 9 Patch \"Dimmer\" 1 1 100").ok);    // B
    CHECK(con.execute("Fixture 2 At 80").ok);                       // C

    Result r1 = con.execute("Undo programmer");
    CHECK(r1.ok);
    CHECK(r1.message.find("80%") != std::string::npos);
    Result r2 = con.execute("Undo programmer");
    CHECK(r2.ok);
    CHECK(r2.message.find("50%") != std::string::npos);
    // B survived both, which is the whole point of scoping.
    CHECK(eng.patch.find(9) != nullptr);
    CHECK(eng.programmer.values().empty());

    // Now the colliding case: a patch edit on the SAME fixture.
    Engine e2;
    Console c2(e2);
    installStarterTypes(e2.patch);
    CHECK(c2.execute("Fixture 1 Thru 2 Patch \"Dimmer\" 2 1 1").ok);
    CHECK(c2.execute("Fixture 1 At 50").ok);
    c2.setPrivilege(Privilege::Protected);
    CHECK(c2.execute("Fixture 1 Unpatch").ok);
    c2.setPrivilege(Privilege::Operator);
    CHECK(c2.execute("Fixture 2 At 80").ok);

    CHECK(c2.execute("Undo programmer").ok);
    Result blocked = c2.execute("Undo programmer");
    CHECK(blocked.parsed);
    CHECK(!blocked.ok);
    // The refusal names the blocker and offers the way through.
    CHECK(blocked.message.find("Unpatch") != std::string::npos);
    CHECK(blocked.message.find("same objects") != std::string::npos);

    CHECK(c2.execute("Undo").ok);                  // through the blocker
    CHECK(c2.execute("Undo programmer").ok);       // now it works

    CHECK(!con.execute("Undo nonsense").parsed || !con.execute("Undo nonsense").ok);
}

static void testUndoThroughCommand() {
    Engine eng;
    Console con(eng);
    installStarterTypes(eng.patch);
    CHECK(con.execute("Fixture 1 Thru 4 Patch \"Dimmer\" 4 1 1").ok);
    for (int v : {10, 20, 30, 40, 50})
        CHECK(con.execute("Fixture 1 At " + std::to_string(v)).ok);
    CHECK_NEAR(dimmerOf(eng, 1).value_or(-1.f), 0.5f);

    // Find the entry for "at 20" and jump back through it.
    int id = 0;
    for (const HistoryEntry& e : eng.history.entries())
        if (e.description.find("at 20%") != std::string::npos) id = e.id;
    CHECK(id > 0);

    // Protected: it can revert a lot at once.
    CHECK(!con.execute("UndoThrough " + std::to_string(id)).ok);
    con.setPrivilege(Privilege::Protected);
    Result r = con.execute("UndoThrough " + std::to_string(id));
    CHECK(r.ok);
    CHECK(r.message.find("at 20%") != std::string::npos);
    CHECK_NEAR(dimmerOf(eng, 1).value_or(-1.f), 0.1f);

    CHECK(!con.execute("UndoThrough 9999").ok);
    con.setPrivilege(Privilege::Operator);
}

static void testTheActionLogTravelsWithTheShow() {
    // §8.6: the undo history is volatile and session-only; the action log is
    // persisted. A reopened show knows what was done to it.
    const std::string path = tmpDir() + "/undo.lumen";
    fs::remove(path);

    Engine a;
    Console con(a);
    installStarterTypes(a.patch);
    CHECK(con.execute("Fixture 1 Thru 2 Patch \"Dimmer\" 2 1 1").ok);
    CHECK(con.execute("Fixture 1 At 70").ok);
    a.actionLog.add("agent raised the wash", "programmer", "agent",
                    "make the front wash warmer");
    CHECK(con.execute("Undo").ok);   // marks one log entry undone

    const size_t logged = a.actionLog.size();
    CHECK(logged >= size_t(3));
    CHECK(a.history.entries().size() > size_t(0));

    SaveReport sr = ShowFile::save(a, path, "Undo Show");
    CHECK(sr.ok);
    CHECK_EQ(sr.actionLog, static_cast<int>(logged));

    Engine b;
    LoadReport lr = ShowFile::load(b, path);
    CHECK(lr.ok);
    CHECK(lr.warnings.empty());
    CHECK_EQ(lr.actionLog, static_cast<int>(logged));
    CHECK_EQ(b.actionLog.size(), logged);

    // The log came back whole, attribution and prompt included.
    bool foundAgent = false;
    bool foundUndone = false;
    for (const LogEntry& l : b.actionLog.all()) {
        if (l.source == "agent" && l.prompt == "make the front wash warmer")
            foundAgent = true;
        if (l.undone) foundUndone = true;
    }
    CHECK(foundAgent);
    CHECK(foundUndone);

    // The undo HISTORY did not come back, by design: a reopened show must not
    // be rewindable past its saved state.
    CHECK_EQ(b.history.entries().size(), size_t(0));
    CHECK(!b.history.canUndo());

    fs::remove(path);
}

int main() {
    std::printf("lumen undo\n");
    testUndoRedoBasics();
    testTransactionsRevertAtomically();
    testScopedUndoGuardsAgainstCollisions();
    testScopedRedo();
    testJumpToPoint();
    testDepthAndByteCaps();
    testProgrammerEditsUndo();
    testPatchEditsUndoAsAUnit();
    testPlaybackIsNotUndoable();
    testHistoryAndLogAreSeparateRecords();
    testScopedUndoThroughTheConsole();
    testUndoThroughCommand();
    testTheActionLogTravelsWithTheShow();
    return check::report("undo");
}
