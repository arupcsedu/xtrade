# Point-in-time feature events

## Contract

The offline event compiler authenticates accepted source reports, their source
manifests, and the referenced source objects before emitting
`feature-event-snapshot-v1`. It performs no network access and has no dependency
on risk, OMS, routing, gateways, or live trading.

The snapshot has two independent coverage families:

| Feature | Current source | Availability used | POC active window |
| --- | --- | --- | --- |
| `news_event_flag` | Accepted SEC filing metadata | Official SEC acceptance timestamp | 24 elapsed hours |
| `macro_event_flag` | Complete approved ALFRED release snapshots | Conservative `known_at_utc_ns` | 24 elapsed hours |

The `news_event_flag` name is retained for schema compatibility. In an
SEC-backed snapshot it means “official issuer disclosure active,” not broad
publisher-news coverage, text sentiment, or event direction. GDELT metadata may
be retained for research, but its adapter does not assert a precise historical
publication time. It is therefore not promoted into an intraday flag by this
compiler.

For each feature cutoff `t`, the event index returns:

- `1` when source coverage contains `t` and at least one event satisfies
  `available_at <= t` and `event_time <= t < valid_until`;
- `0` when source coverage contains `t` and no event is active; or
- `null` when no source coverage for that event family contains `t`.

Every active row retains the exact event record IDs and maximum event
availability timestamp. Dataset leakage validation rejects any row whose event
availability exceeds its knowledge cutoff.

The feature dataset's `event_snapshot_sha256` is the authenticated snapshot
document identity, not a separately recomputed subset hash. This binds the
source-report and source-manifest lineage as well as the event and coverage
rows into dataset identity.

Lookup uses an immutable interval index: a binary search locates the latest
eligible event time and prefix maximum validity bounds stop the reverse scan as
soon as no earlier interval can remain active. Runtime therefore scales with
the overlapping event set rather than rescanning the complete filing or macro
history for every minute row.

The offline builder also resolves each calendar-horizon target once per unique
minute endpoint and reuses that immutable result across instruments and build
passes. The cache has a fixed point-count bound and no eviction-order behavior;
per-instrument prior-session summaries are indexed once per session. Neither
optimization changes feature, label, dataset, or manifest identity.

## Commands

Plan a source-authentication and compile pass without publication:

```bash
export AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts
"$AEGIS_PYTHON_ENV/bin/aegis-feature-events"
```

Publish an immutable SEC-backed event snapshot:

```bash
"$AEGIS_PYTHON_ENV/bin/aegis-feature-events" --execute
```

Add macro coverage only after an accepted ALFRED run exists:

```bash
"$AEGIS_PYTHON_ENV/bin/aegis-feature-events" \
  --alfred-run-report /scratch/djy8hg/aegis_mx_poc_data/reports/alfred/RUN.json \
  --execute
```

Build a new immutable feature dataset with the published snapshot:

```bash
"$AEGIS_PYTHON_ENV/bin/aegis-real-features" \
  --event-snapshot /scratch/djy8hg/aegis_mx_poc_data/reports/feature-events/SNAPSHOT.json \
  --execute
```

The event snapshot is not retrofitted into an existing Parquet dataset. Adding
or changing source coverage creates a new dataset identity and new immutable
partitions.

## Fail-closed checks

Compilation rejects self-hash mismatch, object hash or size mismatch, path
escape or symlink, universe mismatch, SEC coverage that cannot be reproduced,
duplicate accession/event identity, incomplete ALFRED series coverage, event
timestamps outside asserted coverage, and integer overflow. An absent source is
represented as unavailable rather than zero.

## Limitations

- A binary presence flag does not encode event importance, direction, novelty,
  correction state, or surprise.
- SEC current issuer associations are not complete historical symbol truth; the
  resolved-instrument and source-universe hashes retain that limitation.
- ALFRED date-only availability cannot support same-release intraday macro
  claims. A separately approved timestamped source is required for that use.
- The fixed 24-hour windows are POC feature definitions requiring walk-forward
  evaluation; they do not assert predictive value.
