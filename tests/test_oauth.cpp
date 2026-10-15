// mcp::oauth unit tests over a real loopback HTTP server: protected-resource
// and authorization-server discovery, client registration (DCR / CIMD /
// pre-registration), PKCE, the RFC 8707 resource parameter, RFC 9207 `iss`
// validation, scope selection and token-endpoint client authentication.

#ifdef MCP_HAS_HTTP

#include <mcp/mcp.hpp>

#include "../src/base64.h"
#include "../src/sha256.h"

#include <httplib.h>

#include <gtest/gtest.h>

#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace mcp;
using namespace mcp::oauth;

namespace {

std::string base64Url(std::string_view raw) {
  std::string text = detail::base64Encode(raw);
  std::string out;
  for (const char ch : text) {
    if (ch == '+') out.push_back('-');
    else if (ch == '/') out.push_back('_');
    else if (ch != '=') out.push_back(ch);
  }
  return out;
}

std::string pkceChallenge(std::string_view verifier) {
  const detail::Digest256 digest = detail::sha256(verifier);
  return base64Url(std::string_view(reinterpret_cast<const char*>(digest.data()), digest.size()));
}

std::string percentEncode(std::string_view text) {
  static const char* kHex = "0123456789ABCDEF";
  std::string out;
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

// Field value of a `a=b&c=d` form body.
std::string formValue(const std::string& body, const std::string& key) {
  size_t pos = 0;
  while (pos <= body.size()) {
    const size_t amp = body.find('&', pos);
    const std::string pair =
        body.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
    const size_t eq = pair.find('=');
    if (eq != std::string::npos && pair.substr(0, eq) == key) return pair.substr(eq + 1);
    if (amp == std::string::npos) break;
    pos = amp + 1;
  }
  return std::string();
}

std::string queryValue(const std::string& target, const std::string& key) {
  const size_t mark = target.find('?');
  if (mark == std::string::npos) return std::string();
  const std::string query = target.substr(mark + 1);
  size_t pos = 0;
  while (pos <= query.size()) {
    const size_t amp = query.find('&', pos);
    const std::string pair =
        query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
    const size_t eq = pair.find('=');
    if (eq != std::string::npos && pair.substr(0, eq) == key) return pair.substr(eq + 1);
    if (amp == std::string::npos) break;
    pos = amp + 1;
  }
  return std::string();
}

// A configurable authorization server + protected resource on loopback. Every
// endpoint records its path; request/response state a test needs is exposed as
// public members.
class OAuthMock {
 public:
  OAuthMock() {
    svr_.Get("/.well-known/oauth-protected-resource/mcp",
             [this](const httplib::Request&, httplib::Response& res) {
               record("/.well-known/oauth-protected-resource/mcp");
               if (!pathPrmServes) {
                 res.status = 404;
                 res.set_content("{}", "application/json");
                 return;
               }
               res.set_content(prmBody(), "application/json");
             });
    svr_.Get("/.well-known/oauth-protected-resource",
             [this](const httplib::Request&, httplib::Response& res) {
               record("/.well-known/oauth-protected-resource");
               if (!rootPrmServes) {
                 res.status = 404;
                 res.set_content("{}", "application/json");
                 return;
               }
               res.set_content(prmBody(), "application/json");
             });
    for (const char* path :
         {"/.well-known/oauth-authorization-server", "/.well-known/openid-configuration"}) {
      svr_.Get(path, [this, path](const httplib::Request&, httplib::Response& res) {
        record(path);
        if (!asMetadataServed || (asMetadataPath != "*" && asMetadataPath != path)) {
          res.status = 404;
          res.set_content("{}", "application/json");
          return;
        }
        res.set_content(asMetadataBody(), "application/json");
      });
    }
    svr_.Post("/register", [this](const httplib::Request& req, httplib::Response& res) {
      record("/register");
      registrationBody = req.body;
      res.status = 201;
      res.set_content(registrationResponse.empty() ? kDefaultRegistration
                                                    : registrationResponse,
                      "application/json");
    });
    svr_.Get("/authorize", [this](const httplib::Request& req, httplib::Response& res) {
      record("/authorize");
      authorizeTarget = req.target;
      std::string location = redirectUri + "?code=test-auth-code";
      if (req.has_param("state")) location += "&state=" + req.get_param_value("state");
      if (issMode == IssMode::Correct) location += "&iss=" + issuer();
      if (issMode == IssMode::Wrong) location += "&iss=https://evil.example.com";
      if (issMode == IssMode::TrailingSlash) location += "&iss=" + issuer() + "/";
      res.status = 302;
      res.set_header("Location", location);
    });
    svr_.Post("/token", [this](const httplib::Request& req, httplib::Response& res) {
      record("/token");
      tokenBody = req.body;
      tokenAuthHeader = req.get_header_value("Authorization");
      const std::string verifier = formValue(req.body, "code_verifier");
      const std::string challenge = queryValue(authorizeTarget, "code_challenge");
      pkceVerified =
          !verifier.empty() && !challenge.empty() && pkceChallenge(verifier) == challenge;
      res.set_content(kTokenResponse, "application/json");
    });

    port_ = svr_.bind_to_any_port("127.0.0.1");
    runner_ = std::thread([this] { svr_.listen_after_bind(); });
    while (!svr_.is_running()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  ~OAuthMock() {
    svr_.stop();
    if (runner_.joinable()) runner_.join();
  }

  std::string base() const { return "http://127.0.0.1:" + std::to_string(port_); }
  std::string serverUrl() const { return base() + "/mcp"; }
  std::string issuer() const { return asIssuerOverride.empty() ? base() : asIssuerOverride; }
  std::string challenge() const {
    return "Bearer error=\"invalid_token\", resource_metadata=\"" + base() +
           "/.well-known/oauth-protected-resource/mcp\"";
  }
  bool requested(const std::string& path) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& p : requested_) {
      if (p == path) return true;
    }
    return false;
  }
  size_t count(const std::string& path) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    size_t n = 0;
    for (const auto& p : requested_) {
      if (p == path) ++n;
    }
    return n;
  }

  // ---- configuration (set before driving the client) ----------------------
  bool pathPrmServes = true;
  bool rootPrmServes = false;
  bool asMetadataServed = true;
  std::string asMetadataPath = "*";  // which well-known path answers
  bool metadataIssuerMismatch = false;
  std::string resourceOverride;      // PRM resource (default: serverUrl)
  std::vector<std::string> prmScopes;
  std::vector<std::string> asScopes;
  std::vector<std::string> authMethods{"none"};
  std::string registrationResponse;  // overrides the DCR response when set
  bool cimdSupported = false;
  bool issuerParameterSupported = true;
  bool issuerParameterAdvertised = true;
  std::string asIssuerOverride;
  std::string redirectUri = "http://localhost:3000/callback";

  enum class IssMode { Absent, Correct, Wrong, TrailingSlash };
  IssMode issMode = IssMode::Correct;

  // ---- observations -------------------------------------------------------
  std::string authorizeTarget;
  std::string tokenBody;
  std::string tokenAuthHeader;
  std::string registrationBody;
  bool pkceVerified = false;

 private:
  static constexpr const char* kDefaultRegistration =
      R"({"client_id":"dcr-client-id","client_secret":"dcr-secret",)"
      R"("token_endpoint_auth_method":"none"})";
  static constexpr const char* kTokenResponse =
      R"({"access_token":"test-access-token","token_type":"Bearer","expires_in":3600})";

  void record(const std::string& path) {
    const std::lock_guard<std::mutex> lock(mutex_);
    requested_.push_back(path);
  }

  std::string prmBody() const {
    Json body = Json(R"({})");
    body.add("resource", Json::str(resourceOverride.empty() ? serverUrl() : resourceOverride));
    Json servers = Json("[]");
    servers.push_back(Json::str(issuer()));
    body.add("authorization_servers", std::move(servers));
    if (!prmScopes.empty()) {
      Json scopes = Json("[]");
      for (const auto& s : prmScopes) scopes.push_back(Json::str(s));
      body.add("scopes_supported", std::move(scopes));
    }
    return body.toString();
  }

  std::string asMetadataBody() const {
    Json body = Json(R"({})");
    body.add("issuer",
             Json::str(metadataIssuerMismatch ? "https://other.example.com" : issuer()));
    body.add("authorization_endpoint", Json::str(base() + "/authorize"));
    body.add("token_endpoint", Json::str(base() + "/token"));
    body.add("registration_endpoint", Json::str(base() + "/register"));
    Json responseTypes = Json("[]");
    responseTypes.push_back(Json::str("code"));
    body.add("response_types_supported", std::move(responseTypes));
    Json methods = Json("[]");
    for (const auto& m : authMethods) methods.push_back(Json::str(m));
    body.add("token_endpoint_auth_methods_supported", std::move(methods));
    if (!asScopes.empty()) {
      Json scopes = Json("[]");
      for (const auto& s : asScopes) scopes.push_back(Json::str(s));
      body.add("scopes_supported", std::move(scopes));
    }
    if (issuerParameterAdvertised) {
      body.add("authorization_response_iss_parameter_supported", Json(issuerParameterSupported));
    }
    if (cimdSupported) body.add("client_id_metadata_document_supported", Json(true));
    return body.toString();
  }

  httplib::Server svr_;
  std::thread runner_;
  int port_ = 0;
  mutable std::mutex mutex_;
  std::vector<std::string> requested_;
};

Config baseConfig(const OAuthMock& mock) {
  Config config;
  config.resourceUrl = mock.serverUrl();
  config.redirectUri = mock.redirectUri;
  return config;
}

// ---- discovery ---------------------------------------------------------------

TEST(OAuthDiscovery, UsesChallengeResourceMetadata) {
  OAuthMock mock;
  mock.prmScopes = {"mcp:basic"};
  OAuthClient client(baseConfig(mock));
  ASSERT_TRUE(client.discover(mock.challenge()).has_value());
  EXPECT_EQ(client.resourceMetadata().resource, mock.serverUrl());
  ASSERT_EQ(client.resourceMetadata().authorizationServers.size(), 1u);
  EXPECT_EQ(client.resourceMetadata().authorizationServers[0], mock.base());
  EXPECT_TRUE(mock.requested("/.well-known/oauth-protected-resource/mcp"));
  // The challenge pointer wins: the root trap must never be requested.
  EXPECT_FALSE(mock.requested("/.well-known/oauth-protected-resource"));
}

TEST(OAuthDiscovery, DerivesPathBasedWellKnownWithoutChallenge) {
  OAuthMock mock;
  OAuthClient client(baseConfig(mock));
  ASSERT_TRUE(client.discover("").has_value());
  EXPECT_TRUE(mock.requested("/.well-known/oauth-protected-resource/mcp"));
}

TEST(OAuthDiscovery, FallsBackToRootProtectedResourceMetadata) {
  OAuthMock mock;
  mock.pathPrmServes = false;
  mock.rootPrmServes = true;
  OAuthClient client(baseConfig(mock));
  ASSERT_TRUE(client.discover("").has_value());
  EXPECT_TRUE(mock.requested("/.well-known/oauth-protected-resource"));
}

TEST(OAuthDiscovery, RejectsResourceFromAnotherOrigin) {
  OAuthMock mock;
  mock.resourceOverride = "https://evil.example.com/mcp";
  OAuthClient client(baseConfig(mock));
  const auto result = client.discover(mock.challenge());
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, McpErrorCode::InvalidParams);
}

