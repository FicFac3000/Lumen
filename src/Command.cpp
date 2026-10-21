#include "lumen/Command.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace lumen {

namespace {

std::string lower(const std::string& s) {
    std::string o = s;
    std::transform(o.begin(), o.end(), o.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return o;
}

bool isSelectorWord(const std::string& w) {
    const std::string k = lower(w);
    return k == "fixture" || k == "fix" || k == "f" || k == "channel" || k == "chan";
}

std::string jsonEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else o += c;
    }
    return o;
}

} // namespace

const char* privilegeName(Privilege p) {
    switch (p) {
        case Privilege::Any: return "Any";
        case Privilege::Operator: return "Operator";
        case Privilege::Protected: return "Protected";
        case Privilege::Safety: return "Safety";
    }
    return "?";
}

const char* paramTypeName(ParamType t) {
    switch (t) {
        case ParamType::Int: return "int";
        case ParamType::Float: return "float";
        case ParamType::Percent: return "percent";
        case ParamType::String: return "string";
        case ParamType::Attribute: return "attribute";
        case ParamType::Enum: return "enum";
        case ParamType::Bool: return "bool";
        case ParamType::Targets: return "targets";
    }
    return "?";
}

double Context::number(size_t i, double fallback) const {
    const Arg* a = arg(i);
    return (a && a->isNumber) ? a->number : fallback;
}

std::string Context::text(size_t i, const std::string& fallback) const {
    const Arg* a = arg(i);
    return a ? a->raw : fallback;
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------
void CommandRegistry::add(CommandDef def) {
    const std::string key = lower(def.name);
    for (const auto& a : def.aliases) aliasToName_[lower(a)] = key;
    defs_[key] = std::move(def);
}

const CommandDef* CommandRegistry::find(const std::string& name) const {
    const std::string k = lower(name);
    auto it = defs_.find(k);
    if (it != defs_.end()) return &it->second;
    auto ai = aliasToName_.find(k);
    if (ai != aliasToName_.end()) {
        auto di = defs_.find(ai->second);
        if (di != defs_.end()) return &di->second;
    }
    return nullptr;
}

std::vector<const CommandDef*> CommandRegistry::all() const {
    std::vector<const CommandDef*> out;
    out.reserve(defs_.size());
    for (const auto& [k, d] : defs_) {
        (void)k;
        out.push_back(&d);
    }
    return out;
}

std::string CommandRegistry::helpText() const {
    std::ostringstream o;
    for (const auto* d : all()) {
        o << d->name;
        for (const auto& p : d->params) {
            o << (p.required ? " <" : " [") << p.name << (p.required ? ">" : "]");
        }
        if (d->destructive) o << "   [destructive]";
        o << "\n    " << d->help << "\n";
    }
    return o.str();
}

std::string CommandRegistry::helpFor(const std::string& name) const {
    const CommandDef* d = find(name);
    if (!d) return "no such command: " + name;
    std::ostringstream o;
    o << d->name;
    for (const auto& p : d->params)
        o << (p.required ? " <" : " [") << p.name << (p.required ? ">" : "]");
    o << "\n" << d->help << "\n";
    if (!d->aliases.empty()) {
        o << "aliases:";
        for (const auto& a : d->aliases) o << " " << a;
        o << "\n";
    }
    o << "privilege: " << privilegeName(d->privilege);
    if (d->destructive) o << " (destructive)";
    o << "\n";
    for (const auto& p : d->params) {
        o << "  " << p.name << " : " << paramTypeName(p.type)
          << (p.required ? " (required)" : " (optional)");
        if (!p.help.empty()) o << " — " << p.help;
        if (!p.enumValues.empty()) {
            o << " [";
            for (size_t i = 0; i < p.enumValues.size(); ++i)
                o << (i ? "|" : "") << p.enumValues[i];
            o << "]";
        }
        o << "\n";
    }
    return o.str();
}

std::string CommandRegistry::agentSchema() const {
    // Generated from the same metadata as the parser and the help, so the
    // agent's view of the console cannot drift from what the console does.
    std::ostringstream o;
    o << "[\n";
    bool first = true;
    for (const auto* d : all()) {
        if (!first) o << ",\n";
        first = false;
        o << "  {\"name\":\"" << jsonEscape(d->name) << "\""
          << ",\"description\":\"" << jsonEscape(d->help) << "\""
          << ",\"privilege\":\"" << privilegeName(d->privilege) << "\""
          << ",\"destructive\":" << (d->destructive ? "true" : "false")
          << ",\"params\":[";
        for (size_t i = 0; i < d->params.size(); ++i) {
            const auto& p = d->params[i];
            o << (i ? "," : "") << "{\"name\":\"" << jsonEscape(p.name) << "\""
              << ",\"type\":\"" << paramTypeName(p.type) << "\""
              << ",\"required\":" << (p.required ? "true" : "false") << "}";
        }
        o << "]}";
    }
    o << "\n]";
    return o.str();
}

// ---------------------------------------------------------------------------
// Lexer
// ---------------------------------------------------------------------------
std::vector<Token> tokenise(const std::string& line) {
    std::vector<Token> out;
    size_t i = 0;
    const size_t n = line.size();

    while (i < n) {
        const char c = line[i];
        if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }

        if (c == '+') { out.push_back({Token::Kind::Plus, "+", 0, false, 0, 0}); ++i; continue; }
        if (c == '-') { out.push_back({Token::Kind::Minus, "-", 0, false, 0, 0}); ++i; continue; }

        if (c == '"') {
            ++i;
            std::string s;
            while (i < n && line[i] != '"') s += line[i++];
            if (i < n) ++i;
            Token t;
            t.kind = Token::Kind::String;
            t.text = s;
            out.push_back(t);
            continue;
        }

        if (std::isdigit(static_cast<unsigned char>(c))) {
            std::string s;
            while (i < n && (std::isdigit(static_cast<unsigned char>(line[i])) || line[i] == '.'))
                s += line[i++];
            Token t;
            t.kind = Token::Kind::Number;
            t.text = s;
            const size_t dot = s.find('.');
            if (dot != std::string::npos) {
                // `12.3` is fixture 12, cell 3 — not a decimal. Cue numbers
                // are parsed as decimals by their own commands.
                t.hasCell = true;
                t.whole = static_cast<uint32_t>(std::stoul(s.substr(0, dot)));
                const std::string rest = s.substr(dot + 1);
                t.cell = rest.empty() ? 0 : static_cast<uint32_t>(std::stoul(rest));
                t.number = std::stod(s);
            } else {
                t.whole = static_cast<uint32_t>(std::stoul(s));
                t.number = std::stod(s);
            }
            out.push_back(t);
            continue;
        }

        std::string w;
        while (i < n && !std::isspace(static_cast<unsigned char>(line[i])) &&
               line[i] != '+' && line[i] != '"')
            w += line[i++];
        Token t;
        t.text = w;
        t.kind = lower(w) == "thru" ? Token::Kind::Thru : Token::Kind::Word;
        out.push_back(t);
    }

    out.push_back({Token::Kind::End, "", 0, false, 0, 0});
    return out;
}

