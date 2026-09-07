package configservice

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"sync"
	"testing"
	"time"
)

const controlTestSeed = "aegis-mx-control-20260902"

type testFixture struct {
	service    *Service
	privateKey ed25519.PrivateKey
	publicKey  ed25519.PublicKey
	now        *time.Time
	root       string
}

func newFixture(t *testing.T) testFixture {
	t.Helper()
	seed := sha256.Sum256([]byte(controlTestSeed))
	privateKey := ed25519.NewKeyFromSeed(seed[:])
	publicKey := append(ed25519.PublicKey(nil), privateKey.Public().(ed25519.PublicKey)...)
	now := time.Date(2026, 9, 2, 14, 30, 0, 0, time.UTC)
	root := t.TempDir()
	service, err := Open(ServiceConfig{Root: root, TrustedKeys: map[string]ed25519.PublicKey{"control-key": publicKey}, SignerKeyID: "control-key", Signer: privateKey, BootstrapRoles: testRoles(), AuthorityEpoch: 7, Clock: func() time.Time { return now }})
	if err != nil {
		t.Fatalf("open control service: %v", err)
	}
	return testFixture{service: service, privateKey: privateKey, publicKey: publicKey, now: &now, root: root}
}

func testRoles() []OperatorRoleBinding {
	return []OperatorRoleBinding{
		{Principal: "alice", Roles: []OperatorRole{RoleConfigurationAuthor, RoleApprover}},
		{Principal: "bob", Roles: []OperatorRole{RoleApprover}},
		{Principal: "carol", Roles: []OperatorRole{RoleActivator}},
		{Principal: "dave", Roles: []OperatorRole{RoleRollbackOperator}},
		{Principal: "erin", Roles: []OperatorRole{RoleKillOperator}},
		{Principal: "viewer", Roles: []OperatorRole{RoleViewer}},
	}
}

func testConfiguration(now time.Time, revision uint64, parent string) Configuration {
	nowNS := now.UnixNano()
	return Configuration{
		SchemaVersion: ConfigurationSchemaVersion, ConfigurationID: "edge-prod", Revision: revision,
		ParentSHA256: parent, Environment: "production-paper", CreatedWallUTCNS: nowNS - int64(time.Hour),
		ValidFromWallUTCNS: nowNS - int64(time.Minute), ValidUntilWallUTCNS: nowNS + int64(2*time.Hour), MaximumEdgeOfflineAgeNS: uint64(time.Minute),
		Strategies:      []StrategyConfiguration{{StrategyID: "alpha", Enabled: true, MaximumOrderQuantity: 100, MaximumPositionQuantity: 1000, AllowedModelIDs: []string{"micro"}, TradingSessionIDs: []string{"regular"}}},
		Venues:          []VenueConfiguration{{VenueID: "SYNTH", Enabled: true, Mode: TradingModePaper, MaximumOrdersPerSecond: 100, MaximumCancelsPerSecond: 200, RequireSelfTradePrevention: true}},
		RiskLimits:      []RiskLimitConfiguration{{AccountID: "paper-account", MaximumGrossExposureCurrencyNanos: 10_000_000_000, MaximumAbsoluteNetExposureCurrencyNanos: 5_000_000_000, MaximumDailyLossCurrencyNanos: 1_000_000_000, MaximumDrawdownCurrencyNanos: 500_000_000, CreditCapitalLimitCurrencyNanos: 10_000_000_000, MaximumConfigurationAgeNS: uint64(30 * time.Second), SymbolLimits: []SymbolRiskLimit{{InstrumentID: "XYZ", Authorized: true, MaximumOrderQuantityUnits: 100, MaximumOrderNotionalCurrencyNanos: 1_000_000_000, MaximumAbsolutePositionUnits: 1000}, {InstrumentID: "ABC", MaximumOrderQuantityUnits: 100, MaximumOrderNotionalCurrencyNanos: 1_000_000_000, MaximumAbsolutePositionUnits: 1000}}}},
		Models:          []ModelEligibility{{ModelID: "micro", SemanticVersion: "1.2.3", Eligible: true, MaximumOODScorePPM: 100_000, AllowedStrategyIDs: []string{"alpha"}}},
		EnsembleCaps:    []EnsembleCap{{EnsembleID: "primary", ModelID: "micro", MaximumWeightPPM: 750_000}},
		TradingSessions: []TradingSession{{SessionID: "regular", VenueID: "SYNTH", OpensWallUTCNS: nowNS - int64(time.Hour), ClosesWallUTCNS: nowNS + int64(time.Hour)}},
		EventCalendar:   []EventCalendarEntry{{EventID: "macro-1", EventType: "CPI", ScheduledWallUTCNS: nowNS + int64(20*time.Minute), PreEventLeadNS: uint64(5 * time.Minute), PostEventStabilizationNS: uint64(10 * time.Minute), SourceID: "official"}},
		KillSwitches:    []ConfiguredKillSwitch{{KillID: "symbol-safe", Scope: KillScopeSymbol, TargetID: "ABC", Engaged: true, ReasonCode: "MANUAL_REVIEW"}},
		OperatorRoles:   testRoles(),
	}
}

