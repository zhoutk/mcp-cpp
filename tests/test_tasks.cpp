// Tasks extension (SEP-2663): store lifecycle, TTL policy, wire shape, and the
// tasks/get / tasks/update / tasks/cancel routes registered by Builder::tasks.

#include "MemTransport.h"

#include <mcp/mcp.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <variant>

using namespace mcp;
using namespace mcp::server;

namespace {

Json taskCaps() {
  return Json(R"({"extensions":{"io.modelcontextprotocol/tasks":{}}})");
}

std::string makeRequestWithCaps(const std::string& id, const std::string& method,
                                const Json& extraParams) {
  Json params = encodeRequestMeta("2026-07-28", taskCaps(), std::nullopt);
  if (extraParams.isObject()) {
    for (const auto& item : extraParams.getAllKeys().toVector()) {
      params.add(item.toString(), extraParams[item.toString()]);
    }
  }
  return encodeRequest(RequestId(id), method, params);
}

struct Out {
  ParsedMessage parsed;
  const JsonRpcResult* result = nullptr;
  const JsonRpcErrorResponse* error = nullptr;
};

Out serveOnce(const std::shared_ptr<Server>& server, const std::string& line) {
  MemTransport transport({line});
  server->processLine(line, transport);
  EXPECT_EQ(transport.output_.size(), 1u);
  Out out;
  if (transport.output_.empty()) return out;
  out.parsed = parseMessage(transport.output_[0]);
  out.result = std::get_if<JsonRpcResult>(&out.parsed);
  out.error = std::get_if<JsonRpcErrorResponse>(&out.parsed);
  return out;
}

Out tasksCall(const std::shared_ptr<Server>& server, const std::string& id,
              const std::string& method, const std::string& taskId) {
  Json params;
  params.add("taskId", Json::str(taskId));
  return serveOnce(server, makeRequestWithCaps(id, method, params));
}

}  // namespace

// ---- store ------------------------------------------------------------------------

TEST(TasksStore, CreateReadAndTimestamps) {
  tasks::InMemoryTaskStore store;
  const tasks::TaskRecord record = tasks::makeTask();
  EXPECT_EQ(record.status, tasks::TaskStatus::Working);
  EXPECT_FALSE(record.taskId.empty());
  ASSERT_TRUE(record.ttlMs.has_value());
  EXPECT_EQ(*record.ttlMs, tasks::kDefaultTtlMs);
  ASSERT_TRUE(record.pollIntervalMs.has_value());
  EXPECT_EQ(*record.pollIntervalMs, tasks::kDefaultPollIntervalMs);
  ASSERT_FALSE(record.createdAt.empty());
  EXPECT_EQ(record.createdAt.back(), 'Z');

  ASSERT_TRUE(store.put(record));
  auto stored = store.get(record.taskId);
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->taskId, record.taskId);
  EXPECT_EQ(stored->createdAt, record.createdAt);
  EXPECT_FALSE(store.get("missing").has_value());
  EXPECT_TRUE(store.erase(record.taskId));
  EXPECT_FALSE(store.get(record.taskId).has_value());
}

TEST(TasksStore, InputRoundTripAndTerminalTransitions) {
  tasks::InMemoryTaskStore store;
  const tasks::TaskRecord record = tasks::makeTask();
  ASSERT_TRUE(store.put(record));

  ASSERT_TRUE(tasks::requestTaskInput(
      store, record.taskId, Json(R"({"q":{"method":"elicitation/create","params":{}}})")));
  auto pending = store.get(record.taskId);
  ASSERT_TRUE(pending.has_value());
  EXPECT_EQ(pending->status, tasks::TaskStatus::InputRequired);
  EXPECT_TRUE(member(pending->inputRequests, "q").isObject());

  // An unknown key is ignored; the outstanding one is consumed.
  ASSERT_TRUE(store.updateInput(record.taskId,
                                Json(R"({"q":{"action":"accept"},"stale":{"x":1}})")));
  auto answered = store.get(record.taskId);
  ASSERT_TRUE(answered.has_value());
  EXPECT_EQ(answered->status, tasks::TaskStatus::Working);
  EXPECT_TRUE(answered->inputRequests.isNull());
  EXPECT_TRUE(member(answered->inputResponses, "q").isObject());
  EXPECT_TRUE(member(answered->inputResponses, "stale").isNull());

  ASSERT_TRUE(tasks::finishTask(store, record.taskId, Json(R"({"content":[]})")));
  auto completed = store.get(record.taskId);
  ASSERT_TRUE(completed.has_value());
  EXPECT_EQ(completed->status, tasks::TaskStatus::Completed);
  EXPECT_TRUE(member(completed->result, "content").isArray());
  // A settled task does not transition again.
  EXPECT_FALSE(tasks::finishTask(store, record.taskId, Json(R"({})")));
  EXPECT_FALSE(tasks::failTask(store, record.taskId, Json(R"({})")));
}

