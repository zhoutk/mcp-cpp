> 语言 / Language: **中文** | [English](CONTRIBUTING.md)

# 为 mcp-cpp 做贡献

感谢你帮助构建这个 C++ MCP SDK。本文档涵盖如何构建、测试和提交更改，以及本项目
所承诺的依赖策略。

## 环境准备

要求：CMake ≥ 3.18 和一个 C++17 编译器。经验证的主要工具链是 clang 22（MSYS2
clang64）配合 Ninja；GCC ≥ 11 和 MSVC v19.3x 是受支持的目标。

```bash
git clone https://github.com/zhoutk/mcp-cpp
cd mcp-cpp
cmake --preset clang-rlz          # Release; clang-dbg for Debug
cmake --build build -j 8
ctest --test-dir build --output-on-failure
```

在没有预设中硬编码的 MSYS2 路径的机器上，请手动配置：

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

独立配置需要在 `thirds/zjson` 下有一个 zjson 单头文件，或传入
`-DMCP_ZJSON_DIR=<dir>`。

### 常用目标

```bash
ctest --test-dir build -R mcp_test_server -V        # one suite
bin/mcp_test_server.exe --gtest_filter=ServerProgress.*   # one case
bin/mcp_bench_parse.exe                              # benchmarks (not in CTest)
```

可执行文件落在 `bin/`，中间产物在 `build/`。

## 做出更改

1. **对于任何非琐碎的改动，先开一个 issue**，这样我们可以在你写代码之前就先就
   方案达成一致。请使用 issue 模板。
2. **从 `main` 拉分支**；保持提交聚焦。
3. **与周围的代码保持一致**——本代码库有很强的约定：
   - 协议读取走 `mcp::member(obj, key)`，**绝不**用 `obj[key]`——zjson 的
     `operator[]` 会对缺失的键做深度搜索，从而误读嵌套的协议数据。
   - 文本值要用 `Json::str(...)` 包裹——zjson 会嗅探开头的 `{`/`[`。
   - 用 `isNull()`/`isError()`/`hasChild` 判断是否存在，绝不用 `toString().empty()`。
   - 新的协议措辞放在 `guide.cpp`/相关头文件注释中，以保持前后一致。
4. **添加测试。** 新的服务端行为放在 `tests/test_server.cpp`；新的协议类型放在
   对应的 `tests/test_*.cpp`。测试通过内存中的 `MemTransport` 驱动服务器，因此
   无需真实的 IO。
5. **保持构建无警告**（`-Wall -Wextra -Wpedantic`；MSVC 上为 `/W4`）。
6. **在开 PR 之前运行完整套件**：`ctest --test-dir build --output-on-failure`。

## 拉取请求

- 每个 PR 只做一项逻辑改动；描述*为什么*，而不只是*做了什么*。
- 引用它所关闭的 issue。
- CI 必须为绿色（构建矩阵 + 测试 + 一致性）。
- 由一名维护者评审；请预期反馈会先关注协议保真度，其次才是风格。

## 依赖策略

mcp-cpp 刻意保持极小的依赖面，以便能直接放进任何 C++17 项目。

| 依赖 | 作用 | 策略 |
| --- | --- | --- |
| **zjson** | JSON 层（`mcp::Json` 别名） | 随附的单头文件。必需。视为 SDK 的一部分。 |
| **cpp-httplib** | Streamable HTTP 传输 | 可选（`MCP_DISABLE_HTTP=ON` 可移除它）。仅头文件；通过 pimpl 保持在公共头文件之外。 |
| **googletest** | 仅用于测试 | 从不链接进库。 |

规则：

- **新增任何运行时依赖都需要一个 RFC 风格的 issue** 以及维护者的批准。
- 新依赖必须是仅头文件的、或可轻松内嵌的，采用宽松许可证，且不得泄漏进公共头文件
  （`include/` 保持除 `mcp::Json` 别名外无依赖）。
- **更新节奏：** 随附的第三方组件在出现安全公告时刷新，或至少每个次版本刷新一次；
  该改动记录在 `CHANGELOG.md` 中。
- 公共头文件除随附的 JSON 头文件外，不得包含任何第三方头文件。CI 通过 grep 检查
  强制执行此规则。

## 发布

1. 更新 `version.txt`、`include/mcp/version.hpp`（`kSdkVersion`）和 `CHANGELOG.md`。
   `CMakeLists.txt` 直接读取 `version.txt`，无需改 CMake；两者不一致时版本测试会失败。
2. 打 `vX.Y.Z` 标签；CI 的发布任务会构建并附加产物。
3. 运行一致性套件，并将得分记录在 changelog 中。

## 行为准则

参与本项目即表示你同意[行为准则](CODE_OF_CONDUCT.md)。安全问题请按
[SECURITY.md](SECURITY.md) 报告，而不要发在公开 issue 中。

## 提问

发起一个 GitHub Discussion，或使用 `question` 模板开一个 issue。
