# Feed-handler testing

The deterministic feed test seed remains `20260828`. Tests use injected process-
monotonic nanoseconds and never sleep or read a system clock.

The unit and integration suite covers:

- session, venue, and channel validation;
- malformed and corrupted SMX/1 packets;
- missing, duplicate, out-of-order, and configurable sequence wrap behavior;
- matching and conflicting A/B copies;
- simultaneous A/B gaps, synthetic retransmission, timeout, and snapshot rebuild;
- stale detection, receiver/journal/publisher overload, and fail-closed metrics;
- health transition publication and clean shutdown; and
- bounded decoder fuzzing with arbitrary packet bytes and metadata.

Run the C++ tests in the required isolated environment:

```bash
source tools/toolchain.sh
source /scratch/djy8hg/env/aegis_mx_contracts/bin/activate
cmake --preset ci
cmake --build --preset ci
ctest --preset ci
```

The feed benchmark reports receive-to-normalize throughput and p50, p95, p99,
and p99.9 latency counters from a fixed sample array. Its overload case records
receiver drops and verifies that no invalid stream is reported as healthy. System
clock reads are benchmark instrumentation only and are not part of the handler.
