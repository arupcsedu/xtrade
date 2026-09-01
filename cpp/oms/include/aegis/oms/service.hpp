#ifndef AEGIS_OMS_SERVICE_HPP
#define AEGIS_OMS_SERVICE_HPP

#include "aegis/common/build_info.hpp"
#include "aegis/event_bus/snapshot_store.hpp"
#include "aegis/oms/journal.hpp"
#include "aegis/oms/types.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace aegis::oms {

using OmsSnapshotStore = event_bus::ImmutableSnapshotStore<OmsSnapshot, 3U>;

class DeterministicOms final {
public:
  DeterministicOms(OmsConfiguration configuration, OmsJournal& journal) noexcept;

  [[nodiscard]] bool initialized() const noexcept;
  [[nodiscard]] bool ready() const noexcept;
  [[nodiscard]] OmsHealth health() const noexcept;
  [[nodiscard]] OmsInvariant invariant() const noexcept;
  [[nodiscard]] static const common::BuildInfo& build_info() noexcept;
  [[nodiscard]] std::uint64_t configuration_hash() const noexcept;
  [[nodiscard]] ApplyResult apply(const OmsInput& input) noexcept;
  [[nodiscard]] bool lookup_order(common::OrderId order_id,
                                  OmsOrderSnapshot& output) const noexcept;
  [[nodiscard]] OmsSnapshot snapshot_for_quiescent_inspection() const noexcept;
  [[nodiscard]] event_bus::SnapshotReadStatus
  acquire_snapshot(std::uint64_t acquired_at_ns,
                   OmsSnapshotStore::ReadHandle& output) noexcept;
  [[nodiscard]] bool verify_invariants() noexcept;
  [[nodiscard]] RecoveryStatus recover_from(const OmsJournal& source,
                                            std::uint64_t recovery_time_ns) noexcept;
  [[nodiscard]] OmsMetrics metrics() const noexcept;
  void shutdown(std::uint64_t process_monotonic_time_ns) noexcept;

private:
  struct OrderSlot {
    OmsOrderSnapshot value;
    OrderState state_before_pending{OrderState::working};
  };

  struct ExecutionSlot {
    common::GlobalEventId execution_id;
    common::OrderId order_id;
    ExternalOrderId external_order_id;
    std::int64_t price_ticks{};
    std::uint64_t quantity_units{};
    std::uint64_t venue_sequence{};
    std::uint8_t source_mask{};
    bool occupied{false};
  };

  struct ReceiptSlot {
    common::GlobalEventId receipt_id;
    std::uint64_t input_hash{};
    bool occupied{false};
  };

  struct InternalResult {
    ApplyStatus status{ApplyStatus::invalid};
    OmsReason reason{OmsReason::none};
    std::size_t order_index{kMaximumOmsOrders};
    OrderState previous_state{OrderState::created};
    bool state_changed{false};
    bool snapshot_changed{false};
    GatewayCommand gateway_command{};
  };

  struct FillLocation {
    std::size_t order_index{kMaximumOmsOrders};
    std::size_t execution_index{kMaximumOmsExecutions};
  };

