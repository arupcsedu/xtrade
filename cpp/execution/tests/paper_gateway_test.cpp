#include "aegis/execution/simulated_gateway.hpp"

#include "test_support.hpp"

#include <gtest/gtest.h>

namespace aegis::execution::test {

TEST(PaperGatewayTest, ModelsLatencyQueuePartialFillFeesSlippageAndImpact) {
  auto config = configuration(GatewayMode::paper);
  GatewayAuditJournal journal;
  PaperBrokerGateway gateway{config, journal};
  ASSERT_TRUE(gateway.start(kNow - 100U));
  const auto order = request(GatewayMode::paper);
  ASSERT_EQ(gateway.send_order(order).status, GatewaySubmitStatus::accepted);

  GatewayEvent event{};
  EXPECT_EQ(gateway.poll_event(kNow + 99U, event), GatewayPollStatus::empty);
  ASSERT_EQ(gateway.poll_event(kNow + 100U, event), GatewayPollStatus::event);
  EXPECT_EQ(event.kind, GatewayResponseKind::acknowledgement);
  EXPECT_EQ(event.mode, GatewayMode::paper);
  EXPECT_TRUE(event.external_order_id.valid());

  ASSERT_EQ(gateway.on_market_event(status_event(
                market_data::synthetic::TradingStatus::open, 1U, kNow + 150U)),
            GatewayReason::accepted);
  ASSERT_EQ(gateway.on_market_event(trade_event(2U, kNow + 200U)),
            GatewayReason::accepted);
  ASSERT_EQ(gateway.poll_event(kNow + 200U, event), GatewayPollStatus::event);
  EXPECT_EQ(event.kind, GatewayResponseKind::fill);
  EXPECT_EQ(event.quantity_units, 3U);
  EXPECT_EQ(event.remaining_quantity_units, 7U);
  EXPECT_EQ(event.queue_ahead_before_units, 2U);
  EXPECT_EQ(event.queue_ahead_after_units, 0U);
  EXPECT_EQ(event.fee_currency_nanos, 15U);
  EXPECT_EQ(event.modeled_slippage_ticks, 1U);
  EXPECT_EQ(event.modeled_impact_ticks, 1U);
  EXPECT_EQ(event.price_ticks, 100);
  EXPECT_EQ(gateway.metrics().partial_fills, 1U);
  EXPECT_TRUE(journal.verify_chain());
}

TEST(PaperGatewayTest, DeterministicRejectPolicyProducesVenueReject) {
  const auto config = configuration(GatewayMode::paper, 1U);
  GatewayAuditJournal journal;
  PaperBrokerGateway gateway{config, journal};
  ASSERT_TRUE(gateway.start(kNow - 100U));
  auto new_order = request(GatewayMode::paper);
  new_order.safety.effective_configuration_hash = config.stable_hash;
  new_order.safety.stable_hash = stable_final_safety_state_hash(new_order.safety);
  new_order.stable_hash = stable_gateway_request_hash(new_order);
  ASSERT_EQ(gateway.send_order(new_order).status, GatewaySubmitStatus::accepted);
  GatewayEvent event{};
  ASSERT_EQ(gateway.poll_event(kNow + 100U, event), GatewayPollStatus::event);
  EXPECT_EQ(event.kind, GatewayResponseKind::rejection);
  EXPECT_EQ(event.reason, GatewayReason::paper_policy_reject);
  EXPECT_EQ(event.mode, GatewayMode::paper);
}

TEST(PaperGatewayTest, FillMayWinCancelRaceBeforeCancelLatencyExpires) {
  GatewayAuditJournal journal;
  PaperBrokerGateway gateway{configuration(GatewayMode::paper), journal};
  ASSERT_TRUE(gateway.start(kNow - 100U));
  const auto new_order = request(GatewayMode::paper);
  ASSERT_EQ(gateway.send_order(new_order).status, GatewaySubmitStatus::accepted);
  GatewayEvent event{};
  ASSERT_EQ(gateway.poll_event(kNow + 100U, event), GatewayPollStatus::event);
  const auto external = event.external_order_id;

  auto cancel = request(GatewayMode::paper, oms::GatewayCommandKind::cancel, 2U,
                        kNow + 200U, new_order.command.order_id, external);
  ASSERT_EQ(gateway.cancel_order(cancel).status, GatewaySubmitStatus::accepted);
  ASSERT_EQ(gateway.on_market_event(status_event(
                market_data::synthetic::TradingStatus::open, 1U, kNow + 250U)),
            GatewayReason::accepted);
  ASSERT_EQ(gateway.on_market_event(trade_event(2U, kNow + 300U)),
            GatewayReason::accepted);
  ASSERT_EQ(gateway.poll_event(kNow + 300U, event), GatewayPollStatus::event);
  EXPECT_EQ(event.kind, GatewayResponseKind::fill);
  EXPECT_EQ(event.quantity_units, 3U);
  ASSERT_EQ(gateway.poll_event(kNow + 400U, event), GatewayPollStatus::event);
  EXPECT_EQ(event.kind, GatewayResponseKind::cancel_acknowledgement);
  EXPECT_EQ(event.cumulative_fill_quantity_units, 3U);
}

TEST(PaperGatewayTest, ReplaceAppliesOnlyAfterConfiguredLatency) {
  GatewayAuditJournal journal;
  PaperBrokerGateway gateway{configuration(GatewayMode::paper), journal};
  ASSERT_TRUE(gateway.start(kNow - 100U));
  const auto new_order = request(GatewayMode::paper);
  ASSERT_EQ(gateway.send_order(new_order).status, GatewaySubmitStatus::accepted);
  GatewayEvent event{};
  ASSERT_EQ(gateway.poll_event(kNow + 100U, event), GatewayPollStatus::event);

  const auto replace =
      request(GatewayMode::paper, oms::GatewayCommandKind::replace, 2U, kNow + 200U,
              new_order.command.order_id, event.external_order_id, 101, 12U);
  ASSERT_EQ(gateway.replace_order(replace).status, GatewaySubmitStatus::accepted);
  EXPECT_EQ(gateway.poll_event(kNow + 399U, event), GatewayPollStatus::empty);
  ASSERT_EQ(gateway.poll_event(kNow + 400U, event), GatewayPollStatus::event);
  EXPECT_EQ(event.kind, GatewayResponseKind::replace_acknowledgement);
  EXPECT_EQ(event.price_ticks, 101);
  EXPECT_EQ(event.quantity_units, 12U);
  EXPECT_EQ(event.remaining_quantity_units, 12U);
}

TEST(PaperGatewayTest, HaltsSuppressFillsAndAuctionUsesPairedLiquidity) {
  GatewayAuditJournal journal;
  PaperBrokerGateway gateway{configuration(GatewayMode::paper), journal};
  ASSERT_TRUE(gateway.start(kNow - 100U));
  ASSERT_EQ(gateway.send_order(request(GatewayMode::paper)).status,
            GatewaySubmitStatus::accepted);
  GatewayEvent event{};
  ASSERT_EQ(gateway.poll_event(kNow + 100U, event), GatewayPollStatus::event);

  ASSERT_EQ(gateway.on_market_event(status_event(
                market_data::synthetic::TradingStatus::halted, 1U, kNow + 200U)),
            GatewayReason::accepted);
  ASSERT_EQ(gateway.on_market_event(trade_event(2U, kNow + 250U)),
            GatewayReason::accepted);
  EXPECT_EQ(gateway.poll_event(kNow + 250U, event), GatewayPollStatus::empty);

  ASSERT_EQ(gateway.on_market_event(status_event(
                market_data::synthetic::TradingStatus::auction, 3U, kNow + 300U)),
            GatewayReason::accepted);
  ASSERT_EQ(gateway.on_market_event(auction_event(4U, kNow + 350U)),
            GatewayReason::accepted);
  ASSERT_EQ(gateway.poll_event(kNow + 350U, event), GatewayPollStatus::event);
  EXPECT_EQ(event.kind, GatewayResponseKind::fill);
  EXPECT_EQ(event.source_market_event_hash, auction_event(4U, kNow + 350U).event_hash);
}

TEST(PaperGatewayTest, NonValidMarketQualityIsJournaledButCannotFill) {
  GatewayAuditJournal journal;
  PaperBrokerGateway gateway{configuration(GatewayMode::paper), journal};
  ASSERT_TRUE(gateway.start(kNow - 100U));
  ASSERT_EQ(gateway.send_order(request(GatewayMode::paper)).status,
            GatewaySubmitStatus::accepted);
  GatewayEvent event{};
  ASSERT_EQ(gateway.poll_event(kNow + 100U, event), GatewayPollStatus::event);
  ASSERT_EQ(gateway.on_market_event(status_event(
                market_data::synthetic::TradingStatus::open, 1U, kNow + 150U)),
            GatewayReason::accepted);

  auto stale_trade = trade_event(2U, kNow + 200U);
  stale_trade.data_quality = market_data::synthetic::DataQuality::stale;
  stale_trade.event_hash = market_data::synthetic::calculate_event_hash(stale_trade);
  EXPECT_EQ(gateway.on_market_event(stale_trade), GatewayReason::feed_unhealthy);
  EXPECT_EQ(gateway.poll_event(kNow + 200U, event), GatewayPollStatus::empty);
  EXPECT_TRUE(journal.verify_chain());
}

TEST(PaperGatewayTest, DisabledAuctionParticipationSuppressesAuctionFill) {
  auto config = configuration(GatewayMode::paper);
  config.paper_model.allow_auction_orders = false;
  config.stable_hash = stable_gateway_configuration_hash(config);
  GatewayAuditJournal journal;
  PaperBrokerGateway gateway{config, journal};
  ASSERT_TRUE(gateway.start(kNow - 100U));
  auto new_order = request(GatewayMode::paper);
  new_order.safety.effective_configuration_hash = config.stable_hash;
  new_order.safety.stable_hash = stable_final_safety_state_hash(new_order.safety);
  new_order.stable_hash = stable_gateway_request_hash(new_order);
  ASSERT_EQ(gateway.send_order(new_order).status, GatewaySubmitStatus::accepted);
  GatewayEvent event{};
  ASSERT_EQ(gateway.poll_event(kNow + 100U, event), GatewayPollStatus::event);
  ASSERT_EQ(gateway.on_market_event(status_event(
                market_data::synthetic::TradingStatus::auction, 1U, kNow + 150U)),
            GatewayReason::accepted);
  EXPECT_EQ(gateway.on_market_event(auction_event(2U, kNow + 200U)),
            GatewayReason::accepted);
  EXPECT_EQ(gateway.poll_event(kNow + 200U, event), GatewayPollStatus::empty);
}

TEST(SyntheticGatewayTest, HeartbeatTimeoutInhibitsAndSequenceGapIsNotForwarded) {
  GatewayAuditJournal journal;
  SyntheticExchangeGateway gateway{configuration(), journal};
  ASSERT_TRUE(gateway.start(kNow - 100U));
  GatewayEvent gap{.event_id = common::GlobalEventId{700U, 1U},
                   .source_command_id = {},
                   .execution_id = {},
                   .order_id = {},
                   .external_order_id = {},
                   .session_id = risk::test::kSession,
                   .account_id = risk::test::kAccount,
                   .venue_id = risk::test::kVenue,
                   .instrument_id = {},
                   .configuration_version = risk::test::kConfiguration,
                   .authority = kAuthority,
                   .mode = GatewayMode::simulation,
                   .kind = GatewayResponseKind::heartbeat,
                   .reason = GatewayReason::accepted,
                   .venue_sequence = 2U,
                   .process_monotonic_time_ns = kNow};
  gap.stable_hash = stable_gateway_event_hash(gap);
  ASSERT_TRUE(gateway.inject_synthetic_response(gap, kNow));
  GatewayEvent output{};
  EXPECT_EQ(gateway.poll_event(kNow, output), GatewayPollStatus::sequence_error);
  EXPECT_EQ(gateway.health(), GatewayHealth::recovering);
  EXPECT_FALSE(gateway.ready());

  GatewayAuditJournal second_journal;
  SyntheticExchangeGateway second{configuration(), second_journal};
  ASSERT_TRUE(second.start(kNow - 100U));
  EXPECT_EQ(second.poll_event(kNow + 600'000U, output), GatewayPollStatus::recovering);
  EXPECT_EQ(second.session_snapshot().reason, SessionReason::heartbeat_timeout);
}

} // namespace aegis::execution::test
