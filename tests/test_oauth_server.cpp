// OAuth 2.1 authorization server core (mcp::oauth::AuthorizationServer):
// DCR, authorize validation, PKCE S256 enforcement, single-use codes,
// RFC 8707 resource binding, metadata - and a full loop where mcp::oauth
// (the CLIENT) runs discovery/DCR/PKCE against this server over httplib.

#include <mcp/mcp.hpp>
#include <mcp/oauth.hpp>
#include <mcp/oauth_server.hpp>

#include "base64.h"      // detail::base64Decode (Basic auth in the reference wiring)
#include "oauth_util.h"  // private: the test computes S256 challenges itself

#include <httplib.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cctype>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace mcp;
using namespace mcp::oauth;

namespace {

std::string s256(const std::string& verifier) {
  return oauth_detail::pkceChallenge(verifier);
}

AuthzServerConfig testConfig(const std::string& issuer = "http://127.0.0.1:8123") {
  AuthzServerConfig c;
  c.issuer = issuer;
  c.scopesSupported = {"mcp:read", "mcp:write"};
  return c;
}

}  // namespace

// ---- registration ------------------------------------------------------------

TEST(AuthzServer, RegistersClientWithGeneratedSecret) {
  AuthorizationServer as(testConfig());
  auto reg = as.registerClient("test app", {"http://localhost:3000/callback"});
  ASSERT_TRUE(reg.has_value());
  EXPECT_FALSE(reg->clientId.empty());
  EXPECT_FALSE(reg->clientSecret.empty());
  EXPECT_EQ(reg->redirectUris.size(), 1u);
  EXPECT_NE(as.findClient(reg->clientId), nullptr);
  EXPECT_EQ(as.findClient("nope"), nullptr);
}

TEST(AuthzServer, RejectsRegistrationWithoutRedirectUris) {
  AuthorizationServer as(testConfig());
  EXPECT_FALSE(as.registerClient("app", {}).has_value());
  EXPECT_FALSE(as.registerClient("app", {"not-a-url"}).has_value());
}

// ---- authorize validation ------------------------------------------------------

TEST(AuthzServer, AuthorizeValidatesEverythingBeforeIssuing) {
  AuthorizationServer as(testConfig());
  auto reg = as.registerClient("app", {"http://localhost:3000/callback"});
  ASSERT_TRUE(reg.has_value());
  const std::string& id = reg->clientId;
  const std::string& redirect = reg->redirectUris[0];

  EXPECT_FALSE(as.authorize("other-client", redirect, "", "s", "C", "S256", "").has_value());
  EXPECT_FALSE(as.authorize(id, "http://localhost:9999/other", "", "s", "C", "S256", "")
                    .has_value());  // unregistered redirect
  EXPECT_FALSE(as.authorize(id, redirect, "", "s", "", "S256", "").has_value());
  EXPECT_FALSE(as.authorize(id, redirect, "", "s", "C", "plain", "").has_value());
  EXPECT_FALSE(as.authorize(id, redirect, "", "s", "C", "", "").has_value());

  auto ok = as.authorize(id, redirect, "mcp:read", "st", "C", "S256", "");
  ASSERT_TRUE(ok.has_value());
  EXPECT_NE(ok->find("code="), std::string::npos);
  EXPECT_NE(ok->find("state=st"), std::string::npos);
  EXPECT_NE(ok->find(redirect), std::string::npos);
}

// ---- token exchange ------------------------------------------------------------

