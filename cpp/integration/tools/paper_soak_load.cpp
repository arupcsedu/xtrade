#include "aegis/common/build_info.hpp"
#include "aegis/market_data/synthetic/config.hpp"
#include "aegis/market_data/synthetic/event.hpp"
#include "aegis/market_data/synthetic/generator.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <sys/resource.h>
#include <unistd.h>

namespace synthetic = aegis::market_data::synthetic;

namespace {

constexpr std::uint64_t kDefaultSeed = 20'260'907U;
constexpr std::uint64_t kDefaultEvents = 1'000'000U;
constexpr std::uint64_t kDefaultSessionEvents = 100'000U;
constexpr std::uint64_t kDefaultRealtimeRate = 1'000U;
constexpr std::uint64_t kMaximumRssGrowthBytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaximumTailDriftPpm = 500'000U;
constexpr std::uint64_t kTailNoiseAllowanceNs = 500U;
constexpr std::size_t kDefaultSampleLimit = 8'192U;
constexpr std::size_t kMaximumSampleLimit = 65'536U;
constexpr std::uint64_t kMaximumSessions = 100'000U;
constexpr std::uint64_t kHashOffset = 1'469'598'103'934'665'603ULL;
constexpr std::uint64_t kHashPrime = 1'099'511'628'211ULL;

struct Options {
  std::uint64_t seed{kDefaultSeed};
  std::uint64_t events{kDefaultEvents};
  std::uint64_t session_events{kDefaultSessionEvents};
  std::uint64_t realtime_seconds{1U};
  std::uint64_t realtime_rate{kDefaultRealtimeRate};
  std::uint64_t worker_id{};
  std::size_t sample_limit{kDefaultSampleLimit};
  std::filesystem::path machine_path{"soak-load-report.json"};
  std::filesystem::path raw_path{"soak-load-samples.ndjson"};
};

struct ResourceSample {
  std::uint64_t rss_bytes{};
  std::uint64_t open_file_descriptors{};
};

struct LatencySummary {
  std::uint64_t p50_ns{};
  std::uint64_t p95_ns{};
  std::uint64_t p99_ns{};
  std::uint64_t p999_ns{};
  std::uint64_t maximum_ns{};
  std::uint64_t first_half_p99_ns{};
  std::uint64_t second_half_p99_ns{};
  std::uint64_t sample_count{};
};

struct SessionSample {
  std::uint64_t session_index{};
  synthetic::Scenario scenario{synthetic::Scenario::normal};
  std::uint64_t seed{};
  std::uint64_t configuration_hash{};
  std::uint64_t event_count{};
  std::uint64_t primary_stream_hash{};
  std::uint64_t replay_stream_hash{};
  std::uint64_t primary_book_hash{};
  std::uint64_t replay_book_hash{};
  std::uint64_t elapsed_ns{};
  std::uint64_t replay_elapsed_ns{};
  ResourceSample resources{};
  bool valid{false};
  bool deterministic{false};
};

struct PassResult {
  std::uint64_t stream_hash{kHashOffset};
  std::uint64_t book_hash{};
  std::uint64_t elapsed_ns{};
  std::uint64_t event_count{};
  bool valid{false};
};

struct Report {
  Options options;
  std::vector<SessionSample> sessions;
  LatencySummary latency;
  ResourceSample initial_resources;
  ResourceSample warm_resources;
  ResourceSample final_resources;
  std::uint64_t primary_events{};
  std::uint64_t replay_events{};
  std::uint64_t realtime_events{};
  std::uint64_t scenario_coverage_mask{};
  std::uint64_t primary_elapsed_ns{};
  std::uint64_t replay_elapsed_ns{};
  std::uint64_t realtime_elapsed_ns{};
  std::uint64_t accelerated_wall_elapsed_ns{};
  std::uint64_t accelerated_cpu_time_ns{};
  std::uint64_t accelerated_cpu_utilization_ppm{};
  std::uint64_t stream_hash{kHashOffset};
  std::uint64_t replay_hash{kHashOffset};
  std::uint64_t report_hash{kHashOffset};
  bool all_events_valid{true};
  bool deterministic_replay{true};
  bool memory_stable{false};
  bool descriptors_stable{false};
  bool tail_latency_stable{false};
  bool realtime_completed{false};
  bool passed{false};
};

constexpr std::array<synthetic::Scenario, 7U> kScenarios{
    synthetic::Scenario::normal,
    synthetic::Scenario::high_message_rate_burst,
    synthetic::Scenario::earnings_shock,
    synthetic::Scenario::macro_shock,
    synthetic::Scenario::index_rebalance,
    synthetic::Scenario::hidden_liquidity_replenishment,
    synthetic::Scenario::reopening_auction,
};

void mix(std::uint64_t& hash, const std::uint64_t value) noexcept {
  hash ^= value;
  hash *= kHashPrime;
}

[[nodiscard]] std::optional<std::uint64_t>
parse_unsigned(const std::string_view value) noexcept {
  std::uint64_t parsed{};
  const auto* const begin = value.data();
  const auto* const end = begin + value.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc{} || result.ptr != end) {
    return std::nullopt;
  }
  return parsed;
}

