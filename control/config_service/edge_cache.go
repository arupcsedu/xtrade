package configservice

import (
	"bufio"
	"crypto/ed25519"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sync"
	"sync/atomic"
)

type EdgeCacheConfig struct {
	Root                      string
	TrustedKeys               map[string]ed25519.PublicKey
	AuthorityEpoch            uint64
	ExpectedConfigurationID   string
	ExpectedEnvironment       string
	StartupWallUTCNS          int64
	StartupProcessMonotonicNS uint64
}

type cachedConfiguration struct {
	Signed             SignedConfiguration `json:"signed_configuration"`
	InstalledWallUTCNS int64               `json:"installed_wall_utc_ns"`
}

type edgeRuntime struct {
	configuration       Configuration
	installedWallUTCNS  int64
	receivedMonotonicNS uint64
	strategies          map[string]StrategyConfiguration
	venues              map[string]VenueConfiguration
	risk                map[string]RiskLimitConfiguration
	models              map[string]map[string]ModelEligibility
	sessions            map[string]TradingSession
	emergencyKills      map[string]KillCommand
}

// EdgeCache owns the verified local copy consumed by the execution edge. Its
// Evaluate operation performs no RPC, filesystem I/O, or lock acquisition.
type EdgeCache struct {
	root           string
	trustedKeys    map[string]ed25519.PublicKey
	authorityEpoch uint64
	expectedID     string
	environment    string
	mu             sync.Mutex
	runtime        atomic.Pointer[edgeRuntime]
}

// OpenEdgeCache validates locally persisted bytes before making them visible.
func OpenEdgeCache(config EdgeCacheConfig) (*EdgeCache, error) {
	if config.Root == "" || config.AuthorityEpoch == 0 || !validIdentifier(config.ExpectedConfigurationID) || !validIdentifier(config.ExpectedEnvironment) || config.StartupWallUTCNS <= 0 {
		return nil, errors.New("edge root, identity, environment, authority epoch, and startup wall time are required")
	}
	trusted := make(map[string]ed25519.PublicKey, len(config.TrustedKeys))
	for keyID, key := range config.TrustedKeys {
		if !validIdentifier(keyID) || len(key) != ed25519.PublicKeySize {
			return nil, ErrSignature
		}
		trusted[keyID] = append(ed25519.PublicKey(nil), key...)
	}
	if len(trusted) == 0 {
		return nil, ErrSignature
	}
	root, err := filepath.Abs(config.Root)
	if err != nil {
		return nil, err
	}
	if err := os.MkdirAll(root, 0o700); err != nil {
		return nil, err
	}
	cache := &EdgeCache{root: root, trustedKeys: trusted, authorityEpoch: config.AuthorityEpoch, expectedID: config.ExpectedConfigurationID, environment: config.ExpectedEnvironment}
	kills, err := cache.loadKills()
	if err != nil {
		return nil, err
	}
	payload, err := os.ReadFile(filepath.Join(root, "active.json"))
	if errors.Is(err, os.ErrNotExist) {
		return cache, nil
	}
	if err != nil {
		return nil, err
	}
	var stored cachedConfiguration
	if err := decodeStrict(payload, &stored); err != nil {
		return nil, err
	}
	if err := VerifySignedConfiguration(stored.Signed, trusted); err != nil {
		return nil, err
	}
	if stored.Signed.Configuration.ConfigurationID != cache.expectedID || stored.Signed.Configuration.Environment != cache.environment {
		return nil, ErrInvalidTransition
	}
	if stored.InstalledWallUTCNS <= 0 || stored.InstalledWallUTCNS > config.StartupWallUTCNS {
		return nil, ErrConfigurationStale
	}
	cache.runtime.Store(buildEdgeRuntime(stored.Signed.Configuration, stored.InstalledWallUTCNS, config.StartupProcessMonotonicNS, kills))
	return cache, nil
}

