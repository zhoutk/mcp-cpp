// Client over an in-memory transport: request/response correlation,
// notification interleave, error mapping, MRTR retry, subscription ack.

#include "MemTransport.h"

#include <mcp/mcp.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace mcp;
using namespace mcp::client;

namespace {

// A scripted server: pops one canned response line per write; request ids are
// echoed back so correlation is exercised for real.
class ScriptedTransport final : public ITransport {
 public:
  ScriptedTransport(std::vector<std::string> responses, std::vector<std::string> notifications)
      : responses_(responses.begin(), responses.end()),
        notifications_(notifications.begin(), notifications.end()) {}

  McpResult<std::optional<std::string>> read() override {
    // Serve queued notifications first (interleaving), then responses.
    if (!notifications_.empty()) {
      std::string n = notifications_.front();
      notifications_.pop_front();
      return Ok(std::optional<std::string>(std::move(n)));
    }
    if (responses_.empty()) return Err(McpErrorCode::InternalError, "script exhausted");
    std::string r = responses_.front();
    responses_.pop_front();
    return Ok(std::optional<std::string>(std::move(r)));
  }

  McpResult<void> write(std::string_view message) override {
    sent_.emplace_back(message);
    if (responder_) responder_(std::string(message), responses_);
    return Ok();
  }
  McpResult<void> flush() override { return Ok(); }

  // Record header mutations so mirrored-header behaviour is observable.
  void setHeader(std::string name, std::string value) override {
    headers[name] = std::move(value);
  }
  void removeHeader(const std::string& name) override { headers.erase(name); }

  std::vector<std::string> sent_;
  std::map<std::string, std::string> headers;
  std::function<void(const std::string&, std::deque<std::string>&)> responder_;

 private:
  std::deque<std::string> responses_;
  std::deque<std::string> notifications_;
};

std::string idToString(const RequestId& id) {
  if (const auto* i = std::get_if<int64_t>(&id)) return std::to_string(*i);
  return std::get<std::string>(id);
}

std::string makeResultLine(const RequestId& id, const Json& result) {
  return encodeResultResponse(id, result);
}

std::string makeErrorLine(const RequestId& id, int32_t code, const std::string& message) {
  JsonRpcErrorValue v;
  v.code = code;
  v.message = message;
  return encodeErrorResponse(id, v);
}

Json makeNotificationParams(const std::string& subscriptionId) {
  Json params;
  Json meta;
  meta.add(std::string(kMetaSubscriptionId), Json::str(subscriptionId));
  params.add("_meta", std::move(meta));
  return params;
}

}  // namespace

TEST(Client, RequestCorrelationAndResult) {
  ScriptedTransport t({}, {});
  t.responder_ = [](const std::string& req, std::deque<std::string>& out) {
    ParsedMessage parsed = parseMessage(req);
    const auto* request = std::get_if<JsonRpcRequest>(&parsed);
    ASSERT_NE(request, nullptr);
    out.push_back(makeResultLine(request->id,
                                 Json(R"({"resultType":"complete","tools":[]})")));
  };

  Client client("test-client", "1.0");
  client.connect(t);
  auto result = client.listTools();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ((*result)["resultType"].toString(), "complete");
  ASSERT_EQ(t.sent_.size(), 1u);

  // Every request must carry the per-request metadata.
  ParsedMessage parsed = parseMessage(t.sent_[0]);
  const auto* request = std::get_if<JsonRpcRequest>(&parsed);
  ASSERT_NE(request, nullptr);
  auto meta = decodeRequestMeta(request->params);
  ASSERT_TRUE(meta.has_value());
  EXPECT_EQ(meta->protocolVersion, "2026-07-28");
  EXPECT_EQ(meta->clientInfo->name, "test-client");
}

