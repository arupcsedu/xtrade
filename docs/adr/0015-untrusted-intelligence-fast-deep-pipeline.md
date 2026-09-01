# ADR 0015: Untrusted intelligence fast/deep pipeline

- Status: Accepted
- Date: 2026-08-30
- Owners: intelligence, model-contracts, security, compliance

## Context

News, filings, and public notices can carry useful event information, but their
text, markup, metadata, links, and apparent ticker symbols are untrusted. A
licensed provider contract is not present in this repository. The pipeline must
notify downstream consumers quickly without allowing source text to become a
control instruction, silently rewriting an earlier alert, or reaching order
entry.

## Decision

1. Providers implement a bounded polling interface and supply explicit source-
   authentication evidence. The repository contains a deterministic mock,
   checksummed filesystem replay, and an official-public adapter around an
   injected legally reviewed HTTPS client. There is no built-in production
   connector, endpoint, credential, or provider-specific entitlement behavior.
2. Exact input bytes are hashed before decoding. UTF-8 decoding is strict.
   Active HTML elements and all attributes are removed. Prompt-like segments,
   including detected Base64 encodings, are removed from the analysis view.
   The retained inert-text view is separate and exists only for exact evidence
   excerpts.
3. Entity resolution is registry-backed. A ticker-looking token alone does not
   establish identity. Corporate relationships expand one configured graph hop
   with fixed capacity.
4. The fast stage is deterministic and bounded. It publishes an immutable
   preliminary `EventIntelligenceRecord` before optional deep work is queued.
5. The deep-stage interface has no tools, configuration authority, network
   methods, system-prompt mutation, or order API. Its output must match the fast
   source hash and parent identity and must pass strict enum, score, entity,
   fact, excerpt, and provenance validation.
6. A deep result is a new record linked to its fast parent. A refinement,
   dispute, or correction cannot delete or replace the fast record. Late,
   malformed, or unavailable deep output leaves the fast record intact.
7. Deduplication, novelty history, revision state, fact assertions, queues,
   publication history, and audit logs are bounded. Capacity failure is
   explicit and does not cause retry amplification.
8. Schema v1.4 additively appends document/event/stage/adjudication enums,
   immutable lineage, source authentication, scores, resolved entities,
   evidence-backed facts, exact excerpts, and contradiction links to
   `EventIntelligenceRecord`. Existing v1.3 fields retain their meaning.
9. Intelligence records are asynchronous notifications, not executable trading
   forecasts. A later model that derives a numeric market forecast must publish
   the common `ModelForecast` contract. Neither stage imports or addresses OMS,
   risk approval, execution, or a gateway.

## Consequences

- Replay is deterministic for an explicit document order, entity registry,
  configuration, clocks, and model/adjudicator versions.
- Logs and metrics contain hashes, stable reason codes, and bounded counters;
  they do not contain document text, credentials, or provider-specific labels.
- Exact excerpts can contain licensed or sensitive content. Persistent storage,
  retention, redistribution, and operator access require provider-specific
  legal and compliance approval even though the code supports the contract.
- The reference deep adjudicator validates infrastructure only. It does not
  establish language-model accuracy or economic value.

## Rollout and rollback

Deploy v1.4 readers and semantic validators before v1.4 writers. Keep fast
publication enabled independently of the deep stage. Rollback disables v1.4
writers first, drains bounded deep queues, and retains every already published
fast/deep record. A provider adapter remains not-ready until its terms,
authentication, timestamps, correction semantics, and retention behavior are
approved.

## Rejected alternatives

- Passing source text through a system/developer instruction channel was
  rejected because source content has no control authority.
- Resolving every uppercase token as a ticker was rejected because it creates
  unsafe false entity associations.
- Replacing a fast record in place after deep inference was rejected because it
  destroys audit history and hides late corrections or disagreement.
- Connecting classifications directly to order entry was rejected because it
  bypasses the model-contract, ensemble, and deterministic risk boundaries.
