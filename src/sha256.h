// SHA-256 (FIPS 180-4) and HMAC-SHA256 (RFC 2104). Private to the library and
// dependency-free; used by the requestState codec (request_state.hpp).

#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace mcp::detail {

using Digest256 = std::array<uint8_t, 32>;

Digest256 sha256(std::string_view data);
Digest256 hmacSha256(std::string_view key, std::string_view message);

}  // namespace mcp::detail
