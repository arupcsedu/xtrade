#include "aegis/market_data/feed/feed_handler.hpp"
#include "aegis/market_data/feed/synthetic_receiver.hpp"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace {

namespace feed = aegis::market_data::feed;
namespace synthetic = aegis::market_data::synthetic;

constexpr aegis::common::SessionId kSession{0x534D'5842'454E'4348U, 1U};
constexpr feed::FeedIdentity kIdentity{
    .session_id = kSession,
    .venue_id = aegis::common::VenueId{1U, 1U},
    .channel_id = aegis::common::ChannelId{1U, 7U},
    .venue_number = 1U,
    .channel_number = 7U,
};

[[nodiscard]] synthetic::SyntheticEvent event_for(const std::uint64_t sequence) {
  synthetic::SyntheticEvent event{
      .type = synthetic::NativeMessageType::price_level,
      .side = synthetic::Side::bid,
      .action = synthetic::BookAction::change,
      .status = synthetic::TradingStatus::none,
      .data_quality = synthetic::DataQuality::valid,
      .message_flags = synthetic::kMessageFlagPriceLevel,
      .packet_flags = 0U,
      .venue_number = kIdentity.venue_number,
      .channel_number = kIdentity.channel_number,
      .instrument_number = 1U,
      .channel_sequence = sequence,
      .global_ordinal = sequence,
      .exchange_event_time_ns =
          1'800'000'000'000'000'000LL + static_cast<std::int64_t>(sequence),
      .nic_receive_time_ns =
          1'800'000'000'000'000'100LL + static_cast<std::int64_t>(sequence),
      .process_monotonic_time_ns = 1'000U + sequence,
      .synthetic_order_id = 0U,
      .price_ticks = 10'000,
      .quantity_units = 0U,
      .level_quantity_units = 100U + (sequence % 10U),
      .order_count = 1U,
      .auxiliary_code = 0U,
      .auxiliary_value = 0U,
      .event_hash = 0U,
  };
  event.event_hash = synthetic::calculate_event_hash(event);
  return event;
}

class AcceptingJournal final : public feed::RawPacketJournal {
public:
  [[nodiscard]] feed::PublishStatus
  enqueue(const feed::RawPacket& packet) noexcept override {
    static_cast<void>(packet);
    return feed::PublishStatus::accepted;
  }
};

class AcceptingHealthPublisher final : public feed::FeedHealthPublisher {
public:
  [[nodiscard]] feed::PublishStatus
  publish(const feed::DataQualityState& state) noexcept override {
    static_cast<void>(state);
    return feed::PublishStatus::accepted;
  }
};

class CountingPublisher final : public feed::NormalizedEventPublisher {
public:
  [[nodiscard]] feed::PublishStatus
  publish(const feed::NormalizedEvent& event) noexcept override {
    last_hash_ = event.event.event_hash;
    ++count_;
    return feed::PublishStatus::accepted;
  }

  [[nodiscard]] feed::PublishStatus
  publish_snapshot(const feed::RecoverySnapshot& snapshot) noexcept override {
    static_cast<void>(snapshot);
    return feed::PublishStatus::accepted;
  }

  [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
  [[nodiscard]] std::uint64_t last_hash() const noexcept { return last_hash_; }

private:
  std::uint64_t count_{};
  std::uint64_t last_hash_{};
};

class BenchmarkFixture final {
public:
  explicit BenchmarkFixture(const std::size_t receiver_capacity)
      : receiver_(kIdentity, receiver_capacity, 1U), decoder_(kIdentity),
        handler_(
            feed::FeedHandlerConfig{
                .identity = kIdentity,
                .initial_sequence = 1U,
                .maximum_sequence = UINT64_MAX,
                .stale_after_ns = UINT64_MAX,
                .recovery_timeout_ns = 1'000U,
                .arbitration_window = 32U,
            },
            receiver_, decoder_, receiver_, journal_, publisher_, health_) {}

  [[nodiscard]] feed::SyntheticReceiver& receiver() noexcept { return receiver_; }
  [[nodiscard]] CountingPublisher& publisher() noexcept { return publisher_; }
  [[nodiscard]] feed::FeedHandler& handler() noexcept { return handler_; }

private:
  feed::SyntheticReceiver receiver_;
  feed::SyntheticDecoder decoder_;
  AcceptingJournal journal_;
  CountingPublisher publisher_;
  AcceptingHealthPublisher health_;
  feed::FeedHandler handler_;
};

[[nodiscard]] double percentile(const std::array<std::uint64_t, 4'096U>& samples,
                                const std::size_t count,
                                const std::size_t thousandths) {
  if (count == 0U) {
    return 0.0;
  }
  const auto index = ((count - 1U) * thousandths) / 1'000U;
  return static_cast<double>(samples[index]);
}

void benchmark_receive_to_normalized(benchmark::State& state) {
  std::array<std::uint64_t, 4'096U> samples{};
  std::size_t sample_count = 0U;
  std::uint64_t sequence = 1U;
  auto fixture = std::make_unique<BenchmarkFixture>(256U);
  if (!fixture->handler().start(1U)) {
    state.SkipWithError("feed handler setup failed");
    return;
  }
  for (auto iteration : state) {
    static_cast<void>(iteration);
    const auto event = event_for(sequence);
    const auto packet = feed::make_synthetic_raw_packet(
        event, kSession, feed::FeedLeg::a, sequence + 1U);
    if (!fixture->receiver().enqueue(packet, false)) {
      state.SkipWithError("receiver unexpectedly overloaded");
      break;
    }
    const auto before = std::chrono::steady_clock::now();
    const auto result = fixture->handler().poll_once(sequence + 1U);
    const auto after = std::chrono::steady_clock::now();
    if (result != feed::PollResult::processed) {
      state.SkipWithError("packet was not normalized");
      break;
    }
    samples[sample_count % samples.size()] = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count());
    ++sample_count;
    ++sequence;
  }
  benchmark::DoNotOptimize(fixture->publisher().last_hash());
  state.SetItemsProcessed(static_cast<std::int64_t>(fixture->publisher().count()));

  sample_count = std::min(sample_count, samples.size());
  std::sort(samples.begin(),
            samples.begin() + static_cast<std::ptrdiff_t>(sample_count));
  state.counters["p50_ns"] = percentile(samples, sample_count, 500U);
  state.counters["p95_ns"] = percentile(samples, sample_count, 950U);
  state.counters["p99_ns"] = percentile(samples, sample_count, 990U);
  state.counters["p99_9_ns"] = percentile(samples, sample_count, 999U);
}

void benchmark_receiver_overload_fail_closed(benchmark::State& state) {
  std::uint64_t dropped = 0U;
  std::uint64_t normalized = 0U;
  for (auto iteration : state) {
    static_cast<void>(iteration);
    state.PauseTiming();
    auto fixture = std::make_unique<BenchmarkFixture>(4U);
    if (!fixture->handler().start(1U)) {
      state.SkipWithError("feed handler setup failed");
      return;
    }
    state.ResumeTiming();
    for (std::uint64_t sequence = 1U; sequence <= 5U; ++sequence) {
      const auto event = event_for(sequence);
      const auto accepted = fixture->receiver().enqueue(feed::make_synthetic_raw_packet(
          event, kSession, feed::FeedLeg::a, sequence + 1U));
      dropped += accepted ? 0U : 1U;
    }
    auto result = fixture->handler().poll_once(6U);
    benchmark::DoNotOptimize(result);
    normalized += fixture->publisher().count();
  }
  state.counters["dropped_packets"] = static_cast<double>(dropped);
  state.counters["normalized_after_overload"] = static_cast<double>(normalized);
  state.SetItemsProcessed(state.iterations() * 5);
}

BENCHMARK(benchmark_receive_to_normalized)->MinTime(0.02);
BENCHMARK(benchmark_receiver_overload_fail_closed)->MinTime(0.02);

} // namespace
