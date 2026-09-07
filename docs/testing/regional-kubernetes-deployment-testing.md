# Regional Kubernetes deployment testing

## Repository validation

Use the required external Python environment:

```bash
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make regional-validate
/scratch/djy8hg/env/aegis_mx_contracts/bin/python -m pytest \
  -q -o addopts='' python/tests/test_regional_deployment.py
```

The validator rejects duplicate YAML/JSON keys, missing Kustomize references,
unexpected workloads, edge/execution components, namespace or pod-security
weakening, non-ClusterIP exposure, missing quotas/default deny, mutable
configuration, absent probes/resource bounds/anti-affinity, ambient service
tokens, host namespaces, writable/root/capable containers, non-CSI identity,
unsafe HPAs, missing PDBs, incomplete GPU placement, disabled production backup,
and weak admission policy.

Tests use deterministic values only. Release-lock tests accept exactly six
verified, digest-addressed artifacts and reject zero images, unverified
signatures, wrong registries, non-UTC timestamps, incomplete inventory, and
duplicate JSON keys.

## Cluster integration gates

The repository environment does not include Kubernetes tooling or a cluster.
CI or staging must install pinned `kubectl`/Kustomize and a Kubernetes schema
validator, then retain output for:

```bash
kubectl kustomize infra/regional/overlays/local > build/regional-local.yaml
kubeconform -strict -summary build/regional-local.yaml
kubectl apply --server-side --dry-run=server -f build/regional-local.yaml
```

For production, use a release-resolved overlay and prove:

- zero/tag/unsigned images and missing configuration hashes are denied;
- all Pods meet restricted Pod Security and NetworkPolicy is enforced by the
  actual CNI;
- unauthorized cross-namespace, direct Internet, and edge-directed connections
  fail while DNS, collector, and egress-proxy flows succeed;
- certificate rotation, revocation, malformed identity, and missing CSI material
  make the service unready without plaintext fallback;
- rolling updates preserve the PDB and no two replicas share a required failure
  domain when capacity exists;
- HPA respects min/max and scale-down stabilization under load and metric loss;
- TimesFM GPU loss gives bounded fallback and does not cause retry storms; and
- backup, isolated restore, checksum/audit verification, RPO, and RTO pass.

No benchmark is required for static YAML or the offline validator. Regional
capacity tests must separately measure service throughput, queue occupancy,
deadline misses, scaling lag, rollout readiness, GPU utilization, egress proxy
capacity, and backup/restore throughput on representative infrastructure.