func signedTestConfiguration(t *testing.T, fixture testFixture, configuration Configuration) SignedConfiguration {
	t.Helper()
	finalized, err := FinalizeConfiguration(configuration)
	if err != nil {
		t.Fatalf("finalize: %v", err)
	}
	signed, err := SignConfiguration(finalized, "control-key", fixture.privateKey)
	if err != nil {
		t.Fatalf("sign: %v", err)
	}
	return signed
}

func auth(now time.Time, principal, requestID string) AuthContext {
	return AuthContext{Authenticated: true, Principal: principal, RequestID: requestID, AuthorityEpoch: 7, AuthenticatedUTCNS: now.Add(-time.Second).UnixNano(), ExpiresUTCNS: now.Add(5 * time.Minute).UnixNano()}
}

func activateFirst(t *testing.T, fixture testFixture) (SignedConfiguration, Proposal) {
	t.Helper()
	now := *fixture.now
	signed := signedTestConfiguration(t, fixture, testConfiguration(now, 1, ""))
	proposal, err := fixture.service.Propose(context.Background(), ProposeRequest{Authorization: auth(now, "alice", "request-propose-1"), Configuration: signed, ActivationWallUTCNS: now.Add(10 * time.Second).UnixNano(), Reason: "initial reviewed paper configuration"})
	if err != nil {
		t.Fatalf("propose: %v", err)
	}
	proposal, err = fixture.service.Approve(context.Background(), ApprovalRequest{Authorization: auth(now, "bob", "request-approve-1"), ProposalID: proposal.ProposalID, Reason: "independent limits review"})
	if err != nil {
		t.Fatalf("approve: %v", err)
	}
	if _, err := fixture.service.Activate(context.Background(), ActivationRequest{Authorization: auth(now, "carol", "request-activate-early"), ProposalID: proposal.ProposalID, Reason: "premature attempt"}); !errors.Is(err, ErrInvalidTransition) {
		t.Fatalf("premature activation error = %v", err)
	}
	*fixture.now = now.Add(11 * time.Second)
	active, err := fixture.service.Activate(context.Background(), ActivationRequest{Authorization: auth(*fixture.now, "carol", "request-activate-1"), ProposalID: proposal.ProposalID, Reason: "stage reached"})
	if err != nil {
		t.Fatalf("activate: %v", err)
	}
	return active, proposal
}

