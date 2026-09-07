#include "aegis/common/authentication.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace aegis::common {
namespace {

[[nodiscard]] constexpr std::uint8_t nibble(const char value) noexcept {
  return value >= '0' && value <= '9' ? static_cast<std::uint8_t>(value - '0')
                                      : static_cast<std::uint8_t>(value - 'a' + 10);
}

template <std::size_t Size>
[[nodiscard]] constexpr std::array<std::uint8_t, Size>
hex(const std::string_view value) noexcept {
  std::array<std::uint8_t, Size> output{};
  for (std::size_t index = 0U; index < Size; ++index) {
    output[index] = static_cast<std::uint8_t>((nibble(value[index * 2U]) << 4U) |
                                              nibble(value[(index * 2U) + 1U]));
  }
  return output;
}

TEST(AuthenticationTest, MatchesRfc4231HmacSha256Vector) {
  HmacSha256Key key{};
  key.fill(0U);
  std::fill_n(key.begin(), 20U, 0x0BU);
  constexpr std::array<std::uint8_t, 8U> message{'H', 'i', ' ', 'T',
                                                 'h', 'e', 'r', 'e'};
  constexpr auto expected =
      hex<32U>("b0344c61d8db38535ca8afceaf0bf12b" // pragma: allowlist secret
               "881dc200c9833da726e9376c2e32cff7" // pragma: allowlist secret
      );
  Sha256Digest actual{};
  ASSERT_TRUE(hmac_sha256(key, message, actual));
  EXPECT_EQ(actual, expected);
  EXPECT_TRUE(constant_time_equal(actual, expected));
  auto changed = expected;
  changed.back() ^= 1U;
  EXPECT_FALSE(constant_time_equal(actual, changed));
}

TEST(AuthenticationTest, MatchesRfc8032Ed25519EmptyMessageVector) {
  constexpr auto private_key =
      hex<32U>("9d61b19deffd5a60ba844af492ec2cc4" // pragma: allowlist secret
               "4449c5697b326919703bac031cae7f60" // pragma: allowlist secret
      );
  constexpr auto expected_public =
      hex<32U>("d75a980182b10ab7d54bfed3c964073a" // pragma: allowlist secret
               "0ee172f3daa62325af021a68f707511a" // pragma: allowlist secret
      );
  constexpr auto expected_signature =
      hex<64U>("e5564300c360ac729086e2cc806e828a" // pragma: allowlist secret
               "84877f1eb8e5d974d873e06522490155" // pragma: allowlist secret
               "5fb8821590a33bacc61e39701cf9b46b" // pragma: allowlist secret
               "d25bf5f0595bbe24655141438e7a100b" // pragma: allowlist secret
      );
  Ed25519PublicKey public_key{};
  ASSERT_TRUE(ed25519_derive_public_key(private_key, public_key));
  EXPECT_EQ(public_key, expected_public);
  Ed25519Signature signature{};
  ASSERT_TRUE(ed25519_sign(private_key, {}, signature));
  EXPECT_EQ(signature, expected_signature);
  EXPECT_TRUE(ed25519_verify(public_key, {}, signature));
  signature.front() ^= 1U;
  EXPECT_FALSE(ed25519_verify(public_key, {}, signature));
}

TEST(AuthenticationTest, RejectsOversizedHmacMessage) {
  const HmacSha256Key key{};
  std::array<std::uint8_t, kMaximumHmacMessageBytes + 1U> oversized{};
  Sha256Digest digest{};
  EXPECT_FALSE(hmac_sha256(key, oversized, digest));
  EXPECT_TRUE(is_zero_digest(digest));
}

} // namespace
} // namespace aegis::common
