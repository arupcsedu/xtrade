package configservice

import (
	"encoding/json"
	"errors"
	"fmt"
	"sort"
	"strings"
)

func canonicalizeConfiguration(configuration Configuration) Configuration {
	configuration.Strategies = append([]StrategyConfiguration(nil), configuration.Strategies...)
	for index := range configuration.Strategies {
		configuration.Strategies[index].AllowedModelIDs = sortedStrings(configuration.Strategies[index].AllowedModelIDs)
		configuration.Strategies[index].TradingSessionIDs = sortedStrings(configuration.Strategies[index].TradingSessionIDs)
	}
	sort.Slice(configuration.Strategies, func(left int, right int) bool {
		return configuration.Strategies[left].StrategyID < configuration.Strategies[right].StrategyID
	})
	configuration.Venues = append([]VenueConfiguration(nil), configuration.Venues...)
	sort.Slice(configuration.Venues, func(left int, right int) bool {
		return configuration.Venues[left].VenueID < configuration.Venues[right].VenueID
	})
	configuration.RiskLimits = append([]RiskLimitConfiguration(nil), configuration.RiskLimits...)
	for index := range configuration.RiskLimits {
		configuration.RiskLimits[index].SymbolLimits = append([]SymbolRiskLimit(nil), configuration.RiskLimits[index].SymbolLimits...)
		sort.Slice(configuration.RiskLimits[index].SymbolLimits, func(left int, right int) bool {
			return configuration.RiskLimits[index].SymbolLimits[left].InstrumentID < configuration.RiskLimits[index].SymbolLimits[right].InstrumentID
		})
	}
	sort.Slice(configuration.RiskLimits, func(left int, right int) bool {
		return configuration.RiskLimits[left].AccountID < configuration.RiskLimits[right].AccountID
	})
	configuration.Models = append([]ModelEligibility(nil), configuration.Models...)
	for index := range configuration.Models {
		configuration.Models[index].AllowedStrategyIDs = sortedStrings(configuration.Models[index].AllowedStrategyIDs)
	}
	sort.Slice(configuration.Models, func(left int, right int) bool {
		if configuration.Models[left].ModelID == configuration.Models[right].ModelID {
			return configuration.Models[left].SemanticVersion < configuration.Models[right].SemanticVersion
		}
		return configuration.Models[left].ModelID < configuration.Models[right].ModelID
	})
	configuration.EnsembleCaps = append([]EnsembleCap(nil), configuration.EnsembleCaps...)
	sort.Slice(configuration.EnsembleCaps, func(left int, right int) bool {
		leftKey := configuration.EnsembleCaps[left].EnsembleID + "\x00" + configuration.EnsembleCaps[left].ModelID
		rightKey := configuration.EnsembleCaps[right].EnsembleID + "\x00" + configuration.EnsembleCaps[right].ModelID
		return leftKey < rightKey
	})
	configuration.TradingSessions = append([]TradingSession(nil), configuration.TradingSessions...)
	sort.Slice(configuration.TradingSessions, func(left int, right int) bool {
		return configuration.TradingSessions[left].SessionID < configuration.TradingSessions[right].SessionID
	})
	configuration.EventCalendar = append([]EventCalendarEntry(nil), configuration.EventCalendar...)
	sort.Slice(configuration.EventCalendar, func(left int, right int) bool {
		return configuration.EventCalendar[left].EventID < configuration.EventCalendar[right].EventID
	})
	configuration.KillSwitches = append([]ConfiguredKillSwitch(nil), configuration.KillSwitches...)
	sort.Slice(configuration.KillSwitches, func(left int, right int) bool {
		return configuration.KillSwitches[left].KillID < configuration.KillSwitches[right].KillID
	})
	configuration.OperatorRoles = append([]OperatorRoleBinding(nil), configuration.OperatorRoles...)
	for index := range configuration.OperatorRoles {
		configuration.OperatorRoles[index].Roles = append([]OperatorRole(nil), configuration.OperatorRoles[index].Roles...)
		sort.Slice(configuration.OperatorRoles[index].Roles, func(left int, right int) bool {
			return configuration.OperatorRoles[index].Roles[left] < configuration.OperatorRoles[index].Roles[right]
		})
	}
	sort.Slice(configuration.OperatorRoles, func(left int, right int) bool {
		return configuration.OperatorRoles[left].Principal < configuration.OperatorRoles[right].Principal
	})
	return configuration
}

