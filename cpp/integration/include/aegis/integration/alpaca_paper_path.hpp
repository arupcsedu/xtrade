#ifndef AEGIS_INTEGRATION_ALPACA_PAPER_PATH_HPP
#define AEGIS_INTEGRATION_ALPACA_PAPER_PATH_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace aegis::integration {

inline constexpr std::uint16_t kAlpacaPaperPathSchemaMajor = 1U;
inline constexpr std::uint16_t kAlpacaPaperPathSchemaMinor = 0U;
inline constexpr std::size_t kAlpacaPaperSymbolBytes = 17U;
inline constexpr std::size_t kAlpacaClientOrderIdBytes = 33U;
inline constexpr std::uint64_t kAlpacaPaperPriceIncrementCurrencyNanos = 10'000'000U;

enum class AlpacaPaperPathStage : std::uint8_t {
  complete = 1,
  invalid_symbol = 2,
  live_capable_build = 3,
  router_rejected = 4,
  risk_rejected = 5,
  oms_rejected = 6,
  gateway_rejected = 7,
};

// This fixed certification contract contains hashes and non-secret order terms
// only. It contains no endpoint, credential, provider order ID, or raw market
// data. The one-cent order is intentionally nonmarketable and PAPER-only.
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct AlpacaPaperPathEvidence {
  std::uint16_t schema_major{kAlpacaPaperPathSchemaMajor};
  std::uint16_t schema_minor{kAlpacaPaperPathSchemaMinor};
  AlpacaPaperPathStage stage{AlpacaPaperPathStage::invalid_symbol};
  std::array<char, kAlpacaPaperSymbolBytes> symbol{};
  std::uint64_t instrument_id_high{};
  std::uint64_t instrument_id_low{};
  std::array<char, kAlpacaClientOrderIdBytes> client_order_id{};
  std::int64_t price_ticks{};
  std::uint64_t price_increment_currency_nanos{};
  std::uint64_t quantity_units{};
  std::uint64_t router_configuration_hash{};
  std::uint64_t routing_request_hash{};
  std::uint64_t routing_decision_hash{};
  std::uint64_t risk_snapshot_hash{};
  std::uint64_t risk_context_hash{};
  std::uint64_t risk_decision_hash{};
  std::uint64_t risk_journal_sequence{};
  std::uint64_t oms_configuration_hash{};
  std::uint64_t oms_command_hash{};
  std::uint64_t oms_journal_sequence{};
  std::uint64_t gateway_configuration_hash{};
  std::uint64_t gateway_request_hash{};
  std::uint64_t gateway_audit_sequence{};
  std::uint64_t gateway_audit_hash{};
  std::uint64_t gateway_outbound_sequence{};
  bool paper_mode{false};
  bool buy_only{false};
  bool regular_hours_only{false};
  bool fresh_pretrade_risk_required{false};
  bool risk_approved{false};
  bool oms_command_valid{false};
  bool gateway_final_gate_accepted{false};
  bool live_trading_compiled{true};
  std::uint64_t stable_hash{};
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

[[nodiscard]] bool valid_alpaca_paper_symbol(std::string_view symbol) noexcept;
[[nodiscard]] std::uint64_t
stable_alpaca_paper_path_hash(AlpacaPaperPathEvidence evidence) noexcept;
[[nodiscard]] bool
valid_alpaca_paper_path_evidence(const AlpacaPaperPathEvidence& evidence) noexcept;
[[nodiscard]] AlpacaPaperPathEvidence
run_alpaca_paper_path(std::string_view symbol,
                      std::uint64_t seed = 20'260'911U) noexcept;

static_assert(std::is_trivially_copyable_v<AlpacaPaperPathEvidence>);

} // namespace aegis::integration

#endif // AEGIS_INTEGRATION_ALPACA_PAPER_PATH_HPP
