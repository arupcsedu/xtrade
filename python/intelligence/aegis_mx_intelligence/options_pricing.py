"""Deterministic parsing, validation, pricing, and surface primitives."""

from __future__ import annotations

import math
from dataclasses import dataclass
from datetime import UTC, date, datetime
from itertools import pairwise
from typing import Final

from aegis_mx_intelligence.options_types import (
    NANOS_PER_CURRENCY_UNIT,
    NANOS_PER_DAY,
    PPM,
    CarryObservation,
    ExerciseStyle,
    GreeksSnapshot,
    OptionContract,
    OptionQuote,
    OptionRight,
    OptionsReasonCode,
    ParsedOptionSymbol,
    QuoteAssessment,
    QuoteValidity,
    SurfacePoint,
    SurfaceQuality,
    VolatilitySurface,
)

YEAR_NANOSECONDS: Final = 365 * NANOS_PER_DAY
MINIMUM_VOLATILITY_PPM: Final = 1
MAXIMUM_VOLATILITY_PPM: Final = 5_000_000
OSI_SYMBOL_LENGTH: Final = 21


class OptionsNumericalError(ValueError):
    """Fail-closed option pricing, inversion, or surface error."""


class OptionSymbolParser:
    """Strict parser for the public 21-character OSI symbol representation."""

    @staticmethod
    def parse(symbol: str) -> ParsedOptionSymbol:
        """Parse root, YYMMDD, right, and strike-in-thousandths."""
        if len(symbol) != OSI_SYMBOL_LENGTH or not symbol.isascii():
            msg = "OSI symbol must be exactly 21 ASCII characters"
            raise ValueError(msg)
        root_field = symbol[:6]
        if root_field != root_field.rstrip().ljust(6):
            msg = "OSI root padding must consist only of trailing spaces"
            raise ValueError(msg)
        root = root_field.rstrip()
        expiration_field = symbol[6:12]
        right_field = symbol[12]
        strike_field = symbol[13:21]
        if not expiration_field.isdigit() or not strike_field.isdigit():
            msg = "OSI expiration and strike fields must be decimal digits"
            raise ValueError(msg)
        right = {"C": OptionRight.CALL, "P": OptionRight.PUT}.get(right_field)
        if right is None:
            msg = "OSI right field must be C or P"
            raise ValueError(msg)
        year = 2000 + int(expiration_field[:2])
        month = int(expiration_field[2:4])
        day = int(expiration_field[4:6])
        try:
            date(year, month, day)
        except ValueError as exc:
            msg = "OSI expiration is not a valid calendar date"
            raise ValueError(msg) from exc
        return ParsedOptionSymbol(
            root=root,
            expiration_yyyymmdd=year * 10_000 + month * 100 + day,
            right=right,
            strike_currency_milli=int(strike_field),
        )


class ContractMaster:
    """In-memory immutable revision index with corporate-action checks."""

    def __init__(self) -> None:
        """Create an empty provider-neutral contract revision index."""
        self._by_contract: dict[object, OptionContract] = {}
        self._by_instrument: dict[object, OptionContract] = {}

    def add(self, contract: OptionContract) -> None:
        """Add an idempotent revision or reject inconsistent lineage."""
        parsed = OptionSymbolParser.parse(contract.osi_symbol)
        expiration_date = datetime.fromtimestamp(
            contract.expiration_wall_clock_utc_ns / 1_000_000_000,
            tz=UTC,
        ).date()
        expiration_yyyymmdd = (
            expiration_date.year * 10_000
            + expiration_date.month * 100
            + expiration_date.day
        )
        if (
            parsed.root != contract.root
            or parsed.right is not contract.right
            or parsed.strike_currency_milli * 1_000_000
            != contract.strike_currency_nanos
            or parsed.expiration_yyyymmdd != expiration_yyyymmdd
        ):
            msg = "contract fields disagree with its public OSI symbol"
            raise ValueError(msg)
        known_id = self._by_contract.get(contract.contract_id)
        if known_id is not None:
            if known_id != contract:
                msg = "contract identifier cannot be reused for different content"
                raise ValueError(msg)
            return
        previous = self._by_instrument.get(contract.option_instrument_id)
        if previous is not None and (
            contract.reference_version <= previous.reference_version
            or contract.effective_wall_clock_utc_ns
            <= previous.effective_wall_clock_utc_ns
            or contract.corporate_action_revision < previous.corporate_action_revision
        ):
            msg = "contract revision or corporate-action lineage is not increasing"
            raise ValueError(msg)
        self._by_contract[contract.contract_id] = contract
        self._by_instrument[contract.option_instrument_id] = contract

    def get(self, contract_id: object) -> OptionContract:
        """Return a known contract or fail closed."""
        try:
            return self._by_contract[contract_id]
        except KeyError as exc:
            msg = "unknown option contract identifier"
            raise KeyError(msg) from exc

    @property
    def size(self) -> int:
        """Return the bounded number of immutable contract revisions."""
        return len(self._by_contract)


