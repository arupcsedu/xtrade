package configservice

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

// Clock supplies auditable UTC wall time. Ordering and staleness at the edge
// use explicit monotonic values supplied by the edge process.
type Clock func() time.Time

// ServiceConfig configures the local administrative service. Identity
// authentication remains an external boundary; BootstrapRoles apply only
// until the first signed configuration is active.
type ServiceConfig struct {
	Root           string
	TrustedKeys    map[string]ed25519.PublicKey
	SignerKeyID    string
	Signer         ed25519.PrivateKey
	BootstrapRoles []OperatorRoleBinding
	AuthorityEpoch uint64
	Clock          Clock
}

// Service is a filesystem-backed, signed configuration authority. It has no
// dependency on trading processes and is never called by the order hot path.
type Service struct {
	root           string
	trustedKeys    map[string]ed25519.PublicKey
	signerKeyID    string
	signer         ed25519.PrivateKey
	bootstrapRoles map[string]map[OperatorRole]struct{}
	authorityEpoch uint64
	clock          Clock
	mu             sync.Mutex
	closed         atomic.Bool
	state          persistedState
}

// Open verifies the complete audit chain before declaring the service healthy.
func Open(config ServiceConfig) (*Service, error) {
	if strings.TrimSpace(config.Root) == "" || config.AuthorityEpoch == 0 {
		return nil, errors.New("control root and positive authority epoch are required")
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
			return nil, errors.New("signer conflicts with trust store")
		}
		trusted[config.SignerKeyID] = append(ed25519.PublicKey(nil), publicKey...)
	}
	if len(trusted) == 0 {
		return nil, errors.New("at least one trusted key is required")
	}
	if !validateBootstrapRoles(config.BootstrapRoles) {
		return nil, errors.New("bootstrap roles lack two-person and emergency authority coverage")
	}
	bootstrap := sortedRoleMap(config.BootstrapRoles)
	root, err := filepath.Abs(config.Root)
	if err != nil {
		return nil, err
	}
	if err := os.MkdirAll(filepath.Join(root, "configurations"), 0o700); err != nil {
		return nil, err
	}
	clock := config.Clock
	if clock == nil {
		clock = time.Now
	}
	service := &Service{
		root: root, trustedKeys: trusted, signerKeyID: config.SignerKeyID,
		signer: append(ed25519.PrivateKey(nil), config.Signer...), bootstrapRoles: bootstrap,
		authorityEpoch: config.AuthorityEpoch, clock: clock, state: emptyPersistedState(),
	}
	if err := service.loadAudit(); err != nil {
		return nil, fmt.Errorf("verify configuration audit: %w", err)
	}
	for _, proposal := range service.state.proposals {
		if _, err := service.loadConfiguration(proposal.ConfigurationSHA256); err != nil {
			return nil, fmt.Errorf("verify proposed configuration: %w", err)
		}
	}
	if service.state.activeHash != "" {
		if _, err := service.loadConfiguration(service.state.activeHash); err != nil {
			return nil, fmt.Errorf("verify active configuration: %w", err)
		}
	}
	return service, nil
}

// Health exposes build, readiness, trust hash, and current configuration hash.
func (service *Service) Health() ServiceHealth {
	service.mu.Lock()
	defer service.mu.Unlock()
	health := ServiceHealth{Healthy: true, Ready: len(service.signer) == ed25519.PrivateKeySize, ReasonCode: "READY", ConfigurationHash: service.state.activeHash, Build: CurrentBuildInfo()}
	if service.closed.Load() {
		health.Ready = false
		health.ReasonCode = "STOPPED"
		return health
	}
	if !health.Ready {
		health.ReasonCode = "READ_ONLY_SIGNER_UNAVAILABLE"
		if health.ConfigurationHash == "" {
			health.ConfigurationHash = trustConfigurationHash(service.trustedKeys, service.authorityEpoch)
		}
	}
	return health
}

// DryRun canonicalizes and validates without publishing or persisting state.
func (service *Service) DryRun(configuration Configuration) ValidationReport {
	return dryRunConfiguration(configuration)
}

