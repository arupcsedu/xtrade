# Long-duration PAPER soak testing

## Scope

The soak combines long-lived accelerated synthetic generation, exact same-seed
replay, a wall-paced synthetic phase, recurring full-system PAPER scenarios,
and certification probes. Its architecture and fixed acceptance thresholds are
defined by [ADR 0043](../adr/0043-bounded-two-tier-paper-soak.md).

It monitors:

- process RSS and file-descriptor count before, after warm-up, after every
  session, and at completion;
- sampled generator-call p50, p95, p99, p99.9, maximum, and first/second-half
  p99 latency;
- accelerated throughput, CPU time/utilization, cross-worker throughput
  imbalance, and event/hash counters;
- queue/drop counters, feed recovery, clock degradation, model deadline and
  restart behavior, stale-state restrictions, gateway restart, OMS recovery,
  journal integrity, position/P&L reconstruction, and leader fencing;
- duplicate order suppression, acknowledged-state preservation, and exact
  stream/final-book replay hashes.

## Bounded local gate

Use the required isolated environment:

```bash
export AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts
make paper-soak-smoke
```

The smoke profile runs 100,000 primary events, exact replay, one full-system
cycle, and five focused certification probes. It validates the workflow but is
not long-duration evidence.

A configurable local run uses:

```bash
AEGIS_SOAK_EVENTS=10000000 \
AEGIS_SOAK_SESSION_EVENTS=1000000 \
AEGIS_SOAK_CYCLES=3 \
AEGIS_SOAK_REALTIME_SECONDS=10 \
make paper-soak
```

## Parallel Slurm qualification run

The default launcher requests two nodes in the site `parallel` partition:

```bash
sbatch --wait tools/slurm/paper-soak.sbatch
```

Each worker generates one billion primary events, exactly regenerates those one
billion events for replay comparison, then runs 60 seconds of wall-paced
simulation. Each worker also runs ten complete PAPER acceptance cycles and five
certification probes per cycle. The aggregate must therefore record at least
two billion primary and two billion replay events and 20 complete PAPER cycles.

The scheduler job accepts environment overrides for capacity planning, but the
aggregate minimum is derived from the submitted event and cycle counts. An
early exit, missing worker, missing report, fewer events, or failed probe makes
the job fail.

## Evidence contract

The output directory contains:

- `worker-N-load.ndjson`: per-session raw event/hash/resource observations;
- `worker-N-load.json`: load counters, latency, resources, fixed thresholds,
  build metadata, and checks;
- `worker-N-acceptance-*.json` and `.md`: full-system cycle evidence;
- `worker-N-*-*.log`: exact certification-test output;
- `worker-N-probes.ndjson`: command status and SHA-256 records;
- `worker-N.json`: validated worker roll-up with raw artifact hashes;
- `paper-soak-report.json`: aggregate report conforming to
  [`paper-soak-report-v1.schema.json`](../../schemas/paper-soak-report-v1.schema.json);
- `stability-report.md`: human-readable conclusion and limitations.

Raw artifacts must be retained together. Do not copy only the Markdown summary,
change a threshold after execution, or classify a partial run as a pass.
The aggregate rechecks each worker's raw evidence hashes, requires no descriptor
variation across sessions, and limits the second-half RSS range to 1 MiB in
addition to the worker's 64 MiB post-warm-up ceiling.

## Recorded qualification evidence

The two-node qualification run for source revision `3b182c3c` completed as
Slurm job `19436646` with exit code `0:0` in 12m59s. It passed after generating
2,000,000,000 primary events, 2,000,000,000 exact replay events, and 120,000
wall-paced events. It also completed 20 full-system PAPER cycles and 100 focused
certification probes.

An earlier preflight attempt, job `19436560`, generated zero events and failed
in five seconds because Slurm's spooled script path was incorrectly treated as
the checkout path. The launcher now requires and validates `SLURM_SUBMIT_DIR`;
the failure log and remediation revision are retained in the execution
metadata.

- [Stability report](evidence/paper-soak-19436646/stability-report.md)
- [Machine-readable aggregate](evidence/paper-soak-19436646/paper-soak-report.json)
- [Scheduler and source metadata](evidence/paper-soak-19436646/execution-metadata.json)
- [Raw evidence directory](evidence/paper-soak-19436646/)

The committed evidence retains machine-readable per-cycle reports, raw session
and probe NDJSON, and exact test logs. Per-cycle Markdown renderings are omitted
because they duplicate the committed JSON; the aggregate Markdown is retained.

## Interpretation

A pass is infrastructure evidence for the repository-owned synthetic/PAPER
system. It makes no economic-value claim and does not qualify licensed exchange
or news protocols, a real broker, a physical NIC, site PTP, or production
hardware. Generator-call timing includes sampling overhead.