def _normal_cdf(value: float) -> float:
    return 0.5 * (1.0 + math.erf(value / math.sqrt(2.0)))


def _normal_pdf(value: float) -> float:
    return math.exp(-0.5 * value * value) / math.sqrt(2.0 * math.pi)


def _time_years(expiration_ns: int, as_of_ns: int) -> float:
    remaining = expiration_ns - as_of_ns
    if remaining <= 0:
        msg = "option is expired at the requested as-of time"
        raise OptionsNumericalError(msg)
    return remaining / YEAR_NANOSECONDS


def _bsm_values(
    right: OptionRight,
    spot: float,
    strike: float,
    time_years: float,
    rate: float,
    dividend: float,
    volatility: float,
) -> tuple[float, float, float, float]:
    values = (spot, strike, time_years, rate, dividend, volatility)
    if (
        any(not math.isfinite(item) for item in values)
        or spot <= 0.0
        or strike <= 0.0
        or time_years <= 0.0
        or volatility <= 0.0
    ):
        msg = "Black-Scholes-Merton inputs are nonfinite or outside their domain"
        raise OptionsNumericalError(msg)
    root_time = math.sqrt(time_years)
    d1 = (
        math.log(spot / strike)
        + (rate - dividend + 0.5 * volatility * volatility) * time_years
    ) / (volatility * root_time)
    d2 = d1 - volatility * root_time
    discount_rate = math.exp(-rate * time_years)
    discount_dividend = math.exp(-dividend * time_years)
    if right is OptionRight.CALL:
        price = spot * discount_dividend * _normal_cdf(d1) - strike * (
            discount_rate * _normal_cdf(d2)
        )
        delta = discount_dividend * _normal_cdf(d1)
    else:
        price = strike * discount_rate * _normal_cdf(-d2) - spot * (
            discount_dividend * _normal_cdf(-d1)
        )
        delta = discount_dividend * (_normal_cdf(d1) - 1.0)
    gamma = discount_dividend * _normal_pdf(d1) / (spot * volatility * root_time)
    vega = spot * discount_dividend * _normal_pdf(d1) * root_time
    if any(not math.isfinite(item) for item in (price, delta, gamma, vega)):
        msg = "Black-Scholes-Merton output is nonfinite"
        raise OptionsNumericalError(msg)
    return price, delta, gamma, vega


def black_scholes_price_currency_nanos(
    contract: OptionContract,
    spot_currency_nanos: int,
    carry: CarryObservation,
    as_of_wall_clock_utc_ns: int,
    volatility_ppm: int,
) -> int:
    """Return a quantized European BSM theoretical price."""
    if contract.exercise_style is not ExerciseStyle.EUROPEAN:
        msg = "the first pricing slice supports European exercise only"
        raise OptionsNumericalError(msg)
    if volatility_ppm <= 0:
        msg = "volatility_ppm must be positive"
        raise OptionsNumericalError(msg)
    price, _, _, _ = _bsm_values(
        contract.right,
        spot_currency_nanos / NANOS_PER_CURRENCY_UNIT,
        contract.strike_currency_nanos / NANOS_PER_CURRENCY_UNIT,
        _time_years(contract.expiration_wall_clock_utc_ns, as_of_wall_clock_utc_ns),
        carry.risk_free_rate_ppm / PPM,
        carry.dividend_yield_ppm / PPM,
        volatility_ppm / PPM,
    )
    return max(0, round(price * NANOS_PER_CURRENCY_UNIT))


