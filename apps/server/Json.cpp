#include "Json.hpp"

#include <cmath>
#include <cstdio>

namespace lumen {

void Json::separate() {
    if (expectValue_) {
        // We are supplying the value half of a key, so no comma.
        expectValue_ = false;
        return;
    }
    if (empty_.empty()) return;
    if (!empty_.back()) out_ += ',';
    empty_.back() = false;
}

Json& Json::beginObject() {
    separate();
    out_ += '{';
    empty_.push_back(true);
    return *this;
}

Json& Json::endObject() {
    out_ += '}';
    if (!empty_.empty()) empty_.pop_back();
    return *this;
}

Json& Json::beginArray() {
    separate();
    out_ += '[';
    empty_.push_back(true);
    return *this;
}

Json& Json::endArray() {
    out_ += ']';
    if (!empty_.empty()) empty_.pop_back();
    return *this;
}

Json& Json::key(const std::string& k) {
    separate();
    out_ += '"';
    out_ += escape(k);
    out_ += "\":";
    expectValue_ = true;
    return *this;
}

Json& Json::value(const std::string& v) {
    separate();
    out_ += '"';
    out_ += escape(v);
    out_ += '"';
    return *this;
}

Json& Json::value(int v) { return value(static_cast<long long>(v)); }

Json& Json::value(long long v) {
    separate();
    out_ += std::to_string(v);
    return *this;
}

Json& Json::value(unsigned long long v) {
    separate();
    out_ += std::to_string(v);
    return *this;
}

Json& Json::value(double v) {
    separate();
    // JSON has no NaN or Infinity, and emitting either produces a document
    // the browser refuses to parse at all — so one bad float would take the
    // whole snapshot with it.
    if (!std::isfinite(v)) {
        out_ += "null";
        return *this;
    }
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.6g", v);
    out_ += buf;
    return *this;
}

Json& Json::value(bool v) {
    separate();
    out_ += v ? "true" : "false";
    return *this;
}

Json& Json::null() {
    separate();
    out_ += "null";
    return *this;
}

Json& Json::raw(const std::string& json) {
    separate();
    out_ += json;
    return *this;
}

std::string Json::escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    // Control characters have to be escaped or the document
                    // is invalid. Fixture names come from imported files, so
                    // this is not hypothetical.
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

} // namespace lumen
