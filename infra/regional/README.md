# Regional Kubernetes deployment

This tree deploys only non-hot-path Aegis-MX services. The authoritative
allowlist is news intelligence, filings intelligence, macro intelligence,
TimesFM-compatible forecasting, options analytics, model registry,
configuration control plane, dashboards, and research APIs. It contains no
edge core, market-data feed handler, order book, feature engine, ensemble,
deterministic pre-trade risk, OMS, router, or exchange gateway workload.

## Layout

- `base/` contains five isolated namespaces, nine Deployments and ClusterIP
  Services, resource guardrails, immutable configuration, read-only Secrets
  Store CSI mounts, default-deny NetworkPolicies, and a suspended backup job.
- `overlays/local/` reduces replicas to one, uses local service identities, and
  removes the production document-sandbox RuntimeClass requirement.
- `overlays/production/` adds disruption budgets, bounded HPAs for stateless
  services, GPU placement for TimesFM, the backup schedule, and fail-closed
  admission policy.

Validate the repository-owned contracts with:

```bash
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make regional-validate
```

The checked-in image, signature-evidence, and configuration digests are all-zero
placeholders under the non-routable `registry.invalid` domain. This is
intentional. The production admission policy rejects them. A trusted release
controller must verify every exact OCI digest, SBOM, provenance attestation, and
keyless signature; validate its release lock; and produce a separate immutable
overlay containing the verified values. Do not edit the base to make a release.

```bash
regional-deployment validate-release-lock --lock /approved/release-lock.json
kubectl kustomize infra/regional/overlays/production > build/regional-production.yaml
kubectl apply --server-side --dry-run=server -f build/regional-production.yaml
```

The dry run is expected to fail until a release overlay has replaced every
placeholder. Local development likewise requires locally built digest-addressed
images and ten externally provisioned `SecretProviderClass` objects. No secret
value or provider-specific secret identifier belongs in this repository.

See the [architecture contract](../../docs/architecture/regional-kubernetes-deployment.md),
[operations runbook](../../docs/operations/regional-kubernetes-runbook.md), and
[testing guide](../../docs/testing/regional-kubernetes-deployment-testing.md).
