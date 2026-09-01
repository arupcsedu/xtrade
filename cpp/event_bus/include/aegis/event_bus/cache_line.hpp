#ifndef AEGIS_EVENT_BUS_CACHE_LINE_HPP
#define AEGIS_EVENT_BUS_CACHE_LINE_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace aegis::event_bus {

// This value is part of the colocated ABI. It is deliberately not derived from
// std::hardware_destructive_interference_size, whose value may vary by compiler.
inline constexpr std::size_t kCacheLineBytes = 64U;

// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct alignas(kCacheLineBytes) PaddedAtomicU64 {
  std::atomic<std::uint64_t> value{0U};
  std::array<std::byte, kCacheLineBytes - sizeof(std::atomic<std::uint64_t>)> padding{};
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic<std::size_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);
static_assert(sizeof(PaddedAtomicU64) == kCacheLineBytes);
static_assert(alignof(PaddedAtomicU64) == kCacheLineBytes);

} // namespace aegis::event_bus

#endif // AEGIS_EVENT_BUS_CACHE_LINE_HPP
