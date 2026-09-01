#include "aegis/oms/journal.hpp"

#include "test_support.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

namespace aegis::oms::test {
namespace {

TEST(OmsJournalTest, RecordsAreAppendOnlyAndHashChained) {
  auto journal = std::make_unique<OmsJournal>();
  const auto input = accept(approval(), 1U);
  ApplyResult result{.status = ApplyStatus::applied,
                     .reason = OmsReason::accepted,
                     .journal_sequence = 1U,
                     .snapshot_sequence = 1U,
                     .snapshot_hash = 9U};
  result.stable_hash = stable_apply_result_hash(result);
  const auto reservation = journal->try_reserve();
  ASSERT_TRUE(reservation.valid);
  journal->commit(reservation, input, result);
  ASSERT_EQ(journal->size(), 1U);
  EXPECT_TRUE(journal->verify_chain());
  OmsJournalRecord record{};
  ASSERT_TRUE(journal->read(1U, record));
  EXPECT_EQ(record.input.stable_hash, input.stable_hash);
  EXPECT_EQ(record.stable_hash, journal->last_record_hash());
  EXPECT_FALSE(journal->read(2U, record));
}

TEST(OmsJournalTest, BinaryPersistenceRoundTripsWithAbiAndConfigurationFence) {
  auto source_journal = std::make_unique<OmsJournal>();
  auto service = std::make_unique<DeterministicOms>(configuration(), *source_journal);
  const auto order_id = create_working_order(*service);
  (void)service->apply(fill(order_id, 20U, 2U, 1U, 2U));
  ASSERT_TRUE(source_journal->verify_chain());

  std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
  ASSERT_EQ(source_journal->write_binary(stream, configuration().stable_hash),
            PersistenceStatus::completed);
  stream.seekg(0);
  auto restored = std::make_unique<OmsJournal>();
  ASSERT_EQ(restored->load_binary(stream, configuration().stable_hash),
            PersistenceStatus::completed);
  EXPECT_EQ(restored->size(), source_journal->size());
  EXPECT_EQ(restored->last_record_hash(), source_journal->last_record_hash());
  EXPECT_TRUE(restored->verify_chain());
}

TEST(OmsJournalTest, BinaryPersistenceRejectsCorruptionAndWrongConfiguration) {
  auto source = std::make_unique<OmsJournal>();
  auto service = std::make_unique<DeterministicOms>(configuration(), *source);
  (void)service->apply(accept(approval(), 1U));
  std::stringstream encoded(std::ios::in | std::ios::out | std::ios::binary);
  ASSERT_EQ(source->write_binary(encoded, configuration().stable_hash),
            PersistenceStatus::completed);
  const auto bytes = encoded.str();

  std::stringstream wrong_configuration(bytes, std::ios::in | std::ios::out |
                                                   std::ios::binary);
  auto wrong_target = std::make_unique<OmsJournal>();
  EXPECT_EQ(
      wrong_target->load_binary(wrong_configuration, configuration().stable_hash ^ 1U),
      PersistenceStatus::configuration_mismatch);

  auto corrupted_bytes = bytes;
  corrupted_bytes.back() = static_cast<char>(corrupted_bytes.back() ^ 0x01);
  std::stringstream corrupted(corrupted_bytes,
                              std::ios::in | std::ios::out | std::ios::binary);
  auto corrupt_target = std::make_unique<OmsJournal>();
  EXPECT_EQ(corrupt_target->load_binary(corrupted, configuration().stable_hash),
            PersistenceStatus::corrupt_record);
}

} // namespace
} // namespace aegis::oms::test
