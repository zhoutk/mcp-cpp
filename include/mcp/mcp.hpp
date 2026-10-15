#pragma once
// ---------------------------------------------------------------------------
// mcp.hpp - single-include entry point for the mcp-cpp SDK.
//
// Independent MCP (Model Context Protocol) C++ SDK targeting the
// 2026-07-28 specification. See README.md and PLAN.md in the repo root.
// ---------------------------------------------------------------------------

#include <mcp/error.hpp>
#include <mcp/json.hpp>
#include <mcp/jsonrpc.hpp>
#include <mcp/request_state.hpp>
#include <mcp/client.hpp>
#include <mcp/server.hpp>
#include <mcp/transport.hpp>
#include <mcp/types.hpp>
#include <mcp/version.hpp>

// Present only when the HTTP transport is compiled in (MCP_HAS_HTTP); a
// pure-stdio build (MCP_DISABLE_HTTP=ON) omits it.
#if defined(MCP_HAS_HTTP)
#include <mcp/oauth.hpp>
#include <mcp/transport_http.hpp>
#endif