func TestConfigurationWorkflowAndRestart(t *testing.T) {
	fixture := newFixture(t)
	active, proposal := activateFirst(t, fixture)
	if active.Configuration.SHA256 == "" || ConfigurationVersion(active.Configuration) == "" {
		t.Fatal("active configuration lacks immutable identity")
	}
	if proposal.Author != "alice" || proposal.Approver != "bob" {
		t.Fatalf("two-person identities = %#v", proposal)
	}
	if health := fixture.service.Health(); !health.Healthy || !health.Ready || health.ConfigurationHash != active.Configuration.SHA256 || health.Build.LiveTradingCapable {
		t.Fatalf("health = %#v", health)
	}
	metrics, err := fixture.service.Metrics(context.Background())
	if err != nil || metrics.AuditSequence != 3 || metrics.ActiveRevision != 1 || metrics.ConfigurationSHA256 != active.Configuration.SHA256 {
		t.Fatalf("metrics = %#v, %v", metrics, err)
	}
	audit, err := fixture.service.AuditTrail(context.Background(), auth(*fixture.now, "viewer", "read-audit-1"))
	if err != nil || len(audit) != 3 || audit[2].EventType != "CONFIGURATION_ACTIVATED" || audit[2].RecordSHA256 == "" {
		t.Fatalf("audit = %#v, %v", audit, err)
	}
	reopened, err := Open(ServiceConfig{Root: fixture.root, TrustedKeys: map[string]ed25519.PublicKey{"control-key": fixture.publicKey}, SignerKeyID: "control-key", Signer: fixture.privateKey, BootstrapRoles: testRoles(), AuthorityEpoch: 7, Clock: func() time.Time { return *fixture.now }})
	if err != nil {
		t.Fatalf("reopen: %v", err)
	}
	if _, err := reopened.GetActive(context.Background(), AuthContext{}); !errors.Is(err, ErrAuthentication) {
		t.Fatalf("unauthorized read error = %v", err)
	}
	recovered, err := reopened.GetActive(context.Background(), auth(*fixture.now, "viewer", "read-active-1"))
	if err != nil || recovered.Configuration.SHA256 != active.Configuration.SHA256 {
		t.Fatalf("recovered active = %#v, %v", recovered, err)
	}
	if _, err := reopened.Propose(context.Background(), ProposeRequest{Authorization: auth(*fixture.now, "alice", "request-propose-1"), Configuration: active, ActivationWallUTCNS: fixture.now.Add(time.Minute).UnixNano(), Reason: "replay"}); !errors.Is(err, ErrReplay) {
		t.Fatalf("request replay error = %v", err)
	}
	if err := reopened.Shutdown(context.Background()); err != nil {
		t.Fatal(err)
	}
	if health := reopened.Health(); health.Ready || health.ReasonCode != "STOPPED" {
		t.Fatalf("shutdown health = %#v", health)
	}
}

func TestAuthorizationBypassAndTwoPersonControls(t *testing.T) {
	fixture := newFixture(t)
	now := *fixture.now
	signed := signedTestConfiguration(t, fixture, testConfiguration(now, 1, ""))
	tests := []struct {
		name string
		auth AuthContext
		want error
	}{
		{name: "unauthenticated", auth: AuthContext{Principal: "alice", RequestID: "unauth", AuthorityEpoch: 7}, want: ErrAuthentication},
		{name: "wrong role", auth: auth(now, "viewer", "wrong-role"), want: ErrAuthorization},
		{name: "wrong epoch", auth: func() AuthContext { value := auth(now, "alice", "wrong-epoch"); value.AuthorityEpoch = 8; return value }(), want: ErrAuthentication},
		{name: "expired", auth: func() AuthContext {
			value := auth(now, "alice", "expired")
			value.ExpiresUTCNS = now.Add(-time.Second).UnixNano()
			return value
		}(), want: ErrAuthentication},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			_, err := fixture.service.Propose(context.Background(), ProposeRequest{Authorization: test.auth, Configuration: signed, ActivationWallUTCNS: now.Add(time.Minute).UnixNano(), Reason: "authorization negative test"})
			if !errors.Is(err, test.want) {
				t.Fatalf("error = %v, want %v", err, test.want)
			}
		})
	}
	proposal, err := fixture.service.Propose(context.Background(), ProposeRequest{Authorization: auth(now, "alice", "valid-proposal"), Configuration: signed, ActivationWallUTCNS: now.Add(time.Minute).UnixNano(), Reason: "valid proposal"})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := fixture.service.Approve(context.Background(), ApprovalRequest{Authorization: auth(now, "alice", "self-approval"), ProposalID: proposal.ProposalID, Reason: "self approval must fail"}); !errors.Is(err, ErrTwoPersonControl) {
		t.Fatalf("self approval error = %v", err)
	}
}

