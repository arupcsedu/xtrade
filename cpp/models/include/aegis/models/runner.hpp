#ifndef AEGIS_MODELS_RUNNER_HPP
#define AEGIS_MODELS_RUNNER_HPP

#include "aegis/event_bus/spsc_ring.hpp"
#include "aegis/models/model.hpp"
#include "aegis/models/validator.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

namespace aegis::models {

class LocalModelRunner final {
public:
  LocalModelRunner(IForecastModel& model, MonotonicClockSource clock,
                   FallbackPolicy fallback_policy = FallbackPolicy::fail_closed,
                   IForecastModel* fallback_model = nullptr) noexcept;

  [[nodiscard]] RunResult run(const ModelInput& input) noexcept;
  [[nodiscard]] bool apply_control(ModelControlUpdate update) noexcept;
  [[nodiscard]] ModelHealthSnapshot health() const noexcept;

private:
  [[nodiscard]] RunResult run_one(IForecastModel& model, const ModelInput& input,
                                  bool fallback) noexcept;

  IForecastModel& model_;
  MonotonicClockSource clock_;
  FallbackPolicy fallback_policy_;
  IForecastModel* fallback_model_{};
  std::atomic<std::uint64_t> control_{0U};
};

template <std::size_t Capacity> class AsynchronousModelRunner final {
public:
  static_assert(Capacity >= 2U && (Capacity & (Capacity - 1U)) == 0U);

  AsynchronousModelRunner(
      IForecastModel& model, MonotonicClockSource clock,
      const FallbackPolicy fallback_policy = FallbackPolicy::fail_closed,
      IForecastModel* fallback_model = nullptr)
      : runner_(model, clock, fallback_policy, fallback_model), clock_(clock),
        worker_([this] { worker_loop(); }) {}

  ~AsynchronousModelRunner() { shutdown(); }

  AsynchronousModelRunner(const AsynchronousModelRunner&) = delete;
  AsynchronousModelRunner& operator=(const AsynchronousModelRunner&) = delete;

  [[nodiscard]] RunStatus submit(const ModelInput& input) noexcept {
    if (stopping_.load(std::memory_order_acquire)) {
      return RunStatus::stopped;
    }
    const auto now = clock_.now_ns();
    if (now == 0U || now > input.deadline.complete_by_process_monotonic_time_ns) {
      late_discard_count_.fetch_add(1U, std::memory_order_relaxed);
      return RunStatus::deadline_missed;
    }
    const auto status = requests_.enqueue(input);
    if (status != event_bus::EnqueueStatus::accepted) {
      queue_full_count_.fetch_add(1U, std::memory_order_relaxed);
      return RunStatus::queue_full;
    }
    return RunStatus::accepted;
  }

  [[nodiscard]] RunStatus poll(RunResult& output) noexcept {
    RunResult candidate{};
    if (completions_.dequeue(candidate) != event_bus::DequeueStatus::item) {
      return stopping_.load(std::memory_order_acquire) ? RunStatus::stopped
                                                       : RunStatus::no_result;
    }
    const auto now = clock_.now_ns();
    if ((candidate.status == RunStatus::accepted ||
         candidate.status == RunStatus::fallback_accepted) &&
        (now == 0U || now > candidate.forecast.expiration_process_monotonic_time_ns)) {
      late_discard_count_.fetch_add(1U, std::memory_order_relaxed);
      return RunStatus::late_response_discarded;
    }
    output = candidate;
    return candidate.status;
  }

  [[nodiscard]] bool apply_control(const ModelControlUpdate update) noexcept {
    return runner_.apply_control(update);
  }

  void shutdown() noexcept {
    if (!stopping_.exchange(true, std::memory_order_acq_rel) && worker_.joinable()) {
      worker_.join();
    }
  }

  [[nodiscard]] std::uint64_t late_discard_count() const noexcept {
    return late_discard_count_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] std::uint64_t queue_full_count() const noexcept {
    return queue_full_count_.load(std::memory_order_relaxed);
  }

private:
  void worker_loop() noexcept {
    while (!stopping_.load(std::memory_order_acquire)) {
      ModelInput input{};
      if (requests_.dequeue(input) != event_bus::DequeueStatus::item) {
        std::this_thread::yield();
        continue;
      }
      const auto result = runner_.run(input);
      if (completions_.enqueue(result) != event_bus::EnqueueStatus::accepted) {
        queue_full_count_.fetch_add(1U, std::memory_order_relaxed);
      }
    }
  }

  LocalModelRunner runner_;
  MonotonicClockSource clock_;
  event_bus::SpscRing<ModelInput, Capacity> requests_;
  event_bus::SpscRing<RunResult, Capacity> completions_;
  std::atomic<bool> stopping_{false};
  std::atomic<std::uint64_t> late_discard_count_{0U};
  std::atomic<std::uint64_t> queue_full_count_{0U};
  std::thread worker_;
};

} // namespace aegis::models

#endif // AEGIS_MODELS_RUNNER_HPP
