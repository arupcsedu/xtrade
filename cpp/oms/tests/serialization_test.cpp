#include "aegis/oms/serialization.hpp"

#include "test_support.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <span>

namespace aegis::oms::test {
namespace {

[[nodiscard]] std::span<const std::uint8_t>
bytes(const flatbuffers::DetachedBuffer& buffer) noexcept {
  return {buffer.data(), buffer.size()};
}

TEST(OmsSerializationTest, RoundTripsVersionedLifecycleEvidence) {
  auto journal = std::make_unique<OmsJournal>();
  auto service = std::make_unique<DeterministicOms>(configuration(), *journal);
  const auto accepted = service->apply(accept(approval(), 1U));
  (void)service->apply(internal(InputKind::mark_ready, accepted.order_id, 2U));
  (void)service->apply(internal(InputKind::dispatch, accepted.order_id, 3U));
  const auto ack_input = venue(InputKind::acknowledgement, accepted.order_id, 4U, 1U);
  const auto acknowledged = service->apply(ack_input);
  OmsOrderSnapshot order{};
  ASSERT_TRUE(service->lookup_order(accepted.order_id, order));
  const auto contract = build_order_event_contract(ack_input, acknowledged, order);
  ASSERT_NE(contract.data(), nullptr);
  const common::AuditMetadata metadata{
      .envelope_id = common::GlobalEventId{300U, 1U},
      .session_id = risk::test::kSession,
      .configuration_version = risk::test::kConfiguration,
      .created_process_monotonic_time_ns = kNow + 10U,
      .recorded_wall_clock_utc_time_ns = 1'800'000'000'000'000'000LL,
      .previous_envelope_sha256 = {}};
  const auto envelope = common::build_size_prefixed_audit_envelope(
      bytes(contract), common::wire::RecordType::ORDER_EVENT, metadata);
  common::ValidatedAuditEnvelopeView view{};
  const auto validation =
      common::validate_size_prefixed_audit_envelope(bytes(envelope), &view);
  ASSERT_TRUE(validation.ok()) << static_cast<int>(validation.error());
  const auto* wire = view.record->payload_as_OrderEvent();
  ASSERT_NE(wire, nullptr);
  const auto* schema_version = wire->schema_version();
  ASSERT_NE(schema_version, nullptr);
  EXPECT_EQ(schema_version->minor(), common::kCurrentSchemaMinor);
  EXPECT_EQ(wire->current_state(), common::wire::OmsOrderStateCode::WORKING);
  EXPECT_EQ(wire->input_kind(), common::wire::OmsInputKindCode::ACKNOWLEDGEMENT);
  EXPECT_EQ(wire->outcome(), common::wire::OmsApplyStatusCode::APPLIED);
  EXPECT_EQ(wire->journal_sequence(), acknowledged.journal_sequence);
  EXPECT_EQ(wire->journal_record_hash(), acknowledged.journal_record_hash);
  EXPECT_EQ(wire->snapshot_hash(), acknowledged.snapshot_hash);
  const auto* client_order_id = wire->client_order_id();
  ASSERT_NE(client_order_id, nullptr);
  EXPECT_EQ(client_order_id->size(), 32U);
}

TEST(OmsSerializationTest, RejectsUnjournaledOrUnknownOrderEvidence) {
  const auto input = accept(approval(), 1U);
  const ApplyResult result{};
  const OmsOrderSnapshot order{};
  EXPECT_EQ(build_order_event_contract(input, result, order).data(), nullptr);
}

} // namespace
} // namespace aegis::oms::test
