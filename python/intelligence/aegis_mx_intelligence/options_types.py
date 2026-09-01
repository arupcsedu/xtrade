"""Versioned, fixed-point contracts for provider-neutral options analytics."""

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

OPTIONS_SCHEMA_VERSION: Final = "1.0.0"
PPM: Final = 1_000_000
NANOS_PER_CURRENCY_UNIT: Final = 1_000_000_000
NANOS_PER_DAY: Final = 86_400_000_000_000
MAX_CHAIN_SIZE: Final = 16_384
MAX_TEXT_BYTES: Final = 128
MAX_INT64: Final = (1 << 63) - 1
SHA256_BYTES: Final = 32
MINIMUM_OSI_DATE: Final = 20_000_101
MAXIMUM_OSI_DATE: Final = 20_991_231
MAXIMUM_OSI_STRIKE_MILLI: Final = 99_999_999


class OptionRight(IntEnum):
    """Contract payoff right."""

    CALL = 1
    PUT = 2


class ExerciseStyle(IntEnum):
    """Exercise style retained by the contract master."""

    EUROPEAN = 1
    AMERICAN = 2


class SettlementStyle(IntEnum):
    """Settlement convention required for correct contract interpretation."""

    PHYSICAL = 1
    CASH = 2


class QuoteValidity(IntEnum):
    """Fail-closed normalized quote classification."""

    VALID = 1
    STALE = 2
    WIDE = 3
    CROSSED = 4
    ARBITRAGE_INCONSISTENT = 5
    EXPIRED = 6
    UNSUPPORTED_STYLE = 7
    INVALID = 8


class SurfaceQuality(IntEnum):
    """Quality of a fitted surface, independent of its forecast use."""

    HEALTHY = 1
    DEGRADED = 2
    INVALID = 3


class DealerSideAssumption(IntEnum):
    """Explicit unobservable sign assumption for dealer positioning."""

    UNKNOWN_SYMMETRIC = 1
    DEALER_SHORT_CUSTOMER_POSITION = 2
    DEALER_LONG_CUSTOMER_POSITION = 3


class OptionsRegime(IntEnum):
    """Bounded options-market regime classification."""

    INSUFFICIENT = 1
    LOW_VOLATILITY = 2
    BALANCED = 3
    HIGH_VOLATILITY = 4
    EVENT_PREMIUM = 5
    STRESSED = 6


class OptionsDecision(IntEnum):
    """Whether the service produced a usable common forecast."""

    ABSTAIN = 1
    PUBLISH = 2


class OptionsReasonCode(IntEnum):
    """Stable machine-readable quality and abstention reasons."""

    NO_CONTRACTS = 1
    NO_VALID_QUOTES = 2
    STALE_QUOTES = 3
    WIDE_QUOTES = 4
    CROSSED_QUOTES = 5
    ARBITRAGE_INCONSISTENT = 6
    EXPIRED_CONTRACT = 7
    UNSUPPORTED_EXERCISE_STYLE = 8
    IV_NOT_BRACKETED = 9
    INSUFFICIENT_SURFACE = 10
    CALENDAR_ARBITRAGE = 11
    CONVEXITY_ARBITRAGE = 12
    MISSING_OPEN_INTEREST = 13
    MISSING_TRADES = 14
    CORPORATE_ACTION_MISMATCH = 15
    UNKNOWN_DEALER_SIDE = 16
    NUMERICAL_FAILURE = 17
    OOD_INPUT = 18


class OptionsServiceState(IntEnum):
    """Lifecycle state for the asynchronous options signal service."""

    STARTING = 1
    READY = 2
    DEGRADED = 3
    STOPPED = 4


def _bounded_text(value: str, name: str, maximum: int = MAX_TEXT_BYTES) -> None:
    if not value or "\x00" in value or len(value.encode("ascii", "strict")) > maximum:
        msg = f"{name} must be nonempty bounded ASCII without NUL"
        raise ValueError(msg)


def _positive(value: int, name: str) -> None:
    if not 0 < value <= MAX_INT64:
        msg = f"{name} must be a positive signed 64-bit integer"
        raise ValueError(msg)


def _ppm(value: int, name: str) -> None:
    if not 0 <= value <= PPM:
        msg = f"{name} is outside [0, 1,000,000] PPM"
        raise ValueError(msg)