func TestTamperLiveModeAndLineageRejection(t *testing.T) {
	fixture := newFixture(t)
	now := *fixture.now
	configuration := testConfiguration(now, 1, "")
	live := configuration
	live.Venues = append([]VenueConfiguration(nil), configuration.Venues...)
	live.Venues[0].Mode = TradingModeLive
	if report := fixture.service.DryRun(live); report.Valid {
		t.Fatal("LIVE configuration passed dry run")
	}
	if _, err := FinalizeConfiguration(live); !errors.Is(err, ErrLiveModeUnavailable) {
		t.Fatalf("LIVE finalize error = %v", err)
	}
	signed := signedTestConfiguration(t, fixture, configuration)
	tampered := signed
	tampered.Configuration.Strategies[0].MaximumOrderQuantity++
	if err := VerifySignedConfiguration(tampered, map[string]ed25519.PublicKey{"control-key": fixture.publicKey}); !errors.Is(err, ErrHashMismatch) {
		t.Fatalf("tamper error = %v", err)
	}
	active, _ := activateFirst(t, fixture)
	invalidNext := testConfiguration(*fixture.now, 3, active.Configuration.SHA256)
	signedNext := signedTestConfiguration(t, fixture, invalidNext)
	_, err := fixture.service.Propose(context.Background(), ProposeRequest{Authorization: auth(*fixture.now, "alice", "bad-lineage"), Configuration: signedNext, ActivationWallUTCNS: fixture.now.Add(time.Minute).UnixNano(), Reason: "skip revision"})
	if !errors.Is(err, ErrInvalidTransition) {
		t.Fatalf("lineage error = %v", err)
	}
}

func TestRollbackAndEmergencyKill(t *testing.T) {
	fixture := newFixture(t)
	first, _ := activateFirst(t, fixture)
	now := *fixture.now
	second := signedTestConfiguration(t, fixture, testConfiguration(now, 2, first.Configuration.SHA256))
	proposal, err := fixture.service.Propose(context.Background(), ProposeRequest{Authorization: auth(now, "alice", "propose-2"), Configuration: second, ActivationWallUTCNS: now.Add(10 * time.Second).UnixNano(), Reason: "second limits snapshot"})
	if err != nil {
		t.Fatal(err)
	}
	if _, err = fixture.service.Approve(context.Background(), ApprovalRequest{Authorization: auth(now, "bob", "approve-2"), ProposalID: proposal.ProposalID, Reason: "second review"}); err != nil {
		t.Fatal(err)
	}
	*fixture.now = now.Add(11 * time.Second)
	second, err = fixture.service.Activate(context.Background(), ActivationRequest{Authorization: auth(*fixture.now, "carol", "activate-2"), ProposalID: proposal.ProposalID, Reason: "scheduled activation"})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := fixture.service.Rollback(context.Background(), RollbackRequest{Initiator: auth(*fixture.now, "dave", "rollback-self-1"), Approver: auth(*fixture.now, "dave", "rollback-self-2"), ExpectedActiveSHA256: second.Configuration.SHA256, TargetSHA256: first.Configuration.SHA256, Reason: "invalid self approval"}); !errors.Is(err, ErrAuthorization) && !errors.Is(err, ErrTwoPersonControl) {
		t.Fatalf("same-person rollback error = %v", err)
	}
	rolledBack, err := fixture.service.Rollback(context.Background(), RollbackRequest{Initiator: auth(*fixture.now, "dave", "rollback-1"), Approver: auth(*fixture.now, "bob", "rollback-approval-1"), ExpectedActiveSHA256: second.Configuration.SHA256, TargetSHA256: first.Configuration.SHA256, Reason: "restore reviewed limits"})
	if err != nil || rolledBack.Configuration.SHA256 != first.Configuration.SHA256 {
		t.Fatalf("rollback = %#v, %v", rolledBack, err)
	}
	if _, err := fixture.service.EmergencyKill(context.Background(), EmergencyKillRequest{Authorization: auth(*fixture.now, "viewer", "kill-bypass"), CommandID: "kill-bypass", Scope: KillScopeFirm, CommandSequence: 1, ReasonCode: "RISK_STATE_UNSAFE"}); !errors.Is(err, ErrAuthorization) {
		t.Fatalf("kill bypass error = %v", err)
	}
	kill, err := fixture.service.EmergencyKill(context.Background(), EmergencyKillRequest{Authorization: auth(*fixture.now, "erin", "kill-1"), CommandID: "kill-1", Scope: KillScopeFirm, CommandSequence: 1, ReasonCode: "RISK_STATE_UNSAFE"})
	if err != nil || !kill.Command.Engaged {
		t.Fatalf("kill = %#v, %v", kill, err)
	}
	if _, err := fixture.service.EmergencyKill(context.Background(), EmergencyKillRequest{Authorization: auth(*fixture.now, "erin", "kill-2"), CommandID: "kill-2", Scope: KillScopeFirm, CommandSequence: 1, ReasonCode: "DUPLICATE"}); !errors.Is(err, ErrReplay) {
		t.Fatalf("kill sequence replay error = %v", err)
	}
	if _, err := fixture.service.EmergencyKill(context.Background(), EmergencyKillRequest{Authorization: auth(*fixture.now, "erin", "kill-3"), CommandID: "kill-1", Scope: KillScopeFirm, CommandSequence: 2, ReasonCode: "DUPLICATE_ID"}); !errors.Is(err, ErrReplay) {
		t.Fatalf("kill identity replay error = %v", err)
	}
}

