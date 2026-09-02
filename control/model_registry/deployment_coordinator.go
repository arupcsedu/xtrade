package modelregistry

import (
	"context"
	"crypto/ed25519"
	"errors"
	"fmt"
	"sort"
	"strings"
	"sync"
	"time"
)

var (
	ErrDeploymentBlocked  = errors.New("model deployment is fail-closed")
	ErrInvalidObservation = errors.New("invalid paired model observation")
	ErrPromotionNotReady  = errors.New("shadow evidence is not ready for canary approval")
	ErrAutomaticRollback  = errors.New("automatic model rollback triggered")
)

// DeploymentCoordinatorOptions binds a signed configuration to a registry and
// durable off-path audit sink.
type DeploymentCoordinatorOptions struct {
	Registry    *Registry
	Config      SignedDeploymentConfig
	TrustedKeys map[string]ed25519.PublicKey
	AuditSink   DeploymentAuditSink
	Clock       Clock
}

// DeploymentCoordinator compares paired forecasts and controls only model
// lifecycle/scope. It cannot construct or emit an executable order.
type DeploymentCoordinator struct {
	mu                      sync.Mutex
	registry                *Registry
	config                  DeploymentConfig
	audit                   DeploymentAuditSink
	clock                   Clock
	state                   DeploymentRunState
	statistics              map[MarketRegime]map[RollbackMetric]*metricAccumulator
	thresholds              map[RollbackMetric]RollbackThreshold
	allowedRegimes          map[MarketRegime]struct{}
	allowedInstruments      map[string]struct{}
	allowedStrategies       map[string]struct{}
	lastObservationSequence uint64
	lastScopeSequence       uint64
	lastScopeTimeNS         uint64
	orderTimes              []uint64
	observationCount        uint64
	rollbackCount           uint64
	canaryRejections        map[CanaryScopeReason]uint64
	lastTrigger             *RollbackTrigger
}

// NewDeploymentCoordinator verifies signatures, artifacts, exact feature
// compatibility, registry lifecycle, and active environment before starting.
func NewDeploymentCoordinator(options DeploymentCoordinatorOptions) (*DeploymentCoordinator, error) {
	if options.Registry == nil || options.AuditSink == nil {
		return nil, errors.New("registry and deployment audit sink are required")
	}
	if err := VerifySignedDeploymentConfig(options.Config, options.TrustedKeys); err != nil {
		return nil, err
	}
	config := options.Config.Config
	_, _, candidateStatus, err := options.Registry.loadVersion(config.ModelID, config.CandidateVersion, &config.FeatureSchema)
	if err != nil {
		return nil, fmt.Errorf("candidate version: %w", err)
	}
	_, _, productionStatus, err := options.Registry.loadVersion(config.ModelID, config.ProductionVersion, &config.FeatureSchema)
	if err != nil {
		return nil, fmt.Errorf("production version: %w", err)
	}
	if productionStatus.Approval != ApprovalApproved || productionStatus.Lifecycle != LifecycleProduction {
		return nil, errors.New("comparison version must be approved and in PRODUCTION lifecycle")
	}
	state := RunStateShadow
	requiredEnvironment := config.ShadowEnvironment
	if candidateStatus.Lifecycle == LifecycleCanary {
		state = RunStateCanary
		requiredEnvironment = config.CanaryEnvironment
	} else if candidateStatus.Lifecycle != LifecycleShadow {
		return nil, fmt.Errorf("candidate lifecycle must be SHADOW or CANARY, got %s", candidateStatus.Lifecycle)
	}
	if !containsString(candidateStatus.ActiveEnvironments, requiredEnvironment) {
		return nil, fmt.Errorf("candidate is not active in configured environment %q", requiredEnvironment)
	}
	clock := options.Clock
	if clock == nil {
		clock = time.Now
	}
	coordinator := &DeploymentCoordinator{
		registry: options.Registry, config: config, audit: options.AuditSink, clock: clock,
		state:              state,
		statistics:         make(map[MarketRegime]map[RollbackMetric]*metricAccumulator, len(config.RequiredRegimes)),
		thresholds:         make(map[RollbackMetric]RollbackThreshold, len(config.RollbackThresholds)),
		allowedRegimes:     make(map[MarketRegime]struct{}, len(config.RequiredRegimes)),
		allowedInstruments: make(map[string]struct{}, len(config.AllowedInstrumentIDs)),
		allowedStrategies:  make(map[string]struct{}, len(config.AllowedStrategyIDs)),
		orderTimes:         make([]uint64, 0, config.MaximumOrdersPerWindow),
		canaryRejections:   make(map[CanaryScopeReason]uint64),
	}
	for _, threshold := range config.RollbackThresholds {
		coordinator.thresholds[threshold.Metric] = threshold
	}
	for _, regime := range config.RequiredRegimes {
		coordinator.allowedRegimes[regime] = struct{}{}
		coordinator.statistics[regime] = make(map[RollbackMetric]*metricAccumulator, len(allRollbackMetrics))
		for _, metric := range allRollbackMetrics {
			coordinator.statistics[regime][metric] = &metricAccumulator{}
		}
	}
	for _, instrumentID := range config.AllowedInstrumentIDs {
		coordinator.allowedInstruments[instrumentID] = struct{}{}
	}
	for _, strategyID := range config.AllowedStrategyIDs {
		coordinator.allowedStrategies[strategyID] = struct{}{}
	}
	if source, ok := options.AuditSink.(DeploymentAuditSource); ok {
		records, err := source.ReadAll()
		if err != nil {
			return nil, fmt.Errorf("read deployment recovery audit: %w", err)
		}
		if err := coordinator.restore(records, state); err != nil {
			return nil, err
		}
	} else if state == RunStateCanary {
		return nil, errors.New("active canary recovery requires a readable deployment audit")
	}
	return coordinator, nil
}

