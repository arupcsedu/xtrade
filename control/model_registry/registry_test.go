package modelregistry

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"
)

const deterministicTestSeed = int64(20260831)

type registryFixture struct {
	t          testing.TB
	registry   *Registry
	root       string
	privateKey ed25519.PrivateKey
	publicKey  ed25519.PublicKey
	schema     FeatureSchema
	clock      *fixtureClock
}

type fixtureClock struct {
	mu   sync.Mutex
	next time.Time
}

func (clock *fixtureClock) Now() time.Time {
	clock.mu.Lock()
	defer clock.mu.Unlock()
	result := clock.next
	clock.next = clock.next.Add(time.Second)
	return result
}

func newRegistryFixture(t testing.TB) *registryFixture {
	t.Helper()
	seed := sha256.Sum256([]byte("aegis-mx-model-registry-seed-20260831"))
	privateKey := ed25519.NewKeyFromSeed(seed[:])
	publicKey := append(ed25519.PublicKey(nil), privateKey.Public().(ed25519.PublicKey)...)
	schema, err := FinalizeFeatureSchema(FeatureSchema{
		SchemaID: "microstructure-v1",
		Version:  "1.0.0",
		Fields: []FeatureField{
			{Name: "top_imbalance", Type: "int64", Unit: "ppm", Scale: 1_000_000, Required: true},
			{Name: "spread", Type: "uint64", Unit: "ticks", Scale: 1, Required: true},
		},
	})
	if err != nil {
		t.Fatalf("finalize schema: %v", err)
	}
	clock := &fixtureClock{next: time.Unix(1_800_000_000, 0).UTC()}
	root := t.TempDir()
	registry, err := Open(Config{
		Root: root, TrustedKeys: map[string]ed25519.PublicKey{"test-signer": publicKey},
		SignerKeyID: "test-signer", Signer: privateKey, Clock: clock.Now,
	})
	if err != nil {
		t.Fatalf("open registry: %v", err)
	}
	return &registryFixture{t: t, registry: registry, root: root, privateKey: privateKey, publicKey: publicKey, schema: schema, clock: clock}
}

func TestPrivateKeyLoaderRequiresOwnerOnlyFile(t *testing.T) {
	t.Parallel()
	seed := sha256.Sum256([]byte("aegis-mx-private-key-permission-test"))
	path := filepath.Join(t.TempDir(), "registry-private.key")
	encoded := base64.StdEncoding.EncodeToString(ed25519.NewKeyFromSeed(seed[:]))
	if err := os.WriteFile(path, []byte(encoded), 0o644); err != nil {
		t.Fatalf("write private key: %v", err)
	}
	if err := os.Chmod(path, 0o644); err != nil {
		t.Fatalf("make private key insecure for negative test: %v", err)
	}
	if _, err := loadPrivateKey(path); err == nil || !strings.Contains(err.Error(), "owner-only") {
		t.Fatalf("expected owner-only rejection, got %v", err)
	}
	if err := os.Chmod(path, 0o600); err != nil {
		t.Fatalf("secure private key: %v", err)
	}
	if _, err := loadPrivateKey(path); err != nil {
		t.Fatalf("load owner-only private key: %v", err)
	}
}

func (fixture *registryFixture) signedManifest(version string, parent string, artifact []byte) SignedManifest {
	fixture.t.Helper()
	artifactDigest := sha256.Sum256(artifact)
	manifest := ArtifactManifest{
		SchemaVersion: SchemaVersion, ModelID: "mid-direction", SemanticVersion: version,
		ParentSemanticVersion: parent,
		TrainingData: TrainingDataManifest{
			DatasetID: "synthetic-events-v1", ManifestSHA256: digestText("training-data"),
			AsKnownAtUTCNS: 1_700_000_000_000_000_000, SplitPolicy: "walk-forward-v1",
			DeterministicSeed: deterministicTestSeed,
		},
		FeatureSchema: fixture.schema, CodeCommit: strings.Repeat("a", 40),
		DependencyLock:        DigestReference{ID: "go-lock-v1", SHA256: digestText("lock")},
		TrainingConfiguration: DigestReference{ID: "training-config-v1", SHA256: digestText("config")},
		Metrics:               []Metric{{Name: "brier-score", ValuePPM: 250_000, DatasetID: "synthetic-events-v1", MetricUnit: "ppm"}},
		CalibrationData:       DigestReference{ID: "calibration-v1", SHA256: digestText("calibration")},
		OODProfile:            DigestReference{ID: "ood-v1", SHA256: digestText("ood")},
		ArtifactSHA256:        hex.EncodeToString(artifactDigest[:]),
		CreatedAtUTCNS:        1_700_000_001_000_000_000,
	}
	signed, err := SignManifest(manifest, "test-signer", fixture.privateKey)
	if err != nil {
		fixture.t.Fatalf("sign manifest: %v", err)
	}
	return signed
}