TEST(OAuthDiscovery, AcceptsResourceThatIsAPathAncestorOfTheServer) {
  // RFC 9728/RFC 8707: an identifier without the endpoint path still covers the
  // endpoint (the suite's metadata-var2 shape).
  OAuthMock mock;
  mock.resourceOverride = mock.base();
  OAuthClient client(baseConfig(mock));
  ASSERT_TRUE(client.discover(mock.challenge()).has_value());
  EXPECT_EQ(client.resourceMetadata().resource, mock.base());
}

TEST(OAuthDiscovery, RejectsAPathSiblingResource) {
  OAuthMock mock;
  mock.resourceOverride = mock.base() + "/other";
  OAuthClient client(baseConfig(mock));
  EXPECT_FALSE(client.discover(mock.challenge()).has_value());
}

TEST(OAuthDiscovery, UsesOpenIdConfigurationWhenAdvertisedOnlyThere) {
  OAuthMock mock;
  mock.asMetadataPath = "/.well-known/openid-configuration";
  OAuthClient client(baseConfig(mock));
  ASSERT_TRUE(client.discover(mock.challenge()).has_value());
  EXPECT_TRUE(mock.requested("/.well-known/oauth-authorization-server"));
  EXPECT_TRUE(mock.requested("/.well-known/openid-configuration"));
}

