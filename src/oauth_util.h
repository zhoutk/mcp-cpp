// Shared OAuth internals (client + server): CSPRNG tokens, PKCE challenge,
// base64url, wall-clock. Not part of the public API.
//
// These were split out of src/oauth.cpp when the authorization-server core
// (oauth_server.cpp) needed the same primitives; keep a single definition of
// each so a security fix lands in both flows.
#pragma once

#include "base64.h"
#include "sha256.h"

#if defined(_WIN32)
#include <windows.h>
#endif

#include <cstdint>
#include <ctime>
#include <fstream>
#include <random>
#include <string>
#include <string_view>

namespace mcp::oauth_detail {

inline std::string base64Url(std::string_view raw) {
  std::string text = detail::base64Encode(raw);
  std::string out;
  out.reserve(text.size());
  for (const char ch : text) {
    if (ch == '+') out.push_back('-');
    else if (ch == '/') out.push_back('_');
    else if (ch != '=') out.push_back(ch);
  }
  return out;
}

// PKCE S256 challenge: BASE64URL-ENCODE(SHA256(ASCII(code_verifier))).
inline std::string pkceChallenge(std::string_view verifier) {
  const detail::Digest256 digest = detail::sha256(verifier);
  const std::string_view bytes(reinterpret_cast<const char*>(digest.data()), digest.size());
  return base64Url(bytes);
}

// Cryptographically secure random bytes. The PKCE verifier, state parameter
// and issued tokens are security tokens - RFC 7636 requires a CSPRNG, and
// std::random_device provides no such guarantee (its quality is
// implementation-defined). Windows resolves RtlGenRandom dynamically (no link
// dependency); POSIX reads /dev/urandom; std::random_device is the last resort.
inline std::string secureRandomBytes(size_t n) {
  std::string out(n, '\0');
#if defined(_WIN32)
  using RtlGenRandomFn = BOOLEAN(WINAPI*)(PVOID, ULONG);
  static const RtlGenRandomFn rtlGenRandom = []() -> RtlGenRandomFn {
    HMODULE adv = GetModuleHandleA("advapi32.dll");
    if (adv == nullptr) adv = LoadLibraryA("advapi32.dll");
    return adv != nullptr
               ? reinterpret_cast<RtlGenRandomFn>(GetProcAddress(adv, "SystemFunction036"))
               : nullptr;
  }();
  if (rtlGenRandom != nullptr && rtlGenRandom(out.data(), static_cast<ULONG>(n))) {
    return out;
  }
  // fall through to std::random_device
#endif
  {
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    if (urandom.read(out.data(), static_cast<std::streamsize>(n))) return out;
  }
  std::random_device device;
  for (size_t i = 0; i < n; ++i) out[i] = static_cast<char>(device());
  return out;
}

// A URL-safe random token of `length` characters from the RFC 7636 unreserved
// set (one CSPRNG byte per character: the 6-bit mask cannot reach three of the
// 66 unreserved characters, so the alphabet is the remaining 64).
inline std::string randomToken(size_t length) {
  static const char* kAlphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-.";
  const std::string randomness = secureRandomBytes(length);
  std::string out;
  out.reserve(length);
  for (size_t i = 0; i < length; ++i) {
    out.push_back(kAlphabet[static_cast<unsigned char>(randomness[i]) & 63]);
  }
  return out;
}

// Wall clock in whole seconds (the server's single source of "now").
inline int64_t unixNowSeconds() {
  return static_cast<int64_t>(::time(nullptr));
}

}  // namespace mcp::oauth_detail
