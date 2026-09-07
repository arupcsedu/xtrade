#include "aegis/common/build_info.hpp"
#include "aegis/common/sha256.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

constexpr std::size_t kMaximumConfigurationBytes = std::size_t{4U} * 1024U * 1024U;
constexpr std::string_view kRole = AEGIS_EDGE_SERVICE_ROLE;
volatile std::sig_atomic_t stop_requested = 0;

struct Options {
  std::filesystem::path configuration;
  std::filesystem::path status_root{"/run/aegis-mx/health"};
  std::string mode{"SIMULATION"};
  bool once{false};
  bool valid{true};
};

void request_stop(const int signal_number) noexcept {
  static_cast<void>(signal_number);
  stop_requested = 1;
}

[[nodiscard]] Options parse_options(const int argc, char** argv) {
  Options options;
  // Startup is single-threaded and this is the only environment read.
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  if (const auto* mode = std::getenv("AEGIS_TRADING_MODE"); mode != nullptr) {
    options.mode = mode;
  }
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    if ((argument == "--configuration" || argument == "--status-root" ||
         argument == "--mode") &&
        index + 1 >= argc) {
      options.valid = false;
      break;
    }
    if (argument == "--configuration") {
      options.configuration = argv[++index];
    } else if (argument == "--status-root") {
      options.status_root = argv[++index];
    } else if (argument == "--mode") {
      options.mode = argv[++index];
    } else if (argument == "--once") {
      options.once = true;
    } else {
      options.valid = false;
    }
  }
  options.valid = options.valid && !options.configuration.empty() &&
                  !options.status_root.empty() &&
                  (options.mode == "SIMULATION" || options.mode == "PAPER");
  return options;
}

[[nodiscard]] std::string digest_hex(const aegis::common::Sha256Digest& digest) {
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const auto byte : digest) {
    output << std::setw(2) << static_cast<unsigned>(byte);
  }
  return output.str();
}

[[nodiscard]] bool read_configuration(const std::filesystem::path& path,
                                      std::vector<std::uint8_t>& bytes) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error || size == 0U || size > kMaximumConfigurationBytes) {
    return false;
  }
  bytes.resize(static_cast<std::size_t>(size));
  std::ifstream input{path, std::ios::binary};
  input.read(reinterpret_cast<char*>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  if (!input || input.peek() != std::ifstream::traits_type::eof()) {
    return false;
  }
  const std::string_view text{reinterpret_cast<const char*>(bytes.data()),
                              bytes.size()};
  // These binaries are deliberately non-live even when a malformed preflight
  // tries to pass them live configuration.
  return text.find("LIVE") == std::string_view::npos;
}

[[nodiscard]] std::uint64_t monotonic_now_ns() noexcept {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

[[nodiscard]] bool atomic_write(const std::filesystem::path& path,
                                const std::string& content) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) {
    return false;
  }
  auto temporary = path;
  temporary += ".tmp." + std::to_string(static_cast<std::uint64_t>(::getpid()));
  {
    std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
    output << content;
    output.flush();
    if (!output) {
      std::filesystem::remove(temporary, error);
      return false;
    }
  }
  std::filesystem::rename(temporary, path, error);
  if (error) {
    std::filesystem::remove(temporary, error);
    return false;
  }
  return true;
}

[[nodiscard]] std::string health_json(const std::string_view service,
                                      const std::string_view version,
                                      const std::string_view config_hash,
                                      const std::string_view mode, const bool ready) {
  std::ostringstream output;
  output << "{\n"
         << R"(  "build_version": ")" << version << '"' << ",\n"
         << R"(  "configuration_sha256": ")" << config_hash << '"' << ",\n"
         << R"(  "healthy": )" << (ready ? "true" : "false") << ",\n"
         << R"(  "mode": ")" << mode << '"' << ",\n"
         << R"(  "observed_process_monotonic_time_ns": )" << monotonic_now_ns() << ",\n"
         << R"(  "ready": )" << (ready ? "true" : "false") << ",\n"
         << R"(  "schema_version": 1,)" << '\n'
         << R"(  "service": ")" << service << '"' << '\n'
         << "}\n";
  return output.str();
}

void notify_systemd(const std::string_view message) noexcept {
  // The process owns no worker threads; NOTIFY_SOCKET is immutable after the
  // service manager starts it.
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  const auto* socket_name = std::getenv("NOTIFY_SOCKET");
  if (socket_name == nullptr || *socket_name == '\0') {
    return;
  }
  const int descriptor = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (descriptor < 0) {
    return;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const std::string_view name{socket_name};
  if (name.size() >= sizeof(address.sun_path)) {
    ::close(descriptor);
    return;
  }
  std::ranges::copy(name, address.sun_path);
  if (address.sun_path[0] == '@') {
    address.sun_path[0] = '\0';
  }
  const auto length =
      static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + name.size() + 1U);
  static_cast<void>(::sendto(descriptor, message.data(), message.size(), MSG_DONTWAIT,
                             reinterpret_cast<const sockaddr*>(&address), length));
  ::close(descriptor);
}

} // namespace

int main(const int argc, char** argv) {
  const auto options = parse_options(argc, argv);
  if (!options.valid) {
    std::cerr << R"({"level":"ERROR","service":")" << kRole
              << R"(","reason":"INVALID_ARGUMENT"})" << '\n';
    return 2;
  }
  std::vector<std::uint8_t> configuration;
  if (!read_configuration(options.configuration, configuration)) {
    std::cerr << R"({"level":"ERROR","service":")" << kRole
              << R"(","reason":"CONFIGURATION_REJECTED"})" << '\n';
    return 3;
  }
  const auto hash = digest_hex(aegis::common::sha256(configuration));
  const auto& build = aegis::common::current_build_info();
  if (build.live_trading_capable) {
    std::cerr << R"({"level":"ERROR","service":")" << kRole
              << R"(","reason":"LIVE_CAPABLE_BUILD_REJECTED"})" << '\n';
    return 4;
  }
  const auto health_path = options.status_root / (std::string{kRole} + ".json");
  if (!atomic_write(health_path,
                    health_json(kRole, build.version, hash, options.mode, true))) {
    return 5;
  }
  std::signal(SIGINT, request_stop);
  std::signal(SIGTERM, request_stop);
  notify_systemd("READY=1\nSTATUS=healthy; live transmission unavailable");
  std::cout << R"({"level":"INFO","service":")" << kRole << R"(","mode":")"
            << options.mode << R"(","event":"READY","configuration_sha256":")" << hash
            << R"("})" << '\n';
  while (!options.once && stop_requested == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    if (!atomic_write(health_path,
                      health_json(kRole, build.version, hash, options.mode, true))) {
      notify_systemd("STATUS=health publication failed");
      return 6;
    }
    notify_systemd("WATCHDOG=1");
  }
  notify_systemd("STOPPING=1\nSTATUS=graceful shutdown");
  static_cast<void>(atomic_write(
      health_path, health_json(kRole, build.version, hash, options.mode, false)));
  std::cout << R"({"level":"INFO","service":")" << kRole << R"(","event":"STOPPED"})"
            << '\n';
  return 0;
}
