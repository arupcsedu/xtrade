# Data-source authorization runbook

## Purpose

This runbook authorizes an exact remote dataset for the bounded forecasting POC
without authorizing trading. It implements the source gate in the
[forecasting POC contract](../architecture/forecasting-poc-contract.md) and the
decision in [forecasting POC source approval](../compliance/forecasting-poc-source-approval.md).
The default outcome is deny.

This process does not accept or store provider credentials in the repository.
Do not paste a credential into an issue, terminal argument, chat, log, policy,
manifest, report, or approval record.

## Preconditions

- The exact `ticker.txt` snapshot is valid and its source and universe hashes
  are recorded.
- The operator has declared whether the use is personal, academic,
  professional, commercial, or governmental.
- The exact provider, product/feed, plan, account, agreement/order form, and
  upstream owner terms are available to authorized reviewers.
- The requested symbols, dates, fields, sessions, content classes, series,
  environments, machines, users, and purposes are finite.
- Persistent storage, training, derived-data, attribution, retention, deletion,
  and termination rights are answered explicitly.
- The 10 TiB scratch soft quota is confirmed with `/opt/rci/bin/hdquota -s`,
  and current usage can be measured. Unknown or stale quota evidence remains a
  hard stop.
- The proposed source fits the 800 GB target and hard root limit, 20 GB
  temporary limit, and 50 GB filesystem reserve.
- No source adapter or ordinary test can reach the provider before this gate.

## Roles

| Role | Responsibility |
| --- | --- |
| Operator/requester | Declares identity, user class, scope, and purpose; never supplies secrets through Git |
| Data owner/legal/compliance | Interprets executed rights and third-party restrictions |
| Security | Approves secret custody, endpoint allowlist, least privilege, and incident response |
| Data engineering | Maps fields, time, revisions, adjustments, rate limits, and resource estimates |
| Research/model risk | Defines training and derived-artifact use and verifies leakage controls |
| Approver | Signs the immutable decision; cannot approve an incomplete rights matrix |

An approver must not approve their own unsupported assertion. Critical policy
changes use the control-plane two-person workflow when available.

## Procedure

### 1. Freeze the request

Record a request ID and the exact:

- universe file path, file hash, canonical universe hash, and unresolved symbols;
- provider, product/feed/dataset/version, endpoint class, and environment;
- date range, session filter, fields, expected records, compressed/uncompressed
  bytes, temporary peak, retry overhead, and derived outputs;
- purpose and user classification; and
- proposed effective, expiry, review, and deletion dates.

Scope changes create a new request. They never inherit approval implicitly.

### 2. Collect authoritative terms

Capture first-party URLs and the identities/hashes of applicable executed
agreements without committing licensed documents unless their license and the
security policy permit it. Review provider, upstream exchange/data owner, API,
website, plan, and order-form terms together.

Answer `DOCUMENTED`, `PROHIBITED`, or `UNRESOLVED` for:

- automated access;
- persistent raw and canonical storage;
- research/backtesting;
- model training;
- derived features, labels, models, forecasts, and reports;
- redistribution/display/internal sharing;
- attribution; and
- retention, deletion, backup, and post-termination treatment.

Any required `UNRESOLVED` or `PROHIBITED` result is `BLOCKED`. Payment or API
success cannot change the result.

### 3. Review source semantics

Document and test against an authorized specification:

- source identifier and point-in-time instrument mapping;
- event/publication/source-receive/processing/revision times and timezone;
- bar boundaries, eligible trades, missing intervals, sessions, and halts;
- raw versus split/dividend/spin-off adjustment behavior;
- corrections, amendments, vintages, replacements, and deletion notices;
- pagination, maximum page/object size, rate limits, retries, and checksums; and
- unsupported symbols, venues, form types, languages, or series.

Never fill an undocumented timestamp or adjustment field by inference.

### 4. Review security and storage