@dataclass(frozen=True, slots=True, order=True)
class ParsedOptionSymbol:
    """Public OSI symbol fields; this is not a provider wire protocol."""

    root: str
    expiration_yyyymmdd: int
    right: OptionRight
    strike_currency_milli: int

    def __post_init__(self) -> None:
        """Validate normalized public symbol fields."""
        _bounded_text(self.root, "option root", 6)
        if not self.root.isalnum() or self.root != self.root.upper():
            msg = "option root must contain uppercase ASCII letters or digits"
            raise ValueError(msg)
        if not MINIMUM_OSI_DATE <= self.expiration_yyyymmdd <= MAXIMUM_OSI_DATE:
            msg = "option expiration date is outside the supported OSI range"
            raise ValueError(msg)
        if not 0 <= self.strike_currency_milli <= MAXIMUM_OSI_STRIKE_MILLI:
            msg = "option strike is outside the eight-digit OSI range"
            raise ValueError(msg)


@dataclass(frozen=True, slots=True)
class OptionContract:
    """One immutable contract-master revision with adjusted deliverable state."""

    contract_id: GlobalEventId
    option_instrument_id: InstrumentId
    underlying_instrument_id: InstrumentId
    osi_symbol: str
    root: str
    right: OptionRight
    exercise_style: ExerciseStyle
    settlement_style: SettlementStyle
    strike_currency_nanos: int
    multiplier_units: int
    expiration_wall_clock_utc_ns: int
    reference_version: int
    corporate_action_revision: int
    deliverable_units: int
    effective_wall_clock_utc_ns: int

    def __post_init__(self) -> None:
        """Validate explicit units, OSI identity, and reference-data revision."""
        _bounded_text(self.osi_symbol, "OSI symbol", 21)
        _bounded_text(self.root, "contract root", 6)
        for value, name in (
            (self.strike_currency_nanos, "strike_currency_nanos"),
            (self.multiplier_units, "multiplier_units"),
            (self.expiration_wall_clock_utc_ns, "expiration_wall_clock_utc_ns"),
            (self.reference_version, "reference_version"),
            (self.deliverable_units, "deliverable_units"),
            (self.effective_wall_clock_utc_ns, "effective_wall_clock_utc_ns"),
        ):
            _positive(value, name)
        if self.corporate_action_revision < 0:
            msg = "corporate_action_revision cannot be negative"
            raise ValueError(msg)
        if self.effective_wall_clock_utc_ns >= self.expiration_wall_clock_utc_ns:
            msg = "contract effective time must precede expiration"
            raise ValueError(msg)


@dataclass(frozen=True, slots=True)
class OptionQuote:
    """Observed two-sided quote with explicit venue and timestamp domains."""

    contract_id: GlobalEventId
    venue_id: VenueId
    bid_price_currency_nanos: int
    ask_price_currency_nanos: int
    bid_quantity_contracts: int
    ask_quantity_contracts: int
    exchange_event_time_ns: int
    nic_receive_time_ns: int
    received_wall_clock_utc_ns: int

    def __post_init__(self) -> None:
        """Validate representable quote fields without declaring it usable."""
        for value, name in (
            (self.ask_price_currency_nanos, "ask_price_currency_nanos"),
            (self.bid_quantity_contracts, "bid_quantity_contracts"),
            (self.ask_quantity_contracts, "ask_quantity_contracts"),
            (self.exchange_event_time_ns, "exchange_event_time_ns"),
            (self.nic_receive_time_ns, "nic_receive_time_ns"),
            (self.received_wall_clock_utc_ns, "received_wall_clock_utc_ns"),
        ):
            _positive(value, name)
        if not 0 <= self.bid_price_currency_nanos <= MAX_INT64:
            msg = "bid_price_currency_nanos is outside its nonnegative range"
            raise ValueError(msg)


@dataclass(frozen=True, slots=True)
class OptionTrade:
    """Observed options trade used only for volume-based features."""

    contract_id: GlobalEventId
    price_currency_nanos: int
    quantity_contracts: int
    exchange_event_time_ns: int
    received_wall_clock_utc_ns: int

    def __post_init__(self) -> None:
        """Validate positive observed trade fields."""
        for value, name in (
            (self.price_currency_nanos, "trade price_currency_nanos"),
            (self.quantity_contracts, "trade quantity_contracts"),
            (self.exchange_event_time_ns, "trade exchange_event_time_ns"),
            (self.received_wall_clock_utc_ns, "trade received_wall_clock_utc_ns"),
        ):
            _positive(value, name)


