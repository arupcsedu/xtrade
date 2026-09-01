#ifndef AEGIS_FEATURES_PUBLISHER_HPP
#define AEGIS_FEATURES_PUBLISHER_HPP

#include "aegis/features/feature_engine.hpp"

#include <cstdint>

namespace aegis::features {

using FeaturePublishFunction = bool (*)(void*, const FeatureSnapshot&) noexcept;

class FeatureSnapshotPublisher final {
public:
  constexpr FeatureSnapshotPublisher(void* context,
                                     FeaturePublishFunction publish_function) noexcept
      : context_(context), publish_(publish_function) {}

  [[nodiscard]] FeatureError publish(const FeatureEngine& engine,
                                     std::uint64_t process_monotonic_time_ns) noexcept;
  [[nodiscard]] std::uint64_t published_count() const noexcept;
  [[nodiscard]] std::uint64_t backpressure_count() const noexcept;

private:
  void* context_{};
  FeaturePublishFunction publish_{};
  std::uint64_t published_count_{};
  std::uint64_t backpressure_count_{};
};

} // namespace aegis::features

#endif // AEGIS_FEATURES_PUBLISHER_HPP
