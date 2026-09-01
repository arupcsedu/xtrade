#ifndef AEGIS_EXECUTION_SESSION_HPP
#define AEGIS_EXECUTION_SESSION_HPP

#include "aegis/execution/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace aegis::execution {

class SequenceManager final {
public:
  explicit SequenceManager(std::uint64_t first_outbound = 1U,
                           std::uint64_t first_inbound = 1U) noexcept;

  [[nodiscard]] SequenceStatus claim_outbound(std::uint64_t& sequence) noexcept;
  [[nodiscard]] SequenceStatus observe_inbound(std::uint64_t sequence) noexcept;
  [[nodiscard]] std::uint64_t next_outbound() const noexcept;
  [[nodiscard]] std::uint64_t expected_inbound() const noexcept;
  void reset(std::uint64_t first_outbound, std::uint64_t first_inbound) noexcept;

private:
  std::uint64_t next_outbound_{1U};
  std::uint64_t expected_inbound_{1U};
  std::uint64_t last_inbound_{};
};

class FixedWindowRateLimiter final {
public:
  FixedWindowRateLimiter(std::uint64_t window_ns,
                         std::uint32_t maximum_events) noexcept;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] bool allow(std::uint64_t now_ns) noexcept;
  [[nodiscard]] std::uint32_t occupancy(std::uint64_t now_ns) noexcept;
  void reset() noexcept;

private:
  void expire(std::uint64_t now_ns) noexcept;

  std::array<std::uint64_t, kMaximumGatewayCommands> timestamps_{};
  std::uint64_t window_ns_{};
  std::uint64_t last_now_ns_{};
  std::uint32_t maximum_events_{};
  std::uint32_t head_{};
  std::uint32_t count_{};
  bool monotonic_{true};
};

class SessionLifecycle final {
public:
  SessionLifecycle(std::uint64_t exchange_session_epoch, std::uint64_t fencing_token,
                   std::uint64_t heartbeat_interval_ns,
                   std::uint64_t heartbeat_timeout_ns) noexcept;

  [[nodiscard]] bool initialized() const noexcept;
  [[nodiscard]] SessionState state() const noexcept;
  [[nodiscard]] SessionReason reason() const noexcept;
  [[nodiscard]] bool ready() const noexcept;
  [[nodiscard]] bool request_logon(std::uint64_t now_ns) noexcept;
  [[nodiscard]] bool accept_logon(std::uint64_t now_ns) noexcept;
  [[nodiscard]] bool request_logoff(std::uint64_t now_ns) noexcept;
  [[nodiscard]] bool complete_logoff(std::uint64_t now_ns) noexcept;
  [[nodiscard]] bool begin_recovery(std::uint64_t now_ns,
                                    SessionReason reason) noexcept;
  [[nodiscard]] bool complete_recovery(std::uint64_t now_ns,
                                       std::uint64_t next_inbound_sequence) noexcept;
  void halt(std::uint64_t now_ns, SessionReason reason) noexcept;
  void shutdown(std::uint64_t now_ns) noexcept;
  [[nodiscard]] SequenceStatus claim_outbound(std::uint64_t now_ns,
                                              std::uint64_t& sequence) noexcept;
  [[nodiscard]] SequenceStatus observe_inbound(std::uint64_t now_ns,
                                               std::uint64_t sequence) noexcept;
  [[nodiscard]] bool heartbeat_due(std::uint64_t now_ns) const noexcept;
  [[nodiscard]] bool check_heartbeat_timeout(std::uint64_t now_ns) noexcept;
  [[nodiscard]] SessionSnapshot snapshot() const noexcept;

private:
  void transition(SessionState state, SessionReason reason,
                  std::uint64_t now_ns) noexcept;

  SequenceManager sequences_;
  std::uint64_t exchange_session_epoch_{};
  std::uint64_t fencing_token_{};
  std::uint64_t heartbeat_interval_ns_{};
  std::uint64_t heartbeat_timeout_ns_{};
  std::uint64_t last_inbound_ns_{};
  std::uint64_t last_outbound_ns_{};
  std::uint64_t transition_count_{};
  SessionState state_{SessionState::stopped};
  SessionReason reason_{SessionReason::none};
  bool initialized_{false};
};

} // namespace aegis::execution

#endif // AEGIS_EXECUTION_SESSION_HPP
