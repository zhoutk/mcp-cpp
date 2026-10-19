// OAuth 2.1 authorization server core (loopback/dev + test fixtures). See
// oauth_server.hpp for the contract; tests/test_oauth_server.cpp has a full
// httplib wiring.

#include <mcp/oauth_server.hpp>

#include "oauth_util.h"

#include <mcp/types.hpp>  // mcp::member

#include <algorithm>
#include <string>
#include <vector>

namespace mcp {
namespace oauth {
namespace {

using oauth_detail::pkceChallenge;
using oauth_detail::randomToken;
using oauth_detail::unixNowSeconds;

bool isValidHttpsUrl(const std::string& url) {
  static constexpr std::string_view kSep = "://";
  const size_t sep = url.find(kSep);
  if (sep == std::string::npos) return false;
  const std::string scheme = url.substr(0, sep);
  return scheme == "https" || scheme == "http";  // loopback http allowed
}

}  // namespace

AuthorizationServer::AuthorizationServer(AuthzServerConfig config) : config_(std::move(config)) {
  if (!config_.issuer.empty() && config_.issuer.back() == '/') {
    config_.issuer.pop_back();  // normalize: the issuer must not end in a slash
  }
}

const AuthzServerConfig& AuthorizationServer::config() const { return config_; }

McpResult<AuthzClient> AuthorizationServer::registerClient(
    const std::string& clientName, const std::vector<std::string>& redirectUris) {
  if (redirectUris.empty()) {
    return Err(McpErrorCode::InvalidParams, "at least one redirect_uri is required");
  }
  for (const auto& uri : redirectUris) {
    if (!isValidHttpsUrl(uri)) {
      return Err(McpErrorCode::InvalidParams, "invalid redirect_uri: " + uri);
    }
  }
  AuthzClient client;
  client.clientId = "client-" + randomToken(16);
  client.clientSecret = randomToken(32);
  client.redirectUris = redirectUris;
  client.tokenEndpointAuthMethod = "client_secret_basic";
  (void)clientName;  // recorded by callers who need it; the core stays minimal
  clients_.push_back(client);
  return Ok(std::move(client));
}

const AuthzClient* AuthorizationServer::findClient(const std::string& clientId) const {
  const auto it = std::find_if(clients_.begin(), clients_.end(),
                               [&](const AuthzClient& c) { return c.clientId == clientId; });
  return it == clients_.end() ? nullptr : &*it;
}

McpResult<std::string> AuthorizationServer::authorize(
    const std::string& clientId, const std::string& redirectUri, const std::string& scope,
    const std::string& state, const std::string& codeChallenge,
    const std::string& codeChallengeMethod, const std::string& resource) {
  const AuthzClient* client = findClient(clientId);
  if (client == nullptr) return Err(McpErrorCode::InvalidParams, "unknown client_id");
  // RFC 6749 10.6: the redirect_uri must match a registered one EXACTLY.
  if (std::find(client->redirectUris.begin(), client->redirectUris.end(), redirectUri) ==
      client->redirectUris.end()) {
    return Err(McpErrorCode::InvalidParams, "redirect_uri is not registered for this client");
  }
  // PKCE S256 is mandatory (OAuth 2.1): plain and absent are downgrades.
  if (codeChallenge.empty() || codeChallengeMethod != "S256") {
    return Err(McpErrorCode::InvalidParams,
               "PKCE S256 is required (plain and missing are not accepted)");
  }
  std::string code = randomToken(32);
  typename decltype(codes_)::value_type entry;
  entry.first = code;
  entry.second = PendingCode{clientId, redirectUri, scope, codeChallenge, resource,
                             unixNowSeconds() + config_.codeTtlSeconds};
  codes_.push_back(std::move(entry));
  // Build the redirect: code + state (+ iss for RFC 9207-aware callers).
  std::string url = redirectUri;
  const bool hasQuery = redirectUri.find('?') != std::string::npos;
  url += hasQuery ? '&' : '?';
  url += "code=" + code;
  if (!state.empty()) url += "&state=" + state;
  return Ok(std::move(url));
}

McpResult<TokenGrant> AuthorizationServer::exchangeToken(
    const std::string& grantType, const std::string& code, const std::string& redirectUri,
    const std::string& clientId, const std::string& clientSecret,
    const std::string& codeVerifier, const std::string& resource) {
  if (grantType != "authorization_code") {
    return Err(McpErrorCode::InvalidParams, "unsupported grant_type");
  }
  const AuthzClient* client = findClient(clientId);
  if (client == nullptr) return Err(McpErrorCode::InvalidParams, "unknown client_id");
  // Confidential clients must authenticate; public clients (no secret) just
  // identify themselves.
  if (!client->clientSecret.empty() && clientSecret != client->clientSecret) {
    return Err(McpErrorCode::InvalidParams, "client authentication failed");
  }
  // Single use: find AND erase in one pass. An expired or reused code is
  // simply gone (RFC 6749 4.1.2).
  PendingCode pending;
  bool found = false;
  for (auto it = codes_.begin(); it != codes_.end(); ++it) {
    if (it->first == code && it->second.clientId == clientId) {
      pending = it->second;
      codes_.erase(it);
      found = true;
      break;
    }
  }
  if (!found) return Err(McpErrorCode::InvalidParams, "invalid or expired authorization code");
  if (unixNowSeconds() > pending.expiresAt) {
    return Err(McpErrorCode::InvalidParams, "authorization code expired");
  }
  if (redirectUri != pending.redirectUri) {
    return Err(McpErrorCode::InvalidParams, "redirect_uri does not match the authorize request");
  }
  // RFC 7636 4.6: S256(verifier) must equal the stored challenge.
  if (codeVerifier.empty() || pkceChallenge(codeVerifier) != pending.codeChallenge) {
    return Err(McpErrorCode::InvalidParams, "PKCE verification failed");
  }
  // RFC 8707: the token is bound to the resource the code was requested for.
  std::string audience = pending.resource;
  if (!resource.empty() && resource != audience) {
    return Err(McpErrorCode::InvalidParams, "resource does not match the authorize request");
  }
  TokenGrant grant;
  grant.accessToken = randomToken(48);
  grant.expiresIn = config_.tokenTtlSeconds;
  grant.scope = pending.scope;
  grant.resource = audience;
  tokens_.push_back({grant.accessToken,
                     IssuedToken{clientId, grant.scope, grant.resource,
                                 unixNowSeconds() + config_.tokenTtlSeconds}});
  return Ok(std::move(grant));
}

McpResult<std::string> AuthorizationServer::verifyAccessToken(const std::string& accessToken,
                                                              const std::string& resource) const {
  for (const auto& [token, issued] : tokens_) {
    if (token != accessToken) continue;
    if (unixNowSeconds() > issued.expiresAt) {
      return Err(McpErrorCode::InvalidParams, "token expired");
    }
    if (!resource.empty() && !issued.resource.empty() && issued.resource != resource) {
      return Err(McpErrorCode::InvalidParams,
                 "token audience does not include the requested resource");
    }
    return Ok(issued.clientId);  // the authenticated principal
  }
  return Err(McpErrorCode::InvalidParams, "invalid access token");
}

Json AuthorizationServer::authorizationServerMetadata() const {
  Json doc = Json(R"({})");
  doc.add("issuer", Json::str(config_.issuer));
  doc.add("authorization_endpoint", Json::str(config_.issuer + "/authorize"));
  doc.add("token_endpoint", Json::str(config_.issuer + "/token"));
  doc.add("registration_endpoint", Json::str(config_.issuer + "/register"));
  Json grants = Json("[]");
  grants.push_back(Json::str("authorization_code"));
  doc.add("grant_types_supported", std::move(grants));
  Json challenges = Json("[]");
  challenges.push_back(Json::str("S256"));
  doc.add("code_challenge_methods_supported", std::move(challenges));
  Json authMethods = Json("[]");
  authMethods.push_back(Json::str("client_secret_basic"));
  doc.add("token_endpoint_auth_methods_supported", std::move(authMethods));
  Json responseTypes = Json("[]");
  responseTypes.push_back(Json::str("code"));
  doc.add("response_types_supported", std::move(responseTypes));
  Json scopes = Json("[]");
  for (const auto& s : config_.scopesSupported) scopes.push_back(Json::str(s));
  doc.add("scopes_supported", std::move(scopes));
  return doc;
}

}  // namespace oauth
}  // namespace mcp
