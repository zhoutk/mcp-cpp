// Server runtime: stateless dispatch, meta/version/capability gates,
// tools/resources/prompts registries, cancellation, pagination.

#include "MemTransport.h"

#include <mcp/mcp.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <variant>

using namespace mcp;
using namespace mcp::server;

namespace {

struct Out {
  ParsedMessage parsed;
  const JsonRpcResult* result = nullptr;
  const JsonRpcErrorResponse* error = nullptr;
  const JsonRpcNotification* notification = nullptr;
};

// Drives one request through the server and decodes the (single) response.
Out serveOnce(const std::shared_ptr<Server>& server, const std::string& requestLine) {
  MemTransport t({requestLine});
  server->processLine(requestLine, t);
  EXPECT_EQ(t.output_.size(), 1u);
  Out o;
  if (t.output_.empty()) return o;
  o.parsed = parseMessage(t.output_[0]);
  o.result = std::get_if<JsonRpcResult>(&o.parsed);
  o.error = std::get_if<JsonRpcErrorResponse>(&o.parsed);
  o.notification = std::get_if<JsonRpcNotification>(&o.parsed);
  return o;
}

// Builds a modern request line with the required _meta.
std::string makeRequest(const std::string& id, const std::string& method,
                        const Json& extraParams, const std::string& version = "2026-07-28") {
  Json params = encodeRequestMeta(version, Json(R"({})"), std::nullopt);
  if (extraParams.isObject()) {
    for (const auto& item : extraParams.getAllKeys().toVector()) {
      params.add(item.toString(), extraParams[item.toString()]);
    }
  }
  return encodeRequest(RequestId(id), method, params);
}

std::shared_ptr<Server> makeToolServer() {
  Builder b;
  b.name("test").version("1.0");
  b.simpleTool("echo", "echoes", [](Ctx& c) -> McpResult<Json> {
    Json result;
    result.add("echoed", Json::str(c.params["arguments"]["text"].toString()));
    return Ok(std::move(result));
  });
  b.simpleTool("fail", "always errors", [](Ctx&) -> McpResult<Json> {
    return Err(McpErrorCode::InvalidParams, "intentional failure");
  });
  b.simpleTool("longrun", "checks cancellation", [](Ctx& c) -> McpResult<Json> {
    if (c.isCancelled()) {
      return Err(McpErrorCode::InternalError, "cancelled");
    }
    Json result;
    result.add("done", Json(true));
    return Ok(std::move(result));
  });
  return b.build();
}

}  // namespace

// ---- discover --------------------------------------------------------------------

TEST(ServerDiscover, ReturnsVersionsCapabilitiesAndCacheHints) {
  Builder b;
  b.name("disc").version("9.9").instructions("howto").discoverTtlMs(5000)
      .supports(ProtocolVersion::V2025_11_25).supports(ProtocolVersion::Latest);
  auto server = b.build();

  Out o = serveOnce(server, makeRequest("d1", "server/discover", Json(nullptr)));
  ASSERT_NE(o.result, nullptr);
  const Json& r = o.result->result;
  EXPECT_EQ(r["resultType"].toString(), "complete");

  const Json versions = r["supportedVersions"];
  ASSERT_TRUE(versions.isArray());
  ASSERT_EQ(versions.size(), 2);
  EXPECT_EQ(versions[0].toString(), "2025-11-25");  // enum order, not call order
  EXPECT_EQ(versions[1].toString(), "2026-07-28");

  EXPECT_TRUE(r["capabilities"].isObject());
  EXPECT_EQ(r["ttlMs"].toInt64(), 5000);
  EXPECT_EQ(r["cacheScope"].toString(), "public");
  EXPECT_EQ(r["instructions"].toString(), "howto");
  EXPECT_EQ(r["_meta"]["io.modelcontextprotocol/serverInfo"]["name"].toString(), "disc");
}

// ---- meta / version gates ----------------------------------------------------------

TEST(ServerGates, MissingMetaIsInvalidParams) {
  auto server = makeToolServer();
  const std::string line =
      encodeRequest(RequestId(std::string("m1")), "tools/list", Json(nullptr));
  Out o = serveOnce(server, line);
  ASSERT_NE(o.error, nullptr);
  EXPECT_EQ(o.error->error.code, -32602);
}

TEST(ServerGates, UnsupportedVersionListsSupported) {
  auto server = makeToolServer();
  Out o = serveOnce(server, makeRequest("v1", "tools/list", Json(nullptr), "2025-03-26"));
  ASSERT_NE(o.error, nullptr);
  EXPECT_EQ(o.error->error.code, -32022);
  const Json supported = o.error->error.data["supported"];
  ASSERT_TRUE(supported.isArray());
  EXPECT_EQ(supported[0].toString(), "2026-07-28");
  EXPECT_EQ(o.error->error.data["requested"].toString(), "2025-03-26");
}

TEST(ServerGates, UnsupportedVersionAcceptedWhenConfigured) {
  Builder b;
  b.supports(ProtocolVersion::V2025_11_25).supports(ProtocolVersion::Latest);
  b.simpleTool("t", "", [](Ctx&) -> McpResult<Json> { return Ok(Json(R"({"ok":true})")); });
  auto server = b.build();

  Out o = serveOnce(server, makeRequest("v2", "tools/call",
                                        Json(R"({"name":"t"})"), "2025-11-25"));
  ASSERT_NE(o.result, nullptr);
  EXPECT_EQ(o.result->result["ok"].isTrue(), true);
}