def arbitrage_bounds_currency_nanos(
    contract: OptionContract,
    spot_currency_nanos: int,
    carry: CarryObservation,
    as_of_wall_clock_utc_ns: int,
) -> tuple[int, int]:
    """Return discounted European lower and upper no-arbitrage bounds."""
    time_years = _time_years(
        contract.expiration_wall_clock_utc_ns,
        as_of_wall_clock_utc_ns,
    )
    spot = spot_currency_nanos / NANOS_PER_CURRENCY_UNIT
    strike = contract.strike_currency_nanos / NANOS_PER_CURRENCY_UNIT
    discounted_spot = spot * math.exp(-(carry.dividend_yield_ppm / PPM) * time_years)
    discounted_strike = strike * math.exp(
        -(carry.risk_free_rate_ppm / PPM) * time_years
    )
    if contract.right is OptionRight.CALL:
        lower = max(0.0, discounted_spot - discounted_strike)
        upper = discounted_spot
    else:
        lower = max(0.0, discounted_strike - discounted_spot)
        upper = discounted_strike
    return (
        max(0, round(lower * NANOS_PER_CURRENCY_UNIT)),
        max(0, round(upper * NANOS_PER_CURRENCY_UNIT)),
    )


@dataclass(frozen=True, slots=True)
class QuoteValidationConfig:
    """Fixed-point freshness and spread thresholds."""

    stale_after_ns: int = 2_000_000_000
    maximum_spread_ppm_of_underlying: int = 50_000
    arbitrage_tolerance_currency_nanos: int = 1_000

    def __post_init__(self) -> None:
        """Validate positive bounded thresholds."""
        if self.stale_after_ns <= 0 or self.arbitrage_tolerance_currency_nanos < 0:
            msg = "quote freshness and arbitrage thresholds are invalid"
            raise ValueError(msg)
        if not 0 < self.maximum_spread_ppm_of_underlying <= PPM:
            msg = "maximum quote spread must be in (0, 1,000,000] PPM"
            raise ValueError(msg)


class QuoteValidator:
    """Classify quote freshness, market shape, style, and arbitrage bounds."""

    def __init__(self, config: QuoteValidationConfig) -> None:
        """Bind immutable quote-quality thresholds."""
        self._config = config

    def assess(
        self,
        contract: OptionContract,
        quote: OptionQuote,
        spot_currency_nanos: int,
        carry: CarryObservation,
        as_of_wall_clock_utc_ns: int,
    ) -> QuoteAssessment:
        """Return one fail-closed quote assessment."""
        if quote.contract_id != contract.contract_id:
            return QuoteAssessment(
                contract.contract_id, QuoteValidity.INVALID, None, None
            )
        if contract.exercise_style is not ExerciseStyle.EUROPEAN:
            return QuoteAssessment(
                contract.contract_id,
                QuoteValidity.UNSUPPORTED_STYLE,
                None,
                None,
            )
        if as_of_wall_clock_utc_ns >= contract.expiration_wall_clock_utc_ns:
            return QuoteAssessment(
                contract.contract_id, QuoteValidity.EXPIRED, None, None
            )
        if (
            quote.received_wall_clock_utc_ns > as_of_wall_clock_utc_ns
            or as_of_wall_clock_utc_ns - quote.received_wall_clock_utc_ns
            > self._config.stale_after_ns
        ):
            return QuoteAssessment(
                contract.contract_id, QuoteValidity.STALE, None, None
            )
        if quote.bid_price_currency_nanos > quote.ask_price_currency_nanos:
            return QuoteAssessment(
                contract.contract_id, QuoteValidity.CROSSED, None, None
            )
        midpoint = (
            quote.bid_price_currency_nanos + quote.ask_price_currency_nanos
        ) // 2
        spread_ppm = (
            (quote.ask_price_currency_nanos - quote.bid_price_currency_nanos) * PPM
        ) // spot_currency_nanos
        if spread_ppm > self._config.maximum_spread_ppm_of_underlying:
            return QuoteAssessment(
                contract.contract_id,
                QuoteValidity.WIDE,
                midpoint,
                spread_ppm,
            )
        lower, upper = arbitrage_bounds_currency_nanos(
            contract,
            spot_currency_nanos,
            carry,
            as_of_wall_clock_utc_ns,
        )
        tolerance = self._config.arbitrage_tolerance_currency_nanos
        if midpoint < lower - tolerance or midpoint > upper + tolerance:
            return QuoteAssessment(
                contract.contract_id,
                QuoteValidity.ARBITRAGE_INCONSISTENT,
                midpoint,
                spread_ppm,
            )
        return QuoteAssessment(
            contract.contract_id,
            QuoteValidity.VALID,
            midpoint,
            spread_ppm,
        )