TEST(Client, NotificationsInterleaveWithoutBreakingCorrelation) {
  // The wire interleaves a subscription notification before the response.
  ScriptedTransport t({}, {encodeNotification("notifications/tools/list_changed",
                                              makeNotificationParams("7"))});
  t.responder_ = [](const std::string& req, std::deque<std::string>& out) {
    ParsedMessage parsed = parseMessage(req);
    const auto* request = std::get_if<JsonRpcRequest>(&parsed);
    out.push_back(makeResultLine(request->id, Json(R"({"ok":true})")));
  };

  int seen = 0;
  Client client("c", "1");
  client.connect(t);
  client.onNotification([&](const JsonRpcNotification& n) {
    seen++;
    EXPECT_EQ(n.method, "notifications/tools/list_changed");
    EXPECT_EQ(n.params["_meta"]["io.modelcontextprotocol/subscriptionId"].toString(), "7");
  });

  auto result = client.request("tools/call", Json(R"({"name":"x"})"));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(seen, 1);
}

TEST(Client, JsonRpcErrorBecomesMcpError) {
  ScriptedTransport t({}, {});
  t.responder_ = [](const std::string& req, std::deque<std::string>& out) {
    ParsedMessage parsed = parseMessage(req);
    const auto* request = std::get_if<JsonRpcRequest>(&parsed);
    out.push_back(makeErrorLine(request->id, -32022,
                                "Unsupported protocol version"));
  };
  Client client("c", "1");
  client.connect(t);
  auto result = client.listTools();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, McpErrorCode::UnsupportedProtocolVersion);
}

TEST(Client, UnknownErrorCodePreserved) {
  // Application-defined codes outside the reserved range must survive.
  ScriptedTransport t({}, {});
  t.responder_ = [](const std::string& req, std::deque<std::string>& out) {
    ParsedMessage parsed = parseMessage(req);
    const auto* request = std::get_if<JsonRpcRequest>(&parsed);
    out.push_back(makeErrorLine(request->id, -31000, "app error"));
  };
  Client client("c", "1");
  client.connect(t);
  auto result = client.listTools();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(static_cast<int32_t>(result.error().code), -31000);
}

TEST(Client, InputRequiredResultSurfaced) {
  ScriptedTransport t({}, {});
  t.responder_ = [](const std::string& req, std::deque<std::string>& out) {
    ParsedMessage parsed = parseMessage(req);
    const auto* request = std::get_if<JsonRpcRequest>(&parsed);
    out.push_back(makeResultLine(request->id,
                                 Json(R"({"resultType":"input_required","requestState":"s1"})")));
  };
  Client client("c", "1");
  client.connect(t);
  auto result = client.callTool("pay", Json(R"({})"));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(resultTypeOf(*result), kResultTypeInputRequired);

  // MRTR retry: fresh id, original params + inputResponses.
  auto retry = client.callTool("pay", Json(R"({})"), Json(R"({"github_login":{"value":"u"}})"));
  ASSERT_TRUE(retry.has_value());
  ParsedMessage parsed = parseMessage(t.sent_.back());
  const auto* request = std::get_if<JsonRpcRequest>(&parsed);
  EXPECT_TRUE(request->params["inputResponses"].isObject());
}

