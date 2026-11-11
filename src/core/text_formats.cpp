#include "core/text_formats.h"

#include <cstdint>

namespace myalien::text {

namespace {

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

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

// --- KeyValues ---------------------------------------------------------------------------

struct KvReader {
    std::string_view s;
    size_t i = 0;

    // Skips whitespace and // comments.
    void skip() {
        for (;;) {
            while (i < s.size() && isSpace(s[i])) ++i;
            if (i + 1 < s.size() && s[i] == '/' && s[i + 1] == '/') {
                while (i < s.size() && s[i] != '\n') ++i;
                continue;
            }
            return;
        }
    }

    // A quoted or bare token. False at '{', '}' or end of input.
    bool token(std::string& out) {
        skip();
        out.clear();
        if (i >= s.size() || s[i] == '{' || s[i] == '}') return false;
        if (s[i] == '"') {
            ++i;
            while (i < s.size() && s[i] != '"') {
                char c = s[i++];
                if (c == '\\' && i < s.size()) {
                    const char e = s[i++];
                    c = e == 'n' ? '\n' : e == 't' ? '\t' : e;
                }
                out += c;
            }
            if (i < s.size()) ++i;  // closing quote
            return true;
        }
        while (i < s.size() && !isSpace(s[i]) && s[i] != '{' && s[i] != '}' && s[i] != '"') out += s[i++];
        return true;
    }

    void block(KvNode& parent, int depth) {
        std::string key;
        for (;;) {
            skip();
            if (i >= s.size()) return;
            if (s[i] == '}') {
                ++i;
                return;
            }
            if (!token(key)) {  // stray '{'
                ++i;
                continue;
            }
            KvNode node;
            node.key = key;
            skip();
            if (i < s.size() && s[i] == '{') {
                ++i;
                node.block = true;
                if (depth < 32) block(node, depth + 1);
            } else if (!token(node.value)) {
                parent.children.push_back(std::move(node));
                continue;
            }
            parent.children.push_back(std::move(node));
        }
    }
};

// --- JSON --------------------------------------------------------------------------------

struct JsonReader {
    std::string_view s;
    size_t i = 0;

    void ws() {
        while (i < s.size() && isSpace(s[i])) ++i;
    }

    bool hex4(uint32_t& v) {
        if (i + 4 > s.size()) return false;
        v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool string(std::string& out) {
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        out.clear();
        while (i < s.size() && s[i] != '"') {
            const char c = s[i++];
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= s.size()) return false;
            const char e = s[i++];
            switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                        i += 2;
                        uint32_t lo = 0;
                        if (!hex4(lo)) return false;
                        if (lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: out += e; break;  // \" \\ \/
            }
        }
        if (i >= s.size()) return false;
        ++i;
        return true;
    }

    // Skips any value, string-aware. False on malformed input.
    bool skipValue() {
        ws();
        if (i >= s.size()) return false;
        if (s[i] == '"') {
            std::string t;
            return string(t);
        }
        if (s[i] == '{' || s[i] == '[') {
            int depth = 0;
            while (i < s.size()) {
                const char c = s[i];
                if (c == '"') {
                    std::string t;
                    if (!string(t)) return false;
                    continue;
                }
                if (c == '{' || c == '[') ++depth;
                else if (c == '}' || c == ']') --depth;
                ++i;
                if (depth == 0) return true;
            }
            return false;
        }
        while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' && !isSpace(s[i])) ++i;
        return true;
    }
};

}  // namespace

const KvNode* KvNode::find(std::string_view k) const {
    for (const auto& c : children)
        if (iequals(c.key, k)) return &c;
    return nullptr;
}

std::string KvNode::get(std::string_view k) const {
    const KvNode* n = find(k);
    return n && !n->block ? n->value : std::string();
}

KvNode parseKeyValues(std::string_view text) {
    if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
    KvNode root;
    root.block = true;
    KvReader r{text};
    r.block(root, 0);
    return root;
}

std::map<std::string, JsonValue> parseJsonObject(std::string_view text) {
    if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
    std::map<std::string, JsonValue> members;
    JsonReader r{text};
    r.ws();
    if (r.i >= text.size() || text[r.i] != '{') return members;
    ++r.i;
    std::string key;
    for (;;) {
        r.ws();
        if (r.i < text.size() && text[r.i] == '}') break;
        if (!r.string(key)) break;
        r.ws();
        if (r.i >= text.size() || text[r.i] != ':') break;
        ++r.i;
        r.ws();
        JsonValue v;
        if (r.i < text.size() && text[r.i] == '"') {
            v.isString = true;
            if (!r.string(v.text)) break;
        } else {
            const size_t start = r.i;
            if (!r.skipValue()) break;
            v.text = std::string(text.substr(start, r.i - start));
        }
        members[key] = std::move(v);
        r.ws();
        if (r.i < text.size() && text[r.i] == ',') {
            ++r.i;
            continue;
        }
        break;
    }
    return members;
}

std::optional<std::string> yamlScalar(std::string_view text, std::string_view key) {
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(pos, end - pos);
        pos = end + 1;
        if (line.size() <= key.size() || line.substr(0, key.size()) != key || line[key.size()] != ':') continue;
        line.remove_prefix(key.size() + 1);
        while (!line.empty() && isSpace(line.front())) line.remove_prefix(1);
        while (!line.empty() && isSpace(line.back())) line.remove_suffix(1);
        if (line.size() >= 2 && (line.front() == '"' || line.front() == '\'') && line.back() == line.front()) {
            line.remove_prefix(1);
            line.remove_suffix(1);
        }
        return std::string(line);
    }
    return std::nullopt;
}

}  // namespace myalien::text
