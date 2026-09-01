#include "aegis/oms/service.hpp"

#include "test_support.hpp"

#include <gtest/gtest.h>

#include <memory>

namespace aegis::oms::test {
namespace {

[[nodiscard]] OmsInput
recovery_observation(const common::OrderId order_id, const std::uint64_t receipt,
                     const RecoveryObservation observation) noexcept {
  OmsInput input{.receipt_id = common::GlobalEventId{200U, receipt},
                 .kind = InputKind::recovery_observation,
                 .source = EventSource::recovery,
                 .order_id = order_id,
                 .external_order_id = kExternal,
                 .authority = kAuthority,
                 .recovery_observation = observation,
                 .process_monotonic_time_ns = kNow + receipt};
  input.stable_hash = stable_input_hash(input);
  return input;
}

TEST(OmsRecoveryTest, RestartReplaysThenInhibitsEveryLiveOrder) {
  auto source_journal = std::make_unique<OmsJournal>();
  auto source = std::make_unique<DeterministicOms>(configuration(), *source_journal);
  const auto order_id = create_working_order(*source);
  const auto source_size = source_journal->size();

  auto target_journal = std::make_unique<OmsJournal>();
  auto target = std::make_unique<DeterministicOms>(configuration(), *target_journal);
  ASSERT_EQ(target->recover_from(*source_journal, kNow + 100U),
            RecoveryStatus::recovered_inhibited);
  EXPECT_GT(target_journal->size(), source_size);
  EXPECT_EQ(target->health(), OmsHealth::recovering);
  EXPECT_FALSE(target->ready());
  OmsOrderSnapshot restored{};
  ASSERT_TRUE(target->lookup_order(order_id, restored));
  EXPECT_EQ(restored.state, OrderState::unknown_recovery);
  EXPECT_EQ(restored.state_before_recovery, OrderState::working);

  const auto duplicate_dispatch =
      target->apply(internal(InputKind::dispatch, order_id, 200U));
  EXPECT_EQ(duplicate_dispatch.status, ApplyStatus::inhibited);
  EXPECT_EQ(duplicate_dispatch.gateway_command.kind, GatewayCommandKind::none);
}

TEST(OmsRecoveryTest, VerifiedObservationRestoresStateWithoutNewOrderEmission) {
  auto source_journal = std::make_unique<OmsJournal>();
  auto source = std::make_unique<DeterministicOms>(configuration(), *source_journal);
  const auto order_id = create_working_order(*source);
  auto target_journal = std::make_unique<OmsJournal>();
  auto target = std::make_unique<DeterministicOms>(configuration(), *target_journal);
  ASSERT_EQ(target->recover_from(*source_journal, kNow + 100U),
            RecoveryStatus::recovered_inhibited);
  const auto reconciled =
      target->apply(recovery_observation(order_id, 201U, RecoveryObservation::working));
  EXPECT_EQ(reconciled.status, ApplyStatus::reconciled);
  EXPECT_EQ(reconciled.current_state, OrderState::working);
  EXPECT_EQ(reconciled.gateway_command.kind, GatewayCommandKind::none);
  EXPECT_TRUE(target->ready());
  EXPECT_TRUE(target->verify_invariants());
}

TEST(OmsRecoveryTest, AmbiguousAbsenceNeverMeansSafeToResend) {
  auto source_journal = std::make_unique<OmsJournal>();
  auto source = std::make_unique<DeterministicOms>(configuration(), *source_journal);
  const auto accepted = source->apply(accept(approval(), 1U));
  (void)source->apply(internal(InputKind::mark_ready, accepted.order_id, 2U));
  (void)source->apply(internal(InputKind::dispatch, accepted.order_id, 3U));

  auto target_journal = std::make_unique<OmsJournal>();
  auto target = std::make_unique<DeterministicOms>(configuration(), *target_journal);
  ASSERT_EQ(target->recover_from(*source_journal, kNow + 100U),
            RecoveryStatus::recovered_inhibited);
  const auto ambiguous = target->apply(recovery_observation(
      accepted.order_id, 202U, RecoveryObservation::absent_ambiguous));
  EXPECT_EQ(ambiguous.status, ApplyStatus::inhibited);
  EXPECT_EQ(ambiguous.reason, OmsReason::ambiguous_absence);
  EXPECT_EQ(ambiguous.current_state, OrderState::unknown_recovery);
  EXPECT_FALSE(target->ready());
}

TEST(OmsRecoveryTest, AuthorityRotationFencesAndRequiresReconciliation) {
  auto journal = std::make_unique<OmsJournal>();
  auto service = std::make_unique<DeterministicOms>(configuration(), *journal);
  const auto order_id = create_working_order(*service);
  OmsInput update{
      .receipt_id = common::GlobalEventId{201U, 1U},
      .kind = InputKind::authority_update,
      .source = EventSource::internal,
      .authority = {.exchange_session_epoch = kAuthority.exchange_session_epoch,
                    .fencing_token = kAuthority.fencing_token + 1U},
      .process_monotonic_time_ns = kNow + 100U};
  update.stable_hash = stable_input_hash(update);
  ASSERT_EQ(service->apply(update).status, ApplyStatus::applied);
  EXPECT_FALSE(service->ready());
  OmsOrderSnapshot order{};
  ASSERT_TRUE(service->lookup_order(order_id, order));
  EXPECT_EQ(order.state, OrderState::unknown_recovery);

  auto stale = internal(InputKind::dispatch, order_id, 102U);
  EXPECT_EQ(service->apply(stale).status, ApplyStatus::fenced);
}

} // namespace
} // namespace aegis::oms::test
