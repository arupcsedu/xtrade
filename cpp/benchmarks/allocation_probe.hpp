#ifndef AEGIS_BENCHMARKS_ALLOCATION_PROBE_HPP
#define AEGIS_BENCHMARKS_ALLOCATION_PROBE_HPP

#include <cstdint>

namespace allocation_probe {

[[nodiscard]] std::uint64_t count() noexcept;

} // namespace allocation_probe

#endif // AEGIS_BENCHMARKS_ALLOCATION_PROBE_HPP
