#include "aegis/common/audit_envelope.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      const std::size_t size) {
  static_cast<void>(aegis::common::validate_size_prefixed_audit_envelope(
      std::span<const std::uint8_t>{data, size}));
  return 0;
}
