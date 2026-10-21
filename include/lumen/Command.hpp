// Command.hpp — the command layer (SPEC §16.2).
//
// ARCHITECTURAL RULE: the UI calls the command layer, never the model
// directly. Every operation exists as a command, including ones with no
// sensible text form. The moment one feature reaches around this, macros,
// plugins, tests, remote clients and agents all lose access to that feature.
//
// Commands carry structured metadata from ONE declaration. That metadata
// generates the parser, the help text, and the agent tool schema — so they
// cannot drift apart.
#pragma once

#include "lumen/Engine.hpp"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace lumen {

// ---------------------------------------------------------------------------
// Privilege (§16.5). An agent sits strictly below the operator, and a
// destructive command is blocked unless explicitly enabled.
// ---------------------------------------------------------------------------
enum class Privilege : uint8_t {
    Any = 0,      // anyone, including a propose-only agent
    Operator,     // the person at the desk
    Protected,    // destructive: delete, renumber, show overwrite
    Safety,       // laser arm, emergency stop — never granted to an agent
};

const char* privilegeName(Privilege p);

// ---------------------------------------------------------------------------
// Parameter metadata
// ---------------------------------------------------------------------------
enum class ParamType : uint8_t {
    Int, Float, Percent, String, Attribute, Enum, Bool, Targets
};

const char* paramTypeName(ParamType t);

struct ParamDef {
    std::string name;
    ParamType type = ParamType::Float;
    bool required = true;
    std::string help;
    std::vector<std::string> enumValues;

    ParamDef() = default;
    ParamDef(std::string n, ParamType t, bool req, std::string h,
             std::vector<std::string> e = {})
        : name(std::move(n)), type(t), required(req), help(std::move(h)),
          enumValues(std::move(e)) {}
};

// ---------------------------------------------------------------------------
// Values passed to a handler
// ---------------------------------------------------------------------------
struct Arg {
    std::string raw;
    double number = 0.0;
    bool isNumber = false;
    std::optional<Attr> attr;
};

struct Result {
    bool ok = true;
    // True once dispatch found the command, whatever the handler then decided.
    // "You typed nonsense" and "that will not work right now" are different
    // answers and the UI needs to tell them apart.
    bool parsed = false;
    std::string message;
    // Structured payload for introspection commands (§16.3), so a caller gets
    // data rather than having to scrape the message.
    std::vector<std::string> rows;

    static Result good(std::string m = {}) { return Result{true, true, std::move(m), {}}; }
    static Result bad(std::string m) { return Result{false, true, std::move(m), {}}; }
};

class CommandRegistry;

class Console;

struct Context {
    Engine& engine;
    CommandRegistry& registry;
    // The console that dispatched this command, for the handful of commands
    // that need session state rather than show state — Learn's focus, for
    // instance, belongs to the operator's current act of mapping, not to the
    // show. Null when a command is invoked without one.
    Console* console = nullptr;
    std::vector<Target> targets;  // from the selector clause, if any
    std::vector<Arg> args;
    Privilege granted = Privilege::Operator;
    // Who is running this, for the action log (§8.6, §16.5): the operator, a
    // macro, a plugin, or the agent. An agent batch has to be attributable
    // after the fact or the audit record is worthless.
    std::string source = "operator";
    // The prompt behind an agent action, recorded alongside it so the audit
    // trail and the conversation that caused it share one timeline (§16.5).
    std::string prompt;

    const Arg* arg(size_t i) const { return i < args.size() ? &args[i] : nullptr; }
    double number(size_t i, double fallback = 0.0) const;
    std::string text(size_t i, const std::string& fallback = {}) const;
};

using Handler = std::function<Result(Context&)>;

struct CommandDef {
    std::string name;
    std::vector<std::string> aliases;
    std::vector<ParamDef> params;
    Privilege privilege = Privilege::Operator;
    bool destructive = false;
    bool usesSelection = false;  // acts on the selector clause / programmer
    std::string help;
    Handler fn;
};

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------
class CommandRegistry {
public:
    void add(CommandDef def);
    const CommandDef* find(const std::string& name) const;
    std::vector<const CommandDef*> all() const;

    // Generated from the same metadata, which is the point of declaring it
    // once (§16.2).
    std::string helpText() const;
    std::string helpFor(const std::string& name) const;
    std::string agentSchema() const;  // JSON tool schema

private:
    std::map<std::string, CommandDef> defs_;
    std::map<std::string, std::string> aliasToName_;
};

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------
struct Token {
    enum class Kind : uint8_t { Word, Number, String, Plus, Minus, Thru, End };
    Kind kind = Kind::Word;
    std::string text;
    double number = 0.0;
    // A dotted number carries a cell: `12.3` is fixture 12 cell 3.
    bool hasCell = false;
    uint32_t whole = 0;
    uint32_t cell = 0;
};

std::vector<Token> tokenise(const std::string& line);

// Parses a target range: `1`, `1.3`, `1 Thru 10`, `1 + 3 + 5`,
// `1 Thru 4 + 9`, `1.1 Thru 1.12`.
bool parseTargets(const std::vector<Token>& tokens, size_t& i,
                  std::vector<Target>& out, std::string& err);

// ---------------------------------------------------------------------------
// Console — the only mutation path into the engine
// ---------------------------------------------------------------------------
class Console {
public:
    explicit Console(Engine& engine);

    Result execute(const std::string& line);
    // Runs several statements separated by ';', stopping at the first failure.
    Result executeAll(const std::string& script);

    CommandRegistry& registry() { return registry_; }
    const CommandRegistry& registry() const { return registry_; }

    // The agent and remote clients run at a reduced privilege (§16.5).
    void setPrivilege(Privilege p) { granted_ = p; }
    Privilege privilege() const { return granted_; }

    // What the action log attributes commands to. Defaults to "operator", or
    // "agent" when running at the reduced privilege an agent gets.
    void setSource(std::string s) { source_ = std::move(s); }
    const std::string& source() const { return source_; }
    // The prompt behind an agent batch, recorded with each action it causes
    // so the two timelines line up (§16.5).
    void setPrompt(std::string p) { prompt_ = std::move(p); }
    const std::string& prompt() const { return prompt_; }

    const std::vector<std::string>& history() const { return history_; }

    // Learn lives here rather than on the engine: binding a target means
    // naming a command for it, and the command layer is the only thing that
    // knows what is typeable (§12.2, §16.2).
    Learn& learn() { return learn_; }
    const Learn& learn() const { return learn_; }

    // Acts on what the engine's input drain produced. The engine resolves and
    // maps; this runs the commands, because a Command target is the general
    // escape hatch and only the command layer can fire one (§12.2).
    struct Pumped {
        int actions = 0;
        int executed = 0;
        int failed = 0;
        std::vector<std::string> messages;
    };
    Pumped pumpBindings();

private:
    void installBuiltins();

    Engine& engine_;
    CommandRegistry registry_;
    Privilege granted_ = Privilege::Operator;
    std::string source_;
    std::string prompt_;
    std::vector<std::string> history_;
    Learn learn_{engine_.bindings};
};

} // namespace lumen
