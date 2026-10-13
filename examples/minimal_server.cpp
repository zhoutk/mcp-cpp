// minimal_server - smallest useful MCP server.
//
// Serves as:
//  - the stdio end-to-end test peer (tests/test_stdio_e2e.cpp spawns it)
//  - a manual target for the MCP Inspector:
//      npx @modelcontextprotocol/inspector ./minimal_server.exe
//  - an HTTP target for the conformance suite (see docs/CONFORMANCE.md):
//      ./minimal_server.exe --http 8080
//        npx @modelcontextprotocol/conformance server --url http://127.0.0.1:8080/mcp

#include <mcp/mcp.hpp>

#include <cstdlib>
#include <iostream>
#include <string>

using namespace mcp;
using namespace mcp::server;

int main(int argc, char** argv) {
  int httpPort = 0;  // 0 = stdio
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--http" && i + 1 < argc) {
      httpPort = std::atoi(argv[++i]);
    } else if (arg == "--http") {
      httpPort = 8080;
    }
  }

  Builder b;
  b.name("minimal").version("0.1.0")
      .instructions("Echoes messages; use for smoke-testing MCP clients.");

  b.simpleTool("echo", "Returns the input text unchanged.",
               [](Ctx& c) -> McpResult<Json> {
                 const Json args = c.params["arguments"];
                 const Json text = args["text"];
                 if (!text.isString()) {
                   return Err(McpErrorCode::InvalidParams, "arguments.text must be a string");
                 }
                 Json result;
                 Json content = Json("[]");
                 Json item;
                 item.add("type", Json::str("text"));
                 item.add("text", Json::str(text.toString()));
                 content.push_back(std::move(item));
                 result.add("content", std::move(content));
                 return Ok(std::move(result));
               });

  b.simpleTool("add", "Adds two integers.",
               [](Ctx& c) -> McpResult<Json> {
                 const Json args = c.params["arguments"];
                 const Json a = args["a"];
                 const Json bv = args["b"];
                 if (!a.isNumber() || !bv.isNumber()) {
                   return Err(McpErrorCode::InvalidParams, "arguments.a and arguments.b must be numbers");
                 }
                 const int64_t sum = a.toInt64() + bv.toInt64();
                 Json result;
                 Json content = Json("[]");
                 Json item;
                 item.add("type", Json::str("text"));
                 item.add("text", Json::str(std::to_string(sum)));
                 content.push_back(std::move(item));
                 result.add("content", std::move(content));
                 return Ok(std::move(result));
               });

  auto server = b.build();

#if defined(MCP_HAS_HTTP)
  if (httpPort > 0) {
    mcp::http::HttpServerTransport transport(*server, "/mcp");
    auto bound = transport.bind("127.0.0.1", httpPort);
    if (!bound.has_value()) {
      std::cerr << "[minimal] bind failed: " << bound.error().message << std::endl;
      return 1;
    }
    std::cerr << "[minimal] serving Streamable HTTP on http://127.0.0.1:" << httpPort
              << "/mcp" << std::endl;
    const McpResult<void> r = transport.run();
    if (!r.has_value()) {
      std::cerr << "[minimal] transport error: " << r.error().message << std::endl;
      return 1;
    }
    return 0;
  }
#else
  if (httpPort > 0) {
    std::cerr << "[minimal] built without HTTP support (MCP_DISABLE_HTTP)" << std::endl;
    return 1;
  }
#endif

  StdioTransport transport;
  const McpResult<void> r = server->run(transport);
  if (!r.has_value()) {
    std::cerr << "[minimal] transport error: " << r.error().message << std::endl;
    return 1;
  }
  return 0;
}
