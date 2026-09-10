#ifndef AEGIS_COMMON_AUDIT_ENVELOPE_HPP
#define AEGIS_COMMON_AUDIT_ENVELOPE_HPP

#include "aegis/common/identifiers.hpp"
#include "aegis/common/sha256.hpp"
#include "aegis/mx/contracts/v1/audit_envelope_generated.h"

#include <cstddef>
#include <cstdint>
#include <span>

#include <flatbuffers/flatbuffer_builder.h>

namespace aegis::common {

namespace wire = ::aegis::mx::contracts::v1;

inline constexpr std::uint16_t kCurrentSchemaMajor = 1;
inline constexpr std::uint16_t kCurrentSchemaMinor = 9;
inline constexpr std::uint32_t kCurrentSchemaPatch = 0;
inline constexpr std::size_t kMaximumContractBytes = 1U << 20U;
inline constexpr std::size_t kMaximumEnvelopeBytes = 1U << 22U;

enum class ValidationError : std::uint8_t {
  none = 0,
  empty_buffer,
  size_limit_exceeded,
  invalid_size_prefix,
  invalid_flatbuffer,
  unsupported_schema_major,
  invalid_schema_version,
  invalid_identifier,
  invalid_enum,
  missing_payload,
  record_type_mismatch,
  checksum_mismatch,
  invalid_timestamp,
  invalid_unit,
  invalid_quantity,
  invalid_value,
  invalid_relationship,
  invalid_text,
  invalid_scope,
};

class ValidationResult {
public:
  constexpr ValidationResult(
      const ValidationError error = ValidationError::none) noexcept
      : error_(error) {}

  [[nodiscard]] constexpr bool ok() const noexcept {
    return error_ == ValidationError::none;
  }

  [[nodiscard]] constexpr ValidationError error() const noexcept { return error_; }

private:
  ValidationError error_;
};

struct ValidatedAuditEnvelopeView {
  const wire::AuditEnvelope* envelope{};
  const wire::ContractRecord* record{};
  std::span<const std::uint8_t> payload;
};

// Structural verification, SHA-256 validation, and all safety-relevant
// semantic checks execute without dynamic allocation and leave |output|
// untouched on failure.
[[nodiscard]] ValidationResult validate_size_prefixed_audit_envelope(
    std::span<const std::uint8_t> encoded,
    ValidatedAuditEnvelopeView* output = nullptr) noexcept;

struct DataQualityContractInput {
  GlobalEventId record_id;
  VenueId venue_id;
  ChannelId channel_id;
  wire::DataQualityCode state{wire::DataQualityCode::UNKNOWN};
  std::uint64_t observed_process_monotonic_time_ns{};
  std::int64_t last_good_exchange_event_time_ns{};
  std::int64_t last_good_nic_receive_time_ns{};
  std::uint64_t missing_sequence_count{};
  std::uint64_t malformed_record_count{};
  std::uint64_t stale_after_ns{};
};

struct ModelForecastContractInput {
  GlobalEventId record_id;
  ForecastId forecast_id;
  SessionId session_id;
  ModelId model_id;
  ModelVersion model_version;
  InstrumentId instrument_id;
  FeatureSnapshotId feature_snapshot_id;
  ConfigurationVersion configuration_version;
  std::int64_t expected_return_ppm{};
  std::int64_t return_p10_ppm{};
  std::int64_t return_p50_ppm{};
  std::int64_t return_p90_ppm{};
  std::uint32_t probability_down_ppm{};
  std::uint32_t probability_flat_ppm{};
  std::uint32_t probability_up_ppm{};
  std::uint64_t volatility_ppm{};
  std::uint32_t confidence_ppm{};
  std::uint32_t calibration_score_ppm{};
  std::uint32_t data_quality_score_ppm{};
  std::uint32_t ood_score_ppm{};
  std::uint64_t horizon_ns{};
  std::int64_t as_of_exchange_event_time_ns{};
  std::uint64_t production_process_monotonic_time_ns{};
  std::uint64_t expiration_process_monotonic_time_ns{};
  bool has_transaction_cost_estimate{false};
  std::uint64_t estimated_spread_cost_ppm{};
  std::uint64_t estimated_slippage_cost_ppm{};
  std::uint64_t estimated_market_impact_ppm{};
  std::uint64_t estimated_adverse_selection_cost_ppm{};
  std::uint64_t estimated_fee_cost_ppm{};
  wire::ForecastTarget forecast_target{wire::ForecastTarget::RETURN};
  wire::ForecastUnit forecast_unit{wire::ForecastUnit::RETURN_PPM};
  std::int64_t target_value{};
  std::int64_t target_p10{};
  std::int64_t target_p50{};
  std::int64_t target_p90{};
  wire::ForecastHorizonUnit horizon_unit{
      wire::ForecastHorizonUnit::ELAPSED_NANOSECONDS};
  std::uint64_t horizon_value{};
  wire::HorizonHaltPolicy horizon_halt_policy{wire::HorizonHaltPolicy::NOT_APPLICABLE};
  wire::HorizonSessionEndpoint horizon_session_endpoint{
      wire::HorizonSessionEndpoint::NOT_APPLICABLE};
  // Explicit safe absence preserves aggregate callers across the additive API.
  ConfigurationVersion
      horizon_calendar_version{}; // NOLINT(readability-redundant-member-init)
  std::int64_t target_exchange_event_time_ns{};
};

struct AuditMetadata {
  GlobalEventId envelope_id;
  SessionId session_id;
  ConfigurationVersion configuration_version;
  std::uint64_t created_process_monotonic_time_ns{};
  std::int64_t recorded_wall_clock_utc_time_ns{};
  Sha256Digest previous_envelope_sha256{};
};

// Canonical builders allocate and are restricted to serialization/control
// boundaries. Hot-path consumers use validate_size_prefixed_audit_envelope and
// the returned zero-copy view.
[[nodiscard]] flatbuffers::DetachedBuffer
build_data_quality_contract(const DataQualityContractInput& input);

[[nodiscard]] flatbuffers::DetachedBuffer
build_model_forecast_contract(const ModelForecastContractInput& input);

[[nodiscard]] flatbuffers::DetachedBuffer
build_size_prefixed_audit_envelope(std::span<const std::uint8_t> contract_payload,
                                   wire::RecordType record_type,
                                   const AuditMetadata& metadata);

} // namespace aegis::common

#endif // AEGIS_COMMON_AUDIT_ENVELOPE_HPP