[[nodiscard]] ResourceSample resource_sample() {
  ResourceSample sample{};
  std::ifstream status{"/proc/self/status"};
  std::string label;
  while (status >> label) {
    if (label == "VmRSS:") {
      std::uint64_t kibibytes{};
      std::string unit;
      status >> kibibytes >> unit;
      if (kibibytes <= std::numeric_limits<std::uint64_t>::max() / 1024U) {
        sample.rss_bytes = kibibytes * 1024U;
      }
      break;
    }
    std::string rest;
    std::getline(status, rest);
  }

  std::error_code error;
  const std::filesystem::directory_iterator end;
  for (std::filesystem::directory_iterator entry{"/proc/self/fd", error};
       !error && entry != end; entry.increment(error)) {
    ++sample.open_file_descriptors;
  }
  return sample;
}

[[nodiscard]] std::uint64_t process_cpu_time_ns() noexcept {
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0) {
    return 0U;
  }
  const auto seconds = static_cast<std::uint64_t>(usage.ru_utime.tv_sec) +
                       static_cast<std::uint64_t>(usage.ru_stime.tv_sec);
  const auto microseconds = static_cast<std::uint64_t>(usage.ru_utime.tv_usec) +
                            static_cast<std::uint64_t>(usage.ru_stime.tv_usec);
  return (seconds * 1'000'000'000ULL) + (microseconds * 1'000ULL);
}

[[nodiscard]] std::uint64_t percentile(std::vector<std::uint64_t> samples,
                                       const std::uint64_t numerator,
                                       const std::uint64_t denominator) {
  if (samples.empty()) {
    return 0U;
  }
  std::ranges::sort(samples);
  const auto rank = ((samples.size() - 1U) * static_cast<std::size_t>(numerator)) /
                    static_cast<std::size_t>(denominator);
  return samples[rank];
}

