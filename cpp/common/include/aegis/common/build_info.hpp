#ifndef AEGIS_COMMON_BUILD_INFO_HPP
#define AEGIS_COMMON_BUILD_INFO_HPP

#include <string_view>

namespace aegis::common {

struct BuildInfo final {
  std::string_view project;
  std::string_view version;
  std::string_view source_revision;
  std::string_view compiler_id;
  std::string_view compiler_version;
  std::string_view build_type;
  bool live_trading_capable;
};

[[nodiscard]] const BuildInfo& current_build_info() noexcept;

} // namespace aegis::common

#endif // AEGIS_COMMON_BUILD_INFO_HPP