TEST(ServerGates, UnknownMethodIsMethodNotFound) {
  auto server = makeToolServer();
  Out o = serveOnce(server, makeRequest("u1", "sampling/create", Json(nullptr)));
  ASSERT_NE(o.error, nullptr);
  EXPECT_EQ(o.error->error.code, -32601);
}

TEST(ServerGates, UnknownNotificationIsSilentlyIgnored) {
  auto server = makeToolServer();
  MemTransport t({});
  server->processLine(encodeNotification("notifications/whatever", Json(nullptr)), t);
  EXPECT_TRUE(t.output_.empty());
}

TEST(ServerGates, InvalidMessageGetsParseErrorWithoutId) {
  auto server = makeToolServer();
  MemTransport t({});
  server->processLine("not json", t);
  ASSERT_EQ(t.output_.size(), 1u);
  ParsedMessage parsed = parseMessage(t.output_[0]);
  const auto* err = std::get_if<JsonRpcErrorResponse>(&parsed);
  ASSERT_NE(err, nullptr);
  EXPECT_EQ(err->error.code, -32700);
  EXPECT_FALSE(err->id.has_value());
}

// ---- tools --------------------------------------------------------------------------

TEST(ServerTools, ListAndCall) {
  auto server = makeToolServer();
  Out list = serveOnce(server, makeRequest("l1", "tools/list", Json(nullptr)));
  ASSERT_NE(list.result, nullptr);
  const Json tools = list.result->result["tools"];
  ASSERT_TRUE(tools.isArray());
  EXPECT_EQ(tools.size(), 3);  // echo, fail, longrun
  EXPECT_EQ(list.result->result["resultType"].toString(), "complete");

  Out call = serveOnce(server, makeRequest(
                                 "c1", "tools/call",
                                 Json(R"({"name":"echo","arguments":{"text":"hi"}})")));
  ASSERT_NE(call.result, nullptr);
  EXPECT_EQ(call.result->result["echoed"].toString(), "hi");
}

TEST(ServerTools, UnknownToolIsInvalidParams) {
  auto server = makeToolServer();
  Out o = serveOnce(server, makeRequest("c2", "tools/call", Json(R"({"name":"nope"})")));
  ASSERT_NE(o.error, nullptr);
  EXPECT_EQ(o.error->error.code, -32602);
}

TEST(ServerTools, CallWithoutAdvertisedCapabilityIsMethodNotFound) {
  // No tools registered -> tools capability never advertised.
  Builder b;
  auto server = b.build();
  Out o = serveOnce(server, makeRequest("c3", "tools/call", Json(R"({"name":"t"})")));
  ASSERT_NE(o.error, nullptr);
  EXPECT_EQ(o.error->error.code, -32601);
}

TEST(ServerTools, HandlerResultGetsResultTypeInjected) {
  auto server = makeToolServer();
  Out call = serveOnce(server, makeRequest("c4", "tools/call",
                                           Json(R"({"name":"echo","arguments":{"text":"x"}})")));
  ASSERT_NE(call.result, nullptr);
  EXPECT_EQ(call.result->result["resultType"].toString(), "complete");
}

TEST(ServerTools, HandlerErrorBecomesErrorResponse) {
  auto server = makeToolServer();
  Out call = serveOnce(server, makeRequest("c5", "tools/call", Json(R"({"name":"fail"})")));
  ASSERT_NE(call.error, nullptr);
  EXPECT_EQ(call.error->error.code, -32602);  // handler's own InvalidParams
  EXPECT_EQ(call.error->error.message, "intentional failure");
}

// ---- resources / prompts -------------------------------------------------------------

