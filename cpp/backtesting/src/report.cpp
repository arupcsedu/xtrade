#include "aegis/backtesting/backtester.hpp"

#include "aegis/market_data/synthetic/hash.hpp"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>

namespace aegis::backtesting {
namespace synthetic = market_data::synthetic;
namespace {

constexpr std::uint64_t kHardMaximumEvents = 10'000'000U;
constexpr std::uint64_t kHardMaximumOrders = 1'000'000U;
constexpr std::uint64_t kHardMaximumFills = 5'000'000U;

template <typename Identifier>
void add_identifier(synthetic::StableHash64& hash, const Identifier& value) noexcept {
  hash.add_u64(value.high());
  hash.add_u64(value.low());
}

[[nodiscard]] std::string digest_hex(const common::Sha256Digest& digest) {
  constexpr std::string_view digits = "0123456789abcdef";
  std::string result;
  result.resize(64U);
  for (std::size_t index = 0U; index < digest.size(); ++index) {
    result[index * 2U] = digits[digest[index] >> 4U];
    result[(index * 2U) + 1U] = digits[digest[index] & 0x0FU];
  }
  return result;
}

} // namespace

bool valid_config(const BacktestConfig& config) noexcept {
  if (config.deterministic_seed == 0U || config.instrument_count == 0U ||
      config.instrument_count > config.instruments.size() ||
      config.maximum_events == 0U || config.maximum_events > kHardMaximumEvents ||
      config.maximum_orders == 0U || config.maximum_orders > kHardMaximumOrders ||
      config.maximum_external_orders == 0U ||
      config.maximum_external_orders > kHardMaximumOrders ||
      config.maximum_fills == 0U || config.maximum_fills > kHardMaximumFills ||
      config.stale_order_after_ns == 0U || config.default_forecast_horizon_ns == 0U ||
      config.adverse_selection_horizon_ns == 0U ||
      config.reject_probability_ppm > kProbabilityScale ||
      config.hidden_liquidity_probability_ppm > kProbabilityScale) {
    return false;
  }
  const auto& latency = config.acknowledgement_latency;
  if ((latency.kind == LatencyDistributionKind::uniform &&
       latency.maximum_ns < latency.minimum_ns) ||
      latency.secondary_probability_ppm > kProbabilityScale) {
    return false;
  }
  bool has_zero_cost = false;
  bool has_realistic_cost = false;
  for (const auto multiplier : config.cost_multipliers_ppm) {
    has_zero_cost = has_zero_cost || multiplier == 0U;
    has_realistic_cost = has_realistic_cost || multiplier == kProbabilityScale;
  }
  if (!has_zero_cost || !has_realistic_cost) {
    return false;
  }
  for (std::size_t index = 0U; index < config.instrument_count; ++index) {
    const auto& instrument = config.instruments[index];
    if (instrument.venue_number == 0U || instrument.instrument_number == 0U ||
        !instrument.venue_id.valid() || !instrument.instrument_id.valid() ||
        instrument.tick_value_currency_nanos <= 0) {
      return false;
    }
    for (std::size_t other = 0U; other < index; ++other) {
      if ((config.instruments[other].venue_number == instrument.venue_number &&
           config.instruments[other].instrument_number ==
               instrument.instrument_number) ||
          (config.instruments[other].venue_id == instrument.venue_id &&
           config.instruments[other].instrument_id == instrument.instrument_id)) {
        return false;
      }
    }
  }
  return true;
}

std::uint64_t configuration_hash(BacktestConfig config) noexcept {
  synthetic::StableHash64 hash;
  hash.add_u64(config.deterministic_seed);
  hash.add_u64(config.maximum_events);
  hash.add_u64(config.maximum_orders);
  hash.add_u64(config.maximum_external_orders);
  hash.add_u64(config.maximum_fills);
  hash.add_u64(config.stale_order_after_ns);
  hash.add_u64(config.default_forecast_horizon_ns);
  hash.add_u64(config.adverse_selection_horizon_ns);
  hash.add_byte(static_cast<std::uint8_t>(config.acknowledgement_latency.kind));
  hash.add_u64(config.acknowledgement_latency.minimum_ns);
  hash.add_u64(config.acknowledgement_latency.maximum_ns);
  hash.add_u64(config.acknowledgement_latency.secondary_ns);
  hash.add_u32(config.acknowledgement_latency.secondary_probability_ppm);
  hash.add_u32(config.reject_probability_ppm);
  hash.add_u32(config.hidden_liquidity_probability_ppm);
  hash.add_u64(config.maximum_hidden_liquidity_units);
  hash.add_i64(config.maker_fee_per_unit_currency_nanos);
  hash.add_i64(config.taker_fee_per_unit_currency_nanos);
  hash.add_u64(config.slippage_ticks);
  hash.add_u64(config.impact_ticks_per_million_units);
  hash.add_byte(static_cast<std::uint8_t>(config.allow_auction_orders));
  hash.add_u32(config.instrument_count);
  for (std::size_t index = 0U; index < config.instrument_count; ++index) {
    const auto& instrument = config.instruments[index];
    hash.add_u32(instrument.venue_number);
    hash.add_u32(instrument.instrument_number);
    add_identifier(hash, instrument.venue_id);
    add_identifier(hash, instrument.instrument_id);
    hash.add_i64(instrument.tick_value_currency_nanos);
  }
  for (const auto multiplier : config.cost_multipliers_ppm) {
    hash.add_u32(multiplier);
  }
  return hash.value();
}

// NOLINTBEGIN(modernize-raw-string-literal)
std::string report_json(const BacktestReport& report) {
  std::ostringstream output;
  output.imbue(std::locale::classic());
  output << std::setprecision(12);
  output << "{\n"
         << "  \"schema\": \"aegis-mx-event-backtest-report-v1\",\n"
         << "  \"status\": \"" << status_name(report.status) << "\",\n"
         << "  \"deterministic_seed\": " << report.deterministic_seed << ",\n"
         << "  \"configuration_hash\": " << report.configuration_hash << ",\n"
         << "  \"source_events\": " << report.source_events << ",\n"
         << "  \"accepted_events\": " << report.accepted_events << ",\n"
         << "  \"suppressed_unsafe_events\": " << report.suppressed_unsafe_events
         << ",\n"
         << "  \"order_count\": " << report.order_count << ",\n"
         << "  \"fill_count\": " << report.fill_count << ",\n"
         << "  \"final_book_hash\": " << report.final_book_hash << ",\n"
         << "  \"source_event_chain_hash\": " << report.source_event_chain_hash << ",\n"
         << "  \"simulation_assumptions\": {\n"
         << "    \"acknowledgement_latency_kind\": "
         << static_cast<unsigned>(report.acknowledgement_latency.kind) << ",\n"
         << "    \"acknowledgement_latency_minimum_ns\": "
         << report.acknowledgement_latency.minimum_ns << ",\n"
         << "    \"acknowledgement_latency_maximum_ns\": "
         << report.acknowledgement_latency.maximum_ns << ",\n"
         << "    \"hidden_liquidity_probability_ppm\": "
         << report.hidden_liquidity_probability_ppm << ",\n"
         << "    \"maximum_hidden_liquidity_units\": "
         << report.maximum_hidden_liquidity_units << ",\n"
         << "    \"maker_fee_per_unit_currency_nanos\": "
         << report.maker_fee_per_unit_currency_nanos << ",\n"
         << "    \"taker_fee_per_unit_currency_nanos\": "
         << report.taker_fee_per_unit_currency_nanos << ",\n"
         << "    \"slippage_ticks\": " << report.slippage_ticks << ",\n"
         << "    \"impact_ticks_per_million_units\": "
         << report.impact_ticks_per_million_units << ",\n"
         << "    \"worst_queue_model_quality\": \""
         << (report.worst_queue_model_quality == QueueModelQuality::order_level
                 ? "order_level"
                 : "aggregated_price_level")
         << "\"\n  },\n"
         << "  \"deterministic_result_sha256\": \""
         << digest_hex(report.deterministic_result_sha256) << "\",\n"
         << "  \"uses_bar_close_fills\": "
         << (report.uses_bar_close_fills ? "true" : "false") << ",\n"
         << "  \"raw_accuracy_only_claims_permitted\": "
         << (report.raw_accuracy_only_claims_permitted ? "true" : "false") << ",\n"
         << "  \"live_trading_capable\": "
         << (report.live_trading_capable ? "true" : "false") << ",\n"
         << "  \"warnings\": [\n"
         << "    \"forecast accuracy alone is not evidence of profitability\",\n"
         << "    \"economic interpretation requires realistic costs, execution "
            "metrics, uncertainty, and out-of-sample validation\",\n"
         << "    \"simulated hidden liquidity and market impact are assumptions, not "
            "observations\"\n"
         << "  ],\n"
         << "  \"strategies\": [\n";
  for (std::size_t index = 0U; index < report.strategies.size(); ++index) {
    const auto& strategy = report.strategies[index];
    const auto identifier = common::to_hex(strategy.strategy_id);
    output
        << "    {\n"
        << "      \"strategy_id\": \"" << identifier.data() << "\",\n"
        << "      \"forecast\": {\"resolved_samples\": "
        << strategy.forecast.resolved_samples
        << ", \"unresolved_samples\": " << strategy.forecast.unresolved_samples
        << ", \"log_loss\": " << strategy.forecast.log_loss
        << ", \"brier_score\": " << strategy.forecast.brier_score
        << ", \"directional_precision\": " << strategy.forecast.directional_precision
        << ", \"expected_calibration_error\": "
        << strategy.forecast.expected_calibration_error
        << ", \"p10_p90_coverage\": " << strategy.forecast.p10_p90_coverage << "},\n"
        << "      \"execution\": {\"orders\": " << strategy.execution.orders
        << ", \"rejected_orders\": " << strategy.execution.rejected_orders
        << ", \"expired_orders\": " << strategy.execution.expired_orders
        << ", \"shadow_orders\": " << strategy.execution.shadow_orders
        << ", \"requested_quantity_units\": "
        << strategy.execution.requested_quantity_units
        << ", \"filled_quantity_units\": " << strategy.execution.filled_quantity_units
        << ", \"fill_ratio\": " << strategy.execution.fill_ratio
        << ", \"mean_time_to_fill_ns\": " << strategy.execution.mean_time_to_fill_ns
        << ", \"effective_spread_ticks\": " << strategy.execution.effective_spread_ticks
        << ", \"realized_spread_ticks\": " << strategy.execution.realized_spread_ticks
        << ", \"slippage_ticks\": " << strategy.execution.slippage_ticks
        << ", \"adverse_selection_ticks\": "
        << strategy.execution.adverse_selection_ticks
        << ", \"implementation_shortfall_currency_nanos\": "
        << strategy.execution.implementation_shortfall_currency_nanos << "},\n"
        << "      \"portfolio\": {\"gross_pnl_currency_nanos\": "
        << strategy.portfolio.gross_pnl_currency_nanos
        << ", \"transaction_cost_currency_nanos\": "
        << strategy.portfolio.transaction_cost_currency_nanos
        << ", \"net_pnl_currency_nanos\": " << strategy.portfolio.net_pnl_currency_nanos
        << ", \"turnover_currency_nanos\": "
        << strategy.portfolio.turnover_currency_nanos
        << ", \"sharpe_event_sample\": " << strategy.portfolio.sharpe_event_sample
        << ", \"sortino_event_sample\": " << strategy.portfolio.sortino_event_sample
        << ", \"maximum_drawdown_currency_nanos\": "
        << strategy.portfolio.maximum_drawdown_currency_nanos
        << ", \"cvar_95_currency_nanos\": " << strategy.portfolio.cvar_95_currency_nanos
        << "},\n"
        << "      \"cost_sensitivity\": [";
    for (std::size_t cost = 0U; cost < strategy.cost_sensitivity.size(); ++cost) {
      if (cost != 0U) {
        output << ", ";
      }
      output << "{\"cost_multiplier_ppm\": "
             << strategy.cost_sensitivity[cost].cost_multiplier_ppm
             << ", \"net_pnl_currency_nanos\": "
             << strategy.cost_sensitivity[cost].net_pnl_currency_nanos << "}";
    }
    output << "],\n      \"event_period_performance\": [";
    for (std::size_t period = 0U; period < strategy.event_periods.size(); ++period) {
      if (period != 0U) {
        output << ", ";
      }
      output << "{\"period\": \""
             << event_period_name(strategy.event_periods[period].period)
             << "\", \"event_count\": " << strategy.event_periods[period].event_count
             << ", \"pnl_change_currency_nanos\": "
             << strategy.event_periods[period].pnl_change_currency_nanos << "}";
    }
    output << "]\n    }";
    if (index + 1U != report.strategies.size()) {
      output << ',';
    }
    output << '\n';
  }
  output << "  ]\n}\n";
  return output.str();
}
// NOLINTEND(modernize-raw-string-literal)

bool write_report(const std::filesystem::path& path,
                  const BacktestReport& report) noexcept {
  try {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    const auto contents = report_json(report);
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    output.flush();
    return output.good();
  } catch (...) {
    return false;
  }
}

std::string_view status_name(const Status status) noexcept {
  switch (status) {
  case Status::ok:
    return "ok";
  case Status::complete:
    return "complete";
  case Status::invalid_configuration:
    return "invalid_configuration";
  case Status::invalid_event:
    return "invalid_event";
  case Status::invalid_sequence:
    return "invalid_sequence";
  case Status::invalid_strategy_action:
    return "invalid_strategy_action";
  case Status::capacity_exhausted:
    return "capacity_exhausted";
  case Status::arithmetic_overflow:
    return "arithmetic_overflow";
  case Status::strategy_failed:
    return "strategy_failed";
  case Status::source_open_failed:
    return "source_open_failed";
  case Status::source_invalid:
    return "source_invalid";
  case Status::report_io_error:
    return "report_io_error";
  }
  return "unknown";
}

std::string_view order_state_name(const OrderState state) noexcept {
  switch (state) {
  case OrderState::pending_ack:
    return "pending_ack";
  case OrderState::working:
    return "working";
  case OrderState::partially_filled:
    return "partially_filled";
  case OrderState::filled:
    return "filled";
  case OrderState::rejected:
    return "rejected";
  case OrderState::canceled:
    return "canceled";
  case OrderState::expired:
    return "expired";
  }
  return "unknown";
}

std::string_view event_period_name(const EventPeriod period) noexcept {
  switch (period) {
  case EventPeriod::normal:
    return "normal";
  case EventPeriod::auction:
    return "auction";
  case EventPeriod::halted:
    return "halted";
  case EventPeriod::reopening:
    return "reopening";
  case EventPeriod::shock:
    return "shock";
  }
  return "unknown";
}

} // namespace aegis::backtesting