// Propose stores a signed immutable snapshot and stages it for later approval.
func (service *Service) Propose(ctx context.Context, request ProposeRequest) (Proposal, error) {
	var proposal Proposal
	err := service.withExclusiveLock(func() error {
		now := service.nowUTCNS()
		if err := contextError(ctx); err != nil {
			return err
		}
		if err := service.requireMutable(); err != nil {
			return err
		}
		if err := service.authorize(request.Authorization, RoleConfigurationAuthor, now); err != nil {
			return err
		}
		if err := validateReason(request.Reason); err != nil {
			return err
		}
		if err := VerifySignedConfiguration(request.Configuration, service.trustedKeys); err != nil {
			return err
		}
		configuration := request.Configuration.Configuration
		if request.ActivationWallUTCNS <= now || request.ActivationWallUTCNS < configuration.ValidFromWallUTCNS || request.ActivationWallUTCNS >= configuration.ValidUntilWallUTCNS {
			return fmt.Errorf("%w: activation must be staged inside validity interval", ErrInvalidTransition)
		}
		if err := service.validateLineage(configuration); err != nil {
			return err
		}
		proposal = Proposal{
			ProposalID: "proposal-" + configuration.SHA256[:24], ConfigurationSHA256: configuration.SHA256,
			Author: request.Authorization.Principal, ActivationWallUTCNS: request.ActivationWallUTCNS, State: ProposalPending,
		}
		if _, exists := service.state.proposals[proposal.ProposalID]; exists {
			return ErrReplay
		}
		if err := service.storeConfiguration(request.Configuration); err != nil {
			return err
		}
		detail, _ := json.Marshal(proposal)
		if err := service.appendAudit(auditBody{EventType: "CONFIGURATION_PROPOSED", Actor: proposal.Author, RequestIDs: []string{request.Authorization.RequestID}, OccurredWallUTCNS: now, ObjectSHA256: configuration.SHA256, Reason: request.Reason, Detail: detail}); err != nil {
			return err
		}
		service.state.proposals[proposal.ProposalID] = proposal
		service.state.requestIDs[request.Authorization.RequestID] = struct{}{}
		return nil
	})
	return proposal, err
}

// Approve records the mandatory second, distinct principal.
func (service *Service) Approve(ctx context.Context, request ApprovalRequest) (Proposal, error) {
	var proposal Proposal
	err := service.withExclusiveLock(func() error {
		now := service.nowUTCNS()
		if err := contextError(ctx); err != nil {
			return err
		}
		if err := service.requireMutable(); err != nil {
			return err
		}
		if err := service.authorize(request.Authorization, RoleApprover, now); err != nil {
			return err
		}
		if err := validateReason(request.Reason); err != nil {
			return err
		}
		stored, exists := service.state.proposals[request.ProposalID]
		if !exists {
			return ErrNotFound
		}
		if stored.State != ProposalPending {
			return ErrInvalidTransition
		}
		if stored.Author == request.Authorization.Principal {
			return ErrTwoPersonControl
		}
		stored.Approver = request.Authorization.Principal
		stored.State = ProposalApproved
		detail, _ := json.Marshal(stored)
		if err := service.appendAudit(auditBody{EventType: "CONFIGURATION_APPROVED", Actor: stored.Approver, SecondActor: stored.Author, RequestIDs: []string{request.Authorization.RequestID}, OccurredWallUTCNS: now, ObjectSHA256: stored.ConfigurationSHA256, Reason: request.Reason, Detail: detail}); err != nil {
			return err
		}
		service.state.proposals[stored.ProposalID] = stored
		service.state.requestIDs[request.Authorization.RequestID] = struct{}{}
		proposal = stored
		return nil
	})
	return proposal, err
}

