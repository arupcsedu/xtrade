#include "aegis/models/baselines.hpp"
#include "aegis/models/runner.hpp"

#include "test_support.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

namespace models = aegis::models;

// GoogleTest assertion macros intentionally expand into branching control flow.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(AsynchronousModelRunnerTest, PollIsNonBlockingAndExpiredResponseIsDiscarded) {
  std::atomic<std::uint64_t> clock{110U};
  models::ZeroReturnModel model{models::test::metadata()};
  models::AsynchronousModelRunner<4U> runner{model,
                                             {models::test::atomic_clock, &clock}};
  EXPECT_EQ(runner.submit(models::test::input()), models::RunStatus::accepted);

  models::RunResult result{};
  auto status = models::RunStatus::no_result;
  for (std::size_t attempt = 0U;
       attempt < 100'000U && status == models::RunStatus::no_result; ++attempt) {
    status = runner.poll(result);
    std::this_thread::yield();
  }
  ASSERT_EQ(status, models::RunStatus::accepted);
  EXPECT_EQ(result.forecast.prediction.expected_return_ppm, 0);

  EXPECT_EQ(runner.submit(models::test::input(1'000, 110U, 200U)),
            models::RunStatus::accepted);
  do {
    status = runner.poll(result);
    std::this_thread::yield();
  } while (status == models::RunStatus::no_result);
  ASSERT_EQ(status, models::RunStatus::accepted);

  EXPECT_EQ(runner.submit(models::test::input(2'000, 110U, 200U)),
            models::RunStatus::accepted);
  // Let the worker finish, then move the injected clock beyond output expiry.
  for (std::size_t attempt = 0U; attempt < 10'000U; ++attempt) {
    std::this_thread::yield();
  }
  clock.store(1'000U, std::memory_order_relaxed);
  do {
    status = runner.poll(result);
    std::this_thread::yield();
  } while (status == models::RunStatus::no_result);
  EXPECT_EQ(status, models::RunStatus::late_response_discarded);
  EXPECT_EQ(runner.late_discard_count(), 1U);
}
