"""Immutable universe and exchange-calendar horizon contracts for the POC."""

from __future__ import annotations

import hashlib
import json
import re
from dataclasses import dataclass
from enum import IntEnum
from pathlib import Path
from types import MappingProxyType
from typing import TYPE_CHECKING, Final, Protocol, cast

from aegis_mx_intelligence import ConfigurationVersion, Identifier128, InstrumentId

if TYPE_CHECKING:
    from collections.abc import Mapping

UNIVERSE_SCHEMA_VERSION: Final = "1.0.0"
HORIZON_SCHEMA_VERSION: Final = "1.0.0"
TICKER_UNIVERSE_PATH: Final = Path("/scratch/djy8hg/xtrade/ticker.txt")
MAX_UNIVERSE_BYTES: Final = 1 << 20
MAX_UNIVERSE_SYMBOLS: Final = 10_000
MAX_UNIVERSE_SNAPSHOT_BYTES: Final = 8 << 20
MAX_SYMBOL_BYTES: Final = 16
MAX_LABEL_BYTES: Final = 16
MAX_CALENDAR_ID_BYTES: Final = 32
MAX_SESSION_ID_BYTES: Final = 64
MAX_CALENDAR_SESSIONS: Final = 4_096
MAX_HORIZON_VALUE: Final = 10_000
MAX_INT64: Final = (1 << 63) - 1
MINUTE_NS: Final = 60_000_000_000
SHA256_HEX_LENGTH: Final = 64
IDENTIFIER_HEX_LENGTH: Final = 32

_SYMBOL_PATTERN = re.compile(r"^[A-Z][A-Z0-9]*(?:[.-][A-Z0-9]+)?$")
_LABEL_PATTERN = re.compile(r"^[a-z0-9]+$")
_AMBIGUOUS_SYMBOL_CHARACTERS: Final = frozenset(" \t/:,=@")


class UniverseErrorCode(IntEnum):
    """Stable fail-closed universe parsing and snapshot errors."""

    INVALID_ENCODING = 1
    SOURCE_TOO_LARGE = 2
    EMPTY_UNIVERSE = 3
    SYMBOL_TOO_LONG = 4
    MALFORMED_SYMBOL = 5
    AMBIGUOUS_SYMBOL = 6
    DUPLICATE_SYMBOL = 7
    INVALID_RESOLUTION = 8
    AMBIGUOUS_INSTRUMENT = 9
    INVALID_SNAPSHOT = 10
    HASH_MISMATCH = 11
    IO_ERROR = 12


class UniverseContractError(ValueError):
    """Universe rejection retaining a machine-readable reason and line."""

    def __init__(
        self,
        code: UniverseErrorCode,
        detail: str,
        *,
        line_number: int | None = None,
    ) -> None:
        """Create a bounded deterministic universe failure."""
        self.code = code
        self.line_number = line_number
        suffix = "" if line_number is None else f" at line {line_number}"
        super().__init__(f"{code.name.lower()}{suffix}: {detail}")


class InstrumentResolutionCode(IntEnum):
    """Stable resolution result; unresolved symbols remain in the snapshot."""

    RESOLVED = 1
    UNSUPPORTED = 2
    RECENTLY_LISTED = 3
    AMBIGUOUS_REFERENCE = 4
    MISSING_REFERENCE_DATA = 5
    DELISTED = 6


@dataclass(frozen=True, slots=True)
class InstrumentResolution:
    """One resolver outcome with an ID exactly when resolution succeeded."""

    code: InstrumentResolutionCode
    instrument_id: InstrumentId | None

    def __post_init__(self) -> None:
        """Reject an identity detached from its resolution state."""
        if (self.code is InstrumentResolutionCode.RESOLVED) != (
            self.instrument_id is not None
        ):
            raise UniverseContractError(
                UniverseErrorCode.INVALID_RESOLUTION,
                "resolved state and instrument identity disagree",
            )


class InstrumentResolver(Protocol):
    """Point-in-time resolver injected by an authorized reference-data owner."""

    def resolve(self, symbol: str) -> InstrumentResolution:
        """Return a stable identity or an explicit unresolved reason."""


class UnresolvedInstrumentResolver:
    """Safe resolver used before authorized reference data is available."""

    def resolve(self, symbol: str) -> InstrumentResolution:
        """Retain every syntactically valid symbol as unresolved."""
        del symbol
        return InstrumentResolution(
            InstrumentResolutionCode.MISSING_REFERENCE_DATA,
            None,
        )


