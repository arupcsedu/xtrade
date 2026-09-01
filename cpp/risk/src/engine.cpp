#include "aegis/risk/engine.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace aegis::risk {
namespace {

constexpr std::size_t kInvalidIndex = std::numeric_limits<std::size_t>::max();
constexpr std::uint32_t kGateAttempts = 4'096U;

[[nodiscard]] bool checked_add(const std::uint64_t left, const std::uint64_t right,
                               std::uint64_t& result) noexcept {
  if (left > std::numeric_limits<std::uint64_t>::max() - right) {
    return false;
  }
  result = left + right;
  return true;
}

[[nodiscard]] bool checked_multiply(const std::uint64_t left, const std::uint64_t right,
                                    std::uint64_t& result) noexcept {
  if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left) {
    return false;
  }
  result = left * right;
  return true;
}

[[nodiscard]] bool checked_add_signed(const std::int64_t left, const std::int64_t right,
                                      std::int64_t& result) noexcept {
  if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
      (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right)) {
    return false;
  }
  result = left + right;
  return true;
}

[[nodiscard]] bool checked_subtract_signed(const std::int64_t left,
                                           const std::int64_t right,
                                           std::int64_t& result) noexcept {
  if ((right > 0 && left < std::numeric_limits<std::int64_t>::min() + right) ||
      (right < 0 && left > std::numeric_limits<std::int64_t>::max() + right)) {
    return false;
  }
  result = left - right;
  return true;
}

[[nodiscard]] bool checked_multiply_signed(const std::int64_t left,
                                           const std::int64_t right,
                                           std::int64_t& result) noexcept {
  if (left == 0 || right == 0) {
    result = 0;
    return true;
  }
  if ((left == -1 && right == std::numeric_limits<std::int64_t>::min()) ||
      (right == -1 && left == std::numeric_limits<std::int64_t>::min())) {
    return false;
  }
  if (left > 0) {
    if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() / right) ||
        (right < 0 && right < std::numeric_limits<std::int64_t>::min() / left)) {
      return false;
    }
  } else if ((right > 0 && left < std::numeric_limits<std::int64_t>::min() / right) ||
             (right < 0 && left < std::numeric_limits<std::int64_t>::max() / right)) {
    return false;
  }
  result = left * right;
  return true;
}

[[nodiscard]] std::uint64_t magnitude(const std::int64_t value) noexcept {
  return value >= 0 ? static_cast<std::uint64_t>(value)
                    : static_cast<std::uint64_t>(-(value + 1)) + 1U;
}

[[nodiscard]] bool stale(const std::uint64_t observed, const std::uint64_t now,
                         const std::uint64_t maximum_age) noexcept {
  return observed == 0U || now < observed || now - observed > maximum_age;
}

[[nodiscard]] bool unsafe_market_state(const market_state::MarketState state) noexcept {
  return state == market_state::MarketState::startup ||
         state == market_state::MarketState::data_degraded ||
         state == market_state::MarketState::halted ||
         state == market_state::MarketState::reopening ||
         state == market_state::MarketState::recovery ||
         state == market_state::MarketState::shutdown;
}

struct ExposureProjection {
  std::uint64_t gross{};
  std::int64_t net_low{};
  std::int64_t net_high{};
  std::array<std::int64_t, kMaximumRiskSectors> sector_low{};
  std::array<std::int64_t, kMaximumRiskSectors> sector_high{};
  std::array<std::int64_t, kMaximumRiskFactors> factor_low{};
  std::array<std::int64_t, kMaximumRiskFactors> factor_high{};
  std::uint64_t requested_symbol_worst_position{};
  bool requested_symbol_may_be_short{false};
  bool stale_state{false};
  bool arithmetic_failure{false};
};

[[nodiscard]] std::int64_t divide_floor(const std::int64_t value,
                                        const std::int64_t denominator) noexcept {
  const auto quotient = value / denominator;
  return value % denominator < 0 ? quotient - 1 : quotient;
}

[[nodiscard]] std::int64_t divide_ceiling(const std::int64_t value,
                                          const std::int64_t denominator) noexcept {
  const auto quotient = value / denominator;
  return value % denominator > 0 ? quotient + 1 : quotient;
}

} // namespace

DeterministicPreTradeRiskEngine::DeterministicPreTradeRiskEngine(
    RiskLimitSnapshot limits, RiskDecisionJournal& journal) noexcept
    : journal_(journal), limits_(limits) {
  const auto valid = valid_limit_snapshot(limits_);
  initialized_.store(valid, std::memory_order_release);
  state_available_.store(valid, std::memory_order_release);
  authority_epoch_.store(valid ? limits_.authority_epoch : 0U,
                         std::memory_order_release);
  active_limit_revision_.store(valid ? limits_.revision : 0U,
                               std::memory_order_release);
}

bool DeterministicPreTradeRiskEngine::initialized() const noexcept {
  return initialized_.load(std::memory_order_acquire);
}

bool DeterministicPreTradeRiskEngine::try_enter() noexcept {
  for (std::uint32_t attempt = 0U; attempt < kGateAttempts; ++attempt) {
    if (!evaluation_gate_.test_and_set(std::memory_order_acquire)) {
      return true;
    }
  }
  return false;
}

void DeterministicPreTradeRiskEngine::leave() noexcept {
  evaluation_gate_.clear(std::memory_order_release);
}

std::size_t DeterministicPreTradeRiskEngine::find_symbol(
    const common::InstrumentId instrument_id) const noexcept {
  for (std::size_t index = 0U; index < limits_.symbol_count; ++index) {
    if (limits_.symbols[index].instrument_id == instrument_id) {
      return index;
    }
  }
  return kInvalidIndex;
}

std::size_t DeterministicPreTradeRiskEngine::find_strategy(
    const common::StrategyId strategy_id) const noexcept {
  for (std::size_t index = 0U; index < limits_.strategy_count; ++index) {
    if (limits_.strategies[index].strategy_id == strategy_id) {
      return index;
    }
  }
  return kInvalidIndex;
}

std::size_t DeterministicPreTradeRiskEngine::find_venue(
    const common::VenueId venue_id) const noexcept {
  for (std::size_t index = 0U; index < limits_.venue_count; ++index) {
    if (limits_.venues[index].venue_id == venue_id) {
      return index;
    }
  }
  return kInvalidIndex;
}