func TestEdgeCacheFailsClosedAndPersistsKill(t *testing.T) {
	fixture := newFixture(t)
	active, _ := activateFirst(t, fixture)
	now := *fixture.now
	edgeRoot := t.TempDir()
	edge, err := OpenEdgeCache(EdgeCacheConfig{Root: edgeRoot, TrustedKeys: map[string]ed25519.PublicKey{"control-key": fixture.publicKey}, AuthorityEpoch: 7, ExpectedConfigurationID: "edge-prod", ExpectedEnvironment: "production-paper", StartupWallUTCNS: now.UnixNano(), StartupProcessMonotonicNS: 1_000})
	if err != nil {
		t.Fatal(err)
	}
	request := EdgeRequest{NowWallUTCNS: now.UnixNano(), NowProcessMonotonicTimeNS: 1_000, AccountID: "paper-account", InstrumentID: "XYZ", StrategyID: "alpha", VenueID: "SYNTH", SessionID: "regular", ModelID: "micro", ModelVersion: "1.2.3"}
	if decision := edge.Evaluate(request); decision.AllowNewOrders || decision.ReasonCode != "CONFIGURATION_UNAVAILABLE" {
		t.Fatalf("empty edge decision = %#v", decision)
	}
	if err := edge.InstallConfiguration(active, now.UnixNano(), 1_000); err != nil {
		t.Fatal(err)
	}
	wrongEnvironment := testConfiguration(now, 1, "")
	wrongEnvironment.Environment = "staging-paper"
	if err := edge.InstallConfiguration(signedTestConfiguration(t, fixture, wrongEnvironment), now.UnixNano(), 1_000); !errors.Is(err, ErrInvalidTransition) {
		t.Fatalf("wrong-environment install error = %v", err)
	}
	// Identical signed bytes are idempotent but cannot refresh offline age.
	if err := edge.InstallConfiguration(active, now.Add(20*time.Second).UnixNano(), 2_000); err != nil {
		t.Fatal(err)
	}
	active.Configuration.Strategies[0].Enabled = false
	decision := edge.Evaluate(request)
	if !decision.AllowNewOrders || decision.ConfigurationSHA256 != active.Configuration.SHA256 || decision.Mode != TradingModePaper {
		t.Fatalf("edge allow decision = %#v", decision)
	}
	active.Configuration.Strategies[0].Enabled = true
	stale := request
	stale.NowWallUTCNS += int64(31 * time.Second)
	stale.NowProcessMonotonicTimeNS += uint64(31 * time.Second)
	if decision := edge.Evaluate(stale); decision.AllowNewOrders || decision.ReasonCode != "RISK_CONFIGURATION_STALE" {
		t.Fatalf("stale decision = %#v", decision)
	}
	globallyStale := request
	globallyStale.NowWallUTCNS += int64(61 * time.Second)
	globallyStale.NowProcessMonotonicTimeNS += uint64(61 * time.Second)
	if decision := edge.Evaluate(globallyStale); decision.AllowNewOrders || decision.ReasonCode != "CONFIGURATION_CACHE_STALE" {
		t.Fatalf("globally stale decision = %#v", decision)
	}
	expired := request
	expired.NowWallUTCNS = active.Configuration.ValidUntilWallUTCNS
	if decision := edge.Evaluate(expired); decision.AllowNewOrders || decision.ReasonCode != "CONFIGURATION_EXPIRED_OR_CLOCK_UNSAFE" {
		t.Fatalf("expired decision = %#v", decision)
	}
	clockRegression := request
	clockRegression.NowProcessMonotonicTimeNS = 999
	if decision := edge.Evaluate(clockRegression); decision.AllowNewOrders || decision.ReasonCode != "MONOTONIC_CLOCK_REGRESSION" {
		t.Fatalf("clock-regression decision = %#v", decision)
	}
	kill, err := fixture.service.EmergencyKill(context.Background(), EmergencyKillRequest{Authorization: auth(now, "erin", "edge-kill-request"), CommandID: "edge-symbol-kill", Scope: KillScopeSymbol, TargetID: "XYZ", CommandSequence: 1, ReasonCode: "DATA_INVALID"})
	if err != nil {
		t.Fatal(err)
	}
	if err := edge.ApplyEmergencyKill(kill); err != nil {
		t.Fatal(err)
	}
	if decision := edge.Evaluate(request); decision.AllowNewOrders || decision.ReasonCode != "KILL_SWITCH_ENGAGED" {
		t.Fatalf("killed decision = %#v", decision)
	}
	reopened, err := OpenEdgeCache(EdgeCacheConfig{Root: edgeRoot, TrustedKeys: map[string]ed25519.PublicKey{"control-key": fixture.publicKey}, AuthorityEpoch: 7, ExpectedConfigurationID: "edge-prod", ExpectedEnvironment: "production-paper", StartupWallUTCNS: now.Add(time.Second).UnixNano(), StartupProcessMonotonicNS: 10})
	if err != nil {
		t.Fatal(err)
	}
	restartedRequest := request
	restartedRequest.NowWallUTCNS = now.Add(time.Second).UnixNano()
	restartedRequest.NowProcessMonotonicTimeNS = 10
	if decision := reopened.Evaluate(restartedRequest); decision.AllowNewOrders || decision.ReasonCode != "KILL_SWITCH_ENGAGED" {
		t.Fatalf("restarted kill decision = %#v", decision)
	}
	tampered := active
	tampered.SignatureBase64 = "invalid"
	if err := edge.InstallConfiguration(tampered, now.UnixNano(), 1_000); !errors.Is(err, ErrSignature) {
		t.Fatalf("tampered install error = %v", err)
	}
}

