# Chaos and fault-injection framework

| Field | Value |
| --- | --- |
| Status | Implemented deterministic reference framework |
| Decision | [ADR-0037](../adr/0037-deterministic-simulation-only-chaos.md) |
| Scenario catalog | [scenario-catalog-v1.json](../../infra/chaos/scenario-catalog-v1.json) |
| Result schema | [chaos-result-report-v1.schema.json](../../schemas/chaos-result-report-v1.schema.json) |
| Operations | [Chaos testing runbook](../operations/chaos-testing-runbook.md) |

## Safety boundary

The runner is an offline test orchestrator. It contains no network transmitter,
credential loader, exchange endpoint, OMS command method, or live mode.
Every result fixes its mode to SIMULATION. A scenario mutates only a fresh,
bounded in-memory signal map and then compares independently derived
observations with the catalog contract.

Host-pressure scenarios change quota signals. They do not busy-loop, allocate
large buffers, fill a disk, disconnect a real interface, alter system time, or
kill a process. Native component tests remain responsible for verifying the
corresponding queue, parser, state-machine, and recovery implementation.

## Execution model

1. Load no more than 1 MiB and 128 scenarios from the checked-in catalog.
2. Reject unsupported faults, duplicate IDs, duplicate fault definitions,
   invalid combination references, nonpositive deadlines, and unbounded
   iteration counts.
3. Inject the catalog's scalar signal into a new synthetic edge state.
4. Evaluate the independent detector activation rule.
5. Compare detection, transition, response, deadline, recovery, audit events,
   and order-blocking posture.
6. Clear the injected signal and require explicit recovery evidence.
7. Chain attempt evidence hashes and emit an atomic JSON report.

Time is logical nanoseconds. A content hash of seed, scenario ID, and iteration
provides bounded deterministic latency jitter. The report intentionally omits
wall-clock time, hostname, process timing, and unordered collections.

## Fault contracts

The JSON catalog is normative and gives the full automated-response,
recovery-criteria, and audit-event lists for every row. The table is a compact
review index; latency is the maximum permitted logical detection latency.

| Fault | Expected detection | State transition | Maximum | Recovery / audit outcome |
| --- | --- | --- | ---: | --- |
| Packet loss | SEQUENCE_GAP | feed HEALTHY to GAP_DETECTED | 100 us | retransmit, rebuild book, stabilize; gap/recovery events |
| Packet duplication | DUPLICATE_SEQUENCE | feed remains HEALTHY | 50 us | discard and account without sequence advance; duplicate events |
| Packet reordering | OUT_OF_ORDER_SEQUENCE | feed HEALTHY to GAP_DETECTED | 75 us | quarantine, replay, rebuild; ordering/recovery events |
| Feed gap | EXPLICIT_FEED_GAP | feed HEALTHY to RECOVERING | 50 us | gap replay and contiguous sequence; recovery events |
| Feed A/B disagreement | REDUNDANT_FEED_CONFLICT | feed HEALTHY to INVALID | 125 us | quarantine and verified snapshot rebuild; conflict events |
| Stale feed | FEED_AGE_EXCEEDED | feed HEALTHY to STALE | 10 ms | resubscribe and require a fresh snapshot; stale/recovery events |
| Clock drift | PTP_DRIFT_THRESHOLD_EXCEEDED | clock HEALTHY to UNSAFE | 1.5 ms | resynchronize and stabilize; unsafe/recovery events |
| Clock jump | BACKWARD_CLOCK_JUMP | clock HEALTHY to UNSAFE | 25 us | invalidate wall ordering and revalidate PTP; jump/recovery events |
| CPU starvation | HEARTBEAT_DEADLINE_MISSED | edge HEALTHY to DATA_DEGRADED | 7.5 ms | shed optional work, fence stale process, reconcile; heartbeat events |
| Memory pressure | MEMORY_RESERVE_THRESHOLD_EXCEEDED | resource HEALTHY to DEGRADED | 1.25 ms | reject allocation, validate pools, stabilize; pressure events |
| Queue saturation | QUEUE_CAPACITY_EXHAUSTED | bus HEALTHY to OVERLOADED | 100 us | apply backpressure, preserve mandatory audit, rebuild; overload events |
| GPU failure | GPU_INFERENCE_FAILURE | model HEALTHY to DEGRADED | 3.5 ms | invalidate GPU results and use deadline-bound CPU fallback; model events |
| LLM timeout | DEEP_STAGE_DEADLINE_MISSED | intelligence HEALTHY to DEGRADED | 5 s | discard late output and retain fast alert; timeout/recovery events |
| Malformed model response | FORECAST_SCHEMA_VALIDATION_FAILED | model HEALTHY to DEGRADED | 200 us | reject forecast and abstain affected ensemble; validation events |
| News contradiction | CONTRADICTORY_FACT_PROVENANCE | intelligence NORMAL to CONTRADICTED | 15 ms | retain both sources, lower confidence, adjudicate; provenance events |
| Gateway disconnect | SESSION_HEARTBEAT_LOST | gateway LOGGED_ON to DISCONNECTED | 1.5 ms | fence, reconnect, and reconcile open orders; session events |
| Reject burst | REJECT_RATE_LIMIT_EXCEEDED | gateway HEALTHY to DEGRADED | 6 ms | trip venue kill switch and require clearance; reject events |
| Drop-copy disagreement | FILL_RECONCILIATION_MISMATCH | portfolio RECONCILED to INCONSISTENT | 3 ms | quarantine, rebuild fills/positions/risk; reconciliation events |
| Journal truncation | FRAME_TRUNCATION_DETECTED | journal HEALTHY to UNSAFE | 200 us | preserve original and verify copy-only recovery; journal events |
| Configuration corruption | SIGNATURE_OR_HASH_INVALID | config ACTIVE to INVALID | 150 us | block, reject, and require signed approved recovery; config events |
| Split brain | FENCING_TOKEN_CONFLICT | leader ACTIVE_LEADER to FENCED | 25 us | fence both, issue higher witness token, reconcile; ownership events |
| Trading halt | OFFICIAL_HALT_RECEIVED | market NORMAL to HALTED | 50 us | block routing and require official recovery; halt events |
| Auction reopening | OFFICIAL_REOPENING_AUCTION | market HALTED to REOPENING | 50 us | block continuous orders, constrain auction policy, validate book; reopening events |

Every fault emits CHAOS_FAULT_INJECTED plus the fault-specific events in the
catalog. The following faults block new order admission: every market-data,
clock, edge-resource, gateway/risk/journal/configuration/leadership, halt, and
reopening fault except a safely discarded duplicate. GPU, LLM, malformed-model,
and news-contradiction faults degrade or abstain advisory paths and cannot
bypass risk or OMS.

## Nightly combinations

The nightly profile co-injects market-data overload, authority-integrity loss,
execution reconciliation loss, reopening with an unsafe clock, and combined
intelligence/model degradation. Each combination reports its scenario set,
attempt count, maximum detector latency, evidence-chain hash, and aggregate
safety state. Any combination containing a safety-critical fault must report
ORDERS_BLOCKED; advisory-only combinations report ANALYTICS_DEGRADED.

## Native integration boundaries

The production_hook field in each catalog record names the corresponding
repository boundary. It is traceability metadata, not dynamic code execution.
The current runner does not load shared libraries or start services. Physical
packet loss, kernel scheduling, memory exhaustion, GPU reset, disk power loss,
witness partition, broker drop copy, and licensed gateway behavior require
isolated staging infrastructure and provider-approved procedures.
