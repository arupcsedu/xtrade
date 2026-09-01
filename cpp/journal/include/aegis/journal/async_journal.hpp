#ifndef AEGIS_JOURNAL_ASYNC_JOURNAL_HPP
#define AEGIS_JOURNAL_ASYNC_JOURNAL_HPP

#include "aegis/journal/journal.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace aegis::journal {

struct PublishResult {
  Status status{Status::stopped};
  std::uint64_t producer_sequence{};
};

struct AsyncMetrics {
  std::uint64_t accepted_records{};
  std::uint64_t persisted_records{};
  std::uint64_t full_rejections{};
  std::uint64_t contention_rejections{};
  std::uint64_t invalid_rejections{};
  std::uint64_t maximum_queue_lag{};
  std::uint64_t current_queue_lag{};
  std::uint64_t last_persisted_sequence{};
  HealthState health{HealthState::stopped};
  WriterMetrics writer;
};

class AsyncJournal final {
public:
  AsyncJournal();
  ~AsyncJournal();
  AsyncJournal(AsyncJournal&&) noexcept;
  AsyncJournal& operator=(AsyncJournal&&) noexcept;
  AsyncJournal(const AsyncJournal&) = delete;
  AsyncJournal& operator=(const AsyncJournal&) = delete;

  // Initialization and worker lifecycle are control-thread operations.
  [[nodiscard]] Status
  open(const WriterConfig& config,
       BackpressurePolicy backpressure_policy = BackpressurePolicy::fail_closed);
  [[nodiscard]] Status start();

  // The publication path is bounded, allocation-free, nonblocking, performs no
  // clock or disk calls, and copies at most kMaximumHotPayloadBytes.
  [[nodiscard]] PublishResult try_publish(const RecordView& record) noexcept;

  // Deterministic tests and single-threaded service loops may drain explicitly.
  [[nodiscard]] Status drain_once(std::size_t maximum_records,
                                  std::size_t& drained_records);

  [[nodiscard]] Status shutdown(std::chrono::nanoseconds maximum_drain_time);
  [[nodiscard]] AsyncMetrics metrics() const noexcept;
  [[nodiscard]] HealthState health() const noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace aegis::journal

#endif // AEGIS_JOURNAL_ASYNC_JOURNAL_HPP
