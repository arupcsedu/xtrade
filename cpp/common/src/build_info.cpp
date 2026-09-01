#include "aegis/common/build_info.hpp"

#include "aegis/common/build_info_generated.hpp"

namespace aegis::common {

const BuildInfo& current_build_info() noexcept {
  static constexpr BuildInfo info{
      .project = generated::kProject,
      .version = generated::kVersion,
      .source_revision = generated::kSourceRevision,
      .compiler_id = generated::kCompilerId,
      .compiler_version = generated::kCompilerVersion,
      .build_type = generated::kBuildType,
      .live_trading_capable = generated::kLiveTradingCapable,
  };
  return info;
}

} // namespace aegis::common