// Round one asks for input and issues a state; the automatic retry must echo
// that state and carry the handler's inputResponses.
TEST(ClientMrtr, CallToolWithInputFulfilsRounds) {
  ScriptedTransport t({}, {});
  t.responder_ = [](const std::string& req, std::deque<std::string>& out) {
    ParsedMessage parsed = parseMessage(req);
    const auto* request = std::get_if<JsonRpcRequest>(&parsed);
    ASSERT_NE(request, nullptr);
    if (member(request->params, "requestState").isNull()) {
      out.push_back(makeResultLine(request->id, Json(R"({"resultType":"input_required",
          "inputRequests":{"login":{"method":"elicitation/create","params":{}}},
          "requestState":"s1"})")));
    } else {
      out.push_back(makeResultLine(request->id, Json(R"({"resultType":"complete","ok":true})")));
    }
  };

  int handlerCalls = 0;
  Client client("c", "1");
  client.connect(t);
  auto result = client.callToolWithInput("pay", Json(R"({})"),
      [&](const Json& requests) -> McpResult<Json> {
        ++handlerCalls;
        EXPECT_TRUE(member(requests, "login").isObject());
        return Ok(Json(R"({"login":{"value":"u"}})"));
      });
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(resultTypeOf(*result), "complete");
  EXPECT_EQ(handlerCalls, 1);

  ASSERT_EQ(t.sent_.size(), 2u);
  ParsedMessage retryParsed = parseMessage(t.sent_[1]);
  const auto* retryReq = std::get_if<JsonRpcRequest>(&retryParsed);
  ASSERT_NE(retryReq, nullptr);
  EXPECT_EQ(member(retryReq->params, "requestState").toString(), "s1");
  EXPECT_TRUE(member(retryReq->params, "inputResponses").isObject());
}

TEST(ClientMrtr, CallToolWithInputStopsAtMaxRounds) {
  ScriptedTransport t({}, {});
  t.responder_ = [](const std::string& req, std::deque<std::string>& out) {
    ParsedMessage parsed = parseMessage(req);
    const auto* request = std::get_if<JsonRpcRequest>(&parsed);
    ASSERT_NE(request, nullptr);
    out.push_back(makeResultLine(request->id,
                                 Json(R"({"resultType":"input_required","requestState":"loop"})")));
  };

  int handlerCalls = 0;
  Client client("c", "1");
  client.connect(t);
  auto result = client.callToolWithInput("pay", Json(R"({})"),
      [&](const Json&) -> McpResult<Json> {
        ++handlerCalls;
        return Ok(Json(R"({})"));
      },
      /*maxRounds=*/2);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, McpErrorCode::InternalError);
  EXPECT_EQ(handlerCalls, 2);
  EXPECT_EQ(t.sent_.size(), 3u);  // initial call + one retry per round
}

TEST(ClientMrtr, CallToolWithInputPropagatesHandlerError) {
  ScriptedTransport t({}, {});
  t.responder_ = [](const std::string& req, std::deque<std::string>& out) {
    ParsedMessage parsed = parseMessage(req);
    const auto* request = std::get_if<JsonRpcRequest>(&parsed);
    ASSERT_NE(request, nullptr);
    out.push_back(makeResultLine(request->id,
                                 Json(R"({"resultType":"input_required","requestState":"s1"})")));
  };

  Client client("c", "1");
  client.connect(t);
  auto result = client.callToolWithInput("pay", Json(R"({})"),
      [](const Json&) -> McpResult<Json> {
        return Err(McpErrorCode::InvalidParams, "user declined");
      });
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, McpErrorCode::InvalidParams);
  EXPECT_EQ(t.sent_.size(), 1u);  // no retry after a handler failure
}

TEST(Client, SubscribeReceivesAcknowledgedThenNotifications) {
  ScriptedTransport t({}, {});
  t.responder_ = [](const std::string& req, std::deque<std::string>& out) {
    ParsedMessage parsed = parseMessage(req);
    const auto* request = std::get_if<JsonRpcRequest>(&parsed);
    if (request->method == "subscriptions/listen") {
      out.push_back(encodeNotification(
          "notifications/subscriptions/acknowledged",
          makeNotificationParams(idToString(request->id))));
    }
  };
  Client client("c", "1");
  client.connect(t);
  auto ack = client.subscribe(Json(R"({"notifications":{"toolsListChanged":true}})"));
  ASSERT_TRUE(ack.has_value());  // resolved by the acknowledgment notification
  EXPECT_EQ((*ack)["_meta"]["io.modelcontextprotocol/subscriptionId"].toString(), "1");
}

TEST(Client, CancelSendsNotification) {
  ScriptedTransport t({}, {});
  Client client("c", "1");
  client.connect(t);
  auto r = client.cancel(RequestId(int64_t(42)), "user asked");
  ASSERT_TRUE(r.has_value());
  ParsedMessage parsed = parseMessage(t.sent_[0]);
  const auto* n = std::get_if<JsonRpcNotification>(&parsed);
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(n->method, "notifications/cancelled");
  EXPECT_EQ(n->params["requestId"].toInt64(), 42);
  EXPECT_EQ(n->params["reason"].toString(), "user asked");
}

TEST(Client, NotConnectedIsInternalError) {
  Client client("c", "1");
  auto r = client.listTools();
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, McpErrorCode::InternalError);
}

// ---- progress opt-in (basic/patterns/progress.md) --------------------------------

