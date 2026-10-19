#pragma once
// ---------------------------------------------------------------------------
// mcp/oauth.hpp - OAuth 2.1 client for MCP remote transports.
//
// Implements the client half of the MCP authorization model
// (basic/authorization of the 2026-07-28 revision) on top of plain HTTP:
//
//   * Protected-resource metadata discovery (RFC 9728), from the
//     WWW-Authenticate challenge's `resource_metadata` pointer or from the
//     well-known candidates derived from the server URL.
//   * Authorization-server metadata discovery (RFC 8414 / OIDC discovery),
//     path-aware, with the metadata `issuer` validated against the identifier
//     the well-known URL was built from.
//   * Client identity: pre-registration, Dynamic Client Registration
//     (RFC 7591), or a Client ID Metadata Document (SEP-991 / CIMD).
//   * Authorization Code + PKCE (S256), the RFC 8707 `resource` parameter, and
//     the RFC 9207 `iss` response validation.
//   * Token-endpoint client authentication per the advertised method
//     (client_secret_basic / client_secret_post / none).
//
// The interactive hop is headless: the caller fetches the authorization URL
// and hands the resulting redirect target back to handleRedirect().
//
// Every operation is blocking and value-semantic; nothing here throws. The
// module is compiled only when the HTTP transport is available (MCP_HAS_HTTP).
// ---------------------------------------------------------------------------

#include <mcp/error.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mcp {
namespace oauth {

// The protected-resource metadata document (RFC 9728).
struct ResourceMetadata {
  std::string resource;                             // the resource identifier
  std::vector<std::string> authorizationServers;    // issuer identifiers
  std::vector<std::string> scopesSupported;
  std::string metadataUrl;                          // where it was read from
};

// The authorization-server metadata document (RFC 8414 / OIDC).
struct ServerMetadata {
  std::string issuer;
  std::string authorizationEndpoint;
  std::string tokenEndpoint;
  std::string registrationEndpoint;                 // empty when DCR is off
  std::vector<std::string> scopesSupported;
  std::vector<std::string> tokenEndpointAuthMethods;  // default {"none"}
  std::vector<std::string> grantTypesSupported;
  std::vector<std::string> codeChallengeMethods;      // PKCE; must contain S256
  bool issuerParameterSupported = false;            // RFC 9207 advertisement
  bool issuerParameterAdvertised = false;           // the field was present
  bool clientIdMetadataDocumentSupported = false;   // SEP-991 / CIMD
};

// How this client identifies itself to the authorization server.
struct ClientIdentity {
  std::string clientId;
  std::string clientSecret;                         // empty for public clients
  std::string tokenEndpointAuthMethod;              // none | *_basic | *_post
  bool dynamic = false;                             // registered at runtime
};

// The token endpoint response.
struct Tokens {
  std::string accessToken;
  std::string refreshToken;
  std::string tokenType = "Bearer";
  int64_t expiresIn = 0;                            // seconds, 0 = unspecified
  std::vector<std::string> scopes;
  std::string issuer;
};

struct Config {
  std::string resourceUrl;      // the MCP server URL (the RFC 8707 resource)
  std::string redirectUri;      // where the authorization server redirects
  std::string clientName = "mcp-cpp";
  // CIMD document URL; used when the server advertises support for it.
  std::string clientMetadataUrl;
  // Sent in a DCR request (SEP-837): "native" or "web".
  std::string applicationType = "native";
  // Pre-registration: when clientId is set, no DCR/CIMD happens.
  std::string preregisteredClientId;
  std::string preregisteredClientSecret;
  std::string preregisteredAuthMethod;              // optional override
};

class OAuthClient {
 public:
  explicit OAuthClient(Config config);
  ~OAuthClient();
  OAuthClient(OAuthClient&&) noexcept;
  OAuthClient& operator=(OAuthClient&&) noexcept;
  OAuthClient(const OAuthClient&) = delete;
  OAuthClient& operator=(const OAuthClient&) = delete;

  // ---- discovery ----------------------------------------------------------
  // Resolves the protected-resource metadata (using the WWW-Authenticate
  // challenge when one is given), then the authorization-server metadata and
  // the client identity. Fails when the published resource identifier is not
  // consistent with Config::resourceUrl (RFC 9728 / RFC 8707).
  McpResult<void> discover(const std::string& challenge = "");

  // Re-reads the protected-resource metadata and, when its authorization
  // servers changed, re-discovers and re-registers (SEP-2352). Returns true
  // when the authorization server changed. Fails only on transport/parse
  // errors - a missing new metadata document leaves the session untouched.
  McpResult<bool> refreshAuthorizationServer();

  const ResourceMetadata& resourceMetadata() const;
  const ServerMetadata& serverMetadata() const;
  const ClientIdentity& client() const;
  const Tokens& tokens() const;
  const std::string& accessToken() const;

  // The `scope=` values from the challenge `discover()` was given, in order.
  const std::vector<std::string>& challengeScopes() const;

  // ---- authorization ------------------------------------------------------
  // Builds the authorization URL and remembers the PKCE verifier and state.
  // `scopes` empty means "derive": challenge scope, else scopes_supported,
  // else the scope parameter is omitted.
  McpResult<std::string> authorizationUrl(const std::vector<std::string>& scopes);

  // The scopes authorizationUrl() would request for the given input.
  std::vector<std::string> selectScopes(const std::vector<std::string>& requested) const;

  // Consumes the redirect target (Location header) and exchanges the code.
  McpResult<void> handleRedirect(const std::string& redirectUrl);

  // Headless convenience: fetch the authorization URL without following
  // redirects, read the redirect target from the Location header, and complete
  // the token exchange. This is the whole interactive hop for a CLI/test client.
  McpResult<void> authorizeAndExchange(const std::string& authorizationUrl);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace oauth
}  // namespace mcp
