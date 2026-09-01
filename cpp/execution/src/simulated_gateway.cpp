#include "aegis/execution/simulated_gateway.hpp"

#include <algorithm>
#include <bit>
#include <limits>

namespace aegis::execution {
namespace {

constexpr std::size_t kNoCommand = kMaximumGatewayCommands;
constexpr std::size_t kNoOrder = kMaximumGatewayOrders;
constexpr std::size_t kNoEvent = kMaximumGatewayEvents;
constexpr std::size_t kNoMarket = kMaximumGatewayInstruments;
constexpr std::size_t kMaximumWriterAttempts = 32U;

[[nodiscard]] std::size_t command_bucket(const common::GlobalEventId& value) noexcept {
  return static_cast<std::size_t>((value.high() ^ std::rotl(value.low(), 17)) &
                                  (kMaximumGatewayCommands - 1U));
}

[[nodiscard]] std::size_t order_bucket(const common::OrderId& value) noexcept {
  return static_cast<std::size_t>((value.high() ^ std::rotl(value.low(), 23)) &
                                  (kMaximumGatewayOrders - 1U));
}

[[nodiscard]] std::uint64_t nonzero(const std::uint64_t value) noexcept {
  return value == 0U ? 1U : value;
}

[[nodiscard]] bool terminal(const PaperOrderState state) noexcept {
  return state == PaperOrderState::filled || state == PaperOrderState::canceled ||
         state == PaperOrderState::rejected;
}

[[nodiscard]] std::uint64_t saturating_mix(const std::uint64_t left,
                                           const std::uint64_t right) noexcept {
  auto value = left ^ std::rotl(right, 29);
  value *= 0x9E3779B97F4A7C15ULL;
  return nonzero(value ^ (value >> 31U));
}

[[nodiscard]] bool checked_add(const std::uint64_t left, const std::uint64_t right,
                               std::uint64_t& output) noexcept {
  if (left > std::numeric_limits<std::uint64_t>::max() - right) {
    return false;
  }
  output = left + right;
  return true;
}

[[nodiscard]] bool checked_multiply(const std::uint64_t left, const std::uint64_t right,
                                    std::uint64_t& output) noexcept {
  if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left) {
    return false;
  }
  output = left * right;
  return true;
}

[[nodiscard]] common::GlobalEventId event_id(const common::GlobalEventId& command_id,
                                             const std::uint64_t ordinal,
                                             const GatewayResponseKind kind) noexcept {
  const auto kind_word = static_cast<std::uint64_t>(kind);
  return {saturating_mix(command_id.high(), ordinal ^ kind_word),
          saturating_mix(command_id.low(), std::rotl(ordinal, 11) ^ kind_word)};
}

[[nodiscard]] common::GlobalEventId execution_id(const common::OrderId& order_id,
                                                 const std::uint64_t market_hash,
                                                 const std::uint64_t ordinal) noexcept {
  return {saturating_mix(order_id.high(), market_hash),
          saturating_mix(order_id.low(), ordinal)};
}

} // namespace

SimulatedGatewayBase::SimulatedGatewayBase(GatewayConfiguration configuration,
                                           GatewayAuditJournal& journal,
                                           const GatewayMode required_mode) noexcept
    : configuration_(configuration), journal_(journal),
      session_(configuration.exchange_session_epoch,
               configuration.initial_fencing_token, configuration.heartbeat_interval_ns,
               configuration.heartbeat_timeout_ns),
      new_order_limiter_(configuration.command_rate_window_ns,
                         configuration.maximum_new_orders_per_window),
      cancel_limiter_(configuration.command_rate_window_ns,
                      configuration.maximum_cancels_per_window),
      replace_limiter_(configuration.command_rate_window_ns,
                       configuration.maximum_replaces_per_window),
      mode_(required_mode) {
  metrics_.mode = required_mode;
  metrics_.health = GatewayHealth::starting;
  metrics_.session_state = SessionState::stopped;
  if (!valid_gateway_configuration(configuration_) ||
      configuration_.startup_mode != required_mode || !session_.initialized() ||
      !new_order_limiter_.valid() || !cancel_limiter_.valid() ||
      !replace_limiter_.valid()) {
    health_ = GatewayHealth::unsafe;
    mode_ = GatewayMode::halted;
    metrics_.mode = GatewayMode::halted;
    metrics_.health = health_;
    return;
  }
  for (std::size_t index = 0U; index < configuration_.mapping_count; ++index) {
    markets_[index].mapping = configuration_.mappings[index];
    markets_[index].occupied = true;
  }
  health_ = GatewayHealth::starting;
  initialized_.store(true, std::memory_order_release);
}

SyntheticExchangeGateway::SyntheticExchangeGateway(
    GatewayConfiguration configuration, GatewayAuditJournal& journal) noexcept
    : SimulatedGatewayBase(configuration, journal, GatewayMode::simulation) {}

PaperBrokerGateway::PaperBrokerGateway(GatewayConfiguration configuration,
                                       GatewayAuditJournal& journal) noexcept
    : SimulatedGatewayBase(configuration, journal, GatewayMode::paper) {}

bool SimulatedGatewayBase::try_enter() noexcept {
  for (std::size_t attempt = 0U; attempt < kMaximumWriterAttempts; ++attempt) {
    if (!writer_gate_.test_and_set(std::memory_order_acquire)) {
      return true;
    }
  }
  return false;
}

void SimulatedGatewayBase::leave() noexcept {
  writer_gate_.clear(std::memory_order_release);
}

// The output values have distinct documented roles despite sharing an integer type.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
bool SimulatedGatewayBase::append_audit(
    const GatewayAuditJournal::Reservation reservation, const GatewayAuditKind kind,
    const GatewayReason reason, const common::GlobalEventId correlation_id,
    const common::OrderId order_id, const std::uint64_t command_hash,
    const std::uint64_t risk_hash, const std::uint64_t safety_hash,
    const std::uint64_t event_hash, const std::uint64_t now_ns, std::uint64_t& sequence,
    std::uint64_t& record_hash) noexcept {
  const GatewayAuditRecord record{.kind = kind,
                                  .mode = mode_,
                                  .session_state = session_.state(),
                                  .reason = reason,
                                  .correlation_id = correlation_id,
                                  .order_id = order_id,
                                  .command_hash = command_hash,
                                  .risk_decision_hash = risk_hash,
                                  .safety_state_hash = safety_hash,
                                  .gateway_event_hash = event_hash,
                                  .process_monotonic_time_ns = now_ns};
  if (!journal_.commit(reservation, record)) {
    return false;
  }
  sequence = reservation.sequence;
  record_hash = journal_.last_record_hash();
  return true;
}
// NOLINTEND(bugprone-easily-swappable-parameters)

bool SimulatedGatewayBase::append_simple_audit(const GatewayAuditKind kind,
                                               const GatewayReason reason,
                                               const std::uint64_t now_ns) noexcept {
  const auto reservation = journal_.try_reserve();
  if (!reservation.valid) {
    ++metrics_.journal_full_count;
    return false;
  }
  std::uint64_t sequence{};
  std::uint64_t record_hash{};
  return append_audit(reservation, kind, reason, {}, {}, 0U, 0U, 0U, 0U, now_ns,
                      sequence, record_hash);
}

void SimulatedGatewayBase::set_unsafe(const SessionReason reason,
                                      const std::uint64_t now_ns) noexcept {
  health_ = GatewayHealth::unsafe;
  session_.halt(now_ns, reason);
  metrics_.mode = mode_;
  metrics_.health = health_;
  metrics_.session_state = session_.state();
}

