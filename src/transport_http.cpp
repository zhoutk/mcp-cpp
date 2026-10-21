#include "base64.h"
#include "header_params.h"

#include <mcp/transport_http.hpp>

#if defined(MCP_HAS_HTTP)

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <condition_variable>
#include <map>
#include <deque>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace mcp {
namespace http {
namespace {

constexpr const char* kHeaderProtocolVersion = "MCP-Protocol-Version";
constexpr const char* kHeaderMethod = "Mcp-Method";
constexpr const char* kHeaderName = "Mcp-Name";

// The params key mirrored as Mcp-Name, or nullptr when the method carries no
// name. SEP-2663 makes the tasks/* methods mirror params.taskId so
// intermediaries can route polling to the instance holding the task state.
const char* nameParamKey(std::string_view method) {
  if (method == "tools/call" || method == "prompts/get") return "name";
  if (method == "resources/read") return "uri";
  if (method == "tasks/get" || method == "tasks/update" || method == "tasks/cancel") {
    return "taskId";
  }
  return nullptr;
}

bool methodNeedsNameHeader(std::string_view method) {
  return nameParamKey(method) != nullptr;
}

std::string errorBody(const std::optional<RequestId>& id, int32_t code, std::string message) {
  JsonRpcErrorValue v;
  v.code = code;
  v.message = std::move(message);
  return encodeErrorResponse(id, v);
}

std::optional<RequestId> idOf(const ParsedMessage& parsed) {
  if (const auto* r = std::get_if<JsonRpcRequest>(&parsed)) return r->id;
  return std::nullopt;
}

std::string idToString(const RequestId& id) {
  if (const auto* i = std::get_if<int64_t>(&id)) return std::to_string(*i);
  return std::get<std::string>(id);
}

// Thread-safe line queue shared between Server fan-out threads and the SSE
// writer loop of one subscription.
struct Pump {
  std::mutex mu;
  std::condition_variable cv;
  std::deque<std::string> lines;
  bool closed = false;

  void push(std::string line) {
    {
      std::lock_guard<std::mutex> lock(mu);
      lines.push_back(std::move(line));
    }
    cv.notify_one();
  }
  void close() {
    {
      std::lock_guard<std::mutex> lock(mu);
      closed = true;
    }
    cv.notify_all();
  }
  // Pops one line; blocks up to timeoutMs. Returns: 1 = line, 0 = timeout,
  // -1 = closed and drained.
  int waitPop(std::string& out, int timeoutMs) {
    std::unique_lock<std::mutex> lock(mu);
    if (!cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
          return closed || !lines.empty();
        })) {
      return 0;
    }
    if (!lines.empty()) {
      out = std::move(lines.front());
      lines.pop_front();
      return 1;
    }
    return -1;  // closed and drained
  }
};

}  // namespace

// ---- server transport ----------------------------------------------------------

struct HttpServerTransport::Impl {
  server::Server& server;
  httplib::Server svr;
  std::string endpoint;
  OriginValidator originValidator;
  HttpServerTransport::TokenValidator tokenValidator;
  HttpServerTransport::PrincipalExtractor principalExtractor;
  std::atomic_bool running{false};
  // E5: SSE keep-alive cadence in ms (0 = disabled). Atomic because it is read
  // by every subscription's pump thread.
  std::atomic<long long> sseKeepAliveMs{5000};
  std::thread thread;
  std::mutex pumpMutex;                        // guards pumps
  std::vector<std::shared_ptr<Pump>> pumps;    // active subscription streams

