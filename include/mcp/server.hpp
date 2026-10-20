#pragma once
// ---------------------------------------------------------------------------
// mcp/server.hpp - MCP server runtime + builder.
//
// Design (specification 2026-07-28):
//  - stateless dispatch: every request validates its own params._meta
//    (protocolVersion + clientCapabilities required) and is served
//    independently; no session state exists (basic/index.md "Statelessness")
//  - version gate: a request whose _meta version is not in the supported set
//    is answered with UnsupportedProtocolVersionError (-32022) listing the
//    supported versions (basic/versioning.md)
//  - capability gating: calling tools/* without advertising the tools
//    capability is MethodNotFound (-32601) per the schema's
//    MethodNotFoundError documentation; an unknown TOOL is InvalidParams
//    (-32602)
//  - single-threaded dispatch (PLAN.md Update 1): every processLine() call is
//    serialized by dispatchMutex_, so a stream transport with a worker pool
//    (the HTTP transport) is safe and handlers still run one at a time;
//    cancellation is a queryable flag keyed by request id, set by
//    notifications/cancelled
//  - handlers own their result object; the server injects
//    resultType:"complete" when the handler forgot it (spec requires it on
//    every result)
//
// Builder mirrors the portable shape of the Tier-1 SDKs
// (Rust Server::builder().add_tool / TS McpServer.registerTool).
// ---------------------------------------------------------------------------

