package modelregistry

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"errors"
	"os"
	"strings"
	"sync"
	"testing"
)

type memoryDeploymentAudit struct {
	mu      sync.Mutex
	records []DeploymentAuditRecord
	fail    bool
}

func (sink *memoryDeploymentAudit) Append(record DeploymentAuditRecord) error {
	sink.mu.Lock()
	defer sink.mu.Unlock()
	if sink.fail {
		return errors.New("injected deployment audit failure")
	}
	if err := validateDeploymentAuditRecord(record); err != nil {
		return err
	}
	sink.records = append(sink.records, record)
	return nil
}

func (sink *memoryDeploymentAudit) snapshot() []DeploymentAuditRecord {
	sink.mu.Lock()
	defer sink.mu.Unlock()
	return append([]DeploymentAuditRecord(nil), sink.records...)
}

type deploymentFixture struct {
	registryFixture *registryFixture
	coordinator     *DeploymentCoordinator
	audit           *memoryDeploymentAudit
	signedConfig    SignedDeploymentConfig
	production      SignedManifest
}

func newDeploymentFixture(t testing.TB, minimumSamples uint64, regimes []MarketRegime) *deploymentFixture {
	t.Helper()
	fixture := newRegistryFixture(t)
	production := fixture.register("1.0.0", "", []byte("production-model"))
	fixture.validateAndApprove("1.0.0")
	fixture.deployProduction("1.0.0")
	fixture.register("1.1.0", "1.0.0", []byte("candidate-model"))
	fixture.validateAndApprove("1.1.0")
	if _, err := fixture.registry.DeployShadow(context.Background(), DeploymentRequest{
		ModelID: "mid-direction", SemanticVersion: "1.1.0", ExpectedFeatureSchema: fixture.schema,
		Environment: "shadow-candidate", Actor: "shadow-deployer", Reason: "paired shadow validation",
	}); err != nil {
		t.Fatalf("deploy candidate shadow: %v", err)
	}
	thresholds := make([]RollbackThreshold, 0, len(allRollbackMetrics))
	for _, metric := range allRollbackMetrics {
		thresholds = append(thresholds, RollbackThreshold{Metric: metric, MinimumSamples: minimumSamples, MaximumRegression: 1_000})
	}
	config, err := FinalizeDeploymentConfig(DeploymentConfig{
		SchemaVersion: deploymentSchemaVersion, DeploymentID: "deploy-mid-110", ModelID: "mid-direction",
		CandidateVersion: "1.1.0", ProductionVersion: "1.0.0", ShadowEnvironment: "shadow-candidate",
		CanaryEnvironment: "canary-candidate", FeatureSchema: fixture.schema, RequiredRegimes: regimes,
		RollbackThresholds: thresholds, AllowedInstrumentIDs: []string{"instrument-1", "instrument-2"},
		AllowedStrategyIDs: []string{"strategy-1"}, MaximumCapitalCurrencyNanos: 10_000,
		OrderRateWindowNS: 100, MaximumOrdersPerWindow: 2, MaximumRiskSnapshotAgeNS: 50,
		ConfidenceZPPM: 1_960_000, ApprovedBy: "deployment-approver", ApprovalReference: "change-deploy-110",
		AutomaticRollbackActor: "model-rollback-controller", CreatedAtUTCNS: 1_800_000_000_000_000_000,
	})
	if err != nil {
		t.Fatalf("finalize deployment config: %v", err)
	}
	signed, err := SignDeploymentConfig(config, "test-signer", fixture.privateKey)
	if err != nil {
		t.Fatalf("sign deployment config: %v", err)
	}
	audit := &memoryDeploymentAudit{}
	coordinator, err := NewDeploymentCoordinator(DeploymentCoordinatorOptions{
		Registry: fixture.registry, Config: signed,
		TrustedKeys: map[string]ed25519.PublicKey{"test-signer": fixture.publicKey},
		AuditSink:   audit, Clock: fixture.clock.Now,
	})
	if err != nil {
		t.Fatalf("open deployment coordinator: %v", err)
	}
	return &deploymentFixture{registryFixture: fixture, coordinator: coordinator, audit: audit, signedConfig: signed, production: production}
}

