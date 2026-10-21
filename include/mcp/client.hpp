#pragma once
// ---------------------------------------------------------------------------
// mcp/client.hpp - blocking MCP client over any ITransport.
//
// Stateless per the 2026-07-28 spec: no connect/handshake - every request
// embeds the client's protocol version, capabilities and identity in
// params._meta (see types.hpp encodeRequestMeta). Request/response
// correlation is by JSON-RPC id; notifications seen on the wire while
// waiting are handed to the onNotification callback.
//
// MRTR (multi round-trip requests): a result whose resultType is
// "input_required" is surfaced as-is; callers gather the requested inputs and
// retry the same method with callTool(..., inputResponses) - the client
// assigns a fresh request id for the retry (basic/patterns/mrtr.md).
// ---------------------------------------------------------------------------

#include <mcp/error.hpp>
#include <mcp/json.hpp>
#include <mcp/jsonrpc.hpp>
#include <mcp/transport.hpp>
#include <mcp/types.hpp>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mcp {
namespace client {

class Client {
 public:
  using NotificationSink = std::function<void(const JsonRpcNotification&)>;

  Client(std::string clientName, std::string clientVersion,
         Json clientCapabilities = Json(R"({})"));

  // The transport must outlive the client. Not owned.
  void connect(ITransport& transport);

  // Wire metadata attached to every outgoing request.
  void setProtocolVersion(ProtocolVersion v);

  // Async events observed while waiting for a response.
  void onNotification(NotificationSink sink);

  // --- protocol surface ----------------------------------------------------

  // server/discover: supportedVersions, capabilities, instructions, ttl.
  McpResult<Json> discover();

  // E2: era auto-negotiation. Probes server/discover, then classifies the
  // answer:
  //  - a modern result -> "modern" (2026-07-28 stateless protocol); the
  //    protocol version is pinned to the newest revision both sides speak
  //    (from the result's supportedVersions).
  //  - -32022 UnsupportedProtocolVersion carrying data.supported -> retry
  //    discover once with the newest mutually supported revision ("pin the
  //    version"), still modern.
  //  - any other error (method-not-found, transport error, timeout) -> fall
  //    back to the legacy `initialize` handshake at `legacyVersion`.
  // On success, setProtocolVersion() reflects the negotiated revision.
  // `probeTimeout` bounds the probe when no request timeout is configured; a
  // transport that cannot interrupt a blocked read (stdio) ignores it.
  struct EraNegotiation {
    bool modern = false;          // false = legacy initialize handshake
    std::string protocolVersion;  // negotiated wire revision
    Json result;                  // discover result (modern) / initialize result
  };
  McpResult<EraNegotiation> negotiateEra(
      std::string_view legacyVersion = "2025-11-25",
      std::chrono::milliseconds probeTimeout = std::chrono::milliseconds(5000));

  // tools/list with optional pagination cursor.
  McpResult<Json> listTools(const std::string& cursor = "");

  // tools/call. `inputResponses` (from a previous input_required result)
  // is merged into params for the MRTR retry round. Pass `progressToken` to
  // opt into progress: the server's notifications/progress messages are
  // delivered to onNotification() while this call blocks.
  McpResult<Json> callTool(const std::string& name, const Json& arguments,
                           const Json& inputResponses = Json(nullptr),
                           const std::optional<RequestId>& progressToken = std::nullopt);

  McpResult<Json> readResource(const std::string& uri);
  // resources/list with optional pagination cursor.
  McpResult<Json> listResources(const std::string& cursor = "");
  // resources/templates/list with optional pagination cursor: the URI
  // templates the server serves parametric reads from.
  McpResult<Json> listResourceTemplates(const std::string& cursor = "");
  // prompts/list with optional pagination cursor.
  McpResult<Json> listPrompts(const std::string& cursor = "");
  McpResult<Json> getPrompt(const std::string& name, const Json& arguments = Json(nullptr));

  // completion/complete: argument completion for a prompt or resource.
  // `ref` is a {type:"ref/prompt",name} or {type:"ref/resource",uri} object.
  McpResult<Json> complete(const Json& ref, const Json& argument);

