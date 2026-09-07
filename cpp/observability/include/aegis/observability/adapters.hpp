#ifndef AEGIS_OBSERVABILITY_ADAPTERS_HPP
#define AEGIS_OBSERVABILITY_ADAPTERS_HPP

#include "aegis/ensemble/types.hpp"
#include "aegis/execution/router.hpp"
#include "aegis/execution/types.hpp"
#include "aegis/journal/journal.hpp"
#include "aegis/market_data/feed/types.hpp"
#include "aegis/models/types.hpp"
#include "aegis/models/validator.hpp"
#include "aegis/observability/telemetry.hpp"
#include "aegis/risk/portfolio_types.hpp"
#include "aegis/risk/types.hpp"
#include "aegis/time/clock_quality.hpp"

#include <cstdint>

namespace aegis::observability {

// Adapter return values indicate whether every point was accepted. Callers may
// sample the result, but must never alter trading behavior or retry in place.
[[nodiscard]] bool publish_feed_metrics(
    TelemetryPublisher& publisher, const market_data::feed::FeedMetrics& metrics,
    const market_data::feed::DataQualityState& quality,
    std::uint64_t now_process_monotonic_time_ns, bool book_valid) noexcept;
[[nodiscard]] bool publish_model_result(TelemetryPublisher& publisher,
                                        const models::RunResult& result,
                                        const ModelTelemetryContext& context) noexcept;
[[nodiscard]] bool
publish_ensemble_metrics(TelemetryPublisher& publisher,
                         const ensemble::EnsembleForecast& forecast) noexcept;
[[nodiscard]] bool publish_risk_metrics(TelemetryPublisher& publisher,
                                        const risk::RiskMetrics& metrics) noexcept;
[[nodiscard]] bool
publish_gateway_metrics(TelemetryPublisher& publisher,
                        const execution::GatewayMetrics& metrics) noexcept;
[[nodiscard]] bool
publish_portfolio_snapshot(TelemetryPublisher& publisher,
                           const risk::portfolio::PortfolioSnapshot& snapshot) noexcept;
[[nodiscard]] bool
publish_execution_cost(TelemetryPublisher& publisher,
                       const execution::ExecutionCostAttribution& attribution) noexcept;
[[nodiscard]] bool
publish_clock_metrics(TelemetryPublisher& publisher,
                      const time::ClockQualityMetrics& metrics) noexcept;
[[nodiscard]] bool
publish_journal_metrics(TelemetryPublisher& publisher,
                        const journal::WriterMetrics& metrics,
                        std::uint64_t consumed_journal_sequence) noexcept;
[[nodiscard]] bool
publish_infrastructure_metrics(TelemetryPublisher& publisher,
                               const InfrastructureSnapshot& snapshot) noexcept;
[[nodiscard]] bool
publish_observability_drops(TelemetryPublisher& publisher,
                            const ObservabilityDropSnapshot& snapshot) noexcept;

} // namespace aegis::observability

#endif // AEGIS_OBSERVABILITY_ADAPTERS_HPP
