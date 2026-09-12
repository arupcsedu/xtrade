# ADR 0052: Bind a deterministic PAPER command to the Alpaca transport

- Status: Accepted
- Date: 2026-09-11
- Owners: Execution integration, OMS, risk, security, and operations
- Related: [ADR 0051](0051-fixed-host-alpaca-paper-connectivity-certification.md),
  [gateway framework](../architecture/exchange-gateway-framework.md), and
  [system-path architecture](../architecture/alpaca-paper-system-path.md)

## Context

ADR 0051 proved bounded Alpaca paper-account connectivity but deliberately
constructed its own certification order. It therefore could not prove that an
outbound broker request was derived from the repository's deterministic router,
pre-trade risk engine, OMS, and final gateway safety gate. Putting synchronous
HTTPS in those components would violate the engineering contract, while merely
copying an unverified command into Python would permit a boundary substitution.

The authorized certification scope permits at most five one-share, long-only,
regular-hours US-equity PAPER orders from `ticker.txt`, preserves existing
orders and positions, and permits targeted cancellation. Live trading, account
reset, short selling, extended hours, options, and crypto remain prohibited.

## Decision

Add a fixed certification-only C++ vertical slice. It creates a one-share,
one-cent passive objective, then requires successful traversal through:

1. the deterministic smart-order router;
2. the deterministic local pre-trade risk engine and risk journal;
3. the event-sourced OMS and its deterministic client order ID;
4. the repository `PaperBrokerGateway` final safety gate and audit chain; and
5. an immutable, versioned system-path evidence record.

The slice refuses to run in a live-capable build. It exports only the exact
symbol, client order ID, price, quantity, mode/safety assertions, component
hashes, journal sequences, binary build version, and an end-to-end stable hash.
It exports no credential, endpoint, market-data value, provider response, or
arbitrary order field.

The off-hot-path `aegis-alpaca-paper certify-system-path` command executes the
owner-controlled C++ binary without a shell, verifies its SHA-256, the
authorization-bound C++/schema source-tree SHA-256, schema, fixed-width fields,
component gates, and independently recomputed stable hash,
then maps the evidence to exactly one Alpaca PAPER request. Python may not
change the side, quantity, limit price, time in force, extended-hours setting,
symbol, or OMS client order ID. A failed or malformed handoff blocks before the
order `POST`.

The existing quote, account, session, authorization, preservation, ambiguous
submission, targeted-cancel, and zero-fill gates remain mandatory. Synchronous
HTTPS remains outside the strict hot path. The certification report separates:

- `system_order_path_certified`: the outbound command reached and was accepted
  by Alpaca's paper endpoint after all deterministic gates; and
- `broker_response_roundtrip_certified`: acknowledgements, cancels, fills, and
  reconciliation were applied back through the deterministic OMS.

This slice may set only the first field to true. The second is fixed false.

## Consequences

- A successful system-path report links the exact outbound paper request to
  router, risk, OMS, gateway, binary, configuration, and audit evidence.
- The system still cannot operate as an always-on Alpaca gateway. Broker event
  streaming, response sequencing, OMS recovery, durable outbox/inbox state,
  drop-copy reconciliation, and restart idempotency across the network boundary
  remain unimplemented and must not be inferred from this certification.
- The fixed one-cent order is a safety probe, not an execution-quality or
  profitability test.
- Paper connectivity authority does not grant historical market-data storage,
  ML-training, derived-data, retention, or redistribution rights.

## Rejected alternatives

- **Let Python create an arbitrary order from CLI parameters:** this severs the
  deterministic command and risk binding.
- **Place Alpaca HTTPS in the C++ hot path:** network blocking and TLS behavior
  violate the hot-path contract.
- **Claim broker-response round-trip completion:** provider responses are not
  yet consumed by the deterministic OMS.
- **Use a marketable order:** it creates unnecessary fill and position risk.