bool DeterministicPreTradeRiskEngine::applicable_kill(
    const KillScopeIndices indices) const noexcept {
  const auto symbol_bit = std::uint64_t{1U} << indices.symbol;
  const auto strategy_bit = std::uint64_t{1U} << indices.strategy;
  const auto venue_bit = std::uint64_t{1U} << indices.venue;
  return firm_kill_.load(std::memory_order_acquire) != 0U ||
         account_kill_.load(std::memory_order_acquire) != 0U ||
         (symbol_kill_mask_.load(std::memory_order_acquire) & symbol_bit) != 0U ||
         (strategy_kill_mask_.load(std::memory_order_acquire) & strategy_bit) != 0U ||
         (venue_kill_mask_.load(std::memory_order_acquire) & venue_bit) != 0U;
}

bool DeterministicPreTradeRiskEngine::record_intent(const common::IntentId intent_id,
                                                    bool& duplicate) noexcept {
  duplicate = false;
  const auto start =
      static_cast<std::size_t>((intent_id.high() ^ std::rotl(intent_id.low(), 23)) &
                               (kDuplicateIntentCapacity - 1U));
  for (std::size_t probe = 0U; probe < intent_slots_.size(); ++probe) {
    auto& slot = intent_slots_[(start + probe) & (kDuplicateIntentCapacity - 1U)];
    if (slot.occupied) {
      if (slot.intent_id == intent_id) {
        duplicate = true;
        return true;
      }
      continue;
    }
    slot.intent_id = intent_id;
    slot.occupied = true;
    return true;
  }
  return false;
}

bool DeterministicPreTradeRiskEngine::admit_rate(
    RateWindow& window, const RateAdmission admission) noexcept {
  if (window.write_sequence == std::numeric_limits<std::uint64_t>::max()) {
    return false;
  }
  while (window.read_sequence < window.write_sequence) {
    const auto oldest_index =
        static_cast<std::size_t>(window.read_sequence & (kMaximumRateEvents - 1U));
    const auto oldest = window.timestamps[oldest_index];
    if (admission.now_ns < oldest) {
      return false;
    }
    if (admission.now_ns - oldest < admission.duration_ns) {
      break;
    }
    ++window.read_sequence;
  }
  if (window.write_sequence - window.read_sequence >= admission.maximum_events) {
    return false;
  }
  const auto index =
      static_cast<std::size_t>(window.write_sequence & (kMaximumRateEvents - 1U));
  window.timestamps[index] = admission.now_ns;
  ++window.write_sequence;
  return true;
}

RiskDecision DeterministicPreTradeRiskEngine::base_decision(
    const RiskEvaluationRequest& request, const std::uint64_t evaluation_ordinal,
    const std::uint64_t journal_sequence) const noexcept {
  const auto now = request.context.now_process_monotonic_time_ns;
  return {.global_event_id = request.global_event_id,
          .intent_id = request.intent.intent_id,
          .session_id = request.intent.session_id,
          .account_id = request.intent.account_id,
          .strategy_id = request.intent.strategy_id,
          .venue_id = request.intent.venue_id,
          .instrument_id = request.intent.instrument_id,
          .risk_snapshot_id = limits_.risk_snapshot_id,
          .configuration_version = limits_.configuration_version,
          .decision = DecisionCode::rejected,
          .reason = RiskReason::risk_state_unavailable,
          .failed_check = RiskCheck::none,
          .intent_action = request.intent.action,
          .trading_mode = request.context.trading_mode,
          .approved_quantity_units = 0U,
          .approved_price_ticks = 0,
          .evaluated_process_monotonic_time_ns = now,
          .valid_until_process_monotonic_time_ns = now + 1U,
          .intent_hash = request.intent.stable_hash,
          .intent_sha256 = request.intent.canonical_sha256,
          .risk_snapshot_hash = limits_.stable_hash,
          .risk_context_hash = request.context.stable_hash,
          .position_generation = position_generation_.load(std::memory_order_acquire),
          .authority_epoch = authority_epoch_.load(std::memory_order_acquire),
          .policy_revision = limits_.revision,
          .evaluation_ordinal = evaluation_ordinal,
          .journal_sequence = journal_sequence};
}

EvaluationResult DeterministicPreTradeRiskEngine::evaluate(
    const RiskEvaluationRequest& request) noexcept {
  if (!initialized()) {
    return {.status = EvaluationStatus::not_initialized, .decision = {}};
  }
  if (!try_enter()) {
    busy_count_.fetch_add(1U, std::memory_order_relaxed);
    return {.status = EvaluationStatus::engine_busy, .decision = {}};
  }
  const auto reservation = journal_.try_reserve();
  if (!reservation.valid) {
    journal_full_count_.fetch_add(1U, std::memory_order_relaxed);
    leave();
    return {.status = EvaluationStatus::journal_unavailable, .decision = {}};
  }
  auto result = evaluate_locked(request, reservation);
  leave();
  return result;
}

