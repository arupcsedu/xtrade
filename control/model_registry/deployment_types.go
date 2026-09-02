package modelregistry

import (
	"errors"
	"fmt"
	"sort"
	"strings"
)

const (
	deploymentSchemaVersion      uint32 = 1
	maximumDeploymentValue              = int64(1_000_000_000_000_000)
	partsPerMillion                     = int64(1_000_000)
	maximumDeploymentRegimes            = 16
	maximumCanarySymbols                = 64
	maximumCanaryStrategies             = 32
	maximumCanaryOrdersPerWindow        = 4096
)

// DeploymentRunState is the fail-closed coordinator state.
type DeploymentRunState string

const (
	RunStateShadow             DeploymentRunState = "SHADOW"
	RunStatePromotionPending   DeploymentRunState = "PROMOTION_PENDING"
	RunStateCanary             DeploymentRunState = "CANARY"
	RunStatePromotionBlocked   DeploymentRunState = "PROMOTION_BLOCKED"
	RunStateRollbackPending    DeploymentRunState = "ROLLBACK_PENDING"
	RunStateRolledBack         DeploymentRunState = "ROLLED_BACK"
	RunStateDisabledFailClosed DeploymentRunState = "DISABLED_FAIL_CLOSED"
	RunStateDisableFailed      DeploymentRunState = "DISABLE_FAILED_FAIL_CLOSED"
)

// MarketRegime partitions comparisons so aggregate results cannot hide a bad
// event or degraded-data regime.
type MarketRegime string

const (
	RegimeNormal          MarketRegime = "NORMAL"
	RegimeScheduledEvent  MarketRegime = "SCHEDULED_EVENT"
	RegimeBreakingNews    MarketRegime = "BREAKING_NEWS"
	RegimePriceDiscovery  MarketRegime = "PRICE_DISCOVERY"
	RegimeVolatilitySpike MarketRegime = "VOLATILITY_SPIKE"
	RegimeDataDegraded    MarketRegime = "DATA_DEGRADED"
)

// RollbackMetric is one required automatic rollback dimension. Every metric is
// oriented so a positive candidate-minus-production value is deterioration.
type RollbackMetric string

const (
	MetricLatencyRegression                    RollbackMetric = "latency_regression_ns"
	MetricDeadlineMisses                       RollbackMetric = "deadline_miss_rate_ppm"
	MetricCalibrationDeterioration             RollbackMetric = "calibration_deterioration_ppm"
	MetricFeatureDrift                         RollbackMetric = "feature_drift_ppm"
	MetricOODIncrease                          RollbackMetric = "ood_increase_ppm"
	MetricAbnormalTradeRate                    RollbackMetric = "abnormal_trade_rate_ppm"
	MetricUnexpectedDisagreement               RollbackMetric = "unexpected_model_disagreement_ppm"
	MetricImplementationShortfallDeterioration RollbackMetric = "implementation_shortfall_deterioration_ppm"
	MetricPnLAttributionAnomaly                RollbackMetric = "pnl_attribution_anomaly_ppm"
	MetricRiskLimitPressure                    RollbackMetric = "risk_limit_pressure_ppm"
)

var allRollbackMetrics = [...]RollbackMetric{
	MetricLatencyRegression,
	MetricDeadlineMisses,
	MetricCalibrationDeterioration,
	MetricFeatureDrift,
	MetricOODIncrease,
	MetricAbnormalTradeRate,
	MetricUnexpectedDisagreement,
	MetricImplementationShortfallDeterioration,
	MetricPnLAttributionAnomaly,
	MetricRiskLimitPressure,
}

// DecisionAction is explanatory only. It cannot address a venue or construct
// an order.
type DecisionAction string

const (
	DecisionAbstain DecisionAction = "ABSTAIN"
	DecisionBuy     DecisionAction = "BUY"
	DecisionSell    DecisionAction = "SELL"
)

// RollbackThreshold defines a one-sided confidence bound and minimum sample
// size for one required metric.
type RollbackThreshold struct {
	Metric            RollbackMetric `json:"metric"`
	MinimumSamples    uint64         `json:"minimum_samples"`
	MaximumRegression int64          `json:"maximum_regression"`
}