func pairedObservation(sequence uint64, regime MarketRegime, metric RollbackMetric, delta int64) PairedObservation {
	metrics := ForecastMetrics{
		LatencyNS: 10_000, CalibrationErrorPPM: 10_000, FeatureDriftPPM: 10_000,
		OODScorePPM: 10_000, TradeRatePPM: 10_000, ModelDisagreementPPM: 10_000,
		ImplementationShortfallPPM: 10_000, PnLAttributionAnomalyPPM: 10_000, RiskLimitPressurePPM: 10_000,
	}
	production := ForecastSample{
		ForecastID: "production-forecast", ModelVersion: "1.0.0", FeatureSnapshotID: "snapshot-shared",
		FeatureSnapshotSHA256: digestText("shared-feature-snapshot"), AsOfExchangeEventTimeNS: 1_700_000_000_000_000_000,
		CompletedProcessMonotonicTimeNS: 1_000, DeadlineProcessMonotonicTimeNS: 2_000,
		Action: DecisionAbstain, Metrics: metrics,
	}
	candidate := production
	candidate.ForecastID = "candidate-forecast"
	candidate.ModelVersion = "1.1.0"
	candidate.Action = DecisionBuy
	candidate.NetRobustEdgePPM = 250
	applyMetricDelta(&candidate, metric, delta)
	observed := uint64(3_000)
	if candidate.CompletedProcessMonotonicTimeNS > observed {
		observed = candidate.CompletedProcessMonotonicTimeNS
	}
	return PairedObservation{
		Sequence: sequence, InstrumentID: "instrument-1", StrategyID: "strategy-1", Regime: regime,
		ObservedProcessMonotonicTimeNS: observed, Production: production, Candidate: candidate,
	}
}

func applyMetricDelta(candidate *ForecastSample, metric RollbackMetric, delta int64) {
	switch metric {
	case MetricLatencyRegression:
		candidate.Metrics.LatencyNS += delta
	case MetricDeadlineMisses:
		if delta != 0 {
			candidate.CompletedProcessMonotonicTimeNS = candidate.DeadlineProcessMonotonicTimeNS + 1
		}
	case MetricCalibrationDeterioration:
		candidate.Metrics.CalibrationErrorPPM += delta
	case MetricFeatureDrift:
		candidate.Metrics.FeatureDriftPPM += delta
	case MetricOODIncrease:
		candidate.Metrics.OODScorePPM += delta
	case MetricAbnormalTradeRate:
		candidate.Metrics.TradeRatePPM += delta
	case MetricUnexpectedDisagreement:
		candidate.Metrics.ModelDisagreementPPM += delta
	case MetricImplementationShortfallDeterioration:
		candidate.Metrics.ImplementationShortfallPPM += delta
	case MetricPnLAttributionAnomaly:
		candidate.Metrics.PnLAttributionAnomalyPPM += delta
	case MetricRiskLimitPressure:
		candidate.Metrics.RiskLimitPressurePPM += delta
	}
}

func promoteDeploymentCanary(t testing.TB, fixture *deploymentFixture) {
	t.Helper()
	for sequence := uint64(1); sequence <= 3; sequence++ {
		if _, err := fixture.coordinator.Observe(context.Background(), pairedObservation(sequence, RegimeNormal, "", 0)); err != nil {
			t.Fatalf("safe shadow observation %d: %v", sequence, err)
		}
	}
	if _, err := fixture.coordinator.ApproveCanary(context.Background(), CanaryApproval{
		Actor: "canary-operator", Reason: "regime evidence reviewed", ApprovalReference: "canary-change-110",
		ConfigurationSHA256: fixture.signedConfig.Config.ConfigurationSHA256,
	}); err != nil {
		t.Fatalf("approve canary: %v", err)
	}
}

