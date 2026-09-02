// Package modelregistry implements the off-hot-path Aegis-MX model artifact
// registry. It has no order-entry or live-trading capability.
package modelregistry

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"regexp"
	"strings"
)

const (
	// Version is the model-registry implementation version.
	Version = "0.1.0"
	// SchemaVersion is the persisted manifest, event, and pointer schema.
	SchemaVersion uint32 = 1
)

var (
	ErrAlreadyRegistered       = errors.New("model version is already registered")
	ErrArtifactHashMismatch    = errors.New("artifact hash does not match manifest")
	ErrIncompatibleSchema      = errors.New("feature schema is incompatible")
	ErrInvalidManifest         = errors.New("invalid model manifest")
	ErrInvalidTransition       = errors.New("invalid model lifecycle transition")
	ErrNotFound                = errors.New("model version was not found")
	ErrProductionAuthorization = errors.New("explicit production authorization is required")
	ErrRegistryIntegrity       = errors.New("registry integrity verification failed")
	ErrSignature               = errors.New("signature verification failed")
	ErrTwoPersonControl        = errors.New("production promoter must differ from artifact approver")
)

var (
	identifierPattern = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$`)
	semanticPattern   = regexp.MustCompile(`^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$`)
	hex40Pattern      = regexp.MustCompile(`^[0-9a-f]{40}$`)
	hex64Pattern      = regexp.MustCompile(`^[0-9a-f]{64}$`)
)

// LifecycleState is the authoritative, audited state of one model version.
type LifecycleState string

const (
	LifecycleDraft            LifecycleState = "DRAFT"
	LifecycleTrained          LifecycleState = "TRAINED"
	LifecycleOfflineValidated LifecycleState = "OFFLINE_VALIDATED"
	LifecycleReplayValidated  LifecycleState = "REPLAY_VALIDATED"
	LifecycleShadow           LifecycleState = "SHADOW"
	LifecycleCanary           LifecycleState = "CANARY"
	LifecycleLimitedRisk      LifecycleState = "LIMITED_RISK"
	LifecycleProduction       LifecycleState = "PRODUCTION"
	LifecycleDisabled         LifecycleState = "DISABLED"
	LifecycleRolledBack       LifecycleState = "ROLLED_BACK"
	LifecycleRetired          LifecycleState = "RETIRED"
)

// ApprovalState is independent from validation and deployment state.
type ApprovalState string

const (
	ApprovalPending  ApprovalState = "PENDING"
	ApprovalApproved ApprovalState = "APPROVED"
	ApprovalRevoked  ApprovalState = "REVOKED"
)

// DeploymentState identifies where the version is eligible to run.
type DeploymentState string

const (
	DeploymentNotDeployed DeploymentState = "NOT_DEPLOYED"
	DeploymentShadow      DeploymentState = "SHADOW"
	DeploymentCanary      DeploymentState = "CANARY"
	DeploymentLimitedRisk DeploymentState = "LIMITED_RISK"
	DeploymentProduction  DeploymentState = "PRODUCTION"
	DeploymentDisabled    DeploymentState = "DISABLED"
	DeploymentRolledBack  DeploymentState = "ROLLED_BACK"
	DeploymentRetired     DeploymentState = "RETIRED"
)

// ValidationStage selects one explicit validation transition.
type ValidationStage string

const (
	ValidationTrained ValidationStage = "trained"
	ValidationOffline ValidationStage = "offline"
	ValidationReplay  ValidationStage = "replay"
)

// DigestReference binds external immutable metadata without copying licensed
// or proprietary contents into the registry.
type DigestReference struct {
	ID     string `json:"id"`
	SHA256 string `json:"sha256"`
}

// TrainingDataManifest is the point-in-time training-data identity.
type TrainingDataManifest struct {
	DatasetID         string `json:"dataset_id"`
	ManifestSHA256    string `json:"manifest_sha256"`
	AsKnownAtUTCNS    int64  `json:"as_known_at_utc_ns"`
	SplitPolicy       string `json:"split_policy"`
	DeterministicSeed int64  `json:"deterministic_seed"`
}

// FeatureField defines one ordered, fixed-unit model input.
type FeatureField struct {
	Name     string `json:"name"`
	Type     string `json:"type"`
	Unit     string `json:"unit"`
	Scale    int64  `json:"scale"`
	Required bool   `json:"required"`
}

// FeatureSchema binds exact feature order and representation.
type FeatureSchema struct {
	SchemaID string         `json:"schema_id"`
	Version  string         `json:"version"`
	Fields   []FeatureField `json:"fields"`
	SHA256   string         `json:"sha256"`
}

// Metric stores integer/fixed-point evaluation evidence.
type Metric struct {
	Name       string `json:"name"`
	ValuePPM   int64  `json:"value_ppm"`
	DatasetID  string `json:"dataset_id"`
	MetricUnit string `json:"metric_unit"`
}

// ArtifactManifest is immutable once registered and approved. It intentionally
// contains no credentials, provider payloads, or deployment authorization.
type ArtifactManifest struct {
	SchemaVersion         uint32               `json:"schema_version"`
	ModelID               string               `json:"model_id"`
	SemanticVersion       string               `json:"semantic_version"`
	ParentSemanticVersion string               `json:"parent_semantic_version,omitempty"`
	TrainingData          TrainingDataManifest `json:"training_data_manifest"`
	FeatureSchema         FeatureSchema        `json:"feature_schema"`
	CodeCommit            string               `json:"code_commit"`
	DependencyLock        DigestReference      `json:"dependency_lock"`
	TrainingConfiguration DigestReference      `json:"training_configuration"`
	Metrics               []Metric             `json:"metrics"`
	CalibrationData       DigestReference      `json:"calibration_data"`
	OODProfile            DigestReference      `json:"ood_profile"`
	ArtifactSHA256        string               `json:"artifact_sha256"`
	CreatedAtUTCNS        int64                `json:"created_at_utc_ns"`
}

// SignedManifest is a canonical manifest plus its detached Ed25519 signature.
type SignedManifest struct {
	Manifest        ArtifactManifest `json:"manifest"`
	SignerKeyID     string           `json:"signer_key_id"`
	SignatureBase64 string           `json:"signature_base64"`
}

// LifecycleEventBody is the signed, append-only approval and deployment audit
// record. PreviousEventSHA256 creates a per-version hash chain.
type LifecycleEventBody struct {
	SchemaVersion          uint32          `json:"schema_version"`
	ModelID                string          `json:"model_id"`
	SemanticVersion        string          `json:"semantic_version"`
	Revision               uint64          `json:"revision"`
	FromLifecycle          LifecycleState  `json:"from_lifecycle,omitempty"`
	ToLifecycle            LifecycleState  `json:"to_lifecycle"`
	ApprovalState          ApprovalState   `json:"approval_state"`
	DeploymentState        DeploymentState `json:"deployment_state"`
	Environment            string          `json:"environment,omitempty"`
	Actor                  string          `json:"actor"`
	Reason                 string          `json:"reason"`
	AuthorizationReference string          `json:"authorization_reference,omitempty"`
	ApprovalActor          string          `json:"approval_actor,omitempty"`
	RollbackTargetVersion  string          `json:"rollback_target_version,omitempty"`
	OccurredAtUTCNS        int64           `json:"occurred_at_utc_ns"`
	PreviousEventSHA256    string          `json:"previous_event_sha256,omitempty"`
}

// SignedLifecycleEvent authenticates one append-only event.
type SignedLifecycleEvent struct {
	Body            LifecycleEventBody `json:"body"`
	EventSHA256     string             `json:"event_sha256"`
	SignerKeyID     string             `json:"signer_key_id"`
	SignatureBase64 string             `json:"signature_base64"`
}

// DeploymentPointerBody is a signed materialized view. The append-only event
// stream remains authoritative if this view must be rebuilt.
type DeploymentPointerBody struct {
	SchemaVersion     uint32         `json:"schema_version"`
	ModelID           string         `json:"model_id"`
	Environment       string         `json:"environment"`
	ActiveVersion     string         `json:"active_version"`
	Lifecycle         LifecycleState `json:"lifecycle"`
	Generation        uint64         `json:"generation"`
	SourceEventSHA256 string         `json:"source_event_sha256"`
	ChangeEventSHA256 string         `json:"change_event_sha256"`
	ChangedAtUTCNS    int64          `json:"changed_at_utc_ns"`
	Actor             string         `json:"actor"`
}

// SignedDeploymentPointer authenticates a mutable deployment materialization.
type SignedDeploymentPointer struct {
	Body            DeploymentPointerBody `json:"body"`
	PointerSHA256   string                `json:"pointer_sha256"`
	SignerKeyID     string                `json:"signer_key_id"`
	SignatureBase64 string                `json:"signature_base64"`
}

// ModelStatus is reconstructed from verified lifecycle events and deployment
// pointers.
type ModelStatus struct {
	ModelID            string          `json:"model_id"`
	SemanticVersion    string          `json:"semantic_version"`
	Lifecycle          LifecycleState  `json:"lifecycle"`
	Approval           ApprovalState   `json:"approval_state"`
	Deployment         DeploymentState `json:"deployment_state"`
	Environment        string          `json:"environment,omitempty"`
	Active             bool            `json:"active"`
	ActiveEnvironments []string        `json:"active_environments,omitempty"`
	Revision           uint64          `json:"revision"`
	ApprovalActor      string          `json:"approval_actor,omitempty"`
	LastEventSHA256    string          `json:"last_event_sha256"`
}

// HealthState is the non-networked API health/readiness view.
type HealthState struct {
	Healthy           bool      `json:"healthy"`
	Ready             bool      `json:"ready"`
	Reason            string    `json:"reason"`
	ConfigurationHash string    `json:"configuration_hash"`
	Build             BuildInfo `json:"build"`
}

// LineageEntry contains one verified manifest, status, and complete audit log.
type LineageEntry struct {
	Manifest SignedManifest         `json:"manifest"`
	Status   ModelStatus            `json:"status"`
	Events   []SignedLifecycleEvent `json:"events"`
}

// Lineage follows parent versions from the requested version to its root.
type Lineage struct {
	ModelID string         `json:"model_id"`
	Entries []LineageEntry `json:"entries"`
}

// BuildInfo is deterministic and explicitly advertises that this component
// cannot transmit orders.
type BuildInfo struct {
	Project            string `json:"project"`
	Version            string `json:"version"`
	SchemaVersion      uint32 `json:"schema_version"`
	LiveTradingCapable bool   `json:"live_trading_capable"`
}

// CurrentBuildInfo returns host-independent build identity.
func CurrentBuildInfo() BuildInfo {
	return BuildInfo{Project: "Aegis-MX model registry", Version: Version, SchemaVersion: SchemaVersion, LiveTradingCapable: false}
}

// FinalizeFeatureSchema calculates the canonical feature-schema digest.
func FinalizeFeatureSchema(schema FeatureSchema) (FeatureSchema, error) {
	schema.SHA256 = ""
	if err := validateFeatureSchemaFields(schema); err != nil {
		return FeatureSchema{}, err
	}
	digest, err := canonicalSHA256(schema)
	if err != nil {
		return FeatureSchema{}, err
	}
	schema.SHA256 = digest
	return schema, nil
}

// CompatibleFeatureSchema enforces exact identity, order, units, and digest.
func CompatibleFeatureSchema(artifact FeatureSchema, runtime FeatureSchema) error {
	if err := validateFeatureSchema(artifact); err != nil {
		return fmt.Errorf("%w: artifact: %v", ErrIncompatibleSchema, err)
	}
	if err := validateFeatureSchema(runtime); err != nil {
		return fmt.Errorf("%w: runtime: %v", ErrIncompatibleSchema, err)
	}
	if artifact.SchemaID != runtime.SchemaID || artifact.Version != runtime.Version || artifact.SHA256 != runtime.SHA256 {
		return fmt.Errorf("%w: expected %s/%s/%s, got %s/%s/%s", ErrIncompatibleSchema, artifact.SchemaID, artifact.Version, artifact.SHA256, runtime.SchemaID, runtime.Version, runtime.SHA256)
	}
	return nil
}

func validateManifest(manifest ArtifactManifest) error {
	if manifest.SchemaVersion != SchemaVersion {
		return fmt.Errorf("%w: unsupported schema version %d", ErrInvalidManifest, manifest.SchemaVersion)
	}
	if !validIdentifier(manifest.ModelID) || !semanticPattern.MatchString(manifest.SemanticVersion) {
		return fmt.Errorf("%w: invalid model identity", ErrInvalidManifest)
	}
	if manifest.ParentSemanticVersion != "" {
		if !semanticPattern.MatchString(manifest.ParentSemanticVersion) || manifest.ParentSemanticVersion == manifest.SemanticVersion {
			return fmt.Errorf("%w: invalid parent semantic version", ErrInvalidManifest)
		}
	}
	if !hex40Pattern.MatchString(manifest.CodeCommit) && !hex64Pattern.MatchString(manifest.CodeCommit) {
		return fmt.Errorf("%w: code commit must be a lowercase full digest", ErrInvalidManifest)
	}
	if !hex64Pattern.MatchString(manifest.ArtifactSHA256) {
		return fmt.Errorf("%w: invalid artifact digest", ErrInvalidManifest)
	}
	if manifest.CreatedAtUTCNS <= 0 || manifest.TrainingData.AsKnownAtUTCNS <= 0 || manifest.TrainingData.AsKnownAtUTCNS > manifest.CreatedAtUTCNS {
		return fmt.Errorf("%w: invalid point-in-time timestamps", ErrInvalidManifest)
	}
	if !validIdentifier(manifest.TrainingData.DatasetID) || !hex64Pattern.MatchString(manifest.TrainingData.ManifestSHA256) || strings.TrimSpace(manifest.TrainingData.SplitPolicy) == "" || len(manifest.TrainingData.SplitPolicy) > 128 {
		return fmt.Errorf("%w: invalid training-data manifest", ErrInvalidManifest)
	}
	if err := validateFeatureSchema(manifest.FeatureSchema); err != nil {
		return fmt.Errorf("%w: %v", ErrInvalidManifest, err)
	}
	for name, reference := range map[string]DigestReference{
		"dependency lock":        manifest.DependencyLock,
		"training configuration": manifest.TrainingConfiguration,
		"calibration data":       manifest.CalibrationData,
		"OOD profile":            manifest.OODProfile,
	} {
		if !validIdentifier(reference.ID) || !hex64Pattern.MatchString(reference.SHA256) {
			return fmt.Errorf("%w: invalid %s reference", ErrInvalidManifest, name)
		}
	}
	if len(manifest.Metrics) > 256 {
		return fmt.Errorf("%w: metric count exceeds 256", ErrInvalidManifest)
	}
	seenMetrics := make(map[string]struct{}, len(manifest.Metrics))
	for _, metric := range manifest.Metrics {
		if !validIdentifier(metric.Name) || !validIdentifier(metric.DatasetID) || strings.TrimSpace(metric.MetricUnit) == "" || len(metric.MetricUnit) > 64 {
			return fmt.Errorf("%w: invalid metric", ErrInvalidManifest)
		}
		if _, exists := seenMetrics[metric.Name]; exists {
			return fmt.Errorf("%w: duplicate metric %q", ErrInvalidManifest, metric.Name)
		}
		seenMetrics[metric.Name] = struct{}{}
	}
	return nil
}

func validateFeatureSchema(schema FeatureSchema) error {
	declared := schema.SHA256
	if !hex64Pattern.MatchString(declared) {
		return errors.New("feature schema has invalid digest")
	}
	finalized, err := FinalizeFeatureSchema(schema)
	if err != nil {
		return err
	}
	if finalized.SHA256 != declared {
		return errors.New("feature schema digest mismatch")
	}
	return nil
}

func validateFeatureSchemaFields(schema FeatureSchema) error {
	if !validIdentifier(schema.SchemaID) || !semanticPattern.MatchString(schema.Version) {
		return errors.New("feature schema has invalid identity")
	}
	if len(schema.Fields) == 0 || len(schema.Fields) > 256 {
		return errors.New("feature schema field count is outside 1..256")
	}
	seen := make(map[string]struct{}, len(schema.Fields))
	for _, field := range schema.Fields {
		if !validIdentifier(field.Name) || strings.TrimSpace(field.Unit) == "" || len(field.Unit) > 64 || field.Scale <= 0 {
			return fmt.Errorf("invalid feature field %q", field.Name)
		}
		switch field.Type {
		case "bool", "int32", "int64", "uint32", "uint64":
		default:
			return fmt.Errorf("unsupported feature type %q", field.Type)
		}
		if _, exists := seen[field.Name]; exists {
			return fmt.Errorf("duplicate feature field %q", field.Name)
		}
		seen[field.Name] = struct{}{}
	}
	return nil
}

func canonicalSHA256(value any) (string, error) {
	payload, err := json.Marshal(value)
	if err != nil {
		return "", fmt.Errorf("canonical JSON: %w", err)
	}
	digest := sha256.Sum256(payload)
	return hex.EncodeToString(digest[:]), nil
}

func validIdentifier(value string) bool {
	return identifierPattern.MatchString(value)
}

func validateActorReason(actor string, reason string) error {
	if strings.TrimSpace(actor) == "" || len(actor) > 128 || strings.TrimSpace(reason) == "" || len(reason) > 1024 {
		return errors.New("actor and bounded reason are required")
	}
	return nil
}

func deploymentFor(lifecycle LifecycleState) DeploymentState {
	switch lifecycle {
	case LifecycleShadow:
		return DeploymentShadow
	case LifecycleCanary:
		return DeploymentCanary
	case LifecycleLimitedRisk:
		return DeploymentLimitedRisk
	case LifecycleProduction:
		return DeploymentProduction
	case LifecycleDisabled:
		return DeploymentDisabled
	case LifecycleRolledBack:
		return DeploymentRolledBack
	case LifecycleRetired:
		return DeploymentRetired
	default:
		return DeploymentNotDeployed
	}
}