// InstallConfiguration verifies, persists, and atomically publishes a newer
// signed configuration. Revision rollback is accepted only after reopening the
// cache from the authority-selected local artifact, preventing network replay
// from downgrading a running edge.
func (cache *EdgeCache) InstallConfiguration(signed SignedConfiguration, installedWallUTCNS int64, receivedMonotonicNS uint64) error {
	cache.mu.Lock()
	defer cache.mu.Unlock()
	if err := VerifySignedConfiguration(signed, cache.trustedKeys); err != nil {
		return err
	}
	configuration := signed.Configuration
	if configuration.ConfigurationID != cache.expectedID || configuration.Environment != cache.environment {
		return ErrInvalidTransition
	}
	if installedWallUTCNS < configuration.ValidFromWallUTCNS || installedWallUTCNS >= configuration.ValidUntilWallUTCNS {
		return ErrConfigurationStale
	}
	current := cache.runtime.Load()
	if current != nil {
		if configuration.ConfigurationID != current.configuration.ConfigurationID || configuration.Environment != current.configuration.Environment {
			return ErrInvalidTransition
		}
		if configuration.Revision < current.configuration.Revision || (configuration.Revision == current.configuration.Revision && configuration.SHA256 != current.configuration.SHA256) {
			return ErrReplay
		}
		// Replaying identical signed bytes must not refresh the offline-age
		// deadline. Only an authority-issued new revision can do that.
		if configuration.SHA256 == current.configuration.SHA256 {
			return nil
		}
	}
	kills := make(map[string]KillCommand)
	if current != nil {
		for key, command := range current.emergencyKills {
			kills[key] = command
		}
	} else {
		var err error
		kills, err = cache.loadKills()
		if err != nil {
			return err
		}
	}
	payload, err := json.Marshal(cachedConfiguration{Signed: signed, InstalledWallUTCNS: installedWallUTCNS})
	if err != nil {
		return err
	}
	if err := writeAtomicReplace(filepath.Join(cache.root, "active.json"), append(payload, '\n'), 0o600); err != nil {
		return err
	}
	cache.runtime.Store(buildEdgeRuntime(configuration, installedWallUTCNS, receivedMonotonicNS, kills))
	return nil
}

// ApplyEmergencyKill durably records an engage-only command before atomically
// exposing it. Command sequences and authority epochs prevent replay.
func (cache *EdgeCache) ApplyEmergencyKill(signed SignedKillCommand) error {
	cache.mu.Lock()
	defer cache.mu.Unlock()
	if err := VerifySignedKillCommand(signed, cache.trustedKeys); err != nil {
		return err
	}
	current := cache.runtime.Load()
	if current == nil || signed.Command.AuthorityEpoch != cache.authorityEpoch || signed.Command.ConfigurationSHA256 != current.configuration.SHA256 {
		return ErrInvalidTransition
	}
	key := killKey(signed.Command.Scope, signed.Command.TargetID)
	if signed.Command.CommandSequence <= maximumKillSequence(current.emergencyKills) {
		return ErrReplay
	}
	payload, err := json.Marshal(signed)
	if err != nil {
		return err
	}
	file, err := os.OpenFile(filepath.Join(cache.root, "kills.jsonl"), os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o600)
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
	kills := cloneKills(current.emergencyKills)
	kills[key] = signed.Command
	next := buildEdgeRuntime(current.configuration, current.installedWallUTCNS, current.receivedMonotonicNS, kills)
	cache.runtime.Store(next)
	return nil
}

