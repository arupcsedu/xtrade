#ifndef AEGIS_FEATURES_PARITY_HPP
#define AEGIS_FEATURES_PARITY_HPP

#include "aegis/features/feature_engine.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace aegis::features {

enum class FeatureInputKind : std::uint8_t {
  book = 1,
  trade = 2,
};

// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct RecordedFeatureInput {
  FeatureInputKind kind{FeatureInputKind::book};
  BookFeatureEvent book;
  TradeFeatureEvent trade;
};

struct ParityResult {
  FeatureError error{FeatureError::none};
  std::uint64_t online_hash{};
  std::uint64_t replay_hash{};
  std::size_t event_count{};

  [[nodiscard]] constexpr bool matched() const noexcept {
    return error == FeatureError::none && online_hash == replay_hash;
  }
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

class FeatureParityTestHarness final {
public:
  explicit FeatureParityTestHarness(FeatureEngineConfig config);

  [[nodiscard]] FeatureResult on_book(const BookFeatureEvent& event) noexcept;
  [[nodiscard]] FeatureResult on_trade(const TradeFeatureEvent& event) noexcept;
  [[nodiscard]] ParityResult verify(std::uint64_t process_monotonic_time_ns) noexcept;
  [[nodiscard]] std::size_t event_count() const noexcept;
  [[nodiscard]] FeatureEngine& online_engine() noexcept;

private:
  FeatureEngine online_;
  FeatureEngine replay_;
  std::array<RecordedFeatureInput, kMaximumParityEvents> events_{};
  std::size_t event_count_{};
};

} // namespace aegis::features

#endif // AEGIS_FEATURES_PARITY_HPP
