#include <mcp/tasks.hpp>

#include <mcp/server.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <functional>
#include <iostream>
#include <random>
#include <utility>

namespace mcp {
namespace tasks {
namespace {

using Clock = std::chrono::steady_clock;

std::string hex64(uint64_t value) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out(16, '0');
  for (int i = 15; i >= 0; --i) {
    out[static_cast<size_t>(i)] = kDigits[value & 0xf];
    value >>= 4;
  }
  return out;
}

// 48 hex chars: 64 random bits, 64 clock bits, 64 counter bits. The counter
// keeps ids unique even when random_device is deterministic (some MinGW
// implementations) and two tasks are created in the same clock tick.
std::string generateTaskId() {
  static std::atomic<uint64_t> counter{0};
  std::random_device device;
  const uint64_t random = (static_cast<uint64_t>(device()) << 32) ^ device();
  const uint64_t tick = static_cast<uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  return hex64(random) + hex64(tick) + hex64(counter.fetch_add(1) + 1);
}

void settle(TaskRecord& record, TaskStatus status) {
  record.status = status;
  record.lastUpdatedAt = nowIso8601();
  record.terminalSteady = Clock::now();
}

Json errorObject(McpErrorCode code, const std::string& message) {
  Json error;
  error.add("code", Json(static_cast<int64_t>(code)));
  error.add("message", Json::str(message));
  return error;
}

}  // namespace

// ---- status / clocks -------------------------------------------------------------

std::string_view toString(TaskStatus status) {
  switch (status) {
    case TaskStatus::Working: return "working";
    case TaskStatus::InputRequired: return "input_required";
    case TaskStatus::Completed: return "completed";
    case TaskStatus::Failed: return "failed";
    case TaskStatus::Cancelled: return "cancelled";
  }
  return "working";
}

std::optional<TaskStatus> taskStatusFromString(std::string_view text) {
  if (text == "working") return TaskStatus::Working;
  if (text == "input_required") return TaskStatus::InputRequired;
  if (text == "completed") return TaskStatus::Completed;
  if (text == "failed") return TaskStatus::Failed;
  if (text == "cancelled") return TaskStatus::Cancelled;
  return std::nullopt;
}

bool isTerminal(TaskStatus status) {
  return status == TaskStatus::Completed || status == TaskStatus::Failed ||
         status == TaskStatus::Cancelled;
}

std::string nowIso8601() {
  const std::time_t seconds = std::chrono::system_clock::to_time_t(
      std::chrono::system_clock::now());
  std::tm utc{};
#if defined(_WIN32)
  gmtime_s(&utc, &seconds);
#else
  gmtime_r(&seconds, &utc);
#endif
  char buffer[32];
  const size_t written = std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return std::string(buffer, written);
}

// ---- records -------------------------------------------------------------------

TaskRecord makeTask(TaskOptions options) { return makeTask(generateTaskId(), options); }

TaskRecord makeTask(std::string taskId, TaskOptions options) {
  TaskRecord record;
  record.taskId = std::move(taskId);
  record.status = TaskStatus::Working;
  record.statusMessage = std::move(options.statusMessage);
  record.createdAt = nowIso8601();
  record.lastUpdatedAt = record.createdAt;
  record.ttlMs = options.ttlMs;
  record.pollIntervalMs = options.pollIntervalMs;
  record.createdSteady = Clock::now();
  return record;
}

Json taskToJson(const TaskRecord& record) {
  Json out;
  out.add("taskId", Json::str(record.taskId));
  out.add("status", Json::str(std::string(toString(record.status))));
  if (!record.statusMessage.empty()) {
    out.add("statusMessage", Json::str(record.statusMessage));
  }
  out.add("createdAt", Json::str(record.createdAt));
  out.add("lastUpdatedAt", Json::str(record.lastUpdatedAt));
  // null means unlimited retention; the field is always present.
  out.add("ttlMs", record.ttlMs ? Json(*record.ttlMs) : Json(nullptr));
  if (record.pollIntervalMs) out.add("pollIntervalMs", Json(*record.pollIntervalMs));
  return out;
}

Json detailedTaskToJson(const TaskRecord& record) {
  Json out = taskToJson(record);
  switch (record.status) {
    case TaskStatus::InputRequired:
      out.add("inputRequests",
              record.inputRequests.isObject() ? record.inputRequests : Json(R"({})"));
      break;
    case TaskStatus::Completed:
      out.add("result", record.result.isObject() ? record.result : Json(R"({})"));
      break;
    case TaskStatus::Failed:
      out.add("error", record.error.isObject() ? record.error : Json(R"({})"));
      break;
    case TaskStatus::Working:
    case TaskStatus::Cancelled:
      break;
  }
  return out;
}

Json createTaskResult(const TaskRecord& record) {
  Json out = taskToJson(record);
  out.add("resultType", Json::str(std::string(kResultTypeTask)));
  return out;
}

// ---- InMemoryTaskStore ---------------------------------------------------------

InMemoryTaskStore::InMemoryTaskStore(size_t capacity)
    : capacity_(capacity == 0 ? 1 : capacity) {}

bool InMemoryTaskStore::put(const TaskRecord& record) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = entries_.find(record.taskId);
  if (it != entries_.end()) {
    it->second.record = record;
    return true;
  }
  if (entries_.size() >= capacity_) {
    // Evict the oldest TERMINAL record (a running task must stay observable).
    auto victim = entries_.end();
    for (auto candidate = entries_.begin(); candidate != entries_.end(); ++candidate) {
      if (!isTerminal(candidate->second.record.status)) continue;
      if (victim == entries_.end() || candidate->second.sequence < victim->second.sequence) {
        victim = candidate;
      }
    }
    if (victim == entries_.end()) return false;  // capacity full of live tasks
    entries_.erase(victim);
  }
  entries_.emplace(record.taskId, Entry{record, nextSequence_++});
  return true;
}