// Evaluate is the RPC-free edge admission precheck. The deterministic C++ risk
// engine remains authoritative and must perform all order-specific checks.
func (cache *EdgeCache) Evaluate(request EdgeRequest) EdgeDecision {
	runtime := cache.runtime.Load()
	if runtime == nil {
		return EdgeDecision{ReasonCode: "CONFIGURATION_UNAVAILABLE"}
	}
	configuration := runtime.configuration
	decision := EdgeDecision{ReasonCode: "CONFIGURATION_DENIED", ConfigurationID: configuration.ConfigurationID, Revision: configuration.Revision, ConfigurationVersion: ConfigurationVersion(configuration), ConfigurationSHA256: configuration.SHA256}
	if request.NowWallUTCNS < configuration.ValidFromWallUTCNS || request.NowWallUTCNS >= configuration.ValidUntilWallUTCNS || request.NowWallUTCNS < runtime.installedWallUTCNS {
		decision.ReasonCode = "CONFIGURATION_EXPIRED_OR_CLOCK_UNSAFE"
		return decision
	}
	if request.NowProcessMonotonicTimeNS < runtime.receivedMonotonicNS {
		decision.ReasonCode = "MONOTONIC_CLOCK_REGRESSION"
		return decision
	}
	wallAge := uint64(request.NowWallUTCNS - runtime.installedWallUTCNS)
	monotonicAge := request.NowProcessMonotonicTimeNS - runtime.receivedMonotonicNS
	if wallAge > configuration.MaximumEdgeOfflineAgeNS || monotonicAge > configuration.MaximumEdgeOfflineAgeNS {
		decision.ReasonCode = "CONFIGURATION_CACHE_STALE"
		return decision
	}
	if !validIdentifier(request.AccountID) || !validIdentifier(request.InstrumentID) || !validIdentifier(request.StrategyID) || !validIdentifier(request.VenueID) || !validIdentifier(request.SessionID) || !validIdentifier(request.ModelID) || !semanticPattern.MatchString(request.ModelVersion) {
		decision.ReasonCode = "REQUEST_IDENTITY_INVALID"
		return decision
	}
	if killed(configuration.KillSwitches, runtime.emergencyKills, request) {
		decision.ReasonCode = "KILL_SWITCH_ENGAGED"
		return decision
	}
	strategy, exists := runtime.strategies[request.StrategyID]
	if !exists || !strategy.Enabled || !contains(strategy.AllowedModelIDs, request.ModelID) || !contains(strategy.TradingSessionIDs, request.SessionID) {
		decision.ReasonCode = "STRATEGY_NOT_AUTHORIZED"
		return decision
	}
	venue, exists := runtime.venues[request.VenueID]
	if !exists || !venue.Enabled {
		decision.ReasonCode = "VENUE_NOT_AUTHORIZED"
		return decision
	}
	decision.Mode = venue.Mode
	if venue.Mode == TradingModeLive || (venue.Mode != TradingModePaper && venue.Mode != TradingModeSimulation) {
		decision.ReasonCode = "LIVE_MODE_UNAVAILABLE"
		return decision
	}
	limits, exists := runtime.risk[request.AccountID]
	if !exists {
		decision.ReasonCode = "RISK_LIMITS_UNAVAILABLE"
		return decision
	}
	if limits.MaximumConfigurationAgeNS < configuration.MaximumEdgeOfflineAgeNS && (wallAge > limits.MaximumConfigurationAgeNS || monotonicAge > limits.MaximumConfigurationAgeNS) {
		decision.ReasonCode = "RISK_CONFIGURATION_STALE"
		return decision
	}
	symbolAuthorized := false
	for _, symbol := range limits.SymbolLimits {
		if symbol.InstrumentID == request.InstrumentID {
			symbolAuthorized = symbol.Authorized && !symbol.Restricted
			break
		}
	}
	if !symbolAuthorized {
		decision.ReasonCode = "SYMBOL_NOT_AUTHORIZED"
		return decision
	}
	versions, exists := runtime.models[request.ModelID]
	if !exists {
		decision.ReasonCode = "MODEL_NOT_ELIGIBLE"
		return decision
	}
	model, exists := versions[request.ModelVersion]
	if !exists || !model.Eligible || !contains(model.AllowedStrategyIDs, request.StrategyID) {
		decision.ReasonCode = "MODEL_NOT_ELIGIBLE"
		return decision
	}
	session, exists := runtime.sessions[request.SessionID]
	if !exists || session.VenueID != request.VenueID || request.NowWallUTCNS < session.OpensWallUTCNS || request.NowWallUTCNS >= session.ClosesWallUTCNS {
		decision.ReasonCode = "TRADING_SESSION_CLOSED"
		return decision
	}
	decision.AllowNewOrders = true
	decision.ReasonCode = "EDGE_CONFIGURATION_AUTHORIZED"
	return decision
}

