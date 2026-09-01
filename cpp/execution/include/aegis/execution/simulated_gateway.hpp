#ifndef AEGIS_EXECUTION_SIMULATED_GATEWAY_HPP
#define AEGIS_EXECUTION_SIMULATED_GATEWAY_HPP

#include "aegis/execution/interfaces.hpp"
#include "aegis/execution/journal.hpp"
#include "aegis/execution/session.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace aegis::execution {

class SimulatedGatewayBase : public IExchangeGateway {
public:
  [[nodiscard]] bool start(std::uint64_t now_ns) noexcept final;
  [[nodiscard]] bool logoff(std::uint64_t now_ns) noexcept final;
  [[nodiscard]] GatewaySubmitResult
  send_order(const GatewayRequest& request) noexcept final;
  [[nodiscard]] GatewaySubmitResult
  cancel_order(const GatewayRequest& request) noexcept final;
  [[nodiscard]] GatewaySubmitResult
  replace_order(const GatewayRequest& request) noexcept final;
  [[nodiscard]] GatewayReason
  on_market_event(const market_data::synthetic::SyntheticEvent& event) noexcept final;
  [[nodiscard]] GatewayPollStatus poll_event(std::uint64_t now_ns,
                                             GatewayEvent& output) noexcept final;
  [[nodiscard]] bool heartbeat(std::uint64_t now_ns) noexcept final;
  [[nodiscard]] bool begin_recovery(std::uint64_t now_ns) noexcept final;
  [[nodiscard]] RecoveryStatus recover(const GatewayRecoverySnapshot& snapshot,
                                       std::uint64_t now_ns) noexcept final;
  [[nodiscard]] GatewayRecoverySnapshot recovery_snapshot() const noexcept final;
  [[nodiscard]] GatewayMode mode() const noexcept final;
  [[nodiscard]] GatewayHealth health() const noexcept final;
  [[nodiscard]] bool ready() const noexcept final;
  [[nodiscard]] SessionSnapshot session_snapshot() const noexcept final;
  [[nodiscard]] GatewayMetrics metrics() const noexcept final;
  [[nodiscard]] const common::BuildInfo& build_info() const noexcept final;
  [[nodiscard]] std::uint64_t configuration_hash() const noexcept final;
  void shutdown(std::uint64_t now_ns) noexcept final;

  // Test/certification hook for repository-owned normalized responses only.
  // It accepts no bytes, endpoint, credentials, or provider protocol details.
  [[nodiscard]] bool inject_synthetic_response(const GatewayEvent& event,
                                               std::uint64_t due_ns) noexcept;

protected:
  SimulatedGatewayBase(GatewayConfiguration configuration, GatewayAuditJournal& journal,
                       GatewayMode required_mode) noexcept;

private:
  struct CommandSlot {
    common::GlobalEventId command_id;
    std::uint64_t request_hash{};
    GatewaySubmitResult result;
    bool occupied{false};
  };

  struct ScheduledEvent {
    GatewayEvent event;
    std::uint64_t due_ns{};
    std::uint64_t insertion_ordinal{};
    bool occupied{false};
  };

  struct MarketSlot {
    SyntheticInstrumentMapping mapping;
    market_data::synthetic::TradingStatus status{
        market_data::synthetic::TradingStatus::pre_open};
    std::int64_t best_bid_ticks{};
    std::int64_t best_ask_ticks{};
    std::uint64_t best_bid_quantity_units{};
    std::uint64_t best_ask_quantity_units{};
    std::uint64_t last_market_event_hash{};
    bool occupied{false};
  };

