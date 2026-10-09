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

#include <functional>
#include <map>
#include <string>
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

  // Sends notifications/cancelled over the underlying transport (stdio
  // cancellation; on HTTP closing the stream is the cancellation signal).
  McpResult<void> cancel(const RequestId& requestId, const std::string& reason = "");

 private:
  Json buildParams(const Json& extraParams, const std::optional<RequestId>& progressToken) const;

  ITransport* transport_ = nullptr;
  std::string clientName_;
  std::string clientVersion_;
  Json clientCapabilities_;
  std::string protocolVersion_ = std::string(toString(ProtocolVersion::Latest));
  NotificationSink onNotification_;
  std::function<void(const std::string&)> onSubscriptionClosed_;
  bool awaitAck_ = false;  // subscribe() waits for the acknowledgment
  int64_t nextId_ = 1;

  // Tool definitions from the last listTools(): source for the x-mcp-header
  // mirroring required of HTTP clients (basic/transports/streamable-http.md).
  std::map<std::string, Json> toolSchemas_;
  std::vector<std::string> lastParamHeaders_;  // cleared before each call
  void mirrorHeaderParams(const std::string& toolName, const Json& arguments);
};

}  // namespace client
}  // namespace mcp
