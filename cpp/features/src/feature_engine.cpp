#include "aegis/features/feature_engine.hpp"

#include "aegis/features/rolling_window.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <utility>

namespace aegis::features {
namespace {

using DataQuality = market_data::synthetic::DataQuality;
using Side = market_data::synthetic::Side;

struct ConsolidatedTop {
  std::int64_t bid_price_ticks{};
  std::int64_t ask_price_ticks{};
  std::uint64_t bid_quantity_units{};
  std::uint64_t ask_quantity_units{};
  std::uint64_t weighted_bid_quantity_units{};
  std::uint64_t weighted_ask_quantity_units{};
  std::int64_t divergence_half_ticks{};
  std::size_t bid_leader_index{};
  std::size_t ask_leader_index{};
  std::size_t contributing_venues{};
  bool valid{false};
};

struct ActivitySample {
  std::uint64_t time_ns{};
  std::uint64_t depletion_quantity_units{};
  std::uint64_t volume_units{};
  std::int64_t signed_trade_quantity_units{};
  std::int64_t vwap_offset_quantity_product{};
  std::int64_t order_flow_imbalance_units{};
  std::uint32_t add_count{};
  std::uint32_t cancel_count{};
  std::uint32_t execute_count{};
  std::uint32_t trade_count{};
  std::uint32_t quote_count{};
  std::uint32_t replenishment_count{};
  std::uint8_t bid_leader_index{};
  bool has_bid_leader{false};
};

struct ActivityTotals {
  std::uint64_t depletion_quantity_units{};
  std::uint64_t volume_units{};
  std::int64_t signed_trade_quantity_units{};
  std::int64_t vwap_offset_quantity_product{};
  std::int64_t order_flow_imbalance_units{};
  std::uint64_t add_count{};
  std::uint64_t cancel_count{};
  std::uint64_t execute_count{};
  std::uint64_t trade_count{};
  std::uint64_t quote_count{};
  std::uint64_t replenishment_count{};
  std::array<std::uint64_t, kMaximumFeatureVenues> leadership_count{};
};

struct ReturnSample {
  std::uint64_t time_ns{};
  std::int64_t midpoint_half_ticks{};
  std::int64_t return_ppm{};
  std::uint64_t squared_return_ppm{};
};

struct VenueState {
  order_book::DepthSnapshot depth;
  DataQuality quality{DataQuality::invalid};
  std::uint64_t updated_at_ns{};
  bool present{false};
};

struct DepletionState {
  std::uint64_t time_ns{};
  std::int64_t price_ticks{};
  std::uint64_t quantity_units{};
  bool present{false};
};

[[nodiscard]] bool absolute(const std::int64_t value, std::uint64_t& output) noexcept {
  if (value == std::numeric_limits<std::int64_t>::min()) {
    return false;
  }
  output = static_cast<std::uint64_t>(value < 0 ? -value : value);
  return true;
}

// Internal arithmetic order is value, multiplier, divisor at every call site.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] bool multiply_divide_signed(const std::int64_t value,
                                          const std::uint64_t multiplier,
                                          const std::uint64_t divisor,
                                          std::int64_t& output) noexcept {
  if (divisor == 0U) {
    return false;
  }
  std::uint64_t magnitude{};
  if (!absolute(value, magnitude)) {
    return false;
  }
  const auto first = std::gcd(magnitude, divisor);
  magnitude /= first;
  auto remaining_divisor = divisor / first;
  const auto second = std::gcd(multiplier, remaining_divisor);
  const auto reduced_multiplier = multiplier / second;
  remaining_divisor /= second;
  std::uint64_t product{};
  if (__builtin_mul_overflow(magnitude, reduced_multiplier, &product)) {
    return false;
  }
  const auto quotient = product / remaining_divisor;
  if (quotient > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return false;
  }
  const auto signed_quotient = static_cast<std::int64_t>(quotient);
  output = value < 0 ? -signed_quotient : signed_quotient;
  return true;
}

// Internal arithmetic order is value, multiplier, divisor at every call site.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] bool multiply_divide_unsigned(std::uint64_t value,
                                            std::uint64_t multiplier,
                                            std::uint64_t divisor,
                                            std::uint64_t& output) noexcept {
  if (divisor == 0U) {
    return false;
  }
  const auto first = std::gcd(value, divisor);
  value /= first;
  divisor /= first;
  const auto second = std::gcd(multiplier, divisor);
  multiplier /= second;
  divisor /= second;
  std::uint64_t product{};
  if (__builtin_mul_overflow(value, multiplier, &product)) {
    return false;
  }
  output = product / divisor;
  return true;
}

[[nodiscard]] std::uint64_t integer_square_root(std::uint64_t value) noexcept {
  std::uint64_t result{};
  std::uint64_t bit = std::uint64_t{1U} << 62U;
  while (bit > value) {
    bit >>= 2U;
  }
  while (bit != 0U) {
    if (value >= result + bit) {
      value -= result + bit;
      result = (result >> 1U) + bit;
    } else {
      result >>= 1U;
    }
    bit >>= 2U;
  }
  return result;
}

[[nodiscard]] bool valid_quality(const DataQuality quality) noexcept {
  return quality == DataQuality::valid || quality == DataQuality::degraded;
}

[[nodiscard]] bool valid_operation(const order_book::BookOperation operation) noexcept {
  using Operation = order_book::BookOperation;
  switch (operation) {
  case Operation::add:
  case Operation::cancel:
  case Operation::partial_cancel:
  case Operation::replace:
  case Operation::execute:
  case Operation::partial_execute:
  case Operation::delete_order:
  case Operation::trade:
  case Operation::set_level:
  case Operation::delete_level:
  case Operation::clear_side:
  case Operation::clear_book:
  case Operation::set_trading_status:
  case Operation::set_auction_imbalance:
  case Operation::observe_quote:
    return true;
  }
  return false;
}

[[nodiscard]] bool valid_depth(const order_book::DepthSnapshot& depth,
                               const FeatureEngineConfig& config) noexcept {
  if (depth.validity != order_book::BookValidity::valid ||
      depth.bid_count > order_book::kMaximumPublishedDepth ||
      depth.ask_count > order_book::kMaximumPublishedDepth) {
    return false;
  }
  const auto valid_level = [&](const order_book::Level& level) {
    return level.price_ticks > 0 &&
           level.price_ticks <= config.maximum_absolute_price_ticks &&
           level.quantity_units != 0U &&
           level.quantity_units <= config.maximum_quantity_units &&
           level.order_count != 0U;
  };
  for (std::size_t index = 0U; index < depth.bid_count; ++index) {
    if (!valid_level(depth.bids[index]) ||
        (index != 0U &&
         depth.bids[index - 1U].price_ticks <= depth.bids[index].price_ticks)) {
      return false;
    }
  }
  for (std::size_t index = 0U; index < depth.ask_count; ++index) {
    if (!valid_level(depth.asks[index]) ||
        (index != 0U &&
         depth.asks[index - 1U].price_ticks >= depth.asks[index].price_ticks)) {
      return false;
    }
  }
  return depth.bid_count == 0U || depth.ask_count == 0U ||
         depth.bids[0U].price_ticks < depth.asks[0U].price_ticks;
}

void mix_word(std::uint64_t& hash, const std::uint64_t value) noexcept {
  constexpr std::uint64_t kPrime = 1'099'511'628'211ULL;
  for (unsigned shift = 0U; shift < 64U; shift += 8U) {
    hash ^= (value >> shift) & 0xFFU;
    hash *= kPrime;
  }
}

