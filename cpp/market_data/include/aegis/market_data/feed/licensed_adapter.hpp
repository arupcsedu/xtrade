#ifndef AEGIS_MARKET_DATA_FEED_LICENSED_ADAPTER_HPP
#define AEGIS_MARKET_DATA_FEED_LICENSED_ADAPTER_HPP

#include "aegis/market_data/feed/interfaces.hpp"

#include <cstdint>

namespace aegis::market_data::feed {

// Pure skeletons only. Implementations require licensed specifications,
// provider certification fixtures, and an approved protocol provenance record.
class LicensedKernelReceiver : public KernelReceiver {
public:
  [[nodiscard]] virtual std::uint64_t specification_revision_hash() const noexcept = 0;
};

class LicensedBinaryDecoder : public BinaryDecoder {
public:
  [[nodiscard]] virtual std::uint64_t specification_revision_hash() const noexcept = 0;
};

class LicensedRecoveryProvider : public RecoveryProvider {
public:
  [[nodiscard]] virtual std::uint64_t specification_revision_hash() const noexcept = 0;
};

} // namespace aegis::market_data::feed

#endif // AEGIS_MARKET_DATA_FEED_LICENSED_ADAPTER_HPP
