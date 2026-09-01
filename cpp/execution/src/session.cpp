#include "aegis/execution/session.hpp"

#include <limits>

namespace aegis::execution {

// The named outbound/inbound positions are part of the sequence API.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
SequenceManager::SequenceManager(const std::uint64_t first_outbound,
                                 const std::uint64_t first_inbound) noexcept {
  reset(first_outbound, first_inbound);
}

SequenceStatus SequenceManager::claim_outbound(std::uint64_t& sequence) noexcept {
  if (next_outbound_ == 0U) {
    return SequenceStatus::exhausted;
  }
  sequence = next_outbound_;
  if (next_outbound_ == std::numeric_limits<std::uint64_t>::max()) {
    next_outbound_ = 0U;
  } else {
    ++next_outbound_;
  }
  return SequenceStatus::in_order;
}

SequenceStatus SequenceManager::observe_inbound(const std::uint64_t sequence) noexcept {
  if (sequence == 0U || expected_inbound_ == 0U) {
    return SequenceStatus::invalid;
  }
  if (sequence == last_inbound_) {
    return SequenceStatus::duplicate;
  }
  if (sequence < expected_inbound_) {
    return SequenceStatus::out_of_order;
  }
  if (sequence > expected_inbound_) {
    return SequenceStatus::gap;
  }
  last_inbound_ = sequence;
  if (expected_inbound_ == std::numeric_limits<std::uint64_t>::max()) {
    expected_inbound_ = 0U;
  } else {
    ++expected_inbound_;
  }
  return sequence == 1U ? SequenceStatus::first : SequenceStatus::in_order;
}

std::uint64_t SequenceManager::next_outbound() const noexcept { return next_outbound_; }

std::uint64_t SequenceManager::expected_inbound() const noexcept {
  return expected_inbound_;
}

// The named outbound/inbound positions are part of the sequence API.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void SequenceManager::reset(const std::uint64_t first_outbound,
                            const std::uint64_t first_inbound) noexcept {
  next_outbound_ = first_outbound;
  expected_inbound_ = first_inbound;
  last_inbound_ = first_inbound > 1U ? first_inbound - 1U : 0U;
}

// Window duration and event capacity are intentionally adjacent configuration.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
FixedWindowRateLimiter::FixedWindowRateLimiter(
    const std::uint64_t window_ns, const std::uint32_t maximum_events) noexcept
    : window_ns_(window_ns), maximum_events_(maximum_events) {}
// NOLINTEND(bugprone-easily-swappable-parameters)

bool FixedWindowRateLimiter::valid() const noexcept {
  return window_ns_ != 0U && maximum_events_ != 0U &&
         maximum_events_ <= timestamps_.size();
}

void FixedWindowRateLimiter::expire(const std::uint64_t now_ns) noexcept {
  if (now_ns < last_now_ns_) {
    monotonic_ = false;
    return;
  }
  last_now_ns_ = now_ns;
  while (count_ != 0U) {
    const auto oldest = timestamps_[head_];
    if (now_ns < oldest || now_ns - oldest < window_ns_) {
      break;
    }
    head_ = (head_ + 1U) % static_cast<std::uint32_t>(timestamps_.size());
    --count_;
  }
}

bool FixedWindowRateLimiter::allow(const std::uint64_t now_ns) noexcept {
  if (!valid() || now_ns == 0U || !monotonic_) {
    return false;
  }
  expire(now_ns);
  if (!monotonic_ || count_ >= maximum_events_) {
    return false;
  }
  const auto tail = (head_ + count_) % static_cast<std::uint32_t>(timestamps_.size());
  timestamps_[tail] = now_ns;
  ++count_;
  return true;
}

std::uint32_t FixedWindowRateLimiter::occupancy(const std::uint64_t now_ns) noexcept {
  expire(now_ns);
  return count_;
}

void FixedWindowRateLimiter::reset() noexcept {
  timestamps_.fill(0U);
  last_now_ns_ = 0U;
  head_ = 0U;
  count_ = 0U;
  monotonic_ = true;
}

// Session identity and heartbeat values are a fixed construction contract.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
SessionLifecycle::SessionLifecycle(const std::uint64_t exchange_session_epoch,
                                   const std::uint64_t fencing_token,
                                   const std::uint64_t heartbeat_interval_ns,
                                   const std::uint64_t heartbeat_timeout_ns) noexcept
    : sequences_(1U, 1U), exchange_session_epoch_(exchange_session_epoch),
      fencing_token_(fencing_token), heartbeat_interval_ns_(heartbeat_interval_ns),
      heartbeat_timeout_ns_(heartbeat_timeout_ns),
      initialized_(exchange_session_epoch != 0U && fencing_token != 0U &&
                   heartbeat_interval_ns != 0U &&
                   heartbeat_timeout_ns > heartbeat_interval_ns) {}

bool SessionLifecycle::initialized() const noexcept { return initialized_; }

SessionState SessionLifecycle::state() const noexcept { return state_; }

SessionReason SessionLifecycle::reason() const noexcept { return reason_; }

bool SessionLifecycle::ready() const noexcept {
  return initialized_ && state_ == SessionState::active;
}

void SessionLifecycle::transition(const SessionState state, const SessionReason reason,
                                  const std::uint64_t now_ns) noexcept {
  state_ = state;
  reason_ = reason;
  ++transition_count_;
  if (state == SessionState::active && last_inbound_ns_ == 0U) {
    last_inbound_ns_ = now_ns;
  }
}

