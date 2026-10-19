// JSON Schema 2020-12 validator: keyword coverage, $ref, resource bounds.

#include <mcp/mcp.hpp>
#include <mcp/schema.hpp>

#include <gtest/gtest.h>

using namespace mcp;

namespace {
// Always parses - zjson's Json("42") would treat a non-brace text as a
// string value (the sniffing convention); schema literals need real parsing.
Json J(const char* text) {
  std::string err;
  Json parsed = Json::ParseJson(text, err);
  if (parsed.isError()) ADD_FAILURE() << "bad test json: " << text;
  return parsed;
}
}  // namespace

TEST(Schema, TypeKeyword) {
  EXPECT_TRUE(validateSchema(J(R"(42)"), J(R"({"type":"integer"})")).has_value());
  EXPECT_TRUE(validateSchema(J(R"(4.5)"), J(R"({"type":"number"})")).has_value());
  EXPECT_FALSE(validateSchema(J(R"(4.5)"), J(R"({"type":"integer"})")).has_value());
  EXPECT_TRUE(validateSchema(J(R"(42)"), J(R"({"type":"number"})")).has_value());
  EXPECT_TRUE(validateSchema(J(R"("x")"), J(R"({"type":"string"})")).has_value());
  EXPECT_TRUE(validateSchema(J(R"(true)"), J(R"({"type":"boolean"})")).has_value());
  EXPECT_TRUE(validateSchema(J(R"(null)"), J(R"({"type":"null"})")).has_value());
  EXPECT_TRUE(validateSchema(J(R"({})"), J(R"({"type":["string","object"]})")).has_value());
  EXPECT_FALSE(validateSchema(J(R"(1)"), J(R"({"type":["string","object"]})")).has_value());
}

TEST(Schema, EnumAndConst) {
  EXPECT_TRUE(validateSchema(J(R"("a")"), J(R"({"enum":["a","b"]})")).has_value());
  EXPECT_FALSE(validateSchema(J(R"("c")"), J(R"({"enum":["a","b"]})")).has_value());
  EXPECT_TRUE(validateSchema(J(R"(7)"), J(R"({"const":7})")).has_value());
  EXPECT_FALSE(validateSchema(J(R"(8)"), J(R"({"const":7})")).has_value());
  EXPECT_TRUE(
      validateSchema(J(R"({"x":1})"), J(R"({"const":{"x":1}})")).has_value());
}

TEST(Schema, NumericBounds) {
  EXPECT_TRUE(validateSchema(J(R"(5)"), J(R"({"minimum":5})")).has_value());
  EXPECT_FALSE(validateSchema(J(R"(4.9)"), J(R"({"minimum":5})")).has_value());
  EXPECT_FALSE(validateSchema(J(R"(5)"), J(R"({"exclusiveMinimum":5})")).has_value());
  EXPECT_FALSE(validateSchema(J(R"(11)"), J(R"({"maximum":10})")).has_value());
  EXPECT_FALSE(validateSchema(J(R"(10)"), J(R"({"exclusiveMaximum":10})")).has_value());
  EXPECT_TRUE(validateSchema(J(R"(6)"), J(R"({"multipleOf":3})")).has_value());
  EXPECT_FALSE(validateSchema(J(R"(7)"), J(R"({"multipleOf":3})")).has_value());
}

TEST(Schema, StringRules) {
  EXPECT_TRUE(validateSchema(J(R"("abc")"), J(R"({"minLength":3,"maxLength":3})")).has_value());
  EXPECT_FALSE(validateSchema(J(R"("ab")"), J(R"({"minLength":3})")).has_value());
  EXPECT_FALSE(validateSchema(J(R"("abcd")"), J(R"({"maxLength":3})")).has_value());
  EXPECT_TRUE(validateSchema(J(R"("hello world")"), J(R"({"pattern":"world"})")).has_value());
  EXPECT_FALSE(validateSchema(J(R"("hello")"), J(R"({"pattern":"^world"})")).has_value());
}

TEST(Schema, ObjectRules) {
  const Json schema = Json(
      R"({"type":"object","required":["name"],
          "properties":{"name":{"type":"string"},"age":{"type":"integer","minimum":0}},
          "additionalProperties":false})");
  EXPECT_TRUE(validateSchema(J(R"({"name":"t","age":3})"), schema).has_value());
  EXPECT_FALSE(validateSchema(J(R"({"age":3})"), schema).has_value());  // missing required
  EXPECT_FALSE(validateSchema(J(R"({"name":"t","age":-1})"), schema).has_value());
  EXPECT_FALSE(validateSchema(J(R"({"name":"t","extra":1})"), schema).has_value());
  // nested objects
  const Json nested =
      J(R"({"type":"object","properties":{"child":{"type":"object","required":["id"]}}})");
  EXPECT_FALSE(validateSchema(J(R"({"child":{}})"), nested).has_value());
  EXPECT_TRUE(validateSchema(J(R"({"child":{"id":"x"}})"), nested).has_value());
}

