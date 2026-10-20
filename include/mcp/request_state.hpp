#pragma once
// ---------------------------------------------------------------------------
// mcp/request_state.hpp - integrity-protected requestState for MRTR.
//
// A requestState is opaque server state that the client echoes back on the
// retry round (basic/patterns/mrtr.md). The client is untrusted, so a server
// that depends on the echoed state must verify it. RequestStateCodec seals the
// state with HMAC-SHA256 bound to the request context (method plus tool,
// prompt, or resource identifier) and an expiry, and rejects anything else.
//
// Wire form (version 1):
//   "v1." + b64url(expiry:u64be || payload) + "." + b64url(mac:32 bytes)
//   mac = HMAC-SHA256(key, "mcp-cpp/requestState/v1" || u64be(len(context))
//                           || context || expiry:u64be || payload)
// expiry is Unix seconds; 0 means the state never expires.
//
// The key must be a server secret of at least 32 bytes. Rotate by running
// two codecs in turn, or by reissuing states after a short TTL.
//
// Scope of the protection: the MAC binds the state to the request context
// (method + tool/prompt/resource identifier) and to an expiry, so a state
// cannot be replayed against a DIFFERENT tool and cannot outlive its TTL.
// It does NOT make a state single-use: within one TTL, the same state can be
// echoed more than once. Callers that need at-most-once semantics (e.g. a
// one-time redemption) must enforce it themselves - typically by embedding a
// nonce in the payload and recording consumed nonces server-side.
// ---------------------------------------------------------------------------

#include <mcp/error.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace mcp {

class RequestStateCodec {
 public:
  static constexpr size_t kMinKeyBytes = 32;
  static constexpr int64_t kDefaultTtlSeconds = 600;

  // Fails with InternalError when `key` is shorter than kMinKeyBytes. A
  // `ttlSeconds` <= 0 issues states that never expire (discouraged).
  static McpResult<RequestStateCodec> create(std::string_view key,
                                             int64_t ttlSeconds = kDefaultTtlSeconds);

  // Seals `payload` for `context` (for example "tools/call\nquote") at `nowUnix`.
  std::string seal(std::string_view payload, std::string_view context, int64_t nowUnix) const;

  // Verifies and opens `state`. Any failure (unknown version, malformed,
  // wrong context, forged, expired) returns InvalidParams. The message names
  // the reason for logs; servers must not echo it back to the client.
  McpResult<std::string> open(std::string_view state, std::string_view context,
                              int64_t nowUnix) const;

  // D7: principal binding. sealFor() additionally binds the state to the
  // authenticated principal (typically a token subject); openFor() then
  // rejects a state presented by a DIFFERENT principal even though the MAC
  // itself is valid. This separates integrity (the codec) from identity (the
  // embedder's authentication): contexts are method/target scoped, not user
  // scoped, so without binding one user could replay another user's state
  // against the same tool inside the TTL.
  std::string sealFor(std::string_view payload, std::string_view context,
                      std::string_view principal, int64_t nowUnix) const;
  McpResult<std::string> openFor(std::string_view state, std::string_view context,
                                 std::string_view principal, int64_t nowUnix) const;

  int64_t ttlSeconds() const noexcept { return ttl_; }

 private:
  RequestStateCodec(std::string key, int64_t ttlSeconds)
      : key_(std::move(key)), ttl_(ttlSeconds) {}

  std::string key_;
  int64_t ttl_;
};

}  // namespace mcp
