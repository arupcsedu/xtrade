# ADR 0039: Isolated fail-closed regional Kubernetes deployment

- Status: Accepted
- Date: 2026-09-05
- Owners: deployment, security, control-plane, intelligence, model-serving,
  observability, research

## Context

Aegis-MX has off-hot-path intelligence, forecasting, analytics, administration,
observability, and research components with different scaling and trust needs.
The colocated execution path has deterministic latency, host ownership, PTP,
shared-memory, fencing, and final-transmission requirements that Kubernetes
cannot silently replace. A regional deployment also needs a reviewable way to
remain unusable until images, identities, configurations, and backup endpoints
have been verified for a specific environment.

## Decision

1. Kubernetes is permitted only for the regional service allowlist maintained
   by `tools/regional_deployment.py`. Colocated market data, book/feature state,
   ensemble decisions, deterministic pre-trade risk, OMS, routing, gateways,
   journals, and leadership fencing remain in the systemd edge deployment.
2. Five namespaces isolate intelligence, models, control, observability, and
   research. Every namespace enforces the restricted Pod Security standard,
   default-deny ingress/egress, quotas, and limit defaults. Cross-boundary
   traffic uses explicit NetworkPolicy plus application mTLS authorization.
3. Every pod uses a dedicated ServiceAccount without an ambient token, a
   read-only root filesystem, non-root UID/GID, runtime-default seccomp, no host
   namespaces or Linux capabilities, bounded ephemeral storage, and a read-only
   Secrets Store CSI identity mount. Secret-provider objects are site-owned.
4. Base resources are intentionally nondeployable: images use all-zero digests
   in `registry.invalid`, and signature/configuration evidence hashes are zero.
   Production admission fails closed on tags, placeholders, absent verification
   evidence, host namespaces, or any pod that does not deny execution authority.
5. Cryptographic signature verification occurs in the trusted release and
   cluster admission systems. The repository validator checks the resulting
   release-lock structure, exact artifact inventory, digest identities, CI OIDC
   issuer, and verification result; it does not pretend that an annotation is a
   cryptographic verifier.
6. Availability-preserving rolling updates, zone spreading, host anti-affinity,
   and one PodDisruptionBudget per service are required in production. CPU and
   memory HPAs are limited to five stateless services with slow scale-down.
   Administrative services, dashboards, and GPU TimesFM do not autoscale.
7. Production TimesFM requests one GPU per pod, selects reviewed GPU nodes, and
   retains explicit CPU model fallback. Capacity must exist in at least two
   failure domains before the two-replica rollout is admitted.
8. Regional metadata backup is a bounded, nonconcurrent CronJob using a mounted
   workload identity and object-lock-capable storage through the approved egress
   proxy. Base is suspended; production enables it only as part of a resolved
   signed release. Raw evidence, model artifacts, and dashboards use their
   systems of record and separate replication policies.

## Consequences

- Loss of the CSI provider, signature verifier, configuration digest, GPU
  capacity, or approved egress makes affected pods unready or unschedulable; it
  never creates trading authority.
- NetworkPolicy is reachability control, not caller identity. Services still
  authenticate exact SPIFFE identities and authorize routes.
- Local manifests exercise composition with one replica but do not weaken image
  or secret provenance. Developers supply untracked digest locks and local CSI
  identities.
- Production rollout remains blocked until service images actually contain the
  declared healthcheck and graceful-drain contract. Deployment YAML alone does
  not turn the current libraries and CLIs into production network services.

## Rejected alternatives

- Deploying edge risk or routing on Kubernetes was rejected because it violates
  the hot-path and fault-containment contract.
- One shared namespace was rejected because a policy error would collapse all
  trust domains.
- Mutable tags and checked-in Secret objects were rejected because they are not
  immutable, auditable deployment identities.
- Automatic HPA for administrative and GPU services was rejected because
  replica count and device capacity carry state, cost, and failover semantics
  not represented by CPU utilization.
