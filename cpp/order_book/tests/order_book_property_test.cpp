#include "aegis/order_book/order_book.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <memory>

namespace book = aegis::order_book;
namespace synthetic = aegis::market_data::synthetic;

namespace {

inline constexpr std::uint64_t kDeterministicSeed = 20'260'828U;

class SplitMix64 final {
public:
  explicit SplitMix64(const std::uint64_t seed) noexcept : state_(seed) {}

  [[nodiscard]] std::uint64_t next() noexcept {
    auto value = (state_ += 0x9e37'79b9'7f4a'7c15ULL);
    value = (value ^ (value >> 30U)) * 0xbf58'476d'1ce4'e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d0'49bb'1331'11ebULL;
    return value ^ (value >> 31U);
  }

  [[nodiscard]] std::uint64_t bounded(const std::uint64_t bound) noexcept {
    return next() % bound;
  }

private:
  std::uint64_t state_;
};

struct ReferenceOrder {
  synthetic::Side side{synthetic::Side::none};
  std::int64_t price{};
  std::uint64_t quantity{};
};

struct ReferenceAggregate {
  std::uint64_t quantity{};
  std::uint32_t count{};
};

[[nodiscard]] book::BookConfig make_config(const book::BookMode mode) {
  return {
      .identity = {.venue_id = aegis::common::VenueId{1U, 1U},
                   .instrument_id = aegis::common::InstrumentId{2U, 1U},
                   .venue_number = 1U,
                   .instrument_number = 1U},
      .session_id = aegis::common::SessionId{3U, 1U},
      .mode = mode,
      .order_capacity = 128U,
      .level_capacity = 64U,
      .published_depth = 32U,
      .maximum_sequence = 10'000U,
      .permit_crossed_transition = false,
      .require_contiguous_sequence = true,
      .start_recovering = false,
  };
}

[[nodiscard]] constexpr aegis::common::OrderId make_id(const std::uint64_t id) {
  return {55U, id};
}

template <typename Map>
[[nodiscard]] typename Map::iterator select(Map& values, const std::size_t offset) {
  auto iterator = values.begin();
  std::advance(iterator, static_cast<typename Map::difference_type>(offset));
  return iterator;
}

// This test intentionally contains the entire deterministic state-machine loop.
// NOLINTBEGIN(readability-function-cognitive-complexity)
TEST(OrderBookPropertyTest, RandomizedOrderFlowMatchesIndependentAggregateOracle) {
  auto engine =
      std::make_unique<book::OrderBook>(make_config(book::BookMode::order_by_order));
  std::map<std::uint64_t, ReferenceOrder> reference;
  SplitMix64 random{kDeterministicSeed};
  std::uint64_t next_id = 1U;

  for (std::uint64_t sequence = 1U; sequence <= 2'000U; ++sequence) {
    const auto roll = random.bounded(100U);
    book::BookUpdate update{
        .order_id = {}, .sequence = sequence, .process_monotonic_time_ns = sequence};
    if (reference.empty() || (roll < 38U && reference.size() < 100U)) {
      const auto side =
          random.bounded(2U) == 0U ? synthetic::Side::bid : synthetic::Side::ask;
      const auto price = side == synthetic::Side::bid
                             ? 90 + static_cast<std::int64_t>(random.bounded(10U))
                             : 101 + static_cast<std::int64_t>(random.bounded(10U));
      const auto quantity = 1U + random.bounded(1'000U);
      update.operation = book::BookOperation::add;
      update.side = side;
      update.order_id = make_id(next_id);
      update.price_ticks = price;
      update.quantity_units = quantity;
      reference.emplace(
          next_id, ReferenceOrder{.side = side, .price = price, .quantity = quantity});
      ++next_id;
    } else {
      auto selected = select(reference, random.bounded(reference.size()));
      update.order_id = make_id(selected->first);
      const auto action = roll % 5U;
      if (action == 0U) {
        update.operation = book::BookOperation::cancel;
        reference.erase(selected);
      } else if (action == 1U && selected->second.quantity > 1U) {
        const auto removal = 1U + random.bounded(selected->second.quantity - 1U);
        update.operation = book::BookOperation::partial_cancel;
        update.quantity_units = removal;
        selected->second.quantity -= removal;
      } else if (action == 2U && selected->second.quantity > 1U) {
        const auto removal = 1U + random.bounded(selected->second.quantity - 1U);
        update.operation = book::BookOperation::partial_execute;
        update.quantity_units = removal;
        selected->second.quantity -= removal;
      } else if (action == 3U) {
        update.operation = book::BookOperation::execute;
        update.quantity_units = selected->second.quantity;
        reference.erase(selected);
      } else {
        const auto side =
            random.bounded(2U) == 0U ? synthetic::Side::bid : synthetic::Side::ask;
        const auto price = side == synthetic::Side::bid
                               ? 90 + static_cast<std::int64_t>(random.bounded(10U))
                               : 101 + static_cast<std::int64_t>(random.bounded(10U));
        const auto quantity = 1U + random.bounded(1'000U);
        update.operation = book::BookOperation::replace;
        update.side = side;
        update.price_ticks = price;
        update.quantity_units = quantity;
        selected->second = {.side = side, .price = price, .quantity = quantity};
      }
    }

    const auto result = engine->apply(update);
    ASSERT_TRUE(result.accepted())
        << "seed=" << kDeterministicSeed << " sequence=" << sequence
        << " error=" << static_cast<unsigned>(result.error);
    ASSERT_TRUE(engine->check_invariants().valid())
        << "seed=" << kDeterministicSeed << " sequence=" << sequence;

    std::map<std::int64_t, ReferenceAggregate, std::greater<>> bids;
    std::map<std::int64_t, ReferenceAggregate> asks;
    for (const auto& [id, order] : reference) {
      static_cast<void>(id);
      auto& aggregate =
          order.side == synthetic::Side::bid ? bids[order.price] : asks[order.price];
      aggregate.quantity += order.quantity;
      ++aggregate.count;
    }
    book::DepthSnapshot actual{};
    ASSERT_EQ(engine->depth(actual, 32U), book::ApplyError::none);
    ASSERT_EQ(actual.bid_count, bids.size());
    ASSERT_EQ(actual.ask_count, asks.size());
    std::size_t index = 0U;
    for (const auto& [price, aggregate] : bids) {
      EXPECT_EQ(actual.bids[index].price_ticks, price);
      EXPECT_EQ(actual.bids[index].quantity_units, aggregate.quantity);
      EXPECT_EQ(actual.bids[index].order_count, aggregate.count);
      ++index;
    }
    index = 0U;
    for (const auto& [price, aggregate] : asks) {
      EXPECT_EQ(actual.asks[index].price_ticks, price);
      EXPECT_EQ(actual.asks[index].quantity_units, aggregate.quantity);
      EXPECT_EQ(actual.asks[index].order_count, aggregate.count);
      ++index;
    }
    EXPECT_EQ(engine->live_order_count(), reference.size());
    EXPECT_EQ(engine->version(), sequence);
    EXPECT_EQ(engine->last_valid_sequence(), sequence);
  }
}

TEST(OrderBookPropertyTest, RandomizedPriceLevelsRemainSortedAndMatchReference) {
  auto engine =
      std::make_unique<book::OrderBook>(make_config(book::BookMode::price_level));
  std::map<std::int64_t, ReferenceAggregate> bids;
  std::map<std::int64_t, ReferenceAggregate> asks;
  SplitMix64 random{kDeterministicSeed ^ 0x4c45'5645'4cULL};

  for (std::uint64_t sequence = 1U; sequence <= 1'000U; ++sequence) {
    const auto side =
        random.bounded(2U) == 0U ? synthetic::Side::bid : synthetic::Side::ask;
    const auto price = side == synthetic::Side::bid
                           ? 90 + static_cast<std::int64_t>(random.bounded(10U))
                           : 101 + static_cast<std::int64_t>(random.bounded(10U));
    auto& levels = side == synthetic::Side::bid ? bids : asks;
    book::BookUpdate update{.side = side,
                            .order_id = {},
                            .price_ticks = price,
                            .sequence = sequence,
                            .process_monotonic_time_ns = sequence};
    const auto existing = levels.find(price);
    if (existing != levels.end() && random.bounded(4U) == 0U) {
      update.operation = book::BookOperation::delete_level;
      levels.erase(existing);
    } else {
      const auto aggregate = ReferenceAggregate{
          .quantity = 1U + random.bounded(10'000U),
          .count = static_cast<std::uint32_t>(1U + random.bounded(100U))};
      update.operation = book::BookOperation::set_level;
      update.level_quantity_units = aggregate.quantity;
      update.order_count = aggregate.count;
      levels[price] = aggregate;
    }
    ASSERT_TRUE(engine->apply(update).accepted())
        << "seed=" << kDeterministicSeed << " sequence=" << sequence;
    ASSERT_TRUE(engine->check_invariants().valid());
    book::DepthSnapshot actual{};
    ASSERT_EQ(engine->depth(actual, 32U), book::ApplyError::none);
    ASSERT_EQ(actual.bid_count, bids.size());
    ASSERT_EQ(actual.ask_count, asks.size());
  }
}
// NOLINTEND(readability-function-cognitive-complexity)

} // namespace
