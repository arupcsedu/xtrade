"""Versioned fixed-point contracts for rebalance, auction, and liquidity models."""

from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass, fields, is_dataclass
from enum import IntEnum
from typing import Final, cast

from aegis_mx_intelligence.contracts import (
    ConfigurationVersion,
    FeatureSnapshotId,
    ForecastId,
    GlobalEventId,
    Identifier128,
    InstrumentId,
    ModelId,
    ModelVersion,
    SessionId,
    VenueId,
)

MARKET_SPECIALISTS_SCHEMA_VERSION: Final = "1.0.0"
PPM: Final = 1_000_000
SHA256_BYTES: Final = 32
MAX_TEXT_BYTES: Final = 128
MAX_COLLECTION_SIZE: Final = 4_096
MAX_INT64: Final = (1 << 63) - 1
MIN_INT64: Final = -(1 << 63)
MAX_ABSOLUTE_IMPACT_PPM: Final = 10_000_000


class SpecialistKind(IntEnum):
    """Related advisory specialist families."""

    INDEX_REBALANCE = 1
    AUCTION = 2
    HIDDEN_LIQUIDITY = 3


class SpecialistDecision(IntEnum):
    """Whether a detailed estimate also produced common forecast bytes."""

    ABSTAIN = 1
    PUBLISH = 2


class SpecialistReasonCode(IntEnum):
    """Stable quality, disablement, and abstention reasons."""

    SYMBOL_DISABLED = 1
    LOW_DATA_QUALITY = 2
    UNAUTHENTICATED_SOURCE = 3
    MISSING_PROVENANCE = 4
    INSUFFICIENT_HISTORY = 5
    STALE_INPUT = 6
    INVALID_REBALANCE_STATE = 7
    INVALID_AUCTION_STATE = 8
    INSUFFICIENT_LIQUIDITY_EVIDENCE = 9
    ASSUMPTION_LIMITED = 10
    REBALANCE_ACTIVE = 11


class ProvenanceKind(IntEnum):
    """Provider-neutral origin category for normalized inputs."""

    PROVIDER_ANNOUNCEMENT = 1
    INDEX_REFERENCE = 2
    ETF_DATA = 3
    HISTORICAL_AUCTION = 4
    AUCTION_FEED = 5
    VOLUME_FORECAST = 6
    REBALANCE_STATE = 7
    ORDER_BOOK = 8
    EXECUTION_FEED = 9
    VENUE_BEHAVIOR = 10
    SYNTHETIC_REPLAY = 11


class PassiveFlowAssumption(IntEnum):
    """Explicit assumption for converting weight changes into passive flow."""

    UNKNOWN_BOUNDED_TRACKING = 1
    FULL_WEIGHT_DELTA_AT_CLOSE = 2
    PARTIAL_TRACKING_AT_CLOSE = 3


class AuctionAllocationAssumption(IntEnum):
    """Explicit approximation used for auction fill estimates."""

    QUEUE_PRIORITY_UNKNOWN = 1
    PRO_RATA_APPROXIMATION = 2


class HiddenLiquidityAssumption(IntEnum):
    """Required label for inference from replenishment-like observations."""

    REPLENISHMENT_IS_INFERENTIAL = 1


class BookSide(IntEnum):
    """Resting side of displayed or inferred liquidity."""

    BID = 1
    ASK = 2


def _bounded_text(value: str, name: str, maximum: int = MAX_TEXT_BYTES) -> None:
    if not value or "\x00" in value or len(value.encode("ascii", "strict")) > maximum:
        msg = f"{name} must be nonempty bounded ASCII without NUL"
        raise ValueError(msg)


def _positive(value: int, name: str) -> None:
    if not 0 < value <= MAX_INT64:
        msg = f"{name} must be a positive signed 64-bit integer"
        raise ValueError(msg)


def _nonnegative(value: int, name: str) -> None:
    if not 0 <= value <= MAX_INT64:
        msg = f"{name} must be a nonnegative signed 64-bit integer"
        raise ValueError(msg)


def _signed(value: int, name: str) -> None:
    if not MIN_INT64 <= value <= MAX_INT64:
        msg = f"{name} is outside the signed 64-bit range"
        raise ValueError(msg)


def _ppm(value: int, name: str) -> None:
    if not 0 <= value <= PPM:
        msg = f"{name} is outside [0, 1,000,000] PPM"
        raise ValueError(msg)


def _impact(value: int, name: str) -> None:
    if not -MAX_ABSOLUTE_IMPACT_PPM <= value <= MAX_ABSOLUTE_IMPACT_PPM:
        msg = f"{name} exceeds the bounded impact PPM range"
        raise ValueError(msg)