@dataclass(frozen=True, slots=True)
class OpenInterestObservation:
    """Observed contract open interest and its availability time."""

    contract_id: GlobalEventId
    open_interest_contracts: int
    as_of_wall_clock_utc_ns: int
    available_wall_clock_utc_ns: int

    def __post_init__(self) -> None:
        """Validate nonnegative OI and point-in-time availability."""
        if not 0 <= self.open_interest_contracts <= MAX_INT64:
            msg = "open interest is outside its nonnegative range"
            raise ValueError(msg)
        _positive(self.as_of_wall_clock_utc_ns, "OI as_of_wall_clock_utc_ns")
        if self.available_wall_clock_utc_ns < self.as_of_wall_clock_utc_ns:
            msg = "open-interest availability precedes its as-of time"
            raise ValueError(msg)


@dataclass(frozen=True, slots=True)
class UnderlyingObservation:
    """Observed underlying midpoint with event and receive timestamps."""

    instrument_id: InstrumentId
    midpoint_currency_nanos: int
    exchange_event_time_ns: int
    nic_receive_time_ns: int
    received_wall_clock_utc_ns: int

    def __post_init__(self) -> None:
        """Validate positive observed underlying state."""
        for value, name in (
            (self.midpoint_currency_nanos, "underlying midpoint_currency_nanos"),
            (self.exchange_event_time_ns, "underlying exchange_event_time_ns"),
            (self.nic_receive_time_ns, "underlying nic_receive_time_ns"),
            (
                self.received_wall_clock_utc_ns,
                "underlying received_wall_clock_utc_ns",
            ),
        ):
            _positive(value, name)


@dataclass(frozen=True, slots=True)
class CarryObservation:
    """Observed annualized continuously compounded rate and dividend yield."""

    risk_free_rate_ppm: int
    dividend_yield_ppm: int
    as_of_wall_clock_utc_ns: int
    available_wall_clock_utc_ns: int

    def __post_init__(self) -> None:
        """Validate bounded signed rates and point-in-time availability."""
        if not -PPM <= self.risk_free_rate_ppm <= 5 * PPM:
            msg = "risk-free rate is outside the supported PPM range"
            raise ValueError(msg)
        if not -PPM <= self.dividend_yield_ppm <= 5 * PPM:
            msg = "dividend yield is outside the supported PPM range"
            raise ValueError(msg)
        _positive(self.as_of_wall_clock_utc_ns, "carry as_of_wall_clock_utc_ns")
        if self.available_wall_clock_utc_ns < self.as_of_wall_clock_utc_ns:
            msg = "carry availability precedes its as-of time"
            raise ValueError(msg)


