#include "aegis/execution/router.hpp"
#include "aegis/execution/simulated_gateway.hpp"

#include "test_support.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <type_traits>

namespace aegis::execution::test {
namespace {

inline constexpr common::VenueId kSecondVenue{4U, 5U};
inline constexpr std::uint32_t kAllPolicyMask =
    (std::uint32_t{1U} << static_cast<std::uint8_t>(
         ExecutionPolicy::auction_participation)) -
    1U;

[[nodiscard]] RouterConfiguration
router_configuration(const std::uint16_t venue_count = 2U) noexcept {
  RouterConfiguration result{.configuration_version = risk::test::kConfiguration,
                             .maximum_quote_age_ns = 1'000U,
                             .maximum_route_latency_ns = 1'000U,
                             .queue_deterioration_threshold_units = 5U,
                             .uncertainty_penalty_currency_nanos_per_unit = 2U,
                             .maximum_reject_rate_ppm = 200'000U,
                             .minimum_fill_probability_ppm = 100'000U,
                             .maximum_direct_consolidated_divergence_ticks = 0U,
                             .maximum_route_attempts = 3U,
                             .venue_count = venue_count};
  result.venues[0U] = {.venue_id = risk::test::kVenue,
                       .allowed_policy_mask = kAllPolicyMask,
                       .maximum_concentration_ppm = kRouterPartsPerMillion,
                       .maximum_child_quantity_units = 1'000U,
                       .tie_break_rank = 1U,
                       .enabled = true,
                       .regulatory_authorized = true,
                       .require_self_trade_clear = true,
                       .allow_auction = true};
  result.venues[1U] = {.venue_id = kSecondVenue,
                       .allowed_policy_mask = kAllPolicyMask,
                       .maximum_concentration_ppm = kRouterPartsPerMillion,
                       .maximum_child_quantity_units = 1'000U,
                       .tie_break_rank = 2U,
                       .enabled = true,
                       .regulatory_authorized = true,
                       .require_self_trade_clear = true,
                       .allow_auction = true};
  result.stable_hash = stable_router_configuration_hash(result);
  return result;
}

[[nodiscard]] ExecutionObjective
objective(const ExecutionPolicy policy = ExecutionPolicy::passive_join,
          const std::uint64_t ordinal = 1U) noexcept {
  ExecutionObjective result{.objective_id = common::IntentId{100U, ordinal},
                            .session_id = risk::test::kSession,
                            .account_id = risk::test::kAccount,
                            .strategy_id = risk::test::kStrategy,
                            .instrument_id = risk::test::kInstrument,
                            .source_forecast_id = common::ForecastId{101U, ordinal},
                            .feature_snapshot_id =
                                common::FeatureSnapshotId{102U, ordinal},
                            .configuration_version = risk::test::kConfiguration,
                            .target_order_id = {},
                            .side = risk::IntentAction::buy,
                            .policy = policy,
                            .limit_price_ticks = 105,
                            .total_quantity_units = 100U,
                            .filled_quantity_units = 0U,
                            .maximum_child_quantity_units = 10U,
                            .start_process_monotonic_time_ns = kNow - 100U,
                            .end_process_monotonic_time_ns = kNow + 10'000U,
                            .slice_interval_ns = 100U,
                            .alpha_half_life_ns = 10'000U,
                            .expected_alpha_microticks = 2'000'000,
                            .tick_value_currency_nanos = 100U,
                            .participation_rate_ppm = 250'000U,
                            .has_limit_price = true};
  result.stable_hash = stable_execution_objective_hash(result);
  return result;
}

[[nodiscard]] VenueObservation
observation(const common::VenueId venue_id = risk::test::kVenue,
            const std::int64_t bid_ticks = 99,
            const std::int64_t ask_ticks = 101) noexcept {
  VenueObservation result{.venue_id = venue_id,
                          .instrument_id = risk::test::kInstrument,
                          .trading_state = VenueTradingState::open,
                          .self_trade_prevention = SelfTradePreventionState::clear,
                          .bid_price_ticks = bid_ticks,
                          .ask_price_ticks = ask_ticks,
                          .bid_quantity_units = 100U,
                          .ask_quantity_units = 100U,
                          .estimated_hidden_quantity_units = 20U,
                          .queue_ahead_quantity_units = 2U,
                          .venue_latency_ns = 100U,
                          .observed_process_monotonic_time_ns = kNow - 10U,
                          .maker_fee_currency_nanos_per_unit = -1,
                          .taker_fee_currency_nanos_per_unit = 3,
                          .adverse_selection_microticks = 100'000,
                          .fill_probability_ppm = 900'000U,
                          .reject_rate_ppm = 1'000U,
                          .quote_valid = true,
                          .regulatory_eligible = true};
  result.stable_hash = stable_venue_observation_hash(result);
  return result;
}

[[nodiscard]] ConsolidatedQuote
consolidated(const common::VenueId bid_venue = risk::test::kVenue,
             const common::VenueId ask_venue = risk::test::kVenue,
             const std::int64_t bid_ticks = 99,
             const std::int64_t ask_ticks = 101) noexcept {
  ConsolidatedQuote result{.best_bid_venue_id = bid_venue,
                           .best_ask_venue_id = ask_venue,
                           .best_bid_price_ticks = bid_ticks,
                           .best_ask_price_ticks = ask_ticks,
                           .observed_process_monotonic_time_ns = kNow - 10U,
                           .valid = true};
  result.stable_hash = stable_consolidated_quote_hash(result);
  return result;
}

[[nodiscard]] RoutingRequest
routing_request(const ExecutionPolicy policy = ExecutionPolicy::passive_join,
                const std::uint16_t venue_count = 1U,
                const std::uint64_t ordinal = 1U) noexcept {
  RoutingRequest result{.objective = objective(policy, ordinal),
                        .consolidated_quote = consolidated(),
                        .working_order = {},
                        .now_process_monotonic_time_ns = kNow,
                        .market_volume_since_last_slice_units = 100U,
                        .target_cumulative_volume_curve_ppm = 500'000U,
                        .venue_count = venue_count};
  result.venues[0U] = observation();
  result.venues[1U] = observation(kSecondVenue);
  result.stable_hash = stable_routing_request_hash(result);
  return result;
}

void rehash(VenueObservation& value) noexcept {
  value.stable_hash = stable_venue_observation_hash(value);
}

void rehash(ExecutionObjective& value) noexcept {
  value.stable_hash = stable_execution_objective_hash(value);
}

void rehash(RoutingRequest& value) noexcept {
  value.stable_hash = stable_routing_request_hash(value);
}

class RouterPaperGatewayTest : public ::testing::Test {
protected:
  RouterPaperGatewayTest()
      : gateway_configuration_(configuration(GatewayMode::paper)),
        gateway_(gateway_configuration_, gateway_journal_) {}

