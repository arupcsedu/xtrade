#include "aegis/oms/journal.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <ostream>
#include <type_traits>

namespace aegis::oms {
namespace {

constexpr std::uint64_t kFnvOffset = 14'695'981'039'346'656'037ULL;
constexpr std::uint64_t kFnvPrime = 1'099'511'628'211ULL;
constexpr std::uint64_t kJournalMagic = 0x31304C4E4A534D4FULL; // OMSJNL01
constexpr std::uint64_t kJournalFormatVersion = 1U;

void mix(std::uint64_t& hash, const std::uint64_t value) noexcept {
  for (unsigned shift = 0U; shift < 64U; shift += 8U) {
    hash ^= (value >> shift) & 0xFFU;
    hash *= kFnvPrime;
  }
}

using JournalHeader = std::array<std::uint64_t, 8U>;

[[nodiscard]] std::uint64_t header_hash(JournalHeader value) noexcept {
  value.back() = 0U;
  std::uint64_t hash = kFnvOffset;
  for (const auto item : value) {
    mix(hash, item);
  }
  return hash == 0U ? 1U : hash;
}

[[nodiscard]] bool write_bytes(std::ostream& output, const void* data,
                               const std::size_t size) {
  output.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
  return output.good();
}

[[nodiscard]] bool read_bytes(std::istream& input, void* data, const std::size_t size) {
  input.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
  return input.good();
}

} // namespace

std::uint64_t stable_journal_record_hash(OmsJournalRecord value) noexcept {
  value.stable_hash = 0U;
  std::uint64_t hash = kFnvOffset;
  mix(hash, value.sequence);
  mix(hash, value.input.stable_hash);
  mix(hash, value.result.stable_hash);
  mix(hash, value.previous_record_hash);
  return hash == 0U ? 1U : hash;
}

std::uint64_t oms_journal_abi_tag() noexcept {
  std::uint64_t hash = kFnvOffset;
  mix(hash, sizeof(OmsJournalRecord));
  mix(hash, sizeof(OmsInput));
  mix(hash, sizeof(ApplyResult));
  mix(hash, alignof(OmsJournalRecord));
  mix(hash, std::is_trivially_copyable_v<OmsJournalRecord> ? 1U : 0U);
  return hash == 0U ? 1U : hash;
}

OmsJournal::OmsJournal() noexcept {
  for (auto& slot : slots_) {
    slot.published_sequence.store(0U, std::memory_order_relaxed);
  }
}

OmsJournal::Reservation OmsJournal::try_reserve() noexcept {
  const auto reserved = reserved_sequence_.load(std::memory_order_relaxed);
  if (reserved >= slots_.size()) {
    return {};
  }
  reserved_sequence_.store(reserved + 1U, std::memory_order_relaxed);
  return {.sequence = reserved + 1U, .valid = true};
}

void OmsJournal::commit(const Reservation reservation, const OmsInput& input,
                        const ApplyResult& result) noexcept {
  if (!reservation.valid || reservation.sequence == 0U ||
      reservation.sequence > slots_.size()) {
    return;
  }
  const auto index = static_cast<std::size_t>(reservation.sequence - 1U);
  auto& record = slots_[index].record;
  record = {};
  record.sequence = reservation.sequence;
  record.input = input;
  record.result = result;
  record.previous_record_hash = last_record_hash_.load(std::memory_order_relaxed);
  record.stable_hash = stable_journal_record_hash(record);
  record.result.journal_record_hash = record.stable_hash;
  slots_[index].published_sequence.store(reservation.sequence,
                                         std::memory_order_release);
  last_record_hash_.store(record.stable_hash, std::memory_order_release);
  published_sequence_.store(reservation.sequence, std::memory_order_release);
}

bool OmsJournal::read(const std::uint64_t sequence,
                      OmsJournalRecord& output) const noexcept {
  if (sequence == 0U || sequence > slots_.size() ||
      sequence > published_sequence_.load(std::memory_order_acquire)) {
    return false;
  }
  const auto index = static_cast<std::size_t>(sequence - 1U);
  if (slots_[index].published_sequence.load(std::memory_order_acquire) != sequence) {
    return false;
  }
  output = slots_[index].record;
  return stable_input_hash(output.input) == output.input.stable_hash &&
         stable_apply_result_hash(output.result) == output.result.stable_hash &&
         stable_journal_record_hash(output) == output.stable_hash;
}

std::uint64_t OmsJournal::size() const noexcept {
  return published_sequence_.load(std::memory_order_acquire);
}

std::uint64_t OmsJournal::last_record_hash() const noexcept {
  return last_record_hash_.load(std::memory_order_acquire);
}

bool OmsJournal::verify_chain() const noexcept {
  std::uint64_t previous{};
  OmsJournalRecord record{};
  for (std::uint64_t sequence = 1U; sequence <= size(); ++sequence) {
    if (!read(sequence, record) || record.sequence != sequence ||
        record.previous_record_hash != previous) {
      return false;
    }
    previous = record.stable_hash;
  }
  return previous == last_record_hash();
}

PersistenceStatus
OmsJournal::write_binary(std::ostream& output,
                         const std::uint64_t configuration_hash) const {
  if (configuration_hash == 0U || !verify_chain()) {
    return PersistenceStatus::corrupt_record;
  }
  JournalHeader header{
      kJournalMagic,      kJournalFormatVersion, sizeof(OmsJournalRecord), size(),
      configuration_hash, last_record_hash(),    oms_journal_abi_tag(),    0U};
  header.back() = header_hash(header);
  if (!write_bytes(output, header.data(), sizeof(header))) {
    return PersistenceStatus::io_error;
  }
  OmsJournalRecord record{};
  for (std::uint64_t sequence = 1U; sequence <= size(); ++sequence) {
    if (!read(sequence, record) || !write_bytes(output, &record, sizeof(record))) {
      return PersistenceStatus::io_error;
    }
  }
  return output.flush().good() ? PersistenceStatus::completed
                               : PersistenceStatus::io_error;
}

PersistenceStatus
OmsJournal::load_binary(std::istream& input,
                        const std::uint64_t expected_configuration_hash) {
  if (size() != 0U) {
    return PersistenceStatus::target_not_empty;
  }
  JournalHeader header{};
  if (!read_bytes(input, header.data(), sizeof(header))) {
    return PersistenceStatus::io_error;
  }
  if (header[0U] != kJournalMagic || header[1U] != kJournalFormatVersion ||
      header.back() != header_hash(header)) {
    return PersistenceStatus::invalid_header;
  }
  if (header[2U] != sizeof(OmsJournalRecord) || header[6U] != oms_journal_abi_tag()) {
    return PersistenceStatus::incompatible_abi;
  }
  if (header[4U] != expected_configuration_hash) {
    return PersistenceStatus::configuration_mismatch;
  }
  if (header[3U] > kOmsJournalCapacity) {
    return PersistenceStatus::capacity_exhausted;
  }
  std::uint64_t previous{};
  for (std::uint64_t sequence = 1U; sequence <= header[3U]; ++sequence) {
    OmsJournalRecord record{};
    if (!read_bytes(input, &record, sizeof(record))) {
      return PersistenceStatus::io_error;
    }
    if (record.sequence != sequence || record.previous_record_hash != previous ||
        stable_input_hash(record.input) != record.input.stable_hash ||
        stable_apply_result_hash(record.result) != record.result.stable_hash ||
        stable_journal_record_hash(record) != record.stable_hash) {
      return PersistenceStatus::corrupt_record;
    }
    const auto reservation = try_reserve();
    if (!reservation.valid) {
      return PersistenceStatus::capacity_exhausted;
    }
    commit(reservation, record.input, record.result);
    if (last_record_hash() != record.stable_hash) {
      return PersistenceStatus::corrupt_record;
    }
    previous = record.stable_hash;
  }
  if (last_record_hash() != header[5U]) {
    return PersistenceStatus::corrupt_record;
  }
  if (input.peek() != std::char_traits<char>::eof()) {
    return PersistenceStatus::trailing_data;
  }
  return PersistenceStatus::completed;
}

} // namespace aegis::oms