func TestSignedDeploymentConfigurationIsCanonicalAndTamperEvident(t *testing.T) {
	fixture := newDeploymentFixture(t, 3, []MarketRegime{RegimeNormal})
	if err := VerifySignedDeploymentConfig(fixture.signedConfig, map[string]ed25519.PublicKey{"test-signer": fixture.registryFixture.publicKey}); err != nil {
		t.Fatalf("verify signed deployment config: %v", err)
	}
	tampered := fixture.signedConfig
	tampered.Config.MaximumCapitalCurrencyNanos++
	if err := VerifySignedDeploymentConfig(tampered, map[string]ed25519.PublicKey{"test-signer": fixture.registryFixture.publicKey}); err == nil {
		t.Fatal("tampered deployment config was accepted")
	}
	tampered = fixture.signedConfig
	tampered.Config.AllowedInstrumentIDs = []string{"instrument-2", "instrument-1"}
	if err := VerifySignedDeploymentConfig(tampered, map[string]ed25519.PublicKey{"test-signer": fixture.registryFixture.publicKey}); err == nil {
		t.Fatal("noncanonical deployment config was accepted")
	}
}

func TestShadowPairsIdenticalSnapshotsAndRequiresEveryRegime(t *testing.T) {
	fixture := newDeploymentFixture(t, 3, []MarketRegime{RegimeNormal, RegimeBreakingNews})
	sequence := uint64(0)
	for _, regime := range []MarketRegime{RegimeNormal, RegimeBreakingNews} {
		for range 3 {
			sequence++
			outcome, err := fixture.coordinator.Observe(context.Background(), pairedObservation(sequence, regime, "", 0))
			if err != nil {
				t.Fatalf("observe %s: %v", regime, err)
			}
			if outcome.Hypothetical.Executable || !outcome.Hypothetical.DownstreamRiskRequired || outcome.Hypothetical.DecisionSHA256 == "" {
				t.Fatalf("shadow decision was executable or unaudited: %+v", outcome.Hypothetical)
			}
		}
	}
	readiness, err := fixture.coordinator.Readiness()
	if err != nil || !readiness.ReadyForApproval {
		t.Fatalf("expected explicit-approval readiness: %+v err=%v", readiness, err)
	}
	status, err := fixture.coordinator.ApproveCanary(context.Background(), CanaryApproval{
		Actor: "canary-operator", Reason: "all regime evidence reviewed", ApprovalReference: "canary-change-110",
		ConfigurationSHA256: fixture.signedConfig.Config.ConfigurationSHA256,
	})
	if err != nil || status.Lifecycle != LifecycleCanary || fixture.coordinator.State() != RunStateCanary {
		t.Fatalf("canary approval failed closed unexpectedly: %+v err=%v", status, err)
	}
	records := fixture.audit.snapshot()
	if len(records) != 8 || records[0].HypotheticalDecision == nil || records[0].HypotheticalDecision.Executable {
		t.Fatalf("unexpected deployment audit: %+v", records)
	}
	lineage, err := fixture.registryFixture.registry.InspectLineage(context.Background(), "mid-direction", "1.1.0")
	if err != nil {
		t.Fatalf("inspect candidate lineage: %v", err)
	}
	events := lineage.Entries[0].Events
	if events[len(events)-1].Body.AuthorizationReference != "canary-change-110" {
		t.Fatal("registry lifecycle omitted explicit canary approval reference")
	}
}

