// conformance_server - the fixture server the official MCP conformance suite
// expects (see docs/CONFORMANCE.md).
//
// It implements the tool / resource / prompt surface the suite's server
// scenarios call by name, and runs in legacy-client mode so the 2025-era
// handshake and missing-header backward compatibility apply.
//
//   ./bin/conformance_server.exe --port 8099
//   npx @modelcontextprotocol/conformance server --url http://127.0.0.1:8099/mcp
//
// The server-initiated sampling/elicitation tools (test_sampling,
// test_elicitation*) require the server to send a REQUEST to the client
// mid-call. That mechanism was replaced by MRTR (input_required) in the
// 2026-07-28 spec and is deliberately not implemented; those scenarios are
// listed in conformance/expected-failures.yaml.

#include <mcp/mcp.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <functional>
#include <memory>
#include <thread>

using namespace mcp;
using namespace mcp::server;

namespace {

// 1x1 red PNG and a minimal silent WAV, base64-encoded (test fixtures).
constexpr const char* kPngB64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==";
constexpr const char* kWavB64 =
    "UklGRjQAAABXQVZFZm10IBAAAAABAAEAQB8AAIA+AAACABAAZGF0YRAAAAAAAAAAAAAAAAAAAAAAAAAA";

Json contentItem(const char* type) {
  Json item;
  item.add("type", Json::str(type));
  return item;
}

Json textContent(const std::string& text) {
  Json item = contentItem("text");
  item.add("text", Json::str(text));
  return item;
}

Json imageContent(const char* data, const char* mime) {
  Json item = contentItem("image");
  item.add("data", Json::str(data));
  item.add("mimeType", Json::str(mime));
  return item;
}

Json embeddedResource(const std::string& uri, const std::string& mime, const std::string& text) {
  Json res;
  res.add("uri", Json::str(uri));
  res.add("mimeType", Json::str(mime));
  res.add("text", Json::str(text));
  Json item = contentItem("resource");
  item.add("resource", std::move(res));
  return item;
}

Json toolResult(Json content) {
  Json result;
  result.add("content", std::move(content));
  return result;
}

Json oneText(const std::string& text) {
  Json content = Json("[]");
  content.push_back(textContent(text));
  return toolResult(std::move(content));
}

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// A tool definition helper: {name, description, inputSchema}.
Json toolDef(const char* name, const char* description, const char* schemaJson) {
  Json def(R"({})");
  def.add("name", Json::str(name));
  def.add("description", Json::str(description));
  def.add("inputSchema", Json(schemaJson));
  return def;
}

}  // namespace

