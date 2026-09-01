#ifndef AEGIS_EVENT_BUS_SYMBOL_STATE_HPP
#define AEGIS_EVENT_BUS_SYMBOL_STATE_HPP

#include "aegis/common/identifiers.hpp"
#include "aegis/event_bus/snapshot_store.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace aegis::event_bus {

enum class SymbolStateStatus : std::uint8_t {
  registered = 1,
  duplicate_symbol = 2,
  capacity_exhausted = 3,
  unknown_symbol = 4,
  invalid_identifier = 5,
  invalid_checksum = 6,
};

template <typename Value, std::size_t MaximumSymbols, std::size_t SnapshotSlots = 3U>
class RcuSymbolState final {
public:
  static_assert(std::is_trivially_copyable_v<Value>);
  using Store = ImmutableSnapshotStore<Value, SnapshotSlots>;
  using ReadHandle = typename Store::ReadHandle;

  [[nodiscard]] SymbolStateStatus
  register_symbol(const common::InstrumentId instrument_id,
                  const SnapshotChecksumFunction checksum) noexcept {
    if (!instrument_id.valid()) {
      return SymbolStateStatus::invalid_identifier;
    }
    if (checksum == nullptr) {
      return SymbolStateStatus::invalid_checksum;
    }
    for (std::size_t index = 0U; index < size_; ++index) {
      if (entries_[index].instrument_id == instrument_id) {
        return SymbolStateStatus::duplicate_symbol;
      }
    }
    if (size_ == MaximumSymbols) {
      return SymbolStateStatus::capacity_exhausted;
    }
    auto& entry = entries_[size_];
    entry.instrument_id = instrument_id;
    if (!entry.store.initialize(checksum)) {
      return SymbolStateStatus::invalid_checksum;
    }
    ++size_;
    return SymbolStateStatus::registered;
  }

  [[nodiscard]] SnapshotPublishStatus
  publish(const common::InstrumentId instrument_id, const Value& value,
          const std::uint64_t published_at_ns) noexcept {
    auto* entry = find(instrument_id);
    return entry == nullptr ? SnapshotPublishStatus::uninitialized
                            : entry->store.publish(value, published_at_ns);
  }

  [[nodiscard]] SnapshotReadStatus acquire(const common::InstrumentId instrument_id,
                                           const std::uint64_t acquired_at_ns,
                                           ReadHandle& output) noexcept {
    auto* entry = find(instrument_id);
    return entry == nullptr ? SnapshotReadStatus::uninitialized
                            : entry->store.acquire(acquired_at_ns, output);
  }

  [[nodiscard]] const Store*
  store(const common::InstrumentId instrument_id) const noexcept {
    for (std::size_t index = 0U; index < size_; ++index) {
      if (entries_[index].instrument_id == instrument_id) {
        return &entries_[index].store;
      }
    }
    return nullptr;
  }

  [[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
  // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
  struct Entry {
    common::InstrumentId instrument_id;
    Store store;
  };
  // NOLINTEND(misc-non-private-member-variables-in-classes)

  [[nodiscard]] Entry* find(const common::InstrumentId instrument_id) noexcept {
    for (std::size_t index = 0U; index < size_; ++index) {
      if (entries_[index].instrument_id == instrument_id) {
        return &entries_[index];
      }
    }
    return nullptr;
  }

  std::array<Entry, MaximumSymbols> entries_{};
  std::size_t size_{};
};

} // namespace aegis::event_bus

#endif // AEGIS_EVENT_BUS_SYMBOL_STATE_HPP
