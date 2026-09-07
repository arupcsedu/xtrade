#ifndef AEGIS_EXECUTION_INTERFACES_HPP
#define AEGIS_EXECUTION_INTERFACES_HPP

#include "aegis/common/build_info.hpp"
#include "aegis/execution/live_capability.hpp"
#include "aegis/execution/types.hpp"

namespace aegis::execution {

class IExchangeGateway {
public:
  virtual ~IExchangeGateway() = default;

  [[nodiscard]] virtual bool start(std::uint64_t now_ns) noexcept = 0;
  [[nodiscard]] virtual bool logoff(std::uint64_t now_ns) noexcept = 0;
  [[nodiscard]] virtual GatewaySubmitResult
  send_order(const GatewayRequest& request) noexcept = 0;
  [[nodiscard]] virtual GatewaySubmitResult
  cancel_order(const GatewayRequest& request) noexcept = 0;
  [[nodiscard]] virtual GatewaySubmitResult
  replace_order(const GatewayRequest& request) noexcept = 0;
  [[nodiscard]] virtual GatewayReason
  on_market_event(const market_data::synthetic::SyntheticEvent& event) noexcept = 0;
  [[nodiscard]] virtual GatewayPollStatus poll_event(std::uint64_t now_ns,
                                                     GatewayEvent& output) noexcept = 0;
  [[nodiscard]] virtual bool heartbeat(std::uint64_t now_ns) noexcept = 0;
  [[nodiscard]] virtual bool begin_recovery(std::uint64_t now_ns) noexcept = 0;
  [[nodiscard]] virtual RecoveryStatus recover(const GatewayRecoverySnapshot& snapshot,
                                               std::uint64_t now_ns) noexcept = 0;
  [[nodiscard]] virtual GatewayRecoverySnapshot recovery_snapshot() const noexcept = 0;
  [[nodiscard]] virtual GatewayMode mode() const noexcept = 0;
  [[nodiscard]] virtual GatewayHealth health() const noexcept = 0;
  [[nodiscard]] virtual bool ready() const noexcept = 0;
  [[nodiscard]] virtual SessionSnapshot session_snapshot() const noexcept = 0;
  [[nodiscard]] virtual GatewayMetrics metrics() const noexcept = 0;
  [[nodiscard]] virtual const common::BuildInfo& build_info() const noexcept = 0;
  [[nodiscard]] virtual std::uint64_t configuration_hash() const noexcept = 0;
  virtual void shutdown(std::uint64_t now_ns) noexcept = 0;
};

// These boundaries deliberately know no native fields, tags, dictionaries,
// endpoints, credentials, transports, or session semantics. Implementations
// require an authorized specification and remain outside this repository slice.
class INativeProtocolAdapter {
public:
  virtual ~INativeProtocolAdapter() = default;
  [[nodiscard]] virtual AdapterStatus encode(const oms::GatewayCommand& command,
                                             OpaqueProtocolFrame& output) noexcept = 0;
  [[nodiscard]] virtual AdapterStatus decode(const OpaqueProtocolFrame& input,
                                             GatewayEvent& output) noexcept = 0;
};

class IFixCompatibleAdapter {
public:
  virtual ~IFixCompatibleAdapter() = default;
  [[nodiscard]] virtual AdapterStatus encode(const oms::GatewayCommand& command,
                                             OpaqueProtocolFrame& output) noexcept = 0;
  [[nodiscard]] virtual AdapterStatus decode(const OpaqueProtocolFrame& input,
                                             GatewayEvent& output) noexcept = 0;
};

#if AEGIS_LIVE_TRADING_COMPILED
// Merely compiling this boundary does not authorize or implement live trading.
// A future adapter must still implement every runtime activation interlock.
class ILiveTransmissionAdapter {
public:
  virtual ~ILiveTransmissionAdapter() = default;
  [[nodiscard]] virtual AdapterStatus
  transmit(const OpaqueProtocolFrame& frame,
           const VerifiedLiveTransmissionCapability& capability) noexcept = 0;
};
#endif

} // namespace aegis::execution

#endif // AEGIS_EXECUTION_INTERFACES_HPP
