# Long-duration PAPER soak runbook

## Safety boundary

The soak must use a default non-live build and the repository-owned synthetic
feed and PAPER gateway. It requires no credential. Do not add a broker endpoint,
licensed protocol bytes, production configuration, or operator live authority.
The load worker rejects a build that reports live capability.

## Preflight and submission

From a clean source revision:

```bash
export AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts
make paper-soak-smoke
sbatch --wait tools/slurm/paper-soak.sbatch
```

Record the Slurm job ID, source revision, output directory, node list, requested
event count, cycle count, and any environment overrides. Do not reuse an output
directory from a previous run.

## Live monitoring

Use scheduler read-only commands such as `squeue -j JOB_ID` and `sstat -j
JOB_ID.batch`. Do not alter affinity, thresholds, seeds, or binaries after
submission. Worker logs are written only between subprocesses; the accelerated
event loop performs no log or disk write.

The expected default completion is:

- two worker summaries and raw session streams;
- 100 primary/replay sessions per worker;
- 20 full-system acceptance cycle reports total;
- 100 focused certification probe logs total; and
- one passing aggregate JSON and Markdown report.

## Failure triage

1. Treat a nonzero Slurm status or absent aggregate report as a failed run.
2. Find the first false aggregate check in `paper-soak-report.json`.
3. Inspect the referenced worker report, then its per-session NDJSON or first
   failing cycle/probe log.
4. Preserve all raw artifacts and the exact seed before reproducing locally.
5. For stream/book hash mismatch, replay the named session with its recorded
   seed and configuration hash.
6. For memory, descriptor, latency, or imbalance failure, preserve `/proc`,
   scheduler, node, kernel, and hardware metadata. Do not raise the threshold to
   make the result pass.
7. For any invalid state, duplicate emission, lost acknowledgement, journal
   error, stale-state use, or reconciliation failure, keep the system classified
   unsafe and follow the owning component runbook.

## Recovery and retention

The test emits no external order and changes no administrative configuration.
Canceling the scheduler job is safe, but the resulting evidence is incomplete
and cannot pass. Retain the aggregate, worker summaries, raw NDJSON, acceptance
reports, probe logs, scheduler output, and SHA-256 digests under an immutable
run identifier.

