#include "aegis/common/sha256.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace {

[[nodiscard]] std::array<char, 65>
digest_hex(const aegis::common::Sha256Digest& digest) {
  constexpr std::string_view digits = "0123456789abcdef";
  std::array<char, 65> result{};
  for (std::size_t index = 0; index < digest.size(); ++index) {
    result[index * 2U] = digits[digest[index] >> 4U];
    result[(index * 2U) + 1U] = digits[digest[index] & 0x0FU];
  }
  return result;
}

TEST(Sha256Test, MatchesEmptyStandardVector) {
  const auto digest = aegis::common::sha256({});
  EXPECT_EQ(digest_hex(digest).data(),
            std::string_view{"e3b0c44298fc1c149afbf4c8996fb924"
                             "27ae41e4649b934ca495991b7852b855"});
}

TEST(Sha256Test, MatchesAbcStandardVector) {
  constexpr std::string_view input = "abc";
  const auto bytes =
      std::span{reinterpret_cast<const std::uint8_t*>(input.data()), input.size()};
  const auto digest = aegis::common::sha256(bytes);
  EXPECT_EQ(digest_hex(digest).data(),
            std::string_view{"ba7816bf8f01cfea414140de5dae2223"
                             "b00361a396177a9cb410ff61f20015ad"});
}

TEST(Sha256Test, MatchesMultiBlockStandardVector) {
  constexpr std::string_view input =
      "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
      "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
  const auto bytes =
      std::span{reinterpret_cast<const std::uint8_t*>(input.data()), input.size()};
  const auto digest = aegis::common::sha256(bytes);
  EXPECT_EQ(digest_hex(digest).data(),
            std::string_view{"cf5b16a778af8380036ce59e7b049237"
                             "0b249b11e8f07a51afac45037afee9d1"});
}

} // namespace
