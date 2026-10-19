// Installed-package consumer smoke test.
//
// Instantiating the HTTP server transport forces the linker to resolve the
// platform libraries the compiled libmcp objects need (ws2_32 on Windows,
// pthread_* / std::thread on POSIX). Before P0-3 the installed package did not
// propagate those, so this program failed to link even though the in-tree
// build worked.

#include <mcp/mcp.hpp>
#include <mcp/transport_http.hpp>

#include <cstdio>
#include <string>

int main() {
  auto server = mcp::server::Builder()
                    .name("mcp-consumer")
                    .version(std::string(mcp::kSdkVersion))
                    .build();

#if defined(MCP_HAS_HTTP)
  mcp::http::HttpServerTransport transport(*server);
  auto port = transport.bind("127.0.0.1");
  if (!port.has_value() || *port <= 0) {
    std::fprintf(stderr, "mcp consumer: HTTP bind failed\n");
    return 1;
  }
  transport.stop();
  std::printf("mcp consumer ok: HTTP transport bound on %d\n", *port);
#else
  std::printf("mcp consumer ok: stdio-only build\n");
#endif
  std::printf("mcp package version: %s\n", std::string(mcp::kSdkVersion).c_str());
  return 0;
}