func sortedStrings(values []string) []string {
	result := append([]string(nil), values...)
	sort.Strings(result)
	return result
}

func configurationOrderingEqual(left Configuration, right Configuration) bool {
	left.SHA256 = ""
	right.SHA256 = ""
	leftBytes, leftErr := jsonBytes(left)
	rightBytes, rightErr := jsonBytes(right)
	return leftErr == nil && rightErr == nil && string(leftBytes) == string(rightBytes)
}

func jsonBytes(value any) ([]byte, error) { return json.Marshal(value) }

func validateConfigurationFields(configuration Configuration) []string {
	failures := make([]string, 0)
	add := func(condition bool, message string) {
		if !condition {
			failures = append(failures, message)
		}
	}
	add(configuration.SchemaVersion == ConfigurationSchemaVersion, "unsupported schema version")
	add(validIdentifier(configuration.ConfigurationID) && validIdentifier(configuration.Environment), "invalid configuration or environment identity")
	add(configuration.Revision != 0, "revision must be positive")
	add((configuration.Revision == 1 && configuration.ParentSHA256 == "") || (configuration.Revision > 1 && hex64Pattern.MatchString(configuration.ParentSHA256)), "parent hash does not match revision semantics")
	add(configuration.CreatedWallUTCNS > 0 && configuration.ValidFromWallUTCNS >= configuration.CreatedWallUTCNS && configuration.ValidUntilWallUTCNS > configuration.ValidFromWallUTCNS, "invalid explicit UTC validity interval")
	add(configuration.MaximumEdgeOfflineAgeNS > 0 && configuration.MaximumEdgeOfflineAgeNS <= maximumEdgeOfflineAgeNS, "edge offline age is outside the supported range")
	add(len(configuration.Strategies) > 0 && len(configuration.Strategies) <= maximumStrategies, "strategy count is outside the supported range")
	add(len(configuration.Venues) > 0 && len(configuration.Venues) <= maximumVenues, "venue count is outside the supported range")
	add(len(configuration.RiskLimits) > 0 && len(configuration.RiskLimits) <= maximumStrategies, "risk-limit count is outside the supported range")
	add(len(configuration.Models) > 0 && len(configuration.Models) <= maximumModels, "model count is outside the supported range")
	add(len(configuration.EnsembleCaps) > 0 && len(configuration.EnsembleCaps) <= maximumModels*4, "ensemble-cap count is outside the supported range")
	add(len(configuration.TradingSessions) > 0 && len(configuration.TradingSessions) <= maximumSessions, "session count is outside the supported range")
	add(len(configuration.EventCalendar) <= maximumEvents && len(configuration.KillSwitches) <= maximumKills && len(configuration.OperatorRoles) > 0 && len(configuration.OperatorRoles) <= maximumRoles, "calendar, kill, or role count exceeds bounds")

	strategies := make(map[string]struct{}, len(configuration.Strategies))
	for _, strategy := range configuration.Strategies {
		valid := validIdentifier(strategy.StrategyID) && uniqueIdentifiers(strategy.AllowedModelIDs) && uniqueIdentifiers(strategy.TradingSessionIDs)
		if strategy.Enabled {
			valid = valid && strategy.MaximumOrderQuantity > 0 && strategy.MaximumPositionQuantity > 0 && len(strategy.AllowedModelIDs) > 0 && len(strategy.TradingSessionIDs) > 0
		}
		add(valid, "invalid strategy "+strategy.StrategyID)
		_, duplicate := strategies[strategy.StrategyID]
		add(!duplicate, "duplicate strategy "+strategy.StrategyID)
		strategies[strategy.StrategyID] = struct{}{}
	}

	venues := make(map[string]struct{}, len(configuration.Venues))
	for _, venue := range configuration.Venues {
		if venue.Mode == TradingModeLive {
			failures = append(failures, ErrLiveModeUnavailable.Error())
		}
		validMode := venue.Mode == TradingModeSimulation || venue.Mode == TradingModePaper
		add(validIdentifier(venue.VenueID) && validMode && venue.MaximumOrdersPerSecond > 0 && venue.MaximumCancelsPerSecond > 0 && venue.RequireSelfTradePrevention, "invalid or unsafe venue "+venue.VenueID)
		_, duplicate := venues[venue.VenueID]
		add(!duplicate, "duplicate venue "+venue.VenueID)
		venues[venue.VenueID] = struct{}{}
	}

	accounts := make(map[string]struct{}, len(configuration.RiskLimits))
	instruments := make(map[string]struct{})
	for _, limits := range configuration.RiskLimits {
		valid := validIdentifier(limits.AccountID) && limits.MaximumGrossExposureCurrencyNanos > 0 && limits.MaximumAbsoluteNetExposureCurrencyNanos > 0 && limits.MaximumDailyLossCurrencyNanos > 0 && limits.MaximumDrawdownCurrencyNanos > 0 && limits.CreditCapitalLimitCurrencyNanos > 0 && limits.MaximumConfigurationAgeNS > 0 && len(limits.SymbolLimits) > 0 && len(limits.SymbolLimits) <= maximumStrategies
		add(valid, "invalid risk limits for "+limits.AccountID)
		_, duplicate := accounts[limits.AccountID]
		add(!duplicate, "duplicate risk account "+limits.AccountID)
		accounts[limits.AccountID] = struct{}{}
		seenSymbols := make(map[string]struct{}, len(limits.SymbolLimits))
		for _, symbol := range limits.SymbolLimits {
			symbolValid := validIdentifier(symbol.InstrumentID) && symbol.MaximumOrderQuantityUnits > 0 && symbol.MaximumOrderNotionalCurrencyNanos > 0 && symbol.MaximumAbsolutePositionUnits > 0 && (!symbol.Restricted || !symbol.Authorized)
			add(symbolValid, "invalid symbol risk limit "+symbol.InstrumentID)
			_, duplicate := seenSymbols[symbol.InstrumentID]
			add(!duplicate, "duplicate symbol risk limit "+symbol.InstrumentID)
			seenSymbols[symbol.InstrumentID] = struct{}{}
			instruments[symbol.InstrumentID] = struct{}{}
		}
	}

	models := make(map[string]struct{}, len(configuration.Models))
	for _, model := range configuration.Models {
		key := model.ModelID + "@" + model.SemanticVersion
		valid := validIdentifier(model.ModelID) && semanticPattern.MatchString(model.SemanticVersion) && model.MaximumOODScorePPM <= partsPerMillion && uniqueIdentifiers(model.AllowedStrategyIDs)
		if model.Eligible {
			valid = valid && len(model.AllowedStrategyIDs) > 0
		}
		for _, strategyID := range model.AllowedStrategyIDs {
			_, exists := strategies[strategyID]
			valid = valid && exists
		}
		add(valid, "invalid model eligibility "+key)
		_, duplicate := models[key]
		add(!duplicate, "duplicate model eligibility "+key)
		models[key] = struct{}{}
	}

	caps := make(map[string]struct{}, len(configuration.EnsembleCaps))
	for _, cap := range configuration.EnsembleCaps {
		key := cap.EnsembleID + "@" + cap.ModelID
		modelExists := false
		for modelKey := range models {
			if strings.HasPrefix(modelKey, cap.ModelID+"@") {
				modelExists = true
				break
			}
		}
		add(validIdentifier(cap.EnsembleID) && validIdentifier(cap.ModelID) && cap.MaximumWeightPPM > 0 && cap.MaximumWeightPPM <= partsPerMillion && modelExists, "invalid ensemble cap "+key)
		_, duplicate := caps[key]
		add(!duplicate, "duplicate ensemble cap "+key)
		caps[key] = struct{}{}
	}

	sessions := make(map[string]struct{}, len(configuration.TradingSessions))
	for _, session := range configuration.TradingSessions {
		_, venueExists := venues[session.VenueID]
		add(validIdentifier(session.SessionID) && venueExists && session.OpensWallUTCNS > 0 && session.ClosesWallUTCNS > session.OpensWallUTCNS, "invalid trading session "+session.SessionID)
		_, duplicate := sessions[session.SessionID]
		add(!duplicate, "duplicate trading session "+session.SessionID)
		sessions[session.SessionID] = struct{}{}
	}
	for _, strategy := range configuration.Strategies {
		for _, sessionID := range strategy.TradingSessionIDs {
			_, exists := sessions[sessionID]
			add(exists, "strategy references unknown session "+sessionID)
		}
		for _, modelID := range strategy.AllowedModelIDs {
			found := false
			for modelKey := range models {
				if strings.HasPrefix(modelKey, modelID+"@") {
					found = true
					break
				}
			}
			add(found, "strategy references unknown model "+modelID)
		}
	}

	events := make(map[string]struct{}, len(configuration.EventCalendar))
	for _, event := range configuration.EventCalendar {
		add(validIdentifier(event.EventID) && validIdentifier(event.EventType) && validIdentifier(event.SourceID) && event.ScheduledWallUTCNS > 0, "invalid event calendar entry "+event.EventID)
		_, duplicate := events[event.EventID]
		add(!duplicate, "duplicate calendar event "+event.EventID)
		events[event.EventID] = struct{}{}
	}

	kills := make(map[string]struct{}, len(configuration.KillSwitches))
	for _, kill := range configuration.KillSwitches {
		targetExists := true
		switch kill.Scope {
		case KillScopeAccount:
			_, targetExists = accounts[kill.TargetID]
		case KillScopeVenue:
			_, targetExists = venues[kill.TargetID]
		case KillScopeStrategy:
			_, targetExists = strategies[kill.TargetID]
		case KillScopeSymbol:
			_, targetExists = instruments[kill.TargetID]
		}
		add(validIdentifier(kill.KillID) && validKillScopeTarget(kill.Scope, kill.TargetID) && validIdentifier(kill.ReasonCode) && targetExists, "invalid configured kill "+kill.KillID)
		_, duplicate := kills[kill.KillID]
		add(!duplicate, "duplicate configured kill "+kill.KillID)
		kills[kill.KillID] = struct{}{}
	}

	roleSets := make(map[string]map[OperatorRole]struct{}, len(configuration.OperatorRoles))
	for _, binding := range configuration.OperatorRoles {
		valid := validIdentifier(binding.Principal) && len(binding.Roles) > 0 && len(binding.Roles) <= 8
		roles := make(map[OperatorRole]struct{}, len(binding.Roles))
		for _, role := range binding.Roles {
			valid = valid && validRole(role)
			_, duplicate := roles[role]
			valid = valid && !duplicate
			roles[role] = struct{}{}
		}
		_, duplicate := roleSets[binding.Principal]
		add(valid && !duplicate, "invalid or duplicate role binding "+binding.Principal)
		roleSets[binding.Principal] = roles
	}
	add(hasOperationalRoleCoverage(roleSets), "roles do not preserve two-person administration and emergency kill authority")
	return failures
}