TEST(Schema, ArrayRules) {
  EXPECT_TRUE(validateSchema(J(R"([1,2])"), J(R"({"type":"array","minItems":1})")).has_value());
  EXPECT_FALSE(validateSchema(J(R"([])"), J(R"({"minItems":1})")).has_value());
  EXPECT_FALSE(validateSchema(J(R"([1,2,3])"), J(R"({"maxItems":2})")).has_value());
  EXPECT_TRUE(validateSchema(J(R"([1,2])"), J(R"({"uniqueItems":true})")).has_value());
  EXPECT_FALSE(validateSchema(J(R"([1,1])"), J(R"({"uniqueItems":true})")).has_value());
  const Json items = J(R"({"items":{"type":"integer"}})");
  EXPECT_TRUE(validateSchema(J(R"([1,2,3])"), items).has_value());
  EXPECT_FALSE(validateSchema(J(R"([1,"x"])"), items).has_value());
  // tuple form
  const Json tuple = J(R"({"items":[{"type":"string"},{"type":"integer"}]})");
  EXPECT_TRUE(validateSchema(J(R"(["a",1])"), tuple).has_value());
  EXPECT_FALSE(validateSchema(J(R"([1,"a"])"), tuple).has_value());
}

TEST(Schema, Composition) {
  const Json anyOf = J(R"({"anyOf":[{"type":"string"},{"type":"integer"}]})");
  EXPECT_TRUE(validateSchema(J(R"("x")"), anyOf).has_value());
  EXPECT_TRUE(validateSchema(J(R"(3)"), anyOf).has_value());
  EXPECT_FALSE(validateSchema(J(R"(3.5)"), anyOf).has_value());

  const Json oneOf = J(R"({"oneOf":[{"type":"integer"},{"minimum":5}]})");
  EXPECT_TRUE(validateSchema(J(R"(3)"), oneOf).has_value());  // matches first only
  EXPECT_FALSE(validateSchema(J(R"(7)"), oneOf).has_value());  // matches both -> invalid
  EXPECT_FALSE(validateSchema(J(R"(1.5)"), oneOf).has_value());  // matches neither

  const Json notSchema = J(R"({"not":{"type":"string"}})");
  EXPECT_TRUE(validateSchema(J(R"(5)"), notSchema).has_value());
  EXPECT_FALSE(validateSchema(J(R"("s")"), notSchema).has_value());

  const Json allOf = J(R"({"allOf":[{"minimum":0},{"maximum":10}]})");
  EXPECT_TRUE(validateSchema(J(R"(5)"), allOf).has_value());
  EXPECT_FALSE(validateSchema(J(R"(11)"), allOf).has_value());
}

TEST(Schema, LocalRef) {
  const Json schema = Json(
      R"({"$defs":{"name":{"type":"string","minLength":1}},
          "type":"object",
          "required":["first","last"],
          "properties":{"first":{"$ref":"#/$defs/name"},
                        "last":{"$ref":"#/$defs/name"}}})");
  EXPECT_TRUE(validateSchema(J(R"({"first":"a","last":"b"})"), schema).has_value());
  EXPECT_FALSE(validateSchema(J(R"({"first":"","last":"b"})"), schema).has_value());
  EXPECT_FALSE(validateSchema(J(R"({"first":1,"last":"b"})"), schema).has_value());
}

TEST(Schema, NonLocalRefRejected) {
  const Json schema = J(R"({"$ref":"https://example.com/schema.json"})");
  EXPECT_FALSE(validateSchema(J(R"(1)"), schema).has_value());
}

TEST(Schema, BooleanSchemas) {
  EXPECT_TRUE(validateSchema(J(R"(1)"), J(R"(true)")).has_value());
  EXPECT_FALSE(validateSchema(J(R"(1)"), J(R"(false)")).has_value());
}

TEST(Schema, ResourceBounds) {
  // Deeply nested schema must be rejected, not hang.
  std::string deep = "1";
  for (int i = 0; i < 200; ++i) deep = "{\"anyOf\":[{\"type\":\"integer\"}," + deep + "]}";
  const Json schema = Json(deep);
  auto r = validateSchema(J(R"(1)"), schema);
  EXPECT_FALSE(r.has_value());  // depth-bound failure, and it returned at all

  // Bound count: uniqueItems on a huge array with tiny budget.
  SchemaLimits tiny;
  tiny.maxChecks = 10;
  std::string arr = "[";
  for (int i = 0; i < 1000; ++i) arr += (i ? ",1" : "1");
  arr += "]";
  EXPECT_FALSE(validateSchema(Json(arr), J(R"({"uniqueItems":true})"), J(R"(true)"), tiny)
                   .has_value());
}

TEST(Schema, McpToolSchemaShape) {
  // A realistic tool inputSchema like the SDKs emit.
  const Json toolSchema = Json(
      R"({"type":"object",
          "properties":{
            "query":{"type":"string","description":"sql"},
            "limit":{"type":"integer","minimum":1,"maximum":1000}
          },
          "required":["query"]})");
  EXPECT_TRUE(validateSchema(J(R"({"query":"select 1","limit":10})"), toolSchema).has_value());
  EXPECT_FALSE(validateSchema(J(R"({"limit":0})"), toolSchema).has_value());
  EXPECT_FALSE(validateSchema(J(R"({})"), toolSchema).has_value());
  EXPECT_FALSE(validateSchema(J(R"({"query":42})"), toolSchema).has_value());
}