  void SetUp() override { ASSERT_TRUE(gateway_.start(kNow - 100U)); }

  [[nodiscard]] GatewaySubmitResult
  submit(const RoutingDecision& decision, const std::uint64_t ordinal,
         const oms::ExternalOrderId external_order_id = {}) {
    EXPECT_TRUE(valid_routing_decision(decision));
    EXPECT_TRUE(decision.requires_fresh_pretrade_risk);
    EXPECT_EQ(decision.venue_id, risk::test::kVenue);
    auto kind = oms::GatewayCommandKind::new_order;
    if (decision.action == RoutedAction::cancel) {
      kind = oms::GatewayCommandKind::cancel;
    } else if (decision.action == RoutedAction::replace) {
      kind = oms::GatewayCommandKind::replace;
    }
    const auto order_id = decision.target_order_id.valid()
                              ? decision.target_order_id
                              : common::OrderId{200U, ordinal};
    // The test builder invokes the real deterministic risk engine. The router
    // decision itself is not accepted by IExchangeGateway and is never reused
    // as risk approval.
    auto gateway_request =
        request(GatewayMode::paper, kind, ordinal, kNow + ordinal, order_id,
                external_order_id, decision.price_ticks, decision.quantity_units);
    EXPECT_EQ(gateway_request.risk_decision.decision, risk::DecisionCode::approved);
    if (kind == oms::GatewayCommandKind::cancel) {
      return gateway_.cancel_order(gateway_request);
    }
    if (kind == oms::GatewayCommandKind::replace) {
      return gateway_.replace_order(gateway_request);
    }
    return gateway_.send_order(gateway_request);
  }

