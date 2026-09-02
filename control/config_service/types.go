package configservice

import (
	"errors"
	"regexp"
)

const (
	// ConfigurationSchemaVersion is the immutable control configuration schema.
	ConfigurationSchemaVersion     uint32 = 1
	partsPerMillion                       = uint32(1_000_000)
	maximumConfigurationBytes             = int64(4 * 1024 * 1024)
	maximumStrategies                     = 64
	maximumVenues                         = 32
	maximumModels                         = 128
	maximumSessions                       = 128
	maximumEvents                         = 512
	maximumRoles                          = 256
	maximumKills                          = 256
	maximumEdgeOfflineAgeNS               = uint64(24 * 60 * 60 * 1_000_000_000)
	maximumAuthorizationLifetimeNS        = int64(15 * 60 * 1_000_000_000)
)

var (
	ErrAuthentication       = errors.New("control request is not authenticated")
	ErrAuthorization        = errors.New("control request is not authorized")
	ErrConfigurationInvalid = errors.New("configuration is invalid")
	ErrConfigurationStale   = errors.New("configuration is stale or expired")
	ErrHashMismatch         = errors.New("configuration hash mismatch")
	ErrInvalidTransition    = errors.New("invalid configuration workflow transition")
	ErrLiveModeUnavailable  = errors.New("live mode is unavailable in this build")
	ErrNotFound             = errors.New("configuration object was not found")
	ErrReplay               = errors.New("control request or revision was replayed")
	ErrSignature            = errors.New("configuration signature is invalid")
	ErrTwoPersonControl     = errors.New("critical change requires a distinct second approver")
	ErrUnsafeEdgeState      = errors.New("edge configuration state blocks new orders")
)