@dataclass(frozen=True, slots=True)
class ProvenanceRecord:
    """Content-addressed source event with explicit timestamp domains."""

    source_id: str
    source_event_id: GlobalEventId
    kind: ProvenanceKind
    content_sha256: bytes
    received_wall_clock_utc_ns: int
    data_quality_score_ppm: int
    authenticated: bool
    event_wall_clock_utc_ns: int | None = None
    exchange_event_time_ns: int | None = None

    def __post_init__(self) -> None:
        """Validate identity, digest, time domains, and source quality."""
        _bounded_text(self.source_id, "provenance source_id", 64)
        if len(self.content_sha256) != SHA256_BYTES or not any(self.content_sha256):
            msg = "provenance content_sha256 must be a nonzero SHA-256"
            raise ValueError(msg)
        _positive(
            self.received_wall_clock_utc_ns,
            "provenance received_wall_clock_utc_ns",
        )
        _ppm(self.data_quality_score_ppm, "provenance data quality")
        if self.event_wall_clock_utc_ns is None and self.exchange_event_time_ns is None:
            msg = "provenance requires an event wall or exchange timestamp"
            raise ValueError(msg)
        if self.event_wall_clock_utc_ns is not None:
            _positive(
                self.event_wall_clock_utc_ns,
                "provenance event_wall_clock_utc_ns",
            )
            if self.event_wall_clock_utc_ns > self.received_wall_clock_utc_ns:
                msg = "provenance event wall time follows receipt"
                raise ValueError(msg)
        if self.exchange_event_time_ns is not None:
            _positive(
                self.exchange_event_time_ns,
                "provenance exchange_event_time_ns",
            )


@dataclass(frozen=True, slots=True)
class SymbolPolicySnapshot:
    """Immutable versioned symbol-level specialist disablement policy."""

    policy_id: GlobalEventId
    configuration_version: ConfigurationVersion
    effective_wall_clock_utc_ns: int
    disabled_instrument_ids: tuple[InstrumentId, ...]
    reason_code: str

    def __post_init__(self) -> None:
        """Validate bounded, unique disabled symbols and effective time."""
        _positive(
            self.effective_wall_clock_utc_ns,
            "symbol policy effective_wall_clock_utc_ns",
        )
        _bounded_text(self.reason_code, "symbol policy reason_code", 64)
        if len(set(self.disabled_instrument_ids)) != len(self.disabled_instrument_ids):
            msg = "symbol policy disabled instruments must be unique"
            raise ValueError(msg)

    def is_disabled(self, instrument_id: InstrumentId) -> bool:
        """Return whether the immutable policy disables one instrument."""
        return instrument_id in self.disabled_instrument_ids

    @property
    def sha256(self) -> bytes:
        """Hash the complete immutable symbol policy."""
        return hashlib.sha256(_canonical_json_bytes(self)).digest()


@dataclass(frozen=True, slots=True)
class SpecialistContext:
    """Common lineage, clocks, policy, and data quality for one evaluation."""

    event_id: GlobalEventId
    session_id: SessionId
    instrument_id: InstrumentId
    feature_snapshot_id: FeatureSnapshotId
    configuration_version: ConfigurationVersion
    as_of_wall_clock_utc_ns: int
    as_of_exchange_event_time_ns: int
    production_process_monotonic_time_ns: int
    data_quality_score_ppm: int
    provenance: tuple[ProvenanceRecord, ...]
    symbol_policy: SymbolPolicySnapshot

    def __post_init__(self) -> None:
        """Validate time, point-in-time provenance, policy, and source identity."""
        for value, name in (
            (self.as_of_wall_clock_utc_ns, "context as_of_wall_clock_utc_ns"),
            (
                self.as_of_exchange_event_time_ns,
                "context as_of_exchange_event_time_ns",
            ),
            (
                self.production_process_monotonic_time_ns,
                "context production_process_monotonic_time_ns",
            ),
        ):
            _positive(value, name)
        _ppm(self.data_quality_score_ppm, "context data quality")
        if len(self.provenance) > MAX_COLLECTION_SIZE:
            msg = "specialist provenance collection exceeds its bound"
            raise ValueError(msg)
        source_ids = {item.source_id for item in self.provenance}
        if len(source_ids) != len(self.provenance):
            msg = "specialist provenance source identifiers must be unique"
            raise ValueError(msg)
        if any(
            item.received_wall_clock_utc_ns > self.as_of_wall_clock_utc_ns
            for item in self.provenance
        ):
            msg = "specialist provenance was unavailable at the as-of cutoff"
            raise ValueError(msg)
        if (
            self.symbol_policy.effective_wall_clock_utc_ns
            > self.as_of_wall_clock_utc_ns
            or self.symbol_policy.configuration_version != self.configuration_version
        ):
            msg = "symbol policy is not effective for the specialist context"
            raise ValueError(msg)

    def require_sources(self, *source_ids: str) -> None:
        """Require every normalized input source to resolve to provenance."""
        available = {item.source_id for item in self.provenance}
        if any(item not in available for item in source_ids):
            msg = "specialist input source does not resolve to provenance"
            raise ValueError(msg)

    @property
    def sha256(self) -> bytes:
        """Hash common event, feature, configuration, policy, and source lineage."""
        return hashlib.sha256(_canonical_json_bytes(self)).digest()


