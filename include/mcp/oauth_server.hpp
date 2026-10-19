#pragma once
// ---------------------------------------------------------------------------
// mcp/oauth_server.hpp - a compact OAuth 2.1 authorization server core.
//
// Companion to mcp::oauth (the client). It is an IN-MEMORY authorization
// server for loopback/development deployments and conformance-style test
// fixtures: Dynamic Client Registration (RFC 7591), the authorization-code
// flow with mandatory PKCE S256 (RFC 7636; plain is refused, per OAuth 2.1),
// RFC 8707 `resource` binding, and the RFC 8414 metadata document.
//
// The core is HTTP-free: handlers return/accept wire-level values (redirect
// URLs, form fields) so any HTTP layer can mount them - see
// tests/test_oauth_server.cpp for a complete httplib wiring. Everything is
// in-memory: a process restart drops clients, codes and tokens.
//
// Nothing here throws: failures are McpResult errors. Not for production
// multi-tenant use - there is no persistence, no rate limiting, no user
// authentication (the caller owns the user step and calls authorize() once
// the user has approved).
// ---------------------------------------------------------------------------

#include <mcp/error.hpp>
#include <mcp/json.hpp>
#include <mcp/types.hpp>

#include <string>
#include <vector>

namespace mcp {
namespace oauth {

struct AuthzServerConfig {
  std::string issuer;                        // e.g. http://127.0.0.1:8123 (no path)
  std::vector<std::string> scopesSupported;  // advertised in metadata
  int64_t codeTtlSeconds = 600;              // RFC 6749: <= 10 min
  int64_t tokenTtlSeconds = 3600;
};

// A registered client (RFC 7591 Dynamic Client Registration response shape).
struct AuthzClient {
  std::string clientId;
  std::string clientSecret;  // empty for public clients
  std::vector<std::string> redirectUris;
  std::string tokenEndpointAuthMethod = "client_secret_basic";
};

struct TokenGrant {
  std::string accessToken;
  std::string tokenType = "Bearer";
  int64_t expiresIn = 0;
  std::string scope;
  std::string resource;  // RFC 8707 audience the token was issued for
};

class AuthorizationServer {
 public:
  explicit AuthorizationServer(AuthzServerConfig config);

  const AuthzServerConfig& config() const;

  // RFC 7591 3.1: Dynamic Client Registration. Confidential clients get a
  // generated secret; an empty redirectUris list is an error (refreshable
  // flows need somewhere to come back to).
  McpResult<AuthzClient> registerClient(const std::string& clientName,
                                        const std::vector<std::string>& redirectUris);
  // Looks a client up (token endpoint auth, redirect validation).
  const AuthzClient* findClient(const std::string& clientId) const;

  // RFC 6749 4.1.1: validates the /authorize request and returns the redirect
  // URL carrying `code` and `state` (the caller points the user agent at it).
  // PKCE with method "S256" is REQUIRED; "plain" or a missing challenge is
  // refused (OAuth 2.1 downgrade protection). The redirect_uri must exactly
  // match one registered for the client.
  McpResult<std::string> authorize(const std::string& clientId,
                                   const std::string& redirectUri, const std::string& scope,
                                   const std::string& state,
                                   const std::string& codeChallenge,
                                   const std::string& codeChallengeMethod,
                                   const std::string& resource);

  // RFC 6749 4.1.3: the token endpoint (authorization_code grant only).
  // Verifies client authentication, single-use of the code, the redirect_uri
  // echo and PKCE S256; consumes the code; returns a bearer grant bound to
  // the resource the code was requested for (RFC 8707).
  McpResult<TokenGrant> exchangeToken(const std::string& grantType, const std::string& code,
                                      const std::string& redirectUri,
                                      const std::string& clientId,
                                      const std::string& clientSecret,
                                      const std::string& codeVerifier,
                                      const std::string& resource);

  // Resource-server side: verifies a bearer token and its RFC 8707 audience
  // (`resource` empty skips the audience check).
  McpResult<std::string> verifyAccessToken(const std::string& accessToken,
                                           const std::string& resource) const;

  // RFC 8414 metadata document (issuer, endpoints, S256-only PKCE, DCR).
  Json authorizationServerMetadata() const;

 private:
  AuthzServerConfig config_;
  std::vector<AuthzClient> clients_;
  struct PendingCode {
    std::string clientId;
    std::string redirectUri;
    std::string scope;
    std::string codeChallenge;
    std::string resource;
    int64_t expiresAt = 0;
  };
  std::vector<std::pair<std::string, PendingCode>> codes_;  // insertion order
  struct IssuedToken {
    std::string clientId;
    std::string scope;
    std::string resource;
    int64_t expiresAt = 0;
  };
  std::vector<std::pair<std::string, IssuedToken>> tokens_;
};

}  // namespace oauth
}  // namespace mcp
