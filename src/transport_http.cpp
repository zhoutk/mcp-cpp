#include "base64.h"
#include "header_params.h"

#include <mcp/transport_http.hpp>

#if defined(MCP_HAS_HTTP)

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <map>
#include <deque>
#include <set>
#include <thread>
#include <utility>

namespace mcp {
namespace http {
namespace {

constexpr const char* kHeaderProtocolVersion = "MCP-Protocol-Version";
constexpr const char* kHeaderMethod = "Mcp-Method";
constexpr const char* kHeaderName = "Mcp-Name";

bool methodNeedsNameHeader(std::string_view method) {
  return method == "tools/call" || method == "resources/read" || method == "prompts/get";
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
  std::atomic_bool running{false};
  std::thread thread;
  std::mutex pumpMutex;                        // guards pumps
  std::vector<std::shared_ptr<Pump>> pumps;    // active subscription streams

  Impl(server::Server& s, std::string ep) : server(s), endpoint(std::move(ep)) {
    originValidator = [](const std::string& origin) {
      // Default policy: no Origin header (non-browser clients) and explicit
      // localhost origins pass; everything else is rejected (DNS-rebinding
      // protection from the spec's Security & Endpoint section).
      if (origin.empty()) return true;
      return origin.rfind("http://localhost", 0) == 0 ||
             origin.rfind("http://127.0.0.1", 0) == 0;
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
      const char* sourceKey = (method == "tools/call" || method == "prompts/get") ? "name" : "uri";
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
        std::vector<std::pair<std::string, std::string>> expected;
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

        for (const auto& [paramName, rawValue] : expected) {
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
          if (!detail::decodeHeaderValue(it->second, decoded) || decoded != rawValue) {
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

  void handlePost(const httplib::Request& req, httplib::Response& res) {
    // 1. Origin validation.
    const std::string origin = req.get_header_value("Origin");
    if (!originValidator(origin)) {
      res.status = 403;
      return;
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
    // legacy HTTP+SSE hosts, which 404 with an empty body).
    if (request != nullptr && !server::isKnownMethod(method)) {
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
    server.processLine(req.body, sink);

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
      res.status = 200;
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
          while (running.load()) {
            std::string line;
            const int got = pump->waitPop(line, 15000);  // keep-alive cadence
            if (got == 1) {
              std::string event = "data: ";
              event += line;
              event += "\n\n";
              if (!sink.write(event.data(), event.size())) break;
            } else if (got == 0) {
              static const char kKeepAlive[] = ":\n\n";
              if (!sink.write(kKeepAlive, 3)) break;  // SSE comment keep-alive
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
  // instead of idling up to the 15s keep-alive cadence.
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

// ---- client transport -----------------------------------------------------------

struct HttpClientTransport::Impl {
  std::string endpoint;
  httplib::Client cli;
  std::map<std::string, std::string, std::less<>> headers;
  std::mutex queueMutex;          // guards queue
  std::condition_variable queueCv;
  std::deque<std::string> queue;  // completed message lines for read()

  void pushQueue(std::string line) {
    {
      std::lock_guard<std::mutex> lock(queueMutex);
      queue.push_back(std::move(line));
    }
    queueCv.notify_one();
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

  void registerSubscription(httplib::Client* c) {
    std::lock_guard<std::mutex> lock(subMutex);
    activeSubscriptions.insert(c);
  }
  void unregisterSubscription(httplib::Client* c) {
    std::lock_guard<std::mutex> lock(subMutex);
    activeSubscriptions.erase(c);
    if (--subscriptionThreads == 0) subCv.notify_all();
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

void HttpClientTransport::setHeader(std::string name, std::string value) {
  // Stores even empty values: the mirrored Mcp-Param-* contract allows an
  // empty string parameter, which must still be sent as a header.
  impl_->headers[name] = std::move(value);
}

void HttpClientTransport::removeHeader(const std::string& name) {
  impl_->headers.erase(name);
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
  // Stop() forces the blocked Post to return promptly; each background
  // thread then unregisters itself and signals completion.
  for (httplib::Client* c : toStop) c->stop();
  std::unique_lock<std::mutex> lock(impl_->subMutex);
  impl_->subCv.wait(lock, [&] { return impl_->subscriptionThreads == 0; });
}

McpResult<std::optional<std::string>> HttpClientTransport::read() {
  // Blocking read: SSE lines arrive asynchronously on the background stream
  // thread, so a momentarily empty queue is not EOF.
  std::unique_lock<std::mutex> lock(impl_->queueMutex);
  if (!impl_->queueCv.wait_for(lock, std::chrono::seconds(30),
                               [&] { return !impl_->queue.empty(); })) {
    return Ok(std::optional<std::string>(std::nullopt));  // idle timeout
  }
  std::string line = std::move(impl_->queue.front());
  impl_->queue.pop_front();
  return Ok(std::optional<std::string>(std::move(line)));
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
    const char* key = (method == "tools/call" || method == "prompts/get") ? "name" : "uri";
    const Json name = member(params, key);
    if (name.isString()) {
      headers.emplace(kHeaderName, detail::encodeHeaderValue(name.toString()));
    }
  }
  for (const auto& [k, v] : impl_->headers) {
    headers.erase(k);  // override the body-derived mirror (e.g. tamper tests)
    headers.emplace(k, v);
  }
  {
    std::lock_guard<std::mutex> lock(impl_->sharedMutex);
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
  // on a background thread with a dedicated connection so the client loop
  // stays responsive; the acknowledgment is queued like any other message.
  if (request != nullptr && request->method == "subscriptions/listen") {
    std::string body(message);
    auto headersCopy = headers;
    auto stream = std::make_unique<httplib::Client>(impl_->cli.host(), impl_->cli.port());
    stream->set_connection_timeout(5);
    stream->set_read_timeout(1, 0);  // 1s cadence so closeSubscriptions() can break in
    httplib::Client* raw = stream.get();

    // Register the handle BEFORE the thread starts so a racing
    // closeSubscriptions() sees it (or the thread's already-stopped flag).
    {
      std::lock_guard<std::mutex> lock(impl_->subMutex);
      if (impl_->subscriptionsClosed) {
        return Ok();  // transport already shut down; don't start a stream
      }
      ++impl_->subscriptionThreads;
      impl_->activeSubscriptions.insert(raw);
    }

    std::thread([this, stream = std::move(stream), body = std::move(body),
                 headersCopy = std::move(headersCopy), raw]() mutable {
      std::string acc;
      auto receiver = [this, &acc](const char* data, size_t len) -> bool {
        acc.append(data, len);
        // Events end with a blank line: "\n\n" or "\r\n\r\n" (spec allows both).
        std::deque<std::string> events;
        {
          static const std::string kEndLf = "\n\n";
          static const std::string kEndCrLf = "\r\n\r\n";
          for (;;) {
            const size_t a = acc.find(kEndLf);
            const size_t b = acc.find(kEndCrLf);
            size_t pos, size;
            if (a == std::string::npos && b == std::string::npos) break;
            if (b == std::string::npos || (a != std::string::npos && a < b)) {
              pos = a; size = kEndLf.size();
            } else {
              pos = b; size = kEndCrLf.size();
            }
            const std::string event = acc.substr(0, pos);
            acc.erase(0, pos + size);
            std::string data = Impl::sseEventData(event);
            if (!data.empty()) events.push_back(std::move(data));
          }
        }
        for (auto& e : events) impl_->pushQueue(std::move(e));
        // Returning false aborts the connection; stop() also forces a short
        // read-timeout wakeup so a blocking Post returns here promptly.
        return !impl_->subscriptionsClosed;
      };
      // Blocks until the server closes the stream (graceful closure, stop,
      // or transport drop), closeSubscriptions() stops the connection, or
      // the 1s read timeout elapses while subscriptionsClosed is set.
      (void)stream->Post(impl_->endpoint.c_str(), headersCopy, body, "application/json",
                         receiver);
      // Tail flush: whatever is left is at most one unterminated event.
      {
        std::string data = Impl::sseEventData(acc);
        if (!data.empty()) impl_->pushQueue(std::move(data));
      }
      impl_->unregisterSubscription(raw);
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
               headersCopy = std::move(headers), raw, isRequest = (request != nullptr)]() mutable {
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
      if (isSse) {
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
      std::lock_guard<std::mutex> lock(impl_->reqMutex);
      impl_->activeRequestStreams.erase(raw);
      --impl_->requestThreads;
    }
    impl_->reqCv.notify_all();
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
      if (unsupported) break;
      // Honour the server's SSE retry advice (sse-retry scenario: reconnecting
      // earlier than the advised delay is a conformance failure). The advice
      // applies to the connection, so the last known value is kept. The margin
      // covers the lag between our seeing the TCP close and the server recording
      // its own close timestamp; waiting longer is compliant. A forced
      // reconnect (a closed POST stream) waits the full advised delay too; a
      // failed connection attempt backs off modestly instead.
      int delay;
      if (forced || result) {
        delay = 100;
        {
          std::lock_guard<std::mutex> lock(impl_->listenMutex);
          if (impl_->retryMs > 0) delay = impl_->retryMs + 250;
        }
      } else {
        delay = 250;  // connection error backoff
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    }
    {
      std::lock_guard<std::mutex> lock(impl_->listenMutex);
      impl_->listenOpen = false;
      --impl_->listenThreads;
    }
    impl_->listenCv.notify_all();
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
