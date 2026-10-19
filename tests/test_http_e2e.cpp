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
    while (!http->running()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
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
  while (!http.running()) std::this_thread::sleep_for(std::chrono::milliseconds(1));

  httplib::Client c("127.0.0.1", *bound);
  httplib::Headers evil{{"Origin", "http://evil.example.com"}};
  auto r = c.Post("/mcp", evil, "{}", "application/json");
  ASSERT_TRUE(r);
  EXPECT_EQ(r->status, 403);

  http.stop();
  runner.join();
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
TEST(HttpE2E, ConcurrentPostsAreSerializedSafely) {
  Fixture f;
  constexpr int kThreads = 64;
  constexpr int kIterations = 4;
  std::atomic<int> failures{0};

  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([&f, &failures, t] {
      HttpClientTransport transport("127.0.0.1", f.port);
      Client client("worker-" + std::to_string(t), "1.0");
      client.connect(transport);
      for (int i = 0; i < kIterations; ++i) {
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
  while (!svr.is_running()) std::this_thread::sleep_for(std::chrono::milliseconds(1));

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
  while (!svr.is_running()) std::this_thread::sleep_for(std::chrono::milliseconds(1));

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
