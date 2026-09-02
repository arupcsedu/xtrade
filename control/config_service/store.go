package configservice

import (
	"bufio"
	"bytes"
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"syscall"
)

const auditSchemaVersion uint32 = 1

type auditBody struct {
	SchemaVersion     uint32          `json:"schema_version"`
	Sequence          uint64          `json:"sequence"`
	PreviousSHA256    string          `json:"previous_sha256,omitempty"`
	EventType         string          `json:"event_type"`
	Actor             string          `json:"actor"`
	SecondActor       string          `json:"second_actor,omitempty"`
	RequestIDs        []string        `json:"request_ids"`
	OccurredWallUTCNS int64           `json:"occurred_wall_utc_ns"`
	ObjectSHA256      string          `json:"object_sha256,omitempty"`
	Reason            string          `json:"reason"`
	Detail            json.RawMessage `json:"detail"`
}

type signedAuditRecord struct {
	Body            auditBody `json:"body"`
	RecordSHA256    string    `json:"record_sha256"`
	SignerKeyID     string    `json:"signer_key_id"`
	SignatureBase64 string    `json:"signature_base64"`
}

type persistedState struct {
	auditSequence uint64
	auditHash     string
	activeHash    string
	proposals     map[string]Proposal
	requestIDs    map[string]struct{}
	commandIDs    map[string]struct{}
	killSequence  uint64
	audit         []AuditEntry
}

func emptyPersistedState() persistedState {
	return persistedState{proposals: make(map[string]Proposal), requestIDs: make(map[string]struct{}), commandIDs: make(map[string]struct{})}
}

func (service *Service) configurationPath(digest string) string {
	return filepath.Join(service.root, "configurations", digest+".json")
}

func (service *Service) storeConfiguration(signed SignedConfiguration) error {
	payload, err := json.Marshal(signed)
	if err != nil {
		return err
	}
	payload = append(payload, '\n')
	path := service.configurationPath(signed.Configuration.SHA256)
	if stored, readErr := os.ReadFile(path); readErr == nil {
		if bytes.Equal(stored, payload) {
			return nil
		}
		return fmt.Errorf("%w: immutable configuration collision", ErrHashMismatch)
	} else if !errors.Is(readErr, os.ErrNotExist) {
		return readErr
	}
	return writeAtomicExclusive(path, payload, 0o600)
}

func (service *Service) loadConfiguration(digest string) (SignedConfiguration, error) {
	if !hex64Pattern.MatchString(digest) {
		return SignedConfiguration{}, ErrNotFound
	}
	file, err := os.Open(service.configurationPath(digest))
	if errors.Is(err, os.ErrNotExist) {
		return SignedConfiguration{}, ErrNotFound
	}
	if err != nil {
		return SignedConfiguration{}, err
	}
	defer file.Close()
	limited := io.LimitReader(file, maximumConfigurationBytes+1)
	payload, err := io.ReadAll(limited)
	if err != nil {
		return SignedConfiguration{}, err
	}
	if int64(len(payload)) > maximumConfigurationBytes {
		return SignedConfiguration{}, fmt.Errorf("%w: stored configuration exceeds size bound", ErrConfigurationInvalid)
	}
	var signed SignedConfiguration
	if err := decodeStrict(payload, &signed); err != nil {
		return SignedConfiguration{}, fmt.Errorf("decode configuration: %w", err)
	}
	if signed.Configuration.SHA256 != digest {
		return SignedConfiguration{}, ErrHashMismatch
	}
	if err := VerifySignedConfiguration(signed, service.trustedKeys); err != nil {
		return SignedConfiguration{}, err
	}
	return signed, nil
}

func (service *Service) appendAudit(body auditBody) error {
	body.SchemaVersion = auditSchemaVersion
	body.Sequence = service.state.auditSequence + 1
	body.PreviousSHA256 = service.state.auditHash
	digest, err := canonicalSHA256(body)
	if err != nil {
		return err
	}
	digestBytes, err := hex.DecodeString(digest)
	if err != nil {
		return err
	}
	record := signedAuditRecord{
		Body: body, RecordSHA256: digest, SignerKeyID: service.signerKeyID,
		SignatureBase64: base64.StdEncoding.EncodeToString(ed25519.Sign(service.signer, digestBytes)),
	}
	payload, err := json.Marshal(record)
	if err != nil {
		return err
	}
	file, err := os.OpenFile(filepath.Join(service.root, "audit.jsonl"), os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o600)
	if err != nil {
		return err
	}
	if _, err = file.Write(append(payload, '\n')); err == nil {
		err = file.Sync()
	}
	closeErr := file.Close()
	if err != nil {
		return err
	}
	if closeErr != nil {
		return closeErr
	}
	service.state.auditSequence = body.Sequence
	service.state.auditHash = digest
	service.state.audit = append(service.state.audit, publicAuditEntry(body, digest))
	return nil
}

