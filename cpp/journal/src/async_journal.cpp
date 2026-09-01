#include "aegis/journal/async_journal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace aegis::journal {
namespace {

inline constexpr std::size_t kCacheLineBytes = 64U;
inline constexpr std::size_t kMaximumProducerRetries = 64U;

struct alignas(kCacheLineBytes) PaddedAtomicU64 {
  std::atomic<std::uint64_t> value{0U};
  std::array<std::byte, kCacheLineBytes - sizeof(std::atomic<std::uint64_t>)> pad{};
};

struct QueueRecord {
  RecordMetadata metadata;
  std::uint32_t payload_bytes{};
  std::array<std::uint8_t, kMaximumHotPayloadBytes> payload{};
};

class JournalQueue final {
public:
  JournalQueue() noexcept {
    for (std::size_t index = 0U; index < cells_.size(); ++index) {
      cells_[index].sequence.store(index, std::memory_order_relaxed);
    }
  }

  [[nodiscard]] PublishResult enqueue(const RecordView& record,
                                      const bool invalidate_on_failure) noexcept {
    if (invalidated_.value.load(std::memory_order_acquire) != 0U) {
      return {.status = Status::inhibited};
    }
    auto position = enqueue_position_.value.load(std::memory_order_relaxed);
    for (std::size_t retry = 0U; retry < kMaximumProducerRetries; ++retry) {
      auto& cell = cells_[position & (kAsyncQueueCapacity - 1U)];
      const auto sequence = cell.sequence.load(std::memory_order_acquire);
      if (sequence == position) {
        if (enqueue_position_.value.compare_exchange_weak(position, position + 1U,
                                                          std::memory_order_relaxed,
                                                          std::memory_order_relaxed)) {
          cell.record.metadata = record.metadata;
          cell.record.payload_bytes = static_cast<std::uint32_t>(record.payload.size());
          std::memcpy(cell.record.payload.data(), record.payload.data(),
                      record.payload.size());
          cell.sequence.store(position + 1U, std::memory_order_release);
          accepted_.value.fetch_add(1U, std::memory_order_relaxed);
          const auto consumer = dequeue_position_.value.load(std::memory_order_acquire);
          const auto lag = position + 1U >= consumer ? position + 1U - consumer : 0U;
          update_maximum_lag(std::min<std::uint64_t>(lag, kAsyncQueueCapacity));
          return {.status = Status::ok, .producer_sequence = position + 1U};
        }
        continue;
      }
      if (sequence < position) {
        full_.value.fetch_add(1U, std::memory_order_relaxed);
        if (invalidate_on_failure) {
          invalidate();
          return {.status = Status::inhibited, .producer_sequence = position};
        }
        return {.status = Status::queue_full, .producer_sequence = position};
      }
      position = enqueue_position_.value.load(std::memory_order_relaxed);
    }
    contention_.value.fetch_add(1U, std::memory_order_relaxed);
    if (invalidate_on_failure) {
      invalidate();
      return {.status = Status::inhibited, .producer_sequence = position};
    }
    return {.status = Status::queue_contention, .producer_sequence = position};
  }

  [[nodiscard]] Status consume_one(SegmentWriter& writer,
                                   AppendResult& result) noexcept {
    const auto position = dequeue_position_.value.load(std::memory_order_relaxed);
    auto& cell = cells_[position & (kAsyncQueueCapacity - 1U)];
    const auto sequence = cell.sequence.load(std::memory_order_acquire);
    if (sequence != position + 1U) {
      return enqueue_position_.value.load(std::memory_order_acquire) == position
                 ? Status::record_not_found
                 : Status::queue_contention;
    }
    result =
        writer.append({.metadata = cell.record.metadata,
                       .payload = std::span<const std::uint8_t>(
                           cell.record.payload.data(), cell.record.payload_bytes)});
    cell.sequence.store(position + kAsyncQueueCapacity, std::memory_order_release);
    dequeue_position_.value.store(position + 1U, std::memory_order_release);
    consumed_.value.fetch_add(1U, std::memory_order_relaxed);
    return result.status;
  }

