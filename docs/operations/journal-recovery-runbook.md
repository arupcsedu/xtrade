# Journal corruption and recovery runbook

## Safety position

A journal alert revokes readiness for any process whose mandatory evidence may
be missing. Do not clear a kill switch, resume order production, delete a file,
truncate a tail, reuse an old writer epoch, or infer that a gateway command was
not sent. Recovery tools have no transmission capability, but recovered OMS
state still begins inhibited and requires venue reconciliation.

## Triage

1. Stop new intent production and preserve the affected process, journal mount,
   logs, build identity, configuration hash, writer instance, and clock state.
2. If orders may be working, engage the appropriate kill switch and follow OMS
   unknown-recovery and drop-copy reconciliation. Journal evidence alone cannot
   prove venue state.
3. Copy or snapshot the affected directory at the storage layer when policy
   allows. Record custody and hashes outside the affected journal.
4. Run read-only inspection and verification:

   ```bash
   build/dev/cpp/journal/journal-inspect /var/lib/aegis/journal
   build/dev/cpp/journal/journal-verify /var/lib/aegis/journal
   ```

5. Record status, segment, byte offset, expected sequence, valid prefix count,
   mount health, free space, filesystem/kernel errors, and whether the file is
   `.open` or sealed `.ajl`.

## Status interpretation

| Status | Meaning | Required action |
| --- | --- | --- |
| `OK` | Every requested sealed segment, frame, schema, sequence, and chain validated | Continue only if all other readiness checks pass |
| `TRUNCATED_TAIL` | A file ended during a frame or has a preallocated zero tail; only the preceding committed prefix is valid. An unsealed `.open` tail is recoverable for writer continuation, while a truncated sealed `.ajl` is corruption and cannot be reopened. | Preserve source, create a repair copy if replay is required, investigate process/storage loss |
| `CORRUPT_HEADER` | Segment identity/version/header CRC is invalid | Quarantine; no records from that segment are authoritative |
| `CORRUPT_RECORD` / `INVALID_PAYLOAD` | A complete frame, payload, footer, or canonical contract failed | Stop at the reported offset; quarantine and escalate |
| `SEQUENCE_GAP` / `CHAIN_MISMATCH` | Segments or frames are missing, duplicated, reordered, or altered | Treat continuity as lost; locate missing originals; do not bridge silently |
| `DISK_FULL` / `IO_ERROR` | Persistence could not complete | Keep service inhibited, correct storage, and restart through recovery |
| `CORRUPT_INDEX` | Sparse seek metadata is damaged or absent | Verify journal sequentially; rebuild a new derivative index when tooling is approved; never patch journal bytes |

## Copy-only salvage

Create a new, empty target on approved storage:

```bash
build/dev/cpp/journal/journal-repair-copy \
  /var/lib/aegis/journal \
  /var/lib/aegis/recovery/case-IDENTIFIER
build/dev/cpp/journal/journal-verify \
  /var/lib/aegis/recovery/case-IDENTIFIER
```

The command copies only the longest verified prefix and reports
`source_modified=false`, the source status, stop offset, and copied count. Hash
and retain both original and copy under the incident record. Never replace the
original in place or present the copy as proof of events after the stopping
offset.

## Replay and return to service

`journal-replay` extracts verified bytes for offline analysis. It does not
restore authority or reconcile a venue. Before a new edge process can become
ready, verify its build/configuration, clock, data, risk and kill state; restore
positions from all authoritative fills/drop copy; reconcile every potentially
live order; allocate a higher process/writer/leader epoch; and record operator
authorization. A missing audit interval cannot be waived by replay.

## Prohibited actions

- Do not use `truncate`, editor writes, in-place checksum repair, or filesystem
  rename to make `.open` appear sealed.
- Do not delete an index and claim the journal itself was verified.
- Do not skip a corrupt internal frame or synthesize a missing sequence.
- Do not export licensed/raw payloads outside their approved access boundary.
- Do not run retention during an incident or before evidence preservation.
