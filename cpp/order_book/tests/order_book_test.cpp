#include "aegis/order_book/book_universe.hpp"
#include "aegis/order_book/synthetic_adapter.hpp"

#include "aegis/market_data/synthetic/event.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <thread>

namespace book = aegis::order_book;
namespace synthetic = aegis::market_data::synthetic;

namespace {

constexpr aegis::common::SessionId kSession{1U, 1U};
constexpr aegis::common::SessionId kNextSession{1U, 2U};
constexpr aegis::common::VenueId kVenueOne{10U, 1U};
constexpr aegis::common::VenueId kVenueTwo{10U, 2U};
constexpr aegis::common::InstrumentId kInstrument{20U, 1U};
constexpr aegis::common::InstrumentId kOtherInstrument{20U, 2U};

[[nodiscard]] constexpr book::BookIdentity
identity(const aegis::common::VenueId venue = kVenueOne,
         const aegis::common::InstrumentId instrument = kInstrument,
         const std::uint32_t venue_number = 1U,
         const std::uint32_t instrument_number = 1U) {
  return {.venue_id = venue,
          .instrument_id = instrument,
          .venue_number = venue_number,
          .instrument_number = instrument_number};
}

[[nodiscard]] book::BookConfig
config(const book::BookMode mode = book::BookMode::order_by_order,
       const book::BookIdentity book_identity = identity()) {
  return {.identity = book_identity,
          .session_id = kSession,
          .mode = mode,
          .order_capacity = 32U,
          .level_capacity = 16U,
          .published_depth = 8U,
          .maximum_sequence = 1'000U,
          .permit_crossed_transition = false,
          .require_contiguous_sequence = true,
          .start_recovering = false};
}

[[nodiscard]] constexpr aegis::common::OrderId order_id(const std::uint64_t value) {
  return {99U, value};
}

[[nodiscard]] book::BookUpdate add(const std::uint64_t sequence, const std::uint64_t id,
                                   const synthetic::Side side, const std::int64_t price,
                                   const std::uint64_t quantity) {
  return {.operation = book::BookOperation::add,
          .side = side,
          .order_id = order_id(id),
          .price_ticks = price,
          .quantity_units = quantity,
          .sequence = sequence,
          .process_monotonic_time_ns = sequence};
}

[[nodiscard]] book::BookUpdate order_operation(const book::BookOperation operation,
                                               const std::uint64_t sequence,
                                               const std::uint64_t id,
                                               const std::uint64_t quantity = 0U) {
  return {.operation = operation,
          .order_id = order_id(id),
          .quantity_units = quantity,
          .sequence = sequence,
          .process_monotonic_time_ns = sequence};
}

// GoogleTest macros intentionally expand into control flow.
// NOLINTBEGIN(readability-function-cognitive-complexity)

TEST(OrderBookTest, AppliesOrderLifecycleAndPreservesQueuePriority) {
  auto engine = std::make_unique<book::OrderBook>(config());
  EXPECT_TRUE(engine->apply(add(1U, 1U, synthetic::Side::bid, 100, 10U)).accepted());
  EXPECT_TRUE(engine->apply(add(2U, 2U, synthetic::Side::bid, 100, 20U)).accepted());
  ASSERT_NE(engine->find_level(synthetic::Side::bid, 100), nullptr);
  EXPECT_EQ(engine->find_level(synthetic::Side::bid, 100)->quantity_units, 30U);
  EXPECT_EQ(engine->find_level(synthetic::Side::bid, 100)->order_count, 2U);

  EXPECT_TRUE(
      engine->apply(order_operation(book::BookOperation::partial_cancel, 3U, 1U, 4U))
          .accepted());
  ASSERT_NE(engine->find_order(order_id(1U)), nullptr);
  EXPECT_EQ(engine->find_order(order_id(1U))->quantity_units, 6U);

  auto replace = add(4U, 1U, synthetic::Side::bid, 100, 12U);
  replace.operation = book::BookOperation::replace;
  EXPECT_TRUE(engine->apply(replace).accepted());
  EXPECT_GT(engine->find_order(order_id(1U))->priority,
            engine->find_order(order_id(2U))->priority);

  book::BookSnapshot snapshot{};
  EXPECT_EQ(engine->make_snapshot(snapshot), book::ApplyError::none);
  ASSERT_EQ(snapshot.order_count, 2U);
  EXPECT_EQ(snapshot.orders[0U].order_id, order_id(2U));
  EXPECT_EQ(snapshot.orders[1U].order_id, order_id(1U));

  EXPECT_TRUE(
      engine->apply(order_operation(book::BookOperation::partial_execute, 5U, 2U, 5U))
          .accepted());
  EXPECT_TRUE(engine->apply(order_operation(book::BookOperation::execute, 6U, 2U, 15U))
                  .accepted());
  EXPECT_EQ(engine->find_order(order_id(2U)), nullptr);
  EXPECT_TRUE(engine->apply(order_operation(book::BookOperation::delete_order, 7U, 1U))
                  .accepted());
  EXPECT_EQ(engine->live_order_count(), 0U);
  EXPECT_TRUE(engine->check_invariants().valid());
}

TEST(OrderBookTest, MaintainsSortedDepthAndTopCache) {
  auto engine = std::make_unique<book::OrderBook>(config());
  ASSERT_TRUE(engine->apply(add(1U, 1U, synthetic::Side::bid, 98, 3U)).accepted());
  ASSERT_TRUE(engine->apply(add(2U, 2U, synthetic::Side::ask, 104, 4U)).accepted());
  auto result = engine->apply(add(3U, 3U, synthetic::Side::bid, 100, 5U));
  EXPECT_TRUE(result.accepted());
  EXPECT_TRUE(result.top_changed);
  ASSERT_TRUE(engine->apply(add(4U, 4U, synthetic::Side::ask, 102, 6U)).accepted());
  EXPECT_EQ(engine->top().bid.price_ticks, 100);
  EXPECT_EQ(engine->top().ask.price_ticks, 102);

  book::DepthSnapshot depth{};
  EXPECT_EQ(engine->depth(depth, 8U), book::ApplyError::none);
  ASSERT_EQ(depth.bid_count, 2U);
  ASSERT_EQ(depth.ask_count, 2U);
  EXPECT_EQ(depth.bids[0U].price_ticks, 100);
  EXPECT_EQ(depth.bids[1U].price_ticks, 98);
  EXPECT_EQ(depth.asks[0U].price_ticks, 102);
  EXPECT_EQ(depth.asks[1U].price_ticks, 104);
  EXPECT_EQ(depth.version, 4U);
  EXPECT_TRUE(engine->check_invariants().valid());
}

TEST(OrderBookTest, ReplaceCrossCheckExcludesTheOrderBeingMoved) {
  auto engine = std::make_unique<book::OrderBook>(config());
  ASSERT_TRUE(engine->apply(add(1U, 1U, synthetic::Side::bid, 100, 10U)).accepted());
  auto replace = add(2U, 1U, synthetic::Side::ask, 99, 10U);
  replace.operation = book::BookOperation::replace;
  EXPECT_TRUE(engine->apply(replace).accepted());
  EXPECT_FALSE(engine->top().has_bid);
  ASSERT_TRUE(engine->top().has_ask);
  EXPECT_EQ(engine->top().ask.price_ticks, 99);
  EXPECT_TRUE(engine->check_invariants().valid());
}

TEST(OrderBookTest, SupportsAggregatedLevelsClearAndSequenceWrap) {
  auto level_config = config(book::BookMode::price_level);
  level_config.maximum_sequence = 3U;
  auto engine = std::make_unique<book::OrderBook>(level_config);
  book::BookUpdate update{
      .operation = book::BookOperation::set_level,
      .side = synthetic::Side::bid,
      .order_id = {},
      .price_ticks = 100,
      .level_quantity_units = 50U,
      .order_count = 2U,
      .sequence = 1U,
  };
  EXPECT_TRUE(engine->apply(update).accepted());
  update.sequence = 2U;
  update.level_quantity_units = 75U;
  update.order_count = 3U;
  EXPECT_TRUE(engine->apply(update).accepted());
  update.sequence = 3U;
  update.operation = book::BookOperation::clear_side;
  EXPECT_TRUE(engine->apply(update).accepted());
  EXPECT_EQ(engine->bid_level_count(), 0U);
  update.sequence = 1U;
  update.operation = book::BookOperation::set_level;
  update.level_quantity_units = 9U;
  update.order_count = 1U;
  EXPECT_TRUE(engine->apply(update).accepted());
  EXPECT_EQ(engine->last_valid_sequence(), 1U);
}

TEST(OrderBookTest, FailsClosedOnMalformedDuplicateGapAndCrossedUpdates) {
  {
    auto engine = std::make_unique<book::OrderBook>(config());
    ASSERT_TRUE(engine->apply(add(1U, 1U, synthetic::Side::ask, 101, 5U)).accepted());
    const auto crossed = engine->apply(add(2U, 2U, synthetic::Side::bid, 102, 5U));
    EXPECT_EQ(crossed.error, book::ApplyError::crossed_book);
    EXPECT_EQ(engine->validity(), book::BookValidity::invalid);
    EXPECT_EQ(engine->live_order_count(), 1U);
  }
  {
    auto engine = std::make_unique<book::OrderBook>(config());
    ASSERT_TRUE(engine->apply(add(1U, 1U, synthetic::Side::bid, 100, 5U)).accepted());
    const auto duplicate = engine->apply(add(2U, 1U, synthetic::Side::bid, 99, 5U));
    EXPECT_EQ(duplicate.error, book::ApplyError::duplicate_order_id);
    EXPECT_EQ(engine->version(), 1U);
  }
  {
    auto engine = std::make_unique<book::OrderBook>(config());
    EXPECT_EQ(engine->apply(add(2U, 1U, synthetic::Side::bid, 100, 5U)).error,
              book::ApplyError::sequence_gap);
    EXPECT_EQ(engine->last_valid_sequence(), 0U);
  }
  {
    auto engine = std::make_unique<book::OrderBook>(config());
    auto malformed = add(1U, 1U, synthetic::Side::bid, 0, 5U);
    EXPECT_EQ(engine->apply(malformed).error, book::ApplyError::invalid_price);
  }
}

TEST(OrderBookTest, SnapshotLoadIsIntegrityCheckedAndAtomic) {
  auto source = std::make_unique<book::OrderBook>(config());
  ASSERT_TRUE(source->apply(add(1U, 1U, synthetic::Side::bid, 100, 10U)).accepted());
  ASSERT_TRUE(source->apply(add(2U, 2U, synthetic::Side::ask, 102, 20U)).accepted());
  book::BookSnapshot snapshot{};
  ASSERT_EQ(source->make_snapshot(snapshot), book::ApplyError::none);

  auto recovered_config = config();
  recovered_config.start_recovering = true;
  auto recovered = std::make_unique<book::OrderBook>(recovered_config);
  const auto loaded = recovered->load_snapshot(snapshot);
  EXPECT_TRUE(loaded.accepted());
  EXPECT_EQ(recovered->validity(), book::BookValidity::valid);
  EXPECT_EQ(recovered->top().bid.price_ticks, 100);
  EXPECT_EQ(recovered->top().ask.price_ticks, 102);
  EXPECT_TRUE(recovered->check_invariants().valid());

  ++snapshot.bids[0U].quantity_units;
  auto rejected = std::make_unique<book::OrderBook>(recovered_config);
  EXPECT_EQ(rejected->load_snapshot(snapshot).error,
            book::ApplyError::snapshot_hash_mismatch);
  EXPECT_EQ(rejected->live_order_count(), 0U);
}

TEST(OrderBookTest, ResetsForSessionsAndCorporateActionsWithoutPriceAdjustment) {
  auto engine = std::make_unique<book::OrderBook>(config());
  ASSERT_TRUE(engine->apply(add(1U, 1U, synthetic::Side::bid, 100, 10U)).accepted());
  EXPECT_TRUE(engine->session_reset(kNextSession).accepted());
  EXPECT_EQ(engine->session_id(), kNextSession);
  EXPECT_EQ(engine->validity(), book::BookValidity::recovering);
  EXPECT_EQ(engine->live_order_count(), 0U);
  EXPECT_EQ(engine->last_valid_sequence(), 0U);

  ASSERT_TRUE(engine->complete_recovery(0U).accepted());
  ASSERT_TRUE(engine->apply(add(1U, 2U, synthetic::Side::ask, 102, 3U)).accepted());
  EXPECT_TRUE(engine
                  ->corporate_action_reset(aegis::common::SessionId{1U, 3U},
                                           aegis::common::ConfigurationVersion{7U, 1U})
                  .accepted());
  EXPECT_EQ(engine->validity(), book::BookValidity::recovering);
  EXPECT_EQ(engine->live_order_count(), 0U);
  EXPECT_EQ(engine->trading_status(), synthetic::TradingStatus::pre_open);
}

TEST(OrderBookTest, MaintainsTradingAuctionAndTapeStateWhileHaltsOverrideUpdates) {
  auto engine = std::make_unique<book::OrderBook>(config());
  book::BookUpdate status{.operation = book::BookOperation::set_trading_status,
                          .trading_status = synthetic::TradingStatus::auction,
                          .order_id = {},
                          .sequence = 1U};
  ASSERT_TRUE(engine->apply(status).accepted());
  const book::BookUpdate auction{.operation =
                                     book::BookOperation::set_auction_imbalance,
                                 .side = synthetic::Side::bid,
                                 .order_id = {},
                                 .price_ticks = 100,
                                 .quantity_units = 500U,
                                 .paired_quantity_units = 700U,
                                 .sequence = 2U};
  ASSERT_TRUE(engine->apply(auction).accepted());
  EXPECT_TRUE(engine->auction_imbalance().active);
  EXPECT_EQ(engine->auction_imbalance().paired_quantity_units, 700U);
  status.sequence = 3U;
  status.trading_status = synthetic::TradingStatus::open;
  ASSERT_TRUE(engine->apply(status).accepted());
  EXPECT_FALSE(engine->auction_imbalance().active);
  const book::BookUpdate trade{.operation = book::BookOperation::trade,
                               .side = synthetic::Side::ask,
                               .order_id = {},
                               .price_ticks = 101,
                               .quantity_units = 8U,
                               .venue_trade_id = 44U,
                               .sequence = 4U};
  ASSERT_TRUE(engine->apply(trade).accepted());
  EXPECT_TRUE(engine->last_trade().present);
  EXPECT_EQ(engine->live_order_count(), 0U);
  status.sequence = 5U;
  status.trading_status = synthetic::TradingStatus::halted;
  ASSERT_TRUE(engine->apply(status).accepted());
  EXPECT_EQ(engine->apply(add(6U, 1U, synthetic::Side::bid, 100, 1U)).error,
            book::ApplyError::update_while_halted);
}

TEST(BookUniverseTest, EnforcesVenueScopeAndBuildsConsolidatedDepth) {
  book::BookUniverse universe{4U};
  const auto first = identity(kVenueOne, kInstrument, 1U, 1U);
  const auto second = identity(kVenueTwo, kInstrument, 2U, 1U);
  const auto other = identity(kVenueOne, kOtherInstrument, 1U, 2U);
  ASSERT_EQ(universe.register_book(config(book::BookMode::order_by_order, first)),
            book::ApplyError::none);
  ASSERT_EQ(universe.register_book(config(book::BookMode::order_by_order, second)),
            book::ApplyError::none);
  ASSERT_EQ(universe.register_book(config(book::BookMode::order_by_order, other)),
            book::ApplyError::none);
  ASSERT_TRUE(
      universe.apply(first, add(1U, 1U, synthetic::Side::bid, 100, 10U)).accepted());
  ASSERT_TRUE(
      universe.apply(first, add(2U, 2U, synthetic::Side::ask, 104, 10U)).accepted());
  ASSERT_TRUE(
      universe.apply(second, add(1U, 1U, synthetic::Side::bid, 101, 20U)).accepted());
  ASSERT_TRUE(
      universe.apply(second, add(2U, 2U, synthetic::Side::ask, 103, 20U)).accepted());
  EXPECT_EQ(universe.apply(other, add(1U, 1U, synthetic::Side::bid, 99, 1U)).error,
            book::ApplyError::duplicate_order_id);

  book::ConsolidatedDepth consolidated{};
  EXPECT_EQ(universe.consolidated_depth(kInstrument, consolidated, 4U),
            book::ApplyError::none);
  EXPECT_TRUE(consolidated.valid);
  EXPECT_EQ(consolidated.contributing_venues, 2U);
  EXPECT_EQ(consolidated.bids[0U].price_ticks, 101);
  EXPECT_EQ(consolidated.asks[0U].price_ticks, 103);
}

TEST(OrderBookTest, RejectsCrossThreadMutationAndQuery) {
  auto engine = std::make_unique<book::OrderBook>(config());
  book::ApplyError mutation = book::ApplyError::none;
  const book::OrderState sentinel{};
  const book::OrderState* query = &sentinel;
  std::thread worker([&] {
    mutation = engine->apply(add(1U, 1U, synthetic::Side::bid, 100, 1U)).error;
    query = engine->find_order(order_id(1U));
  });
  worker.join();
  EXPECT_EQ(mutation, book::ApplyError::wrong_thread);
  EXPECT_EQ(query, nullptr);
  EXPECT_EQ(engine->validity(), book::BookValidity::valid);
}

TEST(SyntheticAdapterTest, MapsNativeOrderLevelAndRejectsBadQuality) {
  synthetic::SyntheticEvent event{
      .type = synthetic::NativeMessageType::add_order,
      .side = synthetic::Side::bid,
      .action = synthetic::BookAction::add,
      .status = synthetic::TradingStatus::none,
      .data_quality = synthetic::DataQuality::valid,
      .message_flags = synthetic::kMessageFlagOrderLevel,
      .venue_number = 1U,
      .channel_number = 1U,
      .instrument_number = 2U,
      .channel_sequence = 1U,
      .global_ordinal = 1U,
      .exchange_event_time_ns = 100,
      .nic_receive_time_ns = 101,
      .process_monotonic_time_ns = 1U,
      .synthetic_order_id = 9U,
      .price_ticks = 100,
      .quantity_units = 7U,
      .level_quantity_units = 7U,
      .order_count = 1U,
  };
  event.event_hash = synthetic::calculate_event_hash(event);
  book::BookUpdate update{};
  EXPECT_EQ(book::synthetic_update(event, book::BookMode::order_by_order, update),
            book::ApplyError::none);
  EXPECT_EQ(update.operation, book::BookOperation::add);
  EXPECT_TRUE(update.verify_aggregate);
  EXPECT_EQ(update.order_id.low(), 9U);

  auto adapter_config = config(
      book::BookMode::order_by_order,
      identity(kVenueOne, kInstrument, event.venue_number, event.instrument_number));
  adapter_config.require_contiguous_sequence = false;
  auto engine = std::make_unique<book::OrderBook>(adapter_config);
  event.channel_sequence = 5U;
  event.event_hash = synthetic::calculate_event_hash(event);
  EXPECT_TRUE(book::apply_synthetic(*engine, kSession, event).accepted());
  EXPECT_EQ(engine->last_valid_sequence(), 5U);

  auto quote = event;
  quote.type = synthetic::NativeMessageType::quote;
  quote.side = synthetic::Side::none;
  quote.action = synthetic::BookAction::none;
  quote.message_flags = 0U;
  quote.channel_sequence = 8U;
  quote.synthetic_order_id = 0U;
  quote.quantity_units = 11U;
  quote.level_quantity_units = 12U;
  quote.order_count = 0U;
  quote.auxiliary_value = 102U;
  quote.event_hash = synthetic::calculate_event_hash(quote);
  EXPECT_TRUE(book::apply_synthetic(*engine, kSession, quote).accepted());
  EXPECT_EQ(engine->last_valid_sequence(), 8U);

  event.data_quality = synthetic::DataQuality::invalid;
  EXPECT_EQ(book::synthetic_update(event, book::BookMode::order_by_order, update),
            book::ApplyError::invalid_operation);

  auto unsafe = std::make_unique<book::OrderBook>(adapter_config);
  EXPECT_EQ(book::apply_synthetic(*unsafe, kSession, event).error,
            book::ApplyError::invalid_operation);
  EXPECT_EQ(unsafe->validity(), book::BookValidity::invalid);
}

// NOLINTEND(readability-function-cognitive-complexity)

} // namespace
