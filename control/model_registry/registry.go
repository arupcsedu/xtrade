package modelregistry

import (
	"bufio"
	"bytes"
	"context"
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math"
	"os"
	"path/filepath"
	"reflect"
	"sort"
	"strings"
	"sync"
	"syscall"
	"time"
)

// Clock supplies auditable wall-clock UTC timestamps. Lifecycle order is based
// on revisions and hashes, never on this clock.
type Clock func() time.Time

// Config defines local registry storage and signature trust.
type Config struct {
	Root        string
	TrustedKeys map[string]ed25519.PublicKey
	SignerKeyID string
	Signer      ed25519.PrivateKey
	Clock       Clock
}

// Registry is a filesystem-backed, signed, append-only control-plane registry.
type Registry struct {
	root        string
	trustedKeys map[string]ed25519.PublicKey
	signerKeyID string
	signer      ed25519.PrivateKey
	clock       Clock
	mu          sync.Mutex
}

// RegisterRequest creates one immutable artifact and DRAFT lifecycle.
type RegisterRequest struct {
	SignedManifest SignedManifest
	ArtifactPath   string
	Actor          string
	Reason         string
}

// ValidateRequest advances exactly one validation stage.
type ValidateRequest struct {
	ModelID               string
	SemanticVersion       string
	Stage                 ValidationStage
	ExpectedFeatureSchema FeatureSchema
	Actor                 string
	Reason                string
}

// ApproveRequest records an explicit approval after replay validation.
type ApproveRequest struct {
	ModelID               string
	SemanticVersion       string
	ExpectedFeatureSchema FeatureSchema
	Actor                 string
	Reason                string
	ApprovalReference     string
}

// DeploymentRequest deploys to shadow or promotes to canary.
type DeploymentRequest struct {
	ModelID                string
	SemanticVersion        string
	ExpectedFeatureSchema  FeatureSchema
	Environment            string
	Actor                  string
	Reason                 string
	AuthorizationReference string
}

// PromoteRequest advances canary to limited risk or limited risk to production.
type PromoteRequest struct {
	ModelID                         string
	SemanticVersion                 string
	ExpectedFeatureSchema           FeatureSchema
	Target                          LifecycleState
	Environment                     string
	Actor                           string
	Reason                          string
	ExplicitProductionAuthorization bool
	AuthorizationReference          string
}

// RollbackRequest marks the active version rolled back and atomically points
// the named environment at a previously approved compatible version.
type RollbackRequest struct {
	ModelID                string
	SemanticVersion        string
	TargetSemanticVersion  string
	ExpectedFeatureSchema  FeatureSchema
	Environment            string
	Actor                  string
	Reason                 string
	AuthorizationReference string
}

// DisableRequest immediately revokes one model version across all environments.
type DisableRequest struct {
	ModelID         string
	SemanticVersion string
	Actor           string
	Reason          string
}

// RetireRequest permanently retires an already disabled or rolled-back version.
type RetireRequest struct {
	ModelID         string
	SemanticVersion string
	Actor           string
	Reason          string
}

// Open validates the registry configuration and creates private local storage
// directories. A signer is optional for read-only inspection.
func Open(config Config) (*Registry, error) {
	if strings.TrimSpace(config.Root) == "" {
		return nil, errors.New("registry root is required")
	}
	trusted := make(map[string]ed25519.PublicKey, len(config.TrustedKeys)+1)
	for keyID, publicKey := range config.TrustedKeys {
		if !validIdentifier(keyID) || len(publicKey) != ed25519.PublicKeySize {
			return nil, fmt.Errorf("invalid trusted key %q", keyID)
		}
		trusted[keyID] = append(ed25519.PublicKey(nil), publicKey...)
	}
	if len(config.Signer) != 0 {
		if !validIdentifier(config.SignerKeyID) || len(config.Signer) != ed25519.PrivateKeySize {
			return nil, errors.New("signer key ID and Ed25519 private key must both be valid")
		}
		publicKey := config.Signer.Public().(ed25519.PublicKey)
		if existing, ok := trusted[config.SignerKeyID]; ok && !bytes.Equal(existing, publicKey) {
			return nil, errors.New("signer public key conflicts with trust store")
		}
		trusted[config.SignerKeyID] = append(ed25519.PublicKey(nil), publicKey...)
	}
	if len(trusted) == 0 {
		return nil, errors.New("at least one trusted public key is required")
	}
	clock := config.Clock
	if clock == nil {
		clock = time.Now
	}
	root, err := filepath.Abs(config.Root)
	if err != nil {
		return nil, fmt.Errorf("resolve registry root: %w", err)
	}
	for _, directory := range []string{"artifacts", "manifests", "events", "deployments"} {
		if err := os.MkdirAll(filepath.Join(root, directory), 0o700); err != nil {
			return nil, fmt.Errorf("create registry directory: %w", err)
		}
	}
	return &Registry{
		root:        root,
		trustedKeys: trusted,
		signerKeyID: config.SignerKeyID,
		signer:      append(ed25519.PrivateKey(nil), config.Signer...),
		clock:       clock,
	}, nil
}

// Health reports local readiness and a secret-free trust configuration hash.
func (registry *Registry) Health() HealthState {
	type keyDigest struct {
		KeyID        string `json:"key_id"`
		PublicSHA256 string `json:"public_sha256"`
	}
	keys := make([]keyDigest, 0, len(registry.trustedKeys))
	for keyID, publicKey := range registry.trustedKeys {
		digest := sha256.Sum256(publicKey)
		keys = append(keys, keyDigest{KeyID: keyID, PublicSHA256: hex.EncodeToString(digest[:])})
	}
	sort.Slice(keys, func(left int, right int) bool { return keys[left].KeyID < keys[right].KeyID })
	configurationHash, err := canonicalSHA256(struct {
		SchemaVersion uint32      `json:"schema_version"`
		TrustedKeys   []keyDigest `json:"trusted_keys"`
	}{SchemaVersion: SchemaVersion, TrustedKeys: keys})
	if err != nil {
		return HealthState{Healthy: false, Ready: false, Reason: err.Error(), Build: CurrentBuildInfo()}
	}
	ready := len(registry.signer) == ed25519.PrivateKeySize
	reason := "read-only: no lifecycle signer configured"
	if ready {
		reason = "ready"
	}
	return HealthState{Healthy: true, Ready: ready, Reason: reason, ConfigurationHash: configurationHash, Build: CurrentBuildInfo()}
}

