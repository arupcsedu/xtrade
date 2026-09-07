# Colocated edge deployment testing

The deployment tests are offline and need neither root access nor secrets. They validate all six profiles, unit contracts, forbidden live flags, deterministic rendering, host incompatibility, health freshness, rollback integrity, and package reproducibility.

```bash
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make edge-validate
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make edge-package
```

Host qualification is separate. Copy `infra/edge/host/host-facts.example.json`, replace it with audited facts, and run:

```bash
edge-deployment validate-host --profile paper --facts /path/to/host-facts.json
```

Staging and production-disabled must reject their checked-in placeholder NIC. Passing them requires a reviewed, uncommitted/site-managed profile derived from the signed configuration workflow.

```bash
edge-deployment validate-profile --profile-path resolved-staging.json
edge-deployment validate-host --profile-path resolved-staging.json \
  --facts reviewed-host-facts.json
edge-deployment render --profile-path resolved-staging.json \
  --output build/site-edge-render
edge-deployment package-rollback --profile-path resolved-staging.json \
  --facts reviewed-host-facts.json --source-date-epoch 1600000000 \
  --output dist/edge/site-rollback.tar.gz
```

## Performance validation

Before acceptance, bind the benchmark process to each intended CPU and NUMA node and record:

- feed packet-to-normalized-event p50/p95/p99/p99.9, throughput, loss, and queue occupancy;
- book, feature, model, ensemble, risk, OMS, and send-stage latency percentiles;
- journal append/fsync/rotation latency and backlog during sustained load;
- enqueue/dequeue and shared-state contention tails;
- PTP offset/drift, hardware timestamp continuity, and clock-call cost;
- cache misses, context switches, page faults, NUMA misses, CPU frequency, thermal throttling, IRQ placement, NIC drops, and NVMe saturation;
- graceful shutdown, restart recovery, failover fencing, and chaos detection latency.

Run cold-start, warm steady-state, burst, message-loss, journal-pressure, and 24-hour soak variants. Use fixed seeds and capture the code commit, build manifest, profile hash, configuration hash, host facts, firmware, kernel command line, compiler, and benchmark JSON. Thresholds belong in approved site configuration and may not be silently weakened.

No benchmark result establishes permission for live trading or predictive economic value.
