#include "aegis/integration/operator_simulation.hpp"

#include "aegis/common/build_info.hpp"
#include "aegis/market_state/controller.hpp"
#include "aegis/risk/engine.hpp"
#include "aegis/risk/journal.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace aegis::integration {
namespace {

constexpr std::uint64_t kNow = 1'000'000U;
constexpr std::uint64_t kAuthorityEpoch = 43U;
constexpr common::SessionId kSession{1U, 43U};
constexpr common::AccountId kAccount{2U, 43U};
constexpr common::StrategyId kStrategy{3U, 43U};
constexpr common::VenueId kVenue{4U, 43U};
constexpr common::InstrumentId kInstrument{5U, 43U};
constexpr common::ConfigurationVersion kConfiguration{6U, 43U};
constexpr common::RiskSnapshotId kRiskSnapshot{7U, 43U};
constexpr std::string_view kZeroSha256 =
    "0000000000000000000000000000000000000000000000000000000000000000";

[[nodiscard]] std::string digest_hex(const common::Sha256Digest& digest) {
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const auto byte : digest) {
    output << std::setw(2) << static_cast<unsigned int>(byte);
  }
  return output.str();
}

[[nodiscard]] common::Sha256Digest digest_text(const std::string_view input) noexcept {
  return common::sha256(std::span<const std::uint8_t>{
      reinterpret_cast<const std::uint8_t*>(input.data()), input.size()});
}

[[nodiscard]] common::Sha256Digest ordinal_digest(const std::uint64_t seed,
                                                  const std::uint64_t ordinal) {
  std::array<std::uint8_t, sizeof(seed) + sizeof(ordinal)> bytes{};
  for (std::size_t index = 0U; index < sizeof(seed); ++index) {
    bytes[index] = static_cast<std::uint8_t>(seed >> (index * 8U));
    bytes[index + sizeof(seed)] = static_cast<std::uint8_t>(ordinal >> (index * 8U));
  }
  return common::sha256(std::span<const std::uint8_t>{bytes});
}

[[nodiscard]] risk::RiskLimitSnapshot limits() noexcept {
  risk::RiskLimitSnapshot result{
      .risk_snapshot_id = kRiskSnapshot,
      .configuration_version = kConfiguration,
      .policy_version = common::ConfigurationVersion{8U, 43U},
      .session_id = kSession,
      .account_id = kAccount,
      .revision = 1U,
      .authority_epoch = kAuthorityEpoch,
      .published_process_monotonic_time_ns = kNow - 1'000U,
      .valid_until_process_monotonic_time_ns = kNow + 1'000'000U,
      .approval_ttl_ns = 10'000U,
      .maximum_position_age_ns = 10'000U,
      .maximum_market_state_age_ns = 1'000U,
      .maximum_clock_state_age_ns = 1'000U,
      .maximum_feed_book_age_ns = 1'000U,
      .maximum_configuration_age_ns = 10'000U,
      .order_rate_window_ns = 1'000U,
      .cancel_rate_window_ns = 1'000U,
      .maximum_orders_per_window = 100U,
      .maximum_cancels_per_window = 100U,
      .allowed_market_state_mask =
          static_cast<std::uint16_t>(std::uint16_t{1U} << static_cast<std::uint8_t>(
                                         market_state::MarketState::normal)),
      .maximum_gross_exposure_currency_nanos = 1'000'000'000'000U,
      .maximum_absolute_net_exposure_currency_nanos = 1'000'000'000'000U,
      .maximum_daily_loss_currency_nanos = 1'000'000'000U,
      .maximum_drawdown_currency_nanos = 1'000'000'000U,
      .credit_capital_limit_currency_nanos = 1'000'000'000'000U,
      .symbol_count = 1U,
      .strategy_count = 1U,
      .venue_count = 1U,
      .sector_count = 1U,
      .factor_count = 1U,
      .allow_simulation = false,
      .allow_paper = true,
      .require_locate_for_short_sale = true,
      .require_self_trade_prevention = true};
  result.maximum_sector_exposure_currency_nanos[0U] = 1'000'000'000'000U;
  result.maximum_factor_exposure_currency_nanos[0U] = 1'000'000'000'000U;
  result.symbols[0U] = {.instrument_id = kInstrument,
                        .tick_value_currency_nanos = 10U,
                        .price_increment_ticks = 1U,
                        .maximum_order_quantity_units = 1'000U,
                        .maximum_order_notional_currency_nanos = 1'000'000'000U,
                        .maximum_price_deviation_ticks = 100U,
                        .maximum_absolute_position_units = 10'000U,
                        .sector_index = 0U,
                        .factor_beta_ppm = {1'000'000},
                        .authorized = true,
                        .restricted = false};
  result.strategies[0U] = {.strategy_id = kStrategy,
                           .maximum_loss_currency_nanos = 500'000'000U,
                           .maximum_drawdown_currency_nanos = 500'000'000U,
                           .authorized = true};
  result.venues[0U] = {.venue_id = kVenue, .authorized = true};
  result.stable_hash = risk::stable_limit_snapshot_hash(result);
  return result;
}