// DeploymentConfig is canonicalized and signed before a coordinator accepts it.
type DeploymentConfig struct {
	SchemaVersion               uint32              `json:"schema_version"`
	DeploymentID                string              `json:"deployment_id"`
	ModelID                     string              `json:"model_id"`
	CandidateVersion            string              `json:"candidate_version"`
	ProductionVersion           string              `json:"production_version"`
	ShadowEnvironment           string              `json:"shadow_environment"`
	CanaryEnvironment           string              `json:"canary_environment"`
	FeatureSchema               FeatureSchema       `json:"feature_schema"`
	RequiredRegimes             []MarketRegime      `json:"required_regimes"`
	RollbackThresholds          []RollbackThreshold `json:"rollback_thresholds"`
	AllowedInstrumentIDs        []string            `json:"allowed_instrument_ids"`
	AllowedStrategyIDs          []string            `json:"allowed_strategy_ids"`
	MaximumCapitalCurrencyNanos uint64              `json:"maximum_capital_currency_nanos"`
	OrderRateWindowNS           uint64              `json:"order_rate_window_ns"`
	MaximumOrdersPerWindow      uint32              `json:"maximum_orders_per_window"`
	MaximumRiskSnapshotAgeNS    uint64              `json:"maximum_risk_snapshot_age_ns"`
	ConfidenceZPPM              uint32              `json:"confidence_z_ppm"`
	ApprovedBy                  string              `json:"approved_by"`
	ApprovalReference           string              `json:"approval_reference"`
	AutomaticRollbackActor      string              `json:"automatic_rollback_actor"`
	CreatedAtUTCNS              int64               `json:"created_at_utc_ns"`
	ConfigurationSHA256         string              `json:"configuration_sha256"`
}

// SignedDeploymentConfig authenticates deterministic deployment limits.
type SignedDeploymentConfig struct {
	Config          DeploymentConfig `json:"config"`
	SignerKeyID     string           `json:"signer_key_id"`
	SignatureBase64 string           `json:"signature_base64"`
}

// ForecastMetrics are explicit fixed-point or integer-unit observations. Raw
// P&L is intentionally absent; P&L attribution anomaly is one of ten gates.
type ForecastMetrics struct {
	LatencyNS                  int64 `json:"latency_ns"`
	CalibrationErrorPPM        int64 `json:"calibration_error_ppm"`
	FeatureDriftPPM            int64 `json:"feature_drift_ppm"`
	OODScorePPM                int64 `json:"ood_score_ppm"`
	TradeRatePPM               int64 `json:"trade_rate_ppm"`
	ModelDisagreementPPM       int64 `json:"model_disagreement_ppm"`
	ImplementationShortfallPPM int64 `json:"implementation_shortfall_ppm"`
	PnLAttributionAnomalyPPM   int64 `json:"pnl_attribution_anomaly_ppm"`
	RiskLimitPressurePPM       int64 `json:"risk_limit_pressure_ppm"`
}

// ForecastSample binds one forecast to the exact feature snapshot and deadline.
type ForecastSample struct {
	ForecastID                      string          `json:"forecast_id"`
	ModelVersion                    string          `json:"model_version"`
	FeatureSnapshotID               string          `json:"feature_snapshot_id"`
	FeatureSnapshotSHA256           string          `json:"feature_snapshot_sha256"`
	AsOfExchangeEventTimeNS         int64           `json:"as_of_exchange_event_time_ns"`
	CompletedProcessMonotonicTimeNS uint64          `json:"completed_process_monotonic_time_ns"`
	DeadlineProcessMonotonicTimeNS  uint64          `json:"deadline_process_monotonic_time_ns"`
	Action                          DecisionAction  `json:"action"`
	NetRobustEdgePPM                int64           `json:"net_robust_edge_ppm"`
	Metrics                         ForecastMetrics `json:"metrics"`
}

// PairedObservation proves production and candidate consumed an identical
// snapshot before comparing their outcomes.
type PairedObservation struct {
	Sequence                       uint64         `json:"sequence"`
	InstrumentID                   string         `json:"instrument_id"`
	StrategyID                     string         `json:"strategy_id"`
	Regime                         MarketRegime   `json:"regime"`
	ObservedProcessMonotonicTimeNS uint64         `json:"observed_process_monotonic_time_ns"`
	Production                     ForecastSample `json:"production"`
	Candidate                      ForecastSample `json:"candidate"`
}

