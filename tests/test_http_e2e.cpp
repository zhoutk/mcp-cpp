// Streamable HTTP end-to-end: real httplib server + client over loopback.
// Covers the 2026 shape: POST-only, header mirroring, 202 notifications,
// 404 unknown method, 405 GET/DELETE, Origin policy, -32020 header mismatch,
// SSE subscription stream with acknowledgment + live notifications.

#ifdef MCP_HAS_HTTP

#include <mcp/mcp.hpp>
#include <mcp/transport_http.hpp>

#include <httplib.h>

#include <gtest/gtest.h>

#include <atomic>
#include <cctype>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace mcp;
using namespace mcp::server;
using namespace mcp::client;
using namespace mcp::http;

namespace {

// One server + HTTP transport per test (fresh registries, ephemeral port).
struct Fixture {
  std::shared_ptr<Server> server;
  std::unique_ptr<HttpServerTransport> http;
  std::thread runner;
  int port = 0;

  Fixture() {
    Builder b;
    b.name("http-e2e").version("1.0");
    b.simpleTool("echo", "echoes", [](Ctx& c) -> McpResult<Json> {
      Json result;
      result.add("echoed", Json::str(c.params["arguments"]["text"].toString()));
      return Ok(std::move(result));
    });
    b.simpleTool("fail", "always errors", [](Ctx&) -> McpResult<Json> {
      return Err(McpErrorCode::InvalidParams, "intentional failure");
    });
    b.tool(Json(R"({"name":"mirror","description":"mirrors a header param",
        "inputSchema":{"type":"object","properties":{
          "token":{"type":"string","x-mcp-header":"X-Token"}}}})"),
           [](Ctx& c) -> McpResult<Json> {
             Json result;
             result.add("token", Json::str(c.params["arguments"]["token"].toString()));
             return Ok(std::move(result));
           });
    // Emits a notification line first, THEN fails with a protocol-level
    // -32021 (eliciting from a client that did not declare the capability):
    // the multi-line SSE path must carry the 400 status, not just the body.
    b.tool(Json(R"({"name":"progress-then-elicit","description":"x",
        "inputSchema":{"type":"object"}})"),
           [](Ctx& c) -> McpResult<Json> {
             Json p;
             p.add("progress", Json(static_cast<int64_t>(1)));
             c.notify("notifications/progress", std::move(p));
             return c.elicit("go?", Json(R"({"type":"object"})"), "");
           });
    b.resource(Json(R"({"uri":"file:///motd","name":"motd"})"),
               [](Ctx&) -> McpResult<Json> {
                 Json result;
                 Json contents = Json("[]");
                 Json item;
                 item.add("uri", Json::str("file:///motd"));
                 item.add("text", Json::str("welcome"));
                 contents.push_back(std::move(item));
                 result.add("contents", std::move(contents));
                 return Ok(std::move(result));
               });
    server = b.build();

    http = std::make_unique<HttpServerTransport>(*server);
    auto bound = http->bind("127.0.0.1");  // ephemeral port; accepts on run()
    if (!bound.has_value()) return;
    port = *bound;
    runner = std::thread([this] { (void)http->run(); });
    // Wait for the listener.
    for (int i = 0; i < 10000 && !http->running(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));  // deadline: never wait forever
  }

  ~Fixture() {
    http->stop();
    if (runner.joinable()) runner.join();
  }

  std::unique_ptr<Client> makeClient() {
    auto transport = std::make_unique<HttpClientTransport>("127.0.0.1", port);
    auto client = std::make_unique<Client>("http-client", "1.0");
    client->connect(*transport);
    // The transport must outlive the client in this test's scope; keep it.
    transports_.push_back(std::move(transport));
    return client;
  }

  std::vector<std::unique_ptr<HttpClientTransport>> transports_;
};

// Builds a modern request line with the required _meta (for raw-POST tests
// that bypass the Client wrapper).
std::string makeModernRequestLine(const std::string& id, const std::string& method,
                                  const Json& extraParams,
                                  const std::string& version = "2026-07-28") {
  Json params = encodeRequestMeta(version, Json(R"({})"), std::nullopt);
  if (extraParams.isObject()) {
    for (const auto& item : extraParams.getAllKeys().toVector()) {
      params.add(item.toString(), extraParams[item.toString()]);
    }
  }
  return encodeRequest(RequestId(id), method, params);
}

}  // namespace