// The explicit sequence of checks is intentionally linear and mirrors RiskCheck.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
EvaluationResult DeterministicPreTradeRiskEngine::evaluate_locked(
    const RiskEvaluationRequest& request,
    const RiskDecisionJournal::Reservation reservation) noexcept {
  const auto ordinal = evaluation_count_.fetch_add(1U, std::memory_order_relaxed) + 1U;
  auto decision = base_decision(request, ordinal, reservation.sequence);
  const auto reject = [&](const RiskReason reason,
                          const RiskCheck check) -> EvaluationResult {
    decision.decision = DecisionCode::rejected;
    decision.reason = reason;
    decision.failed_check = check;
    decision.approved_quantity_units = 0U;
    decision.approved_price_ticks = 0;
    decision.reason_mask |= reason_bit(reason);
    decision.position_generation = position_generation_.load(std::memory_order_acquire);
    decision.stable_hash = stable_risk_decision_hash(decision);
    journal_.commit(reservation, decision);
    rejection_count_.fetch_add(1U, std::memory_order_relaxed);
    return {.status = EvaluationStatus::journaled, .decision = decision};
  };
  const auto pass = [&decision](const RiskCheck check) {
    decision.completed_check_mask |= check_bit(check);
  };

  if (!request.global_event_id.valid() || !valid_risk_intent(request.intent) ||
      !valid_risk_context(request.context)) {
    return reject(RiskReason::invalid_intent, RiskCheck::none);
  }
  const auto now = request.context.now_process_monotonic_time_ns;
  if (now < request.intent.created_process_monotonic_time_ns ||
      now > request.intent.expire_process_monotonic_time_ns) {
    return reject(RiskReason::expired_intent, RiskCheck::none);
  }
  if (now < last_evaluation_time_ns_) {
    return reject(RiskReason::risk_state_unavailable, RiskCheck::none);
  }
  last_evaluation_time_ns_ = now;

  // 1. Live is deliberately unauthorized in this repository build.
  const auto mode_authorized =
      (request.context.trading_mode == TradingMode::simulation &&
       limits_.allow_simulation) ||
      (request.context.trading_mode == TradingMode::paper && limits_.allow_paper);
  if (!mode_authorized) {
    return reject(RiskReason::trading_mode_unauthorized,
                  RiskCheck::trading_mode_authorization);
  }
  pass(RiskCheck::trading_mode_authorization);

  // 2.
  if (!request.context.operator_authorized || !request.context.session_authorized ||
      request.context.operator_authorization_valid_until_ns <= now ||
      request.intent.session_id != limits_.session_id ||
      request.intent.account_id != limits_.account_id) {
    return reject(RiskReason::operator_session_unauthorized,
                  RiskCheck::operator_session_authorization);
  }
  pass(RiskCheck::operator_session_authorization);

  const auto strategy_index = find_strategy(request.intent.strategy_id);
  // 3.
  if (strategy_index == kInvalidIndex ||
      !limits_.strategies[strategy_index].authorized) {
    return reject(RiskReason::strategy_unauthorized, RiskCheck::strategy_authorization);
  }
  pass(RiskCheck::strategy_authorization);

  const auto symbol_index = find_symbol(request.intent.instrument_id);
  // 4.
  if (symbol_index == kInvalidIndex || !limits_.symbols[symbol_index].authorized) {
    return reject(RiskReason::symbol_unauthorized, RiskCheck::symbol_authorization);
  }
  pass(RiskCheck::symbol_authorization);
  const auto& symbol_limit = limits_.symbols[symbol_index];

  // 5.
  if (symbol_limit.restricted) {
    return reject(RiskReason::restricted_instrument, RiskCheck::restricted_list);
  }
  pass(RiskCheck::restricted_list);

  // 6.
  const auto market_index =
      static_cast<std::size_t>(request.context.market_state_snapshot.state);
  if (unsafe_market_state(request.context.market_state_snapshot.state) ||
      stale(request.context.market_state_snapshot.observed_process_monotonic_time_ns,
            now, limits_.maximum_market_state_age_ns) ||
      (limits_.allowed_market_state_mask & (std::uint16_t{1U} << market_index)) == 0U) {
    return reject(RiskReason::market_state_unsafe, RiskCheck::market_state);
  }
  pass(RiskCheck::market_state);

  // 7.
  if (request.context.official_trading_status !=
      market_state::OfficialTradingStatus::open) {
    return reject(RiskReason::trading_halted, RiskCheck::halt);
  }
  pass(RiskCheck::halt);

  // 8.
  if (request.context.clock_quality_snapshot.state !=
          time::ClockQualityState::healthy ||
      request.context.clock_quality_snapshot.operation_mode !=
          time::ClockOperationMode::normal ||
      stale(request.context.clock_quality_snapshot.observed_at.value(), now,
            limits_.maximum_clock_state_age_ns)) {
    return reject(RiskReason::clock_unhealthy, RiskCheck::clock_health);
  }
  pass(RiskCheck::clock_health);

  // 9.
  if (request.context.feed_health != market_state::FeedHealth::healthy ||
      request.context.book_validity != market_state::BookValidity::valid ||
      stale(request.context.feed_book_observed_process_monotonic_time_ns, now,
            limits_.maximum_feed_book_age_ns)) {
    return reject(RiskReason::feed_book_unhealthy, RiskCheck::feed_book_health);
  }
  pass(RiskCheck::feed_book_health);

  const auto is_cancel = request.intent.action == IntentAction::cancel;
  // 10.
  if (!is_cancel &&
      request.intent.quantity_units > symbol_limit.maximum_order_quantity_units) {
    return reject(RiskReason::order_quantity_exceeded,
                  RiskCheck::maximum_order_quantity);
  }
  pass(RiskCheck::maximum_order_quantity);

  std::uint64_t order_notional{};
  if (!is_cancel) {
    std::uint64_t price_quantity{};
    if (!checked_multiply(static_cast<std::uint64_t>(request.intent.limit_price_ticks),
                          request.intent.quantity_units, price_quantity) ||
        !checked_multiply(price_quantity, symbol_limit.tick_value_currency_nanos,
                          order_notional)) {
      return reject(RiskReason::arithmetic_overflow, RiskCheck::maximum_order_notional);
    }
  }
  // 11.
  if (order_notional > symbol_limit.maximum_order_notional_currency_nanos) {
    return reject(RiskReason::order_notional_exceeded,
                  RiskCheck::maximum_order_notional);
  }
  pass(RiskCheck::maximum_order_notional);

  // 12.
  if (!is_cancel) {
    const auto price = static_cast<std::uint64_t>(request.intent.limit_price_ticks);
    const auto reference =
        static_cast<std::uint64_t>(request.context.reference_price_ticks);
    const auto deviation = price > reference ? price - reference : reference - price;
    if (deviation > symbol_limit.maximum_price_deviation_ticks) {
      return reject(RiskReason::price_collar_exceeded, RiskCheck::price_collar);
    }
  }
  pass(RiskCheck::price_collar);

  // 13.
  if (!is_cancel && static_cast<std::uint64_t>(request.intent.limit_price_ticks) %
                            symbol_limit.price_increment_ticks !=
                        0U) {
    return reject(RiskReason::invalid_tick_size, RiskCheck::tick_size);
  }
  pass(RiskCheck::tick_size);

  // 14.
  bool duplicate{};
  if (!record_intent(request.intent.intent_id, duplicate)) {
    return reject(RiskReason::risk_state_unavailable, RiskCheck::duplicate_intent);
  }
  if (duplicate) {
    return reject(RiskReason::duplicate_intent, RiskCheck::duplicate_intent);
  }
  pass(RiskCheck::duplicate_intent);

  // 15.
  if (!is_cancel &&
      !admit_rate(order_rate_, {.now_ns = now,
                                .duration_ns = limits_.order_rate_window_ns,
                                .maximum_events = limits_.maximum_orders_per_window})) {
    return reject(RiskReason::order_rate_exceeded, RiskCheck::order_rate);
  }
  pass(RiskCheck::order_rate);

  // 16.
  if (is_cancel &&
      !admit_rate(cancel_rate_,
                  {.now_ns = now,
                   .duration_ns = limits_.cancel_rate_window_ns,
                   .maximum_events = limits_.maximum_cancels_per_window})) {
    return reject(RiskReason::cancel_rate_exceeded, RiskCheck::cancel_rate);
  }
  pass(RiskCheck::cancel_rate);

  ExposureProjection exposure{};
  for (std::size_t index = 0U; index < limits_.symbol_count; ++index) {
    const auto& state = symbol_state_[index];
    const auto as_of =
        state.as_of_process_monotonic_time_ns.load(std::memory_order_relaxed);
    const auto mark = state.mark_price_ticks.load(std::memory_order_relaxed);
    if (!state.available.load(std::memory_order_relaxed) || mark <= 0 ||
        stale(as_of, now, limits_.maximum_position_age_ns)) {
      exposure.stale_state = true;
      break;
    }
    const auto position = state.net_quantity_units.load(std::memory_order_relaxed);
    auto pending_buy = state.pending_buy_quantity_units.load(std::memory_order_relaxed);
    auto pending_sell =
        state.pending_sell_quantity_units.load(std::memory_order_relaxed);
    if (pending_buy >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        pending_sell >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      exposure.arithmetic_failure = true;
      break;
    }
    if (index == symbol_index && !is_cancel) {
      auto& requested_pending =
          request.intent.action == IntentAction::buy ? pending_buy : pending_sell;
      if (!checked_add(requested_pending, request.intent.quantity_units,
                       requested_pending) ||
          requested_pending >
              static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        exposure.arithmetic_failure = true;
        break;
      }
    }
    std::int64_t low_position{};
    std::int64_t high_position{};
    if (!checked_subtract_signed(position, static_cast<std::int64_t>(pending_sell),
                                 low_position) ||
        !checked_add_signed(position, static_cast<std::int64_t>(pending_buy),
                            high_position)) {
      exposure.arithmetic_failure = true;
      break;
    }
    const auto worst_position =
        std::max(magnitude(low_position), magnitude(high_position));
    if (index == symbol_index) {
      exposure.requested_symbol_worst_position = worst_position;
      exposure.requested_symbol_may_be_short = low_position < 0;
    }
    const auto endpoint_notional = [&](const std::int64_t endpoint,
                                       std::int64_t& result) {
      std::uint64_t marked_units{};
      std::uint64_t marked_notional{};
      if (!checked_multiply(magnitude(endpoint), static_cast<std::uint64_t>(mark),
                            marked_units) ||
          !checked_multiply(marked_units,
                            limits_.symbols[index].tick_value_currency_nanos,
                            marked_notional) ||
          marked_notional > kMaximumMonetaryNanos) {
        return false;
      }
      result = endpoint < 0 ? -static_cast<std::int64_t>(marked_notional)
                            : static_cast<std::int64_t>(marked_notional);
      return true;
    };
    std::int64_t low_notional{};
    std::int64_t high_notional{};
    std::uint64_t worst_marked_units{};
    std::uint64_t worst_marked_notional{};
    if (!endpoint_notional(low_position, low_notional) ||
        !endpoint_notional(high_position, high_notional) ||
        !checked_multiply(worst_position, static_cast<std::uint64_t>(mark),
                          worst_marked_units) ||
        !checked_multiply(worst_marked_units,
                          limits_.symbols[index].tick_value_currency_nanos,
                          worst_marked_notional) ||
        !checked_add(exposure.gross, worst_marked_notional, exposure.gross) ||
        !checked_add_signed(exposure.net_low, low_notional, exposure.net_low) ||
        !checked_add_signed(exposure.net_high, high_notional, exposure.net_high)) {
      exposure.arithmetic_failure = true;
      break;
    }
    const auto sector = limits_.symbols[index].sector_index;
    if (!checked_add_signed(exposure.sector_low[sector], low_notional,
                            exposure.sector_low[sector]) ||
        !checked_add_signed(exposure.sector_high[sector], high_notional,
                            exposure.sector_high[sector])) {
      exposure.arithmetic_failure = true;
      break;
    }
    for (std::size_t factor = 0U; factor < limits_.factor_count; ++factor) {
      std::int64_t low_product{};
      std::int64_t high_product{};
      const auto beta =
          static_cast<std::int64_t>(limits_.symbols[index].factor_beta_ppm[factor]);
      if (!checked_multiply_signed(low_notional, beta, low_product) ||
          !checked_multiply_signed(high_notional, beta, high_product)) {
        exposure.arithmetic_failure = true;
        break;
      }
      const auto product_low = std::min(low_product, high_product);
      const auto product_high = std::max(low_product, high_product);
      const auto factor_low =
          divide_floor(product_low, static_cast<std::int64_t>(kPartsPerMillion));
      const auto factor_high =
          divide_ceiling(product_high, static_cast<std::int64_t>(kPartsPerMillion));
      if (!checked_add_signed(exposure.factor_low[factor], factor_low,
                              exposure.factor_low[factor]) ||
          !checked_add_signed(exposure.factor_high[factor], factor_high,
                              exposure.factor_high[factor])) {
        exposure.arithmetic_failure = true;
        break;
      }
    }
    if (exposure.arithmetic_failure) {
      break;
    }
  }
  if (exposure.arithmetic_failure) {
    return reject(RiskReason::arithmetic_overflow, RiskCheck::symbol_position);
  }
  if (exposure.stale_state) {
    return reject(RiskReason::stale_position, RiskCheck::symbol_position);
  }

  // 17.
  if (!is_cancel && exposure.requested_symbol_worst_position >
                        symbol_limit.maximum_absolute_position_units) {
    return reject(RiskReason::symbol_position_exceeded, RiskCheck::symbol_position);
  }
  pass(RiskCheck::symbol_position);

  // 18.
  if (exposure.gross > limits_.maximum_gross_exposure_currency_nanos) {
    return reject(RiskReason::gross_exposure_exceeded, RiskCheck::gross_exposure);
  }
  pass(RiskCheck::gross_exposure);

  // 19.
  if (std::max(magnitude(exposure.net_low), magnitude(exposure.net_high)) >
      limits_.maximum_absolute_net_exposure_currency_nanos) {
    return reject(RiskReason::net_exposure_exceeded, RiskCheck::net_exposure);
  }
  pass(RiskCheck::net_exposure);

  // 20.
  for (std::size_t sector = 0U; sector < limits_.sector_count; ++sector) {
    if (std::max(magnitude(exposure.sector_low[sector]),
                 magnitude(exposure.sector_high[sector])) >
        limits_.maximum_sector_exposure_currency_nanos[sector]) {
      return reject(RiskReason::sector_concentration_exceeded,
                    RiskCheck::sector_concentration);
    }
  }
  pass(RiskCheck::sector_concentration);

  // 21.
  for (std::size_t factor = 0U; factor < limits_.factor_count; ++factor) {
    if (std::max(magnitude(exposure.factor_low[factor]),
                 magnitude(exposure.factor_high[factor])) >
        limits_.maximum_factor_exposure_currency_nanos[factor]) {
      return reject(RiskReason::factor_exposure_exceeded, RiskCheck::factor_exposure);
    }
  }
  pass(RiskCheck::factor_exposure);

  const auto pnl_as_of =
      pnl_as_of_process_monotonic_time_ns_.load(std::memory_order_relaxed);
  const auto firm_pnl = firm_daily_pnl_currency_nanos_.load(std::memory_order_relaxed);
  const auto firm_peak = firm_peak_pnl_currency_nanos_.load(std::memory_order_relaxed);
  const auto strategy_pnl = strategy_state_[strategy_index].pnl_currency_nanos.load(
      std::memory_order_relaxed);
  const auto strategy_peak =
      strategy_state_[strategy_index].peak_pnl_currency_nanos.load(
          std::memory_order_relaxed);
  if (stale(pnl_as_of, now, limits_.maximum_position_age_ns) ||
      stale(strategy_state_[strategy_index].as_of_process_monotonic_time_ns.load(
                std::memory_order_relaxed),
            now, limits_.maximum_position_age_ns)) {
    return reject(RiskReason::risk_state_unavailable, RiskCheck::daily_loss);
  }

  // 22.
  if (firm_pnl < 0 && magnitude(firm_pnl) > limits_.maximum_daily_loss_currency_nanos) {
    return reject(RiskReason::daily_loss_exceeded, RiskCheck::daily_loss);
  }
  pass(RiskCheck::daily_loss);

  // 23.
  if (strategy_pnl < 0 &&
      magnitude(strategy_pnl) >
          limits_.strategies[strategy_index].maximum_loss_currency_nanos) {
    return reject(RiskReason::strategy_loss_exceeded, RiskCheck::strategy_loss);
  }
  pass(RiskCheck::strategy_loss);

  // 24.
  std::int64_t firm_drawdown{};
  std::int64_t strategy_drawdown{};
  if ((firm_peak > firm_pnl &&
       !checked_subtract_signed(firm_peak, firm_pnl, firm_drawdown)) ||
      (strategy_peak > strategy_pnl &&
       !checked_subtract_signed(strategy_peak, strategy_pnl, strategy_drawdown))) {
    return reject(RiskReason::arithmetic_overflow, RiskCheck::drawdown);
  }
  if (magnitude(firm_drawdown) > limits_.maximum_drawdown_currency_nanos ||
      magnitude(strategy_drawdown) >
          limits_.strategies[strategy_index].maximum_drawdown_currency_nanos) {
    return reject(RiskReason::drawdown_exceeded, RiskCheck::drawdown);
  }
  pass(RiskCheck::drawdown);

  // 25.
  if (exposure.gross > limits_.credit_capital_limit_currency_nanos) {
    return reject(RiskReason::credit_capital_exceeded, RiskCheck::credit_capital);
  }
  pass(RiskCheck::credit_capital);

  // 26.
  if (!is_cancel && request.intent.action == IntentAction::sell &&
      exposure.requested_symbol_may_be_short && limits_.require_locate_for_short_sale &&
      request.context.short_sale_locate != PolicyHookResult::allowed) {
    return reject(RiskReason::short_sale_locate_denied, RiskCheck::short_sale_locate);
  }
  pass(RiskCheck::short_sale_locate);

  // 27.
  if (!is_cancel && limits_.require_self_trade_prevention &&
      request.context.self_trade_prevention != PolicyHookResult::allowed) {
    return reject(RiskReason::self_trade_prevention_denied,
                  RiskCheck::self_trade_prevention);
  }
  pass(RiskCheck::self_trade_prevention);

  const auto venue_index = find_venue(request.intent.venue_id);
  // 28.
  if (venue_index == kInvalidIndex || !limits_.venues[venue_index].authorized) {
    return reject(RiskReason::venue_unauthorized, RiskCheck::venue_authorization);
  }
  pass(RiskCheck::venue_authorization);

  // 29.
  const KillScopeIndices kill_indices{
      .symbol = symbol_index, .strategy = strategy_index, .venue = venue_index};
  if (applicable_kill(kill_indices)) {
    return reject(RiskReason::kill_switch_engaged, RiskCheck::kill_switch);
  }
  pass(RiskCheck::kill_switch);

  // 30.
  const auto current_epoch = authority_epoch_.load(std::memory_order_acquire);
  if (request.context.authority_epoch != current_epoch ||
      limits_.authority_epoch != current_epoch) {
    return reject(RiskReason::split_brain_epoch, RiskCheck::configuration_freshness);
  }
  if (!state_available_.load(std::memory_order_acquire)) {
    return reject(RiskReason::risk_state_unavailable,
                  RiskCheck::configuration_freshness);
  }
  if (request.intent.configuration_version != limits_.configuration_version ||
      now < limits_.published_process_monotonic_time_ns ||
      now > limits_.valid_until_process_monotonic_time_ns ||
      now - limits_.published_process_monotonic_time_ns >
          limits_.maximum_configuration_age_ns) {
    return reject(RiskReason::configuration_stale, RiskCheck::configuration_freshness);
  }
  pass(RiskCheck::configuration_freshness);

  // Final safety fence: kill and authority state are re-read before publication.
  if (applicable_kill(kill_indices)) {
    decision.completed_check_mask &= ~check_bit(RiskCheck::kill_switch);
    return reject(RiskReason::kill_switch_engaged, RiskCheck::kill_switch);
  }
  if (authority_epoch_.load(std::memory_order_acquire) != current_epoch) {
    decision.completed_check_mask &= ~check_bit(RiskCheck::configuration_freshness);
    return reject(RiskReason::split_brain_epoch, RiskCheck::configuration_freshness);
  }

  std::uint64_t ttl_end{};
  if (!checked_add(now, limits_.approval_ttl_ns, ttl_end)) {
    decision.completed_check_mask &= ~check_bit(RiskCheck::configuration_freshness);
    return reject(RiskReason::arithmetic_overflow, RiskCheck::configuration_freshness);
  }
  decision.valid_until_process_monotonic_time_ns =
      std::min({ttl_end, request.intent.expire_process_monotonic_time_ns,
                limits_.valid_until_process_monotonic_time_ns,
                request.context.operator_authorization_valid_until_ns});
  if (decision.valid_until_process_monotonic_time_ns <= now) {
    return reject(RiskReason::configuration_stale, RiskCheck::configuration_freshness);
  }

  if (!is_cancel) {
    auto& pending = request.intent.action == IntentAction::buy
                        ? symbol_state_[symbol_index].pending_buy_quantity_units
                        : symbol_state_[symbol_index].pending_sell_quantity_units;
    const auto previous = pending.load(std::memory_order_relaxed);
    std::uint64_t next{};
    if (!checked_add(previous, request.intent.quantity_units, next)) {
      decision.completed_check_mask &= ~check_bit(RiskCheck::symbol_position);
      return reject(RiskReason::arithmetic_overflow, RiskCheck::symbol_position);
    }
    pending.store(next, std::memory_order_release);
    position_generation_.fetch_add(1U, std::memory_order_acq_rel);
  }

  decision.decision = DecisionCode::approved;
  decision.reason = RiskReason::within_limits;
  decision.failed_check = RiskCheck::none;
  decision.approved_quantity_units = request.intent.quantity_units;
  decision.approved_price_ticks = request.intent.limit_price_ticks;
  decision.reason_mask = reason_bit(RiskReason::within_limits);
  decision.position_generation = position_generation_.load(std::memory_order_acquire);
  decision.stable_hash = stable_risk_decision_hash(decision);
  journal_.commit(reservation, decision);
  approval_count_.fetch_add(1U, std::memory_order_relaxed);
  return {.status = EvaluationStatus::journaled, .decision = decision};
}

