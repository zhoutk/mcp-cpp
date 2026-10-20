// requestState codec: SHA-256 / HMAC-SHA256 known-answer vectors, then seal/open
// round trips, and rejection of tampered, mis-bound, expired, and malformed states.

#include "../src/sha256.h"

#include <mcp/mcp.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

using namespace mcp;

namespace {

std::string toHex(const detail::Digest256& d) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  for (const uint8_t b : d) {
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 0x0f]);
  }
  return out;
}

const std::string kKey(32, 'k');
constexpr int64_t kNow = 1'000'000;

RequestStateCodec makeCodec(int64_t ttl = 600) {
  auto r = RequestStateCodec::create(kKey, ttl);
  EXPECT_TRUE(r.has_value());
  return std::move(*r);
}

}  // namespace

// ---- primitives (FIPS 180-4 / RFC 4231 vectors) ----------------------------------

TEST(RequestStateSha256, KnownAnswers) {
  EXPECT_EQ(toHex(detail::sha256("")),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(toHex(detail::sha256("abc")),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  // 448-bit message: forces the two-block padding path.
  EXPECT_EQ(toHex(detail::sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(RequestStateHmac, Rfc4231Case1) {
  const std::string key(20, '\x0b');
  EXPECT_EQ(toHex(detail::hmacSha256(key, "Hi There")),
            "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
}

TEST(RequestStateHmac, Rfc4231Case2) {
  EXPECT_EQ(toHex(detail::hmacSha256("Jefe", "what do ya want for nothing?")),
            "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

// RFC 4231 cases 6/7: a key LONGER than the 64-byte block. These are the only
// tests exercising the "hash the key first" branch of hmacSha256 - without them
// a regression there would go unnoticed (the codec requires >=32-byte keys, so
// application code never sends one this large).
TEST(RequestStateHmac, Rfc4231Case6LongerThanBlockKey) {
  const std::string key(131, '\xaa');
  EXPECT_EQ(toHex(detail::hmacSha256(key, "Test Using Larger Than Block-Size Key - Hash Key First")),
            "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

TEST(RequestStateHmac, Rfc4231Case7LongerKeyAndData) {
  const std::string key(131, '\xaa');
  EXPECT_EQ(toHex(detail::hmacSha256(
                key, "This is a test using a larger than block-size key and a larger "
                     "than block-size data. The key needs to be hashed before being "
                     "used by the HMAC algorithm.")),
            "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2");
}

// ---- codec ------------------------------------------------------------------------

TEST(RequestStateCodecTest, ShortKeyRejected) {
  EXPECT_FALSE(RequestStateCodec::create("too-short").has_value());
  EXPECT_TRUE(RequestStateCodec::create(std::string(32, 'k')).has_value());
}

TEST(RequestStateCodecTest, RoundTripReturnsPayload) {
  const auto codec = makeCodec();
  const std::string state = codec.seal("step=2;cursor=abc", "tools/call\nquote", kNow);
  EXPECT_EQ(state.compare(0, 3, "v1."), 0);
  auto opened = codec.open(state, "tools/call\nquote", kNow + 1);
  ASSERT_TRUE(opened.has_value());
  EXPECT_EQ(*opened, "step=2;cursor=abc");
}

// The UNBOUND wire form is frozen: seal() must keep producing exactly the
// pre-principal-binding bytes (the MAC input is domain + len(context) +
// context + expiry + payload, with no principal field). Principal-bound
// states use a separate domain precisely so this encoding does not move -
// a rolling upgrade keeps verifying states minted by older instances.
TEST(RequestStateCodecTest, UnboundWireFormIsByteCompatible) {
  const auto codec = makeCodec();
  EXPECT_EQ(codec.seal("step=2;cursor=abc", "tools/call\nquote", kNow),
            "v1.AAAAAAAPRJhzdGVwPTI7Y3Vyc29yPWFiYw.aehbdYQBQPcJUCGT_sYJxkP6puAmR4OIwVCWDVhyWws");
}

TEST(RequestStateCodecTest, EmptyPayloadRoundTrips) {
  const auto codec = makeCodec();
  const std::string state = codec.seal("", "prompts/get\nsummarize", kNow);
  auto opened = codec.open(state, "prompts/get\nsummarize", kNow);
  ASSERT_TRUE(opened.has_value());
  EXPECT_EQ(*opened, "");
}

TEST(RequestStateCodecTest, WrongContextRejected) {
  const auto codec = makeCodec();
  const std::string state = codec.seal("s", "tools/call\nquote", kNow);
  // A state minted for one tool must not be accepted for another.
  EXPECT_FALSE(codec.open(state, "tools/call\nother", kNow).has_value());
  EXPECT_FALSE(codec.open(state, "prompts/get\nquote", kNow).has_value());
}

TEST(RequestStateCodecTest, TamperedBodyRejected) {
  const auto codec = makeCodec();
  std::string state = codec.seal("amount=1", "tools/call\npay", kNow);
  // Index 5 lies in the body segment (after "v1." and inside the first group).
  state[5] = state[5] == 'A' ? 'B' : 'A';
  EXPECT_FALSE(codec.open(state, "tools/call\npay", kNow).has_value());
}

TEST(RequestStateCodecTest, ForgedWithOtherKeyRejected) {
  const auto signer = makeCodec();
  auto other = RequestStateCodec::create(std::string(32, 'z'));
  ASSERT_TRUE(other.has_value());
  const std::string forged = other->seal("s", "tools/call\nquote", kNow);
  EXPECT_FALSE(signer.open(forged, "tools/call\nquote", kNow).has_value());
}

TEST(RequestStateCodecTest, ExpiresAtTtl) {
  const auto codec = makeCodec(60);
  const std::string state = codec.seal("s", "tools/call\nquote", kNow);
  EXPECT_TRUE(codec.open(state, "tools/call\nquote", kNow + 59).has_value());
  EXPECT_FALSE(codec.open(state, "tools/call\nquote", kNow + 60).has_value());
}

TEST(RequestStateCodecTest, ZeroTtlNeverExpires) {
  const auto codec = makeCodec(0);
  const std::string state = codec.seal("s", "tools/call\nquote", kNow);
  EXPECT_TRUE(codec.open(state, "tools/call\nquote", kNow + 10'000'000).has_value());
}

TEST(RequestStateCodecTest, MalformedInputRejected) {
  const auto codec = makeCodec();
  const std::string ctx = "tools/call\nquote";
  EXPECT_FALSE(codec.open("", ctx, kNow).has_value());
  EXPECT_FALSE(codec.open("v2.abc.def", ctx, kNow).has_value());
  EXPECT_FALSE(codec.open("v1.abc", ctx, kNow).has_value());
  EXPECT_FALSE(codec.open("v1.!!!!.@@@@", ctx, kNow).has_value());
  // Standard-alphabet characters are not valid base64url.
  EXPECT_FALSE(codec.open("v1.ab+/.cd", ctx, kNow).has_value());
}

// ---- D7: principal binding --------------------------------------------------------

TEST(RequestStateCodecTest, PrincipalBoundStateRejectsOtherUsers) {
  auto codec = RequestStateCodec::create(std::string(32, 'k'));
  ASSERT_TRUE(codec.has_value());
  const std::string ctx = "tools/call\nquote";
  const int64_t now = 1'700'000'000;

  const std::string sealed = codec->sealFor("payload", ctx, "user-a", now);
  // The same principal opens it.
  ASSERT_TRUE(codec->openFor(sealed, ctx, "user-a", now).has_value());
  EXPECT_EQ(codec->openFor(sealed, ctx, "user-a", now).value(), "payload");
  // Another principal is rejected even though the MAC is valid.
  EXPECT_FALSE(codec->openFor(sealed, ctx, "user-b", now).has_value());
  // Nor does the principal-less opener accept a bound state: the MAC input
  // differs, so it is an integrity failure rather than a silent downgrade.
  EXPECT_FALSE(codec->open(sealed, ctx, now).has_value());
}

TEST(RequestStateCodecTest, UnboundStateStillWorksWithOpenForEmptyPrincipal) {
  auto codec = RequestStateCodec::create(std::string(32, 'k'));
  ASSERT_TRUE(codec.has_value());
  const std::string ctx = "tools/call\nquote";
  const int64_t now = 1'700'000'000;
  const std::string sealed = codec->seal("payload", ctx, now);
  ASSERT_TRUE(codec->open(sealed, ctx, now).has_value());
  // Empty principal == the legacy unbound form.
  EXPECT_TRUE(codec->openFor(sealed, ctx, "", now).has_value());
  EXPECT_FALSE(codec->openFor(sealed, ctx, "user-a", now).has_value());
}