template <typename Tag>
void mix_identifier(std::uint64_t& hash,
                    const common::Identifier128<Tag> identifier) noexcept {
  mix_word(hash, identifier.high());
  mix_word(hash, identifier.low());
}

[[nodiscard]] std::uint64_t derive_low_hash(const std::uint64_t value) noexcept {
  auto mixed = value + 0x9E37'79B9'7F4A'7C15ULL;
  mixed = (mixed ^ (mixed >> 30U)) * 0xBF58'476D'1CE4'E5B9ULL;
  mixed = (mixed ^ (mixed >> 27U)) * 0x94D0'49BB'1331'11EBULL;
  return mixed ^ (mixed >> 31U);
}

void finalize_snapshot(FeatureSnapshot& snapshot) noexcept {
  snapshot.stable_hash = stable_snapshot_hash(snapshot);
  auto low_hash = derive_low_hash(snapshot.stable_hash);
  if (snapshot.stable_hash == 0U && low_hash == 0U) {
    low_hash = 1U;
  }
  snapshot.snapshot_id = {snapshot.stable_hash, low_hash};
}

[[nodiscard]] constexpr std::size_t side_index(const Side side) noexcept {
  return side == Side::ask ? 1U : 0U;
}

} // namespace

namespace detail {

// Fixed-layout implementation storage is intentionally a passive aggregate.
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct FeatureStorage {
  explicit FeatureStorage(const std::size_t capacity) noexcept
      : activity(capacity), returns(capacity) {}

  RollingWindow<ActivitySample, kMaximumWindowSamples> activity;
  RollingWindow<ReturnSample, kMaximumWindowSamples> returns;
  ActivityTotals totals;
  std::array<VenueState, kMaximumFeatureVenues> venues{};
  std::array<DepletionState, 2U> depletions{};
  std::uint64_t squared_return_sum{};
  std::uint64_t book_event_count{};
  std::uint64_t trade_event_count{};
  std::uint64_t consolidated_observation_count{};
  std::uint64_t truncated_until_ns{};
  std::uint64_t last_book_time_ns{};
  std::uint64_t last_trade_time_ns{};
  std::int64_t vwap_anchor_price_ticks{};
  bool vwap_anchor_set{false};
  bool degraded_input_seen{false};

  void clear() noexcept {
    activity.clear();
    returns.clear();
    totals = {};
    venues = {};
    depletions = {};
    squared_return_sum = 0U;
    book_event_count = 0U;
    trade_event_count = 0U;
    consolidated_observation_count = 0U;
    truncated_until_ns = 0U;
    last_book_time_ns = 0U;
    last_trade_time_ns = 0U;
    vwap_anchor_price_ticks = 0;
    vwap_anchor_set = false;
    degraded_input_seen = false;
  }
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

} // namespace detail

namespace {

// Two bounded venue/depth passes keep intermediate sums checked and explicit.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] FeatureError compute_top(const detail::FeatureStorage& storage,
                                       const FeatureEngineConfig& config,
                                       ConsolidatedTop& output) noexcept {
  output = {};
  std::int64_t minimum_midpoint{};
  std::int64_t maximum_midpoint{};
  bool first_midpoint = true;
  for (std::size_t venue = 0U; venue < config.venue_count; ++venue) {
    const auto& venue_state = storage.venues[venue];
    if (!venue_state.present || venue_state.depth.bid_count == 0U ||
        venue_state.depth.ask_count == 0U) {
      continue;
    }
    const auto& bid = venue_state.depth.bids[0U];
    const auto& ask = venue_state.depth.asks[0U];
    std::int64_t midpoint{};
    if (__builtin_add_overflow(bid.price_ticks, ask.price_ticks, &midpoint)) {
      return FeatureError::numeric_overflow;
    }
    minimum_midpoint = first_midpoint ? midpoint : std::min(minimum_midpoint, midpoint);
    maximum_midpoint = first_midpoint ? midpoint : std::max(maximum_midpoint, midpoint);
    first_midpoint = false;
    ++output.contributing_venues;
    if (!output.valid || bid.price_ticks > output.bid_price_ticks ||
        (bid.price_ticks == output.bid_price_ticks &&
         config.venues[venue].venue_number <
             config.venues[output.bid_leader_index].venue_number)) {
      output.bid_price_ticks = bid.price_ticks;
      output.bid_leader_index = venue;
      output.bid_quantity_units = 0U;
    }
    if (!output.valid || ask.price_ticks < output.ask_price_ticks ||
        (ask.price_ticks == output.ask_price_ticks &&
         config.venues[venue].venue_number <
             config.venues[output.ask_leader_index].venue_number)) {
      output.ask_price_ticks = ask.price_ticks;
      output.ask_leader_index = venue;
      output.ask_quantity_units = 0U;
    }
    output.valid = true;
  }
  if (!output.valid) {
    return FeatureError::none;
  }
  if (output.bid_price_ticks >= output.ask_price_ticks) {
    return FeatureError::crossed_market;
  }

  for (std::size_t venue = 0U; venue < config.venue_count; ++venue) {
    const auto& venue_state = storage.venues[venue];
    if (!venue_state.present) {
      continue;
    }
    if (venue_state.depth.bid_count != 0U &&
        venue_state.depth.bids[0U].price_ticks == output.bid_price_ticks &&
        __builtin_add_overflow(output.bid_quantity_units,
                               venue_state.depth.bids[0U].quantity_units,
                               &output.bid_quantity_units)) {
      return FeatureError::numeric_overflow;
    }
    if (venue_state.depth.ask_count != 0U &&
        venue_state.depth.asks[0U].price_ticks == output.ask_price_ticks &&
        __builtin_add_overflow(output.ask_quantity_units,
                               venue_state.depth.asks[0U].quantity_units,
                               &output.ask_quantity_units)) {
      return FeatureError::numeric_overflow;
    }
    const auto bid_levels =
        std::min<std::size_t>(venue_state.depth.bid_count, config.depth_levels);
    const auto ask_levels =
        std::min<std::size_t>(venue_state.depth.ask_count, config.depth_levels);
    for (std::size_t level = 0U; level < bid_levels; ++level) {
      const auto weight = static_cast<std::uint64_t>(config.depth_levels - level);
      std::uint64_t weighted{};
      if (__builtin_mul_overflow(venue_state.depth.bids[level].quantity_units, weight,
                                 &weighted) ||
          __builtin_add_overflow(output.weighted_bid_quantity_units, weighted,
                                 &output.weighted_bid_quantity_units)) {
        return FeatureError::numeric_overflow;
      }
    }
    for (std::size_t level = 0U; level < ask_levels; ++level) {
      const auto weight = static_cast<std::uint64_t>(config.depth_levels - level);
      std::uint64_t weighted{};
      if (__builtin_mul_overflow(venue_state.depth.asks[level].quantity_units, weight,
                                 &weighted) ||
          __builtin_add_overflow(output.weighted_ask_quantity_units, weighted,
                                 &output.weighted_ask_quantity_units)) {
        return FeatureError::numeric_overflow;
      }
    }
  }
  if (output.bid_quantity_units == 0U || output.ask_quantity_units == 0U ||
      __builtin_sub_overflow(maximum_midpoint, minimum_midpoint,
                             &output.divergence_half_ticks)) {
    return FeatureError::numeric_overflow;
  }
  return FeatureError::none;
}

[[nodiscard]] bool add_activity(ActivityTotals& totals,
                                const ActivitySample& sample) noexcept {
  if (__builtin_add_overflow(totals.depletion_quantity_units,
                             sample.depletion_quantity_units,
                             &totals.depletion_quantity_units) ||
      __builtin_add_overflow(totals.volume_units, sample.volume_units,
                             &totals.volume_units) ||
      __builtin_add_overflow(totals.signed_trade_quantity_units,
                             sample.signed_trade_quantity_units,
                             &totals.signed_trade_quantity_units) ||
      __builtin_add_overflow(totals.vwap_offset_quantity_product,
                             sample.vwap_offset_quantity_product,
                             &totals.vwap_offset_quantity_product) ||
      __builtin_add_overflow(totals.order_flow_imbalance_units,
                             sample.order_flow_imbalance_units,
                             &totals.order_flow_imbalance_units) ||
      __builtin_add_overflow(totals.add_count, sample.add_count, &totals.add_count) ||
      __builtin_add_overflow(totals.cancel_count, sample.cancel_count,
                             &totals.cancel_count) ||
      __builtin_add_overflow(totals.execute_count, sample.execute_count,
                             &totals.execute_count) ||
      __builtin_add_overflow(totals.trade_count, sample.trade_count,
                             &totals.trade_count) ||
      __builtin_add_overflow(totals.quote_count, sample.quote_count,
                             &totals.quote_count) ||
      __builtin_add_overflow(totals.replenishment_count, sample.replenishment_count,
                             &totals.replenishment_count)) {
    return false;
  }
  if (sample.has_bid_leader &&
      __builtin_add_overflow(totals.leadership_count[sample.bid_leader_index], 1U,
                             &totals.leadership_count[sample.bid_leader_index])) {
    return false;
  }
  return true;
}

[[nodiscard]] bool remove_activity(ActivityTotals& totals,
                                   const ActivitySample& sample) noexcept {
  if (totals.depletion_quantity_units < sample.depletion_quantity_units ||
      totals.volume_units < sample.volume_units ||
      totals.add_count < sample.add_count ||
      totals.cancel_count < sample.cancel_count ||
      totals.execute_count < sample.execute_count ||
      totals.trade_count < sample.trade_count ||
      totals.quote_count < sample.quote_count ||
      totals.replenishment_count < sample.replenishment_count ||
      __builtin_sub_overflow(totals.signed_trade_quantity_units,
                             sample.signed_trade_quantity_units,
                             &totals.signed_trade_quantity_units) ||
      __builtin_sub_overflow(totals.vwap_offset_quantity_product,
                             sample.vwap_offset_quantity_product,
                             &totals.vwap_offset_quantity_product) ||
      __builtin_sub_overflow(totals.order_flow_imbalance_units,
                             sample.order_flow_imbalance_units,
                             &totals.order_flow_imbalance_units)) {
    return false;
  }
  totals.depletion_quantity_units -= sample.depletion_quantity_units;
  totals.volume_units -= sample.volume_units;
  totals.add_count -= sample.add_count;
  totals.cancel_count -= sample.cancel_count;
  totals.execute_count -= sample.execute_count;
  totals.trade_count -= sample.trade_count;
  totals.quote_count -= sample.quote_count;
  totals.replenishment_count -= sample.replenishment_count;
  if (sample.has_bid_leader) {
    auto& count = totals.leadership_count[sample.bid_leader_index];
    if (count == 0U) {
      return false;
    }
    --count;
  }
  return true;
}

[[nodiscard]] bool expire_activity(detail::FeatureStorage& storage,
                                   const FeatureEngineConfig& config,
                                   const std::uint64_t now_ns) noexcept {
  const auto threshold =
      now_ns > config.rolling_window_ns ? now_ns - config.rolling_window_ns : 0U;
  while (storage.activity.front() != nullptr &&
         storage.activity.front()->time_ns < threshold) {
    ActivitySample removed{};
    if (!storage.activity.pop_front(&removed) ||
        !remove_activity(storage.totals, removed)) {
      return false;
    }
  }
  while (storage.returns.front() != nullptr &&
         storage.returns.front()->time_ns < threshold) {
    ReturnSample removed{};
    if (!storage.returns.pop_front(&removed) ||
        storage.squared_return_sum < removed.squared_return_ppm) {
      return false;
    }
    storage.squared_return_sum -= removed.squared_return_ppm;
  }
  return true;
}

[[nodiscard]] bool append_activity(detail::FeatureStorage& storage,
                                   const FeatureEngineConfig& config,
                                   const ActivitySample& sample) noexcept {
  if (!expire_activity(storage, config, sample.time_ns)) {
    return false;
  }
  if (storage.activity.full()) {
    const auto* oldest = storage.activity.front();
    if (oldest == nullptr ||
        __builtin_add_overflow(oldest->time_ns, config.rolling_window_ns,
                               &storage.truncated_until_ns)) {
      storage.truncated_until_ns = std::numeric_limits<std::uint64_t>::max();
    }
    ActivitySample removed{};
    if (!storage.activity.pop_front(&removed) ||
        !remove_activity(storage.totals, removed)) {
      return false;
    }
  }
  return add_activity(storage.totals, sample) && storage.activity.push(sample);
}

[[nodiscard]] bool append_return(detail::FeatureStorage& storage,
                                 const FeatureEngineConfig& config,
                                 const ReturnSample& sample) noexcept {
  if (storage.returns.full()) {
    const auto* oldest = storage.returns.front();
    if (oldest == nullptr ||
        __builtin_add_overflow(oldest->time_ns, config.rolling_window_ns,
                               &storage.truncated_until_ns)) {
      storage.truncated_until_ns = std::numeric_limits<std::uint64_t>::max();
    }
    ReturnSample removed{};
    if (!storage.returns.pop_front(&removed) ||
        storage.squared_return_sum < removed.squared_return_ppm) {
      return false;
    }
    storage.squared_return_sum -= removed.squared_return_ppm;
  }
  return !__builtin_add_overflow(storage.squared_return_sum, sample.squared_return_ppm,
                                 &storage.squared_return_sum) &&
         storage.returns.push(sample);
}

[[nodiscard]] bool calculate_ofi(const ConsolidatedTop& previous,
                                 const ConsolidatedTop& current,
                                 std::int64_t& output) noexcept {
  output = 0;
  if (!previous.valid || !current.valid ||
      previous.bid_quantity_units >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
      previous.ask_quantity_units >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
      current.bid_quantity_units >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
      current.ask_quantity_units >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return !previous.valid || !current.valid;
  }
  const auto previous_bid = static_cast<std::int64_t>(previous.bid_quantity_units);
  const auto previous_ask = static_cast<std::int64_t>(previous.ask_quantity_units);
  const auto current_bid = static_cast<std::int64_t>(current.bid_quantity_units);
  const auto current_ask = static_cast<std::int64_t>(current.ask_quantity_units);
  const auto add = [&](const std::int64_t value) {
    return !__builtin_add_overflow(output, value, &output);
  };
  const bool overflow =
      (current.bid_price_ticks >= previous.bid_price_ticks && !add(current_bid)) ||
      (current.bid_price_ticks <= previous.bid_price_ticks && !add(-previous_bid)) ||
      (current.ask_price_ticks <= previous.ask_price_ticks && !add(-current_ask)) ||
      (current.ask_price_ticks >= previous.ask_price_ticks && !add(previous_ask));
  return !overflow;
}

[[nodiscard]] FeatureValidity warmed_validity(const bool warmed,
                                              const bool degraded) noexcept {
  if (!warmed) {
    return FeatureValidity::warming;
  }
  return degraded ? FeatureValidity::degraded : FeatureValidity::valid;
}

} // namespace