// State returns the atomically protected fail-closed run state.
func (coordinator *DeploymentCoordinator) State() DeploymentRunState {
	coordinator.mu.Lock()
	defer coordinator.mu.Unlock()
	return coordinator.state
}

// Observe records one identical-snapshot comparison, updates paired statistics,
// and blocks promotion or automatically rolls back on a confidence-bound breach.
func (coordinator *DeploymentCoordinator) Observe(ctx context.Context, observation PairedObservation) (ObservationOutcome, error) {
	if err := contextError(ctx); err != nil {
		return ObservationOutcome{}, err
	}
	coordinator.mu.Lock()
	if coordinator.state != RunStateShadow && coordinator.state != RunStateCanary {
		state := coordinator.state
		coordinator.mu.Unlock()
		return ObservationOutcome{State: state}, ErrDeploymentBlocked
	}
	if err := coordinator.validateObservation(observation); err != nil {
		wasCanary := coordinator.state == RunStateCanary
		if wasCanary {
			coordinator.state = RunStateRollbackPending
		} else {
			coordinator.state = RunStatePromotionBlocked
		}
		state := coordinator.state
		coordinator.mu.Unlock()
		if wasCanary {
			rollbackErr := coordinator.performRollback(ctx, nil, "INVALID_PAIRED_OBSERVATION")
			return ObservationOutcome{State: coordinator.State()}, errors.Join(err, rollbackErr)
		}
		return ObservationOutcome{State: state}, err
	}
	hypothetical, err := buildHypotheticalDecision(observation)
	if err != nil {
		coordinator.state = RunStatePromotionBlocked
		state := coordinator.state
		coordinator.mu.Unlock()
		return ObservationOutcome{State: state}, err
	}
	record := coordinator.auditRecord(AuditObservation, "PAIRED_FORECAST_OBSERVATION")
	record.ObservationSequence = observation.Sequence
	record.Regime = observation.Regime
	record.PairedObservation = &observation
	record.HypotheticalDecision = &hypothetical
	if err := coordinator.audit.Append(record); err != nil {
		wasCanary := coordinator.state == RunStateCanary
		if wasCanary {
			coordinator.state = RunStateRollbackPending
		} else {
			coordinator.state = RunStatePromotionBlocked
		}
		state := coordinator.state
		coordinator.mu.Unlock()
		if wasCanary {
			rollbackErr := coordinator.performRollback(ctx, nil, "DEPLOYMENT_AUDIT_UNAVAILABLE")
			return ObservationOutcome{State: coordinator.State(), Hypothetical: hypothetical}, errors.Join(err, rollbackErr)
		}
		return ObservationOutcome{State: state, Hypothetical: hypothetical}, fmt.Errorf("append deployment audit: %w", err)
	}
	coordinator.applyObservation(observation)
	intervals, trigger, err := coordinator.intervalsForRegime(observation.Regime)
	if err != nil {
		coordinator.state = RunStatePromotionBlocked
		state := coordinator.state
		coordinator.mu.Unlock()
		return ObservationOutcome{State: state, Hypothetical: hypothetical}, err
	}
	if trigger == nil {
		state := coordinator.state
		coordinator.mu.Unlock()
		return ObservationOutcome{State: state, Hypothetical: hypothetical, Intervals: intervals}, nil
	}
	wasCanary := coordinator.state == RunStateCanary
	if wasCanary {
		coordinator.state = RunStateRollbackPending
	} else {
		coordinator.state = RunStatePromotionBlocked
	}
	coordinator.lastTrigger = trigger
	triggerRecord := coordinator.auditRecord(AuditRollbackTriggered, trigger.ReasonCode)
	triggerRecord.Trigger = trigger
	triggerRecord.Regime = trigger.Regime
	triggerRecord.ObservationSequence = observation.Sequence
	triggerAuditErr := coordinator.audit.Append(triggerRecord)
	state := coordinator.state
	coordinator.mu.Unlock()
	outcome := ObservationOutcome{State: state, Hypothetical: hypothetical, Intervals: intervals, Trigger: trigger}
	if !wasCanary {
		if triggerAuditErr != nil {
			return outcome, errors.Join(ErrAutomaticRollback, triggerAuditErr)
		}
		return outcome, ErrPromotionNotReady
	}
	rollbackErr := coordinator.performRollback(ctx, trigger, trigger.ReasonCode)
	outcome.State = coordinator.State()
	if triggerAuditErr != nil {
		rollbackErr = errors.Join(rollbackErr, triggerAuditErr)
	}
	if rollbackErr != nil {
		return outcome, errors.Join(ErrAutomaticRollback, rollbackErr)
	}
	return outcome, ErrAutomaticRollback
}

