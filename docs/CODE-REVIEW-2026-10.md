# 代码评审报告与下一步开发计划

> 评审对象：mcp-cpp `v0.2.0`（提交 `fa074ce`），include+src 6,136 行，13 个 CTest 套件
> 评审日期：2026-10-19 · 方法：四个并行子评审（协议核心 / 服务器与传输 / schema+crypto+oauth / 测试构建文档）+ 逐项复现
> 关联：`SESSION-REVIEW.md`（上一轮工作评审）、`PROMOTION-STATUS.md`（推广已暂停）

---

## 一、结论先行

**v0.2.0 的协议面是完整的，但工程健壮性不达标。** 上一轮评审聚焦"协议是否对齐"，结论是肯定
的（2026-07-28 双 100%）；本轮聚焦"实现是否可靠"，结论是否定的：**4 个 P0、24 个 P1**，
其中多个是**默认配置下必然触发**的崩溃、挂起或安全绕过，而非边缘情况。

最严重的四个问题：

1. **HTTP 传输与"单线程派发"的架构假设直接冲突**（P0）。httplib 的线程池有 **64 个 worker**
   （`thirds/httplib/CMakeLists.txt:15`），每个 POST 都直接调用 `Server::processLine`
   （`src/transport_http.cpp:286`），而 `Server` 的 `cancellations_`、`logLevel_` 是无锁的
   `std::map`/`std::string`——并发 POST 即数据竞争。**这不是理论问题：任何真实 HTTP 部署
   都是并发的。**
2. **C++23 下直接编译失败**（P1，但影响所有现代消费者）。`Ok(lvalue)` 推导出
   `std::expected<const Json&,…>`，libc++ 拒绝引用类型成员。实测：
   `clang++ -std=c++23 -fsyntax-only src/client.cpp` 报错。库自称"C++17 起、向上兼容"。
3. **OAuth 客户端可被远端一条 401 打崩**（P1 安全）。`parseUrl` 接受端口 99999，
   `httplib::Client` 构造后内部指针为空，`set_connection_timeout` 解引用空指针 →
   恶意服务器用一个 `resource_metadata="http://host:99999/"` 即可让客户端进程 SIGSEGV。
   另有 HTTPS 未编译 TLS 后端时构造函数直接抛异常且无人捕获。
4. **SSE 订阅流在正常空闲下就会死**（P0）。订阅流的读超时设为 **1 秒**
   （`transport_http.cpp:829`），而服务器每 15 秒才发一次 keep-alive——静默超过 1 秒的
   订阅必然被 httplib 判为 `Error::Read` 并断开，且**没有重连**。即
   `subscriptions/listen` 只在持续有流量时可用。

另有若干"文档与实现不符"（`ARCHITECTURE.md` 称"没有 OAuth"、"有元测试强制头文件方向"、
"官方套件没有 2026 场景"——三者均与当前代码/事实矛盾），说明上一轮收尾时文档先于代码更新。

---

## 二、缺陷清单（按严重度）

### P0 — 默认路径必现或结构性错误

| # | 位置 | 问题 | 触发条件 | 复现 |
| --- | --- | --- | --- | --- |
| 1 | `transport_http.cpp:286` + `thirds/httplib/CMakeLists.txt:15` | 64 线程并发调用无锁 `Server::processLine`；`cancellations_`（`server.cpp:418/448`）、`logLevel_`（`:474`）竞争 | 两个并发 HTTP POST | 代码事实 |
| 2 | `transport_http.cpp:829` | 订阅流读超时 1s < keep-alive 15s，空闲即断且不重连 | 订阅后静默 >1s | 代码事实 |
| 3 | `CMakeLists.txt:106/242-251` | 安装后的 `mcp::mcp` 缺 winsock/threads 链接依赖，消费者用 HTTP 传输链接失败（`WSAAccept`/`freeaddrinfo` 未定义） | `find_package` 后使用 HTTP | 子评审已复现 |
| 4 | `CMakeLists.txt:17` | `project(... VERSION 0.1.0)` 与 `version.txt`/`kSdkVersion` 的 0.2.0 不一致；`find_package(mcp 0.2)` 被拒 | 版本化依赖 | 代码事实 |

### P1 — 严重（安全、崩溃、协议不合规）

