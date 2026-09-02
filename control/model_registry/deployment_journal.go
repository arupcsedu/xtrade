package modelregistry

import (
	"bufio"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"sync"
)

// DeploymentAuditKind identifies one control-plane deployment event.
type DeploymentAuditKind string

const (
	AuditObservation             DeploymentAuditKind = "OBSERVATION"
	AuditRollbackTriggered       DeploymentAuditKind = "ROLLBACK_TRIGGERED"
	AuditRollbackCompleted       DeploymentAuditKind = "ROLLBACK_COMPLETED"
	AuditFailClosedDisable       DeploymentAuditKind = "FAIL_CLOSED_DISABLE"
	AuditFailClosedDisableFailed DeploymentAuditKind = "FAIL_CLOSED_DISABLE_FAILED"
	AuditCanaryApprovalRequested DeploymentAuditKind = "CANARY_APPROVAL_REQUESTED"
	AuditCanaryActivated         DeploymentAuditKind = "CANARY_ACTIVATED"
	AuditCanaryScopeDecision     DeploymentAuditKind = "CANARY_SCOPE_DECISION"
)

// DeploymentAuditRecord contains only bounded structured control data.
type DeploymentAuditRecord struct {
	SchemaVersion        uint32                `json:"schema_version"`
	Kind                 DeploymentAuditKind   `json:"kind"`
	DeploymentID         string                `json:"deployment_id"`
	ModelID              string                `json:"model_id"`
	CandidateVersion     string                `json:"candidate_version"`
	ProductionVersion    string                `json:"production_version"`
	ConfigurationSHA256  string                `json:"configuration_sha256"`
	State                DeploymentRunState    `json:"state"`
	OccurredAtUTCNS      int64                 `json:"occurred_at_utc_ns"`
	ObservationSequence  uint64                `json:"observation_sequence,omitempty"`
	Regime               MarketRegime          `json:"regime,omitempty"`
	PairedObservation    *PairedObservation    `json:"paired_observation,omitempty"`
	HypotheticalDecision *HypotheticalDecision `json:"hypothetical_decision,omitempty"`
	Trigger              *RollbackTrigger      `json:"trigger,omitempty"`
	Approval             *CanaryApproval       `json:"approval,omitempty"`
	ScopeRequest         *CanaryScopeRequest   `json:"scope_request,omitempty"`
	ScopeDecision        *CanaryScopeDecision  `json:"scope_decision,omitempty"`
	ReasonCode           string                `json:"reason_code"`
}

// DeploymentAuditEnvelope adds sequence, checksum, and a previous-record hash.
type DeploymentAuditEnvelope struct {
	Sequence       uint64                `json:"sequence"`
	PreviousSHA256 string                `json:"previous_sha256,omitempty"`
	Record         DeploymentAuditRecord `json:"record"`
	RecordSHA256   string                `json:"record_sha256"`
}

// DeploymentAuditSink is outside model inference and may perform durable I/O.
type DeploymentAuditSink interface {
	Append(record DeploymentAuditRecord) error
}

// DeploymentAuditSource allows deterministic restart from verified audit
// evidence. A canary coordinator refuses recovery from an append-only sink that
// cannot supply its prior records.
type DeploymentAuditSource interface {
	ReadAll() ([]DeploymentAuditRecord, error)
}

// FileDeploymentAuditSink is a checksummed append-only JSONL control audit.
// The canonical colocated binary journal remains the authoritative trading
// decision journal; this sink records off-path deployment evaluation.
type FileDeploymentAuditSink struct {
	mu       sync.Mutex
	path     string
	sequence uint64
	lastHash string
}

// OpenFileDeploymentAuditSink verifies any existing chain before appending.
func OpenFileDeploymentAuditSink(path string) (*FileDeploymentAuditSink, error) {
	if path == "" {
		return nil, errors.New("deployment audit path is required")
	}
	absolute, err := filepath.Abs(path)
	if err != nil {
		return nil, fmt.Errorf("resolve deployment audit path: %w", err)
	}
	if err := os.MkdirAll(filepath.Dir(absolute), 0o700); err != nil {
		return nil, fmt.Errorf("create deployment audit directory: %w", err)
	}
	envelopes, err := InspectDeploymentAudit(absolute)
	if err != nil && !errors.Is(err, os.ErrNotExist) {
		return nil, err
	}
	sink := &FileDeploymentAuditSink{path: absolute}
	if len(envelopes) != 0 {
		last := envelopes[len(envelopes)-1]
		sink.sequence = last.Sequence
		sink.lastHash = last.RecordSHA256
	}
	return sink, nil
}

