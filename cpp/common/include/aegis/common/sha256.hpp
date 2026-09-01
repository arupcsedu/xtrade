#ifndef AEGIS_COMMON_SHA256_HPP
#define AEGIS_COMMON_SHA256_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace aegis::common {

using Sha256Digest = std::array<std::uint8_t, 32>;

[[nodiscard]] Sha256Digest sha256(std::span<const std::uint8_t> input) noexcept;

[[nodiscard]] constexpr bool is_zero_digest(const Sha256Digest& digest) noexcept {
  std::uint8_t combined = 0;
  for (const auto byte : digest) {
    combined = static_cast<std::uint8_t>(combined | byte);
  }
  return combined == 0;
}

} // namespace aegis::common

#endif // AEGIS_COMMON_SHA256_HPP
