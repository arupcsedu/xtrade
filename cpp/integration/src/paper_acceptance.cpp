#include "aegis/integration/paper_acceptance.hpp"

#include "aegis/common/sha256.hpp"
#include "aegis/ensemble/gate.hpp"
#include "aegis/event_bus/process_epoch.hpp"
#include "aegis/execution/router.hpp"
#include "aegis/execution/simulated_gateway.hpp"
#include "aegis/features/feature_engine.hpp"
#include "aegis/journal/async_journal.hpp"
#include "aegis/market_data/feed/feed_handler.hpp"
#include "aegis/market_data/feed/synthetic_receiver.hpp"
#include "aegis/market_data/synthetic/hash.hpp"
#include "aegis/models/validator.hpp"
#include "aegis/observability/decision_explanation.hpp"
#include "aegis/observability/telemetry.hpp"
#include "aegis/oms/service.hpp"
#include "aegis/order_book/order_book.hpp"
#include "aegis/order_book/synthetic_adapter.hpp"
#include "aegis/risk/engine.hpp"
#include "aegis/risk/portfolio_service.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

namespace aegis::integration {
namespace {

namespace common = aegis::common;
namespace ensemble = aegis::ensemble;
namespace event_bus = aegis::event_bus;
namespace execution = aegis::execution;
namespace features = aegis::features;
namespace feed = aegis::market_data::feed;
namespace market_state = aegis::market_state;
namespace models = aegis::models;
namespace observability = aegis::observability;
namespace journal = aegis::journal;
namespace oms = aegis::oms;
namespace order_book = aegis::order_book;
namespace portfolio = aegis::risk::portfolio;
namespace risk = aegis::risk;
namespace synthetic = aegis::market_data::synthetic;
namespace time = aegis::time;

inline constexpr std::uint64_t kFnvOffset = 14'695'981'039'346'656'037ULL;
inline constexpr std::uint64_t kFnvPrime = 1'099'511'628'211ULL;
inline constexpr std::uint64_t kBaseNow = 1'000'000U;
inline constexpr std::int64_t kExchangeBase = 1'800'000'000'000'000'000LL;
inline constexpr std::uint64_t kWallBase = 1'800'000'000'000'000'000ULL;
inline constexpr common::SessionId kSession{1U, 1U};
inline constexpr common::AccountId kAccount{2U, 2U};
inline constexpr common::StrategyId kStrategy{3U, 3U};
inline constexpr common::VenueId kVenue{4U, 4U};
inline constexpr common::InstrumentId kInstrument{5U, 5U};
inline constexpr common::ChannelId kChannel{7U, 7U};
inline constexpr common::ConfigurationVersion kConfiguration{6U, 6U};
inline constexpr common::RiskSnapshotId kRiskSnapshot{8U, 8U};
inline constexpr oms::LeaderAuthority kAuthority{.exchange_session_epoch = 77U,
                                                 .fencing_token = 88U};
inline constexpr std::uint32_t kAllRouterPolicies =
    (std::uint32_t{1U} << static_cast<std::uint8_t>(
         execution::ExecutionPolicy::auction_participation)) -
    1U;

void mix(std::uint64_t& hash, const std::uint64_t value) noexcept {
  for (unsigned shift = 0U; shift < 64U; shift += 8U) {
    hash ^= (value >> shift) & 0xFFU;
    hash *= kFnvPrime;
  }
}

[[nodiscard]] std::uint64_t nonzero_hash(std::uint64_t hash) noexcept {
  return hash == 0U ? 1U : hash;
}

struct DigestMaterial {
  std::uint64_t seed{};
  std::uint64_t ordinal{};
};

[[nodiscard]] common::Sha256Digest digest(const DigestMaterial material) noexcept {
  std::array<std::uint8_t, 16U> bytes{};
  for (std::size_t index = 0U; index < 8U; ++index) {
    const auto shift = static_cast<unsigned>(index * 8U);
    bytes[index] = static_cast<std::uint8_t>(material.seed >> shift);
    bytes[index + 8U] = static_cast<std::uint8_t>(material.ordinal >> shift);
  }
  return common::sha256(std::span<const std::uint8_t>{bytes});
}

class TemporaryAuditDirectory final {
public:
  TemporaryAuditDirectory() {
    auto pattern =
        std::filesystem::temp_directory_path() / "aegis-mx-paper-audit.XXXXXX";
    auto value = pattern.string();
    value.push_back('\0');
    if (auto* created = ::mkdtemp(value.data()); created != nullptr) {
      path_ = created;
    }
  }
  ~TemporaryAuditDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
  std::filesystem::path path_;
};

[[nodiscard]] journal::WriterConfig
audit_journal_configuration(const std::filesystem::path& directory,
                            const std::uint64_t seed) noexcept {
  return {.directory = directory,
          .maximum_segment_bytes = std::uint64_t{4U} * 1024U * 1024U,
          .maximum_records_per_segment = 10'000U,
          .index_stride_records = 16U,
          .sync_policy = journal::SyncPolicy::every_record,
          .periodic_sync_records = 1U,
          .configuration_sha256 = digest({.seed = seed, .ordinal = 0xA0D17U}),
          .build_sha256 = digest({.seed = seed, .ordinal = 0xB017DU}),
          .writer_instance_id = common::GlobalEventId{seed, 0xA11D17U}};
}

[[nodiscard]] std::size_t scenario_index(const PaperScenario scenario) noexcept {
  return static_cast<std::size_t>(scenario) - 1U;
}

[[nodiscard]] bool is_restricted_scenario(const PaperScenario scenario) noexcept {
  return scenario == PaperScenario::false_rumor_correction ||
         scenario == PaperScenario::feed_gap ||
         scenario == PaperScenario::clock_degradation ||
         scenario == PaperScenario::model_timeout ||
         scenario == PaperScenario::gateway_disconnect ||
         scenario == PaperScenario::split_brain_attempt;
}

[[nodiscard]] bool scenario_expects_fill(const PaperScenario scenario) noexcept {
  return scenario != PaperScenario::false_rumor_correction &&
         scenario != PaperScenario::feed_gap &&
         scenario != PaperScenario::clock_degradation &&
         scenario != PaperScenario::model_timeout &&
         scenario != PaperScenario::gateway_disconnect &&
         scenario != PaperScenario::split_brain_attempt;
}

[[nodiscard]] bool scenario_is_negative(const PaperScenario scenario) noexcept {
  return scenario == PaperScenario::breaking_negative_news;
}

[[nodiscard]] ensemble::EventState
scenario_event_state(const PaperScenario scenario) noexcept {
  switch (scenario) {
  case PaperScenario::earnings_release:
  case PaperScenario::cpi_release:
    return ensemble::EventState::price_discovery;
  case PaperScenario::breaking_negative_news:
  case PaperScenario::false_rumor_correction:
    return ensemble::EventState::breaking_news;
  case PaperScenario::market_close:
  case PaperScenario::index_rebalance:
    return ensemble::EventState::scheduled;
  case PaperScenario::ordinary_midday:
  case PaperScenario::market_open:
  case PaperScenario::hidden_liquidity_replenishment:
  case PaperScenario::trading_halt_reopening:
  case PaperScenario::feed_gap:
  case PaperScenario::clock_degradation:
  case PaperScenario::model_timeout:
  case PaperScenario::risk_service_restart:
  case PaperScenario::gateway_disconnect:
  case PaperScenario::split_brain_attempt:
    return ensemble::EventState::none;
  }
  return ensemble::EventState::none;
}

class RecordingTransitionJournal final {
public:
  [[nodiscard]] static market_state::JournalAppendStatus
  append(void* context,
         const market_state::MarketStateTransition& transition) noexcept {
    auto& journal = *static_cast<RecordingTransitionJournal*>(context);
    if (journal.size_ == journal.records_.size()) {
      return market_state::JournalAppendStatus::full;
    }
    journal.records_[journal.size_++] = transition;
    return market_state::JournalAppendStatus::accepted;
  }

  [[nodiscard]] market_state::TransitionJournalSink sink() noexcept {
    return {this, &RecordingTransitionJournal::append};
  }

  [[nodiscard]] std::uint64_t stable_hash() const noexcept {
    std::uint64_t result = kFnvOffset;
    for (std::size_t index = 0U; index < size_; ++index) {
      mix(result, records_[index].record_hash);
    }
    return nonzero_hash(result);
  }

private:
  std::array<market_state::MarketStateTransition, 64U> records_{};
  std::size_t size_{};
};

class AcceptingRawJournal final : public feed::RawPacketJournal {
public:
  [[nodiscard]] feed::PublishStatus
  enqueue(const feed::RawPacket& packet) noexcept override {
    mix(hash_, packet.source_sequence_hint);
    mix(hash_, packet.byte_count);
    ++count_;
    return feed::PublishStatus::accepted;
  }

  [[nodiscard]] std::uint64_t hash() const noexcept { return nonzero_hash(hash_); }

private:
  std::uint64_t hash_{kFnvOffset};
  std::uint64_t count_{};
};

class CapturingHealthPublisher final : public feed::FeedHealthPublisher {
public:
  [[nodiscard]] feed::PublishStatus
  publish(const feed::DataQualityState& state) noexcept override {
    last_ = state;
    ++count_;
    return feed::PublishStatus::accepted;
  }

