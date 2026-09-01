#include "aegis/models/microstructure.hpp"

#include "aegis/models/baselines.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <span>
#include <string_view>

namespace aegis::models {
namespace {

constexpr std::int64_t kMaximumArtifactNumericValue = 10'000'000LL;

template <typename Integer>
[[nodiscard]] bool parse_integer(const std::string_view value,
                                 Integer& output) noexcept {
  if (value.empty()) {
    return false;
  }
  Integer parsed{};
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size()) {
    return false;
  }
  output = parsed;
  return true;
}

template <typename Tag>
[[nodiscard]] bool parse_identifier(const std::string_view value,
                                    common::Identifier128<Tag>& output) noexcept {
  const auto separator = value.find(':');
  if (separator == std::string_view::npos ||
      value.find(':', separator + 1U) != std::string_view::npos) {
    return false;
  }
  std::uint64_t high{};
  std::uint64_t low{};
  if (!parse_integer(value.substr(0U, separator), high) ||
      !parse_integer(value.substr(separator + 1U), low)) {
    return false;
  }
  output = common::Identifier128<Tag>{high, low};
  return output.valid();
}

[[nodiscard]] bool parse_hex_digest(const std::string_view value,
                                    common::Sha256Digest& digest) noexcept {
  if (value.size() != digest.size() * 2U) {
    return false;
  }
  const auto nibble = [](const char character) -> std::uint8_t {
    if (character >= '0' && character <= '9') {
      return static_cast<std::uint8_t>(character - '0');
    }
    if (character >= 'a' && character <= 'f') {
      return static_cast<std::uint8_t>(character - 'a' + 10);
    }
    return 0xFFU;
  };
  for (std::size_t index = 0U; index < digest.size(); ++index) {
    const auto high = nibble(value[index * 2U]);
    const auto low = nibble(value[(index * 2U) + 1U]);
    if (high > 0x0FU || low > 0x0FU) {
      return false;
    }
    digest[index] = static_cast<std::uint8_t>((high << 4U) | low);
  }
  return true;
}

[[nodiscard]] bool parse_role(const std::string_view value,
                              MicrostructureModelRole& role) noexcept {
  constexpr std::array<std::pair<std::string_view, MicrostructureModelRole>, 7> kRoles{{
      {"mid_price_direction", MicrostructureModelRole::mid_price_direction},
      {"spread_widening", MicrostructureModelRole::spread_widening},
      {"queue_depletion", MicrostructureModelRole::queue_depletion},
      {"passive_fill_probability", MicrostructureModelRole::passive_fill_probability},
      {"adverse_selection", MicrostructureModelRole::adverse_selection},
      {"transaction_cost", MicrostructureModelRole::transaction_cost},
      {"market_impact", MicrostructureModelRole::market_impact},
  }};
  for (const auto& [name, candidate] : kRoles) {
    if (value == name) {
      role = candidate;
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool parse_family(const std::string_view value,
                                NativeModelFamily& family) noexcept {
  if (value == "logistic_regression") {
    family = NativeModelFamily::logistic_regression;
    return true;
  }
  if (value == "linear_regression") {
    family = NativeModelFamily::linear_regression;
    return true;
  }
  if (value == "gradient_boosted_trees") {
    family = NativeModelFamily::gradient_boosted_trees;
    return true;
  }
  return false;
}

[[nodiscard]] bool parse_feature_name(const std::string_view value,
                                      NativeFeatureName& feature) noexcept {
  constexpr std::array<NativeFeatureName, 32> kNames{
      NativeFeatureName::midpoint_half_ticks,
      NativeFeatureName::spread_ticks,
      NativeFeatureName::relative_spread_ppm,
      NativeFeatureName::microprice_ticks_ppm,
      NativeFeatureName::top_level_imbalance_ppm,
      NativeFeatureName::multi_level_weighted_imbalance_ppm,
      NativeFeatureName::order_flow_imbalance_units,
      NativeFeatureName::signed_trade_imbalance_ppm,
      NativeFeatureName::add_rate_millihertz,
      NativeFeatureName::cancel_rate_millihertz,
      NativeFeatureName::execute_rate_millihertz,
      NativeFeatureName::queue_depletion_rate_units_per_second,
      NativeFeatureName::rolling_return_ppm,
      NativeFeatureName::realized_volatility_ppm,
      NativeFeatureName::volume_units,
      NativeFeatureName::vwap_ticks_ppm,
      NativeFeatureName::trade_intensity_millihertz,
      NativeFeatureName::quote_intensity_millihertz,
      NativeFeatureName::cross_venue_divergence_half_ticks,
      NativeFeatureName::bid_leader_venue_number,
      NativeFeatureName::ask_leader_venue_number,
      NativeFeatureName::venue_leadership_share_ppm,
      NativeFeatureName::replenishment_rate_millihertz,
      NativeFeatureName::data_quality_code,
      NativeFeatureName::data_age_ns,
      NativeFeatureName::session_progress_ppm,
      NativeFeatureName::quantity_ahead_units,
      NativeFeatureName::order_quantity_units,
      NativeFeatureName::order_age_ns,
      NativeFeatureName::price_distance_ticks,
      NativeFeatureName::venue_number,
      NativeFeatureName::fee_rate_ppm,
  };
  for (const auto candidate : kNames) {
    if (value == native_feature_name(candidate)) {
      feature = candidate;
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool next_field(std::string_view& input,
                              std::string_view& field) noexcept {
  const auto delimiter = input.find(',');
  if (delimiter == std::string_view::npos) {
    field = input;
    input = {};
    return !field.empty();
  }
  field = input.substr(0U, delimiter);
  input.remove_prefix(delimiter + 1U);
  return !field.empty();
}

[[nodiscard]] bool parse_feature(std::string_view value,
                                 NativeFeatureTransform& output) noexcept {
  std::array<std::string_view, 6> fields{};
  for (auto& field : fields) {
    if (!next_field(value, field)) {
      return false;
    }
  }
  if (!value.empty() || !parse_feature_name(fields[0U], output.name) ||
      !parse_integer(fields[1U], output.center) ||
      !parse_integer(fields[2U], output.scale) ||
      !parse_integer(fields[3U], output.training_minimum) ||
      !parse_integer(fields[4U], output.training_maximum) ||
      !parse_integer(fields[5U], output.weight_ppm)) {
    return false;
  }
  return output.scale != 0U && output.training_minimum <= output.training_maximum &&
         output.center >= -kMaximumArtifactNumericValue &&
         output.center <= kMaximumArtifactNumericValue &&
         output.scale <= static_cast<std::uint64_t>(kMaximumArtifactNumericValue) &&
         output.training_minimum >= -kMaximumArtifactNumericValue &&
         output.training_maximum <= kMaximumArtifactNumericValue &&
         output.weight_ppm >= -kMaximumArtifactNumericValue &&
         output.weight_ppm <= kMaximumArtifactNumericValue;
}

[[nodiscard]] bool context_feature(const NativeFeatureName name) noexcept {
  return static_cast<std::uint8_t>(name) >= 128U;
}

[[nodiscard]] bool valid_queue_contract(const NativeModelArtifact& artifact) noexcept {
  if (artifact.role != MicrostructureModelRole::queue_depletion &&
      artifact.role != MicrostructureModelRole::passive_fill_probability) {
    return true;
  }
  constexpr std::array<NativeFeatureName, 8> kQueueFeatures{
      NativeFeatureName::quantity_ahead_units,
      NativeFeatureName::add_rate_millihertz,
      NativeFeatureName::cancel_rate_millihertz,
      NativeFeatureName::execute_rate_millihertz,
      NativeFeatureName::order_age_ns,
      NativeFeatureName::price_distance_ticks,
      NativeFeatureName::venue_number,
      NativeFeatureName::session_progress_ppm,
  };
  const auto expected_count =
      artifact.role == MicrostructureModelRole::passive_fill_probability ? 9U : 8U;
  if (artifact.feature_count != expected_count) {
    return false;
  }
  for (std::size_t index = 0U; index < kQueueFeatures.size(); ++index) {
    if (artifact.features[index].name != kQueueFeatures[index]) {
      return false;
    }
  }
  return artifact.role != MicrostructureModelRole::passive_fill_probability ||
         artifact.features[8U].name == NativeFeatureName::order_quantity_units;
}

[[nodiscard]] bool feature_value(const ModelInput& input, const NativeFeatureName name,
                                 std::int64_t& output) noexcept {
  if (!context_feature(name)) {
    const auto index = static_cast<std::uint8_t>(name);
    if (index >= static_cast<std::uint8_t>(features::FeatureName::count)) {
      return false;
    }
    const auto& feature = input.feature_snapshot.get(
        static_cast<features::FeatureName>(static_cast<std::uint8_t>(index)));
    if (feature.validity != features::FeatureValidity::valid &&
        feature.validity != features::FeatureValidity::degraded) {
      return false;
    }
    output = feature.value;
    return true;
  }
  const auto& context = input.microstructure_context;
  if (!context.present) {
    return false;
  }
  switch (name) {
  case NativeFeatureName::quantity_ahead_units:
    if (context.quantity_ahead_units >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return false;
    }
    output = static_cast<std::int64_t>(context.quantity_ahead_units);
    return true;
  case NativeFeatureName::order_quantity_units:
    if (context.order_quantity_units >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return false;
    }
    output = static_cast<std::int64_t>(context.order_quantity_units);
    return true;
  case NativeFeatureName::order_age_ns:
    if (context.order_age_ns >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return false;
    }
    output = static_cast<std::int64_t>(context.order_age_ns);
    return true;
  case NativeFeatureName::price_distance_ticks:
    output = context.price_distance_ticks;
    return true;
  case NativeFeatureName::venue_number:
    output = static_cast<std::int64_t>(context.venue_number);
    return true;
  case NativeFeatureName::fee_rate_ppm:
    output = static_cast<std::int64_t>(context.fee_rate_ppm);
    return true;
  default:
    return false;
  }
}

[[nodiscard]] bool normalized_value(const std::int64_t raw,
                                    const NativeFeatureTransform& transform,
                                    std::int64_t& output) noexcept {
  if (raw < -kMaximumArtifactNumericValue || raw > kMaximumArtifactNumericValue ||
      (transform.center > 0 &&
       raw < std::numeric_limits<std::int64_t>::min() + transform.center) ||
      (transform.center < 0 &&
       raw > std::numeric_limits<std::int64_t>::max() + transform.center)) {
    return false;
  }
  const auto difference = raw - transform.center;
  if (difference < -kMaximumArtifactNumericValue * 2LL ||
      difference > kMaximumArtifactNumericValue * 2LL) {
    return false;
  }
  output = (difference * static_cast<std::int64_t>(kProbabilityScale)) /
           static_cast<std::int64_t>(transform.scale);
  return output >= -kMaximumArtifactNumericValue &&
         output <= kMaximumArtifactNumericValue;
}

[[nodiscard]] std::uint32_t
ood_contribution(const std::int64_t raw,
                 const NativeFeatureTransform& transform) noexcept {
  std::uint64_t distance{};
  if (raw < transform.training_minimum) {
    distance = static_cast<std::uint64_t>(transform.training_minimum - raw);
  } else if (raw > transform.training_maximum) {
    distance = static_cast<std::uint64_t>(raw - transform.training_maximum);
  } else {
    return 0U;
  }
  const auto span = static_cast<std::uint64_t>(transform.training_maximum -
                                               transform.training_minimum);
  const auto denominator = std::max<std::uint64_t>(1U, span + distance);
  return static_cast<std::uint32_t>(std::min<std::uint64_t>(
      kProbabilityScale, (distance * kProbabilityScale) / denominator));
}

[[nodiscard]] std::uint64_t cost_component(const std::uint64_t total,
                                           const std::uint32_t share) noexcept {
  return (total * static_cast<std::uint64_t>(share)) / kProbabilityScale;
}

} // namespace

std::string_view native_feature_name(const NativeFeatureName name) noexcept {
  constexpr std::array<std::string_view, 26> kSnapshotNames{
      "midpoint_half_ticks",
      "spread_ticks",
      "relative_spread_ppm",
      "microprice_ticks_ppm",
      "top_level_imbalance_ppm",
      "multi_level_weighted_imbalance_ppm",
      "order_flow_imbalance_units",
      "signed_trade_imbalance_ppm",
      "add_rate_millihertz",
      "cancel_rate_millihertz",
      "execute_rate_millihertz",
      "queue_depletion_rate_units_per_second",
      "rolling_return_ppm",
      "realized_volatility_ppm",
      "volume_units",
      "vwap_ticks_ppm",
      "trade_intensity_millihertz",
      "quote_intensity_millihertz",
      "cross_venue_divergence_half_ticks",
      "bid_leader_venue_number",
      "ask_leader_venue_number",
      "venue_leadership_share_ppm",
      "replenishment_rate_millihertz",
      "data_quality_code",
      "data_age_ns",
      "session_progress_ppm",
  };
  const auto numeric = static_cast<std::uint8_t>(name);
  if (numeric < kSnapshotNames.size()) {
    return kSnapshotNames[numeric];
  }
  switch (name) {
  case NativeFeatureName::quantity_ahead_units:
    return "quantity_ahead_units";
  case NativeFeatureName::order_quantity_units:
    return "order_quantity_units";
  case NativeFeatureName::order_age_ns:
    return "order_age_ns";
  case NativeFeatureName::price_distance_ticks:
    return "price_distance_ticks";
  case NativeFeatureName::venue_number:
    return "venue_number";
  case NativeFeatureName::fee_rate_ppm:
    return "fee_rate_ppm";
  default:
    return "unknown";
  }
}

std::string_view microstructure_role_name(const MicrostructureModelRole role) noexcept {
  switch (role) {
  case MicrostructureModelRole::mid_price_direction:
    return "mid_price_direction";
  case MicrostructureModelRole::spread_widening:
    return "spread_widening";
  case MicrostructureModelRole::queue_depletion:
    return "queue_depletion";
  case MicrostructureModelRole::passive_fill_probability:
    return "passive_fill_probability";
  case MicrostructureModelRole::adverse_selection:
    return "adverse_selection";
  case MicrostructureModelRole::transaction_cost:
    return "transaction_cost";
  case MicrostructureModelRole::market_impact:
    return "market_impact";
  }
  return "unknown";
}

std::uint32_t deterministic_sigmoid_ppm(const std::int64_t score_ppm) noexcept {
  constexpr std::array<std::uint32_t, 17> kSigmoid{
      335U,     911U,     2'473U,   6'693U,   17'986U,  47'426U,
      119'203U, 268'941U, 500'000U, 731'059U, 880'797U, 952'574U,
      982'014U, 993'307U, 997'527U, 999'089U, 999'665U};
  constexpr std::int64_t kStep = 1'000'000LL;
  const auto clipped =
      std::clamp(score_ppm, std::int64_t{-8} * kStep, std::int64_t{8} * kStep);
  const auto shifted = clipped + (8LL * kStep);
  const auto lower = static_cast<std::size_t>(shifted / kStep);
  if (lower >= kSigmoid.size() - 1U) {
    return kSigmoid.back();
  }
  const auto remainder = static_cast<std::uint64_t>(shifted % kStep);
  const auto delta = kSigmoid[lower + 1U] - kSigmoid[lower];
  return kSigmoid[lower] +
         static_cast<std::uint32_t>((static_cast<std::uint64_t>(delta) * remainder) /
                                    static_cast<std::uint64_t>(kStep));
}

// Artifact parsing is explicit by design: unknown or duplicate keys fail closed.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
NativeArtifactLoadResult load_native_model_artifact(
    const std::string_view encoded, const std::int64_t now_wall_clock_utc_ns,
    const std::uint64_t activation_expiration_process_monotonic_time_ns) noexcept {
  NativeArtifactLoadResult result{};
  constexpr std::string_view kSignaturePrefix{"signature_sha256="};
  const auto signature_marker = encoded.rfind("\nsignature_sha256=");
  if (signature_marker == std::string_view::npos ||
      activation_expiration_process_monotonic_time_ns == 0U) {
    result.error = NativeArtifactError::malformed;
    return result;
  }
  const auto body = encoded.substr(0U, signature_marker + 1U);
  auto signature_value =
      encoded.substr(signature_marker + 1U + kSignaturePrefix.size());
  if (!signature_value.empty() && signature_value.back() == '\n') {
    signature_value.remove_suffix(1U);
  }
  if (signature_value.find('\n') != std::string_view::npos ||
      !parse_hex_digest(signature_value, result.artifact.signature_sha256)) {
    result.error = NativeArtifactError::malformed;
    return result;
  }
  const auto bytes = std::span<const std::uint8_t>{
      reinterpret_cast<const std::uint8_t*>(body.data()), body.size()};
  if (common::sha256(bytes) != result.artifact.signature_sha256) {
    result.error = NativeArtifactError::invalid_signature;
    return result;
  }

  std::array<bool, 21> seen{};
  std::uint32_t declared_feature_count{};
  bool seen_feature_count = false;
  std::string_view remaining = body;
  const auto first_end = remaining.find('\n');
  if (first_end == std::string_view::npos ||
      remaining.substr(0U, first_end) != "AEGIS_MX_NATIVE_MODEL_V1") {
    result.error = NativeArtifactError::unsupported_version;
    return result;
  }
  remaining.remove_prefix(first_end + 1U);
  while (!remaining.empty()) {
    const auto line_end = remaining.find('\n');
    if (line_end == std::string_view::npos) {
      result.error = NativeArtifactError::malformed;
      return result;
    }
    const auto line = remaining.substr(0U, line_end);
    remaining.remove_prefix(line_end + 1U);
    if (line.empty()) {
      continue;
    }
    const auto separator = line.find('=');
    if (separator == std::string_view::npos) {
      result.error = NativeArtifactError::malformed;
      return result;
    }
    const auto key = line.substr(0U, separator);
    const auto value = line.substr(separator + 1U);
    std::size_t seen_index{};
    bool parsed = true;
    if (key == "role") {
      seen_index = 0U;
      parsed = parse_role(value, result.artifact.role);
    } else if (key == "family") {
      seen_index = 1U;
      parsed = parse_family(value, result.artifact.family);
    } else if (key == "model_id") {
      seen_index = 2U;
      parsed = parse_identifier(value, result.artifact.metadata.model_id);
    } else if (key == "model_version") {
      seen_index = 3U;
      parsed = parse_identifier(value, result.artifact.metadata.model_version);
    } else if (key == "instrument_id") {
      seen_index = 4U;
      parsed = parse_identifier(value, result.artifact.metadata.instrument_id);
    } else if (key == "configuration_version") {
      seen_index = 5U;
      parsed = parse_identifier(value, result.artifact.metadata.configuration_version);
    } else if (key == "feature_definition_version") {
      seen_index = 6U;
      parsed =
          parse_identifier(value, result.artifact.metadata.feature_definition_version);
    } else if (key == "horizon_ns") {
      seen_index = 7U;
      parsed = parse_integer(value, result.artifact.metadata.horizon_ns);
    } else if (key == "forecast_ttl_ns") {
      seen_index = 8U;
      parsed = parse_integer(value, result.artifact.metadata.forecast_ttl_ns);
    } else if (key == "expires_wall_clock_utc_ns") {
      seen_index = 9U;
      parsed = parse_integer(value, result.artifact.expires_wall_clock_utc_ns);
    } else if (key == "training_sample_count") {
      seen_index = 10U;
      parsed = parse_integer(value, result.artifact.training_sample_count);
    } else if (key == "calibration_error_ppm") {
      seen_index = 11U;
      parsed = parse_integer(value, result.artifact.calibration_error_ppm);
    } else if (key == "ood_reject_threshold_ppm") {
      seen_index = 12U;
      parsed = parse_integer(value, result.artifact.metadata.ood.reject_threshold_ppm);
    } else if (key == "intercept_ppm") {
      seen_index = 13U;
      parsed = parse_integer(value, result.artifact.intercept_ppm);
    } else if (key == "platt_slope_ppm") {
      seen_index = 14U;
      parsed = parse_integer(value, result.artifact.platt_slope_ppm);
    } else if (key == "platt_intercept_ppm") {
      seen_index = 15U;
      parsed = parse_integer(value, result.artifact.platt_intercept_ppm);
    } else if (key == "fee_share_ppm") {
      seen_index = 16U;
      parsed = parse_integer(value, result.artifact.fee_share_ppm);
    } else if (key == "spread_share_ppm") {
      seen_index = 17U;
      parsed = parse_integer(value, result.artifact.spread_share_ppm);
    } else if (key == "slippage_share_ppm") {
      seen_index = 18U;
      parsed = parse_integer(value, result.artifact.slippage_share_ppm);
    } else if (key == "impact_share_ppm") {
      seen_index = 19U;
      parsed = parse_integer(value, result.artifact.impact_share_ppm);
    } else if (key == "adverse_selection_share_ppm") {
      seen_index = 20U;
      parsed = parse_integer(value, result.artifact.adverse_selection_share_ppm);
    } else if (key == "feature_count") {
      parsed = parse_integer(value, declared_feature_count);
      if (!parsed || seen_feature_count || declared_feature_count == 0U ||
          declared_feature_count > kMaximumNativeModelFeatures) {
        result.error = NativeArtifactError::capacity_exhausted;
        return result;
      }
      seen_feature_count = true;
      continue;
    } else if (key == "feature") {
      if (result.artifact.feature_count >= result.artifact.features.size() ||
          !parse_feature(value,
                         result.artifact.features[result.artifact.feature_count])) {
        result.error = NativeArtifactError::invalid_feature;
        return result;
      }
      const auto feature = result.artifact.features[result.artifact.feature_count].name;
      for (std::size_t index = 0U; index < result.artifact.feature_count; ++index) {
        if (result.artifact.features[index].name == feature) {
          result.error = NativeArtifactError::duplicate_feature;
          return result;
        }
      }
      ++result.artifact.feature_count;
      continue;
    } else {
      result.error = NativeArtifactError::malformed;
      return result;
    }
    if (!parsed || seen[seen_index]) {
      result.error = NativeArtifactError::malformed;
      return result;
    }
    seen[seen_index] = true;
  }
  if (std::ranges::find(seen, false) != seen.end() || !seen_feature_count ||
      declared_feature_count != result.artifact.feature_count) {
    result.error = NativeArtifactError::malformed;
    return result;
  }
  if (result.artifact.family == NativeModelFamily::gradient_boosted_trees) {
    result.error = NativeArtifactError::unsupported_family;
    return result;
  }
  if (!valid_queue_contract(result.artifact)) {
    result.error = NativeArtifactError::invalid_feature;
    return result;
  }
  if (result.artifact.intercept_ppm < -kMaximumArtifactNumericValue ||
      result.artifact.intercept_ppm > kMaximumArtifactNumericValue) {
    result.error = NativeArtifactError::invalid_normalization;
    return result;
  }
  if (result.artifact.training_sample_count == 0U ||
      result.artifact.platt_slope_ppm < 0 ||
      result.artifact.platt_slope_ppm > kMaximumArtifactNumericValue ||
      result.artifact.platt_intercept_ppm < -kMaximumArtifactNumericValue ||
      result.artifact.platt_intercept_ppm > kMaximumArtifactNumericValue ||
      result.artifact.calibration_error_ppm > kProbabilityScale) {
    result.error = NativeArtifactError::invalid_calibration;
    return result;
  }
  const auto cost_share_sum =
      static_cast<std::uint64_t>(result.artifact.fee_share_ppm) +
      result.artifact.spread_share_ppm + result.artifact.slippage_share_ppm +
      result.artifact.impact_share_ppm + result.artifact.adverse_selection_share_ppm;
  if ((result.artifact.role == MicrostructureModelRole::transaction_cost &&
       cost_share_sum != kProbabilityScale) ||
      (result.artifact.role != MicrostructureModelRole::transaction_cost &&
       cost_share_sum != 0U)) {
    result.error = NativeArtifactError::invalid_cost_shares;
    return result;
  }
  if (now_wall_clock_utc_ns <= 0 ||
      result.artifact.expires_wall_clock_utc_ns <= now_wall_clock_utc_ns) {
    result.error = NativeArtifactError::expired;
    return result;
  }

  result.artifact.metadata.kind =
      result.artifact.family == NativeModelFamily::logistic_regression
          ? ModelKind::logistic_regression
          : ModelKind::linear_regression;
  result.artifact.metadata.calibration = {
      .method = CalibrationMethod::platt,
      .calibration_version = result.artifact.metadata.configuration_version,
      .fitted_sample_count = result.artifact.training_sample_count,
      .expected_calibration_error_ppm = result.artifact.calibration_error_ppm,
  };
  result.artifact.metadata.ood.method = OodMethod::bounded_feature_range;
  result.artifact.metadata.ood.detector_version =
      result.artifact.metadata.configuration_version;
  result.artifact.activation_expiration_process_monotonic_time_ns =
      activation_expiration_process_monotonic_time_ns;
  for (std::size_t index = 0U; index < result.artifact.feature_count; ++index) {
    const auto& transform = result.artifact.features[index];
    if (context_feature(transform.name)) {
      continue;
    }
    auto& requirement = result.artifact.metadata
                            .requirements[result.artifact.metadata.requirement_count++];
    requirement = {
        .name = static_cast<features::FeatureName>(
            static_cast<std::uint8_t>(transform.name)),
        .minimum_value = -kMaximumArtifactNumericValue,
        .maximum_value = kMaximumArtifactNumericValue,
        .maximum_age_ns = result.artifact.metadata.forecast_ttl_ns,
        .allow_degraded = true,
    };
  }
  if (!valid_model_metadata(result.artifact.metadata)) {
    result.error = NativeArtifactError::invalid_metadata;
  }
  return result;
}
// NOLINTEND(bugprone-easily-swappable-parameters)

PredictionStatus NativeMicrostructureModel::predict(const ModelInput& input,
                                                    ModelPrediction& output) noexcept {
  if (input.deadline.submitted_process_monotonic_time_ns == 0U ||
      input.deadline.submitted_process_monotonic_time_ns >
          artifact_.activation_expiration_process_monotonic_time_ns) {
    return PredictionStatus::unavailable;
  }
  auto score = artifact_.intercept_ppm;
  std::uint32_t ood_score{};
  for (std::size_t index = 0U; index < artifact_.feature_count; ++index) {
    const auto& transform = artifact_.features[index];
    std::int64_t raw{};
    std::int64_t normalized{};
    if (!feature_value(input, transform.name, raw) ||
        !normalized_value(raw, transform, normalized)) {
      return PredictionStatus::invalid_input;
    }
    const auto term = (normalized * transform.weight_ppm) /
                      static_cast<std::int64_t>(kProbabilityScale);
    if ((term > 0 && score > std::numeric_limits<std::int64_t>::max() - term) ||
        (term < 0 && score < std::numeric_limits<std::int64_t>::min() - term)) {
      return PredictionStatus::numeric_error;
    }
    score += term;
    ood_score = std::max(ood_score, ood_contribution(raw, transform));
  }
  if (ood_score > artifact_.metadata.ood.reject_threshold_ppm) {
    return PredictionStatus::invalid_input;
  }
  if (artifact_.family == NativeModelFamily::logistic_regression) {
    score = ((score * artifact_.platt_slope_ppm) /
             static_cast<std::int64_t>(kProbabilityScale)) +
            artifact_.platt_intercept_ppm;
  }

  if (artifact_.role == MicrostructureModelRole::transaction_cost ||
      artifact_.role == MicrostructureModelRole::market_impact) {
    const auto cost = static_cast<std::uint64_t>(std::clamp(
        score, std::int64_t{0}, static_cast<std::int64_t>(kProbabilityScale)));
    output =
        baseline_prediction(input.feature_snapshot, -static_cast<std::int64_t>(cost));
    output.transaction_cost.present = true;
    if (artifact_.role == MicrostructureModelRole::transaction_cost) {
      output.transaction_cost.fee_cost_ppm =
          cost_component(cost, artifact_.fee_share_ppm);
      output.transaction_cost.spread_cost_ppm =
          cost_component(cost, artifact_.spread_share_ppm);
      output.transaction_cost.slippage_cost_ppm =
          cost_component(cost, artifact_.slippage_share_ppm);
      output.transaction_cost.market_impact_ppm =
          cost_component(cost, artifact_.impact_share_ppm);
      output.transaction_cost.adverse_selection_cost_ppm =
          cost_component(cost, artifact_.adverse_selection_share_ppm);
    } else {
      output.transaction_cost.market_impact_ppm = cost;
    }
  } else {
    const auto probability = deterministic_sigmoid_ppm(score);
    output = baseline_prediction(input.feature_snapshot,
                                 static_cast<std::int64_t>(probability) -
                                     static_cast<std::int64_t>(kProbabilityScale / 2U));
    output.probability_down_ppm = kProbabilityScale - probability;
    output.probability_flat_ppm = 0U;
    output.probability_up_ppm = probability;
    const auto distance = probability > kProbabilityScale / 2U
                              ? probability - (kProbabilityScale / 2U)
                              : (kProbabilityScale / 2U) - probability;
    output.confidence_ppm = std::min<std::uint32_t>(
        kProbabilityScale, static_cast<std::uint32_t>(distance * 2U));
  }
  output.calibration_score_ppm = kProbabilityScale - artifact_.calibration_error_ppm;
  output.data_quality_score_ppm =
      input.feature_snapshot.state == features::EngineState::ready
          ? kProbabilityScale
          : kProbabilityScale / 2U;
  output.ood_score_ppm = ood_score;
  return PredictionStatus::ok;
}

} // namespace aegis::models