// Readiness requires every metric in every configured regime. It never promotes
// and raw aggregate P&L is not an input.
func (coordinator *DeploymentCoordinator) Readiness() (PromotionReadiness, error) {
	coordinator.mu.Lock()
	defer coordinator.mu.Unlock()
	readiness := PromotionReadiness{
		State: coordinator.state, ConfigurationSHA256: coordinator.config.ConfigurationSHA256,
		MissingSamples: make(map[MarketRegime][]RollbackMetric),
		Intervals:      make(map[MarketRegime][]ConfidenceInterval),
	}
	if coordinator.state != RunStateShadow {
		readiness.ReasonCodes = append(readiness.ReasonCodes, "NOT_IN_SHADOW_EVALUATION")
		return readiness, nil
	}
	for _, regime := range coordinator.config.RequiredRegimes {
		intervals, trigger, err := coordinator.intervalsForRegime(regime)
		if err != nil {
			return PromotionReadiness{}, err
		}
		readiness.Intervals[regime] = intervals
		for _, interval := range intervals {
			if !interval.EnoughSamples {
				readiness.MissingSamples[regime] = append(readiness.MissingSamples[regime], interval.Metric)
			}
		}
		if trigger != nil {
			readiness.ReasonCodes = append(readiness.ReasonCodes, trigger.ReasonCode)
		}
	}
	if len(readiness.MissingSamples) != 0 {
		readiness.ReasonCodes = append(readiness.ReasonCodes, "MINIMUM_REGIME_SAMPLES_NOT_MET")
	}
	readiness.ReadyForApproval = len(readiness.MissingSamples) == 0 && len(readiness.ReasonCodes) == 0
	if readiness.ReadyForApproval {
		readiness.ReasonCodes = append(readiness.ReasonCodes, "READY_FOR_EXPLICIT_CANARY_APPROVAL")
	}
	return readiness, nil
}