class StaticInstrumentResolver:
    """Immutable resolver for deterministic fixtures and approved snapshots."""

    __slots__ = ("_resolved", "_unresolved")

    def __init__(
        self,
        resolved: Mapping[str, InstrumentId],
        unresolved: Mapping[str, InstrumentResolutionCode] | None = None,
    ) -> None:
        """Copy and validate bounded fixture mappings."""
        resolved_copy = dict(resolved)
        unresolved_copy = {} if unresolved is None else dict(unresolved)
        if set(resolved_copy) & set(unresolved_copy):
            raise UniverseContractError(
                UniverseErrorCode.INVALID_RESOLUTION,
                "a symbol cannot be both resolved and unresolved",
            )
        if any(
            code is InstrumentResolutionCode.RESOLVED
            for code in unresolved_copy.values()
        ):
            raise UniverseContractError(
                UniverseErrorCode.INVALID_RESOLUTION,
                "unresolved mapping cannot use RESOLVED",
            )
        self._resolved = MappingProxyType(resolved_copy)
        self._unresolved = MappingProxyType(unresolved_copy)

    def resolve(self, symbol: str) -> InstrumentResolution:
        """Resolve from the immutable fixture without guessing aliases."""
        instrument_id = self._resolved.get(symbol)
        if instrument_id is not None:
            return InstrumentResolution(
                InstrumentResolutionCode.RESOLVED, instrument_id
            )
        return InstrumentResolution(
            self._unresolved.get(
                symbol,
                InstrumentResolutionCode.MISSING_REFERENCE_DATA,
            ),
            None,
        )


@dataclass(frozen=True, slots=True)
class UniverseEntry:
    """One source-ordered symbol and its explicit reference-data disposition."""

    source_ordinal: int
    symbol: str
    resolution_code: InstrumentResolutionCode
    instrument_id: InstrumentId | None

    def __post_init__(self) -> None:
        """Validate one immutable snapshot entry."""
        if self.source_ordinal < 0:
            raise UniverseContractError(
                UniverseErrorCode.INVALID_SNAPSHOT,
                "source ordinal cannot be negative",
            )
        _validate_symbol(self.symbol)
        InstrumentResolution(self.resolution_code, self.instrument_id)


@dataclass(frozen=True, slots=True)
class UniverseSnapshot:
    """Source-order view plus deterministic canonical universe identity."""

    source_path: str
    source_file_sha256: bytes
    ordered_entries: tuple[UniverseEntry, ...]
    canonical_symbols: tuple[str, ...]
    universe_snapshot_sha256: bytes
    schema_version: str = UNIVERSE_SCHEMA_VERSION

    def __post_init__(self) -> None:
        """Validate immutable identity and ordering invariants."""
        if self.source_path != str(TICKER_UNIVERSE_PATH):
            raise UniverseContractError(
                UniverseErrorCode.INVALID_SNAPSHOT,
                "snapshot source path is not the authoritative ticker file",
            )
        if self.schema_version != UNIVERSE_SCHEMA_VERSION:
            raise UniverseContractError(
                UniverseErrorCode.INVALID_SNAPSHOT,
                "unsupported universe schema version",
            )
        if len(self.source_file_sha256) != hashlib.sha256().digest_size:
            raise UniverseContractError(
                UniverseErrorCode.INVALID_SNAPSHOT,
                "source digest is not SHA-256",
            )
        if len(self.universe_snapshot_sha256) != hashlib.sha256().digest_size:
            raise UniverseContractError(
                UniverseErrorCode.INVALID_SNAPSHOT,
                "snapshot digest is not SHA-256",
            )
        if not self.ordered_entries:
            raise UniverseContractError(
                UniverseErrorCode.EMPTY_UNIVERSE,
                "snapshot contains no symbols",
            )
        if tuple(entry.source_ordinal for entry in self.ordered_entries) != tuple(
            range(len(self.ordered_entries))
        ):
            raise UniverseContractError(
                UniverseErrorCode.INVALID_SNAPSHOT,
                "source ordinals are not contiguous",
            )
        symbols = tuple(entry.symbol for entry in self.ordered_entries)
        if len(set(symbols)) != len(symbols) or self.canonical_symbols != tuple(
            sorted(symbols)
        ):
            raise UniverseContractError(
                UniverseErrorCode.INVALID_SNAPSHOT,
                "snapshot symbols are duplicated or not canonically sorted",
            )


def _validate_symbol(symbol: str, *, line_number: int | None = None) -> None:
    try:
        encoded = symbol.encode("ascii")
    except UnicodeEncodeError as error:
        raise UniverseContractError(
            UniverseErrorCode.INVALID_ENCODING,
            "symbol is not ASCII",
            line_number=line_number,
        ) from error
    if len(encoded) > MAX_SYMBOL_BYTES:
        raise UniverseContractError(
            UniverseErrorCode.SYMBOL_TOO_LONG,
            f"symbol exceeds {MAX_SYMBOL_BYTES} bytes",
            line_number=line_number,
        )
    if any(character in _AMBIGUOUS_SYMBOL_CHARACTERS for character in symbol):
        raise UniverseContractError(
            UniverseErrorCode.AMBIGUOUS_SYMBOL,
            "symbol contains a qualifier, delimiter, or whitespace",
            line_number=line_number,
        )
    if not _SYMBOL_PATTERN.fullmatch(symbol):
        raise UniverseContractError(
            UniverseErrorCode.MALFORMED_SYMBOL,
            "symbol must be uppercase ASCII with at most one internal qualifier",
            line_number=line_number,
        )