@dataclass(frozen=True, slots=True)
class ImpliedVolatilityConfig:
    """Bounds and deterministic termination rules for IV inversion."""

    maximum_iterations: int = 96
    price_tolerance_currency_nanos: int = 1_000
    volatility_tolerance_ppm: int = 1
    maximum_volatility_ppm: int = MAXIMUM_VOLATILITY_PPM

    def __post_init__(self) -> None:
        """Validate bounded solver controls."""
        if (
            self.maximum_iterations <= 0
            or self.price_tolerance_currency_nanos < 0
            or self.volatility_tolerance_ppm <= 0
            or self.maximum_volatility_ppm <= MINIMUM_VOLATILITY_PPM
        ):
            msg = "implied-volatility solver configuration is invalid"
            raise ValueError(msg)


class ImpliedVolatilitySolver:
    """Bracketed bisection solver with finite, bounded iteration count."""

    def __init__(self, config: ImpliedVolatilityConfig) -> None:
        """Bind immutable solver brackets and termination controls."""
        self._config = config

    def solve(
        self,
        contract: OptionContract,
        midpoint_currency_nanos: int,
        spot_currency_nanos: int,
        carry: CarryObservation,
        as_of_wall_clock_utc_ns: int,
    ) -> int:
        """Invert a validated European option midpoint to integer volatility PPM."""
        lower_bound, upper_bound = arbitrage_bounds_currency_nanos(
            contract,
            spot_currency_nanos,
            carry,
            as_of_wall_clock_utc_ns,
        )
        tolerance = self._config.price_tolerance_currency_nanos
        if (
            not lower_bound - tolerance
            <= midpoint_currency_nanos
            <= upper_bound + tolerance
        ):
            msg = "option price is outside its European arbitrage bounds"
            raise OptionsNumericalError(msg)
        if abs(midpoint_currency_nanos - lower_bound) <= tolerance:
            return 0
        low = MINIMUM_VOLATILITY_PPM
        high = self._config.maximum_volatility_ppm
        low_price = black_scholes_price_currency_nanos(
            contract, spot_currency_nanos, carry, as_of_wall_clock_utc_ns, low
        )
        high_price = black_scholes_price_currency_nanos(
            contract, spot_currency_nanos, carry, as_of_wall_clock_utc_ns, high
        )
        if (
            midpoint_currency_nanos < low_price - tolerance
            or midpoint_currency_nanos > (high_price + tolerance)
        ):
            msg = "option price has no implied volatility in the configured bracket"
            raise OptionsNumericalError(msg)
        for _ in range(self._config.maximum_iterations):
            middle = (low + high) // 2
            price = black_scholes_price_currency_nanos(
                contract,
                spot_currency_nanos,
                carry,
                as_of_wall_clock_utc_ns,
                middle,
            )
            if (
                abs(price - midpoint_currency_nanos) <= tolerance
                or high - low <= self._config.volatility_tolerance_ppm
            ):
                return middle
            if price < midpoint_currency_nanos:
                low = middle + 1
            else:
                high = middle - 1
        msg = "implied-volatility solver exceeded its deterministic iteration bound"
        raise OptionsNumericalError(msg)


