# News and filings intelligence pipeline

The intelligence pipeline is a near-real-time Python component outside the
execution hot path. It accepts bounded untrusted documents and publishes
versioned `EventIntelligenceRecord` notifications only. It has no OMS, risk-
approval, execution, gateway, or operator-authority dependency.

```mermaid
flowchart LR
    P[Provider interface] --> A[Authenticate and timestamp]
    A --> H[Hash and deduplicate]
    H --> S[Strip active content and isolate instructions]
    S --> R[Detect type and language; resolve entities]
    R --> F[Fast rules and fact evidence]
    F --> N[Immutable fast notification]
    F --> Q[Bounded deep queue]
    Q --> D[Tool-less deep adjudicator]
    D --> V[Strict output and provenance validation]
    V --> N2[Immutable linked deep notification]
    N --> C[Downstream intelligence cache or journal]
    N2 --> C
```

## Provider and authentication boundary

`DocumentProvider` returns at most the requested bounded batch. Every
`SourceDocument` identifies the provider and document, declares its content
type, records provider-event and receipt UTC times, and carries authentication
evidence without the credential or raw signature.

- `MockProvider` is deterministic and simulation-only.
- `FilesystemReplayProvider` reads regular nonsymlink JSON envelopes in lexical
  order and validates the exact content SHA-256.
- `OfficialPublicSourceProvider` permits credential-free HTTPS endpoints only,
  rejects host-changing redirects, requires verified TLS peer evidence, bounds
  responses, and delegates transport to an injected reviewed client.

No production provider client is included. Provider terms, identity headers,
rate limits, correction identifiers, timestamp definitions, retention, and
redistribution remain a licensed/legal integration boundary.

## Untrusted-text boundary

The source byte hash is calculated before decoding. Decoding accepts strict
UTF-8 only. The sanitizer removes script, style, iframe, object, embed, form,
template, SVG, MathML, metadata, and all attributes. It creates two immutable
views:

- retained inert text, used only to verify exact fact excerpts and offsets;
- analysis text, from which instruction-like and encoded-instruction segments
  have been removed.

The deep request places analysis text in an explicitly named untrusted-data
field under a fixed configuration-owned policy identity. It provides no tool
interface. An extracted fact must reference one or more excerpts whose exact
text and offsets match retained text. Raw source or retained text is never a
metric label or structured-log field.

## Deterministic fast stage

Document type, coarse language, event type, registry entities, one-hop corporate
relationships, facts, novelty, materiality, source trust, and uncertainty are
calculated with deterministic integer rules. Sixteen event classes are
supported, including rumor and correction. Unregistered ticker-like tokens do
not resolve.

Exact content hashes deduplicate across providers. Provider/document identity
tracks revisions. Corrections link to the prior revision. Conflicting values for
the same entity and fact name link both immutable event identities. State uses
fixed capacities and deterministic oldest-entry eviction.

## Deep stage and publication

The fast alert is published before deep work enters a bounded queue. Deep work
has a monotonic deadline. Output received after the deadline, with mismatched
source/parent provenance, invalid scores/enums, or unsupported evidence is
discarded. The fast record remains published.

Valid deep output publishes a new record with `parent_fast_global_event_id` and
an adjudication of confirmed, refined, disputed, or corrected. A changed or
disputed classification links back through the contradiction set. Publication
is append-only; no API can erase a fast alert.

`EventIntelligenceRecord` is an advisory event notification. Any downstream
component deriving expected return, volatility, volume, spread, or factor
values must publish a separate `ModelForecast`. Cached forecasts remain the
only model input available to the colocated edge.

## Service and failure behavior

`IntelligencePipeline` exposes health/readiness, deterministic build version,
configuration SHA-256, fixed-cardinality Prometheus metrics, bounded structured
audit entries, and deadline-bounded graceful shutdown. Invalid provider state
fails readiness closed. Queue saturation rejects optional work explicitly and
never retries synchronously.

See [ADR 0015](../adr/0015-untrusted-intelligence-fast-deep-pipeline.md), the
[failure runbook](../operations/news-intelligence-failure-runbook.md), and the
[testing guide](../testing/news-intelligence-testing.md).

Domain specialists consume only normalized, sanitized, evidence-backed facts.
See the [earnings specialist](earnings-specialist.md) and
[macroeconomic release specialist](macroeconomic-release-specialist.md). Neither
specialist can connect to order entry.

Evidence-backed earnings releases may be normalized by the separate
[earnings-event specialist](earnings-specialist.md). It receives only structured
facts and exact sanitized excerpts and publishes through `ModelForecast`; raw
document text and order entry remain outside that boundary.
