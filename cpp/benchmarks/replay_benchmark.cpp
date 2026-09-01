#include "aegis/replay/replay.hpp"

#include "aegis/market_data/synthetic/artifacts.hpp"
#include "aegis/market_data/synthetic/config.hpp"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string_view>

#include <cstdlib>

namespace replay = aegis::replay;
namespace synthetic = aegis::market_data::synthetic;

namespace {

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    std::array<char, 64> pattern{};
    constexpr std::string_view prefix = "/tmp/aegis-replay-bench.XXXXXX";
    std::ranges::copy(prefix, pattern.begin());
    if (auto* path = ::mkdtemp(pattern.data()); path != nullptr) {
      path_ = path;
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

void benchmark_maximum_speed_packet_replay(benchmark::State& state) {
  constexpr std::uint64_t kEvents = 4'096U;
  const TemporaryDirectory temporary;
  const synthetic::ArtifactPaths paths{
      .capture = temporary.path() / "events.smxcap",
      .canonical = temporary.path() / "events.amae",
      .expected_book = temporary.path() / "events.book.txt",
  };
  auto generator_config = synthetic::make_default_config();
  generator_config.seed = 0xB3A6'2600U;
  generator_config.event_count = kEvents;
  if (synthetic::generate_artifacts(generator_config, paths, false).error !=
      synthetic::ArtifactError::none) {
    state.SkipWithError("capture generation failed");
    return;
  }
  replay::ReplayConfig config;
  config.source_kind = replay::SourceKind::synthetic_capture;
  config.speed = replay::SpeedMode::maximum;
  config.source = paths.capture;
  config.seed = 0xB3A6'2600U;
  for (auto iteration : state) {
    static_cast<void>(iteration);
    state.PauseTiming();
    replay::EvidenceReplayTarget target;
    replay::VirtualPacer pacer;
    replay::ReplayEngine engine;
    if (engine.prepare(config, target, pacer) != replay::Status::ok) {
      state.SkipWithError("replay prepare failed");
      return;
    }
    state.ResumeTiming();
    if (engine.run() != replay::Status::complete) {
      state.SkipWithError("replay failed");
      return;
    }
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kEvents));
}

BENCHMARK(benchmark_maximum_speed_packet_replay)->Unit(benchmark::kMicrosecond);

} // namespace