  [[nodiscard]] GatewayEvent open_working_order(const common::OrderId order_id,
                                                const std::uint64_t ordinal,
                                                const std::int64_t price_ticks) {
    const auto new_order =
        request(GatewayMode::paper, oms::GatewayCommandKind::new_order, ordinal,
                kNow + ordinal, order_id, {}, price_ticks, 10U);
    EXPECT_EQ(gateway_.send_order(new_order).status, GatewaySubmitStatus::accepted);
    GatewayEvent acknowledgement{};
    EXPECT_EQ(gateway_.poll_event(kNow + ordinal + 100U, acknowledgement),
              GatewayPollStatus::event);
    EXPECT_EQ(acknowledgement.kind, GatewayResponseKind::acknowledgement);
    return acknowledgement;
  }

  // Test fixtures expose paper components to individual scenario bodies.
  // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
  GatewayConfiguration gateway_configuration_;
  GatewayAuditJournal gateway_journal_;
  PaperBrokerGateway gateway_;
  // NOLINTEND(misc-non-private-member-variables-in-classes)
};

static_assert(!std::is_convertible_v<RoutingDecision, GatewayRequest>);

} // namespace

TEST_F(RouterPaperGatewayTest, BestPriceUnavailableFallsBackToEligibleVenue) {
  auto value = routing_request(ExecutionPolicy::aggressive_take, 2U);
  value.consolidated_quote = consolidated(risk::test::kVenue, kSecondVenue, 99, 100);
  value.venues[1U] = observation(kSecondVenue, 98, 100);
  value.venues[1U].quote_valid = false;
  rehash(value.venues[1U]);
  rehash(value);
  SmartOrderRouter router{router_configuration()};
  const auto decision = router.route(value);
  ASSERT_EQ(decision.status, RoutingStatus::routed);
  EXPECT_EQ(decision.venue_id, risk::test::kVenue);
  EXPECT_EQ(decision.venue_scores[1U].eligibility,
            VenueEligibilityReason::quote_invalid);
  EXPECT_EQ(submit(decision, 10U).status, GatewaySubmitStatus::accepted);
}

TEST_F(RouterPaperGatewayTest, HaltedAndStaleVenuesAreHardExcluded) {
  auto halted = routing_request(ExecutionPolicy::aggressive_take, 2U);
  halted.consolidated_quote = consolidated(risk::test::kVenue, kSecondVenue, 99, 100);
  halted.venues[1U] = observation(kSecondVenue, 98, 100);
  halted.venues[1U].trading_state = VenueTradingState::halted;
  rehash(halted.venues[1U]);
  rehash(halted);
  SmartOrderRouter first_router{router_configuration()};
  const auto first = first_router.route(halted);
  ASSERT_EQ(first.status, RoutingStatus::routed);
  EXPECT_EQ(first.venue_scores[1U].eligibility,
            VenueEligibilityReason::trading_state_ineligible);
  EXPECT_EQ(submit(first, 11U).status, GatewaySubmitStatus::accepted);

  auto stale = halted;
  stale.objective = objective(ExecutionPolicy::aggressive_take, 12U);
  stale.venues[1U] = observation(kSecondVenue, 98, 100);
  stale.venues[1U].observed_process_monotonic_time_ns = kNow - 2'000U;
  rehash(stale.venues[1U]);
  rehash(stale);
  SmartOrderRouter second_router{router_configuration()};
  const auto second = second_router.route(stale);
  ASSERT_EQ(second.status, RoutingStatus::routed);
  EXPECT_EQ(second.venue_scores[1U].eligibility, VenueEligibilityReason::quote_stale);
  EXPECT_EQ(submit(second, 12U).status, GatewaySubmitStatus::accepted);
}

TEST_F(RouterPaperGatewayTest, QueueDeteriorationProducesReplaceForWorkingOrder) {
  const common::OrderId order_id{300U, 1U};
  const auto acknowledgement = open_working_order(order_id, 20U, 98);
  auto value = routing_request(ExecutionPolicy::cancel_replace, 1U, 20U);
  value.objective.target_order_id = order_id;
  rehash(value.objective);
  value.working_order = {.order_id = order_id,
                         .venue_id = risk::test::kVenue,
                         .price_ticks = 98,
                         .remaining_quantity_units = 10U,
                         .initial_queue_ahead_quantity_units = 2U,
                         .present = true};
  value.venues[0U].queue_ahead_quantity_units = 20U;
  rehash(value.venues[0U]);
  rehash(value);
  SmartOrderRouter router{router_configuration(1U)};
  const auto decision = router.route(value);
  ASSERT_EQ(decision.status, RoutingStatus::routed);
  EXPECT_EQ(decision.action, RoutedAction::replace);
  EXPECT_EQ(decision.target_order_id, order_id);
  EXPECT_EQ(decision.price_ticks, 100);
  EXPECT_EQ(submit(decision, 21U, acknowledgement.external_order_id).status,
            GatewaySubmitStatus::accepted);
}

TEST_F(RouterPaperGatewayTest, QueueDeteriorationCanProduceProtectiveCancel) {
  const common::OrderId order_id{300U, 2U};
  const auto acknowledgement = open_working_order(order_id, 30U, 98);
  auto value = routing_request(ExecutionPolicy::cancel_replace, 1U, 30U);
  value.objective.target_order_id = order_id;
  value.objective.limit_price_ticks = 98;
  rehash(value.objective);
  value.working_order = {.order_id = order_id,
                         .venue_id = risk::test::kVenue,
                         .price_ticks = 98,
                         .remaining_quantity_units = 10U,
                         .initial_queue_ahead_quantity_units = 2U,
                         .present = true};
  value.venues[0U].queue_ahead_quantity_units = 20U;
  rehash(value.venues[0U]);
  rehash(value);
  SmartOrderRouter router{router_configuration(1U)};
  const auto decision = router.route(value);
  ASSERT_EQ(decision.status, RoutingStatus::routed);
  EXPECT_EQ(decision.action, RoutedAction::cancel);
  EXPECT_EQ(submit(decision, 31U, acknowledgement.external_order_id).status,
            GatewaySubmitStatus::accepted);
}

TEST_F(RouterPaperGatewayTest, PartialFillRoutesOnlyRemainingObjectiveQuantity) {
  auto value = routing_request(ExecutionPolicy::aggressive_take, 1U, 40U);
  value.objective.total_quantity_units = 10U;
  value.objective.filled_quantity_units = 7U;
  value.objective.maximum_child_quantity_units = 10U;
  rehash(value.objective);
  rehash(value);
  SmartOrderRouter router{router_configuration(1U)};
  const auto decision = router.route(value);
  ASSERT_EQ(decision.quantity_units, 3U);
  ASSERT_EQ(submit(decision, 40U).status, GatewaySubmitStatus::accepted);
  GatewayEvent event{};
  ASSERT_EQ(gateway_.poll_event(kNow + 200U, event), GatewayPollStatus::event);
  ASSERT_EQ(gateway_.on_market_event(status_event(
                market_data::synthetic::TradingStatus::open, 1U, kNow + 250U)),
            GatewayReason::accepted);
  ASSERT_EQ(gateway_.on_market_event(trade_event(2U, kNow + 300U, 100, 10U)),
            GatewayReason::accepted);
  ASSERT_EQ(gateway_.poll_event(kNow + 300U, event), GatewayPollStatus::event);
  EXPECT_EQ(event.kind, GatewayResponseKind::fill);
  EXPECT_EQ(event.quantity_units, 3U);
  EXPECT_EQ(event.remaining_quantity_units, 0U);
}

TEST_F(RouterPaperGatewayTest, ConflictingDirectAndConsolidatedDataFailsClosed) {
  auto value = routing_request(ExecutionPolicy::aggressive_take, 2U, 50U);
  value.consolidated_quote = consolidated(risk::test::kVenue, kSecondVenue, 99, 100);
  value.venues[1U] = observation(kSecondVenue, 98, 101);
  rehash(value);
  SmartOrderRouter router{router_configuration()};
  const auto decision = router.route(value);
  ASSERT_EQ(decision.status, RoutingStatus::routed);
  EXPECT_EQ(decision.venue_id, risk::test::kVenue);
  EXPECT_EQ(decision.venue_scores[1U].eligibility,
            VenueEligibilityReason::direct_consolidated_conflict);
  EXPECT_EQ(submit(decision, 50U).status, GatewaySubmitStatus::accepted);
}

TEST_F(RouterPaperGatewayTest, RejectBurstAndVenueConcentrationAreHardLimits) {
  auto reject = routing_request(ExecutionPolicy::aggressive_take, 2U, 60U);
  reject.consolidated_quote = consolidated(risk::test::kVenue, kSecondVenue, 99, 100);
  reject.venues[1U] = observation(kSecondVenue, 98, 100);
  reject.venues[1U].reject_rate_ppm = 300'000U;
  rehash(reject.venues[1U]);
  rehash(reject);
  SmartOrderRouter first_router{router_configuration()};
  const auto first = first_router.route(reject);
  ASSERT_EQ(first.status, RoutingStatus::routed);
  EXPECT_EQ(first.venue_scores[1U].eligibility, VenueEligibilityReason::reject_burst);
  EXPECT_EQ(submit(first, 60U).status, GatewaySubmitStatus::accepted);

  auto concentrated = reject;
  concentrated.objective = objective(ExecutionPolicy::aggressive_take, 61U);
  concentrated.venues[1U] = observation(kSecondVenue, 98, 100);
  concentrated.venues[1U].already_routed_quantity_units = 100U;
  rehash(concentrated.venues[1U]);
  rehash(concentrated);
  SmartOrderRouter second_router{router_configuration()};
  const auto second = second_router.route(concentrated);
  ASSERT_EQ(second.status, RoutingStatus::routed);
  EXPECT_EQ(second.venue_scores[1U].eligibility,
            VenueEligibilityReason::concentration_exhausted);
  EXPECT_EQ(submit(second, 61U).status, GatewaySubmitStatus::accepted);
}

TEST_F(RouterPaperGatewayTest, RebalanceAuctionCapsParticipationToPairedQuantity) {
  auto value = routing_request(ExecutionPolicy::auction_participation, 1U, 70U);
  value.objective.maximum_child_quantity_units = 100U;
  value.objective.participation_rate_ppm = 200'000U;
  rehash(value.objective);
  value.venues[0U].trading_state = VenueTradingState::auction;
  value.venues[0U].auction_paired_quantity_units = 50U;
  value.venues[0U].auction_price_ticks = 100;
  rehash(value.venues[0U]);
  rehash(value);
  SmartOrderRouter router{router_configuration(1U)};
  const auto decision = router.route(value);
  ASSERT_EQ(decision.status, RoutingStatus::routed);
  EXPECT_EQ(decision.time_in_force, RoutedTimeInForce::auction);
  EXPECT_EQ(decision.quantity_units, 10U);
  ASSERT_EQ(submit(decision, 70U).status, GatewaySubmitStatus::accepted);
  GatewayEvent event{};
  ASSERT_EQ(gateway_.poll_event(kNow + 300U, event), GatewayPollStatus::event);
  ASSERT_EQ(gateway_.on_market_event(status_event(
                market_data::synthetic::TradingStatus::auction, 1U, kNow + 350U)),
            GatewayReason::accepted);
  ASSERT_EQ(gateway_.on_market_event(auction_event(2U, kNow + 400U, 100, 20U)),
            GatewayReason::accepted);
  EXPECT_EQ(gateway_.poll_event(kNow + 400U, event), GatewayPollStatus::event);
  EXPECT_EQ(event.kind, GatewayResponseKind::fill);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_F(RouterPaperGatewayTest, PriceAndSchedulePoliciesProduceBoundedSlices) {
  struct Expectation {
    ExecutionPolicy policy;
    std::int64_t price_ticks;
    std::uint64_t quantity_units;
    RoutedTimeInForce time_in_force;
  };
  const std::array expectations{
      Expectation{.policy = ExecutionPolicy::passive_join,
                  .price_ticks = 99,
                  .quantity_units = 10U,
                  .time_in_force = RoutedTimeInForce::day},
      Expectation{.policy = ExecutionPolicy::passive_improve,
                  .price_ticks = 100,
                  .quantity_units = 10U,
                  .time_in_force = RoutedTimeInForce::day},
      Expectation{.policy = ExecutionPolicy::aggressive_take,
                  .price_ticks = 101,
                  .quantity_units = 10U,
                  .time_in_force = RoutedTimeInForce::day},
      Expectation{.policy = ExecutionPolicy::immediate_or_cancel,
                  .price_ticks = 101,
                  .quantity_units = 10U,
                  .time_in_force = RoutedTimeInForce::immediate_or_cancel},
      Expectation{.policy = ExecutionPolicy::participation,
                  .price_ticks = 99,
                  .quantity_units = 10U,
                  .time_in_force = RoutedTimeInForce::day},
      Expectation{.policy = ExecutionPolicy::twap,
                  .price_ticks = 99,
                  .quantity_units = 10U,
                  .time_in_force = RoutedTimeInForce::day},
      Expectation{.policy = ExecutionPolicy::vwap,
                  .price_ticks = 99,
                  .quantity_units = 10U,
                  .time_in_force = RoutedTimeInForce::day},
      Expectation{.policy = ExecutionPolicy::pov,
                  .price_ticks = 99,
                  .quantity_units = 10U,
                  .time_in_force = RoutedTimeInForce::day}};
  std::uint64_t ordinal = 80U;
  for (const auto& expected : expectations) {
    auto value = routing_request(expected.policy, 1U, ordinal);
    if (expected.policy == ExecutionPolicy::participation ||
        expected.policy == ExecutionPolicy::twap ||
        expected.policy == ExecutionPolicy::vwap ||
        expected.policy == ExecutionPolicy::pov) {
      value.objective.maximum_child_quantity_units = 100U;
    }
    value.objective.total_quantity_units = 100U;
    value.objective.participation_rate_ppm = 100'000U;
    if (expected.policy == ExecutionPolicy::twap) {
      value.objective.end_process_monotonic_time_ns = kNow + 900U;
    }
    if (expected.policy == ExecutionPolicy::vwap) {
      value.already_scheduled_quantity_units = 40U;
    }
    rehash(value.objective);
    rehash(value);
    SmartOrderRouter router{router_configuration(1U)};
    const auto decision = router.route(value);
    ASSERT_EQ(decision.status, RoutingStatus::routed);
    EXPECT_EQ(decision.price_ticks, expected.price_ticks);
    EXPECT_EQ(decision.quantity_units, expected.quantity_units);
    EXPECT_EQ(decision.time_in_force, expected.time_in_force);
    EXPECT_EQ(submit(decision, ordinal).status, GatewaySubmitStatus::accepted);
    ++ordinal;
  }
}

TEST_F(RouterPaperGatewayTest, TieBreakAndLoopProtectionAreDeterministic) {
  auto tied = routing_request(ExecutionPolicy::passive_join, 2U, 90U);
  auto config = router_configuration();
  config.venues[1U].tie_break_rank = 1U;
  config.stable_hash = stable_router_configuration_hash(config);
  SmartOrderRouter router{config};
  const auto first = router.route(tied);
  ASSERT_EQ(first.status, RoutingStatus::routed);
  EXPECT_EQ(first.venue_id, risk::test::kVenue);
  SmartOrderRouter replay_router{config};
  const auto replayed = replay_router.route(tied);
  EXPECT_EQ(replayed.stable_hash, first.stable_hash);
  EXPECT_EQ(submit(first, 90U).status, GatewaySubmitStatus::accepted);

  auto looped = routing_request(ExecutionPolicy::passive_join, 2U, 91U);
  looped.visited_venues[0U] = risk::test::kVenue;
  looped.visited_venues[1U] = kSecondVenue;
  looped.visited_venue_count = 2U;
  rehash(looped);
  const auto before = gateway_.metrics().commands_received;
  const auto abstention = router.route(looped);
  EXPECT_EQ(abstention.status, RoutingStatus::abstained);
  EXPECT_EQ(abstention.reason, RoutingReason::routing_loop);
  EXPECT_EQ(gateway_.metrics().commands_received, before);
}

TEST_F(RouterPaperGatewayTest, RegulationStpAndLifecycleFailClosed) {
  auto value = routing_request(ExecutionPolicy::passive_join, 2U, 95U);
  value.venues[1U].self_trade_prevention = SelfTradePreventionState::conflict;
  rehash(value.venues[1U]);
  rehash(value);
  SmartOrderRouter router{router_configuration()};
  ASSERT_TRUE(router.initialized());
  ASSERT_TRUE(router.ready());
  EXPECT_EQ(router.configuration_hash(), router_configuration().stable_hash);
  EXPECT_FALSE(SmartOrderRouter::build_info().project.empty());
  const auto decision = router.route(value);
  ASSERT_EQ(decision.status, RoutingStatus::routed);
  EXPECT_EQ(decision.venue_scores[1U].eligibility,
            VenueEligibilityReason::self_trade_conflict);
  EXPECT_EQ(submit(decision, 95U).status, GatewaySubmitStatus::accepted);

  value.objective = objective(ExecutionPolicy::passive_join, 96U);
  value.venues[1U].self_trade_prevention = SelfTradePreventionState::clear;
  value.venues[1U].regulatory_eligible = false;
  rehash(value.venues[1U]);
  rehash(value);
  const auto regulatory = router.route(value);
  ASSERT_EQ(regulatory.status, RoutingStatus::routed);
  EXPECT_EQ(regulatory.venue_scores[1U].eligibility,
            VenueEligibilityReason::regulatory_ineligible);
  EXPECT_EQ(submit(regulatory, 96U).status, GatewaySubmitStatus::accepted);

  router.shutdown();
  EXPECT_FALSE(router.ready());
  EXPECT_EQ(router.route(value).status, RoutingStatus::stopped);

  auto invalid_configuration = router_configuration();
  invalid_configuration.venues[0U].regulatory_authorized = false;
  const SmartOrderRouter invalid_router{invalid_configuration};
  EXPECT_FALSE(invalid_router.initialized());
  EXPECT_EQ(invalid_router.health(), RouterHealth::unsafe);
}

TEST_F(RouterPaperGatewayTest, CostAttributionUsesIntegerTicksAndStableHash) {
  EXPECT_EQ(fill_uncertainty_ppm(0U), 0U);
  EXPECT_EQ(fill_uncertainty_ppm(500'000U), kRouterPartsPerMillion);
  EXPECT_EQ(fill_uncertainty_ppm(kRouterPartsPerMillion), 0U);
  EXPECT_EQ(decay_alpha_microticks(2'000'000, 0U, 1'000U), 2'000'000);
  EXPECT_EQ(decay_alpha_microticks(2'000'000, 1'000U, 1'000U), 1'000'000);
  auto value = routing_request(ExecutionPolicy::aggressive_take, 1U, 100U);
  SmartOrderRouter router{router_configuration(1U)};
  const auto decision = router.route(value);
  ASSERT_EQ(submit(decision, 100U).status, GatewaySubmitStatus::accepted);
  ExecutionCostAttribution attribution{};
  ASSERT_TRUE(execution_cost_attribution({.objective_id = value.objective.objective_id,
                                          .venue_id = decision.venue_id,
                                          .side = risk::IntentAction::buy,
                                          .arrival_price_ticks = 100,
                                          .decision_price_ticks = 101,
                                          .fill_price_ticks = 102,
                                          .post_fill_reference_price_ticks = 99,
                                          .filled_quantity_units = 3U,
                                          .unfilled_quantity_units = 2U,
                                          .fee_currency_nanos = 5U,
                                          .modeled_impact_ticks = 1U,
                                          .tick_value_currency_nanos = 10U},
                                         attribution));
  EXPECT_EQ(attribution.spread_cost_currency_nanos, 30);
  EXPECT_EQ(attribution.slippage_cost_currency_nanos, 30);
  EXPECT_EQ(attribution.impact_cost_currency_nanos, 30);
  EXPECT_EQ(attribution.adverse_selection_cost_currency_nanos, 90);
  EXPECT_EQ(attribution.opportunity_cost_currency_nanos, -20);
  EXPECT_EQ(attribution.total_cost_currency_nanos, 165);
  EXPECT_EQ(attribution.stable_hash,
            stable_execution_cost_attribution_hash(attribution));
}

} // namespace aegis::execution::test