[[nodiscard]] market_state::MarketStateSnapshot
market_snapshot(const std::uint64_t now) noexcept {
  market_state::MarketStateSnapshot snapshot{
      .state = market_state::MarketState::normal,
      .primary_reason = market_state::MarketStateReason::normal_conditions_stable,
      .reason_mask = market_state::reason_bit(
          market_state::MarketStateReason::normal_conditions_stable),
      .input_sequence = 1U,
      .transition_sequence = 1U,
      .observed_process_monotonic_time_ns = now - 10U,
      .observed_wall_clock_utc_ns = 1'800'000'000'000'000'000ULL,
      .state_entered_process_monotonic_time_ns = now - 100U};
  snapshot.stable_hash = market_state::stable_market_state_hash(snapshot);
  return snapshot;
}

[[nodiscard]] risk::RiskEvaluationRequest request(const std::uint64_t seed,
                                                  const std::uint64_t ordinal) {
  const auto now = kNow + ordinal;
  risk::RiskIntent intent{.intent_id = common::IntentId{20U, ordinal},
                          .session_id = kSession,
                          .account_id = kAccount,
                          .strategy_id = kStrategy,
                          .venue_id = kVenue,
                          .instrument_id = kInstrument,
                          .source_forecast_id = common::ForecastId{21U, ordinal},
                          .feature_snapshot_id =
                              common::FeatureSnapshotId{22U, ordinal},
                          .target_order_id = {},
                          .configuration_version = kConfiguration,
                          .action = risk::IntentAction::buy,
                          .limit_price_ticks = 100,
                          .quantity_units = 10U,
                          .created_process_monotonic_time_ns = now - 100U,
                          .expire_process_monotonic_time_ns = now + 100'000U,
                          .canonical_sha256 = ordinal_digest(seed, ordinal)};
  intent.stable_hash = risk::stable_risk_intent_hash(intent);
  risk::RiskContext context{
      .now_process_monotonic_time_ns = now,
      .trading_mode = risk::TradingMode::paper,
      .operator_authorized = true,
      .session_authorized = true,
      .operator_authorization_valid_until_ns = now + 100'000U,
      .authority_epoch = kAuthorityEpoch,
      .process_id = 43U,
      .market_state_snapshot = market_snapshot(now),
      .official_trading_status = market_state::OfficialTradingStatus::open,
      .feed_health = market_state::FeedHealth::healthy,
      .book_validity = market_state::BookValidity::valid,
      .clock_quality_snapshot = {.state = time::ClockQualityState::healthy,
                                 .reason = time::ClockQualityReason::within_thresholds,
                                 .operation_mode = time::ClockOperationMode::normal,
                                 .observed_at = time::MonotonicTimeNs{now - 10U},
                                 .ptp_offset = time::DurationNs{10},
                                 .drift_ppb = 1,
                                 .source_id = time::ClockSourceId{12U, 43U},
                                 .last_synchronization_time =
                                     time::MonotonicTimeNs{now - 20U},
                                 .synchronization_age = time::DurationNs{20},
                                 .hardware_timestamp_available = true,
                                 .transition_count = 1U,
                                 .observation_count = 2U},
      .feed_book_observed_process_monotonic_time_ns = now - 10U,
      .feed_book_state_hash = 13U,
      .reference_price_ticks = 100,
      .short_sale_locate = risk::PolicyHookResult::allowed,
      .self_trade_prevention = risk::PolicyHookResult::allowed};
  context.stable_hash = risk::stable_risk_context_hash(context);
  return {.global_event_id = common::GlobalEventId{30U, ordinal},
          .intent = intent,
          .context = context};
}