TEST(HttpE2E, DiscoverAndCallToolOverLoopback) {
  Fixture f;
  auto client = f.makeClient();

  auto disc = client->discover();
  ASSERT_TRUE(disc.has_value());
  EXPECT_EQ((*disc)["resultType"].toString(), "complete");
  ASSERT_TRUE((*disc)["supportedVersions"].isArray());

  auto call = client->callTool("echo", Json(R"({"text":"over http"})"));
  ASSERT_TRUE(call.has_value());
  EXPECT_EQ((*call)["echoed"].toString(), "over http");
}

TEST(HttpE2E, ReadResource) {
  Fixture f;
  auto client = f.makeClient();
  auto r = client->readResource("file:///motd");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ((*r)["contents"][0]["text"].toString(), "welcome");
}

TEST(HttpE2E, UnknownMethodIs404WithJsonRpcBody) {
  Fixture f;
  auto client = f.makeClient();
  auto r = client->request("sampling/create", Json(nullptr));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, McpErrorCode::MethodNotFound);
}

TEST(HttpE2E, HeaderMismatchIsRejected) {
  Fixture f;
  // Send a tools/call whose Mcp-Name header will not match the body: we
  // tamper by setting the header directly on a raw transport.
  auto rawTransport = std::make_unique<HttpClientTransport>("127.0.0.1", f.port);
  Client raw("c", "1");
  raw.connect(*rawTransport);
  rawTransport->setHeader("Mcp-Name", "wrong-name");
  auto r = raw.request("tools/call", Json(R"({"name":"echo","arguments":{"text":"x"}})"));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, McpErrorCode::HeaderMismatch);
}

TEST(HttpE2E, GetAndDeleteAre405) {
  httplib::Client cli("127.0.0.1", 0);
  // covered implicitly by the server registering 405 handlers; direct check:
  Fixture f;
  httplib::Client c("127.0.0.1", f.port);
  auto get = c.Get("/mcp");
  ASSERT_TRUE(get);
  EXPECT_EQ(get->status, 405);
  auto del = c.Delete("/mcp");
  ASSERT_TRUE(del);
  EXPECT_EQ(del->status, 405);
}

TEST(HttpE2E, OriginPolicyRejectsForeignOrigins) {
  Builder b;
  b.name("o").version("1");
  auto server = b.build();
  HttpServerTransport http(*server);
  auto bound = http.bind("127.0.0.1");
  ASSERT_TRUE(bound.has_value());
  std::thread runner([&] { (void)http.run(); });
  for (int i = 0; i < 10000 && !http.running(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));  // deadline: never wait forever

  httplib::Client c("127.0.0.1", *bound);
  httplib::Headers evil{{"Origin", "http://evil.example.com"}};
  auto r = c.Post("/mcp", evil, "{}", "application/json");
  ASSERT_TRUE(r);
  EXPECT_EQ(r->status, 403);

  http.stop();
  runner.join();
}

// The default Origin policy matches the HOST exactly after the scheme: a
// prefix match admitted http://localhost.evil.example (DNS rebinding). Any
// port is allowed; scheme must be http/https; IPv6 literals use brackets.
TEST(HttpE2E, OriginPolicyHostMatchIsExact) {
  Builder b;
  b.name("o").version("1");
  auto server = b.build();
  HttpServerTransport http(*server);
  auto bound = http.bind("127.0.0.1");
  ASSERT_TRUE(bound.has_value());
  std::thread runner([&] { (void)http.run(); });
  for (int i = 0; i < 10000 && !http.running(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));  // deadline: never wait forever

  httplib::Client c("127.0.0.1", *bound);
  auto postWithOrigin = [&](const char* origin) {
    httplib::Headers headers{{"Origin", origin}};
    return c.Post("/mcp", headers, "{}", "application/json");
  };

  // Rejected: foreign hosts, including ones that share the localhost PREFIX.
  for (const char* evil : {"http://localhost.evil.example", "http://localhost.com",
                           "http://LOCALHOST.evil.example", "http://127.0.0.1.evil.com",
                           "http://evil.example.com", "ftp://localhost", "not-an-origin"}) {
    auto r = postWithOrigin(evil);
    ASSERT_TRUE(r) << evil;
    EXPECT_EQ(r->status, 403) << evil;
  }

  // Accepted (a later validation stage answers, not the 403 origin gate):
  // loopback hosts, with or without port, http or https, IPv6 in brackets,
  // and non-lowercase spellings (RFC 6454 reg-names are case-insensitive).
  for (const char* good :
       {"http://localhost", "http://localhost:3000", "http://127.0.0.1",
        "http://127.0.0.1:9000", "https://localhost", "http://[::1]", "http://[::1]:8080",
        "http://LOCALHOST", "http://LocalHost:3000"}) {
    auto r = postWithOrigin(good);
    ASSERT_TRUE(r) << good;
    EXPECT_NE(r->status, 403) << good;
  }

  http.stop();
  runner.join();
}

