#include "aegis/common/build_info.hpp"

#include <benchmark/benchmark.h>

namespace {

void benchmark_build_info_access(benchmark::State& state) {
  for (auto _ : state) {
    static_cast<void>(_);
    const auto& info = aegis::common::current_build_info();
    benchmark::DoNotOptimize(info.version.data());
    benchmark::ClobberMemory();
  }
}

BENCHMARK(benchmark_build_info_access);

} // namespace