TEST(ClientProgress, EmbedsTokenAndDeliversNotifications) {
  // The scripted server sends one progress notification, then the result.
  Json progressParams;
  progressParams.add("progressToken", Json::str("tok-9"));
  progressParams.add("progress", Json(50.0));
  progressParams.add("total", Json(100.0));
  std::string progressLine = encodeNotification("notifications/progress", progressParams);
  std::string resultLine = makeResultLine(RequestId(int64_t(1)), Json(R"({"ok":true})"));

  ScriptedTransport t({resultLine}, {progressLine});
  Client c("probe", "1.0");
  c.connect(t);

  std::vector<JsonRpcNotification> seen;
  c.onNotification([&](const JsonRpcNotification& n) { seen.push_back(n); });

  auto r = c.callTool("work", Json(R"({})"), Json(nullptr), RequestId(std::string("tok-9")));
  ASSERT_TRUE(r.has_value()) << r.error().message;

  // The outgoing request carried the token in params._meta.
  ASSERT_FALSE(t.sent_.empty());
  auto sent = parseMessage(t.sent_[0]);
  auto* req = std::get_if<JsonRpcRequest>(&sent);
  ASSERT_NE(req, nullptr);
  EXPECT_EQ(req->params["_meta"]["progressToken"].toString(), "tok-9");

  // The interleaved notification was surfaced, not swallowed.
  ASSERT_EQ(seen.size(), 1u);
  EXPECT_EQ(seen[0].method, "notifications/progress");
  EXPECT_DOUBLE_EQ(seen[0].params["progress"].toDouble(), 50.0);
}

TEST(ClientProgress, OmitsTokenWhenNotRequested) {
  ScriptedTransport t({makeResultLine(RequestId(int64_t(1)), Json(R"({"ok":true})"))}, {});
  Client c("probe", "1.0");
  c.connect(t);
  auto r = c.callTool("work", Json(R"({})"));
  ASSERT_TRUE(r.has_value()) << r.error().message;
  auto sent = parseMessage(t.sent_[0]);
  auto* req = std::get_if<JsonRpcRequest>(&sent);
  ASSERT_NE(req, nullptr);
  EXPECT_FALSE(req->params["_meta"].contains(std::string("progressToken")));
}

// ---- legacy initialize handshake -------------------------------------------------