// Activate publishes an already approved snapshot after its staged time.
func (service *Service) Activate(ctx context.Context, request ActivationRequest) (SignedConfiguration, error) {
	var signed SignedConfiguration
	err := service.withExclusiveLock(func() error {
		now := service.nowUTCNS()
		if err := contextError(ctx); err != nil {
			return err
		}
		if err := service.requireMutable(); err != nil {
			return err
		}
		if err := service.authorize(request.Authorization, RoleActivator, now); err != nil {
			return err
		}
		if err := validateReason(request.Reason); err != nil {
			return err
		}
		proposal, exists := service.state.proposals[request.ProposalID]
		if !exists {
			return ErrNotFound
		}
		if proposal.State != ProposalApproved || proposal.Author == proposal.Approver || now < proposal.ActivationWallUTCNS {
			return ErrInvalidTransition
		}
		var err error
		signed, err = service.loadConfiguration(proposal.ConfigurationSHA256)
		if err != nil {
			return err
		}
		configuration := signed.Configuration
		if now < configuration.ValidFromWallUTCNS || now >= configuration.ValidUntilWallUTCNS {
			return ErrConfigurationStale
		}
		if err := service.validateLineage(configuration); err != nil {
			return err
		}
		proposal.State = ProposalActive
		detail, _ := json.Marshal(proposal)
		if err := service.appendAudit(auditBody{EventType: "CONFIGURATION_ACTIVATED", Actor: request.Authorization.Principal, SecondActor: proposal.Approver, RequestIDs: []string{request.Authorization.RequestID}, OccurredWallUTCNS: now, ObjectSHA256: configuration.SHA256, Reason: request.Reason, Detail: detail}); err != nil {
			return err
		}
		service.state.activeHash = configuration.SHA256
		service.state.proposals[proposal.ProposalID] = proposal
		service.state.requestIDs[request.Authorization.RequestID] = struct{}{}
		return nil
	})
	return signed, err
}

// Rollback atomically selects an older still-valid signed snapshot. It cannot
// edit that artifact and requires a rollback operator plus distinct approver.
func (service *Service) Rollback(ctx context.Context, request RollbackRequest) (SignedConfiguration, error) {
	var target SignedConfiguration
	err := service.withExclusiveLock(func() error {
		now := service.nowUTCNS()
		if err := contextError(ctx); err != nil {
			return err
		}
		if err := service.requireMutable(); err != nil {
			return err
		}
		if err := service.authorize(request.Initiator, RoleRollbackOperator, now); err != nil {
			return err
		}
		if err := service.authorize(request.Approver, RoleApprover, now); err != nil {
			return err
		}
		if request.Initiator.Principal == request.Approver.Principal {
			return ErrTwoPersonControl
		}
		if request.Initiator.RequestID == request.Approver.RequestID {
			return ErrReplay
		}
		if err := validateReason(request.Reason); err != nil {
			return err
		}
		if request.ExpectedActiveSHA256 != service.state.activeHash || !hex64Pattern.MatchString(request.TargetSHA256) {
			return ErrInvalidTransition
		}
		active, err := service.loadConfiguration(service.state.activeHash)
		if err != nil {
			return err
		}
		target, err = service.loadConfiguration(request.TargetSHA256)
		if err != nil {
			return err
		}
		if target.Configuration.ConfigurationID != active.Configuration.ConfigurationID || target.Configuration.Environment != active.Configuration.Environment || target.Configuration.Revision >= active.Configuration.Revision {
			return ErrInvalidTransition
		}
		if now < target.Configuration.ValidFromWallUTCNS || now >= target.Configuration.ValidUntilWallUTCNS {
			return ErrConfigurationStale
		}
		detail, _ := json.Marshal(struct {
			TargetSHA256 string `json:"target_sha256"`
		}{request.TargetSHA256})
		if err := service.appendAudit(auditBody{EventType: "CONFIGURATION_ROLLED_BACK", Actor: request.Initiator.Principal, SecondActor: request.Approver.Principal, RequestIDs: []string{request.Initiator.RequestID, request.Approver.RequestID}, OccurredWallUTCNS: now, ObjectSHA256: request.TargetSHA256, Reason: request.Reason, Detail: detail}); err != nil {
			return err
		}
		service.state.activeHash = request.TargetSHA256
		service.state.requestIDs[request.Initiator.RequestID] = struct{}{}
		service.state.requestIDs[request.Approver.RequestID] = struct{}{}
		return nil
	})
	return target, err
}

