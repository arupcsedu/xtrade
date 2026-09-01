#ifndef AEGIS_RISK_PORTFOLIO_SERVICE_HPP
#define AEGIS_RISK_PORTFOLIO_SERVICE_HPP

#include "aegis/common/build_info.hpp"
#include "aegis/event_bus/snapshot_store.hpp"
#include "aegis/risk/portfolio_journal.hpp"
#include "aegis/risk/portfolio_types.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace aegis::risk::portfolio {

using PortfolioSnapshotStore = event_bus::ImmutableSnapshotStore<PortfolioSnapshot, 3U>;

enum class RecoveryStatus : std::uint8_t {
  recovered = 1,
  source_corrupt = 2,
  replay_diverged = 3,
  target_journal_full = 4,
  busy = 5,
};

class PortfolioRiskService final {
public:
  PortfolioRiskService(PortfolioConfiguration configuration,
                       PortfolioJournal& journal) noexcept;

  [[nodiscard]] bool initialized() const noexcept;
  [[nodiscard]] bool ready() const noexcept;
  [[nodiscard]] PortfolioHealth health() const noexcept;
  [[nodiscard]] static const common::BuildInfo& build_info() noexcept;
  [[nodiscard]] std::uint64_t configuration_hash() const noexcept;
  [[nodiscard]] ApplyResult apply(const PortfolioEvent& event) noexcept;
  [[nodiscard]] PortfolioSnapshot snapshot_for_quiescent_inspection() const noexcept;
  [[nodiscard]] event_bus::SnapshotReadStatus
  acquire_snapshot(std::uint64_t acquired_at_ns,
                   PortfolioSnapshotStore::ReadHandle& output) noexcept;
  [[nodiscard]] StressResult
  evaluate_stress(StressConfiguration configuration) noexcept;
  [[nodiscard]] bool verify_invariants() noexcept;
  [[nodiscard]] RecoveryStatus recover_from(const PortfolioJournal& source) noexcept;
  [[nodiscard]] PortfolioMetrics metrics() const noexcept;
  void shutdown(std::uint64_t process_monotonic_time_ns) noexcept;

private:
  struct PositionSlot {
    common::AccountId account_id;
    common::StrategyId strategy_id;
    common::InstrumentId instrument_id;
    std::int64_t quantity_units{};
    std::int64_t open_cost_currency_nanos{};
    std::int64_t realized_pnl_currency_nanos{};
    std::int64_t base_quantity_units{};
    std::int64_t base_open_cost_currency_nanos{};
    std::int64_t base_realized_pnl_currency_nanos{};
    std::uint64_t action_generation{};
    bool occupied{false};
  };

  struct OrderSlot {
    common::OrderId order_id;
    common::AccountId account_id;
    common::StrategyId strategy_id;
    common::VenueId venue_id;
    common::InstrumentId instrument_id;
    Side side{Side::buy};
    OrderLifecycleState state{OrderLifecycleState::accepted};
    std::int64_t price_ticks{};
    std::uint64_t quantity_units{};
    std::uint64_t cumulative_fill_quantity_units{};
    std::uint64_t remaining_quantity_units{};
    std::uint64_t last_update_time_ns{};
    bool occupied{false};
  };

  struct FillSlot {
    common::GlobalEventId logical_fill_id;
    common::OrderId order_id;
    common::AccountId account_id;
    common::StrategyId strategy_id;
    common::VenueId venue_id;
    common::InstrumentId instrument_id;
    Side side{Side::buy};
    std::int64_t price_ticks{};
    std::uint64_t quantity_units{};
    std::int64_t fee_currency_nanos{};
    std::uint64_t application_ordinal{};
    std::uint64_t action_generation{};
    std::uint8_t source_mask{};
    bool active{false};
    bool occupied{false};
  };

  struct IdempotencySlot {
    common::GlobalEventId event_id;
    bool occupied{false};
  };

  struct InternalApplyResult {
    ApplyStatus status{ApplyStatus::invalid};
    InvariantCode invariant{InvariantCode::none};
    bool state_changed{false};
  };

  struct SnapshotBuildContext {
    std::uint64_t event_time_ns{};
    std::uint64_t journal_sequence{};
  };

  struct FillAccountingValues {
    std::int64_t notional{};
    std::int64_t signed_quantity{};
    std::uint64_t tick_value_currency_nanos{};
  };