InstallStatus
DeterministicPreTradeRiskEngine::install_limits(RiskLimitSnapshot limits) noexcept {
  if (!valid_limit_snapshot(limits)) {
    return InstallStatus::invalid;
  }
  if (!try_enter()) {
    return InstallStatus::busy;
  }
  if (limits.revision < limits_.revision ||
      limits.authority_epoch < authority_epoch_.load(std::memory_order_acquire)) {
    leave();
    return InstallStatus::rollback_rejected;
  }
  if (limits.revision == limits_.revision) {
    const auto result = limits.stable_hash == limits_.stable_hash
                            ? InstallStatus::unchanged
                            : InstallStatus::revision_conflict;
    leave();
    return result;
  }
  limits_ = limits;
  authority_epoch_.store(limits_.authority_epoch, std::memory_order_release);
  active_limit_revision_.store(limits_.revision, std::memory_order_release);
  state_available_.store(false, std::memory_order_release);
  firm_kill_.store(1U, std::memory_order_release);
  kill_generation_.fetch_add(1U, std::memory_order_acq_rel);
  for (auto& state : symbol_state_) {
    state.net_quantity_units.store(0, std::memory_order_relaxed);
    state.pending_buy_quantity_units.store(0U, std::memory_order_relaxed);
    state.pending_sell_quantity_units.store(0U, std::memory_order_relaxed);
    state.mark_price_ticks.store(0, std::memory_order_relaxed);
    state.as_of_process_monotonic_time_ns.store(0U, std::memory_order_relaxed);
    state.available.store(false, std::memory_order_relaxed);
  }
  for (auto& state : strategy_state_) {
    state.pnl_currency_nanos.store(0, std::memory_order_relaxed);
    state.peak_pnl_currency_nanos.store(0, std::memory_order_relaxed);
    state.as_of_process_monotonic_time_ns.store(0U, std::memory_order_relaxed);
  }
  for (auto& slot : intent_slots_) {
    slot = {};
  }
  order_rate_ = {};
  cancel_rate_ = {};
  last_evaluation_time_ns_ = 0U;
  last_portfolio_snapshot_sequence_.store(0U, std::memory_order_relaxed);
  last_portfolio_snapshot_hash_.store(0U, std::memory_order_relaxed);
  position_generation_.fetch_add(1U, std::memory_order_acq_rel);
  initialized_.store(true, std::memory_order_release);
  leave();
  return InstallStatus::installed;
}

