# ADR 0043: Bounded two-tier PAPER soak validation

- Status: Accepted
- Date: 2026-09-07

## Context

A long-duration stability test must expose cumulative leaks and latency drift
without weakening the deterministic safety checks already exercised by the
full-system PAPER acceptance runner. Sending billions of events through a test
that reconstructs every component for each 16-scenario pass would test process
startup more than steady-state bounded behavior. Conversely, timing only the
synthetic generator would not prove risk, OMS, gateway, recovery, or fencing
behavior.

Wall-clock pacing is needed to exercise real-time scheduling, but correctness
cannot depend on host timing. Performance observations are host-specific and
must not be represented as real-NIC or production latency qualification.

## Decision

The soak uses two independently failing tiers:

1. A long-lived C++ load worker generates a bounded sequence of deterministic
   synthetic sessions. It consumes each event immediately, verifies shape and
   content hash, samples generator-call latency, records RSS and open file
   descriptors after each session, and regenerates every session from the same
   seed for exact stream and final-book comparison. A separately counted phase
   paces repository-owned synthetic events against `steady_clock`.
2. A Python control-path runner repeatedly starts the 16-scenario full-system
   PAPER acceptance executable and focused certification tests for feed gap
   recovery, model process restart/disable behavior, gateway restart/fills, OMS
   recovery, and witness-fenced leader failover. These child processes cannot
   contact a network destination and a live-capable build makes the worker fail.

The default two-node Slurm profile processes one billion primary events per
worker and one exact replay of each event. Its aggregate gate therefore requires
two billion primary events, two billion replay events, at least 20 full-system
cycles, all checks true, no telemetry drops, no descriptor growth, at most 64
MiB post-warm-up RSS growth per load worker, and no more than 50% sampled p99
drift plus a 500 ns measurement-noise allowance. Worker throughput may differ by
at most 2:1. The second half of per-session RSS samples may span at most 1 MiB;
every session sample must have the same file-descriptor count. Thresholds are
emitted in the machine report and cannot be changed by the worker CLI.

All generated records carry explicit seeds, configuration hashes, build
revision, host identity, counters, hashes, and SHA-256 evidence references.
The aggregate distinguishes mean per-worker throughput from concurrent
multi-node throughput; cross-worker imbalance uses the individual worker rates.
Report generation is control-path file I/O after or outside event generation;
it is not part of an execution hot path.

## Consequences

This design detects cumulative generator-state memory growth while repeatedly
revalidating the complete PAPER safety boundary and recovery behavior. Process
restarts intentionally contain certification-test state, so child-process RSS
is reported as a maximum rather than misrepresented as a leak time series.

The billion-event tier is synthetic generator and final-book infrastructure
validation, not a claim that billions of orders traversed every strategy or
execution component. Configuration updates are deterministic test-version
changes. Licensed feeds, news providers, broker sessions, physical gateway
fencing, real NICs, and target production hardware remain outside this test.