bool SessionLifecycle::request_logon(const std::uint64_t now_ns) noexcept {
  if (!initialized_ || now_ns == 0U || state_ != SessionState::stopped) {
    return false;
  }
  transition(SessionState::starting, SessionReason::operator_start, now_ns);
  transition(SessionState::logging_on, SessionReason::operator_start, now_ns);
  return true;
}

bool SessionLifecycle::accept_logon(const std::uint64_t now_ns) noexcept {
  if (now_ns == 0U || state_ != SessionState::logging_on) {
    return false;
  }
  last_inbound_ns_ = now_ns;
  last_outbound_ns_ = now_ns;
  transition(SessionState::active, SessionReason::logon_accepted, now_ns);
  return true;
}

bool SessionLifecycle::request_logoff(const std::uint64_t now_ns) noexcept {
  if (now_ns == 0U ||
      (state_ != SessionState::active && state_ != SessionState::recovering &&
       state_ != SessionState::halted)) {
    return false;
  }
  transition(SessionState::logging_off, SessionReason::operator_logoff, now_ns);
  return true;
}

bool SessionLifecycle::complete_logoff(const std::uint64_t now_ns) noexcept {
  if (now_ns == 0U || state_ != SessionState::logging_off) {
    return false;
  }
  transition(SessionState::stopped, SessionReason::logoff_complete, now_ns);
  return true;
}

bool SessionLifecycle::begin_recovery(const std::uint64_t now_ns,
                                      const SessionReason reason) noexcept {
  if (now_ns == 0U ||
      (state_ != SessionState::active && state_ != SessionState::halted)) {
    return false;
  }
  transition(SessionState::recovering, reason, now_ns);
  return true;
}

// The named time and sequence positions are part of the recovery API.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool SessionLifecycle::complete_recovery(
    const std::uint64_t now_ns, const std::uint64_t next_inbound_sequence) noexcept {
  if (now_ns == 0U || next_inbound_sequence == 0U ||
      state_ != SessionState::recovering) {
    return false;
  }
  sequences_.reset(sequences_.next_outbound(), next_inbound_sequence);
  last_inbound_ns_ = now_ns;
  transition(SessionState::active, SessionReason::recovery_complete, now_ns);
  return true;
}

void SessionLifecycle::halt(const std::uint64_t now_ns,
                            const SessionReason reason) noexcept {
  if (state_ != SessionState::stopped) {
    transition(SessionState::halted, reason, now_ns);
  }
}

void SessionLifecycle::shutdown(const std::uint64_t now_ns) noexcept {
  transition(SessionState::stopped, SessionReason::shutdown, now_ns);
}

SequenceStatus SessionLifecycle::claim_outbound(const std::uint64_t now_ns,
                                                std::uint64_t& sequence) noexcept {
  if (state_ != SessionState::active || now_ns == 0U || now_ns < last_outbound_ns_) {
    return SequenceStatus::invalid;
  }
  const auto status = sequences_.claim_outbound(sequence);
  if (status == SequenceStatus::in_order) {
    last_outbound_ns_ = now_ns;
  } else if (status == SequenceStatus::exhausted) {
    halt(now_ns, SessionReason::inbound_sequence_gap);
  }
  return status;
}

// The named time and sequence positions are part of the inbound API.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
SequenceStatus
SessionLifecycle::observe_inbound(const std::uint64_t now_ns,
                                  const std::uint64_t sequence) noexcept {
  if ((state_ != SessionState::active && state_ != SessionState::recovering) ||
      now_ns == 0U || now_ns < last_inbound_ns_) {
    return SequenceStatus::invalid;
  }
  const auto status = sequences_.observe_inbound(sequence);
  if (status == SequenceStatus::first || status == SequenceStatus::in_order ||
      status == SequenceStatus::duplicate) {
    last_inbound_ns_ = now_ns;
  } else if (status == SequenceStatus::gap || status == SequenceStatus::out_of_order ||
             status == SequenceStatus::exhausted) {
    halt(now_ns, SessionReason::inbound_sequence_gap);
  }
  return status;
}
// NOLINTEND(bugprone-easily-swappable-parameters)

bool SessionLifecycle::heartbeat_due(const std::uint64_t now_ns) const noexcept {
  return state_ == SessionState::active && now_ns >= last_outbound_ns_ &&
         now_ns - last_outbound_ns_ >= heartbeat_interval_ns_;
}

bool SessionLifecycle::check_heartbeat_timeout(const std::uint64_t now_ns) noexcept {
  if (state_ != SessionState::active || now_ns < last_inbound_ns_) {
    return state_ == SessionState::halted;
  }
  if (now_ns - last_inbound_ns_ <= heartbeat_timeout_ns_) {
    return false;
  }
  halt(now_ns, SessionReason::heartbeat_timeout);
  return true;
}

SessionSnapshot SessionLifecycle::snapshot() const noexcept {
  SessionSnapshot result{.state = state_,
                         .reason = reason_,
                         .exchange_session_epoch = exchange_session_epoch_,
                         .fencing_token = fencing_token_,
                         .next_outbound_sequence = sequences_.next_outbound(),
                         .expected_inbound_sequence = sequences_.expected_inbound(),
                         .last_inbound_process_monotonic_time_ns = last_inbound_ns_,
                         .last_outbound_process_monotonic_time_ns = last_outbound_ns_,
                         .transition_count = transition_count_};
  result.stable_hash = stable_session_snapshot_hash(result);
  return result;
}

} // namespace aegis::execution