func (service *Service) loadAudit() error {
	state := emptyPersistedState()
	path := filepath.Join(service.root, "audit.jsonl")
	file, err := os.Open(path)
	if errors.Is(err, os.ErrNotExist) {
		service.state = state
		return nil
	}
	if err != nil {
		return err
	}
	defer file.Close()
	scanner := bufio.NewScanner(file)
	scanner.Buffer(make([]byte, 64*1024), int(maximumConfigurationBytes))
	line := 0
	for scanner.Scan() {
		line++
		var record signedAuditRecord
		if err := decodeStrict(scanner.Bytes(), &record); err != nil {
			return fmt.Errorf("audit line %d: %w", line, err)
		}
		if err := service.verifyAuditRecord(record, state); err != nil {
			return fmt.Errorf("audit line %d: %w", line, err)
		}
		for _, requestID := range record.Body.RequestIDs {
			if _, exists := state.requestIDs[requestID]; exists {
				return fmt.Errorf("audit line %d: %w", line, ErrReplay)
			}
			state.requestIDs[requestID] = struct{}{}
		}
		if record.Body.EventType == "EMERGENCY_KILL_ENGAGED" {
			var signed SignedKillCommand
			if err := decodeStrict(record.Body.Detail, &signed); err != nil {
				return fmt.Errorf("audit line %d: %w", line, err)
			}
			if err := VerifySignedKillCommand(signed, service.trustedKeys); err != nil || signed.Command.AuthorityEpoch != service.authorityEpoch {
				return fmt.Errorf("audit line %d: invalid nested kill signature or epoch", line)
			}
		}
		if err := applyAuditRecord(&state, record); err != nil {
			return fmt.Errorf("audit line %d: %w", line, err)
		}
		state.auditSequence = record.Body.Sequence
		state.auditHash = record.RecordSHA256
		state.audit = append(state.audit, publicAuditEntry(record.Body, record.RecordSHA256))
	}
	if err := scanner.Err(); err != nil {
		return fmt.Errorf("scan audit: %w", err)
	}
	service.state = state
	return nil
}

func publicAuditEntry(body auditBody, digest string) AuditEntry {
	return AuditEntry{Sequence: body.Sequence, EventType: body.EventType, Actor: body.Actor, SecondActor: body.SecondActor, RequestIDs: append([]string(nil), body.RequestIDs...), OccurredWallUTCNS: body.OccurredWallUTCNS, ObjectSHA256: body.ObjectSHA256, Reason: body.Reason, RecordSHA256: digest}
}

func (service *Service) verifyAuditRecord(record signedAuditRecord, state persistedState) error {
	if record.Body.SchemaVersion != auditSchemaVersion || record.Body.Sequence != state.auditSequence+1 || record.Body.PreviousSHA256 != state.auditHash || record.Body.OccurredWallUTCNS <= 0 || !validIdentifier(record.Body.Actor) || !validIdentifier(record.Body.EventType) || len(record.Body.Reason) == 0 || len(record.Body.Reason) > 512 || len(record.Body.RequestIDs) == 0 || len(record.Body.RequestIDs) > 2 {
		return errors.New("invalid audit body")
	}
	for _, requestID := range record.Body.RequestIDs {
		if !validIdentifier(requestID) {
			return errors.New("invalid audit request identity")
		}
	}
	if record.Body.SecondActor != "" && !validIdentifier(record.Body.SecondActor) {
		return errors.New("invalid second audit actor")
	}
	digest, err := canonicalSHA256(record.Body)
	if err != nil || digest != record.RecordSHA256 {
		return errors.New("audit digest mismatch")
	}
	publicKey, ok := service.trustedKeys[record.SignerKeyID]
	if !ok {
		return ErrSignature
	}
	signature, err := base64.StdEncoding.Strict().DecodeString(record.SignatureBase64)
	if err != nil {
		return ErrSignature
	}
	digestBytes, _ := hex.DecodeString(digest)
	if !ed25519.Verify(publicKey, digestBytes, signature) {
		return ErrSignature
	}
	return nil
}