TEST(OAuthDiscovery, RejectsAuthorizationServerMetadataWithForeignIssuer) {
  OAuthMock mock;
  mock.metadataIssuerMismatch = true;
  OAuthClient client(baseConfig(mock));
  const auto result = client.discover(mock.challenge());
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, McpErrorCode::InvalidParams);
}

TEST(OAuthDiscovery, FallsBackToServerOriginWhenProtectedResourceMetadataIsAbsent) {
  // The pre-RFC-9728 model (MCP 2025-03-26): no PRM, the authorization server
  // is the MCP server's origin.
  OAuthMock mock;
  mock.pathPrmServes = false;
  mock.rootPrmServes = false;
  OAuthClient client(baseConfig(mock));
  ASSERT_TRUE(client.discover("").has_value());
  ASSERT_EQ(client.resourceMetadata().authorizationServers.size(), 1u);
  EXPECT_EQ(client.resourceMetadata().authorizationServers[0], mock.base());
  EXPECT_EQ(client.resourceMetadata().resource, mock.serverUrl());
  EXPECT_EQ(client.serverMetadata().authorizationEndpoint, mock.base() + "/authorize");
}

TEST(OAuthDiscovery, SynthesizesDefaultEndpointsWhenNoMetadataIsPublished) {
  // "Fallbacks for servers without metadata discovery" (MCP 2025-03-26).
  OAuthMock mock;
  mock.pathPrmServes = false;
  mock.rootPrmServes = false;
  mock.asMetadataServed = false;
  OAuthClient client(baseConfig(mock));
  ASSERT_TRUE(client.discover("").has_value());
  EXPECT_EQ(client.serverMetadata().authorizationEndpoint, mock.base() + "/authorize");
  EXPECT_EQ(client.serverMetadata().tokenEndpoint, mock.base() + "/token");
  EXPECT_EQ(client.serverMetadata().registrationEndpoint, mock.base() + "/register");
}

