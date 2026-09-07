#include "aegis/observability/adapters.hpp"
#include "aegis/observability/structured_log.hpp"
#include "aegis/observability/telemetry.hpp"
#include "aegis/observability/tracing.hpp"

#include "../../models/tests/test_support.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>

namespace aegis::observability::test {
namespace {

TEST(TelemetryTest, AggregatesFixedMetricsAndStageQuantiles) {
  TelemetryPublisher publisher;
  TelemetryProcessor processor{publisher};
  ASSERT_TRUE(publisher.counter_add(MetricId::market_packets, 2U));
  ASSERT_TRUE(publisher.gauge_set(MetricId::market_feed_age_ns, 150));
  ASSERT_TRUE(publisher.gauge_set(MetricId::ensemble_weight_ppm, 750'000, 1U));
  ASSERT_TRUE(publisher.counter_set(MetricId::infrastructure_cache_misses,
                                    std::numeric_limits<std::uint64_t>::max()));
  ASSERT_TRUE(publisher.observe_latency(LatencyStage::risk_check, 100U));
  ASSERT_TRUE(publisher.observe_latency(LatencyStage::risk_check, 1'500U));
  ASSERT_EQ(processor.drain(16U), 6U);

  const auto snapshot = processor.snapshot();
  EXPECT_EQ(snapshot.counters[static_cast<std::size_t>(MetricId::market_packets)][0U],
            2U);
  EXPECT_EQ(snapshot.gauges[static_cast<std::size_t>(MetricId::market_feed_age_ns)][0U],
            150);
  EXPECT_EQ(snapshot.counters[static_cast<std::size_t>(
                MetricId::infrastructure_cache_misses)][0U],
            std::numeric_limits<std::uint64_t>::max());
  const auto& histogram =
      snapshot.latency[static_cast<std::size_t>(LatencyStage::risk_check)];
  EXPECT_EQ(histogram.count, 2U);
  EXPECT_EQ(histogram.maximum_ns, 1'500U);
  EXPECT_EQ(histogram_quantile_ns(histogram, 500U), 100U);
  EXPECT_EQ(histogram_quantile_ns(histogram, 999U), 2'500U);

  const auto text = PrometheusExporter::render(snapshot);
  EXPECT_NE(text.find("aegis_market_data_packets_total 2"), std::string::npos);
  EXPECT_NE(text.find("aegis_ensemble_weight_ppm{slot=\"1\"} 750000"),
            std::string::npos);
  EXPECT_NE(text.find("stage=\"risk_check\",quantile=\"999\""), std::string::npos);
}

TEST(TelemetryTest, RejectsOperationThatDoesNotMatchMetricType) {
  TelemetryPublisher publisher;
  EXPECT_FALSE(publisher.publish({.metric = MetricId::market_feed_age_ns,
                                  .operation = MetricOperation::counter_add,
                                  .value_bits = 1U}));
  EXPECT_EQ(publisher.dropped_points(), 1U);
}

TEST(TelemetryTest, FullQueueRejectsNewestAndCountsLoss) {
  TelemetryPublisher publisher;
  for (std::size_t index = 0U; index < kTelemetryQueueCapacity; ++index) {
    ASSERT_TRUE(publisher.counter_add(MetricId::market_packets));
  }
  EXPECT_FALSE(publisher.counter_add(MetricId::market_packets));
  EXPECT_EQ(publisher.dropped_points(), 1U);
  EXPECT_EQ(publisher.queue_metrics().consumer_lag_events, kTelemetryQueueCapacity);
}

TEST(TelemetryTest, AdaptersCoverMarketModelTradingAndInfrastructureFamilies) {
  TelemetryPublisher publisher;
  const market_data::feed::FeedMetrics feed{.packets_received = 10U,
                                            .normalized_events = 8U,
                                            .duplicate_packets = 1U,
                                            .canonical_gaps = 2U};
  const market_data::feed::DataQualityState quality{
      .identity = {}, .observed_process_monotonic_time_ns = 900U};
  ASSERT_TRUE(publish_feed_metrics(publisher, feed, quality, 1'000U, true));
  ASSERT_TRUE(publish_risk_metrics(
      publisher, {.evaluations = 7U, .approvals = 5U, .rejections = 2U}));
  ASSERT_TRUE(publish_gateway_metrics(
      publisher, {.new_orders = 4U, .cancels = 3U, .rejects = 1U, .fills = 2U}));
  ASSERT_TRUE(publish_infrastructure_metrics(publisher, {.cpu_utilization_ppm = 500'000,
                                                         .numa_remote_accesses = 11U,
                                                         .cache_misses = 12U,
                                                         .ring_occupancy = 13U,
                                                         .queue_drops = 14U,
                                                         .nic_errors = 15U,
                                                         .clock_offset_ns = -16,
                                                         .journal_lag_events = 17U}));
  ASSERT_TRUE(
      publish_model_result(publisher,
                           {.status = models::RunStatus::accepted,
                            .validation_error = models::ForecastValidationError::none,
                            .forecast = models::test::valid_forecast()},
                           {.model_slot = 1U,
                            .now_process_monotonic_time_ns = 200U,
                            .inference_latency_ns = 25U}));
  ASSERT_TRUE(
      publish_observability_drops(publisher, {.structured_log_drops = 2U,
                                              .trace_drops = 3U,
                                              .decision_explanation_drops = 4U}));
  TelemetryProcessor processor{publisher};
  EXPECT_GT(processor.drain(64U), 0U);
  const auto snapshot = processor.snapshot();
  EXPECT_EQ(snapshot.gauges[static_cast<std::size_t>(MetricId::market_book_valid)][0U],
            1);
  EXPECT_EQ(snapshot.counters[static_cast<std::size_t>(MetricId::trading_fills)][0U],
            2U);
  EXPECT_EQ(snapshot.gauges[static_cast<std::size_t>(
                MetricId::infrastructure_clock_offset_ns)][0U],
            -16);
  EXPECT_EQ(snapshot.gauges[static_cast<std::size_t>(MetricId::model_ood_ppm)][1U],
            10'000);
  EXPECT_EQ(
      snapshot
          .counters[static_cast<std::size_t>(MetricId::observability_log_drops)][0U],
      6U);
}

TEST(TelemetryTest, InvalidAcceptedModelForecastDoesNotPublishValues) {
  TelemetryPublisher publisher;
  auto forecast = models::test::valid_forecast();
  ++forecast.prediction.ood_score_ppm;
  EXPECT_FALSE(
      publish_model_result(publisher,
                           {.status = models::RunStatus::accepted,
                            .validation_error = models::ForecastValidationError::none,
                            .forecast = forecast},
                           {.model_slot = 1U,
                            .now_process_monotonic_time_ns = 200U,
                            .inference_latency_ns = 25U}));
}

TEST(StructuredLogTest, UsesFixedCodesAndCorrelationId) {
  StructuredLogRecord record{.severity = LogSeverity::warning,
                             .component = Component::risk,
                             .event_code = LogEventCode::risk_rejected,
                             .correlation_id = common::GlobalEventId{1U, 2U},
                             .session_id = common::SessionId{3U, 4U},
                             .configuration_version =
                                 common::ConfigurationVersion{5U, 6U},
                             .process_monotonic_time_ns = 100U,
                             .wall_clock_utc_time_ns = 200,
                             .value_1 = 7};
  record.stable_hash = stable_structured_log_hash(record);
  ASSERT_TRUE(valid_structured_log_record(record));
  StructuredLogPublisher publisher;
  ASSERT_TRUE(publisher.publish(record));
  StructuredLogRecord consumed;
  ASSERT_TRUE(publisher.consume(consumed));
  EXPECT_EQ(consumed.stable_hash, record.stable_hash);
  const auto json = JsonLogExporter::render(consumed);
  EXPECT_NE(json.find("\"event\":\"risk_rejected\""), std::string::npos);
  EXPECT_NE(json.find("00000000000000010000000000000002"), std::string::npos);
}

TEST(TracingTest, ProducesOtlpJsonOnlyThroughOffPathBoundary) {
  TraceSpanRecord span{.trace_id = common::GlobalEventId{1U, 2U},
                       .span_id = 3U,
                       .correlation_id = common::GlobalEventId{4U, 5U},
                       .session_id = common::SessionId{6U, 7U},
                       .configuration_version = common::ConfigurationVersion{8U, 9U},
                       .component = Component::models,
                       .operation = TraceOperation::asynchronous_model_inference,
                       .status = TraceStatus::ok,
                       .start_process_monotonic_time_ns = 100U,
                       .end_process_monotonic_time_ns = 125U,
                       .start_wall_clock_utc_time_ns = 1'000,
                       .end_wall_clock_utc_time_ns = 1'025};
  span.stable_hash = stable_trace_span_hash(span);
  TracePublisher publisher;
  ASSERT_TRUE(publisher.publish_off_path(span));
  TraceSpanRecord consumed;
  ASSERT_TRUE(publisher.consume(consumed));
  const auto json = OtlpJsonExporter::render(consumed);
  EXPECT_NE(json.find("\"resourceSpans\""), std::string::npos);
  EXPECT_NE(json.find("asynchronous_model_inference"), std::string::npos);
  EXPECT_NE(json.find("aegis.correlation_id"), std::string::npos);
}

} // namespace
} // namespace aegis::observability::test