// ---------------------------------------------------------------------------
// Target ranges
// ---------------------------------------------------------------------------
bool parseTargets(const std::vector<Token>& tokens, size_t& i,
                  std::vector<Target>& out, std::string& err) {
    auto atNumber = [&]() { return tokens[i].kind == Token::Kind::Number; };
    if (!atNumber()) {
        err = "expected a fixture number";
        return false;
    }

    while (atNumber()) {
        const Token first = tokens[i++];

        if (tokens[i].kind == Token::Kind::Thru) {
            ++i;
            if (!atNumber()) {
                err = "expected a number after Thru";
                return false;
            }
            const Token last = tokens[i++];

            // `1.1 Thru 1.12` walks cells of one fixture; `1 Thru 5` walks
            // fixtures. Which one is meant is unambiguous from the operands.
            if (first.hasCell && last.hasCell && first.whole == last.whole) {
                const uint32_t lo = std::min(first.cell, last.cell);
                const uint32_t hi = std::max(first.cell, last.cell);
                for (uint32_t c = lo; c <= hi; ++c)
                    out.push_back({first.whole, static_cast<CellId>(c)});
            } else {
                const uint32_t lo = std::min(first.whole, last.whole);
                const uint32_t hi = std::max(first.whole, last.whole);
                for (uint32_t f = lo; f <= hi; ++f) out.push_back({f, 0});
            }
        } else {
            out.push_back({first.whole, static_cast<CellId>(first.hasCell ? first.cell : 0)});
        }

        if (tokens[i].kind == Token::Kind::Plus) {
            ++i;
            if (!atNumber()) {
                err = "expected a number after +";
                return false;
            }
            continue;
        }
        break;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Console
// ---------------------------------------------------------------------------
Console::Console(Engine& engine) : engine_(engine) { installBuiltins(); }

Result Console::executeAll(const std::string& script) {
    std::stringstream ss(script);
    std::string part;
    Result last = Result::good();
    while (std::getline(ss, part, ';')) {
        // Blank segments between separators are not an error.
        const bool blank = std::all_of(part.begin(), part.end(), [](unsigned char c) {
            return std::isspace(c) != 0;
        });
        if (blank) continue;
        last = execute(part);
        if (!last.ok) return last;
    }
    return last;
}

Result Console::execute(const std::string& line) {
    const auto tokens = tokenise(line);
    size_t i = 0;
    if (tokens[i].kind == Token::Kind::End) return Result::good();

    history_.push_back(line);

    std::vector<Target> targets;

    // Optional selector clause: `Fixture 1 Thru 10 ...`
    if (tokens[i].kind == Token::Kind::Word && isSelectorWord(tokens[i].text)) {
        ++i;
        std::string err;
        if (!parseTargets(tokens, i, targets, err)) return Result::bad(err);

        // A bare selector is a selection command in its own right.
        if (tokens[i].kind == Token::Kind::End) {
            engine_.programmer.selection().set(targets);
            return Result::good("selected " + std::to_string(targets.size()) + " fixture(s)");
        }
    }

    if (tokens[i].kind != Token::Kind::Word)
        return Result::bad("expected a command");

    const std::string verb = tokens[i].text;
    const CommandDef* def = registry_.find(verb);
    if (!def) {
        Result r = Result::bad("unknown command: " + verb);
        r.parsed = false;
        return r;
    }
    ++i;

    // Privilege gate (§16.5). A reduced-privilege caller cannot reach a
    // protected or safety command at all — the path does not exist, rather
    // than being discouraged.
    if (static_cast<int>(def->privilege) > static_cast<int>(granted_))
        return Result::bad(std::string("refused: ") + def->name + " requires " +
                           privilegeName(def->privilege) + " privilege");

    Context ctx{engine_, registry_, targets, {}, granted_};
    for (; tokens[i].kind != Token::Kind::End; ++i) {
        Arg a;
        a.raw = tokens[i].text;
        if (tokens[i].kind == Token::Kind::Number) {
            a.isNumber = true;
            a.number = tokens[i].number;
        } else if (tokens[i].kind == Token::Kind::Word) {
            a.attr = attrFromName(tokens[i].text);
        }
        ctx.args.push_back(std::move(a));
    }

    if (!def->fn) return Result::bad(def->name + " has no handler");
    return def->fn(ctx);
}

} // namespace lumen