// ---- client identity ---------------------------------------------------------

TEST(OAuthRegistration, DynamicRegistrationSendsRequiredMetadata) {
  OAuthMock mock;
  mock.registrationResponse =
      R"({"client_id":"dcr-id","client_secret":"dcr-secret",)"
      R"("token_endpoint_auth_method":"client_secret_basic"})";
  OAuthClient client(baseConfig(mock));
  ASSERT_TRUE(client.discover(mock.challenge()).has_value());
  EXPECT_EQ(client.client().clientId, "dcr-id");
  EXPECT_EQ(client.client().clientSecret, "dcr-secret");
  EXPECT_TRUE(client.client().dynamic);
  EXPECT_NE(mock.registrationBody.find("\"application_type\":\"native\""), std::string::npos);
  EXPECT_NE(mock.registrationBody.find(mock.redirectUri), std::string::npos);
  EXPECT_NE(mock.registrationBody.find("refresh_token"), std::string::npos);
  EXPECT_NE(mock.registrationBody.find("\"token_endpoint_auth_method\":\"none\""),
            std::string::npos);
}

TEST(OAuthRegistration, UsesClientIdMetadataDocumentWhenAdvertised) {
  OAuthMock mock;
  mock.cimdSupported = true;
  Config config = baseConfig(mock);
  config.clientMetadataUrl = "https://client.example/metadata.json";
  OAuthClient client(std::move(config));
  ASSERT_TRUE(client.discover(mock.challenge()).has_value());
  EXPECT_EQ(client.client().clientId, "https://client.example/metadata.json");
  EXPECT_FALSE(mock.requested("/register"));
}

