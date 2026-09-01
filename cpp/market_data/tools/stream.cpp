#include "aegis/market_data/synthetic/artifacts.hpp"
#include "cli_common.hpp"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace synthetic = aegis::market_data::synthetic;

int main(const int argc, char** argv) {
  std::filesystem::path capture;
  std::uint64_t acceleration = 0U;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    if (argument == "--help" || argument == "-h") {
      std::cout << "Usage: synth-exchange-stream --capture FILE "
                   "[--acceleration N] > packets.bin\n"
                << "Acceleration 0 emits without pacing; 1 is capture time.\n";
      return 0;
    }
    if (index + 1 >= argc) {
      std::cerr << "missing value for " << argument << '\n';
      return 2;
    }
    if (argument == "--capture") {
      capture = argv[++index];
    } else if (argument == "--acceleration") {
      const auto parsed =
          synthetic::cli::parse_integer<std::uint64_t>(std::string_view{argv[++index]});
      if (!parsed.has_value()) {
        std::cerr << "invalid --acceleration\n";
        return 2;
      }
      acceleration = *parsed;
    } else {
      std::cerr << "unknown option: " << argument << '\n';
      return 2;
    }
  }
  if (capture.empty()) {
    std::cerr << "--capture is required\n";
    return 2;
  }
  std::uint64_t packets = 0U;
  const auto error =
      synthetic::stream_capture(capture, std::cout, acceleration, packets);
  if (error != synthetic::ArtifactError::none) {
    std::cerr << "stream failed: " << synthetic::artifact_error_name(error) << '\n';
    return 1;
  }
  std::cerr << "streamed_packets=" << packets << '\n';
  return 0;
}