// ---- hardening (CODE-REVIEW-2026-10 B11) ------------------------------------------

// maxChecks must be a GLOBAL ceiling: the anyOf/oneOf/not probes used to each
// start their own counter at 0, so N branches meant N full budgets.
TEST(SchemaHardening, MaxChecksIsGlobalAcrossProbes) {
  // Ten trivial anyOf branches; a 5-check budget cannot cover them all with a
  // shared counter. An instance that matches NOTHING makes every probe run,
  // and the blown budget must surface as a bounds error, not as
  // "matches none of anyOf".
  const Json schema = Json(R"({"anyOf":[
      {"type":"string"},{"type":"string"},{"type":"string"},{"type":"string"},
      {"type":"string"},{"type":"string"},{"type":"string"},{"type":"string"},
      {"type":"string"},{"type":"string"}]})");
  SchemaLimits tiny;
  tiny.maxChecks = 5;
  auto r = validateSchema(J(R"(42)"), schema, schema, tiny);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("resource bounds"), std::string::npos);
}

// 2020-12: $ref is evaluated TOGETHER with its sibling keywords.
TEST(SchemaHardening, RefSiblingsAreEvaluated) {
  const Json schema = Json(
      R"({"$defs":{"str":{"type":"string"}},
          "$ref":"#/$defs/str",
          "maxLength":2})");
  EXPECT_TRUE(validateSchema(J(R"("ab")"), schema).has_value());
  // Sibling maxLength applies: under the old ignore-siblings rule "abc" passed.
  EXPECT_FALSE(validateSchema(J(R"("abc")"), schema).has_value());
}

// 2020-12 tuple form: prefixItems fixes the first N elements, items governs
// everything after them.
TEST(SchemaHardening, PrefixItemsAndTrailingItems) {
  const Json schema = Json(
      R"({"prefixItems":[{"type":"string"},{"type":"integer"}],
          "items":{"type":"boolean"}})");
  EXPECT_TRUE(validateSchema(J(R"(["x", 1, true, false])"), schema).has_value());
  EXPECT_FALSE(validateSchema(J(R"(["x", 1, "nope"])"), schema).has_value());
  EXPECT_FALSE(validateSchema(J(R"([1, 2])"), schema).has_value());
}

// minProperties/maxProperties used Json::size(), which returns -1 for objects
// - both keywords were silently inert.
TEST(SchemaHardening, MinMaxPropertiesCountRealProperties) {
  const Json schema = J(R"({"minProperties":1,"maxProperties":2})");
  EXPECT_FALSE(validateSchema(J(R"({})"), schema).has_value());
  EXPECT_TRUE(validateSchema(J(R"({"a":1})"), schema).has_value());
  EXPECT_FALSE(validateSchema(J(R"({"a":1,"b":2,"c":3})"), schema).has_value());
}

// Property keys are compared as full strings: a key with an embedded NUL used
// to be truncated by c_str() and could spoof the property "a", bypassing
// additionalProperties:false.
TEST(SchemaHardening, NulInPropertyKeyCannotSpoofADeclaredProperty) {
  const Json schema = J(
      R"({"properties":{"a":{"type":"string"}},"additionalProperties":false})");
  Json instance = J(R"({"a":"v"})");
  instance.add(std::string("a\0evil", 6), J(R"("spoof")"));
  EXPECT_FALSE(validateSchema(instance, schema).has_value());
}

// $ref tokens resolve by DIRECT member lookup: an unresolvable token must not
// be found by operator[]'s deep search in unrelated nested data.
TEST(SchemaHardening, RefPointerDoesNotDeepSearch) {
  const Json schema = Json(
      R"({"anyOf":[{"type":"object","properties":{"type":{"type":"string"}}}],
          "$ref":"#/no/such/type"})");
  auto r = validateSchema(J(R"({})"), schema);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("unresolvable $ref token"), std::string::npos);
}

// An overlong $ref array index must fail cleanly, not throw std::out_of_range.
TEST(SchemaHardening, RefArrayIndexOutOfRangeDoesNotThrow) {
  const Json schema = J(R"({"$ref":"#/arr/99999999999999999999"})");
  const Json root = J(R"({"arr":["x"]})");
  EXPECT_FALSE(validateSchema(J(R"("x")"), schema, root, SchemaLimits{}).has_value());
}

// A malformed pattern regex is a schema error, not a crash.
TEST(SchemaHardening, InvalidPatternRegexIsAnError) {
  EXPECT_FALSE(validateSchema(J(R"("x")"), J(R"({"pattern":"(["})")).has_value());
  EXPECT_FALSE(
      validateSchema(J(R"({"k":1})"), J(R"({"patternProperties":{"([":{"type":"string"}}})"))
          .has_value());
}
