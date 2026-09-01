#ifndef AEGIS_RISK_ENGINE_HPP
#define AEGIS_RISK_ENGINE_HPP

#include "aegis/risk/journal.hpp"
#include "aegis/risk/portfolio_types.hpp"
#include "aegis/risk/types.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace aegis::risk {

class DeterministicPreTradeRiskEngine final {
public:
  DeterministicPreTradeRiskEngine(RiskLimitSnapshot limits,
                                  RiskDecisionJournal& journal) noexcept;

  [[nodiscard]] bool initialized() const noexcept;
  [[nodiscard]] EvaluationResult
  evaluate(const RiskEvaluationRequest& request) noexcept;
  [[nodiscard]] InstallStatus install_limits(RiskLimitSnapshot limits) noexcept;
  [[nodiscard]] StateUpdateStatus seed_position(PositionSeed seed) noexcept;
  [[nodiscard]] StateUpdateStatus
  install_portfolio_snapshot(const portfolio::PortfolioSnapshot& snapshot) noexcept;
  [[nodiscard]] StateUpdateStatus apply_fill(FillUpdate update) noexcept;
  [[nodiscard]] StateUpdateStatus update_profit_loss(ProfitLossUpdate update) noexcept;
  [[nodiscard]] StateUpdateStatus update_kill_switch(KillSwitchUpdate update) noexcept;
  void set_state_available(bool available) noexcept;
  [[nodiscard]] RiskMetrics metrics() const noexcept;
  [[nodiscard]] const RiskLimitSnapshot&
  limits_for_quiescent_inspection() const noexcept;

private:
  struct RuntimeSymbolState {
    std::atomic<std::int64_t> net_quantity_units{0};
    std::atomic<std::uint64_t> pending_buy_quantity_units{0U};
    std::atomic<std::uint64_t> pending_sell_quantity_units{0U};
    std::atomic<std::int64_t> mark_price_ticks{0};
    std::atomic<std::uint64_t> as_of_process_monotonic_time_ns{0U};
    std::atomic<bool> available{false};
  };

  struct RuntimeStrategyState {
    std::atomic<std::int64_t> pnl_currency_nanos{0};
    std::atomic<std::int64_t> peak_pnl_currency_nanos{0};
    std::atomic<std::uint64_t> as_of_process_monotonic_time_ns{0U};
  };

  struct IntentSlot {
    common::IntentId intent_id;
    bool occupied{false};
  };

  struct RateWindow {
    std::array<std::uint64_t, kMaximumRateEvents> timestamps{};
    std::uint64_t read_sequence{};
    std::uint64_t write_sequence{};
  };

  struct KillScopeIndices {
    std::size_t symbol{};
    std::size_t strategy{};
    std::size_t venue{};
  };

  struct RateAdmission {
    std::uint64_t now_ns{};
    std::uint64_t duration_ns{};
    std::uint32_t maximum_events{};
  };

  using PortfolioInstrumentIndices = std::array<std::size_t, kMaximumRiskSymbols>;
  using PortfolioStrategyIndices = std::array<std::size_t, kMaximumRiskStrategies>;

  [[nodiscard]] bool try_enter() noexcept;
  void leave() noexcept;
  [[nodiscard]] std::size_t
  find_symbol(common::InstrumentId instrument_id) const noexcept;
  [[nodiscard]] std::size_t
  find_strategy(common::StrategyId strategy_id) const noexcept;
  [[nodiscard]] std::size_t find_venue(common::VenueId venue_id) const noexcept;
  [[nodiscard]] bool applicable_kill(KillScopeIndices indices) const noexcept;
  [[nodiscard]] bool record_intent(common::IntentId intent_id,
                                   bool& duplicate) noexcept;
  [[nodiscard]] static bool admit_rate(RateWindow& window,
                                       RateAdmission admission) noexcept;
  [[nodiscard]] bool portfolio_snapshot_compatible(
      const portfolio::PortfolioSnapshot& snapshot) const noexcept;
  [[nodiscard]] bool
  map_portfolio_snapshot(const portfolio::PortfolioSnapshot& snapshot,
                         PortfolioInstrumentIndices& instrument_indices,
                         PortfolioStrategyIndices& strategy_indices) const noexcept;
  void install_mapped_portfolio_snapshot(
      const portfolio::PortfolioSnapshot& snapshot,
      const PortfolioInstrumentIndices& instrument_indices,
      const PortfolioStrategyIndices& strategy_indices) noexcept;
  [[nodiscard]] RiskDecision
  base_decision(const RiskEvaluationRequest& request, std::uint64_t evaluation_ordinal,
                std::uint64_t journal_sequence) const noexcept;
  [[nodiscard]] EvaluationResult
  evaluate_locked(const RiskEvaluationRequest& request,
                  RiskDecisionJournal::Reservation reservation) noexcept;

  RiskDecisionJournal& journal_;
  RiskLimitSnapshot limits_;
  std::array<RuntimeSymbolState, kMaximumRiskSymbols> symbol_state_{};
  std::array<RuntimeStrategyState, kMaximumRiskStrategies> strategy_state_{};
  std::array<IntentSlot, kDuplicateIntentCapacity> intent_slots_{};
  RateWindow order_rate_;
  RateWindow cancel_rate_;
  std::uint64_t last_evaluation_time_ns_{};
  std::atomic_flag evaluation_gate_ = ATOMIC_FLAG_INIT;
  std::atomic<bool> initialized_{false};
  std::atomic<bool> state_available_{true};
  std::atomic<std::uint64_t> authority_epoch_{0U};
  std::atomic<std::uint64_t> firm_kill_{0U};
  std::atomic<std::uint64_t> account_kill_{0U};
  std::atomic<std::uint64_t> symbol_kill_mask_{0U};
  std::atomic<std::uint64_t> strategy_kill_mask_{0U};
  std::atomic<std::uint64_t> venue_kill_mask_{0U};
  std::atomic<std::uint64_t> last_kill_command_sequence_{0U};
  std::atomic<std::uint64_t> kill_generation_{1U};
  std::atomic<std::uint64_t> position_generation_{1U};
  std::atomic<std::int64_t> firm_daily_pnl_currency_nanos_{0};
  std::atomic<std::int64_t> firm_peak_pnl_currency_nanos_{0};
  std::atomic<std::uint64_t> pnl_as_of_process_monotonic_time_ns_{0U};
  std::atomic<std::uint64_t> evaluation_count_{0U};
  std::atomic<std::uint64_t> approval_count_{0U};
  std::atomic<std::uint64_t> rejection_count_{0U};
  std::atomic<std::uint64_t> busy_count_{0U};
  std::atomic<std::uint64_t> journal_full_count_{0U};
  std::atomic<std::uint64_t> active_limit_revision_{0U};
  std::atomic<std::uint64_t> last_portfolio_snapshot_sequence_{0U};
  std::atomic<std::uint64_t> last_portfolio_snapshot_hash_{0U};
};

} // namespace aegis::risk

#endif // AEGIS_RISK_ENGINE_HPP