TEST(ClientInitialize, SendsLegacyHandshakeThenInitialized) {
  ScriptedTransport t({}, {});
  Client c("probe", "1.0");
  c.connect(t);
  // Respond to initialize with a legacy result carrying the negotiated version.
  t.responder_ = [](const std::string& sent, std::deque<std::string>& responses) {
    static bool first = true;
    if (first) {
      first = false;
      auto parsed = parseMessage(sent);
      auto* req = std::get_if<JsonRpcRequest>(&parsed);
      ASSERT_NE(req, nullptr);
      // Legacy metadata lives at the params root - no _meta envelope.
      EXPECT_EQ(req->method, "initialize");
      EXPECT_EQ(member(req->params, "protocolVersion").toString(), "2025-11-25");
      EXPECT_FALSE(member(req->params, "_meta").isObject());
      responses.push_back(encodeResultResponse(
          req->id, Json(R"({"protocolVersion":"2025-11-25","capabilities":{},
                          "serverInfo":{"name":"m","version":"1"}})")));
    }
  };
  auto r = c.initialize("2025-11-25", Implementation{"probe", std::string(), "1.0",
                                                     std::string(), std::string(),
                                                     Json(nullptr)});
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ(member(r.value(), "protocolVersion").toString(), "2025-11-25");
  // The follow-up notifications/initialized must have gone out.
  bool sawInitialized = false;
  for (const auto& sent : t.sent_) {
    auto parsed = parseMessage(sent);
    if (auto* n = std::get_if<JsonRpcNotification>(&parsed)) {
      if (n->method == "notifications/initialized") sawInitialized = true;
    }
  }
  EXPECT_TRUE(sawInitialized);
}

TEST(ClientInitialize, AdoptsNegotiatedVersionForLaterRequests) {
  // The server negotiates DOWN to 2025-06-18; later requests must declare it.
  ScriptedTransport t({}, {});
  Client c("probe", "1.0");
  c.connect(t);
  std::string declaredVersion;
  t.responder_ = [&](const std::string& sent, std::deque<std::string>& responses) {
    auto parsed = parseMessage(sent);
    if (auto* req = std::get_if<JsonRpcRequest>(&parsed)) {
      if (req->method == "initialize") {
        responses.push_back(encodeResultResponse(
            req->id, Json(R"({"protocolVersion":"2025-06-18","capabilities":{}})")));
        return;
      }
      if (req->method == "tools/list") {
        declaredVersion =
            member(member(req->params, "_meta"), kMetaProtocolVersion).toString();
        responses.push_back(encodeResultResponse(req->id, Json(R"({"tools":[]})")));
      }
    }
  };
  auto r = c.initialize("2025-11-25", Implementation{"probe", std::string(), "1.0",
                                                     std::string(), std::string(),
                                                     Json(nullptr)});
  ASSERT_TRUE(r.has_value());
  (void)c.listTools();
  EXPECT_EQ(declaredVersion, "2025-06-18");
}

TEST(ClientInitialize, DeclaresConfiguredClientCapabilities) {
  // A pre-2026 server gates server-initiated requests on the capabilities the
  // client declares at initialize's params root; sending {} would suppress
  // elicitation/sampling.
  ScriptedTransport t({}, {});
  Client c("probe", "1.0", Json(R"({"elicitation":{},"sampling":{}})"));
  c.connect(t);
  t.responder_ = [](const std::string& sent, std::deque<std::string>& responses) {
    auto parsed = parseMessage(sent);
    auto* req = std::get_if<JsonRpcRequest>(&parsed);
    if (req == nullptr) return;  // notifications/initialized completes the handshake
    responses.push_back(encodeResultResponse(
        req->id, Json(R"({"protocolVersion":"2025-11-25","capabilities":{}})")));
  };
  auto r = c.initialize("2025-11-25", Implementation{"probe", std::string(), "1.0",
                                                     std::string(), std::string(),
                                                     Json(nullptr)});
  ASSERT_TRUE(r.has_value()) << r.error().message;
  auto parsed = parseMessage(t.sent_.front());
  auto* req = std::get_if<JsonRpcRequest>(&parsed);
  ASSERT_NE(req, nullptr);
  const Json caps = member(req->params, "capabilities");
  EXPECT_TRUE(member(caps, "elicitation").isObject());
  EXPECT_TRUE(member(caps, "sampling").isObject());
}

TEST(Client, ErrorResponseWithoutIdIsAccepted) {
  // A legacy peer may answer an unreadable-id request with "id": null. The
  // client is sequential, so that error is the answer to its one outstanding
  // request and must not be dropped (which would stall until the read timeout).
  ScriptedTransport t({}, {});
  t.responder_ = [](const std::string&, std::deque<std::string>& out) {
    out.push_back(
        R"({"jsonrpc":"2.0","id":null,"error":{"code":-32601,"message":"no such method"}})");
  };
  Client c("c", "1");
  c.connect(t);
  auto result = c.listTools();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, McpErrorCode::MethodNotFound);
  EXPECT_EQ(result.error().message, "no such method");
}

TEST(ClientHeaderMirror, SendsEmptyValueAndDropsStaleHeader) {
  // An empty-but-present string argument mirrors as an empty header value and
  // MUST still be sent; a header mirrored for a previous call is removed once
  // the current call no longer carries that argument.
  ScriptedTransport t({}, {});
  t.responder_ = [](const std::string& sent, std::deque<std::string>& out) {
    auto parsed = parseMessage(sent);
    auto* req = std::get_if<JsonRpcRequest>(&parsed);
    ASSERT_NE(req, nullptr);
    if (req->method == "tools/list") {
      out.push_back(encodeResultResponse(req->id, Json(R"({"resultType":"complete","tools":[
        {"name":"t","inputSchema":{"type":"object","properties":{
          "a":{"type":"string","x-mcp-header":"X-A"},
          "b":{"type":"string","x-mcp-header":"X-B"}}}}]})")));
      return;
    }
    out.push_back(encodeResultResponse(req->id, Json(R"({"resultType":"complete"})")));
  };
  Client c("c", "1");
  c.connect(t);
  ASSERT_TRUE(c.listTools().has_value());

  // First call mirrors both, with an EMPTY value for a.
  ASSERT_TRUE(c.callTool("t", Json(R"({"a":"","b":"v"})")).has_value());
  ASSERT_TRUE(t.headers.count("Mcp-Param-X-A") == 1);
  ASSERT_TRUE(t.headers.count("Mcp-Param-X-B") == 1);
  EXPECT_EQ(t.headers["Mcp-Param-X-B"], "v");

  // Second call only carries a: the stale X-B mirror must be removed.
  ASSERT_TRUE(c.callTool("t", Json(R"({"a":"x"})")).has_value());
  EXPECT_EQ(t.headers.count("Mcp-Param-X-B"), 0);
  ASSERT_TRUE(t.headers.count("Mcp-Param-X-A") == 1);
  EXPECT_EQ(t.headers["Mcp-Param-X-A"], "x");
}

