// Tests for the command layer: lexing, ranges, dispatch, privilege, metadata.
#include "Check.hpp"

#include "lumen/Command.hpp"

#include <cmath>

using namespace lumen;

namespace {

struct Desk {
    Engine eng;
    Console con{eng};

    Desk() {
        installStarterTypes(eng.patch);
        con.execute("Fixture 1 Thru 8 Patch \"LED Par RGBW\" 8 1 1");
        eng.setClock(0.0);
    }

    Result run(const std::string& line) { return con.execute(line); }
    float out(FixtureId id, Attr a) {
        eng.tick();
        const auto v = eng.merged().get(makeKey(id, 0, a));
        return v ? *v : -1.f;
    }
    float prog(FixtureId id, Attr a, CellId cell = 0) const {
        const auto v = eng.programmer.values().value(id, cell, a);
        return v ? *v : -1.f;
    }
};

} // namespace

static void testLexer() {
    auto t = tokenise("Fixture 1 Thru 10 At 50");
    CHECK_EQ(t.size(), size_t(7));  // includes End
    CHECK(t[0].kind == Token::Kind::Word);
    CHECK(t[1].kind == Token::Kind::Number);
    CHECK(t[2].kind == Token::Kind::Thru);
    CHECK_EQ(t[3].whole, uint32_t(10));
    CHECK(t[5].kind == Token::Kind::Number);
    CHECK_NEAR(t[5].number, 50.0);

    // A dotted number is fixture.cell, not a decimal — `12.3` is cell 3.
    auto c = tokenise("Fixture 12.3");
    CHECK(c[1].hasCell);
    CHECK_EQ(c[1].whole, uint32_t(12));
    CHECK_EQ(c[1].cell, uint32_t(3));

    // Quoted strings survive spaces.
    auto q = tokenise("Patch \"LED Par RGBW\"");
    CHECK(q[1].kind == Token::Kind::String);
    CHECK(q[1].text == "LED Par RGBW");

    CHECK(tokenise("")[0].kind == Token::Kind::End);
}

static void testRangeParsing() {
    auto parse = [](const std::string& s) {
        const auto toks = tokenise(s);
        size_t i = 0;
        std::vector<Target> out;
        std::string err;
        parseTargets(toks, i, out, err);
        return out;
    };

    CHECK_EQ(parse("5").size(), size_t(1));
    CHECK_EQ(parse("5")[0].fixture, FixtureId(5));

    const auto thru = parse("1 Thru 10");
    CHECK_EQ(thru.size(), size_t(10));
    CHECK_EQ(thru.front().fixture, FixtureId(1));
    CHECK_EQ(thru.back().fixture, FixtureId(10));

    // Reversed ranges still enumerate.
    CHECK_EQ(parse("10 Thru 1").size(), size_t(10));

    // `+` accumulates, and order is capture order (SPEC 20.4).
    const auto plus = parse("1 + 5 + 3");
    CHECK_EQ(plus.size(), size_t(3));
    CHECK_EQ(plus[0].fixture, FixtureId(1));
    CHECK_EQ(plus[1].fixture, FixtureId(5));
    CHECK_EQ(plus[2].fixture, FixtureId(3));

    const auto mixed = parse("1 Thru 4 + 9");
    CHECK_EQ(mixed.size(), size_t(5));
    CHECK_EQ(mixed.back().fixture, FixtureId(9));

    // Cells: `1.3` addresses one cell.
    const auto cell = parse("1.3");
    CHECK_EQ(cell[0].fixture, FixtureId(1));
    CHECK_EQ(cell[0].cell, CellId(3));

    // `1.1 Thru 1.12` walks cells of one fixture, not fixtures.
    const auto cells = parse("1.1 Thru 1.12");
    CHECK_EQ(cells.size(), size_t(12));
    CHECK_EQ(cells.front().cell, CellId(1));
    CHECK_EQ(cells.back().cell, CellId(12));
    for (const Target& t : cells) CHECK_EQ(t.fixture, FixtureId(1));

    // Differing fixtures walk fixtures even when cells are given.
    const auto across = parse("1.1 Thru 3.1");
    CHECK_EQ(across.size(), size_t(3));

    std::string err;
    const auto toks = tokenise("Thru 5");
    size_t i = 0;
    std::vector<Target> out;
    CHECK(!parseTargets(toks, i, out, err));
    CHECK(!err.empty());
}