[[nodiscard]] LatencySummary
summarize_latency(const std::vector<std::uint64_t>& samples) {
  LatencySummary result{};
  result.sample_count = samples.size();
  result.p50_ns = percentile(samples, 50U, 100U);
  result.p95_ns = percentile(samples, 95U, 100U);
  result.p99_ns = percentile(samples, 99U, 100U);
  result.p999_ns = percentile(samples, 999U, 1'000U);
  result.maximum_ns = percentile(samples, 1U, 1U);
  const auto midpoint = samples.size() / 2U;
  if (midpoint != 0U) {
    result.first_half_p99_ns = percentile(
        std::vector<std::uint64_t>(
            samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(midpoint)),
        99U, 100U);
    result.second_half_p99_ns = percentile(
        std::vector<std::uint64_t>(
            samples.begin() + static_cast<std::ptrdiff_t>(midpoint), samples.end()),
        99U, 100U);
  } else {
    result.first_half_p99_ns = result.p99_ns;
    result.second_half_p99_ns = result.p99_ns;
  }
  return result;
}

[[nodiscard]] synthetic::GeneratorConfig
// Both unsigned values have distinct, documented units at this local builder.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
session_config(const Options& options, const std::uint64_t session_index,
               const std::uint64_t event_count) noexcept {
  auto config = synthetic::make_default_config();
  config.seed = options.seed + (session_index * 0x9E37'79B9U);
  config.event_count = event_count;
  config.scenario = kScenarios[session_index % kScenarios.size()];
  config.maximum_active_orders_per_instrument = 128U;
  if (event_count >= 64U) {
    config.auction_start_event = event_count / 4U;
    config.auction_end_event =
        config.auction_start_event + std::min<std::uint64_t>(event_count / 8U, 10'000U);
  }
  return config;
}

[[nodiscard]] PassResult generate_pass(const synthetic::GeneratorConfig& config,
                                       std::vector<std::uint64_t>* latency_samples,
                                       const std::uint64_t sample_interval,
                                       const std::size_t sample_limit) {
  PassResult result{};
  auto generator = synthetic::SyntheticExchangeGenerator::create(config);
  if (!generator.has_value()) {
    return result;
  }
  const auto started = std::chrono::steady_clock::now();
  std::uint64_t previous_monotonic{};
  synthetic::SyntheticEvent event{};
  for (std::uint64_t ordinal = 1U; ordinal <= config.event_count; ++ordinal) {
    const bool measure = latency_samples != nullptr &&
                         latency_samples->size() < sample_limit &&
                         (ordinal == 1U || ordinal % sample_interval == 0U);
    const auto before = measure ? std::chrono::steady_clock::now() : started;
    const auto generation = generator->next(event);
    const auto after = measure ? std::chrono::steady_clock::now() : started;
    if (!generation.ok() || event.global_ordinal != ordinal ||
        event.process_monotonic_time_ns < previous_monotonic ||
        !synthetic::valid_event_shape(event) ||
        event.event_hash != synthetic::calculate_event_hash(event)) {
      result.elapsed_ns = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - started)
              .count());
      return result;
    }
    if (measure) {
      latency_samples->push_back(static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(after - before)
              .count()));
    }
    previous_monotonic = event.process_monotonic_time_ns;
    mix(result.stream_hash, event.event_hash);
    ++result.event_count;
  }
  const auto completion = generator->next(event);
  result.elapsed_ns =
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - started)
                                     .count());
  result.book_hash = generator->expected_book().stable_hash();
  result.valid = completion.error == synthetic::GenerationError::complete &&
                 result.event_count == config.event_count;
  return result;
}

[[nodiscard]] PassResult generate_realtime(const Options& options) {
  PassResult result{};
  if (options.realtime_seconds == 0U) {
    result.valid = true;
    return result;
  }
  if (options.realtime_rate == 0U ||
      options.realtime_seconds >
          std::numeric_limits<std::uint64_t>::max() / options.realtime_rate) {
    return result;
  }
  const auto count = options.realtime_seconds * options.realtime_rate;
  auto config = session_config(options, 0xFFFFU, count);
  config.scenario = synthetic::Scenario::normal;
  config.normal_message_rate_per_second = options.realtime_rate;
  auto generator = synthetic::SyntheticExchangeGenerator::create(config);
  if (!generator.has_value()) {
    return result;
  }
  const auto started = std::chrono::steady_clock::now();
  synthetic::SyntheticEvent event{};
  const auto pacing_batch = std::max<std::uint64_t>(1U, options.realtime_rate / 100U);
  for (std::uint64_t ordinal = 1U; ordinal <= count; ++ordinal) {
    const auto generation = generator->next(event);
    if (!generation.ok() || !synthetic::valid_event_shape(event) ||
        event.event_hash != synthetic::calculate_event_hash(event)) {
      return result;
    }
    mix(result.stream_hash, event.event_hash);
    ++result.event_count;
    if (ordinal % pacing_batch == 0U || ordinal == count) {
      const auto expected_elapsed_ns =
          (ordinal * 1'000'000'000ULL) / options.realtime_rate;
      std::this_thread::sleep_until(started +
                                    std::chrono::nanoseconds(expected_elapsed_ns));
    }
  }
  result.elapsed_ns =
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - started)
                                     .count());
  result.book_hash = generator->expected_book().stable_hash();
  result.valid = result.event_count == count;
  return result;
}