// Register persists a content-addressed artifact, immutable signed manifest,
// and initial signed lifecycle event.
func (registry *Registry) Register(ctx context.Context, request RegisterRequest) (ModelStatus, error) {
	if err := contextError(ctx); err != nil {
		return ModelStatus{}, err
	}
	if len(registry.signer) != ed25519.PrivateKeySize {
		return ModelStatus{}, errors.New("registry is read-only: lifecycle signer unavailable")
	}
	if err := validateActorReason(request.Actor, request.Reason); err != nil {
		return ModelStatus{}, err
	}
	if err := VerifySignedManifest(request.SignedManifest, registry.trustedKeys); err != nil {
		return ModelStatus{}, err
	}
	if strings.TrimSpace(request.ArtifactPath) == "" {
		return ModelStatus{}, errors.New("artifact path is required")
	}
	manifest := request.SignedManifest.Manifest
	var status ModelStatus
	err := registry.withExclusiveLock(ctx, func() error {
		if _, err := os.Stat(registry.eventPath(manifest.ModelID, manifest.SemanticVersion)); err == nil {
			stored, loadErr := registry.loadVerified(manifest.ModelID, manifest.SemanticVersion, nil)
			if loadErr != nil {
				return loadErr
			}
			if !reflect.DeepEqual(stored, request.SignedManifest) {
				return ErrAlreadyRegistered
			}
			events, loadErr := registry.loadEvents(manifest.ModelID, manifest.SemanticVersion)
			if loadErr != nil {
				return loadErr
			}
			status, loadErr = registry.statusFromEvents(events)
			return loadErr
		} else if !errors.Is(err, os.ErrNotExist) {
			return fmt.Errorf("inspect lifecycle events: %w", err)
		}
		artifactDigest, err := hashFile(request.ArtifactPath)
		if err != nil {
			return err
		}
		if artifactDigest != manifest.ArtifactSHA256 {
			return fmt.Errorf("%w: expected %s, got %s", ErrArtifactHashMismatch, manifest.ArtifactSHA256, artifactDigest)
		}
		if manifest.ParentSemanticVersion != "" {
			if _, err := registry.loadVerified(manifest.ModelID, manifest.ParentSemanticVersion, nil); err != nil {
				return fmt.Errorf("parent model version: %w", err)
			}
		}
		if err := registry.storeArtifact(request.ArtifactPath, manifest.ArtifactSHA256); err != nil {
			return err
		}
		stored, loadErr := registry.loadSignedManifest(manifest.ModelID, manifest.SemanticVersion)
		if errors.Is(loadErr, ErrNotFound) {
			if err := registry.storeManifest(request.SignedManifest); err != nil {
				return err
			}
		} else if loadErr != nil {
			return loadErr
		} else if !reflect.DeepEqual(stored, request.SignedManifest) {
			return fmt.Errorf("%w: orphan manifest conflicts with registration", ErrRegistryIntegrity)
		}
		body := LifecycleEventBody{
			SchemaVersion:   SchemaVersion,
			ModelID:         manifest.ModelID,
			SemanticVersion: manifest.SemanticVersion,
			Revision:        1,
			ToLifecycle:     LifecycleDraft,
			ApprovalState:   ApprovalPending,
			DeploymentState: DeploymentNotDeployed,
			Actor:           request.Actor,
			Reason:          request.Reason,
			OccurredAtUTCNS: registry.nowUTCNS(),
		}
		event, err := registry.signEvent(body)
		if err != nil {
			return err
		}
		if err := registry.appendEvent(event); err != nil {
			return err
		}
		status, err = registry.statusFromEvents([]SignedLifecycleEvent{event})
		return err
	})
	return status, err
}

// Validate verifies all immutable bytes and exact feature compatibility before
// advancing one explicit validation stage.
func (registry *Registry) Validate(ctx context.Context, request ValidateRequest) (ModelStatus, error) {
	targets := map[ValidationStage]struct {
		from LifecycleState
		to   LifecycleState
	}{
		ValidationTrained: {from: LifecycleDraft, to: LifecycleTrained},
		ValidationOffline: {from: LifecycleTrained, to: LifecycleOfflineValidated},
		ValidationReplay:  {from: LifecycleOfflineValidated, to: LifecycleReplayValidated},
	}
	target, ok := targets[request.Stage]
	if !ok {
		return ModelStatus{}, errors.New("validation stage must be trained, offline, or replay")
	}
	return registry.transition(ctx, transitionRequest{
		modelID: request.ModelID, semanticVersion: request.SemanticVersion,
		expectedSchema: &request.ExpectedFeatureSchema, from: target.from, to: target.to,
		actor: request.Actor, reason: request.Reason,
	})
}

// Approve records an auditable approval without changing validation state.
func (registry *Registry) Approve(ctx context.Context, request ApproveRequest) (ModelStatus, error) {
	if strings.TrimSpace(request.ApprovalReference) == "" || len(request.ApprovalReference) > 256 {
		return ModelStatus{}, errors.New("approval reference is required")
	}
	return registry.transition(ctx, transitionRequest{
		modelID: request.ModelID, semanticVersion: request.SemanticVersion,
		expectedSchema: &request.ExpectedFeatureSchema,
		from:           LifecycleReplayValidated, to: LifecycleReplayValidated,
		actor: request.Actor, reason: request.Reason,
		authorizationReference: request.ApprovalReference, approve: true,
	})
}

// DeployShadow activates only the named shadow environment.
func (registry *Registry) DeployShadow(ctx context.Context, request DeploymentRequest) (ModelStatus, error) {
	return registry.transition(ctx, transitionRequest{
		modelID: request.ModelID, semanticVersion: request.SemanticVersion,
		expectedSchema: &request.ExpectedFeatureSchema,
		from:           LifecycleReplayValidated, to: LifecycleShadow,
		environment: request.Environment, actor: request.Actor, reason: request.Reason,
		updatePointer: true,
	})
}

// PromoteCanary advances an approved shadow version to canary.
func (registry *Registry) PromoteCanary(ctx context.Context, request DeploymentRequest) (ModelStatus, error) {
	return registry.transition(ctx, transitionRequest{
		modelID: request.ModelID, semanticVersion: request.SemanticVersion,
		expectedSchema: &request.ExpectedFeatureSchema,
		from:           LifecycleShadow, to: LifecycleCanary,
		environment: request.Environment, actor: request.Actor, reason: request.Reason,
		authorizationReference: request.AuthorizationReference,
		updatePointer:          true,
	})
}

