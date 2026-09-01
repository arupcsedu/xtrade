#include "aegis/execution/decoders.hpp"
#include "aegis/execution/simulated_gateway.hpp"
#include "aegis/market_data/synthetic/generator.hpp"
#include "aegis/oms/service.hpp"

#include "test_support.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <utility>

namespace aegis::execution::test {
namespace {

[[nodiscard]] oms::OmsConfiguration oms_configuration(const GatewayMode mode) noexcept {
  oms::OmsConfiguration result{
      .session_id = risk::test::kSession,
      .account_id = risk::test::kAccount,
      .configuration_version = risk::test::kConfiguration,
      .trading_mode = mode == GatewayMode::paper ? risk::TradingMode::paper
                                                 : risk::TradingMode::simulation,
      .exchange_session_epoch = kAuthority.exchange_session_epoch,
      .initial_fencing_token = kAuthority.fencing_token,
      .maximum_order_age_ns = 1'000'000U,
      .require_drop_copy = false};
  result.stable_hash = oms::stable_configuration_hash(result);
  return result;
}

[[nodiscard]] oms::OmsInput accept_input(const ApprovedIntent& approved,
                                         const std::uint64_t receipt) noexcept {
  oms::OmsInput input{.receipt_id = common::GlobalEventId{800U, receipt},
                      .kind = oms::InputKind::accept_intent,
                      .source = oms::EventSource::internal,
                      .intent = approved.intent,
                      .risk_decision = approved.decision,
                      .process_monotonic_time_ns = kNow + receipt};
  input.stable_hash = oms::stable_input_hash(input);
  return input;
}

[[nodiscard]] oms::OmsInput internal_input(const oms::InputKind kind,
                                           const common::OrderId order_id,
                                           const std::uint64_t receipt) noexcept {
  oms::OmsInput input{.receipt_id = common::GlobalEventId{801U, receipt},
                      .kind = kind,
                      .source = oms::EventSource::internal,
                      .order_id = order_id,
                      .authority = kAuthority,
                      .process_monotonic_time_ns = kNow + receipt};
  input.stable_hash = oms::stable_input_hash(input);
  return input;
}

struct CertificationResult {
  std::uint64_t journal_hash{};
  std::uint64_t generated_events{};
  std::uint64_t normalized_responses{};
  std::uint64_t fills{};
};

[[nodiscard]] CertificationResult run_synthetic_certification() {
  auto generator_config = market_data::synthetic::make_default_config();
  generator_config.seed = 20'260'831U;
  generator_config.event_count = 250U;
  generator_config.venue_count = 1U;
  generator_config.instrument_count = 1U;
  generator_config.instruments[0U].venue_number = 1U;
  generator_config.instruments[0U].channel_number = 1U;
  generator_config.instruments[0U].instrument_number = 1U;
  generator_config.instruments[0U].initial_mid_price_ticks = 100;
  generator_config.process_monotonic_start_ns = kNow + 200U;
  generator_config.market_order_intensity_ppm = kPartsPerMillion;
  generator_config.cancellation_intensity_ppm = 0U;
  generator_config.scenario = market_data::synthetic::Scenario::normal;
  auto generator =
      market_data::synthetic::SyntheticExchangeGenerator::create(generator_config);
  if (!generator.has_value()) {
    return {};
  }

  auto journal = std::make_unique<GatewayAuditJournal>();
  auto gateway = std::make_unique<SyntheticExchangeGateway>(configuration(), *journal);
  if (!gateway->start(kNow - 100U)) {
    return {};
  }
  const auto submitted = gateway->send_order(request());
  if (submitted.status != GatewaySubmitStatus::accepted) {
    return {};
  }
  CertificationResult result{};
  market_data::synthetic::SyntheticEvent market_event{};
  while (true) {
    const auto generated = generator->next(market_event);
    if (generated.error == market_data::synthetic::GenerationError::complete) {
      break;
    }
    if (!generated.ok() ||
        gateway->on_market_event(market_event) != GatewayReason::accepted) {
      return {};
    }
    ++result.generated_events;
    GatewayEvent response{};
    while (gateway->poll_event(market_event.process_monotonic_time_ns, response) ==
           GatewayPollStatus::event) {
      ++result.normalized_responses;
      result.fills += response.kind == GatewayResponseKind::fill ? 1U : 0U;
    }
  }
  result.journal_hash = journal->last_record_hash();
  return result;
}

} // namespace

TEST(GatewayCertificationTest, OmsGatewayDecoderRoundTripIsDeterministic) {
  const auto approved = approval(GatewayMode::simulation);
  auto oms_journal = std::make_unique<oms::OmsJournal>();
  auto oms_service = std::make_unique<oms::DeterministicOms>(
      oms_configuration(GatewayMode::simulation), *oms_journal);
  const auto created = oms_service->apply(accept_input(approved, 1U));
  ASSERT_EQ(created.status, oms::ApplyStatus::applied);
  ASSERT_EQ(
      oms_service
          ->apply(internal_input(oms::InputKind::mark_ready, created.order_id, 2U))
          .status,
      oms::ApplyStatus::applied);
  const auto dispatched = oms_service->apply(
      internal_input(oms::InputKind::dispatch, created.order_id, 3U));
  ASSERT_EQ(dispatched.gateway_command.kind, oms::GatewayCommandKind::new_order);

  GatewayAuditJournal gateway_journal;
  SyntheticExchangeGateway gateway{configuration(), gateway_journal};
  ASSERT_TRUE(gateway.start(kNow));
  auto gateway_request = GatewayRequest{.command = dispatched.gateway_command,
                                        .risk_decision = approved.decision,
                                        .safety = safety(kNow + 10U)};
  gateway_request.stable_hash = stable_gateway_request_hash(gateway_request);
  ASSERT_EQ(gateway.send_order(gateway_request).status, GatewaySubmitStatus::accepted);
  GatewayEvent acknowledgement{};
  ASSERT_EQ(gateway.poll_event(kNow + 110U, acknowledgement), GatewayPollStatus::event);
  oms::OmsInput normalized{};
  ASSERT_EQ(AcknowledgementDecoder{}.decode(acknowledgement, normalized),
            AdapterStatus::decoded);
  const auto applied = oms_service->apply(normalized);
  EXPECT_EQ(applied.status, oms::ApplyStatus::applied);
  EXPECT_EQ(applied.current_state, oms::OrderState::working);
  EXPECT_TRUE(oms_service->verify_invariants());
  EXPECT_TRUE(gateway_journal.verify_chain());
}

TEST(GatewayCertificationTest, FixedSeedSyntheticVenueProducesStableAudit) {
  const auto first = run_synthetic_certification();
  const auto second = run_synthetic_certification();
  ASSERT_EQ(first.generated_events, 250U);
  EXPECT_EQ(first.generated_events, second.generated_events);
  EXPECT_EQ(first.normalized_responses, second.normalized_responses);
  EXPECT_EQ(first.fills, second.fills);
  EXPECT_EQ(first.journal_hash, second.journal_hash);
  EXPECT_NE(first.journal_hash, 0U);
  EXPECT_GT(first.normalized_responses, 0U);
}

} // namespace aegis::execution::test
