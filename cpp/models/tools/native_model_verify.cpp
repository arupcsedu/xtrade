#include "aegis/features/types.hpp"
#include "aegis/models/microstructure.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace models = aegis::models;
namespace features = aegis::features;
namespace common = aegis::common;

namespace {

[[nodiscard]] std::vector<std::string> split(const std::string& line) {
  std::vector<std::string> values;
  std::istringstream input{line};
  for (std::string value; std::getline(input, value, ',');) {
    values.push_back(value);
  }
  return values;
}

[[nodiscard]] bool read_bounded_artifact(const std::filesystem::path& path,
                                         std::string& output) {
  constexpr std::uintmax_t kMaximumArtifactBytes = 1U << 20U;
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error || size == 0U || size > kMaximumArtifactBytes ||
      size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
    return false;
  }
  std::ifstream input{path, std::ios::binary};
  if (!input.is_open()) {
    return false;
  }
  output.resize(static_cast<std::size_t>(size));
  input.read(output.data(), static_cast<std::streamsize>(size));
  return input.good() && input.gcount() == static_cast<std::streamsize>(size);
}

[[nodiscard]] bool parse_integer(const std::string_view value,
                                 std::int64_t& output) noexcept {
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), output);
  return error == std::errc{} && end == value.data() + value.size();
}

[[nodiscard]] bool find_value(const std::vector<std::string>& header,
                              const std::vector<std::string>& row,
                              const std::string_view name,
                              std::int64_t& output) noexcept {
  for (std::size_t index = 0U; index < header.size(); ++index) {
    if (header[index] == name) {
      return index < row.size() && parse_integer(row[index], output);
    }
  }
  return false;
}

[[nodiscard]] bool set_context_value(models::MicrostructureContext& context,
                                     const models::NativeFeatureName name,
                                     const std::int64_t value) noexcept {
  if (value < 0) {
    return false;
  }
  switch (name) {
  case models::NativeFeatureName::quantity_ahead_units:
    context.quantity_ahead_units = static_cast<std::uint64_t>(value);
    break;
  case models::NativeFeatureName::order_quantity_units:
    context.order_quantity_units = static_cast<std::uint64_t>(value);
    break;
  case models::NativeFeatureName::order_age_ns:
    context.order_age_ns = static_cast<std::uint64_t>(value);
    break;
  case models::NativeFeatureName::price_distance_ticks:
    context.price_distance_ticks = value;
    break;
  case models::NativeFeatureName::venue_number:
    context.venue_number = static_cast<std::uint32_t>(value);
    break;
  case models::NativeFeatureName::fee_rate_ppm:
    context.fee_rate_ppm = static_cast<std::uint32_t>(value);
    break;
  default:
    return false;
  }
  context.present = true;
  return true;
}

} // namespace

int main(const int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "Usage: native-model-verify ARTIFACT DATASET_CSV\n";
    return 2;
  }
  std::string encoded;
  if (!read_bounded_artifact(argv[1], encoded)) {
    std::cerr << "artifact read failed\n";
    return 1;
  }
  const auto loaded =
      models::load_native_model_artifact(encoded, 1'800'000'000'000'000'000LL, 10'000U);
  if (!loaded.ok()) {
    std::cerr << "artifact activation failed error="
              << static_cast<unsigned>(loaded.error) << '\n';
    return 1;
  }

  std::ifstream dataset{argv[2]};
  std::string header_line;
  std::string row_line;
  if (!std::getline(dataset, header_line) || !std::getline(dataset, row_line)) {
    std::cerr << "dataset read failed\n";
    return 1;
  }
  const auto header = split(header_line);
  const auto row = split(row_line);
  if (header.size() != row.size()) {
    std::cerr << "dataset shape mismatch\n";
    return 1;
  }

  models::ModelInput input{
      .forecast_id = common::ForecastId{1U, 1U},
      .feature_snapshot = {.snapshot_id = common::FeatureSnapshotId{2U, 2U},
                           .instrument_id = common::InstrumentId{1U, 1U},
                           .session_id = common::SessionId{3U, 3U},
                           .feature_definition_version =
                               common::ConfigurationVersion{11U, 11U},
                           .source_first_global_event_id =
                               common::GlobalEventId{4U, 4U},
                           .source_last_global_event_id = common::GlobalEventId{5U, 5U},
                           .source_first_ordinal = 1U,
                           .source_last_ordinal = 1U,
                           .as_of_exchange_event_time_ns = 1'800'000'000'000'000'000LL,
                           .created_process_monotonic_time_ns = 100U,
                           .state = features::EngineState::ready},
      .deadline = {.submitted_process_monotonic_time_ns = 100U,
                   .complete_by_process_monotonic_time_ns = 200U},
      .microstructure_context = {}};
  for (auto& feature : input.feature_snapshot.values) {
    feature = {.value = 0,
               .as_of_process_monotonic_time_ns = 100U,
               .validity = features::FeatureValidity::valid};
  }
  for (std::size_t index = 0U; index < loaded.artifact.feature_count; ++index) {
    const auto feature_name = loaded.artifact.features[index].name;
    std::int64_t value{};
    if (!find_value(header, row, models::native_feature_name(feature_name), value)) {
      std::cerr << "missing artifact feature in dataset\n";
      return 1;
    }
    const auto numeric = static_cast<std::uint8_t>(feature_name);
    if (numeric < static_cast<std::uint8_t>(features::FeatureName::count)) {
      input.feature_snapshot.values[numeric].value = value;
    } else if (!set_context_value(input.microstructure_context, feature_name, value)) {
      std::cerr << "invalid context feature\n";
      return 1;
    }
  }
  input.feature_snapshot.stable_hash =
      features::stable_snapshot_hash(input.feature_snapshot);
  models::NativeMicrostructureModel model{loaded.artifact};
  models::ModelPrediction prediction{};
  const auto status = model.predict(input, prediction);
  if (status != models::PredictionStatus::ok) {
    std::cerr << "inference failed status=" << static_cast<unsigned>(status) << '\n';
    return 1;
  }
  std::cout << "role=" << models::microstructure_role_name(loaded.artifact.role)
            << " expected_return_ppm=" << prediction.expected_return_ppm
            << " probability_up_ppm=" << prediction.probability_up_ppm
            << " probability_down_ppm=" << prediction.probability_down_ppm
            << " ood_score_ppm=" << prediction.ood_score_ppm
            << " fee_cost_ppm=" << prediction.transaction_cost.fee_cost_ppm
            << " spread_cost_ppm=" << prediction.transaction_cost.spread_cost_ppm
            << " slippage_cost_ppm=" << prediction.transaction_cost.slippage_cost_ppm
            << " market_impact_ppm=" << prediction.transaction_cost.market_impact_ppm
            << " adverse_selection_cost_ppm="
            << prediction.transaction_cost.adverse_selection_cost_ppm << '\n';
  return 0;
}