bool FeatureEngineConfig::valid() const noexcept {
  if (!instrument_id.valid() || !session_id.valid() ||
      !feature_definition_version.valid() || venue_count == 0U ||
      venue_count > venues.size() || depth_levels == 0U ||
      depth_levels > kMaximumFeatureDepth || window_capacity == 0U ||
      window_capacity > kMaximumWindowSamples || rolling_window_ns == 0U ||
      stale_after_ns == 0U || maximum_quantity_units == 0U ||
      maximum_quantity_units >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
      maximum_absolute_price_ticks <= 0 ||
      maximum_absolute_price_ticks > 1'000'000'000LL ||
      session_open_exchange_time_ns <= 0 ||
      session_close_exchange_time_ns <= session_open_exchange_time_ns) {
    return false;
  }
  for (std::size_t index = 0U; index < venue_count; ++index) {
    if (!venues[index].valid()) {
      return false;
    }
    for (std::size_t previous = 0U; previous < index; ++previous) {
      if (venues[index].venue_id == venues[previous].venue_id ||
          venues[index].venue_number == venues[previous].venue_number) {
        return false;
      }
    }
  }
  return true;
}

FeatureEngine::FeatureEngine(FeatureEngineConfig config)
    : initial_config_(config), config_(config),
      owner_thread_(std::this_thread::get_id()),
      storage_(std::make_unique<detail::FeatureStorage>(config_.window_capacity)) {
  if (!config_.valid() || !storage_->activity.valid() || !storage_->returns.valid()) {
    state_ = EngineState::invalid;
    last_error_ = FeatureError::invalid_configuration;
  }
}