- Allowlist exact HTTPS hosts and prevent redirects to unapproved hosts.
- Put the provider secret in the approved external secret manager with minimum
  read scope, rotation, owner, and expiry. Record only the mechanism/reference
  class, never the value.
- Require TLS verification, bounded responses, decompression limits, timeouts,
  rate limiting, retry budgets, and graceful shutdown.
- Run `aegis-data estimate` with authoritative quota evidence. Account for
  partial and temporary objects. Never auto-delete data to make room.
- Define quarantine, hash verification, immutable raw publication, correction
  lineage, and incident revocation.

### 5. Issue the immutable approval

The signed approval must satisfy
[Forecasting POC source approval](../compliance/forecasting-poc-source-approval.md).
Publish a new source policy referencing its SHA-256. Do not edit an approved
policy or the checked-in example. The policy remains `enabled=false` until its
staged activation time and all local validation gates pass.

SEC structured data, SEC documents, GDELT metadata, each FRED/ALFRED series,
Nasdaq Trader directories, and each market-data feed receive independent
records.

### 6. Validate without remote mutation

From `/scratch/djy8hg/xtrade`, use the required environment:

```bash
export AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts
$AEGIS_PYTHON_ENV/bin/python tools/source_policy_check.py \
  --policy infra/data_poc/source-policy.example.json \
  --require-default-disabled
make docs-check
```

Expected example-policy result:

```text
PASS: source policy is valid; 7 source records are disabled; network downloads are not authorized
```

Normal unit/integration tests remain network-denied and secret-free. A later
authorized pilot must be a separate operator action with its request ID,
approval hash, quota evidence, and audit output.

### 7. Stage an authorized pilot

Only after the immutable approval exists:

1. create a new, non-example policy instance;
2. bind the exact approval hash and effective/expiry times;
3. enable only the approved source/dataset/use;
4. run a no-network dry-run and storage admission check;
5. run the smallest authorized sample in quarantine;
6. verify hashes, schema, timestamps, adjustments, rate headers, coverage, and
   unsupported-symbol reporting;
7. stop and review evidence before any larger backfill; and
8. keep model training disabled until the training right and dataset manifest
   are independently verified.

## Source-specific stops

| Source | Mandatory stop condition |
| --- | --- |
| Massive/Polygon | No written non-display, storage, training, and derived-work license |
| Alpaca IEX | Outside the expiring owner-only academic/non-commercial approval, changed account/terms/scope, or any redistribution/commercial use |
| SEC | Unapproved form/document class, active content, unsupported personal data, or missing point-in-time semantics |
| GDELT | Request includes publisher page content, broad mirror, missing attribution, or unbounded entity query |
| FRED/ALFRED | Series absent from the owner/rights/unit/vintage allowlist or no reliable release-time source |
| Nasdaq Trader | No express written automated-storage/ML permission or request assumes historical completeness |

## Revocation and incident response

Immediately disable the source and prevent new network access when:

- approval expires, is revoked, or no longer matches the request;
- a provider or upstream term changes;
- a credential is exposed or used outside scope;
- provider identity, TLS, timestamp, schema, adjustment, or hash validation fails;
- quota, projected peak, or reserve becomes unknown/unsafe;
- a deletion demand, correction, or rights dispute arrives; or
- the adapter retrieves an unauthorized dataset or content class.

Preserve non-secret audit evidence. Quarantine affected objects; do not silently
rewrite or delete immutable originals. Legal/compliance determines whether raw,
canonical, derived, backup, feature, model, and report artifacts must be deleted
or retained. Record every action and verification hash. Rotation of a credential
does not restore authorization automatically.

## Recovery and renewal

Renewal repeats the complete review against current terms and the current
universe. A prior approval does not bridge a lapse. After reapproval, reconcile
all source manifests and corrections point in time before resuming. If rights
cannot be re-established, keep the source disabled and produce a cleanup plan;
never automatically delete data.
