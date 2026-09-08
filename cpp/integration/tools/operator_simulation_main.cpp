#include "aegis/integration/operator_simulation.hpp"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace {

struct Options final {
  std::filesystem::path machine{"operator-simulation.json"};
  std::filesystem::path audit{"operator-audit.ndjson"};
  std::uint64_t seed{aegis::integration::kDefaultOperatorSimulationSeed};
  bool valid{true};
};

[[nodiscard]] Options parse_options(const int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    if ((argument == "--machine" || argument == "--audit" || argument == "--seed") &&
        index + 1 >= argc) {
      options.valid = false;
      break;
    }
    if (argument == "--machine") {
      options.machine = argv[++index];
    } else if (argument == "--audit") {
      options.audit = argv[++index];
    } else if (argument == "--seed") {
      const std::string_view value{argv[++index]};
      const auto parsed =
          std::from_chars(value.data(), value.data() + value.size(), options.seed);
      options.valid = parsed.ec == std::errc{} &&
                      parsed.ptr == value.data() + value.size() && options.seed != 0U;
    } else {
      options.valid = false;
    }
  }
  return options;
}

} // namespace

int main(const int argc, char** argv) {
  const auto options = parse_options(argc, argv);
  if (!options.valid) {
    std::cerr << "usage: aegis-operator-simulation [--machine PATH] [--audit PATH] "
                 "[--seed UINT64]\n";
    return 2;
  }
  const auto report = aegis::integration::run_operator_simulation(options.seed);
  if (!aegis::integration::write_operator_audit_extract(report, options.audit) ||
      !aegis::integration::write_operator_simulation_report(report, options.audit,
                                                            options.machine)) {
    std::cerr << "failed to write operator simulation evidence\n";
    return 3;
  }
  std::cout << "PAPER operator simulation: " << (report.passed ? "PASS" : "FAIL")
            << " scopes=" << report.scopes.size()
            << " audit_records=" << report.audit_records.size()
            << " seed=" << report.seed << '\n';
  return report.passed ? 0 : 1;
}
