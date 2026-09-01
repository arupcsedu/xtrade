#ifndef AEGIS_MARKET_DATA_FEED_BOUNDED_QUEUE_HPP
#define AEGIS_MARKET_DATA_FEED_BOUNDED_QUEUE_HPP

#include <array>
#include <cstddef>

namespace aegis::market_data::feed {

// Single-owner fixed ring. Concurrency belongs at a receiver/publisher adapter
// boundary and must not be inferred for this container.
template <typename Value, std::size_t MaximumCapacity> class BoundedQueue {
public:
  constexpr explicit BoundedQueue(const std::size_t capacity) noexcept
      : capacity_(capacity <= MaximumCapacity ? capacity : 0U) {}

  [[nodiscard]] constexpr bool valid() const noexcept { return capacity_ != 0U; }

  [[nodiscard]] bool push(const Value& value) noexcept {
    if (size_ == capacity_ || capacity_ == 0U) {
      return false;
    }
    values_[tail_] = value;
    tail_ = (tail_ + 1U) % capacity_;
    ++size_;
    return true;
  }

  [[nodiscard]] bool pop(Value& output) noexcept {
    if (size_ == 0U) {
      return false;
    }
    output = values_[head_];
    head_ = (head_ + 1U) % capacity_;
    --size_;
    return true;
  }

  void clear() noexcept {
    head_ = 0U;
    tail_ = 0U;
    size_ = 0U;
  }

  [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }
  [[nodiscard]] constexpr std::size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] constexpr std::size_t available() const noexcept {
    return capacity_ - size_;
  }

private:
  std::array<Value, MaximumCapacity> values_{};
  std::size_t capacity_{};
  std::size_t head_{};
  std::size_t tail_{};
  std::size_t size_{};
};

} // namespace aegis::market_data::feed

#endif // AEGIS_MARKET_DATA_FEED_BOUNDED_QUEUE_HPP
