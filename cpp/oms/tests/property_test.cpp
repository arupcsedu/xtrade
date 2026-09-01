#include "aegis/oms/service.hpp"

#include "reference_model.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>

namespace aegis::oms::test {
namespace {

struct AllowedTransition {
  OrderState from;
  InputKind input;
  OrderState to;
};

// Positional triples keep the independently audited transition matrix compact.
// NOLINTBEGIN(modernize-use-designated-initializers)
constexpr auto kAllowed = std::to_array<AllowedTransition>({
    {OrderState::created, InputKind::mark_ready, OrderState::ready},
    {OrderState::ready, InputKind::dispatch, OrderState::pending_ack},
    {OrderState::pending_ack, InputKind::cancel_intent, OrderState::pending_cancel},
    {OrderState::working, InputKind::cancel_intent, OrderState::pending_cancel},
    {OrderState::partially_filled, InputKind::cancel_intent,
     OrderState::pending_cancel},
    {OrderState::pending_replace, InputKind::cancel_intent, OrderState::pending_cancel},
    {OrderState::working, InputKind::replace_intent, OrderState::pending_replace},
    {OrderState::partially_filled, InputKind::replace_intent,
     OrderState::pending_replace},
    {OrderState::pending_ack, InputKind::acknowledgement, OrderState::working},
    {OrderState::partially_filled, InputKind::acknowledgement,
     OrderState::partially_filled},
    {OrderState::pending_cancel, InputKind::acknowledgement,
     OrderState::pending_cancel},
    {OrderState::pending_replace, InputKind::acknowledgement,
     OrderState::pending_replace},
    {OrderState::pending_ack, InputKind::rejection, OrderState::rejected},
    {OrderState::pending_ack, InputKind::fill, OrderState::partially_filled},
    {OrderState::pending_ack, InputKind::fill, OrderState::filled},
    {OrderState::working, InputKind::fill, OrderState::partially_filled},
    {OrderState::working, InputKind::fill, OrderState::filled},
    {OrderState::partially_filled, InputKind::fill, OrderState::partially_filled},
    {OrderState::partially_filled, InputKind::fill, OrderState::filled},
    {OrderState::pending_cancel, InputKind::fill, OrderState::pending_cancel},
    {OrderState::pending_cancel, InputKind::fill, OrderState::filled},
    {OrderState::pending_replace, InputKind::fill, OrderState::pending_replace},
    {OrderState::pending_replace, InputKind::fill, OrderState::filled},
    {OrderState::canceled, InputKind::fill, OrderState::canceled},
    {OrderState::canceled, InputKind::fill, OrderState::filled},
    {OrderState::unknown_recovery, InputKind::fill, OrderState::unknown_recovery},
    {OrderState::pending_ack, InputKind::cancel_acknowledgement, OrderState::canceled},
    {OrderState::working, InputKind::cancel_acknowledgement, OrderState::canceled},
    {OrderState::partially_filled, InputKind::cancel_acknowledgement,
     OrderState::canceled},
    {OrderState::pending_cancel, InputKind::cancel_acknowledgement,
     OrderState::canceled},
    {OrderState::pending_replace, InputKind::cancel_acknowledgement,
     OrderState::canceled},
    {OrderState::filled, InputKind::cancel_acknowledgement, OrderState::filled},
    {OrderState::pending_cancel, InputKind::cancel_rejection, OrderState::working},
    {OrderState::pending_cancel, InputKind::cancel_rejection,
     OrderState::partially_filled},
    {OrderState::pending_replace, InputKind::replace_acknowledgement,
     OrderState::working},
    {OrderState::pending_replace, InputKind::replace_acknowledgement,
     OrderState::partially_filled},
    {OrderState::pending_replace, InputKind::replace_acknowledgement,
     OrderState::filled},
    {OrderState::pending_replace, InputKind::replace_rejection, OrderState::working},
    {OrderState::pending_replace, InputKind::replace_rejection,
     OrderState::partially_filled},
    {OrderState::created, InputKind::expire, OrderState::expired},
    {OrderState::ready, InputKind::expire, OrderState::expired},
    {OrderState::pending_ack, InputKind::expire, OrderState::expired},
    {OrderState::working, InputKind::expire, OrderState::expired},
    {OrderState::partially_filled, InputKind::expire, OrderState::expired},
    {OrderState::created, InputKind::recovery_begin, OrderState::unknown_recovery},
    {OrderState::ready, InputKind::recovery_begin, OrderState::unknown_recovery},
    {OrderState::pending_ack, InputKind::recovery_begin, OrderState::unknown_recovery},
    {OrderState::working, InputKind::recovery_begin, OrderState::unknown_recovery},
    {OrderState::partially_filled, InputKind::recovery_begin,
     OrderState::unknown_recovery},
    {OrderState::pending_cancel, InputKind::recovery_begin,
     OrderState::unknown_recovery},
    {OrderState::pending_replace, InputKind::recovery_begin,
     OrderState::unknown_recovery},
    {OrderState::unknown_recovery, InputKind::recovery_begin,
     OrderState::unknown_recovery},
    {OrderState::unknown_recovery, InputKind::recovery_observation,
     OrderState::working},
    {OrderState::unknown_recovery, InputKind::recovery_observation,
     OrderState::partially_filled},
    {OrderState::unknown_recovery, InputKind::recovery_observation, OrderState::filled},
    {OrderState::unknown_recovery, InputKind::recovery_observation,
     OrderState::canceled},
    {OrderState::unknown_recovery, InputKind::recovery_observation,
     OrderState::rejected},
    {OrderState::unknown_recovery, InputKind::recovery_observation,
     OrderState::expired},
    {OrderState::unknown_recovery, InputKind::recovery_observation,
     OrderState::unknown_recovery},
});
// NOLINTEND(modernize-use-designated-initializers)

[[nodiscard]] constexpr bool expected_allowed(const OrderState from,
                                              const InputKind input,
                                              const OrderState to) noexcept {
  return std::ranges::any_of(kAllowed, [from, input, to](const auto& value) {
    return value.from == from && value.input == input && value.to == to;
  });
}

void compare_order(const DeterministicOms& service, const ReferenceOmsModel& reference,
                   const common::OrderId order_id) {
  OmsOrderSnapshot actual{};
  ASSERT_TRUE(service.lookup_order(order_id, actual));
  const auto& expected = reference.order(order_id);
  EXPECT_EQ(actual.state, expected.state);
  EXPECT_EQ(actual.quantity_units, expected.quantity_units);
  EXPECT_EQ(actual.cumulative_fill_quantity_units,
            expected.cumulative_fill_quantity_units);
  EXPECT_EQ(actual.remaining_quantity_units, expected.remaining_quantity_units);
}

TEST(OmsPropertyTest, TransitionTableForbidsEveryUnlistedTriple) {
  for (std::uint8_t raw_from = 1U; raw_from <= 12U; ++raw_from) {
    for (std::uint8_t raw_input = 1U; raw_input <= 16U; ++raw_input) {
      for (std::uint8_t raw_to = 1U; raw_to <= 12U; ++raw_to) {
        const auto from = static_cast<OrderState>(raw_from);
        const auto input = static_cast<InputKind>(raw_input);
        const auto to = static_cast<OrderState>(raw_to);
        EXPECT_EQ(transition_allowed(from, input, to),
                  expected_allowed(from, input, to))
            << "from=" << static_cast<unsigned>(raw_from)
            << " input=" << static_cast<unsigned>(raw_input)
            << " to=" << static_cast<unsigned>(raw_to);
      }
    }
  }
}

TEST(OmsPropertyTest, ProductionMatchesIndependentReferenceAcrossSeededRaces) {
  constexpr std::uint64_t kSeed = 20'260'828U;
  std::uint64_t state = kSeed;
  auto next = [&state]() noexcept {
    state ^= state << 13U;
    state ^= state >> 7U;
    state ^= state << 17U;
    return state;
  };

  auto journal = std::make_unique<OmsJournal>();
  auto service = std::make_unique<DeterministicOms>(configuration(), *journal);
  ReferenceOmsModel reference;
  for (std::uint64_t ordinal = 1U; ordinal <= 48U; ++ordinal) {
    const auto base = ordinal * 100U;
    const auto created = service->apply(accept(approval(ordinal), base + 1U));
    ASSERT_EQ(created.status, ApplyStatus::applied);
    const auto order_id = created.order_id;
    reference.create(order_id, 10U);
    (void)service->apply(internal(InputKind::mark_ready, order_id, base + 2U));
    reference.ready(order_id);
    (void)service->apply(internal(InputKind::dispatch, order_id, base + 3U));
    reference.dispatch(order_id);

    const ExternalOrderId external{.high = 300U, .low = ordinal};
    const auto first_quantity = 1U + (next() % 4U);
    if ((next() & 1U) == 0U) {
      auto first_fill = fill(order_id, base + 4U, 20U, base + 1U, first_quantity);
      first_fill.external_order_id = external;
      first_fill.stable_hash = stable_input_hash(first_fill);
      (void)service->apply(first_fill);
      reference.fill(order_id, first_quantity);
      (void)service->apply(
          venue(InputKind::acknowledgement, order_id, base + 5U, 10U, external));
      reference.acknowledge(order_id);
    } else {
      (void)service->apply(
          venue(InputKind::acknowledgement, order_id, base + 4U, 10U, external));
      reference.acknowledge(order_id);
      auto first_fill = fill(order_id, base + 5U, 20U, base + 1U, first_quantity);
      first_fill.external_order_id = external;
      first_fill.stable_hash = stable_input_hash(first_fill);
      (void)service->apply(first_fill);
      reference.fill(order_id, first_quantity);
    }
    compare_order(*service, reference, order_id);

    if ((next() & 1U) == 0U) {
      (void)service->apply(cancel(order_id, base + 6U));
      reference.request_cancel(order_id);
      const auto residual = 10U - first_quantity;
      if ((next() & 1U) != 0U && residual > 1U) {
        auto race = fill(order_id, base + 7U, 21U, base + 2U, 1U);
        race.external_order_id = external;
        race.stable_hash = stable_input_hash(race);
        (void)service->apply(race);
        reference.fill(order_id, 1U);
      }
      (void)service->apply(
          venue(InputKind::cancel_acknowledgement, order_id, base + 8U, 22U, external));
      reference.acknowledge_cancel(order_id);
    } else {
      const auto residual = 10U - first_quantity;
      auto final_fill = fill(order_id, base + 6U, 21U, base + 2U, residual);
      final_fill.external_order_id = external;
      final_fill.stable_hash = stable_input_hash(final_fill);
      (void)service->apply(final_fill);
      reference.fill(order_id, residual);
    }
    compare_order(*service, reference, order_id);
  }
  EXPECT_TRUE(service->verify_invariants());
  EXPECT_EQ(service->snapshot_for_quiescent_inspection().order_count, 48U);
}

} // namespace
} // namespace aegis::oms::test