  [[nodiscard]] const feed::DataQualityState& last() const noexcept { return last_; }

private:
  feed::DataQualityState last_{};
  std::uint64_t count_{};
};

[[nodiscard]] order_book::BookConfig book_configuration() noexcept {
  return {.identity = {.venue_id = kVenue,
                       .instrument_id = kInstrument,
                       .venue_number = 1U,
                       .instrument_number = 1U},
          .session_id = kSession,
          .mode = order_book::BookMode::price_level,
          .order_capacity = 1U,
          .level_capacity = 64U,
          .published_depth = 8U,
          .maximum_sequence = std::numeric_limits<std::uint64_t>::max(),
          .permit_crossed_transition = false,
          .require_contiguous_sequence = true,
          .start_recovering = false};
}

[[nodiscard]] features::FeatureEngineConfig feature_configuration() noexcept {
  features::FeatureEngineConfig result{
      .instrument_id = kInstrument,
      .session_id = kSession,
      .feature_definition_version = kConfiguration,
      .venue_count = 1U,
      .depth_levels = 5U,
      .window_capacity = 128U,
      .minimum_book_events = 2U,
      .minimum_trade_events = 0U,
      .rolling_window_ns = 1'000'000U,
      .stale_after_ns = 100'000'000U,
      .replenishment_horizon_ns = 1'000U,
      .minimum_replenishment_quantity_units = 1U,
      .maximum_quantity_units = 1'000'000U,
      .maximum_absolute_price_ticks = 1'000'000,
      .session_open_exchange_time_ns = kExchangeBase - 1'000'000'000LL,
      .session_close_exchange_time_ns = kExchangeBase + 10'000'000'000LL};
  result.venues[0U] = {.venue_id = kVenue, .venue_number = 1U};
  return result;
}

class PipelinePublisher final : public feed::NormalizedEventPublisher {
public:
  PipelinePublisher(order_book::OrderBook& book, features::FeatureEngine& features,
                    execution::PaperBrokerGateway& gateway) noexcept
      : book_(book), features_(features), gateway_(gateway) {}

  [[nodiscard]] feed::PublishStatus
  publish(const feed::NormalizedEvent& normalized) noexcept override {
    ++normalized_count_;
    mix(event_hash_, normalized.event.event_hash);
    if (gateway_.on_market_event(normalized.event) !=
        execution::GatewayReason::accepted) {
      valid_ = false;
      return feed::PublishStatus::stopped;
    }
    const auto applied = order_book::apply_synthetic(book_, kSession, normalized.event);
    if (!applied.accepted()) {
      valid_ = false;
      return feed::PublishStatus::stopped;
    }
    if (normalized.event.type == synthetic::NativeMessageType::price_level) {
      order_book::DepthSnapshot depth{};
      if (book_.depth(depth, 5U) != order_book::ApplyError::none) {
        valid_ = false;
        return feed::PublishStatus::stopped;
      }
      const features::BookFeatureEvent feature{
          .global_event_id = {50U, normalized.event.global_ordinal},
          .session_id = kSession,
          .instrument_id = kInstrument,
          .venue_id = kVenue,
          .venue_number = normalized.event.venue_number,
          .global_ordinal = normalized.event.global_ordinal,
          .exchange_event_time_ns = normalized.event.exchange_event_time_ns,
          .process_monotonic_time_ns = normalized.event.process_monotonic_time_ns,
          .operation = order_book::BookOperation::set_level,
          .side = normalized.event.side,
          .affected_price_ticks = normalized.event.price_ticks,
          .affected_quantity_units = normalized.event.level_quantity_units,
          .depth = depth,
          .data_quality = normalized.event.data_quality,
          .top_changed = applied.top_changed,
          .hidden_replenishment = (normalized.event.message_flags &
                                   synthetic::kMessageFlagHiddenReplenishment) != 0U};
      if (!features_.on_book(feature).accepted()) {
        valid_ = false;
        return feed::PublishStatus::stopped;
      }
    } else if (normalized.event.type == synthetic::NativeMessageType::trade) {
      const features::TradeFeatureEvent feature{
          .global_event_id = {51U, normalized.event.global_ordinal},
          .session_id = kSession,
          .instrument_id = kInstrument,
          .venue_id = kVenue,
          .venue_number = normalized.event.venue_number,
          .global_ordinal = normalized.event.global_ordinal,
          .exchange_event_time_ns = normalized.event.exchange_event_time_ns,
          .process_monotonic_time_ns = normalized.event.process_monotonic_time_ns,
          .aggressor_side = normalized.event.side,
          .price_ticks = normalized.event.price_ticks,
          .quantity_units = normalized.event.quantity_units,
          .data_quality = normalized.event.data_quality};
      if (!features_.on_trade(feature).accepted()) {
        valid_ = false;
        return feed::PublishStatus::stopped;
      }
    }
    return feed::PublishStatus::accepted;
  }

  [[nodiscard]] feed::PublishStatus
  publish_snapshot(const feed::RecoverySnapshot& snapshot) noexcept override {
    mix(event_hash_, snapshot.book_hash);
    ++snapshot_count_;
    return feed::PublishStatus::accepted;
  }

  [[nodiscard]] std::uint64_t normalized_count() const noexcept {
    return normalized_count_;
  }
  [[nodiscard]] std::uint64_t event_hash() const noexcept {
    return nonzero_hash(event_hash_);
  }
  [[nodiscard]] bool valid() const noexcept { return valid_; }

private:
  order_book::OrderBook& book_;
  features::FeatureEngine& features_;
  execution::PaperBrokerGateway& gateway_;
  std::uint64_t normalized_count_{};
  std::uint64_t snapshot_count_{};
  std::uint64_t event_hash_{kFnvOffset};
  bool valid_{true};
};

[[nodiscard]] market_state::MarketStateConfig market_state_configuration() noexcept {
  return {.minimum_dwell_ns = 10U,
          .recovery_stabilization_ns = 20U,
          .reopening_stabilization_ns = 30U,
          .scheduled_event_lead_ns = 100U,
          .volatility_spike_threshold_ppm = 200'000U,
          .spread_spike_threshold_ticks = 10U,
          .minimum_aggregate_depth_units = 10U,
          .model_ood_threshold_ppm = 800'000U,
          .model_disagreement_threshold_ppm = 700'000U};
}

[[nodiscard]] market_state::MarketStateInput
healthy_market_input(const std::uint64_t sequence, const std::uint64_t now,
                     const std::uint64_t wall) noexcept {
  return {.input_sequence = sequence,
          .process_monotonic_time_ns = now,
          .wall_clock_utc_ns = wall,
          .official_trading_status = market_state::OfficialTradingStatus::open,
          .feed_health = market_state::FeedHealth::healthy,
          .book_validity = market_state::BookValidity::valid,
          .clock_quality = market_state::ClockQuality::healthy,
          .news_event_state = market_state::NewsEventState::none,
          .earnings_calendar = {},
          .macro_calendar = {},
          .realized_volatility_ppm = 10'000U,
          .spread_ticks = 2U,
          .aggregate_depth_units = 40U,
          .model_ood_ppm = 10'000U,
          .model_disagreement_ppm = 10'000U,
          .operator_controls = {}};
}

[[nodiscard]] bool reach_normal(market_state::MarketStateController& controller,
                                std::uint64_t& sequence, std::uint64_t& now,
                                std::uint64_t& wall) noexcept {
  const auto stabilization =
      std::max(market_state_configuration().recovery_stabilization_ns,
               market_state_configuration().reopening_stabilization_ns) +
      1U;
  for (std::uint32_t attempt = 0U; attempt < 4U; ++attempt) {
    now += stabilization;
    wall += stabilization;
    const auto input = healthy_market_input(++sequence, now, wall);
    const auto result = controller.evaluate(input);
    if (result.status == market_state::EvaluationStatus::journal_rejected) {
      return false;
    }
    if (result.snapshot.state == market_state::MarketState::normal) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] execution::GatewayConfiguration
gateway_configuration(const std::uint64_t seed) noexcept {
  execution::GatewayConfiguration result{
      .session_id = kSession,
      .account_id = kAccount,
      .venue_id = kVenue,
      .configuration_version = kConfiguration,
      .startup_mode = execution::GatewayMode::paper,
      .exchange_session_epoch = kAuthority.exchange_session_epoch,
      .initial_fencing_token = kAuthority.fencing_token,
      .heartbeat_interval_ns = 100'000U,
      .heartbeat_timeout_ns = 500'000U,
      .maximum_clock_age_ns = 100'000U,
      .command_rate_window_ns = 1'000U,
      .maximum_new_orders_per_window = 100U,
      .maximum_cancels_per_window = 100U,
      .maximum_replaces_per_window = 100U,
      .mapping_count = 1U,
      .paper_model = {.deterministic_seed = seed,
                      .acknowledgement_latency_ns = 100U,
                      .cancel_latency_ns = 200U,
                      .replace_latency_ns = 200U,
                      .initial_queue_ahead_units = 2U,
                      .maximum_fill_chunk_units = 10U,
                      .fee_per_unit_currency_nanos = 5U,
                      .slippage_ticks = 1U,
                      .impact_ticks_per_million_units = 1U,
                      .reject_every_nth_new_order = 0U,
                      .allow_auction_orders = true}};
  result.mappings[0U] = {.venue_id = kVenue,
                         .instrument_id = kInstrument,
                         .synthetic_venue_number = 1U,
                         .synthetic_instrument_number = 1U};
  result.stable_hash = execution::stable_gateway_configuration_hash(result);
  return result;
}

[[nodiscard]] oms::OmsConfiguration oms_configuration() noexcept {
  oms::OmsConfiguration result{.session_id = kSession,
                               .account_id = kAccount,
                               .configuration_version = kConfiguration,
                               .trading_mode = risk::TradingMode::paper,
                               .exchange_session_epoch =
                                   kAuthority.exchange_session_epoch,
                               .initial_fencing_token = kAuthority.fencing_token,
                               .maximum_order_age_ns = 1'000'000U,
                               .require_drop_copy = false};
  result.stable_hash = oms::stable_configuration_hash(result);
  return result;
}

[[nodiscard]] portfolio::PortfolioConfiguration portfolio_configuration() noexcept {
  portfolio::PortfolioConfiguration result;
  result.configuration_version = kConfiguration;
  result.session_id = kSession;
  result.accounts[0U] = kAccount;
  result.strategies[0U] = kStrategy;
  result.instruments[0U] = {.instrument_id = kInstrument,
                            .tick_value_currency_nanos = 10U,
                            .sector_index = 0U,
                            .beta_ppm = 1'000'000,
                            .liquidity_weight_ppm = 1'000'000U,
                            .event_weight_ppm = 1'000'000U,
                            .options_gamma_stress_currency_nanos = 0};
  result.account_count = 1U;
  result.strategy_count = 1U;
  result.instrument_count = 1U;
  result.sector_count = 1U;
  result.require_drop_copy_confirmation = false;
  result.stable_hash = portfolio::stable_configuration_hash(result);
  return result;
}

[[nodiscard]] risk::RiskLimitSnapshot risk_limits() noexcept {
  risk::RiskLimitSnapshot result{
      .risk_snapshot_id = kRiskSnapshot,
      .configuration_version = kConfiguration,
      .policy_version = common::ConfigurationVersion{9U, 9U},
      .session_id = kSession,
      .account_id = kAccount,
      .revision = 1U,
      .authority_epoch = 10U,
      .published_process_monotonic_time_ns = kBaseNow - 1'000U,
      .valid_until_process_monotonic_time_ns = kBaseNow + 10'000'000U,
      .approval_ttl_ns = 100'000U,
      .maximum_position_age_ns = 100'000U,
      .maximum_market_state_age_ns = 100'000U,
      .maximum_clock_state_age_ns = 100'000U,
      .maximum_feed_book_age_ns = 100'000U,
      .maximum_configuration_age_ns = 1'000'000U,
      .order_rate_window_ns = 1'000U,
      .cancel_rate_window_ns = 1'000U,
      .maximum_orders_per_window = 100U,
      .maximum_cancels_per_window = 100U,
      .allowed_market_state_mask = 0U,
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
  for (const auto state :
       {market_state::MarketState::normal, market_state::MarketState::scheduled_event,
        market_state::MarketState::breaking_news,
        market_state::MarketState::event_price_discovery,
        market_state::MarketState::volatility_spike}) {
    result.allowed_market_state_mask |= static_cast<std::uint16_t>(
        std::uint16_t{1U} << static_cast<std::uint8_t>(state));
  }
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

[[nodiscard]] execution::RouterConfiguration router_configuration() noexcept {
  execution::RouterConfiguration result{
      .configuration_version = kConfiguration,
      .maximum_quote_age_ns = 100'000U,
      .maximum_route_latency_ns = 100'000U,
      .queue_deterioration_threshold_units = 5U,
      .uncertainty_penalty_currency_nanos_per_unit = 2U,
      .maximum_reject_rate_ppm = 200'000U,
      .minimum_fill_probability_ppm = 100'000U,
      .maximum_direct_consolidated_divergence_ticks = 0U,
      .maximum_route_attempts = 3U,
      .venue_count = 1U};
  result.venues[0U] = {.venue_id = kVenue,
                       .allowed_policy_mask = kAllRouterPolicies,
                       .maximum_concentration_ppm = execution::kRouterPartsPerMillion,
                       .maximum_child_quantity_units = 1'000U,
                       .tie_break_rank = 1U,
                       .enabled = true,
                       .regulatory_authorized = true,
                       .require_self_trade_clear = true,
                       .allow_auction = true};
  result.stable_hash = execution::stable_router_configuration_hash(result);
  return result;
}

[[nodiscard]] ensemble::EnsembleConfig ensemble_configuration() noexcept {
  ensemble::EnsembleConfig result{.ensemble_model_id = common::ModelId{60U, 1U},
                                  .ensemble_model_version =
                                      common::ModelVersion{61U, 1U},
                                  .configuration_version = kConfiguration,
                                  .gate_kind = ensemble::GateKind::rule_based,
                                  .maximum_expert_weight_ppm = 600'000U,
                                  .ood_reject_threshold_ppm = 800'000U,
                                  .minimum_calibration_score_ppm = 500'000U,
                                  .minimum_data_quality_score_ppm = 500'000U,
                                  .maximum_forecast_age_ns = 100'000U,
                                  .maximum_market_state_age_ns = 100'000U,
                                  .degraded_feed_multiplier_ppm = 500'000U,
                                  .degraded_calibration_multiplier_ppm = 500'000U,
                                  .timeseries_breaking_news_multiplier_ppm = 100'000U,
                                  .uncertainty_penalty_multiplier_ppm = 1'000'000U,
                                  .calibration_uncertainty_multiplier_ppm = 100'000U,
                                  .ood_uncertainty_multiplier_ppm = 100'000U,
                                  .data_quality_uncertainty_multiplier_ppm = 100'000U,
                                  .safety_margin_ppm = 500U,
                                  .linear = {},
                                  .learned = {},
                                  .stable_hash = 0U};
  constexpr auto market_mask = static_cast<std::uint16_t>(
      (std::uint32_t{1U} << ensemble::kMarketStateCount) - 1U);
  constexpr auto event_mask =
      static_cast<std::uint8_t>((std::uint32_t{1U} << ensemble::kEventStateCount) - 1U);
  for (std::size_t index = 0U; index < result.role_policies.size(); ++index) {
    result.role_policies[index] = {.role =
                                       static_cast<ensemble::ExpertRole>(index + 1U),
                                   .allowed_market_state_mask = market_mask,
                                   .allowed_event_state_mask = event_mask,
                                   .base_multiplier_ppm = ensemble::kWeightScale};
  }
  result.stable_hash = ensemble::stable_ensemble_config_hash(result);
  return result;
}

[[nodiscard]] time::ClockQualitySnapshot clock_snapshot(const std::uint64_t now,
                                                        const bool degraded) noexcept {
  time::ClockQualitySnapshot result{
      .state = degraded ? time::ClockQualityState::degraded
                        : time::ClockQualityState::healthy,
      .reason = degraded ? time::ClockQualityReason::offset_degraded
                         : time::ClockQualityReason::within_thresholds,
      .operation_mode = degraded ? time::ClockOperationMode::reduce_only
                                 : time::ClockOperationMode::normal,
      .observed_at = time::MonotonicTimeNs{now - 1U},
      .ptp_offset = time::DurationNs{degraded ? 100'000 : 10},
      .drift_ppb = degraded ? 10'000 : 1,
      .source_id = time::ClockSourceId{12U, 12U},
      .last_synchronization_time = time::MonotonicTimeNs{now - 2U},
      .synchronization_age = time::DurationNs{2},
      .hardware_timestamp_available = true,
      .transition_count = 1U,
      .observation_count = 2U};
  return result;
}

[[nodiscard]] synthetic::SyntheticEvent
market_event(const std::uint64_t sequence, const std::uint64_t now,
             const synthetic::NativeMessageType type,
             const synthetic::Side side = synthetic::Side::none,
             const std::int64_t price_ticks = 0,
             const std::uint64_t quantity_units = 0U,
             const synthetic::TradingStatus status = synthetic::TradingStatus::none,
             const bool hidden_replenishment = false) noexcept {
  synthetic::SyntheticEvent result{
      .type = type,
      .side = side,
      .action = type == synthetic::NativeMessageType::price_level
                    ? synthetic::BookAction::change
                    : synthetic::BookAction::none,
      .status = status,
      .data_quality = synthetic::DataQuality::valid,
      .message_flags = static_cast<std::uint16_t>(
          (type == synthetic::NativeMessageType::price_level
               ? synthetic::kMessageFlagPriceLevel
               : 0U) |
          (hidden_replenishment ? synthetic::kMessageFlagHiddenReplenishment : 0U)),
      .venue_number = 1U,
      .channel_number = 7U,
      .instrument_number = 1U,
      .channel_sequence = sequence,
      .global_ordinal = sequence,
      .exchange_event_time_ns = kExchangeBase + static_cast<std::int64_t>(sequence),
      .nic_receive_time_ns = kExchangeBase + 100 + static_cast<std::int64_t>(sequence),
      .process_monotonic_time_ns = now,
      .price_ticks = price_ticks,
      .quantity_units = quantity_units,
      .level_quantity_units =
          type == synthetic::NativeMessageType::price_level ? quantity_units : 0U,
      .order_count = type == synthetic::NativeMessageType::price_level ? 1U : 0U,
      .auxiliary_value = type == synthetic::NativeMessageType::auction_imbalance
                             ? quantity_units
                             : sequence};
  result.event_hash = synthetic::calculate_event_hash(result);
  return result;
}

struct PortfolioMarkInput {
  std::uint64_t ordinal{};
  std::uint64_t now{};
  std::int64_t price_ticks{};
};

[[nodiscard]] portfolio::PortfolioEvent
portfolio_mark(const PortfolioMarkInput input) noexcept {
  portfolio::PortfolioEvent result;
  result.event_id = {100U, input.ordinal};
  result.session_id = kSession;
  result.configuration_version = kConfiguration;
  result.instrument_id = kInstrument;
  result.kind = portfolio::PortfolioEventKind::mark;
  result.price_ticks = input.price_ticks;
  result.process_monotonic_time_ns = input.now;
  result.stable_hash = portfolio::stable_event_hash(result);
  return result;
}

struct PortfolioOrderInput {
  std::uint64_t ordinal{};
  std::uint64_t now{};
  common::OrderId order_id;
  risk::IntentAction side{risk::IntentAction::buy};
  std::int64_t price_ticks{};
  std::uint64_t quantity_units{};
  portfolio::OrderLifecycleState state{portfolio::OrderLifecycleState::accepted};
  std::uint64_t cumulative_fill{};
  std::uint64_t remaining{};
};

[[nodiscard]] portfolio::PortfolioEvent
portfolio_order(const PortfolioOrderInput input) noexcept {
  portfolio::PortfolioEvent result;
  result.event_id = {101U, input.ordinal};
  result.session_id = kSession;
  result.configuration_version = kConfiguration;
  result.order_id = input.order_id;
  result.account_id = kAccount;
  result.strategy_id = kStrategy;
  result.venue_id = kVenue;
  result.instrument_id = kInstrument;
  result.kind = portfolio::PortfolioEventKind::order;
  result.side = input.side == risk::IntentAction::buy ? portfolio::Side::buy
                                                      : portfolio::Side::sell;
  result.order_state = input.state;
  result.price_ticks = input.price_ticks;
  result.quantity_units = input.quantity_units;
  result.cumulative_fill_quantity_units = input.cumulative_fill;
  result.remaining_quantity_units = input.remaining;
  result.process_monotonic_time_ns = input.now;
  result.stable_hash = portfolio::stable_event_hash(result);
  return result;
}

[[nodiscard]] portfolio::PortfolioEvent
portfolio_fill(const execution::GatewayEvent& event,
               const std::uint64_t ordinal) noexcept {
  portfolio::PortfolioEvent result;
  result.event_id = {102U, ordinal};
  result.logical_fill_id = event.execution_id;
  result.session_id = kSession;
  result.configuration_version = kConfiguration;
  result.order_id = event.order_id;
  result.account_id = kAccount;
  result.strategy_id = kStrategy;
  result.venue_id = kVenue;
  result.instrument_id = kInstrument;
  result.kind = portfolio::PortfolioEventKind::fill;
  result.side = event.price_ticks <= 100 ? portfolio::Side::buy : portfolio::Side::sell;
  result.fill_source = portfolio::FillSource::primary;
  result.price_ticks = event.price_ticks;
  result.quantity_units = event.quantity_units;
  result.fee_currency_nanos = static_cast<std::int64_t>(event.fee_currency_nanos);
  result.process_monotonic_time_ns = event.process_monotonic_time_ns;
  result.stable_hash = portfolio::stable_event_hash(result);
  return result;
}

struct ForecastFixtureInput {
  PaperScenario scenario{PaperScenario::ordinary_midday};
  std::uint64_t ordinal{};
  std::uint64_t now{};
  common::FeatureSnapshotId feature_snapshot_id;
  bool transaction_cost{false};
  bool correction_stage{false};
};

struct ForecastProfile {
  std::int64_t expected_return_ppm{};
  std::int64_t return_p10_ppm{};
  std::int64_t return_p90_ppm{};
  std::uint32_t probability_down_ppm{};
  std::uint32_t probability_flat_ppm{};
  std::uint32_t probability_up_ppm{};
  std::uint32_t volatility_ppm{};
  std::uint64_t component_cost_ppm{};
};

[[nodiscard]] ForecastProfile
forecast_profile(const ForecastFixtureInput& input) noexcept {
  if (input.transaction_cost) {
    return {.probability_flat_ppm = 1'000'000U, .component_cost_ppm = 100U};
  }
  if (input.correction_stage) {
    return {.return_p10_ppm = -4'000,
            .return_p90_ppm = 4'000,
            .probability_down_ppm = 150'000U,
            .probability_flat_ppm = 700'000U,
            .probability_up_ppm = 150'000U,
            .volatility_ppm = 5'000U};
  }
  auto expected_return = scenario_is_negative(input.scenario) ? -50'000 : 50'000;
  if (input.scenario == PaperScenario::false_rumor_correction) {
    expected_return = (input.ordinal & 1U) == 0U ? -400'000 : 400'000;
  }
  const auto negative = expected_return < 0;
  return {.expected_return_ppm = expected_return,
          .return_p10_ppm = expected_return - 4'000,
          .return_p90_ppm = expected_return + 4'000,
          .probability_down_ppm = negative ? 700'000U : 150'000U,
          .probability_flat_ppm = 150'000U,
          .probability_up_ppm = negative ? 150'000U : 700'000U,
          .volatility_ppm = 5'000U};
}

[[nodiscard]] models::ModelForecast
specialist_forecast(const ForecastFixtureInput& input) noexcept {
  const auto profile = forecast_profile(input);
  auto expiration = input.now + 10'000U;
  if (input.scenario == PaperScenario::model_timeout && !input.transaction_cost) {
    expiration = input.now - 1U;
  }
  models::ModelForecast result{
      .forecast_id = common::ForecastId{110U, input.ordinal},
      .session_id = kSession,
      .model_id = common::ModelId{111U, input.ordinal},
      .model_version = common::ModelVersion{112U, 1U},
      .instrument_id = kInstrument,
      .feature_snapshot_id = input.feature_snapshot_id,
      .configuration_version = kConfiguration,
      .model_control_generation = 1U,
      .horizon_ns = 1'000'000U,
      .as_of_exchange_event_time_ns = kExchangeBase + 3,
      .production_process_monotonic_time_ns = input.now - 10U,
      .expiration_process_monotonic_time_ns = expiration,
      .prediction = {
          .expected_return_ppm = profile.expected_return_ppm,
          .return_p10_ppm = profile.return_p10_ppm,
          .return_p50_ppm = profile.expected_return_ppm,
          .return_p90_ppm = profile.return_p90_ppm,
          .probability_down_ppm = profile.probability_down_ppm,
          .probability_flat_ppm = profile.probability_flat_ppm,
          .probability_up_ppm = profile.probability_up_ppm,
          .volatility_ppm = profile.volatility_ppm,
          .confidence_ppm = 900'000U,
          .calibration_score_ppm = 900'000U,
          .data_quality_score_ppm = 900'000U,
          .ood_score_ppm = 20'000U,
          .transaction_cost = {.spread_cost_ppm = profile.component_cost_ppm,
                               .slippage_cost_ppm = profile.component_cost_ppm,
                               .market_impact_ppm = profile.component_cost_ppm,
                               .adverse_selection_cost_ppm = profile.component_cost_ppm,
                               .fee_cost_ppm = profile.component_cost_ppm,
                               .present = input.transaction_cost}}};
  result.stable_hash = models::stable_forecast_hash(result);
  return result;
}

[[nodiscard]] ensemble::EnsembleRequest
ensemble_request(const PaperScenario scenario, const std::uint64_t now,
                 const features::FeatureSnapshot& feature_snapshot,
                 const market_state::MarketStateSnapshot& market_snapshot,
                 const market_state::FeedHealth feed_health,
                 std::uint64_t& specialist_hash,
                 const bool correction_stage = false) noexcept {
  ensemble::EnsembleRequest result{
      .forecast_id =
          common::ForecastId{120U, (static_cast<std::uint64_t>(scenario) * 10U) +
                                       (correction_stage ? 2U : 1U)},
      .session_id = kSession,
      .instrument_id = kInstrument,
      .configuration_version = kConfiguration,
      .horizon_ns = 1'000'000U,
      .now_process_monotonic_time_ns = now,
      .valid_until_process_monotonic_time_ns = now + 10'000U,
      .market_state_snapshot = market_snapshot,
      .event_state = correction_stage ? ensemble::EventState::none
                                      : scenario_event_state(scenario),
      .feed_health = feed_health,
      .input_data_quality_ppm =
          feed_health == market_state::FeedHealth::healthy ? 900'000U : 100'000U,
      .expert_count = 4U,
      .transaction_cost_forecast =
          specialist_forecast({.scenario = scenario,
                               .ordinal = correction_stage ? 200U : 100U,
                               .now = now,
                               .feature_snapshot_id = feature_snapshot.snapshot_id,
                               .transaction_cost = true,
                               .correction_stage = correction_stage}),
      .transaction_cost_health = {.generation = 1U,
                                  .state = models::ModelHealthState::healthy},
      .transaction_cost_present = true};
  constexpr std::array<ensemble::ExpertRole, 4U> roles{
      ensemble::ExpertRole::microstructure, ensemble::ExpertRole::timeseries,
      ensemble::ExpertRole::event, ensemble::ExpertRole::macro};
  specialist_hash = kFnvOffset;
  for (std::size_t index = 0U; index < result.expert_count; ++index) {
    const auto ordinal =
        static_cast<std::uint64_t>(index + 1U) + (correction_stage ? 10U : 0U);
    result.experts[index] = {
        .forecast =
            specialist_forecast({.scenario = scenario,
                                 .ordinal = ordinal,
                                 .now = now,
                                 .feature_snapshot_id = feature_snapshot.snapshot_id,
                                 .transaction_cost = false,
                                 .correction_stage = correction_stage}),
        .health = {.generation = 1U, .state = models::ModelHealthState::healthy},
        .role = roles[index],
        .calibration_health = ensemble::CalibrationHealth::healthy};
    mix(specialist_hash, result.experts[index].forecast.stable_hash);
  }
  mix(specialist_hash, result.transaction_cost_forecast.stable_hash);
  specialist_hash = nonzero_hash(specialist_hash);
  return result;
}

[[nodiscard]] execution::RoutingRequest
routing_request(const PaperScenario scenario,
                const ensemble::EnsembleForecast& forecast,
                const features::FeatureSnapshot& feature_snapshot,
                const order_book::TopOfBook& top, const std::uint64_t now) noexcept {
  const auto buy = forecast.expected_return_ppm >= 0;
  const auto policy = scenario == PaperScenario::index_rebalance
                          ? execution::ExecutionPolicy::auction_participation
                          : execution::ExecutionPolicy::passive_join;
  auto limit_price_ticks = buy ? top.bid.price_ticks : top.ask.price_ticks;
  if (policy == execution::ExecutionPolicy::auction_participation) {
    limit_price_ticks = 101;
  }
  execution::ExecutionObjective objective{
      .objective_id = common::IntentId{130U, static_cast<std::uint64_t>(scenario)},
      .session_id = kSession,
      .account_id = kAccount,
      .strategy_id = kStrategy,
      .instrument_id = kInstrument,
      .source_forecast_id = forecast.forecast_id,
      .feature_snapshot_id = feature_snapshot.snapshot_id,
      .configuration_version = kConfiguration,
      .target_order_id = {},
      .side = buy ? risk::IntentAction::buy : risk::IntentAction::sell,
      .policy = policy,
      .limit_price_ticks = limit_price_ticks,
      .total_quantity_units = 10U,
      .filled_quantity_units = 0U,
      .maximum_child_quantity_units = 10U,
      .start_process_monotonic_time_ns = now - 10U,
      .end_process_monotonic_time_ns = now + 10'000U,
      .slice_interval_ns = 100U,
      .alpha_half_life_ns = 10'000U,
      .expected_alpha_microticks = buy ? 2'000'000 : -2'000'000,
      .tick_value_currency_nanos = 10U,
      .participation_rate_ppm =
          policy == execution::ExecutionPolicy::auction_participation ? 100'000U : 0U,
      .has_limit_price = true};
  objective.stable_hash = execution::stable_execution_objective_hash(objective);
  execution::ConsolidatedQuote consolidated{.best_bid_venue_id = kVenue,
                                            .best_ask_venue_id = kVenue,
                                            .best_bid_price_ticks = top.bid.price_ticks,
                                            .best_ask_price_ticks = top.ask.price_ticks,
                                            .observed_process_monotonic_time_ns =
                                                now - 1U,
                                            .valid = true};
  consolidated.stable_hash = execution::stable_consolidated_quote_hash(consolidated);
  execution::VenueObservation observation{
      .venue_id = kVenue,
      .instrument_id = kInstrument,
      .trading_state = policy == execution::ExecutionPolicy::auction_participation
                           ? execution::VenueTradingState::auction
                           : execution::VenueTradingState::open,
      .self_trade_prevention = execution::SelfTradePreventionState::clear,
      .bid_price_ticks = top.bid.price_ticks,
      .ask_price_ticks = top.ask.price_ticks,
      .bid_quantity_units = top.bid.quantity_units,
      .ask_quantity_units = top.ask.quantity_units,
      .estimated_hidden_quantity_units =
          scenario == PaperScenario::hidden_liquidity_replenishment ? 100U : 10U,
      .queue_ahead_quantity_units = 2U,
      .venue_latency_ns = 100U,
      .observed_process_monotonic_time_ns = now - 1U,
      .already_routed_quantity_units = 0U,
      .auction_paired_quantity_units =
          policy == execution::ExecutionPolicy::auction_participation ? 1'000U : 0U,
      .auction_price_ticks =
          policy == execution::ExecutionPolicy::auction_participation ? 101 : 0,
      .maker_fee_currency_nanos_per_unit = -1,
      .taker_fee_currency_nanos_per_unit = 3,
      .adverse_selection_microticks = 100'000,
      .fill_probability_ppm = 900'000U,
      .reject_rate_ppm = 1'000U,
      .quote_valid = true,
      .regulatory_eligible = true};
  observation.stable_hash = execution::stable_venue_observation_hash(observation);
  execution::RoutingRequest result{.objective = objective,
                                   .consolidated_quote = consolidated,
                                   .working_order = {},
                                   .now_process_monotonic_time_ns = now,
                                   .last_slice_process_monotonic_time_ns = 0U,
                                   .market_volume_since_last_slice_units = 100U,
                                   .already_scheduled_quantity_units = 0U,
                                   .target_cumulative_volume_curve_ppm = 500'000U,
                                   .venue_count = 1U,
                                   .visited_venue_count = 0U,
                                   .route_attempt = 0U};
  result.venues[0U] = observation;
  result.stable_hash = execution::stable_routing_request_hash(result);
  return result;
}

[[nodiscard]] risk::RiskEvaluationRequest
risk_request(const execution::RoutingDecision& route,
             const execution::ExecutionObjective& objective,
             const market_state::MarketStateSnapshot& market_snapshot,
             const market_state::FeedHealth feed_health,
             const market_state::BookValidity book_validity, const std::uint64_t now,
             const std::uint64_t seed, const bool clock_degraded,
             const bool split_brain) noexcept {
  risk::RiskIntent intent{
      .intent_id = objective.objective_id,
      .session_id = kSession,
      .account_id = kAccount,
      .strategy_id = kStrategy,
      .venue_id = route.venue_id,
      .instrument_id = kInstrument,
      .source_forecast_id = objective.source_forecast_id,
      .feature_snapshot_id = objective.feature_snapshot_id,
      .target_order_id = route.target_order_id,
      .configuration_version = kConfiguration,
      .action = objective.side,
      .limit_price_ticks = route.price_ticks,
      .quantity_units = route.quantity_units,
      .created_process_monotonic_time_ns = now - 1U,
      .expire_process_monotonic_time_ns = now + 10'000U,
      .canonical_sha256 =
          digest({.seed = seed,
                  .ordinal = static_cast<std::uint64_t>(route.objective_id.low())})};
  intent.stable_hash = risk::stable_risk_intent_hash(intent);
  risk::RiskContext context{
      .now_process_monotonic_time_ns = now,
      .trading_mode = risk::TradingMode::paper,
      .operator_authorized = true,
      .session_authorized = true,
      .operator_authorization_valid_until_ns = now + 100'000U,
      .authority_epoch = split_brain ? 11U : 10U,
      .process_id = 11U,
      .market_state_snapshot = market_snapshot,
      .official_trading_status =
          market_snapshot.state == market_state::MarketState::halted
              ? market_state::OfficialTradingStatus::halted
              : market_state::OfficialTradingStatus::open,
      .feed_health = feed_health,
      .book_validity = book_validity,
      .clock_quality_snapshot = clock_snapshot(now, clock_degraded),
      .feed_book_observed_process_monotonic_time_ns = now - 1U,
      .feed_book_state_hash = nonzero_hash(market_snapshot.stable_hash ^ 0xB00CU),
      .reference_price_ticks = 101,
      .short_sale_locate = risk::PolicyHookResult::allowed,
      .self_trade_prevention = risk::PolicyHookResult::allowed};
  context.stable_hash = risk::stable_risk_context_hash(context);
  return {.global_event_id = common::GlobalEventId{140U, static_cast<std::uint64_t>(
                                                             route.objective_id.low())},
          .intent = intent,
          .context = context};
}

[[nodiscard]] execution::FinalSafetyState
gateway_safety(const execution::GatewayConfiguration& configuration,
               const risk::RiskContext& context, const std::uint64_t now) noexcept {
  execution::FinalSafetyState result{
      .observed_process_monotonic_time_ns = now,
      .market_state_snapshot = context.market_state_snapshot,
      .official_trading_status = context.official_trading_status,
      .feed_health = context.feed_health,
      .book_validity = context.book_validity,
      .clock_quality_snapshot = context.clock_quality_snapshot,
      .authority = kAuthority,
      .effective_configuration_hash = configuration.stable_hash,
      .activation_record_hash = 0U,
      .operator_authorization_valid_until_ns = 0U,
      .signed_configuration_valid = false,
      .operator_authorized = false,
      .activation_record_durable = false,
      .kill_switch_engaged = false,
      .journal_ready = true};
  result.stable_hash = execution::stable_final_safety_state_hash(result);
  return result;
}

[[nodiscard]] oms::OmsInput
oms_gateway_input(const execution::GatewayEvent& event) noexcept {
  oms::InputKind kind = oms::InputKind::rejection;
  switch (event.kind) {
  case execution::GatewayResponseKind::acknowledgement:
    kind = oms::InputKind::acknowledgement;
    break;
  case execution::GatewayResponseKind::rejection:
    kind = oms::InputKind::rejection;
    break;
  case execution::GatewayResponseKind::fill:
    kind = oms::InputKind::fill;
    break;
  case execution::GatewayResponseKind::cancel_acknowledgement:
    kind = oms::InputKind::cancel_acknowledgement;
    break;
  case execution::GatewayResponseKind::cancel_rejection:
    kind = oms::InputKind::cancel_rejection;
    break;
  case execution::GatewayResponseKind::replace_acknowledgement:
    kind = oms::InputKind::replace_acknowledgement;
    break;
  case execution::GatewayResponseKind::replace_rejection:
    kind = oms::InputKind::replace_rejection;
    break;
  case execution::GatewayResponseKind::heartbeat:
  case execution::GatewayResponseKind::session_status:
    break;
  }
  oms::OmsInput result{.receipt_id = event.event_id,
                       .kind = kind,
                       .source = oms::EventSource::gateway,
                       .order_id = event.order_id,
                       .external_order_id = event.external_order_id,
                       .execution_id = event.execution_id,
                       .authority = event.authority,
                       .price_ticks = event.price_ticks,
                       .quantity_units = event.quantity_units,
                       .venue_sequence = event.venue_sequence,
                       .exchange_event_time_ns = event.exchange_event_time_ns,
                       .nic_receive_time_ns = event.nic_receive_time_ns,
                       .process_monotonic_time_ns = event.process_monotonic_time_ns};
  result.stable_hash = oms::stable_input_hash(result);
  return result;
}

[[nodiscard]] std::uint64_t absolute_quantity(const std::int64_t value) noexcept {
  return value >= 0 ? static_cast<std::uint64_t>(value)
                    : std::uint64_t{0U} - static_cast<std::uint64_t>(value);
}

[[nodiscard]] market_state::FeedHealth
map_feed_health(const feed::FeedState state) noexcept {
  switch (state) {
  case feed::FeedState::healthy:
    return market_state::FeedHealth::healthy;
  case feed::FeedState::starting:
    return market_state::FeedHealth::unknown;
  case feed::FeedState::recovering:
  case feed::FeedState::gap_detected:
  case feed::FeedState::replaying_gap:
    return market_state::FeedHealth::recovering;
  case feed::FeedState::stale:
    return market_state::FeedHealth::stale;
  case feed::FeedState::invalid:
  case feed::FeedState::stopped:
    return market_state::FeedHealth::invalid;
  }
  return market_state::FeedHealth::invalid;
}

[[nodiscard]] market_state::BookValidity
map_book_validity(const order_book::BookValidity validity) noexcept {
  switch (validity) {
  case order_book::BookValidity::valid:
    return market_state::BookValidity::valid;
  case order_book::BookValidity::recovering:
    return market_state::BookValidity::recovering;
  case order_book::BookValidity::invalid:
    return market_state::BookValidity::invalid;
  }
  return market_state::BookValidity::invalid;
}

[[nodiscard]] bool publish_explanation(
    observability::DecisionExplanationPublisher& publisher,
    observability::DecisionExplanationJournalPublisher& journal_publisher,
    const ensemble::EnsembleRequest& request,
    const ensemble::EnsembleForecast& forecast, const std::uint64_t ordinal,
    const risk::RiskDecision* risk_decision,
    const execution::RoutingDecision* routing_decision, std::uint64_t& audit_hash,
    bool count_as_order, ScenarioCounters& counters) noexcept {
  const auto correlation = risk_decision == nullptr
                               ? common::GlobalEventId{150U, ordinal}
                               : risk_decision->global_event_id;
  observability::DecisionExplanationRecord record{};
  const observability::DecisionExplanationInput input{
      .explanation_id = common::GlobalEventId{151U, ordinal},
      .correlation_id = correlation,
      .created_process_monotonic_time_ns = request.now_process_monotonic_time_ns + 1U,
      .created_wall_clock_utc_time_ns =
          kExchangeBase + static_cast<std::int64_t>(ordinal),
      .request = &request,
      .ensemble_forecast = &forecast,
      .risk_decision = risk_decision,
      .routing_decision = routing_decision};
  if (!observability::build_decision_explanation(input, record) ||
      !observability::valid_decision_explanation(record) ||
      !journal_publisher.publish(record).accepted() || !publisher.publish(record)) {
    return false;
  }
  observability::DecisionExplanationRecord consumed{};
  if (!publisher.consume(consumed) || consumed.stable_hash != record.stable_hash) {
    return false;
  }
  mix(audit_hash, consumed.stable_hash);
  if (count_as_order) {
    ++counters.explained_orders;
  }
  return true;
}

[[nodiscard]] bool apply_gateway_event(oms::DeterministicOms& service,
                                       const execution::GatewayEvent& event) noexcept {
  const auto input = oms_gateway_input(event);
  const auto result = service.apply(input);
  return result.status == oms::ApplyStatus::applied ||
         result.status == oms::ApplyStatus::out_of_order_applied ||
         result.status == oms::ApplyStatus::reconciled;
}

[[nodiscard]] std::uint64_t
scenario_outcome_hash(const ScenarioResult& result) noexcept {
  std::uint64_t hash = kFnvOffset;
  mix(hash, static_cast<std::uint64_t>(result.scenario));
  mix(hash, result.seed);
  const auto& counters = result.counters;
  mix(hash, counters.source_events);
  mix(hash, counters.normalized_events);
  mix(hash, counters.specialist_forecasts);
  mix(hash, counters.late_forecasts_discarded);
  mix(hash, counters.eligible_experts);
  mix(hash, counters.ensemble_abstentions);
  mix(hash, counters.route_decisions);
  mix(hash, counters.risk_evaluations);
  mix(hash, counters.risk_approvals);
  mix(hash, counters.risk_rejections);
  mix(hash, counters.oms_orders);
  mix(hash, counters.gateway_commands);
  mix(hash, counters.gateway_accepted);
  mix(hash, counters.fills);
  mix(hash, counters.explained_orders);
  mix(hash, counters.telemetry_drops);
  mix(hash, counters.final_absolute_position_units);
  mix(hash, std::bit_cast<std::uint64_t>(counters.final_pnl_currency_nanos));
  const auto& component = result.hashes;
  mix(hash, component.source_events);
  mix(hash, component.feature_snapshot);
  mix(hash, component.specialist_forecasts);
  mix(hash, component.market_state);
  mix(hash, component.ensemble);
  mix(hash, component.risk);
  mix(hash, component.oms);
  mix(hash, component.gateway);
  mix(hash, component.portfolio);
  mix(hash, component.audit);
  return nonzero_hash(hash);
}

// This is an off-path certification harness. Complexity is kept in one ordered
// function so the report mirrors the actual safety sequence without hidden callbacks.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] ScenarioResult execute_once(const PaperScenario scenario,
                                          const std::uint64_t seed) {
  ScenarioResult result{};
  result.scenario = scenario;
  result.seed = seed;
  bool pipeline_ok = true;
  bool restriction_observed = !is_restricted_scenario(scenario);
  bool restart_reconstructed = scenario != PaperScenario::risk_service_restart;
  bool correction_retained = scenario != PaperScenario::false_rumor_correction;
  bool no_risk_bypass = true;
  bool halt_or_close_blocked = scenario != PaperScenario::market_close &&
                               scenario != PaperScenario::trading_halt_reopening;
  std::uint64_t source_hash = kFnvOffset;
  std::uint64_t specialist_hash = kFnvOffset;
  std::uint64_t ensemble_hash = kFnvOffset;
  std::uint64_t risk_hash = kFnvOffset;
  std::uint64_t audit_hash = kFnvOffset;

  const auto gateway_config = gateway_configuration(seed);
  auto gateway_journal = std::make_unique<execution::GatewayAuditJournal>();
  auto gateway =
      std::make_unique<execution::PaperBrokerGateway>(gateway_config, *gateway_journal);
  pipeline_ok = gateway->start(kBaseNow - 500U) && pipeline_ok;
  auto book = std::make_unique<order_book::OrderBook>(book_configuration());
  auto feature_engine =
      std::make_unique<features::FeatureEngine>(feature_configuration());
  auto publisher =
      std::make_unique<PipelinePublisher>(*book, *feature_engine, *gateway);
  const feed::FeedIdentity identity{.session_id = kSession,
                                    .venue_id = kVenue,
                                    .channel_id = kChannel,
                                    .venue_number = 1U,
                                    .channel_number = 7U};
  auto receiver = std::make_unique<feed::SyntheticReceiver>(identity, 64U, 64U);
  const feed::SyntheticDecoder decoder{identity};
  AcceptingRawJournal raw_journal;
  CapturingHealthPublisher health_publisher;
  auto handler = std::make_unique<feed::FeedHandler>(
      feed::FeedHandlerConfig{.identity = identity,
                              .initial_sequence = 1U,
                              .maximum_sequence =
                                  std::numeric_limits<std::uint64_t>::max(),
                              .stale_after_ns = 100'000U,
                              .recovery_timeout_ns = 1'000U,
                              .arbitration_window = 16U},
      *receiver, decoder, *receiver, raw_journal, *publisher, health_publisher);
  pipeline_ok = handler->start(kBaseNow - 400U) && pipeline_ok;

  RecordingTransitionJournal transition_journal;
  auto controller = std::make_unique<market_state::MarketStateController>(
      market_state_configuration(), transition_journal.sink(), kBaseNow - 500U,
      kWallBase - 500U);
  std::uint64_t state_sequence{};
  std::uint64_t state_now = kBaseNow - 300U;
  std::uint64_t wall_now = kWallBase - 300U;
  if (scenario == PaperScenario::market_open) {
    auto pre_open = healthy_market_input(++state_sequence, ++state_now, ++wall_now);
    pre_open.official_trading_status = market_state::OfficialTradingStatus::pre_open;
    static_cast<void>(controller->evaluate(pre_open));
  }
  pipeline_ok =
      reach_normal(*controller, state_sequence, state_now, wall_now) && pipeline_ok;

  auto portfolio_journal = std::make_unique<portfolio::PortfolioJournal>();
  auto portfolio_service = std::make_unique<portfolio::PortfolioRiskService>(
      portfolio_configuration(), *portfolio_journal);
  pipeline_ok = portfolio_service
                        ->apply(portfolio_mark(
                            {.ordinal = 1U, .now = kBaseNow - 50U, .price_ticks = 101}))
                        .status == portfolio::ApplyStatus::applied &&
                pipeline_ok;
  auto risk_journal = std::make_unique<risk::RiskDecisionJournal>();
  auto risk_engine = std::make_unique<risk::DeterministicPreTradeRiskEngine>(
      risk_limits(), *risk_journal);
  pipeline_ok = risk_engine->install_portfolio_snapshot(
                    portfolio_service->snapshot_for_quiescent_inspection()) ==
                    risk::StateUpdateStatus::applied &&
                pipeline_ok;
  auto oms_journal = std::make_unique<oms::OmsJournal>();
  auto oms_service =
      std::make_unique<oms::DeterministicOms>(oms_configuration(), *oms_journal);
  execution::SmartOrderRouter router{router_configuration()};
  const ensemble::MixtureOfExpertsGate gate{ensemble_configuration()};
  observability::DecisionExplanationPublisher explanation_publisher;
  const TemporaryAuditDirectory audit_directory;
  journal::AsyncJournal decision_journal;
  pipeline_ok = !audit_directory.path().empty() &&
                decision_journal.open(audit_journal_configuration(
                    audit_directory.path() / "journal", seed)) == journal::Status::ok &&
                pipeline_ok;
  observability::DecisionExplanationJournalPublisher decision_journal_publisher{
      decision_journal};
  observability::TelemetryPublisher telemetry;

  std::uint64_t next_sequence = 1U;
  auto send_market_event = [&](const synthetic::SyntheticEvent& event,
                               const bool retain = true) {
    ++result.counters.source_events;
    mix(source_hash, event.event_hash);
    const auto packet = feed::make_synthetic_raw_packet(
        event, kSession, feed::FeedLeg::a, event.process_monotonic_time_ns);
    if (!receiver->enqueue(packet, retain)) {
      return false;
    }
    const auto polled = handler->poll_once(event.process_monotonic_time_ns);
    return polled == feed::PollResult::processed ||
           (scenario == PaperScenario::feed_gap && polled == feed::PollResult::invalid);
  };

  pipeline_ok = send_market_event(market_event(
                    next_sequence++, kBaseNow - 300U,
                    synthetic::NativeMessageType::trading_status, synthetic::Side::none,
                    0, 0U, synthetic::TradingStatus::open)) &&
                pipeline_ok;
  if (scenario == PaperScenario::feed_gap) {
    ++next_sequence;
    pipeline_ok =
        send_market_event(market_event(next_sequence++, kBaseNow - 200U,
                                       synthetic::NativeMessageType::price_level,
                                       synthetic::Side::ask, 102, 20U),
                          false) &&
        pipeline_ok;
    static_cast<void>(feature_engine->mark_gap(kBaseNow - 150U));
  } else {
    pipeline_ok =
        send_market_event(market_event(
            next_sequence++, kBaseNow - 200U, synthetic::NativeMessageType::price_level,
            synthetic::Side::bid, 100, 20U, synthetic::TradingStatus::none,
            scenario == PaperScenario::hidden_liquidity_replenishment)) &&
        pipeline_ok;
    pipeline_ok =
        send_market_event(market_event(next_sequence++, kBaseNow - 100U,
                                       synthetic::NativeMessageType::price_level,
                                       synthetic::Side::ask, 102, 20U)) &&
        pipeline_ok;
  }

  state_now = kBaseNow + 100U;
  wall_now = kWallBase + 100U;
  auto state_input = healthy_market_input(++state_sequence, state_now, wall_now);
  if (scenario == PaperScenario::earnings_release) {
    state_input.news_event_state = market_state::NewsEventState::price_discovery;
  } else if (scenario == PaperScenario::cpi_release) {
    state_input.macro_calendar = {.state =
                                      market_state::CalendarEventState::release_active,
                                  .release_wall_clock_utc_ns = wall_now};
  } else if (scenario == PaperScenario::breaking_negative_news ||
             scenario == PaperScenario::false_rumor_correction) {
    state_input.news_event_state = market_state::NewsEventState::breaking;
  } else if (scenario == PaperScenario::market_close ||
             scenario == PaperScenario::index_rebalance) {
    state_input.earnings_calendar = {.state =
                                         market_state::CalendarEventState::scheduled,
                                     .release_wall_clock_utc_ns = wall_now + 50U};
  } else if (scenario == PaperScenario::feed_gap) {
    state_input.feed_health = map_feed_health(handler->state());
    state_input.book_validity = market_state::BookValidity::invalid;
  } else if (scenario == PaperScenario::clock_degradation) {
    state_input.clock_quality = market_state::ClockQuality::degraded;
  }
  if (scenario == PaperScenario::false_rumor_correction) {
    state_input.model_disagreement_ppm = 900'000U;
  }
  if (scenario == PaperScenario::trading_halt_reopening) {
    state_input.official_trading_status = market_state::OfficialTradingStatus::halted;
  }
  auto pending_state_input = state_input;
  bool state_input_pending = true;

  std::uint64_t decision_ordinal{};
  features::FeatureSnapshot latest_feature_snapshot{};
  ensemble::EnsembleForecast latest_ensemble{};
  risk::RiskDecision latest_risk{};
  bool correction_stage = false;
  // The explicit linear sequence is the certification evidence for the safety
  // boundary; splitting it into callbacks would obscure its order and captures.
  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  auto evaluate_and_execute = [&](const bool expect_blocked) {
    ++decision_ordinal;
    const auto decision_now = state_now + (decision_ordinal * 10U);
    if (feature_engine->make_snapshot(decision_now, latest_feature_snapshot) !=
        features::FeatureError::none) {
      return expect_blocked;
    }
    auto request = ensemble_request(
        scenario, decision_now, latest_feature_snapshot, controller->snapshot(),
        map_feed_health(handler->state()), specialist_hash, correction_stage);
    if (state_input_pending) {
      const auto state_result = controller->evaluate(pending_state_input);
      state_input_pending = false;
      if (state_result.status == market_state::EvaluationStatus::journal_rejected) {
        return false;
      }
      request.market_state_snapshot = state_result.snapshot;
    }
    result.counters.specialist_forecasts += request.expert_count;
    const auto evaluated = gate.evaluate(request);
    latest_ensemble = evaluated.forecast;
    mix(ensemble_hash, latest_ensemble.stable_hash);
    for (std::size_t index = 0U;
         index < latest_ensemble.explanation.supplied_expert_count; ++index) {
      const auto eligibility = latest_ensemble.explanation.experts[index].eligibility;
      if (eligibility == ensemble::EligibilityReason::expired) {
        ++result.counters.late_forecasts_discarded;
      }
      if (eligibility == ensemble::EligibilityReason::eligible) {
        ++result.counters.eligible_experts;
      }
    }
    if (evaluated.status == ensemble::EvaluationStatus::abstained) {
      ++result.counters.ensemble_abstentions;
      const auto explained = publish_explanation(
          explanation_publisher, decision_journal_publisher, request, latest_ensemble,
          decision_ordinal, nullptr, nullptr, audit_hash, false, result.counters);
      return expect_blocked && explained;
    }
    if (evaluated.status != ensemble::EvaluationStatus::produced ||
        latest_ensemble.abstain) {
      return false;
    }
    auto routed_request =
        routing_request(scenario, latest_ensemble, latest_feature_snapshot, book->top(),
                        decision_now + 1U);
    const auto routed = router.route(routed_request);
    ++result.counters.route_decisions;
    if (routed.status != execution::RoutingStatus::routed ||
        !routed.requires_fresh_pretrade_risk) {
      return false;
    }
    const auto split_brain = scenario == PaperScenario::split_brain_attempt;
    const auto risk_evaluation =
        risk_request(routed, routed_request.objective, controller->snapshot(),
                     map_feed_health(handler->state()),
                     map_book_validity(book->validity()), decision_now + 2U, seed,
                     scenario == PaperScenario::clock_degradation, split_brain);
    const auto risk_result = risk_engine->evaluate(risk_evaluation);
    latest_risk = risk_result.decision;
    ++result.counters.risk_evaluations;
    mix(risk_hash, latest_risk.stable_hash);
    if (latest_risk.decision == risk::DecisionCode::rejected) {
      ++result.counters.risk_rejections;
      const auto explained = publish_explanation(
          explanation_publisher, decision_journal_publisher, request, latest_ensemble,
          decision_ordinal, &latest_risk, &routed, audit_hash, false, result.counters);
      return expect_blocked && explained;
    }
    ++result.counters.risk_approvals;
    if (expect_blocked || risk_result.status != risk::EvaluationStatus::journaled) {
      return false;
    }
    oms::OmsInput accept{.receipt_id =
                             common::GlobalEventId{160U, (decision_ordinal * 10U) + 1U},
                         .kind = oms::InputKind::accept_intent,
                         .source = oms::EventSource::internal,
                         .intent = risk_evaluation.intent,
                         .risk_decision = latest_risk,
                         .process_monotonic_time_ns = decision_now + 3U};
    accept.stable_hash = oms::stable_input_hash(accept);
    const auto accepted = oms_service->apply(accept);
    if (accepted.status != oms::ApplyStatus::applied) {
      return false;
    }
    ++result.counters.oms_orders;
    const auto order_id = accepted.order_id;
    oms::OmsInput ready{.receipt_id =
                            common::GlobalEventId{160U, (decision_ordinal * 10U) + 2U},
                        .kind = oms::InputKind::mark_ready,
                        .source = oms::EventSource::internal,
                        .order_id = order_id,
                        .authority = kAuthority,
                        .process_monotonic_time_ns = decision_now + 4U};
    ready.stable_hash = oms::stable_input_hash(ready);
    if (oms_service->apply(ready).status != oms::ApplyStatus::applied) {
      return false;
    }
    oms::OmsInput dispatch{
        .receipt_id = common::GlobalEventId{160U, (decision_ordinal * 10U) + 3U},
        .kind = oms::InputKind::dispatch,
        .source = oms::EventSource::internal,
        .order_id = order_id,
        .authority = kAuthority,
        .process_monotonic_time_ns = decision_now + 5U};
    dispatch.stable_hash = oms::stable_input_hash(dispatch);
    const auto dispatched = oms_service->apply(dispatch);
    if (dispatched.status != oms::ApplyStatus::applied ||
        !oms::valid_gateway_command(dispatched.gateway_command)) {
      return false;
    }
    const auto order_record =
        portfolio_order({.ordinal = 20U + decision_ordinal,
                         .now = decision_now + 5U,
                         .order_id = order_id,
                         .side = risk_evaluation.intent.action,
                         .price_ticks = routed.price_ticks,
                         .quantity_units = routed.quantity_units,
                         .state = portfolio::OrderLifecycleState::accepted,
                         .remaining = routed.quantity_units});
    if (portfolio_service->apply(order_record).status !=
        portfolio::ApplyStatus::applied) {
      return false;
    }
    if (scenario == PaperScenario::gateway_disconnect) {
      if (!gateway->logoff(decision_now + 6U)) {
        return false;
      }
    }
    execution::GatewayRequest gateway_request{
        .command = dispatched.gateway_command,
        .risk_decision = latest_risk,
        .safety =
            gateway_safety(gateway_config, risk_evaluation.context, decision_now + 7U)};
    gateway_request.stable_hash =
        execution::stable_gateway_request_hash(gateway_request);
    ++result.counters.gateway_commands;
    no_risk_bypass =
        no_risk_bypass && latest_risk.decision == risk::DecisionCode::approved &&
        dispatched.gateway_command.risk_decision_hash == latest_risk.stable_hash &&
        dispatched.gateway_command.intent_hash == risk_evaluation.intent.stable_hash;
    if (!publish_explanation(explanation_publisher, decision_journal_publisher, request,
                             latest_ensemble, decision_ordinal, &latest_risk, &routed,
                             audit_hash, true, result.counters)) {
      return false;
    }
    const auto submitted = gateway->send_order(gateway_request);
    if (submitted.status != execution::GatewaySubmitStatus::accepted) {
      restriction_observed =
          scenario == PaperScenario::gateway_disconnect &&
          submitted.reason == execution::GatewayReason::session_not_active;
      return scenario == PaperScenario::gateway_disconnect;
    }
    ++result.counters.gateway_accepted;
    execution::GatewayEvent acknowledgement{};
    if (gateway->poll_event(decision_now + 200U, acknowledgement) !=
            execution::GatewayPollStatus::event ||
        acknowledgement.kind != execution::GatewayResponseKind::acknowledgement ||
        !apply_gateway_event(*oms_service, acknowledgement)) {
      return false;
    }
    const auto open_record =
        portfolio_order({.ordinal = 30U + decision_ordinal,
                         .now = decision_now + 201U,
                         .order_id = order_id,
                         .side = risk_evaluation.intent.action,
                         .price_ticks = routed.price_ticks,
                         .quantity_units = routed.quantity_units,
                         .state = portfolio::OrderLifecycleState::open,
                         .remaining = routed.quantity_units});
    if (portfolio_service->apply(open_record).status !=
        portfolio::ApplyStatus::applied) {
      return false;
    }
    state_now = decision_now + 300U;
    if (scenario == PaperScenario::index_rebalance) {
      if (!send_market_event(market_event(next_sequence++, state_now++,
                                          synthetic::NativeMessageType::trading_status,
                                          synthetic::Side::none, 0, 0U,
                                          synthetic::TradingStatus::auction))) {
        return false;
      }
      if (!send_market_event(
              market_event(next_sequence++, state_now++,
                           synthetic::NativeMessageType::auction_imbalance,
                           synthetic::Side::bid, routed.price_ticks, 100U))) {
        return false;
      }
    } else {
      if (!send_market_event(market_event(
              next_sequence++, state_now++, synthetic::NativeMessageType::trade,
              risk_evaluation.intent.action == risk::IntentAction::buy
                  ? synthetic::Side::ask
                  : synthetic::Side::bid,
              routed.price_ticks, 100U))) {
        return false;
      }
    }
    execution::GatewayEvent fill{};
    if (gateway->poll_event(state_now + 10U, fill) !=
            execution::GatewayPollStatus::event ||
        fill.kind != execution::GatewayResponseKind::fill ||
        !apply_gateway_event(*oms_service, fill)) {
      return false;
    }
    ++result.counters.fills;
    auto fill_record = portfolio_fill(fill, 40U + decision_ordinal);
    fill_record.side = risk_evaluation.intent.action == risk::IntentAction::buy
                           ? portfolio::Side::buy
                           : portfolio::Side::sell;
    fill_record.stable_hash = portfolio::stable_event_hash(fill_record);
    if (portfolio_service->apply(fill_record).status !=
        portfolio::ApplyStatus::applied) {
      return false;
    }
    const auto filled_record =
        portfolio_order({.ordinal = 50U + decision_ordinal,
                         .now = state_now + 11U,
                         .order_id = order_id,
                         .side = risk_evaluation.intent.action,
                         .price_ticks = routed.price_ticks,
                         .quantity_units = routed.quantity_units,
                         .state = portfolio::OrderLifecycleState::filled,
                         .cumulative_fill = fill.cumulative_fill_quantity_units,
                         .remaining = fill.remaining_quantity_units});
    if (portfolio_service->apply(filled_record).status !=
            portfolio::ApplyStatus::applied ||
        portfolio_service
                ->apply(portfolio_mark({.ordinal = 60U + decision_ordinal,
                                        .now = state_now + 12U,
                                        .price_ticks = 103}))
                .status != portfolio::ApplyStatus::applied) {
      return false;
    }
    return risk_engine->install_portfolio_snapshot(
               portfolio_service->snapshot_for_quiescent_inspection()) ==
           risk::StateUpdateStatus::applied;
  };

  if (scenario == PaperScenario::feed_gap) {
    const auto state_result = controller->evaluate(pending_state_input);
    state_input_pending = false;
    pipeline_ok =
        state_result.status != market_state::EvaluationStatus::journal_rejected &&
        pipeline_ok;
    restriction_observed = handler->state() != feed::FeedState::healthy;
    ++result.counters.ensemble_abstentions;
  } else if (scenario == PaperScenario::trading_halt_reopening) {
    const auto accepted_before = result.counters.gateway_accepted;
    pipeline_ok = evaluate_and_execute(true) && pipeline_ok;
    halt_or_close_blocked = result.counters.gateway_accepted == accepted_before;
    pipeline_ok =
        send_market_event(market_event(next_sequence++, state_now + 20U,
                                       synthetic::NativeMessageType::trading_status,
                                       synthetic::Side::none, 0, 0U,
                                       synthetic::TradingStatus::halted)) &&
        pipeline_ok;
    state_now += 40U;
    wall_now += 40U;
    auto reopening = healthy_market_input(++state_sequence, state_now, wall_now);
    reopening.official_trading_status = market_state::OfficialTradingStatus::auction;
    static_cast<void>(controller->evaluate(reopening));
    pipeline_ok =
        send_market_event(market_event(next_sequence++, state_now + 41U,
                                       synthetic::NativeMessageType::trading_status,
                                       synthetic::Side::none, 0, 0U,
                                       synthetic::TradingStatus::auction)) &&
        pipeline_ok;
    state_now += 40U;
    wall_now += 40U;
    auto recovery = healthy_market_input(++state_sequence, state_now, wall_now);
    static_cast<void>(controller->evaluate(recovery));
    pipeline_ok = send_market_event(market_event(
                      next_sequence++, state_now + 81U,
                      synthetic::NativeMessageType::trading_status,
                      synthetic::Side::none, 0, 0U, synthetic::TradingStatus::open)) &&
                  pipeline_ok;
    pipeline_ok =
        reach_normal(*controller, state_sequence, state_now, wall_now) && pipeline_ok;
    pipeline_ok = evaluate_and_execute(false) && pipeline_ok;
  } else {
    const bool decision_must_block = is_restricted_scenario(scenario) &&
                                     scenario != PaperScenario::gateway_disconnect;
    pipeline_ok = evaluate_and_execute(decision_must_block) && pipeline_ok;
    if (scenario == PaperScenario::false_rumor_correction) {
      const auto rumor_disagreement = latest_ensemble.disagreement_ppm;
      const auto rumor_alert_recorded = result.counters.ensemble_abstentions == 1U &&
                                        rumor_disagreement != 0U &&
                                        result.counters.gateway_accepted == 0U;
      state_now += 100U;
      wall_now += 100U;
      pending_state_input = healthy_market_input(++state_sequence, state_now, wall_now);
      state_input_pending = true;
      correction_stage = true;
      pipeline_ok = evaluate_and_execute(true) && pipeline_ok;
      correction_retained = rumor_alert_recorded &&
                            result.counters.ensemble_abstentions == 2U &&
                            latest_ensemble.disagreement_ppm < rumor_disagreement &&
                            result.counters.gateway_accepted == 0U;
    }
    if (scenario == PaperScenario::market_close) {
      const auto accepted_before = result.counters.gateway_accepted;
      state_now += 100U;
      wall_now += 100U;
      auto closed = healthy_market_input(++state_sequence, state_now, wall_now);
      closed.official_trading_status = market_state::OfficialTradingStatus::closed;
      pending_state_input = closed;
      state_input_pending = true;
      pipeline_ok =
          send_market_event(market_event(next_sequence++, state_now + 1U,
                                         synthetic::NativeMessageType::trading_status,
                                         synthetic::Side::none, 0, 0U,
                                         synthetic::TradingStatus::closed)) &&
          pipeline_ok;
      pipeline_ok = evaluate_and_execute(true) && pipeline_ok;
      halt_or_close_blocked = result.counters.gateway_accepted == accepted_before;
    }
  }

  if (scenario == PaperScenario::false_rumor_correction) {
    restriction_observed =
        correction_retained && result.counters.gateway_accepted == 0U;
  }
  if (scenario == PaperScenario::model_timeout) {
    restriction_observed = result.counters.late_forecasts_discarded == 4U &&
                           result.counters.ensemble_abstentions != 0U &&
                           result.counters.gateway_accepted == 0U;
  }
  if (scenario == PaperScenario::clock_degradation) {
    restriction_observed =
        controller->snapshot().state == market_state::MarketState::data_degraded &&
        result.counters.gateway_accepted == 0U;
  }
  if (scenario == PaperScenario::split_brain_attempt) {
    event_bus::ProcessEpochState epoch_state;
    const auto first =
        event_bus::ProcessEpochMechanism::claim(epoch_state, 11U, 1U, kBaseNow, 1'000U);
    const auto second = event_bus::ProcessEpochMechanism::claim(epoch_state, 12U, 1U,
                                                                kBaseNow + 1U, 1'000U);
    restriction_observed = first == event_bus::EpochClaimStatus::acquired &&
                           second == event_bus::EpochClaimStatus::split_brain &&
                           latest_risk.reason == risk::RiskReason::split_brain_epoch &&
                           result.counters.gateway_accepted == 0U;
  }
  if (scenario == PaperScenario::risk_service_restart) {
    auto recovered_oms_journal = std::make_unique<oms::OmsJournal>();
    auto recovered_oms = std::make_unique<oms::DeterministicOms>(
        oms_configuration(), *recovered_oms_journal);
    const auto oms_recovery =
        recovered_oms->recover_from(*oms_journal, state_now + 1'000U);
    auto recovered_portfolio_journal = std::make_unique<portfolio::PortfolioJournal>();
    auto recovered_portfolio = std::make_unique<portfolio::PortfolioRiskService>(
        portfolio_configuration(), *recovered_portfolio_journal);
    const auto portfolio_recovery =
        recovered_portfolio->recover_from(*portfolio_journal);
    auto restarted_risk_journal = std::make_unique<risk::RiskDecisionJournal>();
    auto restarted_risk = std::make_unique<risk::DeterministicPreTradeRiskEngine>(
        risk_limits(), *restarted_risk_journal);
    const auto source_oms = oms_service->snapshot_for_quiescent_inspection();
    const auto target_oms = recovered_oms->snapshot_for_quiescent_inspection();
    const auto source_portfolio =
        portfolio_service->snapshot_for_quiescent_inspection();
    const auto target_portfolio =
        recovered_portfolio->snapshot_for_quiescent_inspection();
    restart_reconstructed =
        oms_recovery == oms::RecoveryStatus::recovered_ready &&
        portfolio_recovery == portfolio::RecoveryStatus::recovered &&
        source_oms.order_count == target_oms.order_count &&
        source_oms.live_order_count == target_oms.live_order_count &&
        source_portfolio.stable_hash == target_portfolio.stable_hash &&
        restarted_risk->install_portfolio_snapshot(target_portfolio) ==
            risk::StateUpdateStatus::applied;
  }

  const auto portfolio_snapshot =
      portfolio_service->snapshot_for_quiescent_inspection();
  result.counters.normalized_events = publisher->normalized_count();
  result.counters.telemetry_drops = telemetry.dropped_points();

  const auto journal_metrics = decision_journal.metrics();
  const auto journal_shutdown = decision_journal.shutdown(std::chrono::seconds(2));
  const auto journal_report =
      journal::RecoveryScanner::scan_directory(audit_directory.path() / "journal");
  bool explanations_recoverable =
      journal_shutdown == journal::Status::ok &&
      journal_report.status == journal::Status::ok &&
      journal_report.valid_record_count == journal_metrics.accepted_records;
  for (const auto& segment : journal_report.segments) {
    for (const auto& persisted : segment.records) {
      observability::DecisionExplanationRecord recovered{};
      explanations_recoverable =
          explanations_recoverable &&
          persisted.metadata.kind == journal::RecordKind::ensemble_decision &&
          observability::deserialize_decision_explanation(persisted.payload,
                                                          recovered) &&
          recovered.stable_hash != 0U;
    }
  }
  if (portfolio_snapshot.instrument_count != 0U) {
    result.counters.final_absolute_position_units =
        absolute_quantity(portfolio_snapshot.instruments[0U].net_quantity_units);
  }
  result.counters.final_pnl_currency_nanos =
      portfolio_snapshot.total_pnl_currency_nanos;
  static_cast<void>(telemetry.counter_set(observability::MetricId::market_packets,
                                          result.counters.source_events));
  static_cast<void>(telemetry.counter_set(observability::MetricId::trading_orders,
                                          result.counters.gateway_accepted));
  static_cast<void>(telemetry.counter_set(observability::MetricId::trading_fills,
                                          result.counters.fills));
  static_cast<void>(
      telemetry.gauge_set(observability::MetricId::trading_net_exposure_currency_nanos,
                          portfolio_snapshot.net_exposure_currency_nanos));
  observability::TelemetryProcessor telemetry_processor{telemetry};
  static_cast<void>(telemetry_processor.drain(128U));
  result.counters.telemetry_drops = telemetry.dropped_points();

  result.hashes.source_events = nonzero_hash(source_hash);
  result.hashes.feature_snapshot = latest_feature_snapshot.stable_hash;
  result.hashes.specialist_forecasts = nonzero_hash(specialist_hash);
  result.hashes.market_state = controller->snapshot().stable_hash;
  result.hashes.ensemble = nonzero_hash(ensemble_hash);
  result.hashes.risk = nonzero_hash(risk_hash);
  result.hashes.oms = oms_service->snapshot_for_quiescent_inspection().stable_hash;
  result.hashes.gateway = gateway_journal->last_record_hash();
  result.hashes.portfolio = portfolio_snapshot.stable_hash;
  mix(audit_hash, transition_journal.stable_hash());
  mix(audit_hash, raw_journal.hash());
  for (std::size_t index = 0U; index < sizeof(std::uint64_t); ++index) {
    mix(audit_hash, journal_report.terminal_record_sha256[index]);
  }
  result.hashes.audit = nonzero_hash(audit_hash);
  result.checks.paper_mode_only =
      gateway_config.startup_mode == execution::GatewayMode::paper &&
      oms_configuration().trading_mode == risk::TradingMode::paper &&
      !execution::kLiveTradingCompiled;
  result.checks.pipeline_complete = pipeline_ok && publisher->valid();
  result.checks.no_risk_bypass = no_risk_bypass && result.counters.gateway_commands <=
                                                       result.counters.risk_approvals;
  result.checks.expected_restriction_observed =
      restriction_observed && halt_or_close_blocked;
  result.checks.audit_complete =
      result.counters.explained_orders == result.counters.oms_orders &&
      explanation_publisher.dropped_records() == 0U && explanations_recoverable;
  result.checks.restart_reconstructed = restart_reconstructed;
  result.checks.correction_retained = correction_retained;
  result.outcome_hash = scenario_outcome_hash(result);
  const bool fill_expectation = scenario_expects_fill(scenario)
                                    ? result.counters.fills != 0U
                                    : result.counters.gateway_accepted == 0U;
  result.passed = result.checks.paper_mode_only && result.checks.pipeline_complete &&
                  result.checks.no_risk_bypass &&
                  result.checks.expected_restriction_observed &&
                  result.checks.audit_complete && result.checks.restart_reconstructed &&
                  result.checks.correction_retained && fill_expectation;
  return result;
}

[[nodiscard]] bool all_scenarios(const PaperAcceptanceReport& report,
                                 const auto predicate) noexcept {
  return std::ranges::all_of(report.scenarios, predicate);
}

[[nodiscard]] std::uint64_t report_hash(const PaperAcceptanceReport& report) noexcept {
  std::uint64_t hash = kFnvOffset;
  mix(hash, report.schema_major);
  mix(hash, report.schema_minor);
  mix(hash, report.seed);
  for (const auto& scenario : report.scenarios) {
    mix(hash, scenario.outcome_hash);
    mix(hash, scenario.replay_outcome_hash);
    mix(hash, scenario.passed ? 1U : 0U);
  }
  const auto& acceptance = report.acceptance;
  mix(hash, acceptance.no_order_bypasses_risk ? 1U : 0U);
  mix(hash, acceptance.halt_blocks_new_orders ? 1U : 0U);
  mix(hash, acceptance.invalid_data_causes_restriction ? 1U : 0U);
  mix(hash, acceptance.late_forecasts_are_discarded ? 1U : 0U);
  mix(hash, acceptance.high_disagreement_reduces_exposure ? 1U : 0U);
  mix(hash, acceptance.all_invalid_models_abstain ? 1U : 0U);
  mix(hash, acceptance.restart_reconstructs_orders_and_positions ? 1U : 0U);
  mix(hash, acceptance.audit_trace_explains_every_order ? 1U : 0U);
  mix(hash, acceptance.deterministic_replay_matches_hashes ? 1U : 0U);
  mix(hash, acceptance.paper_mode_only ? 1U : 0U);
  mix(hash, report.passed ? 1U : 0U);
  return nonzero_hash(hash);
}

[[nodiscard]] std::string hex_hash(const std::uint64_t value) {
  std::ostringstream output;
  output << "0x" << std::hex << std::setfill('0') << std::setw(16) << value;
  return output.str();
}

[[nodiscard]] const char* json_bool(const bool value) noexcept {
  return value ? "true" : "false";
}

[[nodiscard]] bool write_atomically(const std::filesystem::path& path,
                                    const std::string& content) {
  std::error_code error;
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
      return false;
    }
  }
  auto temporary = path;
  temporary += ".tmp";
  {
    std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
    if (!output) {
      return false;
    }
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    output.flush();
    if (!output) {
      return false;
    }
  }
  std::filesystem::rename(temporary, path, error);
  if (!error) {
    return true;
  }
  std::error_code cleanup_error;
  std::filesystem::remove(temporary, cleanup_error);
  return false;
}

void append_check_json(std::ostringstream& output, const char* name, const bool value,
                       const bool comma = true) {
  output << "      \"" << name << "\": " << json_bool(value);
  output << (comma ? ",\n" : "\n");
}

void append_hash_json(std::ostringstream& output, const char* name,
                      const std::uint64_t value, const bool comma = true) {
  output << "        \"" << name << "\": \"" << hex_hash(value) << "\"";
  output << (comma ? ",\n" : "\n");
}

} // namespace

std::string_view scenario_name(const PaperScenario scenario) noexcept {
  switch (scenario) {
  case PaperScenario::ordinary_midday:
    return "ordinary-midday";
  case PaperScenario::market_open:
    return "market-open";
  case PaperScenario::market_close:
    return "market-close";
  case PaperScenario::earnings_release:
    return "earnings-release";
  case PaperScenario::cpi_release:
    return "cpi-release";
  case PaperScenario::breaking_negative_news:
    return "breaking-negative-news";
  case PaperScenario::false_rumor_correction:
    return "false-rumor-correction";
  case PaperScenario::index_rebalance:
    return "index-rebalance";
  case PaperScenario::hidden_liquidity_replenishment:
    return "hidden-liquidity-replenishment";
  case PaperScenario::trading_halt_reopening:
    return "trading-halt-reopening";
  case PaperScenario::feed_gap:
    return "feed-gap";
  case PaperScenario::clock_degradation:
    return "clock-degradation";
  case PaperScenario::model_timeout:
    return "model-timeout";
  case PaperScenario::risk_service_restart:
    return "risk-service-restart";
  case PaperScenario::gateway_disconnect:
    return "gateway-disconnect";
  case PaperScenario::split_brain_attempt:
    return "split-brain-attempt";
  }
  return "unknown";
}

PaperAcceptanceReport run_paper_acceptance(const std::uint64_t seed) {
  PaperAcceptanceReport report;
  report.seed = seed;
  for (std::size_t index = 0U; index < report.scenarios.size(); ++index) {
    const auto scenario = static_cast<PaperScenario>(index + 1U);
    auto primary = execute_once(scenario, seed);
    const auto replayed = execute_once(scenario, seed);
    primary.replay_outcome_hash = replayed.outcome_hash;
    primary.checks.replay_hash_match = primary.outcome_hash == replayed.outcome_hash;
    primary.passed =
        primary.passed && replayed.passed && primary.checks.replay_hash_match;
    report.scenarios[index] = primary;
  }
  const auto& ordinary =
      report.scenarios[scenario_index(PaperScenario::ordinary_midday)];
  const auto& rumor =
      report.scenarios[scenario_index(PaperScenario::false_rumor_correction)];
  const auto& halted =
      report.scenarios[scenario_index(PaperScenario::trading_halt_reopening)];
  const auto& gap = report.scenarios[scenario_index(PaperScenario::feed_gap)];
  const auto& timeout = report.scenarios[scenario_index(PaperScenario::model_timeout)];
  const auto& restart =
      report.scenarios[scenario_index(PaperScenario::risk_service_restart)];
  report.acceptance.no_order_bypasses_risk = all_scenarios(
      report, [](const ScenarioResult& value) { return value.checks.no_risk_bypass; });
  report.acceptance.halt_blocks_new_orders =
      halted.checks.expected_restriction_observed && halted.counters.fills != 0U;
  report.acceptance.invalid_data_causes_restriction =
      gap.checks.expected_restriction_observed && gap.counters.gateway_accepted == 0U;
  report.acceptance.late_forecasts_are_discarded =
      timeout.counters.late_forecasts_discarded == 4U;
  report.acceptance.high_disagreement_reduces_exposure =
      rumor.counters.final_absolute_position_units <
      ordinary.counters.final_absolute_position_units;
  report.acceptance.all_invalid_models_abstain =
      timeout.counters.eligible_experts == 0U &&
      timeout.counters.ensemble_abstentions != 0U &&
      timeout.counters.gateway_accepted == 0U;
  report.acceptance.restart_reconstructs_orders_and_positions =
      restart.checks.restart_reconstructed;
  report.acceptance.audit_trace_explains_every_order = all_scenarios(
      report, [](const ScenarioResult& value) { return value.checks.audit_complete; });
  report.acceptance.deterministic_replay_matches_hashes =
      all_scenarios(report, [](const ScenarioResult& value) {
        return value.checks.replay_hash_match;
      });
  report.acceptance.paper_mode_only = all_scenarios(
      report, [](const ScenarioResult& value) { return value.checks.paper_mode_only; });
  const auto all_acceptance =
      report.acceptance.no_order_bypasses_risk &&
      report.acceptance.halt_blocks_new_orders &&
      report.acceptance.invalid_data_causes_restriction &&
      report.acceptance.late_forecasts_are_discarded &&
      report.acceptance.high_disagreement_reduces_exposure &&
      report.acceptance.all_invalid_models_abstain &&
      report.acceptance.restart_reconstructs_orders_and_positions &&
      report.acceptance.audit_trace_explains_every_order &&
      report.acceptance.deterministic_replay_matches_hashes &&
      report.acceptance.paper_mode_only;
  report.passed =
      all_acceptance &&
      all_scenarios(report, [](const ScenarioResult& value) { return value.passed; });
  report.report_hash = report_hash(report);
  return report;
}

bool write_machine_report(const PaperAcceptanceReport& report,
                          const std::filesystem::path& path) {
  std::ostringstream output;
  output << "{\n"
         << "  \"schema_version\": \"1.0\",\n"
         << "  \"mode\": \"PAPER\",\n"
         << "  \"live_trading_compiled\": "
         << json_bool(execution::kLiveTradingCompiled) << ",\n"
         << "  \"seed\": " << report.seed << ",\n"
         << "  \"passed\": " << json_bool(report.passed) << ",\n"
         << R"(  "report_hash": ")" << hex_hash(report.report_hash) << "\",\n"
         << "  \"execution_order\": [\n"
         << "    \"synthetic-feed\", \"feed-handler\", \"order-book\", "
            "\"features\",\n"
         << "    \"specialist-forecast-contracts\", \"market-state\", "
            "\"ensemble-abstention\",\n"
         << "    \"execution-objective\", \"router\", "
            "\"fresh-exact-risk\", \"oms\",\n"
         << "    \"paper-gateway\", \"fills\", \"positions-pnl\", "
            "\"audit-observability\"\n"
         << "  ],\n"
         << "  \"acceptance\": {\n";
  append_check_json(output, "no_order_bypasses_risk",
                    report.acceptance.no_order_bypasses_risk);
  append_check_json(output, "halt_blocks_new_orders",
                    report.acceptance.halt_blocks_new_orders);
  append_check_json(output, "invalid_data_causes_restriction",
                    report.acceptance.invalid_data_causes_restriction);
  append_check_json(output, "late_forecasts_are_discarded",
                    report.acceptance.late_forecasts_are_discarded);
  append_check_json(output, "high_disagreement_reduces_exposure",
                    report.acceptance.high_disagreement_reduces_exposure);
  append_check_json(output, "all_invalid_models_abstain",
                    report.acceptance.all_invalid_models_abstain);
  append_check_json(output, "restart_reconstructs_orders_and_positions",
                    report.acceptance.restart_reconstructs_orders_and_positions);
  append_check_json(output, "audit_trace_explains_every_order",
                    report.acceptance.audit_trace_explains_every_order);
  append_check_json(output, "deterministic_replay_matches_hashes",
                    report.acceptance.deterministic_replay_matches_hashes);
  append_check_json(output, "paper_mode_only", report.acceptance.paper_mode_only,
                    false);
  output << "  },\n  \"scenarios\": [\n";
  for (std::size_t index = 0U; index < report.scenarios.size(); ++index) {
    const auto& scenario = report.scenarios[index];
    const auto& counters = scenario.counters;
    output << "    {\n"
           << "      \"ordinal\": " << (index + 1U) << ",\n"
           << R"(      "name": ")" << scenario_name(scenario.scenario) << "\",\n"
           << "      \"passed\": " << json_bool(scenario.passed) << ",\n"
           << R"(      "outcome_hash": ")" << hex_hash(scenario.outcome_hash) << "\",\n"
           << R"(      "replay_outcome_hash": ")"
           << hex_hash(scenario.replay_outcome_hash) << "\",\n"
           << "      \"counters\": {\n"
           << "        \"source_events\": " << counters.source_events << ",\n"
           << "        \"normalized_events\": " << counters.normalized_events << ",\n"
           << "        \"specialist_forecasts\": " << counters.specialist_forecasts
           << ",\n"
           << "        \"late_forecasts_discarded\": "
           << counters.late_forecasts_discarded << ",\n"
           << "        \"eligible_experts\": " << counters.eligible_experts << ",\n"
           << "        \"ensemble_abstentions\": " << counters.ensemble_abstentions
           << ",\n"
           << "        \"route_decisions\": " << counters.route_decisions << ",\n"
           << "        \"risk_evaluations\": " << counters.risk_evaluations << ",\n"
           << "        \"risk_approvals\": " << counters.risk_approvals << ",\n"
           << "        \"risk_rejections\": " << counters.risk_rejections << ",\n"
           << "        \"oms_orders\": " << counters.oms_orders << ",\n"
           << "        \"gateway_commands\": " << counters.gateway_commands << ",\n"
           << "        \"gateway_accepted\": " << counters.gateway_accepted << ",\n"
           << "        \"fills\": " << counters.fills << ",\n"
           << "        \"explained_orders\": " << counters.explained_orders << ",\n"
           << "        \"telemetry_drops\": " << counters.telemetry_drops << ",\n"
           << "        \"final_absolute_position_units\": "
           << counters.final_absolute_position_units << ",\n"
           << "        \"final_pnl_currency_nanos\": "
           << counters.final_pnl_currency_nanos << "\n"
           << "      },\n      \"checks\": {\n";
    append_check_json(output, "paper_mode_only", scenario.checks.paper_mode_only);
    append_check_json(output, "pipeline_complete", scenario.checks.pipeline_complete);
    append_check_json(output, "no_risk_bypass", scenario.checks.no_risk_bypass);
    append_check_json(output, "expected_restriction_observed",
                      scenario.checks.expected_restriction_observed);
    append_check_json(output, "audit_complete", scenario.checks.audit_complete);
    append_check_json(output, "restart_reconstructed",
                      scenario.checks.restart_reconstructed);
    append_check_json(output, "correction_retained",
                      scenario.checks.correction_retained);
    append_check_json(output, "replay_hash_match", scenario.checks.replay_hash_match,
                      false);
    output << "      },\n      \"component_hashes\": {\n";
    append_hash_json(output, "source_events", scenario.hashes.source_events);
    append_hash_json(output, "feature_snapshot", scenario.hashes.feature_snapshot);
    append_hash_json(output, "specialist_forecasts",
                     scenario.hashes.specialist_forecasts);
    append_hash_json(output, "market_state", scenario.hashes.market_state);
    append_hash_json(output, "ensemble", scenario.hashes.ensemble);
    append_hash_json(output, "risk", scenario.hashes.risk);
    append_hash_json(output, "oms", scenario.hashes.oms);
    append_hash_json(output, "gateway", scenario.hashes.gateway);
    append_hash_json(output, "portfolio", scenario.hashes.portfolio);
    append_hash_json(output, "audit", scenario.hashes.audit, false);
    output << "      }\n    }";
    output << (index + 1U == report.scenarios.size() ? "\n" : ",\n");
  }
  output << "  ]\n}\n";
  return write_atomically(path, output.str());
}

bool write_human_report(const PaperAcceptanceReport& report,
                        const std::filesystem::path& path) {
  std::ostringstream output;
  output << "# Aegis-MX full-system PAPER acceptance report\n\n"
         << "Overall: **" << (report.passed ? "PASS" : "FAIL") << "**  \n"
         << "Mode: `PAPER`  \n"
         << "Seed: `" << report.seed << "`  \n"
         << "Report hash: `" << hex_hash(report.report_hash) << "`  \n"
         << "Live trading compiled: `"
         << (execution::kLiveTradingCompiled ? "true" : "false") << "`\n\n"
         << "The enforced safety order is objective -> router proposal -> fresh "
            "exact risk -> OMS -> PAPER gateway. Specialist services enter through "
            "the versioned forecast contract; no Python or RPC runs on the edge hot "
            "path.\n\n"
         << "## Acceptance\n\n"
         << "| Invariant | Result |\n|---|---|\n";
  const auto row = [&output](const std::string_view name, const bool value) {
    output << "| " << name << " | " << (value ? "PASS" : "FAIL") << " |\n";
  };
  row("No order bypasses risk", report.acceptance.no_order_bypasses_risk);
  row("Halt blocks new orders", report.acceptance.halt_blocks_new_orders);
  row("Invalid data restricts trading",
      report.acceptance.invalid_data_causes_restriction);
  row("Late forecasts are discarded", report.acceptance.late_forecasts_are_discarded);
  row("High disagreement reduces exposure",
      report.acceptance.high_disagreement_reduces_exposure);
  row("All-invalid models abstain", report.acceptance.all_invalid_models_abstain);
  row("Restart reconstructs orders and positions",
      report.acceptance.restart_reconstructs_orders_and_positions);
  row("Every order has an explanation",
      report.acceptance.audit_trace_explains_every_order);
  row("Repeated replay hashes match",
      report.acceptance.deterministic_replay_matches_hashes);
  row("PAPER-only boundary", report.acceptance.paper_mode_only);
  output
      << "\n## Scenarios\n\n"
      << "| # | Scenario | Result | Events | Forecasts | Risk A/R | Orders | Fills | "
         "Position | Replay hash |\n"
      << "|---:|---|---|---:|---:|---:|---:|---:|---:|---|\n";
  for (std::size_t index = 0U; index < report.scenarios.size(); ++index) {
    const auto& scenario = report.scenarios[index];
    output << "| " << (index + 1U) << " | " << scenario_name(scenario.scenario) << " | "
           << (scenario.passed ? "PASS" : "FAIL") << " | "
           << scenario.counters.source_events << " | "
           << scenario.counters.specialist_forecasts << " | "
           << scenario.counters.risk_approvals << "/"
           << scenario.counters.risk_rejections << " | "
           << scenario.counters.gateway_accepted << " | " << scenario.counters.fills
           << " | " << scenario.counters.final_absolute_position_units << " | `"
           << hex_hash(scenario.replay_outcome_hash) << "` |\n";
  }
  output << "\n## Scope\n\n"
         << "This is deterministic infrastructure and safety validation against the "
            "repository-owned synthetic protocol. It is not evidence of profitability, "
            "licensed-feed correctness, exchange certification, real-NIC latency, or "
            "authorization for live trading.\n";
  return write_atomically(path, output.str());
}

} // namespace aegis::integration