// ApproveCanary performs a separately approved registry transition only after
// complete shadow evidence. It never authorizes an executable order.
func (coordinator *DeploymentCoordinator) ApproveCanary(ctx context.Context, approval CanaryApproval) (ModelStatus, error) {
	if err := contextError(ctx); err != nil {
		return ModelStatus{}, err
	}
	if err := validateActorReason(approval.Actor, approval.Reason); err != nil {
		return ModelStatus{}, err
	}
	if approval.Actor == coordinator.config.ApprovedBy || strings.TrimSpace(approval.ApprovalReference) == "" || len(approval.ApprovalReference) > 256 || approval.ConfigurationSHA256 != coordinator.config.ConfigurationSHA256 {
		return ModelStatus{}, errors.New("canary approval requires second-person actor, bounded reference, and exact configuration hash")
	}
	readiness, err := coordinator.Readiness()
	if err != nil {
		return ModelStatus{}, err
	}
	if !readiness.ReadyForApproval {
		return ModelStatus{}, ErrPromotionNotReady
	}
	coordinator.mu.Lock()
	if coordinator.state != RunStateShadow {
		coordinator.mu.Unlock()
		return ModelStatus{}, ErrPromotionNotReady
	}
	coordinator.state = RunStatePromotionPending
	record := coordinator.auditRecord(AuditCanaryApprovalRequested, "EXPLICIT_CANARY_APPROVAL")
	record.Approval = &approval
	if err := coordinator.audit.Append(record); err != nil {
		coordinator.state = RunStatePromotionBlocked
		coordinator.mu.Unlock()
		return ModelStatus{}, fmt.Errorf("append canary approval audit: %w", err)
	}
	coordinator.mu.Unlock()
	status, err := coordinator.registry.PromoteCanary(ctx, DeploymentRequest{
		ModelID: coordinator.config.ModelID, SemanticVersion: coordinator.config.CandidateVersion,
		ExpectedFeatureSchema: coordinator.config.FeatureSchema, Environment: coordinator.config.CanaryEnvironment,
		Actor: approval.Actor, Reason: approval.Reason, AuthorizationReference: approval.ApprovalReference,
	})
	coordinator.mu.Lock()
	if err != nil {
		coordinator.state = RunStatePromotionBlocked
		coordinator.mu.Unlock()
		return ModelStatus{}, err
	}
	coordinator.state = RunStateCanary
	activated := coordinator.auditRecord(AuditCanaryActivated, "CANARY_ACTIVATED")
	activated.Approval = &approval
	auditErr := coordinator.audit.Append(activated)
	if auditErr != nil {
		coordinator.state = RunStateRollbackPending
	}
	coordinator.mu.Unlock()
	if auditErr != nil {
		rollbackErr := coordinator.performRollback(ctx, nil, "CANARY_ACTIVATION_AUDIT_UNAVAILABLE")
		return status, errors.Join(auditErr, rollbackErr)
	}
	return status, nil
}