@dataclass(frozen=True, slots=True)
class SignedIntRange:
    """Inclusive ordered range for signed fixed-point quantities."""

    lower: int
    upper: int

    def __post_init__(self) -> None:
        """Validate signed bounds and ordering."""
        _signed(self.lower, "signed range lower")
        _signed(self.upper, "signed range upper")
        if self.lower > self.upper:
            msg = "signed range lower exceeds upper"
            raise ValueError(msg)


@dataclass(frozen=True, slots=True)
class QuantityRange:
    """Inclusive ordered range for inferred nonnegative quantities."""

    lower_quantity: int
    upper_quantity: int

    def __post_init__(self) -> None:
        """Validate nonnegative ordered quantities."""
        _nonnegative(self.lower_quantity, "quantity range lower")
        _nonnegative(self.upper_quantity, "quantity range upper")
        if self.lower_quantity > self.upper_quantity:
            msg = "quantity range lower exceeds upper"
            raise ValueError(msg)


@dataclass(frozen=True, slots=True)
class DistributionPpm:
    """Ordered signed PPM distribution quantiles."""

    p10: int
    p50: int
    p90: int

    def __post_init__(self) -> None:
        """Validate impact bounds and quantile ordering."""
        for value, name in (
            (self.p10, "distribution p10"),
            (self.p50, "distribution p50"),
            (self.p90, "distribution p90"),
        ):
            _impact(value, name)
        if not self.p10 <= self.p50 <= self.p90:
            msg = "distribution PPM quantiles are not ordered"
            raise ValueError(msg)


@dataclass(frozen=True, slots=True)
class IndexRebalanceAnnouncement:
    """Normalized provider announcement and point-in-time flow assumptions."""

    source_id: str
    index_id: str
    revision: int
    announcement_wall_clock_utc_ns: int
    effective_auction_wall_clock_utc_ns: int
    old_weight_ppm: int
    new_weight_ppm: int
    float_adjusted_shares: int
    estimated_tracking_assets_currency_nanos: int
    tracking_assets_uncertainty_ppm: int
    passive_participation_lower_ppm: int
    passive_participation_upper_ppm: int

    def __post_init__(self) -> None:
        """Validate announcement times, weights, assets, and participation bounds."""
        _bounded_text(self.source_id, "rebalance announcement source_id", 64)
        _bounded_text(self.index_id, "rebalance index_id", 64)
        if self.revision <= 0:
            msg = "rebalance announcement revision must be positive"
            raise ValueError(msg)
        _positive(
            self.announcement_wall_clock_utc_ns,
            "rebalance announcement_wall_clock_utc_ns",
        )
        _positive(
            self.effective_auction_wall_clock_utc_ns,
            "rebalance effective_auction_wall_clock_utc_ns",
        )
        if self.effective_auction_wall_clock_utc_ns <= (
            self.announcement_wall_clock_utc_ns
        ):
            msg = "rebalance effective auction must follow announcement"
            raise ValueError(msg)
        for value, name in (
            (self.old_weight_ppm, "old index weight"),
            (self.new_weight_ppm, "new index weight"),
            (self.tracking_assets_uncertainty_ppm, "tracking assets uncertainty"),
            (self.passive_participation_lower_ppm, "passive participation lower"),
            (self.passive_participation_upper_ppm, "passive participation upper"),
        ):
            _ppm(value, name)
        _positive(self.float_adjusted_shares, "float_adjusted_shares")
        _positive(
            self.estimated_tracking_assets_currency_nanos,
            "estimated_tracking_assets_currency_nanos",
        )
        if self.passive_participation_lower_ppm > (
            self.passive_participation_upper_ppm
        ):
            msg = "passive participation lower exceeds upper"
            raise ValueError(msg)


