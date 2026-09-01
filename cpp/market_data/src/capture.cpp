#include "aegis/market_data/synthetic/capture.hpp"

#include "aegis/market_data/synthetic/hash.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <ios>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace aegis::market_data::synthetic {
namespace {

constexpr std::array<std::uint8_t, 4> kCaptureMagic{'S', 'M', 'X', 'C'};
constexpr std::uint8_t kCaptureMajor = 1U;
constexpr std::uint8_t kCaptureMinor = 0U;

void put_u16(const std::span<std::uint8_t> output, const std::size_t offset,
             const std::uint16_t value) noexcept {
  output[offset] = static_cast<std::uint8_t>(value >> 8U);
  output[offset + 1U] = static_cast<std::uint8_t>(value);
}

void put_u64(const std::span<std::uint8_t> output, const std::size_t offset,
             const std::uint64_t value) noexcept {
  for (std::size_t index = 0; index < 8U; ++index) {
    output[offset + index] =
        static_cast<std::uint8_t>(value >> static_cast<unsigned>((7U - index) * 8U));
  }
}

void put_i64(const std::span<std::uint8_t> output, const std::size_t offset,
             const std::int64_t value) noexcept {
  put_u64(output, offset, std::bit_cast<std::uint64_t>(value));
}

[[nodiscard]] std::uint16_t get_u16(const std::span<const std::uint8_t> input,
                                    const std::size_t offset) noexcept {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(input[offset]) << 8U) |
                                    static_cast<std::uint16_t>(input[offset + 1U]));
}

[[nodiscard]] std::uint32_t get_u32(const std::span<const std::uint8_t> input,
                                    const std::size_t offset) noexcept {
  std::uint32_t value = 0U;
  for (std::size_t index = 0; index < 4U; ++index) {
    value = static_cast<std::uint32_t>((value << 8U) | input[offset + index]);
  }
  return value;
}

[[nodiscard]] std::uint64_t get_u64(const std::span<const std::uint8_t> input,
                                    const std::size_t offset) noexcept {
  std::uint64_t value = 0U;
  for (std::size_t index = 0; index < 8U; ++index) {
    value = (value << 8U) | input[offset + index];
  }
  return value;
}

[[nodiscard]] std::int64_t get_i64(const std::span<const std::uint8_t> input,
                                   const std::size_t offset) noexcept {
  return std::bit_cast<std::int64_t>(get_u64(input, offset));
}

[[nodiscard]] bool read_exact(std::istream& stream,
                              const std::span<std::uint8_t> output) noexcept {
  stream.read(reinterpret_cast<char*>(output.data()),
              static_cast<std::streamsize>(output.size()));
  return stream.gcount() == static_cast<std::streamsize>(output.size());
}

[[nodiscard]] bool write_exact(std::ostream& stream,
                               const std::span<const std::uint8_t> input) noexcept {
  stream.write(reinterpret_cast<const char*>(input.data()),
               static_cast<std::streamsize>(input.size()));
  return stream.good();
}

[[nodiscard]] bool read_snapshot_levels(std::istream& stream,
                                        const std::string_view expected_label,
                                        std::span<BookLevel> levels,
                                        const bool descending) noexcept {
  std::string label;
  for (std::size_t index = 0; index < levels.size(); ++index) {
    auto& level = levels[index];
    if (!(stream >> label >> level.price_ticks >> level.quantity_units >>
          level.order_count) ||
        label != expected_label || level.price_ticks <= 0 ||
        level.quantity_units == 0U || level.order_count == 0U) {
      return false;
    }
    if (index != 0U) {
      const auto prior_price = levels[index - 1U].price_ticks;
      if ((descending && prior_price <= level.price_ticks) ||
          (!descending && prior_price >= level.price_ticks)) {
        return false;
      }
    }
  }
  return true;
}

