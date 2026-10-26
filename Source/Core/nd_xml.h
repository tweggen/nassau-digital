#pragma once

// nd_xml.h - a small hand-written XML reader and writer (DESIGN §9.2, §12).
//
// The core owns the one XML grammar of a program: qbx hands it the program's
// element subtree as text, and the later plugin player reads the same files
// with the same code. R2 forbids a third-party parser, so this is the subset
// a program file needs, written out by hand with the standard library only:
//
//   * elements with attributes (single or double quotes), nested, or empty
//     (`<x/>`); attribute order is kept;
//   * character data, kept per element as one string (entities decoded);
//     whitespace-only text is dropped, and in an element that also has child
//     elements the text is trimmed at both ends (layout, not content);
//   * the five predefined entities and numeric character references
//     (`&#38;`, `&#x26;`), decoded to UTF-8;
//   * an optional XML declaration, comments, processing instructions and
//     CDATA sections. Comments and processing instructions are skipped, CDATA
//     is character data.
//
// Not supported, and refused with an error rather than misread: a DOCTYPE
// (and so any user-defined entity) and an unknown named entity.
//
// Nothing here runs on the audio thread; it allocates freely.

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace nd {

struct XmlNode {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attributes;  ///< in document order
    std::vector<XmlNode> children;                                ///< element children, in order
    std::string text;  ///< character data directly inside this element, concatenated; empty when it is whitespace only

    /// The attribute's value, or nullptr if absent.
    const std::string* attribute(const std::string& key) const;

    bool operator==(const XmlNode& o) const {
        return name == o.name && attributes == o.attributes && children == o.children && text == o.text;
    }
    bool operator!=(const XmlNode& o) const { return !(*this == o); }
};

struct XmlParseResult {
    bool ok = false;
    std::string error;  ///< "line N: what" when !ok
    XmlNode root;
};

/// Parses one document with exactly one root element.
XmlParseResult parseXml(const std::string& text);

/// Writes `node` as an indented document fragment (two spaces per level),
/// without an XML declaration. Character data that is whitespace only is
/// not written; other text is written escaped, before the children.
std::string writeXml(const XmlNode& node, int indent = 0);

/// Escapes &, <, >, " and ' for an attribute value or character data.
std::string xmlEscape(const std::string& s);

}  // namespace nd
