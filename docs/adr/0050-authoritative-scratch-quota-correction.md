# ADR 0050: Bind POC admission to the authoritative scratch soft quota

- Status: Accepted
- Date: 2026-09-10
- Owners: Research data platform and safety engineering
- Supersedes: The 250 GB administrative-allocation value in ADRs 0045 and 0047
- Root target and hard limit superseded by: [ADR 0062](0062-expand-bounded-poc-data-root-to-800-gb.md)
- Related: [Forecasting POC contract](../architecture/forecasting-poc-contract.md),
  [data repository architecture](../architecture/poc-data-repository.md), and
  [schema evolution policy](../../schemas/schema-evolution-policy.md)

## Context

The initial POC policy treated a user-reported 250 GB value as the complete
scratch allocation. On 2026-09-11 UTC, the cluster's documented
`/opt/rci/bin/hdquota -s` utility established that `/scratch/djy8hg` has a
10 TiB soft quota. Its exact observation was 608,990,093,312 bytes used and
10,386,126,184,448 bytes available. The cluster's 200 GB allocation applies to
`/home/djy8hg`, not scratch.

Keeping the obsolete 250 GB ceiling made fail-closed admission reject every
operation because existing scratch usage already exceeded that value. Using
filesystem-wide free space alone would not establish user entitlement.

## Decision

Storage policy v2 fixes the administrative scratch allocation at the exact
10 TiB soft-limit value, `10,995,116,277,760` bytes. Admission continues to use
the smaller of this policy value and fresh authoritative quota evidence.

The POC scope does not expand: the data-root target remains 80,000,000,000
bytes, its hard limit remains 100,000,000,000 bytes, temporary work remains
bounded to 20,000,000,000 bytes, and the required filesystem reserve remains
50,000,000,000 bytes. Capacity evidence never grants provider authorization.

Because the policy value and policy hash change, publish a side-by-side v2 JSON
schema. Never rewrite a v1 marker or manifest. Migrate a populated v1 root by
initializing a new v2 root, verifying and copying immutable objects and
manifests, and retaining the v1 root until the copied dataset verifies. An empty
v1 root may be archived intact before initializing v2 at the operational path.

## Consequences

- Admission reflects the cluster's user-facing scratch quota without confusing
  it with the home quota.
- Current usage and observation time still have to be supplied for every
  mutation; a stored observation does not become perpetual authority.
- The independent 100 GB root cap prevents the larger account quota from
  broadening the forecasting POC.
- Existing v1 evidence stays verifiable through the retained v1 schema and
  archived root.
