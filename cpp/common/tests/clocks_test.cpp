#include "aegis/time/clock_jump.hpp"
#include "aegis/time/clocks.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace aegis::time {
namespace {

TEST(TestClockTest, MonotonicAdvanceIsCheckedAndNeverMovesBackward) {
  TestMonotonicClock clock{MonotonicTimeNs{100U}};
  EXPECT_EQ(clock.advance(DurationNs{5}), TimeError::none);
  ASSERT_TRUE(clock.monotonic_now().ok());
  EXPECT_EQ(clock.monotonic_now().value, MonotonicTimeNs{105U});
  EXPECT_EQ(clock.advance(DurationNs{-1}), TimeError::invalid_duration);
  EXPECT_EQ(clock.monotonic_now().value, MonotonicTimeNs{105U});
}

TEST(TestClockTest, WallClockSupportsDeterministicBackwardJump) {
  TestWallClock clock{WallClockTimeNs{1'000}};
  EXPECT_EQ(clock.advance(DurationNs{-400}), TimeError::none);
  EXPECT_EQ(clock.wall_now().value, WallClockTimeNs{600});
}

TEST(SimulatedClockTest, AdvancesDomainsAtomicallyOnSuccess) {
  SimulatedClock clock{WallClockTimeNs{10'000}, MonotonicTimeNs{50U}};
  EXPECT_EQ(clock.advance(DurationNs{25}), TimeError::none);
  EXPECT_EQ(clock.wall_now().value, WallClockTimeNs{10'025});
  EXPECT_EQ(clock.monotonic_now().value, MonotonicTimeNs{75U});
}

TEST(SimulatedClockTest, LeavesBothDomainsUnchangedOnOverflow) {
  SimulatedClock clock{WallClockTimeNs{std::numeric_limits<std::int64_t>::max()},
                       MonotonicTimeNs{20U}};
  EXPECT_EQ(clock.advance(DurationNs{1}), TimeError::overflow);
  EXPECT_EQ(clock.wall_now().value,
            WallClockTimeNs{std::numeric_limits<std::int64_t>::max()});
  EXPECT_EQ(clock.monotonic_now().value, MonotonicTimeNs{20U});
}

TEST(SimulatedClockTest, WallJumpNeverChangesMonotonicTime) {
  SimulatedClock clock{WallClockTimeNs{1'000}, MonotonicTimeNs{100U}};
  EXPECT_EQ(clock.jump_wall(DurationNs{-250}), TimeError::none);
  EXPECT_EQ(clock.wall_now().value, WallClockTimeNs{750});
  EXPECT_EQ(clock.monotonic_now().value, MonotonicTimeNs{100U});
}

TEST(WallClockJumpDetectorTest, DetectsBackwardAndForwardResiduals) {
  WallClockJumpDetector detector{5U};
  EXPECT_EQ(detector.observe(WallClockTimeNs{1'000}, MonotonicTimeNs{100U}).state,
            ClockJumpState::first_sample);

  const auto stable = detector.observe(WallClockTimeNs{1'104}, MonotonicTimeNs{200U});
  EXPECT_EQ(stable.state, ClockJumpState::stable);
  EXPECT_EQ(stable.residual, DurationNs{4});

  const auto backward = detector.observe(WallClockTimeNs{1'054}, MonotonicTimeNs{300U});
  EXPECT_EQ(backward.state, ClockJumpState::backward_jump);
  EXPECT_EQ(backward.residual, DurationNs{-150});

  const auto forward = detector.observe(WallClockTimeNs{1'254}, MonotonicTimeNs{400U});
  EXPECT_EQ(forward.state, ClockJumpState::forward_jump);
  EXPECT_EQ(forward.residual, DurationNs{100});
}

TEST(WallClockJumpDetectorTest, RejectsEqualOrRegressingMonotonicSamples) {
  WallClockJumpDetector detector{0U};
  static_cast<void>(detector.observe(WallClockTimeNs{100}, MonotonicTimeNs{10U}));
  EXPECT_EQ(detector.observe(WallClockTimeNs{101}, MonotonicTimeNs{10U}).state,
            ClockJumpState::monotonic_regression);
  EXPECT_EQ(detector.observe(WallClockTimeNs{99}, MonotonicTimeNs{9U}).error,
            TimeError::regression);
}

} // namespace
} // namespace aegis::time