func (fixture *registryFixture) register(version string, parent string, artifact []byte) SignedManifest {
	fixture.t.Helper()
	artifactPath := filepath.Join(fixture.t.TempDir(), "model.bin")
	if err := os.WriteFile(artifactPath, artifact, 0o600); err != nil {
		fixture.t.Fatalf("write artifact: %v", err)
	}
	signed := fixture.signedManifest(version, parent, artifact)
	status, err := fixture.registry.Register(context.Background(), RegisterRequest{
		SignedManifest: signed, ArtifactPath: artifactPath, Actor: "trainer", Reason: "register deterministic test artifact",
	})
	if err != nil {
		fixture.t.Fatalf("register %s: %v", version, err)
	}
	if status.Lifecycle != LifecycleDraft || status.Approval != ApprovalPending || status.Active {
		fixture.t.Fatalf("unexpected registration status: %+v", status)
	}
	return signed
}

func (fixture *registryFixture) validateAndApprove(version string) {
	fixture.t.Helper()
	for _, stage := range []ValidationStage{ValidationTrained, ValidationOffline, ValidationReplay} {
		if _, err := fixture.registry.Validate(context.Background(), ValidateRequest{
			ModelID: "mid-direction", SemanticVersion: version, Stage: stage,
			ExpectedFeatureSchema: fixture.schema, Actor: "validator", Reason: "deterministic validation evidence",
		}); err != nil {
			fixture.t.Fatalf("validate %s at %s: %v", version, stage, err)
		}
	}
	if _, err := fixture.registry.Approve(context.Background(), ApproveRequest{
		ModelID: "mid-direction", SemanticVersion: version, ExpectedFeatureSchema: fixture.schema,
		Actor: "risk-approver", Reason: "reviewed replay evidence", ApprovalReference: "change-1001",
	}); err != nil {
		fixture.t.Fatalf("approve %s: %v", version, err)
	}
}

func (fixture *registryFixture) deployProduction(version string) ModelStatus {
	fixture.t.Helper()
	steps := []struct {
		environment string
		call        func(context.Context, DeploymentRequest) (ModelStatus, error)
	}{
		{environment: "shadow", call: fixture.registry.DeployShadow},
		{environment: "canary", call: fixture.registry.PromoteCanary},
	}
	for _, step := range steps {
		if _, err := step.call(context.Background(), DeploymentRequest{
			ModelID: "mid-direction", SemanticVersion: version, ExpectedFeatureSchema: fixture.schema,
			Environment: step.environment, Actor: "deployer", Reason: "controlled environment promotion",
		}); err != nil {
			fixture.t.Fatalf("deploy %s to %s: %v", version, step.environment, err)
		}
	}
	if _, err := fixture.registry.Promote(context.Background(), PromoteRequest{
		ModelID: "mid-direction", SemanticVersion: version, ExpectedFeatureSchema: fixture.schema,
		Target: LifecycleLimitedRisk, Environment: "limited-risk", Actor: "deployer", Reason: "limited risk authorization",
	}); err != nil {
		fixture.t.Fatalf("limited-risk promotion %s: %v", version, err)
	}
	status, err := fixture.registry.Promote(context.Background(), PromoteRequest{
		ModelID: "mid-direction", SemanticVersion: version, ExpectedFeatureSchema: fixture.schema,
		Target: LifecycleProduction, Environment: "production", Actor: "production-operator",
		Reason: "explicit reviewed production promotion", ExplicitProductionAuthorization: true,
		AuthorizationReference: "production-change-2001",
	})
	if err != nil {
		fixture.t.Fatalf("production promotion %s: %v", version, err)
	}
	return status
}

func digestText(value string) string {
	digest := sha256.Sum256([]byte(value))
	return hex.EncodeToString(digest[:])
}

func requireErrorIs(t *testing.T, err error, target error) {
	t.Helper()
	if !errors.Is(err, target) {
		t.Fatalf("expected %v, got %v", target, err)
	}
}

