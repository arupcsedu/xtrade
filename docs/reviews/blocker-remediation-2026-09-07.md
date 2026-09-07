# Blocker remediation record — 2026-09-07

This record distinguishes defects that can be corrected in the repository from
production dependencies that cannot be fabricated. Passing synthetic tests does
not authorize or certify live trading.

The remediated implementation is immutable at
`bab2be626da56acfb2c217ce84def51520758b56`. Full command outcomes and hashes
are recorded in the [evidence index](evidence-index.md).

## Reproduced defects and root causes

| Finding | Reproduction before fix | Root cause | Remediation |
| --- | --- | --- | --- |
| PRD-C001 | `test_live_transmission_boundary_requires_a_bound_capability` failed because the live interface exposed frame-only transmission | The protocol boundary had no typed, authenticated proof connecting exact output bytes to config, operator, risk, clock/data safety, activation audit, and fencing authority | Added bounded HMAC-SHA-256 live capabilities, exact frame binding, expiry/replay protection, and a verifier-only `VerifiedLiveTransmissionCapability` accepted by the dormant interface |
| PRD-C001 (fresh-audit hardening) | `UnsafeFinalStateCannotIssueCapability` and `VerifiedAuthorityIsMoveOnlyAndConsumedBySend` failed against the first remediation: structurally complete unsafe state could be signed and a verified wrapper could be copied/reused | The issuer validated field presence rather than final safety predicates, while one-shot replay protection ended at the verifier instead of the adapter boundary | Bound issuer trust to configuration/session/fencing authority and clock age; reject unsafe final state; make verified authority move-only; recheck exact frame and consume authority in the adapter's non-virtual send wrapper before its protected hook |
| PRD-C002 | `test_mandatory_explanation_is_accepted_before_gateway_submission` located `send_order` before explanation publication | Explanation publication used a lossy telemetry queue after submission | Added canonical full-record serialization and mandatory fail-closed `AsyncJournal` acceptance before gateway submission; PAPER acceptance reopens and decodes the records |
| PRD-C003 | `HaCoordinatorTest.RejectsSelfAssertedGrantWithRecomputedPublicHash` expected rejection but the coordinator accepted a caller-rehashed grant | FNV stable hashes were treated as authorization and quorum/fencing facts were caller asserted | Added Ed25519 witness signatures, key and trust-root identity, lease checks, and a verifier-created grant type required by the coordinator |
| PRD-B002 (repository portion) | `test_all_profiles_and_assets_are_non_live` failed because deployment validation had no matching repository executables | Systemd contracts preceded their process entry points and configuration verifier | Added five installed non-live service processes with build/config identity, health, structured lifecycle logs, watchdog notification and graceful shutdown; added a strict signed-config verifier and executable inventory gate |
| PRD-B005 (pre-send explanation portion) | Mandatory explanation saturation was not testable because the persistent journal was absent from the composition | Durable and observability queues were independent | `DecisionExplanationTest.MandatoryJournalOverflowFailsClosed` fills the bounded queue and proves permanent inhibition; the order path stops on any non-accepted publication |

## Issues that cannot truthfully be closed here

| Finding | Remaining dependency | Current safe state |
| --- | --- | --- |
| PRD-B003 | Licensed venue/feed/reference-data/news specifications, entitlements, credentials, conformance and certification | No proprietary layout is invented; no real transmitter exists |
| PRD-B004 | Independent production witness consensus, key custody, physical exchange-session fencing, separate-host qualification | Repository grants are now cryptographically verified, but absent external authority keeps production blocked |
| PRD-B006 | Signed OCI images, cluster, production identities, KMS/HSM, managed stores, CSI classes, admission verifier and restore drills | Regional overlays retain zero digests and `registry.invalid`; validator reports not apply-ready |
| PRD-B005 (authoritative recovery portion) | Whole-pipeline fresh-process reconstruction plus authoritative drop-copy/open-order reconciliation and power-loss qualification | Full decision explanations are durable in PAPER; component-local recovery is not represented as production proof |
| PRD-B002 (site qualification portion) | Reviewed NIC mapping and cold-start/watchdog/disk/rollback runs on target colocated hardware | Checked-in production-disabled profile retains `OPERATOR_REQUIRED` and therefore cannot activate |

These rows remain BLOCKERs in the fresh audit. Difficulty or external ownership is
not grounds for closure.

## Fresh-audit disposition

- PRD-C001, PRD-C002, and PRD-C003 are closed; their negative regression tests
  pass in the ordinary and UBSan suites.
- PRD-B001 is closed by the immutable implementation commit and the separate
  evidence-only audit commit.
- PRD-B002 and PRD-B005 are partially remediated but remain BLOCKERs at their
  production/system scope.
- PRD-B003, PRD-B004, and PRD-B006 remain BLOCKERs because their required
  licensed or production dependencies are absent.
- The fresh Prompt 40 verdict is **NO-GO** with zero open CRITICAL findings and
  five open BLOCKER findings.