// Flat CLI dispatch remains more auditable than dynamic option registration.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] bool parse_options(const int argc, char** argv, Options& output) {
  for (int index = 1; index < argc; ++index) {
    const std::string_view option{argv[index]};
    if (option == "--help") {
      std::cout << "Usage: aegis-paper-soak-load [options]\n"
                   "  --events N --session-events N --seed N --worker-id N\n"
                   "  --sample-limit N --realtime-seconds N --realtime-rate N\n"
                   "  --machine PATH --raw PATH\n";
      return false;
    }
    if (index + 1 >= argc) {
      std::cerr << "missing value for " << option << '\n';
      return false;
    }
    const std::string_view value{argv[++index]};
    if (option == "--machine") {
      output.machine_path = value;
    } else if (option == "--raw") {
      output.raw_path = value;
    } else {
      const auto parsed = parse_unsigned(value);
      if (!parsed.has_value()) {
        std::cerr << "invalid unsigned value for " << option << '\n';
        return false;
      }
      if (option == "--events") {
        output.events = *parsed;
      } else if (option == "--session-events") {
        output.session_events = *parsed;
      } else if (option == "--seed") {
        output.seed = *parsed;
      } else if (option == "--worker-id") {
        output.worker_id = *parsed;
      } else if (option == "--sample-limit") {
        if (*parsed > kMaximumSampleLimit) {
          std::cerr << "sample limit exceeds bounded maximum\n";
          return false;
        }
        output.sample_limit = static_cast<std::size_t>(*parsed);
      } else if (option == "--realtime-seconds") {
        output.realtime_seconds = *parsed;
      } else if (option == "--realtime-rate") {
        output.realtime_rate = *parsed;
      } else {
        std::cerr << "unknown option: " << option << '\n';
        return false;
      }
    }
  }
  return output.events != 0U && output.session_events != 0U &&
         output.sample_limit >= 2U && output.seed != 0U &&
         1U + ((output.events - 1U) / output.session_events) <= kMaximumSessions;
}

[[nodiscard]] bool ensure_parent(const std::filesystem::path& path) {
  if (!path.has_parent_path()) {
    return true;
  }
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  return !error;
}

// JSON fragments stay visually aligned with the emitted evidence contract.
// NOLINTBEGIN(modernize-raw-string-literal)
[[nodiscard]] bool write_raw(const Report& report) {
  if (!ensure_parent(report.options.raw_path)) {
    return false;
  }
  std::ofstream output{report.options.raw_path, std::ios::trunc};
  if (!output) {
    return false;
  }
  for (const auto& session : report.sessions) {
    output << "{\"schema_version\":\"1.0\",\"kind\":\"session\","
           << "\"worker_id\":" << report.options.worker_id << ','
           << "\"session_index\":" << session.session_index << ',' << "\"scenario\":\""
           << synthetic::scenario_name(session.scenario)
           << "\",\"seed\":" << session.seed << ','
           << "\"configuration_hash\":" << session.configuration_hash << ','
           << "\"event_count\":" << session.event_count << ','
           << "\"primary_stream_hash\":" << session.primary_stream_hash << ','
           << "\"replay_stream_hash\":" << session.replay_stream_hash << ','
           << "\"primary_book_hash\":" << session.primary_book_hash << ','
           << "\"replay_book_hash\":" << session.replay_book_hash << ','
           << "\"elapsed_ns\":" << session.elapsed_ns << ','
           << "\"replay_elapsed_ns\":" << session.replay_elapsed_ns << ','
           << "\"rss_bytes\":" << session.resources.rss_bytes << ','
           << "\"open_file_descriptors\":" << session.resources.open_file_descriptors
           << ',' << "\"valid\":" << (session.valid ? "true" : "false") << ','
           << "\"deterministic\":" << (session.deterministic ? "true" : "false")
           << "}\n";
  }
  output.flush();
  return output.good();
}

void append_check(std::ostream& output, const std::string_view name, const bool value,
                  bool& first) {
  if (!first) {
    output << ',';
  }
  first = false;
  output << '\n' << "    \"" << name << "\": " << (value ? "true" : "false");
}

