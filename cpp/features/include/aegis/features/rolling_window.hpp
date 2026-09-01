#ifndef AEGIS_FEATURES_ROLLING_WINDOW_HPP
#define AEGIS_FEATURES_ROLLING_WINDOW_HPP

#include <array>
#include <cstddef>
#include <type_traits>

namespace aegis::features {

template <typename T, std::size_t MaximumCapacity> class RollingWindow final {
public:
  static_assert(MaximumCapacity > 0U);

  explicit constexpr RollingWindow(
      const std::size_t capacity = MaximumCapacity) noexcept
      : capacity_(capacity <= MaximumCapacity ? capacity : 0U) {}

  [[nodiscard]] constexpr bool valid() const noexcept { return capacity_ != 0U; }
  [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0U; }
  [[nodiscard]] constexpr bool full() const noexcept { return size_ == capacity_; }
  [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }
  [[nodiscard]] constexpr std::size_t capacity() const noexcept { return capacity_; }

  constexpr void clear() noexcept {
    head_ = 0U;
    size_ = 0U;
  }

  [[nodiscard]] constexpr const T* front() const noexcept {
    return empty() ? nullptr : &storage_[head_];
  }

  [[nodiscard]] constexpr const T* back() const noexcept {
    if (empty()) {
      return nullptr;
    }
    const auto index = (head_ + size_ - 1U) % capacity_;
    return &storage_[index];
  }

  // Returns false when the runtime capacity is invalid. If the ring is full,
  // the oldest item is copied to evicted before it is replaced.
  [[nodiscard]] constexpr bool push(const T& value, T* evicted = nullptr) noexcept {
    if (!valid()) {
      return false;
    }
    if (full()) {
      if (evicted != nullptr) {
        *evicted = storage_[head_];
      }
      storage_[head_] = value;
      head_ = (head_ + 1U) % capacity_;
      return true;
    }
    storage_[(head_ + size_) % capacity_] = value;
    ++size_;
    return true;
  }

  [[nodiscard]] constexpr bool pop_front(T* removed = nullptr) noexcept {
    if (empty()) {
      return false;
    }
    if (removed != nullptr) {
      *removed = storage_[head_];
    }
    head_ = (head_ + 1U) % capacity_;
    --size_;
    return true;
  }

  [[nodiscard]] constexpr const T* at(const std::size_t index) const noexcept {
    if (index >= size_) {
      return nullptr;
    }
    return &storage_[(head_ + index) % capacity_];
  }

private:
  std::array<T, MaximumCapacity> storage_{};
  std::size_t capacity_{};
  std::size_t head_{};
  std::size_t size_{};
};

static_assert(std::is_trivially_copyable_v<RollingWindow<int, 8U>>);

} // namespace aegis::features

#endif // AEGIS_FEATURES_ROLLING_WINDOW_HPP