def _entry_payload(entry: UniverseEntry) -> dict[str, object]:
    return {
        "instrument_id": (
            None if entry.instrument_id is None else entry.instrument_id.hex()
        ),
        "resolution_code": int(entry.resolution_code),
        "source_ordinal": entry.source_ordinal,
        "symbol": entry.symbol,
    }


def _snapshot_payload(snapshot: UniverseSnapshot) -> dict[str, object]:
    return {
        "canonical_entries": [
            _entry_payload(entry)
            for entry in sorted(snapshot.ordered_entries, key=lambda item: item.symbol)
        ],
        "schema_version": snapshot.schema_version,
        "source_file_sha256": snapshot.source_file_sha256.hex(),
        "source_path": snapshot.source_path,
    }


def _canonical_json(payload: Mapping[str, object]) -> bytes:
    return json.dumps(
        payload,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=True,
    ).encode("ascii")


def _snapshot_from_entries(
    source_sha256: bytes,
    entries: tuple[UniverseEntry, ...],
) -> UniverseSnapshot:
    provisional = UniverseSnapshot(
        source_path=str(TICKER_UNIVERSE_PATH),
        source_file_sha256=source_sha256,
        ordered_entries=entries,
        canonical_symbols=tuple(sorted(entry.symbol for entry in entries)),
        universe_snapshot_sha256=bytes(hashlib.sha256().digest_size),
    )
    digest = hashlib.sha256(_canonical_json(_snapshot_payload(provisional))).digest()
    return UniverseSnapshot(
        source_path=provisional.source_path,
        source_file_sha256=source_sha256,
        ordered_entries=entries,
        canonical_symbols=provisional.canonical_symbols,
        universe_snapshot_sha256=digest,
    )


def _parse_symbols(source: bytes) -> tuple[str, ...]:
    if not isinstance(source, bytes):
        raise UniverseContractError(
            UniverseErrorCode.INVALID_ENCODING,
            "ticker source must be bytes",
        )
    if len(source) > MAX_UNIVERSE_BYTES:
        raise UniverseContractError(
            UniverseErrorCode.SOURCE_TOO_LARGE,
            f"ticker source exceeds {MAX_UNIVERSE_BYTES} bytes",
        )
    try:
        text = source.decode("ascii")
    except UnicodeDecodeError as error:
        raise UniverseContractError(
            UniverseErrorCode.INVALID_ENCODING,
            "ticker source is not ASCII",
        ) from error

    symbols: list[str] = []
    observed: set[str] = set()
    for line_number, raw_line in enumerate(text.split("\n"), start=1):
        line = raw_line.removesuffix("\r")
        if line == "":
            continue
        _validate_symbol(line, line_number=line_number)
        if line in observed:
            raise UniverseContractError(
                UniverseErrorCode.DUPLICATE_SYMBOL,
                f"duplicate symbol {line}",
                line_number=line_number,
            )
        observed.add(line)
        symbols.append(line)
        if len(symbols) > MAX_UNIVERSE_SYMBOLS:
            raise UniverseContractError(
                UniverseErrorCode.SOURCE_TOO_LARGE,
                f"ticker source exceeds {MAX_UNIVERSE_SYMBOLS} symbols",
            )
    if not symbols:
        raise UniverseContractError(
            UniverseErrorCode.EMPTY_UNIVERSE,
            "ticker source contains no non-empty symbols",
        )

    return tuple(symbols)


def _resolve_symbols(
    symbols: tuple[str, ...],
    resolver: InstrumentResolver,
) -> tuple[UniverseEntry, ...]:
    entries: list[UniverseEntry] = []
    resolved_ids: dict[Identifier128, str] = {}
    for ordinal, symbol in enumerate(symbols):
        resolution = resolver.resolve(symbol)
        if not isinstance(resolution, InstrumentResolution):
            raise UniverseContractError(
                UniverseErrorCode.INVALID_RESOLUTION,
                f"resolver returned an invalid outcome for {symbol}",
            )
        entry = UniverseEntry(
            source_ordinal=ordinal,
            symbol=symbol,
            resolution_code=resolution.code,
            instrument_id=resolution.instrument_id,
        )
        if entry.instrument_id is not None:
            previous_symbol = resolved_ids.get(entry.instrument_id)
            if previous_symbol is not None:
                raise UniverseContractError(
                    UniverseErrorCode.AMBIGUOUS_INSTRUMENT,
                    f"symbols {previous_symbol} and {symbol} resolve to one instrument",
                )
            resolved_ids[entry.instrument_id] = symbol
        entries.append(entry)
    return tuple(entries)


def parse_ticker_universe(
    source: bytes,
    resolver: InstrumentResolver,
) -> UniverseSnapshot:
    """Parse bounded bytes from the sole ticker-file contract."""
    entries = _resolve_symbols(_parse_symbols(source), resolver)
    return _snapshot_from_entries(hashlib.sha256(source).digest(), entries)


