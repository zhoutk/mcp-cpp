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
             Json params;
             params.add("level", Json::str("info"));
             params.add("data", Json::str(messages[i]));
             c.notify("notifications/message", std::move(params));
           }
           return Ok(oneText("Tool execution completed"));
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
  // $schema, $defs and additionalProperties survive in the tool listing.
  b.tool(Json(R"({
        "name": "json_schema_2020_12_tool",
        "description": "Tool with JSON Schema 2020-12 features",
        "inputSchema": {
          "$schema": "https://json-schema.org/draft/2020-12/schema",
          "type": "object",
          "$defs": {
            "address": {
              "type": "object",
              "properties": { "street": {"type":"string"}, "city": {"type":"string"} }
            }
          },
          "properties": {
            "name": {"type":"string"},
            "address": {"$ref": "#/$defs/address"}
          },
          "additionalProperties": false
        }
      })"),
         [](Ctx&) -> McpResult<Json> { return Ok(oneText("ok")); });

  // The sampling / elicitation tools need a server->client REQUEST, which
  // 2026-07-28 replaced with MRTR. They are intentionally absent here; the
  // scenarios are listed in conformance/expected-failures.yaml.

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
