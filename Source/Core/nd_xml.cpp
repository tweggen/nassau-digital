#include "nd_xml.h"

#include <cstdint>

namespace nd {

const std::string* XmlNode::attribute(const std::string& key) const {
    for (const auto& a : attributes)
        if (a.first == key) return &a.second;
    return nullptr;
}

namespace {

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

bool allSpace(const std::string& s) {
    for (char c : s)
        if (!isSpace(c)) return false;
    return true;
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && isSpace(s[b])) ++b;
    while (e > b && isSpace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

bool isNameStart(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':' || u >= 0x80;
}

bool isNameChar(char c) {
    return isNameStart(c) || (c >= '0' && c <= '9') || c == '-' || c == '.';
}

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

class Parser {
public:
    explicit Parser(const std::string& s) : s_(s) {}

    XmlParseResult run() {
        XmlParseResult r;
        skipMisc();
        if (failed_) return fail(r);
        if (atEnd() || peek() != '<') {
            error("expected the root element");
            return fail(r);
        }
        if (!parseElement(r.root, 0)) return fail(r);
        skipMisc();
        if (failed_) return fail(r);
        if (!atEnd()) {
            error("content after the root element");
            return fail(r);
        }
        r.ok = true;
        return r;
    }

private:
    const std::string& s_;
    size_t p_ = 0;
    bool failed_ = false;
    std::string err_;

    XmlParseResult& fail(XmlParseResult& r) {
        r.ok = false;
        r.error = err_;
        r.root = XmlNode{};
        return r;
    }

    bool atEnd() const { return p_ >= s_.size(); }
    char peek() const { return s_[p_]; }
    bool startsWith(const char* lit) const { return s_.compare(p_, std::char_traits<char>::length(lit), lit) == 0; }

    int line() const {
        int n = 1;
        for (size_t i = 0; i < p_ && i < s_.size(); ++i)
            if (s_[i] == '\n') ++n;
        return n;
    }

    bool error(const std::string& what) {
        if (!failed_) {
            failed_ = true;
            err_ = "line " + std::to_string(line()) + ": " + what;
        }
        return false;
    }

    void skipSpace() {
        while (!atEnd() && isSpace(peek())) ++p_;
    }

    // Skips until `end` (inclusive); errors if it never comes.
    bool skipPast(const char* end, const char* what) {
        const size_t q = s_.find(end, p_);
        if (q == std::string::npos) return error(std::string("unterminated ") + what);
        p_ = q + std::char_traits<char>::length(end);
        return true;
    }

    // Whitespace, comments, processing instructions (and the XML declaration)
    // outside the root element. A DOCTYPE is refused.
    void skipMisc() {
        if (startsWith("\xEF\xBB\xBF")) p_ += 3;  // UTF-8 byte-order mark
        for (;;) {
            skipSpace();
            if (startsWith("<!--")) {
                if (!skipPast("-->", "comment")) return;
            } else if (startsWith("<?")) {
                if (!skipPast("?>", "processing instruction")) return;
            } else if (startsWith("<!DOCTYPE") || startsWith("<!doctype")) {
                error("a DOCTYPE is not supported");
                return;
            } else {
                return;
            }
        }
    }

    bool parseName(std::string& out) {
        if (atEnd() || !isNameStart(peek())) return error("expected a name");
        const size_t b = p_;
        while (!atEnd() && isNameChar(peek())) ++p_;
        out.assign(s_, b, p_ - b);
        return true;
    }

    // Decodes the entity at p_ (which points at '&') and appends it.
    bool parseEntity(std::string& out) {
        const size_t semi = s_.find(';', p_);
        if (semi == std::string::npos || semi - p_ > 12) return error("unterminated entity");
        const std::string ent = s_.substr(p_ + 1, semi - p_ - 1);
        p_ = semi + 1;
        if (ent == "lt") out += '<';
        else if (ent == "gt") out += '>';
        else if (ent == "amp") out += '&';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else if (ent.size() >= 2 && ent[0] == '#') {
            uint32_t cp = 0;
            const bool hex = ent[1] == 'x' || ent[1] == 'X';
            const size_t start = hex ? 2 : 1;
            if (start >= ent.size()) return error("empty character reference");
            for (size_t i = start; i < ent.size(); ++i) {
                const char c = ent[i];
                uint32_t d;
                if (c >= '0' && c <= '9') d = static_cast<uint32_t>(c - '0');
                else if (hex && c >= 'a' && c <= 'f') d = static_cast<uint32_t>(c - 'a' + 10);
                else if (hex && c >= 'A' && c <= 'F') d = static_cast<uint32_t>(c - 'A' + 10);
                else return error("bad character reference &" + ent + ";");
                cp = cp * (hex ? 16u : 10u) + d;
                if (cp > 0x10FFFF) return error("character reference out of range");
            }
            if (cp == 0) return error("character reference to NUL");
            appendUtf8(out, cp);
        } else {
            return error("unknown entity &" + ent + ";");
        }
        return true;
    }

    bool parseAttributeValue(std::string& out) {
        if (atEnd() || (peek() != '"' && peek() != '\'')) return error("expected a quoted attribute value");
        const char q = peek();
        ++p_;
        while (!atEnd() && peek() != q) {
            const char c = peek();
            if (c == '<') return error("'<' in an attribute value");
            if (c == '&') {
                if (!parseEntity(out)) return false;
            } else {
                out += c;
                ++p_;
            }
        }
        if (atEnd()) return error("unterminated attribute value");
        ++p_;
        return true;
    }

    bool parseElement(XmlNode& node, int depth) {
        if (depth > 256) return error("elements nested too deeply");
        ++p_;  // '<'
        if (!parseName(node.name)) return false;
        for (;;) {
            const size_t before = p_;
            skipSpace();
            if (atEnd()) return error("unterminated start tag <" + node.name + ">");
            if (peek() == '/') {
                ++p_;
                if (atEnd() || peek() != '>') return error("expected '>' after '/'");
                ++p_;
                return true;
            }
            if (peek() == '>') {
                ++p_;
                break;
            }
            if (p_ == before) return error("expected whitespace before an attribute");
            std::string key, value;
            if (!parseName(key)) return false;
            skipSpace();
            if (atEnd() || peek() != '=') return error("expected '=' after attribute " + key);
            ++p_;
            skipSpace();
            if (!parseAttributeValue(value)) return false;
            if (node.attribute(key)) return error("duplicate attribute " + key);
            node.attributes.emplace_back(std::move(key), std::move(value));
        }
        // Content.
        for (;;) {
            if (atEnd()) return error("missing </" + node.name + ">");
            if (startsWith("</")) {
                p_ += 2;
                std::string close;
                if (!parseName(close)) return false;
                if (close != node.name) return error("</" + close + "> closes <" + node.name + ">");
                skipSpace();
                if (atEnd() || peek() != '>') return error("expected '>' in end tag");
                ++p_;
                // Whitespace between child elements is layout, not content:
                // whitespace-only text is dropped, and the text of an element
                // that also has child elements (mixed content) is trimmed at
                // both ends. That makes parse(write(x)) == x for any x.
                if (allSpace(node.text)) node.text.clear();
                else if (!node.children.empty()) node.text = trim(node.text);
                return true;
            }
            if (startsWith("<!--")) {
                if (!skipPast("-->", "comment")) return false;
            } else if (startsWith("<![CDATA[")) {
                p_ += 9;
                const size_t q = s_.find("]]>", p_);
                if (q == std::string::npos) return error("unterminated CDATA section");
                node.text.append(s_, p_, q - p_);
                p_ = q + 3;
            } else if (startsWith("<?")) {
                if (!skipPast("?>", "processing instruction")) return false;
            } else if (startsWith("<!")) {
                return error("unsupported markup declaration");
            } else if (peek() == '<') {
                node.children.emplace_back();
                if (!parseElement(node.children.back(), depth + 1)) return false;
            } else if (peek() == '&') {
                if (!parseEntity(node.text)) return false;
            } else {
                node.text += peek();
                ++p_;
            }
        }
    }
};

void writeNode(std::string& out, const XmlNode& n, int indent) {
    out.append(static_cast<size_t>(indent) * 2, ' ');
    out += '<';
    out += n.name;
    for (const auto& a : n.attributes) {
        out += ' ';
        out += a.first;
        out += "=\"";
        out += xmlEscape(a.second);
        out += '"';
    }
    const bool hasText = !n.text.empty() && !allSpace(n.text);
    if (n.children.empty() && !hasText) {
        out += "/>\n";
        return;
    }
    out += '>';
    if (hasText) out += xmlEscape(n.text);
    if (!n.children.empty()) {
        out += '\n';
        for (const auto& c : n.children) writeNode(out, c, indent + 1);
        out.append(static_cast<size_t>(indent) * 2, ' ');
    }
    out += "</";
    out += n.name;
    out += ">\n";
}

}  // namespace

XmlParseResult parseXml(const std::string& text) { return Parser(text).run(); }

std::string xmlEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            case '\n': out += "&#10;"; break;
            case '\r': out += "&#13;"; break;
            case '\t': out += "&#9;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string writeXml(const XmlNode& node, int indent) {
    std::string out;
    writeNode(out, node, indent);
    return out;
}

}  // namespace nd
