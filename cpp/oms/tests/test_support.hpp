#ifndef AEGIS_OMS_TEST_SUPPORT_HPP
#define AEGIS_OMS_TEST_SUPPORT_HPP

#include "aegis/oms/service.hpp"

#include "../../risk/tests/test_support.hpp"

#include <cstdint>

namespace aegis::oms::test {

inline constexpr std::uint64_t kNow = risk::test::kNow;
inline constexpr LeaderAuthority kAuthority{.exchange_session_epoch = 77U,
                                            .fencing_token = 88U};
inline constexpr ExternalOrderId kExternal{.high = 91U, .low = 92U};

struct Approval {
  risk::RiskIntent intent{};
  risk::RiskDecision decision{};
};

struct ApprovalTerms {
  common::OrderId target;
  std::int64_t price_ticks{100};
  std::uint64_t quantity_units{10U};
};

[[nodiscard]] inline OmsConfiguration
configuration(const bool require_drop_copy = false) noexcept {
  OmsConfiguration result{.session_id = risk::test::kSession,
                          .account_id = risk::test::kAccount,
                          .configuration_version = risk::test::kConfiguration,
                          .trading_mode = risk::TradingMode::simulation,
                          .exchange_session_epoch = kAuthority.exchange_session_epoch,
                          .initial_fencing_token = kAuthority.fencing_token,
                          .maximum_order_age_ns = 1'000'000U,
                          .require_drop_copy = require_drop_copy};
  result.stable_hash = stable_configuration_hash(result);
  return result;
}

[[nodiscard]] inline Approval
approval(const std::uint64_t ordinal = 1U,
         const risk::IntentAction action = risk::IntentAction::buy,
         const ApprovalTerms terms = {}) {
  risk::RiskDecisionJournal journal;
  risk::DeterministicPreTradeRiskEngine engine{risk::test::limits(), journal};
  risk::test::initialize_state(engine);
  auto request = risk::test::request(ordinal, action);
  request.intent.limit_price_ticks = terms.price_ticks;
  request.intent.quantity_units = terms.quantity_units;
  if (action == risk::IntentAction::cancel) {
    request.intent.target_order_id = terms.target;
    request.intent.limit_price_ticks = 0;
    request.intent.quantity_units = 0U;
  }
  request.intent.stable_hash = risk::stable_risk_intent_hash(request.intent);
  const auto result = engine.evaluate(request);
  return {.intent = request.intent, .decision = result.decision};
}

[[nodiscard]] inline OmsInput accept(const Approval& approved,
                                     const std::uint64_t receipt = 1U) noexcept {
  OmsInput input{.receipt_id = common::GlobalEventId{100U, receipt},
                 .kind = InputKind::accept_intent,
                 .source = EventSource::internal,
                 .intent = approved.intent,
                 .risk_decision = approved.decision,
                 .process_monotonic_time_ns = kNow + receipt};
  input.stable_hash = stable_input_hash(input);
  return input;
}

[[nodiscard]] inline OmsInput internal(const InputKind kind,
                                       const common::OrderId order_id,
                                       const std::uint64_t receipt) noexcept {
  OmsInput input{.receipt_id = common::GlobalEventId{101U, receipt},
                 .kind = kind,
                 .source = EventSource::internal,
                 .order_id = order_id,
                 .authority = kAuthority,
                 .process_monotonic_time_ns = kNow + receipt};
  input.stable_hash = stable_input_hash(input);
  return input;
}

[[nodiscard]] inline OmsInput cancel(const common::OrderId order_id,
                                     const std::uint64_t receipt) {
  const auto approved =
      approval(1'000U + receipt, risk::IntentAction::cancel, {.target = order_id});
  OmsInput input{.receipt_id = common::GlobalEventId{102U, receipt},
                 .kind = InputKind::cancel_intent,
                 .source = EventSource::internal,
                 .order_id = order_id,
                 .intent = approved.intent,
                 .risk_decision = approved.decision,
                 .authority = kAuthority,
                 .process_monotonic_time_ns = kNow + receipt};
  input.stable_hash = stable_input_hash(input);
  return input;
}

[[nodiscard]] inline OmsInput replace(const common::OrderId order_id,
                                      const std::uint64_t receipt,
                                      const std::int64_t price_ticks,
                                      const std::uint64_t quantity_units) {
  const auto approved = approval(2'000U + receipt, risk::IntentAction::buy,
                                 {.target = common::OrderId{},
                                  .price_ticks = price_ticks,
                                  .quantity_units = quantity_units});
  OmsInput input{.receipt_id = common::GlobalEventId{103U, receipt},
                 .kind = InputKind::replace_intent,
                 .source = EventSource::internal,
                 .order_id = order_id,
                 .intent = approved.intent,
                 .risk_decision = approved.decision,
                 .authority = kAuthority,
                 .process_monotonic_time_ns = kNow + receipt};
  input.stable_hash = stable_input_hash(input);
  return input;
}

[[nodiscard]] inline OmsInput
venue(const InputKind kind, const common::OrderId order_id, const std::uint64_t receipt,
      const std::uint64_t sequence, const ExternalOrderId external = kExternal,
      const EventSource source = EventSource::gateway) noexcept {
  OmsInput input{.receipt_id = common::GlobalEventId{104U, receipt},
                 .kind = kind,
                 .source = source,
                 .order_id = order_id,
                 .external_order_id = external,
                 .authority = kAuthority,
                 .venue_sequence = sequence,
                 .process_monotonic_time_ns = kNow + receipt};
  input.stable_hash = stable_input_hash(input);
  return input;
}

[[nodiscard]] inline OmsInput
fill(const common::OrderId order_id, const std::uint64_t receipt,
     const std::uint64_t sequence, const std::uint64_t execution,
     const std::uint64_t quantity, const EventSource source = EventSource::gateway,
     const std::int64_t price = 100) noexcept {
  OmsInput input{.receipt_id = common::GlobalEventId{105U, receipt},
                 .kind = InputKind::fill,
                 .source = source,
                 .order_id = order_id,
                 .external_order_id = kExternal,
                 .execution_id = common::GlobalEventId{106U, execution},
                 .authority = kAuthority,
                 .price_ticks = price,
                 .quantity_units = quantity,
                 .venue_sequence = sequence,
                 .exchange_event_time_ns =
                     1'800'000'000'000'000'000LL + static_cast<std::int64_t>(receipt),
                 .nic_receive_time_ns =
                     1'800'000'000'000'000'100LL + static_cast<std::int64_t>(receipt),
                 .process_monotonic_time_ns = kNow + receipt};
  input.stable_hash = stable_input_hash(input);
  return input;
}

[[nodiscard]] inline common::OrderId
create_working_order(DeterministicOms& service, const std::uint64_t ordinal = 1U) {
  const auto created = service.apply(accept(approval(ordinal), (ordinal * 10U) + 1U));
  const auto order_id = created.order_id;
  (void)service.apply(internal(InputKind::mark_ready, order_id, (ordinal * 10U) + 2U));
  (void)service.apply(internal(InputKind::dispatch, order_id, (ordinal * 10U) + 3U));
  (void)service.apply(
      venue(InputKind::acknowledgement, order_id, (ordinal * 10U) + 4U, ordinal));
  return order_id;
}

} // namespace aegis::oms::test

#endif // AEGIS_OMS_TEST_SUPPORT_HPP