[[nodiscard]] bool read_snapshot_book(std::istream& stream, BookSet& books) noexcept {
  std::string label;
  std::uint32_t venue{};
  std::uint32_t instrument{};
  unsigned status{};
  unsigned valid{};
  unsigned crossed{};
  std::uint32_t bid_count{};
  std::uint32_t ask_count{};
  if (!(stream >> label >> venue >> instrument >> status >> valid >> crossed >>
        bid_count >> ask_count) ||
      label != "book" || venue == 0U || instrument == 0U ||
      status < static_cast<unsigned>(TradingStatus::pre_open) ||
      status > static_cast<unsigned>(TradingStatus::closed) || valid > 1U ||
      crossed > 1U || bid_count > kMaximumBookDepth || ask_count > kMaximumBookDepth ||
      !books.register_instrument(venue, instrument)) {
    return false;
  }
  auto* book = books.find_mutable(venue, instrument);
  if (book == nullptr) {
    return false;
  }
  book->status = static_cast<TradingStatus>(status);
  book->valid = valid != 0U;
  book->crossed = crossed != 0U;
  book->bid_count = bid_count;
  book->ask_count = ask_count;
  return read_snapshot_levels(stream, "bid",
                              std::span<BookLevel>{book->bids}.first(book->bid_count),
                              true) &&
         read_snapshot_levels(stream, "ask",
                              std::span<BookLevel>{book->asks}.first(book->ask_count),
                              false);
}

} // namespace

EncodedCaptureHeader encode_capture_header(const CaptureHeader& header) noexcept {
  EncodedCaptureHeader encoded{};
  std::ranges::copy(kCaptureMagic, encoded.begin());
  encoded[4] = kCaptureMajor;
  encoded[5] = kCaptureMinor;
  put_u16(encoded, 6U, static_cast<std::uint16_t>(kCaptureHeaderBytes));
  put_u16(encoded, 8U, static_cast<std::uint16_t>(header.scenario));
  put_u16(encoded, 10U, 0U);
  put_u64(encoded, 12U, header.seed);
  put_u64(encoded, 20U, header.configuration_hash);
  put_u64(encoded, 28U, header.logical_event_count);
  put_u64(encoded, 36U, header.physical_packet_count);
  put_i64(encoded, 44U, header.start_exchange_time_ns);
  put_u64(encoded, 52U, header.stale_threshold_ns);
  put_u64(encoded, 60U, header.expected_book_hash);
  put_u64(encoded, 68U, stable_hash(std::span<const std::uint8_t>{encoded}.first(68U)));
  return encoded;
}

CaptureError decode_capture_header(const std::span<const std::uint8_t> bytes,
                                   CaptureHeader& output) noexcept {
  if (bytes.size() < kCaptureHeaderBytes) {
    return CaptureError::truncated;
  }
  if (bytes.size() > kCaptureHeaderBytes) {
    return CaptureError::trailing_bytes;
  }
  if (!std::equal(kCaptureMagic.begin(), kCaptureMagic.end(), bytes.begin())) {
    return CaptureError::invalid_magic;
  }
  if (bytes[4] != kCaptureMajor || bytes[5] != kCaptureMinor) {
    return CaptureError::unsupported_version;
  }
  if (get_u16(bytes, 6U) != kCaptureHeaderBytes) {
    return CaptureError::invalid_header_length;
  }
  if (get_u16(bytes, 10U) != 0U || get_u32(bytes, 76U) != 0U) {
    return CaptureError::invalid_flags;
  }
  const auto scenario_raw = get_u16(bytes, 8U);
  if (scenario_raw >
      static_cast<std::uint16_t>(Scenario::hidden_liquidity_replenishment)) {
    return CaptureError::invalid_scenario;
  }
  if (get_u64(bytes, 68U) != stable_hash(bytes.first(68U))) {
    return CaptureError::checksum_mismatch;
  }
  const CaptureHeader decoded{
      .scenario = static_cast<Scenario>(scenario_raw),
      .seed = get_u64(bytes, 12U),
      .configuration_hash = get_u64(bytes, 20U),
      .logical_event_count = get_u64(bytes, 28U),
      .physical_packet_count = get_u64(bytes, 36U),
      .start_exchange_time_ns = get_i64(bytes, 44U),
      .stale_threshold_ns = get_u64(bytes, 52U),
      .expected_book_hash = get_u64(bytes, 60U),
  };
  if (decoded.logical_event_count == 0U || decoded.physical_packet_count == 0U) {
    return CaptureError::invalid_count;
  }
  if (decoded.start_exchange_time_ns <= 0 || decoded.stale_threshold_ns == 0U) {
    return CaptureError::invalid_timestamp;
  }
  if (decoded.expected_book_hash == 0U) {
    return CaptureError::unfinalized;
  }
  output = decoded;
  return CaptureError::none;
}