var (
	identifierPattern = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$`)
	semanticPattern   = regexp.MustCompile(`^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$`)
	hex64Pattern      = regexp.MustCompile(`^[0-9a-f]{64}$`)
)

// TradingMode is desired policy. LIVE is understood but rejected by this
// non-live implementation.
type TradingMode string

const (
	TradingModeSimulation TradingMode = "SIMULATION"
	TradingModePaper      TradingMode = "PAPER"
	TradingModeLive       TradingMode = "LIVE"
)

// OperatorRole grants one narrow control-plane capability.
type OperatorRole string

const (
	RoleViewer              OperatorRole = "VIEWER"
	RoleConfigurationAuthor OperatorRole = "CONFIGURATION_AUTHOR"
	RoleApprover            OperatorRole = "APPROVER"
	RoleActivator           OperatorRole = "ACTIVATOR"
	RoleRollbackOperator    OperatorRole = "ROLLBACK_OPERATOR"
	RoleKillOperator        OperatorRole = "KILL_OPERATOR"
)

// KillScope matches the hierarchical risk/gateway safety scopes.
type KillScope string

const (
	KillScopeFirm     KillScope = "FIRM"
	KillScopeAccount  KillScope = "ACCOUNT"
	KillScopeVenue    KillScope = "VENUE"
	KillScopeStrategy KillScope = "STRATEGY"
	KillScopeSymbol   KillScope = "SYMBOL"
)

// AuthContext is supplied only after an external authentication boundary. The
// local API validates scope, lifetime, epoch, and replay identity; it does not
// authenticate passwords or credentials itself.
type AuthContext struct {
	Authenticated      bool   `json:"authenticated"`
	Principal          string `json:"principal"`
	RequestID          string `json:"request_id"`
	AuthorityEpoch     uint64 `json:"authority_epoch"`
	AuthenticatedUTCNS int64  `json:"authenticated_utc_ns"`
	ExpiresUTCNS       int64  `json:"expires_utc_ns"`
}

type StrategyConfiguration struct {
	StrategyID              string   `json:"strategy_id"`
	Enabled                 bool     `json:"enabled"`
	MaximumOrderQuantity    uint64   `json:"maximum_order_quantity_units"`
	MaximumPositionQuantity uint64   `json:"maximum_position_quantity_units"`
	AllowedModelIDs         []string `json:"allowed_model_ids"`
	TradingSessionIDs       []string `json:"trading_session_ids"`
}

type VenueConfiguration struct {
	VenueID                    string      `json:"venue_id"`
	Enabled                    bool        `json:"enabled"`
	Mode                       TradingMode `json:"mode"`
	MaximumOrdersPerSecond     uint32      `json:"maximum_orders_per_second"`
	MaximumCancelsPerSecond    uint32      `json:"maximum_cancels_per_second"`
	RequireSelfTradePrevention bool        `json:"require_self_trade_prevention"`
}

type SymbolRiskLimit struct {
	InstrumentID                      string `json:"instrument_id"`
	Authorized                        bool   `json:"authorized"`
	Restricted                        bool   `json:"restricted"`
	MaximumOrderQuantityUnits         uint64 `json:"maximum_order_quantity_units"`
	MaximumOrderNotionalCurrencyNanos uint64 `json:"maximum_order_notional_currency_nanos"`
	MaximumAbsolutePositionUnits      uint64 `json:"maximum_absolute_position_units"`
}

type RiskLimitConfiguration struct {
	AccountID                               string            `json:"account_id"`
	MaximumGrossExposureCurrencyNanos       uint64            `json:"maximum_gross_exposure_currency_nanos"`
	MaximumAbsoluteNetExposureCurrencyNanos uint64            `json:"maximum_absolute_net_exposure_currency_nanos"`
	MaximumDailyLossCurrencyNanos           uint64            `json:"maximum_daily_loss_currency_nanos"`
	MaximumDrawdownCurrencyNanos            uint64            `json:"maximum_drawdown_currency_nanos"`
	CreditCapitalLimitCurrencyNanos         uint64            `json:"credit_capital_limit_currency_nanos"`
	MaximumConfigurationAgeNS               uint64            `json:"maximum_configuration_age_ns"`
	SymbolLimits                            []SymbolRiskLimit `json:"symbol_limits"`
}

type ModelEligibility struct {
	ModelID            string   `json:"model_id"`
	SemanticVersion    string   `json:"semantic_version"`
	Eligible           bool     `json:"eligible"`
	MaximumOODScorePPM uint32   `json:"maximum_ood_score_ppm"`
	AllowedStrategyIDs []string `json:"allowed_strategy_ids"`
}

type EnsembleCap struct {
	EnsembleID       string `json:"ensemble_id"`
	ModelID          string `json:"model_id"`
	MaximumWeightPPM uint32 `json:"maximum_weight_ppm"`
}

type TradingSession struct {
	SessionID       string `json:"session_id"`
	VenueID         string `json:"venue_id"`
	OpensWallUTCNS  int64  `json:"opens_wall_utc_ns"`
	ClosesWallUTCNS int64  `json:"closes_wall_utc_ns"`
}

type EventCalendarEntry struct {
	EventID                  string `json:"event_id"`
	EventType                string `json:"event_type"`
	ScheduledWallUTCNS       int64  `json:"scheduled_wall_utc_ns"`
	PreEventLeadNS           uint64 `json:"pre_event_lead_ns"`
	PostEventStabilizationNS uint64 `json:"post_event_stabilization_ns"`
	SourceID                 string `json:"source_id"`
}

type ConfiguredKillSwitch struct {
	KillID     string    `json:"kill_id"`
	Scope      KillScope `json:"scope"`
	TargetID   string    `json:"target_id,omitempty"`
	Engaged    bool      `json:"engaged"`
	ReasonCode string    `json:"reason_code"`
}

type OperatorRoleBinding struct {
	Principal string         `json:"principal"`
	Roles     []OperatorRole `json:"roles"`
}

// Configuration is the complete immutable administrative snapshot. Every
// execution decision consumes its hash/version through a local edge cache.
type Configuration struct {
	SchemaVersion           uint32                   `json:"schema_version"`
	ConfigurationID         string                   `json:"configuration_id"`
	Revision                uint64                   `json:"revision"`
	ParentSHA256            string                   `json:"parent_sha256,omitempty"`
	Environment             string                   `json:"environment"`
	CreatedWallUTCNS        int64                    `json:"created_wall_utc_ns"`
	ValidFromWallUTCNS      int64                    `json:"valid_from_wall_utc_ns"`
	ValidUntilWallUTCNS     int64                    `json:"valid_until_wall_utc_ns"`
	MaximumEdgeOfflineAgeNS uint64                   `json:"maximum_edge_offline_age_ns"`
	Strategies              []StrategyConfiguration  `json:"strategies"`
	Venues                  []VenueConfiguration     `json:"venues"`
	RiskLimits              []RiskLimitConfiguration `json:"risk_limits"`
	Models                  []ModelEligibility       `json:"models"`
	EnsembleCaps            []EnsembleCap            `json:"ensemble_caps"`
	TradingSessions         []TradingSession         `json:"trading_sessions"`
	EventCalendar           []EventCalendarEntry     `json:"event_calendar"`
	KillSwitches            []ConfiguredKillSwitch   `json:"kill_switches"`
	OperatorRoles           []OperatorRoleBinding    `json:"operator_roles"`
	SHA256                  string                   `json:"sha256"`
}

type SignedConfiguration struct {
	Configuration   Configuration `json:"configuration"`
	SignerKeyID     string        `json:"signer_key_id"`
	SignatureBase64 string        `json:"signature_base64"`
}

type ValidationReport struct {
	Valid                bool          `json:"valid"`
	Configuration        Configuration `json:"configuration"`
	ConfigurationVersion string        `json:"configuration_version"`
	Errors               []string      `json:"errors"`
	Warnings             []string      `json:"warnings"`
}

type ProposalState string

const (
	ProposalPending    ProposalState = "PENDING_APPROVAL"
	ProposalApproved   ProposalState = "APPROVED"
	ProposalActive     ProposalState = "ACTIVE"
	ProposalSuperseded ProposalState = "SUPERSEDED"
)

type Proposal struct {
	ProposalID          string        `json:"proposal_id"`
	ConfigurationSHA256 string        `json:"configuration_sha256"`
	Author              string        `json:"author"`
	Approver            string        `json:"approver,omitempty"`
	ActivationWallUTCNS int64         `json:"activation_wall_utc_ns"`
	State               ProposalState `json:"state"`
}

type ProposeRequest struct {
	Authorization       AuthContext         `json:"authorization"`
	Configuration       SignedConfiguration `json:"configuration"`
	ActivationWallUTCNS int64               `json:"activation_wall_utc_ns"`
	Reason              string              `json:"reason"`
}

type ApprovalRequest struct {
	Authorization AuthContext `json:"authorization"`
	ProposalID    string      `json:"proposal_id"`
	Reason        string      `json:"reason"`
}

type ActivationRequest struct {
	Authorization AuthContext `json:"authorization"`
	ProposalID    string      `json:"proposal_id"`
	Reason        string      `json:"reason"`
}

type RollbackRequest struct {
	Initiator            AuthContext `json:"initiator"`
	Approver             AuthContext `json:"approver"`
	ExpectedActiveSHA256 string      `json:"expected_active_sha256"`
	TargetSHA256         string      `json:"target_sha256"`
	Reason               string      `json:"reason"`
}

type KillCommand struct {
	SchemaVersion       uint32    `json:"schema_version"`
	CommandID           string    `json:"command_id"`
	ConfigurationSHA256 string    `json:"configuration_sha256"`
	Scope               KillScope `json:"scope"`
	TargetID            string    `json:"target_id,omitempty"`
	CommandSequence     uint64    `json:"command_sequence"`
	AuthorityEpoch      uint64    `json:"authority_epoch"`
	EffectiveWallUTCNS  int64     `json:"effective_wall_utc_ns"`
	ReasonCode          string    `json:"reason_code"`
	Engaged             bool      `json:"engaged"`
}

type SignedKillCommand struct {
	Command         KillCommand `json:"command"`
	SignerKeyID     string      `json:"signer_key_id"`
	SignatureBase64 string      `json:"signature_base64"`
}

type EmergencyKillRequest struct {
	Authorization   AuthContext `json:"authorization"`
	CommandID       string      `json:"command_id"`
	Scope           KillScope   `json:"scope"`
	TargetID        string      `json:"target_id,omitempty"`
	CommandSequence uint64      `json:"command_sequence"`
	ReasonCode      string      `json:"reason_code"`
}

type EdgeRequest struct {
	NowWallUTCNS              int64  `json:"now_wall_utc_ns"`
	NowProcessMonotonicTimeNS uint64 `json:"now_process_monotonic_time_ns"`
	AccountID                 string `json:"account_id"`
	InstrumentID              string `json:"instrument_id"`
	StrategyID                string `json:"strategy_id"`
	VenueID                   string `json:"venue_id"`
	SessionID                 string `json:"session_id"`
	ModelID                   string `json:"model_id"`
	ModelVersion              string `json:"model_version"`
}

type EdgeDecision struct {
	AllowNewOrders       bool        `json:"allow_new_orders"`
	ReasonCode           string      `json:"reason_code"`
	ConfigurationID      string      `json:"configuration_id,omitempty"`
	Revision             uint64      `json:"revision,omitempty"`
	ConfigurationVersion string      `json:"configuration_version,omitempty"`
	ConfigurationSHA256  string      `json:"configuration_sha256,omitempty"`
	Mode                 TradingMode `json:"mode,omitempty"`
}

type ServiceHealth struct {
	Healthy           bool      `json:"healthy"`
	Ready             bool      `json:"ready"`
	ReasonCode        string    `json:"reason_code"`
	ConfigurationHash string    `json:"configuration_hash,omitempty"`
	Build             BuildInfo `json:"build"`
}

// AuditEntry is the redacted public view of one verified append-only record.
type AuditEntry struct {
	Sequence          uint64   `json:"sequence"`
	EventType         string   `json:"event_type"`
	Actor             string   `json:"actor"`
	SecondActor       string   `json:"second_actor,omitempty"`
	RequestIDs        []string `json:"request_ids"`
	OccurredWallUTCNS int64    `json:"occurred_wall_utc_ns"`
	ObjectSHA256      string   `json:"object_sha256,omitempty"`
	Reason            string   `json:"reason"`
	RecordSHA256      string   `json:"record_sha256"`
}

type ControlMetricsSnapshot struct {
	AuditSequence         uint64 `json:"audit_sequence"`
	ActiveRevision        uint64 `json:"active_revision"`
	PendingProposals      uint64 `json:"pending_proposals"`
	EmergencyKillSequence uint64 `json:"emergency_kill_sequence"`
	ConfigurationSHA256   string `json:"configuration_sha256,omitempty"`
}

func validIdentifier(value string) bool { return identifierPattern.MatchString(value) }