TEST(TasksStore, CancelIsCooperativeThenTerminal) {
  tasks::InMemoryTaskStore store;
  const tasks::TaskRecord record = tasks::makeTask();
  ASSERT_TRUE(store.put(record));

  ASSERT_TRUE(store.requestCancel(record.taskId));
  auto requested = store.get(record.taskId);
  ASSERT_TRUE(requested.has_value());
  EXPECT_TRUE(requested->cancelRequested);
  EXPECT_EQ(requested->status, tasks::TaskStatus::Working);  // worker decides

  ASSERT_TRUE(tasks::cancelTask(store, record.taskId));
  auto cancelled = store.get(record.taskId);
  ASSERT_TRUE(cancelled.has_value());
  EXPECT_EQ(cancelled->status, tasks::TaskStatus::Cancelled);
  EXPECT_FALSE(store.requestCancel("missing"));
}

TEST(TasksStore, CapacityEvictsOnlyTerminalRecords) {
  tasks::InMemoryTaskStore store(2);
  const auto first = tasks::makeTask();
  const auto second = tasks::makeTask();
  const auto third = tasks::makeTask();
  ASSERT_TRUE(store.put(first));
  ASSERT_TRUE(store.put(second));
  EXPECT_FALSE(store.put(third));  // capacity full of live tasks

  ASSERT_TRUE(tasks::finishTask(store, first.taskId, Json(R"({})")));
  EXPECT_TRUE(store.put(third));
  EXPECT_FALSE(store.get(first.taskId).has_value());
  EXPECT_TRUE(store.get(second.taskId).has_value());
  EXPECT_TRUE(store.get(third.taskId).has_value());
}

TEST(TasksStore, TtlFailsThenEvictsOverdueTasks) {
  tasks::InMemoryTaskStore store;
  tasks::TaskOptions options;
  options.ttlMs = 20;
  const auto record = tasks::makeTask(options);
  ASSERT_TRUE(store.put(record));

  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  tasks::sweepExpired(store);
  auto expired = store.get(record.taskId);
  ASSERT_TRUE(expired.has_value());
  EXPECT_EQ(expired->status, tasks::TaskStatus::Failed);
  EXPECT_EQ(std::string(member(expired->error, "message").toString()),
            "task expired: TTL elapsed before completion");

  // One further TTL window past the terminal transition evicts it.
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  tasks::sweepExpired(store);
  EXPECT_FALSE(store.get(record.taskId).has_value());
}

TEST(TasksStore, NullTtlNeverExpires) {
  tasks::InMemoryTaskStore store;
  tasks::TaskOptions options;
  options.ttlMs = std::nullopt;  // unlimited
  const auto record = tasks::makeTask(options);
  ASSERT_TRUE(store.put(record));
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  tasks::sweepExpired(store);
  auto alive = store.get(record.taskId);
  ASSERT_TRUE(alive.has_value());
  EXPECT_EQ(alive->status, tasks::TaskStatus::Working);
}

// ---- wire shape -------------------------------------------------------------------