func uniqueIdentifiers(values []string) bool {
	seen := make(map[string]struct{}, len(values))
	for _, value := range values {
		if !validIdentifier(value) {
			return false
		}
		if _, exists := seen[value]; exists {
			return false
		}
		seen[value] = struct{}{}
	}
	return true
}

func validRole(role OperatorRole) bool {
	switch role {
	case RoleViewer, RoleConfigurationAuthor, RoleApprover, RoleActivator, RoleRollbackOperator, RoleKillOperator:
		return true
	default:
		return false
	}
}

func hasOperationalRoleCoverage(bindings map[string]map[OperatorRole]struct{}) bool {
	authors := make([]string, 0)
	approvers := make([]string, 0)
	hasActivator := false
	hasRollback := false
	hasKill := false
	hasViewer := false
	for principal, roles := range bindings {
		if _, ok := roles[RoleConfigurationAuthor]; ok {
			authors = append(authors, principal)
		}
		if _, ok := roles[RoleApprover]; ok {
			approvers = append(approvers, principal)
		}
		_, activate := roles[RoleActivator]
		_, rollback := roles[RoleRollbackOperator]
		_, kill := roles[RoleKillOperator]
		_, viewer := roles[RoleViewer]
		hasActivator = hasActivator || activate
		hasRollback = hasRollback || rollback
		hasKill = hasKill || kill
		hasViewer = hasViewer || viewer
	}
	hasDistinctPair := false
	for _, author := range authors {
		for _, approver := range approvers {
			hasDistinctPair = hasDistinctPair || author != approver
		}
	}
	return hasDistinctPair && hasActivator && hasRollback && hasKill && hasViewer
}