@dataclass(frozen=True, slots=True)
class OptionsInputBundle:
    """Immutable point-in-time options chain and common forecast provenance."""

    session_id: SessionId
    underlying_instrument_id: InstrumentId
    feature_snapshot_id: FeatureSnapshotId
    configuration_version: ConfigurationVersion
    contracts: tuple[OptionContract, ...]
    quotes: tuple[OptionQuote, ...]
    trades: tuple[OptionTrade, ...]
    open_interest: tuple[OpenInterestObservation, ...]
    underlying: UnderlyingObservation
    carry: CarryObservation
    as_of_wall_clock_utc_ns: int
    production_process_monotonic_time_ns: int
    schema_version: str = OPTIONS_SCHEMA_VERSION

    def __post_init__(self) -> None:
        """Validate bounds, referential integrity, and point-in-time state."""
        if self.schema_version != OPTIONS_SCHEMA_VERSION:
            msg = "unsupported options input schema version"
            raise ValueError(msg)
        _positive(self.as_of_wall_clock_utc_ns, "options as_of_wall_clock_utc_ns")
        _positive(
            self.production_process_monotonic_time_ns,
            "options production_process_monotonic_time_ns",
        )
        if any(
            len(values) > MAX_CHAIN_SIZE for values in (self.contracts, self.quotes)
        ):
            msg = "options contract or quote chain exceeds its bound"
            raise ValueError(msg)
        contract_ids = {item.contract_id for item in self.contracts}
        if len(contract_ids) != len(self.contracts):
            msg = "options contract identifiers must be unique"
            raise ValueError(msg)
        if (
            any(
                item.underlying_instrument_id != self.underlying_instrument_id
                for item in self.contracts
            )
            or self.underlying.instrument_id != self.underlying_instrument_id
        ):
            msg = "options bundle contains a different underlying instrument"
            raise ValueError(msg)
        observed_ids = [
            *(item.contract_id for item in self.quotes),
            *(item.contract_id for item in self.trades),
            *(item.contract_id for item in self.open_interest),
        ]
        if any(item not in contract_ids for item in observed_ids):
            msg = "options observation references an unknown contract"
            raise ValueError(msg)
        if len({item.contract_id for item in self.quotes}) != len(self.quotes):
            msg = "options bundle supports one consolidated quote per contract"
            raise ValueError(msg)
        if len({item.contract_id for item in self.open_interest}) != len(
            self.open_interest
        ):
            msg = "options open-interest contract identifiers must be unique"
            raise ValueError(msg)
        if (
            self.carry.available_wall_clock_utc_ns > self.as_of_wall_clock_utc_ns
            or self.underlying.received_wall_clock_utc_ns > self.as_of_wall_clock_utc_ns
            or any(
                item.received_wall_clock_utc_ns > self.as_of_wall_clock_utc_ns
                for item in self.quotes
            )
            or any(
                item.received_wall_clock_utc_ns > self.as_of_wall_clock_utc_ns
                for item in self.trades
            )
            or any(
                item.available_wall_clock_utc_ns > self.as_of_wall_clock_utc_ns
                for item in self.open_interest
            )
        ):
            msg = "options bundle contains information unavailable at its as-of time"
            raise ValueError(msg)

    def canonical_bytes(self) -> bytes:
        """Return deterministic canonical JSON for persistence and replay."""
        return _canonical_json_bytes(self)

    @property
    def sha256(self) -> bytes:
        """Hash the complete immutable input bundle."""
        return hashlib.sha256(self.canonical_bytes()).digest()


@dataclass(frozen=True, slots=True)
class QuoteAssessment:
    """Validation result for one observed quote."""

    contract_id: GlobalEventId
    validity: QuoteValidity
    midpoint_currency_nanos: int | None
    spread_ppm_of_underlying: int | None


@dataclass(frozen=True, slots=True)
class GreeksSnapshot:
    """Quantized European BSM Greeks with explicit scales."""

    delta_ppm: int
    gamma_per_currency_ppb: int
    vega_currency_nanos_per_vol_ppm: int
    vanna_per_vol_ppb: int
    charm_per_year_ppb: int


@dataclass(frozen=True, slots=True)
class SurfacePoint:
    """One validated observed quote mapped to implied volatility and Greeks."""

    contract_id: GlobalEventId
    expiration_wall_clock_utc_ns: int
    strike_currency_nanos: int
    right: OptionRight
    implied_volatility_ppm: int
    log_moneyness_ppm: int
    midpoint_currency_nanos: int
    greeks: GreeksSnapshot


@dataclass(frozen=True, slots=True)
class VolatilitySurface:
    """Deterministic sorted surface points and explicit arbitrage quality."""

    points: tuple[SurfacePoint, ...]
    quality: SurfaceQuality
    reason_codes: tuple[OptionsReasonCode, ...]


@dataclass(frozen=True, slots=True)
class HedgingPressureDistribution:
    """Assumption-conditioned sign distribution for inferred hedging pressure."""

    negative_ppm: int
    neutral_ppm: int
    positive_ppm: int

    def __post_init__(self) -> None:
        """Require an exact fixed-point probability distribution."""
        values = (self.negative_ppm, self.neutral_ppm, self.positive_ppm)
        if any(not 0 <= item <= PPM for item in values) or sum(values) != PPM:
            msg = "hedging-pressure probabilities must sum to 1,000,000 PPM"
            raise ValueError(msg)


@dataclass(frozen=True, slots=True)
class DealerPressureEstimate:
    """Clearly inferred exposure summary; dealer inventory is not observed."""

    assumption: DealerSideAssumption
    assumption_uncertainty_ppm: int
    gross_gamma_exposure_currency_nanos: int
    gross_vanna_exposure_currency_nanos: int
    gross_charm_exposure_currency_nanos: int
    signed_gamma_exposure_currency_nanos: int
    signed_vanna_exposure_currency_nanos: int
    signed_charm_exposure_currency_nanos: int
    distribution: HedgingPressureDistribution

    def __post_init__(self) -> None:
        """Validate the uncertainty scale."""
        _ppm(self.assumption_uncertainty_ppm, "dealer assumption uncertainty")


