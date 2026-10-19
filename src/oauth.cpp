// OAuth 2.1 + CIMD client for the MCP authorization model. See oauth.hpp.

#include <mcp/oauth.hpp>

#include "oauth_util.h"

#if defined(MCP_HAS_HTTP)

#include <mcp/types.hpp>  // mcp::member (direct-member lookup)

#include <httplib.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
// <string> / <vector> come in through the header.

namespace mcp {
namespace oauth {

using namespace oauth_detail;  // base64Url/pkceChallenge/randomToken/secureRandomBytes

namespace {

// ---- URL handling -----------------------------------------------------------

struct ParsedUrl {
  std::string scheme;  // http | https
  std::string host;
  int port = 0;        // 0 = default for the scheme
  std::string path = "/";
  std::string query;
  bool valid = false;
};

std::string lowerAscii(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

int defaultPort(std::string_view scheme) { return scheme == "https" ? 443 : 80; }

ParsedUrl parseUrl(std::string_view url) {
  ParsedUrl out;
  const size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string_view::npos) return out;
  const std::string scheme = lowerAscii(url.substr(0, schemeEnd));
  if (scheme != "http" && scheme != "https") return out;
  out.scheme = scheme;
  std::string_view rest = url.substr(schemeEnd + 3);
  const size_t pathStart = rest.find_first_of("/?#");
  std::string_view authority = rest.substr(0, pathStart);
  if (authority.empty()) return out;
  const size_t at = authority.rfind('@');
  if (at != std::string_view::npos) authority = authority.substr(at + 1);  // drop userinfo
  const size_t colon = authority.rfind(':');
  if (colon != std::string_view::npos) {
    const std::string portText(authority.substr(colon + 1));
    if (portText.empty() ||
        portText.find_first_not_of("0123456789") != std::string::npos) {
      return out;
    }
    // Bounds matter: an out-of-range port would flow into the httplib origin
    // string and either throw or silently produce an unusable client.
    const long port = std::strtol(portText.c_str(), nullptr, 10);
    if (port < 1 || port > 65535) return out;
    out.port = static_cast<int>(port);
    authority = authority.substr(0, colon);
  }
  if (authority.empty()) return out;
  out.host = std::string(authority);
  if (pathStart != std::string_view::npos) {
    std::string_view tail = rest.substr(pathStart);
    const size_t hash = tail.find('#');
    if (hash != std::string_view::npos) tail = tail.substr(0, hash);
    const size_t qmark = tail.find('?');
    if (qmark != std::string_view::npos) {
      out.query = std::string(tail.substr(qmark + 1));
      tail = tail.substr(0, qmark);
    }
    if (!tail.empty()) out.path = std::string(tail);
  }
  if (out.port == 0) out.port = defaultPort(out.scheme);
  out.valid = true;
  return out;
}

std::string urlOrigin(const ParsedUrl& url) {
  std::string out = url.scheme + "://" + url.host;
  const int def = defaultPort(url.scheme);
  if (url.port != 0 && url.port != def) out += ":" + std::to_string(url.port);
  return out;
}

std::string percentEncode(std::string_view text) {
  static const char* kHex = "0123456789ABCDEF";
  std::string out;
  out.reserve(text.size() * 3);
  for (const char ch : text) {
    const auto u = static_cast<unsigned char>(ch);
    if ((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') ||
        u == '-' || u == '.' || u == '_' || u == '~') {
      out.push_back(static_cast<char>(u));
    } else {
      out.push_back('%');
      out.push_back(kHex[u >> 4]);
      out.push_back(kHex[u & 0xF]);
    }
  }
  return out;
}

std::string formEncode(const std::vector<std::pair<std::string, std::string>>& fields) {
  std::string out;
  for (const auto& [key, value] : fields) {
    if (!out.empty()) out.push_back('&');
    out += percentEncode(key);
    out.push_back('=');
    out += percentEncode(value);
  }
  return out;
}

std::string percentDecode(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size()) {
      const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      const int hi = hex(text[i + 1]);
      const int lo = hex(text[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out.push_back(text[i] == '+' ? ' ' : text[i]);
  }
  return out;
}

std::multimap<std::string, std::string> parseQuery(std::string_view query) {
  std::multimap<std::string, std::string> out;
  size_t pos = 0;
  while (pos <= query.size()) {
    const size_t amp = query.find('&', pos);
    std::string_view pair =
        query.substr(pos, amp == std::string_view::npos ? std::string_view::npos : amp - pos);
    if (!pair.empty()) {
      const size_t eq = pair.find('=');
      if (eq == std::string_view::npos) {
        out.emplace(percentDecode(pair), std::string());
      } else {
        out.emplace(percentDecode(pair.substr(0, eq)), percentDecode(pair.substr(eq + 1)));
      }
    }
    if (amp == std::string_view::npos) break;
    pos = amp + 1;
  }
  return out;
}

// Cryptographically secure random bytes. The PKCE verifier and the state
// parameter are security tokens - RFC 7636 requires the verifier to come from
// a CSPRNG, and std::random_device provides no such guarantee (its quality is
// implementation-defined). Windows resolves RtlGenRandom dynamically (no link
// dependency); POSIX reads /dev/urandom; std::random_device is the last resort.
// ---- HTTP -------------------------------------------------------------------

struct HttpResponse {
  int status = 0;
  std::string body;
  std::multimap<std::string, std::string> headers;
  bool transportOk = false;
  std::string error;

  std::string header(const std::string& name) const {
    const std::string wanted = lowerAscii(name);
    for (const auto& [key, value] : headers) {
      if (lowerAscii(key) == wanted) return value;
    }
    return std::string();
  }
};

// Loopback hosts are the one exception to the https rule: the conformance
// fixture and local development authorization servers are plain http.
bool isLoopbackHost(const std::string& host) {
  return host == "127.0.0.1" || host == "localhost" || host == "[::1]" || host == "::1";
}

// Returns null when the URL cannot yield a usable client: a port outside
// 1..65535, or httplib refusing the origin. The https constructor THROWS when
// the build has no TLS backend, and requests on an invalid client would
// dereference null internals - oauth.hpp promises "nothing throws", so both
// are caught here and surfaced as a transport error instead.
std::unique_ptr<httplib::Client> makeClient(const ParsedUrl& url) {
  if (url.port <= 0 || url.port > 65535) return nullptr;
  std::string origin = url.scheme + "://" + url.host + ":" + std::to_string(url.port);
  std::unique_ptr<httplib::Client> client;
  try {
    client = std::make_unique<httplib::Client>(origin);
  } catch (...) {  // https without a TLS backend
    return nullptr;
  }
  if (!client->is_valid()) return nullptr;
  client->set_connection_timeout(5);
  client->set_read_timeout(10, 0);
  client->set_follow_location(false);
  return client;
}

std::string pathWithQuery(const ParsedUrl& url) {
  std::string out = url.path.empty() ? "/" : url.path;
  if (!url.query.empty()) out += "?" + url.query;
  return out;
}

HttpResponse sendRequest(const std::string& method, const std::string& url,
                         const httplib::Headers& headers, const std::string& body,
                         const std::string& contentType) {
  HttpResponse out;
  const ParsedUrl parsed = parseUrl(url);
  if (!parsed.valid) {
    out.error = "invalid URL: " + url;
    return out;
  }
  // The token endpoint carries the authorization code, the client secret and
  // the PKCE verifier: refuse plain http everywhere except loopback.
  if (parsed.scheme != "https" && !isLoopbackHost(parsed.host)) {
    out.error = "refusing to send OAuth credentials over plain http to '" + parsed.host +
                "' (use https or a loopback address)";
    return out;
  }
  auto client = makeClient(parsed);
  if (client == nullptr) {
    out.error = "cannot create an HTTP client for " + url;
    return out;
  }
  httplib::Result result;
  if (method == "GET") {
    result = client->Get(pathWithQuery(parsed).c_str(), headers);
  } else {
    result = client->Post(pathWithQuery(parsed).c_str(), headers, body, contentType.c_str());
  }
  if (!result) {
    out.error = "HTTP request failed: " + httplib::to_string(result.error());
    return out;
  }
  out.transportOk = true;
  out.status = result->status;
  out.body = result->body;
  for (const auto& [key, value] : result->headers) out.headers.emplace(key, value);
  return out;
}

HttpResponse httpGet(const std::string& url, const httplib::Headers& headers = {}) {
  return sendRequest("GET", url, headers, std::string(), std::string());
}

HttpResponse httpPostForm(const std::string& url, const std::string& body,
                          const httplib::Headers& headers) {
  return sendRequest("POST", url, headers, body, "application/x-www-form-urlencoded");
}

HttpResponse httpPostJson(const std::string& url, const std::string& body,
                          const httplib::Headers& headers) {
  return sendRequest("POST", url, headers, body, "application/json");
}

// ---- JSON helpers -----------------------------------------------------------

McpResult<Json> parseJsonBody(const HttpResponse& response, const std::string& what) {
  if (!response.transportOk) {
    return Err(McpErrorCode::InternalError, what + ": " + response.error);
  }
  if (response.status < 200 || response.status >= 300) {
    return Err(McpErrorCode::InternalError,
               what + ": HTTP " + std::to_string(response.status));
  }
  std::string error;
  Json doc = Json::ParseJson(response.body, error);
  if (!error.empty() || !doc.isObject()) {
    return Err(McpErrorCode::InternalError, what + ": invalid JSON body");
  }
  return Ok(std::move(doc));
}

std::string stringMember(const Json& object, const char* key) {
  const Json value = member(object, key);
  return value.isString() ? std::string(value.toString()) : std::string();
}

std::vector<std::string> stringArrayMember(const Json& object, const char* key) {
  std::vector<std::string> out;
  const Json value = member(object, key);
  if (!value.isArray()) return out;
  for (const auto& item : value.toVector()) {
    if (item.isString()) out.push_back(std::string(item.toString()));
  }
  return out;
}

// Well-known candidate paths, in the order the reference client tries them.
// `resource` is the RFC 8615 suffix, e.g. "oauth-protected-resource".
std::vector<std::string> wellKnownPaths(const std::string& basePath,
                                        const std::string& resource) {
  std::string trimmed = basePath;
  while (!trimmed.empty() && trimmed.front() == '/') trimmed.erase(trimmed.begin());
  while (!trimmed.empty() && trimmed.back() == '/') trimmed.pop_back();

  std::vector<std::string> candidates;
  const auto push = [&](const std::string& candidate) {
    if (std::find(candidates.begin(), candidates.end(), candidate) == candidates.end()) {
      candidates.push_back(candidate);
    }
  };
  const std::string canonical = "/.well-known/" + resource;
  if (trimmed.empty()) {
    push(canonical);
    return candidates;
  }
  push(canonical + "/" + trimmed);                 // RFC 8414 path insertion
  push("/" + trimmed + canonical);                 // issuer-prefixed form
  push(canonical);                                 // canonical, last resort
  return candidates;
}

// Authorization-server metadata candidates. Unlike the protected-resource
// case, RFC 8414 and OIDC discovery are alternate spellings of the same
// document, so both are tried (in the RFC 8414 order, OIDC appended).
std::vector<std::string> authorizationServerMetadataPaths(const std::string& basePath) {
  std::string trimmed = basePath;
  while (!trimmed.empty() && trimmed.front() == '/') trimmed.erase(trimmed.begin());
  while (!trimmed.empty() && trimmed.back() == '/') trimmed.pop_back();

  std::vector<std::string> candidates;
  const auto push = [&](const std::string& candidate) {
    if (std::find(candidates.begin(), candidates.end(), candidate) == candidates.end()) {
      candidates.push_back(candidate);
    }
  };
  if (trimmed.empty()) {
    push("/.well-known/oauth-authorization-server");
    push("/.well-known/openid-configuration");
    return candidates;
  }
  push("/.well-known/oauth-authorization-server/" + trimmed);  // RFC 8414 path insertion
  push("/.well-known/openid-configuration/" + trimmed);        // RFC 8414 + OIDC suffix
  push("/" + trimmed + "/.well-known/openid-configuration");   // issuer-prefixed OIDC
  push("/.well-known/oauth-authorization-server");             // canonical, last resort
  return candidates;
}

// RFC 9728 / RFC 8707: the published identifier must reference the server URL:
// same scheme/host/port, and equal paths or the server path nested under it.
bool resourceIdentifierValid(const ParsedUrl& expected, const ParsedUrl& actual) {
  if (!actual.valid) return false;
  if (expected.scheme != actual.scheme || expected.host != actual.host ||
      expected.port != actual.port) {
    return false;
  }
  std::string expectedPath = expected.path.empty() ? "/" : expected.path;
  std::string actualPath = actual.path.empty() ? "/" : actual.path;
  if (expectedPath == actualPath) return true;
  if (expectedPath.size() <= actualPath.size()) return false;
  if (expectedPath.compare(0, actualPath.size(), actualPath) != 0) return false;
  return actualPath.back() == '/' || expectedPath[actualPath.size()] == '/';
}

}  // namespace

struct OAuthClient::Impl {
  Config config;
  ResourceMetadata resource;
  ServerMetadata server;
  ClientIdentity identity;
  Tokens tokens;
  std::vector<std::string> challengeScopes;
  std::string pkceVerifier;
  std::string state;
  std::string authorizationEndpoint;  // captured for the redirect check
  // Set when there is no protected-resource metadata document at all: the
  // pre-2025-06-18 model, where the MCP server URL is the resource and its
  // origin is the authorization server.
  bool legacyDiscovery = false;

  explicit Impl(Config cfg) : config(std::move(cfg)) {}

  // Attempts one metadata document; only a 2xx with a JSON object counts.
  McpResult<std::optional<Json>> fetchJson(const std::string& url) {
    const HttpResponse response = httpGet(url);
    if (!response.transportOk) {
      return Err(McpErrorCode::InternalError, "metadata request failed: " + response.error);
    }
    if (response.status < 200 || response.status >= 300) {
      return Ok(std::optional<Json>(std::nullopt));
    }
    std::string error;
    Json doc = Json::ParseJson(response.body, error);
    if (!error.empty() || !doc.isObject()) {
      return Ok(std::optional<Json>(std::nullopt));
    }
    return Ok(std::optional<Json>(std::move(doc)));
  }

  // nullopt = no document at this location (try the next candidate); an error =
  // a document that exists but is unusable, which must not be papered over.
  McpResult<std::optional<ResourceMetadata>> fetchResourceMetadata(
      const std::string& url) const {
    const HttpResponse response = httpGet(url);
    if (!response.transportOk) {
      return Err(McpErrorCode::InternalError,
                 "protected resource metadata request failed: " + response.error);
    }
    if (response.status < 200 || response.status >= 300) {
      return Ok(std::optional<ResourceMetadata>(std::nullopt));
    }
    std::string error;
    Json doc = Json::ParseJson(response.body, error);
    if (!error.empty() || !doc.isObject()) {
      return Ok(std::optional<ResourceMetadata>(std::nullopt));
    }
    ResourceMetadata metadata;
    metadata.metadataUrl = url;
    metadata.resource = stringMember(doc, "resource");
    metadata.authorizationServers = stringArrayMember(doc, "authorization_servers");
    metadata.scopesSupported = stringArrayMember(doc, "scopes_supported");
    if (metadata.resource.empty()) {
      return Err(McpErrorCode::InvalidParams,
                 "protected resource metadata is missing the resource field");
    }
    const ParsedUrl expected = parseUrl(config.resourceUrl);
    const ParsedUrl actual = parseUrl(metadata.resource);
    if (!resourceIdentifierValid(expected, actual)) {
      return Err(McpErrorCode::InvalidParams,
                 "protected resource metadata resource '" + metadata.resource +
                     "' does not match the server URL");
    }
    return Ok(std::optional<ResourceMetadata>(std::move(metadata)));
  }

  McpResult<void> discoverResourceMetadata(const std::string& challenge) {
    // 1. The challenge's `resource_metadata` pointer wins when present.
    if (!challenge.empty()) {
      const size_t at = challenge.find("resource_metadata=\"");
      if (at != std::string::npos) {
        const size_t start = at + std::string("resource_metadata=\"").size();
        const size_t end = challenge.find('"', start);
        if (end != std::string::npos) {
          auto metadata = fetchResourceMetadata(challenge.substr(start, end - start));
          if (!metadata.has_value()) return Err(metadata.error());
          if (metadata->has_value()) {
            resource = std::move(**metadata);
            return Ok();
          }
        }
      }
    }
    // 2. Otherwise the well-known candidates derived from the server URL.
    const ParsedUrl base = parseUrl(config.resourceUrl);
    if (!base.valid) {
      return Err(McpErrorCode::InvalidParams, "invalid resource URL: " + config.resourceUrl);
    }
    std::optional<McpError> fatal;
    for (const auto& candidate : wellKnownPaths(base.path, "oauth-protected-resource")) {
      ParsedUrl target = base;
      target.path = candidate;
      target.query.clear();
      auto metadata = fetchResourceMetadata(urlOrigin(target) + candidate);
      if (!metadata.has_value()) {
        if (!fatal.has_value()) fatal = metadata.error();
        continue;
      }
      if (metadata->has_value()) {
        resource = std::move(**metadata);
        return Ok();
      }
    }
    if (fatal.has_value()) return Err(*fatal);
    // Legacy (2025-03-26) servers publish no protected-resource metadata: the
    // MCP server URL is the resource and its origin is the authorization
    // server (MCP 2025-03-26 "server metadata discovery").
    const ParsedUrl serverUrl = parseUrl(config.resourceUrl);
    if (!serverUrl.valid) {
      return Err(McpErrorCode::InvalidParams, "invalid resource URL: " + config.resourceUrl);
    }
    resource = ResourceMetadata();
    resource.resource = config.resourceUrl;
    resource.authorizationServers = {urlOrigin(serverUrl)};
    legacyDiscovery = true;
    return Ok();
  }

  // Resolves AS metadata for one issuer identifier. Fails on a metadata
  // document whose issuer differs from the identifier it was discovered from.
  McpResult<std::optional<ServerMetadata>> fetchServerMetadata(const std::string& issuer) {
    const ParsedUrl base = parseUrl(issuer);
    if (!base.valid) {
      return Err(McpErrorCode::InvalidParams, "invalid authorization server: " + issuer);
    }
    for (const auto& candidate : authorizationServerMetadataPaths(base.path)) {
      ParsedUrl target = base;
      target.path = candidate;
      target.query.clear();
      auto doc = fetchJson(urlOrigin(target) + candidate);
      if (!doc.has_value()) return Err(doc.error());
      if (!doc->has_value()) continue;
      ServerMetadata metadata;
      metadata.issuer = stringMember(**doc, "issuer");
      metadata.authorizationEndpoint = stringMember(**doc, "authorization_endpoint");
      metadata.tokenEndpoint = stringMember(**doc, "token_endpoint");
      metadata.registrationEndpoint = stringMember(**doc, "registration_endpoint");
      metadata.scopesSupported = stringArrayMember(**doc, "scopes_supported");
      metadata.tokenEndpointAuthMethods =
          stringArrayMember(**doc, "token_endpoint_auth_methods_supported");
      metadata.grantTypesSupported = stringArrayMember(**doc, "grant_types_supported");
      metadata.codeChallengeMethods =
          stringArrayMember(**doc, "code_challenge_methods_supported");
      metadata.clientIdMetadataDocumentSupported =
          member(**doc, "client_id_metadata_document_supported").isTrue();
      const Json issParameter = member(**doc, "authorization_response_iss_parameter_supported");
      if (issParameter.isTrue() || issParameter.isFalse()) {
        metadata.issuerParameterAdvertised = true;
        metadata.issuerParameterSupported = issParameter.isTrue();
      }
      // RFC 8414 3.3: the metadata issuer MUST match the identifier the
      // well-known URL was derived from. Reject instead of using it.
      if (metadata.issuer != issuer) {
        return Err(McpErrorCode::InvalidParams,
                   "authorization server metadata issuer '" + metadata.issuer +
                       "' does not match '" + issuer + "'");
      }
      if (metadata.authorizationEndpoint.empty() || metadata.tokenEndpoint.empty()) {
        return Err(McpErrorCode::InternalError,
                   "authorization server metadata is missing its endpoints");
      }
      // PKCE downgrade protection (OAuth 2.1): the AS must advertise S256.
      // A missing list, or one without S256, means we cannot confirm the
      // token endpoint accepts an S256 challenge - refusing beats silently
      // downgrading to plain PKCE.
      const bool s256 = std::any_of(metadata.codeChallengeMethods.begin(),
                                    metadata.codeChallengeMethods.end(),
                                    [](const std::string& m) { return m == "S256"; });
      if (!s256) {
        return Err(McpErrorCode::InvalidParams,
                   "authorization server does not advertise PKCE S256 "
                   "(code_challenge_methods_supported)");
      }
      return Ok(std::optional<ServerMetadata>(std::move(metadata)));
    }
    return Ok(std::optional<ServerMetadata>(std::nullopt));
  }

  std::string preferredAuthMethod() const {
    if (!identity.tokenEndpointAuthMethod.empty()) return identity.tokenEndpointAuthMethod;
    if (!server.tokenEndpointAuthMethods.empty()) return server.tokenEndpointAuthMethods.front();
    return "none";
  }

  McpResult<void> registerClient() {
    if (!config.preregisteredClientId.empty()) {
      identity.clientId = config.preregisteredClientId;
      identity.clientSecret = config.preregisteredClientSecret;
      identity.tokenEndpointAuthMethod = config.preregisteredAuthMethod;
      if (identity.tokenEndpointAuthMethod.empty()) {
        identity.tokenEndpointAuthMethod =
            !server.tokenEndpointAuthMethods.empty() ? server.tokenEndpointAuthMethods.front()
                                                     : "none";
      }
      return Ok();
    }
    if (server.clientIdMetadataDocumentSupported && !config.clientMetadataUrl.empty()) {
      // SEP-991 / CIMD: the metadata document URL *is* the client identifier.
      identity.clientId = config.clientMetadataUrl;
      identity.tokenEndpointAuthMethod = "none";
      return Ok();
    }
    if (server.registrationEndpoint.empty()) {
      return Err(McpErrorCode::InvalidParams,
                 "no client registration endpoint and no pre-registered client");
    }
    const std::string authMethod =
        !server.tokenEndpointAuthMethods.empty() ? server.tokenEndpointAuthMethods.front()
                                                 : "none";
    Json request = Json(R"({})");
    request.add("client_name", Json::str(config.clientName));
    Json redirectUris = Json("[]");
    redirectUris.push_back(Json::str(config.redirectUri));
    request.add("redirect_uris", std::move(redirectUris));
    request.add("application_type", Json::str(config.applicationType));
    Json grantTypes = Json("[]");
    grantTypes.push_back(Json::str("authorization_code"));
    grantTypes.push_back(Json::str("refresh_token"));
    request.add("grant_types", std::move(grantTypes));
    Json responseTypes = Json("[]");
    responseTypes.push_back(Json::str("code"));
    request.add("response_types", std::move(responseTypes));
    request.add("token_endpoint_auth_method", Json::str(authMethod));

    const HttpResponse response = httpPostJson(server.registrationEndpoint, request.toString(),
                                               {{"Accept", "application/json"}});
    auto doc = parseJsonBody(response, "client registration");
    if (!doc.has_value()) return Err(doc.error());
    identity.clientId = stringMember(*doc, "client_id");
    identity.clientSecret = stringMember(*doc, "client_secret");
    identity.tokenEndpointAuthMethod = stringMember(*doc, "token_endpoint_auth_method");
    identity.dynamic = true;
    if (identity.clientId.empty()) {
      return Err(McpErrorCode::InternalError, "client registration returned no client_id");
    }
    if (identity.tokenEndpointAuthMethod.empty()) identity.tokenEndpointAuthMethod = authMethod;
    return Ok();
  }

  McpResult<void> discoverServerMetadata() {
    if (resource.authorizationServers.empty()) {
      return Err(McpErrorCode::InvalidParams,
                 "protected resource metadata listed no authorization server");
    }
    std::optional<McpError> fatal;
    for (const auto& issuer : resource.authorizationServers) {
      auto metadata = fetchServerMetadata(issuer);
      if (!metadata.has_value()) {
        if (!fatal.has_value()) fatal = metadata.error();
        continue;
      }
      if (!metadata->has_value()) continue;
      server = std::move(**metadata);
      identity = ClientIdentity();
      return Ok();
    }
    if (fatal.has_value()) return Err(*fatal);
    // Fallbacks for servers without metadata discovery (MCP 2025-03-26): the
    // standard OAuth endpoints relative to the authorization server origin.
    if (legacyDiscovery && resource.authorizationServers.size() == 1) {
      const ParsedUrl issuer = parseUrl(resource.authorizationServers[0]);
      if (issuer.valid) {
        server = ServerMetadata();
        server.issuer = resource.authorizationServers[0];
        server.authorizationEndpoint = urlOrigin(issuer) + "/authorize";
        server.tokenEndpoint = urlOrigin(issuer) + "/token";
        server.registrationEndpoint = urlOrigin(issuer) + "/register";
        identity = ClientIdentity();
        return Ok();
      }
    }
    return Err(McpErrorCode::InvalidParams,
               "no usable authorization server metadata for the published issuer(s)");
  }

  McpResult<std::string> authorize() {
    const std::string resourceParam = resource.resource;
    const std::string verifier = randomToken(64);
    pkceVerifier = verifier;
    state = randomToken(32);
    const std::vector<std::string> scopes = pendingScopes;
    std::string scope;
    for (const auto& s : scopes) {
      if (s.empty()) continue;
      if (!scope.empty()) scope.push_back(' ');
      scope += s;
    }
    std::string url = server.authorizationEndpoint;
    url += (url.find('?') == std::string::npos) ? "?" : "&";
    url += "response_type=code";
    url += "&client_id=" + percentEncode(identity.clientId);
    url += "&redirect_uri=" + percentEncode(config.redirectUri);
    url += "&state=" + percentEncode(state);
    url += "&code_challenge=" + percentEncode(pkceChallenge(verifier));
    url += "&code_challenge_method=S256";
    if (!scope.empty()) url += "&scope=" + percentEncode(scope);
    if (!resourceParam.empty()) url += "&resource=" + percentEncode(resourceParam);
    authorizationEndpoint = server.authorizationEndpoint;
    pendingScopes.clear();
    pendingScopes.shrink_to_fit();
    return Ok(url);
  }

  std::vector<std::string> pendingScopes;
};

OAuthClient::OAuthClient(Config config) : impl_(std::make_unique<Impl>(std::move(config))) {}
OAuthClient::~OAuthClient() = default;
OAuthClient::OAuthClient(OAuthClient&&) noexcept = default;
OAuthClient& OAuthClient::operator=(OAuthClient&&) noexcept = default;

McpResult<void> OAuthClient::discover(const std::string& challenge) {
  impl_->challengeScopes.clear();
  if (!challenge.empty()) {
    const size_t at = challenge.find("scope=\"");
    if (at != std::string::npos) {
      const size_t start = at + std::string("scope=\"").size();
      const size_t end = challenge.find('"', start);
      if (end != std::string::npos) {
        std::istringstream stream(challenge.substr(start, end - start));
        std::string scope;
        while (stream >> scope) impl_->challengeScopes.push_back(scope);
      }
    }
  }
  auto resolved = impl_->discoverResourceMetadata(challenge);
  if (!resolved.has_value()) return resolved;
  auto server = impl_->discoverServerMetadata();
  if (!server.has_value()) return server;
  return impl_->registerClient();
}

McpResult<bool> OAuthClient::refreshAuthorizationServer() {
  auto metadata = impl_->fetchResourceMetadata(impl_->resource.metadataUrl);
  if (!metadata.has_value()) return Err(metadata.error());
  if (!metadata->has_value()) {
    return Err(McpErrorCode::InternalError, "protected resource metadata disappeared");
  }
  // SEP-2352: only re-discover when the advertised authorization server moved.
  if ((*metadata)->authorizationServers == impl_->resource.authorizationServers) {
    impl_->resource = std::move(**metadata);
    return Ok(false);
  }
  impl_->resource = std::move(**metadata);
  auto server = impl_->discoverServerMetadata();
  if (!server.has_value()) return Err(server.error());
  auto registered = impl_->registerClient();
  if (!registered.has_value()) return Err(registered.error());
  return Ok(true);
}

const ResourceMetadata& OAuthClient::resourceMetadata() const { return impl_->resource; }
const ServerMetadata& OAuthClient::serverMetadata() const { return impl_->server; }
const ClientIdentity& OAuthClient::client() const { return impl_->identity; }
const Tokens& OAuthClient::tokens() const { return impl_->tokens; }
const std::string& OAuthClient::accessToken() const { return impl_->tokens.accessToken; }
const std::vector<std::string>& OAuthClient::challengeScopes() const {
  return impl_->challengeScopes;
}

std::vector<std::string> OAuthClient::selectScopes(
    const std::vector<std::string>& requested) const {
  // Spec order (MCP "Scope selection strategy"): an explicit request wins,
  // then the challenge's scope, then the resource metadata's scopes_supported,
  // then the authorization server's.
  if (!requested.empty()) return requested;
  if (!impl_->challengeScopes.empty()) return impl_->challengeScopes;
  if (!impl_->resource.scopesSupported.empty()) return impl_->resource.scopesSupported;
  return impl_->server.scopesSupported;
}

McpResult<std::string> OAuthClient::authorizationUrl(const std::vector<std::string>& scopes) {
  impl_->pendingScopes = selectScopes(scopes);
  return impl_->authorize();
}

McpResult<void> OAuthClient::handleRedirect(const std::string& redirectUrl) {
  const ParsedUrl parsed = parseUrl(redirectUrl);
  if (!parsed.valid) {
    return Err(McpErrorCode::InvalidParams, "invalid redirect URL: " + redirectUrl);
  }
  // The redirect must land on the registered redirect URI: an authorization
  // server that redirects elsewhere is either misconfigured or hostile, and
  // consuming that URL would smuggle a foreign authorization response into
  // the token exchange.
  const ParsedUrl registered = parseUrl(impl_->config.redirectUri);
  if (!registered.valid || parsed.scheme != registered.scheme ||
      parsed.host != registered.host ||
      parsed.port != (registered.port != 0 ? registered.port : defaultPort(registered.scheme))) {
    return Err(McpErrorCode::InvalidParams,
               "authorization response redirect does not match the registered redirect URI");
  }
  const auto query = parseQuery(parsed.query);

  const auto first = [&](const std::string& key) -> std::optional<std::string> {
    const auto it = query.find(key);
    if (it == query.end()) return std::nullopt;
    return it->second;
  };

  // RFC 9207 `iss`: always compared when present; required when advertised.
  const std::optional<std::string> iss = first("iss");
  if (iss.has_value()) {
    if (*iss != impl_->server.issuer) {
      return Err(McpErrorCode::InvalidParams,
                 "authorization response iss '" + *iss + "' does not match the issuer '" +
                     impl_->server.issuer + "'");
    }
  } else if (impl_->server.issuerParameterAdvertised &&
             impl_->server.issuerParameterSupported) {
    return Err(McpErrorCode::InvalidParams,
               "authorization response is missing the advertised iss parameter");
  }

  const std::optional<std::string> state = first("state");
  if (!impl_->state.empty() && (!state.has_value() || *state != impl_->state)) {
    return Err(McpErrorCode::InvalidParams, "authorization response state mismatch");
  }
  const std::optional<std::string> code = first("code");
  if (!code.has_value() || code->empty()) {
    const std::optional<std::string> error = first("error");
    return Err(McpErrorCode::InvalidParams,
               "authorization response carried no code" +
                   (error.has_value() ? std::string(" (error: ") + *error + ")" : std::string()));
  }

  std::vector<std::pair<std::string, std::string>> fields = {
      {"grant_type", "authorization_code"},
      {"code", *code},
      {"redirect_uri", impl_->config.redirectUri},
      {"code_verifier", impl_->pkceVerifier},
  };
  if (!impl_->resource.resource.empty()) {
    fields.emplace_back("resource", impl_->resource.resource);
  }
  httplib::Headers headers = {{"Accept", "application/json"}};
  const std::string method = impl_->preferredAuthMethod();
  if (method == "client_secret_basic") {
    const std::string pair = impl_->identity.clientId + ":" + impl_->identity.clientSecret;
    headers.emplace("Authorization", "Basic " + detail::base64Encode(pair));
  } else if (method == "client_secret_post") {
    fields.emplace_back("client_id", impl_->identity.clientId);
    fields.emplace_back("client_secret", impl_->identity.clientSecret);
  } else {
    fields.emplace_back("client_id", impl_->identity.clientId);
  }
  const HttpResponse response =
      httpPostForm(impl_->server.tokenEndpoint, formEncode(fields), headers);
  auto doc = parseJsonBody(response, "token request");
  if (!doc.has_value()) return Err(doc.error());
  Tokens tokens;
  tokens.accessToken = stringMember(*doc, "access_token");
  tokens.refreshToken = stringMember(*doc, "refresh_token");
  tokens.tokenType = stringMember(*doc, "token_type");
  if (tokens.tokenType.empty()) tokens.tokenType = "Bearer";
  const Json expires = member(*doc, "expires_in");
  if (expires.isNumber()) tokens.expiresIn = expires.toInt64();
  tokens.scopes = stringArrayMember(*doc, "scope");
  tokens.issuer = impl_->server.issuer;
  if (tokens.accessToken.empty()) {
    return Err(McpErrorCode::InternalError, "token response carried no access_token");
  }
  impl_->tokens = std::move(tokens);
  return Ok();
}

McpResult<void> OAuthClient::authorizeAndExchange(const std::string& authorizationUrl) {
  const HttpResponse response = httpGet(authorizationUrl, {{"Accept", "text/html"}});
  if (!response.transportOk) {
    return Err(McpErrorCode::InternalError,
               "authorization request failed: " + response.error);
  }
  const std::string location = response.header("Location");
  if (location.empty()) {
    return Err(McpErrorCode::InvalidParams,
               "authorization endpoint did not redirect (HTTP " +
                   std::to_string(response.status) + ")");
  }
  return handleRedirect(location);
}

}  // namespace oauth
}  // namespace mcp

#endif  // MCP_HAS_HTTP