std::optional<TaskRecord> InMemoryTaskStore::get(const std::string& taskId) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = entries_.find(taskId);
  if (it == entries_.end()) return std::nullopt;
  return it->second.record;
}

bool InMemoryTaskStore::erase(const std::string& taskId) {
  std::lock_guard<std::mutex> lock(mutex_);
  return entries_.erase(taskId) > 0;
}

std::vector<TaskRecord> InMemoryTaskStore::all() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<TaskRecord> out;
  out.reserve(entries_.size());
  for (const auto& item : entries_) out.push_back(item.second.record);
  return out;
}

bool InMemoryTaskStore::updateInput(const std::string& taskId, const Json& inputResponses) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = entries_.find(taskId);
  if (it == entries_.end()) return false;
  TaskRecord& record = it->second.record;
  if (inputResponses.isObject()) {
    for (const auto& key : inputResponses.getAllKeys().toVector()) {
      const std::string name(key.toString());
      // Keys that are not outstanding are ignored (spec: a partial or stale
      // response set is not an error).
      if (member(record.inputRequests, name).isNull()) continue;
      Json merged = record.inputResponses.isObject() ? record.inputResponses : Json(R"({})");
      merged.add(name, inputResponses[name]);
      record.inputResponses = std::move(merged);
      record.inputRequests.remove(name);
    }
  }
  if (record.status == TaskStatus::InputRequired &&
      (!record.inputRequests.isObject() ||
       record.inputRequests.getAllKeys().toVector().empty())) {
    record.status = TaskStatus::Working;
    record.inputRequests = Json(nullptr);
  }
  record.lastUpdatedAt = nowIso8601();
  return true;
}

bool InMemoryTaskStore::requestCancel(const std::string& taskId) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = entries_.find(taskId);
  if (it == entries_.end()) return false;
  it->second.record.cancelRequested = true;
  it->second.record.lastUpdatedAt = nowIso8601();
  return true;
}

// ---- lifecycle helpers ---------------------------------------------------------

namespace {

// Reads, applies `mutate`, writes back. False when unknown or terminal.
bool mutateLive(TaskStore& store, const std::string& taskId,
                const std::function<void(TaskRecord&)>& mutate) {
  auto record = store.get(taskId);
  if (!record.has_value() || isTerminal(record->status)) return false;
  mutate(*record);
  return store.put(*record);
}

}  // namespace

bool finishTask(TaskStore& store, const std::string& taskId, Json result) {
  return mutateLive(store, taskId, [&](TaskRecord& record) {
    record.result = std::move(result);
    settle(record, TaskStatus::Completed);
  });
}

bool failTask(TaskStore& store, const std::string& taskId, Json error) {
  return mutateLive(store, taskId, [&](TaskRecord& record) {
    record.error = std::move(error);
    settle(record, TaskStatus::Failed);
  });
}

bool cancelTask(TaskStore& store, const std::string& taskId) {
  return mutateLive(store, taskId, [](TaskRecord& record) {
    record.cancelRequested = true;
    settle(record, TaskStatus::Cancelled);
  });
}