// HypotheticalDecision is the only shadow decision representation. It has no
// order, account, venue, price, quantity, or executable field that can be true.
type HypotheticalDecision struct {
	DecisionSHA256            string         `json:"decision_sha256"`
	FeatureSnapshotID         string         `json:"feature_snapshot_id"`
	FeatureSnapshotSHA256     string         `json:"feature_snapshot_sha256"`
	ProductionForecastID      string         `json:"production_forecast_id"`
	CandidateForecastID       string         `json:"candidate_forecast_id"`
	ProductionAction          DecisionAction `json:"production_action"`
	CandidateAction           DecisionAction `json:"candidate_action"`
	CandidateNetRobustEdgePPM int64          `json:"candidate_net_robust_edge_ppm"`
	Executable                bool           `json:"executable"`
	DownstreamRiskRequired    bool           `json:"downstream_risk_required"`
}

// ConfidenceInterval is an integer, conservative, paired confidence interval.
type ConfidenceInterval struct {
	Metric        RollbackMetric `json:"metric"`
	SampleCount   uint64         `json:"sample_count"`
	MeanDelta     int64          `json:"mean_delta"`
	LowerBound    int64          `json:"lower_bound"`
	UpperBound    int64          `json:"upper_bound"`
	HalfWidth     uint64         `json:"half_width"`
	Threshold     int64          `json:"threshold"`
	EnoughSamples bool           `json:"enough_samples"`
	Triggered     bool           `json:"triggered"`
}

// RollbackTrigger is the machine-readable automatic rollback cause.
type RollbackTrigger struct {
	Metric     RollbackMetric     `json:"metric"`
	Regime     MarketRegime       `json:"regime"`
	Interval   ConfidenceInterval `json:"interval"`
	ReasonCode string             `json:"reason_code"`
}

// ObservationOutcome reports evaluation without exposing an executable action.
type ObservationOutcome struct {
	State        DeploymentRunState   `json:"state"`
	Hypothetical HypotheticalDecision `json:"hypothetical_decision"`
	Intervals    []ConfidenceInterval `json:"intervals"`
	Trigger      *RollbackTrigger     `json:"trigger,omitempty"`
}

// PromotionReadiness requires every metric in every configured regime.
type PromotionReadiness struct {
	ReadyForApproval    bool                                  `json:"ready_for_approval"`
	State               DeploymentRunState                    `json:"state"`
	ConfigurationSHA256 string                                `json:"configuration_sha256"`
	MissingSamples      map[MarketRegime][]RollbackMetric     `json:"missing_samples,omitempty"`
	Intervals           map[MarketRegime][]ConfidenceInterval `json:"intervals"`
	ReasonCodes         []string                              `json:"reason_codes"`
}

// CanaryApproval is a second explicit human authorization for canary.
type CanaryApproval struct {
	Actor               string `json:"actor"`
	Reason              string `json:"reason"`
	ApprovalReference   string `json:"approval_reference"`
	ConfigurationSHA256 string `json:"configuration_sha256"`
}

// CanaryScopeRequest is a bounded policy request, not an order.
type CanaryScopeRequest struct {
	Sequence                        uint64 `json:"sequence"`
	InstrumentID                    string `json:"instrument_id"`
	StrategyID                      string `json:"strategy_id"`
	RiskSnapshotID                  string `json:"risk_snapshot_id"`
	RiskSnapshotSHA256              string `json:"risk_snapshot_sha256"`
	NowProcessMonotonicTimeNS       uint64 `json:"now_process_monotonic_time_ns"`
	RiskSnapshotObservedMonotonicNS uint64 `json:"risk_snapshot_observed_monotonic_ns"`
	CurrentGrossCapitalNanos        uint64 `json:"current_gross_capital_nanos"`
	RequestedCapitalNanos           uint64 `json:"requested_capital_nanos"`
}

// CanaryScopeReason is a stable admission reason.
type CanaryScopeReason string

const (
	CanaryScopeAdmitted          CanaryScopeReason = "ADMITTED"
	CanaryScopeNotCanary         CanaryScopeReason = "NOT_CANARY"
	CanaryScopeInvalidRequest    CanaryScopeReason = "INVALID_REQUEST"
	CanaryScopeSymbolDenied      CanaryScopeReason = "SYMBOL_DENIED"
	CanaryScopeStrategyDenied    CanaryScopeReason = "STRATEGY_DENIED"
	CanaryScopeCapitalExceeded   CanaryScopeReason = "CAPITAL_EXCEEDED"
	CanaryScopeOrderRateExceeded CanaryScopeReason = "ORDER_RATE_EXCEEDED"
	CanaryScopeRiskSnapshotStale CanaryScopeReason = "RISK_SNAPSHOT_STALE"
	CanaryScopeTimeRegressed     CanaryScopeReason = "TIME_REGRESSED"
	CanaryScopeAuditUnavailable  CanaryScopeReason = "AUDIT_UNAVAILABLE"
)

