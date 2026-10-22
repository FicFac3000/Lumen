// Json.hpp — just enough JSON writing for the state snapshot.
//
// Hand-rolled rather than a dependency: the server emits JSON and never reads
// it, so the half that is hard (parsing, with all its error cases) is not
// needed at all.
#pragma once

#include <string>
#include <vector>

namespace lumen {

class Json {
public:
    // Objects and arrays track whether a separator is due, so no call site
    // has to remember whether it is the first member.
    Json& beginObject();
    Json& endObject();
    Json& beginArray();
    Json& endArray();

    Json& key(const std::string& k);

    Json& value(const std::string& v);
    Json& value(const char* v) { return value(std::string(v)); }
    Json& value(int v);
    Json& value(long long v);
    Json& value(unsigned long long v);
    Json& value(size_t v) { return value(static_cast<unsigned long long>(v)); }
    Json& value(double v);
    Json& value(float v) { return value(static_cast<double>(v)); }
    Json& value(bool v);
    Json& null();
    // Already-formatted JSON, for composing one builder's output into another.
    Json& raw(const std::string& json);

    // The common shape: "key": value.
    template <class T>
    Json& member(const std::string& k, const T& v) {
        return key(k).value(v);
    }

    const std::string& str() const { return out_; }

    static std::string escape(const std::string& s);

private:
    void separate();

    std::string out_;
    // True while the current container is still empty, so the next member
    // needs no leading comma.
    std::vector<bool> empty_;
    bool expectValue_ = false;
};

} // namespace lumen