void SimulatedGatewayBase::update_health() noexcept {
  if (stopped_.load(std::memory_order_acquire)) {
    health_ = GatewayHealth::stopped;
  } else if (health_ == GatewayHealth::unsafe) {
    // Unsafe is sticky for this process epoch.
  } else if (session_.state() == SessionState::recovering ||
             session_.state() == SessionState::halted) {
    health_ = GatewayHealth::recovering;
  } else if (session_.state() == SessionState::active) {
    health_ = GatewayHealth::healthy;
  } else {
    health_ = GatewayHealth::starting;
  }
  metrics_.mode = mode_;
  metrics_.health = health_;
  metrics_.session_state = session_.state();
}

bool SimulatedGatewayBase::start(const std::uint64_t now_ns) noexcept {
  if (!initialized_.load(std::memory_order_acquire) ||
      stopped_.load(std::memory_order_acquire) || !try_enter()) {
    return false;
  }
  const auto reservation = journal_.try_reserve();
  if (!reservation.valid) {
    ++metrics_.journal_full_count;
    set_unsafe(SessionReason::journal_unavailable, now_ns);
    leave();
    return false;
  }
  const bool transitioned =
      session_.request_logon(now_ns) && session_.accept_logon(now_ns);
  std::uint64_t sequence{};
  std::uint64_t record_hash{};
  const bool journaled = append_audit(
      reservation, GatewayAuditKind::session_transition,
      transitioned ? GatewayReason::accepted : GatewayReason::session_not_active, {},
      {}, 0U, 0U, 0U, 0U, now_ns, sequence, record_hash);
  if (!transitioned || !journaled) {
    set_unsafe(SessionReason::journal_unavailable, now_ns);
  }
  update_health();
  leave();
  return transitioned && journaled;
}

bool SimulatedGatewayBase::logoff(const std::uint64_t now_ns) noexcept {
  if (!initialized_.load(std::memory_order_acquire) || !try_enter()) {
    return false;
  }
  const auto reservation = journal_.try_reserve();
  if (!reservation.valid) {
    ++metrics_.journal_full_count;
    set_unsafe(SessionReason::journal_unavailable, now_ns);
    leave();
    return false;
  }
  const bool transitioned =
      session_.request_logoff(now_ns) && session_.complete_logoff(now_ns);
  std::uint64_t sequence{};
  std::uint64_t record_hash{};
  const bool journaled = append_audit(
      reservation, GatewayAuditKind::session_transition,
      transitioned ? GatewayReason::accepted : GatewayReason::session_not_active, {},
      {}, 0U, 0U, 0U, 0U, now_ns, sequence, record_hash);
  if (!journaled) {
    set_unsafe(SessionReason::journal_unavailable, now_ns);
  }
  update_health();
  leave();
  return transitioned && journaled;
}

std::size_t SimulatedGatewayBase::find_command(
    const common::GlobalEventId command_id) const noexcept {
  if (!command_id.valid()) {
    return kNoCommand;
  }
  const auto start = command_bucket(command_id);
  for (std::size_t offset = 0U; offset < commands_.size(); ++offset) {
    const auto index = (start + offset) & (commands_.size() - 1U);
    if (!commands_[index].occupied) {
      return kNoCommand;
    }
    if (commands_[index].command_id == command_id) {
      return index;
    }
  }
  return kNoCommand;
}

std::size_t SimulatedGatewayBase::find_free_command(
    const common::GlobalEventId command_id) const noexcept {
  const auto start = command_bucket(command_id);
  for (std::size_t offset = 0U; offset < commands_.size(); ++offset) {
    const auto index = (start + offset) & (commands_.size() - 1U);
    if (!commands_[index].occupied) {
      return index;
    }
  }
  return kNoCommand;
}

std::size_t
SimulatedGatewayBase::find_order(const common::OrderId order_id) const noexcept {
  if (!order_id.valid()) {
    return kNoOrder;
  }
  const auto start = order_bucket(order_id);
  for (std::size_t offset = 0U; offset < orders_.size(); ++offset) {
    const auto index = (start + offset) & (orders_.size() - 1U);
    if (!orders_[index].occupied) {
      return kNoOrder;
    }
    if (orders_[index].order_id == order_id) {
      return index;
    }
  }
  return kNoOrder;
}

std::size_t
SimulatedGatewayBase::find_free_order(const common::OrderId order_id) const noexcept {
  const auto start = order_bucket(order_id);
  for (std::size_t offset = 0U; offset < orders_.size(); ++offset) {
    const auto index = (start + offset) & (orders_.size() - 1U);
    if (!orders_[index].occupied) {
      return index;
    }
  }
  return kNoOrder;
}

std::size_t SimulatedGatewayBase::find_market(
    const common::InstrumentId instrument_id) const noexcept {
  for (std::size_t index = 0U; index < configuration_.mapping_count; ++index) {
    if (markets_[index].occupied &&
        markets_[index].mapping.instrument_id == instrument_id) {
      return index;
    }
  }
  return kNoMarket;
}

std::size_t SimulatedGatewayBase::find_market(
    const market_data::synthetic::SyntheticEvent& event) const noexcept {
  for (std::size_t index = 0U; index < configuration_.mapping_count; ++index) {
    if (markets_[index].occupied &&
        markets_[index].mapping.synthetic_venue_number == event.venue_number &&
        markets_[index].mapping.synthetic_instrument_number ==
            event.instrument_number) {
      return index;
    }
  }
  return kNoMarket;
}

std::size_t SimulatedGatewayBase::find_free_event() const noexcept {
  for (std::size_t index = 0U; index < events_.size(); ++index) {
    if (!events_[index].occupied) {
      return index;
    }
  }
  return kNoEvent;
}

std::size_t
SimulatedGatewayBase::find_due_event(const std::uint64_t now_ns) const noexcept {
  auto result = kNoEvent;
  for (std::size_t index = 0U; index < events_.size(); ++index) {
    if (!events_[index].occupied || events_[index].due_ns > now_ns) {
      continue;
    }
    if (result == kNoEvent || events_[index].due_ns < events_[result].due_ns ||
        (events_[index].due_ns == events_[result].due_ns &&
         events_[index].insertion_ordinal < events_[result].insertion_ordinal)) {
      result = index;
    }
  }
  return result;
}

bool SimulatedGatewayBase::schedule_event(GatewayEvent event,
                                          const std::uint64_t due_ns) noexcept {
  const auto index = find_free_event();
  if (index == kNoEvent || due_ns == 0U) {
    return false;
  }
  event.process_monotonic_time_ns = due_ns;
  event.stable_hash = stable_gateway_event_hash(event);
  events_[index] = {.event = event,
                    .due_ns = due_ns,
                    .insertion_ordinal = next_event_ordinal_++,
                    .occupied = true};
  ++active_event_count_;
  metrics_.event_queue_high_watermark =
      std::max(metrics_.event_queue_high_watermark, active_event_count_);
  return true;
}

bool SimulatedGatewayBase::mode_matches_risk(
    const risk::TradingMode risk_mode) const noexcept {
  return (mode_ == GatewayMode::simulation &&
          risk_mode == risk::TradingMode::simulation) ||
         (mode_ == GatewayMode::paper && risk_mode == risk::TradingMode::paper)
#if AEGIS_LIVE_TRADING_COMPILED
         || (mode_ == GatewayMode::live_active && risk_mode == risk::TradingMode::live)
#endif
      ;
}

