#ifndef AEGIS_EVENT_BUS_FORECAST_CACHE_HPP
#define AEGIS_EVENT_BUS_FORECAST_CACHE_HPP

#include "aegis/common/identifiers.hpp"
#include "aegis/event_bus/cache_line.hpp"
#include "aegis/event_bus/process_epoch.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <type_traits>

namespace aegis::event_bus {

inline constexpr std::uint64_t kForecastCacheMagic = 0x4145'4749'5346'4331ULL;
inline constexpr std::uint64_t kForecastCacheReady = 0x5245'4144'5946'4331ULL;
inline constexpr std::uint64_t kForecastCacheAbiMajor = 1U;
inline constexpr std::uint64_t kForecastCacheAbiMinor = 0U;
inline constexpr std::size_t kForecastCacheReadRetries = 8U;

enum class ForecastCacheStatus : std::uint8_t {
  ok = 1,
  invalid_argument = 2,
  misaligned_memory = 3,
  insufficient_memory = 4,
  unformatted = 5,
  incompatible_abi = 6,
  incompatible_schema = 7,
  stale_segment = 8,
  corrupt_header = 9,
  no_writer_authority = 10,
  split_brain = 11,
  cache_full = 12,
  busy = 13,
  miss = 14,
  stale_forecast = 15,
  corrupt_record = 16,
  epoch_conflict = 17,
};

// Fixed-layout, integer-only forecast payload. Numeric enum codes are retained
// as uint64_t values so unknown values fail validation without ABI ambiguity.
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct ForecastCacheRecord {
  common::InstrumentId instrument_id;
  common::ModelId model_id;
  common::ModelVersion model_version;
  common::ForecastId forecast_id;
  common::FeatureSnapshotId feature_snapshot_id;
  common::ConfigurationVersion configuration_version;
  common::SessionId session_id;
  std::int64_t forecast_value{};
  std::uint64_t confidence_ppm{};
  std::uint64_t horizon_ns{};
  std::uint64_t created_process_monotonic_time_ns{};
  std::uint64_t valid_until_process_monotonic_time_ns{};
  std::uint64_t forecast_unit_code{};
  std::uint64_t schema_version{};
  std::uint64_t writer_epoch{};
  std::uint64_t cache_generation{};
  std::uint64_t payload_hash{};
};

struct ForecastCacheFormatConfig {
  std::uint64_t forecast_schema_version{};
  std::uint64_t region_epoch{};
};

struct ForecastCacheAttachConfig {
  std::uint64_t abi_major{kForecastCacheAbiMajor};
  std::uint64_t minimum_abi_minor{kForecastCacheAbiMinor};
  std::uint64_t forecast_schema_version{};
  std::uint64_t expected_region_epoch{};
  std::uint64_t now_monotonic_time_ns{};
  std::uint64_t heartbeat_timeout_ns{};
  bool require_live_writer{false};
};

// Exact lookup identity for multi-model, multi-horizon publications. The
// legacy instrument-only read remains available for backward compatibility.
struct ForecastCacheKey {
  common::InstrumentId instrument_id;
  common::ModelId model_id;
  std::uint64_t horizon_ns{};
};

struct ForecastReadResult {
  ForecastCacheStatus status{ForecastCacheStatus::miss};
  ForecastCacheRecord record;
  std::uint64_t publication_age_ns{};
};

struct ForecastCacheMetrics {
  std::uint64_t publish_count{};
  std::uint64_t read_count{};
  std::uint64_t miss_count{};
  std::uint64_t stale_count{};
  std::uint64_t corrupt_count{};
  std::uint64_t epoch_conflict_count{};
  std::uint64_t busy_count{};
  std::uint64_t recovered_abandoned_slots{};
  std::uint64_t current_generation{};
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

static_assert(sizeof(ForecastCacheRecord) % sizeof(std::uint64_t) == 0U);
static_assert(std::is_trivially_copyable_v<ForecastCacheRecord>);
static_assert(std::has_unique_object_representations_v<ForecastCacheRecord>);

inline constexpr std::size_t kForecastRecordWords =
    sizeof(ForecastCacheRecord) / sizeof(std::uint64_t);
using ForecastRecordWords = std::array<std::uint64_t, kForecastRecordWords>;

[[nodiscard]] inline std::uint64_t
forecast_record_hash(ForecastCacheRecord record) noexcept {
  record.payload_hash = 0U;
  const auto words = std::bit_cast<ForecastRecordWords>(record);
  std::uint64_t hash = 14'695'981'039'346'656'037ULL;
  constexpr std::uint64_t kPrime = 1'099'511'628'211ULL;
  for (const auto word : words) {
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
      hash ^= (word >> shift) & 0xFFU;
      hash *= kPrime;
    }
  }
  return hash == 0U ? 1U : hash;
}

[[nodiscard]] inline bool
valid_forecast_record(const ForecastCacheRecord& record,
                      const std::uint64_t expected_schema_version) noexcept {
  return record.instrument_id.valid() && record.model_id.valid() &&
         record.model_version.valid() && record.forecast_id.valid() &&
         record.feature_snapshot_id.valid() && record.configuration_version.valid() &&
         record.session_id.valid() && record.confidence_ppm <= 1'000'000U &&
         record.horizon_ns != 0U && record.created_process_monotonic_time_ns != 0U &&
         record.valid_until_process_monotonic_time_ns >=
             record.created_process_monotonic_time_ns &&
         record.forecast_unit_code != 0U &&
         record.schema_version == expected_schema_version;
}

// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct alignas(kCacheLineBytes) ForecastCacheHeader {
  std::uint64_t magic{};
  std::uint64_t abi_major{};
  std::uint64_t abi_minor{};
  std::uint64_t layout_size{};
  std::uint64_t capacity{};
  std::uint64_t forecast_schema_version{};
  std::uint64_t region_epoch{};
  std::uint64_t static_header_hash{};
  alignas(kCacheLineBytes) std::atomic<std::uint64_t> format_state{0U};
  ProcessEpochState writer;
  PaddedAtomicU64 generation;
  PaddedAtomicU64 publish_count;
  PaddedAtomicU64 read_count;
  PaddedAtomicU64 miss_count;
  PaddedAtomicU64 stale_count;
  PaddedAtomicU64 corrupt_count;
  PaddedAtomicU64 epoch_conflict_count;
  PaddedAtomicU64 busy_count;
  PaddedAtomicU64 recovered_abandoned_slots;
};