def load_ticker_universe(resolver: InstrumentResolver) -> UniverseSnapshot:
    """Read only the repository-authoritative ticker path and parse it."""
    try:
        with TICKER_UNIVERSE_PATH.open("rb") as source_file:
            source = source_file.read(MAX_UNIVERSE_BYTES + 1)
    except OSError as error:
        raise UniverseContractError(
            UniverseErrorCode.IO_ERROR,
            "authoritative ticker file cannot be read",
        ) from error
    return parse_ticker_universe(source, resolver)


def encode_universe_snapshot(snapshot: UniverseSnapshot) -> bytes:
    """Serialize one canonical sorted snapshot with its verified SHA-256."""
    payload = _snapshot_payload(snapshot)
    digest = hashlib.sha256(_canonical_json(payload)).digest()
    if digest != snapshot.universe_snapshot_sha256:
        raise UniverseContractError(
            UniverseErrorCode.HASH_MISMATCH,
            "universe snapshot changed after hashing",
        )
    encoded_payload = dict(payload)
    encoded_payload["universe_snapshot_sha256"] = digest.hex()
    encoded = _canonical_json(encoded_payload)
    if len(encoded) > MAX_UNIVERSE_SNAPSHOT_BYTES:
        raise UniverseContractError(
            UniverseErrorCode.SOURCE_TOO_LARGE,
            "encoded universe snapshot exceeds its bound",
        )
    return encoded


def _require_mapping(value: object) -> Mapping[str, object]:
    if not isinstance(value, dict):
        raise UniverseContractError(
            UniverseErrorCode.INVALID_SNAPSHOT,
            "snapshot value must be an object",
        )
    return cast("Mapping[str, object]", value)


def _require_int(value: object, field: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool):
        raise UniverseContractError(
            UniverseErrorCode.INVALID_SNAPSHOT,
            f"{field} must be an integer",
        )
    return value


def _require_text(value: object, field: str) -> str:
    if not isinstance(value, str):
        raise UniverseContractError(
            UniverseErrorCode.INVALID_SNAPSHOT,
            f"{field} must be text",
        )
    return value


def _decode_digest(value: object, field: str) -> bytes:
    text = _require_text(value, field)
    if len(text) != SHA256_HEX_LENGTH or text != text.lower():
        raise UniverseContractError(
            UniverseErrorCode.INVALID_SNAPSHOT,
            f"{field} must be lowercase SHA-256",
        )
    try:
        return bytes.fromhex(text)
    except ValueError as error:
        raise UniverseContractError(
            UniverseErrorCode.INVALID_SNAPSHOT,
            f"{field} must be lowercase SHA-256",
        ) from error


def _decode_identifier(value: object) -> InstrumentId | None:
    if value is None:
        return None
    text = _require_text(value, "instrument_id")
    if len(text) != IDENTIFIER_HEX_LENGTH or text != text.lower():
        raise UniverseContractError(
            UniverseErrorCode.INVALID_SNAPSHOT,
            "instrument_id must be lowercase 128-bit hexadecimal",
        )
    try:
        identifier = Identifier128(int(text[:16], 16), int(text[16:], 16))
    except ValueError as error:
        raise UniverseContractError(
            UniverseErrorCode.INVALID_SNAPSHOT,
            "instrument_id must be lowercase 128-bit hexadecimal",
        ) from error
    return InstrumentId(identifier)


def _decode_entries(raw_entries: object) -> tuple[UniverseEntry, ...]:
    if not isinstance(raw_entries, list) or not raw_entries:
        raise UniverseContractError(
            UniverseErrorCode.INVALID_SNAPSHOT,
            "canonical_entries must be a non-empty array",
        )
    if len(raw_entries) > MAX_UNIVERSE_SYMBOLS:
        raise UniverseContractError(
            UniverseErrorCode.SOURCE_TOO_LARGE,
            "snapshot contains too many entries",
        )
    entries: list[UniverseEntry] = []
    for raw_entry in raw_entries:
        entry = _require_mapping(raw_entry)
        if set(entry) != {
            "instrument_id",
            "resolution_code",
            "source_ordinal",
            "symbol",
        }:
            raise UniverseContractError(
                UniverseErrorCode.INVALID_SNAPSHOT,
                "universe entry fields do not match the contract",
            )
        try:
            code = InstrumentResolutionCode(
                _require_int(entry["resolution_code"], "resolution_code")
            )
        except ValueError as error:
            raise UniverseContractError(
                UniverseErrorCode.INVALID_SNAPSHOT,
                "resolution_code is unknown",
            ) from error
        entries.append(
            UniverseEntry(
                source_ordinal=_require_int(
                    entry["source_ordinal"],
                    "source_ordinal",
                ),
                symbol=_require_text(entry["symbol"], "symbol"),
                resolution_code=code,
                instrument_id=_decode_identifier(entry["instrument_id"]),
            )
        )
    if tuple(entry.symbol for entry in entries) != tuple(
        sorted(entry.symbol for entry in entries)
    ):
        raise UniverseContractError(
            UniverseErrorCode.INVALID_SNAPSHOT,
            "canonical entries are not sorted by symbol",
        )
    return tuple(sorted(entries, key=lambda entry: entry.source_ordinal))


