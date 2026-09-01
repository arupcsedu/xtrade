#ifndef AEGIS_TIME_MONOTONICITY_HPP
#define AEGIS_TIME_MONOTONICITY_HPP

#include <optional>

namespace aegis::time {

enum class MonotonicityStatus : unsigned char {
  first = 0,
  advanced,
  equal,
  regressed,
};

// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct MonotonicityResult {
  MonotonicityStatus status{MonotonicityStatus::first};
  bool accepted{true};
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

template <typename Timestamp> class MonotonicityValidator {
public:
  constexpr explicit MonotonicityValidator(const bool allow_equal = false) noexcept
      : allow_equal_(allow_equal) {}

  [[nodiscard]] constexpr MonotonicityResult observe(const Timestamp value) noexcept {
    if (!last_.has_value()) {
      last_ = value;
      return {};
    }
    if (value < *last_) {
      return {.status = MonotonicityStatus::regressed, .accepted = false};
    }
    if (value == *last_) {
      return {.status = MonotonicityStatus::equal, .accepted = allow_equal_};
    }
    last_ = value;
    return {.status = MonotonicityStatus::advanced, .accepted = true};
  }

  constexpr void reset() noexcept { last_.reset(); }

  [[nodiscard]] constexpr std::optional<Timestamp> last() const noexcept {
    return last_;
  }

private:
  std::optional<Timestamp> last_;
  bool allow_equal_{false};
};

} // namespace aegis::time

#endif // AEGIS_TIME_MONOTONICITY_HPP