// ---- request timeout / cancellation -----------------------------------------------

// A transport whose read() blocks until a response arrives or the client's
// interrupt fires - the shape needed to exercise the request timeout.
class BlockingTransport final : public ITransport {
 public:
  McpResult<std::optional<std::string>> read() override {
    std::unique_lock<std::mutex> lock(m_);
    cv_.wait(lock, [&] { return interrupted_ || !responses_.empty(); });
    if (!responses_.empty()) {
      std::string r = responses_.front();
      responses_.pop_front();
      return Ok(std::optional<std::string>(std::move(r)));
    }
    interrupted_ = false;  // one-shot consume, like HttpClientTransport
    return Err(McpErrorCode::InternalError, "read interrupted");
  }
  McpResult<void> write(std::string_view message) override {
    {
      std::lock_guard<std::mutex> lock(m_);
      lastWrite_ = std::string(message);
    }
    cv_.notify_all();
    return Ok();
  }
  McpResult<void> flush() override { return Ok(); }
  void interruptRead() override {
    {
      std::lock_guard<std::mutex> lock(m_);
      interrupted_ = true;
    }
    cv_.notify_all();
  }
  void clearReadInterrupt() override {
    std::lock_guard<std::mutex> lock(m_);
    interrupted_ = false;
  }

  void deliver(const std::string& line) {
    {
      std::lock_guard<std::mutex> lock(m_);
      responses_.push_back(line);
    }
    cv_.notify_all();
  }

 private:
  std::mutex m_;
  std::condition_variable cv_;
  std::deque<std::string> responses_;
  bool interrupted_ = false;
  std::string lastWrite_;
};

TEST(ClientTimeout, DeadlineFailsTheInFlightRequest) {
  BlockingTransport t;
  Client c("c", "1");
  c.connect(t);
  c.setRequestTimeout(std::chrono::milliseconds(100));

  const auto started = std::chrono::steady_clock::now();
  auto result = c.listTools();
  const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().message.find("timed out"), std::string::npos);
  EXPECT_LT(elapsedMs, 5000);  // fired by the watchdog, not by any 10s fallback

  // The client stays usable: a later exchange is not poisoned by the interrupt.
  t.deliver(encodeResultResponse(RequestId(int64_t(2)), Json(R"({"resultType":"complete","tools":[]})")));
  auto next = c.listTools();
  ASSERT_TRUE(next.has_value());
  EXPECT_EQ(c.requestTimeout(), std::chrono::milliseconds(100));
}

TEST(ClientTimeout, ResponseBeforeTheDeadlineWins) {
  BlockingTransport t;
  Client c("c", "1");
  c.connect(t);
  c.setRequestTimeout(std::chrono::milliseconds(5000));

  std::thread responder([&t] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    t.deliver(encodeResultResponse(RequestId(int64_t(1)),
                                   Json(R"({"resultType":"complete","tools":[]})")));
  });
  auto result = c.listTools();
  responder.join();
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE((*result)["tools"].isArray());
}