// Promote performs only the explicitly requested limited-risk or production
// transition. Production requires a separate approval actor and external change
// authorization reference; it is never inferred or automatic.
func (registry *Registry) Promote(ctx context.Context, request PromoteRequest) (ModelStatus, error) {
	from := LifecycleCanary
	if request.Target == LifecycleProduction {
		from = LifecycleLimitedRisk
		if !request.ExplicitProductionAuthorization || strings.TrimSpace(request.AuthorizationReference) == "" || len(request.AuthorizationReference) > 256 {
			return ModelStatus{}, ErrProductionAuthorization
		}
	} else if request.Target != LifecycleLimitedRisk {
		return ModelStatus{}, errors.New("promotion target must be LIMITED_RISK or PRODUCTION")
	}
	return registry.transition(ctx, transitionRequest{
		modelID: request.ModelID, semanticVersion: request.SemanticVersion,
		expectedSchema: &request.ExpectedFeatureSchema, from: from, to: request.Target,
		environment: request.Environment, actor: request.Actor, reason: request.Reason,
		authorizationReference: request.AuthorizationReference,
		requireTwoPerson:       request.Target == LifecycleProduction, updatePointer: true,
	})
}

// Rollback atomically changes the deployment materialization only after the
// signed rollback event is durable. It never modifies either artifact.
func (registry *Registry) Rollback(ctx context.Context, request RollbackRequest) (ModelStatus, error) {
	if err := contextError(ctx); err != nil {
		return ModelStatus{}, err
	}
	if err := validateActorReason(request.Actor, request.Reason); err != nil {
		return ModelStatus{}, err
	}
	if !validIdentifier(request.Environment) || strings.TrimSpace(request.AuthorizationReference) == "" || len(request.AuthorizationReference) > 256 {
		return ModelStatus{}, errors.New("valid environment and authorization reference are required")
	}
	if request.SemanticVersion == request.TargetSemanticVersion {
		return ModelStatus{}, errors.New("rollback target must differ from current version")
	}
	var status ModelStatus
	err := registry.withExclusiveLock(ctx, func() error {
		_, sourceEvents, sourceStatus, err := registry.loadVersion(request.ModelID, request.SemanticVersion, &request.ExpectedFeatureSchema)
		if err != nil {
			return err
		}
		_, targetEvents, targetStatus, err := registry.loadVersion(request.ModelID, request.TargetSemanticVersion, &request.ExpectedFeatureSchema)
		if err != nil {
			return fmt.Errorf("rollback target: %w", err)
		}
		if sourceStatus.Lifecycle != LifecycleShadow && sourceStatus.Lifecycle != LifecycleCanary && sourceStatus.Lifecycle != LifecycleLimitedRisk && sourceStatus.Lifecycle != LifecycleProduction {
			return fmt.Errorf("%w: %s cannot be rolled back", ErrInvalidTransition, sourceStatus.Lifecycle)
		}
		if targetStatus.Approval != ApprovalApproved || (targetStatus.Lifecycle != LifecycleShadow && targetStatus.Lifecycle != LifecycleCanary && targetStatus.Lifecycle != LifecycleLimitedRisk && targetStatus.Lifecycle != LifecycleProduction) {
			return fmt.Errorf("%w: rollback target is not approved and deployed", ErrInvalidTransition)
		}
		pointer, err := registry.loadDeploymentPointer(request.ModelID, request.Environment)
		if err != nil {
			return err
		}
		if pointer.Body.ActiveVersion != request.SemanticVersion {
			return fmt.Errorf("%w: source is not active in %s", ErrInvalidTransition, request.Environment)
		}
		last := sourceEvents[len(sourceEvents)-1]
		if sourceStatus.Revision == math.MaxUint64 {
			return fmt.Errorf("%w: lifecycle revision exhausted", ErrInvalidTransition)
		}
		body := LifecycleEventBody{
			SchemaVersion: SchemaVersion, ModelID: request.ModelID, SemanticVersion: request.SemanticVersion,
			Revision: sourceStatus.Revision + 1, FromLifecycle: sourceStatus.Lifecycle, ToLifecycle: LifecycleRolledBack,
			ApprovalState: ApprovalRevoked, DeploymentState: DeploymentRolledBack, Environment: request.Environment,
			Actor: request.Actor, Reason: request.Reason, AuthorizationReference: request.AuthorizationReference,
			ApprovalActor: sourceStatus.ApprovalActor, RollbackTargetVersion: request.TargetSemanticVersion,
			OccurredAtUTCNS: registry.nowUTCNS(), PreviousEventSHA256: last.EventSHA256,
		}
		event, err := registry.signEvent(body)
		if err != nil {
			return err
		}
		if err := registry.appendEvent(event); err != nil {
			return err
		}
		if err := registry.disableDeploymentPointers(request.ModelID, request.SemanticVersion, event, request.Actor); err != nil {
			return err
		}
		targetLast := targetEvents[len(targetEvents)-1]
		if err := registry.writeDeploymentPointer(request.ModelID, request.Environment, request.TargetSemanticVersion, targetStatus.Lifecycle, targetLast.EventSHA256, event.EventSHA256, request.Actor); err != nil {
			return err
		}
		status, err = registry.statusFromEvents(append(sourceEvents, event))
		return err
	})
	return status, err
}

