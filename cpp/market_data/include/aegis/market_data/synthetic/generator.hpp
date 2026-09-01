#ifndef AEGIS_MARKET_DATA_SYNTHETIC_GENERATOR_HPP
#define AEGIS_MARKET_DATA_SYNTHETIC_GENERATOR_HPP

#include "aegis/market_data/synthetic/book.hpp"
#include "aegis/market_data/synthetic/config.hpp"
#include "aegis/market_data/synthetic/event.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace aegis::market_data::synthetic {

class SplitMix64 {
public:
  constexpr explicit SplitMix64(const std::uint64_t seed) noexcept : state_(seed) {}

  [[nodiscard]] constexpr std::uint64_t next() noexcept {
    state_ += 0x9E3779B97F4A7C15U;
    auto value = state_;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9U;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBU;
    return value ^ (value >> 31U);
  }

  [[nodiscard]] constexpr std::uint64_t
  bounded(const std::uint64_t upper_exclusive) noexcept {
    if (upper_exclusive == 0U) {
      return 0U;
    }
    return next() % upper_exclusive;
  }

private:
  std::uint64_t state_{};
};

enum class GenerationError : std::uint8_t {
  none = 0,
  complete,
  invalid_config,
  time_overflow,
  sequence_overflow,
  quantity_overflow,
  price_overflow,
  order_capacity_exhausted,
  book_capacity_exhausted,
  book_invariant_failure,
};

// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct GenerationResult {
  GenerationError error{GenerationError::none};

  [[nodiscard]] constexpr bool ok() const noexcept {
    return error == GenerationError::none;
  }
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

class SyntheticExchangeGenerator {
public:
  [[nodiscard]] static std::optional<SyntheticExchangeGenerator>
  create(const GeneratorConfig& config);

  SyntheticExchangeGenerator(SyntheticExchangeGenerator&&) noexcept;
  SyntheticExchangeGenerator& operator=(SyntheticExchangeGenerator&&) noexcept;
  ~SyntheticExchangeGenerator();

  SyntheticExchangeGenerator(const SyntheticExchangeGenerator&) = delete;
  SyntheticExchangeGenerator& operator=(const SyntheticExchangeGenerator&) = delete;

  [[nodiscard]] GenerationResult next(SyntheticEvent& output) noexcept;
  [[nodiscard]] const GeneratorConfig& config() const noexcept;
  [[nodiscard]] const BookSet& expected_book() const noexcept;
  [[nodiscard]] std::uint64_t events_generated() const noexcept;
  [[nodiscard]] std::size_t bounded_state_bytes() const noexcept;

private:
  class Implementation;
  explicit SyntheticExchangeGenerator(std::unique_ptr<Implementation> implementation);

  std::unique_ptr<Implementation> implementation_;
};

[[nodiscard]] std::string_view generation_error_name(GenerationError error) noexcept;

} // namespace aegis::market_data::synthetic

#endif // AEGIS_MARKET_DATA_SYNTHETIC_GENERATOR_HPP
