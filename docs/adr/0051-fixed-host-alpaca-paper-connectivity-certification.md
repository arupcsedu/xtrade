# ADR 0051: Isolate fixed-host Alpaca PAPER connectivity certification

- Status: Accepted; outbound system-path extension defined by ADR 0052
- Date: 2026-09-10
- Owners: Execution integration, risk, security, and operations
- Supersedes: None
- Related: [gateway framework](../architecture/exchange-gateway-framework.md),
  [certification architecture](../architecture/alpaca-paper-certification.md), and
  [operator runbook](../operations/alpaca-paper-certification-runbook.md)
- Extended by:
  [ADR 0052](0052-bounded-alpaca-paper-command-bridge.md)

## Context

The repository-owned `PaperBrokerGateway` is a deterministic in-process test
double and intentionally has no broker connection. An operator has separately
authorized a bounded Alpaca paper-account connectivity check: authenticated
read-only inspection followed, during one named regular session, by no more
than five one-share US-equity paper orders and cancellation of only
certification-created orders. Existing paper orders and positions must be
preserved. Short selling, options, crypto, extended hours, account reset, and
live trading remain unauthorized.

Putting synchronous HTTPS into `IExchangeGateway` would violate the hot-path
contract. Treating an operator connectivity order as if it had traversed the
strategy, risk, OMS, router, and gateway chain would also create false evidence.
The public Alpaca Trading API specification is sufficient for a narrowly
identified paper test, but it does not resolve market-data storage or ML
training rights.

## Decision

Create an off-hot-path operator tool named `aegis-alpaca-paper`. It is not an
`IExchangeGateway` implementation and accepts no forecast, strategy intent,
router output, or raw arbitrary order. Pin trading requests to
`https://paper-api.alpaca.markets` and the ephemeral quote guard to
`https://data.alpaca.markets`. Reject ports, redirects, user information,
queries in configured origins, and every live or broker-sandbox hostname. The
configured `/v2` suffix is normalized only after its host and syntax validate.

Load the three allowlisted credential assignments through an owner-only,
bounded, no-follow file descriptor. Never serialize, hash, print, trace, or
return credential values. Unit tests inject a transport and use no network.

Separate mutation authority from credentials. A private, immutable operator
scope record binds the exact ticker-universe, tool-source, and configuration
hashes, session date, expiry, paper
mode, US-equity class, maximum five submissions, one-share maximum, required
cancellation, and all disabled capabilities. `certify` additionally requires
the explicit `--execute-paper` flag. The initial executable slice submits one
simple day buy-limit order for one share at USD 0.01 only after:

- the account is active and not blocked or suspended;
- the Alpaca clock says the named regular-session date is open;
- the symbol is in `ticker.txt` and is an active tradable US equity;
- an ephemeral IEX bid is at least USD 1.00 and above the limit;
- existing open-order and position snapshots have been hashed; and
- the authorization is nonexpired and matches the universe.

The tool supplies a deterministic `client_order_id`, never retries `POST`, and
resolves an ambiguous response through the documented client-order-ID lookup.
It cancels only the returned UUID and requires a final `canceled` state, zero
fill quantity, and unchanged existing-order and position hashes. It performs no
automatic liquidation or account reset. An unexpected fill or concurrent state
change is an explicit failed certification requiring operator reconciliation.

The `certify` command defined by this ADR always reports
`system_order_path_certified: false`. Broker-connectivity evidence from that
command cannot be used as evidence that deterministic risk, OMS, or the router
reached Alpaca. ADR 0052 adds a separate `certify-system-path` command with a
strictly bounded evidence handoff; it does not change the meaning of historical
`certify` reports.

## Consequences

- Credentials can be verified and a broker paper session can be certified
  without enabling real-money trading or placing network calls in the hot path.
- The maximum mutation is one deeply nonmarketable order submission and its
  targeted cancellation in the current slice, below the authorized cap of five.
- Existing account state is observed but not disclosed in reports; only counts
  and SHA-256 projections are retained.
- Alpaca paper execution behavior is not a realistic liquidity, queue, impact,
  or economic-performance measurement.
- Broker-response ingestion into OMS, streaming updates, durable restart
  recovery, and drop-copy reconciliation remain future work even after the
  outbound slice in ADR 0052.
- Historical Alpaca/IEX storage, retention, derived-data, and training rights
  remain blocked independently.

## Rejected alternatives

- **Reuse the live hostname with paper keys:** endpoint mistakes must be
  structurally impossible, not operational convention.
- **Put Python HTTPS in the gateway hot path:** this violates bounded execution
  and network-blocking constraints.
- **Submit a marketable or market order:** it needlessly changes positions and
  tests simulated fills rather than connectivity.
- **Cancel all open orders before or after the test:** this violates preservation
  authority and could destroy unrelated operator state.
- **Retry a timed-out order submission:** an ambiguous first submission could
  create a duplicate order.
- **Claim end-to-end system certification:** the operator tool deliberately does
  not traverse the deterministic strategy/risk/OMS/router chain.