static void testSelectionAndValues() {
    Desk d;

    // A bare selector is a selection command in its own right.
    Result r = d.run("Fixture 1 Thru 4");
    CHECK(r.ok);
    CHECK_EQ(d.eng.programmer.selection().flatten().size(), size_t(4));

    // At with no attribute means Dimmer, and values are percentages.
    CHECK(d.run("At 50").ok);
    CHECK_NEAR(d.prog(1, Attr::Dimmer), 0.5);
    CHECK_NEAR(d.prog(4, Attr::Dimmer), 0.5);
    CHECK_NEAR(d.prog(5, Attr::Dimmer), -1.0);  // outside the selection

    // Selector and verb in one line.
    CHECK(d.run("Fixture 5 At Red 100").ok);
    CHECK_NEAR(d.prog(5, Attr::Red), 1.0);
    CHECK_NEAR(d.prog(5, Attr::Dimmer), -1.0);

    // Aliases resolve: `@` for At, and attribute shorthands.
    CHECK(d.run("Fixture 6 @ 25").ok);
    CHECK_NEAR(d.prog(6, Attr::Dimmer), 0.25);
    CHECK(d.run("Fixture 6 At B 80").ok);
    CHECK_NEAR(d.prog(6, Attr::Blue), 0.8);

    // And it reaches the output through the merge pipeline.
    CHECK_NEAR(d.out(1, Attr::Dimmer), 0.5);

    // Errors are reported rather than silently ignored.
    CHECK(!d.run("Fixture 1 At Nonsense 50").ok);
    CHECK(!d.run("Fixture 1 At").ok);
    CHECK(!d.run("Wibble 1").ok);
    CHECK(d.run("").ok);  // empty input is not an error
}

static void testCellAddressing() {
    Engine e;
    installStarterTypes(e.patch);
    Console c{e};
    CHECK(c.execute("Fixture 20 Patch \"Pixel Bar 12\" 1 1 1").ok);
    e.setClock(0.0);

    // Dimmer lives on the parent, so it writes one value.
    CHECK(c.execute("Fixture 20 At 100").ok);
    CHECK_EQ(e.programmer.values().size(), size_t(1));

    // Colour exists only on cells, so it broadcasts to all twelve.
    CHECK(c.execute("Fixture 20 At Red 50").ok);
    CHECK_EQ(e.programmer.values().keys(Group::Color).size(), size_t(12));

    // A single cell addresses only itself.
    e.programmer.clearAll();
    CHECK(c.execute("Fixture 20.3 At Green 100").ok);
    CHECK_EQ(e.programmer.values().size(), size_t(1));
    CHECK(e.programmer.values().has(20, 3, Attr::Green));

    // A cell range.
    e.programmer.clearAll();
    CHECK(c.execute("Fixture 20.1 Thru 20.6 At Blue 100").ok);
    CHECK_EQ(e.programmer.values().size(), size_t(6));
}

static void testClearThroughCommands() {
    Desk d;
    CHECK(d.run("Fixture 1 Thru 4 At 100").ok);

    // The staged clear is reachable from the command line, and reports which
    // stage it performed.
    Result r1 = d.run("Clear");
    CHECK(r1.message == "ClearSelection");
    CHECK(d.eng.programmer.selection().empty());
    CHECK_EQ(d.eng.programmer.values().size(), size_t(4));

    Result r2 = d.run("Clear");
    CHECK(r2.message == "ClearActive");
    CHECK_EQ(d.eng.programmer.values().size(), size_t(4));
    CHECK_EQ(d.eng.programmer.storeLayer().size(), size_t(0));

    Result r3 = d.run("Clear");
    CHECK(r3.message == "ClearAll");
    CHECK_EQ(d.eng.programmer.values().size(), size_t(0));

    Result r4 = d.run("Clear");
    CHECK(r4.message == "ClearCommandLine");
}