FeatureEngine::~FeatureEngine() = default;

bool FeatureEngine::owns_current_thread() const noexcept {
  return owner_thread_ == std::this_thread::get_id();
}

FeatureResult FeatureEngine::fail(const FeatureError error) noexcept {
  last_error_ = error;
  state_ = EngineState::invalid;
  return result();
}

FeatureResult FeatureEngine::result() const noexcept {
  return {.error = last_error_,
          .state = state_,
          .accepted_event_count = accepted_event_count_};
}

// The adjacent scalar arguments mirror the immutable event contract.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
FeatureError FeatureEngine::validate_common(
    const common::GlobalEventId event_id, const common::SessionId session_id,
    const common::InstrumentId instrument_id, const common::VenueId venue_id,
    const std::uint32_t venue_number, const std::uint64_t global_ordinal,
    const std::int64_t exchange_event_time_ns,
    const std::uint64_t process_monotonic_time_ns, const DataQuality quality,
    std::size_t& venue_index) const noexcept {
  if (!owns_current_thread()) {
    return FeatureError::wrong_thread;
  }
  if (state_ == EngineState::invalid) {
    return last_error_ == FeatureError::none ? FeatureError::invalid_book : last_error_;
  }
  if (!event_id.valid()) {
    return FeatureError::invalid_event_id;
  }
  if (session_id != config_.session_id) {
    return FeatureError::invalid_session;
  }
  if (instrument_id != config_.instrument_id) {
    return FeatureError::invalid_identity;
  }
  venue_index = config_.venue_count;
  for (std::size_t index = 0U; index < config_.venue_count; ++index) {
    if (config_.venues[index].venue_id == venue_id &&
        config_.venues[index].venue_number == venue_number) {
      venue_index = index;
      break;
    }
  }
  if (venue_index == config_.venue_count) {
    return FeatureError::invalid_venue;
  }
  if (global_ordinal == 0U ||
      (accepted_event_count_ != 0U && global_ordinal <= last_ordinal_)) {
    return FeatureError::invalid_ordinal;
  }
  if (exchange_event_time_ns <= 0 || process_monotonic_time_ns == 0U ||
      (accepted_event_count_ != 0U &&
       process_monotonic_time_ns < last_process_monotonic_time_ns_)) {
    return FeatureError::invalid_timestamp;
  }
  return valid_quality(quality) ? FeatureError::none
                                : FeatureError::invalid_data_quality;
}
// NOLINTEND(bugprone-easily-swappable-parameters)

