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
    std::string message;
    // Structured payload for introspection commands (§16.3), so a caller gets
    // data rather than having to scrape the message.
    std::vector<std::string> rows;

    static Result good(std::string m = {}) { return Result{true, std::move(m), {}}; }
    static Result bad(std::string m) { return Result{false, std::move(m), {}}; }
};

class CommandRegistry;

struct Context {
    Engine& engine;
    CommandRegistry& registry;
    std::vector<Target> targets;  // from the selector clause, if any
    std::vector<Arg> args;
    Privilege granted = Privilege::Operator;

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

    const std::vector<std::string>& history() const { return history_; }

private:
    void installBuiltins();

    Engine& engine_;
    CommandRegistry registry_;
    Privilege granted_ = Privilege::Operator;
    std::vector<std::string> history_;
};

} // namespace lumen
