#include "aegis/market_data/synthetic/canonical.hpp"

#include "aegis/common/audit_envelope.hpp"
#include "aegis/common/identifiers.hpp"
#include "aegis/mx/contracts/v1/contract_record_generated.h"

#include <bit>
#include <cstdint>
#include <span>

#include <flatbuffers/flatbuffer_builder.h>

namespace aegis::market_data::synthetic {
namespace {

namespace wire = ::aegis::mx::contracts::v1;

constexpr std::uint64_t kEventIdDomain = 0x534D'5845'5645'4E54ULL;
constexpr std::uint64_t kSessionIdDomain = 0x534D'5853'4553'534EULL;
constexpr std::uint64_t kVenueIdDomain = 0x534D'5856'454E'5545ULL;
constexpr std::uint64_t kInstrumentIdDomain = 0x534D'5849'4E53'5452ULL;
constexpr std::uint64_t kChannelIdDomain = 0x534D'5843'4841'4E4CULL;
constexpr std::uint64_t kConfigurationDomain = 0x534D'5843'4F4E'4647ULL;

[[nodiscard]] wire::SchemaVersion version() noexcept {
  return {aegis::common::kCurrentSchemaMajor, aegis::common::kCurrentSchemaMinor,
          aegis::common::kCurrentSchemaPatch};
}

[[nodiscard]] constexpr wire::DataQualityCode
to_wire(const DataQuality quality) noexcept {
  return static_cast<wire::DataQualityCode>(quality);
}

[[nodiscard]] constexpr wire::BookSide to_wire(const Side side) noexcept {
  return static_cast<wire::BookSide>(side);
}

[[nodiscard]] constexpr wire::BookAction to_wire(const BookAction action) noexcept {
  return static_cast<wire::BookAction>(action);
}

[[nodiscard]] constexpr wire::TradingStatusCode
to_wire(const TradingStatus status) noexcept {
  return static_cast<wire::TradingStatusCode>(status);
}

[[nodiscard]] constexpr wire::TradeSide trade_side(const Side side) noexcept {
  return side == Side::bid ? wire::TradeSide::BUY : wire::TradeSide::SELL;
}

[[nodiscard]] constexpr wire::AuctionSide auction_side(const Side side) noexcept {
  if (side == Side::bid) {
    return wire::AuctionSide::BUY_IMBALANCE;
  }
  if (side == Side::ask) {
    return wire::AuctionSide::SELL_IMBALANCE;
  }
  return wire::AuctionSide::PAIRED;
}

[[nodiscard]] constexpr std::int64_t
last_good_time(const std::int64_t timestamp, const DataQuality quality) noexcept {
  if (quality == DataQuality::valid || timestamp <= 1) {
    return timestamp;
  }
  return timestamp - 1;
}

} // namespace

flatbuffers::DetachedBuffer build_market_event_contract(const SyntheticEvent& event,
                                                        const GeneratorConfig& config) {
  flatbuffers::FlatBufferBuilder builder{2048U};
  const auto schema_version = version();
  const wire::GlobalEventId event_id{kEventIdDomain, event.global_ordinal};
  const wire::SessionId session_id{kSessionIdDomain, config.seed ^ config_hash(config)};
  const wire::VenueId venue_id{kVenueIdDomain, event.venue_number};
  const wire::InstrumentId instrument_id{kInstrumentIdDomain, event.instrument_number};
  const wire::ChannelId channel_id{kChannelIdDomain, event.channel_number};
  const wire::ExchangeEventTimeNs exchange_time{event.exchange_event_time_ns};
  const wire::NicReceiveTimeNs nic_time{event.nic_receive_time_ns};
  const wire::ProcessMonotonicTimeNs monotonic_time{event.process_monotonic_time_ns};
  const wire::WallClockUtcTimeNs wall_time{event.exchange_event_time_ns};
  const auto last_good_exchange_value =
      last_good_time(event.exchange_event_time_ns, event.data_quality);
  const auto last_good_nic_value =
      last_good_time(event.nic_receive_time_ns, event.data_quality);
  const wire::ExchangeEventTimeNs last_good_exchange_time{last_good_exchange_value};
  const wire::NicReceiveTimeNs last_good_nic_time{last_good_nic_value};

  const auto data_quality = wire::CreateDataQualityState(
      builder, &schema_version, &venue_id, &channel_id, to_wire(event.data_quality),
      &monotonic_time, &last_good_exchange_time, &last_good_nic_time, 0U, 0U,
      config.stale_threshold_ns);
  const auto clock_quality = wire::CreateClockQualityState(
      builder, &schema_version, &channel_id, wire::ClockQualityCode::SYNCHRONIZED,
      &monotonic_time, &wall_time, 0, 1U, 1'000U);

  wire::MarketPayload payload_type = wire::MarketPayload::NONE;
  flatbuffers::Offset<void> payload{};
  switch (event.type) {
  case NativeMessageType::add_order:
  case NativeMessageType::cancel_order:
  case NativeMessageType::modify_order:
  case NativeMessageType::price_level: {
    const auto update = wire::CreateBookUpdate(
        builder, &schema_version, &event_id, &session_id, &venue_id, &instrument_id,
        &channel_id, event.channel_sequence, &exchange_time, &nic_time, &monotonic_time,
        to_wire(event.side), to_wire(event.action), wire::PriceUnit::TICKS,
        event.price_ticks, wire::QuantityUnit::INSTRUMENT_UNITS,
        event.level_quantity_units, event.order_count);
    payload_type = wire::MarketPayload::BookUpdate;
    payload = update.Union();
    break;
  }
  case NativeMessageType::trade: {
    const auto trade = wire::CreateTradeEvent(
        builder, &schema_version, &event_id, &session_id, &venue_id, &instrument_id,
        &channel_id, event.channel_sequence, &exchange_time, &nic_time, &monotonic_time,
        trade_side(event.side), wire::PriceUnit::TICKS, event.price_ticks,
        wire::QuantityUnit::INSTRUMENT_UNITS, event.quantity_units,
        event.auxiliary_value);
    payload_type = wire::MarketPayload::TradeEvent;
    payload = trade.Union();
    break;
  }
  case NativeMessageType::quote: {
    const auto quote = wire::CreateQuoteEvent(
        builder, &schema_version, &event_id, &session_id, &venue_id, &instrument_id,
        &channel_id, event.channel_sequence, &exchange_time, &nic_time, &monotonic_time,
        wire::PriceUnit::TICKS, event.price_ticks,
        std::bit_cast<std::int64_t>(event.auxiliary_value),
        wire::QuantityUnit::INSTRUMENT_UNITS, event.quantity_units,
        event.level_quantity_units);
    payload_type = wire::MarketPayload::QuoteEvent;
    payload = quote.Union();
    break;
  }
  case NativeMessageType::auction_imbalance: {
    const auto imbalance = wire::CreateAuctionImbalance(
        builder, &schema_version, &event_id, &session_id, &venue_id, &instrument_id,
        &channel_id, event.channel_sequence, &exchange_time, &nic_time, &monotonic_time,
        auction_side(event.side), wire::PriceUnit::TICKS, event.price_ticks,
        wire::QuantityUnit::INSTRUMENT_UNITS, event.quantity_units,
        event.auxiliary_value);
    payload_type = wire::MarketPayload::AuctionImbalance;
    payload = imbalance.Union();
    break;
  }
  case NativeMessageType::trading_status: {
    const auto status = wire::CreateTradingStatus(
        builder, &schema_version, &event_id, &session_id, &venue_id, &instrument_id,
        &channel_id, event.channel_sequence, &exchange_time, &nic_time, &monotonic_time,
        to_wire(event.status), event.auxiliary_code);
    payload_type = wire::MarketPayload::TradingStatus;
    payload = status.Union();
    break;
  }
  }

  const auto market_event = wire::CreateMarketEvent(
      builder, &schema_version, &event_id, &session_id, &venue_id, &instrument_id,
      &channel_id, event.channel_sequence, &exchange_time, &nic_time, &monotonic_time,
      data_quality, clock_quality, payload_type, payload);
  const auto record = wire::CreateContractRecord(
      builder, &schema_version, &event_id, wire::RecordType::MARKET_EVENT,
      wire::ContractPayload::MarketEvent, market_event.Union());
  wire::FinishContractRecordBuffer(builder, record);
  return builder.Release();
}

flatbuffers::DetachedBuffer
build_market_event_envelope(const SyntheticEvent& event, const GeneratorConfig& config,
                            const aegis::common::Sha256Digest& previous_digest) {
  const auto contract = build_market_event_contract(event, config);
  const auto configuration_hash = config_hash(config);
  const aegis::common::AuditMetadata metadata{
      .envelope_id = {kEventIdDomain, event.global_ordinal},
      .session_id = {kSessionIdDomain, config.seed ^ configuration_hash},
      .configuration_version = {kConfigurationDomain, configuration_hash},
      .created_process_monotonic_time_ns = event.process_monotonic_time_ns,
      .recorded_wall_clock_utc_time_ns = event.exchange_event_time_ns,
      .previous_envelope_sha256 = previous_digest,
  };
  return aegis::common::build_size_prefixed_audit_envelope(
      std::span<const std::uint8_t>{contract.data(), contract.size()},
      wire::RecordType::MARKET_EVENT, metadata);
}

} // namespace aegis::market_data::synthetic