GatewayReason SimulatedGatewayBase::validate_risk_binding(
    const GatewayRequest& request) const noexcept {
  const auto& command = request.command;
  const auto& decision = request.risk_decision;
  const auto now_ns = request.safety.observed_process_monotonic_time_ns;
  if (decision.decision != risk::DecisionCode::approved) {
    return GatewayReason::risk_decision_rejected;
  }
  if (decision.valid_until_process_monotonic_time_ns < now_ns ||
      decision.evaluated_process_monotonic_time_ns > now_ns ||
      command.generated_process_monotonic_time_ns > now_ns) {
    return GatewayReason::risk_decision_expired;
  }
  if (decision.stable_hash != command.risk_decision_hash ||
      decision.intent_hash != command.intent_hash) {
    return GatewayReason::risk_hash_mismatch;
  }
  if (decision.session_id != command.session_id ||
      decision.account_id != command.account_id ||
      decision.strategy_id != command.strategy_id ||
      decision.venue_id != command.venue_id ||
      decision.instrument_id != command.instrument_id ||
      decision.configuration_version != command.configuration_version) {
    return GatewayReason::scope_mismatch;
  }
  if (!mode_matches_risk(decision.trading_mode)) {
    return GatewayReason::mode_mismatch;
  }
  if (command.kind == oms::GatewayCommandKind::cancel) {
    return decision.intent_action == risk::IntentAction::cancel
               ? GatewayReason::accepted
               : GatewayReason::scope_mismatch;
  }
  return decision.intent_action == command.side &&
                 decision.approved_price_ticks == command.price_ticks &&
                 decision.approved_quantity_units == command.quantity_units
             ? GatewayReason::accepted
             : GatewayReason::scope_mismatch;
}

bool SimulatedGatewayBase::market_state_allows_new(
    const market_state::MarketState state,
    const market_state::OfficialTradingStatus status) const noexcept {
  if (status == market_state::OfficialTradingStatus::open) {
    return state == market_state::MarketState::normal ||
           state == market_state::MarketState::scheduled_event ||
           state == market_state::MarketState::breaking_news ||
           state == market_state::MarketState::event_price_discovery ||
           state == market_state::MarketState::volatility_spike;
  }
  return configuration_.paper_model.allow_auction_orders &&
         status == market_state::OfficialTradingStatus::auction &&
         state == market_state::MarketState::reopening;
}

GatewayReason SimulatedGatewayBase::validate_new_exposure_state(
    const GatewayRequest& request) const noexcept {
  const auto& safety = request.safety;
  const auto now_ns = safety.observed_process_monotonic_time_ns;
  const auto market_observed =
      safety.market_state_snapshot.observed_process_monotonic_time_ns;
  if (market_observed > now_ns ||
      now_ns - market_observed > configuration_.maximum_clock_age_ns ||
      !market_state_allows_new(safety.market_state_snapshot.state,
                               safety.official_trading_status)) {
    return safety.official_trading_status ==
                       market_state::OfficialTradingStatus::halted ||
                   safety.official_trading_status ==
                       market_state::OfficialTradingStatus::closed
               ? GatewayReason::trading_halted
               : GatewayReason::market_state_unsafe;
  }
  if (safety.clock_quality_snapshot.state != time::ClockQualityState::healthy ||
      safety.clock_quality_snapshot.operation_mode !=
          time::ClockOperationMode::normal ||
      !safety.clock_quality_snapshot.hardware_timestamp_available ||
      !safety.clock_quality_snapshot.source_id.valid() ||
      safety.clock_quality_snapshot.observed_at.value() > now_ns ||
      now_ns - safety.clock_quality_snapshot.observed_at.value() >
          configuration_.maximum_clock_age_ns) {
    return GatewayReason::clock_unhealthy;
  }
  if (safety.feed_health != market_state::FeedHealth::healthy) {
    return GatewayReason::feed_unhealthy;
  }
  if (safety.book_validity != market_state::BookValidity::valid) {
    return GatewayReason::book_invalid;
  }
  if (safety.kill_switch_engaged) {
    return GatewayReason::kill_switch_engaged;
  }
  return GatewayReason::accepted;
}

GatewayReason SimulatedGatewayBase::final_gate(
    const GatewayRequest& request,
    const oms::GatewayCommandKind expected) const noexcept {
  if (!valid_gateway_request(request)) {
    return GatewayReason::invalid_request;
  }
  if (request.command.kind != expected) {
    return GatewayReason::command_kind_mismatch;
  }
  if (!session_.ready() || health_ != GatewayHealth::healthy) {
    return GatewayReason::session_not_active;
  }
  if (request.command.session_id != configuration_.session_id ||
      request.command.account_id != configuration_.account_id ||
      request.command.venue_id != configuration_.venue_id ||
      request.command.configuration_version != configuration_.configuration_version ||
      find_market(request.command.instrument_id) == kNoMarket) {
    return GatewayReason::scope_mismatch;
  }
  if (request.command.authority.exchange_session_epoch !=
          configuration_.exchange_session_epoch ||
      request.command.authority.fencing_token != configuration_.initial_fencing_token ||
      request.safety.authority.exchange_session_epoch !=
          request.command.authority.exchange_session_epoch ||
      request.safety.authority.fencing_token !=
          request.command.authority.fencing_token) {
    return GatewayReason::stale_fencing_token;
  }
  if (request.safety.effective_configuration_hash != configuration_.stable_hash) {
    return GatewayReason::scope_mismatch;
  }
  if (!request.safety.journal_ready) {
    return GatewayReason::journal_capacity;
  }
  const auto risk_reason = validate_risk_binding(request);
  if (risk_reason != GatewayReason::accepted) {
    return risk_reason;
  }
#if AEGIS_LIVE_TRADING_COMPILED
  if (mode_ == GatewayMode::live_active) {
    if (!request.safety.signed_configuration_valid) {
      return GatewayReason::signed_configuration_required;
    }
    if (!request.safety.operator_authorized ||
        request.safety.operator_authorization_valid_until_ns <
            request.safety.observed_process_monotonic_time_ns) {
      return GatewayReason::operator_authorization_required;
    }
    if (!request.safety.activation_record_durable ||
        request.safety.activation_record_hash == 0U) {
      return GatewayReason::activation_record_required;
    }
  }
#endif
  if (expected != oms::GatewayCommandKind::cancel) {
    return validate_new_exposure_state(request);
  }
  // Protective cancels are permitted through degraded synthetic/paper market
  // state because their semantics are repository-owned and deterministic.
  return GatewayReason::accepted;
}

GatewayEvent SimulatedGatewayBase::base_event(
    const oms::GatewayCommand& command, const GatewayResponseKind kind,
    const GatewayReason reason, const std::uint64_t event_ordinal) const noexcept {
  return {.event_id = event_id(command.command_id, event_ordinal, kind),
          .source_command_id = command.command_id,
          .execution_id = {},
          .order_id = command.order_id,
          .external_order_id = command.external_order_id,
          .session_id = configuration_.session_id,
          .account_id = configuration_.account_id,
          .venue_id = configuration_.venue_id,
          .instrument_id = command.instrument_id,
          .configuration_version = configuration_.configuration_version,
          .authority = command.authority,
          .mode = mode_,
          .kind = kind,
          .reason = reason,
          .price_ticks = command.price_ticks,
          .quantity_units = command.quantity_units};
}

