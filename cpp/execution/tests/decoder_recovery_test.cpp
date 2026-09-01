#include "aegis/execution/decoders.hpp"
#include "aegis/execution/simulated_gateway.hpp"

#include "test_support.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace aegis::execution::test {

TEST(GatewayDecoderTest, NormalizedAcknowledgementRejectAndExecutionMapToOms) {
  GatewayAuditJournal journal;
  SyntheticExchangeGateway gateway{configuration(), journal};
  ASSERT_TRUE(gateway.start(kNow - 100U));
  ASSERT_EQ(gateway.send_order(request()).status, GatewaySubmitStatus::accepted);
  GatewayEvent acknowledgement{};
  ASSERT_EQ(gateway.poll_event(kNow + 100U, acknowledgement), GatewayPollStatus::event);
  oms::OmsInput input{};
  EXPECT_EQ(AcknowledgementDecoder{}.decode(acknowledgement, input),
            AdapterStatus::decoded);
  EXPECT_EQ(input.kind, oms::InputKind::acknowledgement);
  EXPECT_TRUE(oms::valid_input(input));

  ASSERT_EQ(gateway.on_market_event(status_event(
                market_data::synthetic::TradingStatus::open, 1U, kNow + 150U)),
            GatewayReason::accepted);
  ASSERT_EQ(gateway.on_market_event(trade_event(2U, kNow + 200U)),
            GatewayReason::accepted);
  GatewayEvent fill{};
  ASSERT_EQ(gateway.poll_event(kNow + 200U, fill), GatewayPollStatus::event);
  EXPECT_EQ(ExecutionDecoder{}.decode(fill, input), AdapterStatus::decoded);
  EXPECT_EQ(input.kind, oms::InputKind::fill);
  EXPECT_TRUE(oms::valid_input(input));

  auto malformed = fill;
  malformed.quantity_units = 0U;
  malformed.stable_hash = stable_gateway_event_hash(malformed);
  EXPECT_EQ(ExecutionDecoder{}.decode(malformed, input), AdapterStatus::malformed);
  malformed = acknowledgement;
  constexpr std::uint8_t kInvalidReason = 255U;
  std::memcpy(&malformed.reason, &kInvalidReason, sizeof(kInvalidReason));
  malformed.stable_hash = stable_gateway_event_hash(malformed);
  EXPECT_EQ(AcknowledgementDecoder{}.decode(malformed, input),
            AdapterStatus::malformed);
  EXPECT_EQ(RejectDecoder{}.decode(fill, input), AdapterStatus::unsupported);
}

TEST(GatewayDecoderTest, NormalizedVenueRejectMapsToOmsRejection) {
  const auto config = configuration(GatewayMode::paper, 1U);
  GatewayAuditJournal journal;
  PaperBrokerGateway gateway{config, journal};
  ASSERT_TRUE(gateway.start(kNow - 100U));
  auto new_order = request(GatewayMode::paper);
  new_order.safety.effective_configuration_hash = config.stable_hash;
  new_order.safety.stable_hash = stable_final_safety_state_hash(new_order.safety);
  new_order.stable_hash = stable_gateway_request_hash(new_order);
  ASSERT_EQ(gateway.send_order(new_order).status, GatewaySubmitStatus::accepted);

  GatewayEvent rejection{};
  ASSERT_EQ(gateway.poll_event(kNow + 100U, rejection), GatewayPollStatus::event);
  ASSERT_EQ(rejection.kind, GatewayResponseKind::rejection);
  oms::OmsInput input{};
  EXPECT_EQ(RejectDecoder{}.decode(rejection, input), AdapterStatus::decoded);
  EXPECT_EQ(input.kind, oms::InputKind::rejection);
  EXPECT_TRUE(oms::valid_input(input));
}