// AdmitCanaryScope enforces the canary symbol, strategy, capital, rate, and risk
// freshness envelope. Admitted still means non-executable and risk-required.
func (coordinator *DeploymentCoordinator) AdmitCanaryScope(ctx context.Context, request CanaryScopeRequest) (CanaryScopeDecision, error) {
	if err := contextError(ctx); err != nil {
		return CanaryScopeDecision{}, err
	}
	coordinator.mu.Lock()
	decision := CanaryScopeDecision{Reason: CanaryScopeInvalidRequest, DownstreamRiskRequired: true, OrderExecutable: false}
	if coordinator.state != RunStateCanary {
		decision.Reason = CanaryScopeNotCanary
		coordinator.canaryRejections[decision.Reason]++
		coordinator.mu.Unlock()
		return decision, ErrDeploymentBlocked
	}
	validSequence := request.Sequence != 0 && request.Sequence > coordinator.lastScopeSequence
	validIdentity := validIdentifier(request.InstrumentID) && validIdentifier(request.StrategyID) && validIdentifier(request.RiskSnapshotID) && hex64Pattern.MatchString(request.RiskSnapshotSHA256)
	if !validSequence || !validIdentity || request.NowProcessMonotonicTimeNS == 0 || request.RiskSnapshotObservedMonotonicNS == 0 || request.RequestedCapitalNanos == 0 {
		decision.Reason = CanaryScopeInvalidRequest
	} else if coordinator.lastScopeTimeNS != 0 && request.NowProcessMonotonicTimeNS < coordinator.lastScopeTimeNS {
		decision.Reason = CanaryScopeTimeRegressed
	} else if request.NowProcessMonotonicTimeNS < request.RiskSnapshotObservedMonotonicNS || request.NowProcessMonotonicTimeNS-request.RiskSnapshotObservedMonotonicNS > coordinator.config.MaximumRiskSnapshotAgeNS {
		decision.Reason = CanaryScopeRiskSnapshotStale
	} else if _, allowed := coordinator.allowedInstruments[request.InstrumentID]; !allowed {
		decision.Reason = CanaryScopeSymbolDenied
	} else if _, allowed := coordinator.allowedStrategies[request.StrategyID]; !allowed {
		decision.Reason = CanaryScopeStrategyDenied
	} else if request.CurrentGrossCapitalNanos > coordinator.config.MaximumCapitalCurrencyNanos || request.RequestedCapitalNanos > coordinator.config.MaximumCapitalCurrencyNanos-request.CurrentGrossCapitalNanos {
		decision.Reason = CanaryScopeCapitalExceeded
	} else {
		coordinator.expireOrderTimes(request.NowProcessMonotonicTimeNS)
		if len(coordinator.orderTimes) >= int(coordinator.config.MaximumOrdersPerWindow) {
			decision.Reason = CanaryScopeOrderRateExceeded
		} else {
			decision.Admitted = true
			decision.Reason = CanaryScopeAdmitted
			decision.RemainingCapitalNanos = coordinator.config.MaximumCapitalCurrencyNanos - request.CurrentGrossCapitalNanos - request.RequestedCapitalNanos
			decision.RemainingOrdersInWindow = coordinator.config.MaximumOrdersPerWindow - uint32(len(coordinator.orderTimes)) - 1
		}
	}
	record := coordinator.auditRecord(AuditCanaryScopeDecision, string(decision.Reason))
	record.ScopeRequest = &request
	record.ScopeDecision = &decision
	if err := coordinator.audit.Append(record); err != nil {
		decision.Admitted = false
		decision.Reason = CanaryScopeAuditUnavailable
		coordinator.canaryRejections[decision.Reason]++
		coordinator.state = RunStateRollbackPending
		coordinator.mu.Unlock()
		rollbackErr := coordinator.performRollback(ctx, nil, "CANARY_SCOPE_AUDIT_UNAVAILABLE")
		return decision, errors.Join(err, rollbackErr)
	}
	// Only structurally valid, monotonic requests advance replay protection.
	// A malformed/replayed request must never move the sequence or clock back.
	if validSequence && validIdentity && request.NowProcessMonotonicTimeNS != 0 && request.RiskSnapshotObservedMonotonicNS != 0 && request.RequestedCapitalNanos != 0 {
		coordinator.lastScopeSequence = request.Sequence
	}
	if validSequence && request.NowProcessMonotonicTimeNS >= coordinator.lastScopeTimeNS {
		coordinator.lastScopeTimeNS = request.NowProcessMonotonicTimeNS
	}
	if decision.Admitted {
		coordinator.orderTimes = append(coordinator.orderTimes, request.NowProcessMonotonicTimeNS)
	} else {
		coordinator.canaryRejections[decision.Reason]++
	}
	coordinator.mu.Unlock()
	return decision, nil
}