// Append durably adds one checksummed record and syncs its parent directory.
func (sink *FileDeploymentAuditSink) Append(record DeploymentAuditRecord) error {
	sink.mu.Lock()
	defer sink.mu.Unlock()
	if err := validateDeploymentAuditRecord(record); err != nil {
		return err
	}
	if sink.sequence == ^uint64(0) {
		return errors.New("deployment audit sequence exhausted")
	}
	body := struct {
		Sequence       uint64                `json:"sequence"`
		PreviousSHA256 string                `json:"previous_sha256,omitempty"`
		Record         DeploymentAuditRecord `json:"record"`
	}{Sequence: sink.sequence + 1, PreviousSHA256: sink.lastHash, Record: record}
	digest, err := canonicalSHA256(body)
	if err != nil {
		return err
	}
	envelope := DeploymentAuditEnvelope{
		Sequence: body.Sequence, PreviousSHA256: body.PreviousSHA256,
		Record: body.Record, RecordSHA256: digest,
	}
	payload, err := json.Marshal(envelope)
	if err != nil {
		return fmt.Errorf("encode deployment audit: %w", err)
	}
	payload = append(payload, '\n')
	file, err := os.OpenFile(sink.path, os.O_WRONLY|os.O_CREATE|os.O_APPEND, 0o600)
	if err != nil {
		return fmt.Errorf("open deployment audit: %w", err)
	}
	if _, err := file.Write(payload); err != nil {
		_ = file.Close()
		return fmt.Errorf("append deployment audit: %w", err)
	}
	if err := file.Sync(); err != nil {
		_ = file.Close()
		return fmt.Errorf("sync deployment audit: %w", err)
	}
	if err := file.Close(); err != nil {
		return fmt.Errorf("close deployment audit: %w", err)
	}
	if err := syncDirectory(filepath.Dir(sink.path)); err != nil {
		return err
	}
	sink.sequence = envelope.Sequence
	sink.lastHash = envelope.RecordSHA256
	return nil
}

// ReadAll returns a verified copy of every deployment record.
func (sink *FileDeploymentAuditSink) ReadAll() ([]DeploymentAuditRecord, error) {
	sink.mu.Lock()
	defer sink.mu.Unlock()
	envelopes, err := InspectDeploymentAudit(sink.path)
	if errors.Is(err, os.ErrNotExist) {
		return []DeploymentAuditRecord{}, nil
	}
	if err != nil {
		return nil, err
	}
	records := make([]DeploymentAuditRecord, 0, len(envelopes))
	for _, envelope := range envelopes {
		records = append(records, envelope.Record)
	}
	return records, nil
}

// InspectDeploymentAudit verifies the entire chain without modifying it.
func InspectDeploymentAudit(path string) ([]DeploymentAuditEnvelope, error) {
	file, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	scanner := bufio.NewScanner(file)
	scanner.Buffer(make([]byte, 4096), 1024*1024)
	envelopes := make([]DeploymentAuditEnvelope, 0, 128)
	previous := ""
	sequence := uint64(0)
	for scanner.Scan() {
		var envelope DeploymentAuditEnvelope
		if err := decodeStrict(scanner.Bytes(), &envelope); err != nil {
			return nil, fmt.Errorf("%w: decode deployment audit: %v", ErrRegistryIntegrity, err)
		}
		if envelope.Sequence != sequence+1 || envelope.PreviousSHA256 != previous || !hex64Pattern.MatchString(envelope.RecordSHA256) {
			return nil, fmt.Errorf("%w: deployment audit chain mismatch", ErrRegistryIntegrity)
		}
		if err := validateDeploymentAuditRecord(envelope.Record); err != nil {
			return nil, fmt.Errorf("%w: invalid deployment audit record: %v", ErrRegistryIntegrity, err)
		}
		body := struct {
			Sequence       uint64                `json:"sequence"`
			PreviousSHA256 string                `json:"previous_sha256,omitempty"`
			Record         DeploymentAuditRecord `json:"record"`
		}{Sequence: envelope.Sequence, PreviousSHA256: envelope.PreviousSHA256, Record: envelope.Record}
		digest, err := canonicalSHA256(body)
		if err != nil || digest != envelope.RecordSHA256 {
			return nil, fmt.Errorf("%w: deployment audit checksum mismatch", ErrRegistryIntegrity)
		}
		envelopes = append(envelopes, envelope)
		sequence = envelope.Sequence
		previous = envelope.RecordSHA256
	}
	if err := scanner.Err(); err != nil {
		return nil, fmt.Errorf("scan deployment audit: %w", err)
	}
	return envelopes, nil
}

