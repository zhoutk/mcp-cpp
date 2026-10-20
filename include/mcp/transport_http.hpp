#pragma once
// ---------------------------------------------------------------------------
// mcp/transport_http.hpp - Streamable HTTP binding (2026-07-28 shape).
//
// Compiled only when MCP_HAS_HTTP is defined (httplib available and
// MCP_DISABLE_HTTP off).
//
// Server side (HttpServerTransport):
//  - single POST endpoint; GET/DELETE answered 405 (no GET stream in 2026)
//  - header mirror validation: MCP-Protocol-Version, Mcp-Method, Mcp-Name;
//    mismatch/missing -> 400 + HeaderMismatch (-32020)
//  - unknown method -> 404 + -32601; notifications -> 202 no body
//  - requests answered application/json (SSE is used for subscriptions only)
//  - subscriptions/listen upgrades the response to an SSE stream that
//    receives notifications published via Server::notify* APIs
//
// Client side (HttpClientTransport):
//  - one POST per message with Accept: application/json, text/event-stream
//    and the mirrored request headers; x-mcp-header mirroring is done by
//    mcp::client::Client for tools/call (setMirroredHeaders)
//  - 202 yields no message; application/json / SSE bodies are pushed into
//    the read queue so the same ITransport read loop serves both bindings
// ---------------------------------------------------------------------------

#include <mcp/error.hpp>
#include <mcp/json.hpp>
#include <mcp/jsonrpc.hpp>
#include <mcp/server.hpp>
#include <mcp/transport.hpp>

#include <deque>
#include <functional>
#include <memory>
#include <string>

#if defined(MCP_HAS_HTTP)

namespace httplib {
class Server;
class Client;
}  // namespace httplib

namespace mcp {
namespace http {

// Implementation lives in transport_http.cpp; the class intentionally keeps
// httplib out of this header.
class HttpServerTransport {
 public:
  // Validates the Origin header. Empty string = no Origin header sent
  // (non-browser clients). Return false -> 403.
  using OriginValidator = std::function<bool(const std::string& origin)>;

  explicit HttpServerTransport(server::Server& server, std::string endpoint = "/mcp");
  ~HttpServerTransport();

  HttpServerTransport(const HttpServerTransport&) = delete;
  HttpServerTransport& operator=(const HttpServerTransport&) = delete;

  // Binds without accepting; returns the bound port (use port 0 for an
  // ephemeral port). run() then accepts on that socket.
  McpResult<int> bind(const std::string& host = "127.0.0.1");
  // Binds a specific port (0 = ephemeral, same as the overload above) without
  // accepting; run() then serves. Use this for a fixed deployment port.
  McpResult<int> bind(const std::string& host, int port);
  // Accepts until stop(); blocking (after bind()).
  McpResult<void> run();
  // Convenience for tests/tools: binds (port 0 allowed) and serves on a
  // background thread. Returns once the listener is accepting.
  McpResult<void> runAsync(const std::string& host = "127.0.0.1", int port = 8080);
  void stop();

  void setOriginValidator(OriginValidator validator);

  // D11: resource-server authentication seam. Called for every POST with the
  // request's Authorization header (empty when absent) and the Mcp-Session-Id
  // (empty on 2026 servers). Return true to accept; false -> HTTP 401 with
  // the optional challenge in WWW-Authenticate. The transport never parses
  // tokens itself - an embedder plugs in its JWT/opaque-token validation and
  // audience checks here.
  using TokenValidator = std::function<bool(const std::string& authorizationHeader,
                                            std::string& wwwAuthenticateChallenge)>;
  void setTokenValidator(TokenValidator validator);

  // True while the listener is accepting.
  bool running() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// ---- client side -------------------------------------------------------------

class HttpClientTransport final : public ITransport {
 public:
  HttpClientTransport(std::string host, int port, std::string endpoint = "/mcp");
  ~HttpClientTransport() override;

  // ITransport: write() starts the POST on a background thread (a response may
  // be a long-lived SSE stream) and returns immediately; the response's message
  // lines reach read() through a queue. read() is safe to call concurrently
  // with write(); write() itself is meant for a single client loop.
  McpResult<std::optional<std::string>> read() override;
  McpResult<void> write(std::string_view message) override;
  McpResult<void> flush() override;

  // Adds/replaces a header sent with every POST (e.g. Authorization), or
  // per-request mirrored headers (Mcp-Param-*). An empty value is stored and
  // SENT as-is (the mirrored-header contract allows an empty parameter); use
  // removeHeader() to drop one.
  void setHeader(std::string name, std::string value) override;
  void removeHeader(const std::string& name) override;

  // The protocol revision echoed on the GET listening stream's
  // MCP-Protocol-Version header. Mirrors Client::setProtocolVersion; the
  // fixture calls both. Defaults to the latest revision.
  void setProtocolVersion(ProtocolVersion v);

  // Report an HTTP authentication challenge (a 401/403 carrying a
  // WWW-Authenticate header) so the caller can run the OAuth flow and set the
  // Authorization header. The transport never authenticates by itself.
  using AuthChallengeHandler = std::function<void(int status, const std::string& challenge)>;
  void setAuthChallengeHandler(AuthChallengeHandler handler);

  // Terminates all open subscription streams (background SSE connections)
  // and waits for their threads to finish. Safe to call from any thread;
  // idempotent. Called automatically by the destructor. After this returns,
  // no further notification lines will be queued.
  void closeSubscriptions() override;

  // Makes the next (or a currently blocked) read() return promptly with an
  // error, without closing anything. One shot; the read consumes it. This is
  // what Client request timeouts and cancelCurrentRequest() drive.
  void interruptRead() override;
  void clearReadInterrupt() override;

 private:
  // Stops in-flight request streams and waits for their threads. Destructor
  // only: closing these mid-call would drop the response the caller waits for.
  void closeRequestStreams();

  // Queues a JSON-RPC error response for the request carried by `wire`, so a
  // transport-level failure surfaces to the caller as a normal error result.
  void queueSyntheticError(const std::string& wire, JsonRpcErrorValue v);

  // Forwards a 401/403 WWW-Authenticate challenge to the registered handler.
  void reportAuthChallenge(int status, const std::string& challenge);

  // Opens the GET event stream (once) - the 2025-era server-to-client channel.
  // A 405 answer (2026-style servers) marks it unsupported and it stays closed.
  void openListenStreamOnce();
  // Stops the listening stream and its reconnect loop. Destructor only.
  void closeListenStreams();

  McpResult<void> postOne(std::string_view message);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace http
}  // namespace mcp

#endif  // MCP_HAS_HTTP