@dataclass(frozen=True, slots=True)
class OptionsAnalyticsResult:
    """Detailed options-regime output with optional common forecast bytes."""

    model_id: ModelId
    model_version: ModelVersion
    input_sha256: bytes
    decision: OptionsDecision
    regime: OptionsRegime
    quote_assessments: tuple[QuoteAssessment, ...]
    surface: VolatilitySurface
    dealer_pressure: DealerPressureEstimate
    expected_volatility_ppm: int | None
    option_implied_move_ppm: int | None
    skew_ppm: int | None
    term_structure_slope_ppm: int | None
    unusual_volume_score_ppm: int | None
    zero_dte_concentration_ppm: int | None
    expected_pinning_pressure_ppm: int | None
    confidence_ppm: int
    ood_score_ppm: int
    reason_codes: tuple[OptionsReasonCode, ...]
    production_process_monotonic_time_ns: int
    forecast_id: ForecastId | None
    valid_until_process_monotonic_time_ns: int | None
    forecast_contract_bytes: bytes | None
    schema_version: str = OPTIONS_SCHEMA_VERSION

    def __post_init__(self) -> None:
        """Validate bounds and publish/abstain exclusivity."""
        if len(self.input_sha256) != SHA256_BYTES or not any(self.input_sha256):
            msg = "options result requires a nonzero input SHA-256"
            raise ValueError(msg)
        if self.schema_version != OPTIONS_SCHEMA_VERSION:
            msg = "unsupported options result schema version"
            raise ValueError(msg)
        _ppm(self.confidence_ppm, "options confidence")
        _ppm(self.ood_score_ppm, "options OOD score")
        for value, name in (
            (self.expected_volatility_ppm, "expected volatility"),
            (self.option_implied_move_ppm, "option implied move"),
            (self.unusual_volume_score_ppm, "unusual volume score"),
            (self.zero_dte_concentration_ppm, "zero-DTE concentration"),
            (self.expected_pinning_pressure_ppm, "expected pinning pressure"),
        ):
            if value is not None:
                _ppm(value, name)
        if len(set(self.reason_codes)) != len(self.reason_codes):
            msg = "options result reason codes must be unique"
            raise ValueError(msg)
        _positive(
            self.production_process_monotonic_time_ns,
            "result production_process_monotonic_time_ns",
        )
        forecast_values = (
            self.expected_volatility_ppm,
            self.forecast_id,
            self.valid_until_process_monotonic_time_ns,
            self.forecast_contract_bytes,
        )
        if self.decision is OptionsDecision.ABSTAIN:
            if any(item is not None for item in forecast_values):
                msg = "options abstention cannot carry forecast fields"
                raise ValueError(msg)
            return
        if any(item is None for item in forecast_values):
            msg = "published options result requires all forecast fields"
            raise ValueError(msg)
        valid_until = cast("int", self.valid_until_process_monotonic_time_ns)
        forecast_bytes = cast("bytes", self.forecast_contract_bytes)
        if valid_until <= self.production_process_monotonic_time_ns:
            msg = "published options forecast lifetime is invalid"
            raise ValueError(msg)
        if forecast_bytes[4:8] != b"AMCR":
            msg = "published options result lacks a canonical ModelForecast"
            raise ValueError(msg)

    def canonical_bytes(self) -> bytes:
        """Return deterministic detailed-result bytes for replay comparison."""
        return _canonical_json_bytes(self)

    @property
    def sha256(self) -> bytes:
        """Hash the complete detailed analytics result."""
        return hashlib.sha256(self.canonical_bytes()).digest()


@dataclass(slots=True)
class OptionsServiceMetrics:
    """Fixed-cardinality signal-service counters."""

    requests_total: int = 0
    publications_total: int = 0
    abstentions_total: int = 0
    numerical_failures_total: int = 0
    rejected_after_shutdown_total: int = 0


@dataclass(frozen=True, slots=True)
class OptionsServiceStatus:
    """Common service build, health, readiness, and configuration surface."""

    state: OptionsServiceState
    healthy: bool
    ready: bool
    version: str
    configuration_sha256: str
    live_trading_capable: bool


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
    msg = f"unsupported canonical options value: {type(value).__name__}"
    raise TypeError(msg)


def _canonical_json_bytes(value: object) -> bytes:
    return json.dumps(
        _canonical_value(value),
        ensure_ascii=True,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")