// CanaryScopeDecision never makes an order executable. An admitted request must
// still pass the normal deterministic pre-trade risk, OMS, and gateway gates.
type CanaryScopeDecision struct {
	Admitted                bool              `json:"admitted"`
	Reason                  CanaryScopeReason `json:"reason"`
	OrderExecutable         bool              `json:"order_executable"`
	DownstreamRiskRequired  bool              `json:"downstream_risk_required"`
	RemainingCapitalNanos   uint64            `json:"remaining_capital_nanos"`
	RemainingOrdersInWindow uint32            `json:"remaining_orders_in_window"`
}

func validRegime(regime MarketRegime) bool {
	switch regime {
	case RegimeNormal, RegimeScheduledEvent, RegimeBreakingNews, RegimePriceDiscovery, RegimeVolatilitySpike, RegimeDataDegraded:
		return true
	default:
		return false
	}
}

func validDeploymentRunState(state DeploymentRunState) bool {
	switch state {
	case RunStateShadow, RunStatePromotionPending, RunStateCanary, RunStatePromotionBlocked,
		RunStateRollbackPending, RunStateRolledBack, RunStateDisabledFailClosed, RunStateDisableFailed:
		return true
	default:
		return false
	}
}

func validMetric(metric RollbackMetric) bool {
	for _, candidate := range allRollbackMetrics {
		if candidate == metric {
			return true
		}
	}
	return false
}

func validDecisionAction(action DecisionAction) bool {
	return action == DecisionAbstain || action == DecisionBuy || action == DecisionSell
}

func metricValue(metrics ForecastMetrics, deadlineMiss bool, metric RollbackMetric) int64 {
	switch metric {
	case MetricLatencyRegression:
		return metrics.LatencyNS
	case MetricDeadlineMisses:
		if deadlineMiss {
			return partsPerMillion
		}
		return 0
	case MetricCalibrationDeterioration:
		return metrics.CalibrationErrorPPM
	case MetricFeatureDrift:
		return metrics.FeatureDriftPPM
	case MetricOODIncrease:
		return metrics.OODScorePPM
	case MetricAbnormalTradeRate:
		return metrics.TradeRatePPM
	case MetricUnexpectedDisagreement:
		return metrics.ModelDisagreementPPM
	case MetricImplementationShortfallDeterioration:
		return metrics.ImplementationShortfallPPM
	case MetricPnLAttributionAnomaly:
		return metrics.PnLAttributionAnomalyPPM
	case MetricRiskLimitPressure:
		return metrics.RiskLimitPressurePPM
	default:
		return 0
	}
}

func validateForecastMetrics(metrics ForecastMetrics) error {
	values := [...]int64{
		metrics.LatencyNS, metrics.CalibrationErrorPPM, metrics.FeatureDriftPPM,
		metrics.OODScorePPM, metrics.TradeRatePPM, metrics.ModelDisagreementPPM,
		metrics.ImplementationShortfallPPM, metrics.PnLAttributionAnomalyPPM,
		metrics.RiskLimitPressurePPM,
	}
	for _, value := range values {
		if value < 0 || value > maximumDeploymentValue {
			return errors.New("forecast metric is outside the supported nonnegative range")
		}
	}
	for _, value := range []int64{metrics.CalibrationErrorPPM, metrics.FeatureDriftPPM, metrics.OODScorePPM, metrics.ModelDisagreementPPM, metrics.PnLAttributionAnomalyPPM, metrics.RiskLimitPressurePPM} {
		if value > partsPerMillion {
			return errors.New("normalized forecast metric exceeds one million PPM")
		}
	}
	return nil
}

func validateStringSet(values []string, maximum int, field string) error {
	if len(values) == 0 || len(values) > maximum {
		return fmt.Errorf("%s count is outside 1..%d", field, maximum)
	}
	seen := make(map[string]struct{}, len(values))
	for _, value := range values {
		if !validIdentifier(value) {
			return fmt.Errorf("invalid %s value %q", field, value)
		}
		if _, exists := seen[value]; exists {
			return fmt.Errorf("duplicate %s value %q", field, value)
		}
		seen[value] = struct{}{}
	}
	return nil
}