[[nodiscard]] const char* update_status_name(const risk::StateUpdateStatus status) {
  switch (status) {
  case risk::StateUpdateStatus::applied:
    return "applied";
  case risk::StateUpdateStatus::invalid:
    return "invalid";
  case risk::StateUpdateStatus::arithmetic_overflow:
    return "arithmetic_overflow";
  case risk::StateUpdateStatus::unavailable:
    return "unavailable";
  case risk::StateUpdateStatus::busy:
    return "busy";
  }
  return "unknown";
}

void append_audit(OperatorSimulationReport& report, const std::string_view event,
                  const risk::KillSwitchScope* scope,
                  const std::uint64_t command_sequence, const bool operator_authorized,
                  const std::string_view update_status,
                  const risk::RiskDecision* decision) {
  OperatorAuditRecord record;
  record.sequence = report.audit_records.size() + 1U;
  record.previous_sha256 = report.audit_records.empty()
                               ? std::string{kZeroSha256}
                               : report.audit_records.back().record_sha256;
  record.event = event;
  record.scope = scope == nullptr ? "none" : operator_scope_name(*scope);
  record.command_sequence = command_sequence;
  record.operator_authorized = operator_authorized;
  record.update_status = update_status;
  if (decision != nullptr) {
    record.risk_journal_sequence = decision->journal_sequence;
    record.risk_decision_hash = decision->stable_hash;
  }
  std::ostringstream canonical;
  canonical << record.sequence << '|' << record.previous_sha256 << '|' << record.event
            << '|' << record.scope << '|' << record.command_sequence << '|'
            << (record.operator_authorized ? "true" : "false") << '|'
            << record.update_status << '|' << record.risk_journal_sequence << '|'
            << record.risk_decision_hash;
  record.record_sha256 = digest_hex(digest_text(canonical.str()));
  report.audit_records.push_back(std::move(record));
}

[[nodiscard]] bool is_approved(const risk::RiskDecision& decision) noexcept {
  return decision.decision == risk::DecisionCode::approved &&
         decision.reason == risk::RiskReason::within_limits;
}

[[nodiscard]] bool is_kill_rejection(const risk::RiskDecision& decision) noexcept {
  return decision.decision == risk::DecisionCode::rejected &&
         decision.reason == risk::RiskReason::kill_switch_engaged &&
         decision.failed_check == risk::RiskCheck::kill_switch;
}

[[nodiscard]] bool ensure_parent(const std::filesystem::path& path) {
  if (!path.has_parent_path()) {
    return true;
  }
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  return !error;
}