func applyAuditRecord(state *persistedState, record signedAuditRecord) error {
	switch record.Body.EventType {
	case "CONFIGURATION_PROPOSED":
		var proposal Proposal
		if err := decodeStrict(record.Body.Detail, &proposal); err != nil {
			return err
		}
		if !validIdentifier(proposal.ProposalID) || !hex64Pattern.MatchString(proposal.ConfigurationSHA256) || proposal.State != ProposalPending || proposal.Author != record.Body.Actor || proposal.Approver != "" || proposal.ActivationWallUTCNS <= 0 || record.Body.ObjectSHA256 != proposal.ConfigurationSHA256 {
			return errors.New("invalid proposed configuration audit")
		}
		if _, exists := state.proposals[proposal.ProposalID]; exists {
			return ErrReplay
		}
		state.proposals[proposal.ProposalID] = proposal
	case "CONFIGURATION_APPROVED":
		var proposal Proposal
		if err := decodeStrict(record.Body.Detail, &proposal); err != nil {
			return err
		}
		previous, exists := state.proposals[proposal.ProposalID]
		if !exists || previous.State != ProposalPending || proposal.State != ProposalApproved || proposal.ConfigurationSHA256 != previous.ConfigurationSHA256 || proposal.Author != previous.Author || proposal.ActivationWallUTCNS != previous.ActivationWallUTCNS || proposal.Approver != record.Body.Actor || proposal.Approver == proposal.Author || record.Body.SecondActor != proposal.Author || record.Body.ObjectSHA256 != proposal.ConfigurationSHA256 {
			return errors.New("invalid approval audit transition")
		}
		state.proposals[proposal.ProposalID] = proposal
	case "CONFIGURATION_ACTIVATED":
		var proposal Proposal
		if err := decodeStrict(record.Body.Detail, &proposal); err != nil {
			return err
		}
		previous, exists := state.proposals[proposal.ProposalID]
		if !exists || previous.State != ProposalApproved || proposal.State != ProposalActive || proposal.ConfigurationSHA256 != previous.ConfigurationSHA256 || proposal.Author != previous.Author || proposal.Approver != previous.Approver || proposal.ActivationWallUTCNS != previous.ActivationWallUTCNS || record.Body.SecondActor != proposal.Approver || record.Body.ObjectSHA256 != proposal.ConfigurationSHA256 {
			return errors.New("invalid activation audit transition")
		}
		state.proposals[proposal.ProposalID] = proposal
		state.activeHash = proposal.ConfigurationSHA256
	case "CONFIGURATION_ROLLED_BACK":
		var detail struct {
			TargetSHA256 string `json:"target_sha256"`
		}
		if err := decodeStrict(record.Body.Detail, &detail); err != nil || !hex64Pattern.MatchString(detail.TargetSHA256) || detail.TargetSHA256 != record.Body.ObjectSHA256 || state.activeHash == "" || record.Body.SecondActor == "" || record.Body.SecondActor == record.Body.Actor {
			return errors.New("invalid rollback detail")
		}
		state.activeHash = detail.TargetSHA256
	case "EMERGENCY_KILL_ENGAGED":
		var signed SignedKillCommand
		if err := decodeStrict(record.Body.Detail, &signed); err != nil {
			return err
		}
		if signed.Command.CommandSequence <= state.killSequence || signed.Command.ConfigurationSHA256 != state.activeHash || record.Body.ObjectSHA256 != state.activeHash {
			return ErrReplay
		}
		if _, exists := state.commandIDs[signed.Command.CommandID]; exists {
			return ErrReplay
		}
		state.commandIDs[signed.Command.CommandID] = struct{}{}
		state.killSequence = signed.Command.CommandSequence
	default:
		return errors.New("unknown audit event type")
	}
	return nil
}

func (service *Service) withExclusiveLock(operation func() error) error {
	service.mu.Lock()
	defer service.mu.Unlock()
	lockFile, err := os.OpenFile(filepath.Join(service.root, ".control.lock"), os.O_CREATE|os.O_RDWR, 0o600)
	if err != nil {
		return err
	}
	defer lockFile.Close()
	if err := syscall.Flock(int(lockFile.Fd()), syscall.LOCK_EX); err != nil {
		return err
	}
	defer syscall.Flock(int(lockFile.Fd()), syscall.LOCK_UN) //nolint:errcheck
	if err := service.loadAudit(); err != nil {
		return err
	}
	return operation()
}

func writeAtomicExclusive(path string, payload []byte, mode os.FileMode) error {
	directory := filepath.Dir(path)
	temp, err := os.CreateTemp(directory, ".pending-*")
	if err != nil {
		return err
	}
	tempPath := temp.Name()
	removeTemp := true
	defer func() {
		if removeTemp {
			_ = os.Remove(tempPath)
		}
	}()
	if err := temp.Chmod(mode); err != nil {
		temp.Close()
		return err
	}
	if _, err := temp.Write(payload); err != nil {
		temp.Close()
		return err
	}
	if err := temp.Sync(); err != nil {
		temp.Close()
		return err
	}
	if err := temp.Close(); err != nil {
		return err
	}
	if err := os.Link(tempPath, path); err != nil {
		return err
	}
	removeTemp = false
	if err := os.Remove(tempPath); err != nil {
		return err
	}
	directoryFile, err := os.Open(directory)
	if err != nil {
		return err
	}
	err = directoryFile.Sync()
	closeErr := directoryFile.Close()
	if err != nil {
		return err
	}
	return closeErr
}

func sortedRoleMap(bindings []OperatorRoleBinding) map[string]map[OperatorRole]struct{} {
	result := make(map[string]map[OperatorRole]struct{}, len(bindings))
	for _, binding := range bindings {
		roles := make(map[OperatorRole]struct{}, len(binding.Roles))
		for _, role := range binding.Roles {
			roles[role] = struct{}{}
		}
		result[binding.Principal] = roles
	}
	return result
}

func trustConfigurationHash(keys map[string]ed25519.PublicKey, epoch uint64) string {
	identifiers := make([]string, 0, len(keys))
	for keyID := range keys {
		identifiers = append(identifiers, keyID)
	}
	sort.Strings(identifiers)
	hash := sha256.New()
	for _, keyID := range identifiers {
		hash.Write([]byte(keyID))
		hash.Write(keys[keyID])
	}
	hash.Write([]byte(fmt.Sprintf("%d", epoch)))
	return hex.EncodeToString(hash.Sum(nil))
}