  void invalidate() noexcept {
    invalidated_.value.store(1U, std::memory_order_release);
  }

  [[nodiscard]] bool empty() const noexcept {
    return dequeue_position_.value.load(std::memory_order_acquire) ==
           enqueue_position_.value.load(std::memory_order_acquire);
  }

  [[nodiscard]] std::uint64_t lag() const noexcept {
    const auto producer = enqueue_position_.value.load(std::memory_order_acquire);
    const auto consumer = dequeue_position_.value.load(std::memory_order_acquire);
    return std::min<std::uint64_t>(producer - consumer, kAsyncQueueCapacity);
  }

  [[nodiscard]] std::uint64_t maximum_lag() const noexcept {
    return maximum_lag_.value.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t accepted() const noexcept {
    return accepted_.value.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t consumed() const noexcept {
    return consumed_.value.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t full() const noexcept {
    return full_.value.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t contention() const noexcept {
    return contention_.value.load(std::memory_order_relaxed);
  }

private:
  struct alignas(kCacheLineBytes) Cell {
    std::atomic<std::uint64_t> sequence{0U};
    QueueRecord record{};
  };

  void update_maximum_lag(const std::uint64_t lag) noexcept {
    auto observed = maximum_lag_.value.load(std::memory_order_relaxed);
    while (observed < lag &&
           !maximum_lag_.value.compare_exchange_weak(
               observed, lag, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
  }

  static_assert((kAsyncQueueCapacity & (kAsyncQueueCapacity - 1U)) == 0U);
  std::array<Cell, kAsyncQueueCapacity> cells_{};
  PaddedAtomicU64 enqueue_position_{};
  PaddedAtomicU64 dequeue_position_{};
  PaddedAtomicU64 accepted_{};
  PaddedAtomicU64 consumed_{};
  PaddedAtomicU64 full_{};
  PaddedAtomicU64 contention_{};
  PaddedAtomicU64 maximum_lag_{};
  PaddedAtomicU64 invalidated_{};
};

} // namespace

class AsyncJournal::Impl final {
public:
  Impl() : queue_(std::make_unique<JournalQueue>()) {}

  ~Impl() {
    if (worker_.joinable()) {
      request_stop_.store(true, std::memory_order_release);
      worker_.join();
    }
    (void)writer_.close();
  }

  [[nodiscard]] Status open(const WriterConfig& config,
                            const BackpressurePolicy policy) {
    if (health_.load(std::memory_order_acquire) != HealthState::stopped) {
      return Status::already_open;
    }
    health_.store(HealthState::starting, std::memory_order_release);
    const auto status = writer_.open(config);
    if (status != Status::ok) {
      health_.store(HealthState::unsafe, std::memory_order_release);
      return status;
    }
    policy_ = policy;
    update_writer_snapshot();
    return Status::ok;
  }

  [[nodiscard]] Status start() {
    if (health_.load(std::memory_order_acquire) != HealthState::starting ||
        worker_.joinable()) {
      return Status::invalid_argument;
    }
    request_stop_.store(false, std::memory_order_release);
    drain_deadline_ns_.store(std::numeric_limits<std::uint64_t>::max(),
                             std::memory_order_release);
    health_.store(HealthState::healthy, std::memory_order_release);
    worker_ = std::thread([this] { worker_loop(); });
    return Status::ok;
  }

  [[nodiscard]] PublishResult try_publish(const RecordView& record) noexcept {
    const auto current_health = health_.load(std::memory_order_acquire);
    if (current_health != HealthState::healthy &&
        current_health != HealthState::degraded &&
        current_health != HealthState::starting) {
      return {.status = current_health == HealthState::unsafe ? Status::inhibited
                                                              : Status::stopped};
    }
    if (!is_known_record_kind(record.metadata.kind) ||
        record.metadata.encoding == PayloadEncoding::unknown ||
        record.metadata.encoding > PayloadEncoding::opaque_binary ||
        record.metadata.priority < RecordPriority::advisory ||
        record.metadata.priority > RecordPriority::mandatory ||
        record.metadata.schema_version.major == 0U ||
        record.metadata.created_process_monotonic_time_ns == 0U ||
        record.metadata.recorded_wall_clock_utc_time_ns <= 0 ||
        !record.metadata.global_event_id.valid() || record.payload.empty()) {
      invalid_rejections_.fetch_add(1U, std::memory_order_relaxed);
      if (record.metadata.priority != RecordPriority::advisory) {
        inhibit();
        return {.status = Status::inhibited};
      }
      return {.status = Status::invalid_argument};
    }
    if (record.payload.size() > kMaximumHotPayloadBytes) {
      invalid_rejections_.fetch_add(1U, std::memory_order_relaxed);
      if (record.metadata.priority == RecordPriority::mandatory) {
        inhibit();
        return {.status = Status::inhibited};
      }
      return {.status = Status::payload_too_large};
    }
    const auto fail_closed = policy_ == BackpressurePolicy::fail_closed ||
                             record.metadata.priority == RecordPriority::mandatory;
    const auto result = queue_->enqueue(record, fail_closed);
    if (result.status == Status::inhibited) {
      inhibit();
    }
    return result;
  }

  [[nodiscard]] Status drain_once(const std::size_t maximum_records,
                                  std::size_t& drained_records) {
    drained_records = 0U;
    if (maximum_records == 0U || worker_.joinable()) {
      return Status::invalid_argument;
    }
    return drain(maximum_records, drained_records);
  }

  [[nodiscard]] Status shutdown(const std::chrono::nanoseconds maximum_drain_time) {
    const auto current = health_.load(std::memory_order_acquire);
    if (current == HealthState::stopped) {
      return Status::ok;
    }
    if (maximum_drain_time.count() < 0) {
      return Status::invalid_argument;
    }
    health_.store(HealthState::draining, std::memory_order_release);
    const auto now = steady_now_ns();
    const auto duration = static_cast<std::uint64_t>(maximum_drain_time.count());
    const auto deadline = now > std::numeric_limits<std::uint64_t>::max() - duration
                              ? std::numeric_limits<std::uint64_t>::max()
                              : now + duration;
    drain_deadline_ns_.store(deadline, std::memory_order_release);
    request_stop_.store(true, std::memory_order_release);
    if (worker_.joinable()) {
      worker_.join();
    } else {
      std::size_t drained{};
      while (!queue_->empty() && steady_now_ns() <= deadline) {
        const auto status = drain(kAsyncQueueCapacity, drained);
        if (status != Status::ok) {
          break;
        }
      }
      const auto close_status = writer_.close();
      if (close_status != Status::ok) {
        health_.store(HealthState::unsafe, std::memory_order_release);
        return close_status;
      }
      health_.store(queue_->empty() &&
                            !safety_inhibited_.load(std::memory_order_acquire)
                        ? HealthState::stopped
                        : HealthState::unsafe,
                    std::memory_order_release);
    }
    update_writer_snapshot();
    if (safety_inhibited_.load(std::memory_order_acquire)) {
      return Status::inhibited;
    }
    return health_.load(std::memory_order_acquire) == HealthState::stopped
               ? Status::ok
               : Status::deadline_exceeded;
  }

  [[nodiscard]] AsyncMetrics metrics() const noexcept {
    AsyncMetrics value{
        .accepted_records = queue_->accepted(),
        .persisted_records = persisted_records_.load(std::memory_order_relaxed),
        .full_rejections = queue_->full(),
        .contention_rejections = queue_->contention(),
        .invalid_rejections = invalid_rejections_.load(std::memory_order_relaxed),
        .maximum_queue_lag = queue_->maximum_lag(),
        .current_queue_lag = queue_->lag(),
        .last_persisted_sequence =
            last_persisted_sequence_.load(std::memory_order_acquire),
        .health = health_.load(std::memory_order_acquire),
        .writer = {},
    };
    {
      const std::scoped_lock lock(snapshot_mutex_);
      value.writer = writer_snapshot_;
    }
    return value;
  }

  [[nodiscard]] HealthState health() const noexcept {
    return health_.load(std::memory_order_acquire);
  }

private:
  [[nodiscard]] static std::uint64_t steady_now_ns() noexcept {
    const auto value = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(value).count());
  }

  void worker_loop() {
    for (;;) {
      std::size_t drained{};
      const auto status = drain(kAsyncQueueCapacity, drained);
      if (status != Status::ok && status != Status::record_not_found &&
          status != Status::queue_contention) {
        inhibit();
        break;
      }
      if (request_stop_.load(std::memory_order_acquire)) {
        if (queue_->empty()) {
          break;
        }
        if (steady_now_ns() > drain_deadline_ns_.load(std::memory_order_acquire)) {
          inhibit();
          break;
        }
      }
      if (drained == 0U) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
    }
    const auto status = writer_.close();
    update_writer_snapshot();
    if (status == Status::ok && queue_->empty() &&
        !safety_inhibited_.load(std::memory_order_acquire)) {
      health_.store(HealthState::stopped, std::memory_order_release);
    } else {
      health_.store(HealthState::unsafe, std::memory_order_release);
    }
  }

  [[nodiscard]] Status drain(const std::size_t maximum_records,
                             std::size_t& drained_records) {
    drained_records = 0U;
    for (; drained_records < maximum_records; ++drained_records) {
      AppendResult result{};
      const auto status = queue_->consume_one(writer_, result);
      if (status == Status::record_not_found || status == Status::queue_contention) {
        return Status::ok;
      }
      if (status != Status::ok) {
        update_writer_snapshot();
        return status;
      }
      last_persisted_sequence_.store(result.journal_sequence,
                                     std::memory_order_release);
      persisted_records_.fetch_add(1U, std::memory_order_relaxed);
    }
    update_writer_snapshot();
    return Status::ok;
  }

  void inhibit() noexcept {
    safety_inhibited_.store(true, std::memory_order_release);
    queue_->invalidate();
    health_.store(HealthState::unsafe, std::memory_order_release);
  }

  void update_writer_snapshot() noexcept {
    const auto snapshot = writer_.metrics();
    const std::scoped_lock lock(snapshot_mutex_);
    writer_snapshot_ = snapshot;
  }

  std::unique_ptr<JournalQueue> queue_;
  SegmentWriter writer_;
  std::thread worker_;
  BackpressurePolicy policy_{BackpressurePolicy::fail_closed};
  std::atomic<HealthState> health_{HealthState::stopped};
  std::atomic<bool> request_stop_{false};
  std::atomic<bool> safety_inhibited_{false};
  std::atomic<std::uint64_t> drain_deadline_ns_{0U};
  std::atomic<std::uint64_t> invalid_rejections_{0U};
  std::atomic<std::uint64_t> persisted_records_{0U};
  std::atomic<std::uint64_t> last_persisted_sequence_{0U};
  mutable std::mutex snapshot_mutex_;
  WriterMetrics writer_snapshot_{};
};

AsyncJournal::AsyncJournal() : impl_(std::make_unique<Impl>()) {}
AsyncJournal::~AsyncJournal() = default;
AsyncJournal::AsyncJournal(AsyncJournal&&) noexcept = default;
AsyncJournal& AsyncJournal::operator=(AsyncJournal&&) noexcept = default;

Status AsyncJournal::open(const WriterConfig& config,
                          const BackpressurePolicy backpressure_policy) {
  return impl_->open(config, backpressure_policy);
}
Status AsyncJournal::start() { return impl_->start(); }
PublishResult AsyncJournal::try_publish(const RecordView& record) noexcept {
  return impl_->try_publish(record);
}
Status AsyncJournal::drain_once(const std::size_t maximum_records,
                                std::size_t& drained_records) {
  return impl_->drain_once(maximum_records, drained_records);
}
Status AsyncJournal::shutdown(const std::chrono::nanoseconds maximum_drain_time) {
  return impl_->shutdown(maximum_drain_time);
}
AsyncMetrics AsyncJournal::metrics() const noexcept { return impl_->metrics(); }
HealthState AsyncJournal::health() const noexcept { return impl_->health(); }

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<HealthState>::is_always_lock_free);

} // namespace aegis::journal