// An empty Mcp-Param-* value must round-trip over HTTP: the empty string is a
// real header value, and wrapping it in the base64 sentinel produced an
// encoding the receiver could not decode (permanent HeaderMismatch).
TEST(HttpE2E, EmptyHeaderParamValueRoundTrips) {
  Fixture f;
  auto client = f.makeClient();
  ASSERT_TRUE(client->listTools().has_value());

  Json arguments = Json(R"({"token":""})");
  auto result = client->callTool("mirror", arguments);
  ASSERT_TRUE(result.has_value()) << "empty param value must not fail header validation";
  EXPECT_EQ((*result)["token"].toString(), "");
}

// D1: a plain value that LOOKS like the base64 sentinel must be re-encoded.
// Sending it verbatim makes the receiver decode it (and fail), so the call
// could never succeed.
TEST(HttpE2E, SentinelShapedPlainValueIsReEncoded) {
  Fixture f;
  auto client = f.makeClient();
  ASSERT_TRUE(client->listTools().has_value());

  Json arguments = Json(R"({"token":"=?base64?dGVzdA==?="})");
  auto result = client->callTool("mirror", arguments);
  ASSERT_TRUE(result.has_value()) << "sentinel-shaped value must survive re-encoding";
  EXPECT_EQ((*result)["token"].toString(), "=?base64?dGVzdA==?=");
}

TEST(HttpE2E, SubscriptionStreamDeliversNotifications) {
  Fixture f;
  auto client = f.makeClient();

  std::atomic<int> listChanged{0};
  client->onNotification([&](const JsonRpcNotification& n) {
    if (n.method == "notifications/tools/list_changed") listChanged++;
  });

  Json filter = Json(R"({"notifications":{"toolsListChanged":true}})");
  auto ack = client->subscribe(filter);
  ASSERT_TRUE(ack.has_value());
  const std::string subId =
      std::string((*ack)["_meta"]["io.modelcontextprotocol/subscriptionId"].toString());

  // Publish from this thread (as another server thread would).
  f.server->notifyListChanged("tools");
  f.server->notifyListChanged("tools");
  f.server->notifyResourceUpdated("file:///motd");  // filtered out

  // Notifications flow through the SSE stream into the client's callback.
  // The client is blocked nowhere: poll via a harmless request roundtrip and
  // drain the transport queue first. The queued SSE lines are read by the
  // next request()'s read loop, so we simply wait and force a read cycle.
  for (int i = 0; i < 100 && listChanged.load() < 2; ++i) {
    // A real client would block in its own reader thread; here we drive the
    // queue manually with listTools roundtrips.
    (void)client->listTools();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_GE(listChanged.load(), 2);
  EXPECT_FALSE(subId.empty());

  // Close the subscription stream so the background SSE thread exits and the
  // test process can terminate cleanly. Without this the long-lived stream
  // keeps the transport (and the fixture's server thread) alive forever.
  client->closeSubscriptions();
}

// The HTTP server transport dispatches POSTs from httplib's worker pool (64
// threads by default). Every worker calls Server::processLine, so this test
// hammers that path with concurrent tools/call + notifications/cancelled
// requests - the exact mix the review's P0-1 called out. Without the dispatch
// lock this races on Server::cancellations_ (and TSan reports it).
// Namespace scope: see kParallelDispatchIterations in test_server.cpp (MSVC
// C3493 vs Clang -Wunused-lambda-capture).
constexpr int kConcurrentPostIterations = 4;

TEST(HttpE2E, ConcurrentPostsAreSerializedSafely) {
  Fixture f;
  constexpr int kThreads = 64;
  std::atomic<int> failures{0};

  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([&f, &failures, t] {
      HttpClientTransport transport("127.0.0.1", f.port);
      Client client("worker-" + std::to_string(t), "1.0");
      client.connect(transport);
      for (int i = 0; i < kConcurrentPostIterations; ++i) {
        if (i % 2 == 1) {
          // Cancellation for an id this worker never sent: it still exercises
          // the cancellations_ map on a different worker thread.
          auto cancelled =
              client.cancel(RequestId(int64_t(t * 1000 + i)), "concurrency");
          if (!cancelled.has_value()) ++failures;
          continue;
        }
        const std::string text = std::to_string(t) + ":" + std::to_string(i);
        Json arguments;
        arguments.add("text", Json::str(text));
        auto result = client.callTool("echo", arguments);
        if (!result.has_value() || (*result)["echoed"].toString() != text) ++failures;
      }
    });
  }
  for (auto& worker : workers) worker.join();
  EXPECT_EQ(failures.load(), 0);
}

