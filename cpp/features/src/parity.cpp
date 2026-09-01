#include "aegis/features/parity.hpp"

namespace aegis::features {

FeatureParityTestHarness::FeatureParityTestHarness(FeatureEngineConfig config)
    : online_(config), replay_(config) {}

FeatureResult
FeatureParityTestHarness::on_book(const BookFeatureEvent& event) noexcept {
  if (event_count_ == events_.size()) {
    return {.error = FeatureError::parity_capacity_exhausted,
            .state = online_.state(),
            .accepted_event_count = online_.accepted_event_count()};
  }
  const auto result = online_.on_book(event);
  if (result.accepted()) {
    events_[event_count_] = {
        .kind = FeatureInputKind::book, .book = event, .trade = {}};
    ++event_count_;
  }
  return result;
}

FeatureResult
FeatureParityTestHarness::on_trade(const TradeFeatureEvent& event) noexcept {
  if (event_count_ == events_.size()) {
    return {.error = FeatureError::parity_capacity_exhausted,
            .state = online_.state(),
            .accepted_event_count = online_.accepted_event_count()};
  }
  const auto result = online_.on_trade(event);
  if (result.accepted()) {
    events_[event_count_] = {
        .kind = FeatureInputKind::trade, .book = {}, .trade = event};
    ++event_count_;
  }
  return result;
}

ParityResult FeatureParityTestHarness::verify(
    const std::uint64_t process_monotonic_time_ns) noexcept {
  const auto reset = replay_.reset_for_replay();
  if (!reset.accepted()) {
    return {.error = reset.error, .event_count = event_count_};
  }
  for (std::size_t index = 0U; index < event_count_; ++index) {
    const auto& input = events_[index];
    const auto replay_result = input.kind == FeatureInputKind::book
                                   ? replay_.on_book(input.book)
                                   : replay_.on_trade(input.trade);
    if (!replay_result.accepted()) {
      return {.error = replay_result.error, .event_count = event_count_};
    }
  }
  FeatureSnapshot online_snapshot{};
  FeatureSnapshot replay_snapshot{};
  const auto online_error =
      online_.make_snapshot(process_monotonic_time_ns, online_snapshot);
  const auto replay_error =
      replay_.make_snapshot(process_monotonic_time_ns, replay_snapshot);
  if (online_error != FeatureError::none || replay_error != FeatureError::none) {
    return {.error = online_error != FeatureError::none ? online_error : replay_error,
            .event_count = event_count_};
  }
  const auto error = online_snapshot.stable_hash == replay_snapshot.stable_hash &&
                             online_snapshot.snapshot_id == replay_snapshot.snapshot_id
                         ? FeatureError::none
                         : FeatureError::parity_mismatch;
  return {.error = error,
          .online_hash = online_snapshot.stable_hash,
          .replay_hash = replay_snapshot.stable_hash,
          .event_count = event_count_};
}

std::size_t FeatureParityTestHarness::event_count() const noexcept {
  return event_count_;
}

FeatureEngine& FeatureParityTestHarness::online_engine() noexcept { return online_; }

} // namespace aegis::features