// Disable revokes approval and updates every active pointer for this version.
// Artifact corruption does not prevent this safety action; the signed event
// chain itself must still be intact.
func (registry *Registry) Disable(ctx context.Context, request DisableRequest) (ModelStatus, error) {
	if err := contextError(ctx); err != nil {
		return ModelStatus{}, err
	}
	if err := validateActorReason(request.Actor, request.Reason); err != nil {
		return ModelStatus{}, err
	}
	var status ModelStatus
	err := registry.withExclusiveLock(ctx, func() error {
		events, err := registry.loadEvents(request.ModelID, request.SemanticVersion)
		if err != nil {
			return err
		}
		current, err := registry.statusFromEvents(events)
		if err != nil {
			return err
		}
		if current.Lifecycle == LifecycleDisabled {
			last := events[len(events)-1]
			if err := registry.disableDeploymentPointers(request.ModelID, request.SemanticVersion, last, request.Actor); err != nil {
				return err
			}
			status, err = registry.statusFromEvents(events)
			return err
		}
		if current.Lifecycle == LifecycleRetired {
			return fmt.Errorf("%w: retired model cannot transition", ErrInvalidTransition)
		}
		last := events[len(events)-1]
		if current.Revision == math.MaxUint64 {
			return fmt.Errorf("%w: lifecycle revision exhausted", ErrInvalidTransition)
		}
		body := LifecycleEventBody{
			SchemaVersion: SchemaVersion, ModelID: request.ModelID, SemanticVersion: request.SemanticVersion,
			Revision: current.Revision + 1, FromLifecycle: current.Lifecycle, ToLifecycle: LifecycleDisabled,
			ApprovalState: ApprovalRevoked, DeploymentState: DeploymentDisabled,
			Actor: request.Actor, Reason: request.Reason, ApprovalActor: current.ApprovalActor,
			OccurredAtUTCNS: registry.nowUTCNS(), PreviousEventSHA256: last.EventSHA256,
		}
		event, err := registry.signEvent(body)
		if err != nil {
			return err
		}
		if err := registry.appendEvent(event); err != nil {
			return err
		}
		if err := registry.disableDeploymentPointers(request.ModelID, request.SemanticVersion, event, request.Actor); err != nil {
			return err
		}
		status, err = registry.statusFromEvents(append(events, event))
		return err
	})
	return status, err
}

// Retire permanently transitions a disabled or rolled-back version.
func (registry *Registry) Retire(ctx context.Context, request RetireRequest) (ModelStatus, error) {
	return registry.transition(ctx, transitionRequest{
		modelID: request.ModelID, semanticVersion: request.SemanticVersion,
		fromAlternatives: []LifecycleState{LifecycleDisabled, LifecycleRolledBack}, to: LifecycleRetired,
		actor: request.Actor, reason: request.Reason, revoke: true,
	})
}

// InspectLineage verifies artifact bytes, signatures, event chains, deployment
// pointers, and parent links before returning lineage newest-first.
func (registry *Registry) InspectLineage(ctx context.Context, modelID string, semanticVersion string) (Lineage, error) {
	if err := contextError(ctx); err != nil {
		return Lineage{}, err
	}
	if !validIdentifier(modelID) || !semanticPattern.MatchString(semanticVersion) {
		return Lineage{}, ErrNotFound
	}
	lineage := Lineage{ModelID: modelID}
	seen := make(map[string]struct{})
	current := semanticVersion
	for current != "" {
		if _, exists := seen[current]; exists || len(seen) >= 256 {
			return Lineage{}, fmt.Errorf("%w: cyclic or excessive lineage", ErrRegistryIntegrity)
		}
		seen[current] = struct{}{}
		manifest, events, status, err := registry.loadVersion(modelID, current, nil)
		if err != nil {
			return Lineage{}, err
		}
		lineage.Entries = append(lineage.Entries, LineageEntry{Manifest: manifest, Status: status, Events: events})
		current = manifest.Manifest.ParentSemanticVersion
	}
	return lineage, nil
}

type transitionRequest struct {
	modelID                string
	semanticVersion        string
	expectedSchema         *FeatureSchema
	from                   LifecycleState
	fromAlternatives       []LifecycleState
	to                     LifecycleState
	environment            string
	actor                  string
	reason                 string
	authorizationReference string
	approve                bool
	revoke                 bool
	requireTwoPerson       bool
	updatePointer          bool
}

func (registry *Registry) transition(ctx context.Context, request transitionRequest) (ModelStatus, error) {
	if err := contextError(ctx); err != nil {
		return ModelStatus{}, err
	}
	if err := validateActorReason(request.actor, request.reason); err != nil {
		return ModelStatus{}, err
	}
	if request.updatePointer && !validIdentifier(request.environment) {
		return ModelStatus{}, errors.New("valid deployment environment is required")
	}
	var status ModelStatus
	err := registry.withExclusiveLock(ctx, func() error {
		_, events, current, err := registry.loadVersion(request.modelID, request.semanticVersion, request.expectedSchema)
		if err != nil {
			return err
		}
		fromValid := current.Lifecycle == request.from
		for _, alternative := range request.fromAlternatives {
			fromValid = fromValid || current.Lifecycle == alternative
		}
		if !fromValid {
			return fmt.Errorf("%w: expected %s, got %s", ErrInvalidTransition, request.from, current.Lifecycle)
		}
		if request.approve {
			if current.Approval != ApprovalPending {
				return fmt.Errorf("%w: approval is already %s", ErrInvalidTransition, current.Approval)
			}
		} else if request.to == LifecycleShadow || request.to == LifecycleCanary || request.to == LifecycleLimitedRisk || request.to == LifecycleProduction {
			if current.Approval != ApprovalApproved {
				return fmt.Errorf("%w: deployment requires approval", ErrInvalidTransition)
			}
		}
		if request.requireTwoPerson && current.ApprovalActor == request.actor {
			return ErrTwoPersonControl
		}
		approval := current.Approval
		approvalActor := current.ApprovalActor
		if request.approve {
			approval = ApprovalApproved
			approvalActor = request.actor
		}
		if request.revoke {
			approval = ApprovalRevoked
		}
		last := events[len(events)-1]
		if current.Revision == math.MaxUint64 {
			return fmt.Errorf("%w: lifecycle revision exhausted", ErrInvalidTransition)
		}
		body := LifecycleEventBody{
			SchemaVersion: SchemaVersion, ModelID: request.modelID, SemanticVersion: request.semanticVersion,
			Revision: current.Revision + 1, FromLifecycle: current.Lifecycle, ToLifecycle: request.to,
			ApprovalState: approval, DeploymentState: deploymentFor(request.to), Environment: request.environment,
			Actor: request.actor, Reason: request.reason, AuthorizationReference: request.authorizationReference,
			ApprovalActor: approvalActor, OccurredAtUTCNS: registry.nowUTCNS(), PreviousEventSHA256: last.EventSHA256,
		}
		event, err := registry.signEvent(body)
		if err != nil {
			return err
		}
		if err := registry.appendEvent(event); err != nil {
			return err
		}
		if request.updatePointer {
			if err := registry.writeDeploymentPointer(request.modelID, request.environment, request.semanticVersion, request.to, event.EventSHA256, event.EventSHA256, request.actor); err != nil {
				return err
			}
		}
		status, err = registry.statusFromEvents(append(events, event))
		return err
	})
	return status, err
}