// The update routines are deliberately explicit: every state mutation is
// bounded by configured venue/depth/window capacities and every arithmetic
// boundary produces a fail-closed error.
// NOLINTBEGIN(readability-function-cognitive-complexity)
FeatureResult FeatureEngine::on_book(const BookFeatureEvent& event) noexcept {
  std::size_t venue_index{};
  const auto common_error = validate_common(
      event.global_event_id, event.session_id, event.instrument_id, event.venue_id,
      event.venue_number, event.global_ordinal, event.exchange_event_time_ns,
      event.process_monotonic_time_ns, event.data_quality, venue_index);
  if (common_error != FeatureError::none) {
    if (common_error == FeatureError::wrong_thread) {
      return {.error = common_error,
              .state = state_,
              .accepted_event_count = accepted_event_count_};
    }
    return fail(common_error);
  }
  if (!valid_operation(event.operation) ||
      (event.affected_quantity_units > config_.maximum_quantity_units) ||
      (event.affected_price_ticks < 0) ||
      (event.affected_price_ticks > config_.maximum_absolute_price_ticks) ||
      !valid_depth(event.depth, config_)) {
    return fail(FeatureError::invalid_book);
  }

  ConsolidatedTop previous_top{};
  auto top_error = compute_top(*storage_, config_, previous_top);
  if (top_error != FeatureError::none && top_error != FeatureError::crossed_market) {
    return fail(top_error);
  }
  auto& venue = storage_->venues[venue_index];
  venue.depth = event.depth;
  venue.quality = event.data_quality;
  venue.updated_at_ns = event.process_monotonic_time_ns;
  venue.present = true;
  storage_->degraded_input_seen =
      storage_->degraded_input_seen || event.data_quality == DataQuality::degraded;

  ConsolidatedTop current_top{};
  top_error = compute_top(*storage_, config_, current_top);
  if (top_error != FeatureError::none) {
    return fail(top_error);
  }

  ActivitySample sample{.time_ns = event.process_monotonic_time_ns, .quote_count = 1U};
  using Operation = order_book::BookOperation;
  switch (event.operation) {
  case Operation::add:
    sample.add_count = 1U;
    break;
  case Operation::replace:
    sample.add_count = 1U;
    sample.cancel_count = 1U;
    sample.depletion_quantity_units = event.affected_quantity_units;
    break;
  case Operation::cancel:
  case Operation::partial_cancel:
  case Operation::delete_order:
  case Operation::delete_level:
    sample.cancel_count = 1U;
    sample.depletion_quantity_units = event.affected_quantity_units;
    break;
  case Operation::execute:
  case Operation::partial_execute:
    sample.execute_count = 1U;
    sample.depletion_quantity_units = event.affected_quantity_units;
    break;
  case Operation::trade:
  case Operation::set_level:
  case Operation::clear_side:
  case Operation::clear_book:
  case Operation::set_trading_status:
  case Operation::set_auction_imbalance:
  case Operation::observe_quote:
    break;
  }

  if (!calculate_ofi(previous_top, current_top, sample.order_flow_imbalance_units)) {
    return fail(FeatureError::numeric_overflow);
  }
  if (current_top.valid) {
    sample.bid_leader_index = static_cast<std::uint8_t>(current_top.bid_leader_index);
    sample.has_bid_leader = true;
    ++storage_->consolidated_observation_count;
  }

  const bool depletion_operation = event.operation == Operation::cancel ||
                                   event.operation == Operation::partial_cancel ||
                                   event.operation == Operation::delete_order ||
                                   event.operation == Operation::delete_level ||
                                   event.operation == Operation::execute ||
                                   event.operation == Operation::partial_execute;
  if (depletion_operation && order_book::valid_side(event.side) &&
      event.affected_price_ticks > 0 && event.affected_quantity_units != 0U) {
    storage_->depletions[side_index(event.side)] = {
        .time_ns = event.process_monotonic_time_ns,
        .price_ticks = event.affected_price_ticks,
        .quantity_units = event.affected_quantity_units,
        .present = true};
  }
  bool inferred_replenishment = false;
  if (event.operation == Operation::add && order_book::valid_side(event.side)) {
    const auto& depletion = storage_->depletions[side_index(event.side)];
    inferred_replenishment =
        depletion.present && event.affected_price_ticks == depletion.price_ticks &&
        event.process_monotonic_time_ns >= depletion.time_ns &&
        event.process_monotonic_time_ns - depletion.time_ns <=
            config_.replenishment_horizon_ns &&
        event.affected_quantity_units >= config_.minimum_replenishment_quantity_units;
  }
  sample.replenishment_count =
      event.hidden_replenishment || inferred_replenishment ? 1U : 0U;

  if (!append_activity(*storage_, config_, sample)) {
    return fail(FeatureError::numeric_overflow);
  }

  if (current_top.valid) {
    std::int64_t midpoint{};
    if (__builtin_add_overflow(current_top.bid_price_ticks, current_top.ask_price_ticks,
                               &midpoint)) {
      return fail(FeatureError::numeric_overflow);
    }
    ReturnSample return_sample{.time_ns = event.process_monotonic_time_ns,
                               .midpoint_half_ticks = midpoint};
    if (const auto* previous_return = storage_->returns.back();
        previous_return != nullptr) {
      std::int64_t difference{};
      if (__builtin_sub_overflow(midpoint, previous_return->midpoint_half_ticks,
                                 &difference) ||
          !multiply_divide_signed(
              difference, static_cast<std::uint64_t>(kPartsPerMillion),
              static_cast<std::uint64_t>(previous_return->midpoint_half_ticks),
              return_sample.return_ppm)) {
        return fail(FeatureError::numeric_overflow);
      }
      std::uint64_t magnitude{};
      if (!absolute(return_sample.return_ppm, magnitude) ||
          __builtin_mul_overflow(magnitude, magnitude,
                                 &return_sample.squared_return_ppm)) {
        return fail(FeatureError::numeric_overflow);
      }
    }
    if (!append_return(*storage_, config_, return_sample)) {
      return fail(FeatureError::numeric_overflow);
    }
  }

  ++storage_->book_event_count;
  storage_->last_book_time_ns = event.process_monotonic_time_ns;
  ++accepted_event_count_;
  if (accepted_event_count_ == 1U) {
    first_event_id_ = event.global_event_id;
    first_ordinal_ = event.global_ordinal;
  }
  last_event_id_ = event.global_event_id;
  last_ordinal_ = event.global_ordinal;
  last_process_monotonic_time_ns_ = event.process_monotonic_time_ns;
  last_exchange_event_time_ns_ =
      std::max(last_exchange_event_time_ns_, event.exchange_event_time_ns);
  last_error_ = FeatureError::none;
  refresh_state();
  return result();
}

FeatureResult FeatureEngine::on_trade(const TradeFeatureEvent& event) noexcept {
  std::size_t venue_index{};
  const auto common_error = validate_common(
      event.global_event_id, event.session_id, event.instrument_id, event.venue_id,
      event.venue_number, event.global_ordinal, event.exchange_event_time_ns,
      event.process_monotonic_time_ns, event.data_quality, venue_index);
  if (common_error != FeatureError::none) {
    if (common_error == FeatureError::wrong_thread) {
      return {.error = common_error,
              .state = state_,
              .accepted_event_count = accepted_event_count_};
    }
    return fail(common_error);
  }
  static_cast<void>(venue_index);
  if (!order_book::valid_side(event.aggressor_side) || event.price_ticks <= 0 ||
      event.price_ticks > config_.maximum_absolute_price_ticks ||
      event.quantity_units == 0U ||
      event.quantity_units > config_.maximum_quantity_units) {
    return fail(FeatureError::invalid_trade);
  }
  storage_->degraded_input_seen =
      storage_->degraded_input_seen || event.data_quality == DataQuality::degraded;
  if (!storage_->vwap_anchor_set) {
    storage_->vwap_anchor_price_ticks = event.price_ticks;
    storage_->vwap_anchor_set = true;
  }
  std::int64_t price_offset{};
  std::int64_t offset_quantity{};
  if (__builtin_sub_overflow(event.price_ticks, storage_->vwap_anchor_price_ticks,
                             &price_offset) ||
      __builtin_mul_overflow(price_offset,
                             static_cast<std::int64_t>(event.quantity_units),
                             &offset_quantity)) {
    return fail(FeatureError::numeric_overflow);
  }
  const auto signed_quantity = static_cast<std::int64_t>(event.quantity_units);
  const ActivitySample sample{.time_ns = event.process_monotonic_time_ns,
                              .volume_units = event.quantity_units,
                              .signed_trade_quantity_units =
                                  event.aggressor_side == Side::bid ? signed_quantity
                                                                    : -signed_quantity,
                              .vwap_offset_quantity_product = offset_quantity,
                              .trade_count = 1U};
  if (!append_activity(*storage_, config_, sample)) {
    return fail(FeatureError::numeric_overflow);
  }
  ++storage_->trade_event_count;
  storage_->last_trade_time_ns = event.process_monotonic_time_ns;
  ++accepted_event_count_;
  if (accepted_event_count_ == 1U) {
    first_event_id_ = event.global_event_id;
    first_ordinal_ = event.global_ordinal;
  }
  last_event_id_ = event.global_event_id;
  last_ordinal_ = event.global_ordinal;
  last_process_monotonic_time_ns_ = event.process_monotonic_time_ns;
  last_exchange_event_time_ns_ =
      std::max(last_exchange_event_time_ns_, event.exchange_event_time_ns);
  last_error_ = FeatureError::none;
  refresh_state();
  return result();
}
// NOLINTEND(readability-function-cognitive-complexity)

void FeatureEngine::refresh_state() noexcept {
  if (accepted_event_count_ == 0U) {
    state_ = EngineState::recovering;
    return;
  }
  bool all_venues_ready = true;
  for (std::size_t venue = 0U; venue < config_.venue_count; ++venue) {
    all_venues_ready = all_venues_ready && storage_->venues[venue].present &&
                       storage_->venues[venue].depth.bid_count != 0U &&
                       storage_->venues[venue].depth.ask_count != 0U;
  }
  const bool warmed = all_venues_ready &&
                      storage_->book_event_count >= config_.minimum_book_events &&
                      storage_->trade_event_count >= config_.minimum_trade_events;
  if (!warmed) {
    state_ = EngineState::warming;
    return;
  }
  state_ = storage_->degraded_input_seen ||
                   last_process_monotonic_time_ns_ < storage_->truncated_until_ns
               ? EngineState::degraded
               : EngineState::ready;
}

