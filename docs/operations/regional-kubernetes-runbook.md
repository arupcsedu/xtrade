# Regional Kubernetes deployment runbook

## Preconditions

Do not apply the production overlay until all of the following are recorded:

1. The cluster version supports `admissionregistration.k8s.io/v1`
   `ValidatingAdmissionPolicy`, `policy/v1` PDBs, `autoscaling/v2`, and CronJob
   time zones.
2. A policy-enforcing CNI, restricted Pod Security admission, Secrets Store CSI,
   workload CA, cryptographic image-signature admission controller, metrics
   API, OpenTelemetry collector, and regional egress proxy are healthy.
3. Two zones have capacity for every disruption budget. GPU nodes in two
   failure domains expose `nvidia.com/gpu`, the reviewed RuntimeClass, driver,
   health monitor, node label, and taint.
4. All ten `SecretProviderClass` objects exist in their workload namespaces and
   issue exact `spiffe://aegis-mx/production/<service>` identities. Private
   material is never printed.
5. The six images have SBOM/provenance attestations and valid CI keyless
   signatures. The signed configuration and release lock bind the exact
   digests. No placeholder remains.
6. Database point-in-time recovery, object lock, retention, backup identity,
   restore target, and alert routing have passed a staging restore drill.

## Validate and stage

```bash
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make regional-validate
regional-deployment validate-release-lock --lock /approved/release-lock.json
kubectl kustomize infra/regional/overlays/production > build/regional-production.yaml
kubectl apply --server-side --dry-run=server -f build/regional-production.yaml
```

The release controller must generate an untracked overlay that replaces each
image, signature evidence hash, and configuration hash. A dry-run rejection is
the required result for the checked-in placeholders. Apply namespaces,
admission/signature policies, identity classes, and NetworkPolicies before
workloads. Never bypass admission to recover availability.

## Rollout

Deploy one trust domain at a time: observability, control, models,
intelligence, then research. For each domain:

- watch rollout status and PDB health;
- require semantic readiness, configuration hash, build version, certificate
  identity, queue depth, error rate, and deadline metrics;
- compare the new version in shadow mode before increasing traffic;
- stop if readiness flaps, deadlines regress, OOD rises, a backup overlaps, or
  the egress proxy denies an unexpected destination; and
- retain old ReplicaSets until the observation window completes.

TimesFM additionally requires GPU error, memory, temperature, queue, batching,
deadline, and CPU-fallback metrics. GPU failure may produce bounded CPU fallback
or an explicit unavailable forecast; it cannot weaken edge risk or block OMS.

## Rollback

Rollback means redeploying the previously verified image/configuration digest,
not using a mutable tag:

1. Stop new administrative mutations if control-plane state compatibility is
   uncertain.
2. Verify the previous release lock and database migration compatibility.
3. Apply the previous immutable overlay with the same server-side dry run.
4. Confirm rollout, readiness, audit-chain continuity, model/config lineage,
   and forecast freshness.
5. Record actor, reason, old/new release-lock hashes, start/end times, and health
   evidence.

If data migration is not backward compatible, keep the new application stopped
and follow its documented restore/migration procedure. Do not restore a database
over the current primary merely to make pods ready.

## Backup and restore

Alert on a missed 15-minute recovery point, failed job, retry exhaustion,
object-lock failure, audit-chain mismatch, or restore-test age over 35 days.
Monthly, restore into an isolated account/namespace, verify checksums and audit
lineage, run application consistency checks, measure RPO/RTO, and delete the
test environment according to retention policy. A backup job must never receive
order-entry, gateway, or edge host credentials.

## Local development

Use only mock/filesystem providers and non-sensitive generated identities.
Create digest-addressed local images and CSI classes outside source control,
then render:

```bash
kubectl kustomize infra/regional/overlays/local > build/regional-local.yaml
kubectl apply --server-side --dry-run=server -f build/regional-local.yaml
```

The local overlay has one replica, suspended backups, and no document sandbox
RuntimeClass. It is not evidence for production scheduling, security, backup,
or failover behavior.