func (registry *Registry) loadVersion(modelID string, semanticVersion string, expected *FeatureSchema) (SignedManifest, []SignedLifecycleEvent, ModelStatus, error) {
	manifest, err := registry.loadVerified(modelID, semanticVersion, expected)
	if err != nil {
		return SignedManifest{}, nil, ModelStatus{}, err
	}
	events, err := registry.loadEvents(modelID, semanticVersion)
	if err != nil {
		return SignedManifest{}, nil, ModelStatus{}, err
	}
	status, err := registry.statusFromEvents(events)
	if err != nil {
		return SignedManifest{}, nil, ModelStatus{}, err
	}
	return manifest, events, status, nil
}

func (registry *Registry) loadVerified(modelID string, semanticVersion string, expected *FeatureSchema) (SignedManifest, error) {
	manifest, err := registry.loadSignedManifest(modelID, semanticVersion)
	if err != nil {
		return SignedManifest{}, err
	}
	if err := VerifySignedManifest(manifest, registry.trustedKeys); err != nil {
		return SignedManifest{}, err
	}
	if expected != nil {
		if err := CompatibleFeatureSchema(manifest.Manifest.FeatureSchema, *expected); err != nil {
			return SignedManifest{}, err
		}
	}
	digest, err := hashFile(registry.artifactPath(manifest.Manifest.ArtifactSHA256))
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return SignedManifest{}, fmt.Errorf("%w: artifact missing", ErrRegistryIntegrity)
		}
		return SignedManifest{}, err
	}
	if digest != manifest.Manifest.ArtifactSHA256 {
		return SignedManifest{}, fmt.Errorf("%w: stored artifact digest %s", ErrArtifactHashMismatch, digest)
	}
	return manifest, nil
}

func (registry *Registry) loadSignedManifest(modelID string, semanticVersion string) (SignedManifest, error) {
	if !validIdentifier(modelID) || !semanticPattern.MatchString(semanticVersion) {
		return SignedManifest{}, ErrNotFound
	}
	payload, err := readBoundedFile(registry.manifestPath(modelID, semanticVersion), 4*1024*1024)
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return SignedManifest{}, ErrNotFound
		}
		return SignedManifest{}, fmt.Errorf("read manifest: %w", err)
	}
	var manifest SignedManifest
	if err := decodeStrict(payload, &manifest); err != nil {
		return SignedManifest{}, fmt.Errorf("%w: decode manifest: %v", ErrRegistryIntegrity, err)
	}
	if manifest.Manifest.ModelID != modelID || manifest.Manifest.SemanticVersion != semanticVersion {
		return SignedManifest{}, fmt.Errorf("%w: manifest path identity mismatch", ErrRegistryIntegrity)
	}
	return manifest, nil
}

func (registry *Registry) loadEvents(modelID string, semanticVersion string) ([]SignedLifecycleEvent, error) {
	if !validIdentifier(modelID) || !semanticPattern.MatchString(semanticVersion) {
		return nil, ErrNotFound
	}
	file, err := os.Open(registry.eventPath(modelID, semanticVersion))
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return nil, ErrNotFound
		}
		return nil, fmt.Errorf("open lifecycle log: %w", err)
	}
	defer file.Close()
	scanner := bufio.NewScanner(file)
	scanner.Buffer(make([]byte, 4096), 64*1024)
	events := make([]SignedLifecycleEvent, 0, 16)
	for scanner.Scan() {
		var event SignedLifecycleEvent
		if err := decodeStrict(scanner.Bytes(), &event); err != nil {
			return nil, fmt.Errorf("%w: decode lifecycle event: %v", ErrRegistryIntegrity, err)
		}
		if err := verifyLifecycleEvent(event, registry.trustedKeys); err != nil {
			return nil, err
		}
		if event.Body.ModelID != modelID || event.Body.SemanticVersion != semanticVersion {
			return nil, fmt.Errorf("%w: event path identity mismatch", ErrRegistryIntegrity)
		}
		events = append(events, event)
	}
	if err := scanner.Err(); err != nil {
		return nil, fmt.Errorf("%w: scan lifecycle log: %v", ErrRegistryIntegrity, err)
	}
	if len(events) == 0 {
		return nil, fmt.Errorf("%w: empty lifecycle log", ErrRegistryIntegrity)
	}
	if _, err := registry.statusFromEvents(events); err != nil {
		return nil, err
	}
	return events, nil
}

func (registry *Registry) statusFromEvents(events []SignedLifecycleEvent) (ModelStatus, error) {
	if len(events) == 0 {
		return ModelStatus{}, fmt.Errorf("%w: missing lifecycle event", ErrRegistryIntegrity)
	}
	var previous *SignedLifecycleEvent
	for index := range events {
		if err := validateEventTransition(previous, events[index]); err != nil {
			return ModelStatus{}, err
		}
		previous = &events[index]
	}
	last := events[len(events)-1]
	status := ModelStatus{
		ModelID: last.Body.ModelID, SemanticVersion: last.Body.SemanticVersion,
		Lifecycle: last.Body.ToLifecycle, Approval: last.Body.ApprovalState,
		Deployment: last.Body.DeploymentState, Environment: last.Body.Environment,
		Revision: last.Body.Revision, ApprovalActor: last.Body.ApprovalActor,
		LastEventSHA256: last.EventSHA256,
	}
	environments, err := registry.activeEnvironments(status.ModelID, status.SemanticVersion, events)
	if err != nil {
		return ModelStatus{}, err
	}
	status.ActiveEnvironments = environments
	status.Active = len(environments) != 0
	return status, nil
}