func validateDeploymentAuditRecord(record DeploymentAuditRecord) error {
	if record.SchemaVersion != deploymentSchemaVersion || !validIdentifier(record.DeploymentID) || !validIdentifier(record.ModelID) || !semanticPattern.MatchString(record.CandidateVersion) || !semanticPattern.MatchString(record.ProductionVersion) || !hex64Pattern.MatchString(record.ConfigurationSHA256) || !validDeploymentRunState(record.State) || record.OccurredAtUTCNS <= 0 || len(record.ReasonCode) == 0 || len(record.ReasonCode) > 128 {
		return errors.New("invalid deployment audit identity")
	}
	switch record.Kind {
	case AuditObservation:
		if record.ObservationSequence == 0 || record.PairedObservation == nil || record.HypotheticalDecision == nil || !validRegime(record.Regime) {
			return errors.New("observation audit is missing paired evidence")
		}
		if record.PairedObservation.Sequence != record.ObservationSequence || record.PairedObservation.Regime != record.Regime {
			return errors.New("observation audit identity mismatch")
		}
		if err := validatePairedObservation(*record.PairedObservation, record.ProductionVersion, record.CandidateVersion); err != nil {
			return err
		}
		if err := validateHypotheticalDecision(*record.HypotheticalDecision); err != nil {
			return err
		}
		if record.HypotheticalDecision.FeatureSnapshotID != record.PairedObservation.Candidate.FeatureSnapshotID || record.HypotheticalDecision.FeatureSnapshotSHA256 != record.PairedObservation.Candidate.FeatureSnapshotSHA256 || record.HypotheticalDecision.ProductionForecastID != record.PairedObservation.Production.ForecastID || record.HypotheticalDecision.CandidateForecastID != record.PairedObservation.Candidate.ForecastID {
			return errors.New("hypothetical decision does not match paired observation")
		}
	case AuditRollbackTriggered:
		if record.Trigger == nil || !validRegime(record.Trigger.Regime) || !validMetric(record.Trigger.Metric) || record.Trigger.ReasonCode != record.ReasonCode {
			return errors.New("rollback trigger audit is incomplete")
		}
	case AuditRollbackCompleted, AuditFailClosedDisable, AuditFailClosedDisableFailed:
	case AuditCanaryApprovalRequested, AuditCanaryActivated:
		if record.Approval == nil || strings.TrimSpace(record.Approval.ApprovalReference) == "" || !hex64Pattern.MatchString(record.Approval.ConfigurationSHA256) {
			return errors.New("canary audit is missing approval evidence")
		}
	case AuditCanaryScopeDecision:
		if record.ScopeRequest == nil || record.ScopeDecision == nil || record.ScopeDecision.OrderExecutable || !record.ScopeDecision.DownstreamRiskRequired {
			return errors.New("canary scope audit is incomplete or executable")
		}
	default:
		return errors.New("invalid deployment audit kind")
	}
	return nil
}

func validateHypotheticalDecision(decision HypotheticalDecision) error {
	if decision.Executable || !decision.DownstreamRiskRequired || !validIdentifier(decision.FeatureSnapshotID) || !hex64Pattern.MatchString(decision.FeatureSnapshotSHA256) || !validIdentifier(decision.ProductionForecastID) || !validIdentifier(decision.CandidateForecastID) || !validDecisionAction(decision.ProductionAction) || !validDecisionAction(decision.CandidateAction) || !hex64Pattern.MatchString(decision.DecisionSHA256) {
		return errors.New("hypothetical decision is invalid or executable")
	}
	declared := decision.DecisionSHA256
	decision.DecisionSHA256 = ""
	digest, err := canonicalSHA256(decision)
	if err != nil || digest != declared {
		return errors.New("hypothetical decision checksum mismatch")
	}
	return nil
}