  [[nodiscard]] bool try_enter() noexcept;
  void leave() noexcept;
  [[nodiscard]] std::size_t find_order(common::OrderId order_id) const noexcept;
  [[nodiscard]] std::size_t
  find_order_by_intent(common::IntentId intent_id) const noexcept;
  [[nodiscard]] std::size_t
  find_order_by_external(ExternalOrderId external_order_id) const noexcept;
  [[nodiscard]] std::size_t find_free_order(common::OrderId order_id) const noexcept;
  [[nodiscard]] std::size_t
  find_execution(common::GlobalEventId execution_id) const noexcept;
  [[nodiscard]] std::size_t
  find_free_execution(common::GlobalEventId execution_id) const noexcept;
  [[nodiscard]] std::size_t
  find_receipt(common::GlobalEventId receipt_id) const noexcept;
  [[nodiscard]] std::size_t
  find_free_receipt(common::GlobalEventId receipt_id) const noexcept;
  [[nodiscard]] bool record_receipt(const OmsInput& input) noexcept;
  [[nodiscard]] bool authority_matches(LeaderAuthority authority) const noexcept;
  [[nodiscard]] std::size_t resolve_order(const OmsInput& input) const noexcept;
  [[nodiscard]] InternalResult apply_locked(const OmsInput& input) noexcept;
  [[nodiscard]] InternalResult apply_accept(const OmsInput& input) noexcept;
  [[nodiscard]] InternalResult apply_mark_ready(const OmsInput& input) noexcept;
  [[nodiscard]] InternalResult apply_dispatch(const OmsInput& input) noexcept;
  [[nodiscard]] InternalResult apply_cancel(const OmsInput& input) noexcept;
  [[nodiscard]] InternalResult apply_replace(const OmsInput& input) noexcept;
  [[nodiscard]] InternalResult apply_acknowledgement(const OmsInput& input) noexcept;
  [[nodiscard]] InternalResult apply_rejection(const OmsInput& input) noexcept;
  [[nodiscard]] InternalResult apply_fill(const OmsInput& input) noexcept;
  [[nodiscard]] InternalResult apply_existing_fill(const OmsInput& input,
                                                   FillLocation location) noexcept;
  [[nodiscard]] InternalResult apply_new_fill(const OmsInput& input,
                                              FillLocation location) noexcept;
  [[nodiscard]] InternalResult apply_cancel_response(const OmsInput& input) noexcept;
  [[nodiscard]] InternalResult apply_replace_response(const OmsInput& input) noexcept;
  [[nodiscard]] InternalResult apply_expire(const OmsInput& input) noexcept;
  [[nodiscard]] InternalResult apply_recovery_begin(const OmsInput& input) noexcept;
  [[nodiscard]] InternalResult
  apply_recovery_observation(const OmsInput& input) noexcept;
  [[nodiscard]] InternalResult apply_authority_update(const OmsInput& input) noexcept;
  [[nodiscard]] InternalResult unknown_order(const OmsInput& input) noexcept;
  [[nodiscard]] bool bind_external_id(std::size_t order_index,
                                      ExternalOrderId external_order_id) noexcept;
  [[nodiscard]] bool transition(std::size_t order_index, const OmsInput& input,
                                OrderState next) noexcept;
  void touch_order(std::size_t order_index, const OmsInput& input) noexcept;
  [[nodiscard]] static GatewayCommand
  make_gateway_command(const OmsInput& input, const OrderSlot& order,
                       GatewayCommandKind kind, std::int64_t price_ticks,
                       std::uint64_t quantity_units) noexcept;
  [[nodiscard]] bool recompute_snapshot(std::uint64_t event_time_ns,
                                        std::uint64_t journal_sequence,
                                        std::uint64_t journal_hash) noexcept;
  [[nodiscard]] bool publish_snapshot(std::uint64_t event_time_ns) noexcept;
  [[nodiscard]] bool verify_invariants_locked() const noexcept;
  void update_health_from_state() noexcept;
  void set_unsafe(OmsInvariant invariant) noexcept;
  [[nodiscard]] static common::GlobalEventId
  recovery_receipt(common::OrderId order_id, std::uint64_t journal_sequence) noexcept;

  OmsConfiguration configuration_;
  OmsJournal& journal_;
  std::array<OrderSlot, kMaximumOmsOrders> orders_{};
  std::array<ExecutionSlot, kMaximumOmsExecutions> executions_{};
  std::array<ReceiptSlot, kOmsIdempotencyCapacity> receipts_{};
  OmsSnapshot snapshot_{};
  OmsSnapshotStore snapshot_store_;
  std::uint64_t active_fencing_token_{};
  std::uint64_t snapshot_sequence_{};
  OmsHealth health_{OmsHealth::starting};
  OmsInvariant invariant_{OmsInvariant::none};
  std::atomic_flag writer_gate_ = ATOMIC_FLAG_INIT;
  std::atomic<bool> initialized_{false};
  std::atomic<bool> stopped_{false};
  std::atomic<std::uint64_t> input_count_{0U};
  std::atomic<std::uint64_t> applied_count_{0U};
  std::atomic<std::uint64_t> duplicate_count_{0U};
  std::atomic<std::uint64_t> forbidden_count_{0U};
  std::atomic<std::uint64_t> out_of_order_count_{0U};
  std::atomic<std::uint64_t> reconciliation_count_{0U};
  std::atomic<std::uint64_t> command_count_{0U};
  std::atomic<std::uint64_t> fenced_count_{0U};
  std::atomic<std::uint64_t> journal_full_count_{0U};
  std::atomic<std::uint64_t> snapshot_publication_count_{0U};
  std::atomic<std::uint64_t> snapshot_publish_failure_count_{0U};
};

} // namespace aegis::oms

#endif // AEGIS_OMS_SERVICE_HPP