TEST(TasksWire, BaseAndDetailedTaskJson) {
  tasks::TaskOptions options;
  options.pollIntervalMs = 500;
  tasks::TaskRecord record = tasks::makeTask("t1", options);

  const Json base = tasks::taskToJson(record);
  EXPECT_EQ(std::string(member(base, "taskId").toString()), "t1");
  EXPECT_EQ(std::string(member(base, "status").toString()), "working");
  EXPECT_EQ(member(base, "ttlMs").toInt64(), tasks::kDefaultTtlMs);
  EXPECT_EQ(member(base, "pollIntervalMs").toInt64(), 500);
  EXPECT_TRUE(member(tasks::detailedTaskToJson(record), "result").isNull());

  record.status = tasks::TaskStatus::InputRequired;
  record.inputRequests = Json(R"({"q":{}})");
  EXPECT_TRUE(member(tasks::detailedTaskToJson(record), "inputRequests").isObject());

  record.status = tasks::TaskStatus::Completed;
  record.result = Json(R"({"content":[]})");
  EXPECT_TRUE(member(tasks::detailedTaskToJson(record), "result").isObject());

  record.status = tasks::TaskStatus::Failed;
  record.error = Json(R"({"code":-32603,"message":"boom"})");
  EXPECT_EQ(std::string(member(member(tasks::detailedTaskToJson(record), "error"), "message")
                            .toString()),
            "boom");

  tasks::TaskOptions unlimited;
  unlimited.ttlMs = std::nullopt;
  EXPECT_TRUE(member(tasks::taskToJson(tasks::makeTask("u", unlimited)), "ttlMs").isNull());

  // CreateTaskResult keeps the seed state (base fields) and its discriminator.
  const Json created = tasks::createTaskResult(tasks::makeTask("seed"));
  EXPECT_EQ(std::string(member(created, "resultType").toString()), "task");
  EXPECT_EQ(std::string(member(created, "taskId").toString()), "seed");
  EXPECT_TRUE(member(created, "inputRequests").isNull());
}

// ---- server integration ------------------------------------------------------------

TEST(TasksServer, MethodsServeFromTheStore) {
  auto store = std::make_shared<tasks::InMemoryTaskStore>();
  const auto record = tasks::makeTask();
  ASSERT_TRUE(store->put(record));

  Builder b;
  b.name("tasks-server").version("1.0");
  b.tasks(store);
  auto server = b.build();

  // Advertised under capabilities.extensions.
  Out discover = serveOnce(server, makeRequestWithCaps("d1", "server/discover", Json(nullptr)));
  ASSERT_NE(discover.result, nullptr);
  EXPECT_TRUE(capabilitiesIncludeExtension(member(discover.result->result, "capabilities"),
                                           tasks::kExtensionId));

  Out get = tasksCall(server, "g1", "tasks/get", record.taskId);
  ASSERT_NE(get.result, nullptr);
  EXPECT_EQ(std::string(member(get.result->result, "resultType").toString()), "complete");
  EXPECT_EQ(std::string(member(get.result->result, "taskId").toString()), record.taskId);
  EXPECT_EQ(std::string(member(get.result->result, "status").toString()), "working");

  Out unknown = tasksCall(server, "g2", "tasks/get", "nope");
  ASSERT_NE(unknown.error, nullptr);
  EXPECT_EQ(unknown.error->error.code, -32602);

  // tasks/update delivers outstanding input responses.
  ASSERT_TRUE(tasks::requestTaskInput(*store, record.taskId, Json(R"({"q":{}})")));
  Json updateParams;
  updateParams.add("taskId", Json::str(record.taskId));
  updateParams.add("inputResponses", Json(R"({"q":{"action":"accept"}})"));
  Out update = serveOnce(server, makeRequestWithCaps("u1", "tasks/update", updateParams));
  ASSERT_NE(update.result, nullptr);
  auto answered = store->get(record.taskId);
  ASSERT_TRUE(answered.has_value());
  EXPECT_TRUE(member(answered->inputResponses, "q").isObject());

  // tasks/cancel records the cooperative cancellation intent.
  Out cancel = tasksCall(server, "c1", "tasks/cancel", record.taskId);
  ASSERT_NE(cancel.result, nullptr);
  auto cancelled = store->get(record.taskId);
  ASSERT_TRUE(cancelled.has_value());
  EXPECT_TRUE(cancelled->cancelRequested);

  Out unknownUpdate = serveOnce(server, makeRequestWithCaps(
                                  "u2", "tasks/update",
                                  Json(R"({"taskId":"nope","inputResponses":{}})")));
  ASSERT_NE(unknownUpdate.error, nullptr);
  EXPECT_EQ(unknownUpdate.error->error.code, -32602);
  Out unknownCancel = tasksCall(server, "c2", "tasks/cancel", "nope");
  ASSERT_NE(unknownCancel.error, nullptr);
  EXPECT_EQ(unknownCancel.error->error.code, -32602);
}

