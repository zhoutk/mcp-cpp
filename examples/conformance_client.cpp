// conformance_client - the client fixture for the official conformance suite
// (see docs/CONFORMANCE.md). Counterpart of examples/conformance_server.cpp.
//
// Contract (verified against the suite and the rust-sdk reference fixture):
//   - the harness appends the SERVER URL as the last command-line argument;
//   - MCP_CONFORMANCE_SCENARIO names the scenario;
//   - MCP_CONFORMANCE_PROTOCOL_VERSION pins the spec revision to run
//     (default 2025-11-25; >= 2026-07-28 uses the discover lifecycle);
//   - MCP_CONFORMANCE_CONTEXT (optional JSON) carries scenario data, e.g.
//     {"toolCalls":[{"name":"x","arguments":{...}}]}.
//
// Scenarios mirror the rust-sdk fixture (refer/rust-sdk/conformance/src/bin/
// client.rs): non-auth scenarios are implemented with mcp::client::Client;
// auth/* scenarios require OAuth (not implemented) and exit non-zero so the
// harness records them as baseline expected failures.

#include <mcp/mcp.hpp>

#include "../src/header_params.h"  // SEP-2243 annotation validation (detail)

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(MCP_HAS_HTTP)

using namespace mcp;
using namespace mcp::client;

namespace {

constexpr const char* kDefaultProtocolVersion = "2025-11-25";

struct Url {
  std::string raw;         // the URL exactly as the harness passed it
  std::string host;
  int port = 80;
  std::string endpoint = "/";
};

Url parseUrl(const std::string& url) {
  Url out;
  out.raw = url;
  std::string rest = url;
  if (rest.rfind("http://", 0) == 0) {
    rest = rest.substr(7);
  } else if (rest.rfind("https://", 0) == 0) {
    rest = rest.substr(8);  // plain client: https is not supported
  }
  const size_t slash = rest.find('/');
  const std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
  if (slash != std::string::npos) out.endpoint = rest.substr(slash);
  const size_t colon = authority.find(':');
  out.host = colon == std::string::npos ? authority : authority.substr(0, colon);
  if (colon != std::string::npos) {
    out.port = std::atoi(authority.c_str() + colon + 1);
  }
  if (out.endpoint.empty()) out.endpoint = "/";
  return out;
}

int64_t unixNow() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// A client connected with the lifecycle the runner asked for. `legacy` means
// the 2025-era initialize handshake; otherwise the stateless 2026 discover.
struct Session {
  mcp::http::HttpClientTransport transport;
  Client client;