func TestLifecycleRequiresExplicitTwoPersonProductionPromotion(t *testing.T) {
	fixture := newRegistryFixture(t)
	fixture.register("1.0.0", "", []byte("native-model-v1"))

	if _, err := fixture.registry.Approve(context.Background(), ApproveRequest{
		ModelID: "mid-direction", SemanticVersion: "1.0.0", ExpectedFeatureSchema: fixture.schema,
		Actor: "risk-approver", Reason: "too early", ApprovalReference: "change-early",
	}); !errors.Is(err, ErrInvalidTransition) {
		t.Fatalf("early approval was not rejected: %v", err)
	}
	fixture.validateAndApprove("1.0.0")

	if _, err := fixture.registry.DeployShadow(context.Background(), DeploymentRequest{
		ModelID: "mid-direction", SemanticVersion: "1.0.0", ExpectedFeatureSchema: fixture.schema,
		Environment: "shadow", Actor: "deployer", Reason: "shadow evidence",
	}); err != nil {
		t.Fatalf("deploy shadow: %v", err)
	}
	if _, err := fixture.registry.PromoteCanary(context.Background(), DeploymentRequest{
		ModelID: "mid-direction", SemanticVersion: "1.0.0", ExpectedFeatureSchema: fixture.schema,
		Environment: "canary", Actor: "deployer", Reason: "canary evidence",
	}); err != nil {
		t.Fatalf("promote canary: %v", err)
	}
	if _, err := fixture.registry.Promote(context.Background(), PromoteRequest{
		ModelID: "mid-direction", SemanticVersion: "1.0.0", ExpectedFeatureSchema: fixture.schema,
		Target: LifecycleLimitedRisk, Environment: "limited-risk", Actor: "deployer", Reason: "bounded risk evidence",
	}); err != nil {
		t.Fatalf("promote limited risk: %v", err)
	}
	_, err := fixture.registry.Promote(context.Background(), PromoteRequest{
		ModelID: "mid-direction", SemanticVersion: "1.0.0", ExpectedFeatureSchema: fixture.schema,
		Target: LifecycleProduction, Environment: "production", Actor: "production-operator", Reason: "missing authorization",
	})
	requireErrorIs(t, err, ErrProductionAuthorization)
	_, err = fixture.registry.Promote(context.Background(), PromoteRequest{
		ModelID: "mid-direction", SemanticVersion: "1.0.0", ExpectedFeatureSchema: fixture.schema,
		Target: LifecycleProduction, Environment: "production", Actor: "risk-approver", Reason: "same person",
		ExplicitProductionAuthorization: true, AuthorizationReference: "production-change-1",
	})
	requireErrorIs(t, err, ErrTwoPersonControl)

	status, err := fixture.registry.Promote(context.Background(), PromoteRequest{
		ModelID: "mid-direction", SemanticVersion: "1.0.0", ExpectedFeatureSchema: fixture.schema,
		Target: LifecycleProduction, Environment: "production", Actor: "production-operator", Reason: "explicit production promotion",
		ExplicitProductionAuthorization: true, AuthorizationReference: "production-change-1",
	})
	if err != nil {
		t.Fatalf("promote production: %v", err)
	}
	if status.Lifecycle != LifecycleProduction || status.Approval != ApprovalApproved || !status.Active || status.Revision != 9 {
		t.Fatalf("unexpected production status: %+v", status)
	}
	lineage, err := fixture.registry.InspectLineage(context.Background(), "mid-direction", "1.0.0")
	if err != nil {
		t.Fatalf("inspect lineage: %v", err)
	}
	if len(lineage.Entries) != 1 || len(lineage.Entries[0].Events) != 9 {
		t.Fatalf("unexpected lineage: %+v", lineage)
	}
	health := fixture.registry.Health()
	if !health.Healthy || !health.Ready || health.ConfigurationHash == "" || health.Build.LiveTradingCapable {
		t.Fatalf("unexpected health: %+v", health)
	}
}

func TestTamperedArtifactFailsValidationButCannotBlockDisable(t *testing.T) {
	fixture := newRegistryFixture(t)
	signed := fixture.register("1.0.0", "", []byte("untampered-model"))
	artifactPath := fixture.registry.artifactPath(signed.Manifest.ArtifactSHA256)
	if err := os.Chmod(artifactPath, 0o600); err != nil {
		t.Fatalf("chmod artifact: %v", err)
	}
	if err := os.WriteFile(artifactPath, []byte("tampered-model"), 0o600); err != nil {
		t.Fatalf("tamper artifact: %v", err)
	}
	_, err := fixture.registry.Validate(context.Background(), ValidateRequest{
		ModelID: "mid-direction", SemanticVersion: "1.0.0", Stage: ValidationTrained,
		ExpectedFeatureSchema: fixture.schema, Actor: "validator", Reason: "must detect tamper",
	})
	requireErrorIs(t, err, ErrArtifactHashMismatch)
	status, err := fixture.registry.Disable(context.Background(), DisableRequest{
		ModelID: "mid-direction", SemanticVersion: "1.0.0", Actor: "kill-operator", Reason: "artifact integrity failure",
	})
	if err != nil {
		t.Fatalf("disable tampered artifact: %v", err)
	}
	if status.Lifecycle != LifecycleDisabled || status.Approval != ApprovalRevoked || status.Active {
		t.Fatalf("unexpected disabled status: %+v", status)
	}
	if _, err := fixture.registry.InspectLineage(context.Background(), "mid-direction", "1.0.0"); !errors.Is(err, ErrArtifactHashMismatch) {
		t.Fatalf("inspect did not reject tampered artifact: %v", err)
	}
}