// EmergencyKill signs and durably journals an engage-only command. Clearing a
// kill requires the normal reviewed configuration workflow.
func (service *Service) EmergencyKill(ctx context.Context, request EmergencyKillRequest) (SignedKillCommand, error) {
	var signed SignedKillCommand
	err := service.withExclusiveLock(func() error {
		now := service.nowUTCNS()
		if err := contextError(ctx); err != nil {
			return err
		}
		if err := service.requireMutable(); err != nil {
			return err
		}
		if err := service.authorize(request.Authorization, RoleKillOperator, now); err != nil {
			return err
		}
		if service.state.activeHash == "" {
			return ErrUnsafeEdgeState
		}
		if request.CommandSequence <= service.state.killSequence {
			return ErrReplay
		}
		if _, exists := service.state.commandIDs[request.CommandID]; exists {
			return ErrReplay
		}
		command := KillCommand{SchemaVersion: ConfigurationSchemaVersion, CommandID: request.CommandID, ConfigurationSHA256: service.state.activeHash, Scope: request.Scope, TargetID: request.TargetID, CommandSequence: request.CommandSequence, AuthorityEpoch: service.authorityEpoch, EffectiveWallUTCNS: now, ReasonCode: request.ReasonCode, Engaged: true}
		var err error
		signed, err = signKillCommand(command, service.signerKeyID, service.signer)
		if err != nil {
			return err
		}
		detail, _ := json.Marshal(signed)
		if err := service.appendAudit(auditBody{EventType: "EMERGENCY_KILL_ENGAGED", Actor: request.Authorization.Principal, RequestIDs: []string{request.Authorization.RequestID}, OccurredWallUTCNS: now, ObjectSHA256: service.state.activeHash, Reason: request.ReasonCode, Detail: detail}); err != nil {
			return err
		}
		service.state.killSequence = request.CommandSequence
		service.state.commandIDs[request.CommandID] = struct{}{}
		service.state.requestIDs[request.Authorization.RequestID] = struct{}{}
		return nil
	})
	return signed, err
}

func (service *Service) GetActive(ctx context.Context, authorization AuthContext) (SignedConfiguration, error) {
	if err := contextError(ctx); err != nil {
		return SignedConfiguration{}, err
	}
	service.mu.Lock()
	defer service.mu.Unlock()
	if err := service.authorize(authorization, RoleViewer, service.nowUTCNS()); err != nil {
		return SignedConfiguration{}, err
	}
	if service.state.activeHash == "" {
		return SignedConfiguration{}, ErrNotFound
	}
	return service.loadConfiguration(service.state.activeHash)
}

func (service *Service) Inspect(ctx context.Context, digest string, authorization AuthContext) (SignedConfiguration, error) {
	if err := contextError(ctx); err != nil {
		return SignedConfiguration{}, err
	}
	service.mu.Lock()
	defer service.mu.Unlock()
	if err := service.authorize(authorization, RoleViewer, service.nowUTCNS()); err != nil {
		return SignedConfiguration{}, err
	}
	return service.loadConfiguration(digest)
}

// AuditTrail returns redacted summaries only after the complete chain has been
// verified. Configuration contents and signatures remain separately inspectable.
func (service *Service) AuditTrail(ctx context.Context, authorization AuthContext) ([]AuditEntry, error) {
	if err := contextError(ctx); err != nil {
		return nil, err
	}
	service.mu.Lock()
	defer service.mu.Unlock()
	if err := service.authorize(authorization, RoleViewer, service.nowUTCNS()); err != nil {
		return nil, err
	}
	entries := make([]AuditEntry, len(service.state.audit))
	copy(entries, service.state.audit)
	for index := range entries {
		entries[index].RequestIDs = append([]string(nil), entries[index].RequestIDs...)
	}
	return entries, nil
}

