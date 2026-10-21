#include "lumen/Xml.hpp"

#include <expat.h>

#include <cstdlib>
#include <vector>

namespace lumen {

std::optional<std::string> XmlNode::attr(const std::string& key) const {
    for (const auto& [k, v] : attrs)
        if (k == key) return v;
    return std::nullopt;
}

std::string XmlNode::attrOr(const std::string& key, const std::string& fallback) const {
    const auto v = attr(key);
    return v ? *v : fallback;
}

bool XmlNode::hasAttr(const std::string& key) const { return attr(key).has_value(); }

int XmlNode::attrInt(const std::string& key, int fallback) const {
    const auto v = attr(key);
    if (!v || v->empty()) return fallback;
    try {
        return std::stoi(*v);
    } catch (...) {
        return fallback;
    }
}

double XmlNode::attrDouble(const std::string& key, double fallback) const {
    const auto v = attr(key);
    if (!v || v->empty()) return fallback;
    try {
        return std::stod(*v);
    } catch (...) {
        return fallback;
    }
}

std::vector<const XmlNode*> XmlNode::kids(const std::string& tag) const {
    std::vector<const XmlNode*> out;
    for (const auto& c : children)
        if (c.name == tag) out.push_back(&c);
    return out;
}

const XmlNode* XmlNode::kid(const std::string& tag) const {
    for (const auto& c : children)
        if (c.name == tag) return &c;
    return nullptr;
}

const XmlNode* XmlNode::descend(const std::string& tag) const {
    if (const XmlNode* direct = kid(tag)) return direct;
    for (const auto& c : children)
        if (const XmlNode* deep = c.descend(tag)) return deep;
    return nullptr;
}

namespace {

// The stack holds a PATH OF INDICES, not pointers. Appending to a node's
// children vector can reallocate it, so any pointer into it would dangle --
// the same hazard that bit the executor list earlier.
struct Builder {
    XmlNode root;
    std::vector<size_t> path;
    bool started = false;

    XmlNode* current() {
        XmlNode* n = &root;
        for (size_t i : path) n = &n->children[i];
        return n;
    }
};

void onStart(void* user, const XML_Char* name, const XML_Char** atts) {
    auto* b = static_cast<Builder*>(user);

    XmlNode node;
    node.name = name;
    for (int i = 0; atts[i]; i += 2) node.attrs.emplace_back(atts[i], atts[i + 1]);

    if (!b->started) {
        // The document element becomes the root rather than a child of it.
        b->root = std::move(node);
        b->started = true;
        return;
    }
    XmlNode* parent = b->current();
    parent->children.push_back(std::move(node));
    b->path.push_back(parent->children.size() - 1);
}

void onEnd(void* user, const XML_Char*) {
    auto* b = static_cast<Builder*>(user);
    if (!b->path.empty()) b->path.pop_back();
}

void onText(void* user, const XML_Char* s, int len) {
    auto* b = static_cast<Builder*>(user);
    if (!b->started) return;
    b->current()->text.append(s, static_cast<size_t>(len));
}

} // namespace

XmlDoc parseXml(const std::string& text) {
    XmlDoc doc;
    XML_Parser p = XML_ParserCreate(nullptr);
    if (!p) {
        doc.error = "cannot create parser";
        return doc;
    }

    Builder b;
    XML_SetUserData(p, &b);
    XML_SetElementHandler(p, onStart, onEnd);
    XML_SetCharacterDataHandler(p, onText);

    const int rc = XML_Parse(p, text.data(), static_cast<int>(text.size()), 1);
    if (rc == XML_STATUS_ERROR) {
        doc.error = std::string(XML_ErrorString(XML_GetErrorCode(p))) + " at line " +
                    std::to_string(XML_GetCurrentLineNumber(p));
        XML_ParserFree(p);
        return doc;
    }
    XML_ParserFree(p);

    doc.root = std::move(b.root);
    doc.ok = true;
    return doc;
}

} // namespace lumen
