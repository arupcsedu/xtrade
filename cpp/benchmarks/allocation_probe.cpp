#include "allocation_probe.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

namespace {
std::atomic<std::uint64_t> allocation_count{};
}

namespace allocation_probe {

std::uint64_t count() noexcept {
  return allocation_count.load(std::memory_order_relaxed);
}

} // namespace allocation_probe

// This translation unit is an intentionally complete process-wide allocation
// probe for the benchmark executable. It contains no measured book logic.
// NOLINTBEGIN(cppcoreguidelines-owning-memory, misc-const-correctness,
// misc-new-delete-overloads, readability-named-parameter)
void* operator new(const std::size_t size) {
  allocation_count.fetch_add(1U, std::memory_order_relaxed);
  if (void* memory = std::malloc(size); memory != nullptr) {
    return memory;
  }
  throw std::bad_alloc{};
}

void* operator new[](const std::size_t size) { return ::operator new(size); }

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

void* operator new(const std::size_t size, const std::align_val_t alignment) {
  allocation_count.fetch_add(1U, std::memory_order_relaxed);
  const auto boundary = static_cast<std::size_t>(alignment);
  const auto rounded = ((size + boundary - 1U) / boundary) * boundary;
  if (void* memory = std::aligned_alloc(boundary, rounded); memory != nullptr) {
    return memory;
  }
  throw std::bad_alloc{};
}

void* operator new[](const std::size_t size, const std::align_val_t alignment) {
  return ::operator new(size, alignment);
}

void operator delete(void* memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept {
  std::free(memory);
}
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept {
  std::free(memory);
}
// NOLINTEND(cppcoreguidelines-owning-memory, misc-const-correctness,
// misc-new-delete-overloads, readability-named-parameter)
