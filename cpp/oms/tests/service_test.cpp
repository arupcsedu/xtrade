#include "aegis/oms/service.hpp"

#include "test_support.hpp"

#include <gtest/gtest.h>

#include <memory>

namespace aegis::oms::test {
namespace {

class OmsServiceTest : public ::testing::Test {
protected:
  void SetUp() override {
    journal = std::make_unique<OmsJournal>();
    service = std::make_unique<DeterministicOms>(configuration(), *journal);
  }

  std::unique_ptr<OmsJournal>
      journal; // NOLINT(misc-non-private-member-variables-in-classes)
  std::unique_ptr<DeterministicOms>
      service; // NOLINT(misc-non-private-member-variables-in-classes)
};

TEST_F(OmsServiceTest, ApprovedIntentTraversesCreatedReadyPendingAndWorking) {
  const auto accepted = service->apply(accept(approval(), 1U));
  ASSERT_EQ(accepted.status, ApplyStatus::applied);
  EXPECT_EQ(accepted.current_state, OrderState::created);
  ASSERT_TRUE(accepted.order_id.valid());

  const auto ready_event = internal(InputKind::mark_ready, accepted.order_id, 2U);
  EXPECT_EQ(service->apply(ready_event).current_state, OrderState::ready);
  const auto dispatched =
      service->apply(internal(InputKind::dispatch, accepted.order_id, 3U));
  ASSERT_EQ(dispatched.current_state, OrderState::pending_ack);
  ASSERT_EQ(dispatched.gateway_command.kind, GatewayCommandKind::new_order);
  EXPECT_EQ(dispatched.journal_record_hash, journal->last_record_hash());
  EXPECT_TRUE(valid_gateway_command(dispatched.gateway_command));
  EXPECT_EQ(dispatched.gateway_command.order_id, accepted.order_id);

  const auto acknowledged =
      service->apply(venue(InputKind::acknowledgement, accepted.order_id, 4U, 1U));
  EXPECT_EQ(acknowledged.status, ApplyStatus::applied);
  EXPECT_EQ(acknowledged.current_state, OrderState::working);
  EXPECT_TRUE(service->ready());
  EXPECT_TRUE(service->verify_invariants());
}

TEST_F(OmsServiceTest, CommandsAndAcknowledgementsAreIdempotent) {
  const auto accepted = service->apply(accept(approval(), 1U));
  (void)service->apply(internal(InputKind::mark_ready, accepted.order_id, 2U));
  const auto dispatch_event = internal(InputKind::dispatch, accepted.order_id, 3U);
  const auto first = service->apply(dispatch_event);
  const auto duplicate = service->apply(dispatch_event);
  EXPECT_EQ(first.gateway_command.kind, GatewayCommandKind::new_order);
  EXPECT_EQ(duplicate.status, ApplyStatus::duplicate);
  EXPECT_EQ(duplicate.gateway_command.kind, GatewayCommandKind::none);
  EXPECT_EQ(duplicate.state_version, first.state_version);

  const auto ack = venue(InputKind::acknowledgement, accepted.order_id, 4U, 1U);
  EXPECT_EQ(service->apply(ack).status, ApplyStatus::applied);
  EXPECT_EQ(service->apply(ack).status, ApplyStatus::duplicate);
  auto another_ack = venue(InputKind::acknowledgement, accepted.order_id, 5U, 2U);
  EXPECT_EQ(service->apply(another_ack).status, ApplyStatus::duplicate);
  EXPECT_EQ(service->metrics().emitted_commands, 1U);
}

TEST_F(OmsServiceTest, FillBeforeAckAndCancelRaceRemainDeterministic) {
  const auto accepted = service->apply(accept(approval(), 1U));
  (void)service->apply(internal(InputKind::mark_ready, accepted.order_id, 2U));
  (void)service->apply(internal(InputKind::dispatch, accepted.order_id, 3U));
  const auto early_fill = service->apply(fill(accepted.order_id, 4U, 20U, 1U, 3U));
  ASSERT_EQ(early_fill.current_state, OrderState::partially_filled);

  const auto late_ack =
      service->apply(venue(InputKind::acknowledgement, accepted.order_id, 5U, 10U));
  EXPECT_EQ(late_ack.status, ApplyStatus::out_of_order_applied);
  EXPECT_EQ(late_ack.current_state, OrderState::partially_filled);

  const auto cancel_result = service->apply(cancel(accepted.order_id, 6U));
  ASSERT_EQ(cancel_result.current_state, OrderState::pending_cancel);
  EXPECT_EQ(cancel_result.gateway_command.kind, GatewayCommandKind::cancel);
  const auto racing_fill = service->apply(fill(accepted.order_id, 7U, 21U, 2U, 2U));
  EXPECT_EQ(racing_fill.current_state, OrderState::pending_cancel);
  const auto canceled = service->apply(
      venue(InputKind::cancel_acknowledgement, accepted.order_id, 8U, 22U));
  EXPECT_EQ(canceled.current_state, OrderState::canceled);

  OmsOrderSnapshot order{};
  ASSERT_TRUE(service->lookup_order(accepted.order_id, order));
  EXPECT_EQ(order.cumulative_fill_quantity_units, 5U);
  EXPECT_EQ(order.remaining_quantity_units, 0U);
  EXPECT_TRUE(service->verify_invariants());
}

TEST_F(OmsServiceTest, LatePartialFillAfterCancelUpdatesOnceAndRemainsTerminal) {
  const auto order_id = create_working_order(*service);
  (void)service->apply(cancel(order_id, 20U));
  ASSERT_EQ(service->apply(venue(InputKind::cancel_acknowledgement, order_id, 21U, 2U))
                .current_state,
            OrderState::canceled);
  const auto late = service->apply(fill(order_id, 22U, 3U, 1U, 2U));
  EXPECT_EQ(late.status, ApplyStatus::applied);
  EXPECT_EQ(late.current_state, OrderState::canceled);

  OmsOrderSnapshot order{};
  ASSERT_TRUE(service->lookup_order(order_id, order));
  EXPECT_EQ(order.cumulative_fill_quantity_units, 2U);
  EXPECT_EQ(order.remaining_quantity_units, 0U);
  EXPECT_TRUE(service->verify_invariants());
}

TEST_F(OmsServiceTest, ReplaceIsRiskBoundAndAppliesOnlyOnVenueAck) {
  const auto order_id = create_working_order(*service);
  const auto requested = service->apply(replace(order_id, 20U, 105, 12U));
  ASSERT_EQ(requested.current_state, OrderState::pending_replace);
  EXPECT_EQ(requested.gateway_command.kind, GatewayCommandKind::replace);

  OmsOrderSnapshot before{};
  ASSERT_TRUE(service->lookup_order(order_id, before));
  EXPECT_EQ(before.price_ticks, 100);
  EXPECT_EQ(before.quantity_units, 10U);
  const auto acknowledged =
      service->apply(venue(InputKind::replace_acknowledgement, order_id, 21U, 2U));
  EXPECT_EQ(acknowledged.current_state, OrderState::working);
  OmsOrderSnapshot after{};
  ASSERT_TRUE(service->lookup_order(order_id, after));
  EXPECT_EQ(after.price_ticks, 105);
  EXPECT_EQ(after.quantity_units, 12U);
}

TEST_F(OmsServiceTest, ReplaceRejectRestoresPartialState) {
  const auto order_id = create_working_order(*service);
  ASSERT_EQ(service->apply(fill(order_id, 20U, 2U, 1U, 2U)).current_state,
            OrderState::partially_filled);
  ASSERT_EQ(service->apply(replace(order_id, 21U, 101, 12U)).current_state,
            OrderState::pending_replace);
  const auto rejected =
      service->apply(venue(InputKind::replace_rejection, order_id, 22U, 3U));
  EXPECT_EQ(rejected.current_state, OrderState::partially_filled);
}

TEST(OmsReconciliationTest, DropCopyConfirmsWithoutApplyingFillTwice) {
  auto journal = std::make_unique<OmsJournal>();
  auto service = std::make_unique<DeterministicOms>(configuration(true), *journal);
  const auto order_id = create_working_order(*service);
  const auto primary = service->apply(fill(order_id, 20U, 2U, 1U, 4U));
  ASSERT_EQ(primary.status, ApplyStatus::applied);
  EXPECT_FALSE(service->ready());
  EXPECT_EQ(service->health(), OmsHealth::recovering);

  auto copy = fill(order_id, 21U, 2U, 1U, 4U, EventSource::drop_copy);
  const auto reconciled = service->apply(copy);
  EXPECT_EQ(reconciled.status, ApplyStatus::reconciled);
  EXPECT_TRUE(service->ready());
  OmsOrderSnapshot order{};
  ASSERT_TRUE(service->lookup_order(order_id, order));
  EXPECT_EQ(order.cumulative_fill_quantity_units, 4U);
}

TEST_F(OmsServiceTest, ConflictingExecutionFailsClosed) {
  const auto order_id = create_working_order(*service);
  ASSERT_EQ(service->apply(fill(order_id, 20U, 2U, 1U, 2U)).status,
            ApplyStatus::applied);
  const auto conflict =
      service->apply(fill(order_id, 21U, 2U, 1U, 3U, EventSource::drop_copy));
  EXPECT_EQ(conflict.status, ApplyStatus::invalid);
  EXPECT_EQ(conflict.reason, OmsReason::execution_conflict);
  EXPECT_EQ(service->health(), OmsHealth::unsafe);
  EXPECT_FALSE(service->ready());
}

TEST_F(OmsServiceTest, StaleFencingTokenCannotEmit) {
  const auto accepted = service->apply(accept(approval(), 1U));
  auto ready_event = internal(InputKind::mark_ready, accepted.order_id, 2U);
  ready_event.authority.fencing_token -= 1U;
  ready_event.stable_hash = stable_input_hash(ready_event);
  const auto result = service->apply(ready_event);
  EXPECT_EQ(result.status, ApplyStatus::fenced);
  EXPECT_EQ(result.gateway_command.kind, GatewayCommandKind::none);
}

TEST_F(OmsServiceTest, UnknownExternalOrderEntersRecoveryWithoutEmission) {
  auto event = venue(InputKind::acknowledgement, {}, 1U, 1U,
                     ExternalOrderId{.high = 700U, .low = 800U});
  const auto result = service->apply(event);
  EXPECT_EQ(result.status, ApplyStatus::applied);
  EXPECT_EQ(result.current_state, OrderState::unknown_recovery);
  EXPECT_EQ(result.gateway_command.kind, GatewayCommandKind::none);
  EXPECT_EQ(service->health(), OmsHealth::recovering);
  EXPECT_FALSE(service->ready());
}

TEST_F(OmsServiceTest, InvalidOrExpiredRiskEvidenceCreatesNoOrder) {
  auto input = accept(approval(), 1U);
  input.process_monotonic_time_ns =
      input.risk_decision.valid_until_process_monotonic_time_ns + 1U;
  input.stable_hash = stable_input_hash(input);
  const auto result = service->apply(input);
  EXPECT_EQ(result.status, ApplyStatus::invalid);
  EXPECT_EQ(service->snapshot_for_quiescent_inspection().order_count, 0U);
}

TEST_F(OmsServiceTest, ForbiddenFillDoesNotMutateQuantityOrConsumeExecution) {
  const auto accepted = service->apply(accept(approval(), 1U));
  const auto premature = service->apply(fill(accepted.order_id, 2U, 1U, 7U, 2U));
  ASSERT_EQ(premature.status, ApplyStatus::forbidden_transition);

  OmsOrderSnapshot unchanged{};
  ASSERT_TRUE(service->lookup_order(accepted.order_id, unchanged));
  EXPECT_EQ(unchanged.state, OrderState::created);
  EXPECT_EQ(unchanged.cumulative_fill_quantity_units, 0U);
  EXPECT_EQ(unchanged.remaining_quantity_units, 10U);

  (void)service->apply(internal(InputKind::mark_ready, accepted.order_id, 3U));
  (void)service->apply(internal(InputKind::dispatch, accepted.order_id, 4U));
  const auto valid_fill = service->apply(fill(accepted.order_id, 5U, 2U, 7U, 2U));
  EXPECT_EQ(valid_fill.status, ApplyStatus::applied);
  EXPECT_EQ(valid_fill.current_state, OrderState::partially_filled);
}

TEST_F(OmsServiceTest, ImpossibleLateRejectionFailsIntoExplicitRecovery) {
  const auto order_id = create_working_order(*service);
  const auto rejected = service->apply(venue(InputKind::rejection, order_id, 20U, 2U));
  EXPECT_EQ(rejected.status, ApplyStatus::forbidden_transition);
  EXPECT_EQ(rejected.current_state, OrderState::unknown_recovery);
  EXPECT_EQ(service->health(), OmsHealth::recovering);
  EXPECT_FALSE(service->ready());
}

TEST_F(OmsServiceTest, ServiceContractIsSafeAndShutdownIsTerminal) {
  EXPECT_TRUE(service->initialized());
  EXPECT_TRUE(service->build_info().live_trading_capable == false);
  EXPECT_EQ(service->configuration_hash(), configuration().stable_hash);
  service->shutdown(kNow + 100U);
  EXPECT_EQ(service->health(), OmsHealth::stopped);
  EXPECT_FALSE(service->ready());
  EXPECT_EQ(service->apply(accept(approval(), 101U)).status, ApplyStatus::stopped);
}

} // namespace
} // namespace aegis::oms::test