| # | 位置 | 问题 |
| --- | --- | --- |
| 5 | `error.hpp:84,194` | C++23 编译失败（`Ok(lvalue)` → `expected<const Json&>`）；需 `std::decay_t` |
| 6 | `types.cpp:142` + `server.cpp:376` | legacy 模式下 `decodeRequestMetaRelaxed` 用 Latest 填充，随后被版本门拒绝；只支持 2025 的 legacy 服务器首请求即 `-32022` |
| 7 | `transport_http.cpp:304` | 单行派发回复恒为 HTTP 200；`-32022`/`-32021`/缺 `_meta` 的 `-32602` 规范要求 400 |
| 8 | `server.cpp:214-243` | 客户端能力未声明时回 `-32603` 而非 `-32021 + data.requiredCapabilities`（`MissingRequiredClientCapability` 从未被发出） |
| 9 | `types.cpp:17` / `jsonrpc.cpp:151,161` | `toInt64()` 对超范围 double 是 UB；`-9223372036854775809` 被静默接受为 INT64_MIN（已用 UBSan 复现） |
| 10 | `transport_http.cpp:108` | Origin 前缀匹配：`http://localhost.evil.example` 通过 DNS-rebinding 防护 |
| 11 | `transport_http.cpp:877` | 订阅 POST 丢弃返回值；服务器拒绝（400/403）时不报错、不 ack，客户端干等 30s |
| 12 | `transport_http.cpp:715` | `read()` 30s 空闲返回 EOF 语义；>30s 的请求被误判为"连接关闭" |
| 13 | `server.cpp:313` | `notifications/cancelled` 可为任意 id 建条目且永不清理（内存 DoS）；`idToString` 使 `1` 与 `"1"` 冲突 |
| 14 | `transport_http.cpp:571,1030` | 远端 `retry:` 值经 `atoi` 无上限，`sleep_for(INT_MAX+250)`（有符号溢出 + 线程睡眠 23 天，析构挂死） |
| 15 | `base64.cpp:88-96` | 空字符串头值编码为 `=?base64??=` 且解码端不识别 → 空值参数永久 HeaderMismatch |
| 16 | `transport_http.cpp:267` | `isKnownMethod` 含 legacy-only 方法；严格 2026 服务器对 `initialize/ping` 回 200+`-32601`，规范要求 404 |
| 17 | `transport_http.cpp:277` | `subscriptions/listen` 绕过 `processLine`，不校验 `_meta`/版本/legacy 门 |
| 18 | `oauth.cpp:71,253` | 端口无范围校验 + `httplib::Client` 无 `is_valid()` → 远端可触发空指针崩溃 |
| 19 | `oauth.cpp:253` | 无 TLS 后端时 `https://` 构造抛异常逃逸；`oauth.hpp` 承诺 "nothing throws" |
| 20 | `schema.cpp:352,361,369` | `anyOf/oneOf/not` 重置检查计数器，`maxChecks` 不是全局上限（嵌套 schema CPU DoS，实测 11s） |
| 21 | `schema.cpp:56,838` | 接受 `http://` AS 端点；authorization code / PKCE verifier / client_secret 明文传输 |
| 22 | `oauth.cpp:547,688` | 不解析 `code_challenge_methods_supported`，无法检测 PKCE 降级（规范要求缺失即拒绝） |
| 23 | `schema.cpp:179` | `$ref` 兄弟关键字被忽略（2020-12 应同时求值） |
| 24 | `schema.cpp:90` | JSON Pointer 用 `operator[]`（深搜泄漏）+ `stoi` 越界抛异常逃逸 |
| 25 | `schema.cpp:266` | `minProperties`/`maxProperties` 用 `Json::size()`（对象返回 -1）→ 双双失效 |
| 26 | `schema.cpp:293` | 键查找用 `c_str()`，含 NUL 的键被截断，`additionalProperties:false` 可绕过 |
| 27 | `schema.cpp:247` | 2020-12 `prefixItems` 未实现，元组约束被静默忽略 |
| 28 | `server.cpp:492,654` 等 | handler 抛异常无 try/catch：stdio 下直接终止进程，HTTP 下裸 500 |

### P2 — 次要