def _decode_snapshot_payload(encoded: bytes) -> Mapping[str, object]:
    if not encoded or len(encoded) > MAX_UNIVERSE_SNAPSHOT_BYTES:
        raise UniverseContractError(
            UniverseErrorCode.INVALID_SNAPSHOT,
            "snapshot size is invalid",
        )
    try:
        return _require_mapping(json.loads(encoded.decode("ascii")))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise UniverseContractError(
            UniverseErrorCode.INVALID_SNAPSHOT,
            "snapshot must be ASCII JSON",
        ) from error


def decode_universe_snapshot(encoded: bytes) -> UniverseSnapshot:
    """Decode, structurally validate, and hash-check an untrusted snapshot."""
    payload = _decode_snapshot_payload(encoded)
    expected_keys = {
        "canonical_entries",
        "schema_version",
        "source_file_sha256",
        "source_path",
        "universe_snapshot_sha256",
    }
    if set(payload) != expected_keys:
        raise UniverseContractError(
            UniverseErrorCode.INVALID_SNAPSHOT,
            "snapshot fields do not match the contract",
        )
    if (
        _require_text(payload["schema_version"], "schema_version")
        != UNIVERSE_SCHEMA_VERSION
    ):
        raise UniverseContractError(
            UniverseErrorCode.INVALID_SNAPSHOT,
            "unsupported universe schema version",
        )
    if _require_text(payload["source_path"], "source_path") != str(
        TICKER_UNIVERSE_PATH
    ):
        raise UniverseContractError(
            UniverseErrorCode.INVALID_SNAPSHOT,
            "snapshot source path is not authoritative",
        )
    source_digest = _decode_digest(payload["source_file_sha256"], "source_file_sha256")
    snapshot_digest = _decode_digest(
        payload["universe_snapshot_sha256"],
        "universe_snapshot_sha256",
    )
    ordered = _decode_entries(payload["canonical_entries"])
    snapshot = _snapshot_from_entries(source_digest, ordered)
    if snapshot.universe_snapshot_sha256 != snapshot_digest:
        raise UniverseContractError(
            UniverseErrorCode.HASH_MISMATCH,
            "universe snapshot SHA-256 does not match its content",
        )
    if encode_universe_snapshot(snapshot) != encoded:
        raise UniverseContractError(
            UniverseErrorCode.INVALID_SNAPSHOT,
            "snapshot is not in canonical JSON form",
        )
    return snapshot


class ForecastHorizonUnit(IntEnum):
    """Stable horizon unit values aligned with the FlatBuffers contract."""

    ELAPSED_NANOSECONDS = 1
    TRADING_MINUTES = 2
    TRADING_SESSIONS = 3


class HorizonHaltPolicy(IntEnum):
    """Explicit handling when a declared halt intersects a horizon."""

    NOT_APPLICABLE = 1
    REJECT = 2
    PAUSE = 3
    COUNT_SCHEDULED = 4


class HorizonSessionEndpoint(IntEnum):
    """Target endpoint for a horizon value."""

    NOT_APPLICABLE = 1
    REGULAR_SESSION_CLOSE = 2


class HorizonErrorCode(IntEnum):
    """Stable fail-closed calendar and horizon reason codes."""

    INVALID_SPEC = 1
    INVALID_CALENDAR = 2
    INVALID_SESSION = 3
    INVALID_HALT = 4
    CALENDAR_VERSION_MISMATCH = 5
    INVALID_AS_OF_TIME = 6
    HALT_ENCOUNTERED = 7
    INSUFFICIENT_CALENDAR = 8
    TIMESTAMP_OVERFLOW = 9


class HorizonContractError(ValueError):
    """Horizon rejection retaining a stable machine-readable reason."""

    def __init__(self, code: HorizonErrorCode, detail: str) -> None:
        """Create a deterministic horizon failure."""
        self.code = code
        super().__init__(f"{code.name.lower()}: {detail}")


def _bounded_ascii(value: str, name: str, maximum: int) -> None:
    try:
        encoded = value.encode("ascii")
    except UnicodeEncodeError as error:
        raise HorizonContractError(
            HorizonErrorCode.INVALID_SPEC,
            f"{name} must be ASCII",
        ) from error
    if not encoded or len(encoded) > maximum or b"\x00" in encoded:
        raise HorizonContractError(
            HorizonErrorCode.INVALID_SPEC,
            f"{name} is empty or exceeds {maximum} bytes",
        )


def _timestamp(value: int, code: HorizonErrorCode, name: str) -> None:
    if not 0 < value <= MAX_INT64:
        raise HorizonContractError(
            code,
            f"{name} is outside positive int64 nanoseconds",
        )