func canonicalizeDeploymentConfig(config DeploymentConfig) DeploymentConfig {
	config.RequiredRegimes = append([]MarketRegime(nil), config.RequiredRegimes...)
	config.RollbackThresholds = append([]RollbackThreshold(nil), config.RollbackThresholds...)
	config.AllowedInstrumentIDs = append([]string(nil), config.AllowedInstrumentIDs...)
	config.AllowedStrategyIDs = append([]string(nil), config.AllowedStrategyIDs...)
	sort.Slice(config.RequiredRegimes, func(left int, right int) bool { return config.RequiredRegimes[left] < config.RequiredRegimes[right] })
	sort.Slice(config.RollbackThresholds, func(left int, right int) bool {
		return config.RollbackThresholds[left].Metric < config.RollbackThresholds[right].Metric
	})
	sort.Strings(config.AllowedInstrumentIDs)
	sort.Strings(config.AllowedStrategyIDs)
	return config
}

func validateDeploymentConfigFields(config DeploymentConfig) error {
	if config.SchemaVersion != deploymentSchemaVersion || !validIdentifier(config.DeploymentID) || !validIdentifier(config.ModelID) || !semanticPattern.MatchString(config.CandidateVersion) || !semanticPattern.MatchString(config.ProductionVersion) || config.CandidateVersion == config.ProductionVersion {
		return errors.New("invalid deployment identity or schema version")
	}
	if !validIdentifier(config.ShadowEnvironment) || !validIdentifier(config.CanaryEnvironment) || config.ShadowEnvironment == config.CanaryEnvironment {
		return errors.New("shadow and canary environments must be distinct valid identifiers")
	}
	if err := validateFeatureSchema(config.FeatureSchema); err != nil {
		return fmt.Errorf("deployment feature schema: %w", err)
	}
	if len(config.RequiredRegimes) == 0 || len(config.RequiredRegimes) > maximumDeploymentRegimes {
		return errors.New("required regime count is outside the supported range")
	}
	seenRegimes := make(map[MarketRegime]struct{}, len(config.RequiredRegimes))
	for _, regime := range config.RequiredRegimes {
		if !validRegime(regime) {
			return fmt.Errorf("invalid regime %q", regime)
		}
		if _, exists := seenRegimes[regime]; exists {
			return fmt.Errorf("duplicate regime %q", regime)
		}
		seenRegimes[regime] = struct{}{}
	}
	if len(config.RollbackThresholds) != len(allRollbackMetrics) {
		return errors.New("all ten rollback metrics must be configured exactly once")
	}
	seenMetrics := make(map[RollbackMetric]struct{}, len(config.RollbackThresholds))
	for _, threshold := range config.RollbackThresholds {
		if !validMetric(threshold.Metric) || threshold.MinimumSamples < 2 || threshold.MinimumSamples > 1_000_000 || threshold.MaximumRegression < 0 || threshold.MaximumRegression > maximumDeploymentValue {
			return fmt.Errorf("invalid rollback threshold for %q", threshold.Metric)
		}
		if _, exists := seenMetrics[threshold.Metric]; exists {
			return fmt.Errorf("duplicate rollback threshold %q", threshold.Metric)
		}
		seenMetrics[threshold.Metric] = struct{}{}
	}
	if err := validateStringSet(config.AllowedInstrumentIDs, maximumCanarySymbols, "instrument"); err != nil {
		return err
	}
	if err := validateStringSet(config.AllowedStrategyIDs, maximumCanaryStrategies, "strategy"); err != nil {
		return err
	}
	if config.MaximumCapitalCurrencyNanos == 0 || config.OrderRateWindowNS == 0 || config.MaximumOrdersPerWindow == 0 || config.MaximumOrdersPerWindow > maximumCanaryOrdersPerWindow || config.MaximumRiskSnapshotAgeNS == 0 {
		return errors.New("canary capital, rate, and risk freshness limits must be positive and bounded")
	}
	if config.ConfidenceZPPM < 1_000_000 || config.ConfidenceZPPM > 5_000_000 {
		return errors.New("confidence z-score must be between 1.0 and 5.0 PPM-scaled")
	}
	if strings.TrimSpace(config.ApprovedBy) == "" || len(config.ApprovedBy) > 128 || strings.TrimSpace(config.ApprovalReference) == "" || len(config.ApprovalReference) > 256 || strings.TrimSpace(config.AutomaticRollbackActor) == "" || len(config.AutomaticRollbackActor) > 128 || config.CreatedAtUTCNS <= 0 {
		return errors.New("bounded approval and automatic rollback identities are required")
	}
	return nil
}
