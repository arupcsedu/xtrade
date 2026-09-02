package modelregistry

import (
	"fmt"
	"io"
	"sort"
	"strconv"
	"strings"
)

// DeploymentMetricsSnapshot is a bounded, immutable observability view.
// Labels come only from signed configuration or fixed enums.
type DeploymentMetricsSnapshot struct {
	DeploymentID      string
	ModelID           string
	CandidateVersion  string
	ProductionVersion string
	State             DeploymentRunState
	ObservationCount  uint64
	RollbackCount     uint64
	CanaryRejections  map[CanaryScopeReason]uint64
	Intervals         map[MarketRegime][]ConfidenceInterval
	LastTrigger       *RollbackTrigger
}

// MetricsSnapshot copies coordinator state without exposing mutable maps.
func (coordinator *DeploymentCoordinator) MetricsSnapshot() (DeploymentMetricsSnapshot, error) {
	coordinator.mu.Lock()
	defer coordinator.mu.Unlock()
	snapshot := DeploymentMetricsSnapshot{
		DeploymentID: coordinator.config.DeploymentID, ModelID: coordinator.config.ModelID,
		CandidateVersion: coordinator.config.CandidateVersion, ProductionVersion: coordinator.config.ProductionVersion,
		State: coordinator.state, ObservationCount: coordinator.observationCount, RollbackCount: coordinator.rollbackCount,
		CanaryRejections: make(map[CanaryScopeReason]uint64, len(coordinator.canaryRejections)),
		Intervals:        make(map[MarketRegime][]ConfidenceInterval, len(coordinator.config.RequiredRegimes)),
	}
	for reason, count := range coordinator.canaryRejections {
		snapshot.CanaryRejections[reason] = count
	}
	for _, regime := range coordinator.config.RequiredRegimes {
		intervals, _, err := coordinator.intervalsForRegime(regime)
		if err != nil {
			return DeploymentMetricsSnapshot{}, err
		}
		snapshot.Intervals[regime] = intervals
	}
	if coordinator.lastTrigger != nil {
		trigger := *coordinator.lastTrigger
		snapshot.LastTrigger = &trigger
	}
	return snapshot, nil
}

// WritePrometheus emits stable, low-cardinality text exposition. It performs
// no network I/O and is intended for a control-plane metrics handler.
func (coordinator *DeploymentCoordinator) WritePrometheus(writer io.Writer) error {
	snapshot, err := coordinator.MetricsSnapshot()
	if err != nil {
		return err
	}
	var output strings.Builder
	base := prometheusLabels(map[string]string{
		"deployment": snapshot.DeploymentID, "model": snapshot.ModelID,
		"candidate_version": snapshot.CandidateVersion, "production_version": snapshot.ProductionVersion,
	})
	fmt.Fprintf(&output, "# HELP aegis_model_deployment_info Signed model deployment identity and state.\n")
	fmt.Fprintf(&output, "# TYPE aegis_model_deployment_info gauge\n")
	fmt.Fprintf(&output, "aegis_model_deployment_info%s 1\n", mergePrometheusLabels(base, map[string]string{"state": string(snapshot.State)}))
	fmt.Fprintf(&output, "# TYPE aegis_model_deployment_observations_total counter\n")
	fmt.Fprintf(&output, "aegis_model_deployment_observations_total%s %d\n", base, snapshot.ObservationCount)
	fmt.Fprintf(&output, "# TYPE aegis_model_deployment_rollbacks_total counter\n")
	fmt.Fprintf(&output, "aegis_model_deployment_rollbacks_total%s %d\n", base, snapshot.RollbackCount)
	fmt.Fprintf(&output, "# TYPE aegis_model_canary_rejections_total counter\n")

	reasons := make([]string, 0, len(snapshot.CanaryRejections))
	for reason := range snapshot.CanaryRejections {
		reasons = append(reasons, string(reason))
	}
	sort.Strings(reasons)
	for _, reason := range reasons {
		labels := mergePrometheusLabels(base, map[string]string{"reason": reason})
		fmt.Fprintf(&output, "aegis_model_canary_rejections_total%s %d\n", labels, snapshot.CanaryRejections[CanaryScopeReason(reason)])
	}

	fmt.Fprintf(&output, "# TYPE aegis_model_deployment_metric_samples gauge\n")
	fmt.Fprintf(&output, "# TYPE aegis_model_deployment_metric_mean_delta gauge\n")
	fmt.Fprintf(&output, "# TYPE aegis_model_deployment_metric_ci_upper gauge\n")
	fmt.Fprintf(&output, "# TYPE aegis_model_deployment_metric_threshold gauge\n")
	fmt.Fprintf(&output, "# TYPE aegis_model_deployment_metric_triggered gauge\n")
	regimes := make([]string, 0, len(snapshot.Intervals))
	for regime := range snapshot.Intervals {
		regimes = append(regimes, string(regime))
	}
	sort.Strings(regimes)
	for _, regimeName := range regimes {
		for _, interval := range snapshot.Intervals[MarketRegime(regimeName)] {
			labels := mergePrometheusLabels(base, map[string]string{"regime": regimeName, "metric": string(interval.Metric)})
			fmt.Fprintf(&output, "aegis_model_deployment_metric_samples%s %d\n", labels, interval.SampleCount)
			fmt.Fprintf(&output, "aegis_model_deployment_metric_mean_delta%s %d\n", labels, interval.MeanDelta)
			fmt.Fprintf(&output, "aegis_model_deployment_metric_ci_upper%s %d\n", labels, interval.UpperBound)
			fmt.Fprintf(&output, "aegis_model_deployment_metric_threshold%s %d\n", labels, interval.Threshold)
			triggered := 0
			if interval.Triggered {
				triggered = 1
			}
			fmt.Fprintf(&output, "aegis_model_deployment_metric_triggered%s %d\n", labels, triggered)
		}
	}
	_, err = io.WriteString(writer, output.String())
	return err
}

func prometheusLabels(values map[string]string) string {
	keys := make([]string, 0, len(values))
	for key := range values {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	parts := make([]string, 0, len(keys))
	for _, key := range keys {
		parts = append(parts, key+"="+strconv.Quote(values[key]))
	}
	return "{" + strings.Join(parts, ",") + "}"
}

func mergePrometheusLabels(base string, extra map[string]string) string {
	if len(extra) == 0 {
		return base
	}
	trimmed := strings.TrimSuffix(strings.TrimPrefix(base, "{"), "}")
	additional := strings.TrimSuffix(strings.TrimPrefix(prometheusLabels(extra), "{"), "}")
	if trimmed == "" {
		return "{" + additional + "}"
	}
	return "{" + trimmed + "," + additional + "}"
}
