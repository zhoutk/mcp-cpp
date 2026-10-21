#pragma once
// ---------------------------------------------------------------------------
// mcp/tasks.hpp - Tasks extension (SEP-2663, io.modelcontextprotocol/tasks).
//
// A task-eligible request (currently tools/call) may answer with a
// CreateTaskResult (`resultType: "task"`) instead of the normal result: the
// server has materialized the work as a task and returns its seed state. The
// client then polls `tasks/get`, delivers answers to in-task input requests
// through `tasks/update`, and may ask for cooperative cancellation with
// `tasks/cancel`.
//
// This header provides the durable state half of that contract:
//  - TaskRecord / TaskStatus with the exact wire shape (SEP-2663 `Task` and
//    `DetailedTask`: status-specific payload flattened next to the base
//    fields),
//  - a pluggable TaskStore (InMemoryTaskStore by default),
//  - TTL semantics (null = unlimited; an expired non-terminal task is marked
//    failed, then evicted one TTL window after it reached a terminal state),
//  - Builder::tasks(store), which advertises the extension and registers the
//    three request methods.
//
// The library has no async runtime: an embedder that wants background work
// starts it however it likes (a std::thread, a thread pool, a queue) and
// updates the store from that worker:
//
//   auto record = tasks::makeTask();               // working, TTL 5 min
//   store->put(record);
//   return Ok(tasks::createTaskResult(record));    // from the tool handler
//   ...
//   tasks::finishTask(*store, record.taskId, result);   // worker thread
//
// A handler that returns a CreateTaskResult must set `resultType` itself (the
// server only injects "complete" when the field is absent).
// ---------------------------------------------------------------------------