void FeatureEngine::clear_state(
    const EngineState state, const std::uint64_t process_monotonic_time_ns) noexcept {
  storage_->clear();
  state_ = state;
  last_error_ = FeatureError::none;
  accepted_event_count_ = 0U;
  last_ordinal_ = 0U;
  last_process_monotonic_time_ns_ = process_monotonic_time_ns;
  last_exchange_event_time_ns_ = 0;
  first_event_id_ = {};
  last_event_id_ = {};
  first_ordinal_ = 0U;
}

FeatureResult
FeatureEngine::mark_gap(const std::uint64_t process_monotonic_time_ns) noexcept {
  if (!owns_current_thread()) {
    return {.error = FeatureError::wrong_thread,
            .state = state_,
            .accepted_event_count = accepted_event_count_};
  }
  if (process_monotonic_time_ns == 0U ||
      process_monotonic_time_ns < last_process_monotonic_time_ns_) {
    return fail(FeatureError::invalid_timestamp);
  }
  storage_->clear();
  state_ = EngineState::invalid;
  last_error_ = FeatureError::none;
  last_process_monotonic_time_ns_ = process_monotonic_time_ns;
  return result();
}

FeatureResult
FeatureEngine::begin_recovery(const std::uint64_t process_monotonic_time_ns) noexcept {
  if (!owns_current_thread()) {
    return {.error = FeatureError::wrong_thread,
            .state = state_,
            .accepted_event_count = accepted_event_count_};
  }
  if (process_monotonic_time_ns == 0U ||
      process_monotonic_time_ns < last_process_monotonic_time_ns_) {
    return fail(FeatureError::invalid_timestamp);
  }
  clear_state(EngineState::recovering, process_monotonic_time_ns);
  return result();
}

FeatureResult
FeatureEngine::reset_session(const common::SessionId session_id,
                             const std::int64_t session_open_exchange_time_ns,
                             const std::int64_t session_close_exchange_time_ns,
                             const std::uint64_t process_monotonic_time_ns) noexcept {
  if (!owns_current_thread()) {
    return {.error = FeatureError::wrong_thread,
            .state = state_,
            .accepted_event_count = accepted_event_count_};
  }
  if (!session_id.valid() || session_open_exchange_time_ns <= 0 ||
      session_close_exchange_time_ns <= session_open_exchange_time_ns ||
      process_monotonic_time_ns == 0U ||
      process_monotonic_time_ns < last_process_monotonic_time_ns_) {
    return fail(FeatureError::invalid_session);
  }
  config_.session_id = session_id;
  config_.session_open_exchange_time_ns = session_open_exchange_time_ns;
  config_.session_close_exchange_time_ns = session_close_exchange_time_ns;
  clear_state(EngineState::recovering, process_monotonic_time_ns);
  return result();
}

FeatureResult FeatureEngine::reset_for_replay() noexcept {
  if (!owns_current_thread()) {
    return {.error = FeatureError::wrong_thread,
            .state = state_,
            .accepted_event_count = accepted_event_count_};
  }
  config_ = initial_config_;
  clear_state(config_.valid() ? EngineState::recovering : EngineState::invalid, 0U);
  if (!config_.valid()) {
    last_error_ = FeatureError::invalid_configuration;
  }
  return result();
}