GatewayReason
SimulatedGatewayBase::process_new(const GatewayRequest& request,
                                  const std::uint64_t outbound_sequence,
                                  std::uint64_t& scheduled_hash) noexcept {
  if (find_order(request.command.order_id) != kNoOrder) {
    return GatewayReason::command_identity_conflict;
  }
  const auto order_index = find_free_order(request.command.order_id);
  if (order_index == kNoOrder || find_free_event() == kNoEvent) {
    return order_index == kNoOrder ? GatewayReason::order_capacity
                                   : GatewayReason::event_capacity;
  }
  std::uint64_t due_ns{};
  if (!checked_add(request.safety.observed_process_monotonic_time_ns,
                   configuration_.paper_model.acknowledgement_latency_ns, due_ns)) {
    return GatewayReason::arithmetic_overflow;
  }
  ++new_order_ordinal_;
  const bool rejected =
      configuration_.paper_model.reject_every_nth_new_order != 0U &&
      new_order_ordinal_ % configuration_.paper_model.reject_every_nth_new_order == 0U;
  const auto market_index = find_market(request.command.instrument_id);
  std::uint64_t displayed{};
  if (market_index != kNoMarket) {
    const auto& market = markets_[market_index];
    if (request.command.side == risk::IntentAction::buy &&
        market.best_bid_ticks == request.command.price_ticks) {
      displayed = market.best_bid_quantity_units;
    } else if (request.command.side == risk::IntentAction::sell &&
               market.best_ask_ticks == request.command.price_ticks) {
      displayed = market.best_ask_quantity_units;
    }
  }
  std::uint64_t queue_ahead{};
  if (!checked_add(configuration_.paper_model.initial_queue_ahead_units, displayed,
                   queue_ahead)) {
    return GatewayReason::arithmetic_overflow;
  }
  const oms::ExternalOrderId external{
      .high = nonzero(request.command.order_id.high() ^
                      configuration_.paper_model.deterministic_seed),
      .low = nonzero(request.command.order_id.low() ^ outbound_sequence)};
  orders_[order_index] = {.source_command_id = request.command.command_id,
                          .order_id = request.command.order_id,
                          .external_order_id = external,
                          .instrument_id = request.command.instrument_id,
                          .side = request.command.side,
                          .state = rejected ? PaperOrderState::rejected
                                            : PaperOrderState::pending_ack,
                          .price_ticks = request.command.price_ticks,
                          .quantity_units = request.command.quantity_units,
                          .queue_ahead_units = queue_ahead,
                          .acknowledgement_due_ns = due_ns,
                          .occupied = true};
  auto response = base_event(
      request.command,
      rejected ? GatewayResponseKind::rejection : GatewayResponseKind::acknowledgement,
      rejected ? GatewayReason::paper_policy_reject : GatewayReason::accepted,
      next_event_ordinal_);
  response.external_order_id = rejected ? oms::ExternalOrderId{} : external;
  response.remaining_quantity_units = request.command.quantity_units;
  response.process_monotonic_time_ns = due_ns;
  response.stable_hash = stable_gateway_event_hash(response);
  if (!schedule_event(response, due_ns)) {
    orders_[order_index] = {};
    return GatewayReason::event_capacity;
  }
  scheduled_hash = response.stable_hash;
  return GatewayReason::accepted;
}

GatewayReason
SimulatedGatewayBase::process_cancel(const GatewayRequest& request,
                                     const std::uint64_t /*outbound_sequence*/,
                                     std::uint64_t& scheduled_hash) noexcept {
  if (find_free_event() == kNoEvent) {
    return GatewayReason::event_capacity;
  }
  std::uint64_t due_ns{};
  if (!checked_add(request.safety.observed_process_monotonic_time_ns,
                   configuration_.paper_model.cancel_latency_ns, due_ns)) {
    return GatewayReason::arithmetic_overflow;
  }
  const auto order_index = find_order(request.command.order_id);
  const bool reject = order_index == kNoOrder || terminal(orders_[order_index].state);
  auto response =
      base_event(request.command,
                 reject ? GatewayResponseKind::cancel_rejection
                        : GatewayResponseKind::cancel_acknowledgement,
                 reject ? GatewayReason::order_not_found : GatewayReason::accepted,
                 next_event_ordinal_);
  if (!reject) {
    auto& order = orders_[order_index];
    order.state = PaperOrderState::pending_cancel;
    order.cancel_due_ns = due_ns;
    response.external_order_id = order.external_order_id;
    response.cumulative_fill_quantity_units = order.cumulative_fill_quantity_units;
    response.remaining_quantity_units =
        order.quantity_units - order.cumulative_fill_quantity_units;
  }
  response.process_monotonic_time_ns = due_ns;
  response.stable_hash = stable_gateway_event_hash(response);
  if (!schedule_event(response, due_ns)) {
    return GatewayReason::event_capacity;
  }
  scheduled_hash = response.stable_hash;
  return GatewayReason::accepted;
}

GatewayReason
SimulatedGatewayBase::process_replace(const GatewayRequest& request,
                                      const std::uint64_t /*outbound_sequence*/,
                                      std::uint64_t& scheduled_hash) noexcept {
  if (find_free_event() == kNoEvent) {
    return GatewayReason::event_capacity;
  }
  std::uint64_t due_ns{};
  if (!checked_add(request.safety.observed_process_monotonic_time_ns,
                   configuration_.paper_model.replace_latency_ns, due_ns)) {
    return GatewayReason::arithmetic_overflow;
  }
  const auto order_index = find_order(request.command.order_id);
  const bool reject = order_index == kNoOrder || terminal(orders_[order_index].state) ||
                      request.command.quantity_units <=
                          (order_index == kNoOrder
                               ? 0U
                               : orders_[order_index].cumulative_fill_quantity_units);
  auto response =
      base_event(request.command,
                 reject ? GatewayResponseKind::replace_rejection
                        : GatewayResponseKind::replace_acknowledgement,
                 reject ? GatewayReason::order_not_found : GatewayReason::accepted,
                 next_event_ordinal_);
  if (!reject) {
    auto& order = orders_[order_index];
    order.state = PaperOrderState::pending_replace;
    order.pending_replace_price_ticks = request.command.price_ticks;
    order.pending_replace_quantity_units = request.command.quantity_units;
    order.replace_due_ns = due_ns;
    response.external_order_id = order.external_order_id;
    response.cumulative_fill_quantity_units = order.cumulative_fill_quantity_units;
    response.remaining_quantity_units =
        request.command.quantity_units - order.cumulative_fill_quantity_units;
  }
  response.process_monotonic_time_ns = due_ns;
  response.stable_hash = stable_gateway_event_hash(response);
  if (!schedule_event(response, due_ns)) {
    return GatewayReason::event_capacity;
  }
  scheduled_hash = response.stable_hash;
  return GatewayReason::accepted;
}

GatewayReason
SimulatedGatewayBase::process_command(const GatewayRequest& request,
                                      const std::uint64_t outbound_sequence,
                                      std::uint64_t& scheduled_hash) noexcept {
  switch (request.command.kind) {
  case oms::GatewayCommandKind::new_order:
    return process_new(request, outbound_sequence, scheduled_hash);
  case oms::GatewayCommandKind::cancel:
    return process_cancel(request, outbound_sequence, scheduled_hash);
  case oms::GatewayCommandKind::replace:
    return process_replace(request, outbound_sequence, scheduled_hash);
  case oms::GatewayCommandKind::none:
    break;
  }
  return GatewayReason::invalid_command;
}