StateUpdateStatus
DeterministicPreTradeRiskEngine::seed_position(const PositionSeed seed) noexcept {
  if (!initialized() || seed.symbol_index >= kMaximumRiskSymbols ||
      seed.mark_price_ticks <= 0 || seed.as_of_process_monotonic_time_ns == 0U) {
    return StateUpdateStatus::invalid;
  }
  if (!try_enter()) {
    return StateUpdateStatus::busy;
  }
  if (seed.symbol_index >= limits_.symbol_count) {
    leave();
    return StateUpdateStatus::invalid;
  }
  auto& state = symbol_state_[seed.symbol_index];
  state.net_quantity_units.store(seed.net_quantity_units, std::memory_order_relaxed);
  state.pending_buy_quantity_units.store(0U, std::memory_order_relaxed);
  state.pending_sell_quantity_units.store(0U, std::memory_order_relaxed);
  state.mark_price_ticks.store(seed.mark_price_ticks, std::memory_order_relaxed);
  state.as_of_process_monotonic_time_ns.store(seed.as_of_process_monotonic_time_ns,
                                              std::memory_order_relaxed);
  state.available.store(true, std::memory_order_release);
  position_generation_.fetch_add(1U, std::memory_order_acq_rel);
  leave();
  return StateUpdateStatus::applied;
}

