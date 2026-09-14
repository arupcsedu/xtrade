# Alpaca Basic/IEX data-rights confirmation request

## Status

- Requester: `djy8hg`
- Classification: academic
- Purpose: internal academic research
- Distribution: internal only
- Plan: Alpaca Basic
- Dataset: Historical Stock Bars, `feed=iex`, `timeframe=1Min`
- State: current evidence adjudicated as not authorized; ready to submit for
  provider reconsideration
- Live trading: out of scope and disabled

This document is a request template, not permission and not legal advice. Send
it through [Alpaca Support](https://alpaca.markets/support) or the official
[contact page](https://alpaca.markets/contact). Do not place an API key,
password, account token, signed URL, or other secret in the request or this
repository.

## Request text

Subject: Written data-use confirmation for Alpaca Basic IEX academic POC

> I use an Alpaca Basic account as an academic user for an internal,
> non-distributed research proof of concept. Please confirm whether my current
> account and applicable Alpaca/IEX/upstream agreements authorize automated
> retrieval of Historical Stock Bars using `feed=iex` and `timeframe=1Min` for
> the 79-symbol universe identified by SHA-256
> `c4bfd957725741d76f847405294258f4a8d31d16dff58b0a7c033bafbb446f79`.
>
> The initial test is five regular trading sessions. A later phase may cover up
> to two years. Data would be held under
> `/scratch/djy8hg/aegis_mx_poc_data`, with an 800 GB target and hard limit,
> and would never be redistributed or used to enable live trading.
>
> Please answer each item explicitly: (1) automated API retrieval; (2)
> persistent storage of raw responses; (3) storage of normalized and
> point-in-time datasets; (4) non-display research and backtesting; (5) machine
> learning training and validation; (6) creation and retention of derived
> features, labels, model weights, forecasts, and reports; (7) internal sharing
> within the academic project; (8) required attribution; (9) maximum retention,
> deletion, backup, and post-termination obligations; and (10) any separate IEX,
> SIP, CTA, UTP, or other upstream agreement that applies.
>
> Please identify the governing agreement/product version, effective date,
> expiration or review date, and any restrictions that differ between historical
> IEX data and delayed historical SIP data. A support case identifier or written
> response suitable for an internal audit record would be appreciated.

## Evidence to retain

Retain outside Git any response containing account-identifying information.
Record only these non-secret fields in the signed approval artifact:

- provider and support case or agreement identifier;
- response/agreement date and applicable version;
- each of the ten rights answers above;
- retention, deletion, and post-termination instructions;
- effective, expiry, and review dates;
- approved credential mechanism, without the credential;
- responder/approver identity under the project's access controls; and
- content SHA-256 and signature metadata.

## Fail-closed outcomes

- An unanswered, conditional, or ambiguous required right remains `UNRESOLVED`.
- Any prohibited storage, training, or derived-artifact right keeps the source
  disabled; reducing the scope requires a new reviewed POC contract.
- API success, plugin connectivity, or Basic-plan availability is access
  evidence only and cannot replace the rights response.
- Do not request or download a sample while this request is pending.
- The binding repository decision is recorded in the
  [entitlement decision](alpaca-historical-data-entitlement-decision.md).