  Session(const Url& url, bool legacy, const std::string& protocolVersion,
          const std::string& bearer = std::string())
      : transport(url.host, url.port, url.endpoint),
        client("mcp-cpp-conformance", "1.0.0", Json(R"({"elicitation":{}})")) {
    if (!bearer.empty()) transport.setHeader("Authorization", "Bearer " + bearer);
    // Declare the runner's revision on every request; a 2025-era mock server
    // rejects a 2026 _meta version with -32022.
    if (auto v = fromString(protocolVersion)) client.setProtocolVersion(*v);
    client.connect(transport);
    if (legacy) {
      // A failed handshake surfaces on the first real request below; do not
      // abort here so the harness observes the actual wire behaviour.
      auto init = client.initialize(protocolVersion,
                                    Implementation{"mcp-cpp-conformance", std::string(),
                                                   "1.0.0", std::string(), std::string(),
                                                   Json(nullptr)});
      // The GET listening stream echoes MCP-Protocol-Version - it MUST be the
      // version the server negotiated, not our default. A mismatching header
      // makes the peer reject the stream with 400 and the server-initiated
      // requests never reach us.
      if (init.has_value()) {
        const Json negotiated = member(init.value(), "protocolVersion");
        if (negotiated.isString()) {
          if (auto v = fromString(negotiated.toString())) {
            transport.setProtocolVersion(*v);
          }
        }
      }
    } else {
      (void)client.discover();
    }
  }
};

// SEP-1034: when the server elicits input, a conforming client applies the
// schema's declared defaults. Shared by the legacy (server-initiated request)
// and modern (MRTR inputRequests) shapes.
Json elicitationDefaults(const Json& params) {
  Json content = Json(R"({})");
  const Json schema = member(params, "requestedSchema");
  const Json props = member(schema, "properties");
  if (props.isObject()) {
    for (const auto& key : props.getAllKeys().toVector()) {
      const Json prop = member(props, key.toString());
      const Json def = member(prop, "default");
      if (!def.isNull()) content.add(key.toString(), def);
    }
  }
  Json result;
  result.add("action", Json::str("accept"));
  result.add("content", std::move(content));
  return result;
}

bool isElicitation(const std::string& method) { return method == "elicitation/create"; }

// Synthesises arguments for a tool from its inputSchema (number -> 1.0,
// integer -> 1, string -> "test", boolean -> true, otherwise null).
Json buildToolArguments(const Json& tool) {
  Json args = Json(R"({})");
  const Json props = member(member(tool, "inputSchema"), "properties");
  if (!props.isObject()) return args;
  for (const auto& key : props.getAllKeys().toVector()) {
    const Json prop = member(props, key.toString());
    const Json type = member(prop, "type");
    const std::string t = type.isString() ? std::string(type.toString()) : std::string();
    if (t == "number") args.add(key.toString(), Json(1.0));
    else if (t == "integer") args.add(key.toString(), Json(static_cast<int64_t>(1)));
    else if (t == "string") args.add(key.toString(), Json::str("test"));
    else if (t == "boolean") args.add(key.toString(), Json(true));
  }
  return args;
}

Json contextJson() {
  const char* raw = std::getenv("MCP_CONFORMANCE_CONTEXT");
  if (raw == nullptr) return Json(nullptr);
  std::string err;
  return Json::ParseJson(std::string(raw), err);
}

// Calls the tools the runner asked for: the context's toolCalls verbatim, or
// every listed tool with synthesized arguments. Tools with malformed
// x-mcp-header annotations are EXCLUDED (SEP-2243): one malformed definition
// must not block the valid tools.
McpResult<void> performToolCalls(Client& client, const Json& context) {
  auto tools = client.listTools();
  if (!tools.has_value()) return Err(tools.error());
  const Json list = member(tools.value(), "tools");
  std::cerr << "[conformance-client] tools/list returned "
            << (list.isArray() ? list.toVector().size() : 0) << " tools" << std::endl;

  std::map<std::string, Json> callable;  // name -> definition (annotation-valid)
  for (const auto& tool : list.toVector()) {
    const Json name = member(tool, "name");
    if (!name.isString()) continue;
    if (!detail::headerAnnotationsValid(member(tool, "inputSchema"))) {
      std::cerr << "[conformance-client] excluding invalid-annotation tool "
                << name.toString() << std::endl;
      continue;
    }
    callable[std::string(name.toString())] = tool;
  }

  const Json calls = member(context, "toolCalls");
  if (calls.isArray() && !calls.toVector().empty()) {
    for (const auto& call : calls.toVector()) {
      const Json name = member(call, "name");
      if (!name.isString()) continue;
      if (callable.find(std::string(name.toString())) == callable.end()) {
        std::cerr << "[conformance-client] skipping invalid/unlisted tool "
                  << name.toString() << std::endl;
        continue;
      }
      std::cerr << "[conformance-client] calling (ctx) " << name.toString() << std::endl;
      auto r = client.callTool(std::string(name.toString()), member(call, "arguments"));
      if (!r.has_value()) return Err(r.error());
    }
    return Ok();
  }

  for (const auto& [name, tool] : callable) {
    std::cerr << "[conformance-client] calling " << name << std::endl;
    auto r = client.callTool(name, buildToolArguments(tool));
    if (!r.has_value()) return Err(r.error());
  }
  return Ok();
}

// The lifecycle (legacy initialize vs stateless discover) the runner wants.
bool legacyLifecycle(const std::string& protocolVersion) {
  return protocolVersion < "2026-07-28";
}

// Every failure path must be visible in the harness's captured stderr.
int bail(const std::string& what, const McpError& e) {
  std::cerr << "[conformance-client] " << what << " failed: " << e.message << std::endl;
  return 1;
}

// ---- OAuth (auth/* scenarios) -----------------------------------------------

// Fixed inputs the reference fixture uses; the mocks are written against them.
constexpr const char* kRedirectUri = "http://localhost:3000/callback";
constexpr const char* kClientMetadataUrl = "https://conformance-test.local/client-metadata.json";

std::string contextString(const Json& context, const char* key) {
  const Json value = member(context, key);
  return value.isString() ? std::string(value.toString()) : std::string();
}

// The `scope="a b c"` values from a WWW-Authenticate challenge.
std::vector<std::string> challengeScopes(const std::string& challenge) {
  std::vector<std::string> out;
  const size_t at = challenge.find("scope=\"");
  if (at == std::string::npos) return out;
  const size_t start = at + std::string("scope=\"").size();
  const size_t end = challenge.find('"', start);
  if (end == std::string::npos) return out;
  std::istringstream stream(challenge.substr(start, end - start));
  std::string scope;
  while (stream >> scope) out.push_back(scope);
  return out;
}

// One unauthenticated MCP request, to capture the server's Bearer challenge.
// A server that serves unauthenticated requests yields an empty challenge and
// discovery falls back to the well-known locations.
std::string probeChallenge(const Url& url, const std::string& protocolVersion) {
  mcp::http::HttpClientTransport transport(url.host, url.port, url.endpoint);
  std::string challenge;
  transport.setAuthChallengeHandler([&](int, const std::string& value) {
    if (challenge.empty()) challenge = value;
  });
  Client client("mcp-cpp-conformance", "1.0.0", Json(R"({"elicitation":{}})"));
  if (auto v = fromString(protocolVersion)) client.setProtocolVersion(*v);
  client.connect(transport);
  (void)client.listTools();
  return challenge;
}

// Builds the OAuth client for a scenario: CIMD by default, pre-registration
// when the harness supplied credentials.
oauth::OAuthClient makeOAuthClient(const Url& url, const Json& context, bool preregistered) {
  oauth::Config config;
  config.resourceUrl = url.raw;
  config.redirectUri = kRedirectUri;
  config.clientName = "conformance-client";
  config.clientMetadataUrl = kClientMetadataUrl;
  config.applicationType = "native";
  if (preregistered) {
    config.preregisteredClientId = contextString(context, "client_id");
    config.preregisteredClientSecret = contextString(context, "client_secret");
    config.preregisteredAuthMethod = "client_secret_basic";
  }
  return oauth::OAuthClient(std::move(config));
}

// Runs one authorization-code round and returns the bearer token.
McpResult<std::string> authorize(oauth::OAuthClient& client,
                                 const std::vector<std::string>& scopes) {
  auto authUrl = client.authorizationUrl(scopes);
  if (!authUrl.has_value()) return Err(authUrl.error());
  std::cerr << "[conformance-client] authorization request to " << *authUrl << std::endl;
  auto exchanged = client.authorizeAndExchange(*authUrl);
  if (!exchanged.has_value()) return Err(exchanged.error());
  return Ok(client.accessToken());
}

enum class WorkResult { Ok, ScopeRefused, Unauthorized, Failed };

// Lists and calls every tool the server exposes. A refusal is reported through
// `outStatus`/`outChallenge` (the WWW-Authenticate header of the failing
// response) so the caller can re-authorize.
WorkResult callAllTools(const Url& url, const std::string& protocolVersion,
                        const std::string& token, int& outStatus, std::string& outChallenge) {
  mcp::http::HttpClientTransport transport(url.host, url.port, url.endpoint);
  outStatus = 0;
  outChallenge.clear();
  transport.setAuthChallengeHandler([&](int status, const std::string& value) {
    if (outStatus == 0) {
      outStatus = status;
      outChallenge = value;
    }
  });
  transport.setHeader("Authorization", "Bearer " + token);
  Client client("mcp-cpp-conformance", "1.0.0", Json(R"({"elicitation":{}})"));
  if (auto v = fromString(protocolVersion)) client.setProtocolVersion(*v);
  client.connect(transport);

  const auto verdict = [&]() -> WorkResult {
    if (outStatus == 403) return WorkResult::ScopeRefused;
    if (outStatus == 401) return WorkResult::Unauthorized;
    return WorkResult::Failed;
  };
  auto tools = client.listTools();
  if (!tools.has_value()) return verdict();
  const Json list = member(tools.value(), "tools");
  for (const auto& tool : list.toVector()) {
    const Json name = member(tool, "name");
    if (!name.isString()) continue;
    if (!client.callTool(std::string(name.toString()), buildToolArguments(tool)).has_value()) {
      return verdict();
    }
  }
  return WorkResult::Ok;
}

// auth/* driver: authenticate, then exercise the server, re-authorizing when
// the token is refused. A 403 means "widen the scope" (SEP-2350, union of the
// granted and challenged scopes, at most 3 attempts); a 401 means the token is
// no longer valid - possibly because the protected resource moved to another
// authorization server, in which case the client re-registers there (SEP-2352).
int runAuthScenario(const std::string& scenario, const Url& url, const Json& context,
                    const std::string& protocolVersion) {
  const bool preregistered = scenario == "auth/pre-registration";
  const std::string challenge = probeChallenge(url, protocolVersion);
  std::cerr << "[conformance-client] auth challenge: "
            << (challenge.empty() ? "(none)" : challenge) << std::endl;

  std::vector<std::string> scopes;
  if (scenario == "auth/scope-step-up") scopes = {"mcp:basic"};

  oauth::OAuthClient client = makeOAuthClient(url, context, preregistered);
  auto discovered = client.discover(challenge);
  if (!discovered.has_value()) {
    if (scenario == "auth/resource-mismatch") {
      std::cerr << "[conformance-client] auth correctly rejected: "
                << discovered.error().message << std::endl;
      return 0;
    }
    return bail("oauth discovery", discovered.error());
  }
  // SEP-2207: a client that wants a refresh token asks for offline_access when
  // the authorization server advertises it.
  if (scenario == "auth/offline-access-scope") {
    scopes = client.selectScopes({});
    const auto& asScopes = client.serverMetadata().scopesSupported;
    if (std::find(asScopes.begin(), asScopes.end(), "offline_access") != asScopes.end() &&
        std::find(scopes.begin(), scopes.end(), "offline_access") == scopes.end()) {
      scopes.push_back("offline_access");
    }
  }
  auto token = authorize(client, scopes);
  if (!token.has_value()) return bail("oauth", token.error());

  // The retry cap counts authorization requests: clients MUST NOT loop forever
  // (the spec's "scope challenge handling"; the suite allows at most three).
  constexpr int kMaxAuthorizationAttempts = 3;
  int authAttempts = 1;
  for (int attempt = 0; attempt < 4; ++attempt) {
    int status = 0;
    std::string refusal;
    const WorkResult result = callAllTools(url, protocolVersion, token.value(), status, refusal);
    if (result == WorkResult::Ok) return 0;
    if (result == WorkResult::Failed) return 1;

    if (result == WorkResult::ScopeRefused) {
      const std::vector<std::string> extra = challengeScopes(refusal);
      if (extra.empty()) return 1;
      for (const auto& scope : extra) {
        if (std::find(scopes.begin(), scopes.end(), scope) == scopes.end()) scopes.push_back(scope);
      }
      std::cerr << "[conformance-client] 403; re-authorizing with the scope union" << std::endl;
    } else {
      // SEP-2352: did the protected resource move to another authorization server?
      auto moved = client.refreshAuthorizationServer();
      if (!moved.has_value()) return bail("oauth re-discovery", moved.error());
      if (!moved.value()) {
        std::cerr << "[conformance-client] 401 without an authorization-server change" << std::endl;
        return 1;
      }
      std::cerr << "[conformance-client] 401; authorization server changed, re-registering"
                << std::endl;
    }
    if (authAttempts >= kMaxAuthorizationAttempts) {
      std::cerr << "[conformance-client] authorization retry limit reached; stopping" << std::endl;
      return 0;
    }
    ++authAttempts;
    auto next = authorize(client, scopes);
    if (!next.has_value()) return bail("oauth re-authorization", next.error());
    token = std::move(next);
  }
  std::cerr << "[conformance-client] retry limit reached; stopping" << std::endl;
  return 0;
}

int runScenario(const std::string& scenario, const Url& url, const Json& context,
                const std::string& protocolVersion) {
  const bool legacy = legacyLifecycle(protocolVersion);

  // --- simple connectivity + listing ---------------------------------------
  if (scenario == "initialize" || scenario == "json-schema-ref-no-deref") {
    Session s(url, legacy, protocolVersion);
    auto tools = s.client.listTools();
    if (!tools.has_value()) return bail("tools/list", tools.error());
    std::cerr << "[conformance-client] listed "
              << member(tools.value(), "tools").toVector().size() << " tools" << std::endl;
    return 0;
  }

  // --- modern stateless discovery ------------------------------------------
  if (scenario == "request-metadata") {
    Session s(url, /*legacy=*/false, protocolVersion);
    auto d = s.client.discover();
    if (!d.has_value()) return bail("server/discover", d.error());
    return 0;
  }

  // --- SEP-2322: client-side MRTR behaviour --------------------------------
  // Every listed tool is called; the ones that answer input_required are
  // fulfilled through callToolWithInput, which echoes requestState verbatim on
  // a fresh JSON-RPC id and never carries another tool's state.
  if (scenario == "sep-2322-client-request-state") {
    Session s(url, /*legacy=*/false, protocolVersion);
    auto tools = s.client.listTools();
    if (!tools.has_value()) return bail("tools/list", tools.error());
    const Json list = member(tools.value(), "tools");
    for (const auto& tool : list.toVector()) {
      const Json name = member(tool, "name");
      if (!name.isString()) continue;
      const std::string toolName(name.toString());
      auto result = s.client.callToolWithInput(
          toolName, buildToolArguments(tool), [](const Json& requests) -> McpResult<Json> {
            Json responses = Json(R"({})");
            for (const auto& key : requests.getAllKeys().toVector()) {
              Json answer;
              answer.add("action", Json::str("accept"));
              Json content = Json(R"({})");
              content.add("confirmed", Json(true));
              answer.add("content", std::move(content));
              responses.add(key.toString(), std::move(answer));
            }
            return Ok(std::move(responses));
          });
      if (!result.has_value()) return bail("MRTR " + toolName, result.error());
    }
    return 0;
  }

  // --- tools/call ------------------------------------------------------------
  if (scenario == "tools_call" || scenario == "http-standard-headers" ||
      scenario == "http-custom-headers" || scenario == "http-invalid-tool-headers") {
    Session s(url, legacy, protocolVersion);
    auto r = performToolCalls(s.client, context);
    if (!r.has_value()) return bail("tools/call", r.error());
    return 0;
  }

  // --- SEP-1613: round-trip the focal tool's inputSchema through the echo ---
  if (scenario == "json-schema-2020-12-preservation") {
    Session s(url, legacy, protocolVersion);
    auto tools = s.client.listTools();
    if (!tools.has_value()) return bail("tools/list", tools.error());
    const Json list = member(tools.value(), "tools");
    // Find the focal tool by NAME - member() is an object-only lookup and
    // returns Null for array children.
    Json focal = Json(nullptr);
    for (const auto& tool : list.toVector()) {
      if (member(tool, "name").toString() == "json_schema_2020_12_tool") {
        focal = tool;
        break;
      }
    }
    if (focal.isNull()) return bail("tools/list", McpError{McpErrorCode::InvalidParams,
                                                           "json_schema_2020_12_tool not listed",
                                                           Json(nullptr)});
    Json args = Json(R"({})");
    args.add("schema", member(focal, "inputSchema"));
    auto r = s.client.callTool("json_schema_echo", args);
    if (!r.has_value()) return bail("json_schema_echo", r.error());
    const Json isError = member(r.value(), "isError");
    return isError.isTrue() ? 1 : 0;
  }

  // --- SEP-1034: the client applies elicitation defaults ---------------------
  if (scenario == "elicitation-sep1034-client-defaults") {
    Session s(url, legacy, protocolVersion);
    // Legacy: the server elicits with a server-initiated request.
    s.client.onServerRequest([](const std::string& method, const Json& params,
                                const RequestId&) -> McpResult<Json> {
      if (isElicitation(method)) return Ok(elicitationDefaults(params));
      return Err(McpErrorCode::MethodNotFound, "unsupported server request: " + method);
    });
    auto tools = s.client.listTools();
    if (!tools.has_value()) return bail("tools/list", tools.error());
    const Json list = member(tools.value(), "tools");
    for (const auto& tool : list.toVector()) {
      const Json name = member(tool, "name");
      const std::string n = name.isString() ? std::string(name.toString()) : std::string();
      if (n != "test_client_elicitation_defaults" && n != "test_elicitation_sep1034_defaults") {
        continue;
      }
      // Modern servers run MRTR: auto-fulfil input_required with defaults too.
      auto r = s.client.callToolWithInput(
          n, Json(R"({})"),
          [](const Json& requests) -> McpResult<Json> {
            const Json elicit = member(requests, "elicit");
            if (elicit.isNull()) return Err(McpErrorCode::InvalidParams, "no elicitation request");
            return Ok(elicitationDefaults(member(elicit, "params")));
          });
      if (!r.has_value()) return bail("elicitation tool call", r.error());
      std::cerr << "[conformance-client] elicitation tool '" << n << "' completed" << std::endl;
    }
    return 0;
  }

  // --- SSE reconnection ------------------------------------------------------
  if (scenario == "sse-retry") {
    Session s(url, legacy, protocolVersion);
    auto tools = s.client.listTools();
    if (!tools.has_value()) return bail("tools/list", tools.error());
    const Json list = member(tools.value(), "tools");
    for (const auto& tool : list.toVector()) {
      if (member(tool, "name").toString() == "test_reconnection") {
        auto r = s.client.callTool("test_reconnection", Json(R"({})"));
        if (!r.has_value()) return bail("test_reconnection", r.error());
        return 0;
      }
    }
    return 0;  // tool not listed: nothing to exercise
  }

  // auth/*: the full OAuth 2.1 + CIMD client flow (mcp::oauth).
  if (scenario.rfind("auth/", 0) == 0) return runAuthScenario(scenario, url, context, protocolVersion);

  // Unknown scenario.
  std::cerr << "[conformance-client] scenario not implemented: " << scenario << std::endl;
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string scenario = [] {
    const char* s = std::getenv("MCP_CONFORMANCE_SCENARIO");
    return s != nullptr ? std::string(s) : std::string("initialize");
  }();
  const std::string protocolVersion = [] {
    const char* s = std::getenv("MCP_CONFORMANCE_PROTOCOL_VERSION");
    return s != nullptr ? std::string(s) : std::string(kDefaultProtocolVersion);
  }();
  const std::string url = argc > 1 ? argv[argc - 1] : "http://127.0.0.1:8001/mcp";
  const Json context = contextJson();

  std::cerr << "[conformance-client] scenario '" << scenario << "' against " << url
            << " (protocol " << protocolVersion << ")" << std::endl;


  // Safety net: the harness kills clients at 30s, which has been observed to
  // wedge it. Exit on our own before that. Some scenario servers intentionally
  // misbehave, so a blocking call can hang - run the scenario on its own thread
  // and _Exit if it overstays.
  std::atomic<int> exitCode{1};
  std::atomic<bool> done{false};
  std::thread runner([&] {
    exitCode.store(runScenario(scenario, parseUrl(url), context, protocolVersion));
    done.store(true);
  });
  for (int i = 0; i < 250; ++i) {  // 25s budget
    if (done.load()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (!done.load()) {
    std::cerr << "[conformance-client] scenario '" << scenario << "' timed out" << std::endl;
    std::_Exit(3);  // do not run destructors over a live thread
  }
  runner.join();
  return exitCode.load();
}

#else  // !MCP_HAS_HTTP

#include <iostream>
int main() {
  std::cerr << "conformance_client requires the HTTP transport (MCP_HAS_HTTP)" << std::endl;
  return 1;
}

#endif  // MCP_HAS_HTTP