// NOLINTBEGIN(readability-function-cognitive-complexity)
FeatureError FeatureEngine::make_snapshot(const std::uint64_t process_monotonic_time_ns,
                                          FeatureSnapshot& output) const noexcept {
  if (!owns_current_thread()) {
    return FeatureError::wrong_thread;
  }
  if (accepted_event_count_ == 0U) {
    return FeatureError::no_snapshot;
  }
  if (process_monotonic_time_ns < last_process_monotonic_time_ns_) {
    return FeatureError::invalid_timestamp;
  }
  output = {};
  output.instrument_id = config_.instrument_id;
  output.session_id = config_.session_id;
  output.feature_definition_version = config_.feature_definition_version;
  output.source_first_global_event_id = first_event_id_;
  output.source_last_global_event_id = last_event_id_;
  output.source_first_ordinal = first_ordinal_;
  output.source_last_ordinal = last_ordinal_;
  output.as_of_exchange_event_time_ns = last_exchange_event_time_ns_;
  output.created_process_monotonic_time_ns = process_monotonic_time_ns;
  const auto age = process_monotonic_time_ns - last_process_monotonic_time_ns_;
  const auto trade_age = storage_->last_trade_time_ns == 0U
                             ? std::numeric_limits<std::uint64_t>::max()
                             : process_monotonic_time_ns - storage_->last_trade_time_ns;
  bool market_stale = false;
  for (std::size_t venue = 0U; venue < config_.venue_count; ++venue) {
    market_stale = market_stale ||
                   (storage_->venues[venue].present &&
                    process_monotonic_time_ns - storage_->venues[venue].updated_at_ns >
                        config_.stale_after_ns);
  }
  output.state = state_;
  if (state_ != EngineState::invalid &&
      (age > config_.stale_after_ns || market_stale ||
       (config_.minimum_trade_events != 0U &&
        storage_->trade_event_count >= config_.minimum_trade_events &&
        trade_age > config_.stale_after_ns))) {
    output.state = EngineState::stale;
  }
  const auto base_degraded =
      state_ == EngineState::degraded ||
      last_process_monotonic_time_ns_ < storage_->truncated_until_ns;
  const bool book_warmed = storage_->book_event_count >= config_.minimum_book_events;
  const bool trade_warmed = storage_->trade_event_count >= config_.minimum_trade_events;
  auto book_validity = warmed_validity(book_warmed, base_degraded);
  auto trade_validity = warmed_validity(trade_warmed, base_degraded);
  auto window_validity = warmed_validity(book_warmed, base_degraded);
  if (output.state == EngineState::invalid) {
    book_validity = FeatureValidity::invalid;
    trade_validity = FeatureValidity::invalid;
    window_validity = FeatureValidity::invalid;
  } else if (output.state == EngineState::stale) {
    book_validity = FeatureValidity::stale;
    window_validity = FeatureValidity::stale;
    if (storage_->trade_event_count != 0U && trade_age > config_.stale_after_ns) {
      trade_validity = FeatureValidity::stale;
    }
  }
  const auto put_at = [&](const FeatureName name, const std::int64_t value,
                          const FeatureValidity validity,
                          const std::uint64_t as_of_time_ns) {
    const auto effective_validity =
        output.state == EngineState::invalid ? FeatureValidity::invalid : validity;
    output.values[static_cast<std::size_t>(name)] = {
        .value = effective_validity == FeatureValidity::invalid ||
                         effective_validity == FeatureValidity::missing
                     ? 0
                     : value,
        .as_of_process_monotonic_time_ns = as_of_time_ns,
        .validity = effective_validity};
  };
  const auto put = [&](const FeatureName name, const std::int64_t value,
                       const FeatureValidity validity) {
    put_at(name, value, validity, last_process_monotonic_time_ns_);
  };
  const auto put_book = [&](const FeatureName name, const std::int64_t value,
                            const FeatureValidity validity) {
    put_at(name, value, validity, storage_->last_book_time_ns);
  };
  const auto put_trade = [&](const FeatureName name, const std::int64_t value,
                             const FeatureValidity validity) {
    put_at(name, value, validity, storage_->last_trade_time_ns);
  };

  if (output.state == EngineState::invalid) {
    for (std::size_t index = 0U; index < output.values.size(); ++index) {
      put(static_cast<FeatureName>(index), 0, FeatureValidity::invalid);
    }
    finalize_snapshot(output);
    return FeatureError::none;
  }

  ConsolidatedTop top{};
  const auto top_error = compute_top(*storage_, config_, top);
  if (top_error != FeatureError::none) {
    return top_error;
  }
  if (!top.valid) {
    book_validity = state_ == EngineState::invalid ? FeatureValidity::invalid
                                                   : FeatureValidity::missing;
  }
  if (top.valid) {
    std::int64_t midpoint{};
    std::int64_t spread{};
    std::uint64_t top_quantity{};
    std::uint64_t weighted_quantity{};
    if (__builtin_add_overflow(top.bid_price_ticks, top.ask_price_ticks, &midpoint) ||
        __builtin_sub_overflow(top.ask_price_ticks, top.bid_price_ticks, &spread) ||
        __builtin_add_overflow(top.bid_quantity_units, top.ask_quantity_units,
                               &top_quantity) ||
        __builtin_add_overflow(top.weighted_bid_quantity_units,
                               top.weighted_ask_quantity_units, &weighted_quantity) ||
        top_quantity >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        weighted_quantity >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return FeatureError::numeric_overflow;
    }
    put_book(FeatureName::midpoint_half_ticks, midpoint, book_validity);
    put_book(FeatureName::spread_ticks, spread, book_validity);
    std::int64_t relative_spread{};
    if (!multiply_divide_signed(spread, 2'000'000U,
                                static_cast<std::uint64_t>(midpoint),
                                relative_spread)) {
      return FeatureError::numeric_overflow;
    }
    put_book(FeatureName::relative_spread_ppm, relative_spread, book_validity);
    std::int64_t bid_share_ppm{};
    if (!multiply_divide_signed(static_cast<std::int64_t>(top.bid_quantity_units),
                                static_cast<std::uint64_t>(kPartsPerMillion),
                                top_quantity, bid_share_ppm)) {
      return FeatureError::numeric_overflow;
    }
    std::int64_t scaled_bid{};
    std::int64_t spread_adjustment{};
    std::int64_t microprice{};
    if (__builtin_mul_overflow(top.bid_price_ticks, kPartsPerMillion, &scaled_bid) ||
        __builtin_mul_overflow(spread, bid_share_ppm, &spread_adjustment) ||
        __builtin_add_overflow(scaled_bid, spread_adjustment, &microprice)) {
      return FeatureError::numeric_overflow;
    }
    put_book(FeatureName::microprice_ticks_ppm, microprice, book_validity);
    const auto bid_quantity = static_cast<std::int64_t>(top.bid_quantity_units);
    const auto ask_quantity = static_cast<std::int64_t>(top.ask_quantity_units);
    std::int64_t quantity_difference{};
    std::int64_t top_imbalance{};
    if (__builtin_sub_overflow(bid_quantity, ask_quantity, &quantity_difference) ||
        !multiply_divide_signed(quantity_difference,
                                static_cast<std::uint64_t>(kPartsPerMillion),
                                top_quantity, top_imbalance)) {
      return FeatureError::numeric_overflow;
    }
    put_book(FeatureName::top_level_imbalance_ppm, top_imbalance, book_validity);
    const auto weighted_bid =
        static_cast<std::int64_t>(top.weighted_bid_quantity_units);
    const auto weighted_ask =
        static_cast<std::int64_t>(top.weighted_ask_quantity_units);
    std::int64_t weighted_difference{};
    std::int64_t weighted_imbalance{};
    if (__builtin_sub_overflow(weighted_bid, weighted_ask, &weighted_difference) ||
        !multiply_divide_signed(weighted_difference,
                                static_cast<std::uint64_t>(kPartsPerMillion),
                                weighted_quantity, weighted_imbalance)) {
      return FeatureError::numeric_overflow;
    }
    put_book(FeatureName::multi_level_weighted_imbalance_ppm, weighted_imbalance,
             book_validity);
    put_book(FeatureName::cross_venue_divergence_half_ticks, top.divergence_half_ticks,
             top.contributing_venues >= 2U ? book_validity : FeatureValidity::warming);
    put_book(
        FeatureName::bid_leader_venue_number,
        static_cast<std::int64_t>(config_.venues[top.bid_leader_index].venue_number),
        book_validity);
    put_book(
        FeatureName::ask_leader_venue_number,
        static_cast<std::int64_t>(config_.venues[top.ask_leader_index].venue_number),
        book_validity);
  } else {
    for (const auto name :
         {FeatureName::midpoint_half_ticks, FeatureName::spread_ticks,
          FeatureName::relative_spread_ppm, FeatureName::microprice_ticks_ppm,
          FeatureName::top_level_imbalance_ppm,
          FeatureName::multi_level_weighted_imbalance_ppm,
          FeatureName::cross_venue_divergence_half_ticks,
          FeatureName::bid_leader_venue_number, FeatureName::ask_leader_venue_number}) {
      put_book(name, 0, book_validity);
    }
  }

  put_book(FeatureName::order_flow_imbalance_units,
           storage_->totals.order_flow_imbalance_units,
           storage_->consolidated_observation_count >= 2U ? window_validity
                                                          : FeatureValidity::warming);
  if (storage_->totals.volume_units != 0U) {
    std::int64_t signed_trade_imbalance{};
    if (!multiply_divide_signed(storage_->totals.signed_trade_quantity_units,
                                static_cast<std::uint64_t>(kPartsPerMillion),
                                storage_->totals.volume_units,
                                signed_trade_imbalance)) {
      return FeatureError::numeric_overflow;
    }
    put_trade(FeatureName::signed_trade_imbalance_ppm, signed_trade_imbalance,
              trade_validity);
  } else {
    put_trade(FeatureName::signed_trade_imbalance_ppm, 0,
              trade_warmed ? FeatureValidity::missing : FeatureValidity::warming);
  }

  const auto* first_activity = storage_->activity.front();
  const auto denominator =
      first_activity == nullptr
          ? 1U
          : std::max<std::uint64_t>(1U, last_process_monotonic_time_ns_ -
                                            first_activity->time_ns + 1U);
  const auto rate = [&](const std::uint64_t count, std::int64_t& value) {
    std::uint64_t unsigned_value{};
    if (!multiply_divide_unsigned(count, 1'000'000'000'000ULL, denominator,
                                  unsigned_value) ||
        unsigned_value >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return false;
    }
    value = static_cast<std::int64_t>(unsigned_value);
    return true;
  };
  std::int64_t add_rate{};
  std::int64_t cancel_rate{};
  std::int64_t execute_rate{};
  std::int64_t trade_rate{};
  std::int64_t quote_rate{};
  std::int64_t replenishment_rate{};
  std::uint64_t depletion_rate_unsigned{};
  if (!rate(storage_->totals.add_count, add_rate) ||
      !rate(storage_->totals.cancel_count, cancel_rate) ||
      !rate(storage_->totals.execute_count, execute_rate) ||
      !rate(storage_->totals.trade_count, trade_rate) ||
      !rate(storage_->totals.quote_count, quote_rate) ||
      !rate(storage_->totals.replenishment_count, replenishment_rate) ||
      !multiply_divide_unsigned(storage_->totals.depletion_quantity_units,
                                1'000'000'000ULL, denominator,
                                depletion_rate_unsigned) ||
      depletion_rate_unsigned >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return FeatureError::numeric_overflow;
  }
  put_book(FeatureName::add_rate_millihertz, add_rate, window_validity);
  put_book(FeatureName::cancel_rate_millihertz, cancel_rate, window_validity);
  put_book(FeatureName::execute_rate_millihertz, execute_rate, window_validity);
  put_book(FeatureName::queue_depletion_rate_units_per_second,
           static_cast<std::int64_t>(depletion_rate_unsigned), window_validity);
  put_trade(FeatureName::trade_intensity_millihertz, trade_rate, trade_validity);
  put_book(FeatureName::quote_intensity_millihertz, quote_rate, window_validity);
  put_book(FeatureName::replenishment_rate_millihertz, replenishment_rate,
           window_validity);

  const auto* first_return = storage_->returns.front();
  const auto* last_return = storage_->returns.back();
  if (first_return != nullptr && last_return != nullptr &&
      storage_->returns.size() >= 2U) {
    std::int64_t midpoint_difference{};
    std::int64_t rolling_return{};
    if (__builtin_sub_overflow(last_return->midpoint_half_ticks,
                               first_return->midpoint_half_ticks,
                               &midpoint_difference) ||
        !multiply_divide_signed(
            midpoint_difference, static_cast<std::uint64_t>(kPartsPerMillion),
            static_cast<std::uint64_t>(first_return->midpoint_half_ticks),
            rolling_return)) {
      return FeatureError::numeric_overflow;
    }
    put_book(FeatureName::rolling_return_ppm, rolling_return, window_validity);
    const auto mean_square = storage_->squared_return_sum / storage_->returns.size();
    put_book(FeatureName::realized_volatility_ppm,
             static_cast<std::int64_t>(integer_square_root(mean_square)),
             window_validity);
  } else {
    put_book(FeatureName::rolling_return_ppm, 0, FeatureValidity::warming);
    put_book(FeatureName::realized_volatility_ppm, 0, FeatureValidity::warming);
  }

  if (storage_->totals.volume_units >
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return FeatureError::numeric_overflow;
  }
  put_trade(FeatureName::volume_units,
            static_cast<std::int64_t>(storage_->totals.volume_units), trade_validity);
  if (storage_->totals.volume_units != 0U && storage_->vwap_anchor_set) {
    std::int64_t offset_ppm{};
    std::int64_t anchor_ppm{};
    std::int64_t vwap{};
    if (!multiply_divide_signed(storage_->totals.vwap_offset_quantity_product,
                                static_cast<std::uint64_t>(kPartsPerMillion),
                                storage_->totals.volume_units, offset_ppm) ||
        __builtin_mul_overflow(storage_->vwap_anchor_price_ticks, kPartsPerMillion,
                               &anchor_ppm) ||
        __builtin_add_overflow(anchor_ppm, offset_ppm, &vwap)) {
      return FeatureError::numeric_overflow;
    }
    put_trade(FeatureName::vwap_ticks_ppm, vwap, trade_validity);
  } else {
    put_trade(FeatureName::vwap_ticks_ppm, 0,
              trade_warmed ? FeatureValidity::missing : FeatureValidity::warming);
  }

  const auto leadership_observations = storage_->totals.quote_count;
  if (leadership_observations != 0U) {
    const auto maximum = *std::max_element(storage_->totals.leadership_count.begin(),
                                           storage_->totals.leadership_count.end());
    std::uint64_t share{};
    if (!multiply_divide_unsigned(maximum, static_cast<std::uint64_t>(kPartsPerMillion),
                                  leadership_observations, share) ||
        share > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return FeatureError::numeric_overflow;
    }
    put_book(FeatureName::venue_leadership_share_ppm, static_cast<std::int64_t>(share),
             window_validity);
  } else {
    put_book(FeatureName::venue_leadership_share_ppm, 0, FeatureValidity::warming);
  }

  auto quality_code = static_cast<std::int64_t>(DataQuality::valid);
  for (std::size_t venue = 0U; venue < config_.venue_count; ++venue) {
    if (!storage_->venues[venue].present) {
      quality_code = static_cast<std::int64_t>(DataQuality::invalid);
      break;
    }
    quality_code = std::max(quality_code,
                            static_cast<std::int64_t>(storage_->venues[venue].quality));
  }
  if (storage_->degraded_input_seen) {
    quality_code =
        std::max(quality_code, static_cast<std::int64_t>(DataQuality::degraded));
  }
  auto quality_validity =
      base_degraded ? FeatureValidity::degraded : FeatureValidity::valid;
  if (output.state == EngineState::stale) {
    quality_validity = FeatureValidity::stale;
    quality_code = static_cast<std::int64_t>(DataQuality::stale);
  } else if (output.state == EngineState::invalid) {
    quality_validity = FeatureValidity::invalid;
  }
  put(FeatureName::data_quality_code, quality_code, quality_validity);
  if (age > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return FeatureError::numeric_overflow;
  }
  put(FeatureName::data_age_ns, static_cast<std::int64_t>(age), quality_validity);

  const auto session_duration =
      config_.session_close_exchange_time_ns - config_.session_open_exchange_time_ns;
  const auto elapsed =
      std::clamp(last_exchange_event_time_ns_ - config_.session_open_exchange_time_ns,
                 std::int64_t{0}, session_duration);
  std::int64_t session_progress{};
  if (!multiply_divide_signed(elapsed, static_cast<std::uint64_t>(kPartsPerMillion),
                              static_cast<std::uint64_t>(session_duration),
                              session_progress)) {
    return FeatureError::numeric_overflow;
  }
  put(FeatureName::session_progress_ppm, session_progress, quality_validity);

  finalize_snapshot(output);
  return FeatureError::none;
}
// NOLINTEND(readability-function-cognitive-complexity)