  [[nodiscard]] bool try_enter() noexcept;
  void leave() noexcept;
  [[nodiscard]] GatewaySubmitResult submit(const GatewayRequest& request,
                                           oms::GatewayCommandKind expected) noexcept;
  [[nodiscard]] GatewayReason
  final_gate(const GatewayRequest& request,
             oms::GatewayCommandKind expected) const noexcept;
  [[nodiscard]] GatewayReason
  validate_risk_binding(const GatewayRequest& request) const noexcept;
  [[nodiscard]] GatewayReason
  validate_new_exposure_state(const GatewayRequest& request) const noexcept;
  [[nodiscard]] std::size_t
  find_command(common::GlobalEventId command_id) const noexcept;
  [[nodiscard]] std::size_t
  find_free_command(common::GlobalEventId command_id) const noexcept;
  [[nodiscard]] std::size_t find_order(common::OrderId order_id) const noexcept;
  [[nodiscard]] std::size_t find_free_order(common::OrderId order_id) const noexcept;
  [[nodiscard]] std::size_t
  find_market(common::InstrumentId instrument_id) const noexcept;
  [[nodiscard]] std::size_t
  find_market(const market_data::synthetic::SyntheticEvent& event) const noexcept;
  [[nodiscard]] std::size_t find_free_event() const noexcept;
  [[nodiscard]] std::size_t find_due_event(std::uint64_t now_ns) const noexcept;
  [[nodiscard]] bool schedule_event(GatewayEvent event, std::uint64_t due_ns) noexcept;
  [[nodiscard]] GatewayReason process_command(const GatewayRequest& request,
                                              std::uint64_t outbound_sequence,
                                              std::uint64_t& scheduled_hash) noexcept;
  [[nodiscard]] GatewayReason process_new(const GatewayRequest& request,
                                          std::uint64_t outbound_sequence,
                                          std::uint64_t& scheduled_hash) noexcept;
  [[nodiscard]] GatewayReason process_cancel(const GatewayRequest& request,
                                             std::uint64_t outbound_sequence,
                                             std::uint64_t& scheduled_hash) noexcept;
  [[nodiscard]] GatewayReason process_replace(const GatewayRequest& request,
                                              std::uint64_t outbound_sequence,
                                              std::uint64_t& scheduled_hash) noexcept;
  void settle_due_controls(std::uint64_t now_ns) noexcept;
  [[nodiscard]] GatewayReason
  process_trade(const market_data::synthetic::SyntheticEvent& event,
                std::size_t market_index, std::uint64_t& scheduled_hash) noexcept;
  void update_market(const market_data::synthetic::SyntheticEvent& event,
                     std::size_t market_index) noexcept;
  [[nodiscard]] GatewayEvent base_event(const oms::GatewayCommand& command,
                                        GatewayResponseKind kind, GatewayReason reason,
                                        std::uint64_t event_ordinal) const noexcept;
  [[nodiscard]] bool append_audit(GatewayAuditJournal::Reservation reservation,
                                  GatewayAuditKind kind, GatewayReason reason,
                                  common::GlobalEventId correlation_id,
                                  common::OrderId order_id, std::uint64_t command_hash,
                                  std::uint64_t risk_hash, std::uint64_t safety_hash,
                                  std::uint64_t event_hash, std::uint64_t now_ns,
                                  std::uint64_t& sequence,
                                  std::uint64_t& record_hash) noexcept;
  [[nodiscard]] bool append_simple_audit(GatewayAuditKind kind, GatewayReason reason,
                                         std::uint64_t now_ns) noexcept;
  void set_unsafe(SessionReason reason, std::uint64_t now_ns) noexcept;
  void update_health() noexcept;
  [[nodiscard]] bool mode_matches_risk(risk::TradingMode mode) const noexcept;
  [[nodiscard]] bool
  recovery_scope_matches(const GatewayRecoverySnapshot& snapshot) const noexcept;
  [[nodiscard]] static bool
  recovery_is_ambiguous(const GatewayRecoverySnapshot& snapshot) noexcept;
  [[nodiscard]] bool
  rebuild_recovery_orders(const GatewayRecoverySnapshot& snapshot) noexcept;
  [[nodiscard]] bool
  market_state_allows_new(market_state::MarketState state,
                          market_state::OfficialTradingStatus status) const noexcept;

  GatewayConfiguration configuration_;
  GatewayAuditJournal& journal_;
  SessionLifecycle session_;
  FixedWindowRateLimiter new_order_limiter_;
  FixedWindowRateLimiter cancel_limiter_;
  FixedWindowRateLimiter replace_limiter_;
  std::array<CommandSlot, kMaximumGatewayCommands> commands_{};
  std::array<PaperOrderSnapshot, kMaximumGatewayOrders> orders_{};
  std::array<ScheduledEvent, kMaximumGatewayEvents> events_{};
  std::array<MarketSlot, kMaximumGatewayInstruments> markets_{};
  GatewayMetrics metrics_{};
  std::uint64_t active_event_count_{};
  std::uint64_t next_event_ordinal_{1U};
  std::uint64_t next_execution_ordinal_{1U};
  std::uint64_t new_order_ordinal_{};
  GatewayHealth health_{GatewayHealth::starting};
  GatewayMode mode_{GatewayMode::halted};
  std::atomic_flag writer_gate_ = ATOMIC_FLAG_INIT;
  std::atomic<bool> initialized_{false};
  std::atomic<bool> stopped_{false};
};

class SyntheticExchangeGateway final : public SimulatedGatewayBase {
public:
  SyntheticExchangeGateway(GatewayConfiguration configuration,
                           GatewayAuditJournal& journal) noexcept;
};

class PaperBrokerGateway final : public SimulatedGatewayBase {
public:
  PaperBrokerGateway(GatewayConfiguration configuration,
                     GatewayAuditJournal& journal) noexcept;
};

} // namespace aegis::execution

#endif // AEGIS_EXECUTION_SIMULATED_GATEWAY_HPP