TEST(TasksServer, ClientWithoutExtensionGetsMissingRequiredCapability) {
  auto store = std::make_shared<tasks::InMemoryTaskStore>();
  const auto record = tasks::makeTask();
  ASSERT_TRUE(store->put(record));
  Builder b;
  b.name("tasks-server").version("1.0");
  b.tasks(store);
  auto server = b.build();

  Json params = encodeRequestMeta("2026-07-28", Json(R"({})"), std::nullopt);
  params.add("taskId", Json::str(record.taskId));
  Out denied = serveOnce(
      server, encodeRequest(RequestId("x1"), "tasks/get", params));
  ASSERT_NE(denied.error, nullptr);
  // SEP-2575 Missing Required Capabilities: the official suite requires -32021
  // (not -32601) for an extension-gated method whose extension the client did
  // not declare, with the missing capability under data.requiredCapabilities.
  EXPECT_EQ(denied.error->error.code, -32021);
  const Json required = member(denied.error->error.data, "requiredCapabilities");
  EXPECT_FALSE(member(required, "extensions").isNull());
}

// G5: convenience helpers on the handler surface.
TEST(TasksServer, ClientSupportsReadsPerRequestCapabilities) {
  Builder b;
  b.name("tasks-server").version("1.0");
  b.tasks(std::make_shared<tasks::InMemoryTaskStore>());
  bool withExtension = false;
  b.simpleTool("probe", "reports the client's task support",
               [&](Ctx& c) -> McpResult<Json> {
                 if (tasks::clientSupports(c)) withExtension = true;
                 return Ok(Json(R"({"ok":true})"));
               });
  auto server = b.build();

  Out declared =
      serveOnce(server, makeRequestWithCaps("p1", "tools/call", Json(R"({"name":"probe"})")));
  ASSERT_NE(declared.result, nullptr);
  EXPECT_TRUE(withExtension);

  bool denied = false;
  Builder plain;
  plain.name("tasks-server").version("1.0");
  plain.tasks(std::make_shared<tasks::InMemoryTaskStore>());
  plain.simpleTool("probe", "x", [&](Ctx& c) -> McpResult<Json> {
    denied = !tasks::clientSupports(c);
    return Ok(Json(R"({"ok":true})"));
  });
  auto plainServer = plain.build();
  Out undeclared = serveOnce(
      plainServer,
      encodeRequest(RequestId("p2"), "tools/call",
                    [] {
                      Json params = encodeRequestMeta("2026-07-28", Json(R"({})"),
                                                      std::nullopt);
                      params.add("name", Json::str("probe"));
                      return params;
                    }()));
  ASSERT_NE(undeclared.result, nullptr);
  EXPECT_TRUE(denied);
}

TEST(TasksServer, NotifyStatusEmitsDetailedTask) {
  auto store = std::make_shared<tasks::InMemoryTaskStore>();
  const auto record = tasks::makeTask();
  ASSERT_TRUE(store->put(record));

  bool emitted = false;
  Builder b;
  b.name("tasks-server").version("1.0");
  b.tasks(store);
  b.simpleTool("notify", "emits a task status notification",
               [&](Ctx& c) -> McpResult<Json> {
                 emitted = tasks::notifyStatus(c, record);
                 return Ok(Json(R"({"ok":true})"));
               });
  auto server = b.build();

  Json params = encodeRequestMeta("2026-07-28", taskCaps(), std::nullopt);
  params.add("name", Json::str("notify"));
  params.add("arguments", Json(R"({})"));
  MemTransport t({});
  server->processLine(encodeRequest(RequestId("n1"), "tools/call", params), t);

  ASSERT_EQ(t.output_.size(), 2u);
  EXPECT_TRUE(emitted);
  const ParsedMessage parsed = parseMessage(t.output_[0]);
  const auto* notif = std::get_if<JsonRpcNotification>(&parsed);
  ASSERT_NE(notif, nullptr);
  EXPECT_EQ(notif->method, "notifications/tasks/status");
  EXPECT_EQ(std::string(member(notif->params, "taskId").toString()), record.taskId);
  EXPECT_EQ(std::string(member(notif->params, "status").toString()), "working");
}