static void testStoreAndPlayback() {
    Desk d;

    // Store the programmer's active values into a cue.
    CHECK(d.run("Fixture 1 Thru 4 At 80").ok);
    Result s = d.run("Store Cue 1 1");
    CHECK(s.ok);
    CHECK(d.eng.sequence(1) != nullptr);
    CHECK_EQ(d.eng.sequence(1)->cues.size(), size_t(1));

    // A second cue, so Go has somewhere to advance to.
    CHECK(d.run("Clear").ok);
    CHECK(d.run("Clear").ok);
    CHECK(d.run("Clear").ok);
    CHECK(d.run("Fixture 1 Thru 4 At 20").ok);
    CHECK(d.run("Store Cue 2 1").ok);
    CHECK_EQ(d.eng.sequence(1)->cues.size(), size_t(2));

    // Storing nothing is refused rather than creating an empty cue.
    CHECK(d.run("ClearAll").ok);
    CHECK(!d.run("Store Cue 3 1").ok);

    // Assign and run it.
    CHECK(d.run("Assign 1 1").ok);
    for (auto& c : d.eng.sequence(1)->cues) c.timing.fadeIn = 0.0;

    CHECK(d.run("Go 1").ok);
    CHECK_NEAR(d.out(1, Attr::Dimmer), 0.8);
    CHECK(d.run("Go 1").ok);
    CHECK_NEAR(d.out(1, Attr::Dimmer), 0.2);
    CHECK(d.run("Back 1").ok);
    CHECK_NEAR(d.out(1, Attr::Dimmer), 0.8);
    CHECK(d.run("Goto 2 1").ok);
    CHECK_NEAR(d.out(1, Attr::Dimmer), 0.2);

    CHECK(d.run("Off 1").ok);
    CHECK_NEAR(d.out(1, Attr::Dimmer), -1.0);

    CHECK(!d.run("Go 99").ok);
    CHECK(!d.run("Goto 99 1").ok);
}

static void testMastersThroughCommands() {
    Desk d;
    CHECK(d.run("Fixture 1 At 100").ok);
    CHECK_NEAR(d.out(1, Attr::Dimmer), 1.0);

    CHECK(d.run("GrandMaster 50").ok);
    d.eng.tick();
    CHECK_EQ(int(d.eng.universe(1)->data[0]), 128);

    CHECK(d.run("Blackout on").ok);
    d.eng.tick();
    CHECK_EQ(int(d.eng.universe(1)->data[0]), 0);
    CHECK(d.run("Blackout off").ok);
    CHECK(d.run("GrandMaster 100").ok);
    d.eng.tick();
    CHECK_EQ(int(d.eng.universe(1)->data[0]), 255);

    // Toggling with no argument.
    CHECK(d.run("Blackout").ok);
    CHECK(d.eng.blackout);
    CHECK(d.run("Blackout").ok);
    CHECK(!d.eng.blackout);

    CHECK(d.run("UnusedChannels default").ok);
    CHECK(d.eng.unusedMode == UnusedMode::ToDefault);
    CHECK(d.run("UnusedChannels hold").ok);
    CHECK(d.eng.unusedMode == UnusedMode::HoldLast);
    CHECK(!d.run("UnusedChannels sideways").ok);
}

