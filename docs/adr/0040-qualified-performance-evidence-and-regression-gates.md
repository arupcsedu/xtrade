# ADR 0040: Qualified performance evidence and regression gates

- Status: Accepted
- Date: 2026-09-06

## Context

Component-local Google Benchmarks existed, but their outputs did not form one
auditable platform report. They also did not consistently preserve observations,
identify simulated versus real-NIC measurements, or prevent comparisons across
different hardware. A single average is especially unsafe for a latency-sensitive
system because it can conceal tail regressions and queue saturation.

## Decision

Aegis-MX uses a versioned performance report and a separate immutable NDJSON sample
stream. Each case is identified by scenario and stage. The report contains p50,
p95, p99, p99.9, maximum, throughput, thread CPU utilization, allocation count,
queue occupancy, and packet drops. Cache misses are collected through Linux perf
counters when permitted; otherwise the value is null and the denial is recorded.

The benchmark process pins itself to one CPU selected from its inherited affinity
mask and verifies the resulting mask. It performs an untimed warm-up before taking
samples. A run with fewer than 10,000 latency samples per measured case is a smoke
run and cannot be used as a qualified regression baseline.

The checked-in comparison policy permits 15% regression at p50 through p99, 25%
at p99.9, 30% at maximum, and 12% throughput loss. Hot-path allocations and packet
drops under ordinary load remain zero-tolerance. The comparison rejects mismatched
CPU models, build types, measurement sources, pinning state, or real-NIC status.
Baseline approval and storage are operator-controlled; CI never silently promotes
its current result into a baseline.

Halt and unsafe-data scenarios are successful only when executable work is blocked.
Their tick-to-intent and downstream send latency are reported as `SAFETY_BLOCKED`,
never as zero.

## Consequences

- Raw evidence is larger than summary-only benchmark output but remains auditable.
- Shared CI runners produce useful smoke evidence but generally are not suitable
  baseline hosts.
- Kernel policy may make cache-miss counters unavailable; this is visible instead
  of being guessed.
- Synthetic packet timing cannot be presented as NIC-to-user-space performance.

## Alternatives rejected

- Comparing Google Benchmark means: does not characterize operation-level tails.
- Checking in one developer workstation baseline: creates false precision and
  encourages invalid cross-host comparisons.
- Treating blocked sends as zero latency: would reward unsafe behavior.