TEST(OAuthRegistration, PreRegisteredClientSkipsRegistration) {
  OAuthMock mock;
  Config config = baseConfig(mock);
  config.preregisteredClientId = "pre-id";
  config.preregisteredClientSecret = "pre-secret";
  config.preregisteredAuthMethod = "client_secret_basic";
  OAuthClient client(std::move(config));
  ASSERT_TRUE(client.discover(mock.challenge()).has_value());
  EXPECT_EQ(client.client().clientId, "pre-id");
  EXPECT_FALSE(mock.requested("/register"));
}

// ---- authorization request ---------------------------------------------------

TEST(OAuthAuthorization, UrlCarriesPkceStateResourceAndScope) {
  OAuthMock mock;
  mock.prmScopes = {"mcp:basic", "mcp:read"};
  OAuthClient client(baseConfig(mock));
  ASSERT_TRUE(client.discover(mock.challenge()).has_value());
  const auto url = client.authorizationUrl({});
  ASSERT_TRUE(url.has_value());
  EXPECT_NE(url->find("response_type=code"), std::string::npos);
  EXPECT_NE(url->find("code_challenge_method=S256"), std::string::npos);
  EXPECT_NE(url->find("code_challenge="), std::string::npos);
  EXPECT_NE(url->find("state="), std::string::npos);
  EXPECT_NE(url->find("resource=" + percentEncode(mock.serverUrl())), std::string::npos);
  EXPECT_NE(url->find("scope=mcp%3Abasic%20mcp%3Aread"), std::string::npos);
}

TEST(OAuthScopeSelection, UsesTheChallengeScope) {
  OAuthMock mock;
  OAuthClient client(baseConfig(mock));
  ASSERT_TRUE(client.discover("Bearer error=\"x\", scope=\"challenge:c\"").has_value());
  const auto scopes = client.selectScopes({});
  ASSERT_EQ(scopes.size(), 1u);
  EXPECT_EQ(scopes[0], "challenge:c");
}

TEST(OAuthScopeSelection, ExplicitRequestWins) {
  OAuthMock mock;
  mock.prmScopes = {"prm:a"};
  OAuthClient client(baseConfig(mock));
  ASSERT_TRUE(client.discover("Bearer error=\"x\", scope=\"challenge:c\"").has_value());
  const auto scopes = client.selectScopes({"explicit:d", "explicit:e"});
  ASSERT_EQ(scopes.size(), 2u);
  EXPECT_EQ(scopes[0], "explicit:d");
}

TEST(OAuthScopeSelection, PrefersResourceMetadataOverServerMetadata) {
  OAuthMock mock;
  mock.prmScopes = {"prm:a"};
  mock.asScopes = {"as:b"};
  OAuthClient client(baseConfig(mock));
  ASSERT_TRUE(client.discover("").has_value());
  const auto scopes = client.selectScopes({});
  ASSERT_EQ(scopes.size(), 1u);
  EXPECT_EQ(scopes[0], "prm:a");
}

TEST(OAuthScopeSelection, FallsBackToServerMetadata) {
  OAuthMock mock;
  mock.asScopes = {"as:b", "as:c"};
  OAuthClient client(baseConfig(mock));
  ASSERT_TRUE(client.discover("").has_value());
  const auto scopes = client.selectScopes({});
  ASSERT_EQ(scopes.size(), 2u);
  EXPECT_EQ(scopes[0], "as:b");
}

TEST(OAuthScopeSelection, OmitsScopeWhenNothingDefinesOne) {
  OAuthMock mock;
  OAuthClient client(baseConfig(mock));
  ASSERT_TRUE(client.discover("").has_value());
  EXPECT_TRUE(client.selectScopes({}).empty());
  const auto url = client.authorizationUrl({});
  ASSERT_TRUE(url.has_value());
  EXPECT_EQ(url->find("scope="), std::string::npos);
}

// ---- token exchange ----------------------------------------------------------