static void testHighlightThroughCommands() {
    Desk d;
    CHECK(d.run("Fixture 1 Thru 2").ok);
    CHECK(d.run("Highlight on").ok);
    CHECK_NEAR(d.out(1, Attr::Dimmer), 1.0);
    CHECK_NEAR(d.out(3, Attr::Dimmer), -1.0);

    // The second axis: unselected fixtures blacked out is solo.
    CHECK(d.run("Unselected blackout").ok);
    CHECK_NEAR(d.out(3, Attr::Dimmer), 0.0);
    CHECK(d.run("Unselected lowlight").ok);
    CHECK(d.out(3, Attr::Dimmer) > 0.0f);

    // Highlight never enters the programmer, so Store cannot capture it.
    CHECK_EQ(d.eng.programmer.storeLayer().size(), size_t(0));
    CHECK(d.run("Highlight off").ok);
    CHECK(d.run("Unselected unaffected").ok);
}

static void testPrivilege() {
    Desk d;

    // §16.5: an agent sits strictly below the operator. A protected command
    // is not merely discouraged — the path does not exist at that privilege.
    d.con.setPrivilege(Privilege::Any);
    Result r = d.run("Fixture 1 Unpatch");
    CHECK(!r.ok);
    CHECK(r.message.find("refused") != std::string::npos);
    CHECK(r.message.find("Protected") != std::string::npos);

    // Read-only introspection stays available at the lowest privilege, which
    // is what makes propose-only agent operation possible.
    CHECK(d.run("List patch").ok);
    CHECK(d.run("Help").ok);
    CHECK(d.run("Schema").ok);

    // And setting a value is operator-level, so a reduced caller cannot.
    CHECK(!d.run("Fixture 1 At 100").ok);

    d.con.setPrivilege(Privilege::Protected);
    CHECK(d.run("Fixture 1 At 100").ok);
    CHECK(d.run("Fixture 8 Unpatch").ok);
    CHECK(d.eng.patch.find(8) == nullptr);
}

static void testIntrospection() {
    Desk d;
    CHECK(d.run("Fixture 1 Thru 4 At 60").ok);

    Result patch = d.run("List patch");
    CHECK(patch.ok);
    CHECK_EQ(patch.rows.size(), size_t(8));
    CHECK(patch.rows[0].find("uid=") != std::string::npos);

    Result types = d.run("List types");
    CHECK(types.rows.size() >= 4);

    Result prog = d.run("List programmer");
    CHECK_EQ(prog.rows.size(), size_t(4));
    CHECK(prog.rows[0].find("Dimmer") != std::string::npos);

    Result sel = d.run("List selection");
    CHECK_EQ(sel.rows.size(), size_t(4));

    Result cmds = d.run("List commands");
    CHECK(cmds.rows.size() >= 20);

    CHECK(!d.run("List nonsense").ok);
}

static void testMetadataGeneratesEverything() {
    Desk d;

    // §16.2: one declaration generates the parser, the help and the agent
    // schema. If they came from separate sources they would drift.
    const auto& reg = d.con.registry();
    const CommandDef* at = reg.find("At");
    CHECK(at != nullptr);
    CHECK_EQ(at->params.size(), size_t(2));
    CHECK(at->params[0].type == ParamType::Attribute);
    CHECK(!at->params[0].required);
    CHECK(at->params[1].type == ParamType::Percent);
    CHECK(at->params[1].required);

    // Aliases declared once are usable on the command line.
    CHECK(reg.find("@") == at);
    CHECK(reg.find("at") == at);

    // Help is derived, not hand-written twice.
    const std::string h = reg.helpFor("At");
    CHECK(h.find("attribute") != std::string::npos);
    CHECK(h.find("value") != std::string::npos);
    CHECK(h.find("Operator") != std::string::npos);

    // Destructive and privilege are visible in the generated help.
    const std::string un = reg.helpFor("Unpatch");
    CHECK(un.find("Protected") != std::string::npos);
    CHECK(un.find("destructive") != std::string::npos);

    // The agent schema covers every command with its privilege and params.
    const std::string schema = reg.agentSchema();
    CHECK(schema.find("\"name\":\"At\"") != std::string::npos);
    CHECK(schema.find("\"privilege\":\"Protected\"") != std::string::npos);
    CHECK(schema.find("\"destructive\":true") != std::string::npos);
    CHECK(schema.front() == '[');
    CHECK(schema.back() == ']');

    // Every registered command carries help text and a handler — the metadata
    // is not optional.
    for (const auto* def : reg.all()) {
        CHECK(!def->help.empty());
        CHECK(def->fn != nullptr);
    }
}