| # | 位置 | 问题 |
| --- | --- | --- |
| 29 | `jsonrpc.cpp:14` | 使用宽松解析（允许注释）；JSON-RPC 要求严格 JSON |
| 30 | `jsonrpc.cpp:71,79` | 可恢复的 id 在部分无效请求路径被丢弃，错误回复缺 id |
| 31 | `types.cpp:34` | 未知 `resultType` 被当作 `complete`；规范要求视为无效 |
| 32 | `header_params.h:48` | `operator[]` 深搜：嵌套参数值被镜像进 HTTP 头 |
| 33 | `jsonrpc.cpp:14` | 未校验 UTF-8（接受 0xFF、孤立代理项） |
| 34 | `jsonrpc.cpp:161` | 错误码 int64→int32 静默截断 |
| 35 | `types.cpp:45` + `client.cpp:234` | `requestState` 空串与缺失不可区分，空串不回显 |
| 36 | `types.cpp:122` | 小数 `progressToken` 被静默丢弃 |
| 37 | `transport_http.cpp:219` | 拒绝一切未识别 `Mcp-Param-*`（规范要求忽略） |
| 38 | `transport_http.cpp:834` | `thread(...).detach()` 创建失败时计数器泄漏，析构永久等待 |
| 39 | `CMakeLists.txt:92,127` | `MCP_DISABLE_HTTP` 只守 httplib 发现；宿主已有 `httplib` target 时纯 stdio 仍构建空测试 |
| 40 | `CMakeLists.txt:180` | 嵌入宿主时 `enable_testing()` 在子目录内，根 `ctest` 列 0 个测试 |
| 41 | `CMakeLists.txt:199+` | 测试无 `TIMEOUT`；e2e 启动循环可无限自旋 |
| 42 | 文档 | `ARCHITECTURE.md` 三处陈述与代码矛盾（No OAuth / 有元测试 / 套件无 2026 场景）；`CONFORMANCE.md` 同 |
| 43 | 测试 | `onServerRequest`、`handleRedirect` 的 state 校验、多个 client 封装、`structuredResult` 无测试 |
| 44 | CI | 从未构建 `MCP_DISABLE_HTTP=ON`、`MCP_BUILD_TESTS=OFF`、`cmake --install` + 消费者 |

---

## 三、根因分析

1. **"单线程派发"在 HTTP 下从未被真正验证。** 设计文档写的是单线程，HTTP 传输也没加锁，
   但 httplib 的线程池默认 64 并发——两者一直共存却没被发现，因为所有测试都是单请求串行。
   这是"架构假设未经并发验证"的典型。
2. **参考实现只被用来对标协议面，没有用来对标工程面。** rust/TS 的传输层都有显式的
   "transport is not Send/Sync" 或锁边界；我们只抄了协议行为。
3. **测试集中在 happy path。** 13 个套件里没有一个并发测试、超时测试、异常测试或
   "安装后消费者"测试——这恰好是 P0 集中的区域。
4. **文档先于代码更新**（上一轮收尾时把 `ARCHITECTURE.md` 写成目标状态而非实际状态）。
5. **schema 校验器是自研的，但只按"够用"实现**，`anyOf` 计数、`prefixItems`、`size()` 等
   边界没有测试覆盖。

---

## 四、下一步开发计划

### 阶段 A：止血（P0，1~3 天）—— 不修完不得对外宣传

| # | 任务 | 验收 |
| --- | --- | --- |
| A1 | HTTP 并发安全：给 `Server::processLine` 加互斥（最简正确解），或改为每请求独立 `Server` 实例（对齐 TS 的 factory 模型）。**二选一需在报告中定案** | 新增并发测试：64 个并发 POST 混发 `tools/call` + `cancel`，TSan 无告警 |
| A2 | 订阅流超时改为 30s，并加重新订阅/重连 | 新增"静默 5s 后仍收到通知"的 e2e |
| A3 | 安装包补 `ws2_32`/`Threads::Threads` 等链接依赖；CI 加"安装后消费者"测试 | 消费者 `find_package` + 使用 HTTP 传输能链接运行 |
| A4 | `project(VERSION 0.2.0)`，并让 `test_version` 校验 `version.txt` 与 `kSdkVersion` 一致 | 版本测试改为真实断言 |

### 阶段 B：安全与崩溃（P1，3~5 天）

| # | 任务 |
| --- | --- |
| B1 | `Ok(lvalue)` 用 `std::decay_t`；CI 增加 `-std=c++23` 编译检查 |
| B2 | legacy 版本门：relaxed 路径跳过或采用协商版本 |
| B3 | HTTP 状态码：`-32022`/`-32021`/缺 `_meta` 的 `-32602` 回 400 |
| B4 | `-32021` 携带 `data.requiredCapabilities` |
| B5 | 数值边界：id/错误码/进展 token 先 `isIntegral()` 或范围检查 |
| B6 | Origin 精确匹配（host 解析后比较） |
| B7 | 订阅 POST 结果检查 + 错误入队；`read()` 空闲超时与 EOF 区分 |
| B8 | `cancellations_` 清理与 id 类型区分；`retry:` 上限钳制 |
| B9 | 空头值 base64 编解码修复 |
| B10 | OAuth：端口范围校验、`is_valid()`、捕获构造异常、强制 HTTPS、PKCE 降级检测 |
| B11 | schema：全局检查计数、`prefixItems`、`size()`→真实属性数、NUL 键、`$ref` 兄弟、Pointer 安全解析、正则异常 |

