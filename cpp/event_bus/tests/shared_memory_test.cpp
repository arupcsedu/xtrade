#include "aegis/event_bus/forecast_cache.hpp"
#include "aegis/event_bus/process_epoch.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#ifdef __unix__
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace bus = aegis::event_bus;

namespace {

using Cache = bus::ForecastCacheView<4U>;
using Region = Cache::Region;

struct alignas(Region) RegionMemory {
  std::array<std::byte, sizeof(Region)> bytes{};
};

[[nodiscard]] bus::ForecastCacheStatus format_cache(void* memory,
                                                    const std::size_t bytes) {
  return Cache::format(memory, bytes,
                       {.forecast_schema_version = 1U, .region_epoch = 7U});
}

[[nodiscard]] bus::ForecastCacheAttachConfig
attach_config(const std::uint64_t schema = 1U, const std::uint64_t region_epoch = 7U,
              const std::uint64_t now = 100U, const bool require_live = false) {
  return {.forecast_schema_version = schema,
          .expected_region_epoch = region_epoch,
          .now_monotonic_time_ns = now,
          .heartbeat_timeout_ns = 100U,
          .require_live_writer = require_live};
}

[[nodiscard]] bus::ForecastCacheRecord record(const std::uint64_t instrument = 1U) {
  return {.instrument_id = {1U, instrument},
          .model_id = {2U, 1U},
          .model_version = {2U, 2U},
          .forecast_id = {3U, instrument},
          .feature_snapshot_id = {4U, instrument},
          .configuration_version = {5U, 1U},
          .session_id = {6U, 1U},
          .forecast_value = 42,
          .confidence_ppm = 800'000U,
          .horizon_ns = 1'000U,
          .created_process_monotonic_time_ns = 100U,
          .valid_until_process_monotonic_time_ns = 1'000U,
          .forecast_unit_code = 1U,
          .schema_version = 1U};
}

// GoogleTest assertion macros inflate the reported cognitive complexity.
// NOLINTBEGIN(readability-function-cognitive-complexity)
TEST(ProcessEpochTest, DetectsCrashRestartAndSplitBrain) {
  bus::ProcessEpochState state;
  EXPECT_EQ(bus::ProcessEpochMechanism::claim(state, 11U, 1U, 100U, 50U),
            bus::EpochClaimStatus::acquired);
  EXPECT_EQ(bus::ProcessEpochMechanism::heartbeat(state, 11U, 1U, 99U),
            bus::EpochHeartbeatStatus::time_regression);
  EXPECT_EQ(bus::ProcessEpochMechanism::claim(state, 12U, 2U, 120U, 50U),
            bus::EpochClaimStatus::split_brain);
  EXPECT_EQ(bus::ProcessEpochMechanism::claim(state, 12U, 2U, 200U, 50U),
            bus::EpochClaimStatus::stale_takeover);
  EXPECT_EQ(bus::ProcessEpochMechanism::heartbeat(state, 11U, 1U, 201U),
            bus::EpochHeartbeatStatus::lost_authority);
  EXPECT_EQ(bus::ProcessEpochMechanism::inspect(state, 210U, 50U).health,
            bus::EpochHealth::live);
  EXPECT_EQ(bus::ProcessEpochMechanism::mark_stopped(state, 12U, 2U),
            bus::EpochHeartbeatStatus::recorded);
  EXPECT_EQ(bus::ProcessEpochMechanism::inspect(state, 211U, 50U).health,
            bus::EpochHealth::stale);
}

TEST(ForecastCacheTest, ValidatesAbiSchemaAndStaleSegments) {
  RegionMemory memory;
  ASSERT_EQ(format_cache(memory.bytes.data(), memory.bytes.size()),
            bus::ForecastCacheStatus::ok);
  Cache cache;
  EXPECT_EQ(
      Cache::attach(memory.bytes.data(), memory.bytes.size(), attach_config(), cache),
      bus::ForecastCacheStatus::ok);
  Cache incompatible;
  EXPECT_EQ(Cache::attach(memory.bytes.data(), memory.bytes.size(), attach_config(2U),
                          incompatible),
            bus::ForecastCacheStatus::incompatible_schema);
  EXPECT_EQ(Cache::attach(memory.bytes.data(), memory.bytes.size(),
                          attach_config(1U, 8U), incompatible),
            bus::ForecastCacheStatus::stale_segment);
  auto bad_abi = attach_config();
  bad_abi.abi_major = 2U;
  EXPECT_EQ(
      Cache::attach(memory.bytes.data(), memory.bytes.size(), bad_abi, incompatible),
      bus::ForecastCacheStatus::incompatible_abi);
}

TEST(ForecastCacheTest, PublishesVersionedForecastAndRejectsLateReads) {
  RegionMemory memory;
  ASSERT_EQ(format_cache(memory.bytes.data(), memory.bytes.size()),
            bus::ForecastCacheStatus::ok);
  Cache cache;
  ASSERT_EQ(
      Cache::attach(memory.bytes.data(), memory.bytes.size(), attach_config(), cache),
      bus::ForecastCacheStatus::ok);
  ASSERT_EQ(cache.claim_writer(11U, 1U, 100U), bus::EpochClaimStatus::acquired);
  ASSERT_EQ(cache.publish(record(), 11U, 1U, 100U), bus::ForecastCacheStatus::ok);
  const auto current = cache.read({1U, 1U}, 150U);
  ASSERT_EQ(current.status, bus::ForecastCacheStatus::ok);
  EXPECT_EQ(current.record.forecast_value, 42);
  EXPECT_EQ(current.record.writer_epoch, 1U);
  EXPECT_EQ(current.record.cache_generation, 1U);
  EXPECT_EQ(current.publication_age_ns, 50U);
  EXPECT_EQ(cache.read({1U, 1U}, 1'001U).status,
            bus::ForecastCacheStatus::stale_forecast);
  EXPECT_EQ(cache.metrics().publish_count, 1U);
}

TEST(ForecastCacheTest, KeysMultipleModelsAndHorizonsWithoutRpc) {
  RegionMemory memory;
  ASSERT_EQ(format_cache(memory.bytes.data(), memory.bytes.size()),
            bus::ForecastCacheStatus::ok);
  Cache cache;
  ASSERT_EQ(
      Cache::attach(memory.bytes.data(), memory.bytes.size(), attach_config(), cache),
      bus::ForecastCacheStatus::ok);
  ASSERT_EQ(cache.claim_writer(11U, 1U, 100U), bus::EpochClaimStatus::acquired);

  auto short_horizon = record();
  auto long_horizon = record();
  long_horizon.forecast_id = {3U, 2U};
  long_horizon.horizon_ns = 5'000U;
  long_horizon.forecast_value = 84;
  auto alternate_model = record();
  alternate_model.model_id = {2U, 3U};
  alternate_model.forecast_id = {3U, 3U};
  alternate_model.forecast_value = -7;

  ASSERT_EQ(cache.publish(short_horizon, 11U, 1U, 100U), bus::ForecastCacheStatus::ok);
  ASSERT_EQ(cache.publish(long_horizon, 11U, 1U, 100U), bus::ForecastCacheStatus::ok);
  ASSERT_EQ(cache.publish(alternate_model, 11U, 1U, 100U),
            bus::ForecastCacheStatus::ok);

  const auto long_result = cache.read(
      {.instrument_id = {1U, 1U}, .model_id = {2U, 1U}, .horizon_ns = 5'000U}, 150U);
  ASSERT_EQ(long_result.status, bus::ForecastCacheStatus::ok);
  EXPECT_EQ(long_result.record.forecast_value, 84);
  const auto alternate_result = cache.read(
      {.instrument_id = {1U, 1U}, .model_id = {2U, 3U}, .horizon_ns = 1'000U}, 150U);
  ASSERT_EQ(alternate_result.status, bus::ForecastCacheStatus::ok);
  EXPECT_EQ(alternate_result.record.forecast_value, -7);
  EXPECT_EQ(
      cache
          .read({.instrument_id = {1U, 1U}, .model_id = {9U, 9U}, .horizon_ns = 1'000U},
                150U)
          .status,
      bus::ForecastCacheStatus::miss);
  EXPECT_EQ(
      cache
          .read({.instrument_id = {1U, 1U}, .model_id = {}, .horizon_ns = 1'000U}, 150U)
          .status,
      bus::ForecastCacheStatus::invalid_argument);
}

TEST(ForecastCacheTest, CrashTakeoverRecoversOddSlotAndRevokesOldEpoch) {
  RegionMemory memory;
  ASSERT_EQ(format_cache(memory.bytes.data(), memory.bytes.size()),
            bus::ForecastCacheStatus::ok);
  Cache cache;
  ASSERT_EQ(
      Cache::attach(memory.bytes.data(), memory.bytes.size(), attach_config(), cache),
      bus::ForecastCacheStatus::ok);
  ASSERT_EQ(cache.claim_writer(11U, 1U, 100U), bus::EpochClaimStatus::acquired);
  auto* const region = cache.region();
  if (region == nullptr) {
    FAIL() << "attached cache did not retain its region";
    return;
  }
  region->slots[0U].sequence.store(1U, std::memory_order_release);
  ASSERT_EQ(cache.claim_writer(12U, 2U, 250U), bus::EpochClaimStatus::stale_takeover);
  EXPECT_EQ(cache.metrics().recovered_abandoned_slots, 1U);
  EXPECT_EQ(cache.publish(record(), 11U, 1U, 251U),
            bus::ForecastCacheStatus::no_writer_authority);
  EXPECT_EQ(cache.publish(record(), 12U, 2U, 251U), bus::ForecastCacheStatus::ok);
}

TEST(ForecastCacheTest, DetectsRecordAndHeaderCorruption) {
  RegionMemory memory;
  ASSERT_EQ(format_cache(memory.bytes.data(), memory.bytes.size()),
            bus::ForecastCacheStatus::ok);
  Cache cache;
  ASSERT_EQ(
      Cache::attach(memory.bytes.data(), memory.bytes.size(), attach_config(), cache),
      bus::ForecastCacheStatus::ok);
  ASSERT_EQ(cache.claim_writer(11U, 1U, 100U), bus::EpochClaimStatus::acquired);
  ASSERT_EQ(cache.publish(record(), 11U, 1U, 100U), bus::ForecastCacheStatus::ok);
  auto* const region = cache.region();
  if (region == nullptr) {
    FAIL() << "attached cache did not retain its region";
    return;
  }
  region->slots[0U].words[7U].fetch_xor(1U, std::memory_order_relaxed);
  EXPECT_EQ(cache.read({1U, 1U}, 150U).status,
            bus::ForecastCacheStatus::corrupt_record);

  region->header.capacity = 99U;
  Cache corrupted;
  EXPECT_EQ(Cache::attach(memory.bytes.data(), memory.bytes.size(), attach_config(),
                          corrupted),
            bus::ForecastCacheStatus::corrupt_header);
}

TEST(ForecastCacheTest, LiveAttachRejectsStaleWriterHeartbeat) {
  RegionMemory memory;
  ASSERT_EQ(format_cache(memory.bytes.data(), memory.bytes.size()),
            bus::ForecastCacheStatus::ok);
  Cache cache;
  ASSERT_EQ(
      Cache::attach(memory.bytes.data(), memory.bytes.size(), attach_config(), cache),
      bus::ForecastCacheStatus::ok);
  ASSERT_EQ(cache.claim_writer(11U, 1U, 100U), bus::EpochClaimStatus::acquired);
  Cache stale;
  EXPECT_EQ(Cache::attach(memory.bytes.data(), memory.bytes.size(),
                          attach_config(1U, 7U, 250U, true), stale),
            bus::ForecastCacheStatus::stale_segment);
}

#ifdef __unix__
TEST(ForecastCacheTest, MappedRegionSurvivesWriterProcessCrashAndTakeover) {
  void* mapping = mmap(nullptr, Cache::required_bytes(), PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  ASSERT_NE(mapping, MAP_FAILED);
  ASSERT_EQ(format_cache(mapping, Cache::required_bytes()),
            bus::ForecastCacheStatus::ok);

  const auto child = fork();
  ASSERT_NE(child, -1);
  if (child == 0) {
    Cache child_cache;
    const bool successful =
        Cache::attach(mapping, Cache::required_bytes(), attach_config(), child_cache) ==
            bus::ForecastCacheStatus::ok &&
        child_cache.claim_writer(11U, 1U, 100U) == bus::EpochClaimStatus::acquired &&
        child_cache.publish(record(), 11U, 1U, 100U) == bus::ForecastCacheStatus::ok;
    _exit(successful ? 0 : 1);
  }

  int child_status{};
  ASSERT_EQ(waitpid(child, &child_status, 0), child);
  ASSERT_TRUE(WIFEXITED(child_status));
  ASSERT_EQ(WEXITSTATUS(child_status), 0);

  Cache parent_cache;
  ASSERT_EQ(
      Cache::attach(mapping, Cache::required_bytes(), attach_config(), parent_cache),
      bus::ForecastCacheStatus::ok);
  EXPECT_EQ(parent_cache.read({1U, 1U}, 150U).status, bus::ForecastCacheStatus::ok);
  EXPECT_EQ(parent_cache.claim_writer(12U, 2U, 250U),
            bus::EpochClaimStatus::stale_takeover);
  ASSERT_EQ(parent_cache.publish(record(), 12U, 2U, 251U),
            bus::ForecastCacheStatus::ok);
  EXPECT_EQ(parent_cache.read({1U, 1U}, 252U).record.writer_epoch, 2U);

  std::destroy_at(parent_cache.region());
  EXPECT_EQ(munmap(mapping, Cache::required_bytes()), 0);
}
#endif
// NOLINTEND(readability-function-cognitive-complexity)

} // namespace