// Metrics exposes bounded-cardinality service state without principal labels.
func (service *Service) Metrics(ctx context.Context) (ControlMetricsSnapshot, error) {
	if err := contextError(ctx); err != nil {
		return ControlMetricsSnapshot{}, err
	}
	service.mu.Lock()
	defer service.mu.Unlock()
	metrics := ControlMetricsSnapshot{AuditSequence: service.state.auditSequence, EmergencyKillSequence: service.state.killSequence, ConfigurationSHA256: service.state.activeHash}
	for _, proposal := range service.state.proposals {
		if proposal.State == ProposalPending || proposal.State == ProposalApproved {
			metrics.PendingProposals++
		}
	}
	if service.state.activeHash != "" {
		active, err := service.loadConfiguration(service.state.activeHash)
		if err != nil {
			return ControlMetricsSnapshot{}, err
		}
		metrics.ActiveRevision = active.Configuration.Revision
	}
	return metrics, nil
}

// Shutdown prevents future mutations. There are no background goroutines or
// held descriptors; completed calls have already fsynced before returning.
func (service *Service) Shutdown(ctx context.Context) error {
	if err := contextError(ctx); err != nil {
		return err
	}
	service.mu.Lock()
	defer service.mu.Unlock()
	service.closed.Store(true)
	return nil
}

func (service *Service) validateLineage(configuration Configuration) error {
	if service.state.activeHash == "" {
		if configuration.Revision != 1 || configuration.ParentSHA256 != "" {
			return ErrInvalidTransition
		}
		return nil
	}
	active, err := service.loadConfiguration(service.state.activeHash)
	if err != nil {
		return err
	}
	if configuration.ConfigurationID != active.Configuration.ConfigurationID || configuration.Environment != active.Configuration.Environment || configuration.Revision != active.Configuration.Revision+1 || configuration.ParentSHA256 != active.Configuration.SHA256 {
		return ErrInvalidTransition
	}
	return nil
}

func (service *Service) authorize(auth AuthContext, role OperatorRole, now int64) error {
	if !auth.Authenticated || !validIdentifier(auth.Principal) || !validIdentifier(auth.RequestID) || auth.AuthorityEpoch != service.authorityEpoch || auth.AuthenticatedUTCNS <= 0 || auth.AuthenticatedUTCNS > now || auth.ExpiresUTCNS <= now || auth.ExpiresUTCNS <= auth.AuthenticatedUTCNS || auth.ExpiresUTCNS-auth.AuthenticatedUTCNS > maximumAuthorizationLifetimeNS {
		return ErrAuthentication
	}
	if _, used := service.state.requestIDs[auth.RequestID]; used {
		return ErrReplay
	}
	roles := service.bootstrapRoles
	if service.state.activeHash != "" {
		active, err := service.loadConfiguration(service.state.activeHash)
		if err != nil {
			return err
		}
		roles = sortedRoleMap(active.Configuration.OperatorRoles)
	}
	principalRoles, exists := roles[auth.Principal]
	if !exists {
		return ErrAuthorization
	}
	if _, allowed := principalRoles[role]; !allowed {
		return ErrAuthorization
	}
	return nil
}

func (service *Service) requireMutable() error {
	if service.closed.Load() {
		return errors.New("control service is stopped")
	}
	if len(service.signer) != ed25519.PrivateKeySize {
		return errors.New("control service is read-only: signer unavailable")
	}
	return nil
}

func (service *Service) nowUTCNS() int64 { return service.clock().UTC().UnixNano() }

func validateReason(reason string) error {
	if strings.TrimSpace(reason) == "" || len(reason) > 512 {
		return errors.New("auditable reason is required and bounded")
	}
	return nil
}

func contextError(ctx context.Context) error {
	if ctx == nil {
		return errors.New("nil context")
	}
	select {
	case <-ctx.Done():
		return ctx.Err()
	default:
		return nil
	}
}