func validateEventTransition(previous *SignedLifecycleEvent, current SignedLifecycleEvent) error {
	body := current.Body
	if body.SchemaVersion != SchemaVersion || !validIdentifier(body.ModelID) || !semanticPattern.MatchString(body.SemanticVersion) || body.OccurredAtUTCNS <= 0 {
		return fmt.Errorf("%w: invalid lifecycle event fields", ErrRegistryIntegrity)
	}
	if err := validateActorReason(body.Actor, body.Reason); err != nil {
		return fmt.Errorf("%w: %v", ErrRegistryIntegrity, err)
	}
	if len(body.AuthorizationReference) > 256 || len(body.ApprovalActor) > 128 || (body.RollbackTargetVersion != "" && !semanticPattern.MatchString(body.RollbackTargetVersion)) {
		return fmt.Errorf("%w: invalid authorization, approver, or rollback reference", ErrRegistryIntegrity)
	}
	if body.DeploymentState != deploymentFor(body.ToLifecycle) {
		return fmt.Errorf("%w: lifecycle/deployment state mismatch", ErrRegistryIntegrity)
	}
	if previous == nil {
		if body.Revision != 1 || body.FromLifecycle != "" || body.ToLifecycle != LifecycleDraft || body.ApprovalState != ApprovalPending || body.PreviousEventSHA256 != "" {
			return fmt.Errorf("%w: invalid initial lifecycle event", ErrRegistryIntegrity)
		}
		return nil
	}
	prior := previous.Body
	if body.Revision != prior.Revision+1 || body.PreviousEventSHA256 != previous.EventSHA256 || body.FromLifecycle != prior.ToLifecycle || body.ModelID != prior.ModelID || body.SemanticVersion != prior.SemanticVersion {
		return fmt.Errorf("%w: lifecycle hash chain or revision mismatch", ErrRegistryIntegrity)
	}
	allowed := false
	switch {
	case prior.ToLifecycle == LifecycleDraft && body.ToLifecycle == LifecycleTrained:
		allowed = true
	case prior.ToLifecycle == LifecycleTrained && body.ToLifecycle == LifecycleOfflineValidated:
		allowed = true
	case prior.ToLifecycle == LifecycleOfflineValidated && body.ToLifecycle == LifecycleReplayValidated:
		allowed = true
	case prior.ToLifecycle == LifecycleReplayValidated && body.ToLifecycle == LifecycleReplayValidated && prior.ApprovalState == ApprovalPending && body.ApprovalState == ApprovalApproved:
		allowed = body.ApprovalActor == body.Actor && body.AuthorizationReference != ""
	case prior.ToLifecycle == LifecycleReplayValidated && body.ToLifecycle == LifecycleShadow:
		allowed = true
	case prior.ToLifecycle == LifecycleShadow && body.ToLifecycle == LifecycleCanary:
		allowed = true
	case prior.ToLifecycle == LifecycleCanary && body.ToLifecycle == LifecycleLimitedRisk:
		allowed = true
	case prior.ToLifecycle == LifecycleLimitedRisk && body.ToLifecycle == LifecycleProduction:
		allowed = body.AuthorizationReference != "" && body.Actor != body.ApprovalActor
	case body.ToLifecycle == LifecycleDisabled && prior.ToLifecycle != LifecycleRetired:
		allowed = body.ApprovalState == ApprovalRevoked
	case body.ToLifecycle == LifecycleRolledBack && (prior.ToLifecycle == LifecycleShadow || prior.ToLifecycle == LifecycleCanary || prior.ToLifecycle == LifecycleLimitedRisk || prior.ToLifecycle == LifecycleProduction):
		allowed = body.ApprovalState == ApprovalRevoked && body.RollbackTargetVersion != "" && body.AuthorizationReference != ""
	case body.ToLifecycle == LifecycleRetired && (prior.ToLifecycle == LifecycleDisabled || prior.ToLifecycle == LifecycleRolledBack):
		allowed = body.ApprovalState == ApprovalRevoked
	}
	if !allowed {
		return fmt.Errorf("%w: forbidden %s to %s event", ErrRegistryIntegrity, prior.ToLifecycle, body.ToLifecycle)
	}
	if body.ToLifecycle != LifecycleDisabled && body.ToLifecycle != LifecycleRolledBack && body.ToLifecycle != LifecycleRetired && body.ApprovalState != prior.ApprovalState && body.ToLifecycle != LifecycleReplayValidated {
		return fmt.Errorf("%w: approval state changed outside approval/revocation", ErrRegistryIntegrity)
	}
	if body.ToLifecycle == LifecycleShadow || body.ToLifecycle == LifecycleCanary || body.ToLifecycle == LifecycleLimitedRisk || body.ToLifecycle == LifecycleProduction {
		if body.ApprovalState != ApprovalApproved || !validIdentifier(body.Environment) {
			return fmt.Errorf("%w: deployment is not approved or has invalid environment", ErrRegistryIntegrity)
		}
	}
	return nil
}

func (registry *Registry) signEvent(body LifecycleEventBody) (SignedLifecycleEvent, error) {
	if len(registry.signer) != ed25519.PrivateKeySize {
		return SignedLifecycleEvent{}, errors.New("registry is read-only: lifecycle signer unavailable")
	}
	if body.OccurredAtUTCNS <= 0 || body.DeploymentState != deploymentFor(body.ToLifecycle) {
		return SignedLifecycleEvent{}, errors.New("invalid lifecycle event time or state")
	}
	if err := validateActorReason(body.Actor, body.Reason); err != nil {
		return SignedLifecycleEvent{}, err
	}
	return signLifecycleEvent(body, registry.signerKeyID, registry.signer)
}

func (registry *Registry) storeArtifact(source string, digest string) error {
	destination := registry.artifactPath(digest)
	if existingDigest, err := hashFile(destination); err == nil {
		if existingDigest != digest {
			return fmt.Errorf("%w: content-addressed artifact collision", ErrRegistryIntegrity)
		}
		return nil
	} else if !errors.Is(err, os.ErrNotExist) {
		return err
	}
	input, err := os.Open(source)
	if err != nil {
		return fmt.Errorf("open artifact: %w", err)
	}
	defer input.Close()
	output, err := os.OpenFile(destination, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o400)
	if err != nil {
		return fmt.Errorf("create immutable artifact: %w", err)
	}
	complete := false
	defer func() {
		output.Close()
		if !complete {
			_ = os.Remove(destination)
		}
	}()
	hasher := sha256.New()
	if _, err := io.Copy(io.MultiWriter(output, hasher), input); err != nil {
		return fmt.Errorf("copy artifact: %w", err)
	}
	if err := output.Sync(); err != nil {
		return fmt.Errorf("sync artifact: %w", err)
	}
	if hex.EncodeToString(hasher.Sum(nil)) != digest {
		return ErrArtifactHashMismatch
	}
	if err := output.Close(); err != nil {
		return fmt.Errorf("close artifact: %w", err)
	}
	complete = true
	return syncDirectory(filepath.Dir(destination))
}