[[nodiscard]] std::string audit_json(const OperatorSimulationReport& report) {
  std::ostringstream output;
  for (const auto& record : report.audit_records) {
    output << R"({"schema_version":"1.0","kind":"operator_drill_audit",)"
           << R"("sequence":)" << record.sequence << R"(,"previous_sha256":")"
           << record.previous_sha256 << R"(","event":")" << record.event
           << R"(","scope":")" << record.scope << R"(","command_sequence":)"
           << record.command_sequence << R"(,"operator_authorized":)"
           << (record.operator_authorized ? "true" : "false") << R"(,"update_status":")"
           << record.update_status << R"(","risk_journal_sequence":)"
           << record.risk_journal_sequence << R"(,"risk_decision_hash":")"
           << record.risk_decision_hash << R"(","record_sha256":")"
           << record.record_sha256 << R"("})" << '\n';
  }
  return output.str();
}

} // namespace

const char* operator_scope_name(const risk::KillSwitchScope scope) noexcept {
  switch (scope) {
  case risk::KillSwitchScope::symbol:
    return "symbol";
  case risk::KillSwitchScope::strategy:
    return "strategy";
  case risk::KillSwitchScope::venue:
    return "venue";
  case risk::KillSwitchScope::account:
    return "account";
  case risk::KillSwitchScope::firm:
    return "firm";
  }
  return "unknown";
}

