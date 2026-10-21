// Xml.hpp — a minimal XML DOM, built on expat.
//
// GDTF's description.xml is deeply nested and attribute-heavy, and a SAX
// callback soup would be unreadable. This is just enough DOM to walk it.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace lumen {

struct XmlNode {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<XmlNode> children;
    std::string text;

    // Attribute access. `attr` returns nullopt when absent, so a caller can
    // tell "missing" from "present but empty" — which matters in GDTF, where
    // an empty ChannelSet name is meaningful.
    std::optional<std::string> attr(const std::string& key) const;
    std::string attrOr(const std::string& key, const std::string& fallback = {}) const;
    int attrInt(const std::string& key, int fallback = 0) const;
    double attrDouble(const std::string& key, double fallback = 0.0) const;
    bool hasAttr(const std::string& key) const;

    // Direct children with a given tag.
    std::vector<const XmlNode*> kids(const std::string& tag) const;
    const XmlNode* kid(const std::string& tag) const;
    // First descendant at any depth, for the handful of places GDTF nests
    // a section deeper than the schema suggests.
    const XmlNode* descend(const std::string& tag) const;
};

struct XmlDoc {
    bool ok = false;
    std::string error;
    XmlNode root;
};

XmlDoc parseXml(const std::string& text);

} // namespace lumen