func TestShadowRejectsMismatchedFeatureSnapshotsAndCannotPromoteOnPartialEvidence(t *testing.T) {
	fixture := newDeploymentFixture(t, 3, []MarketRegime{RegimeNormal, RegimeBreakingNews})
	observation := pairedObservation(1, RegimeNormal, "", 0)
	observation.Candidate.FeatureSnapshotSHA256 = digestText("different-snapshot")
	_, err := fixture.coordinator.Observe(context.Background(), observation)
	requireErrorIs(t, err, ErrInvalidObservation)
	if fixture.coordinator.State() != RunStatePromotionBlocked {
		t.Fatalf("mismatched snapshot did not block promotion: %s", fixture.coordinator.State())
	}

	partial := newDeploymentFixture(t, 3, []MarketRegime{RegimeNormal, RegimeBreakingNews})
	for sequence := uint64(1); sequence <= 3; sequence++ {
		if _, err := partial.coordinator.Observe(context.Background(), pairedObservation(sequence, RegimeNormal, MetricPnLAttributionAnomaly, 0)); err != nil {
			t.Fatalf("normal observation: %v", err)
		}
	}
	readiness, err := partial.coordinator.Readiness()
	if err != nil || readiness.ReadyForApproval || len(readiness.MissingSamples[RegimeBreakingNews]) != len(allRollbackMetrics) {
		t.Fatalf("aggregate-only evidence was accepted: %+v err=%v", readiness, err)
	}
	_, err = partial.coordinator.ApproveCanary(context.Background(), CanaryApproval{
		Actor: "canary-operator", Reason: "attempt partial promotion", ApprovalReference: "partial-change",
		ConfigurationSHA256: partial.signedConfig.Config.ConfigurationSHA256,
	})
	requireErrorIs(t, err, ErrPromotionNotReady)
}

func TestEveryConfiguredMetricCanAutomaticallyRollbackCanary(t *testing.T) {
	for _, metric := range allRollbackMetrics {
		t.Run(string(metric), func(t *testing.T) {
			fixture := newDeploymentFixture(t, 3, []MarketRegime{RegimeNormal})
			promoteDeploymentCanary(t, fixture)
			delta := int64(100_000)
			if metric == MetricDeadlineMisses {
				delta = 1
			}
			outcome, err := fixture.coordinator.Observe(context.Background(), pairedObservation(4, RegimeNormal, metric, delta))
			requireErrorIs(t, err, ErrAutomaticRollback)
			if outcome.Trigger == nil || outcome.Trigger.Metric != metric || outcome.State != RunStateRolledBack {
				t.Fatalf("wrong rollback trigger: %+v", outcome)
			}
			lineage, inspectErr := fixture.registryFixture.registry.InspectLineage(context.Background(), "mid-direction", "1.1.0")
			if inspectErr != nil || lineage.Entries[0].Status.Lifecycle != LifecycleRolledBack {
				t.Fatalf("candidate was not rolled back: %+v err=%v", lineage, inspectErr)
			}
		})
	}
}

func TestMinimumSamplesPreventEarlyRollback(t *testing.T) {
	fixture := newDeploymentFixture(t, 3, []MarketRegime{RegimeNormal})
	for sequence := uint64(1); sequence <= 2; sequence++ {
		outcome, err := fixture.coordinator.Observe(context.Background(), pairedObservation(sequence, RegimeNormal, MetricLatencyRegression, 100_000))
		if err != nil || outcome.Trigger != nil || outcome.State != RunStateShadow {
			t.Fatalf("rollback triggered below minimum sample size: %+v err=%v", outcome, err)
		}
	}
	_, err := fixture.coordinator.Observe(context.Background(), pairedObservation(3, RegimeNormal, MetricLatencyRegression, 100_000))
	requireErrorIs(t, err, ErrPromotionNotReady)
	if fixture.coordinator.State() != RunStatePromotionBlocked {
		t.Fatalf("threshold breach did not block shadow promotion: %s", fixture.coordinator.State())
	}
}