### 阶段 C：测试补齐（P2，3~5 天）

- C1 并发/超时/异常三类"非 happy path"测试成为每个传输的标配。
- C2 `onServerRequest`、`handleRedirect` 负路径、client 全部封装、`structuredResult` 补测。
- C3 CI 矩阵补 `MCP_DISABLE_HTTP=ON`、`MCP_BUILD_TESTS=OFF`、install+consumer、`-std=c++23`、TSan。
- C4 测试加 `TIMEOUT`，启动循环加截止时间。
- C5 文档纠正三处与代码矛盾的陈述，补"并发模型"章节说明 HTTP 的实际线程模型。

### 阶段 D：发布

- D1 修复并全绿后发布 **v0.3.0**（语义化：并发与安全修复属于行为修正，但 `Server` 的
  线程安全承诺变化，按 MINOR + 显著 release note 处理；若改成每请求实例则可能需 MAJOR）。
- D2 更新 `ROADMAP.md` 与推广材料中的"生产就绪"表述——**在 A/B 完成前不宣称生产可用**。

---

## 五、优先级建议（如果只能做三件事）

1. **A1 + A2**：HTTP 并发锁与订阅超时——这两个是真实部署中 100% 会遇到的问题。
2. **A3 + A4**：安装包与版本——消费者第一次 `find_package` 就会踩到。
3. **B1 + B5 + B10**：C++23、数值 UB、OAuth 空指针——一个是编译门槛、一个是崩溃、
   一个是可被远端利用的崩溃。

---

*评审：四个独立子评审 + 主评审复现。所有 P0/P1 均给出文件行号与触发条件；标记"已复现"
的项由主评审或子评审实际编译/运行验证。*

---

## 六、修复进展（2026-10-19）

### 阶段 A（P0）—— 已全部完成

| # | 结论 | 实现 | 验证 |
| --- | --- | --- | --- |
| A1 — HTTP 并发 | **定案：给 `processLine` 加互斥**（每请求实例需要重构 handler 生命周期与注册表，代价高于本轮收益；互斥保留了"handler 单线程"的既有承诺） | `Server::dispatchMutex_`，`processLine()` 全程持锁；`cancellations_` 独立 `cancellationsMutex_`；取消通知在锁**之前**处理并原地置位已有 flag | `ServerConcurrency.*`（16 线程 + 取消可见性）、`HttpE2E.ConcurrentPostsAreSerializedSafely`（64 并发客户端混发 `tools/call` + `notifications/cancelled`）；CI 新增 `tsan` job |
| A2 — 订阅流 | 读超时 1s → **30s**（> keep-alive 节奏，现为 5s）；断流后**自动重订阅**；`retry:` 钳制 100ms–30s；重连 ack 去重 | `src/transport_http.cpp` 订阅循环：`stream->set_read_timeout(30, 0)`，`receiver` 拆事件 → 队列，失败后按 `retry` 等待再发 `subscriptions/listen` | `HttpE2E.SubscriptionSurvivesIdlePeriod`（静默 5s 后仍收到通知）、`HttpE2E.SubscriptionStreamDeliversNotifications` |
| A3 — 安装包链接 | `mcp` 显式 `INTERFACE` 传播 `ws2_32`（Windows）/`Threads::Threads`（POSIX）；`mcpConfig.cmake.in` 解析 `Threads` | `CMakeLists.txt` HTTP 分支；`cmake/mcpConfig.cmake.in` | 本机 `cmake --install` + `tests/consumer`（`find_package(mcp 0.2 CONFIG)`，使用 `HttpServerTransport`）配置、编译、运行全部通过；CI 新增 `install-consumer` job |
| A4 — 版本 | `project(VERSION)` 改为读取 `version.txt`（唯一事实来源） | `CMakeLists.txt` 顶部 `file(READ)/string(STRIP)`；`mcp_test_version` 注入 `MCP_EXPECTED_SDK_VERSION` 做真实断言 | 安装包 `mcpConfigVersion.cmake` 为 `0.2.0`；`mcp_test_version` 通过 |

**回归结果**：13/13 CTest 全绿；`-std=c++17`（默认）与安装后消费者均通过。

### 阶段 B（P1）—— 进度