  // --- MRTR (basic/patterns/mrtr.md) ---------------------------------------
  // Retries a request that previously answered with an InputRequiredResult.
  // `previous` is that result; its `requestState` (if any) is echoed back
  // verbatim and `inputResponses` is merged into params. The retry is a fresh
  // JSON-RPC request (new id), as the spec requires.
  McpResult<Json> retry(std::string_view method, const Json& extraParams, const Json& previous,
                        const Json& inputResponses);

  // tools/call convenience wrapper over retry(): same tool and arguments as the
  // first attempt, plus the gathered inputs.
  McpResult<Json> retryToolCall(const std::string& name, const Json& arguments,
                                const Json& previous, const Json& inputResponses);

  // Answers an InputRequiredResult: receives the server's inputRequests object
  // and returns the inputResponses object keyed the same way.
  using InputHandler = std::function<McpResult<Json>(const Json& inputRequests)>;

  // tools/call that fulfils MRTR automatically: each input_required result is
  // passed to `handle`, and the retry echoes requestState and inputResponses.
  // Returns the first complete result. Fails with InternalError once
  // `maxRounds` input rounds have been used, or with the handler's error.
  // E7: the cap is bounded by kMaxInputRounds (a server that keeps answering
  // input_required cannot drive an unbounded loop whatever the caller passed).
  static constexpr int kDefaultMaxInputRounds = 10;
  static constexpr int kMaxInputRounds = 100;
  McpResult<Json> callToolWithInput(const std::string& name, const Json& arguments,
                                    const InputHandler& handle,
                                    int maxRounds = kDefaultMaxInputRounds);

  // subscriptions/listen: opens a long-lived notification stream. Returns
  // after the acknowledged notification; subsequent notifications flow
  // through onNotification, each carrying io.modelcontextprotocol/
  // subscriptionId in _meta. `filter` matches the spec's Notification Filter
  // object (toolsListChanged/promptsListChanged/resourcesListChanged/
  // resourceSubscriptions).
  McpResult<Json> subscribe(const Json& filter);
  // Callback for graceful server-side subscription closure (the eventual
  // result response carrying the subscriptionId in _meta).
  void onSubscriptionClosed(std::function<void(const std::string& subscriptionId)> cb);

  // Terminates all subscription streams opened through subscribe() and waits
  // for their background threads to exit. No further notifications will be
  // delivered after this returns. Safe to call multiple times (idempotent);
  // a no-op when the transport does not expose subscription control.
  void closeSubscriptions();

  // Generic request when the SDK has no dedicated wrapper yet. `progressToken`
  // (optional) opts this request into notifications/progress delivery.
  McpResult<Json> request(std::string_view method, const Json& extraParams,
                          const std::optional<RequestId>& progressToken = std::nullopt);

  // --- client-side response cache (E3) ---------------------------------------
  // CacheableResult clients SHOULD honor `ttlMs` + `cacheScope`. The cache is
  // on by default and covers `tools/list`, `resources/list`,
  // `resources/templates/list`, `prompts/list` and `resources/read`:
  //  - an entry is only stored when the result carries a positive `ttlMs` and
  //    a `cacheScope` of "public" or "private"; a missing/zero ttl or an
  //    unknown scope means "do not cache" (conservative, spec-safe).
  //  - `server/discover` is NEVER cached (it is the negotiation/version probe
  //    and must always reach the server).
  //  - requests carrying a progressToken, and results that are not complete,
  //    are never cached.
  // A hit returns the stored result without touching the transport.
  void setResponseCache(bool enabled);
  void clearResponseCache();

  // --- legacy-era client (pre-2026) -----------------------------------------
  // Runs the initialize handshake: sends the legacy `initialize` request
  // (protocolVersion/capabilities/clientInfo at the params ROOT - no _meta
  // envelope), then the notifications/initialized notification. Required by
  // clients that must also serve pre-2026 servers, e.g. conformance fixtures.
  McpResult<Json> initialize(std::string_view protocolVersion,
                             const Implementation& clientInfo);

