// Client over an in-memory transport: request/response correlation,
// notification interleave, error mapping, MRTR retry, subscription ack.

#include "MemTransport.h"

#include <mcp/mcp.hpp>

#include <gtest/gtest.h>

#include <deque>
#include <string>
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

  std::vector<std::string> sent_;
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