// The explicit gate order mirrors the auditable fail-closed decision sequence.
// NOLINTBEGIN(readability-function-cognitive-complexity)
GatewaySubmitResult
SimulatedGatewayBase::submit(const GatewayRequest& request,
                             const oms::GatewayCommandKind expected) noexcept {
  ++metrics_.commands_received;
  if (stopped_.load(std::memory_order_acquire)) {
    GatewaySubmitResult result{.status = GatewaySubmitStatus::stopped,
                               .reason = GatewayReason::shutdown};
    result.stable_hash = stable_gateway_submit_result_hash(result);
    return result;
  }
  if (!initialized_.load(std::memory_order_acquire)) {
    GatewaySubmitResult result{.status = GatewaySubmitStatus::rejected,
                               .reason = GatewayReason::invalid_configuration};
    result.stable_hash = stable_gateway_submit_result_hash(result);
    return result;
  }
  if (!try_enter()) {
    GatewaySubmitResult result{.status = GatewaySubmitStatus::busy,
                               .reason = GatewayReason::recovery_required};
    result.stable_hash = stable_gateway_submit_result_hash(result);
    return result;
  }
  const auto reservation = journal_.try_reserve();
  if (!reservation.valid) {
    ++metrics_.journal_full_count;
    set_unsafe(SessionReason::journal_unavailable,
               request.safety.observed_process_monotonic_time_ns);
    leave();
    GatewaySubmitResult result{.status = GatewaySubmitStatus::capacity_exhausted,
                               .reason = GatewayReason::journal_capacity};
    result.stable_hash = stable_gateway_submit_result_hash(result);
    return result;
  }

  GatewaySubmitResult result{};
  std::uint64_t scheduled_hash{};
  const auto existing = find_command(request.command.command_id);
  if (existing != kNoCommand) {
    if (commands_[existing].request_hash == request.stable_hash) {
      result = {.status = GatewaySubmitStatus::duplicate,
                .reason = GatewayReason::duplicate_command};
      ++metrics_.duplicate_commands;
    } else {
      result = {.status = GatewaySubmitStatus::rejected,
                .reason = GatewayReason::command_identity_conflict};
      set_unsafe(SessionReason::split_brain,
                 request.safety.observed_process_monotonic_time_ns);
    }
  } else {
    const auto free_command = find_free_command(request.command.command_id);
    auto gate_reason = final_gate(request, expected);
    if (free_command == kNoCommand) {
      gate_reason = GatewayReason::order_capacity;
    }
    if (gate_reason == GatewayReason::accepted) {
      bool rate_allowed = false;
      if (expected == oms::GatewayCommandKind::new_order) {
        rate_allowed =
            new_order_limiter_.allow(request.safety.observed_process_monotonic_time_ns);
      } else if (expected == oms::GatewayCommandKind::cancel) {
        rate_allowed =
            cancel_limiter_.allow(request.safety.observed_process_monotonic_time_ns);
      } else if (expected == oms::GatewayCommandKind::replace) {
        rate_allowed =
            replace_limiter_.allow(request.safety.observed_process_monotonic_time_ns);
      }
      if (!rate_allowed) {
        gate_reason = GatewayReason::rate_limited;
        ++metrics_.rate_limit_rejections;
      }
    }
    std::uint64_t outbound_sequence{};
    if (gate_reason == GatewayReason::accepted &&
        session_.claim_outbound(request.safety.observed_process_monotonic_time_ns,
                                outbound_sequence) != SequenceStatus::in_order) {
      gate_reason = GatewayReason::sequence_gap;
    }
    if (gate_reason == GatewayReason::accepted) {
      gate_reason = process_command(request, outbound_sequence, scheduled_hash);
    }
    result.outbound_sequence = outbound_sequence;
    result.reason = gate_reason;
    result.status = GatewaySubmitStatus::rejected;
    if (gate_reason == GatewayReason::accepted) {
      result.status = GatewaySubmitStatus::accepted;
    } else if (gate_reason == GatewayReason::order_capacity ||
               gate_reason == GatewayReason::event_capacity) {
      result.status = GatewaySubmitStatus::capacity_exhausted;
    }
    if (free_command != kNoCommand) {
      commands_[free_command] = {.command_id = request.command.command_id,
                                 .request_hash = request.stable_hash,
                                 .result = result,
                                 .occupied = true};
    }
  }

  result.journal_sequence = reservation.sequence;
  std::uint64_t record_sequence{};
  std::uint64_t record_hash{};
  const bool journaled = append_audit(
      reservation, GatewayAuditKind::command_decision, result.reason,
      request.command.command_id, request.command.order_id, request.command.stable_hash,
      request.risk_decision.stable_hash, request.safety.stable_hash, scheduled_hash,
      request.safety.observed_process_monotonic_time_ns, record_sequence, record_hash);
  if (!journaled) {
    set_unsafe(SessionReason::journal_unavailable,
               request.safety.observed_process_monotonic_time_ns);
    result.status = GatewaySubmitStatus::capacity_exhausted;
    result.reason = GatewayReason::journal_capacity;
  } else {
    result.journal_record_hash = record_hash;
  }
  result.stable_hash = stable_gateway_submit_result_hash(result);
  if (result.status == GatewaySubmitStatus::accepted) {
    ++metrics_.commands_accepted;
    if (expected == oms::GatewayCommandKind::new_order) {
      ++metrics_.new_orders;
    } else if (expected == oms::GatewayCommandKind::cancel) {
      ++metrics_.cancels;
    } else {
      ++metrics_.replaces;
    }
  } else if (result.status != GatewaySubmitStatus::duplicate) {
    ++metrics_.commands_rejected;
  }
  update_health();
  leave();
  return result;
}
// NOLINTEND(readability-function-cognitive-complexity)

GatewaySubmitResult
SimulatedGatewayBase::send_order(const GatewayRequest& request) noexcept {
  return submit(request, oms::GatewayCommandKind::new_order);
}

GatewaySubmitResult
SimulatedGatewayBase::cancel_order(const GatewayRequest& request) noexcept {
  return submit(request, oms::GatewayCommandKind::cancel);
}

GatewaySubmitResult
SimulatedGatewayBase::replace_order(const GatewayRequest& request) noexcept {
  return submit(request, oms::GatewayCommandKind::replace);
}

void SimulatedGatewayBase::settle_due_controls(const std::uint64_t now_ns) noexcept {
  for (auto& order : orders_) {
    if (!order.occupied || terminal(order.state)) {
      continue;
    }
    if (order.state == PaperOrderState::pending_ack &&
        now_ns >= order.acknowledgement_due_ns) {
      order.state = PaperOrderState::working;
    } else if (order.state == PaperOrderState::pending_cancel &&
               now_ns >= order.cancel_due_ns) {
      order.state = PaperOrderState::canceled;
    } else if (order.state == PaperOrderState::pending_replace &&
               now_ns >= order.replace_due_ns) {
      order.price_ticks = order.pending_replace_price_ticks;
      order.quantity_units = order.pending_replace_quantity_units;
      order.pending_replace_price_ticks = 0;
      order.pending_replace_quantity_units = 0U;
      order.queue_ahead_units = configuration_.paper_model.initial_queue_ahead_units;
      order.state = PaperOrderState::working;
    }
  }
}

void SimulatedGatewayBase::update_market(
    const market_data::synthetic::SyntheticEvent& event,
    const std::size_t market_index) noexcept {
  auto& market = markets_[market_index];
  market.last_market_event_hash = event.event_hash;
  if (event.type == market_data::synthetic::NativeMessageType::trading_status) {
    market.status = event.status;
    return;
  }
  if (event.side == market_data::synthetic::Side::bid && event.price_ticks > 0) {
    market.best_bid_ticks = event.price_ticks;
    market.best_bid_quantity_units = event.level_quantity_units != 0U
                                         ? event.level_quantity_units
                                         : event.quantity_units;
  } else if (event.side == market_data::synthetic::Side::ask && event.price_ticks > 0) {
    market.best_ask_ticks = event.price_ticks;
    market.best_ask_quantity_units = event.level_quantity_units != 0U
                                         ? event.level_quantity_units
                                         : event.quantity_units;
  }
}

