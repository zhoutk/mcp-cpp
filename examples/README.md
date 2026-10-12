# Examples

Every example is a standalone executable built by the normal build
(`cmake --build build -j 8`, binaries land in `bin/`). Read them in this order.

| Example | What it teaches |
| --- | --- |
| `minimal_server.cpp` | The smallest useful server: tool registration, stdio serving, and `--http <port>` for Streamable HTTP. Also the manual target for the [MCP Inspector](https://modelcontextprotocol.io/docs/tools/inspector). |
| `conformance_server.cpp` | The fixture the official conformance suite drives: full tool/resource/prompt surface, legacy-client mode, fixed-port HTTP. See [docs/CONFORMANCE.md](../docs/CONFORMANCE.md). |

### Running a server under the Inspector

```bash
npx @modelcontextprotocol/inspector ./bin/minimal_server.exe
```

### Running a server over Streamable HTTP

```bash
./bin/minimal_server.exe --http 8080
# then point any MCP HTTP client at http://127.0.0.1:8080/mcp
```

### Embedded example: zorm_server

`examples/zorm_server.cpp` is built only inside the
[zorm](https://github.com/zhoutk/zorm) workspace (it links zormlib) and shows a
real database exposed as MCP tools in ~150 lines — the flagship consumer of this
SDK.

## Adding an example

Keep examples single-file, self-contained, and buildable with the default
options (no HTTP-only features behind `#ifdef` unless the example is about the
HTTP transport). Register the target in the root `CMakeLists.txt` under
`MCP_BUILD_EXAMPLES` and add a row to the table above.