TEST(OAuthToken, ExchangesCodeWithPkceAndResource) {
  OAuthMock mock;
  OAuthClient client(baseConfig(mock));
  ASSERT_TRUE(client.discover(mock.challenge()).has_value());
  const auto url = client.authorizationUrl({});
  ASSERT_TRUE(url.has_value());
  ASSERT_TRUE(client.authorizeAndExchange(*url).has_value());
  EXPECT_TRUE(mock.pkceVerified);
  EXPECT_EQ(formValue(mock.tokenBody, "grant_type"), "authorization_code");
  EXPECT_EQ(formValue(mock.tokenBody, "code"), "test-auth-code");
  EXPECT_GE(formValue(mock.tokenBody, "code_verifier").size(), 43u);
  EXPECT_EQ(formValue(mock.tokenBody, "resource"), percentEncode(mock.serverUrl()));
  EXPECT_EQ(client.tokens().accessToken, "test-access-token");
  EXPECT_EQ(client.tokens().tokenType, "Bearer");
  EXPECT_EQ(client.tokens().expiresIn, 3600);
}

TEST(OAuthToken, ClientSecretBasic) {
  OAuthMock mock;
  mock.authMethods = {"client_secret_basic"};
  mock.registrationResponse =
      R"({"client_id":"id","client_secret":"secret",)"
      R"("token_endpoint_auth_method":"client_secret_basic"})";
  OAuthClient client(baseConfig(mock));
  ASSERT_TRUE(client.discover(mock.challenge()).has_value());
  const auto url = client.authorizationUrl({});
  ASSERT_TRUE(url.has_value());
  ASSERT_TRUE(client.authorizeAndExchange(*url).has_value());
  EXPECT_EQ(mock.tokenAuthHeader, "Basic " + detail::base64Encode("id:secret"));
  EXPECT_EQ(mock.tokenBody.find("client_secret"), std::string::npos);
}

TEST(OAuthToken, ClientSecretPost) {
  OAuthMock mock;
  mock.authMethods = {"client_secret_post"};
  mock.registrationResponse =
      R"({"client_id":"id","client_secret":"secret",)"
      R"("token_endpoint_auth_method":"client_secret_post"})";
  OAuthClient client(baseConfig(mock));
  ASSERT_TRUE(client.discover(mock.challenge()).has_value());
  const auto url = client.authorizationUrl({});
  ASSERT_TRUE(url.has_value());
  ASSERT_TRUE(client.authorizeAndExchange(*url).has_value());
  EXPECT_TRUE(mock.tokenAuthHeader.empty());
  EXPECT_EQ(formValue(mock.tokenBody, "client_id"), "id");
  EXPECT_EQ(formValue(mock.tokenBody, "client_secret"), "secret");
}

TEST(OAuthToken, PublicClientSendsNoSecret) {
  OAuthMock mock;
  mock.authMethods = {"none"};
  OAuthClient client(baseConfig(mock));
  ASSERT_TRUE(client.discover(mock.challenge()).has_value());
  const auto url = client.authorizationUrl({});
  ASSERT_TRUE(url.has_value());
  ASSERT_TRUE(client.authorizeAndExchange(*url).has_value());
  EXPECT_TRUE(mock.tokenAuthHeader.empty());
  EXPECT_EQ(mock.tokenBody.find("client_secret"), std::string::npos);
}

// ---- RFC 9207 iss ------------------------------------------------------------

TEST(OAuthIss, AcceptsMatchingIssuer) {
  OAuthMock mock;
  mock.issMode = OAuthMock::IssMode::Correct;
  OAuthClient client(baseConfig(mock));
  ASSERT_TRUE(client.discover(mock.challenge()).has_value());
  const auto url = client.authorizationUrl({});
  ASSERT_TRUE(url.has_value());
  EXPECT_TRUE(client.authorizeAndExchange(*url).has_value());
}

TEST(OAuthIss, RejectsMismatchedIssuer) {
  OAuthMock mock;
  mock.issMode = OAuthMock::IssMode::Wrong;
  OAuthClient client(baseConfig(mock));
  ASSERT_TRUE(client.discover(mock.challenge()).has_value());
  const auto url = client.authorizationUrl({});
  ASSERT_TRUE(url.has_value());
  EXPECT_FALSE(client.authorizeAndExchange(*url).has_value());
  EXPECT_TRUE(mock.tokenBody.empty());  // never reached the token endpoint
}