func TestCanaryScopeEnforcesSymbolStrategyCapitalRateAndFreshRisk(t *testing.T) {
	fixture := newDeploymentFixture(t, 3, []MarketRegime{RegimeNormal})
	promoteDeploymentCanary(t, fixture)
	base := CanaryScopeRequest{
		Sequence: 1, InstrumentID: "instrument-1", StrategyID: "strategy-1", RiskSnapshotID: "risk-snapshot-1",
		RiskSnapshotSHA256: digestText("risk-snapshot"), NowProcessMonotonicTimeNS: 1_000,
		RiskSnapshotObservedMonotonicNS: 975, CurrentGrossCapitalNanos: 1_000, RequestedCapitalNanos: 500,
	}
	decision, err := fixture.coordinator.AdmitCanaryScope(context.Background(), base)
	if err != nil || !decision.Admitted || decision.OrderExecutable || !decision.DownstreamRiskRequired {
		t.Fatalf("bounded canary request was not admitted: %+v err=%v", decision, err)
	}
	tests := []struct {
		name   string
		modify func(*CanaryScopeRequest)
		reason CanaryScopeReason
	}{
		{name: "symbol", modify: func(request *CanaryScopeRequest) { request.InstrumentID = "instrument-9" }, reason: CanaryScopeSymbolDenied},
		{name: "strategy", modify: func(request *CanaryScopeRequest) { request.StrategyID = "strategy-9" }, reason: CanaryScopeStrategyDenied},
		{name: "capital", modify: func(request *CanaryScopeRequest) {
			request.CurrentGrossCapitalNanos = 9_900
			request.RequestedCapitalNanos = 101
		}, reason: CanaryScopeCapitalExceeded},
		{name: "stale-risk", modify: func(request *CanaryScopeRequest) { request.RiskSnapshotObservedMonotonicNS = 900 }, reason: CanaryScopeRiskSnapshotStale},
	}
	sequence := uint64(1)
	for _, test := range tests {
		sequence++
		request := base
		request.Sequence = sequence
		request.NowProcessMonotonicTimeNS = 1_000 + sequence
		test.modify(&request)
		decision, err := fixture.coordinator.AdmitCanaryScope(context.Background(), request)
		if err != nil || decision.Admitted || decision.Reason != test.reason || decision.OrderExecutable {
			t.Fatalf("%s scope result: %+v err=%v", test.name, decision, err)
		}
	}
	malformed := base
	malformed.Sequence = 0
	if decision, err := fixture.coordinator.AdmitCanaryScope(context.Background(), malformed); err != nil || decision.Reason != CanaryScopeInvalidRequest {
		t.Fatalf("malformed scope result: %+v err=%v", decision, err)
	}
	replayed := base
	replayed.Sequence = 2
	if decision, err := fixture.coordinator.AdmitCanaryScope(context.Background(), replayed); err != nil || decision.Reason != CanaryScopeInvalidRequest {
		t.Fatalf("replay protection regressed: %+v err=%v", decision, err)
	}

	second := base
	second.Sequence = sequence + 1
	second.NowProcessMonotonicTimeNS = 1_010
	if decision, err := fixture.coordinator.AdmitCanaryScope(context.Background(), second); err != nil || !decision.Admitted {
		t.Fatalf("second bounded order-rate request: %+v err=%v", decision, err)
	}
	third := second
	third.Sequence++
	third.NowProcessMonotonicTimeNS++
	if decision, err := fixture.coordinator.AdmitCanaryScope(context.Background(), third); err != nil || decision.Reason != CanaryScopeOrderRateExceeded {
		t.Fatalf("order-rate limit did not reject: %+v err=%v", decision, err)
	}
}

func TestInvalidCanaryObservationAndAuditFailureFailClosed(t *testing.T) {
	fixture := newDeploymentFixture(t, 3, []MarketRegime{RegimeNormal})
	promoteDeploymentCanary(t, fixture)
	invalid := pairedObservation(4, RegimeNormal, "", 0)
	invalid.Candidate.FeatureSnapshotID = "different-snapshot"
	_, err := fixture.coordinator.Observe(context.Background(), invalid)
	requireErrorIs(t, err, ErrInvalidObservation)
	if fixture.coordinator.State() != RunStateRolledBack {
		t.Fatalf("invalid canary observation did not rollback: %s", fixture.coordinator.State())
	}

	auditFailure := newDeploymentFixture(t, 3, []MarketRegime{RegimeNormal})
	promoteDeploymentCanary(t, auditFailure)
	auditFailure.audit.mu.Lock()
	auditFailure.audit.fail = true
	auditFailure.audit.mu.Unlock()
	request := CanaryScopeRequest{
		Sequence: 1, InstrumentID: "instrument-1", StrategyID: "strategy-1", RiskSnapshotID: "risk-snapshot-1",
		RiskSnapshotSHA256: digestText("risk"), NowProcessMonotonicTimeNS: 100,
		RiskSnapshotObservedMonotonicNS: 90, CurrentGrossCapitalNanos: 1, RequestedCapitalNanos: 1,
	}
	decision, err := auditFailure.coordinator.AdmitCanaryScope(context.Background(), request)
	if err == nil || decision.Admitted || auditFailure.coordinator.State() != RunStateRolledBack {
		t.Fatalf("audit failure did not fail closed: %+v state=%s err=%v", decision, auditFailure.coordinator.State(), err)
	}
}