  Impl(server::Server& s, std::string ep) : server(s), endpoint(std::move(ep)) {
    originValidator = [](const std::string& origin) {
      // Default policy: no Origin header (non-browser clients) and explicit
      // localhost origins pass; everything else is rejected (DNS-rebinding
      // protection from the spec's Security & Endpoint section). The HOST
      // must match exactly after the scheme: a prefix match would admit
      // http://localhost.evil.example. Any port is fine; bracketed IPv6
      // literals are compared with the brackets kept.
      if (origin.empty()) return true;
      static constexpr std::string_view kSchemeSep = "://";
      const size_t schemeEnd = origin.find(kSchemeSep);
      if (schemeEnd == std::string::npos) return false;
      const std::string scheme = origin.substr(0, schemeEnd);
      if (scheme != "http" && scheme != "https") return false;
      const size_t hostStart = schemeEnd + kSchemeSep.size();
      size_t hostEnd;
      if (hostStart < origin.size() && origin[hostStart] == '[') {
        const size_t close = origin.find(']', hostStart);
        if (close == std::string::npos) return false;
        hostEnd = close + 1;
      } else {
        hostEnd = origin.find_first_of(":/", hostStart);
      }
      std::string host = origin.substr(
          hostStart, hostEnd == std::string::npos ? std::string::npos : hostEnd - hostStart);
      // RFC 6454: reg-names are ASCII case-insensitive; browsers always send
      // lowercase, but be liberal in what we accept.
      for (char& c : host) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
      }
      return host == "localhost" || host == "127.0.0.1" || host == "[::1]";
    };
  }

  // Header mirror validation. Returns {httpStatus, body}; status 0 = pass.
  std::pair<int, std::string> validateHeaders(const httplib::Request& req,
                                              const ParsedMessage& parsed) const {
    const JsonRpcRequest* request = std::get_if<JsonRpcRequest>(&parsed);
    const JsonRpcNotification* notification = std::get_if<JsonRpcNotification>(&parsed);
    if (request == nullptr && notification == nullptr) {
      return {400, errorBody(std::nullopt, static_cast<int32_t>(McpErrorCode::InvalidRequest),
                             "Invalid request: expected a JSON-RPC request or notification")};
    }
    const std::string method = request ? request->method : notification->method;
    const Json& params = request ? request->params : notification->params;
    const std::optional<RequestId> id = idOf(parsed);

    // 2026-07-28 header mirroring (streamable-http.md "Protocol Version
    // Header" / "Server Validation"). The mirrored headers did not exist
    // before 2025-06-18, and the spec says a server that supports such old
    // clients MAY treat a request that OMITS them as protocol version
    // 2025-03-26. We apply that relaxation only in legacy mode; a strict
    // 2026-07-28 server MUST reject a request without the headers.
    const bool legacy = server.acceptsLegacyClients();

    const std::string headerVersion = req.get_header_value(kHeaderProtocolVersion);
    if (headerVersion.empty()) {
      if (!legacy) {
        return {400, errorBody(id, static_cast<int32_t>(McpErrorCode::HeaderMismatch),
                               "Header mismatch: MCP-Protocol-Version header is missing")};
      }
    } else if (request) {
      const Json meta = member(params, "_meta");
      const Json bodyVersion = member(meta, kMetaProtocolVersion);
      if (bodyVersion.isString() && bodyVersion.toString() != headerVersion) {
        return {400, errorBody(id, static_cast<int32_t>(McpErrorCode::HeaderMismatch),
                               "Header mismatch: MCP-Protocol-Version does not match body _meta")};
      }
    }

    const std::string headerMethod = req.get_header_value(kHeaderMethod);
    if (headerMethod.empty()) {
      if (!legacy) {
        return {400, errorBody(id, static_cast<int32_t>(McpErrorCode::HeaderMismatch),
                               "Header mismatch: Mcp-Method header is missing")};
      }
    } else if (headerMethod != method) {
      return {400, errorBody(id, static_cast<int32_t>(McpErrorCode::HeaderMismatch),
                             "Header mismatch: Mcp-Method header does not match body method")};
    }

    if (methodNeedsNameHeader(method)) {
      const std::string headerName = req.get_header_value(kHeaderName);
      const char* sourceKey = nameParamKey(method);
      const Json name = member(params, sourceKey);
      if (!name.isString() || (headerName.empty() && !legacy)) {
        return {400, errorBody(id, static_cast<int32_t>(McpErrorCode::HeaderMismatch),
                               "Header mismatch: Mcp-Name header is missing or malformed")};
      }
      if (!headerName.empty()) {
        std::string decoded;
        if (!detail::decodeHeaderValue(headerName, decoded) || decoded != name.toString()) {
          return {400, errorBody(id, static_cast<int32_t>(McpErrorCode::HeaderMismatch),
                                 "Header mismatch: Mcp-Name header does not match body value")};
        }
      }
    }

    // Mcp-Param-* mirrored tool parameters: every annotated parameter with a
    // value in the body must appear as a header with the matching value, and
    // every mirrored header must correspond to an annotated parameter.
    if (request != nullptr && request->method == "tools/call") {
      const Json toolName = member(request->params, "name");
      const Json toolDef = server.findToolDefinition(std::string(toolName.toString()));
      const Json inputSchema = member(toolDef, "inputSchema");
      if (inputSchema.isObject()) {
        std::vector<detail::HeaderParam> expected;
        detail::collectHeaderParams(inputSchema, member(request->params, "arguments"), {},
                                    expected);

        // index received Mcp-Param-* headers (case-insensitive names)
        std::map<std::string, std::string> received;  // upper(name) -> (name,value)
        for (const auto& field : req.headers) {
          std::string upper;
          for (const char ch : field.first) {
            upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
          }
          if (upper.rfind("MCP-PARAM-", 0) == 0) received[upper] = field.second;
        }

        for (const auto& param : expected) {
          const std::string& paramName = param.name;
          const std::string& rawValue = param.value;
          std::string upper = "MCP-PARAM-";
          for (const char ch : paramName) {
            upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
          }
          auto it = received.find(upper);
          if (it == received.end()) {
            return {400, errorBody(id, static_cast<int32_t>(McpErrorCode::HeaderMismatch),
                                   "Header mismatch: Mcp-Param-" + paramName +
                                       " header is missing")};
          }
          std::string decoded;
          if (!detail::decodeHeaderValue(it->second, decoded) ||
              !headerValuesEqual(decoded, rawValue, param.integer)) {
            return {400, errorBody(id, static_cast<int32_t>(McpErrorCode::HeaderMismatch),
                                   "Header mismatch: Mcp-Param-" + paramName +
                                       " does not match body value")};
          }
          received.erase(upper);
        }
        if (!received.empty()) {
          return {400, errorBody(id, static_cast<int32_t>(McpErrorCode::HeaderMismatch),
                                 "Header mismatch: unrecognized Mcp-Param-* header")};
        }
      }
    }
    return {0, ""};
  }

  // Integer-typed parameters MUST compare numerically (D10): "42.0" and
  // "42e0" are the same argument as 42. Only parameters DECLARED
  // `"type": "integer"` qualify - for everything else (strings, booleans) the
  // spec requires an exact match, so a string property must not accept a
  // numerically-equal spelling.
  static bool headerValuesEqual(const std::string& headerValue, const std::string& bodyValue,
                                bool integerTyped) {
    if (headerValue == bodyValue) return true;
    if (!integerTyped) return false;
    // Decimal literals only: strtod also accepts hex floats and inf/nan, which
    // are not valid renderings of an integer argument and would let two
    // different strings compare equal.
    const auto parseDecimal = [](const std::string& text, double& out) -> bool {
      if (text.empty()) return false;
      for (const char c : text) {
        const bool digit = c >= '0' && c <= '9';
        const bool sign = c == '+' || c == '-';
        const bool exp = c == 'e' || c == 'E';
        if (!digit && !sign && !exp && c != '.') return false;
      }
      char* end = nullptr;
      out = std::strtod(text.c_str(), &end);
      return end != nullptr && *end == '\0';
    };
    double lhs = 0;
    double rhs = 0;
    if (!parseDecimal(headerValue, lhs) || !parseDecimal(bodyValue, rhs)) return false;
    return lhs == rhs;
  }

  void registerHandlers() {
    svr.Post(endpoint, [this](const httplib::Request& req, httplib::Response& res) {
      handlePost(req, res);
    });
    // 2026 revision: no GET stream, no session DELETE.
    svr.Get(endpoint, [](const httplib::Request&, httplib::Response& res) {
      res.status = 405;
    });
    svr.Delete(endpoint, [](const httplib::Request&, httplib::Response& res) {
      res.status = 405;
    });
  }

  // Maps a single-line server response onto the HTTP status. The 2026 spec
  // answers protocol-level rejections with 400: an unsupported protocol
  // version (-32022), a missing required client capability (-32021), a
  // mirrored-header mismatch (-32020; rejected above) - the whole MCP-reserved
  // range - and a request whose _meta envelope is missing or ill-typed
  // (InvalidParams carrying the "_meta" marker from decodeRequestMeta). A TOOL
  // or handler InvalidParams must stay 200 with its JSON-RPC error body, so
  // the marker matters. test_server.cpp locks the _meta marker messages.
  static int statusForResponseLine(const std::string& line) {
    const ParsedMessage parsed = parseMessage(line);
    const auto* err = std::get_if<JsonRpcErrorResponse>(&parsed);
    if (err == nullptr) return 200;
    if (isMcpReservedCode(err->error.code)) return 400;
    if (err->error.code == static_cast<int32_t>(McpErrorCode::InvalidParams) &&
        err->error.message.find("_meta") != std::string::npos) {
      return 400;
    }
    return 200;
  }

  void handlePost(const httplib::Request& req, httplib::Response& res) {
    // 1. Origin validation.
    const std::string origin = req.get_header_value("Origin");
    if (!originValidator(origin)) {
      res.status = 403;
      return;
    }

    // 1b. Token validation (only when the embedder installed a validator).
    if (tokenValidator) {
      std::string challenge;
      if (!tokenValidator(req.get_header_value("Authorization"), challenge)) {
        res.status = 401;
        if (!challenge.empty()) res.set_header("WWW-Authenticate", challenge);
        return;
      }
    }

    // 2. Parse the body.
    const ParsedMessage parsed = parseMessage(req.body);
    const bool isNotification = std::holds_alternative<JsonRpcNotification>(parsed);

    // 3. Header mirror validation.
    const auto [status, body] = validateHeaders(req, parsed);
    if (status != 0) {
      res.status = status;
      res.set_content(body, "application/json");
      return;
    }

    const JsonRpcRequest* request = std::get_if<JsonRpcRequest>(&parsed);
    const JsonRpcNotification* notification = std::get_if<JsonRpcNotification>(&parsed);
    const std::string method = request ? request->method : notification->method;

    // 4. Unknown method -> 404 + -32601 (distinguishes modern servers from
    // legacy HTTP+SSE hosts, which 404 with an empty body). A method the server
    // does not SERVE in the current era also lands here: a modern-only server
    // answers 404 for the methods 2026-07-28 removed, as the spec requires.
    if (request != nullptr && !server.isServedMethod(method)) {
      res.status = 404;
      res.set_content(
          errorBody(request->id, static_cast<int32_t>(McpErrorCode::MethodNotFound),
                    "Method not found: " + method),
          "application/json");
      return;
    }

    // 5. subscriptions/listen -> SSE stream (acknowledged + live notifications).
    if (request != nullptr && method == "subscriptions/listen") {
      startSubscriptionStream(*request, res);
      return;
    }

    // 6. Everything else: dispatch through the shared server logic into a
    // capture sink, then answer.
    std::vector<std::string> lines;
    LineCapture sink(lines);
    std::string principal;
    if (principalExtractor) principal = principalExtractor(req.get_header_value("Authorization"));
    server.processLine(req.body, sink, principal);

    if (isNotification) {
      // Accepted notification: 202 with no body (any server error response to
      // a notification - malformed params - still flows through the capture).
      if (lines.empty()) {
        res.status = 202;
      } else {
        res.status = 400;  // server rejected the notification (invalid params)
        res.set_content(lines.back(), "application/json");
      }
      return;
    }

    if (lines.empty()) {
      res.status = 202;  // no response produced (defensive)
      return;
    }
    if (lines.size() == 1) {
      res.status = statusForResponseLine(lines[0]);
      res.set_content(lines[0], "application/json");
      return;
    }
    // The handler emitted notifications (progress, logging) before its result:
    // stream them back as SSE, result last (streamable-http.md "SSE").
    std::string sseBody;
    for (const auto& line : lines) {
      sseBody += "data: ";
      sseBody += line;
      sseBody += "\n\n";
    }
    // A protocol-level rejection buried in the stream (e.g. the handler
    // emitted progress, then elicitation failed with -32021) still means the
    // REQUEST was invalid, so the status carries it too, not just the body.
    for (const auto& line : lines) {
      const int lineStatus = statusForResponseLine(line);
      if (lineStatus != 200) {
        res.status = lineStatus;
        break;
      }
    }
    res.set_header("X-Accel-Buffering", "no");
    res.set_content(sseBody, "text/event-stream");
  }

  // Collects the line(s) the server writes for one request. A handler that
  // emits notifications (progress, logging) before its result produces more
  // than one line; handlePost streams those back as SSE.
  struct LineCapture final : ITransport {
    explicit LineCapture(std::vector<std::string>& out) : out_(out) {}
    McpResult<std::optional<std::string>> read() override {
      return Ok(std::optional<std::string>(std::nullopt));
    }
    McpResult<void> write(std::string_view message) override {
      out_.emplace_back(message);
      return Ok();
    }
    McpResult<void> flush() override { return Ok(); }

   private:
    std::vector<std::string>& out_;
  };

  void startSubscriptionStream(const JsonRpcRequest& request, httplib::Response& res) {
    auto pump = std::make_shared<Pump>();
    const server::SubscriptionFilter filter = server::parseSubscriptionFilter(request.params);

    res.status = 200;
    res.set_header("Cache-Control", "no-cache");
    res.set_header("X-Accel-Buffering", "no");
    res.set_chunked_content_provider(
        "text/event-stream",
        [this, pump, request, filter](size_t /*offset*/, httplib::DataSink& sink) -> bool {
          // Acknowledgment MUST be the first message on the stream.
          {
            Json ackParams;
            Json ackMeta;
            ackMeta.add(std::string(kMetaSubscriptionId), encodeRequestId(request.id));
            ackParams.add("_meta", std::move(ackMeta));
            ackParams.add("notifications", server::subscriptionFilterToJson(filter));
            std::string line = "data: ";
            line += encodeNotification("notifications/subscriptions/acknowledged", ackParams);
            line += "\n\n";
            if (!sink.write(line.data(), line.size())) return false;
          }

          {
            std::lock_guard<std::mutex> lock(pumpMutex);
            pumps.push_back(pump);
          }
          server.registerSubscription(request.id, filter,
                                      [pump](const std::string& wireLine) {
                                        pump->push(wireLine);
                                      });

          // Pump until the client disconnects or the server is stopped.
          // The keep-alive comment also bounds how long a client can take to
          // notice a closure: httplib's Client::stop() does not always
          // interrupt a blocked read on Windows, so the client wakes on the
          // next keep-alive. 5 s keeps shutdown responsive without chattering.
          while (running.load()) {
            std::string line;
            // When keep-alives are disabled the pump still wakes periodically
            // so stop() (which closes the pump) is not the only exit path;
            // it just writes nothing on an idle tick.
            const long long keepAliveMs = sseKeepAliveMs.load();
            const int waitMs = keepAliveMs > 0 ? static_cast<int>(keepAliveMs) : 60000;
            const int got = pump->waitPop(line, waitMs);
            if (got == 1) {
              std::string event = "data: ";
              event += line;
              event += "\n\n";
              if (!sink.write(event.data(), event.size())) break;
            } else if (got == 0) {
              if (keepAliveMs > 0) {
                static const char kKeepAlive[] = ":\n\n";
                if (!sink.write(kKeepAlive, 3)) break;  // SSE comment keep-alive
              }
            } else {
              break;
            }
          }
          server.removeSubscription(idToString(request.id));
          {
            std::lock_guard<std::mutex> lock(pumpMutex);
            for (auto it = pumps.begin(); it != pumps.end(); ++it) {
              if (*it == pump) { pumps.erase(it); break; }
            }
          }
          sink.done();
          return true;
        });
  }
};