TEST(AuthzServer, ExchangesCodeWithPkceAndConsumesIt) {
  AuthorizationServer as(testConfig());
  auto reg = as.registerClient("app", {"http://localhost:3000/callback"});
  ASSERT_TRUE(reg.has_value());
  const std::string verifier = "verifier-string-with-enough-entropy-1234567890";
  const std::string challenge = s256(verifier);
  auto url = as.authorize(reg->clientId, reg->redirectUris[0], "mcp:read", "s", challenge,
                          "S256", "http://127.0.0.1:8123");
  ASSERT_TRUE(url.has_value());
  const std::string code = url->substr(url->find("code=") + 5, url->find('&') - url->find("code=") - 5);

  auto grant = as.exchangeToken("authorization_code", code, reg->redirectUris[0],
                                reg->clientId, reg->clientSecret, verifier,
                                "http://127.0.0.1:8123");
  ASSERT_TRUE(grant.has_value());
  EXPECT_EQ(grant->tokenType, "Bearer");
  EXPECT_FALSE(grant->accessToken.empty());
  EXPECT_EQ(grant->scope, "mcp:read");
  EXPECT_EQ(grant->resource, "http://127.0.0.1:8123");

  // The code is single use: the second exchange fails.
  auto reuse = as.exchangeToken("authorization_code", code, reg->redirectUris[0],
                                reg->clientId, reg->clientSecret, verifier,
                                "http://127.0.0.1:8123");
  EXPECT_FALSE(reuse.has_value());

  // The issued token verifies for the bound resource.
  auto verified = as.verifyAccessToken(grant->accessToken, "http://127.0.0.1:8123");
  ASSERT_TRUE(verified.has_value());
  EXPECT_EQ(verified.value(), reg->clientId);
  auto wrongAudience = as.verifyAccessToken(grant->accessToken, "http://other.example");
  EXPECT_FALSE(wrongAudience.has_value());
  EXPECT_FALSE(as.verifyAccessToken("forged-token", "").has_value());
}

TEST(AuthzServer, ExchangeRejectsBadClientVerifierOrRedirect) {
  AuthorizationServer as(testConfig());
  auto reg = as.registerClient("app", {"http://localhost:3000/callback"});
  ASSERT_TRUE(reg.has_value());
  const std::string verifier = "a-verifier-of-sufficient-length";
  const std::string challenge = s256(verifier);
  auto url = as.authorize(reg->clientId, reg->redirectUris[0], "", "s", challenge, "S256", "");
  ASSERT_TRUE(url.has_value());
  const std::string code = url->substr(url->find("code=") + 5, url->find('&') - url->find("code=") - 5);

  EXPECT_FALSE(as.exchangeToken("client_credentials", code, reg->redirectUris[0],
                                reg->clientId, reg->clientSecret, verifier, "")
                    .has_value());  // unsupported grant
  EXPECT_FALSE(as.exchangeToken("authorization_code", code, reg->redirectUris[0],
                                reg->clientId, "wrong-secret", verifier, "")
                    .has_value());  // bad client auth
  EXPECT_FALSE(as.exchangeToken("authorization_code", code, "http://localhost:3000/other",
                                reg->clientId, reg->clientSecret, verifier, "")
                    .has_value());  // redirect mismatch
  EXPECT_FALSE(as.exchangeToken("authorization_code", code, reg->redirectUris[0],
                                reg->clientId, reg->clientSecret, "wrong-verifier", "")
                    .has_value());  // PKCE failure
  EXPECT_FALSE(as.exchangeToken("authorization_code", code, reg->redirectUris[0],
                                reg->clientId, reg->clientSecret, "", "")
                    .has_value());  // missing verifier
  // Every failed attempt CONSUMED the code - deliberate: without it, the
  // wrong-verifier path would be a PKCE retry oracle. A fresh code works.
  auto url2 = as.authorize(reg->clientId, reg->redirectUris[0], "", "s", challenge, "S256", "");
  ASSERT_TRUE(url2.has_value());
  const std::string code2 =
      url2->substr(url2->find("code=") + 5, url2->find('&') - url2->find("code=") - 5);
  EXPECT_TRUE(as.exchangeToken("authorization_code", code2, reg->redirectUris[0], reg->clientId,
                               reg->clientSecret, verifier, "")
                  .has_value());
}

TEST(AuthzServer, ResourceMismatchIsRejected) {
  AuthorizationServer as(testConfig());
  auto reg = as.registerClient("app", {"http://localhost:3000/callback"});
  ASSERT_TRUE(reg.has_value());
  const std::string verifier = "a-verifier-of-sufficient-length";
  auto url = as.authorize(reg->clientId, reg->redirectUris[0], "", "s",
                          s256(verifier), "S256", "http://a.example");
  ASSERT_TRUE(url.has_value());
  const std::string code = url->substr(url->find("code=") + 5, url->find('&') - url->find("code=") - 5);
  EXPECT_FALSE(as.exchangeToken("authorization_code", code, reg->redirectUris[0], reg->clientId,
                                reg->clientSecret, verifier, "http://b.example")
                    .has_value());
}

