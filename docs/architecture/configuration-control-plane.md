# Configuration and administrative control plane

## Scope and safety boundary

`control/config_service` implements the off-hot-path Go API, CLI, validation,
signing, immutable storage, approval workflow, rollback, emergency inhibit, and
local edge cache. It does not submit orders, contact exchanges, or enable live
trading. The authoritative C++ pre-trade risk engine still evaluates every
order-specific quantity, price, exposure, state, and limit check.

The control-plane process and edge are separated as follows:

```text
authenticated identity boundary
          |
          v
 Go Service API -> signed/hash-chained audit -> immutable configuration files
          |                                      |
          | asynchronous distribution            v
          +------------------------------> verified local edge cache
                                                     |
                                                     v
                                configuration hash + eligibility precheck
                                                     |
                                                     v
                                    deterministic C++ pre-trade risk
```

No arrow points from risk or OMS back to the service. Distribution, signature
verification, JSON decoding, filesystem sync, and audit recovery are outside
the hot path.

## Snapshot contract

Schema version 1 is a bounded complete snapshot containing:

- strategy enablement, quantity/position limits, model and session allowlists;
- venue enablement, simulation/paper mode, rates, and mandatory self-trade
  prevention policy;
- account and per-symbol risk limits in integer quantity/currency-nanosecond
  units;
- exact model semantic versions and OOD thresholds in parts per million;
- ensemble per-model caps in parts per million;
- absolute UTC trading sessions and scheduled-event entries;
- configured hierarchical kill switches; and
- principal-to-role bindings.

Timestamps explicitly use wall-clock UTC nanoseconds in control artifacts and
process-monotonic nanoseconds for edge-local age. Canonical ordering makes the
same logical snapshot hash identically. `ConfigurationVersion` is the leading
128 bits of the full SHA-256; decisions retain both.

## Workflow

| Operation | Required authority | Deterministic gates |
| --- | --- | --- |
| Dry run | None; no state change | Complete schema, bounds, references, canonical hash |
| Inspect active/artifact/audit | `VIEWER` | Short-lived identity, authority epoch, verified storage |
| Propose | `CONFIGURATION_AUTHOR` | Signed artifact, parent/revision, future stage, validity |
| Approve | distinct `APPROVER` | Pending proposal, author cannot approve |
| Activate | `ACTIVATOR` | Approved, staged time reached, active parent unchanged |
| Roll back | `ROLLBACK_OPERATOR` + distinct `APPROVER` | Expected active hash, older compatible valid target |
| Emergency kill | `KILL_OPERATOR` | Engage-only, scope/target, epoch, strict sequence |

Role changes are part of the same two-person snapshot, so no administrator can
grant themselves authority with a separate mutable side channel. The initial
bootstrap role file is deployment input and stops governing after revision 1
activates.

## Edge behavior

`EdgeCache.Evaluate` uses one immutable atomic pointer. It reports the exact
configuration ID, revision, 128-bit version, full hash, mode, and reason. It
permits the configuration layer only when:

- wall and monotonic time are non-regressing;
- signed validity and offline/risk age remain valid;
- the account, symbol, strategy, venue, model version, and session all match;
- no configured or emergency kill matches; and
- mode is `SIMULATION` or `PAPER`.

An allow result is not an order authorization. It is input to market state,
clock, feed/book, kill, limits, positions, deterministic risk, OMS fencing, and
gateway checks. A denial is final for new orders.

## Service contract

- Version/build: `CurrentBuildInfo` and `config-service version`.
- Health/readiness/configuration hash: `Service.Health` and
  `config-service health`.
- Structured output: all CLI results are JSON; errors contain no key bytes.
- Metrics: `Service.Metrics` and `config-service metrics` expose audit sequence,
  active revision/hash, pending-proposal count, and kill sequence without
  principal labels. Edge decisions expose bounded reason codes and the exact
  configuration hash.
- Graceful shutdown: `Service.Shutdown` prevents new mutations. The API has no
  background worker; each completed mutation has already released its file lock
  and fsynced its record. Canceled Go contexts are rejected before mutation.

See [ADR 0033](../adr/0033-signed-two-person-configuration-control.md), the
[operations runbook](../operations/configuration-control-runbook.md), and the
[test guide](../testing/configuration-control-testing.md).