@dataclass(frozen=True, slots=True)
class EtfRebalanceContext:
    """Observed ETF and liquidity state used by the passive-flow estimate."""

    source_id: str
    reference_price_currency_nanos: int
    etf_net_assets_currency_nanos: int
    etf_net_flow_currency_nanos: int
    average_daily_volume_shares: int
    forecast_closing_auction_volume_shares: int

    def __post_init__(self) -> None:
        """Validate explicit currency and share units."""
        _bounded_text(self.source_id, "ETF source_id", 64)
        _positive(self.reference_price_currency_nanos, "ETF reference price")
        _positive(self.etf_net_assets_currency_nanos, "ETF net assets")
        _signed(self.etf_net_flow_currency_nanos, "ETF net flow")
        _positive(self.average_daily_volume_shares, "ETF average daily volume")
        _positive(
            self.forecast_closing_auction_volume_shares,
            "ETF forecast closing auction volume",
        )


@dataclass(frozen=True, slots=True)
class HistoricalRebalanceAuction:
    """Point-in-time historical closing-auction observation."""

    source_id: str
    event_wall_clock_utc_ns: int
    available_wall_clock_utc_ns: int
    passive_demand_shares: int
    auction_volume_shares: int
    realized_impact_ppm: int
    reversed_within_window: bool

    def __post_init__(self) -> None:
        """Validate availability, quantities, and bounded realized impact."""
        _bounded_text(self.source_id, "historical rebalance source_id", 64)
        _positive(self.event_wall_clock_utc_ns, "historical rebalance event time")
        _positive(
            self.available_wall_clock_utc_ns,
            "historical rebalance available time",
        )
        if self.available_wall_clock_utc_ns < self.event_wall_clock_utc_ns:
            msg = "historical rebalance availability precedes its event"
            raise ValueError(msg)
        _signed(self.passive_demand_shares, "historical passive demand")
        _positive(self.auction_volume_shares, "historical auction volume")
        _impact(self.realized_impact_ppm, "historical realized impact")


@dataclass(frozen=True, slots=True)
class RebalanceInputBundle:
    """Announcement, ETF state, and historical context for one symbol."""

    context: SpecialistContext
    announcement: IndexRebalanceAnnouncement
    etf: EtfRebalanceContext
    history: tuple[HistoricalRebalanceAuction, ...]
    assumption: PassiveFlowAssumption
    schema_version: str = MARKET_SPECIALISTS_SCHEMA_VERSION

    def __post_init__(self) -> None:
        """Validate version, collection bound, sources, and point-in-time history."""
        if self.schema_version != MARKET_SPECIALISTS_SCHEMA_VERSION:
            msg = "unsupported rebalance input schema version"
            raise ValueError(msg)
        if len(self.history) > MAX_COLLECTION_SIZE:
            msg = "rebalance history exceeds its bound"
            raise ValueError(msg)
        self.context.require_sources(
            self.announcement.source_id,
            self.etf.source_id,
            *(item.source_id for item in self.history),
        )
        if self.announcement.announcement_wall_clock_utc_ns > (
            self.context.as_of_wall_clock_utc_ns
        ) or any(
            item.available_wall_clock_utc_ns > self.context.as_of_wall_clock_utc_ns
            for item in self.history
        ):
            msg = "rebalance input was unavailable at the as-of cutoff"
            raise ValueError(msg)

    @property
    def sha256(self) -> bytes:
        """Hash the complete immutable rebalance input."""
        return hashlib.sha256(_canonical_json_bytes(self)).digest()


@dataclass(frozen=True, slots=True)
class RebalanceEstimate:
    """Estimated passive flow, auction demand, impact, and reversal state."""

    assumption: PassiveFlowAssumption
    passive_flow_currency_nanos: SignedIntRange
    timing_start_wall_clock_utc_ns: int
    timing_end_wall_clock_utc_ns: int
    auction_demand_shares: SignedIntRange
    price_impact_distribution_ppm: DistributionPpm
    reversal_probability_ppm: int
    uncertainty_ppm: int

    def __post_init__(self) -> None:
        """Validate timing and probability scales."""
        _positive(self.timing_start_wall_clock_utc_ns, "rebalance timing start")
        _positive(self.timing_end_wall_clock_utc_ns, "rebalance timing end")
        if self.timing_end_wall_clock_utc_ns <= self.timing_start_wall_clock_utc_ns:
            msg = "rebalance timing window is not increasing"
            raise ValueError(msg)
        _ppm(self.reversal_probability_ppm, "rebalance reversal probability")
        _ppm(self.uncertainty_ppm, "rebalance uncertainty")


