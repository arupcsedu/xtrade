#include "aegis/market_data/synthetic/artifacts.hpp"
#include "cli_common.hpp"

#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string_view>

namespace synthetic = aegis::market_data::synthetic;

int main(const int argc, char** argv) {
  std::filesystem::path capture;
  std::filesystem::path book;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    if (argument == "--help" || argument == "-h") {
      std::cout << "Usage: synth-exchange-verify --capture FILE --book FILE\n";
      return 0;
    }
    if (index + 1 >= argc) {
      std::cerr << "missing value for " << argument << '\n';
      return 2;
    }
    if (argument == "--capture") {
      capture = argv[++index];
    } else if (argument == "--book") {
      book = argv[++index];
    } else {
      std::cerr << "unknown option: " << argument << '\n';
      return 2;
    }
  }
  if (capture.empty() || book.empty()) {
    std::cerr << "--capture and --book are required\n";
    return 2;
  }
  const auto report =
      synthetic::verify_artifacts({.capture = capture, .expected_book = book});
  std::cout << "result=" << (report.ok() ? "valid" : "invalid")
            << " packets=" << report.packets << " applied=" << report.applied_events
            << " duplicates=" << report.duplicate_packets
            << " gaps=" << report.sequence_gaps
            << " out_of_order=" << report.out_of_order_packets
            << " stale=" << report.stale_intervals
            << " crossed=" << report.crossed_books
            << " halted_updates=" << report.halted_update_attempts
            << " scenario_expected=" << report.scenario_expectation_met
            << " book_match=" << report.final_book_matches << " calculated_book_hash=0x"
            << std::hex << report.calculated_book_hash << " expected_book_hash=0x"
            << report.expected_book_hash << std::dec << '\n';
  if (!report.ok()) {
    std::cerr << "verification failed: " << synthetic::artifact_error_name(report.error)
              << " capture=" << synthetic::capture_error_name(report.capture_error)
              << " decode=" << synthetic::decode_error_name(report.decode_error)
              << '\n';
    return 1;
  }
  return 0;
}
