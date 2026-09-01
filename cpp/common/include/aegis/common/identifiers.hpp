#ifndef AEGIS_COMMON_IDENTIFIERS_HPP
#define AEGIS_COMMON_IDENTIFIERS_HPP

#include <array>
#include <compare>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace aegis::common {

template <typename Tag> class Identifier128 {
public:
  constexpr Identifier128() noexcept = default;
  // The two words have a fixed wire order; strong identifier aliases prevent
  // identifiers from different domains being mixed at call sites.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  constexpr Identifier128(const std::uint64_t high, const std::uint64_t low) noexcept
      : high_(high), low_(low) {}

  [[nodiscard]] constexpr std::uint64_t high() const noexcept { return high_; }

  [[nodiscard]] constexpr std::uint64_t low() const noexcept { return low_; }

  [[nodiscard]] constexpr bool valid() const noexcept {
    return high_ != 0U || low_ != 0U;
  }

  auto operator<=>(const Identifier128&) const = default;

private:
  std::uint64_t high_{};
  std::uint64_t low_{};
};

struct VenueIdTag;
struct InstrumentIdTag;
struct ChannelIdTag;
struct StrategyIdTag;
struct ModelIdTag;
struct ModelVersionTag;
struct OrderIdTag;
struct IntentIdTag;
struct ForecastIdTag;
struct FeatureSnapshotIdTag;
struct RiskSnapshotIdTag;
struct ConfigurationVersionTag;
struct GlobalEventIdTag;
struct SessionIdTag;
struct AccountIdTag;

using VenueId = Identifier128<VenueIdTag>;
using InstrumentId = Identifier128<InstrumentIdTag>;
using ChannelId = Identifier128<ChannelIdTag>;
using StrategyId = Identifier128<StrategyIdTag>;
using ModelId = Identifier128<ModelIdTag>;
using ModelVersion = Identifier128<ModelVersionTag>;
using OrderId = Identifier128<OrderIdTag>;
using IntentId = Identifier128<IntentIdTag>;
using ForecastId = Identifier128<ForecastIdTag>;
using FeatureSnapshotId = Identifier128<FeatureSnapshotIdTag>;
using RiskSnapshotId = Identifier128<RiskSnapshotIdTag>;
using ConfigurationVersion = Identifier128<ConfigurationVersionTag>;
using GlobalEventId = Identifier128<GlobalEventIdTag>;
using SessionId = Identifier128<SessionIdTag>;
using AccountId = Identifier128<AccountIdTag>;

template <typename Tag>
[[nodiscard]] constexpr std::array<char, 33>
to_hex(const Identifier128<Tag> identifier) noexcept {
  constexpr std::string_view digits = "0123456789abcdef";
  std::array<char, 33> result{};
  for (std::size_t index = 0; index < 16; ++index) {
    const auto shift = static_cast<unsigned>((15U - index) * 4U);
    result[index] = digits[(identifier.high() >> shift) & 0x0FU];
    result[index + 16] = digits[(identifier.low() >> shift) & 0x0FU];
  }
  result[32] = '\0';
  return result;
}

static_assert(sizeof(VenueId) == 16);
static_assert(std::is_trivially_copyable_v<VenueId>);
static_assert(!std::is_same_v<VenueId, InstrumentId>);
static_assert(!std::is_convertible_v<VenueId, InstrumentId>);
static_assert(!std::is_same_v<AccountId, SessionId>);
static_assert(!std::is_convertible_v<AccountId, SessionId>);

} // namespace aegis::common

#endif // AEGIS_COMMON_IDENTIFIERS_HPP