StateUpdateStatus DeterministicPreTradeRiskEngine::install_portfolio_snapshot(
    const portfolio::PortfolioSnapshot& snapshot) noexcept {
  if (!portfolio_snapshot_compatible(snapshot)) {
    return StateUpdateStatus::invalid;
  }
  if (!try_enter()) {
    return StateUpdateStatus::busy;
  }
  const auto previous_sequence =
      last_portfolio_snapshot_sequence_.load(std::memory_order_relaxed);
  const auto previous_hash =
      last_portfolio_snapshot_hash_.load(std::memory_order_relaxed);
  if (snapshot.sequence < previous_sequence ||
      (snapshot.sequence == previous_sequence && previous_sequence != 0U &&
       snapshot.stable_hash != previous_hash)) {
    leave();
    return StateUpdateStatus::unavailable;
  }
  if (snapshot.sequence == previous_sequence && snapshot.stable_hash == previous_hash) {
    leave();
    return StateUpdateStatus::applied;
  }

  PortfolioInstrumentIndices instrument_indices{};
  PortfolioStrategyIndices strategy_indices{};
  if (!map_portfolio_snapshot(snapshot, instrument_indices, strategy_indices)) {
    leave();
    return StateUpdateStatus::unavailable;
  }
  install_mapped_portfolio_snapshot(snapshot, instrument_indices, strategy_indices);
  leave();
  return StateUpdateStatus::applied;
}

bool DeterministicPreTradeRiskEngine::portfolio_snapshot_compatible(
    const portfolio::PortfolioSnapshot& snapshot) const noexcept {
  return initialized() && snapshot.schema_major == portfolio::kPortfolioSchemaMajor &&
         snapshot.schema_minor <= portfolio::kPortfolioSchemaMinor && snapshot.ready &&
         snapshot.health == portfolio::PortfolioHealth::healthy &&
         snapshot.stable_hash != 0U &&
         portfolio::stable_snapshot_hash(snapshot) == snapshot.stable_hash &&
         snapshot.session_id == limits_.session_id &&
         snapshot.configuration_version == limits_.configuration_version &&
         snapshot.account_count == 1U &&
         snapshot.accounts[0U].account_id == limits_.account_id &&
         snapshot.as_of_process_monotonic_time_ns != 0U;
}

