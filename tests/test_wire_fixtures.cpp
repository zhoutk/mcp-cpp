// Golden wire fixtures (E6): the shapes this SDK emits are pinned to
// checked-in JSON fixtures, and the same shapes are validated against small
// spec-schema twins (CacheableResult / InputRequiredResult / JSON-RPC error)
// with the SDK's own validator. A wire change that is not deliberate breaks
// these tests; when a spec change IS deliberate, update the fixture in the
// same commit and the diff records the wire change.

#include "MemTransport.h"

#include <mcp/mcp.hpp>
#include <mcp/schema.hpp>

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef MCP_FIXTURE_DIR
#define MCP_FIXTURE_DIR "tests/fixtures"
#endif

using namespace mcp;
using namespace mcp::server;

namespace {

Json loadJson(const std::string& relativePath) {
  const std::string path = std::string(MCP_FIXTURE_DIR) + "/" + relativePath;
  std::ifstream in(path, std::ios::binary);
  if (!in) return Json(nullptr);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return Json(buffer.str());
}

// Structural equality: key order and whitespace do not matter, but every
// member, array element and scalar value must match.
bool jsonEqual(const Json& a, const Json& b) {
  if (a.isNull() || b.isNull()) return a.isNull() && b.isNull();
  if (a.isObject() && b.isObject()) {
    const auto aKeys = a.getAllKeys().toVector();
    const auto bKeys = b.getAllKeys().toVector();
    if (aKeys.size() != bKeys.size()) return false;
    for (const auto& key : aKeys) {
      const std::string name(key.toString());
      if (!jsonEqual(member(a, name), member(b, name))) return false;
    }
    return true;
  }
  if (a.isArray() && b.isArray()) {
    const auto av = a.toVector();
    const auto bv = b.toVector();
    if (av.size() != bv.size()) return false;
    for (size_t i = 0; i < av.size(); ++i) {
      if (!jsonEqual(av[i], bv[i])) return false;
    }
    return true;
  }
  if (a.isNumber() && b.isNumber()) return a.toDouble() == b.toDouble();
  if (a.isString() && b.isString()) return a.toString() == b.toString();
  if ((a.isTrue() || a.isFalse()) && (b.isTrue() || b.isFalse())) {
    return a.isTrue() == b.isTrue();
  }
  return false;
}

std::string makeRequest(const std::string& id, const std::string& method,
                        const Json& extraParams,
                        const std::string& version = "2026-07-28") {
  Json params = encodeRequestMeta(version, Json(R"({})"), std::nullopt);
  if (extraParams.isObject()) {
    for (const auto& item : extraParams.getAllKeys().toVector()) {
      params.add(item.toString(), extraParams[item.toString()]);
    }
  }
  return encodeRequest(RequestId(id), method, params);
}

std::vector<std::string> serve(const std::shared_ptr<Server>& server,
                               const std::string& line) {
  MemTransport transport({line});
  server->processLine(line, transport);
  return transport.output_;
}

Json parseOne(const std::shared_ptr<Server>& server, const std::string& line) {
  const auto output = serve(server, line);
  EXPECT_EQ(output.size(), 1u);
  if (output.empty()) return Json(nullptr);
  return Json(output[0]);
}

// The result object of a single response line.
Json resultOf(const std::shared_ptr<Server>& server, const std::string& line) {
  const Json message = parseOne(server, line);
  const ParsedMessage parsed = parseMessage(message.toString());
  const auto* result = std::get_if<JsonRpcResult>(&parsed);
  EXPECT_NE(result, nullptr);
  return result != nullptr ? result->result : Json(nullptr);
}

std::shared_ptr<Server> fixtureServer() {
  Builder b;
  b.name("fixture").version("1.0");
  b.simpleTool("echo", "echoes", [](Ctx&) -> McpResult<Json> { return Ok(Json(R"({})")); });
  return b.build();
}

void expectValid(const Json& instance, const std::string& schemaPath) {
  const Json schema = loadJson(schemaPath);
  ASSERT_FALSE(schema.isNull()) << "missing schema fixture: " << schemaPath;
  auto verdict = validateSchema(instance, schema);
  EXPECT_TRUE(verdict.has_value()) << (verdict.has_value() ? "" : verdict.error().message);
}

}  // namespace

TEST(WireFixtures, ToolsListResultMatchesGolden) {
  auto server = fixtureServer();
  const Json actual = resultOf(server, makeRequest("t1", "tools/list", Json(nullptr)));
  const Json expected = loadJson("wire/tools_list_result.json");
  ASSERT_FALSE(expected.isNull());
  EXPECT_TRUE(jsonEqual(actual, expected))
      << "actual:   " << actual.toString() << "\nexpected: " << expected.toString();
  expectValid(actual, "schema/cacheable_result.schema.json");
}