func TestTamperedManifestAndAuditLogAreRejected(t *testing.T) {
	t.Run("manifest", func(t *testing.T) {
		fixture := newRegistryFixture(t)
		fixture.register("1.0.0", "", []byte("model"))
		path := fixture.registry.manifestPath("mid-direction", "1.0.0")
		payload, err := os.ReadFile(path)
		if err != nil {
			t.Fatalf("read manifest: %v", err)
		}
		var signed SignedManifest
		if err := json.Unmarshal(payload, &signed); err != nil {
			t.Fatalf("decode manifest: %v", err)
		}
		signed.Manifest.Metrics[0].ValuePPM++
		payload, _ = json.Marshal(signed)
		if err := os.Chmod(path, 0o600); err != nil {
			t.Fatalf("chmod manifest: %v", err)
		}
		if err := os.WriteFile(path, payload, 0o600); err != nil {
			t.Fatalf("tamper manifest: %v", err)
		}
		_, err = fixture.registry.Validate(context.Background(), ValidateRequest{
			ModelID: "mid-direction", SemanticVersion: "1.0.0", Stage: ValidationTrained,
			ExpectedFeatureSchema: fixture.schema, Actor: "validator", Reason: "detect manifest tamper",
		})
		requireErrorIs(t, err, ErrSignature)
	})

	t.Run("audit", func(t *testing.T) {
		fixture := newRegistryFixture(t)
		fixture.register("1.0.0", "", []byte("model"))
		path := fixture.registry.eventPath("mid-direction", "1.0.0")
		file, err := os.OpenFile(path, os.O_APPEND|os.O_WRONLY, 0o600)
		if err != nil {
			t.Fatalf("open audit log: %v", err)
		}
		if _, err := file.WriteString("{\"truncated\":"); err != nil {
			t.Fatalf("tamper audit log: %v", err)
		}
		_ = file.Close()
		_, err = fixture.registry.InspectLineage(context.Background(), "mid-direction", "1.0.0")
		requireErrorIs(t, err, ErrRegistryIntegrity)
	})
}

func TestIncompatibleFeatureSchemaFailsClosedWithoutTransition(t *testing.T) {
	fixture := newRegistryFixture(t)
	fixture.register("1.0.0", "", []byte("model"))
	incompatible := fixture.schema
	incompatible.Fields = append([]FeatureField(nil), fixture.schema.Fields...)
	incompatible.Fields[0].Unit = "basis-points"
	var err error
	incompatible, err = FinalizeFeatureSchema(incompatible)
	if err != nil {
		t.Fatalf("finalize incompatible schema: %v", err)
	}
	_, err = fixture.registry.Validate(context.Background(), ValidateRequest{
		ModelID: "mid-direction", SemanticVersion: "1.0.0", Stage: ValidationTrained,
		ExpectedFeatureSchema: incompatible, Actor: "validator", Reason: "incompatible runtime schema",
	})
	requireErrorIs(t, err, ErrIncompatibleSchema)
	lineage, err := fixture.registry.InspectLineage(context.Background(), "mid-direction", "1.0.0")
	if err != nil {
		t.Fatalf("inspect after rejection: %v", err)
	}
	if lineage.Entries[0].Status.Lifecycle != LifecycleDraft || len(lineage.Entries[0].Events) != 1 {
		t.Fatalf("rejected validation mutated state: %+v", lineage.Entries[0])
	}
}