@dataclass(frozen=True, slots=True)
class HorizonSpec:
    """Versioned semantic horizon; elapsed nanoseconds are not authoritative."""

    label: str
    unit: ForecastHorizonUnit
    value: int
    halt_policy: HorizonHaltPolicy
    session_endpoint: HorizonSessionEndpoint
    calendar_version: ConfigurationVersion | None
    schema_version: str = HORIZON_SCHEMA_VERSION

    def __post_init__(self) -> None:
        """Validate unit-specific horizon semantics."""
        _bounded_ascii(self.label, "horizon label", MAX_LABEL_BYTES)
        if not _LABEL_PATTERN.fullmatch(self.label):
            raise HorizonContractError(
                HorizonErrorCode.INVALID_SPEC,
                "horizon label must be lowercase alphanumeric text",
            )
        if self.schema_version != HORIZON_SCHEMA_VERSION:
            raise HorizonContractError(
                HorizonErrorCode.INVALID_SPEC,
                "unsupported horizon schema version",
            )
        if (
            not isinstance(self.unit, ForecastHorizonUnit)
            or not isinstance(self.halt_policy, HorizonHaltPolicy)
            or not isinstance(self.session_endpoint, HorizonSessionEndpoint)
        ):
            raise HorizonContractError(
                HorizonErrorCode.INVALID_SPEC,
                "horizon contains an unknown enum value",
            )
        maximum_value = (
            MAX_INT64
            if self.unit is ForecastHorizonUnit.ELAPSED_NANOSECONDS
            else MAX_HORIZON_VALUE
        )
        if not 0 < self.value <= maximum_value:
            raise HorizonContractError(
                HorizonErrorCode.INVALID_SPEC,
                "horizon value is outside the supported bound",
            )
        if self.unit is ForecastHorizonUnit.ELAPSED_NANOSECONDS:
            if (
                self.halt_policy is not HorizonHaltPolicy.NOT_APPLICABLE
                or self.session_endpoint is not HorizonSessionEndpoint.NOT_APPLICABLE
                or self.calendar_version is not None
            ):
                raise HorizonContractError(
                    HorizonErrorCode.INVALID_SPEC,
                    "legacy elapsed horizon cannot carry calendar semantics",
                )
            return
        if self.calendar_version is None:
            raise HorizonContractError(
                HorizonErrorCode.INVALID_SPEC,
                "trading horizon requires a calendar version",
            )
        if self.halt_policy not in {
            HorizonHaltPolicy.REJECT,
            HorizonHaltPolicy.PAUSE,
            HorizonHaltPolicy.COUNT_SCHEDULED,
        }:
            raise HorizonContractError(
                HorizonErrorCode.INVALID_SPEC,
                "trading horizon requires an explicit halt policy",
            )
        expected_endpoint = (
            HorizonSessionEndpoint.NOT_APPLICABLE
            if self.unit is ForecastHorizonUnit.TRADING_MINUTES
            else HorizonSessionEndpoint.REGULAR_SESSION_CLOSE
        )
        if self.session_endpoint is not expected_endpoint:
            raise HorizonContractError(
                HorizonErrorCode.INVALID_SPEC,
                "session endpoint does not agree with the horizon unit",
            )

    @classmethod
    def trading_minutes(
        cls,
        label: str,
        value: int,
        calendar_version: ConfigurationVersion,
        *,
        halt_policy: HorizonHaltPolicy = HorizonHaltPolicy.PAUSE,
    ) -> HorizonSpec:
        """Construct an eligible regular-session-minute horizon."""
        return cls(
            label,
            ForecastHorizonUnit.TRADING_MINUTES,
            value,
            halt_policy,
            HorizonSessionEndpoint.NOT_APPLICABLE,
            calendar_version,
        )

    @classmethod
    def trading_sessions(
        cls,
        label: str,
        value: int,
        calendar_version: ConfigurationVersion,
        *,
        halt_policy: HorizonHaltPolicy = HorizonHaltPolicy.PAUSE,
    ) -> HorizonSpec:
        """Construct a future regular-session-close horizon."""
        return cls(
            label,
            ForecastHorizonUnit.TRADING_SESSIONS,
            value,
            halt_policy,
            HorizonSessionEndpoint.REGULAR_SESSION_CLOSE,
            calendar_version,
        )


REQUIRED_HORIZON_LABELS: Final = (
    "5m",
    "10m",
    "15m",
    "30m",
    "60m",
    "2h",
    "5h",
    "1d",
    "1w",
    "2w",
    "1mo",
    "2mo",
)
_REQUIRED_TRADING_MINUTES: Final = (5, 10, 15, 30, 60, 120, 300)
_REQUIRED_TRADING_SESSIONS: Final = (1, 5, 10, 21, 42)


def required_horizon_specs(
    calendar_version: ConfigurationVersion,
    *,
    halt_policy: HorizonHaltPolicy = HorizonHaltPolicy.PAUSE,
) -> tuple[HorizonSpec, ...]:
    """Return the twelve required POC horizons in canonical CLI order."""
    minute_specs = tuple(
        HorizonSpec.trading_minutes(
            label,
            value,
            calendar_version,
            halt_policy=halt_policy,
        )
        for label, value in zip(
            REQUIRED_HORIZON_LABELS[: len(_REQUIRED_TRADING_MINUTES)],
            _REQUIRED_TRADING_MINUTES,
            strict=True,
        )
    )
    session_specs = tuple(
        HorizonSpec.trading_sessions(
            label,
            value,
            calendar_version,
            halt_policy=halt_policy,
        )
        for label, value in zip(
            REQUIRED_HORIZON_LABELS[len(_REQUIRED_TRADING_MINUTES) :],
            _REQUIRED_TRADING_SESSIONS,
            strict=True,
        )
    )
    return minute_specs + session_specs