func TestRollbackFailureDisablesCandidate(t *testing.T) {
	fixture := newDeploymentFixture(t, 3, []MarketRegime{RegimeNormal})
	promoteDeploymentCanary(t, fixture)
	artifactPath := fixture.registryFixture.registry.artifactPath(fixture.production.Manifest.ArtifactSHA256)
	if err := os.Chmod(artifactPath, 0o600); err != nil {
		t.Fatalf("make production artifact writable: %v", err)
	}
	if err := os.WriteFile(artifactPath, []byte("tampered-production"), 0o600); err != nil {
		t.Fatalf("tamper rollback target: %v", err)
	}
	_, err := fixture.coordinator.Observe(context.Background(), pairedObservation(4, RegimeNormal, MetricOODIncrease, 100_000))
	requireErrorIs(t, err, ErrAutomaticRollback)
	if fixture.coordinator.State() != RunStateDisabledFailClosed {
		t.Fatalf("rollback failure did not disable candidate: %s", fixture.coordinator.State())
	}
	events, inspectErr := fixture.registryFixture.registry.loadEvents("mid-direction", "1.1.0")
	status, statusErr := fixture.registryFixture.registry.statusFromEvents(events)
	if inspectErr != nil || statusErr != nil || status.Lifecycle != LifecycleDisabled {
		t.Fatalf("candidate disable was not durable: %+v load_err=%v status_err=%v", status, inspectErr, statusErr)
	}
}

func TestRollbackAndDisableFailureAreReportedWithoutFalseDisableClaim(t *testing.T) {
	fixture := newDeploymentFixture(t, 3, []MarketRegime{RegimeNormal})
	promoteDeploymentCanary(t, fixture)
	eventPath := fixture.registryFixture.registry.eventPath("mid-direction", "1.1.0")
	payload, err := os.ReadFile(eventPath)
	if err != nil {
		t.Fatalf("read candidate events: %v", err)
	}
	tampered := bytes.Replace(payload, []byte(`"actor":"trainer"`), []byte(`"actor":"intruder"`), 1)
	if bytes.Equal(payload, tampered) {
		t.Fatal("candidate event tamper fixture did not match")
	}
	if err := os.WriteFile(eventPath, tampered, 0o600); err != nil {
		t.Fatalf("tamper candidate events: %v", err)
	}
	_, err = fixture.coordinator.Observe(context.Background(), pairedObservation(4, RegimeNormal, MetricOODIncrease, 100_000))
	requireErrorIs(t, err, ErrAutomaticRollback)
	if fixture.coordinator.State() != RunStateDisableFailed {
		t.Fatalf("double registry failure was misreported: %s", fixture.coordinator.State())
	}
	decision, scopeErr := fixture.coordinator.AdmitCanaryScope(context.Background(), CanaryScopeRequest{})
	if !errors.Is(scopeErr, ErrDeploymentBlocked) || decision.Admitted {
		t.Fatalf("locally fail-closed coordinator admitted scope: %+v err=%v", decision, scopeErr)
	}
	records := fixture.audit.snapshot()
	if records[len(records)-1].Kind != AuditFailClosedDisableFailed {
		t.Fatalf("double failure was not audited explicitly: %+v", records[len(records)-1])
	}
}

