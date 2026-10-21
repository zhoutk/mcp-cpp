#include <mcp/server.hpp>

#include <mcp/schema.hpp>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <utility>

namespace mcp {
namespace server {
namespace {

// Method -> capability key required to serve it (schema: MethodNotFoundError
// documents that un-advertised features answer MethodNotFound).
struct MethodRoute {
  std::string_view method;
  std::string_view capability;  // empty = core, always available
  std::string_view registryKey; // "tools" / "resources" / "prompts" / ""
};

constexpr MethodRoute kRoutes[] = {
    {"server/discover", "", ""},
    {"initialize", "", ""},  // legacy handshake; served only when acceptLegacy_
    {"ping", "", ""},        // legacy (removed in 2026-07-28)
    {"logging/setLevel", "", ""},  // legacy (removed in 2026-07-28)
    {"subscriptions/listen", "", ""},
    {"tools/list", "tools", "tools"},
    {"tools/call", "tools", "tools"},
    {"resources/list", "resources", "resources"},
    {"resources/read", "resources", "resources"},
    {"resources/templates/list", "resources", "resources"},
    {"resources/subscribe", "resources", "resources"},    // legacy
    {"resources/unsubscribe", "resources", "resources"},  // legacy
    {"prompts/list", "prompts", "prompts"},
    {"prompts/get", "prompts", "prompts"},
    {"completion/complete", "completions", ""},
};

std::string idToString(const RequestId& id) {
  if (const auto* i = std::get_if<int64_t>(&id)) return std::to_string(*i);
  return std::get<std::string>(id);
}

// Key for the cancellations_ map: idToString renders the NUMBER 1 and the
// STRING "1" identically, so a cancel for one type would set (or adopt) the
// other type's flag. The type tag keeps the two apart. The subscription map
// keeps idToString - its ids come from the same request that created them, so
// a collision merges one client's own subscriptions, not state across ids.
std::string cancelKey(const RequestId& id) {
  if (const auto* i = std::get_if<int64_t>(&id)) return "n:" + std::to_string(*i);
  return "s:" + std::get<std::string>(id);
}

// Upper bound for parked cancel flags (cancellations whose request has not
// arrived). A client can cancel arbitrary ids that never name a request;
// without a cap that grows the map without bound (memory DoS).
constexpr size_t kMaxParkedCancels = 1024;

JsonRpcErrorValue toErrorValue(McpErrorCode code, std::string message) {
  JsonRpcErrorValue v;
  v.code = static_cast<int32_t>(code);
  v.message = std::move(message);
  return v;
}

JsonRpcErrorValue toErrorValue(const McpError& e) {
  JsonRpcErrorValue v = toErrorValue(e.code, e.message);
  v.data = e.data;  // structured payloads (e.g. -32021 requiredCapabilities) survive
  return v;
}

const MethodRoute* findRoute(std::string_view method) {
  for (const auto& route : kRoutes) {
    if (route.method == method) return &route;
  }
  return nullptr;
}

}  // namespace

// ---- URI templates (RFC 6570 subset) ------------------------------------------
// Supports literal runs plus {var} (a non-empty run without '/') and {+var}
// (a non-empty run that may contain '/'). No {?query}/{#fragment}/multi-var
// expressions - enough for path-shaped resources such as
// db://schema/{db}/{table}. Matching is single-pass and greedy: a variable is
// bounded by the next literal, taking its first occurrence.
bool matchUriTemplate(const std::string& tmpl, const std::string& uri,
                      std::vector<std::pair<std::string, std::string>>* vars) {
  size_t ti = 0, ui = 0;
  while (ti < tmpl.size()) {
    const size_t open = tmpl.find('{', ti);
    const std::string literal =
        tmpl.substr(ti, open == std::string::npos ? std::string::npos : open - ti);
    if (uri.compare(ui, literal.size(), literal) != 0) return false;
    ui += literal.size();
    if (open == std::string::npos) break;

    const size_t close = tmpl.find('}', open);
    if (close == std::string::npos) return false;  // malformed template
    std::string expr = tmpl.substr(open + 1, close - open - 1);
    const bool allowSlash = !expr.empty() && expr[0] == '+';
    if (allowSlash) expr.erase(0, 1);
    if (expr.empty()) return false;  // malformed template

    ti = close + 1;
    const size_t nextOpen = tmpl.find('{', ti);
    const std::string nextLiteral =
        tmpl.substr(ti, nextOpen == std::string::npos ? std::string::npos : nextOpen - ti);
    size_t valueEnd = uri.size();
    if (!nextLiteral.empty()) {
      valueEnd = uri.find(nextLiteral, ui);
      if (valueEnd == std::string::npos) return false;
    }
    const std::string value = uri.substr(ui, valueEnd - ui);
    if (value.empty()) return false;  // a variable never matches an empty run
    if (!allowSlash && value.find('/') != std::string::npos) return false;
    if (vars) vars->emplace_back(expr, value);
    ui = valueEnd;
  }
  return ui == uri.size();
}

SubscriptionFilter parseSubscriptionFilter(const Json& obj) {
  SubscriptionFilter f;
  if (!obj.isObject()) return f;
  // member(), not operator[]: zjson's operator[] deep-searches nested layers
  // when the key is absent at the top level (see CLAUDE.md).
  const Json notifications = member(obj, "notifications");
  if (!notifications.isObject()) return f;
  if (member(notifications, "toolsListChanged").isTrue()) f.toolsListChanged = true;
  if (member(notifications, "promptsListChanged").isTrue()) f.promptsListChanged = true;
  if (member(notifications, "resourcesListChanged").isTrue()) f.resourcesListChanged = true;
  const Json uris = member(notifications, "resourceSubscriptions");
  if (uris.isArray()) {
    for (const auto& item : uris.toVector()) {
      f.resourceUris.emplace_back(item.toString());
    }
  }
  return f;
}

Json subscriptionFilterToJson(const SubscriptionFilter& f) {
  Json n;
  if (f.toolsListChanged) n.add("toolsListChanged", Json(true));
  if (f.promptsListChanged) n.add("promptsListChanged", Json(true));
  if (f.resourcesListChanged) n.add("resourcesListChanged", Json(true));
  if (!f.resourceUris.empty()) {
    Json uris = Json("[]");
    for (const auto& uri : f.resourceUris) uris.push_back(Json::str(uri));
    n.add("resourceSubscriptions", std::move(uris));
  }
  return n;
}

Server::Server(Server&& other) noexcept
    : extensionMethods_(std::move(other.extensionMethods_)),
      tools_(std::move(other.tools_)),
      resources_(std::move(other.resources_)),
      resourceTemplates_(std::move(other.resourceTemplates_)),
      prompts_(std::move(other.prompts_)),
      capabilities_(std::move(other.capabilities_)),
      supported_(std::move(other.supported_)),
      supportedSet_(std::move(other.supportedSet_)),
      instructions_(std::move(other.instructions_)),
      pageSize_(other.pageSize_),
      discoverTtlMs_(other.discoverTtlMs_),
      listTtlMs_(other.listTtlMs_),
      acceptLegacy_(other.acceptLegacy_),
      listCacheScope_(std::move(other.listCacheScope_)),
      logLevel_(std::move(other.logLevel_)),
      name_(std::move(other.name_)),
      version_(std::move(other.version_)),
      // Declaration order matters: resultServerInfo_ is declared after
      // name_/version_ in server.hpp, and -Werror turns a mismatch
      // (-Wreorder-ctor) into a build failure.
      resultServerInfo_(other.resultServerInfo_),
      completionsHandler_(std::move(other.completionsHandler_)),
      stateCodec_(std::move(other.stateCodec_)),
      principalResolver_(std::move(other.principalResolver_)),
      cancellations_(std::move(other.cancellations_)),
      subscriptions_(std::move(other.subscriptions_)) {}

// ---- result builders ---------------------------------------------------------

Json textResult(const std::string& text, bool isError) {
  Json item;
  item.add("type", Json::str("text"));
  item.add("text", Json::str(text));
  Json content = Json("[]");
  content.push_back(std::move(item));
  Json result;
  result.add("content", std::move(content));
  if (isError) result.add("isError", Json(true));
  return result;
}

Json structuredResult(const Json& structured, const std::string& textForCompat) {
  Json result;
  Json item;
  item.add("type", Json::str("text"));
  item.add("text", Json::str(textForCompat.empty() ? structured.toString() : textForCompat));
  Json content = Json("[]");
  content.push_back(std::move(item));
  result.add("content", std::move(content));
  result.add("structuredContent", structured);
  return result;
}

// ---- dispatch loop -----------------------------------------------------------

namespace {

constexpr const char* kBadRequestState = "Invalid requestState";

int64_t unixNowSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

Ctx Server::makeCtx(const Json& params, const RequestMeta& meta,
                    const std::function<bool()>& cancelled, const NotifyFn& notify,
                    std::string stateContext, std::string principal) const {
  Ctx ctx{params, meta, cancelled, {}, {}, {}, {}, {}, {}, {}, {}};
  ctx.principal = std::move(principal);
  ctx.notify = notify;
  std::string configuredLevel;
  {
    std::lock_guard<std::mutex> lock(logLevelMutex_);
    configuredLevel = logLevel_;
  }
  // Structured logging (see Ctx::log in server.hpp). A 2026-07-28 request
  // opts in through _meta.logLevel and MUST NOT receive notifications/message
  // without it; Builder::logLevel only raises the threshold for opted-in
  // requests. A legacy (pre-2026) request has no per-request field - it opts
  // in through logging/setLevel, which sets the server-wide level - so the
  // configured level is the whole threshold there.
  ctx.log = [notify, logger = name_, configuredLevel = std::move(configuredLevel),
             requestLevel = meta.logLevel,
             modern = meta.complete](const std::string& level, const std::string& message) {
    static constexpr std::string_view kLevels[] = {"debug",    "info",     "notice",
                                                   "warning",  "error",    "critical",
                                                   "alert",    "emergency"};
    const auto rank = [&](std::string_view l) -> int {
      for (int i = 0; i < 8; ++i)
        if (kLevels[i] == l) return i;
      return -1;
    };
    const int have = rank(level);
    const int serverRank = rank(configuredLevel);
    int threshold = -1;  // -1: no filtering
    if (modern) {
      if (requestLevel.empty()) return;  // no opt-in -> no notification
      threshold = rank(requestLevel);
      if (serverRank > threshold) threshold = serverRank;
    } else {
      threshold = serverRank;
    }
    if (have >= 0 && threshold >= 0 && have < threshold) return;
    Json p;
    p.add("level", Json::str(level));
    p.add("data", Json::str(message));
    if (!logger.empty()) p.add("logger", Json::str(logger));
    // 2026 clients read the level from _meta; the root member stays for
    // pre-2026 clients.
    Json m = Json(R"({})");
    m.add(std::string(kMetaLogLevel), Json::str(level));
    p.add("_meta", std::move(m));
    notify("notifications/message", std::move(p));
  };
  ctx.progress = [notify, token = meta.progressToken](double value, double total,
                                                      const std::string& message) {
    if (!token) return;  // client did not opt in to progress for this request
    Json p;
    p.add("progressToken", encodeRequestId(*token));
    p.add("progress", Json(value));
    if (total >= 0) p.add("total", Json(total));
    if (!message.empty()) p.add("message", Json::str(message));
    notify("notifications/progress", std::move(p));
  };
  ctx.inputRequired = [meta](Json inputRequests, std::string requestState) -> McpResult<Json> {
    const bool hasRequests =
        inputRequests.isObject() && !inputRequests.getAllKeys().toVector().empty();
    // Spec: at least one of inputRequests / requestState MUST be present.
    if (!hasRequests && requestState.empty()) {
      return Err(McpErrorCode::InternalError,
                 "InputRequiredResult requires inputRequests or requestState");
    }
    if (hasRequests) {
      // F4: report EVERY missing capability in one -32021 instead of failing on
      // the first one. A client that has to discover them one retry at a time
      // would otherwise pay a full round trip per capability.
      std::set<std::string> missing;
      for (const auto& key : inputRequests.getAllKeys().toVector()) {
        const Json request = member(inputRequests, key.toString());
        const Json method = member(request, "method");
        if (!method.isString()) {
          return Err(McpErrorCode::InternalError,
                     "inputRequests entry '" + std::string(key.toString()) + "' has no method");
        }
        const std::string_view capability = mrtrCapabilityFor(method.toString());
        if (capability.empty()) {
          return Err(McpErrorCode::InternalError,
                     "inputRequests method is not a valid MRTR request: " +
                         std::string(method.toString()));
        }
        // Spec: MUST NOT request something the client did not declare.
        if (!capabilitiesInclude(meta.clientCapabilities, capability)) {
          missing.emplace(capability);
        }
      }
      if (!missing.empty()) {
        // MissingRequiredClientCapability (-32021) with the missing
        // capabilities in data - not InternalError, which would blame the
        // server for a client-side protocol violation.
        McpError e;
        e.code = McpErrorCode::MissingRequiredClientCapability;
        std::string joined;
        for (const auto& capability : missing) {
          if (!joined.empty()) joined += ", ";
          joined += capability;
        }
        e.message = "client did not declare the required capabilities: " + joined;
        // schema: data.requiredCapabilities is a ClientCapabilities OBJECT
        // (e.g. {"elicitation":{}}), not a list of names.
        Json required = Json(R"({})");
        for (const auto& capability : missing) {
          required.add(capability, Json(R"({})"));
        }
        Json data;
        data.add("requiredCapabilities", std::move(required));
        e.data = std::move(data);
        return Err(std::move(e));
      }
    }
    Json result;
    result.add(std::string(kResultTypeKey), Json::str(std::string(kResultTypeInputRequired)));
    if (hasRequests) result.add(std::string(kInputRequestsKey), std::move(inputRequests));
    if (!requestState.empty()) {
      result.add(std::string(kRequestStateKey), Json::str(std::move(requestState)));
    }
    return Ok(std::move(result));
  };
  ctx.elicit = [inputRequired = ctx.inputRequired](std::string message, Json requestedSchema,
                                                   std::string requestState) -> McpResult<Json> {
    Json params;
    params.add("mode", Json::str("form"));
    params.add("message", Json::str(std::move(message)));
    params.add("requestedSchema", std::move(requestedSchema));
    Json request;
    request.add("method", Json::str("elicitation/create"));
    request.add("params", std::move(params));
    Json requests = Json(R"({})");
    requests.add("elicit", std::move(request));
    return inputRequired(std::move(requests), std::move(requestState));
  };
  // F1: a non-empty principal turns the codec's identity binding on for this
  // request (D7's sealFor/openFor); without one the wire form stays unbound.
  const std::string identity = ctx.principal;
  ctx.openRequestState = [codec = stateCodec_, context = stateContext,
                          paramsPtr = &params, identity]() -> McpResult<std::optional<std::string>> {
    if (!codec) {
      return Err(McpErrorCode::InternalError, "requestState requires Builder::requestStateCodec");
    }
    if (context.empty()) {
      return Err(McpErrorCode::InternalError, "requestState is not available for this method");
    }
    const Json echoed = member(*paramsPtr, kRequestStateKey);
    if (echoed.isNull()) return Ok(std::optional<std::string>{});
    if (!echoed.isString()) return Err(McpErrorCode::InvalidParams, kBadRequestState);
    const std::string wire(echoed.toString());
    auto opened = identity.empty()
                      ? codec->open(wire, context, unixNowSeconds())
                      : codec->openFor(wire, context, identity, unixNowSeconds());
    if (!opened.has_value()) return Err(McpErrorCode::InvalidParams, kBadRequestState);
    return Ok(std::optional<std::string>(std::move(opened).value()));
  };
  ctx.sealRequestState = [codec = stateCodec_, context = stateContext, identity](
                             std::string payload) -> McpResult<std::string> {
    if (!codec) {
      return Err(McpErrorCode::InternalError, "requestState requires Builder::requestStateCodec");
    }
    if (context.empty()) {
      return Err(McpErrorCode::InternalError, "requestState is not available for this method");
    }
    return Ok(identity.empty()
                  ? codec->seal(payload, context, unixNowSeconds())
                  : codec->sealFor(payload, context, identity, unixNowSeconds()));
  };
  return ctx;
}

McpResult<void> Server::run(ITransport& transport) {
  while (true) {
    auto line = transport.read();
    if (!line.has_value()) return Err(line.error());  // transport failure
    if (!line->has_value()) return Ok();              // EOF: graceful shutdown
    processLine(**line, transport);
  }
}

void Server::acquireDispatchSlot() {
  std::unique_lock<std::mutex> lock(dispatchMutex_);
  dispatchSlotsCv_.wait(lock, [this] { return dispatchSlots_ > 0; });
  --dispatchSlots_;
}

void Server::releaseDispatchSlot() {
  {
    std::lock_guard<std::mutex> lock(dispatchMutex_);
    ++dispatchSlots_;
  }
  dispatchSlotsCv_.notify_one();
}

void Server::processLine(const std::string& line, ITransport& out) {
  processLine(line, out, std::string());
}

void Server::processLine(const std::string& line, ITransport& out,
                         const std::string& principal) {
  const ParsedMessage parsed = parseMessage(line);

  // Cancellation is handled BEFORE the dispatch lock: the HTTP transport
  // delivers it on another worker thread and it must take effect while the
  // named request is still running. It only touches cancellations_ (own
  // mutex) and the already-thread-safe subscription map.
  if (const auto* notification = std::get_if<JsonRpcNotification>(&parsed)) {
    if (notification->method == "notifications/cancelled") {
      const Json requestId = member(notification->params, "requestId");
      auto id = decodeRequestId(requestId);
      if (id) {
        const std::string key = cancelKey(*id);
        {
          std::lock_guard<std::mutex> lock(cancellationsMutex_);
          auto it = cancellations_.find(key);
          if (it != cancellations_.end()) {
            // The request is IN FLIGHT: it already holds this flag, so it must
            // be flipped - replacing the map entry would leave the running
            // handler looking at the old (false) flag forever.
            it->second->store(true);
          } else {
            // Cancellation arrived before the request (out-of-order delivery):
            // park a set flag for the request to pick up. Parked entries are
            // capped: cancels for arbitrary never-sent ids must not grow the
            // map without bound. Eviction drops the OLDEST still-parked flag -
            // the only loss is a cancel for a request that never arrived.
            // (The cap applies to PARKED entries; in-flight flags are erased
            // when their request completes and are bounded by dispatch.)
            cancellations_[key] = std::make_shared<std::atomic_bool>(true);
            parkedCancels_.push_back(key);
            while (parkedCancels_.size() > kMaxParkedCancels) {
              const std::string oldest = parkedCancels_.front();
              parkedCancels_.pop_front();
              cancellations_.erase(oldest);  // no-op if adopted-and-completed
            }
          }
        }
        // The subscription map keeps the plain id string (see cancelKey).
        closeSubscription(idToString(*id));  // cancelled subscription ends with a closure result
      }
      return;
    }
    // Other notifications MUST NOT be answered; unknown ones are ignored.
    return;
  }

  // The HTTP transport calls this from its worker pool. Wait for a dispatch
  // slot: one slot reproduces the serialized model, MCP_WORKERS slots allow
  // that many handlers at once. Cancellation deliberately bypasses this gate.
  acquireDispatchSlot();
  struct SlotRelease {
    Server* server;
    ~SlotRelease() { server->releaseDispatchSlot(); }
  } slotRelease{this};

  if (const auto* invalid = std::get_if<InvalidMessage>(&parsed)) {
    JsonRpcErrorValue v = toErrorValue(invalid->code, invalid->message);
    (void)out.write(encodeErrorResponse(invalid->id, v));
    return;
  }

  // Responses arriving at a server: this runtime never initiates requests
  // (server-to-client interaction travels inside InputRequiredResult), so
  // any response is unexpected - ignore it rather than corrupt the stream.
  if (!std::holds_alternative<JsonRpcRequest>(parsed)) return;

  const JsonRpcRequest& req = std::get<JsonRpcRequest>(parsed);

  // Legacy dual-era: `initialize` handshake (params carry protocolVersion at
  // the root, no _meta by definition). Strict mode answers MethodNotFound.
  if (req.method == "initialize") {
    if (!acceptLegacy_) {
      // The legacy client's only diagnostic is the error: name the versions
      // this server does support (D8).
      JsonRpcErrorValue v = toErrorValue(McpErrorCode::MethodNotFound,
                                         "Method not found: initialize");
      Json data;
      Json versions = Json("[]");
      for (const auto& wire : supported_) versions.push_back(Json::str(std::string(wire)));
      data.add("supported", std::move(versions));
      v.data = std::move(data);
      (void)out.write(encodeErrorResponse(req.id, v));
      return;
    }
    McpResult<Json> initResult = handleInitialize(req);
    if (initResult.has_value()) {
      // Legacy InitializeResult: pre-2026 schemas have no resultType and reject
      // unknown keys, so it is not added here (see the modern path below).
      (void)out.write(encodeResultResponse(req.id, *initResult));
    } else {
      (void)out.write(encodeErrorResponse(req.id, toErrorValue(initResult.error())));
    }
    return;
  }

  // Stateless protocol: every request carries its own metadata. Dual-era
  // relaxation: when legacy clients are accepted, a request with NO _meta
  // member at all - or with a PARTIAL _meta (e.g. only clientCapabilities,
  // or an empty object - some AI clients send "_meta":{}) - is served with
  // default metadata instead of failing. Strict mode keeps the -32602 for
  // any ill-formed _meta (the 2026-07-28 spec mandates those fields).
  McpResult<RequestMeta> metaResult = acceptLegacy_
      ? decodeRequestMetaRelaxed(
            req.params, supported_.empty() ? std::string_view()
                                           : std::string_view(supported_.front()))
      : decodeRequestMeta(req.params);
  if (!metaResult.has_value()) {
    (void)out.write(encodeErrorResponse(req.id, toErrorValue(metaResult.error())));
    return;
  }
  RequestMeta meta = std::move(*metaResult);
  if (!meta.complete && !acceptLegacy_) {
    // Strict mode: partial metadata is invalid. (decodeRequestMeta already
    // rejects it, so this is defensive for future callers.)
    (void)out.write(encodeErrorResponse(
        req.id, toErrorValue(McpErrorCode::InvalidParams,
                             "request _meta is missing required protocol fields")));
    return;
  }

  if (!versionSupported(meta.protocolVersion)) {
    JsonRpcErrorValue v = toErrorValue(McpErrorCode::UnsupportedProtocolVersion,
                                       "Unsupported protocol version");
    Json data;
    Json versions = Json("[]");
    for (const auto& wire : supported_) versions.push_back(Json::str(std::string(wire)));
    data.add("supported", std::move(versions));
    data.add("requested", Json::str(meta.protocolVersion));
    v.data = std::move(data);
    (void)out.write(encodeErrorResponse(req.id, v));
    return;
  }

  // A method is routable when it is a spec route or a registered extension
  // method (E4); anything else is MethodNotFound before any handler can run.
  if (findRoute(req.method) == nullptr &&
      extensionMethods_.find(req.method) == extensionMethods_.end()) {
    (void)out.write(encodeErrorResponse(
        req.id, toErrorValue(McpErrorCode::MethodNotFound,
                             "Method not found: " + req.method)));
    return;
  }

  if (req.method == "subscriptions/listen") {
    const SubscriptionFilter filter = parseSubscriptionFilter(req.params);
    const std::string subscriptionId = idToString(req.id);
    auto sink = [&out](const std::string& line) { (void)out.write(line); };
    {
      std::lock_guard<std::mutex> lock(subscriptionsMutex_);
      subscriptions_[subscriptionId] = Subscription{filter, sink, req.id};
    }
    // Acknowledgment MUST be the first message for this subscription.
    Json ackParams;
    Json ackMeta;
    ackMeta.add(std::string(kMetaSubscriptionId), encodeRequestId(req.id));
    ackParams.add("_meta", std::move(ackMeta));
    ackParams.add("notifications", subscriptionFilterToJson(filter));
    (void)out.write(encodeNotification("notifications/subscriptions/acknowledged", ackParams));
    return;  // no JSON-RPC response: the stream stays open
  }

  // Cancellation flag for this request. Reuse an existing slot: if a
  // notifications/cancelled arrived before the request itself (out-of-order
  // delivery), the request must observe cancelled=true, not a fresh flag.
  std::shared_ptr<std::atomic_bool> cancelFlag;
  {
    const std::string key = cancelKey(req.id);
    std::lock_guard<std::mutex> lock(cancellationsMutex_);
    auto& slot = cancellations_[key];
    if (!slot) slot = std::make_shared<std::atomic_bool>(false);
    cancelFlag = slot;
    // The flag now belongs to this request: remove it from the parked queue
    // so a later eviction can never reclaim an IN-FLIGHT cancel flag.
    parkedCancels_.erase(std::remove(parkedCancels_.begin(), parkedCancels_.end(), key),
                         parkedCancels_.end());
  }
  const std::function<bool()> cancelled = [cancelFlag] { return cancelFlag->load(); };

  // Server-to-client notifications for this request (progress, logging).
  // Best-effort: a write failure must not abort the request.
  const NotifyFn notify = [&out](const std::string& method, Json params) {
    (void)out.write(encodeNotification(method, std::move(params)));
  };

  // F1: the transport-supplied principal wins; otherwise let the builder's
  // resolver derive one from this request's metadata (empty = unauthenticated).
  std::string identity = principal;
  if (identity.empty() && principalResolver_) identity = principalResolver_(meta, req.params);

  McpResult<Json> result = dispatchRequest(req, meta, cancelled, notify, identity);
  // MRTR: an InputRequiredResult may only answer tools/call, prompts/get and
  // resources/read (basic/patterns/mrtr.md "Supported Requests").
  if (result.has_value() && isInputRequiredResult(*result) && req.method != "tools/call" &&
      req.method != "prompts/get" && req.method != "resources/read") {
    result = Err(McpErrorCode::InternalError,
                 "input_required is not a valid result for " + req.method);
  }
  if (result.has_value()) {
    // 2026-07-28 requires resultType on every result; the pre-2026 schemas have
    // no such field and reject unknown keys, so it is injected only for modern
    // requests (a legacy request is one served with relaxed, incomplete _meta).
    if (meta.complete && result->isObject()) {
      if (!result->contains(std::string(kResultTypeKey))) {
        result->add(std::string(kResultTypeKey), Json::str(std::string(kResultTypeComplete)));
      }
      // D8: every modern result carries serverInfo in _meta (SHOULD). The
      // discover result builds its own; do not overwrite it.
      addServerInfoToResult(*result);
    }
    (void)out.write(encodeResultResponse(req.id, *result));
  } else {
    (void)out.write(encodeErrorResponse(req.id, toErrorValue(result.error())));
  }
  {
    std::lock_guard<std::mutex> lock(cancellationsMutex_);
    cancellations_.erase(cancelKey(req.id));
  }
}

// D8: attaches io.modelcontextprotocol/serverInfo to a result's _meta unless
// the result already carries one (server/discover builds its own). Opt out
// with Builder::resultServerInfo(false).
void Server::addServerInfoToResult(Json& result) const {
  if (!resultServerInfo_) return;
  Json meta = member(result, "_meta");
  if (!meta.isObject()) meta = Json(R"({})");
  if (!member(meta, kMetaServerInfo).isNull()) return;  // already present
  Json info;
  info.add("name", Json::str(name_));
  info.add("version", Json::str(version_));
  meta.add(std::string(kMetaServerInfo), std::move(info));
  result.add("_meta", std::move(meta));
}

McpResult<Json> Server::dispatchRequest(const JsonRpcRequest& req, const RequestMeta& meta,
                                        const std::function<bool()>& cancelled,
                                        const NotifyFn& notify, const std::string& principal) {
  const MethodRoute* route = findRoute(req.method);

  // E4: extension methods are dynamic routes. They are served only when the
  // request advertises the extension (SEP-2133 negotiation) and - when the
  // binding names one - at the bound protocol revision. Both failures are
  // MethodNotFound, exactly like an unadvertised core capability.
  if (route == nullptr) {
    auto it = extensionMethods_.find(req.method);
    if (it == extensionMethods_.end()) {
      return Err(McpErrorCode::MethodNotFound, "Method not found: " + req.method);
    }
    const ExtensionMethod& extension = it->second;
    if (!capabilitiesIncludeExtension(meta.clientCapabilities, extension.extensionId)) {
      // SEP-2575 Missing Required Capabilities: the client did not declare the
      // extension this method belongs to. `data.requiredCapabilities` is a
      // ClientCapabilities object, so the extension lives under its
      // `extensions` map. -32021 (not -32601) is what the conformance suite
      // requires for extension-gated methods such as tasks/*.
      McpError e;
      e.code = McpErrorCode::MissingRequiredClientCapability;
      e.message = "client did not declare the '" + extension.extensionId +
                  "' extension required by " + req.method;
      Json required = Json(R"({})");
      Json extensions = Json(R"({})");
      extensions.add(extension.extensionId, Json(R"({})"));
      required.add("extensions", std::move(extensions));
      Json data;
      data.add("requiredCapabilities", std::move(required));
      e.data = std::move(data);
      return Err(std::move(e));
    }
    if (!extension.protocolVersion.empty() &&
        extension.protocolVersion != meta.protocolVersion) {
      return Err(McpErrorCode::MethodNotFound,
                 "Method not found: " + req.method + " at protocol version " +
                     meta.protocolVersion);
    }
    Ctx ctx = makeCtx(req.params, meta, cancelled, notify, {}, principal);
    return extension.handler(ctx);
  }

  // Capability gate: the server must have advertised the feature.
  if (!route->capability.empty() && !capabilitiesInclude(capabilities_, route->capability)) {
    return Err(McpErrorCode::MethodNotFound,
               "Method not found: " + req.method + " (capability not advertised)");
  }

  // Legacy-era utilities. 2026-07-28 removed ping and logging/setLevel, and
  // replaced resource subscriptions with subscriptions/listen. They are served
  // only when the server opted into legacy clients; a strict 2026 server
  // answers MethodNotFound (-32601), matching the "unknown method" contract.
  if (req.method == "ping" || req.method == "logging/setLevel") {
    if (!acceptLegacy_) {
      return Err(McpErrorCode::MethodNotFound, "Method not found: " + req.method);
    }
    // logging/setLevel: accept and record the level (notifications/message is
    // emitted through Ctx::notify; filtering is the handler's concern).
    if (req.method == "logging/setLevel") {
      const Json level = member(req.params, "level");
      if (level.isString()) {
        std::lock_guard<std::mutex> lock(logLevelMutex_);
        logLevel_ = std::string(level.toString());
      }
    }
    return Ok(Json(R"({})"));
  }
  if (req.method == "resources/subscribe" || req.method == "resources/unsubscribe") {
    if (!acceptLegacy_) {
      return Err(McpErrorCode::MethodNotFound, "Method not found: " + req.method);
    }
    // The legacy per-URI subscription model: accept and acknowledge. Servers
    // that need real per-URI update fan-out implement it in their transport
    // (2026 clients use subscriptions/listen instead).
    return Ok(Json(R"({})"));
  }

  if (req.method == "server/discover") return handleDiscover(meta);
  if (req.method == "tools/list") return handlePaginatedList(tools_, req.params, "tools");
  if (req.method == "completion/complete") {
    Ctx ctx = makeCtx(req.params, meta, cancelled, notify, {}, principal);
    return completionsHandler_(ctx);
  }
  if (req.method == "tools/call") return handleToolCall(req, meta, cancelled, notify, principal);
  if (req.method == "resources/list") return handlePaginatedList(resources_, req.params, "resources");
  if (req.method == "prompts/list") return handlePaginatedList(prompts_, req.params, "prompts");

  if (req.method == "resources/templates/list") {
    return handlePaginatedList(resourceTemplates_, req.params, "resourceTemplates");
  }

  if (req.method == "resources/read") {
    const Json uri = member(req.params, "uri");
    if (!uri.isString()) return Err(McpErrorCode::InvalidParams, "resources/read requires a string uri");
    const std::string uriText(uri.toString());

    // Exact registration wins; otherwise the most specific matching URI
    // template serves the read (templates cover URIs that did not exist when
    // the server started - a table created at runtime, a file that appeared
    // later - without needing a restart).
    const Handler* handler = nullptr;
    auto it = resources_.handlers.find(uriText);
    if (it != resources_.handlers.end()) {
      handler = &it->second;
    } else {
      size_t bestVars = 0, bestPrefix = 0;
      for (const auto& entry : resourceTemplates_.handlers) {
        std::vector<std::pair<std::string, std::string>> vars;
        if (!matchUriTemplate(entry.first, uriText, &vars)) continue;
        const size_t open = entry.first.find('{');
        const size_t prefix = open == std::string::npos ? entry.first.size() : open;
        if (handler == nullptr || vars.size() < bestVars ||
            (vars.size() == bestVars && prefix > bestPrefix)) {
          handler = &entry.second;
          bestVars = vars.size();
          bestPrefix = prefix;
        }
      }
    }
    if (handler == nullptr) {
      return Err(McpErrorCode::InvalidParams, "Unknown resource: " + uriText);
    }
    Ctx ctx = makeCtx(req.params, meta, cancelled, notify, "resources/read\n" + uriText, principal);
    McpResult<Json> read = (*handler)(ctx);
    // 2026-07-28 requires CacheableResult (ttlMs + cacheScope) on resources/read
    // too: a strict modern client rejects the result without them, so a handler
    // that omitted the hints inherits the builder's cache settings. Opt-out
    // (listTtlMs_ < 0) and input_required results are left untouched.
    if (read.has_value() && read->isObject() && listTtlMs_ >= 0 &&
        resultTypeOf(*read) == std::string(kResultTypeComplete)) {
      if (!read->contains(std::string("ttlMs")))
        read->add(std::string("ttlMs"), Json(static_cast<int64_t>(listTtlMs_)));
      if (!read->contains(std::string("cacheScope")))
        read->add(std::string("cacheScope"), Json::str(std::string(listCacheScope_)));
    }
    return read;
  }

  if (req.method == "prompts/get") {
    const Json name = member(req.params, "name");
    if (!name.isString()) return Err(McpErrorCode::InvalidParams, "prompts/get requires a string name");
    auto it = prompts_.handlers.find(std::string(name.toString()));
    if (it == prompts_.handlers.end()) {
      return Err(McpErrorCode::InvalidParams, "Unknown prompt: " + std::string(name.toString()));
    }
    Ctx ctx = makeCtx(req.params, meta, cancelled, notify,
                      "prompts/get\n" + std::string(name.toString()), principal);
    return it->second(ctx);
  }

  return Err(McpErrorCode::MethodNotFound, "Method not found: " + req.method);
}

// ---- per-method handlers --------------------------------------------------------

McpResult<Json> Server::handleDiscover(const RequestMeta& /*meta*/) {
  Json result;
  result.add(std::string(kResultTypeKey), Json::str(std::string(kResultTypeComplete)));
  Json versions = Json("[]");
  for (const auto& wire : supported_) versions.push_back(Json::str(std::string(wire)));
  result.add("supportedVersions", std::move(versions));
  result.add("capabilities", capabilities_);
  if (!instructions_.empty()) result.add("instructions", Json::str(instructions_));
  result.add("ttlMs", Json(static_cast<int64_t>(discoverTtlMs_)));
  result.add("cacheScope", Json::str("public"));
  Json meta;
  Json info;
  info.add("name", Json::str(name_));
  info.add("version", Json::str(version_));
  meta.add(std::string(kMetaServerInfo), std::move(info));
  result.add("_meta", std::move(meta));
  return Ok(std::move(result));
}

McpResult<Json> Server::handlePaginatedList(const Registry& reg, const Json& params,
                                            std::string_view arrayKey) {
  Json result;
  result.add(std::string(kResultTypeKey), Json::str(std::string(kResultTypeComplete)));

  const size_t total = reg.definitions.size();
  size_t offset = 0;
  const Json cursor = member(params, "cursor");
  if (cursor.isString()) {
    // Opaque token: this registry encodes the plain offset. Unknown cursors
    // are rejected rather than silently restarting.
    const std::string c(cursor.toString());
    size_t parsed = 0;
    bool numeric = !c.empty();
    for (const char ch : c) {
      if (ch < '0' || ch > '9') { numeric = false; break; }
      parsed = parsed * 10 + static_cast<size_t>(ch - '0');
    }
    if (!numeric || parsed > total) {
      return Err(McpErrorCode::InvalidParams, "Invalid cursor");
    }
    offset = parsed;
  }

  Json arr = Json("[]");
  const size_t limit = (pageSize_ > 0) ? static_cast<size_t>(pageSize_) : total;
  for (size_t i = offset; i < total && i - offset < limit; ++i) {
    arr.push_back(reg.definitions[i]);
  }
  result.add(std::string(arrayKey), std::move(arr));
  if (listTtlMs_ >= 0) {
    result.add("ttlMs", Json(static_cast<int64_t>(listTtlMs_)));
    result.add("cacheScope", Json::str(std::string(listCacheScope_)));
  }
  if (pageSize_ > 0 && offset + limit < total) {
    result.add("nextCursor", Json::str(std::to_string(offset + limit)));
  }
  return Ok(std::move(result));
}

McpResult<Json> Server::handleToolCall(const JsonRpcRequest& req, const RequestMeta& meta,
                                       const std::function<bool()>& cancelled,
                                       const NotifyFn& notify, const std::string& principal) {
  const Json name = member(req.params, "name");
  if (!name.isString()) {
    return Err(McpErrorCode::InvalidParams, "tools/call requires a string name");
  }
  auto it = tools_.handlers.find(std::string(name.toString()));
  if (it == tools_.handlers.end()) {
    return Err(McpErrorCode::InvalidParams, "Unknown tool: " + std::string(name.toString()));
  }
  // inputSchema enforcement (JSON Schema 2020-12): arguments must validate
  // when the tool declares an object-shaped inputSchema.
  const Json def = findToolDefinition(std::string(name.toString()));
  const Json inputSchema = member(def, "inputSchema");
  if (inputSchema.isObject()) {
    const Json arguments = member(req.params, "arguments");
    const Json toValidate = arguments.isObject() ? arguments : Json(R"({})");
    auto verdict = validateSchema(toValidate, inputSchema);
    if (!verdict.has_value()) {
      return Err(McpErrorCode::InvalidParams, "Invalid tool arguments: " + verdict.error().message);
    }
  }

  // Ctx: handlers may emit progress notifications, arbitrary notifications, and
  // MRTR input requests.
  Ctx ctx = makeCtx(req.params, meta, cancelled, notify,
                    "tools/call\n" + std::string(name.toString()), principal);

  McpResult<Json> result = it->second(ctx);
  if (!result.has_value()) return result;

  // outputSchema enforcement: the spec says a server that declares an output
  // schema MUST return structuredContent conforming to it (tools.md "Output
  // Schema"). Only enforced when the handler actually returned structured
  // content - a text-only result stays valid.
  const Json outputSchema = member(def, "outputSchema");
  if (outputSchema.isObject()) {
    const Json structured = member(*result, "structuredContent");
    if (!structured.isNull()) {
      auto verdict = validateSchema(structured, outputSchema);
      if (!verdict.has_value()) {
        return Err(McpErrorCode::InternalError,
                   "Tool result does not conform to outputSchema: " + verdict.error().message);
      }
    }
  }
  return result;
}


std::string Server::registerSubscription(const RequestId& id, const SubscriptionFilter& filter,
                                         NotifSink sink) {
  const std::string subscriptionId = idToString(id);
  std::lock_guard<std::mutex> lock(subscriptionsMutex_);
  subscriptions_[subscriptionId] = Subscription{filter, std::move(sink), id};
  return subscriptionId;
}

void Server::removeSubscription(const std::string& subscriptionId) {
  std::lock_guard<std::mutex> lock(subscriptionsMutex_);
  subscriptions_.erase(subscriptionId);
}

void Server::closeSubscription(const std::string& subscriptionId) {
  NotifSink sink;
  RequestId id = RequestId(int64_t(0));
  {
    std::lock_guard<std::mutex> lock(subscriptionsMutex_);
    auto it = subscriptions_.find(subscriptionId);
    if (it == subscriptions_.end()) return;
    sink = it->second.sink;
    id = it->second.id;
    subscriptions_.erase(it);
  }
  // Graceful closure: the result response to the original listen request,
  // carrying the subscriptionId in _meta.
  Json result;
  result.add(std::string(kResultTypeKey), Json::str(std::string(kResultTypeComplete)));
  Json meta;
  meta.add(std::string(kMetaSubscriptionId), encodeRequestId(id));
  result.add("_meta", std::move(meta));
  if (sink) (void)sink(encodeResultResponse(id, result));
}

void Server::notifyListChanged(const std::string& feature) {
  std::map<std::string, Subscription> snapshot;
  {
    std::lock_guard<std::mutex> lock(subscriptionsMutex_);
    snapshot = subscriptions_;
  }
  const std::string method = "notifications/" + feature + "/list_changed";
  for (auto& [id, sub] : snapshot) {
    const bool match = (feature == "tools" && sub.filter.toolsListChanged) ||
                       (feature == "prompts" && sub.filter.promptsListChanged) ||
                       (feature == "resources" && sub.filter.resourcesListChanged);
    if (!match || !sub.sink) continue;
    Json params;
    Json meta;
    // Type fidelity: the subscriptionId is the listen request's JSON-RPC id,
    // so a numeric id stays numeric (a strict client correlates by value
    // AND type; encoding the map key as a string turned 1 into "1").
    meta.add(std::string(kMetaSubscriptionId), encodeRequestId(sub.id));
    params.add("_meta", std::move(meta));
    (void)sub.sink(encodeNotification(method, params));
  }
}

void Server::notifyResourceUpdated(const std::string& uri) {
  std::map<std::string, Subscription> snapshot;
  {
    std::lock_guard<std::mutex> lock(subscriptionsMutex_);
    snapshot = subscriptions_;
  }
  for (auto& [id, sub] : snapshot) {
    bool match = false;
    for (const auto& candidate : sub.filter.resourceUris) {
      if (candidate == uri) { match = true; break; }
    }
    if (!match || !sub.sink) continue;
    Json params;
    params.add("uri", Json::str(uri));
    Json meta;
    meta.add(std::string(kMetaSubscriptionId), encodeRequestId(sub.id));  // type fidelity
    params.add("_meta", std::move(meta));
    (void)sub.sink(encodeNotification("notifications/resources/updated", params));
  }
}

Json Server::findToolDefinition(const std::string& name) const {
  for (const auto& def : tools_.definitions) {
    const Json defName = member(def, "name");
    if (defName.isString() && defName.toString() == name) return def;
  }
  return Json("null");  // parsed Null - callers probe isObject()
}

// Legacy `initialize` handshake (revisions <= 2025-11-25): params carry
// protocolVersion/clientInfo at the ROOT (no _meta), and the result echoes
// the negotiated version + capabilities + serverInfo. The 2026 resultType
// injection still applies (harmless extra field for legacy clients).
McpResult<Json> Server::handleInitialize(const JsonRpcRequest& req) {
  const Json requested = member(req.params, "protocolVersion");
  std::string negotiated = std::string(toString(ProtocolVersion::Latest));
  if (requested.isString()) {
    if (!versionSupported(requested.toString())) {
      McpError e;
      e.code = McpErrorCode::UnsupportedProtocolVersion;
      e.message = "Unsupported protocol version";
      Json data;
      Json versions = Json("[]");
      for (const auto& wire : supported_) versions.push_back(Json::str(std::string(wire)));
      data.add("supported", std::move(versions));
      data.add("requested", Json::str(requested.toString()));
      e.data = std::move(data);
      return Err(std::move(e));
    }
    negotiated = std::string(requested.toString());
  }
  Json result;
  result.add("protocolVersion", Json::str(negotiated));
  result.add("capabilities", capabilities_);
  Json info;
  info.add("name", Json::str(name_));
  info.add("version", Json::str(version_));
  result.add("serverInfo", std::move(info));
  if (!instructions_.empty()) result.add("instructions", Json::str(instructions_));
  return Ok(std::move(result));
}

bool Server::versionSupported(std::string_view wire) const {
  const auto v = fromString(wire);
  return v.has_value() && supportedSet_.count(*v) > 0;
}

// ---- builder ----------------------------------------------------------------------

Builder& Builder::name(std::string n) { server_.name_ = std::move(n); return *this; }
Builder& Builder::version(std::string v) { server_.version_ = std::move(v); return *this; }
Builder& Builder::instructions(std::string text) { server_.instructions_ = std::move(text); return *this; }
Builder& Builder::pageSize(int n) { server_.pageSize_ = n; return *this; }
Builder& Builder::discoverTtlMs(long ms) { server_.discoverTtlMs_ = ms; return *this; }

Builder& Builder::completions(Handler handler) {
  server_.completionsHandler_ = std::move(handler);
  if (!capabilitiesInclude(server_.capabilities_, "completions")) {
    Json empty;
    server_.capabilities_.add("completions", std::move(empty));
  }
  return *this;
}

Builder& Builder::acceptLegacyClients(bool on) {
  server_.acceptLegacy_ = on;
  return *this;
}

Builder& Builder::requestStateCodec(RequestStateCodec codec) {
  server_.stateCodec_ = std::make_shared<const RequestStateCodec>(std::move(codec));
  return *this;
}

Builder& Builder::requestStatePrincipal(PrincipalResolver resolver) {
  server_.principalResolver_ = std::move(resolver);
  return *this;
}

Builder& Builder::listCache(long ttlMs, std::string cacheScope) {
  server_.listTtlMs_ = ttlMs;
  server_.listCacheScope_ = std::move(cacheScope);
  return *this;
}

Builder& Builder::resultServerInfo(bool enabled) {
  server_.resultServerInfo_ = enabled;
  return *this;
}

Builder& Builder::logLevel(std::string level) {
  server_.logLevel_ = std::move(level);
  return *this;
}

Builder& Builder::supports(ProtocolVersion v) {
  if (server_.supportedSet_.empty()) {
    // First explicit choice replaces the implicit Latest-only default.
    server_.supportedSet_.clear();
    server_.supported_.clear();
  }
  server_.supportedSet_.insert(v);
  server_.supported_.emplace_back(std::string(toString(v)));
  return *this;
}

Builder& Builder::tool(Json definition, Handler handler) {
  const Json name = member(definition, "name");
  if (!name.isString()) {
    std::cerr << "[mcp-cpp] builder: tool definition without name ignored\n";
    return *this;
  }
  const std::string key(name.toString());
  server_.tools_.definitions.push_back(std::move(definition));
  server_.tools_.handlers.emplace(key, std::move(handler));
  // Advertise the capability if not present yet.
  if (!capabilitiesInclude(server_.capabilities_, "tools")) {
    Json empty;
    server_.capabilities_.add("tools", std::move(empty));
  }
  return *this;
}

// D9: resource/tool/prompt capability objects advertise listChanged and
// subscribe flags. The fan-out already works (notifyListChanged /
// notifyResourceUpdated); without the flags a strict client has no reason to
// subscribe.
void Builder::capabilityFlag(std::string_view capability, std::string_view flag) {
  Json entry = member(server_.capabilities_, capability);
  if (!entry.isObject()) entry = Json(R"({})");
  entry.add(std::string(flag), Json(true));
  server_.capabilities_.remove(std::string(capability));
  server_.capabilities_.add(std::string(capability), std::move(entry));
}

Builder& Builder::listChanged(std::string capability) {
  capabilityFlag(capability, "listChanged");
  return *this;
}

bool Server::isKnownMethod(std::string_view method) const {
  return findRoute(method) != nullptr || extensionMethods_.count(std::string(method)) > 0;
}

Builder& Builder::extension(std::string id, std::string method, std::string protocolVersion,
                            Handler handler) {
  if (id.find('/') == std::string::npos) {
    std::cerr << "[mcp-cpp] builder: extension id without a namespace ignored: " << id << '\n';
    return *this;
  }
  // SEP-2133: extension methods are additive. A binding that names a spec
  // route would shadow (or be shadowed by) the server's own handler.
  if (method.empty() || method.find('/') == std::string::npos) {
    std::cerr << "[mcp-cpp] builder: extension method without a namespace ignored: " << method
              << '\n';
    return *this;
  }
  if (isKnownMethod(method)) {
    std::cerr << "[mcp-cpp] builder: extension method shadows a spec method, ignored: " << method
              << '\n';
    return *this;
  }
  if (!protocolVersion.empty() && !fromString(protocolVersion)) {
    std::cerr << "[mcp-cpp] builder: extension method bound to unknown protocol version: "
              << protocolVersion << '\n';
    return *this;
  }
  server_.extensionMethods_[method] =
      Server::ExtensionMethod{std::move(id), std::move(protocolVersion), std::move(handler)};
  // Advertise the extension (empty settings) unless it is already advertised;
  // an explicit extension(id, settings) call keeps its settings.
  const std::string extensionId =
      server_.extensionMethods_[method].extensionId;  // copy before potential rehash
  Json extensions = member(server_.capabilities_, "extensions");
  if (!extensions.isObject()) extensions = Json(R"({})");
  if (member(extensions, extensionId).isNull()) {
    extensions.add(extensionId, Json(R"({})"));
    server_.capabilities_.remove(std::string("extensions"));
    server_.capabilities_.add("extensions", std::move(extensions));
  }
  return *this;
}

Builder& Builder::extension(std::string id, Json settings) {
  // Extension identifiers carry a mandatory prefix ("io.modelcontextprotocol/
  // tasks", vendor ids likewise): a bare word cannot be namespaced and is
  // ignored rather than advertised.
  if (id.find('/') == std::string::npos) {
    std::cerr << "[mcp-cpp] builder: extension id without a namespace ignored: " << id
              << '\n';
    return *this;
  }
  Json extensions = member(server_.capabilities_, "extensions");
  if (!extensions.isObject()) extensions = Json(R"({})");
  extensions.add(id, settings.isObject() ? std::move(settings) : Json(R"({})"));
  server_.capabilities_.remove(std::string("extensions"));
  server_.capabilities_.add("extensions", std::move(extensions));
  return *this;
}

Builder& Builder::resourceSubscribe() {
  capabilityFlag("resources", "subscribe");
  return *this;
}

Builder& Builder::resource(Json definition, Handler readHandler) {
  const Json uri = member(definition, "uri");
  if (!uri.isString()) {
    std::cerr << "[mcp-cpp] builder: resource definition without uri ignored\n";
    return *this;
  }
  const std::string key(uri.toString());
  server_.resources_.definitions.push_back(std::move(definition));
  server_.resources_.handlers.emplace(key, std::move(readHandler));
  if (!capabilitiesInclude(server_.capabilities_, "resources")) {
    Json empty;
    server_.capabilities_.add("resources", std::move(empty));
  }
  return *this;
}

Builder& Builder::resourceTemplate(Json definition, Handler readHandler) {
  const Json tmpl = member(definition, "uriTemplate");
  if (!tmpl.isString()) {
    std::cerr << "[mcp-cpp] builder: resource template without uriTemplate ignored\n";
    return *this;
  }
  const std::string key(tmpl.toString());
  server_.resourceTemplates_.definitions.push_back(std::move(definition));
  server_.resourceTemplates_.handlers.emplace(key, std::move(readHandler));
  if (!capabilitiesInclude(server_.capabilities_, "resources")) {
    Json empty;
    server_.capabilities_.add("resources", std::move(empty));
  }
  return *this;
}

Builder& Builder::prompt(Json definition, Handler handler) {
  const Json name = member(definition, "name");
  if (!name.isString()) {
    std::cerr << "[mcp-cpp] builder: prompt definition without name ignored\n";
    return *this;
  }
  const std::string key(name.toString());
  server_.prompts_.definitions.push_back(std::move(definition));
  server_.prompts_.handlers.emplace(key, std::move(handler));
  if (!capabilitiesInclude(server_.capabilities_, "prompts")) {
    Json empty;
    server_.capabilities_.add("prompts", std::move(empty));
  }
  return *this;
}

Builder& Builder::simpleTool(std::string toolName, std::string description, Handler handler) {
  Json schema = Json(R"({"type":"object"})");
  Json def;
  def.add("name", Json::str(toolName));
  if (!description.empty()) def.add("description", Json::str(description));
  def.add("inputSchema", std::move(schema));
  return tool(std::move(def), std::move(handler));
}

std::shared_ptr<Server> Builder::build() {
  if (server_.supportedSet_.empty()) {
    supports(ProtocolVersion::Latest);
  }
  // Rebuild the supported list in enum order for a stable wire form.
  server_.supported_.clear();
  for (const auto v : {ProtocolVersion::V2024_11_05, ProtocolVersion::V2025_03_26,
                       ProtocolVersion::V2025_06_18, ProtocolVersion::V2025_11_25,
                       ProtocolVersion::V2026_07_28}) {
    if (server_.supportedSet_.count(v) > 0) {
      server_.supported_.emplace_back(std::string(toString(v)));
    }
  }
  // std::mutex members forbid both copy and move: release from the heap.
  return std::shared_ptr<Server>(new Server(std::move(server_)));
}

bool isKnownMethod(std::string_view method) { return findRoute(method) != nullptr; }

}  // namespace server
}  // namespace mcp