@dataclass(frozen=True, slots=True, order=True)
class HaltInterval:
    """Half-open exchange-event-time halt interval."""

    start_exchange_event_time_ns: int
    end_exchange_event_time_ns: int

    def __post_init__(self) -> None:
        """Require a positive, representable, nonempty interval."""
        _timestamp(
            self.start_exchange_event_time_ns,
            HorizonErrorCode.TIMESTAMP_OVERFLOW,
            "halt start",
        )
        _timestamp(
            self.end_exchange_event_time_ns,
            HorizonErrorCode.TIMESTAMP_OVERFLOW,
            "halt end",
        )
        if self.end_exchange_event_time_ns <= self.start_exchange_event_time_ns:
            raise HorizonContractError(
                HorizonErrorCode.INVALID_HALT,
                "halt interval is empty or reversed",
            )


@dataclass(frozen=True, slots=True)
class TradingSession:
    """One versioned regular session with explicit optional halts."""

    session_id: str
    open_exchange_event_time_ns: int
    close_exchange_event_time_ns: int
    halt_intervals: tuple[HaltInterval, ...] = ()

    def __post_init__(self) -> None:
        """Validate bounds, minute grid, and halt ordering."""
        _bounded_ascii(self.session_id, "session_id", MAX_SESSION_ID_BYTES)
        _timestamp(
            self.open_exchange_event_time_ns,
            HorizonErrorCode.TIMESTAMP_OVERFLOW,
            "session open",
        )
        _timestamp(
            self.close_exchange_event_time_ns,
            HorizonErrorCode.TIMESTAMP_OVERFLOW,
            "session close",
        )
        duration = self.close_exchange_event_time_ns - self.open_exchange_event_time_ns
        if duration <= 0 or duration % MINUTE_NS != 0:
            raise HorizonContractError(
                HorizonErrorCode.INVALID_SESSION,
                "session must contain a positive whole number of minutes",
            )
        previous_end = self.open_exchange_event_time_ns
        for halt in self.halt_intervals:
            if (
                halt.start_exchange_event_time_ns < self.open_exchange_event_time_ns
                or halt.end_exchange_event_time_ns > self.close_exchange_event_time_ns
                or halt.start_exchange_event_time_ns < previous_end
            ):
                raise HorizonContractError(
                    HorizonErrorCode.INVALID_HALT,
                    "halt is outside the session or overlaps a prior halt",
                )
            previous_end = halt.end_exchange_event_time_ns


@dataclass(frozen=True, slots=True)
class ExchangeCalendar:
    """Immutable ordered regular-session snapshot with version identity."""

    calendar_id: str
    calendar_version: ConfigurationVersion
    sessions: tuple[TradingSession, ...]

    def __post_init__(self) -> None:
        """Require a bounded strictly ordered nonoverlapping session set."""
        _bounded_ascii(self.calendar_id, "calendar_id", MAX_CALENDAR_ID_BYTES)
        if not 0 < len(self.sessions) <= MAX_CALENDAR_SESSIONS:
            raise HorizonContractError(
                HorizonErrorCode.INVALID_CALENDAR,
                "calendar session count is outside the supported bound",
            )
        previous_close = 0
        session_ids: set[str] = set()
        for session in self.sessions:
            if (
                session.session_id in session_ids
                or session.open_exchange_event_time_ns <= previous_close
            ):
                raise HorizonContractError(
                    HorizonErrorCode.INVALID_CALENDAR,
                    "calendar sessions overlap, are unordered, or repeat identity",
                )
            session_ids.add(session.session_id)
            previous_close = session.close_exchange_event_time_ns


@dataclass(frozen=True, slots=True)
class HorizonTarget:
    """Explicit exchange timestamp selected by a semantic horizon."""

    spec: HorizonSpec
    as_of_exchange_event_time_ns: int
    target_exchange_event_time_ns: int
    elapsed_ns: int

    def __post_init__(self) -> None:
        """Bind the target to an exact positive elapsed interval."""
        _timestamp(
            self.as_of_exchange_event_time_ns,
            HorizonErrorCode.TIMESTAMP_OVERFLOW,
            "as-of timestamp",
        )
        _timestamp(
            self.target_exchange_event_time_ns,
            HorizonErrorCode.TIMESTAMP_OVERFLOW,
            "target timestamp",
        )
        if (
            self.target_exchange_event_time_ns <= self.as_of_exchange_event_time_ns
            or self.elapsed_ns
            != self.target_exchange_event_time_ns - self.as_of_exchange_event_time_ns
        ):
            raise HorizonContractError(
                HorizonErrorCode.INVALID_SPEC,
                "target timestamp and elapsed interval disagree",
            )