std::optional<CaptureWriter> CaptureWriter::create(const std::filesystem::path& path,
                                                   const CaptureHeader& header) {
  if (header.logical_event_count == 0U || header.physical_packet_count == 0U ||
      header.start_exchange_time_ns <= 0 || header.stale_threshold_ns == 0U) {
    return std::nullopt;
  }
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream.is_open()) {
    return std::nullopt;
  }
  const auto encoded = encode_capture_header(header);
  if (!write_exact(stream, encoded)) {
    return std::nullopt;
  }
  return CaptureWriter(std::move(stream), header);
}

CaptureWriter::CaptureWriter(std::ofstream stream, CaptureHeader header) noexcept
    : stream_(std::move(stream)), header_(header) {}

CaptureError CaptureWriter::write(const CaptureRecord& record) noexcept {
  if (finalized_ || records_written_ >= header_.physical_packet_count ||
      record.capture_time_ns <= 0) {
    return CaptureError::invalid_count;
  }
  std::array<std::uint8_t, kCaptureRecordHeaderBytes> record_header{};
  put_i64(record_header, 0U, record.capture_time_ns);
  put_u16(record_header, 8U, static_cast<std::uint16_t>(record.packet.size()));
  put_u16(record_header, 10U, 0U);
  if (!write_exact(stream_, record_header) || !write_exact(stream_, record.packet)) {
    return CaptureError::write_failed;
  }
  ++records_written_;
  return CaptureError::none;
}

CaptureError CaptureWriter::finalize(const std::uint64_t expected_book_hash) noexcept {
  if (finalized_ || records_written_ != header_.physical_packet_count ||
      expected_book_hash == 0U) {
    return CaptureError::invalid_count;
  }
  header_.expected_book_hash = expected_book_hash;
  const auto encoded = encode_capture_header(header_);
  stream_.seekp(0, std::ios::beg);
  if (!stream_.good()) {
    return CaptureError::seek_failed;
  }
  if (!write_exact(stream_, encoded)) {
    return CaptureError::write_failed;
  }
  stream_.flush();
  if (!stream_.good()) {
    return CaptureError::write_failed;
  }
  finalized_ = true;
  return CaptureError::none;
}

std::uint64_t CaptureWriter::records_written() const noexcept {
  return records_written_;
}

std::optional<CaptureReader> CaptureReader::open(const std::filesystem::path& path,
                                                 CaptureError& error) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream.is_open()) {
    error = CaptureError::open_failed;
    return std::nullopt;
  }
  EncodedCaptureHeader bytes{};
  if (!read_exact(stream, bytes)) {
    error = CaptureError::truncated;
    return std::nullopt;
  }
  CaptureHeader header{};
  error = decode_capture_header(bytes, header);
  if (error != CaptureError::none) {
    return std::nullopt;
  }
  return CaptureReader(std::move(stream), header);
}

CaptureReader::CaptureReader(std::ifstream stream, CaptureHeader header) noexcept
    : stream_(std::move(stream)), header_(header) {}

const CaptureHeader& CaptureReader::header() const noexcept { return header_; }

CaptureError CaptureReader::next(CaptureRecord& output) noexcept {
  if (records_read_ >= header_.physical_packet_count) {
    return CaptureError::invalid_count;
  }
  std::array<std::uint8_t, kCaptureRecordHeaderBytes> record_header{};
  if (!read_exact(stream_, record_header)) {
    return CaptureError::truncated;
  }
  if (get_u16(record_header, 8U) != kPacketBytes) {
    return CaptureError::invalid_record_length;
  }
  if (get_u16(record_header, 10U) != 0U) {
    return CaptureError::invalid_flags;
  }
  CaptureRecord record{.capture_time_ns = get_i64(record_header, 0U)};
  if (record.capture_time_ns <= 0) {
    return CaptureError::invalid_timestamp;
  }
  if (!read_exact(stream_, record.packet)) {
    return CaptureError::truncated;
  }
  ++records_read_;
  output = record;
  return CaptureError::none;
}

CaptureError CaptureReader::finish() noexcept {
  if (records_read_ != header_.physical_packet_count) {
    return CaptureError::invalid_count;
  }
  char extra{};
  stream_.read(&extra, 1);
  if (stream_.gcount() != 0) {
    return CaptureError::trailing_bytes;
  }
  return stream_.eof() ? CaptureError::none : CaptureError::read_failed;
}

