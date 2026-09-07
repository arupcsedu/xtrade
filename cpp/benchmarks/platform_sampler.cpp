#include "aegis/ensemble/gate.hpp"
#include "aegis/event_bus/forecast_cache.hpp"
#include "aegis/event_bus/snapshot_store.hpp"
#include "aegis/execution/simulated_gateway.hpp"
#include "aegis/features/feature_engine.hpp"
#include "aegis/journal/async_journal.hpp"
#include "aegis/market_data/feed/feed_handler.hpp"
#include "aegis/market_data/feed/synthetic_receiver.hpp"
#include "aegis/market_data/synthetic/artifacts.hpp"
#include "aegis/market_data/synthetic/config.hpp"
#include "aegis/models/microstructure.hpp"
#include "aegis/models/runner.hpp"
#include "aegis/oms/service.hpp"
#include "aegis/order_book/order_book.hpp"
#include "aegis/order_book/synthetic_adapter.hpp"
#include "aegis/replay/replay.hpp"
#include "aegis/risk/engine.hpp"

#include "../execution/tests/test_support.hpp"
#include "../oms/tests/test_support.hpp"
#include "../risk/tests/test_support.hpp"
#include "allocation_probe.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <cstdlib>
#include <linux/perf_event.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace {

namespace common = aegis::common;
namespace ensemble = aegis::ensemble;
namespace event_bus = aegis::event_bus;
namespace execution = aegis::execution;
namespace features = aegis::features;
namespace feed = aegis::market_data::feed;
namespace journal = aegis::journal;
namespace models = aegis::models;
namespace oms = aegis::oms;
namespace order_book = aegis::order_book;
namespace replay = aegis::replay;
namespace risk = aegis::risk;
namespace synthetic = aegis::market_data::synthetic;

inline constexpr std::uint64_t kDefaultSeed = 20'260'906U;
inline constexpr std::uint64_t kBaseNow = 1'000'000U;
inline constexpr std::int64_t kExchangeBase = 1'800'000'000'000'000'000LL;
inline constexpr common::SessionId kSession{1U, 1U};
inline constexpr common::VenueId kVenue{4U, 4U};
inline constexpr common::InstrumentId kInstrument{5U, 5U};
inline constexpr common::ConfigurationVersion kConfiguration{6U, 6U};

