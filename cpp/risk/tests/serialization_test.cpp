#include "aegis/risk/serialization.hpp"

#include "portfolio_test_support.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <span>

namespace risk = aegis::risk;
namespace common = aegis::common;

namespace {

[[nodiscard]] std::span<const std::uint8_t>
bytes(const flatbuffers::DetachedBuffer& buffer) noexcept {
  return {buffer.data(), buffer.size()};
}

[[nodiscard]] common::AuditMetadata metadata() noexcept {
  return {.envelope_id = common::GlobalEventId{100U, 100U},
          .session_id = risk::test::kSession,
          .configuration_version = risk::test::kConfiguration,
          .created_process_monotonic_time_ns = risk::test::kNow + 1U,
          .recorded_wall_clock_utc_time_ns = 1'800'000'000'000'000'000LL,
          .previous_envelope_sha256 = {}};
}

} // namespace

TEST(RiskSerializationTest, RoundTripsApprovalThroughAuditEnvelope) {
  risk::RiskDecisionJournal journal;
  risk::DeterministicPreTradeRiskEngine engine{risk::test::limits(), journal};
  risk::test::initialize_state(engine);
  const auto result = engine.evaluate(risk::test::request());
  ASSERT_TRUE(risk::valid_risk_decision(result.decision));
  const auto contract = risk::build_risk_decision_contract(result.decision);
  ASSERT_NE(contract.data(), nullptr);
  const auto envelope = common::build_size_prefixed_audit_envelope(
      bytes(contract), common::wire::RecordType::RISK_DECISION, metadata());

  common::ValidatedAuditEnvelopeView view{};
  const auto validation =
      common::validate_size_prefixed_audit_envelope(bytes(envelope), &view);
  ASSERT_TRUE(validation.ok());
  const auto* wire = view.record->payload_as_RiskDecision();
  ASSERT_NE(wire, nullptr);
  EXPECT_EQ(wire->decision_hash(), result.decision.stable_hash);
  EXPECT_EQ(wire->failed_check(), common::wire::RiskCheckCode::NONE);
  EXPECT_EQ(wire->completed_check_mask(), (std::uint32_t{1U} << 30U) - 1U);
  EXPECT_EQ(wire->account_id()->high(), risk::test::kAccount.high());
}

TEST(RiskSerializationTest, RoundTripsExplicitRejectionReason) {
  risk::RiskDecisionJournal journal;
  risk::DeterministicPreTradeRiskEngine engine{risk::test::limits(), journal};
  risk::test::initialize_state(engine);
  auto request = risk::test::request();
  request.context.trading_mode = risk::TradingMode::live;
  request.context.stable_hash = risk::stable_risk_context_hash(request.context);
  const auto result = engine.evaluate(request);
  ASSERT_TRUE(risk::valid_risk_decision(result.decision));
  const auto contract = risk::build_risk_decision_contract(result.decision);
  const auto envelope = common::build_size_prefixed_audit_envelope(
      bytes(contract), common::wire::RecordType::RISK_DECISION, metadata());

  common::ValidatedAuditEnvelopeView view{};
  const auto validation =
      common::validate_size_prefixed_audit_envelope(bytes(envelope), &view);
  ASSERT_TRUE(validation.ok()) << static_cast<int>(validation.error());
  const auto* wire = view.record->payload_as_RiskDecision();
  ASSERT_NE(wire, nullptr);
  EXPECT_EQ(wire->reason(), common::wire::RiskReasonCode::TRADING_MODE_UNAUTHORIZED);
  EXPECT_EQ(wire->failed_check(),
            common::wire::RiskCheckCode::TRADING_MODE_AUTHORIZATION);
}

TEST(RiskSerializationTest, RejectsTamperedDecisionEvenWithRecomputedHash) {
  risk::RiskDecisionJournal journal;
  risk::DeterministicPreTradeRiskEngine engine{risk::test::limits(), journal};
  risk::test::initialize_state(engine);
  auto decision = engine.evaluate(risk::test::request()).decision;
  decision.completed_check_mask &= ~risk::check_bit(risk::RiskCheck::price_collar);
  decision.stable_hash = risk::stable_risk_decision_hash(decision);

  const auto contract = risk::build_risk_decision_contract(decision);
  EXPECT_EQ(contract.data(), nullptr);
}

TEST(RiskSerializationTest, RoundTripsVersionedPortfolioPositionEvidence) {
  namespace portfolio = risk::portfolio;
  namespace portfolio_test = aegis::risk::portfolio::test;
  portfolio::PortfolioJournal journal;
  portfolio::PortfolioRiskService service{portfolio_test::configuration(), journal};
  ASSERT_EQ(service.apply(portfolio_test::mark(1U, 110)).status,
            portfolio::ApplyStatus::applied);
  ASSERT_EQ(service.apply(portfolio_test::order(2U)).status,
            portfolio::ApplyStatus::applied);
  ASSERT_EQ(service.apply(portfolio_test::fill(3U, 1U)).status,
            portfolio::ApplyStatus::applied);
  const auto snapshot = service.snapshot_for_quiescent_inspection();
  const auto event_id = common::GlobalEventId{201U, 1U};
  const auto contract = risk::build_position_snapshot_contract(snapshot, 0U, event_id);
  ASSERT_NE(contract.data(), nullptr);
  const common::AuditMetadata position_metadata{
      .envelope_id = common::GlobalEventId{202U, 1U},
      .session_id = portfolio_test::kSession,
      .configuration_version = portfolio_test::kConfiguration,
      .created_process_monotonic_time_ns = portfolio_test::kStart + 4U,
      .recorded_wall_clock_utc_time_ns = 1'800'000'000'000'000'000LL,
      .previous_envelope_sha256 = {}};
  const auto envelope = common::build_size_prefixed_audit_envelope(
      bytes(contract), common::wire::RecordType::POSITION_SNAPSHOT, position_metadata);
  common::ValidatedAuditEnvelopeView view{};
  const auto validation =
      common::validate_size_prefixed_audit_envelope(bytes(envelope), &view);
  ASSERT_TRUE(validation.ok()) << static_cast<int>(validation.error());
  const auto* wire = view.record->payload_as_PositionSnapshot();
  ASSERT_NE(wire, nullptr);
  const auto* schema_version = wire->schema_version();
  ASSERT_NE(schema_version, nullptr);
  EXPECT_EQ(schema_version->minor(), common::kCurrentSchemaMinor);
  EXPECT_EQ(wire->portfolio_sequence(), snapshot.sequence);
  EXPECT_EQ(wire->portfolio_snapshot_hash(), snapshot.stable_hash);
  EXPECT_EQ(wire->average_price_ticks(), 100);
  EXPECT_EQ(wire->health(), common::wire::PortfolioHealthCode::HEALTHY);
  EXPECT_TRUE(wire->ready());
}
