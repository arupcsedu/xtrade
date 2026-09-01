#include "aegis/oms/types.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

namespace {

[[nodiscard]] std::uint64_t read_word(std::span<const std::uint8_t> data,
                                      std::size_t& cursor) noexcept {
  std::uint64_t value{};
  for (std::size_t index = 0U; index < sizeof(value) && cursor < data.size();
       ++index, ++cursor) {
    value |= static_cast<std::uint64_t>(data[cursor]) << (index * 8U);
  }
  return value;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* bytes,
                                      const std::size_t size) {
  if (bytes == nullptr || size == 0U) {
    return 0;
  }
  const std::span<const std::uint8_t> data{bytes, size};
  std::size_t cursor{};
  const auto from =
      static_cast<aegis::oms::OrderState>((read_word(data, cursor) % 16U));
  const auto input =
      static_cast<aegis::oms::InputKind>((read_word(data, cursor) % 20U));
  const auto to = static_cast<aegis::oms::OrderState>((read_word(data, cursor) % 16U));
  const bool allowed = aegis::oms::transition_allowed(from, input, to);
  if (allowed &&
      (!aegis::oms::valid_order_state(from) || !aegis::oms::valid_order_state(to))) {
    std::abort();
  }
  if (allowed && aegis::oms::terminal_state(from) &&
      input != aegis::oms::InputKind::fill &&
      input != aegis::oms::InputKind::cancel_acknowledgement) {
    std::abort();
  }

  aegis::oms::OmsInput event{};
  event.receipt_id = {read_word(data, cursor), read_word(data, cursor)};
  event.kind = input;
  event.source = static_cast<aegis::oms::EventSource>(read_word(data, cursor) % 8U);
  event.order_id = {read_word(data, cursor), read_word(data, cursor)};
  event.external_order_id = {read_word(data, cursor), read_word(data, cursor)};
  event.execution_id = {read_word(data, cursor), read_word(data, cursor)};
  event.authority = {read_word(data, cursor), read_word(data, cursor)};
  event.price_ticks = static_cast<std::int64_t>(read_word(data, cursor));
  event.quantity_units = read_word(data, cursor);
  event.venue_sequence = read_word(data, cursor);
  event.exchange_event_time_ns = static_cast<std::int64_t>(read_word(data, cursor));
  event.nic_receive_time_ns = static_cast<std::int64_t>(read_word(data, cursor));
  event.process_monotonic_time_ns = read_word(data, cursor);
  event.stable_hash = (size & 1U) == 0U ? aegis::oms::stable_input_hash(event)
                                        : read_word(data, cursor);
  (void)aegis::oms::valid_input(event);
  return 0;
}
