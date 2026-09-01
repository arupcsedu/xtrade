#ifndef AEGIS_OMS_REFERENCE_MODEL_HPP
#define AEGIS_OMS_REFERENCE_MODEL_HPP

#include "aegis/oms/types.hpp"

#include <cstdint>
#include <map>
#include <utility>

namespace aegis::oms::test {

struct ReferenceOrder {
  OrderState state{OrderState::created};
  std::uint64_t quantity_units{};
  std::uint64_t cumulative_fill_quantity_units{};
  std::uint64_t remaining_quantity_units{};
};

class ReferenceOmsModel final {
public:
  void create(const common::OrderId order_id, const std::uint64_t quantity_units) {
    orders_[key(order_id)] = {.state = OrderState::created,
                              .quantity_units = quantity_units,
                              .remaining_quantity_units = quantity_units};
  }

  void ready(const common::OrderId order_id) {
    orders_.at(key(order_id)).state = OrderState::ready;
  }

  void dispatch(const common::OrderId order_id) {
    orders_.at(key(order_id)).state = OrderState::pending_ack;
  }

  void acknowledge(const common::OrderId order_id) {
    auto& order = orders_.at(key(order_id));
    if (order.state == OrderState::pending_ack) {
      order.state = OrderState::working;
    }
  }

  void fill(const common::OrderId order_id, const std::uint64_t quantity_units) {
    auto& order = orders_.at(key(order_id));
    order.cumulative_fill_quantity_units += quantity_units;
    order.remaining_quantity_units =
        order.quantity_units - order.cumulative_fill_quantity_units;
    if (order.remaining_quantity_units == 0U) {
      order.state = OrderState::filled;
    } else if (order.state != OrderState::pending_cancel) {
      order.state = OrderState::partially_filled;
    }
  }

  void request_cancel(const common::OrderId order_id) {
    orders_.at(key(order_id)).state = OrderState::pending_cancel;
  }

  void acknowledge_cancel(const common::OrderId order_id) {
    auto& order = orders_.at(key(order_id));
    if (order.state != OrderState::filled) {
      order.state = OrderState::canceled;
      order.remaining_quantity_units = 0U;
    }
  }

  [[nodiscard]] const ReferenceOrder& order(const common::OrderId order_id) const {
    return orders_.at(key(order_id));
  }

private:
  [[nodiscard]] static std::pair<std::uint64_t, std::uint64_t>
  key(const common::OrderId order_id) noexcept {
    return {order_id.high(), order_id.low()};
  }

  std::map<std::pair<std::uint64_t, std::uint64_t>, ReferenceOrder> orders_;
};

} // namespace aegis::oms::test

#endif // AEGIS_OMS_REFERENCE_MODEL_HPP
