#ifndef AEGIS_MARKET_DATA_SYNTHETIC_HASH_HPP
#define AEGIS_MARKET_DATA_SYNTHETIC_HASH_HPP

#include <cstddef>
#include <cstdint>
#include <span>

namespace aegis::market_data::synthetic {

inline constexpr std::uint64_t kFnv1aOffsetBasis = 14'695'981'039'346'656'037U;
inline constexpr std::uint64_t kFnv1aPrime = 1'099'511'628'211U;

class StableHash64 {
public:
  constexpr StableHash64() noexcept = default;

  constexpr void add_byte(const std::uint8_t value) noexcept {
    value_ ^= value;
    value_ *= kFnv1aPrime;
  }

  constexpr void add_u16(const std::uint16_t value) noexcept {
    add_byte(static_cast<std::uint8_t>(value >> 8U));
    add_byte(static_cast<std::uint8_t>(value));
  }

  constexpr void add_u32(const std::uint32_t value) noexcept {
    for (int shift = 24; shift >= 0; shift -= 8) {
      add_byte(static_cast<std::uint8_t>(value >> static_cast<unsigned>(shift)));
    }
  }

  constexpr void add_u64(const std::uint64_t value) noexcept {
    for (int shift = 56; shift >= 0; shift -= 8) {
      add_byte(static_cast<std::uint8_t>(value >> static_cast<unsigned>(shift)));
    }
  }

  constexpr void add_i64(const std::int64_t value) noexcept {
    add_u64(static_cast<std::uint64_t>(value));
  }

  constexpr void add(const std::span<const std::uint8_t> bytes) noexcept {
    for (const auto byte : bytes) {
      add_byte(byte);
    }
  }

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

private:
  std::uint64_t value_{kFnv1aOffsetBasis};
};

[[nodiscard]] constexpr std::uint64_t
stable_hash(const std::span<const std::uint8_t> bytes) noexcept {
  StableHash64 hash;
  hash.add(bytes);
  return hash.value();
}

} // namespace aegis::market_data::synthetic

#endif // AEGIS_MARKET_DATA_SYNTHETIC_HASH_HPP