class GreeksEngine:
    """Analytic BSM delta/gamma/vega plus bounded vanna/charm differences."""

    _VOLATILITY_BUMP_PPM: Final = 100
    _TIME_BUMP_NS: Final = 60_000_000_000

    def calculate(
        self,
        contract: OptionContract,
        spot_currency_nanos: int,
        carry: CarryObservation,
        as_of_wall_clock_utc_ns: int,
        volatility_ppm: int,
    ) -> GreeksSnapshot:
        """Calculate and immediately quantize all supported Greeks."""
        volatility = volatility_ppm / PPM
        time_years = _time_years(
            contract.expiration_wall_clock_utc_ns,
            as_of_wall_clock_utc_ns,
        )
        spot = spot_currency_nanos / NANOS_PER_CURRENCY_UNIT
        strike = contract.strike_currency_nanos / NANOS_PER_CURRENCY_UNIT
        rate = carry.risk_free_rate_ppm / PPM
        dividend = carry.dividend_yield_ppm / PPM
        _, delta, gamma, vega = _bsm_values(
            contract.right,
            spot,
            strike,
            time_years,
            rate,
            dividend,
            volatility,
        )
        vol_bump = min(
            self._VOLATILITY_BUMP_PPM / PPM,
            max(volatility / 2.0, 1.0 / PPM),
        )
        _, delta_up, _, _ = _bsm_values(
            contract.right,
            spot,
            strike,
            time_years,
            rate,
            dividend,
            volatility + vol_bump,
        )
        _, delta_down, _, _ = _bsm_values(
            contract.right,
            spot,
            strike,
            time_years,
            rate,
            dividend,
            max(volatility - vol_bump, 1.0 / PPM),
        )
        vanna = (delta_up - delta_down) / (
            volatility + vol_bump - max(volatility - vol_bump, 1.0 / PPM)
        )
        time_bump = min(
            self._TIME_BUMP_NS / YEAR_NANOSECONDS,
            time_years / 2.0,
        )
        _, delta_longer, _, _ = _bsm_values(
            contract.right,
            spot,
            strike,
            time_years + time_bump,
            rate,
            dividend,
            volatility,
        )
        _, delta_shorter, _, _ = _bsm_values(
            contract.right,
            spot,
            strike,
            time_years - time_bump,
            rate,
            dividend,
            volatility,
        )
        charm = -(delta_longer - delta_shorter) / (2.0 * time_bump)
        return GreeksSnapshot(
            delta_ppm=round(delta * PPM),
            gamma_per_currency_ppb=round(gamma * 1_000_000_000),
            vega_currency_nanos_per_vol_ppm=round(vega * 1_000),
            vanna_per_vol_ppb=round(vanna * 1_000_000_000),
            charm_per_year_ppb=round(charm * 1_000_000_000),
        )


@dataclass(frozen=True, slots=True)
class VolatilitySurfaceConfig:
    """Minimum data and integer tolerance for surface quality checks."""

    minimum_points: int = 4
    minimum_expirations: int = 2
    price_tolerance_currency_nanos: int = 100_000

    def __post_init__(self) -> None:
        """Validate positive surface controls."""
        if (
            self.minimum_points <= 0
            or self.minimum_expirations <= 0
            or self.price_tolerance_currency_nanos < 0
        ):
            msg = "volatility-surface configuration is invalid"
            raise ValueError(msg)


