#include <mcp/client.hpp>

#include "base64.h"  // x-mcp-header value encoding
#include "header_params.h"

#include <utility>

namespace mcp {
namespace client {
namespace {

RequestId nextRequestId(int64_t& counter) { return RequestId(counter++); }

}  // namespace

Client::Client(std::string clientName, std::string clientVersion, Json clientCapabilities)
    : clientName_(std::move(clientName)),
      clientVersion_(std::move(clientVersion)),
      clientCapabilities_(std::move(clientCapabilities)) {}

void Client::connect(ITransport& transport) { transport_ = &transport; }

void Client::closeSubscriptions() {
  if (transport_ != nullptr) transport_->closeSubscriptions();
}

void Client::setProtocolVersion(ProtocolVersion v) { protocolVersion_ = std::string(toString(v)); }

void Client::onNotification(NotificationSink sink) { onNotification_ = std::move(sink); }

void Client::onSubscriptionClosed(std::function<void(const std::string& subscriptionId)> cb) {
  onSubscriptionClosed_ = std::move(cb);
}

namespace {

std::string idToString(const RequestId& id) {
  if (const auto* i = std::get_if<int64_t>(&id)) return std::to_string(*i);
  return std::get<std::string>(id);
}

}  // namespace

// ---- request plumbing -------------------------------------------------------

Json Client::buildParams(const Json& extraParams,
                         const std::optional<RequestId>& progressToken) const {
  Json params = encodeRequestMeta(
      protocolVersion_, clientCapabilities_,
      Implementation{clientName_, std::string(), clientVersion_, std::string(), std::string(),
                     Json(nullptr)},
      progressToken);
  if (extraParams.isObject()) {
    for (const auto& item : extraParams.getAllKeys().toVector()) {
      params.add(item.toString(), extraParams[item.toString()]);
    }
  }
  return params;
}

McpResult<Json> Client::request(std::string_view method, const Json& extraParams,
                                const std::optional<RequestId>& progressToken) {
  return exchange(method, buildParams(extraParams, progressToken));
}

McpResult<Json> Client::initialize(std::string_view protocolVersion,
                                   const Implementation& clientInfo) {
  Json params;
  params.add("protocolVersion", Json::str(std::string(protocolVersion)));
  params.add("capabilities", Json(R"({})"));
  params.add("clientInfo", encodeImplementation(clientInfo));
  auto result = exchange("initialize", std::move(params));
  if (!result.has_value()) return result;
  // Later requests must declare the version the server actually negotiated.
  const Json negotiated = member(result.value(), "protocolVersion");
  if (negotiated.isString()) {
    if (auto v = fromString(negotiated.toString())) setProtocolVersion(*v);
  }
  // The legacy lifecycle completes with notifications/initialized.
  if (transport_ != nullptr) {
    (void)transport_->write(encodeNotification("notifications/initialized", Json(nullptr)));
  }
  return result;
}

void Client::onServerRequest(ServerRequestHandler handler) {
  onServerRequest_ = std::move(handler);
}

McpResult<Json> Client::exchange(std::string_view method, const Json& params) {
  if (transport_ == nullptr) {
    return Err(McpErrorCode::InternalError, "client is not connected");
  }
  const RequestId id = nextRequestId(nextId_);
  const std::string idString = idToString(id);
  const std::string line = encodeRequest(id, method, params);
  auto wrote = transport_->write(line);
  if (!wrote.has_value()) return Err(wrote.error());

  // Read until the response with our id arrives; notifications en route are
  // delivered to the callback, never swallowed.
  while (true) {
    auto raw = transport_->read();
    if (!raw.has_value()) return Err(raw.error());
    if (!raw->has_value()) {
      return Err(McpErrorCode::InternalError, "connection closed by peer before response");
    }
    const ParsedMessage parsed = parseMessage(**raw);
    if (const auto* notification = std::get_if<JsonRpcNotification>(&parsed)) {
      // subscribe() resolves on the server's acknowledgment for this request.
      if (awaitAck_ && notification->method == "notifications/subscriptions/acknowledged") {
        const Json subId = member(member(notification->params, "_meta"), kMetaSubscriptionId);
        if (subId.isString() || subId.isNumber()) {
          awaitAck_ = false;
          if (subId.toString() == idString) return Ok(notification->params);
        }
      }
      if (onNotification_) onNotification_(*notification);
      continue;
    }
    if (const auto* invalid = std::get_if<InvalidMessage>(&parsed)) {
      return Err(invalid->code, invalid->message);
    }
    if (const auto* err = std::get_if<JsonRpcErrorResponse>(&parsed)) {
      if (err->id && *err->id == id) {
        McpError e;
        e.code = static_cast<McpErrorCode>(err->error.code);
        e.message = err->error.message;
        e.data = err->error.data;
        return Err(std::move(e));
      }
      continue;  // response to someone else (should not happen; skip defensively)
    }
    if (const auto* res = std::get_if<JsonRpcResult>(&parsed)) {
      // A result without an id is accepted: the client is sequential, so an
      // id-less result can only be the answer to the one outstanding request
      // (legacy-era peers omit it; 2026-07-28 always includes it).
      if (!res->id.has_value() || res->id == id) return Ok(res->result);
      // A result carrying a subscriptionId closes that subscription
      // gracefully (basic/patterns/subscriptions.md "Graceful Closure").
      const Json subId = member(member(res->result, "_meta"), kMetaSubscriptionId);
      if (onSubscriptionClosed_ && (subId.isString() || subId.isNumber())) {
        onSubscriptionClosed_(std::string(subId.toString()));
      }
      continue;
    }
    if (const auto* incoming = std::get_if<JsonRpcRequest>(&parsed)) {
      // A request the SERVER initiated (pre-2026 servers do this: sampling,
      // elicitation, roots). Answer it if a handler is registered, then keep
      // waiting for our own response. Writing the answer goes out as a fresh
      // POST, which is exactly how streamable HTTP carries it.
      if (onServerRequest_) {
        auto answer = onServerRequest_(std::string(incoming->method), incoming->params,
                                       incoming->id);
        if (answer.has_value()) {
          (void)transport_->write(encodeResultResponse(incoming->id, answer.value()));
        } else {
          // Local conversion: toErrorValue is a server-side helper.
          JsonRpcErrorValue v;
          v.code = static_cast<int32_t>(answer.error().code);
          v.message = answer.error().message;
          v.data = answer.error().data;
          (void)transport_->write(encodeErrorResponse(incoming->id, v));
        }
      }
      continue;  // not our response; keep waiting
    }
  }
}

// ---- protocol surface --------------------------------------------------------

McpResult<Json> Client::discover() { return request("server/discover", Json(nullptr)); }

McpResult<Json> Client::listTools(const std::string& cursor) {
  Json extra;
  if (!cursor.empty()) extra.add("cursor", Json::str(cursor));
  auto result = request("tools/list", extra);
  if (result.has_value()) {
    // Cache definitions for x-mcp-header mirroring.
    toolSchemas_.clear();
    const Json tools = member(*result, "tools");
    if (tools.isArray()) {
      for (const auto& item : tools.toVector()) {
        const Json name = member(item, "name");
        if (name.isString()) toolSchemas_[std::string(name.toString())] = item;
      }
    }
  }
  return result;
}

McpResult<Json> Client::listResourceTemplates(const std::string& cursor) {
  Json extra;
  if (!cursor.empty()) extra.add("cursor", Json::str(cursor));
  return request("resources/templates/list", extra);
}

McpResult<Json> Client::listResources(const std::string& cursor) {
  Json extra;
  if (!cursor.empty()) extra.add("cursor", Json::str(cursor));
  return request("resources/list", extra);
}

McpResult<Json> Client::listPrompts(const std::string& cursor) {
  Json extra;
  if (!cursor.empty()) extra.add("cursor", Json::str(cursor));
  return request("prompts/list", extra);
}

McpResult<Json> Client::complete(const Json& ref, const Json& argument) {
  Json extra;
  extra.add("ref", ref);
  extra.add("argument", argument);
  return request("completion/complete", extra);
}

McpResult<Json> Client::retry(std::string_view method, const Json& extraParams,
                              const Json& previous, const Json& inputResponses) {
  Json params = extraParams.isObject() ? extraParams : Json(R"({})");
  if (!inputResponses.isNull()) {
    params.add(std::string(kInputResponsesKey), inputResponses);
  }
  // Echo requestState EXACTLY, and only when the server sent one (spec: a
  // client MUST NOT invent one). Its contents are opaque and never inspected.
  const std::string state = requestStateOf(previous);
  if (!state.empty()) {
    params.add(std::string(kRequestStateKey), Json::str(state));
  }
  return request(method, params);  // fresh request id
}

McpResult<Json> Client::retryToolCall(const std::string& name, const Json& arguments,
                                      const Json& previous, const Json& inputResponses) {
  mirrorHeaderParams(name, arguments);  // same header mirroring as the first attempt
  Json extra;
  extra.add("name", Json::str(name));
  if (!arguments.isNull()) extra.add("arguments", arguments);
  return retry("tools/call", extra, previous, inputResponses);
}

McpResult<Json> Client::callToolWithInput(const std::string& name, const Json& arguments,
                                          const InputHandler& handle, int maxRounds) {
  McpResult<Json> current = callTool(name, arguments);
  int rounds = 0;
  while (current.has_value() && isInputRequiredResult(current.value())) {
    if (rounds >= maxRounds) {
      // Structured data so a caller can resume manually: the last input_required
      // result and the round count (mirrors the official SDKs' error payloads).
      McpError e;
      e.code = McpErrorCode::InternalError;
      e.message = "MRTR: exceeded " + std::to_string(maxRounds) + " input rounds";
      e.data.add("rounds", Json(static_cast<int64_t>(rounds)));
      e.data.add("lastResult", current.value());
      return Err(std::move(e));
    }
    ++rounds;
    McpResult<Json> responses = handle(inputRequestsOf(current.value()));
    if (!responses.has_value()) return Err(responses.error());
    current = retryToolCall(name, arguments, current.value(), responses.value());
  }
  return current;
}

McpResult<Json> Client::callTool(const std::string& name, const Json& arguments,
                                 const Json& inputResponses,
                                 const std::optional<RequestId>& progressToken) {
  mirrorHeaderParams(name, arguments);
  Json extra;
  extra.add("name", Json::str(name));
  if (!arguments.isNull()) extra.add("arguments", arguments);
  if (!inputResponses.isNull()) extra.add("inputResponses", inputResponses);
  return request("tools/call", extra, progressToken);
}

McpResult<Json> Client::readResource(const std::string& uri) {
  Json extra;
  extra.add("uri", Json::str(uri));
  return request("resources/read", extra);
}

McpResult<Json> Client::getPrompt(const std::string& name, const Json& arguments) {
  Json extra;
  extra.add("name", Json::str(name));
  if (arguments.isObject()) extra.add("arguments", arguments);
  return request("prompts/get", extra);
}

McpResult<Json> Client::subscribe(const Json& filter) {
  awaitAck_ = true;
  auto r = request("subscriptions/listen", filter);
  awaitAck_ = false;
  return r;
}

void Client::mirrorHeaderParams(const std::string& toolName, const Json& arguments) {
  auto it = toolSchemas_.find(toolName);
  std::vector<std::pair<std::string, std::string>> params;
  if (it != toolSchemas_.end()) {
    const Json inputSchema = member(it->second, "inputSchema");
    if (inputSchema.isObject()) {
      detail::collectHeaderParams(inputSchema, arguments, {}, params);
    }
  }
  // Drop headers mirrored for the previous call that the current call does not
  // mirror; empty string values are real headers and must be SENT.
  for (const auto& h : lastParamHeaders_) {
    const bool stillMirrored =
        std::any_of(params.begin(), params.end(), [&](const auto& p) {
          return "Mcp-Param-" + p.first == h;
        });
    if (!stillMirrored) transport_->removeHeader(h);
  }
  lastParamHeaders_.clear();
  for (auto& [headerName, value] : params) {
    const std::string wireName = "Mcp-Param-" + headerName;
    transport_->setHeader(wireName, detail::encodeHeaderValue(value));
    lastParamHeaders_.push_back(wireName);
  }
}

McpResult<void> Client::cancel(const RequestId& requestId, const std::string& reason) {
  if (transport_ == nullptr) {
    return Err(McpErrorCode::InternalError, "client is not connected");
  }
  Json params;
  params.add("requestId", encodeRequestId(requestId));
  if (!reason.empty()) params.add("reason", Json::str(reason));
  const std::string line = encodeNotification("notifications/cancelled", params);
  return transport_->write(line);
}

}  // namespace client
}  // namespace mcp