std::uint64_t CaptureReader::records_read() const noexcept { return records_read_; }

std::string_view capture_error_name(const CaptureError error) noexcept {
  switch (error) {
  case CaptureError::none:
    return "none";
  case CaptureError::invalid_config:
    return "invalid-config";
  case CaptureError::open_failed:
    return "open-failed";
  case CaptureError::read_failed:
    return "read-failed";
  case CaptureError::write_failed:
    return "write-failed";
  case CaptureError::seek_failed:
    return "seek-failed";
  case CaptureError::truncated:
    return "truncated";
  case CaptureError::trailing_bytes:
    return "trailing-bytes";
  case CaptureError::invalid_magic:
    return "invalid-magic";
  case CaptureError::unsupported_version:
    return "unsupported-version";
  case CaptureError::invalid_header_length:
    return "invalid-header-length";
  case CaptureError::invalid_flags:
    return "invalid-flags";
  case CaptureError::invalid_scenario:
    return "invalid-scenario";
  case CaptureError::invalid_count:
    return "invalid-count";
  case CaptureError::invalid_timestamp:
    return "invalid-timestamp";
  case CaptureError::checksum_mismatch:
    return "checksum-mismatch";
  case CaptureError::invalid_record_length:
    return "invalid-record-length";
  case CaptureError::packet_error:
    return "packet-error";
  case CaptureError::unfinalized:
    return "unfinalized";
  }
  return "invalid-error";
}

bool write_book_snapshot(const std::filesystem::path& path,
                         const GeneratorConfig& config, const BookSet& books) noexcept {
  std::ofstream stream(path, std::ios::trunc);
  if (!stream.is_open()) {
    return false;
  }
  stream << "SMXBOOK 1\n";
  stream << "config_hash " << std::hex << std::setw(16) << std::setfill('0')
         << config_hash(config) << '\n';
  stream << "book_hash " << std::hex << std::setw(16) << std::setfill('0')
         << books.stable_hash() << '\n';
  stream << std::dec << "book_count " << books.size() << '\n';
  for (std::size_t index = 0; index < books.size(); ++index) {
    const auto& book = books.at(index);
    stream << "book " << book.venue_number << ' ' << book.instrument_number << ' '
           << static_cast<unsigned>(book.status) << ' ' << book.valid << ' '
           << book.crossed << ' ' << book.bid_count << ' ' << book.ask_count << '\n';
    for (std::uint32_t level = 0; level < book.bid_count; ++level) {
      stream << "bid " << book.bids[level].price_ticks << ' '
             << book.bids[level].quantity_units << ' ' << book.bids[level].order_count
             << '\n';
    }
    for (std::uint32_t level = 0; level < book.ask_count; ++level) {
      stream << "ask " << book.asks[level].price_ticks << ' '
             << book.asks[level].quantity_units << ' ' << book.asks[level].order_count
             << '\n';
    }
  }
  stream.flush();
  return stream.good();
}

std::optional<std::uint64_t>
read_book_snapshot_hash(const std::filesystem::path& path) noexcept {
  std::ifstream stream(path);
  if (!stream.is_open()) {
    return std::nullopt;
  }
  std::string key;
  unsigned version_number{};
  if (!(stream >> key >> version_number) || key != "SMXBOOK" || version_number != 1U) {
    return std::nullopt;
  }
  std::uint64_t ignored_configuration_hash{};
  if (!(stream >> key >> std::hex >> ignored_configuration_hash) ||
      key != "config_hash") {
    return std::nullopt;
  }
  std::uint64_t book_hash{};
  if (!(stream >> key >> std::hex >> book_hash) || key != "book_hash" ||
      book_hash == 0U) {
    return std::nullopt;
  }
  std::uint64_t book_count{};
  if (!(stream >> std::dec >> key >> book_count) || key != "book_count" ||
      book_count == 0U || book_count > kMaximumInstruments) {
    return std::nullopt;
  }
  BookSet books;
  for (std::uint64_t book_index = 0; book_index < book_count; ++book_index) {
    if (!read_snapshot_book(stream, books)) {
      return std::nullopt;
    }
  }
  if (stream >> key) {
    return std::nullopt;
  }
  return books.stable_hash() == book_hash ? std::optional{book_hash} : std::nullopt;
}

} // namespace aegis::market_data::synthetic