  // Handles a request the SERVER initiates on the wire (pre-2026 servers do
  // this: sampling/createMessage, elicitation/create, roots/list). 2026-07-28
  // replaced this with MRTR, so the modern path never invokes it. The handler
  // receives the method, the params and the request id, and returns the result
  // object to answer with (or an error, which becomes a JSON-RPC error
  // response). Without a handler, arriving server requests are ignored.
  using ServerRequestHandler = std::function<McpResult<Json>(
      const std::string& method, const Json& params, const RequestId& id)>;
  void onServerRequest(ServerRequestHandler handler);

  // Sends notifications/cancelled over the underlying transport (stdio
  // cancellation; on HTTP closing the stream is the cancellation signal).
  McpResult<void> cancel(const RequestId& requestId, const std::string& reason = "");

  // --- request timeout / cancellation (client side) --------------------------
  // Bounds each request/response exchange. 0 (the default) waits forever. When
  // the deadline passes, the blocked read() is interrupted and the in-flight
  // call fails with "request timed out". Requires a transport that implements
  // interruptRead() (HttpClientTransport does; blocking stdio streams cannot
  // honor a deadline). The timeout covers ONE round trip - callToolWithInput
  // re-arms it per round.
  void setRequestTimeout(std::chrono::milliseconds timeout);
  std::chrono::milliseconds requestTimeout() const;
  // Fails the in-flight request promptly (the same interrupt mechanism as the
  // timeout). The client is sequential, so there is at most one request in
  // flight; with none, the interrupt is consumed by the next request's first
  // read - which is why exchange() clears stale interrupts on entry.
  void cancelCurrentRequest();

  ~Client();

 private:
  Json buildParams(const Json& extraParams, const std::optional<RequestId>& progressToken) const;
  // One request/response exchange with the params already final. Callers pick
  // the envelope: request() wraps _meta (modern), initialize() sends the legacy
  // root-level metadata instead.
  McpResult<Json> exchange(std::string_view method, const Json& params);

  ITransport* transport_ = nullptr;
  std::string clientName_;
  std::string clientVersion_;
  Json clientCapabilities_;
  std::string protocolVersion_ = std::string(toString(ProtocolVersion::Latest));
  NotificationSink onNotification_;
  ServerRequestHandler onServerRequest_;
  std::function<void(const std::string&)> onSubscriptionClosed_;
  bool awaitAck_ = false;  // subscribe() waits for the acknowledgment
  int64_t nextId_ = 1;

  // Tool definitions from the last listTools(): source for the x-mcp-header
  // mirroring required of HTTP clients (basic/transports/streamable-http.md).
  std::map<std::string, Json> toolSchemas_;
  std::vector<std::string> lastParamHeaders_;  // cleared before each call
  void mirrorHeaderParams(const std::string& toolName, const Json& arguments);

  // E3 response cache. Entries are keyed by method + the request's explicit
  // params; expiries use steady_clock (immune to wall-clock jumps). The cache
  // is bounded: the least recently used entry is evicted beyond kMaxEntries.
  static constexpr size_t kMaxCacheEntries = 256;
  struct CacheEntry {
    Json result;
    std::chrono::steady_clock::time_point expires;
    uint64_t lastUsed = 0;
  };
  bool responseCache_ = true;
  uint64_t cacheClock_ = 0;
  std::map<std::string, CacheEntry> responseCacheEntries_;
  std::optional<Json> cacheLookup(const std::string& key);
  void cacheStore(const std::string& key, const Json& result);
  // Drops every entry of one cached method (list-change notifications).
  void invalidateCacheFor(std::string_view cachedMethod);

  // Request timeout. A watchdog thread (lazily started by setRequestTimeout)
  // fires transport_->interruptRead() when the in-flight exchange passes its
  // deadline; the blocked read() then fails and exchange() reports the
  // timeout. Guarded by watchdogMutex_.
  std::chrono::milliseconds requestTimeout_{0};
  std::thread watchdog_;
  std::mutex watchdogMutex_;
  std::condition_variable watchdogCv_;
  bool watchdogStop_ = false;
  bool requestActive_ = false;
  std::chrono::steady_clock::time_point deadline_;
  void watchdogLoop();
};

}  // namespace client
}  // namespace mcp