  [[nodiscard]] bool try_enter() noexcept;
  void leave() noexcept;
  [[nodiscard]] std::size_t find_account(common::AccountId value) const noexcept;
  [[nodiscard]] std::size_t find_strategy(common::StrategyId value) const noexcept;
  [[nodiscard]] std::size_t find_instrument(common::InstrumentId value) const noexcept;
  [[nodiscard]] std::size_t find_order(common::OrderId value) const noexcept;
  [[nodiscard]] std::size_t find_fill(common::GlobalEventId value) const noexcept;
  [[nodiscard]] std::size_t
  find_position(common::AccountId account, common::StrategyId strategy,
                common::InstrumentId instrument) const noexcept;
  [[nodiscard]] std::size_t
  find_or_create_position(common::AccountId account, common::StrategyId strategy,
                          common::InstrumentId instrument) noexcept;
  [[nodiscard]] bool seen_event(common::GlobalEventId value) const noexcept;
  [[nodiscard]] bool record_event(common::GlobalEventId value) noexcept;
  [[nodiscard]] InternalApplyResult apply_locked(const PortfolioEvent& event) noexcept;
  [[nodiscard]] InternalApplyResult apply_order(const PortfolioEvent& event) noexcept;
  [[nodiscard]] InternalApplyResult apply_fill(const PortfolioEvent& event) noexcept;
  [[nodiscard]] InternalApplyResult
  apply_fill_correction(const PortfolioEvent& event) noexcept;
  [[nodiscard]] InternalApplyResult
  apply_fill_bust(const PortfolioEvent& event) noexcept;
  [[nodiscard]] InternalApplyResult apply_mark(const PortfolioEvent& event) noexcept;
  [[nodiscard]] InternalApplyResult
  apply_rollover(const PortfolioEvent& event) noexcept;
  [[nodiscard]] InternalApplyResult
  apply_corporate_action(const PortfolioEvent& event) noexcept;
  [[nodiscard]] bool apply_fill_accounting(PositionSlot& position,
                                           const FillSlot& fill) noexcept;
  [[nodiscard]] static bool
  apply_same_direction_fill(PositionSlot& position, const FillSlot& fill,
                            FillAccountingValues values) noexcept;
  [[nodiscard]] static bool apply_opposing_fill(PositionSlot& position,
                                                const FillSlot& fill,
                                                FillAccountingValues values) noexcept;
  [[nodiscard]] bool rebuild_position(std::size_t position_index) noexcept;
  [[nodiscard]] bool refresh_snapshot(std::uint64_t event_time_ns,
                                      std::uint64_t journal_sequence) noexcept;
  [[nodiscard]] bool recompute_snapshot(PortfolioSnapshot& output,
                                        SnapshotBuildContext context) const noexcept;
  void initialize_snapshot(PortfolioSnapshot& output, SnapshotBuildContext context,
                           bool& missing_mark) const noexcept;
  [[nodiscard]] bool accumulate_orders(PortfolioSnapshot& output) const noexcept;
  void accumulate_fills(PortfolioSnapshot& output) const noexcept;
  [[nodiscard]] bool accumulate_position(PortfolioSnapshot& output,
                                         const PositionSlot& position) const noexcept;
  [[nodiscard]] bool accumulate_positions(PortfolioSnapshot& output) const noexcept;
  [[nodiscard]] bool finalize_snapshot(PortfolioSnapshot& output,
                                       bool missing_mark) const noexcept;
  [[nodiscard]] bool
  corporate_action_is_exact(const PortfolioEvent& event,
                            std::size_t instrument_index) const noexcept;
  void commit_corporate_action(const PortfolioEvent& event,
                               std::size_t instrument_index) noexcept;
  [[nodiscard]] bool verify_invariants_locked() const noexcept;
  void set_unsafe(InvariantCode code) noexcept;
  void clear_session_state(bool carry_positions) noexcept;

  PortfolioConfiguration configuration_;
  PortfolioJournal& journal_;
  std::array<PositionSlot, kMaximumPortfolioPositions> positions_{};
  std::array<OrderSlot, kMaximumPortfolioOrders> orders_{};
  std::array<FillSlot, kMaximumPortfolioFills> fills_{};
  std::array<IdempotencySlot, kPortfolioIdempotencyCapacity> idempotency_{};
  std::array<std::int64_t, kMaximumPortfolioInstruments> marks_{};
  std::array<std::uint64_t, kMaximumPortfolioInstruments> mark_times_{};
  std::array<std::uint64_t, kMaximumPortfolioInstruments>
      corporate_action_generations_{};
  std::array<std::int64_t, kMaximumPortfolioStrategies> strategy_peaks_{};
  PortfolioSnapshot snapshot_{};
  PortfolioSnapshotStore snapshot_store_;
  std::uint64_t event_ordinal_{};
  std::uint64_t snapshot_sequence_{};
  std::uint64_t invariant_failure_count_{};
  PortfolioHealth health_{PortfolioHealth::starting};
  InvariantCode invariant_{InvariantCode::none};
  std::atomic_flag writer_gate_ = ATOMIC_FLAG_INIT;
  std::atomic<bool> initialized_{false};
  std::atomic<bool> stopped_{false};
  std::atomic<std::uint64_t> event_count_{0U};
  std::atomic<std::uint64_t> applied_count_{0U};
  std::atomic<std::uint64_t> duplicate_count_{0U};
  std::atomic<std::uint64_t> reconciled_count_{0U};
  std::atomic<std::uint64_t> rejected_count_{0U};
  std::atomic<std::uint64_t> correction_count_{0U};
  std::atomic<std::uint64_t> bust_count_{0U};
  std::atomic<std::uint64_t> snapshot_publication_count_{0U};
  std::atomic<std::uint64_t> snapshot_publish_failure_count_{0U};
  std::atomic<std::uint64_t> journal_full_count_{0U};
};

} // namespace aegis::risk::portfolio

#endif // AEGIS_RISK_PORTFOLIO_SERVICE_HPP