static void testScripts() {
    Desk d;

    // Several statements in one go, which is what a macro is (§16.2).
    Result r = d.con.executeAll(
        "Fixture 1 Thru 4 At 100; Fixture 5 At Red 50; GrandMaster 75");
    CHECK(r.ok);
    CHECK_NEAR(d.prog(1, Attr::Dimmer), 1.0);
    CHECK_NEAR(d.prog(5, Attr::Red), 0.5);
    CHECK_NEAR(d.eng.grandMaster, 0.75);

    // A script stops at the first failure rather than carrying on blindly.
    Result bad = d.con.executeAll("Fixture 6 At 10; Nonsense; Fixture 7 At 10");
    CHECK(!bad.ok);
    CHECK_NEAR(d.prog(6, Attr::Dimmer), 0.1);
    CHECK_NEAR(d.prog(7, Attr::Dimmer), -1.0);  // never ran

    // Empty segments are tolerated.
    CHECK(d.con.executeAll("Fixture 1 At 50;;").ok);

    // History records what was run, for the action log (§8.6).
    CHECK(d.con.history().size() > 5);
}

static void testSelectionOrderPreserved() {
    Desk d;

    // Order is capture order all the way through the command layer, because
    // it determines FX spread and fan direction (§20.4).
    CHECK(d.run("Fixture 4 + 1 + 7").ok);
    const auto sel = d.eng.programmer.selection().flatten();
    CHECK_EQ(sel.size(), size_t(3));
    CHECK_EQ(sel[0].fixture, FixtureId(4));
    CHECK_EQ(sel[1].fixture, FixtureId(1));
    CHECK_EQ(sel[2].fixture, FixtureId(7));

    CHECK(d.run("Reverse").ok);
    CHECK_EQ(d.eng.programmer.selection().flatten()[0].fixture, FixtureId(7));

    // Shuffle is seeded, so the same seed gives the same order.
    CHECK(d.run("Fixture 1 Thru 8").ok);
    CHECK(d.run("Shuffle 42").ok);
    const auto a = d.eng.programmer.selection().flatten();
    CHECK(d.run("Fixture 1 Thru 8").ok);
    CHECK(d.run("Shuffle 42").ok);
    const auto b = d.eng.programmer.selection().flatten();
    for (size_t i = 0; i < a.size(); ++i) CHECK_EQ(a[i].fixture, b[i].fixture);
}

// A selection can start from a group as well as from fixture numbers, and a
// preset can be applied — both are command grammar, because §16.2 means the
// UI has no other way to do either.
void testGroupSelector() {
    Desk d;

    GroupDef wash;
    wash.id = 1;
    wash.name = "Front Wash";
    wash.members = {{1, 0}, {2, 0}, {3, 0}, {4, 0}};
    d.eng.groups.add(wash);

    GroupDef odds;
    odds.id = 2;
    odds.name = "Odds";
    odds.members = {{5, 0}, {7, 0}};
    d.eng.groups.add(odds);

    // A bare group selector is a selection in its own right.
    CHECK(d.run("Group 1").ok);
    CHECK_EQ(d.eng.programmer.selection().size(), size_t(4));

    // Several groups, in typing order, each kept as a group: §20.8's tree is
    // what multi-level fan needs, so selecting two groups must not flatten.
    CHECK(d.run("Group 1 + 2").ok);
    CHECK_EQ(d.eng.programmer.selection().size(), size_t(6));
    CHECK_EQ(d.eng.programmer.selection().roots().size(), size_t(2));
    CHECK(d.eng.programmer.selection().roots()[0].type == SelNode::Type::Group);

    // A command after the selector operates on it.
    CHECK(d.run("Group 1 At 50").ok);
    CHECK_NEAR(d.out(1, Attr::Dimmer), 0.5f);
    CHECK_NEAR(d.out(4, Attr::Dimmer), 0.5f);
    CHECK(d.out(5, Attr::Dimmer) <= 0.f);

    // Abbreviations, because nobody types the long word on a show.
    CHECK(d.run("g 2").ok);
    CHECK_EQ(d.eng.programmer.selection().size(), size_t(2));

    // A group that is not there is named, not silently ignored.
    const Result miss = d.run("Group 9");
    CHECK(!miss.ok);
    CHECK(miss.parsed);
    CHECK(miss.message.find("9") != std::string::npos);

    // An empty group is refused rather than quietly selecting nothing, which
    // would look like the command worked.
    GroupDef empty;
    empty.id = 3;
    empty.name = "Nothing";
    d.eng.groups.add(empty);
    CHECK(!d.run("Group 3").ok);
}