@dataclass(frozen=True, slots=True)
class AuctionSnapshot:
    """Observed auction state with explicit cutoff duration and prices."""

    source_id: str
    venue_id: VenueId
    signed_imbalance_quantity: int
    paired_quantity: int
    indicative_match_price_currency_nanos: int
    reference_price_currency_nanos: int
    cutoff_wall_clock_utc_ns: int
    time_to_cutoff_ns: int

    def __post_init__(self) -> None:
        """Validate signed quantity, prices, cutoff, and duration."""
        _bounded_text(self.source_id, "auction snapshot source_id", 64)
        _signed(self.signed_imbalance_quantity, "auction signed imbalance")
        _nonnegative(self.paired_quantity, "auction paired quantity")
        _positive(
            self.indicative_match_price_currency_nanos,
            "auction indicative match price",
        )
        _positive(self.reference_price_currency_nanos, "auction reference price")
        _positive(self.cutoff_wall_clock_utc_ns, "auction cutoff wall time")
        _positive(self.time_to_cutoff_ns, "auction time_to_cutoff_ns")


@dataclass(frozen=True, slots=True)
class HistoricalAuctionBehavior:
    """Point-in-time historical auction clearing and allocation observation."""

    source_id: str
    available_wall_clock_utc_ns: int
    signed_imbalance_quantity: int
    paired_quantity: int
    auction_volume_quantity: int
    clearing_return_ppm: int
    eligible_fill_probability_ppm: int

    def __post_init__(self) -> None:
        """Validate quantities, return, probability, and availability."""
        _bounded_text(self.source_id, "historical auction source_id", 64)
        _positive(
            self.available_wall_clock_utc_ns,
            "historical auction available_wall_clock_utc_ns",
        )
        _signed(self.signed_imbalance_quantity, "historical auction imbalance")
        _nonnegative(self.paired_quantity, "historical paired quantity")
        _positive(self.auction_volume_quantity, "historical auction volume")
        _impact(self.clearing_return_ppm, "historical clearing return")
        _ppm(self.eligible_fill_probability_ppm, "historical fill probability")


@dataclass(frozen=True, slots=True)
class AuctionVolumeForecast:
    """Point-in-time integer auction-volume forecast."""

    source_id: str
    forecast_quantity: int
    confidence_ppm: int
    available_wall_clock_utc_ns: int

    def __post_init__(self) -> None:
        """Validate forecast quantity, confidence, and availability."""
        _bounded_text(self.source_id, "auction volume source_id", 64)
        _positive(self.forecast_quantity, "auction forecast quantity")
        _ppm(self.confidence_ppm, "auction volume confidence")
        _positive(
            self.available_wall_clock_utc_ns,
            "auction volume available_wall_clock_utc_ns",
        )


@dataclass(frozen=True, slots=True)
class RebalanceAuctionState:
    """Optional rebalance demand state bound into an auction evaluation."""

    source_id: str
    active: bool
    demand_shares: SignedIntRange
    uncertainty_ppm: int

    def __post_init__(self) -> None:
        """Validate source identity and uncertainty."""
        _bounded_text(self.source_id, "rebalance state source_id", 64)
        _ppm(self.uncertainty_ppm, "rebalance state uncertainty")
        if not self.active and (self.demand_shares.lower or self.demand_shares.upper):
            msg = "inactive rebalance state cannot carry demand"
            raise ValueError(msg)


@dataclass(frozen=True, slots=True)
class AuctionInputBundle:
    """Current auction, history, volume forecast, and rebalance state."""

    context: SpecialistContext
    snapshot: AuctionSnapshot
    history: tuple[HistoricalAuctionBehavior, ...]
    volume_forecast: AuctionVolumeForecast
    rebalance_state: RebalanceAuctionState
    assumption: AuctionAllocationAssumption
    schema_version: str = MARKET_SPECIALISTS_SCHEMA_VERSION

    def __post_init__(self) -> None:
        """Validate version, sources, cutoff duration, and point-in-time inputs."""
        if self.schema_version != MARKET_SPECIALISTS_SCHEMA_VERSION:
            msg = "unsupported auction input schema version"
            raise ValueError(msg)
        if len(self.history) > MAX_COLLECTION_SIZE:
            msg = "auction history exceeds its bound"
            raise ValueError(msg)
        self.context.require_sources(
            self.snapshot.source_id,
            self.volume_forecast.source_id,
            self.rebalance_state.source_id,
            *(item.source_id for item in self.history),
        )
        if (
            self.snapshot.cutoff_wall_clock_utc_ns
            - (self.context.as_of_wall_clock_utc_ns)
            != self.snapshot.time_to_cutoff_ns
        ):
            msg = "auction time_to_cutoff differs from wall-clock cutoff"
            raise ValueError(msg)
        if self.volume_forecast.available_wall_clock_utc_ns > (
            self.context.as_of_wall_clock_utc_ns
        ) or any(
            item.available_wall_clock_utc_ns > self.context.as_of_wall_clock_utc_ns
            for item in self.history
        ):
            msg = "auction input was unavailable at the as-of cutoff"
            raise ValueError(msg)

    @property
    def sha256(self) -> bytes:
        """Hash the complete immutable auction input."""
        return hashlib.sha256(_canonical_json_bytes(self)).digest()