func TestDeploymentAuditDetectsTampering(t *testing.T) {
	fixture := newDeploymentFixture(t, 3, []MarketRegime{RegimeNormal})
	path := t.TempDir() + "/deployment-audit.jsonl"
	sink, err := OpenFileDeploymentAuditSink(path)
	if err != nil {
		t.Fatalf("open deployment audit: %v", err)
	}
	coordinator, err := NewDeploymentCoordinator(DeploymentCoordinatorOptions{
		Registry: fixture.registryFixture.registry, Config: fixture.signedConfig,
		TrustedKeys: map[string]ed25519.PublicKey{"test-signer": fixture.registryFixture.publicKey},
		AuditSink:   sink, Clock: fixture.registryFixture.clock.Now,
	})
	if err != nil {
		t.Fatalf("open coordinator with file audit: %v", err)
	}
	if _, err := coordinator.Observe(context.Background(), pairedObservation(1, RegimeNormal, "", 0)); err != nil {
		t.Fatalf("journal observation: %v", err)
	}
	envelopes, err := InspectDeploymentAudit(path)
	if err != nil || len(envelopes) != 1 {
		t.Fatalf("inspect deployment audit: count=%d err=%v", len(envelopes), err)
	}
	payload, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read deployment audit: %v", err)
	}
	payload = bytes.Replace(payload, []byte(`"candidate_action":"BUY"`), []byte(`"candidate_action":"SELL"`), 1)
	if err := os.WriteFile(path, payload, 0o600); err != nil {
		t.Fatalf("tamper deployment audit: %v", err)
	}
	if _, err := InspectDeploymentAudit(path); !errors.Is(err, ErrRegistryIntegrity) {
		t.Fatalf("tampered audit was not rejected: %v", err)
	}
}

func TestDeploymentCoordinatorRestoresCanaryEvidenceAndRateState(t *testing.T) {
	fixture := newDeploymentFixture(t, 3, []MarketRegime{RegimeNormal})
	path := t.TempDir() + "/deployment-recovery.jsonl"
	sink, err := OpenFileDeploymentAuditSink(path)
	if err != nil {
		t.Fatalf("open deployment audit: %v", err)
	}
	coordinator, err := NewDeploymentCoordinator(DeploymentCoordinatorOptions{
		Registry: fixture.registryFixture.registry, Config: fixture.signedConfig,
		TrustedKeys: map[string]ed25519.PublicKey{"test-signer": fixture.registryFixture.publicKey},
		AuditSink:   sink, Clock: fixture.registryFixture.clock.Now,
	})
	if err != nil {
		t.Fatalf("open deployment coordinator: %v", err)
	}
	for sequence := uint64(1); sequence <= 3; sequence++ {
		if _, err := coordinator.Observe(context.Background(), pairedObservation(sequence, RegimeNormal, "", 0)); err != nil {
			t.Fatalf("shadow observation: %v", err)
		}
	}
	if _, err := coordinator.ApproveCanary(context.Background(), CanaryApproval{
		Actor: "canary-operator", Reason: "restore fixture approval", ApprovalReference: "canary-restore-110",
		ConfigurationSHA256: fixture.signedConfig.Config.ConfigurationSHA256,
	}); err != nil {
		t.Fatalf("approve canary: %v", err)
	}
	request := CanaryScopeRequest{
		Sequence: 1, InstrumentID: "instrument-1", StrategyID: "strategy-1", RiskSnapshotID: "risk-snapshot-1",
		RiskSnapshotSHA256: digestText("risk-restore"), NowProcessMonotonicTimeNS: 1_000,
		RiskSnapshotObservedMonotonicNS: 975, CurrentGrossCapitalNanos: 1, RequestedCapitalNanos: 1,
	}
	if decision, err := coordinator.AdmitCanaryScope(context.Background(), request); err != nil || !decision.Admitted {
		t.Fatalf("initial canary scope: %+v err=%v", decision, err)
	}

	reopenedSink, err := OpenFileDeploymentAuditSink(path)
	if err != nil {
		t.Fatalf("reopen deployment audit: %v", err)
	}
	recovered, err := NewDeploymentCoordinator(DeploymentCoordinatorOptions{
		Registry: fixture.registryFixture.registry, Config: fixture.signedConfig,
		TrustedKeys: map[string]ed25519.PublicKey{"test-signer": fixture.registryFixture.publicKey},
		AuditSink:   reopenedSink, Clock: fixture.registryFixture.clock.Now,
	})
	if err != nil {
		t.Fatalf("recover coordinator: %v", err)
	}
	snapshot, err := recovered.MetricsSnapshot()
	if err != nil || snapshot.State != RunStateCanary || snapshot.ObservationCount != 3 {
		t.Fatalf("recovered evidence mismatch: %+v err=%v", snapshot, err)
	}
	if _, err := recovered.Observe(context.Background(), pairedObservation(4, RegimeNormal, "", 0)); err != nil {
		t.Fatalf("observation sequence did not recover: %v", err)
	}
	second := request
	second.Sequence = 2
	second.NowProcessMonotonicTimeNS = 1_001
	if decision, err := recovered.AdmitCanaryScope(context.Background(), second); err != nil || !decision.Admitted {
		t.Fatalf("second recovered scope: %+v err=%v", decision, err)
	}
	third := second
	third.Sequence = 3
	third.NowProcessMonotonicTimeNS = 1_002
	if decision, err := recovered.AdmitCanaryScope(context.Background(), third); err != nil || decision.Reason != CanaryScopeOrderRateExceeded {
		t.Fatalf("recovered rate state did not reject: %+v err=%v", decision, err)
	}
}

