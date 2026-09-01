#include "aegis/features/publisher.hpp"

namespace aegis::features {

FeatureError FeatureSnapshotPublisher::publish(
    const FeatureEngine& engine,
    const std::uint64_t process_monotonic_time_ns) noexcept {
  if (publish_ == nullptr) {
    return FeatureError::invalid_configuration;
  }
  FeatureSnapshot snapshot{};
  const auto result = engine.make_snapshot(process_monotonic_time_ns, snapshot);
  if (result != FeatureError::none) {
    return result;
  }
  if (!publish_(context_, snapshot)) {
    ++backpressure_count_;
    return FeatureError::publisher_backpressure;
  }
  ++published_count_;
  return FeatureError::none;
}

std::uint64_t FeatureSnapshotPublisher::published_count() const noexcept {
  return published_count_;
}

std::uint64_t FeatureSnapshotPublisher::backpressure_count() const noexcept {
  return backpressure_count_;
}

} // namespace aegis::features