def _overlaps_minute(halt: HaltInterval, endpoint_ns: int) -> bool:
    minute_start = endpoint_ns - MINUTE_NS
    return (
        halt.start_exchange_event_time_ns < endpoint_ns
        and halt.end_exchange_event_time_ns > minute_start
    )


def _halted_minute(session: TradingSession, endpoint_ns: int) -> bool:
    return any(_overlaps_minute(halt, endpoint_ns) for halt in session.halt_intervals)


def _as_of_session_index(calendar: ExchangeCalendar, as_of_ns: int) -> int:
    _timestamp(as_of_ns, HorizonErrorCode.TIMESTAMP_OVERFLOW, "as-of timestamp")
    for index, session in enumerate(calendar.sessions):
        if (
            session.open_exchange_event_time_ns
            < as_of_ns
            <= session.close_exchange_event_time_ns
        ):
            if (as_of_ns - session.open_exchange_event_time_ns) % MINUTE_NS != 0:
                break
            return index
    raise HorizonContractError(
        HorizonErrorCode.INVALID_AS_OF_TIME,
        "as-of time is not an eligible regular-session minute endpoint",
    )


def _resolve_trading_minutes(
    spec: HorizonSpec,
    as_of_ns: int,
    calendar: ExchangeCalendar,
    session_index: int,
) -> int:
    remaining = spec.value
    for index in range(session_index, len(calendar.sessions)):
        session = calendar.sessions[index]
        endpoint = session.open_exchange_event_time_ns + MINUTE_NS
        if index == session_index:
            endpoint = as_of_ns + MINUTE_NS
        while endpoint <= session.close_exchange_event_time_ns:
            halted = _halted_minute(session, endpoint)
            if halted and spec.halt_policy is HorizonHaltPolicy.REJECT:
                raise HorizonContractError(
                    HorizonErrorCode.HALT_ENCOUNTERED,
                    "halt intersects the requested trading-minute horizon",
                )
            if not halted or spec.halt_policy is HorizonHaltPolicy.COUNT_SCHEDULED:
                remaining -= 1
                if remaining == 0:
                    return endpoint
            endpoint += MINUTE_NS
    raise HorizonContractError(
        HorizonErrorCode.INSUFFICIENT_CALENDAR,
        "calendar does not cover the requested trading-minute horizon",
    )


def _resolve_trading_sessions(
    spec: HorizonSpec,
    calendar: ExchangeCalendar,
    session_index: int,
) -> int:
    remaining = spec.value
    for session in calendar.sessions[session_index + 1 :]:
        if session.halt_intervals and spec.halt_policy is HorizonHaltPolicy.REJECT:
            raise HorizonContractError(
                HorizonErrorCode.HALT_ENCOUNTERED,
                "halt intersects a candidate future session",
            )
        close_halted = _halted_minute(
            session,
            session.close_exchange_event_time_ns,
        )
        if close_halted and spec.halt_policy is HorizonHaltPolicy.PAUSE:
            continue
        remaining -= 1
        if remaining == 0:
            return session.close_exchange_event_time_ns
    raise HorizonContractError(
        HorizonErrorCode.INSUFFICIENT_CALENDAR,
        "calendar does not cover the requested trading-session horizon",
    )


def resolve_horizon(
    spec: HorizonSpec,
    as_of_exchange_event_time_ns: int,
    calendar: ExchangeCalendar,
) -> HorizonTarget:
    """Resolve one semantic horizon to an explicit exchange-event timestamp."""
    if spec.unit is ForecastHorizonUnit.ELAPSED_NANOSECONDS:
        target = as_of_exchange_event_time_ns + spec.value
        if target > MAX_INT64:
            raise HorizonContractError(
                HorizonErrorCode.TIMESTAMP_OVERFLOW,
                "elapsed horizon target overflows int64",
            )
        return HorizonTarget(spec, as_of_exchange_event_time_ns, target, spec.value)
    if spec.calendar_version != calendar.calendar_version:
        raise HorizonContractError(
            HorizonErrorCode.CALENDAR_VERSION_MISMATCH,
            "horizon and exchange calendar versions differ",
        )
    session_index = _as_of_session_index(calendar, as_of_exchange_event_time_ns)
    current_session = calendar.sessions[session_index]
    if spec.halt_policy is not HorizonHaltPolicy.COUNT_SCHEDULED and _halted_minute(
        current_session, as_of_exchange_event_time_ns
    ):
        raise HorizonContractError(
            HorizonErrorCode.INVALID_AS_OF_TIME,
            "as-of minute is halted under the selected policy",
        )
    target = (
        _resolve_trading_minutes(
            spec,
            as_of_exchange_event_time_ns,
            calendar,
            session_index,
        )
        if spec.unit is ForecastHorizonUnit.TRADING_MINUTES
        else _resolve_trading_sessions(spec, calendar, session_index)
    )
    return HorizonTarget(
        spec,
        as_of_exchange_event_time_ns,
        target,
        target - as_of_exchange_event_time_ns,
    )