func TestDeploymentMetricsExposeRegimeBoundsAndState(t *testing.T) {
	fixture := newDeploymentFixture(t, 3, []MarketRegime{RegimeNormal})
	if _, err := fixture.coordinator.Observe(context.Background(), pairedObservation(1, RegimeNormal, "", 0)); err != nil {
		t.Fatalf("observe: %v", err)
	}
	request := CanaryScopeRequest{}
	if _, err := fixture.coordinator.AdmitCanaryScope(context.Background(), request); !errors.Is(err, ErrDeploymentBlocked) {
		t.Fatalf("shadow scope was not blocked: %v", err)
	}
	var output strings.Builder
	if err := fixture.coordinator.WritePrometheus(&output); err != nil {
		t.Fatalf("write metrics: %v", err)
	}
	for _, expected := range []string{
		`state="SHADOW"`, `aegis_model_deployment_observations_total`,
		`regime="NORMAL"`, `metric="latency_regression_ns"`,
		`aegis_model_canary_rejections_total`, `reason="NOT_CANARY"`,
	} {
		if !strings.Contains(output.String(), expected) {
			t.Fatalf("metrics omitted %q:\n%s", expected, output.String())
		}
	}
}

func TestDeterministicPairedConfidenceInterval(t *testing.T) {
	accumulator := &metricAccumulator{}
	for _, value := range []int64{0, 10} {
		accumulator.add(value)
	}
	interval, err := accumulator.interval(MetricLatencyRegression, RollbackThreshold{
		Metric: MetricLatencyRegression, MinimumSamples: 2, MaximumRegression: 10,
	}, 1_000_000)
	if err != nil {
		t.Fatalf("confidence interval: %v", err)
	}
	if interval.MeanDelta != 5 || interval.HalfWidth != 5 || interval.LowerBound != 0 || interval.UpperBound != 10 || interval.Triggered {
		t.Fatalf("unexpected deterministic interval: %+v", interval)
	}
}

type discardDeploymentAudit struct{}

func (discardDeploymentAudit) Append(record DeploymentAuditRecord) error {
	return validateDeploymentAuditRecord(record)
}

func BenchmarkDeploymentObservation(b *testing.B) {
	fixture := newDeploymentFixture(b, 1_000_000, []MarketRegime{RegimeNormal})
	fixture.coordinator.audit = discardDeploymentAudit{}
	observation := pairedObservation(1, RegimeNormal, "", 0)
	b.ReportAllocs()
	b.ResetTimer()
	for index := 0; index < b.N; index++ {
		observation.Sequence = uint64(index + 1)
		if _, err := fixture.coordinator.Observe(context.Background(), observation); err != nil {
			b.Fatalf("deployment observation: %v", err)
		}
	}
}