func TestAuditCorruptionBlocksStartup(t *testing.T) {
	fixture := newFixture(t)
	activateFirst(t, fixture)
	path := filepath.Join(fixture.root, "audit.jsonl")
	payload, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	payload[len(payload)/2] ^= 0x01
	if err := os.WriteFile(path, payload, 0o600); err != nil {
		t.Fatal(err)
	}
	_, err = Open(ServiceConfig{Root: fixture.root, TrustedKeys: map[string]ed25519.PublicKey{"control-key": fixture.publicKey}, BootstrapRoles: testRoles(), AuthorityEpoch: 7, Clock: func() time.Time { return *fixture.now }})
	if err == nil {
		t.Fatal("corrupted audit opened")
	}
}

func TestConcurrentAuthorizationHasSingleWinner(t *testing.T) {
	fixture := newFixture(t)
	now := *fixture.now
	signed := signedTestConfiguration(t, fixture, testConfiguration(now, 1, ""))
	request := ProposeRequest{Authorization: auth(now, "alice", "concurrent-request"), Configuration: signed, ActivationWallUTCNS: now.Add(time.Minute).UnixNano(), Reason: "concurrent replay defense"}
	var wait sync.WaitGroup
	wait.Add(2)
	errorsSeen := make(chan error, 2)
	for range 2 {
		go func() {
			defer wait.Done()
			_, err := fixture.service.Propose(context.Background(), request)
			errorsSeen <- err
		}()
	}
	wait.Wait()
	close(errorsSeen)
	successes := 0
	for err := range errorsSeen {
		if err == nil {
			successes++
		} else if !errors.Is(err, ErrReplay) {
			t.Fatalf("unexpected concurrent error: %v", err)
		}
	}
	if successes != 1 {
		t.Fatalf("successes = %d, want 1", successes)
	}
}