func buildEdgeRuntime(configuration Configuration, installedWallUTCNS int64, monotonicNS uint64, kills map[string]KillCommand) *edgeRuntime {
	// Detach slice backing arrays from caller-owned signed input before the
	// verified snapshot becomes concurrently readable.
	configuration = canonicalizeConfiguration(configuration)
	runtime := &edgeRuntime{configuration: configuration, installedWallUTCNS: installedWallUTCNS, receivedMonotonicNS: monotonicNS, strategies: make(map[string]StrategyConfiguration), venues: make(map[string]VenueConfiguration), risk: make(map[string]RiskLimitConfiguration), models: make(map[string]map[string]ModelEligibility), sessions: make(map[string]TradingSession), emergencyKills: cloneKills(kills)}
	for _, item := range configuration.Strategies {
		runtime.strategies[item.StrategyID] = item
	}
	for _, item := range configuration.Venues {
		runtime.venues[item.VenueID] = item
	}
	for _, item := range configuration.RiskLimits {
		runtime.risk[item.AccountID] = item
	}
	for _, item := range configuration.Models {
		versions := runtime.models[item.ModelID]
		if versions == nil {
			versions = make(map[string]ModelEligibility)
			runtime.models[item.ModelID] = versions
		}
		versions[item.SemanticVersion] = item
	}
	for _, item := range configuration.TradingSessions {
		runtime.sessions[item.SessionID] = item
	}
	return runtime
}

func (cache *EdgeCache) loadKills() (map[string]KillCommand, error) {
	result := make(map[string]KillCommand)
	var lastSequence uint64
	file, err := os.Open(filepath.Join(cache.root, "kills.jsonl"))
	if errors.Is(err, os.ErrNotExist) {
		return result, nil
	}
	if err != nil {
		return nil, err
	}
	defer file.Close()
	scanner := bufio.NewScanner(file)
	scanner.Buffer(make([]byte, 64*1024), int(maximumConfigurationBytes))
	for scanner.Scan() {
		var signed SignedKillCommand
		if err := decodeStrict(scanner.Bytes(), &signed); err != nil {
			return nil, err
		}
		if err := VerifySignedKillCommand(signed, cache.trustedKeys); err != nil || signed.Command.AuthorityEpoch != cache.authorityEpoch {
			return nil, ErrSignature
		}
		key := killKey(signed.Command.Scope, signed.Command.TargetID)
		if signed.Command.CommandSequence <= lastSequence {
			return nil, ErrReplay
		}
		result[key] = signed.Command
		lastSequence = signed.Command.CommandSequence
	}
	if err := scanner.Err(); err != nil && !errors.Is(err, io.EOF) {
		return nil, err
	}
	return result, nil
}

func killed(configured []ConfiguredKillSwitch, emergency map[string]KillCommand, request EdgeRequest) bool {
	for _, kill := range configured {
		if kill.Engaged && killMatches(kill.Scope, kill.TargetID, request) {
			return true
		}
	}
	for _, kill := range emergency {
		if kill.Engaged && killMatches(kill.Scope, kill.TargetID, request) {
			return true
		}
	}
	return false
}

func killMatches(scope KillScope, target string, request EdgeRequest) bool {
	switch scope {
	case KillScopeFirm:
		return true
	case KillScopeAccount:
		return target == request.AccountID
	case KillScopeVenue:
		return target == request.VenueID
	case KillScopeStrategy:
		return target == request.StrategyID
	case KillScopeSymbol:
		return target == request.InstrumentID
	default:
		return true
	}
}

func killKey(scope KillScope, target string) string { return string(scope) + "\x00" + target }

func cloneKills(source map[string]KillCommand) map[string]KillCommand {
	result := make(map[string]KillCommand, len(source))
	for key, value := range source {
		result[key] = value
	}
	return result
}

func maximumKillSequence(source map[string]KillCommand) uint64 {
	var maximum uint64
	for _, command := range source {
		if command.CommandSequence > maximum {
			maximum = command.CommandSequence
		}
	}
	return maximum
}

func contains(values []string, target string) bool {
	for _, value := range values {
		if value == target {
			return true
		}
	}
	return false
}

func writeAtomicReplace(path string, payload []byte, mode os.FileMode) error {
	directory := filepath.Dir(path)
	temp, err := os.CreateTemp(directory, ".replace-*")
	if err != nil {
		return err
	}
	tempPath := temp.Name()
	defer os.Remove(tempPath) //nolint:errcheck
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
	if err := os.Rename(tempPath, path); err != nil {
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

// EdgeConfigurationHash allows metrics/readiness code to report the exact
// decision-bound hash without exposing configuration content.
func (cache *EdgeCache) EdgeConfigurationHash() string {
	runtime := cache.runtime.Load()
	if runtime == nil {
		return ""
	}
	return runtime.configuration.SHA256
}

func (cache *EdgeCache) String() string {
	return fmt.Sprintf("edge-cache(config=%s)", cache.EdgeConfigurationHash())
}
