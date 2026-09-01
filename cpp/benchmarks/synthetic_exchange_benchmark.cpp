#include "aegis/market_data/synthetic/generator.hpp"
#include "aegis/market_data/synthetic/mock_protocol.hpp"

#include <benchmark/benchmark.h>

#include <cstdint>

namespace {

namespace synthetic = aegis::market_data::synthetic;

void benchmark_bounded_generation(benchmark::State& state) {
  auto config = synthetic::make_default_config();
  config.event_count = static_cast<std::uint64_t>(state.range(0));
  std::uint64_t processed = 0U;
  for (auto iteration : state) {
    static_cast<void>(iteration);
    state.PauseTiming();
    auto generator = synthetic::SyntheticExchangeGenerator::create(config);
    state.ResumeTiming();
    if (!generator.has_value()) {
      state.SkipWithError("generator setup failed");
      break;
    }
    for (std::uint64_t index = 0; index < config.event_count; ++index) {
      synthetic::SyntheticEvent event{};
      const auto result = generator->next(event);
      if (!result.ok()) {
        state.SkipWithError("generation failed");
        return;
      }
      benchmark::DoNotOptimize(event.event_hash);
    }
    processed += config.event_count;
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(processed));
  state.SetBytesProcessed(static_cast<std::int64_t>(processed) *
                          static_cast<std::int64_t>(sizeof(synthetic::SyntheticEvent)));
}

void benchmark_mock_packet_encoding(benchmark::State& state) {
  auto config = synthetic::make_default_config();
  config.event_count = 1U;
  auto generator = synthetic::SyntheticExchangeGenerator::create(config);
  if (!generator.has_value()) {
    state.SkipWithError("generator setup failed");
    return;
  }
  synthetic::SyntheticEvent event{};
  if (!generator->next(event).ok()) {
    state.SkipWithError("generation failed");
    return;
  }
  for (auto iteration : state) {
    static_cast<void>(iteration);
    const auto packet = synthetic::encode_packet(event);
    benchmark::DoNotOptimize(packet.data());
  }
  state.SetItemsProcessed(state.iterations());
  state.SetBytesProcessed(state.iterations() *
                          static_cast<std::int64_t>(synthetic::kPacketBytes));
}

BENCHMARK(benchmark_bounded_generation)->Arg(65'536);
BENCHMARK(benchmark_mock_packet_encoding);

} // namespace