struct Options {
  std::filesystem::path metadata_path{"platform-sampler.json"};
  std::filesystem::path samples_path{"platform-samples.ndjson"};
  std::size_t samples{1'024U};
  std::size_t warmup{256U};
  std::uint64_t seed{kDefaultSeed};
};

struct Scenario {
  std::string_view name;
  std::uint32_t load_multiplier;
  std::uint32_t symbol_count;
  ensemble::EventState event_state;
  bool halted;
  bool degraded_feed;
};

inline constexpr std::array<Scenario, 9U> kScenarios{{
    {.name = "ordinary",
     .load_multiplier = 1U,
     .symbol_count = 1U,
     .event_state = ensemble::EventState::none,
     .halted = false,
     .degraded_feed = false},
    {.name = "peak-2x",
     .load_multiplier = 2U,
     .symbol_count = 1U,
     .event_state = ensemble::EventState::none,
     .halted = false,
     .degraded_feed = false},
    {.name = "peak-3x",
     .load_multiplier = 3U,
     .symbol_count = 1U,
     .event_state = ensemble::EventState::none,
     .halted = false,
     .degraded_feed = false},
    {.name = "burst",
     .load_multiplier = 8U,
     .symbol_count = 1U,
     .event_state = ensemble::EventState::volatility_shock,
     .halted = false,
     .degraded_feed = false},
    {.name = "multi-symbol",
     .load_multiplier = 4U,
     .symbol_count = 8U,
     .event_state = ensemble::EventState::none,
     .halted = false,
     .degraded_feed = false},
    {.name = "news-event",
     .load_multiplier = 2U,
     .symbol_count = 4U,
     .event_state = ensemble::EventState::breaking_news,
     .halted = false,
     .degraded_feed = false},
    {.name = "macro-event",
     .load_multiplier = 3U,
     .symbol_count = 8U,
     .event_state = ensemble::EventState::scheduled,
     .halted = false,
     .degraded_feed = false},
    {.name = "halt",
     .load_multiplier = 1U,
     .symbol_count = 1U,
     .event_state = ensemble::EventState::none,
     .halted = true,
     .degraded_feed = false},
    {.name = "degraded-feed",
     .load_multiplier = 1U,
     .symbol_count = 1U,
     .event_state = ensemble::EventState::none,
     .halted = false,
     .degraded_feed = true},
}};

inline constexpr std::array<std::string_view, 14U> kStages{{
    "packet-to-normalized",
    "normalized-to-book",
    "book-to-features",
    "features-to-micro-forecast",
    "forecasts-to-ensemble",
    "ensemble-to-risk",
    "risk-to-gateway-enqueue",
    "tick-to-intent",
    "tick-to-paper-send",
    "oms-response-handling",
    "journal-publication",
    "shared-memory-read",
    "forecast-cache-read",
    "replay-throughput",
}};

struct Observation {
  std::uint64_t latency_ns{};
  std::uint64_t allocations{};
  std::uint64_t operations{1U};
  bool ok{true};
};

struct CaseResult {
  std::string scenario;
  std::string stage;
  std::string status{"MEASURED"};
  std::vector<std::uint64_t> latency_samples;
  std::uint64_t elapsed_ns{};
  std::uint64_t cpu_time_ns{};
  std::uint64_t allocations{};
  std::optional<std::uint64_t> cache_misses;
  std::string cache_misses_unavailable_reason{
      "Linux hardware counter collection is unavailable in the reference sampler"};
  std::optional<std::uint64_t> queue_capacity;
  std::optional<std::uint64_t> queue_high_watermark;
  std::uint64_t packet_drops{};
  std::uint64_t operation_count{};
  std::vector<std::string> notes;
};

[[nodiscard]] std::uint64_t monotonic_raw_ns() noexcept {
  timespec value{};
  if (::clock_gettime(CLOCK_MONOTONIC_RAW, &value) != 0) {
    return 0U;
  }
  return (static_cast<std::uint64_t>(value.tv_sec) * 1'000'000'000U) +
         static_cast<std::uint64_t>(value.tv_nsec);
}

[[nodiscard]] std::uint64_t thread_cpu_ns() noexcept {
  timespec value{};
  if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0) {
    return 0U;
  }
  return (static_cast<std::uint64_t>(value.tv_sec) * 1'000'000'000U) +
         static_cast<std::uint64_t>(value.tv_nsec);
}

class CacheMissCounter final {
public:
  CacheMissCounter() {
    perf_event_attr attributes{};
    attributes.type = PERF_TYPE_HARDWARE;
    attributes.size = sizeof(attributes);
    attributes.config = PERF_COUNT_HW_CACHE_MISSES;
    attributes.disabled = 1U;
    attributes.exclude_kernel = 1U;
    attributes.exclude_hv = 1U;
    const auto opened = ::syscall(SYS_perf_event_open, &attributes, 0, -1, -1, 0);
    if (opened < 0) {
      unavailable_reason_ = std::string{"perf_event_open: "} +
                            std::error_code{errno, std::generic_category()}.message();
      return;
    }
    descriptor_ = static_cast<int>(opened);
  }
  ~CacheMissCounter() {
    if (descriptor_ >= 0) {
      static_cast<void>(::close(descriptor_));
    }
  }
  CacheMissCounter(const CacheMissCounter&) = delete;
  CacheMissCounter& operator=(const CacheMissCounter&) = delete;

  [[nodiscard]] bool start() const noexcept {
    return descriptor_ >= 0 && ::ioctl(descriptor_, PERF_EVENT_IOC_RESET, 0) == 0 &&
           ::ioctl(descriptor_, PERF_EVENT_IOC_ENABLE, 0) == 0;
  }

  [[nodiscard]] std::optional<std::uint64_t> finish() const noexcept {
    if (descriptor_ < 0 || ::ioctl(descriptor_, PERF_EVENT_IOC_DISABLE, 0) != 0) {
      return std::nullopt;
    }
    std::uint64_t value{};
    const auto bytes = ::read(descriptor_, &value, sizeof(value));
    if (bytes < 0 || static_cast<std::size_t>(bytes) != sizeof(value)) {
      return std::nullopt;
    }
    return value;
  }

  [[nodiscard]] const std::string& unavailable_reason() const noexcept {
    return unavailable_reason_;
  }

private:
  int descriptor_{-1};
  std::string unavailable_reason_{"hardware counter start/read failed"};
};

template <typename Function> [[nodiscard]] Observation timed(Function&& function) {
  const auto allocations_before = allocation_probe::count();
  const auto before = monotonic_raw_ns();
  const bool ok = std::invoke(std::forward<Function>(function));
  const auto after = monotonic_raw_ns();
  return {.latency_ns = after >= before ? after - before : 0U,
          .allocations = allocation_probe::count() - allocations_before,
          .operations = 1U,
          .ok = ok && before != 0U && after != 0U};
}

[[nodiscard]] std::string json_escape(const std::string_view value) {
  std::ostringstream output;
  for (const char raw_character : value) {
    const auto character = static_cast<unsigned char>(raw_character);
    switch (character) {
    case '"':
      output << "\\\"";
      break;
    case '\\':
      output << "\\\\";
      break;
    case '\n':
      output << "\\n";
      break;
    case '\r':
      output << "\\r";
      break;
    case '\t':
      output << "\\t";
      break;
    default:
      if (character < 0x20U) {
        constexpr std::string_view kHex{"0123456789abcdef"};
        output << "\\u00" << kHex[(character >> 4U) & 0xFU] << kHex[character & 0xFU];
      } else {
        output << static_cast<char>(character);
      }
    }
  }
  return output.str();
}

[[nodiscard]] std::string read_first_matching_line(const std::filesystem::path& path,
                                                   const std::string_view prefix) {
  std::ifstream input(path);
  std::string line;
  while (std::getline(input, line)) {
    if (line.starts_with(prefix)) {
      const auto separator = line.find(':');
      if (separator != std::string::npos) {
        const auto first = line.find_first_not_of(" \t", separator + 1U);
        return first == std::string::npos ? std::string{"unknown"} : line.substr(first);
      }
    }
  }
  return "unknown";
}

[[nodiscard]] std::string uname_field() {
  utsname value{};
  if (::uname(&value) != 0) {
    return "unknown";
  }
  return std::string{value.sysname} + " " + value.release + " " + value.machine;
}

[[nodiscard]] std::string hostname() {
  std::array<char, 256U> buffer{};
  if (::gethostname(buffer.data(), buffer.size() - 1U) != 0) {
    return "unknown";
  }
  return buffer.data();
}

[[nodiscard]] std::optional<int> numa_node_for_cpu(const int cpu) {
  const auto cpu_path =
      std::filesystem::path{"/sys/devices/system/cpu"} / ("cpu" + std::to_string(cpu));
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(cpu_path, error)) {
    const auto name = entry.path().filename().string();
    if (name.starts_with("node")) {
      try {
        return std::stoi(name.substr(4U));
      } catch (const std::exception&) {
        return std::nullopt;
      }
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<std::string> cpu_governor(const int cpu) {
  const auto path = std::filesystem::path{"/sys/devices/system/cpu"} /
                    ("cpu" + std::to_string(cpu)) / "cpufreq/scaling_governor";
  std::ifstream input(path);
  std::string value;
  if (!std::getline(input, value) || value.empty()) {
    return std::nullopt;
  }
  return value;
}

[[nodiscard]] std::optional<int> pin_to_first_allowed_cpu() noexcept {
  cpu_set_t inherited{};
  CPU_ZERO(&inherited);
  if (::sched_getaffinity(0, sizeof(inherited), &inherited) != 0) {
    return std::nullopt;
  }
  int selected = -1;
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
    if (CPU_ISSET(static_cast<std::size_t>(cpu), &inherited) != 0) {
      selected = cpu;
      break;
    }
  }
  if (selected < 0) {
    return std::nullopt;
  }
  cpu_set_t target{};
  CPU_ZERO(&target);
  CPU_SET(static_cast<std::size_t>(selected), &target);
  if (::sched_setaffinity(0, sizeof(target), &target) != 0) {
    return std::nullopt;
  }
  cpu_set_t verified{};
  CPU_ZERO(&verified);
  if (::sched_getaffinity(0, sizeof(verified), &verified) != 0 ||
      CPU_COUNT(&verified) != 1 ||
      CPU_ISSET(static_cast<std::size_t>(selected), &verified) == 0) {
    return std::nullopt;
  }
  return selected;
}

class AcceptingPacketJournal final : public feed::RawPacketJournal {
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

class CapturingPublisher final : public feed::NormalizedEventPublisher {
public:
  [[nodiscard]] feed::PublishStatus
  publish(const feed::NormalizedEvent& event) noexcept override {
    last_ = event;
    ++count_;
    return feed::PublishStatus::accepted;
  }
  [[nodiscard]] feed::PublishStatus
  publish_snapshot(const feed::RecoverySnapshot& snapshot) noexcept override {
    static_cast<void>(snapshot);
    return feed::PublishStatus::accepted;
  }
  [[nodiscard]] const feed::NormalizedEvent& last() const noexcept { return last_; }
  [[nodiscard]] std::uint64_t count() const noexcept { return count_; }

private:
  feed::NormalizedEvent last_{};
  std::uint64_t count_{};
};

[[nodiscard]] synthetic::SyntheticEvent market_event(const std::uint64_t sequence,
                                                     const bool degraded = false) {
  synthetic::SyntheticEvent event{
      .type = synthetic::NativeMessageType::price_level,
      .side = (sequence & 1U) == 0U ? synthetic::Side::ask : synthetic::Side::bid,
      .action = synthetic::BookAction::change,
      .status = synthetic::TradingStatus::none,
      .data_quality =
          degraded ? synthetic::DataQuality::stale : synthetic::DataQuality::valid,
      .message_flags = synthetic::kMessageFlagPriceLevel,
      .venue_number = 1U,
      .channel_number = 7U,
      .instrument_number = 1U,
      .channel_sequence = sequence,
      .global_ordinal = sequence,
      .exchange_event_time_ns = kExchangeBase + static_cast<std::int64_t>(sequence),
      .nic_receive_time_ns = kExchangeBase + 100 + static_cast<std::int64_t>(sequence),
      .process_monotonic_time_ns = kBaseNow + sequence,
      .price_ticks = (sequence & 1U) == 0U ? 10'002 : 10'000,
      .level_quantity_units = 100U + (sequence % 100U),
      .order_count = 1U};
  event.event_hash = synthetic::calculate_event_hash(event);
  return event;
}

class FeedFixture final {
public:
  FeedFixture()
      : identity_{.session_id = kSession,
                  .venue_id = kVenue,
                  .channel_id = common::ChannelId{7U, 7U},
                  .venue_number = 1U,
                  .channel_number = 7U},
        receiver_(identity_, feed::kMaximumReceiverQueue, 1U), decoder_(identity_),
        handler_({.identity = identity_,
                  .initial_sequence = 1U,
                  .maximum_sequence = std::numeric_limits<std::uint64_t>::max(),
                  .stale_after_ns = std::numeric_limits<std::uint64_t>::max(),
                  .recovery_timeout_ns = 1'000U,
                  .arbitration_window = 64U},
                 receiver_, decoder_, receiver_, packet_journal_, publisher_, health_) {
    started_ = handler_.start(1U);
  }

  [[nodiscard]] Observation run(const bool degraded) {
    const auto event = market_event(sequence_, degraded);
    const auto packet = feed::make_synthetic_raw_packet(
        event, kSession, feed::FeedLeg::a, sequence_ + 1U);
    if (!started_ || !receiver_.enqueue(packet, false)) {
      ++drops_;
      return {.ok = false};
    }
    const auto result = timed([&] {
      const auto status = handler_.poll_once(kBaseNow + sequence_ + 1U);
      return status == feed::PollResult::processed ||
             (degraded && status == feed::PollResult::invalid);
    });
    ++sequence_;
    high_watermark_ = std::max<std::uint64_t>(high_watermark_, 1U);
    return result;
  }

  [[nodiscard]] std::uint64_t drops() const noexcept { return drops_; }
  [[nodiscard]] std::uint64_t high_watermark() const noexcept {
    return high_watermark_;
  }
  [[nodiscard]] const synthetic::SyntheticEvent& last_event() const noexcept {
    return publisher_.last().event;
  }

private:
  feed::FeedIdentity identity_;
  feed::SyntheticReceiver receiver_;
  feed::SyntheticDecoder decoder_;
  AcceptingPacketJournal packet_journal_;
  CapturingPublisher publisher_;
  AcceptingHealthPublisher health_;
  feed::FeedHandler handler_;
  std::uint64_t sequence_{1U};
  std::uint64_t drops_{};
  std::uint64_t high_watermark_{};
  bool started_{false};
};

[[nodiscard]] order_book::BookConfig book_config() {
  return {.identity = {.venue_id = kVenue,
                       .instrument_id = kInstrument,
                       .venue_number = 1U,
                       .instrument_number = 1U},
          .session_id = kSession,
          .mode = order_book::BookMode::price_level,
          .order_capacity = 1U,
          .level_capacity = 128U,
          .published_depth = 16U,
          .maximum_sequence = std::numeric_limits<std::uint64_t>::max(),
          .permit_crossed_transition = false,
          .require_contiguous_sequence = true,
          .start_recovering = false};
}

class BookFixture final {
public:
  BookFixture() : book_(book_config()) {}
  [[nodiscard]] Observation run() {
    const auto event = market_event(sequence_++);
    return run(event);
  }
  [[nodiscard]] Observation run(const synthetic::SyntheticEvent& event) {
    return timed(
        [&] { return order_book::apply_synthetic(book_, kSession, event).accepted(); });
  }
  [[nodiscard]] const order_book::OrderBook& book() const noexcept { return book_; }

private:
  order_book::OrderBook book_;
  std::uint64_t sequence_{1U};
};

[[nodiscard]] features::FeatureEngineConfig feature_config() {
  features::FeatureEngineConfig result{
      .instrument_id = kInstrument,
      .session_id = kSession,
      .feature_definition_version = kConfiguration,
      .venue_count = 1U,
      .depth_levels = 5U,
      .window_capacity = 2'048U,
      .minimum_book_events = 1U,
      .minimum_trade_events = 0U,
      // Keep the synthetic event-time window shorter than the fixed capacity so
      // qualification runs exercise eviction instead of exhausting the buffer.
      .rolling_window_ns = 1'000U,
      .stale_after_ns = 500'000'000U,
      .maximum_quantity_units = 1'000'000U,
      .maximum_absolute_price_ticks = 1'000'000,
      .session_open_exchange_time_ns = kExchangeBase - 1'000'000'000LL,
      .session_close_exchange_time_ns = kExchangeBase + 10'000'000'000LL};
  result.venues[0U] = {.venue_id = kVenue, .venue_number = 1U};
  return result;
}

[[nodiscard]] features::BookFeatureEvent feature_event(const std::uint64_t ordinal,
                                                       const bool degraded = false) {
  order_book::DepthSnapshot depth{};
  for (std::size_t index = 0U; index < 5U; ++index) {
    depth.bids[index] = {.price_ticks = 10'000 - static_cast<std::int64_t>(index),
                         .quantity_units = 100U + (ordinal % 10U) + index,
                         .order_count = 1U};
    depth.asks[index] = {.price_ticks = 10'002 + static_cast<std::int64_t>(index),
                         .quantity_units = 110U + (ordinal % 10U) + index,
                         .order_count = 1U};
  }
  depth.bid_count = 5U;
  depth.ask_count = 5U;
  depth.version = ordinal;
  depth.validity = order_book::BookValidity::valid;
  return {.global_event_id = {50U, ordinal},
          .session_id = kSession,
          .instrument_id = kInstrument,
          .venue_id = kVenue,
          .venue_number = 1U,
          .global_ordinal = ordinal,
          .exchange_event_time_ns = kExchangeBase + static_cast<std::int64_t>(ordinal),
          .process_monotonic_time_ns = kBaseNow + ordinal,
          .operation = order_book::BookOperation::observe_quote,
          .depth = depth,
          .data_quality = degraded ? synthetic::DataQuality::degraded
                                   : synthetic::DataQuality::valid,
          .top_changed = true};
}

class FeatureFixture final {
public:
  FeatureFixture() : engine_(feature_config()) {}
  [[nodiscard]] Observation run(const bool degraded) {
    const auto event = feature_event(ordinal_++, degraded);
    return timed([&] { return engine_.on_book(event).accepted(); });
  }
  [[nodiscard]] Observation run(const order_book::OrderBook& book,
                                const synthetic::SyntheticEvent& source) {
    order_book::DepthSnapshot depth{};
    if (book.depth(depth, 5U) != order_book::ApplyError::none) {
      return {.ok = false};
    }
    const features::BookFeatureEvent event{
        .global_event_id = {50U, source.global_ordinal},
        .session_id = kSession,
        .instrument_id = kInstrument,
        .venue_id = kVenue,
        .venue_number = source.venue_number,
        .global_ordinal = source.global_ordinal,
        .exchange_event_time_ns = source.exchange_event_time_ns,
        .process_monotonic_time_ns = source.process_monotonic_time_ns,
        .operation = order_book::BookOperation::set_level,
        .side = source.side,
        .affected_price_ticks = source.price_ticks,
        .affected_quantity_units = source.level_quantity_units,
        .depth = depth,
        .data_quality = source.data_quality,
        .top_changed = true};
    ++ordinal_;
    return timed([&] { return engine_.on_book(event).accepted(); });
  }
  [[nodiscard]] features::FeatureSnapshot snapshot() const {
    features::FeatureSnapshot value{};
    static_cast<void>(engine_.make_snapshot(kBaseNow + ordinal_, value));
    return value;
  }

private:
  features::FeatureEngine engine_;
  std::uint64_t ordinal_{1U};
};

[[nodiscard]] models::ModelMetadata model_metadata() {
  models::ModelMetadata result{.model_id = {60U, 1U},
                               .model_version = {61U, 1U},
                               .instrument_id = kInstrument,
                               .configuration_version = kConfiguration,
                               .feature_definition_version = kConfiguration,
                               .kind = models::ModelKind::logistic_regression,
                               .requirement_count = 1U,
                               .horizon_ns = 1'000'000U,
                               .forecast_ttl_ns = 1'000'000U,
                               .calibration = {},
                               .ood = {}};
  result.requirements[0U] = {.name = features::FeatureName::spread_ticks,
                             .minimum_value = 0,
                             .maximum_value = 1'000'000,
                             .maximum_age_ns = 1'000'000U,
                             .allow_warming = true};
  return result;
}

[[nodiscard]] models::NativeModelArtifact native_artifact() {
  models::NativeModelArtifact result{
      .metadata = model_metadata(),
      .role = models::MicrostructureModelRole::queue_depletion,
      .family = models::NativeModelFamily::logistic_regression,
      .feature_count = 2U,
      .intercept_ppm = -100'000,
      .platt_slope_ppm = 1'000'000,
      .training_sample_count = 100'000U,
      .calibration_error_ppm = 20'000U,
      .expires_wall_clock_utc_ns = kExchangeBase + 100'000'000'000LL,
      .activation_expiration_process_monotonic_time_ns = kBaseNow + 100'000'000U};
  result.features[0U] = {.name = models::NativeFeatureName::spread_ticks,
                         .scale = 1'000U,
                         .training_minimum = -10'000,
                         .training_maximum = 10'000,
                         .weight_ppm = 200'000};
  result.features[1U] = {.name = models::NativeFeatureName::quantity_ahead_units,
                         .center = 500,
                         .scale = 500U,
                         .training_minimum = 0,
                         .training_maximum = 1'000,
                         .weight_ppm = -100'000};
  return result;
}

[[nodiscard]] features::FeatureSnapshot model_snapshot(const std::uint64_t ordinal) {
  features::FeatureSnapshot result{.snapshot_id = {70U, ordinal},
                                   .instrument_id = kInstrument,
                                   .session_id = kSession,
                                   .feature_definition_version = kConfiguration,
                                   .source_first_global_event_id = {71U, ordinal},
                                   .source_last_global_event_id = {71U, ordinal},
                                   .source_first_ordinal = ordinal,
                                   .source_last_ordinal = ordinal,
                                   .as_of_exchange_event_time_ns =
                                       kExchangeBase +
                                       static_cast<std::int64_t>(ordinal),
                                   .created_process_monotonic_time_ns = kBaseNow,
                                   .state = features::EngineState::ready};
  for (auto& value : result.values) {
    value = {.value = 0,
             .as_of_process_monotonic_time_ns = kBaseNow,
             .validity = features::FeatureValidity::valid};
  }
  result.values[static_cast<std::size_t>(features::FeatureName::spread_ticks)].value =
      2;
  result.stable_hash = features::stable_snapshot_hash(result);
  return result;
}

[[nodiscard]] models::ModelInput model_input(const std::uint64_t ordinal) {
  return {.forecast_id = {72U, ordinal},
          .feature_snapshot = model_snapshot(ordinal),
          .deadline = {.submitted_process_monotonic_time_ns = kBaseNow,
                       .complete_by_process_monotonic_time_ns = kBaseNow + 10'000U},
          .microstructure_context = {.quantity_ahead_units = 400U,
                                     .order_quantity_units = 100U,
                                     .order_age_ns = 50'000U,
                                     .price_distance_ticks = 1,
                                     .venue_number = 1U,
                                     .fee_rate_ppm = 30U,
                                     .present = true}};
}

[[nodiscard]] std::uint64_t fixture_clock(void* context) noexcept {
  return *static_cast<std::uint64_t*>(context);
}

class ModelFixture final {
public:
  ModelFixture()
      : model_(native_artifact()), runner_(model_, {fixture_clock, &clock_now_}) {}
  [[nodiscard]] Observation run() {
    const auto input = model_input(ordinal_++);
    return timed([&] {
      models::ModelPrediction prediction{};
      return model_.predict(input, prediction) == models::PredictionStatus::ok;
    });
  }
  [[nodiscard]] Observation run(const features::FeatureSnapshot& snapshot,
                                models::ModelForecast& output) {
    clock_now_ = std::max(kBaseNow, snapshot.created_process_monotonic_time_ns + 1U);
    auto input = model_input(ordinal_++);
    input.feature_snapshot = snapshot;
    input.forecast_id = {72U, ordinal_};
    input.deadline = {.submitted_process_monotonic_time_ns = clock_now_ - 1U,
                      .complete_by_process_monotonic_time_ns = clock_now_ + 10'000U};
    return timed([&] {
      const auto result = runner_.run(input);
      output = result.forecast;
      return result.status == models::RunStatus::accepted;
    });
  }

private:
  models::NativeMicrostructureModel model_;
  std::uint64_t clock_now_{kBaseNow};
  models::LocalModelRunner runner_;
  std::uint64_t ordinal_{1U};
};

[[nodiscard]] models::ModelForecast forecast(const std::uint64_t ordinal,
                                             const bool cost = false,
                                             const std::uint64_t now = kBaseNow) {
  models::ModelForecast result{
      .forecast_id = {80U, ordinal},
      .session_id = kSession,
      .model_id = {81U, ordinal},
      .model_version = {82U, ordinal},
      .instrument_id = kInstrument,
      .feature_snapshot_id = {83U, ordinal},
      .configuration_version = kConfiguration,
      .model_control_generation = 1U,
      .horizon_ns = 1'000'000U,
      .as_of_exchange_event_time_ns = kExchangeBase,
      .production_process_monotonic_time_ns = now - 100U,
      .expiration_process_monotonic_time_ns = now + 1'000'000U,
      .prediction = {
          .expected_return_ppm = cost ? 0 : 50'000,
          .return_p10_ppm = cost ? 0 : 46'000,
          .return_p50_ppm = cost ? 0 : 50'000,
          .return_p90_ppm = cost ? 0 : 54'000,
          .probability_down_ppm = cost ? 0U : 200'000U,
          .probability_flat_ppm = cost ? 1'000'000U : 150'000U,
          .probability_up_ppm = cost ? 0U : 650'000U,
          .volatility_ppm = 3'000U,
          .confidence_ppm = 900'000U,
          .calibration_score_ppm = 900'000U,
          .data_quality_score_ppm = 900'000U,
          .ood_score_ppm = 20'000U,
          .transaction_cost = {.spread_cost_ppm = cost ? 100U : 0U,
                               .slippage_cost_ppm = cost ? 100U : 0U,
                               .market_impact_ppm = cost ? 100U : 0U,
                               .adverse_selection_cost_ppm = cost ? 100U : 0U,
                               .fee_cost_ppm = cost ? 100U : 0U,
                               .present = cost}}};
  result.stable_hash = models::stable_forecast_hash(result);
  return result;
}

[[nodiscard]] ensemble::EnsembleConfig ensemble_config() {
  ensemble::EnsembleConfig result{.ensemble_model_id = {90U, 1U},
                                  .ensemble_model_version = {91U, 1U},
                                  .configuration_version = kConfiguration,
                                  .gate_kind = ensemble::GateKind::rule_based,
                                  .maximum_expert_weight_ppm = 800'000U,
                                  .ood_reject_threshold_ppm = 800'000U,
                                  .minimum_calibration_score_ppm = 500'000U,
                                  .minimum_data_quality_score_ppm = 500'000U,
                                  .maximum_forecast_age_ns = 10'000U,
                                  .maximum_market_state_age_ns = 10'000U,
                                  .degraded_feed_multiplier_ppm = 500'000U,
                                  .degraded_calibration_multiplier_ppm = 500'000U,
                                  .timeseries_breaking_news_multiplier_ppm = 100'000U,
                                  .uncertainty_penalty_multiplier_ppm = 100'000U,
                                  .calibration_uncertainty_multiplier_ppm = 100'000U,
                                  .ood_uncertainty_multiplier_ppm = 100'000U,
                                  .data_quality_uncertainty_multiplier_ppm = 100'000U,
                                  .safety_margin_ppm = 100U,
                                  .linear = {},
                                  .learned = {},
                                  .stable_hash = 0U};
  constexpr auto market_mask = static_cast<std::uint16_t>(
      (std::uint32_t{1U} << ensemble::kMarketStateCount) - 1U);
  constexpr auto event_mask =
      static_cast<std::uint8_t>((std::uint32_t{1U} << ensemble::kEventStateCount) - 1U);
  for (std::size_t index = 0U; index < result.role_policies.size(); ++index) {
    result.role_policies[index] = {.role =
                                       static_cast<ensemble::ExpertRole>(index + 1U),
                                   .allowed_market_state_mask = market_mask,
                                   .allowed_event_state_mask = event_mask,
                                   .base_multiplier_ppm = ensemble::kWeightScale};
  }
  result.stable_hash = ensemble::stable_ensemble_config_hash(result);
  return result;
}

[[nodiscard]] ensemble::EnsembleRequest
ensemble_request(const Scenario& scenario, const std::uint64_t ordinal,
                 const std::uint64_t now = kBaseNow) {
  auto market = risk::test::market_snapshot(now);
  if (scenario.halted) {
    market.state = aegis::market_state::MarketState::halted;
    market.primary_reason = aegis::market_state::MarketStateReason::official_halt;
    market.reason_mask = aegis::market_state::reason_bit(
        aegis::market_state::MarketStateReason::official_halt);
    market.stable_hash = aegis::market_state::stable_market_state_hash(market);
  }
  ensemble::EnsembleRequest result{
      .forecast_id = {92U, ordinal},
      .session_id = kSession,
      .instrument_id = kInstrument,
      .configuration_version = kConfiguration,
      .horizon_ns = 1'000'000U,
      .now_process_monotonic_time_ns = now,
      .valid_until_process_monotonic_time_ns = now + 10'000U,
      .market_state_snapshot = market,
      .event_state = scenario.event_state,
      .feed_health = scenario.degraded_feed ? aegis::market_state::FeedHealth::stale
                                            : aegis::market_state::FeedHealth::healthy,
      .input_data_quality_ppm = scenario.degraded_feed ? 100'000U : 900'000U,
      .expert_count = 4U,
      .transaction_cost_forecast = forecast(100U, true, now),
      .transaction_cost_health = {.generation = 1U,
                                  .state = models::ModelHealthState::healthy},
      .transaction_cost_present = true};
  for (std::size_t index = 0U; index < result.expert_count; ++index) {
    result.experts[index] = {
        .forecast = forecast(index + 1U, false, now),
        .health = {.generation = 1U, .state = models::ModelHealthState::healthy},
        .role = static_cast<ensemble::ExpertRole>(index + 1U),
        .calibration_health = ensemble::CalibrationHealth::healthy};
  }
  return result;
}

class EnsembleFixture final {
public:
  EnsembleFixture() : gate_(ensemble_config()) {}
  [[nodiscard]] Observation run(const Scenario& scenario) {
    const auto input = ensemble_request(scenario, ordinal_++);
    return timed([&] {
      const auto result = gate_.evaluate(input);
      return result.status == ensemble::EvaluationStatus::produced ||
             result.status == ensemble::EvaluationStatus::abstained;
    });
  }
  [[nodiscard]] Observation run(const Scenario& scenario,
                                const models::ModelForecast& model_forecast,
                                ensemble::EnsembleForecast& output) {
    const auto now = model_forecast.production_process_monotonic_time_ns;
    auto input = ensemble_request(scenario, ordinal_++, now);
    input.experts[0U].forecast = model_forecast;
    input.experts[0U].health.generation = model_forecast.model_control_generation;
    return timed([&] {
      const auto result = gate_.evaluate(input);
      output = result.forecast;
      return result.status == ensemble::EvaluationStatus::produced;
    });
  }

private:
  ensemble::MixtureOfExpertsGate gate_;
  std::uint64_t ordinal_{1U};
};

[[nodiscard]] risk::RiskLimitSnapshot risk_limits() {
  auto result = risk::test::limits();
  result.maximum_orders_per_window = 2'048U;
  result.order_rate_window_ns = 1U;
  result.maximum_position_age_ns = 100'000'000U;
  result.maximum_market_state_age_ns = 100'000'000U;
  result.maximum_clock_state_age_ns = 100'000'000U;
  result.maximum_feed_book_age_ns = 100'000'000U;
  result.maximum_configuration_age_ns = 100'000'000U;
  result.valid_until_process_monotonic_time_ns = kBaseNow + 100'000'000U;
  result.stable_hash = risk::stable_limit_snapshot_hash(result);
  return result;
}

class RiskFixture final {
public:
  RiskFixture() { reset(); }
  void prepare() {
    // Each approved fixture intent reserves ten units. Reset the isolated
    // account before its 10,000-unit position limit is reached.
    if (since_reset_ >= 800U) {
      reset();
    }
  }
  [[nodiscard]] Observation run(const Scenario& scenario) {
    risk::RiskIntent ignored_intent{};
    risk::RiskDecision ignored_decision{};
    return run(scenario, {}, {}, false, ignored_intent, ignored_decision);
  }
  [[nodiscard]] Observation run(const Scenario& scenario,
                                const common::ForecastId source_forecast_id,
                                const common::FeatureSnapshotId feature_snapshot_id,
                                const bool paper, risk::RiskIntent& output_intent,
                                risk::RiskDecision& output_decision) {
    prepare();
    auto request =
        risk::test::request(ordinal_, risk::IntentAction::buy, kBaseNow + ordinal_);
    request.intent.created_process_monotonic_time_ns = kBaseNow;
    request.intent.expire_process_monotonic_time_ns = kBaseNow + 100'000'000U;
    if (source_forecast_id.valid()) {
      request.intent.source_forecast_id = source_forecast_id;
    }
    if (feature_snapshot_id.valid()) {
      request.intent.feature_snapshot_id = feature_snapshot_id;
    }
    request.intent.stable_hash = risk::stable_risk_intent_hash(request.intent);
    if (paper) {
      request.context.trading_mode = risk::TradingMode::paper;
    }
    if (scenario.halted) {
      request.context.official_trading_status =
          aegis::market_state::OfficialTradingStatus::halted;
      request.context.market_state_snapshot.state =
          aegis::market_state::MarketState::halted;
      request.context.market_state_snapshot.primary_reason =
          aegis::market_state::MarketStateReason::official_halt;
      request.context.market_state_snapshot.reason_mask =
          aegis::market_state::reason_bit(
              aegis::market_state::MarketStateReason::official_halt);
      request.context.market_state_snapshot.stable_hash =
          aegis::market_state::stable_market_state_hash(
              request.context.market_state_snapshot);
    }
    if (scenario.degraded_feed) {
      request.context.feed_health = aegis::market_state::FeedHealth::stale;
      request.context.book_validity = aegis::market_state::BookValidity::invalid;
    }
    request.context.stable_hash = risk::stable_risk_context_hash(request.context);
    output_intent = request.intent;
    const auto result = timed([&] {
      const auto evaluated = engine_->evaluate(request);
      output_decision = evaluated.decision;
      risk::RiskDecision drained{};
      static_cast<void>(journal_->try_pop(drained));
      return evaluated.status == risk::EvaluationStatus::journaled;
    });
    ++ordinal_;
    ++since_reset_;
    return result;
  }

private:
  void reset() {
    journal_ = std::make_unique<risk::RiskDecisionJournal>();
    engine_ = std::make_unique<risk::DeterministicPreTradeRiskEngine>(risk_limits(),
                                                                      *journal_);
    risk::test::initialize_state(*engine_, kBaseNow);
    since_reset_ = 0U;
  }
  std::unique_ptr<risk::RiskDecisionJournal> journal_;
  std::unique_ptr<risk::DeterministicPreTradeRiskEngine> engine_;
  std::uint64_t ordinal_{1U};
  std::uint64_t since_reset_{};
};

class GatewayFixture final {
public:
  GatewayFixture() { reset(); }
  void prepare() {
    if (since_reset_ >= 800U) {
      reset();
    }
  }
  [[nodiscard]] Observation run(const Scenario& scenario) {
    prepare();
    auto request = execution::test::request(
        execution::GatewayMode::paper, oms::GatewayCommandKind::new_order, ordinal_,
        kBaseNow + ordinal_, common::OrderId{500U, ordinal_});
    request.safety.effective_configuration_hash = configuration_.stable_hash;
    if (scenario.halted) {
      request.safety.official_trading_status =
          aegis::market_state::OfficialTradingStatus::halted;
    }
    if (scenario.degraded_feed) {
      request.safety.feed_health = aegis::market_state::FeedHealth::stale;
      request.safety.book_validity = aegis::market_state::BookValidity::invalid;
    }
    request.safety.stable_hash =
        execution::stable_final_safety_state_hash(request.safety);
    request.stable_hash = execution::stable_gateway_request_hash(request);
    const auto result = timed([&] {
      const auto submitted = gateway_->send_order(request);
      return submitted.status == execution::GatewaySubmitStatus::accepted ||
             submitted.status == execution::GatewaySubmitStatus::rejected;
    });
    ++ordinal_;
    ++since_reset_;
    return result;
  }
  [[nodiscard]] Observation submit(const oms::GatewayCommand& command,
                                   const risk::RiskDecision& decision,
                                   const Scenario& scenario) {
    prepare();
    auto safety =
        execution::test::safety(command.generated_process_monotonic_time_ns + 1U,
                                execution::GatewayMode::paper);
    safety.effective_configuration_hash = configuration_.stable_hash;
    if (scenario.halted) {
      safety.official_trading_status =
          aegis::market_state::OfficialTradingStatus::halted;
    }
    if (scenario.degraded_feed) {
      safety.feed_health = aegis::market_state::FeedHealth::stale;
      safety.book_validity = aegis::market_state::BookValidity::invalid;
    }
    safety.stable_hash = execution::stable_final_safety_state_hash(safety);
    execution::GatewayRequest request{
        .command = command, .risk_decision = decision, .safety = safety};
    request.stable_hash = execution::stable_gateway_request_hash(request);
    const auto result = timed([&] {
      return gateway_->send_order(request).status ==
             execution::GatewaySubmitStatus::accepted;
    });
    ++ordinal_;
    ++since_reset_;
    return result;
  }
  [[nodiscard]] std::uint64_t high_watermark() const noexcept {
    return std::max(maximum_high_watermark_,
                    gateway_->metrics().event_queue_high_watermark);
  }

private:
  void reset() {
    if (gateway_ != nullptr) {
      maximum_high_watermark_ = std::max(
          maximum_high_watermark_, gateway_->metrics().event_queue_high_watermark);
    }
    configuration_ = execution::test::configuration(execution::GatewayMode::paper);
    configuration_.maximum_new_orders_per_window = 4'096U;
    configuration_.command_rate_window_ns = 1U;
    configuration_.stable_hash =
        execution::stable_gateway_configuration_hash(configuration_);
    journal_ = std::make_unique<execution::GatewayAuditJournal>();
    gateway_ =
        std::make_unique<execution::PaperBrokerGateway>(configuration_, *journal_);
    static_cast<void>(gateway_->start(kBaseNow - 100U));
    since_reset_ = 0U;
  }
  execution::GatewayConfiguration configuration_{};
  std::unique_ptr<execution::GatewayAuditJournal> journal_;
  std::unique_ptr<execution::PaperBrokerGateway> gateway_;
  std::uint64_t ordinal_{1U};
  std::uint64_t since_reset_{};
  std::uint64_t maximum_high_watermark_{};
};

class OmsFixture final {
public:
  OmsFixture() { reset(); }
  [[nodiscard]] Observation run() {
    if (ordinal_ % 500U == 0U) {
      reset();
    }
    const auto approved = oms::test::approval(ordinal_);
    const auto accepted = service_->apply(oms::test::accept(approved, ordinal_ * 10U));
    static_cast<void>(service_->apply(oms::test::internal(
        oms::InputKind::mark_ready, accepted.order_id, (ordinal_ * 10U) + 1U)));
    static_cast<void>(service_->apply(oms::test::internal(
        oms::InputKind::dispatch, accepted.order_id, (ordinal_ * 10U) + 2U)));
    const auto response = oms::test::venue(oms::InputKind::acknowledgement,
                                           accepted.order_id, (ordinal_ * 10U) + 3U,
                                           ordinal_, {.high = 600U, .low = ordinal_});
    const auto result = timed(
        [&] { return service_->apply(response).status == oms::ApplyStatus::applied; });
    ++ordinal_;
    return result;
  }

private:
  void reset() {
    journal_ = std::make_unique<oms::OmsJournal>();
    service_ =
        std::make_unique<oms::DeterministicOms>(oms::test::configuration(), *journal_);
  }
  std::unique_ptr<oms::OmsJournal> journal_;
  std::unique_ptr<oms::DeterministicOms> service_;
  std::uint64_t ordinal_{1U};
};

class PipelineOmsFixture final {
public:
  PipelineOmsFixture() { reset(); }
  void prepare() {
    if (since_reset_ >= 400U) {
      reset();
    }
  }
  [[nodiscard]] Observation dispatch(const risk::RiskIntent& intent,
                                     const risk::RiskDecision& decision,
                                     oms::GatewayCommand& output) {
    prepare();
    return timed([&] {
      oms::OmsInput accept{.receipt_id = {800U, ordinal_ * 10U},
                           .kind = oms::InputKind::accept_intent,
                           .source = oms::EventSource::internal,
                           .intent = intent,
                           .risk_decision = decision,
                           .process_monotonic_time_ns =
                               decision.evaluated_process_monotonic_time_ns + 1U};
      accept.stable_hash = oms::stable_input_hash(accept);
      const auto accepted = service_->apply(accept);
      if (accepted.status != oms::ApplyStatus::applied) {
        return false;
      }
      oms::OmsInput ready{.receipt_id = {800U, (ordinal_ * 10U) + 1U},
                          .kind = oms::InputKind::mark_ready,
                          .source = oms::EventSource::internal,
                          .order_id = accepted.order_id,
                          .authority = oms::test::kAuthority,
                          .process_monotonic_time_ns =
                              decision.evaluated_process_monotonic_time_ns + 2U};
      ready.stable_hash = oms::stable_input_hash(ready);
      if (service_->apply(ready).status != oms::ApplyStatus::applied) {
        return false;
      }
      oms::OmsInput dispatch{.receipt_id = {800U, (ordinal_ * 10U) + 2U},
                             .kind = oms::InputKind::dispatch,
                             .source = oms::EventSource::internal,
                             .order_id = accepted.order_id,
                             .authority = oms::test::kAuthority,
                             .process_monotonic_time_ns =
                                 decision.evaluated_process_monotonic_time_ns + 3U};
      dispatch.stable_hash = oms::stable_input_hash(dispatch);
      const auto dispatched = service_->apply(dispatch);
      output = dispatched.gateway_command;
      ++ordinal_;
      ++since_reset_;
      return dispatched.status == oms::ApplyStatus::applied &&
             oms::valid_gateway_command(output);
    });
  }

private:
  void reset() {
    auto configuration = oms::test::configuration();
    configuration.trading_mode = risk::TradingMode::paper;
    configuration.stable_hash = oms::stable_configuration_hash(configuration);
    journal_ = std::make_unique<oms::OmsJournal>();
    service_ = std::make_unique<oms::DeterministicOms>(configuration, *journal_);
    since_reset_ = 0U;
  }
  std::unique_ptr<oms::OmsJournal> journal_;
  std::unique_ptr<oms::DeterministicOms> service_;
  std::uint64_t ordinal_{1U};
  std::uint64_t since_reset_{};
};

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    std::array<char, 64U> pattern{};
    constexpr std::string_view prefix = "/tmp/aegis-platform-bench.XXXXXX";
    std::ranges::copy(prefix, pattern.begin());
    if (auto* value = ::mkdtemp(pattern.data()); value != nullptr) {
      path_ = value;
    }
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
  std::filesystem::path path_;
};

[[nodiscard]] common::Sha256Digest digest(const std::string_view value) {
  return common::sha256(std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
}

[[nodiscard]] std::string digest_hex(const common::Sha256Digest& value) {
  constexpr std::string_view kHex{"0123456789abcdef"};
  std::string output(value.size() * 2U, '0');
  for (std::size_t index = 0U; index < value.size(); ++index) {
    output[index * 2U] = kHex[value[index] >> 4U];
    output[(index * 2U) + 1U] = kHex[value[index] & 0x0FU];
  }
  return output;
}

class JournalFixture final {
public:
  JournalFixture() {
    const journal::WriterConfig config{
        .directory = temporary_.path(),
        .maximum_segment_bytes = 512ULL * 1024U * 1024U,
        .maximum_records_per_segment = 10'000'000U,
        .index_stride_records = 1'024U,
        .sync_policy = journal::SyncPolicy::segment_close,
        .periodic_sync_records = 4'096U,
        .configuration_sha256 = digest("platform-benchmark-config"),
        .build_sha256 = digest("platform-benchmark-build"),
        .writer_instance_id = {700U, 1U},
        .validate_canonical_envelopes = false};
    opened_ = service_.open(config, journal::BackpressurePolicy::fail_closed) ==
              journal::Status::ok;
  }
  ~JournalFixture() { static_cast<void>(service_.shutdown(std::chrono::seconds(5))); }
  [[nodiscard]] Observation run() {
    const journal::RecordMetadata metadata{
        .kind = journal::RecordKind::normalized_market_event,
        .encoding = journal::PayloadEncoding::opaque_binary,
        .priority = journal::RecordPriority::mandatory,
        .schema_version = {.major = 1U, .minor = 8U, .patch = 0U},
        .created_process_monotonic_time_ns = kBaseNow + ordinal_,
        .recorded_wall_clock_utc_time_ns =
            kExchangeBase + static_cast<std::int64_t>(ordinal_),
        .source_event_sequence = ordinal_,
        .global_event_id = {701U, ordinal_}};
    const auto result = timed([&] {
      return service_.try_publish({.metadata = metadata, .payload = payload_}).status ==
             journal::Status::ok;
    });
    std::size_t drained{};
    static_cast<void>(service_.drain_once(1U, drained));
    ++ordinal_;
    return {.latency_ns = result.latency_ns,
            .allocations = result.allocations,
            .operations = result.operations,
            .ok = opened_ && result.ok && drained == 1U};
  }
  [[nodiscard]] journal::AsyncMetrics metrics() const noexcept {
    return service_.metrics();
  }

private:
  TemporaryDirectory temporary_;
  journal::AsyncJournal service_;
  std::array<std::uint8_t, 256U> payload_{};
  std::uint64_t ordinal_{1U};
  bool opened_{false};
};

struct SnapshotValue {
  std::uint64_t sequence{};
  std::uint64_t complement{};
};

[[nodiscard]] std::uint64_t snapshot_checksum(const void* value) noexcept {
  const auto& snapshot = *static_cast<const SnapshotValue*>(value);
  return snapshot.sequence ^ snapshot.complement ^ 0xC3A5'C85C'97CB'3127ULL;
}

class SnapshotFixture final {
public:
  SnapshotFixture() {
    ready_ = store_.initialize(snapshot_checksum) &&
             store_.publish({.sequence = 1U, .complement = ~std::uint64_t{1U}}, 1U) ==
                 event_bus::SnapshotPublishStatus::published;
  }
  [[nodiscard]] Observation run() {
    return timed([&] {
      Store::ReadHandle handle;
      return ready_ &&
             store_.acquire(2U, handle) == event_bus::SnapshotReadStatus::snapshot &&
             handle->sequence == 1U;
    });
  }

private:
  using Store = event_bus::ImmutableSnapshotStore<SnapshotValue, 4U>;
  Store store_;
  bool ready_{false};
};

class ForecastCacheFixture final {
public:
  ForecastCacheFixture() {
    ready_ = Cache::format(memory_.bytes.data(), memory_.bytes.size(),
                           {.forecast_schema_version = 1U, .region_epoch = 1U}) ==
             event_bus::ForecastCacheStatus::ok;
    if (!ready_) {
      return;
    }
    ready_ = Cache::attach(memory_.bytes.data(), memory_.bytes.size(),
                           {.forecast_schema_version = 1U,
                            .expected_region_epoch = 1U,
                            .now_monotonic_time_ns = kBaseNow,
                            .heartbeat_timeout_ns = 1'000'000'000U,
                            .require_live_writer = false},
                           cache_) == event_bus::ForecastCacheStatus::ok;
    ready_ = ready_ && cache_.claim_writer(kWriterProcess, kWriterEpoch, kBaseNow) ==
                           event_bus::EpochClaimStatus::acquired;
    const event_bus::ForecastCacheRecord value{
        .instrument_id = kInstrument,
        .model_id = {60U, 1U},
        .model_version = {61U, 1U},
        .forecast_id = {63U, 1U},
        .feature_snapshot_id = {62U, 1U},
        .configuration_version = kConfiguration,
        .session_id = kSession,
        .forecast_value = 25,
        .confidence_ppm = 750'000U,
        .horizon_ns = 1'000'000U,
        .created_process_monotonic_time_ns = kBaseNow,
        .valid_until_process_monotonic_time_ns = kBaseNow + 500'000'000U,
        .forecast_unit_code = 1U,
        .schema_version = 1U};
    ready_ = ready_ && cache_.publish(value, kWriterProcess, kWriterEpoch, kBaseNow) ==
                           event_bus::ForecastCacheStatus::ok;
  }

  ~ForecastCacheFixture() {
    if (cache_.region() != nullptr) {
      std::destroy_at(cache_.region());
    }
  }

  ForecastCacheFixture(const ForecastCacheFixture&) = delete;
  ForecastCacheFixture& operator=(const ForecastCacheFixture&) = delete;
  ForecastCacheFixture(ForecastCacheFixture&&) = delete;
  ForecastCacheFixture& operator=(ForecastCacheFixture&&) = delete;

  [[nodiscard]] Observation run() {
    return timed([&] {
      const auto output = cache_.read({.instrument_id = kInstrument,
                                       .model_id = {60U, 1U},
                                       .horizon_ns = 1'000'000U},
                                      kBaseNow + 1U);
      return ready_ && output.status == event_bus::ForecastCacheStatus::ok &&
             output.record.forecast_id == common::ForecastId{63U, 1U};
    });
  }

private:
  using Cache = event_bus::ForecastCacheView<16U>;
  using Region = Cache::Region;
  struct alignas(Region) RegionMemory {
    std::array<std::byte, sizeof(Region)> bytes{};
  };
  static constexpr std::uint64_t kWriterProcess = 99U;
  static constexpr std::uint64_t kWriterEpoch = 1U;

  RegionMemory memory_;
  Cache cache_;
  bool ready_{false};
};

class ReplayFixture final {
public:
  explicit ReplayFixture(const std::uint64_t seed) {
    paths_ = {.capture = temporary_.path() / "events.smxcap",
              .canonical = temporary_.path() / "events.amae",
              .expected_book = temporary_.path() / "events.book.txt"};
    auto generator = synthetic::make_default_config();
    generator.seed = seed;
    generator.event_count = event_count_;
    ready_ = synthetic::generate_artifacts(generator, paths_, false).error ==
             synthetic::ArtifactError::none;
    config_.source_kind = replay::SourceKind::synthetic_capture;
    config_.speed = replay::SpeedMode::maximum;
    config_.source = paths_.capture;
    config_.seed = seed;
  }
  [[nodiscard]] Observation run() {
    replay::EvidenceReplayTarget target;
    replay::VirtualPacer pacer;
    replay::ReplayEngine engine;
    if (!ready_ || engine.prepare(config_, target, pacer) != replay::Status::ok) {
      return {.ok = false};
    }
    auto result = timed([&] { return engine.run() == replay::Status::complete; });
    result.operations = event_count_;
    return result;
  }

private:
  static constexpr std::uint64_t event_count_{256U};
  TemporaryDirectory temporary_;
  synthetic::ArtifactPaths paths_;
  replay::ReplayConfig config_;
  bool ready_{false};
};

class PipelineFixture final {
public:
  [[nodiscard]] Observation to_intent(const Scenario& scenario) {
    return timed([&] {
      const auto feed_result = feed_.run(false);
      const auto book_result = book_.run(feed_.last_event());
      const auto feature_result = feature_.run(book_.book(), feed_.last_event());
      const auto snapshot = feature_.snapshot();
      models::ModelForecast model_forecast{};
      const auto model_result = model_.run(snapshot, model_forecast);
      ensemble::EnsembleForecast ensemble_forecast{};
      const auto ensemble_result =
          ensemble_.run(scenario, model_forecast, ensemble_forecast);
      auto intent = risk::test::intent(++ordinal_);
      intent.source_forecast_id = ensemble_forecast.forecast_id;
      intent.feature_snapshot_id = snapshot.snapshot_id;
      intent.stable_hash = risk::stable_risk_intent_hash(intent);
      return feed_result.ok && book_result.ok && feature_result.ok && model_result.ok &&
             ensemble_result.ok && !ensemble_forecast.abstain &&
             intent.stable_hash != 0U;
    });
  }
  [[nodiscard]] Observation to_paper_send(const Scenario& scenario) {
    if (scenario.halted || scenario.degraded_feed) {
      return {.ok = false};
    }
    risk_.prepare();
    oms_.prepare();
    gateway_.prepare();
    return timed([&] {
      const auto feed_result = feed_.run(false);
      const auto book_result = book_.run(feed_.last_event());
      const auto feature_result = feature_.run(book_.book(), feed_.last_event());
      const auto snapshot = feature_.snapshot();
      models::ModelForecast model_forecast{};
      const auto model_result = model_.run(snapshot, model_forecast);
      ensemble::EnsembleForecast ensemble_forecast{};
      const auto ensemble_result =
          ensemble_.run(scenario, model_forecast, ensemble_forecast);
      risk::RiskIntent intent{};
      risk::RiskDecision decision{};
      const auto risk_result = risk_.run(scenario, ensemble_forecast.forecast_id,
                                         snapshot.snapshot_id, true, intent, decision);
      oms::GatewayCommand command{};
      const auto oms_result = oms_.dispatch(intent, decision, command);
      const auto gateway_result = gateway_.submit(command, decision, scenario);
      ++ordinal_;
      if (!feed_result.ok) {
        last_failure_ = "feed";
      } else if (!book_result.ok) {
        last_failure_ = "book";
      } else if (!feature_result.ok) {
        last_failure_ = "feature";
      } else if (!model_result.ok) {
        last_failure_ = "model";
      } else if (!ensemble_result.ok || ensemble_forecast.abstain) {
        last_failure_ = "ensemble";
      } else if (!risk_result.ok || decision.decision != risk::DecisionCode::approved) {
        last_failure_ = "risk";
      } else if (!oms_result.ok) {
        last_failure_ = "oms";
      } else if (!gateway_result.ok) {
        last_failure_ = "gateway";
      } else {
        last_failure_.clear();
      }
      return last_failure_.empty();
    });
  }
  [[nodiscard]] std::uint64_t packet_drops() const noexcept { return feed_.drops(); }
  [[nodiscard]] std::uint64_t queue_high_watermark() const noexcept {
    return std::max(feed_.high_watermark(), gateway_.high_watermark());
  }
  [[nodiscard]] const std::string& last_failure() const noexcept {
    return last_failure_;
  }

private:
  FeedFixture feed_;
  BookFixture book_;
  FeatureFixture feature_;
  ModelFixture model_;
  EnsembleFixture ensemble_;
  RiskFixture risk_;
  PipelineOmsFixture oms_;
  GatewayFixture gateway_;
  std::uint64_t ordinal_{};
  std::string last_failure_;
};

template <typename Operation>
[[nodiscard]] CaseResult
collect_case(const Scenario& scenario, const std::string_view stage,
             // Warmup and samples together define one sampling plan.
             // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
             const std::size_t warmup, const std::size_t samples,
             Operation&& operation) {
  CaseResult result;
  result.scenario = scenario.name;
  result.stage = stage;
  result.latency_samples.reserve(samples);
  std::vector<std::array<std::uint64_t, 4'096U>> symbol_working_set(
      scenario.symbol_count);
  std::uint64_t symbol_working_set_checksum{};
  std::size_t offered_ordinal{};
  const auto touch_symbol_state = [&] {
    const auto symbol = offered_ordinal % scenario.symbol_count;
    const auto slot = (offered_ordinal * 1'031U) % symbol_working_set[symbol].size();
    symbol_working_set[symbol][slot] ^=
        static_cast<std::uint64_t>(offered_ordinal + 1U);
    symbol_working_set_checksum ^= symbol_working_set[symbol][slot];
    ++offered_ordinal;
  };
  for (std::size_t index = 0U; index < warmup; ++index) {
    for (std::uint32_t offered = 0U; offered < scenario.load_multiplier; ++offered) {
      touch_symbol_state();
      static_cast<void>(std::invoke(operation));
    }
  }
  const CacheMissCounter cache_counter;
  const bool cache_started = cache_counter.start();
  const auto elapsed_before = monotonic_raw_ns();
  const auto cpu_before = thread_cpu_ns();
  for (std::size_t index = 0U; index < samples; ++index) {
    std::uint64_t worst_latency{};
    for (std::uint32_t offered = 0U; offered < scenario.load_multiplier; ++offered) {
      touch_symbol_state();
      const auto observed = std::invoke(operation);
      if (!observed.ok) {
        result.status = "UNAVAILABLE";
        result.notes.emplace_back("component operation rejected during sampling");
        result.latency_samples.clear();
        result.operation_count = 0U;
        break;
      }
      worst_latency = std::max(worst_latency, observed.latency_ns);
      result.allocations += observed.allocations;
      result.operation_count += observed.operations;
    }
    if (result.status != "MEASURED") {
      break;
    }
    result.latency_samples.push_back(worst_latency);
  }
  const auto cpu_after = thread_cpu_ns();
  const auto elapsed_after = monotonic_raw_ns();
  result.elapsed_ns =
      elapsed_after >= elapsed_before ? elapsed_after - elapsed_before : 0U;
  result.cpu_time_ns = cpu_after >= cpu_before ? cpu_after - cpu_before : 0U;
  if (cache_started) {
    result.cache_misses = cache_counter.finish();
  }
  if (result.cache_misses.has_value()) {
    result.cache_misses_unavailable_reason.clear();
  } else {
    result.cache_misses_unavailable_reason = cache_counter.unavailable_reason();
  }
  result.notes.emplace_back("in-process synthetic input; no physical NIC was measured");
  if (scenario.load_multiplier > 1U) {
    result.notes.emplace_back("each sample is the worst per-operation latency in the "
                              "back-to-back offered batch");
  }
  if (scenario.symbol_count > 1U) {
    result.notes.emplace_back("preallocated symbol working-set lanes=" +
                              std::to_string(scenario.symbol_count) + "; checksum=" +
                              std::to_string(symbol_working_set_checksum));
  }
  return result;
}

[[nodiscard]] CaseResult safety_blocked_case(const Scenario& scenario,
                                             const std::string_view stage) {
  CaseResult result;
  result.scenario = scenario.name;
  result.stage = stage;
  result.status = "SAFETY_BLOCKED";
  result.cache_misses_unavailable_reason =
      "hardware counter not collected because safety prevented the operation";
  result.notes.emplace_back("pre-trade safety prevented executable paper transmission");
  return result;
}

[[nodiscard]] std::vector<CaseResult> run_scenario(const Scenario& scenario,
                                                   const Options& options) {
  std::vector<CaseResult> output;
  output.reserve(kStages.size());

  FeedFixture feed_fixture;
  auto feed_case =
      collect_case(scenario, kStages[0U], options.warmup, options.samples,
                   [&] { return feed_fixture.run(scenario.degraded_feed); });
  feed_case.queue_capacity = feed::kMaximumReceiverQueue;
  feed_case.queue_high_watermark = feed_fixture.high_watermark();
  feed_case.packet_drops = feed_fixture.drops();
  output.push_back(std::move(feed_case));

  BookFixture book_fixture;
  output.push_back(collect_case(scenario, kStages[1U], options.warmup, options.samples,
                                [&] { return book_fixture.run(); }));

  FeatureFixture feature_fixture;
  output.push_back(
      collect_case(scenario, kStages[2U], options.warmup, options.samples,
                   [&] { return feature_fixture.run(scenario.degraded_feed); }));

  ModelFixture model_fixture;
  output.push_back(collect_case(scenario, kStages[3U], options.warmup, options.samples,
                                [&] { return model_fixture.run(); }));

  EnsembleFixture ensemble_fixture;
  output.push_back(collect_case(scenario, kStages[4U], options.warmup, options.samples,
                                [&] { return ensemble_fixture.run(scenario); }));

  RiskFixture risk_fixture;
  output.push_back(collect_case(scenario, kStages[5U], options.warmup, options.samples,
                                [&] { return risk_fixture.run(scenario); }));

  GatewayFixture gateway_fixture;
  auto gateway_case =
      collect_case(scenario, kStages[6U], options.warmup, options.samples,
                   [&] { return gateway_fixture.run(scenario); });
  gateway_case.queue_capacity = execution::kMaximumGatewayEvents;
  gateway_case.queue_high_watermark = gateway_fixture.high_watermark();
  output.push_back(std::move(gateway_case));

  if (scenario.halted || scenario.degraded_feed) {
    output.push_back(safety_blocked_case(scenario, kStages[7U]));
  } else {
    PipelineFixture intent_pipeline;
    auto intent_case =
        collect_case(scenario, kStages[7U], options.warmup, options.samples,
                     [&] { return intent_pipeline.to_intent(scenario); });
    intent_case.queue_capacity = feed::kMaximumReceiverQueue;
    intent_case.queue_high_watermark = intent_pipeline.queue_high_watermark();
    intent_case.packet_drops = intent_pipeline.packet_drops();
    output.push_back(std::move(intent_case));
  }

  if (scenario.halted || scenario.degraded_feed) {
    output.push_back(safety_blocked_case(scenario, kStages[8U]));
  } else {
    PipelineFixture send_pipeline;
    auto send_case =
        collect_case(scenario, kStages[8U], options.warmup, options.samples,
                     [&] { return send_pipeline.to_paper_send(scenario); });
    send_case.queue_capacity = execution::kMaximumGatewayEvents;
    send_case.queue_high_watermark = send_pipeline.queue_high_watermark();
    send_case.packet_drops = send_pipeline.packet_drops();
    if (!send_pipeline.last_failure().empty()) {
      send_case.notes.emplace_back("composed pipeline failure stage: " +
                                   send_pipeline.last_failure());
    }
    send_case.notes.emplace_back("paper gateway only; no live transmission code path");
    output.push_back(std::move(send_case));
  }

  OmsFixture oms_fixture;
  output.push_back(collect_case(scenario, kStages[9U], options.warmup, options.samples,
                                [&] { return oms_fixture.run(); }));

  JournalFixture journal_fixture;
  auto journal_case =
      collect_case(scenario, kStages[10U], options.warmup, options.samples,
                   [&] { return journal_fixture.run(); });
  const auto journal_metrics = journal_fixture.metrics();
  journal_case.queue_capacity = 4'096U;
  journal_case.queue_high_watermark = journal_metrics.maximum_queue_lag;
  output.push_back(std::move(journal_case));

  SnapshotFixture snapshot_fixture;
  output.push_back(collect_case(scenario, kStages[11U], options.warmup, options.samples,
                                [&] { return snapshot_fixture.run(); }));

  ForecastCacheFixture cache_fixture;
  output.push_back(collect_case(scenario, kStages[12U], options.warmup, options.samples,
                                [&] { return cache_fixture.run(); }));

  ReplayFixture replay_fixture(options.seed);
  output.push_back(collect_case(scenario, kStages[13U],
                                std::min(options.warmup, std::size_t{4U}),
                                options.samples, [&] { return replay_fixture.run(); }));
  return output;
}

void write_optional_integer(std::ostream& output,
                            const std::optional<std::uint64_t> value) {
  if (value.has_value()) {
    output << *value;
  } else {
    output << "null";
  }
}

[[nodiscard]] bool write_samples(const std::filesystem::path& path,
                                 const std::span<const CaseResult> results) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    return false;
  }
  // Escaped JSON fragments make the NDJSON separators explicit and auditable.
  // NOLINTBEGIN(modernize-raw-string-literal)
  for (const auto& result : results) {
    for (const auto latency : result.latency_samples) {
      output << "{\"latency_ns\":" << latency << ",\"scenario\":\""
             << json_escape(result.scenario) << "\",\"stage\":\""
             << json_escape(result.stage) << "\"}\n";
    }
  }
  // NOLINTEND(modernize-raw-string-literal)
  output.flush();
  return output.good();
}

[[nodiscard]] bool write_metadata(const std::filesystem::path& path,
                                  const std::span<const CaseResult> results,
                                  const Options& options, const int pinned_cpu) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    return false;
  }
  const auto numa = numa_node_for_cpu(pinned_cpu);
  const auto governor = cpu_governor(pinned_cpu);
  const auto cpu_count = std::thread::hardware_concurrency();
  const auto configuration_hash =
      digest_hex(digest("aegis-mx-colocated-performance-v1"));
  const auto& build = common::current_build_info();
  const auto cache_available =
      std::ranges::any_of(results, [](const CaseResult& result) {
        return result.cache_misses.has_value();
      });
  std::string cache_unavailable_reason = "perf_event hardware counter unavailable";
  if (!cache_available) {
    const auto reason = std::ranges::find_if(results, [](const CaseResult& result) {
      return !result.cache_misses_unavailable_reason.empty();
    });
    if (reason != results.end()) {
      cache_unavailable_reason = reason->cache_misses_unavailable_reason;
    }
  }
  // Escaped fragments keep the emitted JSON layout legible beside values.
  // NOLINTBEGIN(modernize-raw-string-literal)
  output << "{\n  \"sampler_schema_version\": 1,\n"
         << "  \"suite_id\": \"aegis-mx-full-platform-v1\",\n"
         << "  \"mode\": \"PAPER\",\n"
         << "  \"measurement_source\": \"IN_PROCESS_SYNTHETIC\",\n"
         << "  \"seed\": " << options.seed << ",\n"
         << "  \"configuration_sha256\": \"" << configuration_hash << "\",\n"
         << "  \"code_commit\": \"" << json_escape(build.source_revision) << "\",\n"
         << "  \"environment\": {\n"
         << "    \"hostname\": \"" << json_escape(hostname()) << "\",\n"
         << "    \"operating_system\": \"" << json_escape(uname_field()) << "\",\n"
         << "    \"kernel\": \"" << json_escape(uname_field()) << "\",\n"
         << "    \"cpu_model\": \""
         << json_escape(read_first_matching_line("/proc/cpuinfo", "model name"))
         << "\",\n"
         << "    \"logical_cpu_count\": " << std::max(cpu_count, 1U) << ",\n"
         << "    \"pinned_cpu\": " << pinned_cpu << ",\n"
         << "    \"thread_pinning_verified\": true,\n"
         << "    \"numa_node\": ";
  if (numa.has_value()) {
    output << *numa;
  } else {
    output << "null";
  }
  output << ",\n    \"cpu_governor\": ";
  if (governor.has_value()) {
    output << '"' << json_escape(*governor) << '"';
  } else {
    output << "null";
  }
  output << ",\n    \"build_type\": \"" << json_escape(build.build_type) << "\",\n"
         << "    \"compiler\": \"" << json_escape(build.compiler_id) << ' '
         << json_escape(build.compiler_version) << "\"\n  },\n"
         << "  \"methodology\": {\n"
         << "    \"warmup_iterations\": " << options.warmup << ",\n"
         << "    \"samples_per_latency_case\": " << options.samples << ",\n"
         << "    \"clock\": \"CLOCK_MONOTONIC_RAW\",\n"
         << "    \"caches_warmed\": true,\n"
         << "    \"allocation_probe_enabled\": true,\n"
         << "    \"hardware_cache_misses_available\": "
         << (cache_available ? "true" : "false") << ",\n"
         << "    \"hardware_cache_misses_unavailable_reason\": ";
  if (cache_available) {
    output << "null";
  } else {
    output << '"' << json_escape(cache_unavailable_reason) << '"';
  }
  output << "\n  },\n"
         << "  \"cases\": [\n";
  for (std::size_t index = 0U; index < results.size(); ++index) {
    const auto& result = results[index];
    output << "    {\"scenario\":\"" << json_escape(result.scenario)
           << "\",\"stage\":\"" << json_escape(result.stage) << "\",\"status\":\""
           << result.status << "\",\"operation_count\":" << result.operation_count
           << ",\"elapsed_ns\":" << result.elapsed_ns
           << ",\"cpu_time_ns\":" << result.cpu_time_ns
           << ",\"allocations\":" << result.allocations << ",\"cache_misses\":";
    write_optional_integer(output, result.cache_misses);
    output << ",\"cache_misses_unavailable_reason\":";
    if (result.cache_misses_unavailable_reason.empty()) {
      output << "null";
    } else {
      output << '"' << json_escape(result.cache_misses_unavailable_reason) << '"';
    }
    output << ",\"queue_capacity\":";
    write_optional_integer(output, result.queue_capacity);
    output << ",\"queue_high_watermark\":";
    write_optional_integer(output, result.queue_high_watermark);
    output << ",\"packet_drops\":" << result.packet_drops << ",\"notes\":[";
    for (std::size_t note = 0U; note < result.notes.size(); ++note) {
      if (note != 0U) {
        output << ',';
      }
      output << '"' << json_escape(result.notes[note]) << '"';
    }
    output << "]}" << (index + 1U == results.size() ? "\n" : ",\n");
  }
  output << "  ]\n}\n";
  // NOLINTEND(modernize-raw-string-literal)
  output.flush();
  return output.good();
}

[[nodiscard]] std::optional<std::size_t> parse_size(const std::string_view value) {
  try {
    std::size_t consumed{};
    const auto parsed = std::stoull(std::string{value}, &consumed, 10);
    if (consumed != value.size() || parsed == 0U || parsed > 1'000'000U) {
      return std::nullopt;
    }
    return static_cast<std::size_t>(parsed);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

[[nodiscard]] std::optional<std::uint64_t> parse_seed(const std::string_view value) {
  try {
    std::size_t consumed{};
    const auto parsed = std::stoull(std::string{value}, &consumed, 10);
    if (consumed != value.size() || parsed == 0U) {
      return std::nullopt;
    }
    return parsed;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

[[nodiscard]] std::optional<Options> parse_options(const int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    if (index + 1 >= argc) {
      return std::nullopt;
    }
    const std::string_view value{argv[++index]};
    if (argument == "--metadata") {
      options.metadata_path = value;
    } else if (argument == "--samples-output") {
      options.samples_path = value;
    } else if (argument == "--samples") {
      const auto parsed = parse_size(value);
      if (!parsed.has_value()) {
        return std::nullopt;
      }
      options.samples = *parsed;
    } else if (argument == "--warmup") {
      const auto parsed = parse_size(value);
      if (!parsed.has_value()) {
        return std::nullopt;
      }
      options.warmup = *parsed;
    } else if (argument == "--seed") {
      const auto parsed = parse_seed(value);
      if (!parsed.has_value()) {
        return std::nullopt;
      }
      options.seed = *parsed;
    } else {
      return std::nullopt;
    }
  }
  return options;
}

} // namespace

int main(const int argc, char** argv) {
  const auto options = parse_options(argc, argv);
  if (!options.has_value()) {
    std::cerr << "Usage: aegis_platform_benchmark --metadata PATH --samples-output "
                 "PATH --samples N --warmup N --seed N\n";
    return 2;
  }
  const auto pinned_cpu = pin_to_first_allowed_cpu();
  if (!pinned_cpu.has_value()) {
    const auto pin_error = std::error_code{errno, std::generic_category()};
    std::cerr << "Unable to pin benchmark to exactly one inherited CPU: "
              << pin_error.message() << '\n';
    return 3;
  }
  std::error_code error;
  std::filesystem::create_directories(options->metadata_path.parent_path(), error);
  if (error) {
    std::cerr << "Unable to create metadata directory: " << error.message() << '\n';
    return 4;
  }
  std::filesystem::create_directories(options->samples_path.parent_path(), error);
  if (error) {
    std::cerr << "Unable to create sample directory: " << error.message() << '\n';
    return 4;
  }
  std::vector<CaseResult> results;
  results.reserve(kScenarios.size() * kStages.size());
  for (const auto& scenario : kScenarios) {
    auto scenario_results = run_scenario(scenario, *options);
    std::ranges::move(scenario_results, std::back_inserter(results));
  }
  if (!write_samples(options->samples_path, results) ||
      !write_metadata(options->metadata_path, results, *options, *pinned_cpu)) {
    std::cerr << "Unable to write benchmark evidence\n";
    return 5;
  }
  const auto unavailable = std::ranges::count_if(
      results, [](const CaseResult& value) { return value.status == "UNAVAILABLE"; });
  std::cout << "{\"cases\":" << results.size() << ",\"pinned_cpu\":" << *pinned_cpu
            << ",\"samples_per_case\":" << options->samples
            << ",\"unavailable_cases\":" << unavailable << "}\n";
  return unavailable == 0 ? 0 : 1;
}