// Fill modeling keeps validation, overflow checks, and mutation in one ordered path.
// NOLINTBEGIN(readability-function-cognitive-complexity)
GatewayReason
SimulatedGatewayBase::process_trade(const market_data::synthetic::SyntheticEvent& event,
                                    const std::size_t market_index,
                                    std::uint64_t& scheduled_hash) noexcept {
  const auto& market = markets_[market_index];
  const bool continuous =
      event.type == market_data::synthetic::NativeMessageType::trade &&
      market.status == market_data::synthetic::TradingStatus::open;
  const bool auction =
      event.type == market_data::synthetic::NativeMessageType::auction_imbalance &&
      market.status == market_data::synthetic::TradingStatus::auction;
  if (!continuous && !auction) {
    return GatewayReason::accepted;
  }
  if (auction && !configuration_.paper_model.allow_auction_orders) {
    return GatewayReason::accepted;
  }
  if (event.price_ticks <= 0 || event.quantity_units == 0U) {
    return GatewayReason::invalid_request;
  }
  for (auto& order : orders_) {
    if (!order.occupied ||
        (order.state != PaperOrderState::working &&
         order.state != PaperOrderState::pending_cancel &&
         order.state != PaperOrderState::pending_replace) ||
        order.instrument_id != market.mapping.instrument_id) {
      continue;
    }
    const bool eligible = order.side == risk::IntentAction::buy
                              ? event.price_ticks <= order.price_ticks
                              : event.price_ticks >= order.price_ticks;
    if (!eligible) {
      continue;
    }
    const auto queue_before = order.queue_ahead_units;
    if (order.queue_ahead_units >= event.quantity_units) {
      order.queue_ahead_units -= event.quantity_units;
      return GatewayReason::accepted;
    }
    const auto available = event.quantity_units - order.queue_ahead_units;
    const auto remaining = order.quantity_units - order.cumulative_fill_quantity_units;
    const auto fill_quantity = std::min(
        {available, remaining, configuration_.paper_model.maximum_fill_chunk_units});
    if (fill_quantity == 0U || find_free_event() == kNoEvent) {
      return fill_quantity == 0U ? GatewayReason::accepted
                                 : GatewayReason::event_capacity;
    }
    const auto quotient = fill_quantity / kPartsPerMillion;
    const auto remainder = fill_quantity % kPartsPerMillion;
    std::uint64_t whole_impact{};
    std::uint64_t partial_product{};
    if (!checked_multiply(quotient,
                          configuration_.paper_model.impact_ticks_per_million_units,
                          whole_impact) ||
        !checked_multiply(remainder,
                          configuration_.paper_model.impact_ticks_per_million_units,
                          partial_product)) {
      return GatewayReason::arithmetic_overflow;
    }
    const auto partial_impact =
        partial_product == 0U ? 0U : ((partial_product - 1U) / kPartsPerMillion) + 1U;
    std::uint64_t impact{};
    std::uint64_t modeled_cost{};
    if (!checked_add(whole_impact, partial_impact, impact) ||
        !checked_add(configuration_.paper_model.slippage_ticks, impact, modeled_cost) ||
        modeled_cost >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return GatewayReason::arithmetic_overflow;
    }
    auto fill_price = event.price_ticks;
    const auto signed_cost = static_cast<std::int64_t>(modeled_cost);
    if (order.side == risk::IntentAction::buy) {
      if (fill_price > std::numeric_limits<std::int64_t>::max() - signed_cost) {
        return GatewayReason::arithmetic_overflow;
      }
      fill_price = std::min(fill_price + signed_cost, order.price_ticks);
    } else {
      if (fill_price < std::numeric_limits<std::int64_t>::min() + signed_cost) {
        return GatewayReason::arithmetic_overflow;
      }
      fill_price = std::max(fill_price - signed_cost, order.price_ticks);
    }
    std::uint64_t fee{};
    if (!checked_multiply(fill_quantity,
                          configuration_.paper_model.fee_per_unit_currency_nanos,
                          fee)) {
      return GatewayReason::arithmetic_overflow;
    }
    order.queue_ahead_units = 0U;
    order.cumulative_fill_quantity_units += fill_quantity;
    if (order.cumulative_fill_quantity_units == order.quantity_units) {
      order.state = PaperOrderState::filled;
    }
    const oms::GatewayCommand synthetic_command{
        .command_id = order.source_command_id,
        .kind = oms::GatewayCommandKind::new_order,
        .order_id = order.order_id,
        .external_order_id = order.external_order_id,
        .session_id = configuration_.session_id,
        .account_id = configuration_.account_id,
        .venue_id = configuration_.venue_id,
        .instrument_id = order.instrument_id,
        .configuration_version = configuration_.configuration_version,
        .side = order.side,
        .price_ticks = order.price_ticks,
        .quantity_units = order.quantity_units,
        .authority = {.exchange_session_epoch = configuration_.exchange_session_epoch,
                      .fencing_token = configuration_.initial_fencing_token}};
    auto response = base_event(synthetic_command, GatewayResponseKind::fill,
                               GatewayReason::accepted, next_event_ordinal_);
    response.execution_id =
        execution_id(order.order_id, event.event_hash, next_execution_ordinal_++);
    response.external_order_id = order.external_order_id;
    response.exchange_event_time_ns = event.exchange_event_time_ns;
    response.nic_receive_time_ns = event.nic_receive_time_ns;
    response.price_ticks = fill_price;
    response.quantity_units = fill_quantity;
    response.cumulative_fill_quantity_units = order.cumulative_fill_quantity_units;
    response.remaining_quantity_units =
        order.quantity_units - order.cumulative_fill_quantity_units;
    response.fee_currency_nanos = fee;
    response.queue_ahead_before_units = queue_before;
    response.queue_ahead_after_units = order.queue_ahead_units;
    response.modeled_slippage_ticks = configuration_.paper_model.slippage_ticks;
    response.modeled_impact_ticks = impact;
    response.source_market_event_hash = event.event_hash;
    response.process_monotonic_time_ns = event.process_monotonic_time_ns;
    response.stable_hash = stable_gateway_event_hash(response);
    if (!schedule_event(response, event.process_monotonic_time_ns)) {
      return GatewayReason::event_capacity;
    }
    scheduled_hash = response.stable_hash;
    return GatewayReason::accepted;
  }
  return GatewayReason::accepted;
}
// NOLINTEND(readability-function-cognitive-complexity)

GatewayReason SimulatedGatewayBase::on_market_event(
    const market_data::synthetic::SyntheticEvent& event) noexcept {
  if (!initialized_.load(std::memory_order_acquire) ||
      stopped_.load(std::memory_order_acquire)) {
    return GatewayReason::shutdown;
  }
  if (!market_data::synthetic::valid_event_shape(event) || event.event_hash == 0U ||
      event.event_hash != market_data::synthetic::calculate_event_hash(event)) {
    return GatewayReason::invalid_request;
  }
  if (!try_enter()) {
    return GatewayReason::recovery_required;
  }
  const auto reservation = journal_.try_reserve();
  if (!reservation.valid) {
    ++metrics_.journal_full_count;
    set_unsafe(SessionReason::journal_unavailable, event.process_monotonic_time_ns);
    leave();
    return GatewayReason::journal_capacity;
  }
  const auto market_index = find_market(event);
  auto reason = GatewayReason::accepted;
  if (event.data_quality != market_data::synthetic::DataQuality::valid) {
    reason = GatewayReason::feed_unhealthy;
  } else if (market_index == kNoMarket) {
    reason = GatewayReason::scope_mismatch;
  }
  std::uint64_t scheduled_hash{};
  if (reason == GatewayReason::accepted) {
    settle_due_controls(event.process_monotonic_time_ns);
    update_market(event, market_index);
    reason = process_trade(event, market_index, scheduled_hash);
  }
  std::uint64_t sequence{};
  std::uint64_t record_hash{};
  if (!append_audit(reservation, GatewayAuditKind::market_observation, reason, {}, {},
                    0U, 0U, 0U, saturating_mix(event.event_hash, scheduled_hash),
                    event.process_monotonic_time_ns, sequence, record_hash)) {
    set_unsafe(SessionReason::journal_unavailable, event.process_monotonic_time_ns);
    reason = GatewayReason::journal_capacity;
  }
  update_health();
  leave();
  return reason;
}

