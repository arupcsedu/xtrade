# Model registry testing

## Determinism and scope

Registry tests use deterministic seed `20260831`, a deterministic test-only
Ed25519 key, injected UTC timestamps, synthetic artifact bytes, temporary
directories, and no network or credentials. The key is created in test memory;
it is not a live credential and is not committed as a key file.

Run the focused gates:

```bash
source tools/toolchain.sh
(cd control && gofmt -w model_registry cmd/model-registry)
(cd control && go vet ./...)
(cd control && go test ./model_registry)
(cd control && go test -race ./model_registry)
(cd control && go test -coverprofile=../build/reports/control/coverage.out ./...)
(cd control && go test -run '^$' \
  -bench 'Benchmark(VerifySignedManifest|FeatureSchemaCompatibility|InspectLineage|DeploymentObservation)$' \
  -benchmem -count=1 ./model_registry)
```

The repository-level `make lint`, `make test`, `make test-sanitizers`,
`make benchmark`, `make dependency-scan`, and `make package` include the Go
package. `make benchmark` retains
`build/reports/benchmarks/model-registry.txt`; packaging emits the
`dist/control/model-registry` binary.

## Coverage

Tests assert:

- registration, immutable object identity, idempotent concurrent retry, and
  conflicting registration rejection;
- exact validation progression, early approval rejection, explicit approval,
  shadow, canary, limited-risk, and two-person explicit production promotion;
- artifact, signed manifest, event-tail, and signed pointer tamper detection;
- incompatible name/unit/order/digest feature schemas fail without mutation;
- untrusted signers, invalid manifests, floating-point feature contracts,
  unavailable signing keys, and cancelled requests fail closed;
- rollback restores only an already approved/deployed compatible prior version,
  revokes every outgoing deployment pointer, retains parent lineage, and leaves
  immutable bytes untouched;
- model disable is idempotent, global across environments, and is not blocked by
  corrupted artifact bytes;
- retirement, signed audit lineage, configuration hash, readiness, and explicit
  absence of live-trading capability; and
- CLI register, staged validate, approve, shadow, canary, limited-risk,
  production, inspect, and disable operations with no key disclosure.

The race test exercises concurrent idempotent registration, deployment
coordination, and filesystem coordination code. Benchmarks measure manifest
signature verification, exact schema compatibility, complete lineage inspection,
and paired deployment evaluation. These are control-plane capacity measurements,
not hot-path latency claims. Deployment-specific cases and limitations are in
[Model Deployment Testing](model-deployment-testing.md).

## Manual CLI smoke test

The CLI expects external base64-encoded raw Ed25519 keys. Never put a production
key in the repository, shell history, test logs, or command output. In an
authorized isolated test environment, prepare an unsigned manifest and schema,
then run:

```bash
source tools/toolchain.sh
(cd control && go run ./cmd/model-registry register \
  --root /approved/test/registry --key-id test-operator \
  --private-key /approved/test/private.key \
  --actor test-trainer --reason 'synthetic registration' \
  --manifest /approved/test/manifest.json \
  --artifact /approved/test/model.bin)

(cd control && go run ./cmd/model-registry validate \
  --root /approved/test/registry --key-id test-operator \
  --private-key /approved/test/private.key \
  --actor test-validator --reason 'offline evidence reviewed' \
  --model-id example-model --version 1.0.0 --stage trained \
  --feature-schema /approved/test/feature-schema.json)
```

Repeat validation explicitly for `offline` and then `replay`; approval and each
deployment environment are separate commands. `--explicit-production` alone is
insufficient: production also requires an authorization reference and a
promoter different from the approval actor.

## Known test boundaries

The local slice has no multi-host consensus, network API, organization RBAC,
HSM/KMS, certificate rotation, object-storage failure injection, PostgreSQL,
backup restore, or disaster-recovery test. Those gates become mandatory before
a distributed or production registry is claimed. The tests do not authorize
live trading or validate predictive economic value.