@dataclass(frozen=True, slots=True)
class ClearingPriceDistribution:
    """Ordered integer currency-nanos clearing-price quantiles."""

    p10_currency_nanos: int
    p50_currency_nanos: int
    p90_currency_nanos: int

    def __post_init__(self) -> None:
        """Validate positive ordered clearing prices."""
        for value, name in (
            (self.p10_currency_nanos, "clearing price p10"),
            (self.p50_currency_nanos, "clearing price p50"),
            (self.p90_currency_nanos, "clearing price p90"),
        ):
            _positive(value, name)
        if not (
            self.p10_currency_nanos
            <= self.p50_currency_nanos
            <= self.p90_currency_nanos
        ):
            msg = "clearing price quantiles are not ordered"
            raise ValueError(msg)


@dataclass(frozen=True, slots=True)
class AuctionEstimate:
    """Estimated clearing price, fill, conservative cap, and uncertainty."""

    assumption: AuctionAllocationAssumption
    clearing_price_distribution: ClearingPriceDistribution
    fill_probability_ppm: int
    recommended_participation_cap_quantity: int
    uncertainty_ppm: int

    def __post_init__(self) -> None:
        """Validate probability, nonnegative cap, and uncertainty."""
        _ppm(self.fill_probability_ppm, "auction fill probability")
        _nonnegative(
            self.recommended_participation_cap_quantity,
            "auction recommended participation cap",
        )
        _ppm(self.uncertainty_ppm, "auction uncertainty")


@dataclass(frozen=True, slots=True)
class DisplayedDepthObservation:
    """Observed displayed quantity at a price and resting side."""

    source_id: str
    price_ticks: int
    side: BookSide
    displayed_quantity: int
    exchange_event_time_ns: int

    def __post_init__(self) -> None:
        """Validate integer ticks, quantity, and exchange event time."""
        _bounded_text(self.source_id, "displayed depth source_id", 64)
        _positive(self.price_ticks, "displayed depth price_ticks")
        _nonnegative(self.displayed_quantity, "displayed depth quantity")
        _positive(self.exchange_event_time_ns, "displayed depth exchange event time")


@dataclass(frozen=True, slots=True)
class LiquidityExecution:
    """Observed execution against one resting side and price."""

    source_id: str
    price_ticks: int
    resting_side: BookSide
    executed_quantity: int
    exchange_event_time_ns: int

    def __post_init__(self) -> None:
        """Validate execution price, quantity, time, and provenance key."""
        _bounded_text(self.source_id, "liquidity execution source_id", 64)
        _positive(self.price_ticks, "liquidity execution price_ticks")
        _positive(self.executed_quantity, "liquidity executed quantity")
        _positive(self.exchange_event_time_ns, "execution exchange event time")


@dataclass(frozen=True, slots=True)
class ReplenishmentObservation:
    """Observed displayed-depth replenishment; not proof of an iceberg."""

    source_id: str
    price_ticks: int
    side: BookSide
    replenished_quantity: int
    delay_ns: int
    exchange_event_time_ns: int

    def __post_init__(self) -> None:
        """Validate price, quantity, delay, and exchange event time."""
        _bounded_text(self.source_id, "replenishment source_id", 64)
        _positive(self.price_ticks, "replenishment price_ticks")
        _positive(self.replenished_quantity, "replenished quantity")
        _nonnegative(self.delay_ns, "replenishment delay_ns")
        _positive(self.exchange_event_time_ns, "replenishment exchange event time")


@dataclass(frozen=True, slots=True)
class PartialFillSequence:
    """Observed partial-fill pattern at one price and resting side."""

    source_id: str
    price_ticks: int
    side: BookSide
    initial_displayed_quantity: int
    cumulative_executed_quantity: int
    replenishment_count: int
    duration_ns: int

    def __post_init__(self) -> None:
        """Validate sequence quantities, count, duration, and source."""
        _bounded_text(self.source_id, "partial fill source_id", 64)
        _positive(self.price_ticks, "partial fill price_ticks")
        _positive(self.initial_displayed_quantity, "initial displayed quantity")
        _positive(self.cumulative_executed_quantity, "cumulative executed quantity")
        _nonnegative(self.replenishment_count, "partial fill replenishment count")
        _positive(self.duration_ns, "partial fill duration_ns")