GatewayPollStatus SimulatedGatewayBase::poll_event(const std::uint64_t now_ns,
                                                   GatewayEvent& output) noexcept {
  if (stopped_.load(std::memory_order_acquire)) {
    return GatewayPollStatus::stopped;
  }
  if (!initialized_.load(std::memory_order_acquire) || !try_enter()) {
    return GatewayPollStatus::recovering;
  }
  if (session_.check_heartbeat_timeout(now_ns)) {
    ++metrics_.sequence_gaps;
    update_health();
    leave();
    return GatewayPollStatus::recovering;
  }
  settle_due_controls(now_ns);
  const auto index = find_due_event(now_ns);
  if (index == kNoEvent) {
    leave();
    return GatewayPollStatus::empty;
  }
  const auto reservation = journal_.try_reserve();
  if (!reservation.valid) {
    ++metrics_.journal_full_count;
    set_unsafe(SessionReason::journal_unavailable, now_ns);
    leave();
    return GatewayPollStatus::recovering;
  }
  auto event = events_[index].event;
  if (event.kind == GatewayResponseKind::cancel_acknowledgement ||
      event.kind == GatewayResponseKind::replace_acknowledgement ||
      event.kind == GatewayResponseKind::acknowledgement) {
    const auto order_index = find_order(event.order_id);
    if (order_index != kNoOrder) {
      const auto& order = orders_[order_index];
      event.external_order_id = order.external_order_id;
      event.cumulative_fill_quantity_units = order.cumulative_fill_quantity_units;
      event.remaining_quantity_units =
          order.quantity_units - order.cumulative_fill_quantity_units;
      event.price_ticks = order.price_ticks;
      event.quantity_units = order.quantity_units;
    }
  }
  if (event.venue_sequence == 0U) {
    event.venue_sequence = session_.snapshot().expected_inbound_sequence;
  }
  event.process_monotonic_time_ns = events_[index].due_ns;
  event.stable_hash = stable_gateway_event_hash(event);
  const auto sequence_status = session_.observe_inbound(now_ns, event.venue_sequence);
  if (sequence_status == SequenceStatus::gap ||
      sequence_status == SequenceStatus::out_of_order ||
      sequence_status == SequenceStatus::invalid ||
      sequence_status == SequenceStatus::exhausted) {
    ++metrics_.sequence_gaps;
    events_[index] = {};
    --active_event_count_;
    std::uint64_t sequence{};
    std::uint64_t record_hash{};
    (void)append_audit(reservation, GatewayAuditKind::response_emitted,
                       GatewayReason::sequence_gap, event.source_command_id,
                       event.order_id, 0U, 0U, 0U, event.stable_hash, now_ns, sequence,
                       record_hash);
    update_health();
    leave();
    return GatewayPollStatus::sequence_error;
  }
  if (sequence_status == SequenceStatus::duplicate) {
    events_[index] = {};
    --active_event_count_;
    std::uint64_t sequence{};
    std::uint64_t record_hash{};
    (void)append_audit(reservation, GatewayAuditKind::response_emitted,
                       GatewayReason::duplicate_command, event.source_command_id,
                       event.order_id, 0U, 0U, 0U, event.stable_hash, now_ns, sequence,
                       record_hash);
    leave();
    return GatewayPollStatus::empty;
  }
  if (!valid_gateway_event(event)) {
    set_unsafe(SessionReason::inbound_sequence_gap, now_ns);
    std::uint64_t sequence{};
    std::uint64_t record_hash{};
    (void)append_audit(reservation, GatewayAuditKind::response_emitted,
                       GatewayReason::decode_failure, event.source_command_id,
                       event.order_id, 0U, 0U, 0U, event.stable_hash, now_ns, sequence,
                       record_hash);
    leave();
    return GatewayPollStatus::sequence_error;
  }
  std::uint64_t journal_sequence{};
  std::uint64_t record_hash{};
  if (!append_audit(reservation, GatewayAuditKind::response_emitted,
                    GatewayReason::accepted, event.source_command_id, event.order_id,
                    0U, 0U, 0U, event.stable_hash, now_ns, journal_sequence,
                    record_hash)) {
    set_unsafe(SessionReason::journal_unavailable, now_ns);
    leave();
    return GatewayPollStatus::recovering;
  }
  events_[index] = {};
  --active_event_count_;
  output = event;
  switch (event.kind) {
  case GatewayResponseKind::acknowledgement:
  case GatewayResponseKind::cancel_acknowledgement:
  case GatewayResponseKind::replace_acknowledgement:
    ++metrics_.acknowledgements;
    break;
  case GatewayResponseKind::rejection:
  case GatewayResponseKind::cancel_rejection:
  case GatewayResponseKind::replace_rejection:
    ++metrics_.rejects;
    break;
  case GatewayResponseKind::fill:
    ++metrics_.fills;
    metrics_.partial_fills += event.remaining_quantity_units != 0U ? 1U : 0U;
    break;
  case GatewayResponseKind::heartbeat:
    ++metrics_.heartbeats;
    break;
  case GatewayResponseKind::session_status:
    break;
  }
  update_health();
  leave();
  return GatewayPollStatus::event;
}

bool SimulatedGatewayBase::heartbeat(const std::uint64_t now_ns) noexcept {
  if (!initialized_.load(std::memory_order_acquire) ||
      stopped_.load(std::memory_order_acquire) || !try_enter()) {
    return false;
  }
  if (session_.check_heartbeat_timeout(now_ns)) {
    update_health();
    leave();
    return false;
  }
  if (!session_.heartbeat_due(now_ns)) {
    leave();
    return true;
  }
  std::uint64_t outbound_sequence{};
  if (session_.claim_outbound(now_ns, outbound_sequence) != SequenceStatus::in_order) {
    update_health();
    leave();
    return false;
  }
  const oms::GatewayCommand command{
      .command_id =
          common::GlobalEventId{
              nonzero(configuration_.session_id.high() ^ outbound_sequence),
              nonzero(configuration_.session_id.low() ^ 0x4845415254424541ULL)},
      .session_id = configuration_.session_id,
      .account_id = configuration_.account_id,
      .venue_id = configuration_.venue_id,
      .configuration_version = configuration_.configuration_version,
      .authority = {.exchange_session_epoch = configuration_.exchange_session_epoch,
                    .fencing_token = configuration_.initial_fencing_token}};
  auto event = base_event(command, GatewayResponseKind::heartbeat,
                          GatewayReason::accepted, next_event_ordinal_);
  const bool scheduled = schedule_event(event, now_ns);
  leave();
  return scheduled;
}