const FeatureEngineConfig& FeatureEngine::config() const noexcept { return config_; }

EngineState FeatureEngine::state() const noexcept { return state_; }

FeatureError FeatureEngine::last_error() const noexcept { return last_error_; }

std::uint64_t FeatureEngine::accepted_event_count() const noexcept {
  return accepted_event_count_;
}

std::size_t FeatureEngine::memory_bytes_per_symbol() const noexcept {
  return sizeof(*this) + sizeof(detail::FeatureStorage);
}

std::uint64_t stable_snapshot_hash(const FeatureSnapshot& snapshot) noexcept {
  std::uint64_t hash = 14'695'981'039'346'656'037ULL;
  mix_identifier(hash, snapshot.instrument_id);
  mix_identifier(hash, snapshot.session_id);
  mix_identifier(hash, snapshot.feature_definition_version);
  mix_identifier(hash, snapshot.source_first_global_event_id);
  mix_identifier(hash, snapshot.source_last_global_event_id);
  mix_word(hash, snapshot.source_first_ordinal);
  mix_word(hash, snapshot.source_last_ordinal);
  mix_word(hash, static_cast<std::uint64_t>(snapshot.as_of_exchange_event_time_ns));
  mix_word(hash, snapshot.created_process_monotonic_time_ns);
  mix_word(hash, static_cast<std::uint64_t>(snapshot.state));
  for (const auto& value : snapshot.values) {
    mix_word(hash, static_cast<std::uint64_t>(value.value));
    mix_word(hash, value.as_of_process_monotonic_time_ns);
    mix_word(hash, static_cast<std::uint64_t>(value.validity));
  }
  return hash;
}

} // namespace aegis::features