已随本轮一并修复：

- B5 数值边界（id / 错误码范围），B8 的 `retry:` 上限，B9 之外的取消语义缺口
  （`Client::cancel` 缺 `_meta` 导致 HTTP 400）。

尚未开始：B1 的 CI `-std=c++23` 任务、B2–B4（legacy 版本门 / HTTP 400 / `-32021` 数据）、
B6–B11（Origin 精确匹配、订阅 POST 结果检查、`read()` 空闲语义、base64 空值、OAuth 端口与
TLS、schema 的计数/`prefixItems`/`size()`/NUL 键/`$ref` 兄弟）。这些问题不会在默认路径上
崩溃，但仍是发布 v0.3.0 前必须清掉的清单。

### 本轮发现并修复的新缺陷

- **取消只会"预登记"，不会"生效"。** 原实现把 `cancellations_[id]` **替换**为新 flag，而已在
  运行的请求持有的是旧 flag，因此并发取消永远观察不到（单线程顺序测试无法暴露）。
  现改为：存在则 `store(true)`，不存在才插入预置 flag。
- **`Client::cancel()` 缺少 `_meta`。** HTTP 传输无法镜像 `MCP-Protocol-Version`，严格 2026
  服务器回 400，取消在 HTTP 上完全不可用。
- **订阅重连的 ack 重复。** 重连会重发 `subscriptions/listen`，服务器再回一次 ack；
  客户端现只入队首次 ack，避免调用方认为有多条订阅。

### 复核结论与下一步计划（2026-10-19 审核后）

**复核结论：阶段 A 全部属实、实现正确。** 逐项对照了 `server.cpp`/`server.hpp`/`transport_http.cpp`/
`client.cpp`/CMake/CI 的 diff：

- A1 的"取消先于 dispatch 锁 + 原地翻转 flag"顺序正确；`cancellationsMutex_` 与 `dispatchMutex_`
  无嵌套，`subscriptionsMutex_` 单独持有，无锁序环。锁注释与 AGENTS.md/ARCHITECTURE.md 已同步。
- A2 的订阅重连循环：注册/注销均在 `subMutex` 内，`closeSubscriptions()` 的 `notify_all` 补齐了
  "线程正处在重连等待" 的唤醒缺口；ack 去重与服务端行为吻合（ack=notification，关闭=result）。
- A3/A4 的 `INTERFACE` 传播、`find_dependency(Threads)`、`version.txt` 事实源与
  `MCP_EXPECTED_SDK_VERSION` 断言均正确；`tests/consumer` 与 `install-consumer`/`tsan` CI job 就位。
- **本机验证：13/13 CTest 全绿**（含 `ServerConcurrency.*` 与三个新 HTTP e2e 用例）。
- 复核中仅发现一处失效注释（`SubscriptionSurvivesIdlePeriod` 仍写"keep-alive 15s"，实际已改 5s），
  已顺手修正，无代码问题。

**下一步计划（阶段 B 收尾 → C → D）—— 已于 2026-10-19 全部完成：**

| 序 | 任务 | 提交 |
| --- | --- | --- |
| 1 | B7 + B8 收尾（`read()` 空闲/EOF 语义、订阅拒绝上报、取消键类型化 + parked 上限） | `a021f99` |
| 2 | B3 + B4（协议级拒绝回 HTTP 400、`-32021` + `data.requiredCapabilities`、`error.data` 不再丢失） | `b54dad8` |
| 3 | B6 + B9（Origin host 精确匹配、空头值 base64 往返） | `eb0b6d6` |
| 4 | B10 + B11（OAuth：端口边界/不抛异常/强制 https/PKCE 降级拒绝；schema：全局预算、`$ref` 兄弟、Pointer 安全、`min/maxProperties`、NUL 键、`prefixItems`） | `caf1b83` |
| 5 | B1 收尾（CI `cxx23` 任务）+ B2（legacy 版本门填入最老支持版本） | `72fbc96` |
| 6 | 阶段 C（纯 stdio CI 任务、CTest `TIMEOUT`、启动循环加截止时间） | `dbf4d54` |
| 7 | 阶段 D：**v0.3.0 已发布**（`92518fd`，tag `v0.3.0`），CHANGELOG/README/ROADMAP 已更新 | `92518fd` |

发布时验证：13/13 CTest（HTTP）、11/11（纯 stdio）、13/13（C++23 `-Werror` 构建）、
安装包版本 0.3.0。本文档的 P0/P1 清单至此全部收口；后续工作回到 ROADMAP.md。
