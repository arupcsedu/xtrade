#include "aegis/models/microstructure.hpp"
#include "aegis/models/microstructure_dataset.hpp"
#include "test_support.hpp"

#include "aegis/common/sha256.hpp"
#include "aegis/market_data/synthetic/config.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>

namespace models = aegis::models;
namespace synthetic = aegis::market_data::synthetic;

namespace {

[[nodiscard]] std::string digest_hex(const aegis::common::Sha256Digest& digest) {
  constexpr std::string_view kDigits{"0123456789abcdef"};
  std::string output(digest.size() * 2U, '0');
  for (std::size_t index = 0U; index < digest.size(); ++index) {
    output[index * 2U] = kDigits[digest[index] >> 4U];
    output[(index * 2U) + 1U] = kDigits[digest[index] & 0x0FU];
  }
  return output;
}

// Compact artifact fixtures keep serialized field order visible at call sites.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
[[nodiscard]] std::string
signed_artifact(const std::string_view role, const std::string_view family,
                const std::string_view features,
                const std::string_view cost_shares =
                    "fee_share_ppm=0\nspread_share_ppm=0\nslippage_share_ppm=0\n"
                    "impact_share_ppm=0\nadverse_selection_share_ppm=0\n",
                const std::int64_t expires_ns = 1'900'000'000'000'000'000LL,
                const std::int64_t intercept_ppm = 0) {
  std::ostringstream body;
  body << "AEGIS_MX_NATIVE_MODEL_V1\n"
       << "role=" << role << '\n'
       << "family=" << family << '\n'
       << "model_id=3:3\n"
       << "model_version=4:4\n"
       << "instrument_id=1:1\n"
       << "configuration_version=5:5\n"
       << "feature_definition_version=11:11\n"
       << "horizon_ns=1000000\n"
       << "forecast_ttl_ns=1000\n"
       << "expires_wall_clock_utc_ns=" << expires_ns << '\n'
       << "training_sample_count=1000\n"
       << "calibration_error_ppm=20000\n"
       << "ood_reject_threshold_ppm=1000000\n"
       << "intercept_ppm=" << intercept_ppm << '\n'
       << "platt_slope_ppm=1000000\n"
       << "platt_intercept_ppm=0\n"
       << cost_shares << "feature_count=";
  std::uint32_t feature_count{};
  for (std::size_t position = 0U;
       (position = features.find("feature=", position)) != std::string_view::npos;
       position += 8U) {
    ++feature_count;
  }
  body << feature_count << '\n' << features;
  const auto body_text = body.str();
  const auto bytes = std::span<const std::uint8_t>{
      reinterpret_cast<const std::uint8_t*>(body_text.data()), body_text.size()};
  return body_text + "signature_sha256=" + digest_hex(aegis::common::sha256(bytes)) +
         '\n';
}
// NOLINTEND(bugprone-easily-swappable-parameters)

[[nodiscard]] constexpr std::string_view
role_features(const std::string_view role) noexcept {
  constexpr std::string_view kQueue{
      "feature=quantity_ahead_units,100,100,1,1000,-100000\n"
      "feature=add_rate_millihertz,0,1000,0,1000000,-100000\n"
      "feature=cancel_rate_millihertz,0,1000,0,1000000,200000\n"
      "feature=execute_rate_millihertz,0,1000,0,1000000,300000\n"
      "feature=order_age_ns,0,1000,0,1000000,100000\n"
      "feature=price_distance_ticks,0,1,0,100,100000\n"
      "feature=venue_number,1,1,1,8,10000\n"
      "feature=session_progress_ppm,0,1000000,0,1000000,10000\n"};
  constexpr std::string_view kPassive{
      "feature=quantity_ahead_units,100,100,1,1000,-100000\n"
      "feature=add_rate_millihertz,0,1000,0,1000000,-100000\n"
      "feature=cancel_rate_millihertz,0,1000,0,1000000,200000\n"
      "feature=execute_rate_millihertz,0,1000,0,1000000,300000\n"
      "feature=order_age_ns,0,1000,0,1000000,100000\n"
      "feature=price_distance_ticks,0,1,0,100,100000\n"
      "feature=venue_number,1,1,1,8,10000\n"
      "feature=session_progress_ppm,0,1000000,0,1000000,10000\n"
      "feature=order_quantity_units,100,100,1,1000,10000\n"};
  if (role == "queue_depletion") {
    return kQueue;
  }
  if (role == "passive_fill_probability") {
    return kPassive;
  }
  return "feature=spread_ticks,0,10,0,100,100000\n";
}

[[nodiscard]] constexpr std::string_view
role_cost_shares(const std::string_view role) noexcept {
  if (role == "transaction_cost") {
    return "fee_share_ppm=100000\nspread_share_ppm=300000\n"
           "slippage_share_ppm=200000\nimpact_share_ppm=250000\n"
           "adverse_selection_share_ppm=150000\n";
  }
  return "fee_share_ppm=0\nspread_share_ppm=0\n"
         "slippage_share_ppm=0\nimpact_share_ppm=0\n"
         "adverse_selection_share_ppm=0\n";
}

[[nodiscard]] models::NativeArtifactLoadResult
load(const std::string& encoded,
     const std::int64_t now_ns = 1'800'000'000'000'000'000LL) {
  return models::load_native_model_artifact(encoded, now_ns, 10'000U);
}

} // namespace

TEST(NativeArtifactTest, VerifiesSignatureAndRejectsExpiry) {
  const auto encoded = signed_artifact(
      "mid_price_direction", "logistic_regression",
      "feature=top_level_imbalance_ppm,0,1000000,-1000000,1000000,1000000\n");
  EXPECT_TRUE(load(encoded).ok());

  auto tampered = encoded;
  tampered[tampered.find("1000000,1000000") + 8U] = '9';
  EXPECT_EQ(load(tampered).error, models::NativeArtifactError::invalid_signature);

  const auto expired = signed_artifact(
      "mid_price_direction", "logistic_regression",
      "feature=top_level_imbalance_ppm,0,1000000,-1000000,1000000,1000000\n",
      "fee_share_ppm=0\nspread_share_ppm=0\nslippage_share_ppm=0\n"
      "impact_share_ppm=0\nadverse_selection_share_ppm=0\n",
      1'700'000'000'000'000'000LL);
  EXPECT_EQ(load(expired).error, models::NativeArtifactError::expired);
}

TEST(NativeMicrostructureModelTest, DeterministicLogisticInferenceMatchesTable) {
  const auto loaded = load(signed_artifact(
      "mid_price_direction", "logistic_regression",
      "feature=top_level_imbalance_ppm,0,1000000,-1000000,1000000,1000000\n"));
  ASSERT_TRUE(loaded.ok());
  models::NativeMicrostructureModel model{loaded.artifact};
  auto input = models::test::input();
  input.feature_snapshot
      .values[static_cast<std::size_t>(
          aegis::features::FeatureName::top_level_imbalance_ppm)]
      .value = 500'000;
  input.feature_snapshot.stable_hash =
      aegis::features::stable_snapshot_hash(input.feature_snapshot);
  models::ModelPrediction output{};
  EXPECT_EQ(model.predict(input, output), models::PredictionStatus::ok);
  EXPECT_EQ(output.probability_up_ppm, 615'529U);
  EXPECT_EQ(output.probability_down_ppm, 384'471U);
  EXPECT_EQ(output.expected_return_ppm, 115'529);
  EXPECT_EQ(output.calibration_score_ppm, 980'000U);
  input.deadline.submitted_process_monotonic_time_ns = 10'001U;
  EXPECT_EQ(model.predict(input, output), models::PredictionStatus::unavailable);
}

TEST(NativeMicrostructureModelTest, QueueContractRequiresAllEightInputs) {
  const auto loaded =
      load(signed_artifact("queue_depletion", "logistic_regression",
                           "feature=quantity_ahead_units,100,100,1,1000,-100000\n"
                           "feature=add_rate_millihertz,0,1000,0,1000000,-100000\n"
                           "feature=cancel_rate_millihertz,0,1000,0,1000000,200000\n"
                           "feature=execute_rate_millihertz,0,1000,0,1000000,300000\n"
                           "feature=order_age_ns,0,1000,0,1000000,100000\n"
                           "feature=price_distance_ticks,0,1,0,100,100000\n"
                           "feature=venue_number,1,1,1,8,10000\n"
                           "feature=session_progress_ppm,0,1000000,0,1000000,10000\n"));
  ASSERT_TRUE(loaded.ok());
  models::NativeMicrostructureModel model{loaded.artifact};
  auto input = models::test::input();
  input.microstructure_context = {.quantity_ahead_units = 200U,
                                  .order_quantity_units = 50U,
                                  .order_age_ns = 5'000U,
                                  .price_distance_ticks = 1,
                                  .venue_number = 2U,
                                  .fee_rate_ppm = 30U,
                                  .present = true};
  models::ModelPrediction output{};
  EXPECT_EQ(model.predict(input, output), models::PredictionStatus::ok);
  input.microstructure_context.present = false;
  EXPECT_EQ(model.predict(input, output), models::PredictionStatus::invalid_input);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(NativeMicrostructureModelTest, AllSevenRolesLoadAndCostHasFiveParts) {
  constexpr std::array<std::string_view, 7> kRoles{
      "mid_price_direction", "spread_widening",
      "queue_depletion",     "passive_fill_probability",
      "adverse_selection",   "transaction_cost",
      "market_impact"};
  for (const auto role : kRoles) {
    const bool regression = role == "transaction_cost" || role == "market_impact";
    const auto loaded = load(
        signed_artifact(role, regression ? "linear_regression" : "logistic_regression",
                        role_features(role), role_cost_shares(role),
                        1'900'000'000'000'000'000LL, regression ? 100'000 : 0));
    ASSERT_TRUE(loaded.ok()) << role;
    models::NativeMicrostructureModel model{loaded.artifact};
    auto input = models::test::input();
    input.microstructure_context = {.quantity_ahead_units = 200U,
                                    .order_quantity_units = 100U,
                                    .order_age_ns = 5'000U,
                                    .price_distance_ticks = 1,
                                    .venue_number = 2U,
                                    .fee_rate_ppm = 30U,
                                    .present = true};
    input.feature_snapshot
        .values[static_cast<std::size_t>(aegis::features::FeatureName::spread_ticks)]
        .value = 10;
    input.feature_snapshot.stable_hash =
        aegis::features::stable_snapshot_hash(input.feature_snapshot);
    models::ModelPrediction output{};
    EXPECT_EQ(model.predict(input, output), models::PredictionStatus::ok) << role;
    if (role == "transaction_cost") {
      EXPECT_TRUE(output.transaction_cost.present);
      EXPECT_GT(output.transaction_cost.fee_cost_ppm, 0U);
      EXPECT_GT(output.transaction_cost.spread_cost_ppm, 0U);
      EXPECT_GT(output.transaction_cost.slippage_cost_ppm, 0U);
      EXPECT_GT(output.transaction_cost.market_impact_ppm, 0U);
      EXPECT_GT(output.transaction_cost.adverse_selection_cost_ppm, 0U);
    }
  }
}

TEST(MicrostructureDatasetTest, SimulatorRowsAreSeedDeterministic) {
  auto config = synthetic::make_default_config();
  config.seed = 20'260'829U;
  config.event_count = 128U;
  std::ostringstream first;
  std::ostringstream second;
  models::MicrostructureDatasetSummary first_summary{};
  models::MicrostructureDatasetSummary second_summary{};
  ASSERT_TRUE(
      models::write_synthetic_microstructure_dataset(config, first, first_summary));
  ASSERT_TRUE(
      models::write_synthetic_microstructure_dataset(config, second, second_summary));
  EXPECT_EQ(first.str(), second.str());
  EXPECT_EQ(first_summary.row_count, 128U);
  EXPECT_EQ(first_summary.stable_hash, second_summary.stable_hash);
  EXPECT_NE(first_summary.stable_hash, 0U);
}