// GoogleTest assertion macro expansion inflates the reported branch count.
// NOLINTBEGIN(readability-function-cognitive-complexity)
TEST(GatewayRecoveryTest, ExplicitSnapshotRecoversButAmbiguousOrdersDoNot) {
  GatewayAuditJournal journal;
  SyntheticExchangeGateway gateway{configuration(), journal};
  ASSERT_TRUE(gateway.start(kNow - 100U));
  ASSERT_EQ(gateway.send_order(request()).status, GatewaySubmitStatus::accepted);
  GatewayEvent event{};
  ASSERT_EQ(gateway.poll_event(kNow + 100U, event), GatewayPollStatus::event);
  const auto known = gateway.recovery_snapshot();
  ASSERT_TRUE(valid_recovery_snapshot(known));

  auto tampered = known;
  for (auto& order : tampered.orders) {
    if (order.occupied) {
      ++order.acknowledgement_due_ns;
      break;
    }
  }
  EXPECT_FALSE(valid_recovery_snapshot(tampered));

  auto relocated = known;
  auto occupied_index = relocated.orders.size();
  auto empty_index = relocated.orders.size();
  for (std::size_t index = 0U; index < relocated.orders.size(); ++index) {
    occupied_index = relocated.orders[index].occupied ? index : occupied_index;
    empty_index = !relocated.orders[index].occupied ? index : empty_index;
  }
  ASSERT_LT(occupied_index, relocated.orders.size());
  ASSERT_LT(empty_index, relocated.orders.size());
  relocated.orders[empty_index] = relocated.orders[occupied_index];
  relocated.orders[occupied_index] = {};
  relocated.stable_hash = stable_recovery_snapshot_hash(relocated);
  ASSERT_TRUE(valid_recovery_snapshot(relocated));

  ASSERT_TRUE(gateway.begin_recovery(kNow + 200U));
  EXPECT_FALSE(gateway.ready());
  EXPECT_EQ(gateway.recover(relocated, kNow + 201U), RecoveryStatus::completed);
  EXPECT_TRUE(gateway.ready());
  EXPECT_EQ(gateway.recovery_snapshot().stable_hash, known.stable_hash);

  ASSERT_TRUE(gateway.begin_recovery(kNow + 300U));
  const auto ambiguous = gateway.recovery_snapshot();
  ASSERT_TRUE(valid_recovery_snapshot(ambiguous));
  EXPECT_EQ(gateway.recover(ambiguous, kNow + 301U), RecoveryStatus::ambiguous);
  EXPECT_FALSE(gateway.ready());
}
// NOLINTEND(readability-function-cognitive-complexity)

TEST(ProtocolBoundaryTest, SyntheticInjectionRejectsCrossScopeResponse) {
  GatewayAuditJournal journal;
  SyntheticExchangeGateway gateway{configuration(), journal};
  ASSERT_TRUE(gateway.start(kNow - 100U));
  ASSERT_EQ(gateway.send_order(request()).status, GatewaySubmitStatus::accepted);
  GatewayEvent acknowledgement{};
  ASSERT_EQ(gateway.poll_event(kNow + 100U, acknowledgement), GatewayPollStatus::event);
  acknowledgement.event_id = common::GlobalEventId{900U, 1U};
  acknowledgement.session_id = common::SessionId{999U, 999U};
  acknowledgement.venue_sequence = 2U;
  acknowledgement.stable_hash = stable_gateway_event_hash(acknowledgement);
  EXPECT_FALSE(gateway.inject_synthetic_response(acknowledgement, kNow + 200U));
}

TEST(ProtocolBoundaryTest, InterfacesContainNoEndpointOrWireAssumptions) {
  EXPECT_EQ(sizeof(OpaqueProtocolFrame::bytes), kMaximumProtocolFrameBytes);
  OpaqueProtocolFrame native{};
  native.boundary = ProtocolBoundaryKind::native_protocol;
  EXPECT_EQ(native.byte_count, 0U);
  OpaqueProtocolFrame fix{};
  fix.boundary = ProtocolBoundaryKind::fix_compatible;
  EXPECT_EQ(fix.byte_count, 0U);
}

} // namespace aegis::execution::test