@dataclass(frozen=True, slots=True)
class VenueBehaviorPrior:
    """Point-in-time venue-specific inference prior, not protocol behavior."""

    source_id: str
    venue_id: VenueId
    baseline_replenishment_probability_ppm: int
    false_positive_probability_ppm: int
    typical_displayed_quantity: int
    maximum_latent_multiple_ppm: int

    def __post_init__(self) -> None:
        """Validate prior probabilities and positive size scales."""
        _bounded_text(self.source_id, "venue behavior source_id", 64)
        _ppm(
            self.baseline_replenishment_probability_ppm,
            "venue baseline replenishment probability",
        )
        _ppm(
            self.false_positive_probability_ppm,
            "venue false-positive probability",
        )
        _positive(self.typical_displayed_quantity, "venue typical displayed quantity")
        if self.maximum_latent_multiple_ppm < PPM:
            msg = "venue maximum latent multiple must be at least one"
            raise ValueError(msg)


@dataclass(frozen=True, slots=True)
class PriceImpactObservation:
    """Observed post-execution price impact with an explicit horizon."""

    source_id: str
    resting_side: BookSide
    executed_quantity: int
    signed_impact_ppm: int
    horizon_ns: int
    exchange_event_time_ns: int

    def __post_init__(self) -> None:
        """Validate impact, quantity, horizon, event time, and source."""
        _bounded_text(self.source_id, "price impact source_id", 64)
        _positive(self.executed_quantity, "price impact executed quantity")
        _impact(self.signed_impact_ppm, "observed price impact")
        _positive(self.horizon_ns, "price impact horizon_ns")
        _positive(self.exchange_event_time_ns, "price impact exchange event time")


@dataclass(frozen=True, slots=True)
class HiddenLiquidityInputBundle:
    """Observed displayed and execution sequence for one price/side candidate."""

    context: SpecialistContext
    venue_id: VenueId
    candidate_price_ticks: int
    candidate_side: BookSide
    displayed_depth: tuple[DisplayedDepthObservation, ...]
    executions: tuple[LiquidityExecution, ...]
    replenishments: tuple[ReplenishmentObservation, ...]
    partial_fill_sequences: tuple[PartialFillSequence, ...]
    price_impacts: tuple[PriceImpactObservation, ...]
    venue_behavior: VenueBehaviorPrior
    assumption: HiddenLiquidityAssumption = (
        HiddenLiquidityAssumption.REPLENISHMENT_IS_INFERENTIAL
    )
    schema_version: str = MARKET_SPECIALISTS_SCHEMA_VERSION

    def __post_init__(self) -> None:
        """Validate version, bounds, provenance, venue, and candidate filtering."""
        if self.schema_version != MARKET_SPECIALISTS_SCHEMA_VERSION:
            msg = "unsupported hidden-liquidity input schema version"
            raise ValueError(msg)
        _positive(self.candidate_price_ticks, "hidden-liquidity candidate price")
        collections = (
            self.displayed_depth,
            self.executions,
            self.replenishments,
            self.partial_fill_sequences,
            self.price_impacts,
        )
        if any(len(items) > MAX_COLLECTION_SIZE for items in collections):
            msg = "hidden-liquidity observation collection exceeds its bound"
            raise ValueError(msg)
        source_ids = [
            self.venue_behavior.source_id,
            *(item.source_id for items in collections for item in items),
        ]
        self.context.require_sources(*source_ids)
        if self.venue_behavior.venue_id != self.venue_id:
            msg = "hidden-liquidity venue prior references a different venue"
            raise ValueError(msg)
        if (
            any(
                item.price_ticks != self.candidate_price_ticks
                or item.side is not self.candidate_side
                for item in self.displayed_depth
            )
            or any(
                item.price_ticks != self.candidate_price_ticks
                or item.resting_side is not self.candidate_side
                for item in self.executions
            )
            or any(
                item.price_ticks != self.candidate_price_ticks
                or item.side is not self.candidate_side
                for item in self.replenishments
            )
            or any(
                item.price_ticks != self.candidate_price_ticks
                or item.side is not self.candidate_side
                for item in self.partial_fill_sequences
            )
            or any(
                item.resting_side is not self.candidate_side
                for item in self.price_impacts
            )
        ):
            msg = "hidden-liquidity observations do not match candidate price and side"
            raise ValueError(msg)

    @property
    def sha256(self) -> bytes:
        """Hash the complete immutable hidden-liquidity input."""
        return hashlib.sha256(_canonical_json_bytes(self)).digest()