TEST(WireFixtures, ResourcesReadResultMatchesGolden) {
  Builder b;
  b.name("fixture").version("1.0");
  b.resource(Json(R"({"uri":"file:///x","name":"x"})"), [](Ctx&) -> McpResult<Json> {
    Json contents = Json("[]");
    Json item;
    item.add("uri", Json::str("file:///x"));
    item.add("text", Json::str("hi"));
    contents.push_back(std::move(item));
    Json result;
    result.add("contents", std::move(contents));
    return Ok(std::move(result));
  });
  auto server = b.build();

  const Json actual =
      resultOf(server, makeRequest("r1", "resources/read", Json(R"({"uri":"file:///x"})")));
  const Json expected = loadJson("wire/resources_read_result.json");
  ASSERT_FALSE(expected.isNull());
  EXPECT_TRUE(jsonEqual(actual, expected))
      << "actual:   " << actual.toString() << "\nexpected: " << expected.toString();
  expectValid(actual, "schema/cacheable_result.schema.json");
}

TEST(WireFixtures, InputRequiredResultMatchesGolden) {
  Builder b;
  b.name("fixture").version("1.0");
  b.simpleTool("ask", "state-only round", [](Ctx& c) -> McpResult<Json> {
    return c.inputRequired(Json(), "golden-state");
  });
  auto server = b.build();

  const Json actual =
      resultOf(server, makeRequest("a1", "tools/call", Json(R"({"name":"ask"})")));
  const Json expected = loadJson("wire/input_required_result.json");
  ASSERT_FALSE(expected.isNull());
  EXPECT_TRUE(jsonEqual(actual, expected))
      << "actual:   " << actual.toString() << "\nexpected: " << expected.toString();
  expectValid(actual, "schema/input_required_result.schema.json");
}

TEST(WireFixtures, MethodNotFoundErrorMatchesGolden) {
  auto server = fixtureServer();
  const Json actual = parseOne(server, makeRequest("m1", "unknown/method", Json(nullptr)));
  const Json expected = loadJson("wire/error_method_not_found.json");
  ASSERT_FALSE(expected.isNull());
  EXPECT_TRUE(jsonEqual(actual, expected))
      << "actual:   " << actual.toString() << "\nexpected: " << expected.toString();
  expectValid(actual, "schema/jsonrpc_error.schema.json");
}

TEST(WireFixtures, UnsupportedVersionErrorMatchesGolden) {
  auto server = fixtureServer();
  const Json actual =
      parseOne(server, makeRequest("v1", "tools/list", Json(nullptr), "2025-01-01"));
  const Json expected = loadJson("wire/error_unsupported_version.json");
  ASSERT_FALSE(expected.isNull());
  EXPECT_TRUE(jsonEqual(actual, expected))
      << "actual:   " << actual.toString() << "\nexpected: " << expected.toString();
  expectValid(actual, "schema/jsonrpc_error.schema.json");
}

TEST(WireFixtures, MissingCapabilityErrorMatchesGolden) {
  Builder b;
  b.name("fixture").version("1.0");
  b.simpleTool("ask", "elicits", [](Ctx& c) -> McpResult<Json> {
    return c.elicit("continue?", Json(R"({"type":"object"})"), "");
  });
  auto server = b.build();

  const Json actual =
      parseOne(server, makeRequest("e1", "tools/call", Json(R"({"name":"ask"})")));
  const Json expected = loadJson("wire/error_missing_capability.json");
  ASSERT_FALSE(expected.isNull());
  EXPECT_TRUE(jsonEqual(actual, expected))
      << "actual:   " << actual.toString() << "\nexpected: " << expected.toString();
  expectValid(actual, "schema/jsonrpc_error.schema.json");
}

TEST(WireFixtures, SubscriptionAcknowledgmentMatchesGolden) {
  auto server = fixtureServer();
  const Json actual = parseOne(
      server,
      makeRequest("sub1", "subscriptions/listen",
                  Json(R"({"notifications":{"toolsListChanged":true}})")));
  const Json expected = loadJson("wire/subscriptions_ack.json");
  ASSERT_FALSE(expected.isNull());
  EXPECT_TRUE(jsonEqual(actual, expected))
      << "actual:   " << actual.toString() << "\nexpected: " << expected.toString();
}