func TestRollbackRestoresApprovedVersionAndRevokesOutgoingVersion(t *testing.T) {
	fixture := newRegistryFixture(t)
	fixture.register("1.0.0", "", []byte("model-v1"))
	fixture.validateAndApprove("1.0.0")
	fixture.deployProduction("1.0.0")
	fixture.register("2.0.0", "1.0.0", []byte("model-v2"))
	fixture.validateAndApprove("2.0.0")
	fixture.deployProduction("2.0.0")

	status, err := fixture.registry.Rollback(context.Background(), RollbackRequest{
		ModelID: "mid-direction", SemanticVersion: "2.0.0", TargetSemanticVersion: "1.0.0",
		ExpectedFeatureSchema: fixture.schema, Environment: "production", Actor: "rollback-operator",
		Reason: "canary regression confirmed", AuthorizationReference: "rollback-change-3001",
	})
	if err != nil {
		t.Fatalf("rollback: %v", err)
	}
	if status.Lifecycle != LifecycleRolledBack || status.Approval != ApprovalRevoked || status.Active {
		t.Fatalf("unexpected rolled-back status: %+v", status)
	}
	pointer, err := fixture.registry.loadDeploymentPointer("mid-direction", "production")
	if err != nil {
		t.Fatalf("load production pointer: %v", err)
	}
	if pointer.Body.ActiveVersion != "1.0.0" || pointer.Body.Lifecycle != LifecycleProduction {
		t.Fatalf("rollback pointer did not restore v1: %+v", pointer)
	}
	lineage, err := fixture.registry.InspectLineage(context.Background(), "mid-direction", "2.0.0")
	if err != nil {
		t.Fatalf("inspect rollback lineage: %v", err)
	}
	if len(lineage.Entries) != 2 || lineage.Entries[1].Manifest.Manifest.SemanticVersion != "1.0.0" {
		t.Fatalf("parent lineage was not retained: %+v", lineage)
	}
	retired, err := fixture.registry.Retire(context.Background(), RetireRequest{
		ModelID: "mid-direction", SemanticVersion: "2.0.0", Actor: "registry-admin", Reason: "retire rolled-back artifact",
	})
	if err != nil || retired.Lifecycle != LifecycleRetired {
		t.Fatalf("retire rolled-back version: status=%+v err=%v", retired, err)
	}
	disabled, err := fixture.registry.Disable(context.Background(), DisableRequest{
		ModelID: "mid-direction", SemanticVersion: "1.0.0", Actor: "kill-operator", Reason: "global model disable",
	})
	if err != nil || disabled.Active || disabled.Lifecycle != LifecycleDisabled {
		t.Fatalf("disable restored version: status=%+v err=%v", disabled, err)
	}
	pointer, err = fixture.registry.loadDeploymentPointer("mid-direction", "production")
	if err != nil || pointer.Body.Lifecycle != LifecycleDisabled {
		t.Fatalf("production pointer was not disabled: pointer=%+v err=%v", pointer, err)
	}
}

func TestRegistrationIsIdempotentAndConcurrent(t *testing.T) {
	fixture := newRegistryFixture(t)
	artifact := []byte("concurrent-model")
	artifactPath := filepath.Join(t.TempDir(), "model.bin")
	if err := os.WriteFile(artifactPath, artifact, 0o600); err != nil {
		t.Fatalf("write artifact: %v", err)
	}
	signed := fixture.signedManifest("1.0.0", "", artifact)
	request := RegisterRequest{SignedManifest: signed, ArtifactPath: artifactPath, Actor: "trainer", Reason: "idempotent registration"}
	errorsByWorker := make(chan error, 2)
	for range 2 {
		go func() {
			_, err := fixture.registry.Register(context.Background(), request)
			errorsByWorker <- err
		}()
	}
	for range 2 {
		if err := <-errorsByWorker; err != nil {
			t.Fatalf("concurrent register: %v", err)
		}
	}
	lineage, err := fixture.registry.InspectLineage(context.Background(), "mid-direction", "1.0.0")
	if err != nil || len(lineage.Entries[0].Events) != 1 {
		t.Fatalf("idempotent register duplicated audit event: lineage=%+v err=%v", lineage, err)
	}

	changed := signed.Manifest
	changed.CreatedAtUTCNS++
	changedSigned, err := SignManifest(changed, "test-signer", fixture.privateKey)
	if err != nil {
		t.Fatalf("sign changed manifest: %v", err)
	}
	_, err = fixture.registry.Register(context.Background(), RegisterRequest{SignedManifest: changedSigned, ArtifactPath: artifactPath, Actor: "trainer", Reason: "conflicting registration"})
	requireErrorIs(t, err, ErrAlreadyRegistered)
}