func (coordinator *DeploymentCoordinator) validateObservation(observation PairedObservation) error {
	if observation.Sequence == 0 || observation.Sequence <= coordinator.lastObservationSequence || !validIdentifier(observation.InstrumentID) || !validIdentifier(observation.StrategyID) || observation.ObservedProcessMonotonicTimeNS == 0 {
		return fmt.Errorf("%w: invalid identity, time, or sequence", ErrInvalidObservation)
	}
	if _, ok := coordinator.allowedRegimes[observation.Regime]; !ok {
		return fmt.Errorf("%w: unconfigured regime %q", ErrInvalidObservation, observation.Regime)
	}
	return validatePairedObservation(observation, coordinator.config.ProductionVersion, coordinator.config.CandidateVersion)
}

func validatePairedObservation(observation PairedObservation, productionVersion string, candidateVersion string) error {
	if observation.Sequence == 0 || !validIdentifier(observation.InstrumentID) || !validIdentifier(observation.StrategyID) || !validRegime(observation.Regime) || observation.ObservedProcessMonotonicTimeNS == 0 {
		return fmt.Errorf("%w: invalid observation identity, regime, time, or sequence", ErrInvalidObservation)
	}
	if err := validateForecastSample(observation.Production, productionVersion); err != nil {
		return fmt.Errorf("%w: production: %v", ErrInvalidObservation, err)
	}
	if err := validateForecastSample(observation.Candidate, candidateVersion); err != nil {
		return fmt.Errorf("%w: candidate: %v", ErrInvalidObservation, err)
	}
	if observation.Production.FeatureSnapshotID != observation.Candidate.FeatureSnapshotID || observation.Production.FeatureSnapshotSHA256 != observation.Candidate.FeatureSnapshotSHA256 || observation.Production.AsOfExchangeEventTimeNS != observation.Candidate.AsOfExchangeEventTimeNS {
		return fmt.Errorf("%w: forecasts did not consume an identical feature snapshot", ErrInvalidObservation)
	}
	if observation.ObservedProcessMonotonicTimeNS < observation.Production.CompletedProcessMonotonicTimeNS || observation.ObservedProcessMonotonicTimeNS < observation.Candidate.CompletedProcessMonotonicTimeNS {
		return fmt.Errorf("%w: observation predates forecast completion", ErrInvalidObservation)
	}
	return nil
}

func (coordinator *DeploymentCoordinator) applyObservation(observation PairedObservation) {
	coordinator.lastObservationSequence = observation.Sequence
	coordinator.observationCount++
	productionMiss := observation.Production.CompletedProcessMonotonicTimeNS > observation.Production.DeadlineProcessMonotonicTimeNS
	candidateMiss := observation.Candidate.CompletedProcessMonotonicTimeNS > observation.Candidate.DeadlineProcessMonotonicTimeNS
	for _, metric := range allRollbackMetrics {
		production := metricValue(observation.Production.Metrics, productionMiss, metric)
		candidate := metricValue(observation.Candidate.Metrics, candidateMiss, metric)
		coordinator.statistics[observation.Regime][metric].add(candidate - production)
	}
}