void testPresetThroughCommands() {
    Desk d;

    Preset red;
    red.id = 1;
    red.name = "Red";
    red.group = Group::Color;
    // Fixture 0 is the wildcard: one palette for the whole rig (§7.1).
    red.values.set(0, 0, Attr::Red, Value::hard(1.f));
    red.values.set(0, 0, Attr::Green, Value::hard(0.f));
    red.values.set(0, 0, Attr::Blue, Value::hard(0.f));
    d.eng.presets.ensurePool(1, "Colour", Group::Color);
    const Preset& stored = d.eng.presets.add(1, red);

    Preset pos;
    pos.id = 1;
    pos.name = "Home";
    pos.group = Group::Position;
    pos.values.set(0, 0, Attr::Pan, Value::hard(0.5f));
    d.eng.presets.ensurePool(2, "Position", Group::Position);
    d.eng.presets.add(2, pos);

    // Dotted pool.id form.
    CHECK(d.run("Fixture 1 Thru 4 Preset 1.1").ok);
    CHECK_NEAR(d.out(1, Attr::Red), 1.f);
    CHECK_NEAR(d.out(1, Attr::Green), 0.f);

    // §11.8: it lands as a REFERENCE, so editing the preset moves every value
    // that came from it. A copy would not.
    const Value* v = d.eng.programmer.values().find(makeKey(1, 0, Attr::Red));
    CHECK(v != nullptr);
    CHECK(v && v->isRef());
    CHECK(v && v->preset == stored.uid);

    // By name, when exactly one pool holds it (§8.2).
    CHECK(d.run("Fixture 5 Preset Red").ok);
    CHECK_NEAR(d.out(5, Attr::Red), 1.f);

    // A name nothing holds.
    CHECK(!d.run("Fixture 1 Preset Nope").ok);

    // A bare number cannot name a preset: which pool is a real question, and
    // guessing would be worse than asking.
    const Result bare = d.run("Fixture 1 Preset 1");
    CHECK(!bare.ok);
    CHECK(bare.message.find("pool") != std::string::npos);

    // A position preset on fixtures with no pan is refused with a reason,
    // rather than appearing to work.
    const Result wrong = d.run("Fixture 1 Thru 4 Preset 2.1");
    CHECK(!wrong.ok);

    // Group selector and preset compose, which is the common console line.
    GroupDef g;
    g.id = 1;
    g.name = "Wash";
    g.members = {{6, 0}, {7, 0}};
    d.eng.groups.add(g);
    CHECK(d.run("Group 1 Preset 1.1").ok);
    CHECK_NEAR(d.out(7, Attr::Red), 1.f);
}

int main() {
    std::printf("lumen command\n");
    testLexer();
    testRangeParsing();
    testSelectionAndValues();
    testCellAddressing();
    testClearThroughCommands();
    testStoreAndPlayback();
    testMastersThroughCommands();
    testHighlightThroughCommands();
    testPrivilege();
    testIntrospection();
    testMetadataGeneratesEverything();
    testScripts();
    testSelectionOrderPreserved();
    testGroupSelector();
    testPresetThroughCommands();
    return check::report("command");
}
