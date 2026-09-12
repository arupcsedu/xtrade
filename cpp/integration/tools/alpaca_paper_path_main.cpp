#include "aegis/common/build_info.hpp"
#include "aegis/integration/alpaca_paper_path.hpp"

#include <charconv>
#include <cstdint>
#include <iostream>
#include <string_view>

namespace {

[[nodiscard]] bool parse_seed(const std::string_view value,
                              std::uint64_t& output) noexcept {
  const auto parsed =
      std::from_chars(value.data(), value.data() + value.size(), output);
  return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() &&
         output != 0U;
}

[[nodiscard]] std::string_view
stage_name(const aegis::integration::AlpacaPaperPathStage stage) noexcept {
  using Stage = aegis::integration::AlpacaPaperPathStage;
  switch (stage) {
  case Stage::complete:
    return "COMPLETE";
  case Stage::invalid_symbol:
    return "INVALID_SYMBOL";
  case Stage::live_capable_build:
    return "LIVE_CAPABLE_BUILD";
  case Stage::router_rejected:
    return "ROUTER_REJECTED";
  case Stage::risk_rejected:
    return "RISK_REJECTED";
  case Stage::oms_rejected:
    return "OMS_REJECTED";
  case Stage::gateway_rejected:
    return "GATEWAY_REJECTED";
  }
  return "INVALID";
}

void print_boolean(const bool value) { std::cout << (value ? "true" : "false"); }

} // namespace

int main(const int argc, const char* const* argv) {
  std::string_view symbol{"NVDA"};
  std::uint64_t seed = 20'260'911U;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    if (argument == "--symbol" && index + 1 < argc) {
      symbol = argv[++index];
    } else if (argument == "--seed" && index + 1 < argc) {
      if (!parse_seed(argv[++index], seed)) {
        std::cerr << "invalid --seed\n";
        return 2;
      }
    } else {
      std::cerr << "usage: aegis-alpaca-paper-path [--symbol SYMBOL] [--seed N]\n";
      return 2;
    }
  }
  const auto evidence = aegis::integration::run_alpaca_paper_path(symbol, seed);
  const auto& build = aegis::common::current_build_info();
  // Escaped quotes keep this compact fixed-schema writer easier to audit than
  // raw strings containing physical newlines.
  // NOLINTBEGIN(modernize-raw-string-literal)
  std::cout << "{\n"
            << "  \"build_version\": \"" << build.version << "\",\n"
            << "  \"buy_only\": ";
  print_boolean(evidence.buy_only);
  std::cout << ",\n  \"client_order_id\": \"" << evidence.client_order_id.data()
            << "\",\n"
            << "  \"fresh_pretrade_risk_required\": ";
  print_boolean(evidence.fresh_pretrade_risk_required);
  std::cout << ",\n  \"gateway_audit_hash\": " << evidence.gateway_audit_hash
            << ",\n  \"gateway_audit_sequence\": " << evidence.gateway_audit_sequence
            << ",\n  \"gateway_configuration_hash\": "
            << evidence.gateway_configuration_hash
            << ",\n  \"gateway_final_gate_accepted\": ";
  print_boolean(evidence.gateway_final_gate_accepted);
  std::cout << ",\n  \"gateway_outbound_sequence\": "
            << evidence.gateway_outbound_sequence
            << ",\n  \"gateway_request_hash\": " << evidence.gateway_request_hash
            << ",\n  \"live_trading_compiled\": ";
  print_boolean(evidence.live_trading_compiled);
  std::cout << ",\n  \"instrument_id_high\": " << evidence.instrument_id_high
            << ",\n  \"instrument_id_low\": " << evidence.instrument_id_low
            << ",\n  \"mode\": \"PAPER\",\n"
            << "  \"oms_command_hash\": " << evidence.oms_command_hash
            << ",\n  \"oms_command_valid\": ";
  print_boolean(evidence.oms_command_valid);
  std::cout << ",\n  \"oms_configuration_hash\": " << evidence.oms_configuration_hash
            << ",\n  \"oms_journal_sequence\": " << evidence.oms_journal_sequence
            << ",\n  \"paper_mode\": ";
  print_boolean(evidence.paper_mode);
  std::cout << ",\n  \"price_increment_currency_nanos\": "
            << evidence.price_increment_currency_nanos
            << ",\n  \"price_ticks\": " << evidence.price_ticks
            << ",\n  \"quantity_units\": " << evidence.quantity_units
            << ",\n  \"regular_hours_only\": ";
  print_boolean(evidence.regular_hours_only);
  std::cout << ",\n  \"risk_approved\": ";
  print_boolean(evidence.risk_approved);
  std::cout << ",\n  \"risk_context_hash\": " << evidence.risk_context_hash
            << ",\n  \"risk_decision_hash\": " << evidence.risk_decision_hash
            << ",\n  \"risk_journal_sequence\": " << evidence.risk_journal_sequence
            << ",\n  \"risk_snapshot_hash\": " << evidence.risk_snapshot_hash
            << ",\n  \"router_configuration_hash\": "
            << evidence.router_configuration_hash
            << ",\n  \"routing_decision_hash\": " << evidence.routing_decision_hash
            << ",\n  \"routing_request_hash\": " << evidence.routing_request_hash
            << ",\n  \"schema_version\": \"" << evidence.schema_major << '.'
            << evidence.schema_minor << ".0\",\n"
            << "  \"stable_hash\": " << evidence.stable_hash << ",\n  \"stage\": \""
            << stage_name(evidence.stage) << "\",\n  \"symbol\": \""
            << evidence.symbol.data() << "\"\n}\n";
  // NOLINTEND(modernize-raw-string-literal)
  return aegis::integration::valid_alpaca_paper_path_evidence(evidence) ? 0 : 1;
}
