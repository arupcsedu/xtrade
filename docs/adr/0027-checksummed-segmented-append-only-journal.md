# ADR 0027: Checksummed segmented append-only audit journal

- Status: Accepted
- Date: 2026-08-31
- Owners: edge-core, replay, OMS, risk, compliance, operations

## Context

Risk, OMS, gateway, market-data, feature, and model components already retain
bounded in-process evidence before exposing a result. Those component-local
journals are not durable, interoperable archives. A colocated process must move
that evidence to local NVMe without putting a disk operation, allocator, clock
read, logger, or wait in a producer's critical path. A crash may interrupt any
write, and an audit tool must never turn ambiguous bytes into a valid event or
silently rewrite the source.

Canonical cross-version records are size-prefixed FlatBuffer `AuditEnvelope`
bytes under ADR 0004. A durable file additionally needs framing, segment
lifecycle, ordering, integrity, recovery, seeking, and explicit durability
semantics. The repository has no licensed venue protocol, so raw records may
contain only synthetic bytes or provider-neutral metadata unless an authorized
adapter supplies its own lawful payload.

## Decision

Use a repository-owned little-endian journal format, version 1.0:

1. A segment begins with a 192-byte header that binds format version, segment
   and first-record sequences, creation clocks, writer instance, configuration
   SHA-256, build SHA-256, prior terminal record SHA-256, sync policy, and a
   CRC32C.
2. Each record has a 160-byte header, bounded payload, and 64-byte commit
   trailer. The header binds a stable record-kind code, payload encoding,
   priority, payload schema version, global event ID, process-monotonic and wall
   UTC timestamps, source sequence, journal sequence, prior record SHA-256, and
   payload SHA-256. Its CRC32C detects header damage. The record SHA-256 covers
   the complete header and therefore transitively binds its payload digest and
   chain predecessor. The trailer repeats sequence, length, and record digest;
   a CRC32C covers header, payload, and normalized trailer.
3. A record exists only when its entire valid commit trailer is readable. An
   incomplete EOF header, payload, or trailer in an unsealed `.open` file is a
   recoverable truncated tail. The same condition in a sealed `.ajl` is a
   fail-closed integrity error for writer open, indexed reads, replay, and
   retention; copy-repair may preserve only the verified prefix. A checksum,
   semantic schema, ordering, or chain failure in a complete frame is corruption
   and stops recovery.
4. A normal close appends a checksummed 128-byte segment footer, synchronizes
   journal and sparse index, renames `.open` to `.ajl`, and synchronizes the
   directory. Sealed journal bytes are never reopened for writing. A crashed
   `.open` file is retained unchanged; a replacement writer scans its committed
   prefix and starts a higher segment ID.
5. A derivative sparse `.idx` file records sequence, offset, record digest, and
   entry CRC at a configured stride. Its header is finalized at seal. Index
   loss or corruption cannot make a journal record valid and does not justify
   journal modification; it is rebuildable from verified frames.
6. The writer is single-owner. It opens a unique new segment with `O_EXCL`,
   optionally reserves extents with `posix_fallocate`, advises sequential IO,
   uses vectored sequential writes, maps `ENOSPC`/`EDQUOT` distinctly, and uses
   `fdatasync` according to an explicit policy. The result says whether a record
   was durable when append returned. `none` and `segment_close` are permitted
   for tests or explicitly accepted lower-durability data only; mandatory audit
   deployments use `every_record` or a reviewed periodic bound.
7. Hot producers publish into a preallocated 256-cell MPSC queue with a 16 KiB
   per-record ceiling, sequence counters, acquire/release publication, and 64
   bounded producer retries. Publication performs no allocation, disk or clock
   call, logging, callback, sleep, or network operation. One asynchronous
   consumer validates and appends. A mandatory oversize, full, contention, or
   persistence failure latches `UNSAFE`; no overwrite or implicit retry occurs.
   Advisory records may use explicit reject-newest mode, but the rejection is
   counted and returned.
8. Canonical envelope publication revalidates FlatBuffer structure, semantic
   rules, SHA-256, schema version, record-kind compatibility, global event ID,
   and both recorded clock values. Opaque and synthetic encodings remain
   checksummed but are not mistaken for canonical contracts.
9. Retention is disabled by default. An explicit policy can remove only fully
   verified sealed segment/index pairs older than its floor while retaining
   configured segment and record minima. The caller must record the returned
   removal report in a separately retained audit domain. Active, corrupt, or
   ambiguous segments are never retention candidates.
10. Repair is copy-only. It reads the source with read-only descriptors, copies
    the longest verified prefix into a new empty directory, reports the exact
    stopping offset and source status, and never truncates, renames, patches, or
    deletes an original journal.

`journal-replay` is a verified byte-stream extractor only. It has no strategy,
risk, OMS, gateway, credential, network, or transmission dependency. Faithful
decision re-execution and divergence reporting remain the next Phase 10 layer.

## Consequences

The authoritative persistent chain remains portable and independent of
component-local C++ ABIs. Incomplete power-loss tails are unambiguous, while a
valid checksum never hides a gap or malformed canonical payload. Sync policy
makes the crash-durability window measurable instead of implied.

The hot queue uses about 4 MiB plus alignment and caps a single hot publication
at 16 KiB. Larger artifacts must be content-addressed and journal bounded
metadata, or be handed to the writer from an explicitly off-hot-path bounded
pipeline. SHA-256 and semantic verification execute on the writer thread and
reduce raw disk throughput; that is intentional audit work and is benchmarked.

CRC32C and SHA-256 provide accidental/tamper evidence, not identity or
authorization. File ownership, encryption at rest, access control, signed
configuration, retention approval, and external immutable archival are
deployment responsibilities. A future authenticated segment signature is
additive metadata, not permission to weaken the current checks.

## Rejected alternatives

- Writing from risk/OMS/gateway threads was rejected because disk latency and
  filesystem failure are unbounded hot-path dependencies.
- Unbounded queues, overwrite-oldest rings, and blocking-on-full were rejected
  because each can hide lost mandatory evidence or violate bounded execution.
- Treating component-local raw C++ structs as the archival format was rejected
  because padding, ABI, compiler, and version changes prevent durable replay.
- Updating a damaged file in place was rejected because it destroys forensic
  evidence and makes the repair itself unauditable.
- JSON framing was rejected for authoritative bytes due to ambiguous numeric
  representation, allocation, size, and parser surface. JSONL is export only.

## Review triggers

Revisit this decision before format major 2, multiple journal consumers,
payloads above 4 MiB, queue resizing, direct IO/io_uring, segment signatures or
encryption, automatic remote archival, retention without a separately durable
audit, or any live-trading readiness claim.