#include <mcp/error.hpp>
#include <mcp/json.hpp>

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcp {
namespace tasks {

// Extension identifier advertised under capabilities.extensions.
inline constexpr std::string_view kExtensionId = "io.modelcontextprotocol/tasks";

// Result discriminator of a CreateTaskResult.
inline constexpr std::string_view kResultTypeTask = "task";

// Server-to-client status notification (params = a DetailedTask).
inline constexpr std::string_view kNotificationTaskStatus = "notifications/tasks";

// Defaults (SEP-2663 example values; both are server policy).
inline constexpr int64_t kDefaultTtlMs = 300'000;   // 5 minutes
inline constexpr int64_t kDefaultPollIntervalMs = 1'000;

enum class TaskStatus {
  Working = 0,
  InputRequired,
  Completed,
  Failed,
  Cancelled,
};

// "working" / "input_required" / "completed" / "failed" / "cancelled".
std::string_view toString(TaskStatus status);
std::optional<TaskStatus> taskStatusFromString(std::string_view text);

// True for completed / failed / cancelled.
bool isTerminal(TaskStatus status);

// ISO 8601 UTC timestamp ("2026-10-21T12:34:56Z").
std::string nowIso8601();

// ---- records ------------------------------------------------------------------

// One task. Public fields map to the spec's `Task` plus its status payload;
// the trailing steady-clock fields are server-internal (TTL bookkeeping) and
// never serialized.
struct TaskRecord {
  std::string taskId;
  TaskStatus status = TaskStatus::Working;
  std::string statusMessage;      // empty = omitted
  std::string createdAt;          // ISO 8601
  std::string lastUpdatedAt;      // ISO 8601
  std::optional<int64_t> ttlMs;   // nullopt = unlimited (wire `null`)
  std::optional<int64_t> pollIntervalMs;

  // Status-specific payload; only the one matching `status` is serialized.
  Json inputRequests = Json(nullptr);  // status == InputRequired
  Json result = Json(nullptr);         // status == Completed
  Json error = Json(nullptr);          // status == Failed

  // Server-internal (never serialized):
  bool cancelRequested = false;        // tasks/cancel intent observed by the worker
  Json inputResponses = Json(nullptr); // responses delivered by tasks/update
  std::chrono::steady_clock::time_point createdSteady{};
  std::chrono::steady_clock::time_point terminalSteady{};
};

struct TaskOptions {
  std::optional<int64_t> ttlMs = kDefaultTtlMs;            // nullopt = unlimited
  std::optional<int64_t> pollIntervalMs = kDefaultPollIntervalMs;
  std::string statusMessage;
};

// A new `working` record with a generated id and timestamps.
TaskRecord makeTask(TaskOptions options = {});
// As above with a caller-chosen id (must be unique in the store).
TaskRecord makeTask(std::string taskId, TaskOptions options = {});

// The spec's `Task` (base fields only), used inside a CreateTaskResult.
Json taskToJson(const TaskRecord& record);
// The spec's `DetailedTask`: base fields + the status-specific payload.
Json detailedTaskToJson(const TaskRecord& record);
// CreateTaskResult: `resultType: "task"` + base fields. The server adds
// `_meta.serverInfo` and leaves the discriminator alone.
Json createTaskResult(const TaskRecord& record);

// ---- store --------------------------------------------------------------------

// Durable task state. Implement this to back tasks with a database or a shared
// cache; InMemoryTaskStore is the default for single-process servers.
// Implementations MUST be safe to call from multiple threads: a worker thread
// completes tasks while request threads poll them.
class TaskStore {
 public:
  virtual ~TaskStore() = default;

  // Creates or replaces a record. Returns false when the store refuses
  // (e.g. capacity exhausted).
  virtual bool put(const TaskRecord& record) = 0;
  virtual std::optional<TaskRecord> get(const std::string& taskId) = 0;
  virtual bool erase(const std::string& taskId) = 0;
  virtual std::vector<TaskRecord> all() = 0;

  // tasks/update: merges `inputResponses` into the record (keys not currently
  // outstanding are ignored, per spec) and clears the answered keys from
  // `inputRequests`; an empty set returns the status to `working`.
  // False when the task is unknown.
  virtual bool updateInput(const std::string& taskId, const Json& inputResponses) = 0;

  // tasks/cancel: records the cooperative cancellation intent. The running
  // worker observes TaskRecord::cancelRequested and settles the task with
  // cancelTask() (a task may also legitimately finish as completed).
  // False when the task is unknown.
  virtual bool requestCancel(const std::string& taskId) = 0;
};

// Mutex-guarded map store, bounded by `capacity` (oldest terminal records are
// evicted first when full).
class InMemoryTaskStore final : public TaskStore {
 public:
  explicit InMemoryTaskStore(size_t capacity = 1024);

  bool put(const TaskRecord& record) override;
  std::optional<TaskRecord> get(const std::string& taskId) override;
  bool erase(const std::string& taskId) override;
  std::vector<TaskRecord> all() override;
  bool updateInput(const std::string& taskId, const Json& inputResponses) override;
  bool requestCancel(const std::string& taskId) override;

 private:
  struct Entry {
    TaskRecord record;
    uint64_t sequence = 0;  // insertion order, for capacity eviction
  };
  std::mutex mutex_;
  std::map<std::string, Entry> entries_;
  size_t capacity_;
  uint64_t nextSequence_ = 0;
};

// ---- lifecycle helpers (worker-side) -------------------------------------------

// Marks the task completed with `result` (a tool-call result object).
bool finishTask(TaskStore& store, const std::string& taskId, Json result);
// Marks the task failed with the JSON-RPC error object `error`
// ({"code":...,"message":...,"data":...} as it should appear on the wire).
bool failTask(TaskStore& store, const std::string& taskId, Json error);
// Marks the task cancelled (the cooperative exit path).
bool cancelTask(TaskStore& store, const std::string& taskId);
// Publishes outstanding in-task requests and switches to `input_required`.
bool requestTaskInput(TaskStore& store, const std::string& taskId, Json inputRequests);
// Refreshes lastUpdatedAt (and the status message when non-empty).
bool touchTask(TaskStore& store, const std::string& taskId,
               const std::string& statusMessage = {});

// Applies the TTL policy to every record the store exposes:
//  - a non-terminal task past createdSteady + ttlMs becomes `failed`
//    ("task expired"), the SDK's hard-stop safety valve;
//  - a terminal task is erased one further TTL window after it settled, so
//    pollers can still observe the final state.
// The three tasks/* handlers call this before every operation.
void sweepExpired(TaskStore& store);

}  // namespace tasks
}  // namespace mcp