TEST(OAuthIss, TreatsTrailingSlashVariantAsMismatch) {
  // RFC 9207 compares byte-wise: no normalization may hide a mismatch.
  OAuthMock mock;
  mock.issMode = OAuthMock::IssMode::TrailingSlash;
  OAuthClient client(baseConfig(mock));
  ASSERT_TRUE(client.discover(mock.challenge()).has_value());
  const auto url = client.authorizationUrl({});
  ASSERT_TRUE(url.has_value());
  EXPECT_FALSE(client.authorizeAndExchange(*url).has_value());
}

TEST(OAuthIss, RequiresIssuerWhenAdvertised) {
  OAuthMock mock;
  mock.issMode = OAuthMock::IssMode::Absent;
  mock.issuerParameterAdvertised = true;
  mock.issuerParameterSupported = true;
  OAuthClient client(baseConfig(mock));
  ASSERT_TRUE(client.discover(mock.challenge()).has_value());
  const auto url = client.authorizationUrl({});
  ASSERT_TRUE(url.has_value());
  EXPECT_FALSE(client.authorizeAndExchange(*url).has_value());
}

TEST(OAuthIss, ProceedsWhenIssuerIsNotAdvertisedAndAbsent) {
  OAuthMock mock;
  mock.issMode = OAuthMock::IssMode::Absent;
  mock.issuerParameterAdvertised = false;
  OAuthClient client(baseConfig(mock));
  ASSERT_TRUE(client.discover(mock.challenge()).has_value());
  const auto url = client.authorizationUrl({});
  ASSERT_TRUE(url.has_value());
  EXPECT_TRUE(client.authorizeAndExchange(*url).has_value());
}

TEST(OAuthIss, RejectsUnadvertisedButWrongIssuer) {
  OAuthMock mock;
  mock.issMode = OAuthMock::IssMode::Wrong;
  mock.issuerParameterAdvertised = false;
  OAuthClient client(baseConfig(mock));
  ASSERT_TRUE(client.discover(mock.challenge()).has_value());
  const auto url = client.authorizationUrl({});
  ASSERT_TRUE(url.has_value());
  EXPECT_FALSE(client.authorizeAndExchange(*url).has_value());
}

// ---- SEP-2352: authorization-server migration --------------------------------

TEST(OAuthMigration, ReRegistersWhenTheAuthorizationServerChanges) {
  OAuthMock resource;   // hosts the PRM + the first authorization server
  OAuthMock secondAs;   // the new authorization server
  resource.registrationResponse =
      R"({"client_id":"as1-id","client_secret":"as1-secret",)"
      R"("token_endpoint_auth_method":"none"})";
  secondAs.registrationResponse =
      R"({"client_id":"as2-id","client_secret":"as2-secret",)"
      R"("token_endpoint_auth_method":"none"})";

  OAuthClient client(baseConfig(resource));
  ASSERT_TRUE(client.discover(resource.challenge()).has_value());
  EXPECT_EQ(client.client().clientId, "as1-id");

  // The protected resource now points at a different authorization server.
  resource.asIssuerOverride = secondAs.base();
  // secondAs must accept the well-known request on its own origin, and its
  // metadata issuer must match that origin.
  const auto refreshed = client.refreshAuthorizationServer();
  ASSERT_TRUE(refreshed.has_value()) << refreshed.error().message;
  EXPECT_TRUE(refreshed.value());
  EXPECT_EQ(client.client().clientId, "as2-id");
  EXPECT_TRUE(secondAs.requested("/register"));
}

TEST(OAuthMigration, NoChangeLeavesTheSessionAlone) {
  OAuthMock mock;
  OAuthClient client(baseConfig(mock));
  ASSERT_TRUE(client.discover(mock.challenge()).has_value());
  ASSERT_EQ(mock.count("/register"), 1u);
  const auto refreshed = client.refreshAuthorizationServer();
  ASSERT_TRUE(refreshed.has_value());
  EXPECT_FALSE(refreshed.value());
  EXPECT_EQ(mock.count("/register"), 1u);  // no second registration
}

}  // namespace

#endif  // MCP_HAS_HTTP