bool DeterministicPreTradeRiskEngine::map_portfolio_snapshot(
    const portfolio::PortfolioSnapshot& snapshot,
    PortfolioInstrumentIndices& instrument_indices,
    PortfolioStrategyIndices& strategy_indices) const noexcept {
  for (std::size_t symbol = 0U; symbol < limits_.symbol_count; ++symbol) {
    auto found = static_cast<std::size_t>(snapshot.instrument_count);
    for (std::size_t index = 0U; index < snapshot.instrument_count; ++index) {
      if (snapshot.instruments[index].instrument_id ==
          limits_.symbols[symbol].instrument_id) {
        found = index;
        break;
      }
    }
    if (found == snapshot.instrument_count ||
        snapshot.instruments[found].mark_price_ticks <= 0) {
      return false;
    }
    instrument_indices[symbol] = found;
  }
  for (std::size_t strategy = 0U; strategy < limits_.strategy_count; ++strategy) {
    auto found = static_cast<std::size_t>(snapshot.strategy_count);
    for (std::size_t index = 0U; index < snapshot.strategy_count; ++index) {
      if (snapshot.strategies[index].strategy_id ==
          limits_.strategies[strategy].strategy_id) {
        found = index;
        break;
      }
    }
    if (found == snapshot.strategy_count) {
      return false;
    }
    strategy_indices[strategy] = found;
  }
  return true;
}

void DeterministicPreTradeRiskEngine::install_mapped_portfolio_snapshot(
    const portfolio::PortfolioSnapshot& snapshot,
    const PortfolioInstrumentIndices& instrument_indices,
    const PortfolioStrategyIndices& strategy_indices) noexcept {
  for (std::size_t symbol = 0U; symbol < limits_.symbol_count; ++symbol) {
    const auto& source = snapshot.instruments[instrument_indices[symbol]];
    auto& target = symbol_state_[symbol];
    target.net_quantity_units.store(source.net_quantity_units,
                                    std::memory_order_relaxed);
    target.pending_buy_quantity_units.store(source.pending_buy_quantity_units,
                                            std::memory_order_relaxed);
    target.pending_sell_quantity_units.store(source.pending_sell_quantity_units,
                                             std::memory_order_relaxed);
    target.mark_price_ticks.store(source.mark_price_ticks, std::memory_order_relaxed);
    target.as_of_process_monotonic_time_ns.store(
        snapshot.as_of_process_monotonic_time_ns, std::memory_order_relaxed);
    target.available.store(true, std::memory_order_release);
  }
  for (std::size_t strategy = 0U; strategy < limits_.strategy_count; ++strategy) {
    const auto& source = snapshot.strategies[strategy_indices[strategy]];
    auto& target = strategy_state_[strategy];
    target.pnl_currency_nanos.store(source.pnl_currency_nanos,
                                    std::memory_order_relaxed);
    target.peak_pnl_currency_nanos.store(source.peak_pnl_currency_nanos,
                                         std::memory_order_relaxed);
    target.as_of_process_monotonic_time_ns.store(
        snapshot.as_of_process_monotonic_time_ns, std::memory_order_relaxed);
  }
  firm_daily_pnl_currency_nanos_.store(snapshot.total_pnl_currency_nanos,
                                       std::memory_order_relaxed);
  firm_peak_pnl_currency_nanos_.store(snapshot.peak_pnl_currency_nanos,
                                      std::memory_order_relaxed);
  pnl_as_of_process_monotonic_time_ns_.store(snapshot.as_of_process_monotonic_time_ns,
                                             std::memory_order_release);
  last_portfolio_snapshot_sequence_.store(snapshot.sequence, std::memory_order_relaxed);
  last_portfolio_snapshot_hash_.store(snapshot.stable_hash, std::memory_order_relaxed);
  state_available_.store(true, std::memory_order_release);
  position_generation_.fetch_add(1U, std::memory_order_acq_rel);
}

StateUpdateStatus
DeterministicPreTradeRiskEngine::apply_fill(const FillUpdate update) noexcept {
  if (!initialized() || update.symbol_index >= kMaximumRiskSymbols ||
      update.strategy_index >= kMaximumRiskStrategies ||
      (update.side != IntentAction::buy && update.side != IntentAction::sell) ||
      update.fill_quantity_units == 0U ||
      update.fill_quantity_units >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
      update.as_of_process_monotonic_time_ns == 0U) {
    return StateUpdateStatus::invalid;
  }
  if (!try_enter()) {
    return StateUpdateStatus::busy;
  }
  if (update.symbol_index >= limits_.symbol_count ||
      update.strategy_index >= limits_.strategy_count) {
    leave();
    return StateUpdateStatus::invalid;
  }
  auto& symbol = symbol_state_[update.symbol_index];
  auto& strategy = strategy_state_[update.strategy_index];
  const auto previous_as_of =
      symbol.as_of_process_monotonic_time_ns.load(std::memory_order_relaxed);
  auto& pending = update.side == IntentAction::buy ? symbol.pending_buy_quantity_units
                                                   : symbol.pending_sell_quantity_units;
  const auto previous_pending = pending.load(std::memory_order_relaxed);
  if (!symbol.available.load(std::memory_order_relaxed) ||
      update.as_of_process_monotonic_time_ns < previous_as_of ||
      update.release_reserved_quantity_units > previous_pending) {
    leave();
    return StateUpdateStatus::unavailable;
  }
  const auto previous_position =
      symbol.net_quantity_units.load(std::memory_order_relaxed);
  const auto quantity = static_cast<std::int64_t>(update.fill_quantity_units);
  std::int64_t next_position{};
  const auto position_valid =
      update.side == IntentAction::buy
          ? checked_add_signed(previous_position, quantity, next_position)
          : checked_subtract_signed(previous_position, quantity, next_position);
  const auto previous_firm_pnl =
      firm_daily_pnl_currency_nanos_.load(std::memory_order_relaxed);
  const auto previous_strategy_pnl =
      strategy.pnl_currency_nanos.load(std::memory_order_relaxed);
  const auto previous_firm_peak =
      firm_peak_pnl_currency_nanos_.load(std::memory_order_relaxed);
  const auto previous_strategy_peak =
      strategy.peak_pnl_currency_nanos.load(std::memory_order_relaxed);
  std::int64_t next_firm_pnl{};
  std::int64_t next_strategy_pnl{};
  if (!position_valid ||
      !checked_add_signed(previous_firm_pnl, update.realized_pnl_delta_currency_nanos,
                          next_firm_pnl) ||
      !checked_add_signed(previous_strategy_pnl,
                          update.realized_pnl_delta_currency_nanos,
                          next_strategy_pnl)) {
    leave();
    return StateUpdateStatus::arithmetic_overflow;
  }
  symbol.net_quantity_units.store(next_position, std::memory_order_relaxed);
  pending.store(previous_pending - update.release_reserved_quantity_units,
                std::memory_order_relaxed);
  symbol.as_of_process_monotonic_time_ns.store(update.as_of_process_monotonic_time_ns,
                                               std::memory_order_release);
  firm_daily_pnl_currency_nanos_.store(next_firm_pnl, std::memory_order_relaxed);
  firm_peak_pnl_currency_nanos_.store(std::max(previous_firm_peak, next_firm_pnl),
                                      std::memory_order_relaxed);
  strategy.pnl_currency_nanos.store(next_strategy_pnl, std::memory_order_relaxed);
  strategy.peak_pnl_currency_nanos.store(
      std::max(previous_strategy_peak, next_strategy_pnl), std::memory_order_relaxed);
  strategy.as_of_process_monotonic_time_ns.store(update.as_of_process_monotonic_time_ns,
                                                 std::memory_order_relaxed);
  pnl_as_of_process_monotonic_time_ns_.store(update.as_of_process_monotonic_time_ns,
                                             std::memory_order_release);
  position_generation_.fetch_add(1U, std::memory_order_acq_rel);
  leave();
  return StateUpdateStatus::applied;
}