func TestRegistrationRecoversVerifiedPreEventCrashArtifacts(t *testing.T) {
	fixture := newRegistryFixture(t)
	artifact := []byte("orphaned-before-event")
	path := filepath.Join(t.TempDir(), "model.bin")
	if err := os.WriteFile(path, artifact, 0o600); err != nil {
		t.Fatalf("write artifact: %v", err)
	}
	signed := fixture.signedManifest("1.0.0", "", artifact)
	if err := fixture.registry.storeArtifact(path, signed.Manifest.ArtifactSHA256); err != nil {
		t.Fatalf("store pre-crash artifact: %v", err)
	}
	if err := fixture.registry.storeManifest(signed); err != nil {
		t.Fatalf("store pre-crash manifest: %v", err)
	}
	status, err := fixture.registry.Register(context.Background(), RegisterRequest{
		SignedManifest: signed, ArtifactPath: path, Actor: "trainer", Reason: "retry after pre-event crash",
	})
	if err != nil || status.Lifecycle != LifecycleDraft || status.Revision != 1 {
		t.Fatalf("recover pre-event registration: status=%+v err=%v", status, err)
	}
}

func TestUntrustedSignatureAndInvalidManifestAreRejected(t *testing.T) {
	fixture := newRegistryFixture(t)
	artifact := []byte("model")
	signed := fixture.signedManifest("1.0.0", "", artifact)
	otherSeed := sha256.Sum256([]byte("untrusted-key"))
	otherPrivate := ed25519.NewKeyFromSeed(otherSeed[:])
	signed, err := SignManifest(signed.Manifest, "other-signer", otherPrivate)
	if err != nil {
		t.Fatalf("sign untrusted manifest: %v", err)
	}
	artifactPath := filepath.Join(t.TempDir(), "model.bin")
	if err := os.WriteFile(artifactPath, artifact, 0o600); err != nil {
		t.Fatalf("write artifact: %v", err)
	}
	_, err = fixture.registry.Register(context.Background(), RegisterRequest{SignedManifest: signed, ArtifactPath: artifactPath, Actor: "trainer", Reason: "untrusted signer"})
	requireErrorIs(t, err, ErrSignature)

	invalid := signed.Manifest
	invalid.CodeCommit = "short"
	if _, err := SignManifest(invalid, "test-signer", fixture.privateKey); !errors.Is(err, ErrInvalidManifest) {
		t.Fatalf("invalid manifest was signed: %v", err)
	}
	invalidSchema := fixture.schema
	invalidSchema.Fields[0].Type = "float64"
	if _, err := FinalizeFeatureSchema(invalidSchema); err == nil {
		t.Fatal("floating-point hot-path feature type was accepted")
	}
}

