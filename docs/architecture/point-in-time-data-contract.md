# Point-in-time data contract

| Field | Value |
| --- | --- |
| Contract version | `1.0.0` |
| Implementation | `python/research/aegis_mx_research` |
| Execution class | Offline research and replay validation |
| Deterministic test seed | `20260831` |
| Live/order capability | None |

## Temporal model

Every timestamp is a positive signed integer count of wall-clock UTC
nanoseconds. The fields are not interchangeable:

| Field | Meaning |
| --- | --- |
| `event_time_ns` | Time the economic, reference-data, document, or membership event is effective |
| `publication_time_ns` | Time the source states it published this exact version |
| `receive_time_ns` | Time Aegis-MX received this exact version |
| `processing_time_ns` | Time validation and normalization completed locally |
| `revision_time_ns` | Time assigned to this exact correction/vintage by the normalized source contract |
| `valid_from_ns` | Inclusive start of business effectiveness |
| `valid_to_ns` | Exclusive end of business effectiveness, or absent for open-ended validity |

Receipt cannot precede publication, processing cannot precede receipt, and
revision time cannot precede publication. Event time must lie inside the
record's validity interval. Event time may be later than publication, as with an
announced future symbol change or split.

The record becomes available to research at:

```text
available_at_ns = max(processing_time_ns, revision_time_ns)
```

This prevents an early local receipt from exposing a later provider revision
and prevents unprocessed input from appearing in a dataset.

## Identity, source, and versions

Each record carries a bounded unique record ID, canonical logical key, record
kind, schema version, positive contiguous record version, immutable payload,
and `DataSource`. Source identity includes provider-neutral source category,
provider and document identifiers, authentication state, and nonzero SHA-256 of
the exact content. Licensed content is not stored by this contract.

The first series version is one. Later versions increase by exactly one and
have strictly increasing revision time. News corrections and filing amendments
also link to the prior accepted record ID. An old version remains immutable and
queryable after a correction.

## Supported record families

| Family | Canonical series key | Point-in-time use |
| --- | --- | --- |
| Corporate action | action ID | Complete action history and effective adjustment timing |
| Symbol mapping | symbol | Symbol-to-stable-instrument mapping by validity interval |
| Delisting | stable instrument ID | Historical universe and survivorship checks |
| Index membership | index ID + instrument ID | Constituents effective and known at the requested time |
| Analyst estimate | instrument + metric + fiscal period | Consensus/estimate vintages without post-event data |
| Macro vintage | series + reference period | Initial release and subsequent revisions |
| News revision | document ID | Corrections without erasing the original alert/document |
| Filing revision | accession ID | Amendments linked to the prior accepted filing |

Payloads use integer values and explicit units. Provider-specific field
mapping, symbology, calendars, correction chains, entitlements, and retention
rules must be supplied by an authorized adapter.

## Query semantics

- `as_known_at(t)` returns the latest version of every logical series whose
  availability is less than or equal to `t`.
- `latest_available_before(t)` applies a strict availability boundary.
- `revisions_after(t)` returns immutable records whose revision time is greater
  than `t`, ordered deterministically.
- `membership_at(t, index_id=..., known_at_ns=k)` evaluates effective index
  membership at `t` using only versions available by `k`; `k` defaults to `t`.
- `symbol_mapping_at(t, symbol=..., known_at_ns=k)` applies the same two-time
  rule to symbology.
- `corporate_action_history(instrument_id, known_at_ns=k)` returns the latest
  known revision of each action without applying actions outside validity.

All results have stable ordering. Missing, malformed, unknown-version, or
broken-lineage data fails closed rather than being inferred.

## Dataset and leakage contract

Every sample binds its event time, feature interval, label interval, split,
exact record IDs, and complete historical universe. A manifest binds the
dataset identity, creation time, split method, optional embargo, universe index,
and random seed when one was used.

| Rejection | Rule |
| --- | --- |
| Future macro revision | Macro vintage availability is after the feature/event cutoff |
| Future constituent | Membership was unavailable or ineffective at sample time |
| Post-event estimate | Estimate availability is after the event cutoff |
| Future corporate action | Action was unavailable or applied before effectiveness |
| Survivorship bias | Sample universe differs from membership known and effective then |
| Label overlap | Labels overlap features or a later split after embargo |
| Randomized time split | Train/validation/test assignment used random observation mixing |
| Nonchronological split | A later partition contains an earlier/equal event time |
| Missing provenance | A sample references an unknown immutable record ID |

Validation returns every deterministic finding, sorted by reason, sample, and
record. `validate_or_raise` and `LeakageReport.raise_if_invalid` reject the
entire dataset when any finding exists.

## Compatibility and limitations

Version `1.0.0` is an in-process Python reference contract. Additive payload
families require a schema-version review, ADR update, compatibility tests, and
explicit leakage policy. Existing meanings and query boundary inclusivity may
not change in place.

The store is bounded but uses Python allocation and linear reference scans. It
does not provide a database, licensed provider connector, distributed snapshot,
retention enforcement, or hot-path cache. Production analytical storage must
preserve these semantics and prove query parity before replacing it.
