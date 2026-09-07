#include "aegis/integration/paper_acceptance.hpp"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace {

struct Options {
  std::filesystem::path machine{"acceptance-report.json"};
  std::filesystem::path human{"system-report.md"};
  std::uint64_t seed{aegis::integration::kDefaultPaperAcceptanceSeed};
  bool valid{true};
};

[[nodiscard]] Options parse_options(const int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    if ((argument == "--machine" || argument == "--human" || argument == "--seed") &&
        index + 1 >= argc) {
      options.valid = false;
      break;
    }
    if (argument == "--machine") {
      options.machine = argv[++index];
    } else if (argument == "--human") {
      options.human = argv[++index];
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
    std::cerr << "usage: aegis-paper-acceptance [--machine PATH] [--human PATH] "
                 "[--seed UINT64]\n";
    return 2;
  }
  const auto report = aegis::integration::run_paper_acceptance(options.seed);
  if (!aegis::integration::write_machine_report(report, options.machine) ||
      !aegis::integration::write_human_report(report, options.human)) {
    std::cerr << "failed to write PAPER acceptance reports\n";
    return 3;
  }
  std::cout << "PAPER acceptance: " << (report.passed ? "PASS" : "FAIL")
            << " scenarios=" << report.scenarios.size() << " seed=" << report.seed
            << " machine=" << options.machine << " human=" << options.human << '\n';
  return report.passed ? 0 : 1;
}