func (registry *Registry) storeManifest(manifest SignedManifest) error {
	payload, err := json.MarshalIndent(manifest, "", "  ")
	if err != nil {
		return fmt.Errorf("encode manifest: %w", err)
	}
	payload = append(payload, '\n')
	path := registry.manifestPath(manifest.Manifest.ModelID, manifest.Manifest.SemanticVersion)
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		return fmt.Errorf("create manifest directory: %w", err)
	}
	file, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o400)
	if err != nil {
		return fmt.Errorf("create immutable manifest: %w", err)
	}
	complete := false
	defer func() {
		file.Close()
		if !complete {
			_ = os.Remove(path)
		}
	}()
	if _, err := file.Write(payload); err != nil {
		return fmt.Errorf("write manifest: %w", err)
	}
	if err := file.Sync(); err != nil {
		return fmt.Errorf("sync manifest: %w", err)
	}
	if err := file.Close(); err != nil {
		return fmt.Errorf("close manifest: %w", err)
	}
	complete = true
	return syncDirectory(filepath.Dir(path))
}

func (registry *Registry) appendEvent(event SignedLifecycleEvent) error {
	path := registry.eventPath(event.Body.ModelID, event.Body.SemanticVersion)
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		return fmt.Errorf("create event directory: %w", err)
	}
	payload, err := json.Marshal(event)
	if err != nil {
		return fmt.Errorf("encode lifecycle event: %w", err)
	}
	payload = append(payload, '\n')
	file, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_APPEND, 0o600)
	if err != nil {
		return fmt.Errorf("open lifecycle log: %w", err)
	}
	defer file.Close()
	if _, err := file.Write(payload); err != nil {
		return fmt.Errorf("append lifecycle event: %w", err)
	}
	if err := file.Sync(); err != nil {
		return fmt.Errorf("sync lifecycle event: %w", err)
	}
	return syncDirectory(filepath.Dir(path))
}

func (registry *Registry) writeDeploymentPointer(modelID string, environment string, version string, lifecycle LifecycleState, sourceEvent string, changeEvent string, actor string) error {
	path := registry.deploymentPath(environment, modelID)
	generation := uint64(1)
	if existing, err := registry.loadDeploymentPointer(modelID, environment); err == nil {
		if existing.Body.Generation == math.MaxUint64 {
			return fmt.Errorf("%w: deployment generation exhausted", ErrRegistryIntegrity)
		}
		generation = existing.Body.Generation + 1
	} else if !errors.Is(err, ErrNotFound) {
		return err
	}
	changedAt := registry.nowUTCNS()
	if changedAt <= 0 {
		return errors.New("invalid deployment pointer time")
	}
	body := DeploymentPointerBody{
		SchemaVersion: SchemaVersion, ModelID: modelID, Environment: environment,
		ActiveVersion: version, Lifecycle: lifecycle, Generation: generation,
		SourceEventSHA256: sourceEvent, ChangeEventSHA256: changeEvent,
		ChangedAtUTCNS: changedAt, Actor: actor,
	}
	pointer, err := signDeploymentPointer(body, registry.signerKeyID, registry.signer)
	if err != nil {
		return err
	}
	payload, err := json.MarshalIndent(pointer, "", "  ")
	if err != nil {
		return fmt.Errorf("encode deployment pointer: %w", err)
	}
	payload = append(payload, '\n')
	return atomicWrite(path, payload)
}

func (registry *Registry) loadDeploymentPointer(modelID string, environment string) (SignedDeploymentPointer, error) {
	if !validIdentifier(modelID) || !validIdentifier(environment) {
		return SignedDeploymentPointer{}, ErrNotFound
	}
	payload, err := readBoundedFile(registry.deploymentPath(environment, modelID), 64*1024)
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return SignedDeploymentPointer{}, ErrNotFound
		}
		return SignedDeploymentPointer{}, fmt.Errorf("read deployment pointer: %w", err)
	}
	var pointer SignedDeploymentPointer
	if err := decodeStrict(payload, &pointer); err != nil {
		return SignedDeploymentPointer{}, fmt.Errorf("%w: decode deployment pointer: %v", ErrRegistryIntegrity, err)
	}
	if err := verifyDeploymentPointer(pointer, registry.trustedKeys); err != nil {
		return SignedDeploymentPointer{}, err
	}
	validLifecycle := pointer.Body.Lifecycle == LifecycleShadow || pointer.Body.Lifecycle == LifecycleCanary || pointer.Body.Lifecycle == LifecycleLimitedRisk || pointer.Body.Lifecycle == LifecycleProduction || pointer.Body.Lifecycle == LifecycleDisabled || pointer.Body.Lifecycle == LifecycleRolledBack
	if pointer.Body.ModelID != modelID || pointer.Body.Environment != environment || pointer.Body.SchemaVersion != SchemaVersion || pointer.Body.Generation == 0 || !semanticPattern.MatchString(pointer.Body.ActiveVersion) || !hex64Pattern.MatchString(pointer.Body.SourceEventSHA256) || !hex64Pattern.MatchString(pointer.Body.ChangeEventSHA256) || !validLifecycle || strings.TrimSpace(pointer.Body.Actor) == "" || len(pointer.Body.Actor) > 128 || pointer.Body.ChangedAtUTCNS <= 0 {
		return SignedDeploymentPointer{}, fmt.Errorf("%w: invalid deployment pointer fields", ErrRegistryIntegrity)
	}
	return pointer, nil
}