StateUpdateStatus DeterministicPreTradeRiskEngine::update_profit_loss(
    const ProfitLossUpdate update) noexcept {
  if (!initialized() || update.strategy_index >= kMaximumRiskStrategies ||
      update.as_of_process_monotonic_time_ns == 0U ||
      update.firm_peak_pnl_currency_nanos < update.firm_daily_pnl_currency_nanos ||
      update.strategy_peak_pnl_currency_nanos < update.strategy_pnl_currency_nanos) {
    return StateUpdateStatus::invalid;
  }
  if (!try_enter()) {
    return StateUpdateStatus::busy;
  }
  if (update.strategy_index >= limits_.strategy_count) {
    leave();
    return StateUpdateStatus::invalid;
  }
  const auto previous =
      pnl_as_of_process_monotonic_time_ns_.load(std::memory_order_relaxed);
  if (update.as_of_process_monotonic_time_ns < previous) {
    leave();
    return StateUpdateStatus::unavailable;
  }
  firm_daily_pnl_currency_nanos_.store(update.firm_daily_pnl_currency_nanos,
                                       std::memory_order_relaxed);
  firm_peak_pnl_currency_nanos_.store(update.firm_peak_pnl_currency_nanos,
                                      std::memory_order_relaxed);
  strategy_state_[update.strategy_index].pnl_currency_nanos.store(
      update.strategy_pnl_currency_nanos, std::memory_order_relaxed);
  strategy_state_[update.strategy_index].peak_pnl_currency_nanos.store(
      update.strategy_peak_pnl_currency_nanos, std::memory_order_relaxed);
  strategy_state_[update.strategy_index].as_of_process_monotonic_time_ns.store(
      update.as_of_process_monotonic_time_ns, std::memory_order_relaxed);
  pnl_as_of_process_monotonic_time_ns_.store(update.as_of_process_monotonic_time_ns,
                                             std::memory_order_release);
  position_generation_.fetch_add(1U, std::memory_order_acq_rel);
  leave();
  return StateUpdateStatus::applied;
}

StateUpdateStatus DeterministicPreTradeRiskEngine::update_kill_switch(
    const KillSwitchUpdate update) noexcept {
  const auto valid_target =
      (update.scope == KillSwitchScope::symbol &&
       update.target_index < kMaximumRiskSymbols) ||
      (update.scope == KillSwitchScope::strategy &&
       update.target_index < kMaximumRiskStrategies) ||
      (update.scope == KillSwitchScope::venue &&
       update.target_index < kMaximumRiskVenues) ||
      (update.scope == KillSwitchScope::account && update.target_index == 0U) ||
      (update.scope == KillSwitchScope::firm && update.target_index == 0U);
  if (!initialized() || !valid_target || update.command_sequence == 0U ||
      (!update.engaged && !update.operator_authorized)) {
    return StateUpdateStatus::invalid;
  }
  if (!try_enter()) {
    return StateUpdateStatus::busy;
  }
  if (update.authority_epoch != authority_epoch_.load(std::memory_order_acquire)) {
    leave();
    return StateUpdateStatus::invalid;
  }
  auto previous_sequence = last_kill_command_sequence_.load(std::memory_order_acquire);
  if (update.command_sequence <= previous_sequence ||
      !last_kill_command_sequence_.compare_exchange_strong(
          previous_sequence, update.command_sequence, std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    leave();
    return StateUpdateStatus::unavailable;
  }
  const auto mask = update.target_index < 64U ? std::uint64_t{1U} << update.target_index
                                              : std::uint64_t{0U};
  auto apply = [engaged = update.engaged](std::atomic<std::uint64_t>& target,
                                          const std::uint64_t value) {
    if (engaged) {
      target.fetch_or(value, std::memory_order_acq_rel);
    } else {
      target.fetch_and(~value, std::memory_order_acq_rel);
    }
  };
  switch (update.scope) {
  case KillSwitchScope::symbol:
    apply(symbol_kill_mask_, mask);
    break;
  case KillSwitchScope::strategy:
    apply(strategy_kill_mask_, mask);
    break;
  case KillSwitchScope::venue:
    apply(venue_kill_mask_, mask);
    break;
  case KillSwitchScope::account:
    apply(account_kill_, 1U);
    break;
  case KillSwitchScope::firm:
    apply(firm_kill_, 1U);
    break;
  }
  kill_generation_.fetch_add(1U, std::memory_order_acq_rel);
  leave();
  return StateUpdateStatus::applied;
}

void DeterministicPreTradeRiskEngine::set_state_available(
    const bool available) noexcept {
  state_available_.store(available, std::memory_order_release);
}

RiskMetrics DeterministicPreTradeRiskEngine::metrics() const noexcept {
  return {.evaluations = evaluation_count_.load(std::memory_order_relaxed),
          .approvals = approval_count_.load(std::memory_order_relaxed),
          .rejections = rejection_count_.load(std::memory_order_relaxed),
          .busy_results = busy_count_.load(std::memory_order_relaxed),
          .journal_full_results = journal_full_count_.load(std::memory_order_relaxed),
          .position_generation = position_generation_.load(std::memory_order_acquire),
          .kill_generation = kill_generation_.load(std::memory_order_acquire),
          .active_limit_revision =
              active_limit_revision_.load(std::memory_order_acquire)};
}

const RiskLimitSnapshot&
DeterministicPreTradeRiskEngine::limits_for_quiescent_inspection() const noexcept {
  return limits_;
}

} // namespace aegis::risk