int main(int argc, char** argv) {
  int port = 8099;
  bool http = true;
  bool strict = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--port" && i + 1 < argc) {
      port = std::atoi(argv[++i]);
    } else if (arg == "--stdio") {
      http = false;
    } else if (arg == "--strict") {
      strict = true;
    }
  }

  Builder b;
  b.name("conformance-test-server").version("1.0.0");
  if (strict) {
    // Strict 2026-07-28 mode: stateless _meta is REQUIRED and invalid metadata
    // is rejected (-32602). This is what the server-stateless scenario probes.
  } else {
    // The 2025-era suite drives the server with the old handshake and without
    // the mirrored headers; legacy mode serves both. Declaring the older
    // revisions is what makes the handshake negotiate successfully.
    b.acceptLegacyClients(true);
    // The first supports() call replaces the Latest-only default, so Latest
    // must be listed explicitly to keep serving 2026-07-28 too.
    b.supports(ProtocolVersion::Latest)
        .supports(ProtocolVersion::V2025_03_26)
        .supports(ProtocolVersion::V2025_06_18)
        .supports(ProtocolVersion::V2025_11_25);
  }

  // ---- tools ---------------------------------------------------------------
  b.tool(toolDef("test_simple_text", "Returns simple text content",
                 R"({"type":"object","additionalProperties":false})"),
         [](Ctx&) -> McpResult<Json> {
           return Ok(oneText("This is a simple text response for testing."));
         });

  b.tool(toolDef("test_image_content", "Returns image content",
                 R"({"type":"object","additionalProperties":false})"),
         [](Ctx&) -> McpResult<Json> {
           Json content = Json("[]");
           content.push_back(imageContent(kPngB64, "image/png"));
           return Ok(toolResult(std::move(content)));
         });

  b.tool(toolDef("test_audio_content", "Returns audio content",
                 R"({"type":"object","additionalProperties":false})"),
         [](Ctx&) -> McpResult<Json> {
           Json item = contentItem("audio");
           item.add("data", Json::str(kWavB64));
           item.add("mimeType", Json::str("audio/wav"));
           Json content = Json("[]");
           content.push_back(std::move(item));
           return Ok(toolResult(std::move(content)));
         });

  b.tool(toolDef("test_embedded_resource", "Returns an embedded resource",
                 R"({"type":"object","additionalProperties":false})"),
         [](Ctx&) -> McpResult<Json> {
           Json content = Json("[]");
           content.push_back(embeddedResource("test://embedded-resource", "text/plain",
                                              "This is an embedded resource content."));
           return Ok(toolResult(std::move(content)));
         });

  b.tool(toolDef("test_multiple_content_types", "Returns text, image and resource content",
                 R"({"type":"object","additionalProperties":false})"),
         [](Ctx&) -> McpResult<Json> {
           Json content = Json("[]");
           content.push_back(textContent("Multiple content types test:"));
           content.push_back(imageContent(kPngB64, "image/png"));
           content.push_back(embeddedResource("test://mixed-content-resource",
                                              "application/json", R"({"test":"data","value":123})"));
           return Ok(toolResult(std::move(content)));
         });

  b.tool(toolDef("test_error_handling", "Always returns a tool error",
                 R"({"type":"object","additionalProperties":false})"),
         [](Ctx&) -> McpResult<Json> {
           Json content = Json("[]");
           content.push_back(textContent("This tool intentionally returns an error for testing"));
           Json result;
           result.add("content", std::move(content));
           result.add("isError", Json(true));
           return Ok(std::move(result));
         });

  b.tool(toolDef("test_tool_with_logging", "Emits three log notifications",
                 R"({"type":"object","additionalProperties":false})"),
         [](Ctx& c) -> McpResult<Json> {
           const char* messages[] = {"Tool execution started", "Tool processing data",
                                     "Tool execution completed"};
           for (int i = 0; i < 3; ++i) {
             if (i > 0) sleepMs(50);
             // Ctx::log (not a raw notification): the server drops the message
             // for a modern request that did not set _meta.logLevel, which is
             // exactly the MUST the server-no-log-without-loglevel check probes.
             c.log("info", messages[i]);
           }
           return Ok(oneText("Tool execution completed"));
         });

  // The stateless scenario's logging probe: same tool under the name it looks
  // for. It attempts to log with no _meta.logLevel, so nothing may be emitted.
  b.tool(toolDef("test_logging_tool", "Attempts to log without opt-in",
                 R"({"type":"object"})"),
         [](Ctx& c) -> McpResult<Json> {
           c.log("error", "this must not reach a client that did not opt in");
           Json params;
           params.add("level", Json::str("error"));
           params.add("data", Json::str("raw notify must be gated too"));
           // Raw escape hatch: the server gates notifications/message itself.
           c.notify("notifications/message", std::move(params));
           return Ok(oneText("logging attempted"));
         });

  b.tool(toolDef("test_tool_with_progress", "Reports progress 0/50/100",
                 R"({"type":"object","additionalProperties":false})"),
         [](Ctx& c) -> McpResult<Json> {
           c.progress(0, 100, "");
           sleepMs(50);
           c.progress(50, 100, "");
           sleepMs(50);
           c.progress(100, 100, "");
           return Ok(oneText("Progress tool completed"));
         });

  // JSON Schema 2020-12 keyword preservation (SEP-1613): the suite checks that
  // these keywords survive verbatim in the tool listing, so the fixture carries
  // one of each (composition, conditional, $anchor alongside $defs/$ref).
  b.tool(Json(R"({
        "name": "json_schema_2020_12_tool",
        "description": "Tool with JSON Schema 2020-12 features",
        "inputSchema": {
          "$schema": "https://json-schema.org/draft/2020-12/schema",
          "type": "object",
          "$defs": {
            "address": {
              "$anchor": "addressAnchor",
              "type": "object",
              "properties": { "street": {"type":"string"}, "city": {"type":"string"} }
            }
          },
          "properties": {
            "name": {"type":"string"},
            "address": {"$ref": "#/$defs/address"}
          },
          "allOf": [
            {"properties": {"name": {"type":"string"}}},
            {"anyOf": [
              {"properties": {"address": {"type":"object"}}},
              {"required": ["name"]}
            ]}
          ],
          "if": {"properties": {"name": {"type":"string"}}, "required": ["name"]},
          "then": {"properties": {"address": {"type":"object"}}},
          "else": {"properties": {"address": {"type":"object"}}},
          "additionalProperties": false
        }
      })"),
         [](Ctx&) -> McpResult<Json> { return Ok(oneText("ok")); });

  // The sampling / elicitation tools need a server->client REQUEST, which
  // 2026-07-28 replaced with MRTR. They are intentionally absent here; the
  // scenarios are listed in conformance/expected-failures.yaml.
  //
  // The MRTR surface (SEP-2322) is different: the server answers the original
  // request with InputRequiredResult and the client retries, so these tools are
  // exercised by the input-required-result-* scenarios.
  //
  // requestState needs a codec; the key is a fixture value (a real deployment
  // supplies its own secret).
  auto codec = RequestStateCodec::create("conformance-fixture-requeststate-key-0001", 600);
  if (codec.has_value()) b.requestStateCodec(std::move(*codec));

  // This round's inputResponses entry (or Null on round 1). The value is the
  // client's RESULT for that request: an elicitation result wraps the answers
  // in `content`, while sampling and roots return their own result shape.
  const auto inputResult = [](const Ctx& c, const char* key) {
    return member(member(c.params, kInputResponsesKey), key);
  };
  // The elicitation answers of this round (Null when not an elicitation round).
  const auto inputContent = [inputResult](const Ctx& c, const char* key) {
    return member(inputResult(c, key), "content");
  };
  // The elicitation request shape used by the MRTR scenarios.
  const auto elicitation = [](const char* message, const char* schemaJson) {
    Json params;
    params.add("mode", Json::str("form"));
    params.add("message", Json::str(message));
    params.add("requestedSchema", Json(schemaJson));
    Json request;
    request.add("method", Json::str("elicitation/create"));
    request.add("params", std::move(params));
    return request;
  };

  // A1: one elicitation, then "Hello, <name>!".
  b.tool(toolDef("test_input_required_result_elicitation", "MRTR: one elicitation",
                 R"({"type":"object"})"),
         [inputContent, elicitation](Ctx& c) -> McpResult<Json> {
           const Json content = inputContent(c, "user_name");
           if (!content.isObject()) {
             Json requests = Json(R"({})");
             requests.add("user_name",
                          elicitation("What is your name?",
                                      R"({"type":"object","properties":{"name":{"type":"string"}},
                                          "required":["name"]})"));
             return c.inputRequired(std::move(requests), "");
           }
           const Json name = member(content, "name");
           return Ok(oneText("Hello, " + std::string(name.toString()) + "!"));
         });

  // A2: one sampling request, then the model's answer.
  b.tool(toolDef("test_input_required_result_sampling", "MRTR: one sampling request",
                 R"({"type":"object"})"),
         [inputResult](Ctx& c) -> McpResult<Json> {
           const Json content = inputResult(c, "capital_question");
           if (!content.isObject()) {
             Json sampling;
             Json messages = Json("[]");
             Json message;
             message.add("role", Json::str("user"));
             Json text;
             text.add("type", Json::str("text"));
             text.add("text", Json::str("What is the capital of France?"));
             message.add("content", std::move(text));
             messages.push_back(std::move(message));
             sampling.add("messages", std::move(messages));
             sampling.add("maxTokens", Json(static_cast<int64_t>(100)));
             Json request;
             request.add("method", Json::str("sampling/createMessage"));
             request.add("params", std::move(sampling));
             Json requests = Json(R"({})");
             requests.add("capital_question", std::move(request));
             return c.inputRequired(std::move(requests), "");
           }
           const Json samplingContent = member(content, "content");
           const std::string answer =
               member(samplingContent, "text").isString()
                   ? std::string(member(samplingContent, "text").toString())
                   : std::string(content.toString());
           return Ok(oneText("answer: " + answer));
         });

  // A3: one roots/list request, then a result referencing the roots.
  b.tool(toolDef("test_input_required_result_list_roots", "MRTR: roots/list",
                 R"({"type":"object"})"),
         [inputResult](Ctx& c) -> McpResult<Json> {
           const Json content = inputResult(c, "client_roots");
           if (!content.isObject()) {
             Json request;
             request.add("method", Json::str("roots/list"));
             request.add("params", Json(R"({})"));
             Json requests = Json(R"({})");
             requests.add("client_roots", std::move(request));
             return c.inputRequired(std::move(requests), "");
           }
           const Json roots = member(content, "roots");
           std::string first;
           if (roots.isArray()) {
             for (const auto& root : roots.toVector()) {
               first = std::string(member(root, "uri").toString());
               break;
             }
           }
           return Ok(oneText("roots: " + first));
         });

  // A4: requestState round-trips; the completion must say "state-ok".
  b.tool(toolDef("test_input_required_result_request_state",
                 "MRTR: requestState round trip", R"({"type":"object"})"),
         [inputContent, elicitation](Ctx& c) -> McpResult<Json> {
           const Json content = inputContent(c, "confirm");
           if (!content.isObject()) {
             Json requests = Json(R"({})");
             requests.add("confirm",
                          elicitation("Please confirm",
                                      R"({"type":"object","properties":{"ok":{"type":"boolean"}},
                                          "required":["ok"]})"));
             auto state = c.sealRequestState("fixture-state");
             if (!state.has_value()) return Err(state.error());
             return c.inputRequired(std::move(requests), state.value());
           }
           // A forged or stale state makes openRequestState fail with -32602.
           auto opened = c.openRequestState();
           if (!opened.has_value()) return Err(opened.error());
           if (!opened->has_value()) return Err(McpErrorCode::InvalidParams, "missing requestState");
           return Ok(oneText("state-ok"));
         });

  // A5: three simultaneous input requests plus state.
  b.tool(toolDef("test_input_required_result_multiple_inputs",
                 "MRTR: elicitation + sampling + roots", R"({"type":"object"})"),
         [inputContent, inputResult, elicitation](Ctx& c) -> McpResult<Json> {
           const bool done = inputContent(c, "user_name").isObject() &&
                             inputResult(c, "greeting").isObject() &&
                             inputResult(c, "client_roots").isObject();
           if (!done) {
             Json requests = Json(R"({})");
             requests.add("user_name",
                          elicitation("What is your name?",
                                      R"({"type":"object","properties":{"name":{"type":"string"}},
                                          "required":["name"]})"));
             Json sampling;
             Json messages = Json("[]");
             Json message;
             message.add("role", Json::str("user"));
             Json text;
             text.add("type", Json::str("text"));
             text.add("text", Json::str("Generate a greeting"));
             message.add("content", std::move(text));
             messages.push_back(std::move(message));
             sampling.add("messages", std::move(messages));
             sampling.add("maxTokens", Json(static_cast<int64_t>(50)));
             Json samplingRequest;
             samplingRequest.add("method", Json::str("sampling/createMessage"));
             samplingRequest.add("params", std::move(sampling));
             requests.add("greeting", std::move(samplingRequest));
             Json rootsRequest;
             rootsRequest.add("method", Json::str("roots/list"));
             rootsRequest.add("params", Json(R"({})"));
             requests.add("client_roots", std::move(rootsRequest));
             auto state = c.sealRequestState("multi-round-1");
             if (!state.has_value()) return Err(state.error());
             return c.inputRequired(std::move(requests), state.value());
           }
           auto opened = c.openRequestState();
           if (!opened.has_value()) return Err(opened.error());
           return Ok(oneText("all inputs received"));
         });

  // A6: three rounds with evolving state.
  b.tool(toolDef("test_input_required_result_multi_round", "MRTR: multi round",
                 R"({"type":"object"})"),
         [inputContent, elicitation](Ctx& c) -> McpResult<Json> {
           Json requests = Json(R"({})");
           // Each retry carries ONLY the newly answered key, so the round is
           // selected by which key arrived, not by which are absent.
           const Json step1 = inputContent(c, "step1");
           const Json step2 = inputContent(c, "step2");
           if (!step1.isObject() && !step2.isObject()) {
             requests.add("step1",
                          elicitation("Step 1: What is your name?",
                                      R"({"type":"object","properties":{"name":{"type":"string"}},
                                          "required":["name"]})"));
             auto state = c.sealRequestState("step=1");
             if (!state.has_value()) return Err(state.error());
             return c.inputRequired(std::move(requests), state.value());
           }
           if (!step2.isObject()) {
             // Step 1 arrived: the state must be the one step 1 issued.
             auto opened = c.openRequestState();
             if (!opened.has_value()) return Err(opened.error());
             if (!opened->has_value() || **opened != "step=1") {
               return Err(McpErrorCode::InvalidParams, "unexpected requestState");
             }
             requests.add("step2",
                          elicitation("Step 2: What is your favorite color?",
                                      R"({"type":"object","properties":{"color":{"type":"string"}},
                                          "required":["color"]})"));
             auto state = c.sealRequestState("step=2");
             if (!state.has_value()) return Err(state.error());
             return c.inputRequired(std::move(requests), state.value());
           }
           // Step 2 arrived: the state must be the one step 2 issued.
           auto verified = c.openRequestState();
           if (!verified.has_value()) return Err(verified.error());
           if (!verified->has_value() || **verified != "step=2") {
             return Err(McpErrorCode::InvalidParams, "unexpected requestState");
           }
           return Ok(oneText("multi round complete"));
         });

  // A7: integrity-protected state; a tampered value must be rejected.
  b.tool(toolDef("test_input_required_result_tampered_state",
                 "MRTR: rejects tampered requestState", R"({"type":"object"})"),
         [inputContent, elicitation](Ctx& c) -> McpResult<Json> {
           const Json content = inputContent(c, "answer");
           if (!content.isObject()) {
             Json requests = Json(R"({})");
             requests.add("answer",
                          elicitation("Answer to continue",
                                      R"({"type":"object","properties":{"v":{"type":"string"}}})"));
             auto state = c.sealRequestState("tamper-check");
             if (!state.has_value()) return Err(state.error());
             return c.inputRequired(std::move(requests), state.value());
           }
           auto opened = c.openRequestState();
           if (!opened.has_value()) return Err(opened.error());
           if (!opened->has_value() || **opened != "tamper-check") {
             return Err(McpErrorCode::InvalidParams, "requestState verification failed");
           }
           return Ok(oneText("state valid"));
         });

  // A8b: only request what the client declared. The suite sends sampling-only
  // capabilities and rejects any elicitation/create request in the answer.
  b.tool(toolDef("test_input_required_result_capabilities",
                 "MRTR: requests only declared capabilities", R"({"type":"object"})"),
         [elicitation](Ctx& c) -> McpResult<Json> {
           const Json capabilities = c.meta.clientCapabilities;
           Json requests = Json(R"({})");
           if (capabilitiesInclude(capabilities, "sampling")) {
             Json sampling;
             Json messages = Json("[]");
             Json message;
             message.add("role", Json::str("user"));
             Json text;
             text.add("type", Json::str("text"));
             text.add("text", Json::str("Declared-capability probe"));
             message.add("content", std::move(text));
             messages.push_back(std::move(message));
             sampling.add("messages", std::move(messages));
             sampling.add("maxTokens", Json(static_cast<int64_t>(16)));
             Json request;
             request.add("method", Json::str("sampling/createMessage"));
             request.add("params", std::move(sampling));
             requests.add("sampling_probe", std::move(request));
           }
           if (capabilitiesInclude(capabilities, "elicitation")) {
             requests.add("elicit_probe",
                          elicitation("Declared-capability probe",
                                      R"({"type":"object","properties":{"v":{"type":"string"}}})"));
           }
           if (capabilitiesInclude(capabilities, "roots")) {
             Json request;
             request.add("method", Json::str("roots/list"));
             request.add("params", Json(R"({})"));
             requests.add("roots_probe", std::move(request));
           }
           // requestState keeps the result valid even when nothing was
           // declared (InputRequiredResult requires one of the two).
           auto state = c.sealRequestState("capability-probe");
           if (!state.has_value()) return Err(state.error());
           return c.inputRequired(std::move(requests), state.value());
         });

  // A8: the same contract on prompts/get (InputRequiredResult is universal).
  Json mrtrPrompt;
  mrtrPrompt.add("name", Json::str("test_input_required_result_prompt"));
  mrtrPrompt.add("description", Json::str("MRTR prompt"));
  b.prompt(mrtrPrompt, [inputContent, elicitation](Ctx& c) -> McpResult<Json> {
    const Json content = inputContent(c, "user_context");
    if (!content.isObject()) {
      Json requests = Json(R"({})");
      requests.add("user_context",
                   elicitation("What context should the prompt use?",
                               R"({"type":"object","properties":{"context":{"type":"string"}},
                                   "required":["context"]})"));
      return c.inputRequired(std::move(requests), "");
    }
    Json message;
    message.add("role", Json::str("user"));
    Json text;
    text.add("type", Json::str("text"));
    text.add("text", Json::str("context: " + std::string(member(content, "context").toString())));
    message.add("content", std::move(text));
    Json messages = Json("[]");
    messages.push_back(std::move(message));
    Json result;
    result.add("messages", std::move(messages));
    return Ok(std::move(result));
  });

  // Stateless scenario: a tool that needs a capability the caller did not
  // declare. It asks for sampling, so the -32021 payload names "sampling".
  b.tool(toolDef("test_missing_capability", "Requires the sampling capability",
                 R"({"type":"object"})"),
         [](Ctx& c) -> McpResult<Json> {
           Json sampling;
           Json messages = Json("[]");
           Json message;
           message.add("role", Json::str("user"));
           Json text;
           text.add("type", Json::str("text"));
           text.add("text", Json::str("probe"));
           message.add("content", std::move(text));
           messages.push_back(std::move(message));
           sampling.add("messages", std::move(messages));
           sampling.add("maxTokens", Json(static_cast<int64_t>(8)));
           Json request;
           request.add("method", Json::str("sampling/createMessage"));
           request.add("params", std::move(sampling));
           Json requests = Json(R"({})");
           requests.add("probe", std::move(request));
           return c.inputRequired(std::move(requests), "");
         });

  // Stateless scenario: an MRTR answer delivered on the request's own stream.
  b.tool(toolDef("test_streaming_elicitation", "MRTR answer on a stream",
                 R"({"type":"object"})"),
         [](Ctx& c) -> McpResult<Json> {
           Json requests = Json(R"({})");
           Json params;
           params.add("mode", Json::str("form"));
           params.add("message", Json::str("Streaming elicitation"));
           params.add("requestedSchema",
                      Json(R"({"type":"object","properties":{"v":{"type":"string"}}})"));
           Json request;
           request.add("method", Json::str("elicitation/create"));
           request.add("params", std::move(params));
           requests.add("stream_input", std::move(request));
           return c.inputRequired(std::move(requests), "");
         });

  // SEP-2243: a tool whose string parameter is mirrored into a header. The
  // suite drives its Base64/literal validation cases through this definition.
  b.tool(Json(R"({
        "name": "test_header_param",
        "description": "Mirrors a string parameter into Mcp-Param-X-Region",
        "inputSchema": {
          "type": "object",
          "properties": {
            "region": {"type": "string", "x-mcp-header": "X-Region"},
            "query": {"type": "string"}
          },
          "required": ["region"]
        }
      })"),
         [](Ctx& c) -> McpResult<Json> {
           const Json region = member(c.params["arguments"], "region");
           return Ok(oneText("region=" + std::string(region.toString())));
         });

  // ---- tasks extension (SEP-2663) -----------------------------------------
  // The suite's tasks-* scenarios drive these tools. `taskSupport = required`
  // is fixture policy (not library policy): without the extension the call is
  // rejected with -32021, with it the work becomes a Task.
  auto store = std::make_shared<tasks::InMemoryTaskStore>();
  b.tasks(store);

  // tasks::clientSupports() reads the per-request extension advertisement; the
  // fixture's own missingTasks() builds the -32021 the suite expects when a
  // task-required tool is called without it.
  const auto hasTasks = [](Ctx& c) { return tasks::clientSupports(c); };

  auto missingTasks = [] {
    McpError error;
    error.code = McpErrorCode::MissingRequiredClientCapability;
    error.message = "client did not declare the 'io.modelcontextprotocol/tasks' extension";
    Json extensions = Json(R"({})");
    extensions.add(std::string(tasks::kExtensionId), Json(R"({})"));
    Json required = Json(R"({})");
    required.add("extensions", std::move(extensions));
    Json data;
    data.add("requiredCapabilities", std::move(required));
    error.data = std::move(data);
    return error;
  };

  // Creates a task and settles it from a detached worker. The worker polls so
  // tasks/cancel is observed promptly (the suite cancels a 60 s task and
  // expects `cancelled` well inside its poll window).
  const auto startTask =
      [store](int64_t seconds,
              std::function<Json(const tasks::TaskRecord&)> produce) {
        tasks::TaskOptions options;
        options.pollIntervalMs = 100;
        tasks::TaskRecord record = tasks::makeTask(options);
        const std::string id = record.taskId;
        store->put(record);
        std::thread([store, id, seconds, produce]() {
          const auto deadline =
              std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
          while (std::chrono::steady_clock::now() < deadline) {
            auto current = store->get(id);
            if (!current.has_value() || current->cancelRequested) {
              tasks::cancelTask(*store, id);
              return;
            }
            sleepMs(20);
          }
          auto current = store->get(id);
          if (!current.has_value()) return;
          if (current->cancelRequested) {
            tasks::cancelTask(*store, id);
            return;
          }
          tasks::finishTask(*store, id, produce(*current));
        }).detach();
        return record;
      };

  // Parks the (already created) task on `inputRequests` and completes it once
  // every key has been answered through tasks/update. Answers arrive in
  // TaskRecord::inputResponses (store-internal), which is what the worker polls.
  const auto parkForInput =
      [store](const std::string& id, Json inputRequests,
              std::vector<std::string> keys,
              std::function<Json(const tasks::TaskRecord&)> produce) {
        if (!tasks::requestTaskInput(*store, id, inputRequests)) return;
        std::thread([store, id, keys, produce]() {
          for (;;) {
            auto current = store->get(id);
            if (!current.has_value()) return;
            if (current->cancelRequested) {
              tasks::cancelTask(*store, id);
              return;
            }
            bool complete = current->inputResponses.isObject();
            for (const auto& key : keys) {
              if (complete && member(current->inputResponses, key).isNull()) {
                complete = false;
              }
            }
            if (complete) break;
            sleepMs(20);
          }
          tasks::finishTask(*store, id, produce(*store->get(id)));
        }).detach();
      };

  // greet: sync-only ("Hello, {name}!").
  b.tool(toolDef("greet", "Greets by name",
                 R"({"type":"object","properties":{"name":{"type":"string"}},
                     "required":["name"]})"),
         [](Ctx& c) -> McpResult<Json> {
           const Json name = member(c.params["arguments"], "name");
           return Ok(oneText("Hello, " + std::string(name.toString()) + "!"));
         });

  // slow_compute: task-supporting. seconds == 0 takes the immediate path.
  b.tool(toolDef("slow_compute", "Sleeps then returns a result",
                 R"({"type":"object","properties":{"seconds":{"type":"integer"},
                     "label":{"type":"string"}}})"),
         [startTask, hasTasks](Ctx& c) -> McpResult<Json> {
           const int64_t seconds = c.params["arguments"]["seconds"].isNumber()
                                       ? c.params["arguments"]["seconds"].toInt64()
                                       : 1;
           const std::string label = member(c.params["arguments"], "label").isString()
                                         ? std::string(c.params["arguments"]["label"].toString())
                                         : std::string("unnamed");
           if (!hasTasks(c)) {
             if (seconds > 0) sleepMs(static_cast<int>(seconds * 1000));
             return Ok(oneText("computed:" + label));
           }
           const auto record = startTask(seconds, [label](const tasks::TaskRecord&) {
             return oneText("computed:" + label);
           });
           return Ok(tasks::createTaskResult(record));
         });

  // failing_job: task support REQUIRED; settles as completed with an isError
  // result (a tool execution error is not a failed task).
  b.tool(toolDef("failing_job", "Always fails as a tool execution error",
                 R"({"type":"object"})"),
         [startTask, hasTasks, missingTasks](Ctx& c) -> McpResult<Json> {
           if (!hasTasks(c)) return Err(missingTasks());
           const auto record = startTask(0, [](const tasks::TaskRecord&) {
             Json content = Json("[]");
             content.push_back(textContent("intentional tool execution error"));
             Json result;
             result.add("content", std::move(content));
             result.add("isError", Json(true));
             return result;
           });
           return Ok(tasks::createTaskResult(record));
         });

  // protocol_error_job: task-supporting; settles as failed with a JSON-RPC
  // error object (a protocol-level error, not a tool execution error).
  b.tool(toolDef("protocol_error_job", "Fails with a protocol error",
                 R"({"type":"object"})"),
         [store, hasTasks, missingTasks](Ctx& c) -> McpResult<Json> {
           if (!hasTasks(c)) return Err(missingTasks());
           tasks::TaskOptions options;
           options.pollIntervalMs = 100;
           tasks::TaskRecord record = tasks::makeTask(options);
           const std::string id = record.taskId;
           store->put(record);
           std::thread([store, id]() {
             sleepMs(20);
             Json error;
             error.add("code", Json(static_cast<int64_t>(McpErrorCode::InternalError)));
             error.add("message", Json::str("intentional protocol error"));
             tasks::failTask(*store, id, std::move(error));
           }).detach();
           return Ok(tasks::createTaskResult(record));
         });

  // confirm_delete: task-supporting; parks on one elicitation input request.
  b.tool(toolDef("confirm_delete", "Asks for confirmation before deleting",
                 R"({"type":"object"})"),
         [startTask, parkForInput, store, hasTasks, missingTasks](Ctx& c) -> McpResult<Json> {
           if (!hasTasks(c)) return Err(missingTasks());
           tasks::TaskOptions options;
           options.pollIntervalMs = 100;
           tasks::TaskRecord record = tasks::makeTask(options);
           const std::string id = record.taskId;
           store->put(record);
           Json requests = Json(R"({})");
           Json elicit;
           elicit.add("method", Json::str("elicitation/create"));
           Json params;
           params.add("mode", Json::str("form"));
           params.add("message", Json::str("Delete everything?"));
           params.add("requestedSchema",
                      Json(R"({"type":"object","properties":{"confirm":{"type":"boolean"}},
                              "required":["confirm"]})"));
           elicit.add("params", std::move(params));
           requests.add("confirm", std::move(elicit));
           parkForInput(id, std::move(requests), {"confirm"},
                        [](const tasks::TaskRecord& record) {
                          const Json confirm =
                              member(member(record.inputResponses, "confirm"), "content");
                          return oneText(member(confirm, "confirm").isTrue()
                                             ? "deleted"
                                             : "cancelled by user");
                        });
           return Ok(tasks::createTaskResult(record));
         });

  // multi_input: two simultaneous input requests (partial answers must keep the
  // task in input_required until both arrive).
  b.tool(toolDef("multi_input", "Fans out two input requests",
                 R"({"type":"object"})"),
         [store, parkForInput, hasTasks, missingTasks](Ctx& c) -> McpResult<Json> {
           if (!hasTasks(c)) return Err(missingTasks());
           tasks::TaskOptions options;
           options.pollIntervalMs = 100;
           tasks::TaskRecord record = tasks::makeTask(options);
           const std::string id = record.taskId;
           store->put(record);
           Json requests = Json(R"({})");
           for (const char* key : {"first", "second"}) {
             Json elicit;
             elicit.add("method", Json::str("elicitation/create"));
             Json params;
             params.add("mode", Json::str("form"));
             params.add("message", Json::str(std::string("Value for ") + key + "?"));
             params.add("requestedSchema",
                        Json(R"({"type":"object","properties":{"v":{"type":"string"}}})"));
             elicit.add("params", std::move(params));
             requests.add(std::string(key), std::move(elicit));
           }
           parkForInput(id, std::move(requests), {"first", "second"},
                        [](const tasks::TaskRecord& record) {
                          const Json first =
                              member(member(record.inputResponses, "first"), "content");
                          const Json second =
                              member(member(record.inputResponses, "second"), "content");
                          return oneText("first=" + std::string(member(first, "v").toString()) +
                                         " second=" + std::string(member(second, "v").toString()));
                        });
           return Ok(tasks::createTaskResult(record));
         });

  // test_tool_with_task: task support REQUIRED. Round 1 answers MRTR
  // (InputRequiredResult), round 2 - with inputResponses echoed back - creates
  // the task, whose result carries the gathered name.
  b.tool(toolDef("test_tool_with_task", "Gathers input, then runs as a task",
                 R"({"type":"object"})"),
         [startTask, hasTasks, missingTasks](Ctx& c) -> McpResult<Json> {
           if (!hasTasks(c)) return Err(missingTasks());
           Json requests = Json(R"({})");
           Json elicit;
           elicit.add("method", Json::str("elicitation/create"));
           Json params;
           params.add("mode", Json::str("form"));
           params.add("message", Json::str("Your name?"));
           params.add("requestedSchema",
                      Json(R"({"type":"object","properties":{"user_name":{"type":"string"}},
                              "required":["user_name"]})"));
           elicit.add("params", std::move(params));
           requests.add("user_name", std::move(elicit));

           const Json responses = member(c.params, "inputResponses");
           if (!responses.isObject()) {
             return c.inputRequired(std::move(requests), "");
           }
           const Json content = member(member(responses, "user_name"), "content");
           // Read the gathered value leniently: the client echoes what the user
           // typed under the schema's key, but harnesses have also been seen to
           // answer with a generic `name` field.
           std::string name = "anonymous";
           if (content.isObject()) {
             for (const char* key : {"user_name", "name"}) {
               const Json value = member(content, key);
               if (value.isString()) {
                 name = std::string(value.toString());
                 break;
               }
             }
           }
           const auto record = startTask(0, [name](const tasks::TaskRecord&) {
             return oneText("task completed for " + name);
           });
           return Ok(tasks::createTaskResult(record));
         });

  // ---- resources -----------------------------------------------------------
  auto textResource = [](const char* uri, const char* name, const char* mime,
                         const char* text) {
    return [uri, name, mime, text](Ctx&) -> McpResult<Json> {
      Json c;
      c.add("uri", Json::str(uri));
      c.add("mimeType", Json::str(mime));
      c.add("text", Json::str(text));
      Json contents = Json("[]");
      contents.push_back(std::move(c));
      Json result;
      result.add("contents", std::move(contents));
      (void)name;
      return Ok(std::move(result));
    };
  };

  Json staticText(R"({"uri":"test://static-text","name":"static text","mimeType":"text/plain"})");
  b.resource(staticText, textResource("test://static-text", "static text", "text/plain",
                                      "This is the content of the static text resource."));

  Json staticBinary(R"({"uri":"test://static-binary","name":"static binary","mimeType":"image/png"})");
  b.resource(staticBinary, [](Ctx&) -> McpResult<Json> {
    Json c;
    c.add("uri", Json::str("test://static-binary"));
    c.add("mimeType", Json::str("image/png"));
    c.add("blob", Json::str(kPngB64));
    Json contents = Json("[]");
    contents.push_back(std::move(c));
    Json result;
    result.add("contents", std::move(contents));
    return Ok(std::move(result));
  });

  Json watched(R"({"uri":"test://watched-resource","name":"watched","mimeType":"text/plain"})");
  b.resource(watched, textResource("test://watched-resource", "watched", "text/plain",
                                   "This resource is watched for updates."));

  Json tmpl(R"({"uriTemplate":"test://template/{id}/data","name":"template data",
                "mimeType":"application/json"})");
  b.resourceTemplate(tmpl, [](Ctx& c) -> McpResult<Json> {
    std::vector<std::pair<std::string, std::string>> vars;
    const std::string uri = c.params["uri"].toString();
    matchUriTemplate("test://template/{id}/data", uri, &vars);
    const std::string id = vars.empty() ? std::string() : vars[0].second;
    Json payload;
    payload.add("id", Json::str(id));
    payload.add("templateTest", Json(true));
    payload.add("data", Json::str("Data for ID: " + id));
    Json c2;
    c2.add("uri", Json::str(uri));
    c2.add("mimeType", Json::str("application/json"));
    c2.add("text", Json::str(payload.toString()));
    Json contents = Json("[]");
    contents.push_back(std::move(c2));
    Json result;
    result.add("contents", std::move(contents));
    return Ok(std::move(result));
  });

  // ---- prompts -------------------------------------------------------------
  Json simplePrompt(R"({"name":"test_simple_prompt","description":"A simple prompt"})");
  b.prompt(simplePrompt, [](Ctx&) -> McpResult<Json> {
    Json msg;
    msg.add("role", Json::str("user"));
    msg.add("content", textContent("This is a simple prompt for testing."));
    Json messages = Json("[]");
    messages.push_back(std::move(msg));
    Json result;
    result.add("messages", std::move(messages));
    return Ok(std::move(result));
  });

  Json argsPrompt(R"({"name":"test_prompt_with_arguments","description":"Prompt with arguments",
      "arguments":[{"name":"arg1","description":"First","required":true},
                   {"name":"arg2","description":"Second","required":true}]})");
  b.prompt(argsPrompt, [](Ctx& c) -> McpResult<Json> {
    const Json args = member(c.params, "arguments");
    const std::string a1 = member(args, "arg1").toString();
    const std::string a2 = member(args, "arg2").toString();
    Json msg;
    msg.add("role", Json::str("user"));
    msg.add("content",
            textContent("Prompt with arguments: arg1='" + a1 + "', arg2='" + a2 + "'"));
    Json messages = Json("[]");
    messages.push_back(std::move(msg));
    Json result;
    result.add("messages", std::move(messages));
    return Ok(std::move(result));
  });

  Json embedPrompt(R"({"name":"test_prompt_with_embedded_resource",
      "description":"Prompt with an embedded resource",
      "arguments":[{"name":"resourceUri","description":"URI to embed","required":true}]})");
  b.prompt(embedPrompt, [](Ctx& c) -> McpResult<Json> {
    const Json args = member(c.params, "arguments");
    const std::string uri = member(args, "resourceUri").toString();
    Json messages = Json("[]");
    Json m1;
    m1.add("role", Json::str("user"));
    m1.add("content", embeddedResource(uri, "text/plain",
                                       "Embedded resource content for testing."));
    messages.push_back(std::move(m1));
    Json m2;
    m2.add("role", Json::str("user"));
    m2.add("content", textContent("Please process the embedded resource above."));
    messages.push_back(std::move(m2));
    Json result;
    result.add("messages", std::move(messages));
    return Ok(std::move(result));
  });

  Json imagePrompt(R"({"name":"test_prompt_with_image","description":"Prompt with an image"})");
  b.prompt(imagePrompt, [](Ctx&) -> McpResult<Json> {
    Json messages = Json("[]");
    Json m1;
    m1.add("role", Json::str("user"));
    m1.add("content", imageContent(kPngB64, "image/png"));
    messages.push_back(std::move(m1));
    Json m2;
    m2.add("role", Json::str("user"));
    m2.add("content", textContent("Please analyze the image above."));
    messages.push_back(std::move(m2));
    Json result;
    result.add("messages", std::move(messages));
    return Ok(std::move(result));
  });

  // ---- completion ----------------------------------------------------------
  b.completions([](Ctx&) -> McpResult<Json> {
    Json completion;
    Json values = Json("[]");
    values.push_back(Json::str("paris"));
    values.push_back(Json::str("park"));
    values.push_back(Json::str("party"));
    completion.add("values", std::move(values));
    completion.add("total", Json(static_cast<int64_t>(3)));
    completion.add("hasMore", Json(false));
    Json result;
    result.add("completion", std::move(completion));
    return Ok(std::move(result));
  });

  auto server = b.build();

#if defined(MCP_HAS_HTTP)
  if (http) {
    mcp::http::HttpServerTransport transport(*server, "/mcp");
    auto bound = transport.bind("127.0.0.1", port);
    if (!bound.has_value()) {
      std::cerr << "[conformance] bind failed: " << bound.error().message << std::endl;
      return 1;
    }
    std::cerr << "[conformance] serving on http://127.0.0.1:" << port << "/mcp" << std::endl;
    const McpResult<void> r = transport.run();
    return r.has_value() ? 0 : 1;
  }
#else
  if (http) {
    std::cerr << "[conformance] built without HTTP support" << std::endl;
    return 1;
  }
#endif

  StdioTransport transport;
  const McpResult<void> r = server->run(transport);
  return r.has_value() ? 0 : 1;
}