bool SimulatedGatewayBase::begin_recovery(const std::uint64_t now_ns) noexcept {
  if (!initialized_.load(std::memory_order_acquire) || !try_enter()) {
    return false;
  }
  const auto reservation = journal_.try_reserve();
  if (!reservation.valid) {
    ++metrics_.journal_full_count;
    set_unsafe(SessionReason::journal_unavailable, now_ns);
    leave();
    return false;
  }
  const bool transitioned =
      session_.begin_recovery(now_ns, SessionReason::recovery_requested);
  if (transitioned) {
    for (auto& order : orders_) {
      if (order.occupied && !terminal(order.state)) {
        order.state = PaperOrderState::unknown_recovery;
      }
    }
    for (auto& event : events_) {
      event = {};
    }
    active_event_count_ = 0U;
  }
  std::uint64_t sequence{};
  std::uint64_t record_hash{};
  const bool journaled =
      append_audit(reservation, GatewayAuditKind::recovery,
                   transitioned ? GatewayReason::recovery_required
                                : GatewayReason::session_not_active,
                   {}, {}, 0U, 0U, 0U, 0U, now_ns, sequence, record_hash);
  if (!journaled) {
    set_unsafe(SessionReason::journal_unavailable, now_ns);
  }
  update_health();
  leave();
  return transitioned && journaled;
}

bool SimulatedGatewayBase::recovery_scope_matches(
    const GatewayRecoverySnapshot& snapshot) const noexcept {
  if (snapshot.session_id != configuration_.session_id ||
      snapshot.account_id != configuration_.account_id ||
      snapshot.venue_id != configuration_.venue_id ||
      snapshot.configuration_version != configuration_.configuration_version ||
      snapshot.configuration_hash != configuration_.stable_hash ||
      snapshot.exchange_session_epoch != configuration_.exchange_session_epoch) {
    return false;
  }
  return std::ranges::all_of(snapshot.orders, [this](const auto& order) {
    return !order.occupied || find_market(order.instrument_id) != kNoMarket;
  });
}

bool SimulatedGatewayBase::recovery_is_ambiguous(
    const GatewayRecoverySnapshot& snapshot) noexcept {
  return std::ranges::any_of(snapshot.orders, [](const PaperOrderSnapshot& order) {
    return order.occupied && order.state == PaperOrderState::unknown_recovery;
  });
}

bool SimulatedGatewayBase::rebuild_recovery_orders(
    const GatewayRecoverySnapshot& snapshot) noexcept {
  orders_.fill({});
  // Canonical insertion mutates bounded lookup slots in deterministic order.
  // NOLINTBEGIN(readability-use-anyofallof)
  for (const auto& order : snapshot.orders) {
    if (!order.occupied) {
      continue;
    }
    const auto index = find_free_order(order.order_id);
    if (index == kNoOrder) {
      return false;
    }
    orders_[index] = order;
  }
  // NOLINTEND(readability-use-anyofallof)
  return true;
}

RecoveryStatus SimulatedGatewayBase::recover(const GatewayRecoverySnapshot& snapshot,
                                             const std::uint64_t now_ns) noexcept {
  if (!initialized_.load(std::memory_order_acquire) || !try_enter()) {
    return RecoveryStatus::busy;
  }
  if (!valid_recovery_snapshot(snapshot)) {
    leave();
    return RecoveryStatus::invalid_snapshot;
  }
  if (!recovery_scope_matches(snapshot)) {
    leave();
    return RecoveryStatus::configuration_mismatch;
  }
  const auto reservation = journal_.try_reserve();
  if (!reservation.valid) {
    ++metrics_.journal_full_count;
    set_unsafe(SessionReason::journal_unavailable, now_ns);
    leave();
    return RecoveryStatus::busy;
  }
  auto ambiguous = recovery_is_ambiguous(snapshot);
  if (!ambiguous) {
    ambiguous = !rebuild_recovery_orders(snapshot);
  }
  const bool completed =
      !ambiguous && session_.complete_recovery(now_ns, snapshot.next_inbound_sequence);
  std::uint64_t sequence{};
  std::uint64_t record_hash{};
  const auto reason =
      completed ? GatewayReason::accepted : GatewayReason::recovery_ambiguous;
  if (!append_audit(reservation, GatewayAuditKind::recovery, reason, {}, {}, 0U, 0U, 0U,
                    snapshot.stable_hash, now_ns, sequence, record_hash)) {
    set_unsafe(SessionReason::journal_unavailable, now_ns);
    leave();
    return RecoveryStatus::busy;
  }
  if (completed) {
    ++metrics_.recovery_count;
  }
  update_health();
  leave();
  return completed ? RecoveryStatus::completed : RecoveryStatus::ambiguous;
}

GatewayRecoverySnapshot SimulatedGatewayBase::recovery_snapshot() const noexcept {
  GatewayRecoverySnapshot result{
      .session_id = configuration_.session_id,
      .account_id = configuration_.account_id,
      .venue_id = configuration_.venue_id,
      .configuration_version = configuration_.configuration_version,
      .configuration_hash = configuration_.stable_hash,
      .exchange_session_epoch = configuration_.exchange_session_epoch,
      .next_inbound_sequence = session_.snapshot().expected_inbound_sequence,
      .orders = orders_};
  for (const auto& order : orders_) {
    result.order_count += order.occupied ? 1U : 0U;
  }
  result.stable_hash = stable_recovery_snapshot_hash(result);
  return result;
}

bool SimulatedGatewayBase::inject_synthetic_response(
    const GatewayEvent& event, const std::uint64_t due_ns) noexcept {
  if (!initialized_.load(std::memory_order_acquire) || event.mode != mode_ ||
      !valid_gateway_event(event) || event.session_id != configuration_.session_id ||
      event.account_id != configuration_.account_id ||
      event.venue_id != configuration_.venue_id ||
      event.configuration_version != configuration_.configuration_version ||
      event.authority.exchange_session_epoch != configuration_.exchange_session_epoch ||
      event.authority.fencing_token != configuration_.initial_fencing_token ||
      ((event.kind != GatewayResponseKind::heartbeat &&
        event.kind != GatewayResponseKind::session_status) &&
       find_market(event.instrument_id) == kNoMarket) ||
      !try_enter()) {
    return false;
  }
  const bool scheduled = schedule_event(event, due_ns);
  leave();
  return scheduled;
}

GatewayMode SimulatedGatewayBase::mode() const noexcept { return mode_; }

GatewayHealth SimulatedGatewayBase::health() const noexcept { return health_; }

bool SimulatedGatewayBase::ready() const noexcept {
  return initialized_.load(std::memory_order_acquire) &&
         !stopped_.load(std::memory_order_acquire) && mode_ != GatewayMode::halted &&
         health_ == GatewayHealth::healthy && session_.ready();
}

SessionSnapshot SimulatedGatewayBase::session_snapshot() const noexcept {
  return session_.snapshot();
}

GatewayMetrics SimulatedGatewayBase::metrics() const noexcept { return metrics_; }

const common::BuildInfo& SimulatedGatewayBase::build_info() const noexcept {
  return common::current_build_info();
}

std::uint64_t SimulatedGatewayBase::configuration_hash() const noexcept {
  return configuration_.stable_hash;
}

void SimulatedGatewayBase::shutdown(const std::uint64_t now_ns) noexcept {
  if (stopped_.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  if (!try_enter()) {
    health_ = GatewayHealth::unsafe;
    return;
  }
  session_.shutdown(now_ns);
  if (!append_simple_audit(GatewayAuditKind::shutdown, GatewayReason::shutdown,
                           now_ns)) {
    ++metrics_.journal_full_count;
  }
  update_health();
  leave();
}

} // namespace aegis::execution