OperatorSimulationReport run_operator_simulation(const std::uint64_t seed) {
  OperatorSimulationReport report;
  report.seed = seed;
  report.production_activation_attempted = false;
  report.paper_mode_only = !common::current_build_info().live_trading_capable;

  risk::RiskDecisionJournal journal;
  risk::DeterministicPreTradeRiskEngine engine{limits(), journal};
  const auto position_status =
      engine.seed_position({.symbol_index = 0U,
                            .net_quantity_units = 0,
                            .mark_price_ticks = 100,
                            .as_of_process_monotonic_time_ns = kNow - 10U});
  const auto pnl_status =
      engine.update_profit_loss({.strategy_index = 0U,
                                 .firm_daily_pnl_currency_nanos = 0,
                                 .firm_peak_pnl_currency_nanos = 0,
                                 .strategy_pnl_currency_nanos = 0,
                                 .strategy_peak_pnl_currency_nanos = 0,
                                 .as_of_process_monotonic_time_ns = kNow - 10U});
  if (position_status != risk::StateUpdateStatus::applied ||
      pnl_status != risk::StateUpdateStatus::applied) {
    return report;
  }

  report.baseline_decision = engine.evaluate(request(seed, 1U)).decision;
  report.baseline_approved = is_approved(report.baseline_decision);
  append_audit(report, "baseline_approved", nullptr, 0U, true, "not_applicable",
               &report.baseline_decision);

  constexpr std::array scopes{
      risk::KillSwitchScope::symbol, risk::KillSwitchScope::strategy,
      risk::KillSwitchScope::venue, risk::KillSwitchScope::firm};
  std::uint64_t command_sequence = 1U;
  std::uint64_t intent_ordinal = 2U;
  for (std::size_t index = 0U; index < scopes.size(); ++index) {
    auto& result = report.scopes[index];
    result.scope = scopes[index];
    result.engage_command_sequence = command_sequence++;
    result.engage_status =
        engine.update_kill_switch({.scope = result.scope,
                                   .target_index = 0U,
                                   .authority_epoch = kAuthorityEpoch,
                                   .command_sequence = result.engage_command_sequence,
                                   .engaged = true,
                                   .operator_authorized = false});
    append_audit(report, "kill_engaged", &result.scope, result.engage_command_sequence,
                 false, update_status_name(result.engage_status), nullptr);

    result.blocked_decision = engine.evaluate(request(seed, intent_ordinal++)).decision;
    append_audit(report, "kill_rejection_observed", &result.scope,
                 result.engage_command_sequence, true, "not_applicable",
                 &result.blocked_decision);

    result.clear_command_sequence = command_sequence++;
    result.unauthorized_clear_status =
        engine.update_kill_switch({.scope = result.scope,
                                   .target_index = 0U,
                                   .authority_epoch = kAuthorityEpoch,
                                   .command_sequence = result.clear_command_sequence,
                                   .engaged = false,
                                   .operator_authorized = false});
    append_audit(report, "unauthorized_clear_rejected", &result.scope,
                 result.clear_command_sequence, false,
                 update_status_name(result.unauthorized_clear_status), nullptr);

    result.authorized_clear_status =
        engine.update_kill_switch({.scope = result.scope,
                                   .target_index = 0U,
                                   .authority_epoch = kAuthorityEpoch,
                                   .command_sequence = result.clear_command_sequence,
                                   .engaged = false,
                                   .operator_authorized = true});
    append_audit(report, "authorized_clear_applied", &result.scope,
                 result.clear_command_sequence, true,
                 update_status_name(result.authorized_clear_status), nullptr);

    result.recovered_decision =
        engine.evaluate(request(seed, intent_ordinal++)).decision;
    append_audit(report, "recovery_approved", &result.scope,
                 result.clear_command_sequence, true, "not_applicable",
                 &result.recovered_decision);
    result.passed =
        result.engage_status == risk::StateUpdateStatus::applied &&
        is_kill_rejection(result.blocked_decision) &&
        result.unauthorized_clear_status == risk::StateUpdateStatus::invalid &&
        result.authorized_clear_status == risk::StateUpdateStatus::applied &&
        is_approved(result.recovered_decision);
  }

  report.every_kill_blocked = true;
  report.unauthorized_clear_rejected = true;
  report.authorized_recovery_succeeded = true;
  for (const auto& result : report.scopes) {
    report.every_kill_blocked =
        report.every_kill_blocked && is_kill_rejection(result.blocked_decision);
    report.unauthorized_clear_rejected =
        report.unauthorized_clear_rejected &&
        result.unauthorized_clear_status == risk::StateUpdateStatus::invalid;
    report.authorized_recovery_succeeded =
        report.authorized_recovery_succeeded &&
        result.authorized_clear_status == risk::StateUpdateStatus::applied &&
        is_approved(result.recovered_decision);
  }

  std::array<std::uint64_t, kOperatorSimulationRiskDecisionCount> expected_hashes{};
  expected_hashes[0U] = report.baseline_decision.stable_hash;
  for (std::size_t index = 0U; index < report.scopes.size(); ++index) {
    expected_hashes[(index * 2U) + 1U] =
        report.scopes[index].blocked_decision.stable_hash;
    expected_hashes[(index * 2U) + 2U] =
        report.scopes[index].recovered_decision.stable_hash;
  }
  report.decision_journal_complete = true;
  risk::RiskDecision extracted;
  for (std::size_t index = 0U; index < expected_hashes.size(); ++index) {
    if (!journal.try_pop(extracted) || extracted.journal_sequence != index + 1U ||
        extracted.stable_hash != expected_hashes[index]) {
      report.decision_journal_complete = false;
      break;
    }
    ++report.extracted_risk_decisions;
  }
  report.risk_journal_records_remaining = journal.size();
  report.decision_journal_complete =
      report.decision_journal_complete &&
      report.extracted_risk_decisions == expected_hashes.size() && journal.size() == 0U;

  const auto audit = audit_json(report);
  report.audit_extract_sha256 = digest_text(audit);
  report.audit_chain_final_sha256 = report.audit_records.empty()
                                        ? std::string{kZeroSha256}
                                        : report.audit_records.back().record_sha256;
  report.audit_chain_valid = report.audit_records.size() == 21U &&
                             report.audit_chain_final_sha256.size() == 64U;
  report.passed = report.paper_mode_only && !report.production_activation_attempted &&
                  report.baseline_approved && report.every_kill_blocked &&
                  report.unauthorized_clear_rejected &&
                  report.authorized_recovery_succeeded &&
                  report.decision_journal_complete && report.audit_chain_valid;
  return report;
}

