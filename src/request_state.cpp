#include <mcp/request_state.hpp>

#include "base64.h"
#include "sha256.h"

#include <utility>

namespace mcp {
namespace {

constexpr std::string_view kVersionPrefix = "v1.";
constexpr std::string_view kDomain = "mcp-cpp/requestState/v1";
// Principal-bound states use a separate domain so the unbound encoding can
// stay byte-compatible with pre-D7 states.
constexpr std::string_view kBoundDomain = "mcp-cpp/requestState/v1/principal";
constexpr size_t kExpiryBytes = 8;
constexpr size_t kMacBytes = 32;

std::string toBase64Url(std::string_view raw) {
  std::string s = detail::base64Encode(raw);
  for (char& c : s) {
    if (c == '+') c = '-';
    else if (c == '/') c = '_';
  }
  while (!s.empty() && s.back() == '=') s.pop_back();
  return s;
}

// Strict base64url: the standard-alphabet characters '+', '/' and padding '='
// are rejected so that each state has exactly one accepted spelling.
bool fromBase64Url(std::string_view text, std::string& out) {
  for (const char c : text) {
    if (c == '+' || c == '/' || c == '=') return false;
  }
  std::string s(text);
  for (char& c : s) {
    if (c == '-') c = '+';
    else if (c == '_') c = '/';
  }
  if (s.size() % 4 == 1) return false;
  while (s.size() % 4 != 0) s.push_back('=');
  return detail::base64Decode(s, out);
}

void appendU64(std::string& out, uint64_t v) {
  for (int i = 7; i >= 0; --i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}

uint64_t readU64(const char* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | static_cast<unsigned char>(p[i]);
  return v;
}

// MAC input binds the domain, the context (length-prefixed so that no two
// (context, body) pairs share an encoding), and the expiry+payload body.
//
// Two encodings, separated by DOMAIN rather than by a field:
//   - unbound (seal/open): byte-identical to the pre-D7 format, so a state
//     minted by an older instance still verifies after a rolling upgrade;
//   - principal-bound (sealFor/openFor): a distinct domain plus a
//     length-prefixed principal.
// The domains make the two forms non-interchangeable: a bound state never
// verifies under the unbound opener and vice versa, and no concatenation of
// context/principal/body can collide across forms.
std::string macInput(std::string_view context, std::string_view principal,
                     std::string_view body) {
  std::string in;
  if (principal.empty()) {
    in.reserve(kDomain.size() + 8 + context.size() + body.size());
    in.append(kDomain);
    appendU64(in, static_cast<uint64_t>(context.size()));
    in.append(context);
    in.append(body);
    return in;
  }
  in.reserve(kBoundDomain.size() + 16 + context.size() + principal.size() + body.size());
  in.append(kBoundDomain);
  appendU64(in, static_cast<uint64_t>(context.size()));
  in.append(context);
  appendU64(in, static_cast<uint64_t>(principal.size()));
  in.append(principal);
  in.append(body);
  return in;
}

bool constantTimeEqual(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  unsigned char diff = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
  }
  return diff == 0;
}

}  // namespace

McpResult<RequestStateCodec> RequestStateCodec::create(std::string_view key, int64_t ttlSeconds) {
  if (key.size() < kMinKeyBytes) {
    return Err(McpErrorCode::InternalError, "requestState key must be at least 32 bytes");
  }
  return Ok(RequestStateCodec(std::string(key), ttlSeconds));
}

namespace {
std::string sealWith(const std::string& key, int64_t ttl, std::string_view payload,
                     std::string_view context, std::string_view principal, int64_t nowUnix) {
  const uint64_t expiry =
      ttl > 0 ? static_cast<uint64_t>(nowUnix) + static_cast<uint64_t>(ttl) : 0;
  std::string body;
  body.reserve(kExpiryBytes + payload.size());
  appendU64(body, expiry);
  body.append(payload);

  const detail::Digest256 mac = detail::hmacSha256(key, macInput(context, principal, body));
  const std::string_view macView(reinterpret_cast<const char*>(mac.data()), mac.size());
  std::string out(kVersionPrefix);
  out += toBase64Url(body);
  out += '.';
  out += toBase64Url(macView);
  return out;
}
}  // namespace

std::string RequestStateCodec::seal(std::string_view payload, std::string_view context,
                                    int64_t nowUnix) const {
  return sealWith(key_, ttl_, payload, context, std::string_view(), nowUnix);
}

std::string RequestStateCodec::sealFor(std::string_view payload, std::string_view context,
                                       std::string_view principal, int64_t nowUnix) const {
  return sealWith(key_, ttl_, payload, context, principal, nowUnix);
}

namespace {
McpResult<std::string> openWith(const std::string& key, std::string_view state,
                                std::string_view context, std::string_view principal,
                                int64_t nowUnix) {
  const auto rejected = [](std::string_view why) {
    return Err(McpErrorCode::InvalidParams, "invalid requestState: " + std::string(why));
  };
  if (state.substr(0, kVersionPrefix.size()) != kVersionPrefix) return rejected("unknown version");
  state.remove_prefix(kVersionPrefix.size());

  const size_t dot = state.find('.');
  if (dot == std::string_view::npos) return rejected("malformed");
  std::string body;
  std::string mac;
  if (!fromBase64Url(state.substr(0, dot), body) ||
      !fromBase64Url(state.substr(dot + 1), mac)) {
    return rejected("malformed");
  }
  if (body.size() < kExpiryBytes || mac.size() != kMacBytes) return rejected("malformed");

  // Verify integrity before trusting any field inside the body.
  const detail::Digest256 expected = detail::hmacSha256(key, macInput(context, principal, body));
  const std::string_view expectedView(reinterpret_cast<const char*>(expected.data()),
                                      expected.size());
  if (!constantTimeEqual(mac, expectedView)) return rejected("integrity check failed");

  const uint64_t expiry = readU64(body.data());
  if (expiry != 0 && static_cast<uint64_t>(nowUnix) >= expiry) return rejected("expired");
  return Ok(body.substr(kExpiryBytes));
}
}  // namespace

McpResult<std::string> RequestStateCodec::open(std::string_view state, std::string_view context,
                                               int64_t nowUnix) const {
  return openWith(key_, state, context, std::string_view(), nowUnix);
}

McpResult<std::string> RequestStateCodec::openFor(std::string_view state,
                                                  std::string_view context,
                                                  std::string_view principal,
                                                  int64_t nowUnix) const {
  return openWith(key_, state, context, principal, nowUnix);
}

}  // namespace mcp