bool requestTaskInput(TaskStore& store, const std::string& taskId, Json inputRequests) {
  if (!inputRequests.isObject() ||
      inputRequests.getAllKeys().toVector().empty()) {
    return false;  // input_required without outstanding requests is invalid
  }
  return mutateLive(store, taskId, [&](TaskRecord& record) {
    record.inputRequests = std::move(inputRequests);
    record.status = TaskStatus::InputRequired;
    record.lastUpdatedAt = nowIso8601();
  });
}

bool touchTask(TaskStore& store, const std::string& taskId,
               const std::string& statusMessage) {
  auto record = store.get(taskId);
  if (!record.has_value()) return false;
  if (!statusMessage.empty()) record->statusMessage = statusMessage;
  record->lastUpdatedAt = nowIso8601();
  return store.put(*record);
}

void sweepExpired(TaskStore& store) {
  const auto now = Clock::now();
  for (const auto& record : store.all()) {
    if (!record.ttlMs.has_value() || *record.ttlMs < 0) continue;  // unlimited
    const auto ttl = std::chrono::milliseconds(*record.ttlMs);
    if (!isTerminal(record.status)) {
      if (record.createdSteady != Clock::time_point{} && now - record.createdSteady >= ttl) {
        TaskRecord expired = record;
        expired.error = errorObject(McpErrorCode::InternalError,
                                    "task expired: TTL elapsed before completion");
        settle(expired, TaskStatus::Failed);
        store.put(expired);
      }
    } else if (record.terminalSteady != Clock::time_point{} &&
               now - record.terminalSteady >= ttl) {
      store.erase(record.taskId);
    }
  }
}

}  // namespace tasks

// ---- Builder integration --------------------------------------------------------

namespace server {
namespace {

std::optional<std::string> requiredTaskId(const Json& params) {
  const Json taskId = member(params, "taskId");
  if (!taskId.isString()) return std::nullopt;
  return std::string(taskId.toString());
}

}  // namespace

// E1 (SEP-2663): advertise the extension and serve tasks/get, tasks/update and
// tasks/cancel from `store`. The handlers sweep the TTL policy first, so an
// expired task is reported failed (and eventually evicted) even when the
// embedder's worker never polls again.
Builder& Builder::tasks(std::shared_ptr<tasks::TaskStore> store) {
  if (!store) {
    std::cerr << "[mcp-cpp] builder: tasks() requires a TaskStore\n";
    return *this;
  }
  auto getTask = [store](Ctx& c) -> McpResult<Json> {
    const auto taskId = requiredTaskId(c.params);
    if (!taskId) return Err(McpErrorCode::InvalidParams, "tasks/get requires a string taskId");
    tasks::sweepExpired(*store);
    auto record = store->get(*taskId);
    if (!record.has_value()) {
      return Err(McpErrorCode::InvalidParams, "unknown task: " + *taskId);
    }
    Json result = tasks::detailedTaskToJson(*record);
    result.add(std::string(kResultTypeKey), Json::str(std::string(kResultTypeComplete)));
    return Ok(std::move(result));
  };
  auto updateTask = [store](Ctx& c) -> McpResult<Json> {
    const auto taskId = requiredTaskId(c.params);
    if (!taskId) return Err(McpErrorCode::InvalidParams, "tasks/update requires a string taskId");
    const Json responses = member(c.params, kInputResponsesKey);
    if (!responses.isObject()) {
      return Err(McpErrorCode::InvalidParams,
                 "tasks/update requires an inputResponses object");
    }
    tasks::sweepExpired(*store);
    if (!store->updateInput(*taskId, responses)) {
      return Err(McpErrorCode::InvalidParams, "unknown task: " + *taskId);
    }
    return Ok(Json(R"({})"));
  };
  auto cancelTask = [store](Ctx& c) -> McpResult<Json> {
    const auto taskId = requiredTaskId(c.params);
    if (!taskId) return Err(McpErrorCode::InvalidParams, "tasks/cancel requires a string taskId");
    tasks::sweepExpired(*store);
    if (!store->requestCancel(*taskId)) {
      return Err(McpErrorCode::InvalidParams, "unknown task: " + *taskId);
    }
    return Ok(Json(R"({})"));
  };

  extension(std::string(tasks::kExtensionId), "tasks/get", "", getTask);
  extension(std::string(tasks::kExtensionId), "tasks/update", "", updateTask);
  extension(std::string(tasks::kExtensionId), "tasks/cancel", "", cancelTask);
  return *this;
}

}  // namespace server
}  // namespace mcp
