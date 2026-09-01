#include "aegis/market_data/synthetic/artifacts.hpp"
#include "cli_common.hpp"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace synthetic = aegis::market_data::synthetic;

int main(const int argc, char** argv) {
  std::filesystem::path capture;
  std::uint64_t maximum_records = 20U;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    if (argument == "--help" || argument == "-h") {
      std::cout << "Usage: synth-exchange-inspect --capture FILE [--max N]\n";
      return 0;
    }
    if (index + 1 >= argc) {
      std::cerr << "missing value for " << argument << '\n';
      return 2;
    }
    if (argument == "--capture") {
      capture = argv[++index];
    } else if (argument == "--max") {
      const auto parsed =
          synthetic::cli::parse_integer<std::uint64_t>(std::string_view{argv[++index]});
      if (!parsed.has_value()) {
        std::cerr << "invalid --max\n";
        return 2;
      }
      maximum_records = *parsed;
    } else {
      std::cerr << "unknown option: " << argument << '\n';
      return 2;
    }
  }
  if (capture.empty()) {
    std::cerr << "--capture is required\n";
    return 2;
  }
  const auto error = synthetic::inspect_capture(capture, std::cout, maximum_records);
  if (error != synthetic::ArtifactError::none) {
    std::cerr << "inspection failed: " << synthetic::artifact_error_name(error) << '\n';
    return 1;
  }
  return 0;
}