[[nodiscard]] bool write_machine(const Report& report) {
  if (!ensure_parent(report.options.machine_path)) {
    return false;
  }
  const auto temporary = report.options.machine_path.string() + ".tmp";
  std::ofstream output{temporary, std::ios::trunc};
  if (!output) {
    return false;
  }
  const auto& build = aegis::common::current_build_info();
  std::array<char, 256U> hostname{};
  if (gethostname(hostname.data(), hostname.size() - 1U) != 0) {
    hostname[0U] = '\0';
  }
  const auto rss_growth =
      report.final_resources.rss_bytes >= report.warm_resources.rss_bytes
          ? report.final_resources.rss_bytes - report.warm_resources.rss_bytes
          : 0U;
  const auto fd_growth = report.final_resources.open_file_descriptors >=
                                 report.initial_resources.open_file_descriptors
                             ? report.final_resources.open_file_descriptors -
                                   report.initial_resources.open_file_descriptors
                             : 0U;
  const auto total_elapsed = report.primary_elapsed_ns + report.replay_elapsed_ns;
  const auto accelerated_events = report.primary_events + report.replay_events;
  const auto throughput =
      total_elapsed == 0U
          ? 0U
          : static_cast<std::uint64_t>(
                (static_cast<long double>(accelerated_events) * 1'000'000'000.0L) /
                static_cast<long double>(total_elapsed));
  output << "{\n"
         << "  \"schema_version\": \"1.0\",\n"
         << "  \"kind\": \"paper_soak_load_worker\",\n"
         << "  \"mode\": \"PAPER\",\n"
         << "  \"live_trading_capable\": "
         << (build.live_trading_capable ? "true" : "false") << ",\n"
         << "  \"worker_id\": " << report.options.worker_id << ",\n"
         << "  \"seed\": " << report.options.seed << ",\n"
         << "  \"host\": \"" << hostname.data() << "\",\n"
         << "  \"source_revision\": \"" << build.source_revision << "\",\n"
         << "  \"build_type\": \"" << build.build_type << "\",\n"
         << "  \"primary_events\": " << report.primary_events << ",\n"
         << "  \"replay_events\": " << report.replay_events << ",\n"
         << "  \"realtime_events\": " << report.realtime_events << ",\n"
         << "  \"accelerated_events\": " << accelerated_events << ",\n"
         << "  \"session_count\": " << report.sessions.size() << ",\n"
         << "  \"scenario_coverage_mask\": " << report.scenario_coverage_mask << ",\n"
         << "  \"primary_elapsed_ns\": " << report.primary_elapsed_ns << ",\n"
         << "  \"replay_elapsed_ns\": " << report.replay_elapsed_ns << ",\n"
         << "  \"realtime_elapsed_ns\": " << report.realtime_elapsed_ns << ",\n"
         << "  \"accelerated_wall_elapsed_ns\": "
         << report.accelerated_wall_elapsed_ns << ",\n"
         << "  \"accelerated_cpu_time_ns\": " << report.accelerated_cpu_time_ns << ",\n"
         << "  \"accelerated_cpu_utilization_ppm\": "
         << report.accelerated_cpu_utilization_ppm << ",\n"
         << "  \"accelerated_throughput_events_per_second\": " << throughput << ",\n"
         << "  \"stream_hash\": " << report.stream_hash << ",\n"
         << "  \"replay_hash\": " << report.replay_hash << ",\n"
         << "  \"report_hash\": " << report.report_hash << ",\n"
         << "  \"resources\": {\n"
         << "    \"initial_rss_bytes\": " << report.initial_resources.rss_bytes << ",\n"
         << "    \"warm_rss_bytes\": " << report.warm_resources.rss_bytes << ",\n"
         << "    \"final_rss_bytes\": " << report.final_resources.rss_bytes << ",\n"
         << "    \"rss_growth_after_warmup_bytes\": " << rss_growth << ",\n"
         << "    \"maximum_rss_growth_bytes\": " << kMaximumRssGrowthBytes << ",\n"
         << "    \"initial_open_file_descriptors\": "
         << report.initial_resources.open_file_descriptors << ",\n"
         << "    \"final_open_file_descriptors\": "
         << report.final_resources.open_file_descriptors << ",\n"
         << "    \"file_descriptor_growth\": " << fd_growth << "\n"
         << "  },\n"
         << "  \"latency_ns\": {\n"
         << "    \"sample_count\": " << report.latency.sample_count << ",\n"
         << "    \"p50\": " << report.latency.p50_ns << ",\n"
         << "    \"p95\": " << report.latency.p95_ns << ",\n"
         << "    \"p99\": " << report.latency.p99_ns << ",\n"
         << "    \"p99_9\": " << report.latency.p999_ns << ",\n"
         << "    \"maximum\": " << report.latency.maximum_ns << ",\n"
         << "    \"first_half_p99\": " << report.latency.first_half_p99_ns << ",\n"
         << "    \"second_half_p99\": " << report.latency.second_half_p99_ns << ",\n"
         << "    \"maximum_tail_drift_ppm\": " << kMaximumTailDriftPpm << ",\n"
         << "    \"noise_allowance_ns\": " << kTailNoiseAllowanceNs << "\n"
         << "  },\n"
         << "  \"checks\": {";
  bool first = true;
  append_check(output, "all_events_valid", report.all_events_valid, first);
  append_check(output, "deterministic_replay", report.deterministic_replay, first);
  append_check(output, "memory_stable", report.memory_stable, first);
  append_check(output, "file_descriptors_stable", report.descriptors_stable, first);
  append_check(output, "tail_latency_stable", report.tail_latency_stable, first);
  append_check(output, "realtime_simulation_completed", report.realtime_completed,
               first);
  append_check(output, "paper_mode_only", !build.live_trading_capable, first);
  output << "\n  },\n"
         << "  \"passed\": " << (report.passed ? "true" : "false") << "\n"
         << "}\n";
  output.flush();
  if (!output.good()) {
    return false;
  }
  output.close();
  std::error_code error;
  std::filesystem::rename(temporary, report.options.machine_path, error);
  if (!error) {
    return true;
  }
  std::filesystem::remove(temporary, error);
  return false;
}
// NOLINTEND(modernize-raw-string-literal)

[[nodiscard]] Report run(const Options& options) {
  Report report{};
  report.options = options;
  report.initial_resources = resource_sample();
  const auto session_count = 1U + ((options.events - 1U) / options.session_events);
  report.sessions.reserve(static_cast<std::size_t>(session_count));
  std::vector<std::uint64_t> latency_samples;
  latency_samples.reserve(options.sample_limit);
  const auto sample_interval =
      std::max<std::uint64_t>(1U, options.events / options.sample_limit);
  const auto accelerated_cpu_started = process_cpu_time_ns();
  const auto accelerated_wall_started = std::chrono::steady_clock::now();

  std::uint64_t remaining = options.events;
  for (std::uint64_t session_index = 0U; session_index < session_count;
       ++session_index) {
    const auto event_count = std::min(remaining, options.session_events);
    const auto config = session_config(options, session_index, event_count);
    auto* sample_target =
        latency_samples.size() < options.sample_limit ? &latency_samples : nullptr;
    const auto primary =
        generate_pass(config, sample_target, sample_interval, options.sample_limit);
    if (session_index == 0U) {
      report.warm_resources = resource_sample();
    }
    const auto replay =
        generate_pass(config, nullptr, sample_interval, options.sample_limit);
    const bool deterministic = primary.valid && replay.valid &&
                               primary.stream_hash == replay.stream_hash &&
                               primary.book_hash == replay.book_hash;
    const auto resources = resource_sample();
    report.sessions.push_back({.session_index = session_index,
                               .scenario = config.scenario,
                               .seed = config.seed,
                               .configuration_hash = synthetic::config_hash(config),
                               .event_count = event_count,
                               .primary_stream_hash = primary.stream_hash,
                               .replay_stream_hash = replay.stream_hash,
                               .primary_book_hash = primary.book_hash,
                               .replay_book_hash = replay.book_hash,
                               .elapsed_ns = primary.elapsed_ns,
                               .replay_elapsed_ns = replay.elapsed_ns,
                               .resources = resources,
                               .valid = primary.valid && replay.valid,
                               .deterministic = deterministic});
    report.primary_events += primary.event_count;
    report.replay_events += replay.event_count;
    report.primary_elapsed_ns += primary.elapsed_ns;
    report.replay_elapsed_ns += replay.elapsed_ns;
    report.scenario_coverage_mask |= 1ULL << static_cast<std::uint8_t>(config.scenario);
    mix(report.stream_hash, primary.stream_hash);
    mix(report.stream_hash, primary.book_hash);
    mix(report.replay_hash, replay.stream_hash);
    mix(report.replay_hash, replay.book_hash);
    report.all_events_valid = report.all_events_valid && primary.valid && replay.valid;
    report.deterministic_replay = report.deterministic_replay && deterministic;
    remaining -= event_count;
  }

  const auto accelerated_cpu_completed = process_cpu_time_ns();
  report.accelerated_wall_elapsed_ns = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - accelerated_wall_started)
          .count());
  report.accelerated_cpu_time_ns =
      accelerated_cpu_completed >= accelerated_cpu_started
          ? accelerated_cpu_completed - accelerated_cpu_started
          : 0U;
  report.accelerated_cpu_utilization_ppm =
      report.accelerated_wall_elapsed_ns == 0U
          ? 0U
          : std::min<std::uint64_t>(
                1'000'000U,
                static_cast<std::uint64_t>(
                    (static_cast<long double>(report.accelerated_cpu_time_ns) *
                     1'000'000.0L) /
                    static_cast<long double>(report.accelerated_wall_elapsed_ns)));

  const auto realtime = generate_realtime(options);
  report.realtime_events = realtime.event_count;
  report.realtime_elapsed_ns = realtime.elapsed_ns;
  report.realtime_completed = realtime.valid;
  report.final_resources = resource_sample();
  report.latency = summarize_latency(latency_samples);

  const auto rss_limit =
      report.warm_resources.rss_bytes >
              std::numeric_limits<std::uint64_t>::max() - kMaximumRssGrowthBytes
          ? std::numeric_limits<std::uint64_t>::max()
          : report.warm_resources.rss_bytes + kMaximumRssGrowthBytes;
  report.memory_stable = report.final_resources.rss_bytes <= rss_limit;
  report.descriptors_stable = report.final_resources.open_file_descriptors <=
                              report.initial_resources.open_file_descriptors;
  const auto drift_limit =
      report.latency.first_half_p99_ns +
      std::max(kTailNoiseAllowanceNs,
               (report.latency.first_half_p99_ns * kMaximumTailDriftPpm) / 1'000'000U);
  report.tail_latency_stable = report.latency.sample_count >= 2U &&
                               report.latency.second_half_p99_ns <= drift_limit;
  report.passed = report.primary_events == options.events &&
                  report.replay_events == options.events && report.all_events_valid &&
                  report.deterministic_replay && report.memory_stable &&
                  report.descriptors_stable && report.tail_latency_stable &&
                  report.realtime_completed &&
                  !aegis::common::current_build_info().live_trading_capable;
  mix(report.report_hash, report.stream_hash);
  mix(report.report_hash, report.replay_hash);
  mix(report.report_hash, report.primary_events);
  mix(report.report_hash, report.replay_events);
  mix(report.report_hash, report.realtime_events);
  mix(report.report_hash, report.scenario_coverage_mask);
  mix(report.report_hash, report.passed ? 1U : 0U);
  return report;
}

} // namespace

int main(const int argc, char** argv) {
  Options options{};
  if (!parse_options(argc, argv, options)) {
    return 2;
  }
  const auto report = run(options);
  if (!write_raw(report) || !write_machine(report)) {
    std::cerr << "failed to publish soak load evidence\n";
    return 1;
  }
  std::cout << "paper soak load worker=" << options.worker_id
            << " primary_events=" << report.primary_events
            << " replay_events=" << report.replay_events
            << " realtime_events=" << report.realtime_events
            << " report_hash=" << report.report_hash
            << " passed=" << (report.passed ? "true" : "false") << '\n';
  return report.passed ? 0 : 1;
}