func (registry *Registry) activeEnvironments(modelID string, semanticVersion string, events []SignedLifecycleEvent) ([]string, error) {
	eventsByHash := make(map[string]SignedLifecycleEvent, len(events))
	for _, event := range events {
		eventsByHash[event.EventSHA256] = event
	}
	entries, err := os.ReadDir(filepath.Join(registry.root, "deployments"))
	if err != nil {
		return nil, fmt.Errorf("read deployments: %w", err)
	}
	environments := make([]string, 0)
	for _, entry := range entries {
		if !entry.IsDir() || !validIdentifier(entry.Name()) {
			continue
		}
		pointer, err := registry.loadDeploymentPointer(modelID, entry.Name())
		if errors.Is(err, ErrNotFound) {
			continue
		}
		if err != nil {
			return nil, err
		}
		if pointer.Body.ActiveVersion != semanticVersion {
			continue
		}
		sourceEvent, ok := eventsByHash[pointer.Body.SourceEventSHA256]
		if !ok || sourceEvent.Body.ToLifecycle != pointer.Body.Lifecycle {
			return nil, fmt.Errorf("%w: deployment pointer is detached from lifecycle event", ErrRegistryIntegrity)
		}
		if pointer.Body.Lifecycle == LifecycleShadow || pointer.Body.Lifecycle == LifecycleCanary || pointer.Body.Lifecycle == LifecycleLimitedRisk || pointer.Body.Lifecycle == LifecycleProduction {
			environments = append(environments, entry.Name())
		}
	}
	sort.Strings(environments)
	return environments, nil
}

func (registry *Registry) disableDeploymentPointers(modelID string, semanticVersion string, event SignedLifecycleEvent, actor string) error {
	entries, err := os.ReadDir(filepath.Join(registry.root, "deployments"))
	if err != nil {
		return fmt.Errorf("read deployments: %w", err)
	}
	for _, entry := range entries {
		if !entry.IsDir() || !validIdentifier(entry.Name()) {
			continue
		}
		pointer, err := registry.loadDeploymentPointer(modelID, entry.Name())
		if errors.Is(err, ErrNotFound) {
			continue
		}
		if err != nil {
			return err
		}
		if pointer.Body.ActiveVersion != semanticVersion {
			continue
		}
		if pointer.Body.Lifecycle == event.Body.ToLifecycle && pointer.Body.SourceEventSHA256 == event.EventSHA256 {
			continue
		}
		if err := registry.writeDeploymentPointer(modelID, entry.Name(), semanticVersion, event.Body.ToLifecycle, event.EventSHA256, event.EventSHA256, actor); err != nil {
			return err
		}
	}
	return nil
}

func (registry *Registry) withExclusiveLock(ctx context.Context, action func() error) error {
	registry.mu.Lock()
	defer registry.mu.Unlock()
	if err := contextError(ctx); err != nil {
		return err
	}
	lockFile, err := os.OpenFile(filepath.Join(registry.root, ".registry.lock"), os.O_CREATE|os.O_RDWR, 0o600)
	if err != nil {
		return fmt.Errorf("open registry lock: %w", err)
	}
	defer lockFile.Close()
	lockContext, cancel := context.WithTimeout(ctx, 5*time.Second)
	defer cancel()
	for {
		err = syscall.Flock(int(lockFile.Fd()), syscall.LOCK_EX|syscall.LOCK_NB)
		if err == nil {
			break
		}
		if !errors.Is(err, syscall.EWOULDBLOCK) && !errors.Is(err, syscall.EAGAIN) {
			return fmt.Errorf("lock registry: %w", err)
		}
		timer := time.NewTimer(10 * time.Millisecond)
		select {
		case <-lockContext.Done():
			timer.Stop()
			return fmt.Errorf("lock registry: %w", lockContext.Err())
		case <-timer.C:
		}
	}
	defer syscall.Flock(int(lockFile.Fd()), syscall.LOCK_UN) //nolint:errcheck
	if err := contextError(ctx); err != nil {
		return err
	}
	return action()
}

func (registry *Registry) artifactPath(digest string) string {
	return filepath.Join(registry.root, "artifacts", digest+".bin")
}

func (registry *Registry) manifestPath(modelID string, semanticVersion string) string {
	return filepath.Join(registry.root, "manifests", modelID, semanticVersion+".json")
}

func (registry *Registry) eventPath(modelID string, semanticVersion string) string {
	return filepath.Join(registry.root, "events", modelID, semanticVersion+".jsonl")
}

func (registry *Registry) deploymentPath(environment string, modelID string) string {
	return filepath.Join(registry.root, "deployments", environment, modelID+".json")
}

func (registry *Registry) nowUTCNS() int64 {
	return registry.clock().UTC().UnixNano()
}

func hashFile(path string) (string, error) {
	file, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer file.Close()
	hasher := sha256.New()
	if _, err := io.Copy(hasher, file); err != nil {
		return "", fmt.Errorf("hash file: %w", err)
	}
	return hex.EncodeToString(hasher.Sum(nil)), nil
}

func decodeStrict(payload []byte, destination any) error {
	decoder := json.NewDecoder(bytes.NewReader(payload))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(destination); err != nil {
		return err
	}
	if decoder.Decode(&struct{}{}) != io.EOF {
		return errors.New("trailing JSON value")
	}
	return nil
}

func atomicWrite(path string, payload []byte) error {
	directory := filepath.Dir(path)
	if err := os.MkdirAll(directory, 0o700); err != nil {
		return fmt.Errorf("create pointer directory: %w", err)
	}
	temporary, err := os.CreateTemp(directory, ".pointer-*")
	if err != nil {
		return fmt.Errorf("create temporary pointer: %w", err)
	}
	temporaryPath := temporary.Name()
	complete := false
	defer func() {
		temporary.Close()
		if !complete {
			_ = os.Remove(temporaryPath)
		}
	}()
	if err := temporary.Chmod(0o600); err != nil {
		return fmt.Errorf("set pointer permissions: %w", err)
	}
	if _, err := temporary.Write(payload); err != nil {
		return fmt.Errorf("write deployment pointer: %w", err)
	}
	if err := temporary.Sync(); err != nil {
		return fmt.Errorf("sync deployment pointer: %w", err)
	}
	if err := temporary.Close(); err != nil {
		return fmt.Errorf("close deployment pointer: %w", err)
	}
	if err := os.Rename(temporaryPath, path); err != nil {
		return fmt.Errorf("replace deployment pointer: %w", err)
	}
	complete = true
	return syncDirectory(directory)
}

func syncDirectory(directory string) error {
	file, err := os.Open(directory)
	if err != nil {
		return fmt.Errorf("open directory for sync: %w", err)
	}
	defer file.Close()
	if err := file.Sync(); err != nil {
		return fmt.Errorf("sync directory: %w", err)
	}
	return nil
}

func contextError(ctx context.Context) error {
	if ctx == nil {
		return errors.New("context is required")
	}
	return ctx.Err()
}