struct alignas(kCacheLineBytes) ForecastCacheSlot {
  std::atomic<std::uint64_t> sequence{0U};
  std::array<std::atomic<std::uint64_t>, kForecastRecordWords> words{};
};

template <std::size_t Capacity> struct alignas(kCacheLineBytes) ForecastCacheRegion {
  static_assert(Capacity >= 2U);
  ForecastCacheHeader header;
  std::array<ForecastCacheSlot, Capacity> slots{};
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

[[nodiscard]] inline std::uint64_t
forecast_header_hash(const ForecastCacheHeader& header) noexcept {
  const std::array words{header.magic,       header.abi_major,
                         header.abi_minor,   header.layout_size,
                         header.capacity,    header.forecast_schema_version,
                         header.region_epoch};
  std::uint64_t hash = 14'695'981'039'346'656'037ULL;
  for (const auto word : words) {
    hash ^= word;
    hash *= 1'099'511'628'211ULL;
  }
  return hash == 0U ? 1U : hash;
}

template <std::size_t Capacity> class ForecastCacheView final {
public:
  using Region = ForecastCacheRegion<Capacity>;

  [[nodiscard]] static constexpr std::size_t required_bytes() noexcept {
    return sizeof(Region);
  }

  [[nodiscard]] static ForecastCacheStatus
  format(void* memory, const std::size_t memory_bytes,
         const ForecastCacheFormatConfig& config) noexcept {
    if (memory == nullptr || config.forecast_schema_version == 0U ||
        config.region_epoch == 0U) {
      return ForecastCacheStatus::invalid_argument;
    }
    if (memory_bytes < sizeof(Region)) {
      return ForecastCacheStatus::insufficient_memory;
    }
    if (reinterpret_cast<std::uintptr_t>(memory) % alignof(Region) != 0U) {
      return ForecastCacheStatus::misaligned_memory;
    }
    auto* region = std::construct_at(static_cast<Region*>(memory));
    region->header.magic = kForecastCacheMagic;
    region->header.abi_major = kForecastCacheAbiMajor;
    region->header.abi_minor = kForecastCacheAbiMinor;
    region->header.layout_size = sizeof(Region);
    region->header.capacity = Capacity;
    region->header.forecast_schema_version = config.forecast_schema_version;
    region->header.region_epoch = config.region_epoch;
    region->header.static_header_hash = forecast_header_hash(region->header);
    // Release is the final format step. Attachers acquire this marker before
    // reading any immutable ABI field.
    region->header.format_state.store(kForecastCacheReady, std::memory_order_release);
    return ForecastCacheStatus::ok;
  }

  [[nodiscard]] static ForecastCacheStatus
  attach(void* memory, const std::size_t memory_bytes,
         const ForecastCacheAttachConfig& config, ForecastCacheView& output) noexcept {
    if (memory == nullptr || config.abi_major == 0U ||
        config.forecast_schema_version == 0U || config.expected_region_epoch == 0U ||
        config.heartbeat_timeout_ns == 0U) {
      return ForecastCacheStatus::invalid_argument;
    }
    if (memory_bytes < sizeof(Region)) {
      return ForecastCacheStatus::insufficient_memory;
    }
    if (reinterpret_cast<std::uintptr_t>(memory) % alignof(Region) != 0U) {
      return ForecastCacheStatus::misaligned_memory;
    }
    auto* region = static_cast<Region*>(memory);
    if (region->header.format_state.load(std::memory_order_acquire) !=
        kForecastCacheReady) {
      return ForecastCacheStatus::unformatted;
    }
    if (region->header.magic != kForecastCacheMagic ||
        region->header.layout_size != sizeof(Region) ||
        region->header.capacity != Capacity ||
        region->header.static_header_hash != forecast_header_hash(region->header)) {
      return ForecastCacheStatus::corrupt_header;
    }
    if (region->header.abi_major != config.abi_major ||
        region->header.abi_minor < config.minimum_abi_minor) {
      return ForecastCacheStatus::incompatible_abi;
    }
    if (region->header.forecast_schema_version != config.forecast_schema_version) {
      return ForecastCacheStatus::incompatible_schema;
    }
    if (region->header.region_epoch != config.expected_region_epoch) {
      return ForecastCacheStatus::stale_segment;
    }
    if (config.require_live_writer) {
      const auto writer = ProcessEpochMechanism::inspect(region->header.writer,
                                                         config.now_monotonic_time_ns,
                                                         config.heartbeat_timeout_ns);
      if (writer.health == EpochHealth::corrupt) {
        return ForecastCacheStatus::corrupt_header;
      }
      if (writer.health != EpochHealth::live) {
        return ForecastCacheStatus::stale_segment;
      }
    }
    output.region_ = region;
    output.heartbeat_timeout_ns_ = config.heartbeat_timeout_ns;
    return ForecastCacheStatus::ok;
  }

  [[nodiscard]] EpochClaimStatus
  claim_writer(const std::uint64_t process_id, const std::uint64_t epoch,
               const std::uint64_t now_monotonic_time_ns) noexcept {
    if (region_ == nullptr) {
      return EpochClaimStatus::invalid;
    }
    const auto result =
        ProcessEpochMechanism::claim(region_->header.writer, process_id, epoch,
                                     now_monotonic_time_ns, heartbeat_timeout_ns_);
    if (result == EpochClaimStatus::stale_takeover) {
      recover_abandoned_slots();
    }
    return result;
  }

  [[nodiscard]] EpochHeartbeatStatus
  heartbeat(const std::uint64_t process_id, const std::uint64_t epoch,
            const std::uint64_t now_monotonic_time_ns) noexcept {
    return region_ == nullptr
               ? EpochHeartbeatStatus::invalid
               : ProcessEpochMechanism::heartbeat(region_->header.writer, process_id,
                                                  epoch, now_monotonic_time_ns);
  }

  [[nodiscard]] ForecastCacheStatus
  publish(ForecastCacheRecord record, const std::uint64_t process_id,
          const std::uint64_t epoch,
          const std::uint64_t now_monotonic_time_ns) noexcept {
    if (region_ == nullptr ||
        !valid_forecast_record(record, region_->header.forecast_schema_version)) {
      return ForecastCacheStatus::invalid_argument;
    }
    if (!authorized(process_id, epoch, now_monotonic_time_ns)) {
      return ForecastCacheStatus::no_writer_authority;
    }
    for (auto& slot : region_->slots) {
      ForecastCacheRecord existing{};
      const auto load_status = load_slot(slot, existing);
      if (load_status == ForecastCacheStatus::busy) {
        continue;
      }
      if (load_status == ForecastCacheStatus::corrupt_record) {
        region_->header.corrupt_count.value.fetch_add(1U, std::memory_order_relaxed);
        return load_status;
      }
      if (load_status == ForecastCacheStatus::ok &&
          (existing.instrument_id != record.instrument_id ||
           existing.model_id != record.model_id ||
           existing.horizon_ns != record.horizon_ns)) {
        continue;
      }
      auto sequence = slot.sequence.load(std::memory_order_acquire);
      if ((sequence & 1U) != 0U ||
          !slot.sequence.compare_exchange_strong(sequence, sequence + 1U,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
        continue;
      }
      if (!authorized(process_id, epoch, now_monotonic_time_ns)) {
        auto owned_sequence = sequence + 1U;
        static_cast<void>(slot.sequence.compare_exchange_strong(
            owned_sequence, sequence + 2U, std::memory_order_release,
            std::memory_order_relaxed));
        return ForecastCacheStatus::no_writer_authority;
      }
      record.writer_epoch = epoch;
      record.cache_generation =
          region_->header.generation.value.fetch_add(1U, std::memory_order_relaxed) +
          1U;
      record.payload_hash = forecast_record_hash(record);
      const auto words = std::bit_cast<ForecastRecordWords>(record);
      for (std::size_t index = 0U; index < words.size(); ++index) {
        slot.words[index].store(words[index], std::memory_order_relaxed);
      }
      // Release publishes every atomic payload word as one stable generation.
      // A CAS also fences a paused old writer whose odd generation was
      // recovered by a newer process epoch while this writer was descheduled.
      auto owned_sequence = sequence + 1U;
      if (!slot.sequence.compare_exchange_strong(owned_sequence, sequence + 2U,
                                                 std::memory_order_release,
                                                 std::memory_order_relaxed)) {
        region_->header.epoch_conflict_count.value.fetch_add(1U,
                                                             std::memory_order_relaxed);
        return ForecastCacheStatus::epoch_conflict;
      }
      region_->header.publish_count.value.fetch_add(1U, std::memory_order_relaxed);
      return ForecastCacheStatus::ok;
    }
    return ForecastCacheStatus::cache_full;
  }

  [[nodiscard]] ForecastReadResult
  read(const common::InstrumentId instrument_id,
       const std::uint64_t now_monotonic_time_ns) noexcept {
    return read_matching(instrument_id, {}, 0U, false, now_monotonic_time_ns);
  }

  [[nodiscard]] ForecastReadResult
  read(const ForecastCacheKey& key,
       const std::uint64_t now_monotonic_time_ns) noexcept {
    if (!key.model_id.valid() || key.horizon_ns == 0U) {
      return {.status = ForecastCacheStatus::invalid_argument,
              .record = {},
              .publication_age_ns = 0U};
    }
    return read_matching(key.instrument_id, key.model_id, key.horizon_ns, true,
                         now_monotonic_time_ns);
  }

  [[nodiscard]] ForecastCacheMetrics metrics() const noexcept {
    if (region_ == nullptr) {
      return {};
    }
    return {
        .publish_count =
            region_->header.publish_count.value.load(std::memory_order_relaxed),
        .read_count = region_->header.read_count.value.load(std::memory_order_relaxed),
        .miss_count = region_->header.miss_count.value.load(std::memory_order_relaxed),
        .stale_count =
            region_->header.stale_count.value.load(std::memory_order_relaxed),
        .corrupt_count =
            region_->header.corrupt_count.value.load(std::memory_order_relaxed),
        .epoch_conflict_count =
            region_->header.epoch_conflict_count.value.load(std::memory_order_relaxed),
        .busy_count = region_->header.busy_count.value.load(std::memory_order_relaxed),
        .recovered_abandoned_slots =
            region_->header.recovered_abandoned_slots.value.load(
                std::memory_order_relaxed),
        .current_generation =
            region_->header.generation.value.load(std::memory_order_relaxed)};
  }

  [[nodiscard]] Region* region() noexcept { return region_; }

private:
  [[nodiscard]] ForecastReadResult
  read_matching(const common::InstrumentId instrument_id,
                const common::ModelId model_id, const std::uint64_t horizon_ns,
                const bool exact_match,
                const std::uint64_t now_monotonic_time_ns) noexcept {
    if (region_ == nullptr || !instrument_id.valid() || now_monotonic_time_ns == 0U) {
      return {.status = ForecastCacheStatus::invalid_argument,
              .record = {},
              .publication_age_ns = 0U};
    }
    bool busy_seen = false;
    for (auto& slot : region_->slots) {
      ForecastCacheRecord record{};
      const auto status = load_slot(slot, record);
      if (status == ForecastCacheStatus::busy) {
        busy_seen = true;
        continue;
      }
      if (status == ForecastCacheStatus::miss) {
        continue;
      }
      if (status == ForecastCacheStatus::corrupt_record) {
        region_->header.corrupt_count.value.fetch_add(1U, std::memory_order_relaxed);
        return {.status = status, .record = {}, .publication_age_ns = 0U};
      }
      if (record.instrument_id != instrument_id ||
          (exact_match &&
           (record.model_id != model_id || record.horizon_ns != horizon_ns))) {
        continue;
      }
      const auto writer = ProcessEpochMechanism::inspect(
          region_->header.writer, now_monotonic_time_ns, heartbeat_timeout_ns_);
      if (writer.health == EpochHealth::corrupt) {
        region_->header.corrupt_count.value.fetch_add(1U, std::memory_order_relaxed);
        return {.status = ForecastCacheStatus::corrupt_header,
                .record = {},
                .publication_age_ns = 0U};
      }
      if (record.writer_epoch != writer.epoch) {
        region_->header.epoch_conflict_count.value.fetch_add(1U,
                                                             std::memory_order_relaxed);
        return {.status = ForecastCacheStatus::epoch_conflict,
                .record = {},
                .publication_age_ns = 0U};
      }
      if (writer.health != EpochHealth::live ||
          now_monotonic_time_ns > record.valid_until_process_monotonic_time_ns) {
        region_->header.stale_count.value.fetch_add(1U, std::memory_order_relaxed);
        return {.status = ForecastCacheStatus::stale_forecast,
                .record = {},
                .publication_age_ns = 0U};
      }
      if (now_monotonic_time_ns < record.created_process_monotonic_time_ns) {
        region_->header.corrupt_count.value.fetch_add(1U, std::memory_order_relaxed);
        return {.status = ForecastCacheStatus::corrupt_record,
                .record = {},
                .publication_age_ns = 0U};
      }
      region_->header.read_count.value.fetch_add(1U, std::memory_order_relaxed);
      return {.status = ForecastCacheStatus::ok,
              .record = record,
              .publication_age_ns =
                  now_monotonic_time_ns - record.created_process_monotonic_time_ns};
    }
    if (busy_seen) {
      region_->header.busy_count.value.fetch_add(1U, std::memory_order_relaxed);
      return {
          .status = ForecastCacheStatus::busy, .record = {}, .publication_age_ns = 0U};
    }
    region_->header.miss_count.value.fetch_add(1U, std::memory_order_relaxed);
    return {
        .status = ForecastCacheStatus::miss, .record = {}, .publication_age_ns = 0U};
  }

  // The three fields are the already-validated caller identity and its injected
  // monotonic observation time.
  // NOLINTBEGIN(bugprone-easily-swappable-parameters)
  [[nodiscard]] bool
  authorized(const std::uint64_t process_id, const std::uint64_t epoch,
             const std::uint64_t now_monotonic_time_ns) const noexcept {
    const auto writer = ProcessEpochMechanism::inspect(
        region_->header.writer, now_monotonic_time_ns, heartbeat_timeout_ns_);
    return writer.health == EpochHealth::live && writer.process_id == process_id &&
           writer.epoch == epoch;
  }
  // NOLINTEND(bugprone-easily-swappable-parameters)

  [[nodiscard]] ForecastCacheStatus
  load_slot(const ForecastCacheSlot& slot, ForecastCacheRecord& output) const noexcept {
    for (std::size_t retry = 0U; retry < kForecastCacheReadRetries; ++retry) {
      const auto first = slot.sequence.load(std::memory_order_acquire);
      if ((first & 1U) != 0U) {
        continue;
      }
      ForecastRecordWords words{};
      bool empty = true;
      for (std::size_t index = 0U; index < words.size(); ++index) {
        words[index] = slot.words[index].load(std::memory_order_relaxed);
        empty = empty && words[index] == 0U;
      }
      if (slot.sequence.load(std::memory_order_acquire) != first) {
        continue;
      }
      if (empty) {
        return ForecastCacheStatus::miss;
      }
      output = std::bit_cast<ForecastCacheRecord>(words);
      return valid_forecast_record(output, region_->header.forecast_schema_version) &&
                     output.payload_hash == forecast_record_hash(output)
                 ? ForecastCacheStatus::ok
                 : ForecastCacheStatus::corrupt_record;
    }
    return ForecastCacheStatus::busy;
  }

  void recover_abandoned_slots() noexcept {
    for (auto& slot : region_->slots) {
      auto sequence = slot.sequence.load(std::memory_order_acquire);
      if ((sequence & 1U) == 0U) {
        continue;
      }
      for (auto& word : slot.words) {
        word.store(0U, std::memory_order_relaxed);
      }
      if (slot.sequence.compare_exchange_strong(sequence, sequence + 1U,
                                                std::memory_order_release,
                                                std::memory_order_acquire)) {
        region_->header.recovered_abandoned_slots.value.fetch_add(
            1U, std::memory_order_relaxed);
      }
    }
  }

  Region* region_{};
  std::uint64_t heartbeat_timeout_ns_{};
};

} // namespace aegis::event_bus

#endif // AEGIS_EVENT_BUS_FORECAST_CACHE_HPP