HttpServerTransport::HttpServerTransport(server::Server& server, std::string endpoint)
    : impl_(std::make_unique<Impl>(server, std::move(endpoint))) {
  impl_->registerHandlers();
}

HttpServerTransport::~HttpServerTransport() { stop(); }

McpResult<int> HttpServerTransport::bind(const std::string& host) {
  const int port = impl_->svr.bind_to_any_port(host);
  if (port <= 0) return Err(McpErrorCode::InternalError, "http bind failed");
  return Ok(port);
}

McpResult<int> HttpServerTransport::bind(const std::string& host, int port) {
  if (port == 0) return bind(host);
  if (!impl_->svr.bind_to_port(host, port)) {
    return Err(McpErrorCode::InternalError, "http bind failed on port " + std::to_string(port));
  }
  return Ok(port);
}

McpResult<void> HttpServerTransport::run() {
  impl_->running = true;
  const bool ok = impl_->svr.listen_after_bind();
  impl_->running = false;
  if (!ok) return Err(McpErrorCode::InternalError, "http server stopped");
  return Ok();
}

McpResult<void> HttpServerTransport::runAsync(const std::string& host, int port) {
  if (port == 0) {
    auto bound = bind(host);
    if (!bound.has_value()) return Err(bound.error());
    impl_->thread = std::thread([this] { (void)run(); });
    return Ok();
  }
  impl_->running = true;
  impl_->thread = std::thread([this, host, port] {
    if (!impl_->svr.listen(host, port)) impl_->running = false;
  });
  // Wait until the listener is accepting (or failed).
  for (int i = 0; i < 5000 && !impl_->svr.is_running(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const bool accepting = impl_->svr.is_running();
  if (!accepting) return Err(McpErrorCode::InternalError, "http listen failed");
  return Ok();
}

void HttpServerTransport::stop() {
  // Signal SSE providers BEFORE svr.stop(): httplib's stop() joins worker
  // threads, and is_running() only clears after that join - the providers
  // would deadlock waiting on it. Each open subscription's pump is closed
  // first so its chunked_content_provider exits its waitPop immediately
  // instead of idling up to the keep-alive cadence.
  impl_->running = false;
  {
    std::lock_guard<std::mutex> lock(impl_->pumpMutex);
    for (const auto& pump : impl_->pumps) pump->close();
  }
  impl_->svr.stop();
  if (impl_->thread.joinable()) impl_->thread.join();
}

bool HttpServerTransport::running() const { return impl_->svr.is_running(); }

void HttpServerTransport::setOriginValidator(OriginValidator validator) {
  impl_->originValidator = std::move(validator);
}

void HttpServerTransport::setTokenValidator(TokenValidator validator) {
  impl_->tokenValidator = std::move(validator);
}

void HttpServerTransport::setPrincipalExtractor(PrincipalExtractor extractor) {
  impl_->principalExtractor = std::move(extractor);
}

McpResult<void> HttpServerTransport::setSseKeepAlive(std::chrono::milliseconds interval) {
  const long long ms = static_cast<long long>(interval.count());
  // Upper bound: the pump casts the value to int for its timed wait, so an
  // absurd interval would overflow into a negative wait. One hour is far past
  // any useful keep-alive cadence.
  constexpr long long kMaxKeepAliveMs = 3'600'000;
  if (ms < 0) {
    return Err(McpErrorCode::InvalidParams, "SSE keep-alive interval must not be negative");
  }
  if (ms > kMaxKeepAliveMs) {
    return Err(McpErrorCode::InvalidParams,
               "SSE keep-alive interval must not exceed 3600000 ms (1 hour)");
  }
  if (ms > 0 && ms < 100) {
    return Err(McpErrorCode::InvalidParams,
               "SSE keep-alive interval must be 0 (disabled) or at least 100 ms");
  }
  impl_->sseKeepAliveMs.store(ms);
  return Ok();
}

// ---- client transport -----------------------------------------------------------

struct HttpClientTransport::Impl {
  std::string endpoint;
  httplib::Client cli;
  std::map<std::string, std::string, std::less<>> headers;
  std::mutex queueMutex;          // guards queue
  std::condition_variable queueCv;
  std::deque<std::string> queue;  // completed message lines for read()
  // Set by the destructor: read() returns orderly EOF. Guarded by queueMutex.
  bool transportClosed = false;
  // One-shot read interrupt (see ITransport::interruptRead). Consumed by the
  // next read(). Guarded by queueMutex.
  bool readInterrupt = false;
  // Threads currently inside read(). The destructor waits for this to drop to
  // zero before impl_ is destroyed: waking a blocked reader is asynchronous,
  // and destroying the mutex/cv under it would be a use-after-free.
  size_t readers = 0;
  // Set when the listening stream could not reconnect (peer unreachable):
  // read() reports a lost connection instead of blocking on a dead server.
  // Cleared on any successful (re)connect and when the peer turns out to be a
  // 405 no-GET server (POST streams then carry the liveness signals).
  // Guarded by queueMutex.
  bool peerGone = false;

  void pushQueue(std::string line) {
    {
      std::lock_guard<std::mutex> lock(queueMutex);
      queue.push_back(std::move(line));
    }
    queueCv.notify_one();
  }

  void setPeerGone(bool gone) {
    if (gone) {
      // The GET listen stream is only the legacy server->client side channel:
      // its failure does NOT mean a call is lost, because the request's own
      // POST stream can still deliver the response (in the sse-retry scenario
      // the server drops the GET stream while tools/call is in flight). Only
      // report a lost peer when no request stream can answer whatever read()
      // is waiting for.
      //
      // NOTE: this window is narrow - the POST thread decrements requestThreads
      // BEFORE it forces a listen-stream reconnect, so a self-inflicted stop()
      // error can land after the count reached zero. Callers therefore also
      // pass the `forced` flag (see the listen loop): never report gone for a
      // stream we stopped ourselves.
      std::lock_guard<std::mutex> reqLock(reqMutex);
      if (requestThreads > 0) return;
    }
    // Notify under the lock, like every other cv wake here.
    std::lock_guard<std::mutex> lock(queueMutex);
    peerGone = gone;
    if (gone) queueCv.notify_all();  // wake a reader blocked on an empty queue
  }

  // ---- subscription lifecycle --------------------------------------------
  // Each subscriptions/listen spawns a dedicated background connection whose
  // receiver runs until the server closes the stream, the transport is
  // destroyed, or closeSubscriptions() is called. `subscriptionsClosed`
  // flips on shutdown; each background thread registers its client handle so
  // closeSubscriptions() can stop() it, then signals completion.

  std::mutex subMutex;                                   // guards sub fields
  std::condition_variable subCv;
  std::set<httplib::Client*> activeSubscriptions;        // handles to stop()
  size_t subscriptionThreads = 0;                        // still-running count
  bool subscriptionsClosed = false;

  // ---- request stream lifecycle -------------------------------------------
  // A request POST may answer with a long-lived SSE stream: the server holds
  // it open while it elicits mid-call (MRTR over streamable HTTP). Receiving
  // it synchronously would block the client loop from answering, so every
  // request POST streams on its own background thread and queues events as
  // they arrive; write() returns immediately.
  std::mutex reqMutex;
  std::condition_variable reqCv;
  // Session header from a legacy-era initialize response (2025-03-26 introduced
  // Mcp-Session-Id; 2026-07-28 removed it). Echoed on every later POST.
  std::string sessionId;
  std::string protocolVersion = "2026-07-28";  // echoed on GET listen streams
  std::set<httplib::Client*> activeRequestStreams;       // handles to stop()
  size_t requestThreads = 0;                             // still-running count
  bool requestStreamsClosed = false;
  // `sessionId` and `protocolVersion` are written on the caller's thread and
  // read on the GET-listen thread. A dedicated mutex keeps them out of the
  // reqMutex -> listenMutex order the SSE drain uses, so no cycle can form.
  std::mutex sharedMutex;

  // ---- listening stream (GET SSE; the 2025-era server->client channel) -----
  // 2025-11-25 servers deliver server-initiated requests (elicitation,
  // sampling) over a GET event stream; a graceful close is reconnectable via
  // Last-Event-ID (basic/transports + sse-retry scenario). 2026-07-28 servers
  // answer GET with 405, which marks the channel unsupported.
  std::mutex listenMutex;
  std::condition_variable listenCv;
  std::set<httplib::Client*> activeListenStreams;        // handles to stop()
  size_t listenThreads = 0;                              // still-running count
  bool listenClosed = false;                             // shutdown
  bool listenOpen = false;                               // a loop is running
  bool listenUnsupported = false;                        // peer said 405
  // Set when a closed POST stream forces the listening stream to reconnect:
  // the loop then waits the advised retry delay before the new GET.
  std::atomic<bool> listenForced{false};
  std::string lastEventId;                               // SSE id: for reconnect
  int retryMs = 0;                                       // SSE retry: advice

  // Set by the caller; invoked when a response carries an authentication
  // challenge. Guarded by sharedMutex so it can be swapped safely.
  HttpClientTransport::AuthChallengeHandler authChallenge;

  // Emits every complete "data: <json>" event currently buffered in `acc`
  // (called with accMutex NOT held - it only touches the queue). Events end
  // with a blank line, which peers write as "\n\n" or "\r\n\r\n" (the SSE
  // spec allows both). An event's payload is ALL its "data:" lines joined
  // with "\n"; "event:"/"id:"/"retry:"/comment lines are ignored - peers do
  // emit "event: message" ahead of the data line.
  static std::string sseEventData(const std::string& event, std::string* eventId = nullptr,
                                  int* retryMs = nullptr) {
    std::string out;
    bool any = false;
    size_t pos = 0;
    if (eventId != nullptr) eventId->clear();
    if (retryMs != nullptr) *retryMs = -1;
    while (pos <= event.size()) {
      const size_t eol = event.find('\n', pos);
      std::string_view line = std::string_view(event).substr(
          pos, eol == std::string::npos ? std::string::npos : eol - pos);
      while (!line.empty() && line.back() == '\r') line.remove_suffix(1);
      if (line.rfind("data:", 0) == 0) {
        std::string_view value = line.substr(5);
        if (!value.empty() && value.front() == ' ') value.remove_prefix(1);
        if (any) out += '\n';
        out.append(value);
        any = true;
      } else if (eventId != nullptr && line.rfind("id:", 0) == 0) {
        std::string_view value = line.substr(3);
        if (!value.empty() && value.front() == ' ') value.remove_prefix(1);
        *eventId = std::string(value);
      } else if (retryMs != nullptr && line.rfind("retry:", 0) == 0) {
        // SSE retry field: reconnect advice in milliseconds.
        std::string_view value = line.substr(6);
        while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
        *retryMs = std::atoi(std::string(value).c_str());
      }
      if (eol == std::string::npos) break;
      pos = eol + 1;
    }
    return any ? out : std::string();
  }

  // SSE retry advice is peer-controlled. Clamp it so a hostile or buggy peer
  // cannot make a reconnect wait for weeks (the old atoi path could overflow
  // and sleep for INT_MAX ms). Applies to both the listening stream and a
  // subscription reconnect.
  static int clampRetryMs(int ms) {
    constexpr int kMinMs = 100;
    constexpr int kMaxMs = 30000;
    if (ms <= 0) return kMinMs;
    return ms > kMaxMs ? kMaxMs : ms;
  }

  // Splits every complete event ("\n\n" or "\r\n\r\n" terminated) out of
  // `acc`, returning the concatenated data payload of each event. Used by the
  // subscription reconnect loop; drainSseIntoQueue does the same while also
  // tracking id/retry.
  static void extractSseEvents(std::string& acc, std::vector<std::string>& out) {
    static const std::string kEndLf = "\n\n";
    static const std::string kEndCrLf = "\r\n\r\n";
    for (;;) {
      const size_t a = acc.find(kEndLf);
      const size_t b = acc.find(kEndCrLf);
      size_t pos, size;
      if (a == std::string::npos && b == std::string::npos) return;
      if (b == std::string::npos || (a != std::string::npos && a < b)) {
        pos = a;
        size = kEndLf.size();
      } else {
        pos = b;
        size = kEndCrLf.size();
      }
      std::string event = acc.substr(0, pos);
      acc.erase(0, pos + size);
      std::string data = sseEventData(event);
      if (!data.empty()) out.push_back(std::move(data));
    }
  }

  bool subscriptionShutdown() {
    std::lock_guard<std::mutex> lock(subMutex);
    return subscriptionsClosed;
  }

  // Returns true when at least one message was queued.
  bool drainSseIntoQueue(std::string& acc) {
    static const std::string kEndLf = "\n\n";
    static const std::string kEndCrLf = "\r\n\r\n";
    bool queued = false;
    for (;;) {
      const size_t a = acc.find(kEndLf);
      const size_t b = acc.find(kEndCrLf);
      size_t pos, size;
      if (a == std::string::npos && b == std::string::npos) return queued;
      if (b == std::string::npos || (a != std::string::npos && a < b)) {
        pos = a; size = kEndLf.size();
      } else {
        pos = b; size = kEndCrLf.size();
      }
      std::string event = acc.substr(0, pos);
      acc.erase(0, pos + size);
      std::string id;
      int retry = -1;
      std::string data = sseEventData(event, &id, &retry);
      // Every SSE stream can advance the reconnection state (the sse-retry
      // scenario puts "id:"/"retry:" on the tools/call stream).
      {
        std::lock_guard<std::mutex> lock(listenMutex);
        if (!id.empty()) lastEventId = id;
        if (retry >= 0) retryMs = retry;
      }
      if (!data.empty()) {
        pushQueue(std::move(data));
        queued = true;
      }
    }
  }

  // Does the response so far look like an SSE stream rather than a JSON body?
  // Peers prefix the events with `data:`/`event:`/`id:`/`retry:` or a comment.
  static bool looksLikeSse(std::string_view acc) {
    return acc.rfind("data:", 0) == 0 || acc.rfind("event:", 0) == 0 ||
           acc.rfind("retry:", 0) == 0 || acc.rfind("id:", 0) == 0 ||
           (acc.size() > 1 && acc[0] == ':');
  }

  // Mcp-Param-* headers mirror a TOOL CALL's arguments; they are meaningless on
  // a GET listen stream (which carries no method/arguments), so the configured
  // header merge skips them. Comparison is case-insensitive (HTTP field names).
  static bool isMirroredParamHeader(const std::string& name) {
    static constexpr std::string_view kPrefix = "mcp-param-";
    if (name.size() < kPrefix.size()) return false;
    for (size_t i = 0; i < kPrefix.size(); ++i) {
      const unsigned char c = static_cast<unsigned char>(name[i]);
      if (static_cast<char>(std::tolower(c)) != kPrefix[i]) return false;
    }
    return true;
  }

  Impl(std::string host, int port, std::string ep)
      : endpoint(std::move(ep)), cli(std::move(host), port) {}
};

HttpClientTransport::HttpClientTransport(std::string host, int port, std::string endpoint)
    : impl_(std::make_unique<Impl>(std::move(host), port, std::move(endpoint))) {
  impl_->cli.set_connection_timeout(5);
  impl_->cli.set_read_timeout(30, 0);  // long enough for SSE test streams
}

HttpClientTransport::~HttpClientTransport() {
  // Wake a reader blocked on an empty queue BEFORE tearing down, so read()
  // returns orderly EOF instead of waiting on a queue that is about to die.
  {
    std::unique_lock<std::mutex> lock(impl_->queueMutex);
    impl_->transportClosed = true;
    impl_->queueCv.notify_all();
    // The wake-up is asynchronous: hold impl_ alive until every reader has
    // left read() (the wait below releases the mutex while waiting).
    impl_->queueCv.wait(lock, [&] { return impl_->readers == 0; });
  }
  // Stop any live streams so their background threads do not outlive the
  // transport (they would push into a destroyed queue).
  closeSubscriptions();
  closeRequestStreams();
  closeListenStreams();
}

void HttpClientTransport::setProtocolVersion(ProtocolVersion v) {
  std::lock_guard<std::mutex> lock(impl_->sharedMutex);
  impl_->protocolVersion = std::string(toString(v));
}

void HttpClientTransport::setAuthChallengeHandler(AuthChallengeHandler handler) {
  std::lock_guard<std::mutex> lock(impl_->sharedMutex);
  impl_->authChallenge = std::move(handler);
}

// Reports a 401/403 WWW-Authenticate challenge, if one is present. Called from
// whichever thread performed the POST; the handler copy keeps the callback out
// of the lock.
void HttpClientTransport::reportAuthChallenge(int status, const std::string& challenge) {
  if (status != 401 && status != 403) return;
  if (challenge.empty()) return;
  AuthChallengeHandler handler;
  {
    std::lock_guard<std::mutex> lock(impl_->sharedMutex);
    handler = impl_->authChallenge;
  }
  if (handler) handler(status, challenge);
}

void HttpClientTransport::setHeader(std::string name, std::string value) {
  // Stores even empty values: the mirrored Mcp-Param-* contract allows an
  // empty string parameter, which must still be sent as a header.
  // sharedMutex: the GET listen thread also reads the configured headers (F3).
  std::lock_guard<std::mutex> lock(impl_->sharedMutex);
  impl_->headers[name] = std::move(value);
}

void HttpClientTransport::removeHeader(const std::string& name) {
  std::lock_guard<std::mutex> lock(impl_->sharedMutex);
  impl_->headers.erase(name);
}

std::string HttpClientTransport::authorizationContext() const {
  // Case-insensitive lookup: header names are case-insensitive on the wire, so
  // "authorization" and "Authorization" name the same context.
  std::lock_guard<std::mutex> lock(impl_->sharedMutex);
  for (const auto& [name, value] : impl_->headers) {
    if (name.size() != 13) continue;
    static constexpr std::string_view kName = "authorization";
    bool match = true;
    for (size_t i = 0; i < kName.size(); ++i) {
      const unsigned char c = static_cast<unsigned char>(name[i]);
      if (static_cast<char>(std::tolower(c)) != kName[i]) {
        match = false;
        break;
      }
    }
    if (match) return value;
  }
  return {};
}

void HttpClientTransport::closeRequestStreams() {
  std::vector<httplib::Client*> toStop;
  {
    std::lock_guard<std::mutex> lock(impl_->reqMutex);
    if (impl_->requestStreamsClosed) return;
    impl_->requestStreamsClosed = true;
    toStop.assign(impl_->activeRequestStreams.begin(), impl_->activeRequestStreams.end());
  }
  for (httplib::Client* c : toStop) c->stop();
  std::unique_lock<std::mutex> lock(impl_->reqMutex);
  impl_->reqCv.wait(lock, [&] { return impl_->requestThreads == 0; });
}

void HttpClientTransport::closeSubscriptions() {
  std::vector<httplib::Client*> toStop;
  {
    std::lock_guard<std::mutex> lock(impl_->subMutex);
    if (impl_->subscriptionsClosed) return;  // idempotent
    impl_->subscriptionsClosed = true;
    toStop.assign(impl_->activeSubscriptions.begin(), impl_->activeSubscriptions.end());
  }
  // Stop() forces the blocked Post to return promptly; the reconnect wait is
  // woken by the notification. Each background thread then decrements the
  // count and signals completion.
  impl_->subCv.notify_all();
  for (httplib::Client* c : toStop) c->stop();
  std::unique_lock<std::mutex> lock(impl_->subMutex);
  impl_->subCv.wait(lock, [&] { return impl_->subscriptionThreads == 0; });
}

McpResult<std::optional<std::string>> HttpClientTransport::read() {
  // Blocking read: SSE lines arrive asynchronously on the background stream
  // threads, so a momentarily empty queue is not EOF. There is deliberately
  // NO idle timeout - a quiet connection is alive, and a result may
  // legitimately take longer than any fixed timeout (the old 30s idle return
  // made Client misjudge a slow request as "closed by peer"). nullopt means
  // orderly teardown (destructor); a lost connection surfaces as an error as
  // soon as the listen loop fails to reconnect.
  std::unique_lock<std::mutex> lock(impl_->queueMutex);
  ++impl_->readers;
  impl_->queueCv.wait(lock, [&] {
    return !impl_->queue.empty() || impl_->transportClosed || impl_->peerGone ||
           impl_->readInterrupt;
  });
  const bool haveLine = !impl_->queue.empty();
  std::string line;
  if (haveLine) {
    line = std::move(impl_->queue.front());
    impl_->queue.pop_front();
  }
  const bool closed = impl_->transportClosed;
  const bool interrupted = impl_->readInterrupt;
  impl_->readInterrupt = false;  // one-shot: consume the interrupt
  --impl_->readers;
  // Notify WHILE STILL HOLDING the lock: the destructor waits for readers==0
  // and then destroys this cv - if the notify happened after unlocking, it
  // could fire on a destroyed cv (TSan: pthread_cond_broadcast after
  // pthread_cond_destroy).
  impl_->queueCv.notify_all();
  lock.unlock();
  if (haveLine) return Ok(std::optional<std::string>(std::move(line)));
  if (closed) return Ok(std::optional<std::string>(std::nullopt));
  if (interrupted) return Err(McpErrorCode::InternalError, "read interrupted");
  return Err(McpErrorCode::InternalError, "connection lost");
}

void HttpClientTransport::interruptRead() {
  // Notify under the lock, like every other cv wake here: the caller can be
  // the Client watchdog, which may race transport destruction.
  std::lock_guard<std::mutex> lock(impl_->queueMutex);
  impl_->readInterrupt = true;
  impl_->queueCv.notify_all();  // wake a reader blocked on an empty queue
}

void HttpClientTransport::clearReadInterrupt() {
  std::lock_guard<std::mutex> lock(impl_->queueMutex);
  impl_->readInterrupt = false;
}

McpResult<void> HttpClientTransport::write(std::string_view message) {
  return postOne(message);
}

McpResult<void> HttpClientTransport::flush() { return Ok(); }

namespace {


}  // namespace

McpResult<void> HttpClientTransport::postOne(std::string_view message) {
  // Derive the mirrored headers from the outgoing message itself.
  const ParsedMessage parsed = parseMessage(message);
  const JsonRpcRequest* request = std::get_if<JsonRpcRequest>(&parsed);
  const JsonRpcNotification* notification = std::get_if<JsonRpcNotification>(&parsed);
  if (request == nullptr && notification == nullptr) {
    // A client-to-server RESPONSE: the answer to a server-initiated request
    // (pre-2026 elicitation/sampling). No mirrored headers apply - post it
    // verbatim, fire-and-forget; a lost answer surfaces as a peer timeout.
    httplib::Headers responseHeaders;
    responseHeaders.emplace("Accept", "application/json, text/event-stream");
    {
      std::lock_guard<std::mutex> lock(impl_->sharedMutex);
      for (const auto& [k, v] : impl_->headers) {
        if (Impl::isMirroredParamHeader(k)) continue;
        responseHeaders.erase(k);
        responseHeaders.emplace(k, v);
      }
      if (!impl_->sessionId.empty()) {
        responseHeaders.emplace("Mcp-Session-Id", impl_->sessionId);
      }
    }
    (void)impl_->cli.Post(impl_->endpoint.c_str(), responseHeaders, std::string(message),
                          "application/json");
    return Ok();
  }
  const std::string method = request ? request->method : notification->method;
  const Json& params = request ? request->params : notification->params;

  httplib::Headers headers;
  headers.emplace("Accept", "application/json, text/event-stream");
  const Json meta = member(params, "_meta");
  const Json version = member(meta, kMetaProtocolVersion);
  if (version.isString()) headers.emplace(kHeaderProtocolVersion, std::string(version.toString()));
  headers.emplace(kHeaderMethod, method);
  if (methodNeedsNameHeader(method)) {
    const char* key = nameParamKey(method);
    const Json name = member(params, key);
    if (name.isString()) {
      headers.emplace(kHeaderName, detail::encodeHeaderValue(name.toString()));
    }
  }
  {
    std::lock_guard<std::mutex> lock(impl_->sharedMutex);
    for (const auto& [k, v] : impl_->headers) {
      headers.erase(k);  // override the body-derived mirror (e.g. tamper tests)
      headers.emplace(k, v);
    }
    if (!impl_->sessionId.empty()) headers.emplace("Mcp-Session-Id", impl_->sessionId);
  }

  // A notification expects a 202 and carries no request id to key an
  // asynchronous error to, so POST it synchronously: a transport failure must
  // reach the caller (Client::cancel(), notifications/initialized) as an Err.
  // A peer that multiplexes a response stream onto the notification POST is
  // still drained into the queue rather than dropped.
  if (request == nullptr) {
    std::string acc;
    bool isSse = false;
    auto receiver = [&](const char* data, size_t len) -> bool {
      {
        std::lock_guard<std::mutex> lock(impl_->reqMutex);
        if (impl_->requestStreamsClosed) return false;
      }
      acc.append(data, len);
      if (!isSse && !acc.empty()) isSse = Impl::looksLikeSse(acc);
      if (isSse) impl_->drainSseIntoQueue(acc);
      return true;
    };
    auto res = impl_->cli.Post(impl_->endpoint.c_str(), headers, std::string(message),
                               "application/json", receiver);
    if (!res) {
      return Err(McpErrorCode::InternalError,
                 "http POST failed: " + httplib::to_string(res.error()));
    }
    reportAuthChallenge(res->status, res->get_header_value("WWW-Authenticate"));
    if (isSse) {
      impl_->drainSseIntoQueue(acc);
    } else if (!acc.empty()) {
      impl_->pushQueue(std::move(acc));
    } else if (res->status >= 400) {
      return Err(McpErrorCode::InternalError,
                 "http POST returned status " + std::to_string(res->status));
    }
    return Ok();
  }

  // subscriptions/listen: the response is a long-lived SSE stream. Serve it
  // on one background thread with a dedicated connection so the client loop
  // stays responsive; the acknowledgment is queued like any other message.
  // The thread RECONNECTS while the transport is alive: a subscription is a
  // durable channel, not a single HTTP request.
  if (request != nullptr && request->method == "subscriptions/listen") {
    std::string body(message);
    auto headersCopy = headers;
    const std::string host = impl_->cli.host();
    const int port = impl_->cli.port();

    // Register the thread slot BEFORE it starts so a racing
    // closeSubscriptions() sees it (or the thread's already-stopped flag).
    {
      std::lock_guard<std::mutex> lock(impl_->subMutex);
      if (impl_->subscriptionsClosed) {
        return Ok();  // transport already shut down; don't start a stream
      }
      ++impl_->subscriptionThreads;
    }

    std::thread([this, host, port, body = std::move(body),
                 headersCopy = std::move(headersCopy)]() mutable {
      // A reconnect re-sends the subscribe request, so the server
      // acknowledges it again; the caller must still see exactly one ack.
      // A graceful closure (a result carrying the subscriptionId) also ends
      // the loop: re-subscribing after the server closed the subscription
      // would resurrect a channel the peer deliberately terminated.
      bool firstAck = true;
      bool closedByPeer = false;
      auto emit = [this, &firstAck, &closedByPeer](std::string data) {
        const ParsedMessage parsed = parseMessage(data);
        if (const auto* notification = std::get_if<JsonRpcNotification>(&parsed)) {
          if (notification->method == "notifications/subscriptions/acknowledged") {
            if (!firstAck) return;  // duplicate acknowledgment from a reconnect
            firstAck = false;
          }
        } else if (std::get_if<JsonRpcResult>(&parsed) != nullptr) {
          closedByPeer = true;  // graceful closure result for this subscription
        }
        impl_->pushQueue(std::move(data));
      };

      while (true) {
        {
          std::lock_guard<std::mutex> lock(impl_->subMutex);
          if (impl_->subscriptionsClosed) break;
        }
        auto stream = std::make_unique<httplib::Client>(host, port);
        stream->set_connection_timeout(5);
        // The server sends an SSE keep-alive comment every 5 s; the read
        // timeout MUST exceed that cadence or an idle subscription is torn
        // down between keep-alives (the old 1 s value did exactly that).
        // Shutdown latency is bounded by the keep-alive cadence: httplib's
        // Client::stop() does not reliably interrupt a blocked read here, so
        // the loop wakes on the next keep-alive comment.
        stream->set_read_timeout(30, 0);
        httplib::Client* raw = stream.get();
        {
          std::lock_guard<std::mutex> lock(impl_->subMutex);
          if (impl_->subscriptionsClosed) break;
          impl_->activeSubscriptions.insert(raw);
        }

        std::string acc;
        bool isSse = false;  // is the response an event stream at all?
        auto receiver = [this, &acc, &emit, &isSse](const char* data, size_t len) -> bool {
          acc.append(data, len);
          if (!isSse && !acc.empty()) isSse = Impl::looksLikeSse(acc);
          if (isSse) {
            std::vector<std::string> events;
            Impl::extractSseEvents(acc, events);
            for (auto& event : events) emit(std::move(event));
          }
          return !impl_->subscriptionShutdown();
        };
        auto post = stream->Post(impl_->endpoint.c_str(), headersCopy, body,
                                 "application/json", receiver);
        if (isSse) {
          // Tail flush: whatever is left is at most one unterminated event.
          std::vector<std::string> events;
          Impl::extractSseEvents(acc, events);
          for (auto& event : events) emit(std::move(event));
        } else if (post && post->status < 400) {
          // A successful response that is not an event stream. It may be the
          // peer's JSON-RPC error for the listen request (some servers answer
          // 200 with an error object instead of an SSE stream) - queue it so
          // subscribe() resolves, and stop: no stream will ever follow. An
          // empty body queues a synthetic error for the same reason; the
          // pending subscribe() has no timeout to fall back on.
          // A 4xx body is not queued here: the rejection path below turns the
          // status into exactly one error, so two replies never race for the
          // same request id.
          if (!acc.empty()) {
            emit(std::move(acc));
            acc.clear();
          } else {
            JsonRpcErrorValue v;
            v.code = static_cast<int32_t>(McpErrorCode::InternalError);
            v.message = "subscription response was not an event stream";
            this->queueSyntheticError(body, v);
          }
        }
        {
          std::lock_guard<std::mutex> lock(impl_->subMutex);
          impl_->activeSubscriptions.erase(raw);
        }

        // A 401/403 on the subscription POST carries the same auth challenge
        // as any other request; report it so an OAuth client can refresh.
        if (post) {
          reportAuthChallenge(post->status, post->get_header_value("WWW-Authenticate"));
        }
        // The server REJECTED the subscribe request (4xx): re-sending it
        // cannot succeed, and silently waiting for an ack that never comes
        // leaves subscribe() hanging until its own timeout. Surface it as a
        // normal JSON-RPC error for the listen request instead. 408/429 mean
        // "later", not "no" - those stay on the reconnect path.
        if (post && post->status >= 400 && post->status != 408 && post->status != 429) {
          JsonRpcErrorValue v;
          v.code = static_cast<int32_t>(McpErrorCode::InternalError);
          v.message =
              "subscription rejected by server (HTTP " + std::to_string(post->status) + ")";
          this->queueSyntheticError(body, v);
          break;
        }

        if (closedByPeer || impl_->subscriptionShutdown()) break;
        // A successful NON-SSE answer is a one-shot reply (usually the
        // JSON-RPC error queued above): the peer is not going to stream on
        // this request, so re-subscribing would spin forever.
        if (!isSse && post && post->status < 400) break;
        // The stream dropped (network error, server restart, read timeout):
        // honour the server's retry advice when it gave one, then re-subscribe.
        int delayMs = 1000;
        {
          std::lock_guard<std::mutex> lock(impl_->listenMutex);
          if (impl_->retryMs > 0) delayMs = Impl::clampRetryMs(impl_->retryMs);
        }
        std::unique_lock<std::mutex> lock(impl_->subMutex);
        if (impl_->subCv.wait_for(lock, std::chrono::milliseconds(delayMs),
                                  [&] { return impl_->subscriptionsClosed; })) {
          break;
        }
      }
      {
        // The count decrement AND the notify stay under the lock: the joiner
        // (closeSubscriptions) wakes on threads==0 and destroys this cv right
        // after - a notify released from the lock could fire on a destroyed
        // cv (TSan).
        std::lock_guard<std::mutex> lock(impl_->subMutex);
        if (impl_->subscriptionThreads > 0) --impl_->subscriptionThreads;
        impl_->subCv.notify_all();
      }
    }).detach();
    return Ok();
  }

  // Stream the response on a background thread (see the request-stream
  // lifecycle note in Impl). write() returns as soon as the POST is in
  // flight; the response's messages reach read() through the queue.
  std::string body(message);
  auto stream = std::make_unique<httplib::Client>(impl_->cli.host(), impl_->cli.port());
  stream->set_connection_timeout(5);
  stream->set_read_timeout(30, 0);  // match cli: a stream may stay open a while
  httplib::Client* raw = stream.get();
  {
    std::lock_guard<std::mutex> lock(impl_->reqMutex);
    if (impl_->requestStreamsClosed) return Ok();  // transport shutting down
    ++impl_->requestThreads;
    impl_->activeRequestStreams.insert(raw);
  }
  std::thread([this, stream = std::move(stream), body = std::move(body),
               headersCopy = std::move(headers), raw, isRequest = (request != nullptr),
               // The legacy `initialize` response is the one that can establish
               // a session (its Mcp-Session-Id must be echoed by the very next
               // request). Hold its messages back until Post() returns and the
               // id is captured, so the caller cannot race ahead of it.
               isHandshake = (request != nullptr && request->method == "initialize")]() mutable {
    std::string acc;       // response bytes not yet turned into queued messages
    bool isSse = false;    // does the response look like an event stream?
    bool delivered = false;  // did the stream deliver a message for this request?
    auto receiver = [&](const char* data, size_t len) -> bool {
      {
        std::lock_guard<std::mutex> lock(impl_->reqMutex);
        if (impl_->requestStreamsClosed) return false;
      }
      acc.append(data, len);
      if (!isSse && !acc.empty()) isSse = Impl::looksLikeSse(acc);
      if (isSse && !isHandshake) {
        std::lock_guard<std::mutex> lock(impl_->reqMutex);
        delivered |= impl_->drainSseIntoQueue(acc);
      }
      return true;
    };
    auto result = raw->Post(impl_->endpoint.c_str(), headersCopy, body,
                            "application/json", receiver);
    if (result) {
      // First response carrying a session header wins (the legacy initialize).
      // 2025-03-26 introduced Mcp-Session-Id; 2026-07-28 removed it, so the
      // header is echoed only when a peer issued one.
      const std::string sid = result->get_header_value("Mcp-Session-Id");
      if (!sid.empty()) {
        std::lock_guard<std::mutex> lock(impl_->sharedMutex);
        if (impl_->sessionId.empty()) impl_->sessionId = sid;
      }
      // Open the GET listening stream after the first successful exchange -
      // this is where a 2025-era server delivers server-initiated requests.
      if (result->status < 400) openListenStreamOnce();
      reportAuthChallenge(result->status, result->get_header_value("WWW-Authenticate"));
    }
    {
      std::lock_guard<std::mutex> lock(impl_->reqMutex);
      if (isSse) {
        delivered |= impl_->drainSseIntoQueue(acc);
      } else if (!acc.empty()) {
        // application/json (or an error body) - one message line.
        impl_->pushQueue(std::move(acc));
        delivered = true;
      }
    }
    // Surface failures the same way the wire would: as a message in the queue.
    // Only when nothing was delivered - a stream that answered the request and
    // then hit its read timeout must not inject a second, spurious error for an
    // already-satisfied request id.
    if (!delivered) {
      if (!result) {
        JsonRpcErrorValue v;
        v.code = static_cast<int32_t>(McpErrorCode::InternalError);
        v.message = "http POST failed: " + httplib::to_string(result.error());
        queueSyntheticError(body, v);
      } else if (result->status >= 400) {
        JsonRpcErrorValue v;
        v.code = static_cast<int32_t>(McpErrorCode::InternalError);
        v.message = "http POST returned status " + std::to_string(result->status);
        queueSyntheticError(body, v);
      }
    }
    {
      // Same as the subscription tail: decrement + notify under the lock, so
      // the joiner cannot destroy the cv before this notify has happened.
      std::lock_guard<std::mutex> lock(impl_->reqMutex);
      impl_->activeRequestStreams.erase(raw);
      --impl_->requestThreads;
      impl_->reqCv.notify_all();
    }
    // A gracefully closed POST stream means the server dropped the connection
    // (SEP-1699): force the listening stream to reconnect - a pending result
    // may be delivered on the reconnected stream. The listen loop applies the
    // advised retry delay when it reopens (see listenForced below).
    // Only when the POST answered nothing: a request whose response arrived
    // inline (or a notification) has no pending result to recover, and
    // reconnecting then would churn the GET stream for no reason.
    if (result && result->status < 400 && isRequest && !delivered) {
      std::vector<httplib::Client*> toStop;
      {
        std::lock_guard<std::mutex> lock(impl_->listenMutex);
        if (!impl_->listenClosed && !impl_->listenUnsupported) {
          impl_->listenForced.store(true);
          toStop.assign(impl_->activeListenStreams.begin(), impl_->activeListenStreams.end());
        }
      }
      for (httplib::Client* c : toStop) c->stop();
    }
  }).detach();
  return Ok();
}

// Builds a JSON-RPC error response for the outgoing request carried by `wire`
// and queues it, so exchange() surfaces transport failures as normal errors.
void HttpClientTransport::queueSyntheticError(const std::string& wire, JsonRpcErrorValue v) {
  const ParsedMessage parsed = parseMessage(wire);
  if (const auto* request = std::get_if<JsonRpcRequest>(&parsed)) {
    impl_->pushQueue(encodeErrorResponse(request->id, v));
  }
}

void HttpClientTransport::openListenStreamOnce() {
  {
    std::lock_guard<std::mutex> lock(impl_->listenMutex);
    if (impl_->listenClosed || impl_->listenOpen || impl_->listenUnsupported) return;
    impl_->listenOpen = true;
    ++impl_->listenThreads;
  }
  std::thread([this]() mutable {
    // Reconnect loop: a graceful server-side close is reconnectable via
    // Last-Event-ID (the sse-retry scenario); a 405 means the peer is a
    // 2026-style server with no GET stream - give up quietly.
    // The small startup delay avoids racing the peer's session registration:
    // a GET that lands before the session is fully established is dropped by
    // some servers, and our reconnect would then be keyed to a dead stream.
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    while (true) {
      {
        std::lock_guard<std::mutex> lock(impl_->listenMutex);
        if (impl_->listenClosed || impl_->listenUnsupported) break;
      }
      // A POST stream that closed without answering sets the forced flag: the
      // peer dropped our stream, so reconnect only after the advised retry
      // delay. This can happen while the loop is idle (e.g. before its very
      // first GET), so it is honoured before opening the connection too.
      if (impl_->listenForced.exchange(false)) {
        int waitMs = 100;
        {
          std::lock_guard<std::mutex> lock(impl_->listenMutex);
          if (impl_->retryMs > 0) waitMs = Impl::clampRetryMs(impl_->retryMs) + 250;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));
        std::lock_guard<std::mutex> lock(impl_->listenMutex);
        if (impl_->listenClosed || impl_->listenUnsupported) break;
      }
      auto stream = std::make_unique<httplib::Client>(impl_->cli.host(), impl_->cli.port());
      stream->set_connection_timeout(5);
      stream->set_read_timeout(30, 0);  // keepalives keep it alive; close reconnects
      httplib::Client* raw = stream.get();
      {
        std::lock_guard<std::mutex> lock(impl_->listenMutex);
        if (impl_->listenClosed) break;
        impl_->activeListenStreams.insert(raw);
      }
      httplib::Headers headers;
      headers.emplace("Accept", "text/event-stream");
      {
        std::lock_guard<std::mutex> lock(impl_->sharedMutex);
        // F3: the legacy listen stream is a request like any other - it must
        // carry the configured headers (Authorization above all). Without this
        // a protected 2025-era server rejected the GET while POSTs succeeded.
        // Mcp-Param-* mirrors belong to a tool call, not to the stream.
        for (const auto& [k, v] : impl_->headers) {
          if (Impl::isMirroredParamHeader(k)) continue;
          headers.erase(k);
          headers.emplace(k, v);
        }
        headers.emplace("MCP-Protocol-Version", impl_->protocolVersion);
        if (!impl_->sessionId.empty()) headers.emplace("Mcp-Session-Id", impl_->sessionId);
      }
      {
        std::lock_guard<std::mutex> lock(impl_->listenMutex);
        if (!impl_->lastEventId.empty()) {
          headers.emplace("Last-Event-ID", impl_->lastEventId);
        }
      }
      std::string acc;
      auto receiver = [&](const char* data, size_t len) -> bool {
        {
          std::lock_guard<std::mutex> lock(impl_->listenMutex);
          if (impl_->listenClosed) return false;
        }
        acc.append(data, len);
        impl_->drainSseIntoQueue(acc);
        return true;
      };
      auto result = raw->Get(impl_->endpoint.c_str(), headers, receiver);
      bool unsupported = false;
      bool forced = false;
      {
        std::lock_guard<std::mutex> lock(impl_->listenMutex);
        impl_->activeListenStreams.erase(raw);
        if (result && result->status == 405) {
          impl_->listenUnsupported = true;
          unsupported = true;
        }
        forced = impl_->listenForced.exchange(false);
      }
      if (unsupported) {
        // 405 no-GET server: this loop is done, and POST streams carry the
        // liveness signals. Clear the flag so read() keeps blocking quietly.
        impl_->setPeerGone(false);
        break;
      }
      // Connection-level failure (refused, reset, timeout): the peer is
      // unreachable RIGHT NOW - read() must surface that instead of blocking
      // forever on a dead server. A completed response means the peer was
      // reachable: re-arm read() for blocking.
      //
      // `forced` means WE stopped this stream: a POST response ended without
      // answering its request, so the pending result can only arrive on a
      // reconnected GET (SEP-1699). The resulting stop() error is self-
      // inflicted and must never be reported as a lost peer - doing so aborted
      // the in-flight call and made the sse-retry scenario flaky under
      // parallel load.
      if (!result) {
        if (!forced) impl_->setPeerGone(true);
      } else if (result->status < 400) {
        impl_->setPeerGone(false);
      }
      // Honour the server's SSE retry advice (sse-retry scenario: reconnecting
      // earlier than the advised delay is a conformance failure). The advice
      // applies to the connection, so the last known value is kept. The margin
      // covers the lag between our seeing the TCP close and the server recording
      // its own close timestamp; waiting longer is compliant.
      //
      // The advice applies to EVERY reconnect, including one triggered by a
      // transport error: a dropped connection often surfaces as an error rather
      // than a clean result, and ignoring the advice there made reconnects
      // early by however long the error path took.
      //
      // With no advice yet, never reconnect eagerly after a FORCED close: under
      // parallel load the `retry:` line can still be in flight when the stream
      // drops, and reconnecting before it is parsed is a conformance failure.
      // SSE's conventional 3 s default covers that window (waiting longer than
      // advised is always compliant). A plain graceful close keeps the prompt
      // 100 ms reconnect, and a failed connection attempt backs off a little.
      int delay = forced ? 3000 : (result ? 100 : 1000);
      {
        std::lock_guard<std::mutex> lock(impl_->listenMutex);
        if (impl_->retryMs > 0) delay = Impl::clampRetryMs(impl_->retryMs) + 250;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    }
    {
      // Same as the subscription/request tails: decrement + notify under the
      // lock, so closeListenStreams() (which joins on threads==0) cannot
      // destroy this cv before the notify has happened (the exact race TSan
      // flagged on this line).
      std::lock_guard<std::mutex> lock(impl_->listenMutex);
      impl_->listenOpen = false;
      --impl_->listenThreads;
      impl_->listenCv.notify_all();
    }
  }).detach();
}

void HttpClientTransport::closeListenStreams() {
  std::vector<httplib::Client*> toStop;
  {
    std::lock_guard<std::mutex> lock(impl_->listenMutex);
    if (impl_->listenClosed) return;
    impl_->listenClosed = true;
    toStop.assign(impl_->activeListenStreams.begin(), impl_->activeListenStreams.end());
  }
  for (httplib::Client* c : toStop) c->stop();
  std::unique_lock<std::mutex> lock(impl_->listenMutex);
  impl_->listenCv.wait(lock, [&] { return impl_->listenThreads == 0; });
}

}  // namespace http
}  // namespace mcp

#endif  // MCP_HAS_HTTP