TEST(AuthzServer, MetadataAdvertisesS256AndEndpoints) {
  AuthorizationServer as(testConfig());
  const Json doc = as.authorizationServerMetadata();
  EXPECT_EQ(member(doc, "issuer").toString(), "http://127.0.0.1:8123");
  EXPECT_EQ(member(doc, "token_endpoint").toString(), "http://127.0.0.1:8123/token");
  const Json challenges = member(doc, "code_challenge_methods_supported");
  ASSERT_TRUE(challenges.isArray());
  ASSERT_EQ(challenges.size(), 1u);
  EXPECT_EQ(challenges[0].toString(), "S256");
}

// ---- full loop: our CLIENT against our SERVER ------------------------------------

namespace {

// Mounts the authorization-server core on httplib (PRM + AS metadata + DCR +
// authorize + token). This is the reference wiring for embedders.
class AuthzHttpFixture {
 public:
  AuthzHttpFixture() {
    port_ = svr_.bind_to_any_port("127.0.0.1");
    as_ = std::make_unique<AuthorizationServer>(testConfig("http://127.0.0.1:" +
                                                           std::to_string(port_)));
    wire();
    runner_ = std::thread([this] { (void)svr_.listen_after_bind(); });
    for (int i = 0; i < 10000 && !svr_.is_running(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ~AuthzHttpFixture() {
    svr_.stop();
    if (runner_.joinable()) runner_.join();
  }
  const std::string& issuer() const { return as_->config().issuer; }

 private:
  static std::string percentDecode(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
      if (text[i] == '%' && i + 2 < text.size() && std::isxdigit(static_cast<unsigned char>(text[i + 1])) &&
          std::isxdigit(static_cast<unsigned char>(text[i + 2]))) {
        const auto hex = [](char c) {
          return c <= '9' ? c - '0' : (c | 32) - 'a' + 10;
        };
        out.push_back(static_cast<char>((hex(text[i + 1]) << 4) | hex(text[i + 2])));
        i += 2;
        continue;
      }
      out.push_back(text[i] == '+' ? ' ' : text[i]);
    }
    return out;
  }

  static std::map<std::string, std::string> parseForm(const std::string& body) {
    std::map<std::string, std::string> out;
    size_t pos = 0;
    while (pos <= body.size()) {
      const size_t amp = body.find('&', pos);
      const std::string pair =
          body.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
      const size_t eq = pair.find('=');
      if (eq != std::string::npos) {
        out[percentDecode(pair.substr(0, eq))] = percentDecode(pair.substr(eq + 1));
      }
      if (amp == std::string::npos) break;
      pos = amp + 1;
    }
    return out;
  }

  void wire() {
    // PRM: this server doubles as the protected resource.
    svr_.Get("/.well-known/oauth-protected-resource",
             [this](const httplib::Request&, httplib::Response& res) {
               Json doc = Json(R"({})");
               doc.add("resource", Json::str(issuer()));
               Json servers = Json("[]");
               servers.push_back(Json::str(issuer()));
               doc.add("authorization_servers", std::move(servers));
               res.status = 200;
               res.set_content(doc.toString(), "application/json");
             });
    // AS metadata.
    svr_.Get("/.well-known/oauth-authorization-server",
             [this](const httplib::Request&, httplib::Response& res) {
               res.status = 200;
               res.set_content(as_->authorizationServerMetadata().toString(),
                               "application/json");
             });
    // DCR.
    svr_.Post("/register", [this](const httplib::Request& req, httplib::Response& res) {
      const Json doc = Json(req.body);
      std::vector<std::string> uris;
      for (const auto& u : member(doc, "redirect_uris").toVector()) {
        uris.push_back(std::string(u.toString()));
      }
      auto registered = as_->registerClient(std::string(member(doc, "client_name").toString()),
                                            uris);
      if (!registered.has_value()) {
        res.status = 400;
        res.set_content(R"({"error":"invalid_client_metadata"})", "application/json");
        return;
      }
      Json out = Json(R"({})");
      out.add("client_id", Json::str(registered->clientId));
      out.add("client_secret", Json::str(registered->clientSecret));
      out.add("token_endpoint_auth_method", Json::str(registered->tokenEndpointAuthMethod));
      res.status = 201;
      res.set_content(out.toString(), "application/json");
    });
    // /authorize: validate, then 302 to the redirect with code+state.
    svr_.Get("/authorize", [this](const httplib::Request& req, httplib::Response& res) {
      auto redirect = as_->authorize(
          req.get_param_value("client_id"), req.get_param_value("redirect_uri"),
          req.get_param_value("scope"), req.get_param_value("state"),
          req.get_param_value("code_challenge"), req.get_param_value("code_challenge_method"),
          req.get_param_value("resource"));
      if (!redirect.has_value()) {
        res.status = 400;
        res.set_content(R"({"error":"invalid_request"})", "application/json");
        return;
      }
      res.status = 302;
      res.set_header("Location", redirect.value());
    });
    // /token: form parse -> core -> JSON grant. Client auth comes from either
    // the Basic header (client_secret_basic, what DCR handed out here) or the
    // form fields (client_secret_post).
    svr_.Post("/token", [this](const httplib::Request& req, httplib::Response& res) {
      const auto form = parseForm(req.body);
      std::string clientId = form.count("client_id") ? form.at("client_id") : "";
      std::string clientSecret = form.count("client_secret") ? form.at("client_secret") : "";
      const std::string auth = req.get_header_value("Authorization");
      if (auth.rfind("Basic ", 0) == 0 && clientId.empty()) {
        std::string decoded;
        if (detail::base64Decode(auth.substr(6), decoded)) {
          const size_t colon = decoded.find(':');
          if (colon != std::string::npos) {
            clientId = decoded.substr(0, colon);
            clientSecret = decoded.substr(colon + 1);
          }
        }
      }
      auto grant = as_->exchangeToken(
          form.count("grant_type") ? form.at("grant_type") : "",
          form.count("code") ? form.at("code") : "",
          form.count("redirect_uri") ? form.at("redirect_uri") : "",
          clientId, clientSecret,
          form.count("code_verifier") ? form.at("code_verifier") : "",
          form.count("resource") ? form.at("resource") : "");
      if (!grant.has_value()) {
        res.status = 400;
        res.set_content(R"({"error":"invalid_grant"})", "application/json");
        return;
      }
      Json out = Json(R"({})");
      out.add("access_token", Json::str(grant->accessToken));
      out.add("token_type", Json::str(grant->tokenType));
      out.add("expires_in", Json(grant->expiresIn));
      out.add("scope", Json::str(grant->scope));
      res.status = 200;
      res.set_content(out.toString(), "application/json");
    });
  }

  httplib::Server svr_;
  std::unique_ptr<AuthorizationServer> as_;
  std::thread runner_;
  int port_ = 0;
};

}  // namespace

TEST(AuthzServerLoop, ClientRunsDiscoveryDcrAndPkceAgainstOurServer) {
  AuthzHttpFixture fx;

  Config cfg;
  cfg.resourceUrl = fx.issuer();
  cfg.redirectUri = "http://localhost:3000/callback";
  cfg.clientName = "loopback-test";
  OAuthClient client(cfg);

  // Discovery: PRM -> AS metadata -> DCR (our metadata advertises S256, which
  // the client requires).
  ASSERT_TRUE(client.discover("").has_value());
  EXPECT_EQ(client.serverMetadata().issuer, fx.issuer());
  EXPECT_FALSE(client.client().clientId.empty());

  // The interactive hop, headless: fetch the authorization URL, take the 302
  // Location, complete the PKCE exchange.
  auto url = client.authorizationUrl({});
  ASSERT_TRUE(url.has_value());
  auto exchanged = client.authorizeAndExchange(url.value());
  ASSERT_TRUE(exchanged.has_value()) << "msg=[" << exchanged.error().message << "] code=[" << static_cast<int>(exchanged.error().code) << "]";
  EXPECT_FALSE(client.accessToken().empty());

  // A second, independent client runs the whole flow again: no state leaks
  // between clients and DCR keeps working.
  OAuthClient second(cfg);
  ASSERT_TRUE(second.discover("").has_value());
  auto url2 = second.authorizationUrl({});
  ASSERT_TRUE(url2.has_value());
  EXPECT_TRUE(second.authorizeAndExchange(url2.value()).has_value());
  EXPECT_NE(second.accessToken(), client.accessToken());
}