TEST(ServerRegistrys, ResourcesAndPrompts) {
  Builder b;
  b.resource(Json(R"({"uri":"file:///etc/motd","name":"motd"})"),
             [](Ctx&) -> McpResult<Json> {
               Json result;
               Json contents = Json("[]");
               Json item;
               item.add("uri", Json::str("file:///etc/motd"));
               item.add("text", Json::str("hello"));
               contents.push_back(std::move(item));
               result.add("contents", std::move(contents));
               return Ok(std::move(result));
             });
  b.prompt(Json(R"({"name":"greet","description":"greeting"})"),
           [](Ctx&) -> McpResult<Json> {
             Json result;
             Json messages = Json("[]");
             Json msg;
             msg.add("role", Json::str("user"));
             Json content;
             content.add("type", Json::str("text"));
             content.add("text", Json::str("hi there"));
             msg.add("content", std::move(content));
             messages.push_back(std::move(msg));
             result.add("messages", std::move(messages));
             return Ok(std::move(result));
           });
  auto server = b.build();

  Out rl = serveOnce(server, makeRequest("r1", "resources/list", Json(nullptr)));
  ASSERT_NE(rl.result, nullptr);
  EXPECT_EQ(rl.result->result["resources"].size(), 1);

  Out rr = serveOnce(server, makeRequest("r2", "resources/read",
                                         Json(R"({"uri":"file:///etc/motd"})")));
  ASSERT_NE(rr.result, nullptr);
  EXPECT_EQ(rr.result->result["contents"][0]["text"].toString(), "hello");
  // CacheableResult is required on resources/read by 2026-07-28, so a handler
  // that omitted the hints still gets the conformant defaults injected.
  EXPECT_EQ(rr.result->result["ttlMs"].toInt64(), 0);
  EXPECT_EQ(rr.result->result["cacheScope"].toString(), "public");

  Out pl = serveOnce(server, makeRequest("p1", "prompts/list", Json(nullptr)));
  ASSERT_NE(pl.result, nullptr);
  EXPECT_EQ(pl.result->result["prompts"].size(), 1);

  Out pg = serveOnce(server, makeRequest("p2", "prompts/get",
                                         Json(R"({"name":"greet"})")));
  ASSERT_NE(pg.result, nullptr);
  EXPECT_EQ(pg.result->result["messages"][0]["content"]["text"].toString(), "hi there");

  // Unknown resource/prompt are InvalidParams.
  Out bad = serveOnce(server, makeRequest("r3", "resources/read",
                                          Json(R"({"uri":"file:///nope"})")));
  ASSERT_NE(bad.error, nullptr);
  EXPECT_EQ(bad.error->error.code, -32602);
}

TEST(ServerRegistrys, ResourceTemplates) {
  Builder b;
  auto echo = [](Ctx& c) -> McpResult<Json> {
    Json result;
    Json contents = Json("[]");
    Json item;
    item.add("uri", member(c.params, "uri"));
    item.add("text", Json::str("served"));
    contents.push_back(std::move(item));
    result.add("contents", std::move(contents));
    return Ok(std::move(result));
  };
  // Exact registration + two templates of different specificity.
  b.resource(Json(R"({"uri":"db://schema","name":"overview"})"), echo);
  b.resourceTemplate(
      Json(R"({"uriTemplate":"db://schema/{db}","name":"tables of a db"})"), echo);
  b.resourceTemplate(
      Json(R"({"uriTemplate":"db://schema/{db}/{table}","name":"table schema"})"),
      echo);
  auto server = b.build();

  // Templates are listed separately from concrete resources.
  Out tl = serveOnce(server, makeRequest("t1", "resources/templates/list", Json(nullptr)));
  ASSERT_NE(tl.result, nullptr);
  ASSERT_EQ(tl.result->result["resourceTemplates"].size(), 2);
  Out rl = serveOnce(server, makeRequest("t2", "resources/list", Json(nullptr)));
  ASSERT_NE(rl.result, nullptr);
  EXPECT_EQ(rl.result->result["resources"].size(), 1);  // only the exact one

  // A URI that matches a template is served even though it was never
  // registered - the point of templates (runtime-created tables, etc.).
  Out two = serveOnce(server, makeRequest("t3", "resources/read",
                                          Json(R"({"uri":"db://schema/sqlite3/t_new"})")));
  ASSERT_NE(two.result, nullptr) << (two.error ? two.error->error.message : "");
  EXPECT_EQ(two.result->result["contents"][0]["text"].toString(), "served");

  Out one = serveOnce(server, makeRequest("t4", "resources/read",
                                          Json(R"({"uri":"db://schema/jsonfile"})")));
  ASSERT_NE(one.result, nullptr);

  // Exact match beats the template that would also match.
  Out exact = serveOnce(server, makeRequest("t5", "resources/read",
                                            Json(R"({"uri":"db://schema"})")));
  ASSERT_NE(exact.result, nullptr);

  // A URI no template matches stays InvalidParams.
  Out bad = serveOnce(server, makeRequest("t6", "resources/read",
                                          Json(R"({"uri":"db://other/x/y"})")));
  ASSERT_NE(bad.error, nullptr);
  EXPECT_EQ(bad.error->error.code, -32602);

  // Templates inherit the CacheableResult hints like concrete resources.
  EXPECT_EQ(two.result->result["ttlMs"].toInt64(), 0);
  EXPECT_EQ(two.result->result["cacheScope"].toString(), "public");
}

TEST(ServerRegistrys, UriTemplateMatching) {
  std::vector<std::pair<std::string, std::string>> vars;
  EXPECT_TRUE(matchUriTemplate("db://schema/{db}/{table}", "db://schema/sqlite3/t1", &vars));
  ASSERT_EQ(vars.size(), 2u);
  EXPECT_EQ(vars[0].first, "db");
  EXPECT_EQ(vars[0].second, "sqlite3");
  EXPECT_EQ(vars[1].second, "t1");

  // {var} never spans '/' or matches empty; {+var} may span '/'.
  EXPECT_FALSE(matchUriTemplate("db://schema/{db}", "db://schema/a/b", nullptr));
  EXPECT_FALSE(matchUriTemplate("db://schema/{db}", "db://schema/", nullptr));
  EXPECT_TRUE(matchUriTemplate("db://schema/{db}", "db://schema/a", nullptr));
  EXPECT_TRUE(matchUriTemplate("file:///{+path}", "file:///a/b/c.txt", nullptr));
  EXPECT_FALSE(matchUriTemplate("db://schema/{db}", "db://other/a", nullptr));
  EXPECT_FALSE(matchUriTemplate("db://schema/{db}", "db://schema/a/b", nullptr));
  // Trailing literal must be consumed exactly.
  EXPECT_FALSE(matchUriTemplate("db://x/{a}.json", "db://x/y.txt", nullptr));
  EXPECT_TRUE(matchUriTemplate("db://x/{a}.json", "db://x/y.json", nullptr));
}

// ---- cancellation ----------------------------------------------------------------------

TEST(ServerCancellation, CancelledBeforeRequestIsObserved) {
  auto server = makeToolServer();
  // notifications/cancelled names id "cancel-1" before the matching request.
  Json cancelParams;
  cancelParams.add("requestId", Json::str("cancel-1"));
  MemTransport t({});
  server->processLine(encodeNotification("notifications/cancelled", cancelParams), t);
  EXPECT_TRUE(t.output_.empty());  // never answered

  Out o = serveOnce(server, makeRequest("cancel-1", "tools/call",
                                        Json(R"({"name":"longrun"})")));
  ASSERT_NE(o.error, nullptr);
  EXPECT_EQ(o.error->error.message, "cancelled");
}

// ---- pagination ---------------------------------------------------------------------------

TEST(ServerPagination, CursorPages) {
  Builder b;
  b.pageSize(1);
  for (const char* n : {"t1", "t2", "t3"}) {
    b.simpleTool(n, "", [](Ctx&) -> McpResult<Json> { return Ok(Json(R"({})")); });
  }
  auto server = b.build();

  Out p1 = serveOnce(server, makeRequest("pg1", "tools/list", Json(nullptr)));
  ASSERT_NE(p1.result, nullptr);
  EXPECT_EQ(p1.result->result["tools"].size(), 1);
  EXPECT_EQ(p1.result->result["nextCursor"].toString(), "1");

  Out p2 = serveOnce(server, makeRequest("pg2", "tools/list",
                                         Json(R"({"cursor":"1"})")));
  ASSERT_NE(p2.result, nullptr);
  EXPECT_EQ(p2.result->result["tools"][0]["name"].toString(), "t2");
  EXPECT_EQ(p2.result->result["nextCursor"].toString(), "2");

  Out p3 = serveOnce(server, makeRequest("pg3", "tools/list",
                                         Json(R"({"cursor":"2"})")));
  ASSERT_NE(p3.result, nullptr);
  EXPECT_EQ(p3.result->result["tools"][0]["name"].toString(), "t3");
  EXPECT_TRUE(p3.result->result["nextCursor"].isError());  // absent

  Out bad = serveOnce(server, makeRequest("pg4", "tools/list",
                                          Json(R"({"cursor":"x"})")));
  ASSERT_NE(bad.error, nullptr);
  EXPECT_EQ(bad.error->error.code, -32602);
}

// ---- run loop ---------------------------------------------------------------------------------

TEST(ServerRun, ProcessesUntilEof) {
  auto server = makeToolServer();
  std::vector<std::string> in = {
      makeRequest("1", "server/discover", Json(nullptr)),
      encodeNotification("notifications/whatever", Json(nullptr)),  // no reply
      makeRequest("2", "tools/list", Json(nullptr)),
  };
  MemTransport t(in);
  const McpResult<void> r = server->run(t);
  ASSERT_TRUE(r.has_value());          // EOF is graceful
  EXPECT_EQ(t.output_.size(), 2u);     // notification produced no output
}

// ---- M4: inputSchema enforcement, completions, CacheableResult --------------------

TEST(ServerM4, ToolInputSchemaValidatesArguments) {
  Builder b;
  Json schema = Json(
      R"({"type":"object","required":["query"],
          "properties":{"query":{"type":"string"},"limit":{"type":"integer","minimum":1}}})");
  Json def;
  def.add("name", Json::str("search"));
  def.add("description", Json::str("searches"));
  def.add("inputSchema", std::move(schema));
  b.tool(std::move(def), [](Ctx&) -> McpResult<Json> { return Ok(Json(R"({"found":true})")); });
  auto server = b.build();

  Out good = serveOnce(server, makeRequest("s1", "tools/call",
                                           Json(R"({"name":"search","arguments":{"query":"q","limit":5}})")));
  ASSERT_NE(good.result, nullptr);

  Out missing = serveOnce(server, makeRequest("s2", "tools/call",
                                              Json(R"({"name":"search","arguments":{}})")));
  ASSERT_NE(missing.error, nullptr);
  EXPECT_EQ(missing.error->error.code, -32602);

  Out negative = serveOnce(server, makeRequest("s3", "tools/call",
                                               Json(R"({"name":"search","arguments":{"query":"q","limit":0}})")));
  ASSERT_NE(negative.error, nullptr);
  EXPECT_EQ(negative.error->error.code, -32602);
}

// Regression: protocol reads must be direct-member lookups. With the top-level
// `arguments` absent, a nested `arguments` key (here inside inputResponses)
// must NOT be picked up as the tool's arguments.
TEST(ServerM4, ArgumentsAreDirectMemberOnly) {
  Builder b;
  Json def = Json(R"({"name":"strict","description":"requires text",
      "inputSchema":{"type":"object","required":["text"],
                     "properties":{"text":{"type":"string"}}}})");
  b.tool(std::move(def), [](Ctx&) -> McpResult<Json> { return Ok(Json(R"({"ok":true})")); });
  auto server = b.build();

  Out nested = serveOnce(server, makeRequest("n1", "tools/call",
      Json(R"({"name":"strict","inputResponses":{"x":{"arguments":{"text":"nested"}}}})")));
  ASSERT_NE(nested.error, nullptr);
  EXPECT_EQ(nested.error->error.code, -32602);
}

// ---- MRTR requestState -----------------------------------------------------------

namespace {

Json statefulDef(const std::string& name) {
  Json def;
  def.add("name", Json::str(name));
  def.add("description", Json::str("two-round MRTR"));
  Json schema(R"({"type":"object"})");
  def.add("inputSchema", std::move(schema));
  return def;
}

// Round one (no echoed state) seals "step1" and asks for a state-only retry;
// round two verifies the echoed state and returns the payload it carries.
McpResult<Json> statefulRound(Ctx& c) {
  auto opened = c.openRequestState();
  if (!opened.has_value()) return Err(opened.error());
  if (!opened.value().has_value()) {
    auto state = c.sealRequestState("step1");
    if (!state.has_value()) return Err(state.error());
    return c.inputRequired(Json(), state.value());
  }
  Json r;
  r.add("payload", Json::str(*opened.value()));
  return Ok(std::move(r));
}

// Reports whether openRequestState succeeded and, if not, the error code.
McpResult<Json> probeRound(Ctx& c) {
  auto opened = c.openRequestState();
  Json r;
  r.add("opened", Json::str(opened.has_value() ? "yes" : "no"));
  if (!opened.has_value()) {
    r.add("code", Json::str(std::to_string(static_cast<int>(opened.error().code))));
  }
  return Ok(std::move(r));
}

std::shared_ptr<Server> makeStatefulServer(bool withCodec) {
  Builder b;
  b.name("state").version("1.0");
  if (withCodec) {
    auto codec = RequestStateCodec::create(std::string(32, 'k'));
    EXPECT_TRUE(codec.has_value());
    b.requestStateCodec(std::move(codec).value());
  }
  b.tool(statefulDef("stateful"), statefulRound);
  b.tool(statefulDef("other"), statefulRound);
  b.tool(statefulDef("probe"), probeRound);
  return b.build();
}

Out callTool(const std::shared_ptr<Server>& server, const std::string& id,
             const std::string& tool, const std::string& state = {}) {
  Json extra;
  extra.add("name", Json::str(tool));
  extra.add("arguments", Json(R"({})"));
  if (!state.empty()) extra.add("requestState", Json::str(state));
  return serveOnce(server, makeRequest(id, "tools/call", extra));
}

}  // namespace

TEST(ServerRequestState, RoundTripAcrossRounds) {
  auto server = makeStatefulServer(true);
  Out round1 = callTool(server, "q1", "stateful");
  ASSERT_NE(round1.result, nullptr);
  const std::string state = requestStateOf(round1.result->result);
  ASSERT_FALSE(state.empty());

  Out round2 = callTool(server, "q2", "stateful", state);
  ASSERT_NE(round2.result, nullptr);
  EXPECT_EQ(std::string(member(round2.result->result, "payload").toString()), "step1");
}

TEST(ServerRequestState, TamperedStateRejectedWithGenericError) {
  auto server = makeStatefulServer(true);
  Out round1 = callTool(server, "q1", "stateful");
  ASSERT_NE(round1.result, nullptr);
  std::string tampered = requestStateOf(round1.result->result);
  ASSERT_GT(tampered.size(), 10u);
  tampered[10] = tampered[10] == 'A' ? 'B' : 'A';

  Out bad = callTool(server, "q2", "stateful", tampered);
  ASSERT_NE(bad.error, nullptr);
  EXPECT_EQ(bad.error->error.code, -32602);
  // The cause (MAC mismatch, expiry, context) must not leak to the client.
  EXPECT_EQ(bad.error->error.message.find("rejected"), std::string::npos);
}

TEST(ServerRequestState, StateBoundToToolNameCannotBeReplayed) {
  auto server = makeStatefulServer(true);
  Out round1 = callTool(server, "q1", "stateful");
  ASSERT_NE(round1.result, nullptr);
  const std::string state = requestStateOf(round1.result->result);

  Out cross = callTool(server, "q2", "other", state);
  ASSERT_NE(cross.error, nullptr);
  EXPECT_EQ(cross.error->error.code, -32602);
}

TEST(ServerRequestState, ProbeSeesNoEchoAsNullopt) {
  auto server = makeStatefulServer(true);
  Out probe = callTool(server, "p1", "probe");
  ASSERT_NE(probe.result, nullptr);
  EXPECT_EQ(std::string(member(probe.result->result, "opened").toString()), "yes");
}

TEST(ServerRequestState, NoCodecConfiguredIsInternalError) {
  auto server = makeStatefulServer(false);
  Out probe = callTool(server, "p1", "probe");
  ASSERT_NE(probe.result, nullptr);
  EXPECT_EQ(std::string(member(probe.result->result, "opened").toString()), "no");
  EXPECT_EQ(std::string(member(probe.result->result, "code").toString()), "-32603");
}

TEST(ServerM4, CompletionRoute) {
  Builder b;
  b.completions([](Ctx&) -> McpResult<Json> {
    Json result;
    Json completion;
    Json values = Json("[]");
    values.push_back(Json::str("option-1"));
    completion.add("values", std::move(values));
    completion.add("hasMore", Json(false));
    result.add("completion", std::move(completion));
    return Ok(std::move(result));
  });
  auto server = b.build();

  Out o = serveOnce(server, makeRequest("cm1", "completion/complete",
                                        Json(R"({"ref":{"type":"ref/prompt","name":"greet"},
                                              "argument":{"name":"lang","value":"zh"}})")));
  ASSERT_NE(o.result, nullptr);
  EXPECT_EQ(o.result->result["completion"]["values"].size(), 1);

  // capability advertised on discover
  Out d = serveOnce(server, makeRequest("cm2", "server/discover", Json(nullptr)));
  ASSERT_NE(d.result, nullptr);
  EXPECT_TRUE(capabilitiesInclude(d.result->result["capabilities"], "completions"));
}

TEST(ServerM4, ListCacheHints) {
  Builder b;
  b.listCache(60000, "private");
  b.simpleTool("t", "", [](Ctx&) -> McpResult<Json> { return Ok(Json(R"({})")); });
  auto server = b.build();

  Out o = serveOnce(server, makeRequest("h1", "tools/list", Json(nullptr)));
  ASSERT_NE(o.result, nullptr);
  EXPECT_EQ(o.result->result["ttlMs"].toInt64(), 60000);
  EXPECT_EQ(o.result->result["cacheScope"].toString(), "private");

  // default: CacheableResult is REQUIRED by 2026-07-28, so a plain builder
  // emits ttlMs 0 / public rather than omitting the fields (still needs the
  // tools capability).
  Builder plain;
  plain.simpleTool("t2", "", [](Ctx&) -> McpResult<Json> { return Ok(Json(R"({})")); });
  auto pserver = plain.build();
  Out d = serveOnce(pserver, makeRequest("h2", "tools/list", Json(nullptr)));
  ASSERT_NE(d.result, nullptr);
  EXPECT_EQ(d.result->result["ttlMs"].toInt64(), 0);
  EXPECT_EQ(d.result->result["cacheScope"].toString(), "public");

  // Same for resources/read, whose hints the server injects for the handler.
  // (A fresh Builder: build() moves the internal Server, so a builder must
  // not be reused for a second build - the moved-from cacheScope would be empty.)
  Builder plain2;
  plain2.simpleTool("t2", "", [](Ctx&) -> McpResult<Json> { return Ok(Json(R"({})")); });
  plain2.resource(Json(R"({"uri":"file:///a","name":"a"})"),
                  [](Ctx&) -> McpResult<Json> {
                    Json result;
                    Json contents = Json("[]");
                    Json item;
                    item.add("uri", Json::str("file:///a"));
                    item.add("text", Json::str("aa"));
                    contents.push_back(std::move(item));
                    result.add("contents", std::move(contents));
                    return Ok(std::move(result));
                  });
  auto rserver = plain2.build();
  Out rd = serveOnce(rserver, makeRequest("h3", "resources/read", Json(R"({"uri":"file:///a"})")));
  ASSERT_NE(rd.result, nullptr);
  EXPECT_EQ(rd.result->result["ttlMs"].toInt64(), 0);
  EXPECT_EQ(rd.result->result["cacheScope"].toString(), "public");

  // Explicit opt-out (legacy-only servers, wire probing) omits both fields.
  Builder off;
  off.listCache(-1);
  off.simpleTool("t3", "", [](Ctx&) -> McpResult<Json> { return Ok(Json(R"({})")); });
  auto oserver = off.build();
  Out x = serveOnce(oserver, makeRequest("h4", "tools/list", Json(nullptr)));
  ASSERT_NE(x.result, nullptr);
  EXPECT_TRUE(x.result->result["ttlMs"].isError());
  EXPECT_TRUE(x.result->result["cacheScope"].isError());
}

// ---- legacy dual-era mode (acceptLegacyClients) ------------------------------------

TEST(ServerLegacy, InitializeHandshakeWhenEnabled) {
  Builder b;
  b.acceptLegacyClients(true)
      .supports(ProtocolVersion::V2025_06_18)
      .supports(ProtocolVersion::Latest);
  auto server = b.build();

  const std::string initLine = encodeRequest(
      RequestId(int64_t(1)), "initialize",
      Json(R"({"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"cc","version":"1"}})"));
  Out o = serveOnce(server, initLine);
  ASSERT_NE(o.result, nullptr);
  EXPECT_EQ(o.result->result["protocolVersion"].toString(), "2025-06-18");
  EXPECT_EQ(o.result->result["serverInfo"]["name"].toString(), "mcp-cpp-server");

  // Unsupported legacy version -> -32022 with the supported list.
  Out bad = serveOnce(server, encodeRequest(
      RequestId(int64_t(2)), "initialize",
      Json(R"({"protocolVersion":"1999-01-01","capabilities":{}})")));
  ASSERT_NE(bad.error, nullptr);
  EXPECT_EQ(bad.error->error.code, -32022);
}

TEST(ServerLegacy, RequestsWithoutMetaServedWhenEnabled) {
  Builder b;
  b.acceptLegacyClients(true);
  b.simpleTool("t", "", [](Ctx&) -> McpResult<Json> { return Ok(Json(R"({"ok":true})")); });
  auto server = b.build();

  // No _meta at all: served with default metadata.
  Out o = serveOnce(server, encodeRequest(RequestId(int64_t(1)), "tools/call",
                                          Json(R"({"name":"t"})")));
  ASSERT_NE(o.result, nullptr);
  EXPECT_EQ(o.result->result["ok"].isTrue(), true);

  // Empty _meta object (some AI clients send this): also served.
  Out o2 = serveOnce(server, encodeRequest(RequestId(int64_t(2)), "tools/call",
                                           Json(R"({"name":"t","_meta":{}})")));
  ASSERT_NE(o2.result, nullptr);
  EXPECT_EQ(o2.result->result["ok"].isTrue(), true);

  // Partial _meta (missing required fields): served with defaults in legacy.
  Out o3 = serveOnce(server, encodeRequest(RequestId(int64_t(3)), "tools/call",
                                           Json(R"({"name":"t","_meta":{"foo":1}})")));
  ASSERT_NE(o3.result, nullptr);
  EXPECT_EQ(o3.result->result["ok"].isTrue(), true);

  // _meta present but not an object: still an error.
  Out bad = serveOnce(server, encodeRequest(RequestId(int64_t(4)), "tools/call",
                                            Json(R"({"name":"t","_meta":5})")));
  ASSERT_NE(bad.error, nullptr);
  EXPECT_EQ(bad.error->error.code, -32602);

  // Parameterless request (params:null) - legacy clients call tools/list
  // without params; it must be served with defaults.
  Out o4 = serveOnce(server, encodeRequest(RequestId(int64_t(5)), "tools/list",
                                           Json(nullptr)));
  ASSERT_NE(o4.result, nullptr);
  EXPECT_TRUE(o4.result->result["tools"].isArray());

  // notifications/initialized is a notification: silently ignored.
  MemTransport t({});
  server->processLine(encodeNotification("notifications/initialized", Json(nullptr)), t);
  EXPECT_TRUE(t.output_.empty());
}

TEST(ServerLegacy, PartialMetaStillStrictWhenDisabled) {
  Builder b;  // acceptLegacyClients NOT enabled
  b.simpleTool("t", "", [](Ctx&) -> McpResult<Json> { return Ok(Json(R"({"ok":true})")); });
  auto server = b.build();

  // Empty _meta object fails in strict mode.
  Out o = serveOnce(server, encodeRequest(RequestId(int64_t(1)), "tools/call",
                                          Json(R"({"name":"t","_meta":{}})")));
  ASSERT_NE(o.error, nullptr);
  EXPECT_EQ(o.error->error.code, -32602);

  // No _meta at all fails in strict mode.
  Out o2 = serveOnce(server, encodeRequest(RequestId(int64_t(2)), "tools/call",
                                           Json(R"({"name":"t"})")));
  ASSERT_NE(o2.error, nullptr);
  EXPECT_EQ(o2.error->error.code, -32602);
}

TEST(ServerLegacy, StrictByDefault) {
  Builder b;  // acceptLegacyClients NOT enabled
  auto server = b.build();
  Out o = serveOnce(server, encodeRequest(
      RequestId(int64_t(1)), "initialize",
      Json(R"({"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"cc","version":"1"}})")));
  ASSERT_NE(o.error, nullptr);
  EXPECT_EQ(o.error->error.code, -32601);
}

// ---- progress notifications (basic/patterns/progress.md) -------------------------

TEST(ServerProgress, EmitsProgressForOptedInRequest) {
  Builder b;
  b.simpleTool("work", "long task", [](Ctx& c) -> McpResult<Json> {
    c.progress(1, 3, "starting");
    c.progress(3, 3, "");  // no message, total known
    return Ok(Json(R"({"resultType":"complete"})"));
  });
  auto server = b.build();

  // Request WITH a progressToken in _meta: expect 2 progress notifications then
  // the result (3 wire messages).
  Json params = encodeRequestMeta("2026-07-28", Json(R"({})"), std::nullopt,
                                  RequestId(std::string("tok-1")));
  params.add("name", Json::str("work"));
  params.add("arguments", Json(R"({})"));
  const std::string line = encodeRequest(RequestId(int64_t(1)), "tools/call", params);

  MemTransport t({line});
  server->processLine(line, t);
  ASSERT_EQ(t.output_.size(), 3u);

  auto n1 = parseMessage(t.output_[0]);
  auto* notif1 = std::get_if<JsonRpcNotification>(&n1);
  ASSERT_NE(notif1, nullptr);
  EXPECT_EQ(notif1->method, "notifications/progress");
  EXPECT_EQ(notif1->params["progressToken"].toString(), "tok-1");
  EXPECT_DOUBLE_EQ(notif1->params["progress"].toDouble(), 1.0);
  EXPECT_DOUBLE_EQ(notif1->params["total"].toDouble(), 3.0);
  EXPECT_EQ(notif1->params["message"].toString(), "starting");

  auto n2 = parseMessage(t.output_[1]);
  auto* notif2 = std::get_if<JsonRpcNotification>(&n2);
  ASSERT_NE(notif2, nullptr);
  EXPECT_DOUBLE_EQ(notif2->params["progress"].toDouble(), 3.0);
  EXPECT_FALSE(notif2->params.contains(std::string("message")));

  auto res = parseMessage(t.output_[2]);
  EXPECT_NE(std::get_if<JsonRpcResult>(&res), nullptr);
}

TEST(ServerProgress, SilentWithoutToken) {
  Builder b;
  b.simpleTool("work", "long task", [](Ctx& c) -> McpResult<Json> {
    c.progress(1, 3, "ignored");  // no token -> no-op
    return Ok(Json(R"({"resultType":"complete"})"));
  });
  auto server = b.build();
  Out o = serveOnce(server, makeRequest("1", "tools/call",
                                        Json(R"({"name":"work","arguments":{}})")));
  ASSERT_NE(o.result, nullptr);
  // Exactly one message (the result) - serveOnce already asserts output size 1.
}

TEST(ServerProgress, ArbitraryNotificationReachesTransport) {
  Builder b;
  b.simpleTool("log", "logs", [](Ctx& c) -> McpResult<Json> {
    Json p;
    p.add("level", Json::str("info"));
    p.add("data", Json::str("hello"));
    c.notify("notifications/message", std::move(p));
    return Ok(Json(R"({"resultType":"complete"})"));
  });
  auto server = b.build();
  const std::string line =
      makeRequest("1", "tools/call", Json(R"({"name":"log","arguments":{}})"));
  MemTransport t({line});
  server->processLine(line, t);
  ASSERT_EQ(t.output_.size(), 2u);
  auto n = parseMessage(t.output_[0]);
  auto* notif = std::get_if<JsonRpcNotification>(&n);
  ASSERT_NE(notif, nullptr);
  EXPECT_EQ(notif->method, "notifications/message");
}

// ---- outputSchema enforcement (server/tools.md "Output Schema") ------------------

TEST(ServerOutputSchema, RejectsNonConformingStructuredContent) {
  Builder b;
  Json def(R"({"name":"weather","description":"w",
               "inputSchema":{"type":"object"},
               "outputSchema":{"type":"object","properties":{"temp":{"type":"number"}},
                               "required":["temp"]}})");
  b.tool(def, [](Ctx&) -> McpResult<Json> {
    // temp is a string, not a number -> violates outputSchema.
    return Ok(Json(R"({"resultType":"complete","content":[],"structuredContent":{"temp":"cold"}})"));
  });
  auto server = b.build();
  Out o = serveOnce(server, makeRequest("1", "tools/call",
                                        Json(R"({"name":"weather","arguments":{}})")));
  ASSERT_NE(o.error, nullptr);
  EXPECT_EQ(o.error->error.code, static_cast<int32_t>(McpErrorCode::InternalError));
}

TEST(ServerOutputSchema, AcceptsConformingStructuredContent) {
  Builder b;
  Json def(R"({"name":"weather","description":"w",
               "inputSchema":{"type":"object"},
               "outputSchema":{"type":"object","properties":{"temp":{"type":"number"}},
                               "required":["temp"]}})");
  b.tool(def, [](Ctx&) -> McpResult<Json> {
    return Ok(Json(R"({"resultType":"complete","content":[],"structuredContent":{"temp":21.5}})"));
  });
  auto server = b.build();
  Out o = serveOnce(server, makeRequest("1", "tools/call",
                                        Json(R"({"name":"weather","arguments":{}})")));
  ASSERT_NE(o.result, nullptr);
  EXPECT_DOUBLE_EQ(o.result->result["structuredContent"]["temp"].toDouble(), 21.5);
}

TEST(ServerOutputSchema, TextOnlyResultSkipsValidation) {
  Builder b;
  Json def(R"({"name":"weather","description":"w",
               "inputSchema":{"type":"object"},
               "outputSchema":{"type":"object","required":["temp"]}})");
  b.tool(def, [](Ctx&) -> McpResult<Json> {
    // No structuredContent: nothing to validate (text-only results stay valid).
    return Ok(Json(R"({"resultType":"complete","content":[]})"));
  });
  auto server = b.build();
  Out o = serveOnce(server, makeRequest("1", "tools/call",
                                        Json(R"({"name":"weather","arguments":{}})")));
  ASSERT_NE(o.result, nullptr);
}

// ---- legacy-era compatibility (2025-xx clients) ----------------------------------
// 2026-07-28 removed ping and logging/setLevel and replaced per-URI resource
// subscriptions with subscriptions/listen. In legacy mode they are served
// again, and legacy results omit resultType (pre-2026 schemas reject it).

TEST(ServerLegacy, PingAndLoggingServedInLegacyMode) {
  Builder b;
  b.acceptLegacyClients(true);
  auto server = b.build();

  Out p = serveOnce(server, encodeRequest(RequestId(int64_t(1)), "ping", Json(nullptr)));
  ASSERT_NE(p.result, nullptr);
  EXPECT_FALSE(p.result->result.contains(std::string("resultType")));  // legacy: no resultType

  Out l = serveOnce(server, encodeRequest(RequestId(int64_t(2)), "logging/setLevel",
                                          Json(R"({"level":"debug"})")));
  ASSERT_NE(l.result, nullptr);
}

TEST(ServerLegacy, PingIsMethodNotFoundInStrictMode) {
  Builder b;  // strict 2026-07-28 server
  auto server = b.build();
  Out o = serveOnce(server, makeRequest("1", "ping", Json(nullptr)));
  ASSERT_NE(o.error, nullptr);
  EXPECT_EQ(o.error->error.code, -32601);
}

TEST(ServerLegacy, ResourceSubscribeServedInLegacyMode) {
  Builder b;
  b.acceptLegacyClients(true);
  b.resource(Json(R"({"uri":"test://r","name":"r"})"),
             [](Ctx&) -> McpResult<Json> { return Ok(Json(R"({"contents":[]})")); });
  auto server = b.build();
  Out o = serveOnce(server, encodeRequest(RequestId(int64_t(1)), "resources/subscribe",
                                          Json(R"({"uri":"test://r"})")));
  ASSERT_NE(o.result, nullptr);
}

TEST(ServerLegacy, LegacyRequestOmitsResultTypeModernKeepsIt) {
  Builder b;
  b.acceptLegacyClients(true);
  b.simpleTool("t", "", [](Ctx&) -> McpResult<Json> { return Ok(Json(R"({"ok":true})")); });
  auto server = b.build();

  // Legacy: no _meta -> relaxed meta -> no resultType on the result.
  Out legacy = serveOnce(server, encodeRequest(RequestId(int64_t(1)), "tools/call",
                                               Json(R"({"name":"t","arguments":{}})")));
  ASSERT_NE(legacy.result, nullptr);
  EXPECT_FALSE(legacy.result->result.contains(std::string("resultType")));

  // Modern: full _meta -> resultType injected (2026-07-28 requirement).
  Out modern = serveOnce(server, makeRequest("2", "tools/call",
                                             Json(R"({"name":"t","arguments":{}})")));
  ASSERT_NE(modern.result, nullptr);
  ASSERT_TRUE(modern.result->result.contains(std::string("resultType")));
  EXPECT_EQ(modern.result->result["resultType"].toString(), "complete");
}

TEST(ServerLegacy, DeclaresConfiguredVersionsInDiscover) {
  Builder b;
  b.supports(ProtocolVersion::Latest)
      .supports(ProtocolVersion::V2025_11_25)
      .supports(ProtocolVersion::V2025_06_18);
  auto server = b.build();
  const auto& versions = server->supportedVersions();
  EXPECT_EQ(versions.size(), 3u);
  auto has = [&](const char* v) {
    return std::find(versions.begin(), versions.end(), v) != versions.end();
  };
  // All three declared versions are advertised; the first supports() call
  // replaced the Latest-only default, so Latest must be listed explicitly.
  EXPECT_TRUE(has("2026-07-28"));
  EXPECT_TRUE(has("2025-11-25"));
  EXPECT_TRUE(has("2025-06-18"));
}