#include <mcp/error.hpp>
#include <mcp/json.hpp>
#include <mcp/jsonrpc.hpp>
#include <mcp/request_state.hpp>
#include <mcp/transport.hpp>
#include <mcp/types.hpp>

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace mcp {
namespace server {

// ---- handler surface ---------------------------------------------------------

// Per-request context handed to handlers. References stay valid for the
// duration of the handler call.
struct Ctx {
  const Json& params;              // full request params (validated object)
  const RequestMeta& meta;         // decoded params._meta
  const std::function<bool()>& isCancelled;  // true once notifications/cancelled names this request id

  // Emits notifications/progress for THIS request (basic/patterns/progress.md).
  // A no-op when the request carried no params._meta.progressToken - the spec
  // only allows progress notifications for tokens from an active request.
  // `progress` MUST increase across calls (the SDK does not enforce ordering).
  // Pass `total < 0` to omit the total; an empty `message` is omitted.
  std::function<void(double progress, double total, const std::string& message)> progress;

  // Emits an arbitrary notification object on the active transport, e.g.
  // notifications/message for logging (2026-07-28 carries log level in _meta).
  // Best-effort: write errors are ignored. A no-op on one-shot transports.
  std::function<void(const std::string& method, Json params)> notify;

  // Structured logging (utilities/logging): emits notifications/message with
  // {level, data, logger} plus the level echoed in _meta
  // (io.modelcontextprotocol/logLevel) for 2026 clients. `level` is one of
  // debug/info/notice/warning/error/critical/alert/emergency; the message is
  // DROPPED when its severity ranks below the request's _meta logLevel (if
  // the client sent one) or the server's configured threshold (Builder::
  // logLevel / legacy logging/setLevel). Unknown levels pass unfiltered.
  // Best-effort like notify().
  std::function<void(const std::string& level, const std::string& message)> log;

  // Builds an InputRequiredResult (MRTR, basic/patterns/mrtr.md) for this
  // request. `inputRequests` maps server-assigned ids to request objects
  // (elicitation/create, sampling/createMessage, roots/list); `requestState` is
  // opaque server state the client echoes back on retry. Enforces the spec's
  // server requirements: at least one of the two must be present, every
  // inputRequests method must be one of the three valid ones, and the client
  // must have declared the matching capability. Returns Err(InternalError) when
  // a requirement is violated. Only tools/call, prompts/get and resources/read
  // may answer with the result it builds.
  std::function<McpResult<Json>(Json inputRequests, std::string requestState)> inputRequired;

  // Convenience for the common single-elicitation case: builds an
  // InputRequiredResult containing one elicitation/create request.
  std::function<McpResult<Json>(std::string message, Json requestedSchema,
                                std::string requestState)> elicit;

  // Verified MRTR requestState (requires Builder::requestStateCodec). The state
  // is bound to this method and tool name, prompt name, or resource URI, so a
  // state issued for one call cannot be replayed against another. Returns
  // nullopt when the client echoed no state (first round), the verified payload
  // when valid, and Err(InvalidParams) with a generic message when the state is
  // forged, expired, or mismatched. The generic message is safe to return to the
  // client; the cause is not revealed. Err(InternalError) when no codec is
  // configured or the method cannot carry state.
  std::function<McpResult<std::optional<std::string>>()> openRequestState;

  // Seals `payload` into a requestState for the next round of this request.
  // Err(InternalError) when no codec is configured or the method cannot carry state.
  std::function<McpResult<std::string>(std::string payload)> sealRequestState;
};

// ---- subscriptions -----------------------------------------------------------
// The filter mirrors the spec's Notification Filter object; every field the
// server honors is echoed in the acknowledgment.

struct SubscriptionFilter {
  bool toolsListChanged = false;
  bool promptsListChanged = false;
  bool resourcesListChanged = false;
  std::vector<std::string> resourceUris;  // notifications/resources/updated
};

// Receives one wire line per notification (or the graceful-closure result).
// Implementations must be safe to call from any thread that may publish.
using NotifSink = std::function<void(const std::string& line)>;

// Parses the spec's Notification Filter object from a subscriptions/listen
// params value; serializes a filter back (the acknowledged subset).
SubscriptionFilter parseSubscriptionFilter(const Json& params);
Json subscriptionFilterToJson(const SubscriptionFilter& filter);

// True when `method` has a server-side route (used by the HTTP transport for
// the 404-vs-405 distinction required by the spec).
bool isKnownMethod(std::string_view method);

// Matches `uri` against an RFC 6570 template (subset: literal runs, {var} for a
// non-empty run without '/', {+var} for one that may contain '/'). On a match,
// `vars` (when non-null) receives the extracted variable name/value pairs in
// template order. Used by the server for resources/read template routing and
// exposed for server authors who need the same matching.
bool matchUriTemplate(const std::string& tmpl, const std::string& uri,
                      std::vector<std::pair<std::string, std::string>>* vars);

// A handler returns the full result object (including resultType when it
// matters; the server injects "complete" when absent), or an error that
// becomes the JSON-RPC error response verbatim.
using Handler = std::function<McpResult<Json>(Ctx&)>;

// Sends one server-to-client notification (method + params) on the active
// transport. Best-effort: write failures are ignored.
using NotifyFn = std::function<void(const std::string& method, Json params)>;

// ---- result builders ---------------------------------------------------------
// Convenience constructors for the two shapes a tools/call handler usually
// returns (server/tools.md "Tool Result"). Both set resultType:"complete".

// A single TextContent block: {"resultType":"complete","content":[{type,text}]}.
// `isError` marks a tool-level (not protocol-level) failure.
Json textResult(const std::string& text, bool isError = false);

// A structured result: structuredContent plus - per the spec's
// backward-compatibility SHOULD - the serialized JSON as a TextContent block.
// When `textForCompat` is empty the SDK serializes `structured` itself.
// Validate the result against the tool's outputSchema (the server does this
// automatically when the tool definition declares one).
Json structuredResult(const Json& structured, const std::string& textForCompat = "");

// ---- server --------------------------------------------------------------------

class Server {
 public:
  friend class Builder;

  // Runs the dispatch loop until the transport reaches EOF (graceful shutdown
  // signal for stdio). Returns the first transport error, if any.
  McpResult<void> run(ITransport& transport);

  // Processes one raw message and writes any response(s) to `out`.
  // Public so tests can drive the server over in-memory transports.
  void processLine(const std::string& line, ITransport& out);

  // --- introspection for tests ---
  const Json& serverCapabilities() const { return capabilities_; }
  const std::vector<std::string>& supportedVersions() const { return supported_; }
  // True when the server was built to serve legacy (pre-2026) clients. The
  // HTTP transport consults this to apply the spec's backward-compatibility
  // rule for a missing MCP-Protocol-Version header (treat as 2025-03-26).
  bool acceptsLegacyClients() const { return acceptLegacy_; }

  // The wire-form definition of a registered tool (Error Json if unknown).
  // Used by the HTTP transport to validate Mcp-Param-* mirrored headers.
  Json findToolDefinition(const std::string& name) const;

  // --- subscriptions ----------------------------------------------------------
  // processLine handles subscriptions/listen on the current channel (stdio:
  // acknowledged then later notifications as plain lines). Stream transports
  // instead call registerSubscription with their own sink after validating.

  // Registers a subscription and returns the acknowledged notification line
  // (the caller sends it before or after registering - server-side ordering
  // guarantees no notification precedes the acknowledgment).
  std::string registerSubscription(const RequestId& id, const SubscriptionFilter& filter,
                                   NotifSink sink);
  // Removes without a closure message (transport dropped).
  void removeSubscription(const std::string& subscriptionId);
  // Sends the graceful-closure result through the sink, then removes.
  void closeSubscription(const std::string& subscriptionId);

  // Fan-out helpers (thread-safe). Matched subscriptions receive the
  // notification stamped with their subscriptionId in _meta.
  void notifyListChanged(const std::string& feature);  // "tools"|"prompts"|"resources"
  void notifyResourceUpdated(const std::string& uri);

 private:
  Server() = default;
  Server(Server&& other) noexcept;  // mutex members are default-constructed

  struct Registry {
    std::vector<Json> definitions;             // wire form, served by *\/list
    std::map<std::string, Handler> handlers;   // key: name (or uri)
  };

  // handler dispatch, after meta/version/capability validation
  void addServerInfoToResult(Json& result) const;
  McpResult<Json> dispatchRequest(const JsonRpcRequest& req, const RequestMeta& meta,
                                  const std::function<bool()>& cancelled,
                                  const NotifyFn& notify);

  McpResult<Json> handleDiscover(const RequestMeta& meta);
  McpResult<Json> handleInitialize(const JsonRpcRequest& req);
  McpResult<Json> handlePaginatedList(const Registry& reg, const Json& params,
                                      std::string_view arrayKey);
  McpResult<Json> handleToolCall(const JsonRpcRequest& req, const RequestMeta& meta,
                                 const std::function<bool()>& cancelled,
                                 const NotifyFn& notify);

  // Builds the per-request Ctx, wiring progress/notify/inputRequired against the
  // active transport and this request's metadata. `params`/`meta` must outlive
  // the returned Ctx (they are referenced, not copied).
  // `stateContext` identifies the call for requestState binding (for example
  // "tools/call\nname"); empty disables requestState for this context.
  Ctx makeCtx(const Json& params, const RequestMeta& meta,
              const std::function<bool()>& cancelled, const NotifyFn& notify,
              std::string stateContext = {}) const;

  bool versionSupported(std::string_view wire) const;

  Registry tools_;
  Registry resources_;
  // URI templates (RFC 6570 subset: {var} / {+var}), served by
  // resources/templates/list and matched by resources/read as a fallback.
  Registry resourceTemplates_;
  Registry prompts_;
  Json capabilities_;  // starts as an empty object; keys added per feature
  std::vector<std::string> supported_;   // wire strings, used for -32022 data
  std::set<ProtocolVersion> supportedSet_;
  std::string instructions_;
  int pageSize_ = 0;          // 0 = no pagination on *\/list
  long discoverTtlMs_ = 0;    // 0 = discover result immediately stale
  // 2026-07-28 makes CacheableResult (ttlMs + cacheScope) REQUIRED on */list and
  // resources/read, so the conformant default is 0 (immediately stale) rather
  // than "absent"; strict modern clients (MCP SDK 2.0+) reject results without it.
  // listCache(-1) opts out explicitly (legacy-only servers / wire probing).
  long listTtlMs_ = 0;
  bool acceptLegacy_ = false;
  std::string listCacheScope_ = "public";
  std::string logLevel_;  // legacy logging/setLevel value (informational)
  std::string name_ = "mcp-cpp-server";
  // Default serverInfo.version tracks the SDK release, so a server that does
  // not call Builder::version() still reports a real version.
  std::string version_ = std::string(kSdkVersion);
  bool resultServerInfo_ = true;  // D8: serverInfo in every modern result's _meta
  Handler completionsHandler_;
  std::shared_ptr<const RequestStateCodec> stateCodec_;  // null: requestState unsupported

  // request id (typed form, see cancelKey in server.cpp) -> cancel flag.
  // Guarded by cancellationsMutex_ rather than dispatchMutex_: a cancellation
  // must flip the flag WHILE the request it names is running, so its
  // notification cannot wait for the dispatch lock (a long handler would
  // otherwise make cancel useless over HTTP, where the notification arrives
  // on another worker thread).
  std::map<std::string, std::shared_ptr<std::atomic_bool>> cancellations_;
  // FIFO of keys parked by a cancellation whose request has not arrived yet
  // (out-of-order delivery). Capped: cancels for arbitrary never-sent ids must
  // not grow the map without bound. Guarded by cancellationsMutex_; entries
  // are removed when a request adopts its flag, so eviction below can only
  // reclaim still-unadopted (parked) flags.
  std::deque<std::string> parkedCancels_;
  std::mutex cancellationsMutex_;

  struct Subscription {
    SubscriptionFilter filter;
    NotifSink sink;
    RequestId id = RequestId(int64_t(0));  // original listen request id
  };
  std::map<std::string, Subscription> subscriptions_;
  std::mutex subscriptionsMutex_;  // guards subscriptions_ (publish from any thread)

  // Serializes processLine(). The HTTP transport dispatches from an httplib
  // worker pool (64 threads by default), so the per-request state that is not
  // otherwise synchronized - cancellations_ above all - must be protected.
  // This keeps the documented "single-threaded dispatch" model (handlers run
  // one at a time, no handler locking required) while making concurrent HTTP
  // POSTs correct. Re-entering processLine() from a handler is not supported.
  std::mutex dispatchMutex_;
};

// ---- builder ---------------------------------------------------------------------

class Builder {
 public:
  Builder& name(std::string n);
  Builder& version(std::string v);
  // Attach serverInfo to every modern result's _meta (default true). Turn
  // off when another layer owns the _meta payload.
  Builder& resultServerInfo(bool enabled);
  // Advertise listChanged on "tools"/"prompts"/"resources" so clients know
  // the *ListChanged notifications may arrive (the fan-out itself always
  // works; this is the wire declaration).
  Builder& listChanged(std::string capability);
  // Advertise resources.subscribe (notifications/resources/updated).
  Builder& resourceSubscribe();
  Builder& instructions(std::string text);
  // Adds a supported protocol version. Repeatable. NOTE: the default is
  // Latest-only, and the FIRST explicit call REPLACES that default - to keep
  // serving the latest revision alongside older ones, call
  // `.supports(ProtocolVersion::Latest)` as well.
  Builder& supports(ProtocolVersion v);
  Builder& pageSize(int n);                     // 0 disables *\/list pagination
  Builder& discoverTtlMs(long ms);

  // Registers a tool and advertises the tools capability.
  // `definition` must be a Tool object (name required, inputSchema recommended).
  Builder& tool(Json definition, Handler handler);
  // Registers a resource (uri-keyed) and advertises the resources capability.
  Builder& resource(Json definition, Handler readHandler);
  // Registers a resource TEMPLATE (uriTemplate-keyed, RFC 6570 subset) and
  // advertises the resources capability. Templates are listed by
  // resources/templates/list and serve resources/read for every URI they
  // match - including URIs the server did not know at startup (tables created
  // at runtime, files that appeared later, ...). An exact resource() match
  // always wins over a template; among templates the most specific one (fewest
  // variables, then longest literal prefix) is chosen.
  Builder& resourceTemplate(Json definition, Handler readHandler);
  // Registers a prompt and advertises the prompts capability.
  Builder& prompt(Json definition, Handler handler);

  // Convenience: a text tool whose inputSchema is {"type":"object"}; the
  // server validates arguments against it on tools/call (M4).
  Builder& simpleTool(std::string toolName, std::string description, Handler handler);

  // Registers the completion/complete handler and advertises the completions
  // capability. The handler receives the full params (ref + argument).
  Builder& completions(Handler handler);

  // CacheableResult hints (ttlMs/cacheScope) for */list and resources/read.
  // On by default with ttlMs 0 (required by 2026-07-28); pass a positive ttlMs
  // to let clients cache, or -1 to omit the fields entirely.
  Builder& listCache(long ttlMs, std::string cacheScope = "public");
  // Default severity threshold for Ctx::log (debug..emergency). A client may
  // raise or lower it per request via _meta io.modelcontextprotocol/logLevel.
  Builder& logLevel(std::string level);

  // Dual-era server: also accept LEGACY clients (initialize handshake, no
  // per-request _meta - e.g. Claude Code / VS Code on protocol revisions
  // <= 2025-11-25). Default off: the 2026-07-28 stateless gates stay strict.
  // When on:
  //   * `initialize` is served (legacy handshake) with an InitializeResult
  //   * requests without params._meta are served with default metadata
  //   * requests WITH an ill-formed _meta still fail -32602 (modern clients)
  Builder& acceptLegacyClients(bool on = true);

  // Enables integrity-protected MRTR requestState (see RequestStateCodec). Without
  // it, Ctx::openRequestState and Ctx::sealRequestState return InternalError.
  Builder& requestStateCodec(RequestStateCodec codec);

  std::shared_ptr<Server> build();

 private:
  void capabilityFlag(std::string_view capability, std::string_view flag);
  Server server_;
};

}  // namespace server
}  // namespace mcp