bool write_operator_audit_extract(const OperatorSimulationReport& report,
                                  const std::filesystem::path& path) {
  if (!ensure_parent(path)) {
    return false;
  }
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  const auto audit = audit_json(report);
  output.write(audit.data(), static_cast<std::streamsize>(audit.size()));
  return output.good() && digest_text(audit) == report.audit_extract_sha256;
}

bool write_operator_simulation_report(const OperatorSimulationReport& report,
                                      const OperatorSimulationOutputPaths& paths) {
  if (!ensure_parent(paths.machine_report)) {
    return false;
  }
  const auto& build = common::current_build_info();
  std::ofstream output{paths.machine_report, std::ios::trunc};
  if (!output) {
    return false;
  }
  output << '{' << '\n'
         << R"(  "schema_version": "1.0",)" << '\n'
         << R"(  "kind": "paper_operator_simulation",)" << '\n'
         << R"(  "mode": "PAPER",)" << '\n'
         << R"(  "live_trading_compiled": )"
         << (build.live_trading_capable ? "true" : "false") << ',' << '\n'
         << R"(  "production_activation_attempted": )"
         << (report.production_activation_attempted ? "true" : "false") << ',' << '\n'
         << R"(  "seed": )" << report.seed << ',' << '\n'
         << R"(  "source_revision": ")" << build.source_revision << R"(",)" << '\n'
         << R"(  "audit_extract": {)" << '\n'
         << R"(    "path": ")" << paths.audit_extract.filename().string() << R"(",)"
         << '\n'
         << R"(    "sha256": ")" << digest_hex(report.audit_extract_sha256) << R"(",)"
         << '\n'
         << R"(    "chain_final_sha256": ")" << report.audit_chain_final_sha256
         << R"(",)" << '\n'
         << R"(    "records": )" << report.audit_records.size() << ',' << '\n'
         << R"(    "risk_decisions": )" << report.extracted_risk_decisions << '\n'
         << R"(  },)" << '\n'
         << R"(  "checks": {)" << '\n'
         << R"(    "paper_mode_only": )" << (report.paper_mode_only ? "true" : "false")
         << ',' << '\n'
         << R"(    "baseline_approved": )"
         << (report.baseline_approved ? "true" : "false") << ',' << '\n'
         << R"(    "every_kill_blocked": )"
         << (report.every_kill_blocked ? "true" : "false") << ',' << '\n'
         << R"(    "unauthorized_clear_rejected": )"
         << (report.unauthorized_clear_rejected ? "true" : "false") << ',' << '\n'
         << R"(    "authorized_recovery_succeeded": )"
         << (report.authorized_recovery_succeeded ? "true" : "false") << ',' << '\n'
         << R"(    "decision_journal_complete": )"
         << (report.decision_journal_complete ? "true" : "false") << ',' << '\n'
         << R"(    "audit_chain_valid": )"
         << (report.audit_chain_valid ? "true" : "false") << '\n'
         << R"(  },)" << '\n'
         << R"(  "scopes": [)" << '\n';
  for (std::size_t index = 0U; index < report.scopes.size(); ++index) {
    const auto& scope = report.scopes[index];
    output << R"(    {"scope": ")" << operator_scope_name(scope.scope)
           << R"(", "engage_sequence": )" << scope.engage_command_sequence
           << R"(, "clear_sequence": )" << scope.clear_command_sequence
           << R"(, "blocked_journal_sequence": )"
           << scope.blocked_decision.journal_sequence
           << R"(, "recovered_journal_sequence": )"
           << scope.recovered_decision.journal_sequence << R"(, "passed": )"
           << (scope.passed ? "true" : "false") << '}';
    output << (index + 1U == report.scopes.size() ? "\n" : ",\n");
  }
  output << R"(  ],)" << '\n'
         << R"(  "passed": )" << (report.passed ? "true" : "false") << '\n'
         << '}' << '\n';
  return output.good();
}

} // namespace aegis::integration