// A subscription must survive silence longer than the old 1 s read timeout.
// The server emits an SSE keep-alive comment every 5 s, so the client's read
// timeout has to be larger than that cadence (30 s) and the stream must not be
// treated as "closed" simply because nothing was said.
TEST(HttpE2E, SubscriptionSurvivesIdlePeriod) {
  Fixture f;
  auto client = f.makeClient();

  std::atomic<int> listChanged{0};
  client->onNotification([&](const JsonRpcNotification& n) {
    if (n.method == "notifications/tools/list_changed") listChanged++;
  });

  Json filter = Json(R"({"notifications":{"toolsListChanged":true}})");
  auto ack = client->subscribe(filter);
  ASSERT_TRUE(ack.has_value());

  std::this_thread::sleep_for(std::chrono::seconds(5));
  f.server->notifyListChanged("tools");

  for (int i = 0; i < 200 && listChanged.load() < 1; ++i) {
    (void)client->listTools();  // drains queued notifications
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_EQ(listChanged.load(), 1);

  client->closeSubscriptions();
}

// A 4xx answer to the subscriptions/listen POST must reach subscribe() as a
// normal error instead of leaving it to wait for an ack that never comes (the
// old code dropped the POST result entirely).
TEST(HttpClientTransport, SubscriptionRejectionSurfacesAsAnError) {
  httplib::Server svr;
  svr.Post("/mcp", [](const httplib::Request&, httplib::Response& res) { res.status = 403; });
  const int port = svr.bind_to_any_port("127.0.0.1");
  std::thread runner([&] { (void)svr.listen_after_bind(); });
  for (int i = 0; i < 10000 && !svr.is_running(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));  // deadline: never wait forever

  HttpClientTransport transport("127.0.0.1", port, "/mcp");
  Client client("reject-e2e", "1.0");
  client.connect(transport);

  const auto started = std::chrono::steady_clock::now();
  auto subscribed = client.subscribe(Json(R"({"notifications":{"toolsListChanged":true}})"));
  const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();

  EXPECT_FALSE(subscribed.has_value());
  if (!subscribed.has_value()) {
    EXPECT_NE(subscribed.error().message.find("subscription rejected"), std::string::npos);
  }
  EXPECT_LT(elapsedMs, 5000);  // must not wait for an ack that never comes

  svr.stop();
  runner.join();
}

// A peer that answers subscriptions/listen with 200 + a JSON-RPC error (not an
// SSE stream) is refusing the subscription in-band. The client must surface
// that error to subscribe() and stop, not drop the body and reconnect forever
// (which, with the idle timeout removed, would also block the caller for good).
TEST(HttpClientTransport, SubscriptionJsonRpcErrorSurfacesAndStopsReconnecting) {
  httplib::Server svr;
  std::atomic<int> posts{0};
  svr.Post("/mcp", [&](const httplib::Request& req, httplib::Response& res) {
    ++posts;
    // Echo the request's own id: a JSON-RPC error for a different id would be
    // skipped by the client's sequential read loop.
    std::string id = "1";
    const size_t p = req.body.find("\"id\":");
    if (p != std::string::npos) {
      size_t start = p + 5;
      while (start < req.body.size() && req.body[start] == ' ') ++start;
      size_t end = start;
      while (end < req.body.size() &&
             (std::isdigit(static_cast<unsigned char>(req.body[end])) || req.body[end] == '-')) {
        ++end;
      }
      if (end > start) id = req.body.substr(start, end - start);
    }
    res.status = 200;
    res.set_content(R"({"jsonrpc":"2.0","id":)" + id +
                        R"(,"error":{"code":-32601,"message":"subscriptions unavailable"}})",
                    "application/json");
  });
  const int port = svr.bind_to_any_port("127.0.0.1");
  std::thread runner([&] { (void)svr.listen_after_bind(); });
  for (int i = 0; i < 10000 && !svr.is_running(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));

  HttpClientTransport transport("127.0.0.1", port, "/mcp");
  Client client("json-error-sub", "1.0");
  client.connect(transport);

  const auto started = std::chrono::steady_clock::now();
  auto subscribed = client.subscribe(Json(R"({"notifications":{"toolsListChanged":true}})"));
  const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();

  EXPECT_FALSE(subscribed.has_value());
  if (!subscribed.has_value()) {
    EXPECT_NE(subscribed.error().message.find("subscriptions unavailable"), std::string::npos);
  }
  EXPECT_LT(elapsedMs, 5000);
  // One POST for the subscription: no reconnect storm behind the error.
  EXPECT_EQ(posts.load(), 1);

  transport.closeSubscriptions();
  svr.stop();
  runner.join();
}

// An authentication challenge on the subscription POST must reach the auth
// handler, exactly like a tools/call 401: otherwise an OAuth client cannot
// learn where the protected-resource metadata lives before re-subscribing.
TEST(HttpClientTransport, SubscriptionAuthChallengeIsReported) {
  httplib::Server svr;
  svr.Post("/mcp", [](const httplib::Request&, httplib::Response& res) {
    res.status = 401;
    res.set_header("WWW-Authenticate",
                   "Bearer error=\"invalid_token\", resource_metadata=\"http://example.test/prm\"");
    res.set_content(R"({"jsonrpc":"2.0","id":1,"error":{"code":-32000,"message":"unauthorized"}})",
                    "application/json");
  });
  const int port = svr.bind_to_any_port("127.0.0.1");
  std::thread runner([&] { (void)svr.listen_after_bind(); });
  for (int i = 0; i < 10000 && !svr.is_running(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));

  HttpClientTransport transport("127.0.0.1", port, "/mcp");
  int status = 0;
  std::string challenge;
  transport.setAuthChallengeHandler([&](int code, const std::string& value) {
    status = code;
    challenge = value;
  });
  Client client("sub-auth", "1.0");
  client.connect(transport);

  auto subscribed = client.subscribe(Json(R"({"notifications":{"toolsListChanged":true}})"));
  EXPECT_FALSE(subscribed.has_value());
  EXPECT_EQ(status, 401);
  EXPECT_NE(challenge.find("resource_metadata="), std::string::npos);

  transport.closeSubscriptions();
  svr.stop();
  runner.join();
}

// 408/429 on the subscription POST mean "later", not "no": the client must
// retry and succeed on a subsequent attempt instead of giving up.
TEST(HttpClientTransport, SubscriptionRetryableStatusRetriesAndSucceeds) {
  httplib::Server svr;
  std::atomic<int> posts{0};
  svr.Post("/mcp", [&](const httplib::Request&, httplib::Response& res) {
    if (posts.fetch_add(1) == 0) {
      res.status = 429;  // "later": the client must come back
      return;
    }
    // Second attempt: acknowledge and hold the stream open with keep-alives.
    res.status = 200;
    res.set_header("Cache-Control", "no-cache");
    res.set_chunked_content_provider(
        "text/event-stream",
        [&, done = std::make_shared<std::atomic<bool>>(false)](size_t, httplib::DataSink& sink) {
          if (!done->exchange(true)) {
            static const std::string ack =
                "data: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/"
                "subscriptions/acknowledged\",\"params\":{\"_meta\":{"
                "\"io.modelcontextprotocol/subscriptionId\":\"1\"}}}\n\n";
            sink.write(ack.data(), ack.size());
          }
          // Keep the stream open so the subscription is live.
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
          return !done->load() || true;  // ends when svr.stop() closes it
        });
  });
  const int port = svr.bind_to_any_port("127.0.0.1");
  std::thread runner([&] { (void)svr.listen_after_bind(); });
  for (int i = 0; i < 10000 && !svr.is_running(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));

  HttpClientTransport transport("127.0.0.1", port, "/mcp");
  Client client("retry-sub", "1.0");
  client.connect(transport);

  auto subscribed = client.subscribe(Json(R"({"notifications":{"toolsListChanged":true}})"));
  ASSERT_TRUE(subscribed.has_value()) << "429 must be retried, not treated as a rejection";
  EXPECT_EQ(posts.load(), 2);

  transport.closeSubscriptions();
  svr.stop();
  runner.join();
}

// A graceful closure (a result carrying the subscriptionId in _meta) ends the
// subscription for good: the client must not re-subscribe behind the server's
// back, and onSubscriptionClosed must fire exactly once.
TEST(HttpClientTransport, GracefulClosureStopsReconnecting) {
  httplib::Server svr;
  std::atomic<int> posts{0};
  std::atomic<bool> closureSent{false};
  svr.Post("/mcp", [&](const httplib::Request& req, httplib::Response& res) {
    // Only subscriptions/listen is a subscription stream; anything else gets a
    // plain JSON answer (the test drains the queue with tools/list).
    if (req.get_header_value("Mcp-Method") != "subscriptions/listen") {
      res.status = 200;
      res.set_content(R"({"jsonrpc":"2.0","id":2,"result":{"resultType":"complete","tools":[]}})",
                      "application/json");
      return;
    }
    posts.fetch_add(1);
    res.status = 200;
    res.set_header("Cache-Control", "no-cache");
    res.set_chunked_content_provider(
        "text/event-stream",
        [&](size_t, httplib::DataSink& sink) -> bool {
          if (!closureSent.exchange(true)) {
            static const std::string ack =
                "data: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/"
                "subscriptions/acknowledged\",\"params\":{\"_meta\":{"
                "\"io.modelcontextprotocol/subscriptionId\":\"1\"}}}\n\n";
            sink.write(ack.data(), ack.size());
            return true;
          }
          // Second provider call (after the test arms the closure): the
          // server-initiated graceful closure, then end the stream.
          static const std::string closure =
              "data: {\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{"
              "\"resultType\":\"complete\",\"_meta\":{"
              "\"io.modelcontextprotocol/subscriptionId\":\"1\"}}}\n\n";
          sink.write(closure.data(), closure.size());
          return false;
        });
  });
  const int port = svr.bind_to_any_port("127.0.0.1");
  std::thread runner([&] { (void)svr.listen_after_bind(); });
  for (int i = 0; i < 10000 && !svr.is_running(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));

  HttpClientTransport transport("127.0.0.1", port, "/mcp");
  Client client("closure-sub", "1.0");
  client.connect(transport);
  std::atomic<int> closedCount{0};
  std::string closedId;
  client.onSubscriptionClosed([&](const std::string& id) {
    closedCount.fetch_add(1);
    closedId = id;
  });

  ASSERT_TRUE(client.subscribe(Json(R"({"notifications":{"toolsListChanged":true}})")).has_value());
  ASSERT_EQ(posts.load(), 1);

  // Arm the closure, then give the client ample time to wrongly re-subscribe.
  closureSent = true;
  std::this_thread::sleep_for(std::chrono::seconds(2));
  EXPECT_EQ(posts.load(), 1);  // no reconnect after a graceful closure
  // The closure event is queued; onSubscriptionClosed fires when an exchange
  // reads past it. Drain with an unrelated request.
  auto drained = client.listTools();
  ASSERT_TRUE(drained.has_value());
  EXPECT_EQ(closedCount.load(), 1);
  EXPECT_EQ(closedId, "1");

  transport.closeSubscriptions();
  svr.stop();
  runner.join();
}
// read() has no idle timeout: a quiet connection must NOT look closed (the
// old 30s idle return made a >30s request look like "closed by peer"). Only
// orderly transport shutdown ends the read - with EOF, not an error.
TEST(HttpClientTransport, ReadBlocksWhileIdleAndReturnsEofOnShutdown) {
  Fixture f;
  auto transport = std::make_unique<HttpClientTransport>("127.0.0.1", f.port, "/mcp");
  Client client("idle-e2e", "1.0");
  client.connect(*transport);
  auto tools = client.listTools();
  ASSERT_TRUE(tools.has_value());

  // The reader thread takes a RAW pointer, not the unique_ptr: unique_ptr::
  // reset() writes the pointer member BEFORE destroying the pointee, so a
  // reader dereferencing the unique_ptr races with reset() (TSan). A plain
  // pointer removes the shared write; the destructor's reader-count handshake
  // already guarantees the reader has left read() before impl_ dies.
  HttpClientTransport* const raw = transport.get();
  std::atomic<bool> done{false};
  bool eof = false;
  std::thread reader([raw, &eof, &done] {
    auto received = raw->read();
    // Reached only on shutdown: orderly EOF, not an idle timeout.
    eof = received.has_value() && !received->has_value();
    done = true;
  });

  // Still blocked after 1s of silence: idleness is not EOF.
  std::this_thread::sleep_for(std::chrono::seconds(1));
  EXPECT_FALSE(done.load());

  transport.reset();  // orderly teardown wakes the reader
  for (int i = 0; i < 10000 && !done.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_TRUE(done.load());
  if (done.load()) {
    reader.join();
  } else {
    reader.detach();  // already failed; avoid blocking the suite on teardown
  }
  EXPECT_TRUE(eof);
}

// Protocol-level rejections answer HTTP 400 (2026 streamable HTTP): an
// unsupported protocol version (-32022) and a missing _meta envelope
// (-32602). A TOOL-level InvalidParams stays 200 with its JSON-RPC error.
// The mirrored headers are SENT correctly in all three cases, so the
// transport's own header validation passes and the SERVER produces the
// error whose code picks the status.
TEST(HttpE2E, ProtocolRejectionsAreHttp400ToolErrorsStay200) {
  Fixture f;
  httplib::Client raw("127.0.0.1", f.port);
  raw.set_connection_timeout(5);
  raw.set_read_timeout(10, 0);
  auto post = [&raw](const httplib::Headers& headers, const std::string& body) {
    return raw.Post("/mcp", headers, body, "application/json");
  };
  auto expectStatus = [](const httplib::Result& res, int status, int32_t code) {
    ASSERT_TRUE(res) << "no response";
    EXPECT_EQ(res->status, status);
    const ParsedMessage parsed = parseMessage(res->body);
    const auto* err = std::get_if<JsonRpcErrorResponse>(&parsed);
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->error.code, code);
  };

  // Unsupported protocol version -> -32022 -> 400. Header and body agree on
  // the unsupported version (a mismatch would be -32020 first).
  {
    httplib::Headers headers = {{"MCP-Protocol-Version", "2024-11-05"},
                                {"Mcp-Method", "tools/list"}};
    expectStatus(post(headers, makeModernRequestLine("1", "tools/list", Json(nullptr),
                                                     "2024-11-05")),
                 400, -32022);
  }

  // Missing _meta -> -32602 -> 400.
  {
    httplib::Headers headers = {{"MCP-Protocol-Version", "2026-07-28"},
                                {"Mcp-Method", "tools/list"}};
    expectStatus(post(headers, encodeRequest(RequestId("2"), "tools/list", Json(R"({})"))),
                 400, -32602);
  }

  // A tool that fails -> -32602 WITHOUT the _meta marker -> 200.
  {
    httplib::Headers headers = {{"MCP-Protocol-Version", "2026-07-28"},
                                {"Mcp-Method", "tools/call"},
                                {"Mcp-Name", "fail"}};
    expectStatus(post(headers, makeModernRequestLine("3", "tools/call",
                                                     Json(R"({"name":"fail"})"))),
                 200, -32602);
  }
}

// A protocol-level rejection in the MULTI-LINE path (handler emitted a
// notification, then the elicitation failed with -32021) must still answer
// HTTP 400 - the body streams as SSE, but the status carries the rejection.
TEST(HttpE2E, ProtocolRejectionInSsePathIsHttp400) {
  Fixture f;
  httplib::Client raw("127.0.0.1", f.port);
  raw.set_connection_timeout(5);
  raw.set_read_timeout(10, 0);
  httplib::Headers headers = {{"MCP-Protocol-Version", "2026-07-28"},
                              {"Mcp-Method", "tools/call"},
                              {"Mcp-Name", "progress-then-elicit"}};
  auto res = raw.Post("/mcp", headers,
                      makeModernRequestLine("1", "tools/call",
                                            Json(R"({"name":"progress-then-elicit"})")),
                      "application/json");
  ASSERT_TRUE(res) << "no response";
  EXPECT_EQ(res->status, 400);
  // The body still streams: the notification line, then the -32021 error.
  EXPECT_NE(res->body.find("-32021"), std::string::npos);
  EXPECT_NE(res->body.find("notifications/progress"), std::string::npos);
}

TEST(HttpClientTransport, PostFailureSurfacesAsAnError) {
  // Grab an ephemeral port, then release it: connecting to it is refused.
  int deadPort = 0;
  {
    httplib::Server probe;
    deadPort = probe.bind_to_any_port("127.0.0.1");
    probe.stop();
  }
  ASSERT_GT(deadPort, 0);
  HttpClientTransport transport("127.0.0.1", deadPort, "/mcp");

  // A notification POST is synchronous (it has no id to key an async error to),
  // so the failure must reach the caller - Client::cancel() reporting success
  // while the peer is unreachable is the bug this locks down.
  Client notifier("probe", "1.0");
  notifier.connect(transport);
  auto cancelled = notifier.cancel(RequestId(int64_t(1)), "stop");
  EXPECT_FALSE(cancelled.has_value());

  // A request POST is asynchronous: the failure is queued as a synthetic error
  // response and surfaces through read().
  Client requester("probe", "1.0");
  requester.connect(transport);
  auto listed = requester.listTools();
  EXPECT_FALSE(listed.has_value());
}

TEST(HttpClientTransport, Reports401AuthChallenge) {
  // A 401 carrying WWW-Authenticate must reach the registered handler - it is
  // how a client learns where the protected-resource metadata lives.
  httplib::Server svr;
  const char* kChallenge =
      "Bearer error=\"invalid_token\", resource_metadata=\"http://example.test/"
      ".well-known/oauth-protected-resource\"";
  svr.Post("/mcp", [&](const httplib::Request&, httplib::Response& res) {
    res.status = 401;
    res.set_header("WWW-Authenticate", kChallenge);
    res.set_content(
        R"({"jsonrpc":"2.0","error":{"code":-32000,"message":"unauthorized"},"id":null})",
        "application/json");
  });
  const int port = svr.bind_to_any_port("127.0.0.1");
  std::thread runner([&] { (void)svr.listen_after_bind(); });
  for (int i = 0; i < 10000 && !svr.is_running(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));  // deadline: never wait forever

  HttpClientTransport transport("127.0.0.1", port, "/mcp");
  int status = 0;
  std::string challenge;
  transport.setAuthChallengeHandler([&](int code, const std::string& value) {
    status = code;
    challenge = value;
  });
  Client client("probe", "1.0");
  client.connect(transport);
  const auto result = client.listTools();
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(status, 401);
  EXPECT_NE(challenge.find("resource_metadata="), std::string::npos);
  EXPECT_NE(challenge.find("invalid_token"), std::string::npos);

  svr.stop();
  if (runner.joinable()) runner.join();
}

TEST(HttpClientTransport, Reports403ScopeChallenge) {
  // A 403 insufficient_scope challenge carries the scope the client must ask
  // for on re-authorization (SEP-2350).
  httplib::Server svr;
  svr.Post("/mcp", [](const httplib::Request&, httplib::Response& res) {
    res.status = 403;
    res.set_header("WWW-Authenticate",
                   "Bearer error=\"insufficient_scope\", scope=\"mcp:write\"");
    res.set_content(
        R"({"jsonrpc":"2.0","error":{"code":-32000,"message":"insufficient"},"id":null})",
        "application/json");
  });
  const int port = svr.bind_to_any_port("127.0.0.1");
  std::thread runner([&] { (void)svr.listen_after_bind(); });
  for (int i = 0; i < 10000 && !svr.is_running(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));  // deadline: never wait forever

  HttpClientTransport transport("127.0.0.1", port, "/mcp");
  int status = 0;
  std::string challenge;
  transport.setAuthChallengeHandler([&](int code, const std::string& value) {
    status = code;
    challenge = value;
  });
  Client client("probe", "1.0");
  client.connect(transport);
  EXPECT_FALSE(client.listTools().has_value());
  EXPECT_EQ(status, 403);
  EXPECT_NE(challenge.find("scope=\"mcp:write\""), std::string::npos);

  svr.stop();
  if (runner.joinable()) runner.join();
}

#endif  // MCP_HAS_HTTP