func TestCLIDryRunAndStrictInput(t *testing.T) {
	now := time.Date(2026, 9, 2, 14, 30, 0, 0, time.UTC)
	configuration := testConfiguration(now, 1, "")
	payload, _ := json.Marshal(configuration)
	var stdout, stderr bytes.Buffer
	if code := RunCLI(context.Background(), []string{"help"}, bytes.NewReader(nil), &stdout, &stderr); code != 0 || !bytes.Contains(stdout.Bytes(), []byte("emergency-kill")) {
		t.Fatalf("CLI help code=%d stdout=%s stderr=%s", code, stdout.String(), stderr.String())
	}
	stdout.Reset()
	stderr.Reset()
	if code := RunCLI(context.Background(), []string{"dry-run", "--config", "-"}, bytes.NewReader(payload), &stdout, &stderr); code != 0 {
		t.Fatalf("CLI code=%d stderr=%s", code, stderr.String())
	}
	var report ValidationReport
	if err := json.Unmarshal(stdout.Bytes(), &report); err != nil || !report.Valid || report.ConfigurationVersion == "" {
		t.Fatalf("CLI report=%#v err=%v", report, err)
	}
	stdout.Reset()
	stderr.Reset()
	malformed := append(payload[:len(payload)-1], []byte(`,"unknown":true}`)...)
	if code := RunCLI(context.Background(), []string{"dry-run", "--config", "-"}, bytes.NewReader(malformed), &stdout, &stderr); code == 0 {
		t.Fatal("strict CLI accepted unknown field")
	}
}

func TestDecodeSignedConfigurationRejectsUnknownTrailingAndOversizedInput(t *testing.T) {
	t.Parallel()
	fixture := newFixture(t)
	configuration, err := FinalizeConfiguration(testConfiguration(*fixture.now, 1, ""))
	if err != nil {
		t.Fatal(err)
	}
	signed, err := SignConfiguration(configuration, "control-key", fixture.privateKey)
	if err != nil {
		t.Fatal(err)
	}
	payload, err := json.Marshal(signed)
	if err != nil {
		t.Fatal(err)
	}
	decoded, err := DecodeSignedConfiguration(bytes.NewReader(payload))
	if err != nil || decoded.Configuration.SHA256 != configuration.SHA256 {
		t.Fatalf("valid signed configuration was not decoded: %v", err)
	}
	if _, err := DecodeSignedConfiguration(bytes.NewReader(append(payload, []byte("{}")...))); err == nil {
		t.Fatal("trailing JSON object was accepted")
	}
	if _, err := DecodeSignedConfiguration(bytes.NewReader(bytes.Repeat([]byte{'x'}, int(maximumConfigurationBytes+1)))); err == nil {
		t.Fatal("oversized configuration was accepted")
	}
}

func BenchmarkEdgeEvaluate(b *testing.B) {
	seed := sha256.Sum256([]byte(controlTestSeed))
	privateKey := ed25519.NewKeyFromSeed(seed[:])
	publicKey := privateKey.Public().(ed25519.PublicKey)
	now := time.Date(2026, 9, 2, 14, 30, 0, 0, time.UTC)
	configuration, err := FinalizeConfiguration(testConfiguration(now, 1, ""))
	if err != nil {
		b.Fatal(err)
	}
	signed, err := SignConfiguration(configuration, "control-key", privateKey)
	if err != nil {
		b.Fatal(err)
	}
	edge, err := OpenEdgeCache(EdgeCacheConfig{Root: b.TempDir(), TrustedKeys: map[string]ed25519.PublicKey{"control-key": publicKey}, AuthorityEpoch: 7, ExpectedConfigurationID: "edge-prod", ExpectedEnvironment: "production-paper", StartupWallUTCNS: now.UnixNano(), StartupProcessMonotonicNS: 100})
	if err != nil {
		b.Fatal(err)
	}
	if err := edge.InstallConfiguration(signed, now.UnixNano(), 100); err != nil {
		b.Fatal(err)
	}
	request := EdgeRequest{NowWallUTCNS: now.UnixNano(), NowProcessMonotonicTimeNS: 100, AccountID: "paper-account", InstrumentID: "XYZ", StrategyID: "alpha", VenueID: "SYNTH", SessionID: "regular", ModelID: "micro", ModelVersion: "1.2.3"}
	b.ReportAllocs()
	b.ResetTimer()
	for range b.N {
		if !edge.Evaluate(request).AllowNewOrders {
			b.Fatal("unexpected denial")
		}
	}
}
