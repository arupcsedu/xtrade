#include "aegis/common/sha256.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace aegis::common {
namespace {

constexpr std::array<std::uint32_t, 64> round_constants{
    0x428A2F98U, 0x71374491U, 0xB5C0FBCFU, 0xE9B5DBA5U, 0x3956C25BU, 0x59F111F1U,
    0x923F82A4U, 0xAB1C5ED5U, 0xD807AA98U, 0x12835B01U, 0x243185BEU, 0x550C7DC3U,
    0x72BE5D74U, 0x80DEB1FEU, 0x9BDC06A7U, 0xC19BF174U, 0xE49B69C1U, 0xEFBE4786U,
    0x0FC19DC6U, 0x240CA1CCU, 0x2DE92C6FU, 0x4A7484AAU, 0x5CB0A9DCU, 0x76F988DAU,
    0x983E5152U, 0xA831C66DU, 0xB00327C8U, 0xBF597FC7U, 0xC6E00BF3U, 0xD5A79147U,
    0x06CA6351U, 0x14292967U, 0x27B70A85U, 0x2E1B2138U, 0x4D2C6DFCU, 0x53380D13U,
    0x650A7354U, 0x766A0ABBU, 0x81C2C92EU, 0x92722C85U, 0xA2BFE8A1U, 0xA81A664BU,
    0xC24B8B70U, 0xC76C51A3U, 0xD192E819U, 0xD6990624U, 0xF40E3585U, 0x106AA070U,
    0x19A4C116U, 0x1E376C08U, 0x2748774CU, 0x34B0BCB5U, 0x391C0CB3U, 0x4ED8AA4AU,
    0x5B9CCA4FU, 0x682E6FF3U, 0x748F82EEU, 0x78A5636FU, 0x84C87814U, 0x8CC70208U,
    0x90BEFFFAU, 0xA4506CEBU, 0xBEF9A3F7U, 0xC67178F2U,
};

constexpr std::array<std::uint32_t, 8> initial_state{
    0x6A09E667U, 0xBB67AE85U, 0x3C6EF372U, 0xA54FF53AU,
    0x510E527FU, 0x9B05688CU, 0x1F83D9ABU, 0x5BE0CD19U,
};

[[nodiscard]] constexpr std::uint32_t
choose(const std::uint32_t x, const std::uint32_t y, const std::uint32_t z) noexcept {
  return (x & y) ^ (~x & z);
}

[[nodiscard]] constexpr std::uint32_t
majority(const std::uint32_t x, const std::uint32_t y, const std::uint32_t z) noexcept {
  return (x & y) ^ (x & z) ^ (y & z);
}

void transform(std::array<std::uint32_t, 8>& state,
               const std::array<std::uint8_t, 64>& block) noexcept {
  std::array<std::uint32_t, 64> schedule{};
  for (std::size_t index = 0; index < 16; ++index) {
    const auto offset = index * 4U;
    schedule[index] = (static_cast<std::uint32_t>(block[offset]) << 24U) |
                      (static_cast<std::uint32_t>(block[offset + 1U]) << 16U) |
                      (static_cast<std::uint32_t>(block[offset + 2U]) << 8U) |
                      static_cast<std::uint32_t>(block[offset + 3U]);
  }
  for (std::size_t index = 16; index < schedule.size(); ++index) {
    const auto sigma_zero = std::rotr(schedule[index - 15U], 7) ^
                            std::rotr(schedule[index - 15U], 18) ^
                            (schedule[index - 15U] >> 3U);
    const auto sigma_one = std::rotr(schedule[index - 2U], 17) ^
                           std::rotr(schedule[index - 2U], 19) ^
                           (schedule[index - 2U] >> 10U);
    schedule[index] =
        schedule[index - 16U] + sigma_zero + schedule[index - 7U] + sigma_one;
  }

  auto a = state[0];
  auto b = state[1];
  auto c = state[2];
  auto d = state[3];
  auto e = state[4];
  auto f = state[5];
  auto g = state[6];
  auto h = state[7];

  for (std::size_t index = 0; index < schedule.size(); ++index) {
    const auto sum_one = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
    const auto temporary_one =
        h + sum_one + choose(e, f, g) + round_constants[index] + schedule[index];
    const auto sum_zero = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
    const auto temporary_two = sum_zero + majority(a, b, c);

    h = g;
    g = f;
    f = e;
    e = d + temporary_one;
    d = c;
    c = b;
    b = a;
    a = temporary_one + temporary_two;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

} // namespace

Sha256Digest sha256(const std::span<const std::uint8_t> input) noexcept {
  auto state = initial_state;
  std::array<std::uint8_t, 64> block{};
  std::size_t offset = 0;

  while (input.size() - offset >= block.size()) {
    std::copy_n(input.data() + offset, block.size(), block.data());
    transform(state, block);
    offset += block.size();
  }

  const auto remaining = input.size() - offset;
  block.fill(0);
  if (remaining > 0U) {
    std::copy_n(input.data() + offset, remaining, block.data());
  }
  block[remaining] = 0x80U;

  if (remaining >= 56U) {
    transform(state, block);
    block.fill(0);
  }

  const auto bit_length = static_cast<std::uint64_t>(input.size()) * 8U;
  for (std::size_t index = 0; index < 8; ++index) {
    block[63U - index] = static_cast<std::uint8_t>(bit_length >> (index * 8U));
  }
  transform(state, block);

  Sha256Digest digest{};
  for (std::size_t index = 0; index < state.size(); ++index) {
    digest[index * 4U] = static_cast<std::uint8_t>(state[index] >> 24U);
    digest[(index * 4U) + 1U] = static_cast<std::uint8_t>(state[index] >> 16U);
    digest[(index * 4U) + 2U] = static_cast<std::uint8_t>(state[index] >> 8U);
    digest[(index * 4U) + 3U] = static_cast<std::uint8_t>(state[index]);
  }
  return digest;
}

} // namespace aegis::common
