#pragma once

// Minimal readers for the launcher files game mode needs. They take UTF-8 text and never
// throw: malformed input gives an empty or partial result.

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace myalien::text {

// Valve KeyValues (Steam .vdf / .acf): "key" "value" pairs and "key" { ... } blocks.
struct KvNode {
    std::string key;
    std::string value;  // empty for a block
    std::vector<KvNode> children;
    bool block = false;

    // First child with this key (ASCII case-insensitive), or nullptr.
    const KvNode* find(std::string_view k) const;
    // Value of a direct child, or "" when missing or a block.
    std::string get(std::string_view k) const;
};

// Root node holding the top-level entries (for an .acf: one "AppState" block).
KvNode parseKeyValues(std::string_view text);

// One JSON value at the top level of an object: strings are decoded, everything else
// (numbers, true/false/null, nested objects and arrays) is kept as raw JSON text.
struct JsonValue {
    bool isString = false;
    std::string text;
};

// Members of a top-level JSON object. Empty when the text is not an object.
std::map<std::string, JsonValue> parseJsonObject(std::string_view text);

// "key: value" from a flat YAML file (quotes removed), or nullopt. Only top-level scalar keys.
std::optional<std::string> yamlScalar(std::string_view text, std::string_view key);

}  // namespace myalien::text