class VolatilitySurfaceBuilder:
    """Build deterministic sorted points and flag static arbitrage."""

    def __init__(self, config: VolatilitySurfaceConfig) -> None:
        """Bind immutable surface sufficiency and arbitrage tolerances."""
        self._config = config

    def build(
        self, points: tuple[SurfacePoint, ...], as_of_ns: int
    ) -> VolatilitySurface:
        """Check point count, calendar total variance, and strike convexity."""
        ordered = tuple(
            sorted(
                points,
                key=lambda item: (
                    item.expiration_wall_clock_utc_ns,
                    int(item.right),
                    item.strike_currency_nanos,
                    item.contract_id.hex(),
                ),
            )
        )
        reasons: list[OptionsReasonCode] = []
        expirations = {item.expiration_wall_clock_utc_ns for item in ordered}
        if len(ordered) < self._config.minimum_points or len(expirations) < (
            self._config.minimum_expirations
        ):
            reasons.append(OptionsReasonCode.INSUFFICIENT_SURFACE)
        if self._has_calendar_arbitrage(ordered, as_of_ns):
            reasons.append(OptionsReasonCode.CALENDAR_ARBITRAGE)
        if self._has_convexity_arbitrage(ordered):
            reasons.append(OptionsReasonCode.CONVEXITY_ARBITRAGE)
        quality = SurfaceQuality.HEALTHY
        if OptionsReasonCode.INSUFFICIENT_SURFACE in reasons:
            quality = SurfaceQuality.DEGRADED
        if any(
            item
            in {
                OptionsReasonCode.CALENDAR_ARBITRAGE,
                OptionsReasonCode.CONVEXITY_ARBITRAGE,
            }
            for item in reasons
        ):
            quality = SurfaceQuality.INVALID
        return VolatilitySurface(ordered, quality, tuple(reasons))

    @staticmethod
    def _has_calendar_arbitrage(
        points: tuple[SurfacePoint, ...], as_of_ns: int
    ) -> bool:
        groups: dict[tuple[int, OptionRight], list[SurfacePoint]] = {}
        for point in points:
            groups.setdefault((point.strike_currency_nanos, point.right), []).append(
                point
            )
        for values in groups.values():
            previous_variance = -1.0
            for point in sorted(
                values, key=lambda item: item.expiration_wall_clock_utc_ns
            ):
                time_years = _time_years(point.expiration_wall_clock_utc_ns, as_of_ns)
                variance = (point.implied_volatility_ppm / PPM) ** 2 * time_years
                if variance + 1e-15 < previous_variance:
                    return True
                previous_variance = variance
        return False

    def _has_convexity_arbitrage(self, points: tuple[SurfacePoint, ...]) -> bool:
        groups: dict[tuple[int, OptionRight], list[SurfacePoint]] = {}
        for point in points:
            groups.setdefault(
                (point.expiration_wall_clock_utc_ns, point.right), []
            ).append(point)
        tolerance = self._config.price_tolerance_currency_nanos
        for (_, right), values in groups.items():
            ordered = sorted(values, key=lambda item: item.strike_currency_nanos)
            prices = [item.midpoint_currency_nanos for item in ordered]
            if right is OptionRight.CALL and any(
                left + tolerance < right_price for left, right_price in pairwise(prices)
            ):
                return True
            if right is OptionRight.PUT and any(
                left > right_price + tolerance for left, right_price in pairwise(prices)
            ):
                return True
            for left, middle, right_point in zip(
                ordered,
                ordered[1:],
                ordered[2:],
                strict=False,
            ):
                left_slope_numerator = (
                    middle.midpoint_currency_nanos - left.midpoint_currency_nanos
                )
                right_slope_numerator = (
                    right_point.midpoint_currency_nanos - middle.midpoint_currency_nanos
                )
                left_width = middle.strike_currency_nanos - left.strike_currency_nanos
                right_width = (
                    right_point.strike_currency_nanos - middle.strike_currency_nanos
                )
                if (
                    left_slope_numerator * right_width
                    > right_slope_numerator * left_width
                    + tolerance * max(left_width, right_width)
                ):
                    return True
        return False
