#include "aegis/time/latency.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace aegis::time {
namespace {

[[nodiscard]] LatencyTrace valid_trace() noexcept {
  return {
      .local_receive_time = MonotonicTimeNs{100U},
      .decode_complete_time = MonotonicTimeNs{110U},
      .book_complete_time = MonotonicTimeNs{125U},
      .feature_complete_time = MonotonicTimeNs{145U},
      .inference_start_time = MonotonicTimeNs{150U},
      .inference_complete_time = MonotonicTimeNs{180U},
      .decision_complete_time = MonotonicTimeNs{190U},
      .send_handoff_time = MonotonicTimeNs{202U},
      .acknowledgement_time = MonotonicTimeNs{250U},
  };
}

TEST(LatencyTest, DecomposesAllRequiredStagesExactly) {
  const auto result = decompose_latency(valid_trace());
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value.wire_to_decode, DurationNs{10});
  EXPECT_EQ(result.value.decode_to_book, DurationNs{15});
  EXPECT_EQ(result.value.book_to_feature, DurationNs{20});
  EXPECT_EQ(result.value.inference, DurationNs{30});
  EXPECT_EQ(result.value.decision_to_send, DurationNs{12});
  EXPECT_EQ(result.value.order_round_trip, DurationNs{48});
}

TEST(LatencyTest, RejectsRegressionInUnreportedTraceGap) {
  auto trace = valid_trace();
  trace.inference_start_time = MonotonicTimeNs{140U};
  const auto result = decompose_latency(trace);
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.failed_stage, LatencyStage::feature_to_inference);
  EXPECT_EQ(result.error, TimeError::regression);
}

TEST(LatencyTest, PropagatesUnrepresentableElapsedDuration) {
  auto trace = valid_trace();
  trace.local_receive_time = MonotonicTimeNs{0U};
  trace.decode_complete_time = MonotonicTimeNs{std::uint64_t{1} << 63U};
  trace.book_complete_time = trace.decode_complete_time;
  trace.feature_complete_time = trace.decode_complete_time;
  trace.inference_start_time = trace.decode_complete_time;
  trace.inference_complete_time = trace.decode_complete_time;
  trace.decision_complete_time = trace.decode_complete_time;
  trace.send_handoff_time = trace.decode_complete_time;
  trace.acknowledgement_time = trace.decode_complete_time;

  const auto result = decompose_latency(trace);
  EXPECT_EQ(result.failed_stage, LatencyStage::wire_to_decode);
  EXPECT_EQ(result.error, TimeError::overflow);
}

TEST(LatencyTest, IndividualUtilitiesAreDomainSpecificAndChecked) {
  EXPECT_EQ(inference_latency(MonotonicTimeNs{4U}, MonotonicTimeNs{9U}).value,
            DurationNs{5});
  EXPECT_EQ(order_round_trip_latency(MonotonicTimeNs{9U}, MonotonicTimeNs{8U}).error,
            TimeError::regression);
}

} // namespace
} // namespace aegis::time