func TestReadOnlyRegistryAndCancellationFailClosed(t *testing.T) {
	fixture := newRegistryFixture(t)
	readOnly, err := Open(Config{Root: fixture.root, TrustedKeys: map[string]ed25519.PublicKey{"test-signer": fixture.publicKey}, Clock: fixture.clock.Now})
	if err != nil {
		t.Fatalf("open read-only registry: %v", err)
	}
	health := readOnly.Health()
	if !health.Healthy || health.Ready {
		t.Fatalf("unexpected read-only health: %+v", health)
	}
	artifact := []byte("model")
	path := filepath.Join(t.TempDir(), "model.bin")
	if err := os.WriteFile(path, artifact, 0o600); err != nil {
		t.Fatalf("write artifact: %v", err)
	}
	_, err = readOnly.Register(context.Background(), RegisterRequest{
		SignedManifest: fixture.signedManifest("1.0.0", "", artifact), ArtifactPath: path,
		Actor: "trainer", Reason: "read-only mutation",
	})
	if err == nil || !strings.Contains(err.Error(), "read-only") {
		t.Fatalf("read-only mutation was not rejected: %v", err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err := fixture.registry.InspectLineage(ctx, "mid-direction", "1.0.0"); !errors.Is(err, context.Canceled) {
		t.Fatalf("cancelled inspection did not fail: %v", err)
	}
}

func TestTamperedDeploymentPointerIsRejected(t *testing.T) {
	fixture := newRegistryFixture(t)
	fixture.register("1.0.0", "", []byte("model"))
	fixture.validateAndApprove("1.0.0")
	if _, err := fixture.registry.DeployShadow(context.Background(), DeploymentRequest{
		ModelID: "mid-direction", SemanticVersion: "1.0.0", ExpectedFeatureSchema: fixture.schema,
		Environment: "shadow", Actor: "deployer", Reason: "shadow",
	}); err != nil {
		t.Fatalf("deploy shadow: %v", err)
	}
	path := fixture.registry.deploymentPath("shadow", "mid-direction")
	payload, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read pointer: %v", err)
	}
	var pointer SignedDeploymentPointer
	if err := json.Unmarshal(payload, &pointer); err != nil {
		t.Fatalf("decode pointer: %v", err)
	}
	pointer.Body.ActiveVersion = "9.9.9"
	payload, _ = json.Marshal(pointer)
	if err := os.WriteFile(path, payload, 0o600); err != nil {
		t.Fatalf("tamper pointer: %v", err)
	}
	_, err = fixture.registry.InspectLineage(context.Background(), "mid-direction", "1.0.0")
	requireErrorIs(t, err, ErrRegistryIntegrity)
}

func TestSignedPointerDetachedFromLifecycleIsRejected(t *testing.T) {
	fixture := newRegistryFixture(t)
	fixture.register("1.0.0", "", []byte("model"))
	fixture.validateAndApprove("1.0.0")
	if _, err := fixture.registry.DeployShadow(context.Background(), DeploymentRequest{
		ModelID: "mid-direction", SemanticVersion: "1.0.0", ExpectedFeatureSchema: fixture.schema,
		Environment: "shadow", Actor: "deployer", Reason: "shadow",
	}); err != nil {
		t.Fatalf("deploy shadow: %v", err)
	}
	pointer, err := fixture.registry.loadDeploymentPointer("mid-direction", "shadow")
	if err != nil {
		t.Fatalf("load pointer: %v", err)
	}
	pointer.Body.SourceEventSHA256 = digestText("not-a-lifecycle-event")
	detached, err := signDeploymentPointer(pointer.Body, "test-signer", fixture.privateKey)
	if err != nil {
		t.Fatalf("sign detached pointer fixture: %v", err)
	}
	writeJSONFile(t, fixture.registry.deploymentPath("shadow", "mid-direction"), detached)
	_, err = fixture.registry.InspectLineage(context.Background(), "mid-direction", "1.0.0")
	requireErrorIs(t, err, ErrRegistryIntegrity)
}

func TestCLIExecutesAuditedLifecycleOperations(t *testing.T) {
	fixture := newRegistryFixture(t)
	cliRoot := filepath.Join(t.TempDir(), "registry")
	privatePath := filepath.Join(t.TempDir(), "private.key")
	publicPath := filepath.Join(t.TempDir(), "public.key")
	manifestPath := filepath.Join(t.TempDir(), "manifest.json")
	schemaPath := filepath.Join(t.TempDir(), "schema.json")
	artifactPath := filepath.Join(t.TempDir(), "model.bin")
	artifact := []byte("cli-model")
	manifest := fixture.signedManifest("1.0.0", "", artifact).Manifest
	writeJSONFile(t, manifestPath, manifest)
	writeJSONFile(t, schemaPath, fixture.schema)
	if err := os.WriteFile(artifactPath, artifact, 0o600); err != nil {
		t.Fatalf("write artifact: %v", err)
	}
	if err := os.WriteFile(privatePath, []byte(base64.StdEncoding.EncodeToString(fixture.privateKey)), 0o600); err != nil {
		t.Fatalf("write private key: %v", err)
	}
	if err := os.WriteFile(publicPath, []byte(base64.StdEncoding.EncodeToString(fixture.publicKey)), 0o600); err != nil {
		t.Fatalf("write public key: %v", err)
	}
	mutation := []string{"--root", cliRoot, "--key-id", "test-signer", "--private-key", privatePath}
	actorReason := func(actor string) []string {
		return []string{"--actor", actor, "--reason", "deterministic CLI test"}
	}
	invoke := func(arguments []string, wantCode int) string {
		t.Helper()
		var stdout bytes.Buffer
		var stderr bytes.Buffer
		code := RunCLI(context.Background(), arguments, strings.NewReader(""), &stdout, &stderr)
		if code != wantCode {
			t.Fatalf("CLI %v code=%d want=%d stderr=%s", arguments, code, wantCode, stderr.String())
		}
		if strings.Contains(stdout.String(), base64.StdEncoding.EncodeToString(fixture.privateKey)) || strings.Contains(stderr.String(), base64.StdEncoding.EncodeToString(fixture.privateKey)) {
			t.Fatal("CLI exposed private key")
		}
		return stdout.String()
	}

	invoke(nil, 2)
	invoke([]string{"unknown"}, 2)
	invoke([]string{"version"}, 0)
	invoke([]string{"health", "--root", cliRoot, "--key-id", "test-signer", "--public-key", publicPath}, 0)
	registerArgs := append([]string{"register"}, mutation...)
	registerArgs = append(registerArgs, actorReason("trainer")...)
	registerArgs = append(registerArgs, "--manifest", manifestPath, "--artifact", artifactPath)
	invoke(registerArgs, 0)
	for _, stage := range []string{"trained", "offline", "replay"} {
		arguments := append([]string{"validate"}, mutation...)
		arguments = append(arguments, actorReason("validator")...)
		arguments = append(arguments, "--model-id", "mid-direction", "--version", "1.0.0", "--stage", stage, "--feature-schema", schemaPath)
		invoke(arguments, 0)
	}
	approveArgs := append([]string{"approve"}, mutation...)
	approveArgs = append(approveArgs, actorReason("risk-approver")...)
	approveArgs = append(approveArgs, "--model-id", "mid-direction", "--version", "1.0.0", "--feature-schema", schemaPath, "--approval-reference", "approval-cli-1")
	invoke(approveArgs, 0)
	for _, command := range []struct {
		name        string
		environment string
	}{
		{name: "deploy-shadow", environment: "shadow"},
		{name: "promote-canary", environment: "canary"},
	} {
		arguments := append([]string{command.name}, mutation...)
		arguments = append(arguments, actorReason("deployer")...)
		arguments = append(arguments, "--model-id", "mid-direction", "--version", "1.0.0", "--feature-schema", schemaPath, "--environment", command.environment)
		invoke(arguments, 0)
	}
	limitedArgs := append([]string{"promote"}, mutation...)
	limitedArgs = append(limitedArgs, actorReason("deployer")...)
	limitedArgs = append(limitedArgs, "--model-id", "mid-direction", "--version", "1.0.0", "--feature-schema", schemaPath, "--target", "LIMITED_RISK", "--environment", "limited-risk")
	invoke(limitedArgs, 0)
	productionArgs := append([]string{"promote"}, mutation...)
	productionArgs = append(productionArgs, actorReason("production-operator")...)
	productionArgs = append(productionArgs, "--model-id", "mid-direction", "--version", "1.0.0", "--feature-schema", schemaPath, "--target", "PRODUCTION", "--environment", "production", "--explicit-production", "--authorization-reference", "production-cli-1")
	invoke(productionArgs, 0)
	lineageOutput := invoke([]string{"inspect-lineage", "--root", cliRoot, "--key-id", "test-signer", "--public-key", publicPath, "--model-id", "mid-direction", "--version", "1.0.0"}, 0)
	if !strings.Contains(lineageOutput, `"lifecycle": "PRODUCTION"`) {
		t.Fatalf("CLI lineage omitted production state: %s", lineageOutput)
	}
	disableArgs := append([]string{"disable"}, mutation...)
	disableArgs = append(disableArgs, actorReason("kill-operator")...)
	disableArgs = append(disableArgs, "--model-id", "mid-direction", "--version", "1.0.0")
	invoke(disableArgs, 0)
}

func writeJSONFile(t *testing.T, path string, value any) {
	t.Helper()
	payload, err := json.MarshalIndent(value, "", "  ")
	if err != nil {
		t.Fatalf("encode JSON: %v", err)
	}
	if err := os.WriteFile(path, payload, 0o600); err != nil {
		t.Fatalf("write JSON: %v", err)
	}
}

func BenchmarkVerifySignedManifest(b *testing.B) {
	fixture := newRegistryFixture(b)
	signed := fixture.signedManifest("1.0.0", "", []byte("benchmark-model"))
	trusted := map[string]ed25519.PublicKey{"test-signer": fixture.publicKey}
	b.ReportAllocs()
	b.ResetTimer()
	for range b.N {
		if err := VerifySignedManifest(signed, trusted); err != nil {
			b.Fatalf("verify signed manifest: %v", err)
		}
	}
}

func BenchmarkFeatureSchemaCompatibility(b *testing.B) {
	fixture := newRegistryFixture(b)
	b.ReportAllocs()
	b.ResetTimer()
	for range b.N {
		if err := CompatibleFeatureSchema(fixture.schema, fixture.schema); err != nil {
			b.Fatalf("compatible schema: %v", err)
		}
	}
}

func BenchmarkInspectLineage(b *testing.B) {
	fixture := newRegistryFixture(b)
	fixture.register("1.0.0", "", []byte("benchmark-model"))
	fixture.validateAndApprove("1.0.0")
	b.ReportAllocs()
	b.ResetTimer()
	for range b.N {
		if _, err := fixture.registry.InspectLineage(context.Background(), "mid-direction", "1.0.0"); err != nil {
			b.Fatalf("inspect lineage: %v", err)
		}
	}
}