@dataclass(frozen=True, slots=True)
class HiddenLiquidityEstimate:
    """Inferred iceberg, latent size, and support/resistance persistence."""

    assumption: HiddenLiquidityAssumption
    iceberg_probability_ppm: int
    latent_size_range: QuantityRange
    support_resistance_persistence_ppm: int
    confidence_ppm: int

    def __post_init__(self) -> None:
        """Validate probabilities and confidence scales."""
        _ppm(self.iceberg_probability_ppm, "iceberg probability")
        _ppm(
            self.support_resistance_persistence_ppm,
            "support/resistance persistence",
        )
        _ppm(self.confidence_ppm, "hidden-liquidity confidence")


@dataclass(frozen=True, slots=True)
class SpecialistResult[T]:
    """Content-addressed detailed result with optional common forecast bytes."""

    specialist_kind: SpecialistKind
    event_id: GlobalEventId
    instrument_id: InstrumentId
    model_id: ModelId
    model_version: ModelVersion
    input_sha256: bytes
    symbol_policy_sha256: bytes
    decision: SpecialistDecision
    reason_codes: tuple[SpecialistReasonCode, ...]
    data_quality_score_ppm: int
    uncertainty_ppm: int
    estimate: T | None
    production_process_monotonic_time_ns: int
    forecast_id: ForecastId | None
    valid_until_process_monotonic_time_ns: int | None
    forecast_contract_bytes: bytes | None
    schema_version: str = MARKET_SPECIALISTS_SCHEMA_VERSION

    def __post_init__(self) -> None:
        """Validate hashes, scores, reasons, and publish/abstain exclusivity."""
        for digest, name in (
            (self.input_sha256, "specialist input_sha256"),
            (self.symbol_policy_sha256, "specialist symbol_policy_sha256"),
        ):
            if len(digest) != SHA256_BYTES or not any(digest):
                msg = f"{name} must be a nonzero SHA-256"
                raise ValueError(msg)
        if self.schema_version != MARKET_SPECIALISTS_SCHEMA_VERSION:
            msg = "unsupported specialist result schema version"
            raise ValueError(msg)
        _ppm(self.data_quality_score_ppm, "specialist result data quality")
        _ppm(self.uncertainty_ppm, "specialist result uncertainty")
        _positive(
            self.production_process_monotonic_time_ns,
            "specialist result production_process_monotonic_time_ns",
        )
        if len(set(self.reason_codes)) != len(self.reason_codes):
            msg = "specialist result reason codes must be unique"
            raise ValueError(msg)
        forecast_values = (
            self.forecast_id,
            self.valid_until_process_monotonic_time_ns,
            self.forecast_contract_bytes,
        )
        if self.decision is SpecialistDecision.ABSTAIN:
            if self.estimate is not None or any(
                item is not None for item in forecast_values
            ):
                msg = "specialist abstention cannot carry estimate or forecast fields"
                raise ValueError(msg)
            return
        if self.estimate is None or any(item is None for item in forecast_values):
            msg = "published specialist result requires estimate and forecast fields"
            raise ValueError(msg)
        valid_until = cast("int", self.valid_until_process_monotonic_time_ns)
        forecast_bytes = cast("bytes", self.forecast_contract_bytes)
        _positive(valid_until, "specialist forecast valid-until monotonic time")
        if valid_until <= self.production_process_monotonic_time_ns:
            msg = "published specialist forecast lifetime is invalid"
            raise ValueError(msg)
        if forecast_bytes[4:8] != b"AMCR":
            msg = "published specialist result lacks a canonical ModelForecast"
            raise ValueError(msg)

    def canonical_bytes(self) -> bytes:
        """Return deterministic detailed-result bytes for replay comparison."""
        return _canonical_json_bytes(self)

    @property
    def sha256(self) -> bytes:
        """Hash the complete detailed result and common forecast bytes."""
        return hashlib.sha256(self.canonical_bytes()).digest()


def _canonical_value(value: object) -> object:
    if isinstance(value, IntEnum):
        return value.name
    if isinstance(value, Identifier128):
        return value.hex()
    if isinstance(value, bytes):
        return value.hex()
    if is_dataclass(value) and not isinstance(value, type):
        return {
            item.name: _canonical_value(getattr(value, item.name))
            for item in fields(value)
        }
    if isinstance(value, tuple):
        return [_canonical_value(item) for item in value]
    if value is None or isinstance(value, (bool, int, str)):
        return value
    msg = f"unsupported canonical market-specialist value: {type(value).__name__}"
    raise TypeError(msg)


def _canonical_json_bytes(value: object) -> bytes:
    return json.dumps(
        _canonical_value(value),
        ensure_ascii=True,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")