func validateBootstrapRoles(bindings []OperatorRoleBinding) bool {
	if len(bindings) == 0 || len(bindings) > maximumRoles {
		return false
	}
	roleSets := make(map[string]map[OperatorRole]struct{}, len(bindings))
	for _, binding := range bindings {
		if !validIdentifier(binding.Principal) || len(binding.Roles) == 0 || len(binding.Roles) > 8 {
			return false
		}
		if _, exists := roleSets[binding.Principal]; exists {
			return false
		}
		roles := make(map[OperatorRole]struct{}, len(binding.Roles))
		for _, role := range binding.Roles {
			if !validRole(role) {
				return false
			}
			if _, exists := roles[role]; exists {
				return false
			}
			roles[role] = struct{}{}
		}
		roleSets[binding.Principal] = roles
	}
	return hasOperationalRoleCoverage(roleSets)
}

func validKillScopeTarget(scope KillScope, target string) bool {
	if scope == KillScopeFirm {
		return target == ""
	}
	switch scope {
	case KillScopeAccount, KillScopeVenue, KillScopeStrategy, KillScopeSymbol:
		return validIdentifier(target)
	default:
		return false
	}
}

func validateKillCommand(command KillCommand) error {
	if command.SchemaVersion != ConfigurationSchemaVersion || !validIdentifier(command.CommandID) || !hex64Pattern.MatchString(command.ConfigurationSHA256) || !validKillScopeTarget(command.Scope, command.TargetID) || command.CommandSequence == 0 || command.AuthorityEpoch == 0 || command.EffectiveWallUTCNS <= 0 || !validIdentifier(command.ReasonCode) || !command.Engaged {
		return errors.New("invalid fail-closed kill command")
	}
	return nil
}

func dryRunConfiguration(configuration Configuration) ValidationReport {
	canonical := canonicalizeConfiguration(configuration)
	canonical.SHA256 = ""
	report := ValidationReport{Configuration: canonical, Errors: validateConfigurationFields(canonical)}
	if len(report.Errors) != 0 {
		return report
	}
	digest, err := canonicalSHA256(canonical)
	if err != nil {
		report.Errors = append(report.Errors, fmt.Sprintf("hash configuration: %v", err))
		return report
	}
	canonical.SHA256 = digest
	report.Valid = true
	report.Configuration = canonical
	report.ConfigurationVersion = ConfigurationVersion(canonical)
	if len(canonical.EventCalendar) == 0 {
		report.Warnings = append(report.Warnings, "event calendar is empty")
	}
	return report
}