func (coordinator *DeploymentCoordinator) restore(records []DeploymentAuditRecord, registryState DeploymentRunState) error {
	for index, record := range records {
		if record.DeploymentID != coordinator.config.DeploymentID || record.ModelID != coordinator.config.ModelID || record.CandidateVersion != coordinator.config.CandidateVersion || record.ProductionVersion != coordinator.config.ProductionVersion || record.ConfigurationSHA256 != coordinator.config.ConfigurationSHA256 {
			return fmt.Errorf("deployment audit record %d does not match signed configuration", index+1)
		}
		if err := validateDeploymentAuditRecord(record); err != nil {
			return fmt.Errorf("deployment audit record %d: %w", index+1, err)
		}
		switch record.Kind {
		case AuditObservation:
			if err := coordinator.validateObservation(*record.PairedObservation); err != nil {
				return fmt.Errorf("restore observation %d: %w", index+1, err)
			}
			coordinator.applyObservation(*record.PairedObservation)
		case AuditRollbackTriggered:
			trigger := *record.Trigger
			coordinator.lastTrigger = &trigger
		case AuditRollbackCompleted, AuditFailClosedDisable, AuditFailClosedDisableFailed:
			coordinator.rollbackCount++
		case AuditCanaryScopeDecision:
			request := *record.ScopeRequest
			decision := *record.ScopeDecision
			if request.Sequence <= coordinator.lastScopeSequence || request.NowProcessMonotonicTimeNS < coordinator.lastScopeTimeNS {
				return fmt.Errorf("deployment audit record %d has regressed canary scope identity", index+1)
			}
			coordinator.expireOrderTimes(request.NowProcessMonotonicTimeNS)
			coordinator.lastScopeSequence = request.Sequence
			coordinator.lastScopeTimeNS = request.NowProcessMonotonicTimeNS
			if decision.Admitted {
				coordinator.orderTimes = append(coordinator.orderTimes, request.NowProcessMonotonicTimeNS)
			} else {
				coordinator.canaryRejections[decision.Reason]++
			}
		}
		coordinator.state = record.State
	}
	if len(records) == 0 {
		coordinator.state = registryState
	}
	if registryState == RunStateCanary && coordinator.state != RunStateCanary && coordinator.state != RunStateRollbackPending {
		return errors.New("registry canary state does not match recovered deployment audit")
	}
	if registryState == RunStateShadow && (coordinator.state == RunStateCanary || coordinator.state == RunStateRollbackPending) {
		return errors.New("registry shadow state does not match recovered deployment audit")
	}
	return nil
}

func validateForecastSample(sample ForecastSample, expectedVersion string) error {
	if !validIdentifier(sample.ForecastID) || sample.ModelVersion != expectedVersion || !validIdentifier(sample.FeatureSnapshotID) || !hex64Pattern.MatchString(sample.FeatureSnapshotSHA256) || sample.AsOfExchangeEventTimeNS <= 0 || sample.CompletedProcessMonotonicTimeNS == 0 || sample.DeadlineProcessMonotonicTimeNS == 0 || !validDecisionAction(sample.Action) || sample.NetRobustEdgePPM < -maximumDeploymentValue || sample.NetRobustEdgePPM > maximumDeploymentValue {
		return errors.New("invalid forecast identity, provenance, deadline, action, or edge")
	}
	return validateForecastMetrics(sample.Metrics)
}

func buildHypotheticalDecision(observation PairedObservation) (HypotheticalDecision, error) {
	decision := HypotheticalDecision{
		FeatureSnapshotID:         observation.Candidate.FeatureSnapshotID,
		FeatureSnapshotSHA256:     observation.Candidate.FeatureSnapshotSHA256,
		ProductionForecastID:      observation.Production.ForecastID,
		CandidateForecastID:       observation.Candidate.ForecastID,
		ProductionAction:          observation.Production.Action,
		CandidateAction:           observation.Candidate.Action,
		CandidateNetRobustEdgePPM: observation.Candidate.NetRobustEdgePPM,
		Executable:                false, DownstreamRiskRequired: true,
	}
	digest, err := canonicalSHA256(decision)
	if err != nil {
		return HypotheticalDecision{}, err
	}
	decision.DecisionSHA256 = digest
	return decision, nil
}

func (coordinator *DeploymentCoordinator) intervalsForRegime(regime MarketRegime) ([]ConfidenceInterval, *RollbackTrigger, error) {
	intervals := make([]ConfidenceInterval, 0, len(allRollbackMetrics))
	var trigger *RollbackTrigger
	for _, metric := range allRollbackMetrics {
		interval, err := coordinator.statistics[regime][metric].interval(metric, coordinator.thresholds[metric], coordinator.config.ConfidenceZPPM)
		if err != nil {
			return nil, nil, err
		}
		intervals = append(intervals, interval)
		if interval.Triggered && trigger == nil {
			candidate := RollbackTrigger{
				Metric: metric, Regime: regime, Interval: interval,
				ReasonCode: "ROLLBACK_" + strings.ToUpper(string(metric)),
			}
			trigger = &candidate
		}
	}
	return intervals, trigger, nil
}

