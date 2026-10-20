// x-mcp-header annotation walk (private to the library).
// Shared by the HTTP client (mirroring values into headers) and the HTTP
// server (validating that mirrored headers match the body).
//
// Spec constraints (basic/transports/streamable-http.md "Schema Extension"):
// only parameters statically reachable from the schema root via a chain of
// `properties` keys qualify; a chain must not pass through items, composition
// keywords, conditionals or $ref. The walk below simply never descends
// through anything else, which yields exactly the reachable set.
#pragma once

#include <mcp/json.hpp>
#include <mcp/types.hpp>  // mcp::member (direct-member lookup)

#include <set>
#include <string>
#include <utility>
#include <vector>

namespace mcp::detail {

// Renders one annotated argument value as its wire string form.
inline std::string headerParamText(const Json& value) {
  if (value.isString()) return std::string(value.toString());
  if (value.isNumber()) return std::to_string(value.toInt64());
  if (value.isFalse()) return "false";
  if (value.isTrue()) return "true";
  return "";  // unsupported (object/array/number-typed per spec) -> omit
}

// Collects (headerName, renderedValue) pairs for every annotated property
// whose value is present (and non-null) in `args`. Header names are the raw
// `x-mcp-header` values; encoding to `Mcp-Param-{name}` wire form happens at
// the call sites.
inline void collectHeaderParams(const Json& schema, const Json& args,
                                const std::vector<std::string>& path,
                                std::vector<std::pair<std::string, std::string>>& out) {
  if (!schema.isObject() || !args.isObject()) return;
  const Json properties = mcp::member(schema, "properties");
  if (!properties.isObject()) return;
  for (const auto& key : properties.getAllKeys().toVector()) {
    const std::string keyStr(key.toString());
    const Json prop = properties[keyStr];
    const Json annotation = mcp::member(prop, "x-mcp-header");
    if (annotation.isString() && !annotation.toString().empty()) {
      // Read the value at this exact path in the arguments.
      Json node = args;
      for (const auto& seg : path) node = node[seg];
      const Json value = node[keyStr];
      if (value.isError() || value.isNull()) continue;  // absent -> omit header
      // An EMPTY string value is still a value: the mirrored header MUST be
      // sent with an empty payload (the http-custom-headers scenario probes
      // exactly this). Only absent/null arguments omit the header.
      std::string text = headerParamText(value);
      out.emplace_back(std::string(annotation.toString()), std::move(text));
    }
    // Descend through properties chains only.
    std::vector<std::string> nextPath = path;
    nextPath.push_back(keyStr);
    collectHeaderParams(prop, args[keyStr], nextPath, out);
  }
}

// SEP-2243 client-side annotation validation. A tool whose x-mcp-header
// annotations are malformed MUST be excluded by clients:
//   - the annotation must be a non-empty ASCII header name with no spaces,
//     colons or control characters;
//   - only string/integer/boolean properties may be annotated (number,
//     object, array, null are invalid);
//   - header names must be unique case-insensitively within the tool.
// RFC 9110 token characters ("1*tchar"): ALPHA / DIGIT /
// "!#$%&'*+-.^_`|~". The old check only rejected space/CTL/colon, so names
// like "X(1)" or 'X"y' passed though they are not header field names.
inline bool isTChar(char c) {
  const auto u = static_cast<unsigned char>(c);
  if ((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9')) return true;
  switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'': case '*':
    case '+': case '-': case '.': case '^': case '_': case '`': case '|': case '~':
      return true;
    default:
      return false;
  }
}

inline bool headerNameValid(std::string_view name) {
  if (name.empty()) return false;
  for (const char c : name) {
    if (!isTChar(c)) return false;  // space, colon, parentheses, quotes, non-ASCII...
  }
  return true;
}

// Walks a subschema that is NOT a properties chain (items, prefixItems,
// composition, conditionals, patternProperties, $defs, ...). Any annotation
// found there is invalid per the spec - those values are not statically
// reachable through `properties` keys, so they cannot be mirrored.
inline bool noAnnotationsDeep(const Json& node) {
  if (node.isArray()) {
    for (const auto& item : node.toVector()) {
      if (!noAnnotationsDeep(item)) return false;
    }
    return true;
  }
  if (!node.isObject()) return true;
  if (mcp::member(node, "x-mcp-header").isString()) return false;
  // Descend through EVERYTHING (properties included): this subtree is not a
  // properties chain, so any annotation reachable here is invalid.
  for (const auto& key : node.getAllKeys().toVector()) {
    if (!noAnnotationsDeep(node[key.toString()])) return false;
  }
  return true;
}

// Keywords whose value is itself a subschema (or a map/array of them).
inline bool isSchemaBearingKeyword(const std::string& key) {
  static const char* kKeywords[] = {
      "items",       "prefixItems",     "additionalProperties", "patternProperties",
      "properties",  "anyOf",           "oneOf",                "allOf",
      "not",         "if",              "then",                 "else",
      "contains",    "propertyNames",   "dependentSchemas",     "$defs",
      "definitions", "unevaluatedItems", "unevaluatedProperties"};
  for (const char* known : kKeywords) {
    if (key == known) return true;
  }
  return false;
}

inline bool headerAnnotationsValid(const Json& schema, std::set<std::string>& seenNames) {
  if (!schema.isObject()) return true;
  // Valid annotations may appear ONLY on properties reached through the
  // properties chain; anything inside items/composition/conditionals/
  // patternProperties/$defs invalidates the tool.
  for (const auto& key : schema.getAllKeys().toVector()) {
    const std::string k(key.toString());
    if (k == "properties" || k == "x-mcp-header" || !isSchemaBearingKeyword(k)) continue;
    if (k == "properties") continue;
    if (!noAnnotationsDeep(schema[k])) return false;
  }
  const Json properties = mcp::member(schema, "properties");
  if (!properties.isObject()) return true;
  for (const auto& key : properties.getAllKeys().toVector()) {
    const Json prop = properties[key.toString()];
    const Json annotation = mcp::member(prop, "x-mcp-header");
    const Json type = mcp::member(prop, "type");
    const std::string t = type.isString() ? std::string(type.toString()) : std::string();
    if (annotation.isString()) {
      const std::string name(annotation.toString());
      if (name.empty() || !headerNameValid(name)) return false;
      std::string lower;
      lower.reserve(name.size());
      for (const char c : name) {
        lower.push_back(static_cast<char>(
            c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
      }
      if (!seenNames.insert(lower).second) return false;  // duplicate header name
      if (t != "string" && t != "integer" && t != "boolean") return false;
    }
    // Recurse into the property's own properties chain.
    if (!headerAnnotationsValid(prop, seenNames)) return false;
  }
  return true;
}

// Convenience for a tool definition: validates the tool's inputSchema.
inline bool headerAnnotationsValid(const Json& toolSchema) {
  std::set<std::string> seen;
  return headerAnnotationsValid(toolSchema, seen);
}

}  // namespace mcp::detail