TEST(ClientTimeout, CancelCurrentRequestUnblocksTheCaller) {
  BlockingTransport t;
  Client c("c", "1");
  c.connect(t);
  c.setRequestTimeout(std::chrono::milliseconds(60000));  // arms the watchdog

  std::thread canceller([&c] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    c.cancelCurrentRequest();
  });
  const auto started = std::chrono::steady_clock::now();
  auto result = c.listTools();
  const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
  canceller.join();
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().message.find("interrupted"), std::string::npos);
  EXPECT_LT(elapsedMs, 5000);  // not the 60 s deadline
}

// A transport without interruptRead() cannot honor a deadline: the call must
// still WORK (never fail just because a timeout was configured).
TEST(ClientTimeout, NonInterruptibleTransportIgnoresTheDeadline) {
  ScriptedTransport t({encodeResultResponse(RequestId(int64_t(1)), Json(R"({"resultType":"complete","tools":[]})"))}, {});
  Client c("c", "1");
  c.connect(t);
  c.setRequestTimeout(std::chrono::milliseconds(10));  // would fire if honored
  auto result = c.listTools();
  ASSERT_TRUE(result.has_value());
}

// ---- SEP-2243 x-mcp-header validation (D4/D5) -------------------------------------

namespace {
// Serves one tools/list result and echoes tools/call success.
int listToolsThrough(const std::string& toolsJson) {
  ScriptedTransport t({encodeResultResponse(
      RequestId(int64_t(1)),
      Json("{\"resultType\":\"complete\",\"tools\":" + toolsJson + "}"))}, {});
  Client c("c", "1");
  c.connect(t);
  auto listed = c.listTools();
  if (!listed.has_value()) return -1;
  const Json tools = member(*listed, "tools");
  return tools.isArray() ? static_cast<int>(tools.toVector().size()) : -2;
}
}  // namespace

// A valid definition survives; each invalid-annotation shape is excluded.
TEST(ClientToolFilter, ExcludesInvalidHeaderAnnotations) {
  EXPECT_EQ(listToolsThrough(R"json([{"name":"ok","inputSchema":{"type":"object","properties":{
                     "a":{"type":"string","x-mcp-header":"X-A"}}}}])json"),
            1);

  // Non-tchar header name (D4: parentheses are not RFC 9110 tchars).
  EXPECT_EQ(listToolsThrough(R"json([{"name":"bad","inputSchema":{"type":"object","properties":{
                     "a":{"type":"string","x-mcp-header":"X (A)"}}}}])json"),
            0);

  // Annotation nested under items (not statically reachable via properties).
  EXPECT_EQ(listToolsThrough(R"json([{"name":"bad","inputSchema":{"type":"object","properties":{
                     "a":{"type":"array","items":{"type":"object","properties":{
                       "b":{"type":"string","x-mcp-header":"X-B"}}}}}}}])json"),
            0);

  // Annotation inside a composition keyword.
  EXPECT_EQ(listToolsThrough(R"json([{"name":"bad","inputSchema":{"type":"object","properties":{
                     "a":{"anyOf":[{"type":"string","x-mcp-header":"X-A"}]}}}}])json"),
            0);

  // Annotation inside a conditional.
  EXPECT_EQ(listToolsThrough(R"json([{"name":"bad","inputSchema":{"type":"object",
                     "if":{"properties":{"a":{"type":"string","x-mcp-header":"X-A"}}}}}])json"),
            0);

  // Duplicate header name (case-insensitive) across properties.
  EXPECT_EQ(listToolsThrough(R"json([{"name":"bad","inputSchema":{"type":"object","properties":{
                     "a":{"type":"string","x-mcp-header":"X-A"},
                     "b":{"type":"string","x-mcp-header":"x-a"}}}}])json"),
            0);

  // Annotated non-string/integer/boolean property type.
  EXPECT_EQ(listToolsThrough(R"json([{"name":"bad","inputSchema":{"type":"object","properties":{
                     "a":{"type":"number","x-mcp-header":"X-A"}}}}])json"),
            0);

  // A malformed tool must not hide the valid ones in the same response.
  EXPECT_EQ(listToolsThrough(R"json([{"name":"bad","inputSchema":{"type":"object","properties":{
                     "a":{"type":"number","x-mcp-header":"X-A"}}}},
                    {"name":"ok","inputSchema":{"type":"object","properties":{
                     "b":{"type":"string","x-mcp-header":"X-B"}}}}])json"),
            1);
}