func (coordinator *DeploymentCoordinator) performRollback(ctx context.Context, trigger *RollbackTrigger, reasonCode string) error {
	reason := reasonCode
	if trigger != nil {
		reason = fmt.Sprintf("%s regime=%s upper=%d threshold=%d", trigger.ReasonCode, trigger.Regime, trigger.Interval.UpperBound, trigger.Interval.Threshold)
	}
	status, rollbackErr := coordinator.registry.Rollback(ctx, RollbackRequest{
		ModelID: coordinator.config.ModelID, SemanticVersion: coordinator.config.CandidateVersion,
		TargetSemanticVersion: coordinator.config.ProductionVersion,
		ExpectedFeatureSchema: coordinator.config.FeatureSchema, Environment: coordinator.config.CanaryEnvironment,
		Actor: coordinator.config.AutomaticRollbackActor, Reason: reason,
		AuthorizationReference: coordinator.config.ApprovalReference,
	})
	if rollbackErr == nil {
		coordinator.mu.Lock()
		defer coordinator.mu.Unlock()
		coordinator.state = RunStateRolledBack
		coordinator.rollbackCount++
		record := coordinator.auditRecord(AuditRollbackCompleted, reasonCode)
		record.Trigger = trigger
		if err := coordinator.audit.Append(record); err != nil {
			return fmt.Errorf("rollback succeeded at registry revision %d but deployment audit append failed: %w", status.Revision, err)
		}
		return nil
	}
	disableStatus, disableErr := coordinator.registry.Disable(ctx, DisableRequest{
		ModelID: coordinator.config.ModelID, SemanticVersion: coordinator.config.CandidateVersion,
		Actor: coordinator.config.AutomaticRollbackActor, Reason: "automatic rollback failed; fail-closed disable",
	})
	coordinator.mu.Lock()
	defer coordinator.mu.Unlock()
	coordinator.rollbackCount++
	if disableErr != nil {
		coordinator.state = RunStateDisableFailed
		record := coordinator.auditRecord(AuditFailClosedDisableFailed, "ROLLBACK_AND_DISABLE_FAILED")
		record.Trigger = trigger
		auditErr := coordinator.audit.Append(record)
		return errors.Join(rollbackErr, disableErr, auditErr)
	}
	coordinator.state = RunStateDisabledFailClosed
	record := coordinator.auditRecord(AuditFailClosedDisable, "ROLLBACK_FAILED_DISABLED")
	record.Trigger = trigger
	auditErr := coordinator.audit.Append(record)
	return errors.Join(fmt.Errorf("rollback failed; candidate disabled at revision %d: %w", disableStatus.Revision, rollbackErr), auditErr)
}

func (coordinator *DeploymentCoordinator) expireOrderTimes(now uint64) {
	firstActive := 0
	for firstActive < len(coordinator.orderTimes) {
		timestamp := coordinator.orderTimes[firstActive]
		if now < timestamp || now-timestamp < coordinator.config.OrderRateWindowNS {
			break
		}
		firstActive++
	}
	if firstActive != 0 {
		copy(coordinator.orderTimes, coordinator.orderTimes[firstActive:])
		coordinator.orderTimes = coordinator.orderTimes[:len(coordinator.orderTimes)-firstActive]
	}
}

func (coordinator *DeploymentCoordinator) auditRecord(kind DeploymentAuditKind, reasonCode string) DeploymentAuditRecord {
	return DeploymentAuditRecord{
		SchemaVersion: deploymentSchemaVersion, Kind: kind,
		DeploymentID: coordinator.config.DeploymentID, ModelID: coordinator.config.ModelID,
		CandidateVersion: coordinator.config.CandidateVersion, ProductionVersion: coordinator.config.ProductionVersion,
		ConfigurationSHA256: coordinator.config.ConfigurationSHA256,
		State:               coordinator.state, OccurredAtUTCNS: coordinator.clock().UTC().UnixNano(),
		ReasonCode: reasonCode,
	}
}

func containsString(values []string, expected string) bool {
	index := sort.SearchStrings(values, expected)
	return index < len(values) && values[index] == expected
}
