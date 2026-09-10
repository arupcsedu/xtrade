#include "aegis/common/audit_envelope.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>

#include <flatbuffers/verifier.h>

namespace aegis::common {
namespace {

constexpr std::size_t kSizePrefixBytes = sizeof(flatbuffers::uoffset_t);
constexpr std::uint32_t kMaximumConfidencePpm = 1'000'000U;
constexpr std::int64_t kMaximumAbsoluteReturnPpm = 10'000'000LL;
constexpr std::uint64_t kMaximumVolatilityPpm = 10'000'000U;
constexpr std::int64_t kMaximumVolumeUnits = 1'000'000'000'000LL;
constexpr std::int64_t kMaximumSpreadTicks = 1'000'000'000LL;
constexpr std::size_t kMaximumSymbolBytes = 64U;
constexpr std::size_t kMaximumContributors = 64U;
constexpr std::size_t kMaximumEnsembleExperts = 16U;
constexpr std::uint64_t kMaximumAggregateCostPpm = 5'000'000U;
constexpr std::uint64_t kMaximumEffectiveUncertaintyPpm = 30'000'000U;
constexpr std::int64_t kMaximumNetRobustEdgePpm = 30'000'000LL;
constexpr std::size_t kMaximumIntelligenceEntities = 32U;
constexpr std::size_t kMaximumIntelligenceFacts = 32U;
constexpr std::size_t kMaximumIntelligenceEvidence = 64U;
constexpr std::size_t kMaximumIntelligenceTextBytes = 512U;

template <typename Identifier>
[[nodiscard]] bool valid_identifier(const Identifier* identifier) noexcept {
  return identifier != nullptr && (identifier->high() != 0U || identifier->low() != 0U);
}

template <typename Timestamp>
[[nodiscard]] bool positive_timestamp(const Timestamp* timestamp) noexcept {
  return timestamp != nullptr && timestamp->value() > 0;
}

template <typename Enum> [[nodiscard]] bool valid_enum(const Enum value) noexcept {
  return value > Enum::UNKNOWN && value <= Enum::MAX;
}

[[nodiscard]] bool valid_version(const wire::SchemaVersion* version) noexcept {
  return version != nullptr && version->major() == kCurrentSchemaMajor;
}

[[nodiscard]] bool nonzero_digest(const wire::Sha256Digest* digest) noexcept {
  if (digest == nullptr) {
    return false;
  }
  std::uint8_t aggregate = 0;
  for (std::size_t index = 0; index < digest->bytes()->size(); ++index) {
    aggregate = static_cast<std::uint8_t>(
        aggregate | digest->bytes()->Get(static_cast<flatbuffers::uoffset_t>(index)));
  }
  return aggregate != 0U;
}

[[nodiscard]] bool digest_matches(const wire::Sha256Digest* expected,
                                  const Sha256Digest& actual) noexcept {
  if (expected == nullptr) {
    return false;
  }
  std::uint8_t difference = 0;
  for (std::size_t index = 0; index < actual.size(); ++index) {
    difference = static_cast<std::uint8_t>(
        difference |
        (expected->bytes()->Get(static_cast<flatbuffers::uoffset_t>(index)) ^
         actual[index]));
  }
  return difference == 0U;
}

template <typename MarketRecord>
[[nodiscard]] ValidationResult
validate_market_metadata(const MarketRecord* record) noexcept {
  if (record == nullptr) {
    return {ValidationError::missing_payload};
  }
  if (!valid_version(record->schema_version())) {
    return {ValidationError::unsupported_schema_major};
  }
  if (!valid_identifier(record->global_event_id()) ||
      !valid_identifier(record->session_id()) ||
      !valid_identifier(record->venue_id()) ||
      !valid_identifier(record->instrument_id()) ||
      !valid_identifier(record->channel_id())) {
    return {ValidationError::invalid_identifier};
  }
  if (record->channel_sequence() == 0U) {
    return {ValidationError::invalid_value};
  }
  if (!positive_timestamp(record->exchange_event_time()) ||
      !positive_timestamp(record->nic_receive_time()) ||
      !positive_timestamp(record->process_monotonic_time())) {
    return {ValidationError::invalid_timestamp};
  }
  return {};
}

[[nodiscard]] ValidationResult
validate_instrument_reference(const wire::InstrumentReferenceData* record) noexcept {
  if (record == nullptr) {
    return {ValidationError::missing_payload};
  }
  if (!valid_version(record->schema_version())) {
    return {ValidationError::unsupported_schema_major};
  }
  if (!valid_identifier(record->instrument_id()) ||
      !valid_identifier(record->venue_id()) ||
      !valid_identifier(record->reference_data_version())) {
    return {ValidationError::invalid_identifier};
  }
  const auto* symbol = record->symbol();
  const auto* currency_code = record->currency_code();
  if (symbol == nullptr || symbol->empty() || symbol->size() > kMaximumSymbolBytes ||
      currency_code == nullptr || currency_code->size() != 3U) {
    return {ValidationError::invalid_text};
  }
  for (const char character : currency_code->string_view()) {
    if (character < 'A' || character > 'Z') {
      return {ValidationError::invalid_text};
    }
  }
  if (record->price_unit() != wire::PriceUnit::TICKS ||
      record->quantity_unit() != wire::QuantityUnit::INSTRUMENT_UNITS) {
    return {ValidationError::invalid_unit};
  }
  if (record->tick_value_currency_nanos() <= 0 ||
      record->lot_size_quantity_units() == 0U ||
      record->minimum_order_quantity_units() == 0U ||
      record->maximum_order_quantity_units() < record->minimum_order_quantity_units()) {
    return {ValidationError::invalid_quantity};
  }
  if (!positive_timestamp(record->valid_from_exchange_event_time())) {
    return {ValidationError::invalid_timestamp};
  }
  return {};
}

[[nodiscard]] ValidationResult
validate_data_quality(const wire::DataQualityState* record) noexcept {
  if (record == nullptr) {
    return {ValidationError::missing_payload};
  }
  if (!valid_version(record->schema_version())) {
    return {ValidationError::unsupported_schema_major};
  }
  if (!valid_identifier(record->venue_id()) ||
      !valid_identifier(record->channel_id())) {
    return {ValidationError::invalid_identifier};
  }
  if (!valid_enum(record->state())) {
    return {ValidationError::invalid_enum};
  }
  if (!positive_timestamp(record->observed_process_monotonic_time()) ||
      !positive_timestamp(record->last_good_exchange_event_time()) ||
      !positive_timestamp(record->last_good_nic_receive_time())) {
    return {ValidationError::invalid_timestamp};
  }
  if (record->stale_after_ns() == 0U) {
    return {ValidationError::invalid_relationship};
  }
  return {};
}

[[nodiscard]] ValidationResult
validate_clock_quality(const wire::ClockQualityState* record) noexcept {
  if (record == nullptr) {
    return {ValidationError::missing_payload};
  }
  if (!valid_version(record->schema_version())) {
    return {ValidationError::unsupported_schema_major};
  }
  if (!valid_identifier(record->source_channel_id())) {
    return {ValidationError::invalid_identifier};
  }
  if (!valid_enum(record->state())) {
    return {ValidationError::invalid_enum};
  }
  if (!positive_timestamp(record->observed_process_monotonic_time()) ||
      !positive_timestamp(record->observed_wall_clock_utc_time())) {
    return {ValidationError::invalid_timestamp};
  }
  if (record->maximum_permitted_uncertainty_ns() == 0U ||
      (record->state() == wire::ClockQualityCode::SYNCHRONIZED &&
       record->uncertainty_ns() > record->maximum_permitted_uncertainty_ns())) {
    return {ValidationError::invalid_relationship};
  }
  return {};
}

[[nodiscard]] ValidationResult
validate_book_update(const wire::BookUpdate* record) noexcept {
  const auto metadata = validate_market_metadata(record);
  if (!metadata.ok()) {
    return metadata;
  }
  if (!valid_enum(record->side()) || !valid_enum(record->action())) {
    return {ValidationError::invalid_enum};
  }
  if (record->price_unit() != wire::PriceUnit::TICKS ||
      record->quantity_unit() != wire::QuantityUnit::INSTRUMENT_UNITS) {
    return {ValidationError::invalid_unit};
  }
  if ((record->action() == wire::BookAction::ADD ||
       record->action() == wire::BookAction::CHANGE) &&
      record->quantity_units() == 0U) {
    return {ValidationError::invalid_quantity};
  }
  return {};
}

[[nodiscard]] ValidationResult validate_trade(const wire::TradeEvent* record) noexcept {
  const auto metadata = validate_market_metadata(record);
  if (!metadata.ok()) {
    return metadata;
  }
  if (!valid_enum(record->aggressor_side())) {
    return {ValidationError::invalid_enum};
  }
  if (record->price_unit() != wire::PriceUnit::TICKS ||
      record->quantity_unit() != wire::QuantityUnit::INSTRUMENT_UNITS) {
    return {ValidationError::invalid_unit};
  }
  if (record->quantity_units() == 0U || record->venue_trade_id() == 0U) {
    return {ValidationError::invalid_quantity};
  }
  return {};
}

[[nodiscard]] ValidationResult validate_quote(const wire::QuoteEvent* record) noexcept {
  const auto metadata = validate_market_metadata(record);
  if (!metadata.ok()) {
    return metadata;
  }
  if (record->price_unit() != wire::PriceUnit::TICKS ||
      record->quantity_unit() != wire::QuantityUnit::INSTRUMENT_UNITS) {
    return {ValidationError::invalid_unit};
  }
  if (record->bid_quantity_units() == 0U || record->ask_quantity_units() == 0U) {
    return {ValidationError::invalid_quantity};
  }
  if (record->bid_price_ticks() > record->ask_price_ticks()) {
    return {ValidationError::invalid_relationship};
  }
  return {};
}

[[nodiscard]] ValidationResult
validate_auction(const wire::AuctionImbalance* record) noexcept {
  const auto metadata = validate_market_metadata(record);
  if (!metadata.ok()) {
    return metadata;
  }
  if (!valid_enum(record->imbalance_side())) {
    return {ValidationError::invalid_enum};
  }
  if (record->price_unit() != wire::PriceUnit::TICKS ||
      record->quantity_unit() != wire::QuantityUnit::INSTRUMENT_UNITS) {
    return {ValidationError::invalid_unit};
  }
  if (record->imbalance_side() != wire::AuctionSide::PAIRED &&
      record->imbalance_quantity_units() == 0U) {
    return {ValidationError::invalid_quantity};
  }
  return {};
}

[[nodiscard]] ValidationResult
validate_trading_status(const wire::TradingStatus* record) noexcept {
  const auto metadata = validate_market_metadata(record);
  if (!metadata.ok()) {
    return metadata;
  }
  if (!valid_enum(record->status())) {
    return {ValidationError::invalid_enum};
  }
  return {};
}

[[nodiscard]] bool same_identifier(const auto* left, const auto* right) noexcept {
  return left != nullptr && right != nullptr && left->high() == right->high() &&
         left->low() == right->low();
}

template <typename Inner>
[[nodiscard]] ValidationResult
validate_market_event_relationships(const wire::MarketEvent* outer, const Inner* inner,
                                    const ValidationResult inner_validation) noexcept {
  if (!inner_validation.ok()) {
    return inner_validation;
  }
  if (outer == nullptr || inner == nullptr) {
    return {ValidationError::missing_payload};
  }
  const auto* outer_exchange_time = outer->exchange_event_time();
  const auto* inner_exchange_time = inner->exchange_event_time();
  const auto* outer_nic_time = outer->nic_receive_time();
  const auto* inner_nic_time = inner->nic_receive_time();
  const auto* outer_monotonic_time = outer->process_monotonic_time();
  const auto* inner_monotonic_time = inner->process_monotonic_time();
  if (outer_exchange_time == nullptr || inner_exchange_time == nullptr ||
      outer_nic_time == nullptr || inner_nic_time == nullptr ||
      outer_monotonic_time == nullptr || inner_monotonic_time == nullptr) {
    return {ValidationError::invalid_timestamp};
  }
  if (!same_identifier(outer->global_event_id(), inner->global_event_id()) ||
      !same_identifier(outer->session_id(), inner->session_id()) ||
      !same_identifier(outer->venue_id(), inner->venue_id()) ||
      !same_identifier(outer->instrument_id(), inner->instrument_id()) ||
      !same_identifier(outer->channel_id(), inner->channel_id()) ||
      outer->channel_sequence() != inner->channel_sequence() ||
      outer_exchange_time->value() != inner_exchange_time->value() ||
      outer_nic_time->value() != inner_nic_time->value() ||
      outer_monotonic_time->value() != inner_monotonic_time->value()) {
    return {ValidationError::invalid_relationship};
  }
  return {};
}

[[nodiscard]] ValidationResult
validate_market_event(const wire::MarketEvent* record) noexcept {
  const auto metadata = validate_market_metadata(record);
  if (!metadata.ok()) {
    return metadata;
  }
  const auto data_quality = validate_data_quality(record->data_quality_state());
  if (!data_quality.ok()) {
    return data_quality;
  }
  const auto clock_quality = validate_clock_quality(record->clock_quality_state());
  if (!clock_quality.ok()) {
    return clock_quality;
  }

  switch (record->market_payload_type()) {
  case wire::MarketPayload::BookUpdate:
    return validate_market_event_relationships(
        record, record->market_payload_as_BookUpdate(),
        validate_book_update(record->market_payload_as_BookUpdate()));
  case wire::MarketPayload::TradeEvent:
    return validate_market_event_relationships(
        record, record->market_payload_as_TradeEvent(),
        validate_trade(record->market_payload_as_TradeEvent()));
  case wire::MarketPayload::QuoteEvent:
    return validate_market_event_relationships(
        record, record->market_payload_as_QuoteEvent(),
        validate_quote(record->market_payload_as_QuoteEvent()));
  case wire::MarketPayload::AuctionImbalance:
    return validate_market_event_relationships(
        record, record->market_payload_as_AuctionImbalance(),
        validate_auction(record->market_payload_as_AuctionImbalance()));
  case wire::MarketPayload::TradingStatus:
    return validate_market_event_relationships(
        record, record->market_payload_as_TradingStatus(),
        validate_trading_status(record->market_payload_as_TradingStatus()));
  case wire::MarketPayload::NONE:
  default:
    return {ValidationError::invalid_enum};
  }
}

[[nodiscard]] ValidationResult
validate_feature_snapshot(const wire::FeatureSnapshotMetadata* record) noexcept {
  if (record == nullptr) {
    return {ValidationError::missing_payload};
  }
  if (!valid_version(record->schema_version())) {
    return {ValidationError::unsupported_schema_major};
  }
  if (!valid_identifier(record->feature_snapshot_id()) ||
      !valid_identifier(record->session_id()) ||
      !valid_identifier(record->instrument_id()) ||
      !valid_identifier(record->feature_definition_version()) ||
      !valid_identifier(record->source_first_global_event_id()) ||
      !valid_identifier(record->source_last_global_event_id())) {
    return {ValidationError::invalid_identifier};
  }
  if (!positive_timestamp(record->as_of_exchange_event_time()) ||
      !positive_timestamp(record->created_process_monotonic_time())) {
    return {ValidationError::invalid_timestamp};
  }
  if (record->feature_count() == 0U ||
      !nonzero_digest(record->feature_payload_sha256())) {
    return {ValidationError::invalid_value};
  }
  return {};
}

[[nodiscard]] ValidationResult
validate_model_forecast_v1_1(const wire::ModelForecast* record) noexcept {
  if (record->forecast_unit() != wire::ForecastUnit::RETURN_PPM ||
      record->forecast_value() != record->expected_return_ppm()) {
    return {ValidationError::invalid_unit};
  }
  if (!positive_timestamp(record->as_of_exchange_event_time())) {
    return {ValidationError::invalid_timestamp};
  }
  if (record->expected_return_ppm() < -kMaximumAbsoluteReturnPpm ||
      record->expected_return_ppm() > kMaximumAbsoluteReturnPpm ||
      record->return_p10_ppm() < -kMaximumAbsoluteReturnPpm ||
      record->return_p90_ppm() > kMaximumAbsoluteReturnPpm ||
      record->return_p10_ppm() > record->return_p50_ppm() ||
      record->return_p50_ppm() > record->return_p90_ppm() ||
      record->volatility_ppm() > kMaximumVolatilityPpm) {
    return {ValidationError::invalid_value};
  }
  const auto probability_sum =
      static_cast<std::uint64_t>(record->probability_down_ppm()) +
      static_cast<std::uint64_t>(record->probability_flat_ppm()) +
      static_cast<std::uint64_t>(record->probability_up_ppm());
  if (record->probability_down_ppm() > kMaximumConfidencePpm ||
      record->probability_flat_ppm() > kMaximumConfidencePpm ||
      record->probability_up_ppm() > kMaximumConfidencePpm ||
      probability_sum != kMaximumConfidencePpm ||
      record->calibration_score_ppm() > kMaximumConfidencePpm ||
      record->data_quality_score_ppm() > kMaximumConfidencePpm ||
      record->ood_score_ppm() > kMaximumConfidencePpm) {
    return {ValidationError::invalid_value};
  }
  if ((!record->has_transaction_cost_estimate() &&
       (record->estimated_spread_cost_ppm() != 0U ||
        record->estimated_slippage_cost_ppm() != 0U ||
        record->estimated_market_impact_ppm() != 0U ||
        record->estimated_adverse_selection_cost_ppm() != 0U ||
        record->estimated_fee_cost_ppm() != 0U)) ||
      record->estimated_spread_cost_ppm() > kMaximumConfidencePpm ||
      record->estimated_slippage_cost_ppm() > kMaximumConfidencePpm ||
      record->estimated_market_impact_ppm() > kMaximumConfidencePpm ||
      record->estimated_adverse_selection_cost_ppm() > kMaximumConfidencePpm ||
      record->estimated_fee_cost_ppm() > kMaximumConfidencePpm) {
    return {ValidationError::invalid_value};
  }
  return {};
}

[[nodiscard]] bool valid_forecast_costs(const wire::ModelForecast* record) noexcept {
  const bool all_costs_zero = record->estimated_spread_cost_ppm() == 0U &&
                              record->estimated_slippage_cost_ppm() == 0U &&
                              record->estimated_market_impact_ppm() == 0U &&
                              record->estimated_adverse_selection_cost_ppm() == 0U &&
                              record->estimated_fee_cost_ppm() == 0U;
  return (record->has_transaction_cost_estimate() || all_costs_zero) &&
         record->estimated_spread_cost_ppm() <= kMaximumConfidencePpm &&
         record->estimated_slippage_cost_ppm() <= kMaximumConfidencePpm &&
         record->estimated_market_impact_ppm() <= kMaximumConfidencePpm &&
         record->estimated_adverse_selection_cost_ppm() <= kMaximumConfidencePpm &&
         record->estimated_fee_cost_ppm() <= kMaximumConfidencePpm;
}

[[nodiscard]] bool
neutral_non_return_fields(const wire::ModelForecast* record) noexcept {
  return record->expected_return_ppm() == 0 && record->return_p10_ppm() == 0 &&
         record->return_p50_ppm() == 0 && record->return_p90_ppm() == 0 &&
         record->probability_down_ppm() == 0U &&
         record->probability_flat_ppm() == kMaximumConfidencePpm &&
         record->probability_up_ppm() == 0U;
}

[[nodiscard]] bool valid_target_unit(const wire::ForecastTarget target,
                                     const wire::ForecastUnit unit) noexcept {
  switch (target) {
  case wire::ForecastTarget::RETURN:
    return unit == wire::ForecastUnit::RETURN_PPM;
  case wire::ForecastTarget::REALIZED_VOLATILITY:
    return unit == wire::ForecastUnit::VOLATILITY_PPM;
  case wire::ForecastTarget::VOLUME:
    return unit == wire::ForecastUnit::VOLUME_UNITS;
  case wire::ForecastTarget::SPREAD:
    return unit == wire::ForecastUnit::SPREAD_TICKS;
  case wire::ForecastTarget::MARKET_FACTOR:
  case wire::ForecastTarget::SECTOR_FACTOR:
    return unit == wire::ForecastUnit::FACTOR_PPM;
  case wire::ForecastTarget::UNKNOWN:
    return false;
  }
  return false;
}

[[nodiscard]] bool valid_target_value(const wire::ForecastTarget target,
                                      const std::int64_t value) noexcept {
  switch (target) {
  case wire::ForecastTarget::RETURN:
  case wire::ForecastTarget::MARKET_FACTOR:
  case wire::ForecastTarget::SECTOR_FACTOR:
    return value >= -kMaximumAbsoluteReturnPpm && value <= kMaximumAbsoluteReturnPpm;
  case wire::ForecastTarget::REALIZED_VOLATILITY:
    return value >= 0 && std::cmp_less_equal(value, kMaximumVolatilityPpm);
  case wire::ForecastTarget::VOLUME:
    return value >= 0 && value <= kMaximumVolumeUnits;
  case wire::ForecastTarget::SPREAD:
    return value >= 0 && value <= kMaximumSpreadTicks;
  case wire::ForecastTarget::UNKNOWN:
    return false;
  }
  return false;
}

[[nodiscard]] ValidationResult
validate_model_forecast_v1_3(const wire::ModelForecast* record) noexcept {
  if (!valid_enum(record->forecast_target()) ||
      !valid_target_unit(record->forecast_target(), record->forecast_unit()) ||
      record->forecast_value() != record->target_value() ||
      record->target_p10() > record->target_p50() ||
      record->target_p50() > record->target_p90() ||
      !valid_target_value(record->forecast_target(), record->target_value()) ||
      !valid_target_value(record->forecast_target(), record->target_p10()) ||
      !valid_target_value(record->forecast_target(), record->target_p50()) ||
      !valid_target_value(record->forecast_target(), record->target_p90())) {
    return {ValidationError::invalid_value};
  }
  if (record->forecast_target() == wire::ForecastTarget::RETURN) {
    if (record->target_value() != record->expected_return_ppm() ||
        record->target_p10() != record->return_p10_ppm() ||
        record->target_p50() != record->return_p50_ppm() ||
        record->target_p90() != record->return_p90_ppm()) {
      return {ValidationError::invalid_relationship};
    }
    return validate_model_forecast_v1_1(record);
  }
  if (!neutral_non_return_fields(record)) {
    return {ValidationError::invalid_relationship};
  }
  if (!positive_timestamp(record->as_of_exchange_event_time())) {
    return {ValidationError::invalid_timestamp};
  }
  if (record->volatility_ppm() > kMaximumVolatilityPpm ||
      record->confidence_ppm() > kMaximumConfidencePpm ||
      record->calibration_score_ppm() > kMaximumConfidencePpm ||
      record->data_quality_score_ppm() > kMaximumConfidencePpm ||
      record->ood_score_ppm() > kMaximumConfidencePpm ||
      !valid_forecast_costs(record)) {
    return {ValidationError::invalid_value};
  }
  return {};
}

[[nodiscard]] ValidationResult
validate_model_forecast_v1_9(const wire::ModelForecast* record) noexcept {
  const auto* spec = record->horizon_spec();
  const auto* target = record->target_exchange_event_time();
  const auto* as_of = record->as_of_exchange_event_time();
  if (spec == nullptr) {
    return {ValidationError::missing_payload};
  }
  if (!valid_enum(spec->unit()) || !valid_enum(spec->halt_policy()) ||
      !valid_enum(spec->session_endpoint())) {
    return {ValidationError::invalid_enum};
  }
  if (spec->value() == 0U || !positive_timestamp(target) ||
      !positive_timestamp(as_of) || target->value() <= as_of->value() ||
      static_cast<std::uint64_t>(target->value() - as_of->value()) !=
          record->horizon_ns()) {
    return {ValidationError::invalid_relationship};
  }

  switch (spec->unit()) {
  case wire::ForecastHorizonUnit::ELAPSED_NANOSECONDS:
    if (spec->value() != record->horizon_ns() ||
        spec->halt_policy() != wire::HorizonHaltPolicy::NOT_APPLICABLE ||
        spec->session_endpoint() != wire::HorizonSessionEndpoint::NOT_APPLICABLE ||
        spec->calendar_version() != nullptr) {
      return {ValidationError::invalid_relationship};
    }
    return {};
  case wire::ForecastHorizonUnit::TRADING_MINUTES:
    if (!valid_identifier(spec->calendar_version()) ||
        spec->halt_policy() == wire::HorizonHaltPolicy::NOT_APPLICABLE ||
        spec->session_endpoint() != wire::HorizonSessionEndpoint::NOT_APPLICABLE) {
      return {ValidationError::invalid_relationship};
    }
    return {};
  case wire::ForecastHorizonUnit::TRADING_SESSIONS:
    if (!valid_identifier(spec->calendar_version()) ||
        spec->halt_policy() == wire::HorizonHaltPolicy::NOT_APPLICABLE ||
        spec->session_endpoint() !=
            wire::HorizonSessionEndpoint::REGULAR_SESSION_CLOSE) {
      return {ValidationError::invalid_relationship};
    }
    return {};
  case wire::ForecastHorizonUnit::UNKNOWN:
    return {ValidationError::invalid_enum};
  }
  return {ValidationError::invalid_enum};
}

[[nodiscard]] ValidationResult
validate_model_forecast(const wire::ModelForecast* record) noexcept {
  if (record == nullptr) {
    return {ValidationError::missing_payload};
  }
  if (!valid_version(record->schema_version())) {
    return {ValidationError::unsupported_schema_major};
  }
  if (!valid_identifier(record->forecast_id()) ||
      !valid_identifier(record->session_id()) ||
      !valid_identifier(record->model_id()) ||
      !valid_identifier(record->model_version()) ||
      !valid_identifier(record->instrument_id()) ||
      !valid_identifier(record->feature_snapshot_id()) ||
      !valid_identifier(record->configuration_version())) {
    return {ValidationError::invalid_identifier};
  }
  if (!valid_enum(record->forecast_unit())) {
    return {ValidationError::invalid_enum};
  }
  if (record->confidence_ppm() > kMaximumConfidencePpm || record->horizon_ns() == 0U) {
    return {ValidationError::invalid_value};
  }
  if (!positive_timestamp(record->created_process_monotonic_time()) ||
      !positive_timestamp(record->valid_until_process_monotonic_time()) ||
      record->valid_until_process_monotonic_time()->value() <=
          record->created_process_monotonic_time()->value()) {
    return {ValidationError::invalid_timestamp};
  }
  if (record->forecast_unit() == wire::ForecastUnit::PROBABILITY_PPM &&
      (record->forecast_value() < 0 ||
       record->forecast_value() > kMaximumConfidencePpm)) {
    return {ValidationError::invalid_value};
  }
  if (record->schema_version()->minor() >= 3U) {
    const auto base = validate_model_forecast_v1_3(record);
    if (!base.ok() || record->schema_version()->minor() < 9U) {
      return base;
    }
    return validate_model_forecast_v1_9(record);
  }
  if (record->schema_version()->minor() >= 1U) {
    return validate_model_forecast_v1_1(record);
  }
  return {};
}

[[nodiscard]] ValidationResult
validate_ensemble_distribution_v1_5(const wire::EnsembleForecast* record) noexcept {
  const auto probability_sum =
      static_cast<std::uint64_t>(record->probability_down_ppm()) +
      record->probability_flat_ppm() + record->probability_up_ppm();
  if (record->forecast_unit() != wire::ForecastUnit::RETURN_PPM ||
      record->forecast_value() != record->expected_return_ppm()) {
    return {ValidationError::invalid_unit};
  }
  if (record->expected_return_ppm() < -kMaximumAbsoluteReturnPpm ||
      record->expected_return_ppm() > kMaximumAbsoluteReturnPpm ||
      record->return_p10_ppm() < -kMaximumAbsoluteReturnPpm ||
      record->return_p90_ppm() > kMaximumAbsoluteReturnPpm ||
      record->return_p10_ppm() > record->return_p50_ppm() ||
      record->return_p50_ppm() > record->return_p90_ppm() ||
      probability_sum != kMaximumConfidencePpm ||
      record->probability_down_ppm() > kMaximumConfidencePpm ||
      record->probability_flat_ppm() > kMaximumConfidencePpm ||
      record->probability_up_ppm() > kMaximumConfidencePpm ||
      record->effective_uncertainty_ppm() > kMaximumEffectiveUncertaintyPpm ||
      record->disagreement_ppm() > kMaximumEffectiveUncertaintyPpm ||
      record->estimated_transaction_cost_ppm() > kMaximumAggregateCostPpm ||
      record->uncertainty_penalty_ppm() > kMaximumAggregateCostPpm ||
      record->safety_margin_ppm() > kMaximumAggregateCostPpm ||
      record->net_robust_edge_ppm() < -kMaximumNetRobustEdgePpm ||
      record->net_robust_edge_ppm() > kMaximumNetRobustEdgePpm ||
      record->ensemble_hash() == 0U) {
    return {ValidationError::invalid_value};
  }
  return {};
}

using EnsembleWeightVector = flatbuffers::Vector<const wire::EnsembleModelWeight*>;
using ForecastIdVector = flatbuffers::Vector<const wire::ForecastId*>;

struct EnsembleWeightSummary {
  std::uint64_t weight_sum{};
  std::uint32_t eligible_count{};
  std::uint32_t contributing_count{};
};

[[nodiscard]] ValidationResult validate_ensemble_explanation_header(
    const wire::EnsembleForecast* record,
    const wire::EnsembleDecisionExplanation* explanation,
    const EnsembleWeightVector* weights,
    const ForecastIdVector* contributors) noexcept {
  if (!valid_enum(explanation->gate_kind()) ||
      !valid_enum(explanation->market_state()) ||
      !valid_enum(explanation->event_state()) ||
      !valid_enum(explanation->dominant_reason()) ||
      !valid_enum(record->primary_reason())) {
    return {ValidationError::invalid_enum};
  }
  if (explanation->input_data_quality_ppm() > kMaximumConfidencePpm ||
      explanation->supplied_expert_count() > kMaximumEnsembleExperts ||
      explanation->eligible_expert_count() > explanation->supplied_expert_count() ||
      explanation->contributing_expert_count() > explanation->eligible_expert_count() ||
      weights->size() != explanation->supplied_expert_count() ||
      contributors->size() != explanation->contributing_expert_count() ||
      explanation->market_state_snapshot_hash() == 0U ||
      explanation->configuration_hash() == 0U ||
      explanation->explanation_hash() == 0U ||
      (record->reason_mask() & (std::uint64_t{1U} << static_cast<std::uint8_t>(
                                    record->primary_reason()))) == 0U) {
    return {ValidationError::invalid_value};
  }
  return {};
}

[[nodiscard]] ValidationResult
validate_positive_ensemble_weight(const wire::EnsembleModelWeight* weight,
                                  const ForecastIdVector* contributors,
                                  const std::size_t contributor_index) noexcept {
  if (weight->eligibility() != wire::EnsembleEligibilityCode::ELIGIBLE ||
      !valid_identifier(&weight->forecast_id()) ||
      !valid_identifier(&weight->model_id()) ||
      !valid_identifier(&weight->model_version()) || weight->forecast_hash() == 0U ||
      contributor_index >= contributors->size()) {
    return {ValidationError::invalid_identifier};
  }
  const auto* contributor =
      contributors->Get(static_cast<flatbuffers::uoffset_t>(contributor_index));
  if (contributor == nullptr || contributor->high() != weight->forecast_id().high() ||
      contributor->low() != weight->forecast_id().low()) {
    return {ValidationError::invalid_relationship};
  }
  return {};
}

[[nodiscard]] ValidationResult
validate_ensemble_weights(const EnsembleWeightVector* weights,
                          const ForecastIdVector* contributors,
                          EnsembleWeightSummary& summary) noexcept {
  std::size_t contributor_index{};
  for (const auto* weight : *weights) {
    if (weight == nullptr || !valid_enum(weight->role()) ||
        !valid_enum(weight->eligibility()) ||
        weight->weight_ppm() > kMaximumConfidencePpm ||
        weight->freshness_ppm() > kMaximumConfidencePpm) {
      return {ValidationError::invalid_value};
    }
    summary.eligible_count +=
        weight->eligibility() == wire::EnsembleEligibilityCode::ELIGIBLE ? 1U : 0U;
    if (weight->weight_ppm() != 0U) {
      const auto positive =
          validate_positive_ensemble_weight(weight, contributors, contributor_index);
      if (!positive.ok()) {
        return positive;
      }
      ++contributor_index;
      ++summary.contributing_count;
    }
    summary.weight_sum += weight->weight_ppm();
  }
  return {};
}

[[nodiscard]] ValidationResult
validate_ensemble_weight_summary(const wire::EnsembleDecisionExplanation* explanation,
                                 const EnsembleWeightSummary& summary) noexcept {
  const auto expected_weight_sum =
      summary.contributing_count == 0U ? std::uint64_t{0U} : kMaximumConfidencePpm;
  if (summary.eligible_count != explanation->eligible_expert_count() ||
      summary.contributing_count != explanation->contributing_expert_count() ||
      summary.weight_sum != expected_weight_sum) {
    return {ValidationError::invalid_relationship};
  }
  return {};
}

[[nodiscard]] ValidationResult
validate_ensemble_dominant(const wire::EnsembleForecast* record,
                           const wire::EnsembleDecisionExplanation* explanation,
                           const std::uint32_t contributing_count) noexcept {
  if (contributing_count == 0U) {
    return !record->abstain() ||
                   explanation->dominant_reason() !=
                       wire::EnsembleDominantExpertReasonCode::NO_ELIGIBLE_EXPERT
               ? ValidationResult{ValidationError::invalid_relationship}
               : ValidationResult{};
  }
  if (!valid_identifier(explanation->dominant_forecast_id()) ||
      !valid_identifier(explanation->dominant_model_id()) ||
      !valid_identifier(explanation->dominant_model_version())) {
    return {ValidationError::invalid_identifier};
  }
  return explanation->dominant_reason() ==
                 wire::EnsembleDominantExpertReasonCode::NO_ELIGIBLE_EXPERT
             ? ValidationResult{ValidationError::invalid_relationship}
             : ValidationResult{};
}

[[nodiscard]] ValidationResult
validate_ensemble_edge(const wire::EnsembleForecast* record,
                       const wire::EnsembleDecisionExplanation* explanation) noexcept {
  const auto reason_is_combined =
      record->primary_reason() == wire::EnsembleDecisionReasonCode::COMBINED;
  if (record->abstain() == reason_is_combined) {
    return {ValidationError::invalid_relationship};
  }
  const auto required_edge = record->estimated_transaction_cost_ppm() +
                             record->uncertainty_penalty_ppm() +
                             record->safety_margin_ppm();
  const auto absolute_return = static_cast<std::uint64_t>(
      record->expected_return_ppm() < 0 ? -record->expected_return_ppm()
                                        : record->expected_return_ppm());
  std::int64_t expected_net_robust_edge{};
  if (record->expected_return_ppm() > 0) {
    expected_net_robust_edge =
        record->expected_return_ppm() - static_cast<std::int64_t>(required_edge);
  } else if (record->expected_return_ppm() < 0) {
    expected_net_robust_edge =
        record->expected_return_ppm() + static_cast<std::int64_t>(required_edge);
  }
  if (required_edge != explanation->required_edge_ppm() ||
      record->net_robust_edge_ppm() != expected_net_robust_edge ||
      record->abstain() != (absolute_return <= required_edge)) {
    return {ValidationError::invalid_relationship};
  }
  return {};
}

[[nodiscard]] ValidationResult
validate_ensemble_explanation_v1_5(const wire::EnsembleForecast* record) noexcept {
  const auto* explanation = record->decision_explanation();
  const auto* weights = record->model_weights();
  const auto* contributors = record->contributing_forecast_ids();
  if (explanation == nullptr || weights == nullptr || contributors == nullptr) {
    return {ValidationError::missing_payload};
  }
  const auto header =
      validate_ensemble_explanation_header(record, explanation, weights, contributors);
  if (!header.ok()) {
    return header;
  }
  if ((explanation->gate_kind() == wire::EnsembleGateKind::QUANTIZED_LEARNED) !=
      (explanation->learned_gate_signature_hash() != 0U)) {
    return {ValidationError::invalid_relationship};
  }
  EnsembleWeightSummary summary{};
  const auto weights_valid = validate_ensemble_weights(weights, contributors, summary);
  if (!weights_valid.ok()) {
    return weights_valid;
  }
  const auto summary_valid = validate_ensemble_weight_summary(explanation, summary);
  if (!summary_valid.ok()) {
    return summary_valid;
  }
  const auto dominant =
      validate_ensemble_dominant(record, explanation, summary.contributing_count);
  return dominant.ok() ? validate_ensemble_edge(record, explanation) : dominant;
}

[[nodiscard]] ValidationResult
validate_ensemble_forecast(const wire::EnsembleForecast* record) noexcept {
  if (record == nullptr) {
    return {ValidationError::missing_payload};
  }
  if (!valid_version(record->schema_version())) {
    return {ValidationError::unsupported_schema_major};
  }
  if (!valid_identifier(record->forecast_id()) ||
      !valid_identifier(record->session_id()) ||
      !valid_identifier(record->instrument_id()) ||
      !valid_identifier(record->ensemble_model_id()) ||
      !valid_identifier(record->ensemble_model_version()) ||
      !valid_identifier(record->configuration_version())) {
    return {ValidationError::invalid_identifier};
  }
  if (!valid_enum(record->forecast_unit())) {
    return {ValidationError::invalid_enum};
  }
  if (record->confidence_ppm() > kMaximumConfidencePpm || record->horizon_ns() == 0U ||
      record->contributing_forecast_ids() == nullptr ||
      record->contributing_forecast_ids()->size() > kMaximumContributors) {
    return {ValidationError::invalid_value};
  }
  for (const auto* identifier : *record->contributing_forecast_ids()) {
    if (!valid_identifier(identifier)) {
      return {ValidationError::invalid_identifier};
    }
  }
  if (!positive_timestamp(record->created_process_monotonic_time()) ||
      !positive_timestamp(record->valid_until_process_monotonic_time()) ||
      record->valid_until_process_monotonic_time()->value() <=
          record->created_process_monotonic_time()->value()) {
    return {ValidationError::invalid_timestamp};
  }
  if (record->schema_version()->minor() >= 5U) {
    const auto distribution = validate_ensemble_distribution_v1_5(record);
    return distribution.ok() ? validate_ensemble_explanation_v1_5(record)
                             : distribution;
  }
  if (record->contributing_forecast_ids()->empty()) {
    return {ValidationError::invalid_value};
  }
  return {};
}

[[nodiscard]] ValidationResult
validate_order_intent(const wire::OrderIntent* record) noexcept {
  if (record == nullptr) {
    return {ValidationError::missing_payload};
  }
  if (!valid_version(record->schema_version())) {
    return {ValidationError::unsupported_schema_major};
  }
  if (!valid_identifier(record->intent_id()) ||
      !valid_identifier(record->session_id()) ||
      !valid_identifier(record->strategy_id()) ||
      !valid_identifier(record->instrument_id()) ||
      !valid_identifier(record->configuration_version())) {
    return {ValidationError::invalid_identifier};
  }
  if (!valid_enum(record->action())) {
    return {ValidationError::invalid_enum};
  }
  if (record->price_unit() != wire::PriceUnit::TICKS ||
      record->quantity_unit() != wire::QuantityUnit::INSTRUMENT_UNITS) {
    return {ValidationError::invalid_unit};
  }
  if (!positive_timestamp(record->created_process_monotonic_time()) ||
      !positive_timestamp(record->expire_process_monotonic_time()) ||
      record->expire_process_monotonic_time()->value() <=
          record->created_process_monotonic_time()->value()) {
    return {ValidationError::invalid_timestamp};
  }
  if (record->schema_version()->minor() >= 6U &&
      (!valid_identifier(record->venue_id()) ||
       !valid_identifier(record->account_id()) || record->intent_hash() == 0U)) {
    return {ValidationError::invalid_identifier};
  }
  if (record->action() == wire::IntentAction::CANCEL) {
    if (!valid_identifier(record->target_order_id()) ||
        record->quantity_units() != 0U || record->has_limit_price()) {
      return {ValidationError::invalid_relationship};
    }
    return {};
  }
  if (!valid_identifier(record->source_forecast_id()) ||
      !valid_identifier(record->feature_snapshot_id()) ||
      record->target_order_id() != nullptr || record->quantity_units() == 0U ||
      !record->has_limit_price()) {
    return {ValidationError::invalid_relationship};
  }
  return {};
}

[[nodiscard]] bool valid_risk_check_v1_6(const wire::RiskCheckCode check) noexcept {
  return check >= wire::RiskCheckCode::NONE && check <= wire::RiskCheckCode::MAX;
}

[[nodiscard]] ValidationResult
validate_risk_decision_v1_6(const wire::RiskDecision* record) noexcept {
  if (!valid_identifier(record->account_id()) ||
      !valid_identifier(record->strategy_id()) ||
      !valid_identifier(record->venue_id()) ||
      !valid_identifier(record->instrument_id())) {
    return {ValidationError::invalid_identifier};
  }
  if (!valid_enum(record->intent_action()) || !valid_enum(record->trading_mode()) ||
      !valid_risk_check_v1_6(record->failed_check())) {
    return {ValidationError::invalid_enum};
  }
  if (record->approved_price_unit() != wire::PriceUnit::TICKS ||
      record->intent_hash() == 0U || record->risk_snapshot_hash() == 0U ||
      record->risk_context_hash() == 0U || record->position_generation() == 0U ||
      record->authority_epoch() == 0U || record->policy_revision() == 0U ||
      record->evaluation_ordinal() == 0U || record->journal_sequence() == 0U ||
      record->decision_hash() == 0U) {
    return {ValidationError::invalid_value};
  }
  const auto reason_ordinal = static_cast<std::uint16_t>(record->reason());
  if (reason_ordinal >= 64U ||
      (record->reason_mask() & (std::uint64_t{1U} << reason_ordinal)) == 0U) {
    return {ValidationError::invalid_relationship};
  }
  const auto approved = record->decision() == wire::RiskDecisionCode::APPROVED;
  const auto cancel = record->intent_action() == wire::IntentAction::CANCEL;
  if (approved) {
    constexpr auto kAllRiskChecks = (std::uint32_t{1U} << 30U) - 1U;
    if (record->failed_check() != wire::RiskCheckCode::NONE ||
        record->completed_check_mask() != kAllRiskChecks ||
        (cancel && (record->approved_quantity_units() != 0U ||
                    record->approved_price_ticks() != 0)) ||
        (!cancel && (record->approved_quantity_units() == 0U ||
                     record->approved_price_ticks() <= 0))) {
      return {ValidationError::invalid_relationship};
    }
    return {};
  }
  if (record->approved_quantity_units() != 0U || record->approved_price_ticks() != 0 ||
      record->reason() == wire::RiskReasonCode::WITHIN_LIMITS) {
    return {ValidationError::invalid_relationship};
  }
  if (record->failed_check() != wire::RiskCheckCode::NONE) {
    const auto check_ordinal = static_cast<std::uint8_t>(record->failed_check());
    if ((record->completed_check_mask() &
         (std::uint32_t{1U} << (check_ordinal - 1U))) != 0U) {
      return {ValidationError::invalid_relationship};
    }
  }
  return {};
}

[[nodiscard]] ValidationResult
validate_risk_decision(const wire::RiskDecision* record) noexcept {
  if (record == nullptr) {
    return {ValidationError::missing_payload};
  }
  if (!valid_version(record->schema_version())) {
    return {ValidationError::unsupported_schema_major};
  }
  if (!valid_identifier(record->global_event_id()) ||
      !valid_identifier(record->intent_id()) ||
      !valid_identifier(record->session_id()) ||
      !valid_identifier(record->risk_snapshot_id()) ||
      !valid_identifier(record->configuration_version())) {
    return {ValidationError::invalid_identifier};
  }
  if (!valid_enum(record->decision()) || !valid_enum(record->reason())) {
    return {ValidationError::invalid_enum};
  }
  if (record->approved_quantity_unit() != wire::QuantityUnit::INSTRUMENT_UNITS) {
    return {ValidationError::invalid_unit};
  }
  if (!nonzero_digest(record->intent_sha256())) {
    return {ValidationError::invalid_value};
  }
  if (!positive_timestamp(record->evaluated_process_monotonic_time()) ||
      !positive_timestamp(record->valid_until_process_monotonic_time()) ||
      record->valid_until_process_monotonic_time()->value() <=
          record->evaluated_process_monotonic_time()->value()) {
    return {ValidationError::invalid_timestamp};
  }
  if ((record->decision() == wire::RiskDecisionCode::APPROVED) !=
      (record->reason() == wire::RiskReasonCode::WITHIN_LIMITS)) {
    return {ValidationError::invalid_relationship};
  }
  if (record->schema_version()->minor() >= 6U) {
    return validate_risk_decision_v1_6(record);
  }
  if ((record->decision() == wire::RiskDecisionCode::APPROVED) !=
      (record->approved_quantity_units() > 0U)) {
    return {ValidationError::invalid_relationship};
  }
  return {};
}

[[nodiscard]] ValidationResult
validate_order_event_v1_8(const wire::OrderEvent* record) noexcept {
  if (!valid_identifier(record->account_id()) ||
      !valid_identifier(record->strategy_id())) {
    return {ValidationError::invalid_identifier};
  }
  if (!valid_enum(record->input_kind()) || !valid_enum(record->source()) ||
      !valid_enum(record->outcome()) || !valid_enum(record->previous_state()) ||
      !valid_enum(record->current_state())) {
    return {ValidationError::invalid_enum};
  }
  const auto* client_order_id = record->client_order_id();
  if (client_order_id == nullptr || client_order_id->size() != 32U) {
    return {ValidationError::invalid_text};
  }
  for (const char character : client_order_id->string_view()) {
    const bool decimal_digit = character >= '0' && character <= '9';
    const bool lowercase_hex = character >= 'a' && character <= 'f';
    if (!decimal_digit && !lowercase_hex) {
      return {ValidationError::invalid_text};
    }
  }
  if (record->state_version() == 0U || record->exchange_session_epoch() == 0U ||
      record->active_fencing_token() == 0U || record->risk_decision_hash() == 0U ||
      record->journal_sequence() == 0U || record->journal_record_hash() == 0U ||
      record->snapshot_sequence() == 0U || record->snapshot_hash() == 0U ||
      record->order_event_hash() == 0U) {
    return {ValidationError::invalid_value};
  }
  return {};
}

[[nodiscard]] ValidationResult
validate_order_event(const wire::OrderEvent* record) noexcept {
  if (record == nullptr) {
    return {ValidationError::missing_payload};
  }
  if (!valid_version(record->schema_version())) {
    return {ValidationError::unsupported_schema_major};
  }
  if (!valid_identifier(record->global_event_id()) ||
      !valid_identifier(record->order_id()) || !valid_identifier(record->intent_id()) ||
      !valid_identifier(record->session_id()) ||
      !valid_identifier(record->venue_id()) ||
      !valid_identifier(record->instrument_id()) ||
      !valid_identifier(record->configuration_version())) {
    return {ValidationError::invalid_identifier};
  }
  if (!valid_enum(record->event())) {
    return {ValidationError::invalid_enum};
  }
  if (record->price_unit() != wire::PriceUnit::TICKS ||
      record->quantity_unit() != wire::QuantityUnit::INSTRUMENT_UNITS) {
    return {ValidationError::invalid_unit};
  }
  const auto order_quantity = record->order_quantity_units();
  const auto cumulative_fill = record->cumulative_fill_quantity_units();
  const auto remaining = record->remaining_quantity_units();
  if (order_quantity == 0U || cumulative_fill > order_quantity ||
      remaining > order_quantity || cumulative_fill > order_quantity - remaining) {
    return {ValidationError::invalid_quantity};
  }
  if (!positive_timestamp(record->process_monotonic_time())) {
    return {ValidationError::invalid_timestamp};
  }
  const auto has_exchange_time = record->exchange_event_time() != nullptr;
  const auto has_nic_time = record->nic_receive_time() != nullptr;
  if (has_exchange_time != has_nic_time ||
      (has_exchange_time && (!positive_timestamp(record->exchange_event_time()) ||
                             !positive_timestamp(record->nic_receive_time())))) {
    return {ValidationError::invalid_timestamp};
  }
  if (record->schema_version()->minor() >= 8U) {
    return validate_order_event_v1_8(record);
  }
  return {};
}

[[nodiscard]] ValidationResult validate_fill(const wire::FillEvent* record) {
  if (record == nullptr) {
    return {ValidationError::missing_payload};
  }
  if (!valid_version(record->schema_version())) {
    return {ValidationError::unsupported_schema_major};
  }
  if (!valid_identifier(record->global_event_id()) ||
      !valid_identifier(record->order_id()) || !valid_identifier(record->intent_id()) ||
      !valid_identifier(record->session_id()) ||
      !valid_identifier(record->venue_id()) ||
      !valid_identifier(record->instrument_id())) {
    return {ValidationError::invalid_identifier};
  }
  if (record->price_unit() != wire::PriceUnit::TICKS ||
      record->quantity_unit() != wire::QuantityUnit::INSTRUMENT_UNITS ||
      record->currency_unit() != wire::CurrencyUnit::CURRENCY_NANOS) {
    return {ValidationError::invalid_unit};
  }
  if (record->fill_quantity_units() == 0U || record->venue_trade_id() == 0U) {
    return {ValidationError::invalid_quantity};
  }
  if (!positive_timestamp(record->exchange_event_time()) ||
      !positive_timestamp(record->nic_receive_time()) ||
      !positive_timestamp(record->process_monotonic_time())) {
    return {ValidationError::invalid_timestamp};
  }
  return {};
}

[[nodiscard]] ValidationResult
validate_position(const wire::PositionSnapshot* record) noexcept {
  if (record == nullptr) {
    return {ValidationError::missing_payload};
  }
  if (!valid_version(record->schema_version())) {
    return {ValidationError::unsupported_schema_major};
  }
  if (!valid_identifier(record->global_event_id()) ||
      !valid_identifier(record->session_id()) ||
      !valid_identifier(record->strategy_id()) ||
      !valid_identifier(record->instrument_id()) ||
      !valid_identifier(record->risk_snapshot_id()) ||
      !valid_identifier(record->configuration_version())) {
    return {ValidationError::invalid_identifier};
  }
  if (record->quantity_unit() != wire::QuantityUnit::INSTRUMENT_UNITS ||
      record->price_unit() != wire::PriceUnit::TICKS ||
      record->currency_unit() != wire::CurrencyUnit::CURRENCY_NANOS) {
    return {ValidationError::invalid_unit};
  }
  if (!positive_timestamp(record->as_of_process_monotonic_time())) {
    return {ValidationError::invalid_timestamp};
  }
  if (record->schema_version()->minor() >= 7U) {
    if (!valid_identifier(record->account_id())) {
      return {ValidationError::invalid_identifier};
    }
    if (record->portfolio_sequence() == 0U || record->source_journal_sequence() == 0U ||
        record->portfolio_snapshot_hash() == 0U) {
      return {ValidationError::invalid_value};
    }
    if (!valid_enum(record->health()) || !valid_enum(record->invariant())) {
      return {ValidationError::invalid_enum};
    }
    if (record->ready() && record->health() != wire::PortfolioHealthCode::HEALTHY) {
      return {ValidationError::invalid_value};
    }
  }
  return {};
}

[[nodiscard]] ValidationResult
validate_intelligence_base(const wire::EventIntelligenceRecord* record) noexcept {
  if (record == nullptr) {
    return {ValidationError::missing_payload};
  }
  if (!valid_version(record->schema_version())) {
    return {ValidationError::unsupported_schema_major};
  }
  if (!valid_identifier(record->global_event_id()) ||
      !valid_identifier(record->session_id()) ||
      !valid_identifier(record->source_channel_id()) ||
      !valid_identifier(record->instrument_id()) ||
      !valid_identifier(record->model_id()) ||
      !valid_identifier(record->model_version()) ||
      !valid_identifier(record->configuration_version())) {
    return {ValidationError::invalid_identifier};
  }
  if (!valid_enum(record->trust()) || !valid_enum(record->text_trust())) {
    return {ValidationError::invalid_enum};
  }
  if (!positive_timestamp(record->provider_event_time()) ||
      !positive_timestamp(record->received_wall_clock_utc_time()) ||
      !positive_timestamp(record->processed_monotonic_time())) {
    return {ValidationError::invalid_timestamp};
  }
  if (record->relevance_ppm() > kMaximumConfidencePpm ||
      record->classification_code() == 0U ||
      !nonzero_digest(record->untrusted_source_content_sha256()) ||
      !nonzero_digest(record->sanitized_content_sha256())) {
    return {ValidationError::invalid_value};
  }
  return {};
}

[[nodiscard]] bool valid_bounded_text(const flatbuffers::String* value,
                                      const std::size_t maximum) noexcept {
  return value != nullptr && !value->empty() && value->size() <= maximum;
}

[[nodiscard]] ValidationResult validate_intelligence_v1_4_metadata(
    const wire::EventIntelligenceRecord* record) noexcept {
  if (!valid_enum(record->document_type()) || !valid_enum(record->event_type()) ||
      !valid_enum(record->stage()) || !valid_enum(record->adjudication())) {
    return {ValidationError::invalid_enum};
  }
  if (record->classification_code() !=
          static_cast<std::uint32_t>(record->event_type()) ||
      record->revision() == 0U || record->novelty_ppm() > kMaximumConfidencePpm ||
      record->materiality_ppm() > kMaximumConfidencePpm ||
      record->source_trust_ppm() > kMaximumConfidencePpm ||
      record->uncertainty_ppm() > kMaximumConfidencePpm ||
      !nonzero_digest(record->source_authentication_sha256())) {
    return {ValidationError::invalid_value};
  }
  const auto* language = record->language_code();
  const auto* provider = record->source_provider_id();
  const auto* document = record->source_document_id();
  if (!valid_bounded_text(language, 16U) || !valid_bounded_text(provider, 64U) ||
      !valid_bounded_text(document, 128U)) {
    return {ValidationError::invalid_text};
  }
  if ((record->stage() == wire::IntelligenceStage::FAST &&
       record->parent_fast_global_event_id() != nullptr) ||
      (record->stage() == wire::IntelligenceStage::DEEP &&
       !valid_identifier(record->parent_fast_global_event_id())) ||
      (record->stage() == wire::IntelligenceStage::DEEP &&
       record->adjudication() == wire::IntelligenceAdjudicationCode::PRELIMINARY)) {
    return {ValidationError::invalid_relationship};
  }
  return {};
}

[[nodiscard]] bool
valid_intelligence_entity(const wire::IntelligenceResolvedEntity* entity) noexcept {
  return entity != nullptr && nonzero_digest(entity->entity_sha256()) &&
         valid_identifier(entity->instrument_id()) &&
         entity->confidence_ppm() <= kMaximumConfidencePpm &&
         valid_bounded_text(entity->canonical_name(), 128U) &&
         valid_bounded_text(entity->relationship(), 64U);
}

[[nodiscard]] bool
valid_intelligence_excerpt(const wire::IntelligenceEvidenceExcerpt* excerpt) noexcept {
  return excerpt != nullptr && excerpt->excerpt_id() != 0U &&
         excerpt->source_end_utf8_offset() > excerpt->source_start_utf8_offset() &&
         valid_bounded_text(excerpt->exact_text(), kMaximumIntelligenceTextBytes) &&
         nonzero_digest(excerpt->exact_text_sha256());
}

[[nodiscard]] bool evidence_contains_id(
    const flatbuffers::Vector<flatbuffers::Offset<wire::IntelligenceEvidenceExcerpt>>*
        evidence,
    const std::uint32_t excerpt_id) noexcept {
  return std::ranges::any_of(*evidence, [excerpt_id](const auto* excerpt) {
    return excerpt->excerpt_id() == excerpt_id;
  });
}

[[nodiscard]] ValidationResult validate_intelligence_fact(
    const wire::IntelligenceFact* fact,
    const flatbuffers::Vector<flatbuffers::Offset<wire::IntelligenceEvidenceExcerpt>>*
        evidence) noexcept {
  if (fact == nullptr || !valid_bounded_text(fact->name(), 128U) ||
      !valid_bounded_text(fact->value(), kMaximumIntelligenceTextBytes) ||
      !valid_bounded_text(fact->unit(), 64U) ||
      fact->uncertainty_ppm() > kMaximumConfidencePpm ||
      fact->evidence_excerpt_ids() == nullptr ||
      fact->evidence_excerpt_ids()->empty() ||
      fact->evidence_excerpt_ids()->size() > 8U) {
    return {ValidationError::invalid_value};
  }
  for (const auto excerpt_id : *fact->evidence_excerpt_ids()) {
    if (!evidence_contains_id(evidence, excerpt_id)) {
      return {ValidationError::invalid_relationship};
    }
  }
  return {};
}

[[nodiscard]] ValidationResult validate_intelligence_collections(
    const wire::EventIntelligenceRecord* record) noexcept {
  const auto* entities = record->entities();
  const auto* facts = record->facts();
  const auto* evidence = record->evidence();
  const auto* contradictions = record->contradicts_global_event_ids();
  if (entities == nullptr || entities->empty() ||
      entities->size() > kMaximumIntelligenceEntities || facts == nullptr ||
      facts->size() > kMaximumIntelligenceFacts || evidence == nullptr ||
      evidence->size() > kMaximumIntelligenceEvidence || contradictions == nullptr ||
      contradictions->size() > kMaximumIntelligenceEntities) {
    return {ValidationError::invalid_value};
  }
  for (const auto* entity : *entities) {
    if (!valid_intelligence_entity(entity)) {
      return {ValidationError::invalid_value};
    }
  }
  for (const auto* excerpt : *evidence) {
    if (!valid_intelligence_excerpt(excerpt)) {
      return {ValidationError::invalid_value};
    }
  }
  for (const auto* fact : *facts) {
    const auto validation = validate_intelligence_fact(fact, evidence);
    if (!validation.ok()) {
      return validation;
    }
  }
  for (const auto* contradiction : *contradictions) {
    if (!valid_identifier(contradiction)) {
      return {ValidationError::invalid_identifier};
    }
  }
  return {};
}

[[nodiscard]] ValidationResult
validate_intelligence(const wire::EventIntelligenceRecord* record) noexcept {
  if (record == nullptr) {
    return {ValidationError::missing_payload};
  }
  const auto base = validate_intelligence_base(record);
  if (!base.ok()) {
    return base;
  }
  const auto* version = record->schema_version();
  if (version == nullptr) {
    return {ValidationError::unsupported_schema_major};
  }
  if (version->minor() < 4U) {
    return {};
  }
  const auto metadata = validate_intelligence_v1_4_metadata(record);
  if (!metadata.ok()) {
    return metadata;
  }
  return validate_intelligence_collections(record);
}

[[nodiscard]] bool
valid_legacy_kill_scope(const wire::KillSwitchEvent* record) noexcept {
  switch (record->scope()) {
  case wire::KillSwitchScope::GLOBAL:
    return record->target_venue_id() == nullptr &&
           record->target_strategy_id() == nullptr;
  case wire::KillSwitchScope::VENUE:
    return valid_identifier(record->target_venue_id()) &&
           record->target_strategy_id() == nullptr;
  case wire::KillSwitchScope::STRATEGY:
    return valid_identifier(record->target_strategy_id()) &&
           record->target_venue_id() == nullptr;
  case wire::KillSwitchScope::UNKNOWN:
  case wire::KillSwitchScope::SYMBOL:
  case wire::KillSwitchScope::ACCOUNT:
    return false;
  }
  return false;
}

[[nodiscard]] bool valid_v1_6_kill_scope(const wire::KillSwitchEvent* record) noexcept {
  const auto no_venue = record->target_venue_id() == nullptr;
  const auto no_strategy = record->target_strategy_id() == nullptr;
  const auto no_symbol = record->target_instrument_id() == nullptr;
  const auto no_account = record->target_account_id() == nullptr;
  switch (record->scope()) {
  case wire::KillSwitchScope::GLOBAL:
    return no_venue && no_strategy && no_symbol && no_account;
  case wire::KillSwitchScope::VENUE:
    return valid_identifier(record->target_venue_id()) && no_strategy && no_symbol &&
           no_account;
  case wire::KillSwitchScope::STRATEGY:
    return no_venue && valid_identifier(record->target_strategy_id()) && no_symbol &&
           no_account;
  case wire::KillSwitchScope::SYMBOL:
    return no_venue && no_strategy &&
           valid_identifier(record->target_instrument_id()) && no_account;
  case wire::KillSwitchScope::ACCOUNT:
    return no_venue && no_strategy && no_symbol &&
           valid_identifier(record->target_account_id());
  case wire::KillSwitchScope::UNKNOWN:
    return false;
  }
  return false;
}

[[nodiscard]] ValidationResult
validate_kill_switch(const wire::KillSwitchEvent* record) noexcept {
  if (record == nullptr) {
    return {ValidationError::missing_payload};
  }
  if (!valid_version(record->schema_version())) {
    return {ValidationError::unsupported_schema_major};
  }
  if (!valid_identifier(record->global_event_id()) ||
      !valid_identifier(record->session_id()) ||
      !valid_identifier(record->configuration_version())) {
    return {ValidationError::invalid_identifier};
  }
  if (!valid_enum(record->state()) || !valid_enum(record->scope()) ||
      !valid_enum(record->reason())) {
    return {ValidationError::invalid_enum};
  }
  if (!positive_timestamp(record->effective_process_monotonic_time()) ||
      !positive_timestamp(record->recorded_wall_clock_utc_time())) {
    return {ValidationError::invalid_timestamp};
  }
  if (record->schema_version()->minor() >= 6U) {
    if (!valid_v1_6_kill_scope(record) || record->command_sequence() == 0U ||
        record->authority_epoch() == 0U) {
      return {ValidationError::invalid_scope};
    }
  } else if (!valid_legacy_kill_scope(record)) {
    return {ValidationError::invalid_scope};
  }
  if ((record->state() == wire::KillSwitchStateCode::RESET ||
       record->reason() == wire::KillSwitchReasonCode::OPERATOR) &&
      !nonzero_digest(record->operator_authorization_sha256())) {
    return {ValidationError::invalid_value};
  }
  return {};
}

[[nodiscard]] bool
record_type_matches_payload(const wire::RecordType record_type,
                            const wire::ContractPayload payload_type) noexcept {
  switch (record_type) {
  case wire::RecordType::INSTRUMENT_REFERENCE_DATA:
    return payload_type == wire::ContractPayload::InstrumentReferenceData;
  case wire::RecordType::MARKET_EVENT:
    return payload_type == wire::ContractPayload::MarketEvent;
  case wire::RecordType::BOOK_UPDATE:
    return payload_type == wire::ContractPayload::BookUpdate;
  case wire::RecordType::TRADE_EVENT:
    return payload_type == wire::ContractPayload::TradeEvent;
  case wire::RecordType::QUOTE_EVENT:
    return payload_type == wire::ContractPayload::QuoteEvent;
  case wire::RecordType::AUCTION_IMBALANCE:
    return payload_type == wire::ContractPayload::AuctionImbalance;
  case wire::RecordType::TRADING_STATUS:
    return payload_type == wire::ContractPayload::TradingStatus;
  case wire::RecordType::FEATURE_SNAPSHOT_METADATA:
    return payload_type == wire::ContractPayload::FeatureSnapshotMetadata;
  case wire::RecordType::MODEL_FORECAST:
    return payload_type == wire::ContractPayload::ModelForecast;
  case wire::RecordType::ENSEMBLE_FORECAST:
    return payload_type == wire::ContractPayload::EnsembleForecast;
  case wire::RecordType::ORDER_INTENT:
    return payload_type == wire::ContractPayload::OrderIntent;
  case wire::RecordType::RISK_DECISION:
    return payload_type == wire::ContractPayload::RiskDecision;
  case wire::RecordType::ORDER_EVENT:
    return payload_type == wire::ContractPayload::OrderEvent;
  case wire::RecordType::FILL_EVENT:
    return payload_type == wire::ContractPayload::FillEvent;
  case wire::RecordType::POSITION_SNAPSHOT:
    return payload_type == wire::ContractPayload::PositionSnapshot;
  case wire::RecordType::EVENT_INTELLIGENCE_RECORD:
    return payload_type == wire::ContractPayload::EventIntelligenceRecord;
  case wire::RecordType::DATA_QUALITY_STATE:
    return payload_type == wire::ContractPayload::DataQualityState;
  case wire::RecordType::CLOCK_QUALITY_STATE:
    return payload_type == wire::ContractPayload::ClockQualityState;
  case wire::RecordType::KILL_SWITCH_EVENT:
    return payload_type == wire::ContractPayload::KillSwitchEvent;
  case wire::RecordType::UNKNOWN:
  default:
    return false;
  }
}

[[nodiscard]] ValidationResult
validate_payload(const wire::ContractRecord* record) noexcept {
  switch (record->record_type()) {
  case wire::RecordType::INSTRUMENT_REFERENCE_DATA:
    return validate_instrument_reference(record->payload_as_InstrumentReferenceData());
  case wire::RecordType::MARKET_EVENT:
    return validate_market_event(record->payload_as_MarketEvent());
  case wire::RecordType::BOOK_UPDATE:
    return validate_book_update(record->payload_as_BookUpdate());
  case wire::RecordType::TRADE_EVENT:
    return validate_trade(record->payload_as_TradeEvent());
  case wire::RecordType::QUOTE_EVENT:
    return validate_quote(record->payload_as_QuoteEvent());
  case wire::RecordType::AUCTION_IMBALANCE:
    return validate_auction(record->payload_as_AuctionImbalance());
  case wire::RecordType::TRADING_STATUS:
    return validate_trading_status(record->payload_as_TradingStatus());
  case wire::RecordType::FEATURE_SNAPSHOT_METADATA:
    return validate_feature_snapshot(record->payload_as_FeatureSnapshotMetadata());
  case wire::RecordType::MODEL_FORECAST:
    return validate_model_forecast(record->payload_as_ModelForecast());
  case wire::RecordType::ENSEMBLE_FORECAST:
    return validate_ensemble_forecast(record->payload_as_EnsembleForecast());
  case wire::RecordType::ORDER_INTENT:
    return validate_order_intent(record->payload_as_OrderIntent());
  case wire::RecordType::RISK_DECISION:
    return validate_risk_decision(record->payload_as_RiskDecision());
  case wire::RecordType::ORDER_EVENT:
    return validate_order_event(record->payload_as_OrderEvent());
  case wire::RecordType::FILL_EVENT:
    return validate_fill(record->payload_as_FillEvent());
  case wire::RecordType::POSITION_SNAPSHOT:
    return validate_position(record->payload_as_PositionSnapshot());
  case wire::RecordType::EVENT_INTELLIGENCE_RECORD:
    return validate_intelligence(record->payload_as_EventIntelligenceRecord());
  case wire::RecordType::DATA_QUALITY_STATE:
    return validate_data_quality(record->payload_as_DataQualityState());
  case wire::RecordType::CLOCK_QUALITY_STATE:
    return validate_clock_quality(record->payload_as_ClockQualityState());
  case wire::RecordType::KILL_SWITCH_EVENT:
    return validate_kill_switch(record->payload_as_KillSwitchEvent());
  case wire::RecordType::UNKNOWN:
  default:
    return {ValidationError::invalid_enum};
  }
}

[[nodiscard]] wire::SchemaVersion current_wire_version() noexcept {
  return {kCurrentSchemaMajor, kCurrentSchemaMinor, kCurrentSchemaPatch};
}

[[nodiscard]] wire::Sha256Digest to_wire_digest(const Sha256Digest& digest) noexcept {
  return wire::Sha256Digest{
      flatbuffers::span<const std::uint8_t, 32>{digest.data(), digest.size()}};
}

} // namespace

ValidationResult validate_size_prefixed_audit_envelope(
    const std::span<const std::uint8_t> encoded,
    ValidatedAuditEnvelopeView* output) noexcept { // NOLINT(misc-use-internal-linkage)
  if (encoded.empty()) {
    return {ValidationError::empty_buffer};
  }
  if (encoded.size() > kMaximumEnvelopeBytes) {
    return {ValidationError::size_limit_exceeded};
  }
  if (encoded.size() < kSizePrefixBytes) {
    return {ValidationError::invalid_size_prefix};
  }
  const auto declared_size =
      flatbuffers::ReadScalar<flatbuffers::uoffset_t>(encoded.data());
  if (declared_size != encoded.size() - kSizePrefixBytes) {
    return {ValidationError::invalid_size_prefix};
  }

  flatbuffers::Verifier envelope_verifier(encoded.data(), encoded.size(), 64,
                                          1'000'000U);
  if (!wire::VerifySizePrefixedAuditEnvelopeBuffer(envelope_verifier)) {
    return {ValidationError::invalid_flatbuffer};
  }
  const auto* envelope = wire::GetSizePrefixedAuditEnvelope(encoded.data());
  if (!valid_version(envelope->schema_version())) {
    return {ValidationError::unsupported_schema_major};
  }
  if (!valid_identifier(envelope->envelope_id()) ||
      !valid_identifier(envelope->session_id()) ||
      !valid_identifier(envelope->configuration_version())) {
    return {ValidationError::invalid_identifier};
  }
  if (!valid_enum(envelope->record_type())) {
    return {ValidationError::invalid_enum};
  }
  if (!positive_timestamp(envelope->created_process_monotonic_time()) ||
      !positive_timestamp(envelope->recorded_wall_clock_utc_time())) {
    return {ValidationError::invalid_timestamp};
  }
  if (envelope->payload() == nullptr || envelope->payload()->empty()) {
    return {ValidationError::missing_payload};
  }
  if (envelope->payload()->size() > kMaximumContractBytes) {
    return {ValidationError::size_limit_exceeded};
  }

  const std::span<const std::uint8_t> payload{envelope->payload()->Data(),
                                              envelope->payload()->size()};
  if (!digest_matches(envelope->payload_sha256(), sha256(payload))) {
    return {ValidationError::checksum_mismatch};
  }
  flatbuffers::Verifier contract_verifier(payload.data(), payload.size(), 64,
                                          1'000'000U);
  if (!wire::VerifyContractRecordBuffer(contract_verifier)) {
    return {ValidationError::invalid_flatbuffer};
  }
  const auto* record = wire::GetContractRecord(payload.data());
  if (!valid_version(record->schema_version())) {
    return {ValidationError::unsupported_schema_major};
  }
  if (!valid_identifier(record->record_id())) {
    return {ValidationError::invalid_identifier};
  }
  if (!record_type_matches_payload(record->record_type(), record->payload_type()) ||
      envelope->record_type() != record->record_type()) {
    return {ValidationError::record_type_mismatch};
  }
  const auto payload_validation = validate_payload(record);
  if (!payload_validation.ok()) {
    return payload_validation;
  }

  if (output != nullptr) {
    *output = {.envelope = envelope, .record = record, .payload = payload};
  }
  return {};
}

flatbuffers::DetachedBuffer build_data_quality_contract(
    const DataQualityContractInput& input) { // NOLINT(misc-use-internal-linkage)
  flatbuffers::FlatBufferBuilder builder{512U};
  const auto version = current_wire_version();
  const wire::GlobalEventId record_id{input.record_id.high(), input.record_id.low()};
  const wire::VenueId venue_id{input.venue_id.high(), input.venue_id.low()};
  const wire::ChannelId channel_id{input.channel_id.high(), input.channel_id.low()};
  const wire::ProcessMonotonicTimeNs observed_time{
      input.observed_process_monotonic_time_ns};
  const wire::ExchangeEventTimeNs exchange_time{input.last_good_exchange_event_time_ns};
  const wire::NicReceiveTimeNs nic_time{input.last_good_nic_receive_time_ns};
  const auto data_quality = wire::CreateDataQualityState(
      builder, &version, &venue_id, &channel_id, input.state, &observed_time,
      &exchange_time, &nic_time, input.missing_sequence_count,
      input.malformed_record_count, input.stale_after_ns);
  const auto record = wire::CreateContractRecord(
      builder, &version, &record_id, wire::RecordType::DATA_QUALITY_STATE,
      wire::ContractPayload::DataQualityState, data_quality.Union());
  wire::FinishContractRecordBuffer(builder, record);
  return builder.Release();
}

flatbuffers::DetachedBuffer build_model_forecast_contract(
    const ModelForecastContractInput& input) { // NOLINT(misc-use-internal-linkage)
  flatbuffers::FlatBufferBuilder builder{1024U};
  const auto version = current_wire_version();
  const wire::GlobalEventId record_id{input.record_id.high(), input.record_id.low()};
  const wire::ForecastId forecast_id{input.forecast_id.high(), input.forecast_id.low()};
  const wire::SessionId session_id{input.session_id.high(), input.session_id.low()};
  const wire::ModelId model_id{input.model_id.high(), input.model_id.low()};
  const wire::ModelVersion model_version{input.model_version.high(),
                                         input.model_version.low()};
  const wire::InstrumentId instrument_id{input.instrument_id.high(),
                                         input.instrument_id.low()};
  const wire::FeatureSnapshotId feature_snapshot_id{input.feature_snapshot_id.high(),
                                                    input.feature_snapshot_id.low()};
  const wire::ConfigurationVersion configuration_version{
      input.configuration_version.high(), input.configuration_version.low()};
  const wire::ProcessMonotonicTimeNs production_time{
      input.production_process_monotonic_time_ns};
  const wire::ProcessMonotonicTimeNs expiration_time{
      input.expiration_process_monotonic_time_ns};
  const wire::ExchangeEventTimeNs as_of_time{input.as_of_exchange_event_time_ns};
  const auto horizon_value =
      input.horizon_value == 0U ? input.horizon_ns : input.horizon_value;
  const wire::ConfigurationVersion horizon_calendar_version{
      input.horizon_calendar_version.high(), input.horizon_calendar_version.low()};
  const auto* horizon_calendar =
      input.horizon_calendar_version.valid() ? &horizon_calendar_version : nullptr;
  const auto horizon_spec = wire::CreateHorizonSpec(
      builder, input.horizon_unit, horizon_value, input.horizon_halt_policy,
      input.horizon_session_endpoint, horizon_calendar);
  std::int64_t target_exchange_event_time_ns = input.target_exchange_event_time_ns;
  if (target_exchange_event_time_ns == 0 &&
      input.horizon_unit == wire::ForecastHorizonUnit::ELAPSED_NANOSECONDS &&
      input.as_of_exchange_event_time_ns > 0 &&
      std::cmp_less_equal(input.horizon_ns, std::numeric_limits<std::int64_t>::max() -
                                                input.as_of_exchange_event_time_ns)) {
    target_exchange_event_time_ns = input.as_of_exchange_event_time_ns +
                                    static_cast<std::int64_t>(input.horizon_ns);
  }
  const wire::ExchangeEventTimeNs target_exchange_time{target_exchange_event_time_ns};
  const auto forecast = wire::CreateModelForecast(
      builder, &version, &forecast_id, &session_id, &model_id, &model_version,
      &instrument_id, &feature_snapshot_id, &configuration_version, input.forecast_unit,
      input.forecast_target == wire::ForecastTarget::RETURN ? input.expected_return_ppm
                                                            : input.target_value,
      input.confidence_ppm, input.horizon_ns, &production_time, &expiration_time,
      &as_of_time, input.expected_return_ppm, input.return_p10_ppm,
      input.return_p50_ppm, input.return_p90_ppm, input.probability_down_ppm,
      input.probability_flat_ppm, input.probability_up_ppm, input.volatility_ppm,
      input.calibration_score_ppm, input.data_quality_score_ppm, input.ood_score_ppm,
      input.has_transaction_cost_estimate, input.estimated_spread_cost_ppm,
      input.estimated_market_impact_ppm, input.estimated_fee_cost_ppm,
      input.estimated_slippage_cost_ppm, input.estimated_adverse_selection_cost_ppm,
      input.forecast_target,
      input.forecast_target == wire::ForecastTarget::RETURN ? input.expected_return_ppm
                                                            : input.target_value,
      input.forecast_target == wire::ForecastTarget::RETURN ? input.return_p10_ppm
                                                            : input.target_p10,
      input.forecast_target == wire::ForecastTarget::RETURN ? input.return_p50_ppm
                                                            : input.target_p50,
      input.forecast_target == wire::ForecastTarget::RETURN ? input.return_p90_ppm
                                                            : input.target_p90,
      horizon_spec, &target_exchange_time);
  const auto record = wire::CreateContractRecord(
      builder, &version, &record_id, wire::RecordType::MODEL_FORECAST,
      wire::ContractPayload::ModelForecast, forecast.Union());
  wire::FinishContractRecordBuffer(builder, record);
  return builder.Release();
}

flatbuffers::DetachedBuffer build_size_prefixed_audit_envelope(
    const std::span<const std::uint8_t> contract_payload,
    const wire::RecordType record_type,
    const AuditMetadata& metadata) { // NOLINT(misc-use-internal-linkage)
  flatbuffers::FlatBufferBuilder builder{1024U};
  const auto version = current_wire_version();
  const wire::GlobalEventId envelope_id{metadata.envelope_id.high(),
                                        metadata.envelope_id.low()};
  const wire::SessionId session_id{metadata.session_id.high(),
                                   metadata.session_id.low()};
  const wire::ConfigurationVersion configuration_version{
      metadata.configuration_version.high(), metadata.configuration_version.low()};
  const wire::ProcessMonotonicTimeNs monotonic_time{
      metadata.created_process_monotonic_time_ns};
  const wire::WallClockUtcTimeNs wall_time{metadata.recorded_wall_clock_utc_time_ns};
  const auto payload_digest = to_wire_digest(sha256(contract_payload));
  const auto previous_digest = to_wire_digest(metadata.previous_envelope_sha256);
  const auto payload =
      builder.CreateVector(contract_payload.data(), contract_payload.size());
  const auto envelope = wire::CreateAuditEnvelope(
      builder, &version, &envelope_id, &session_id, &configuration_version,
      &monotonic_time, &wall_time, record_type, payload, &payload_digest,
      &previous_digest);
  wire::FinishSizePrefixedAuditEnvelopeBuffer(builder, envelope);
  return builder.Release();
}

} // namespace aegis::common
