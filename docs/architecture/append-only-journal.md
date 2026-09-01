# Append-only journal and audit trail

## Scope and implementation plan

The `aegis::journal` boundary durably records canonical market, decision, risk,
order, fill, position, configuration, operator, and safety evidence. It does not
make trading decisions and has no exchange transport. The current vertical
slice includes bounded asynchronous publication, segmented persistence, sparse
indexes, integrity/recovery scanning, verified prefix repair, explicit
retention, export, and replay extraction.

The integration sequence is:

1. component-local journals reserve and commit evidence before exposing a
   decision or command;
2. an adapter serializes that evidence as a canonical `AuditEnvelope` and calls
   `AsyncJournal::try_publish`;
3. queue acceptance is the bounded hot-path handoff, not a durability claim;
4. the single writer validates, frames, appends, indexes, and synchronizes under
   its configured policy;
5. readiness is revoked if mandatory publication or persistence cannot be
   proven;
6. replay verifies the complete chain before delivering any record callback.

Adapters from each existing component-local journal are deliberately the next
dependency. Adding them requires canonical envelope builders for any record
type not yet covered by the common helpers; raw same-build C++ structs must not
be promoted to an archival contract.

## Ownership and concurrency

`SegmentWriter` has exactly one owner and is never called from an execution
critical loop. `AsyncJournal` preallocates 256 cells of 16 KiB each. Multiple
producers reserve cells with a bounded compare/exchange loop and release-publish
metadata plus copied payload. One consumer acquire-loads the cell, calls the
writer, then release-publishes the reusable generation. Producers do not call a
clock, allocator, filesystem, logger, callback, Python, RPC, or database.

Mandatory queue full, producer contention, malformed metadata, oversize, disk
full, or writer error latches the service `UNSAFE`. Previously accepted records
are drained where possible, but shutdown returns `INHIBITED` so an operator
cannot confuse draining with restored readiness. Restart and fresh verification
are required. Advisory reject-newest mode never overwrites unread records.

## Format summary

All integers are fixed-width little-endian. Files are not serialized C++
structs and contain no padding dependency.

| Object | Size | Integrity and purpose |
| --- | ---: | --- |
| Segment header | 192 bytes | Magic/version, identity, first sequence, clocks, config/build/prior-chain SHA-256, CRC32C |
| Record header | 160 bytes | Kind/encoding/schema/IDs/times/sequences, prior and payload SHA-256, CRC32C |
| Record payload | 1–4 MiB | Canonical envelope, synthetic metadata, or explicitly opaque bytes |
| Commit trailer | 64 bytes | Repeated sequence/size/digest plus whole-frame CRC32C |
| Segment footer | 128 bytes | Sealed range/count/bytes/terminal digest/header digest plus CRC32C |
| Index header | 128 bytes | Segment/range/stride/count/terminal digest plus CRC32C |
| Index entry | 64 bytes | Sequence, file offset, record digest, CRC32C |

Files use `journal-%020u.open` while owned by a writer and `.ajl` after a clean
seal. Sparse indexes use `.idx.open` and `.idx`. A frame is committed only when
the complete trailer validates. The record SHA-256 covers the header, whose
payload SHA-256 binds the exact payload; the prior digest makes a chain within
and across segments. The segment footer prevents an incomplete seal from being
reported as clean.

Canonical `AuditEnvelope` payloads are structurally and semantically validated
and must agree with frame schema version, record kind, global event ID,
process-monotonic timestamp, and wall UTC timestamp. Raw packet metadata is
limited to synthetic/provider-neutral contracts in this repository. Licensed
payload retention requires an authorized adapter and data-governance policy.

## Durability and crash behavior

| Sync policy | `durable_at_return` | Intended use |
| --- | --- | --- |
| `every_record` | Yes after successful `fdatasync` | Highest-durability mandatory evidence |
| `periodic_records` | Yes only on the record that completes a sync batch | Reviewed, bounded loss window |
| `segment_close` | No until seal | Tests or explicitly lower-durability data |
| `none` | No | Benchmarks/tests only |

The writer may reserve a local-NVMe extent with `posix_fallocate` and uses a
single exclusive sequential file cursor plus `POSIX_FADV_SEQUENTIAL`. On crash,
reserved zero tail bytes and incomplete EOF frames in an unsealed `.open` file
are recoverable only after every preceding frame verifies. The same condition
in a sealed `.ajl` fails writer open, indexed read, replay, and retention; only
copy-repair may salvage its verified prefix. Recovery never scans past an
internal corrupt frame. Normal close truncates unused reservation, writes and
synchronizes the footer, finalizes the index, renames both, and synchronizes the
directory.

## Record-kind coverage

Stable kinds cover raw packet metadata, normalized market events, book validity,
feature snapshot metadata, model and ensemble forecasts, risk results, order
commands, gateway responses, fills, positions, kill switches, configuration,
operator actions, data quality, and clock quality. Canonical kind/type matching
is fail-closed. Configuration and operator records currently use an explicitly
versioned opaque payload until canonical contracts are added; they are still
checksummed and timestamped but are not advertised as canonical.

## Tools

After a development build, tools are under `build/dev/cpp/journal/`:

```bash
build/dev/cpp/journal/journal-inspect JOURNAL
build/dev/cpp/journal/journal-verify JOURNAL
build/dev/cpp/journal/journal-repair-copy SOURCE NEW_EMPTY_DIRECTORY
build/dev/cpp/journal/journal-export SOURCE export.jsonl
build/dev/cpp/journal/journal-replay SOURCE replay.ajrp [FIRST] [LAST]
```

`inspect` and `verify` report stable status, segment, sequence, and failure
offsets. `repair-copy` never opens the source for writing. `export` emits exact
payload bytes as hex and must therefore be handled as sensitive audit data.
`replay` emits a versioned length-prefixed stream only after full verification;
it cannot route or transmit orders.

## Retention and immutability

Retention is opt-in and refuses zero safety floors. It considers only verified
sealed segments old enough under caller-supplied UTC, preserves minimum segment
and record counts, deletes the derivative index and segment explicitly, and
synchronizes the directory. Production callers must first or atomically record
the planned deletion under a separately retained compliance policy. No current
default invokes retention.

Repair and recovery never modify source bytes. A repaired journal is a new
artifact with its own directory, header identities, and report; the damaged
original remains forensic evidence.

## Acceptance and limitations

Tests cover rotation, sparse seeking, restart sequence continuity, additive
payload schema versions, canonical validation, MPSC concurrency, full and
oversize overload, process-death/preallocated tails, partial records, payload
and index corruption, cross-segment copy-only repair, replay refusal, tools, and
retention. The frame parser has a libFuzzer target, and writer/publisher
benchmarks report throughput and hot-path allocation counts.

The current replay tool verifies and extracts records; it does not yet
orchestrate market-data, book, feature, ensemble, risk, OMS, position, and paper
gateway state machines or produce divergence reports. Index rebuilding after
loss is possible from the journal format but does not yet have a dedicated CLI.
External immutable archival, encryption, signatures, ACL deployment, storage
capacity qualification, and a retention approval service remain operational
dependencies.

See [ADR 0027](../adr/0027-checksummed-segmented-append-only-journal.md) and the
[recovery runbook](../operations/journal-recovery-runbook.md).
