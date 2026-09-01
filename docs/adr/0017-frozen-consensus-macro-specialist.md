# ADR 0017: Frozen-consensus macroeconomic release specialist

- Status: Accepted
- Date: 2026-08-30
- Owners: intelligence, model-contracts, research, compliance

## Context

Scheduled macroeconomic events combine a calendar, point-in-time consensus,
official and provider observations, revisions to earlier periods, and market
responses that become available on different clocks. Treating the scheduled
time as the data-availability time creates look-ahead bias. Replacing a prior
value with its later revision destroys replay truth. Partial releases and
provider conflicts must not become directional forecasts merely because some
fields are present.

The common `ModelForecast` is intentionally domain-neutral. Macro-specific
calendar fields, surprise diagnostics, revisions, source quality, and
abstention reasons do not belong in its hot-path ABI.

## Decision

1. The subsystem runs in Python outside the execution hot path. It exposes no
   OMS, risk, strategy, execution, gateway, live-authorization, or order API.
2. Normalized inputs and detailed results use immutable schema `1.0.0`
   dataclasses, canonical sorted JSON, and SHA-256 identities. These are
   off-path replay artifacts; canonical FlatBuffers remain the cross-component
   forecast wire format.
3. A calendar field identity contains event type, provider-independent field
   ID, headline/core/subcomponent role, explicit integer unit, reference period,
   seasonal-adjustment convention, and annualization convention. Comparisons
   require exact identity equality.
4. The consensus snapshot freezes before the scheduled release. Every estimate
   and supporting source must be available by the freeze time. A
   `FrozenMacroEventProcessor` retains its digest and rejects later mutation,
   stale release revisions, or a correction not linked to the accepted release.
5. Scheduled wall-clock UTC time, official publication wall-clock UTC time,
   receipt wall-clock UTC time, exchange event time, and process monotonic time
   remain separate typed fields. A scheduled time never substitutes for actual
   availability.
6. Prior values retain the value known before release and the newly reported
   revision separately. The revision never overwrites the earlier value.
7. Headline, core, and subcomponent surprises use integer PPM:

   ```text
   surprise_ppm = (actual - frozen_consensus) * 1,000,000
                  / max(consensus_dispersion, unit_epsilon)
   ```

   Revision surprise is a separately labeled relative change:

   ```text
   revision_surprise_ppm = (revised_prior - prior_known_before_release)
                           * 1,000,000
                           / max(abs(prior_known_before_release), unit_epsilon)
   ```

   Division truncates toward zero. The empirical surprise percentile is the
   proportion of point-in-time historical headline surprises less than or equal
   to the current surprise.
8. Cross-asset response observations retain instrument, asset kind, raw integer
   value, unit, and wall-clock response window. The configured completeness
   groups are equity-index futures, rates through a Treasury future or yield,
   FX proxies, volatility instruments, and sector ETFs.
9. A provider-declared partial release, missing required official field,
   missing or incompatible consensus, conflicting provider value, insufficient
   history, invalid/missing response group, unauthenticated source, or source
   below the configured quality floor produces an abstention artifact and no
   `ModelForecast` bytes. Neutral-looking forecast bytes are not used to encode
   abstention.
10. Complete original and corrected releases may publish through the existing
    canonical common forecast contract. The initial scoring rule is a
    deterministic infrastructure baseline with zero calibration score and no
    predictive-value or profitability claim.

## Consequences

- Replay preserves what was scheduled, what was expected, what was actually
  available, and what changed later.
- Partial and conflicting data fail closed without an ambiguous neutral model
  output.
- Corrections produce new content identities while retaining the original
  frozen consensus and release lineage.
- Adding an event family requires reviewed field definitions, units,
  direction conventions, source policy, fixtures, and replay tests.
- Approved official calendars, releases, consensus providers, and licensed
  cross-asset data remain external integration dependencies.

## Rollback

Disable macro forecast publication and retain scheduled, release, correction,
and abstention artifacts by digest. No common schema migration or order-path
change is required.
