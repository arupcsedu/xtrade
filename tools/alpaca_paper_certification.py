"""Fail-closed Alpaca PAPER connectivity certification outside the hot path."""

from __future__ import annotations

import argparse
import hashlib
import http.client
import json
import os
import re
import stat
import subprocess
import sys
import tempfile
import time
import uuid
from contextlib import suppress
from dataclasses import dataclass
from datetime import UTC, date, datetime
from decimal import Decimal, InvalidOperation
from enum import StrEnum
from pathlib import Path
from typing import TYPE_CHECKING, Final, Protocol, cast
from urllib.parse import quote, urlencode, urlsplit
from zoneinfo import ZoneInfo

from aegis_mx_research.forecast_contracts import (
    TICKER_UNIVERSE_PATH,
    UnresolvedInstrumentResolver,
    load_ticker_universe,
)

if TYPE_CHECKING:
    from collections.abc import Callable, Mapping, Sequence

PAPER_ORIGIN: Final = "https://paper-api.alpaca.markets"
DATA_ORIGIN: Final = "https://data.alpaca.markets"
DEFAULT_SECRET_FILE: Final = Path("/scratch/djy8hg/ALPACA/.keys")
DEFAULT_SYSTEM_PATH_BINARY: Final = Path(
    "build/release/cpp/integration/aegis-alpaca-paper-path"
)
REPORT_SCHEMA_VERSION: Final = "1.0.0"
AUTHORIZATION_SCHEMA_VERSION: Final = "1.0.0"
BUILD_VERSION: Final = "0.1.0"
MAX_SECRET_FILE_BYTES: Final = 16_384
MAX_RESPONSE_BYTES: Final = 4_000_000
MAX_SECRET_VALUE_BYTES: Final = 4_096
MAX_TIMESTAMP_BYTES: Final = 64
MAX_SYMBOL_BYTES: Final = 16
MAX_AUTHORIZATION_BYTES: Final = 32_768
MAX_SYSTEM_PATH_BINARY_BYTES: Final = 134_217_728
MAX_SYSTEM_PATH_OUTPUT_BYTES: Final = 32_768
MAX_SYSTEM_PATH_SOURCE_FILE_BYTES: Final = 16_777_216
MAX_SYSTEM_PATH_SOURCE_TREE_BYTES: Final = 268_435_456
MAX_CLIENT_TIMEOUT_SECONDS: Final = 30.0
MAX_POLL_ATTEMPTS: Final = 100
MAX_CERTIFICATION_ORDERS: Final = 5
MAX_CERTIFICATION_QUANTITY: Final = 1
SYSTEM_PATH_PRICE_INCREMENT_CURRENCY_NANOS: Final = 10_000_000
CERTIFICATION_LIMIT_PRICE: Final = Decimal("0.01")
MINIMUM_REFERENCE_BID: Final = Decimal("1.00")
DEFAULT_TIMEOUT_SECONDS: Final = 10.0
DEFAULT_POLL_ATTEMPTS: Final = 12
DEFAULT_POLL_INTERVAL_SECONDS: Final = 0.25
_SYMBOL = re.compile(r"^[A-Z][A-Z0-9]*(?:[.-][A-Z0-9]+)?$")
_CLIENT_ORDER_ID = re.compile(r"^aegis-paper-[0-9]{8}-[0-9a-f]{16}$")
_SYSTEM_CLIENT_ORDER_ID = re.compile(r"^[0-9a-f]{32}$")
_AUTHORIZATION_ID = re.compile(r"^[a-z0-9][a-z0-9_-]{2,127}$")
_TERMINAL_ORDER_STATES: Final = frozenset(
    {"canceled", "filled", "expired", "rejected", "replaced"}
)
_SECRET_KEYS: Final = frozenset(
    {
        "AEGIS_ALPACA_PAPER_ENDPOINT",
        "AEGIS_ALPACA_PAPER_KEY_ID",
        "AEGIS_ALPACA_PAPER_SECRET_KEY",
    }
)


class CertificationErrorCode(StrEnum):
    """Stable fail-closed outcomes for operator and audit automation."""

    INVALID_ARGUMENT = "INVALID_ARGUMENT"
    SECRET_FILE_UNSAFE = "SECRET_FILE_UNSAFE"  # noqa: S105  # pragma: allowlist secret
    CREDENTIAL_MALFORMED = "CREDENTIAL_MALFORMED"
    NON_PAPER_ENDPOINT = "NON_PAPER_ENDPOINT"
    AUTHORIZATION_INVALID = "AUTHORIZATION_INVALID"
    AUTHORIZATION_EXPIRED = "AUTHORIZATION_EXPIRED"
    UNIVERSE_MISMATCH = "UNIVERSE_MISMATCH"
    NETWORK_FAILURE = "NETWORK_FAILURE"
    RESPONSE_TOO_LARGE = "RESPONSE_TOO_LARGE"
    RESPONSE_MALFORMED = "RESPONSE_MALFORMED"
    PROVIDER_REJECTED = "PROVIDER_REJECTED"
    ACCOUNT_UNSAFE = "ACCOUNT_UNSAFE"
    MARKET_CLOSED = "MARKET_CLOSED"
    WRONG_SESSION = "WRONG_SESSION"
    ASSET_INELIGIBLE = "ASSET_INELIGIBLE"
    QUOTE_UNSAFE = "QUOTE_UNSAFE"
    ORDER_AMBIGUOUS = "ORDER_AMBIGUOUS"
    ORDER_MISMATCH = "ORDER_MISMATCH"
    CANCEL_UNCONFIRMED = "CANCEL_UNCONFIRMED"
    STATE_CHANGED = "STATE_CHANGED"
    REPORT_WRITE_FAILED = "REPORT_WRITE_FAILED"
    SYSTEM_PATH_INVALID = "SYSTEM_PATH_INVALID"
    SYSTEM_PATH_FAILED = "SYSTEM_PATH_FAILED"


class CertificationError(RuntimeError):
    """A bounded error that never embeds credentials or response bodies."""

    def __init__(self, code: CertificationErrorCode, message: str) -> None:
        """Create one typed, bounded certification failure."""
        self.code = code
        super().__init__(message[:512])


class SecretText:
    """Credential text whose string representations are always redacted."""

    __slots__ = ("__value",)

    def __init__(self, value: str) -> None:
        """Retain a bounded nonempty secret."""
        if (
            not value
            or "\x00" in value
            or len(value.encode("utf-8")) > MAX_SECRET_VALUE_BYTES
        ):
            raise CertificationError(
                CertificationErrorCode.CREDENTIAL_MALFORMED,
                "credential value is absent or malformed",
            )
        self.__value = value

    def reveal(self) -> str:
        """Reveal only at the HTTP authentication boundary."""
        return self.__value

    def __repr__(self) -> str:
        """Prevent accidental credential disclosure."""
        return "[REDACTED]"

    def __str__(self) -> str:
        """Prevent accidental credential disclosure."""
        return "[REDACTED]"


@dataclass(frozen=True, slots=True)
class PaperCredentials:
    """Validated Alpaca PAPER credentials bound to the fixed paper origin."""

    key_id: SecretText
    secret_key: SecretText
    source_path: Path
    origin: str = PAPER_ORIGIN


@dataclass(frozen=True, slots=True)
class HttpResponse:
    """Bounded provider response used by real and deterministic transports."""

    status: int
    body: bytes


class HttpTransport(Protocol):
    """Injectable HTTP boundary; unit tests never perform network access."""

    def request(
        self,
        *,
        method: str,
        origin: str,
        path: str,
        headers: Mapping[str, str],
        body: bytes | None,
        timeout_seconds: float,
    ) -> HttpResponse:
        """Perform one bounded request without following redirects."""


@dataclass(frozen=True, slots=True)
class SystemPathExecution:
    """Validated process result and immutable binary identity."""

    evidence: dict[str, object]
    binary_sha256: str


class SystemPathRunner(Protocol):
    """Injectable C++ router/risk/OMS/final-gateway path boundary."""

    def run(self, *, symbol: str, seed: int) -> SystemPathExecution:
        """Run the fixed PAPER path and return bounded evidence."""


class CppSystemPathRunner:
    """Invoke only the repository C++ certification binary without a shell."""

    def __init__(self, binary: Path = DEFAULT_SYSTEM_PATH_BINARY) -> None:
        """Bind an immutable executable path."""
        self._binary = binary

    def run(self, *, symbol: str, seed: int) -> SystemPathExecution:
        """Execute the deterministic path and validate its bounded JSON."""
        _require_symbol(symbol)
        if not 0 < seed < 2**64:
            raise CertificationError(
                CertificationErrorCode.INVALID_ARGUMENT,
                "system-path seed is outside uint64 range",
            )
        if self._binary.is_symlink():
            raise CertificationError(
                CertificationErrorCode.SYSTEM_PATH_FAILED,
                "system-path binary cannot be a symbolic link",
            )
        try:
            binary = self._binary.resolve(strict=True)
            descriptor = os.open(binary, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
            metadata = os.fstat(descriptor)
        except OSError as error:
            raise CertificationError(
                CertificationErrorCode.SYSTEM_PATH_FAILED,
                f"system-path binary is unavailable ({type(error).__name__})",
            ) from None
        if (
            not stat.S_ISREG(metadata.st_mode)
            or metadata.st_uid != os.getuid()
            or metadata.st_mode & 0o022
            or not os.access(binary, os.X_OK)
            or not 0 < metadata.st_size <= MAX_SYSTEM_PATH_BINARY_BYTES
        ):
            os.close(descriptor)
            raise CertificationError(
                CertificationErrorCode.SYSTEM_PATH_FAILED,
                "system-path binary is not an owner-controlled executable",
            )
        identity = (
            metadata.st_dev,
            metadata.st_ino,
            metadata.st_size,
            metadata.st_mtime_ns,
            metadata.st_ctime_ns,
        )
        try:
            binary_sha256 = _sha256_file_descriptor(descriptor)
            completed = subprocess.run(
                [
                    f"/proc/self/fd/{descriptor}",
                    "--symbol",
                    symbol,
                    "--seed",
                    str(seed),
                ],
                check=False,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                pass_fds=(descriptor,),
                timeout=10,
            )
            after = os.fstat(descriptor)
            after_identity = (
                after.st_dev,
                after.st_ino,
                after.st_size,
                after.st_mtime_ns,
                after.st_ctime_ns,
            )
            binary_changed = identity != after_identity or (
                binary_sha256 != _sha256_file_descriptor(descriptor)
            )
        except (OSError, subprocess.SubprocessError) as error:
            raise CertificationError(
                CertificationErrorCode.SYSTEM_PATH_FAILED,
                f"system-path execution failed ({type(error).__name__})",
            ) from None
        finally:
            os.close(descriptor)
        if binary_changed:
            raise CertificationError(
                CertificationErrorCode.SYSTEM_PATH_FAILED,
                "system-path binary changed during execution",
            )
        if completed.returncode != 0 or not 0 < len(completed.stdout) <= (
            MAX_SYSTEM_PATH_OUTPUT_BYTES
        ):
            raise CertificationError(
                CertificationErrorCode.SYSTEM_PATH_FAILED,
                "system-path binary rejected the certification request",
            )
        try:
            evidence = _object(json.loads(completed.stdout))
        except (UnicodeDecodeError, json.JSONDecodeError):
            raise CertificationError(
                CertificationErrorCode.SYSTEM_PATH_INVALID,
                "system-path output is malformed",
            ) from None
        _validate_system_path_evidence(evidence, symbol)
        return SystemPathExecution(
            evidence=evidence,
            binary_sha256=binary_sha256,
        )


class FixedHttpsTransport:
    """TLS-validating transport restricted to the two documented Alpaca hosts."""

    def request(
        self,
        *,
        method: str,
        origin: str,
        path: str,
        headers: Mapping[str, str],
        body: bytes | None,
        timeout_seconds: float,
    ) -> HttpResponse:
        """Perform one request and return no provider headers."""
        host = _validated_host(origin)
        if not path.startswith("/") or "\r" in path or "\n" in path:
            raise CertificationError(
                CertificationErrorCode.INVALID_ARGUMENT,
                "HTTP path is malformed",
            )
        connection = http.client.HTTPSConnection(host, timeout=timeout_seconds)
        try:
            connection.request(method, path, body=body, headers=dict(headers))
            response = connection.getresponse()
            payload = response.read(MAX_RESPONSE_BYTES + 1)
        except (OSError, http.client.HTTPException) as error:
            raise CertificationError(
                CertificationErrorCode.NETWORK_FAILURE,
                f"Alpaca HTTPS request failed ({type(error).__name__})",
            ) from None
        finally:
            connection.close()
        if len(payload) > MAX_RESPONSE_BYTES:
            raise CertificationError(
                CertificationErrorCode.RESPONSE_TOO_LARGE,
                "Alpaca response exceeded the configured byte limit",
            )
        return HttpResponse(response.status, payload)


@dataclass(frozen=True, slots=True)
class PaperAuthorization:
    """Durable operator scope for one bounded PAPER certification session."""

    authorization_id: str
    expected_session_date: date
    issued_at_utc: datetime
    expires_at_utc: datetime
    universe_snapshot_sha256: str
    configuration_sha256: str
    tool_source_sha256: str
    system_path_source_sha256: str
    maximum_orders: int = MAX_CERTIFICATION_ORDERS
    maximum_quantity_per_order: int = MAX_CERTIFICATION_QUANTITY
    order_submission_authorized: bool = True
    order_cancellation_authorized: bool = True
    preserve_existing_orders_positions: bool = True
    account_reset_authorized: bool = False
    short_selling_enabled: bool = False
    extended_hours_enabled: bool = False
    asset_class: str = "us_equity"
    mode: str = "PAPER"
    schema_version: str = AUTHORIZATION_SCHEMA_VERSION

    def validate(self, *, now_utc: datetime, universe_sha256: str) -> None:
        """Reject incomplete, stale, expanded, or differently scoped authority."""
        if (
            self.schema_version != AUTHORIZATION_SCHEMA_VERSION
            or self.mode != "PAPER"
            or self.asset_class != "us_equity"
            or _AUTHORIZATION_ID.fullmatch(self.authorization_id) is None
            or now_utc.tzinfo is None
            or now_utc.utcoffset() is None
            or self.issued_at_utc.tzinfo is None
            or self.expires_at_utc.tzinfo is None
            or self.issued_at_utc.utcoffset() is None
            or self.expires_at_utc.utcoffset() is None
            or self.issued_at_utc >= self.expires_at_utc
            or now_utc < self.issued_at_utc
            or not self.order_submission_authorized
            or not self.order_cancellation_authorized
            or not self.preserve_existing_orders_positions
            or self.account_reset_authorized
            or self.short_selling_enabled
            or self.extended_hours_enabled
            or not 1 <= self.maximum_orders <= MAX_CERTIFICATION_ORDERS
            or self.maximum_quantity_per_order != MAX_CERTIFICATION_QUANTITY
            or self.configuration_sha256 != _configuration_sha256()
            or self.tool_source_sha256 != _tool_source_sha256()
            or self.system_path_source_sha256 != _system_path_source_sha256()
        ):
            raise CertificationError(
                CertificationErrorCode.AUTHORIZATION_INVALID,
                "paper authorization expands or omits the approved scope",
            )
        if now_utc >= self.expires_at_utc:
            raise CertificationError(
                CertificationErrorCode.AUTHORIZATION_EXPIRED,
                "paper authorization has expired",
            )
        if self.universe_snapshot_sha256 != universe_sha256:
            raise CertificationError(
                CertificationErrorCode.UNIVERSE_MISMATCH,
                "paper authorization is bound to a different ticker universe",
            )

    def to_dict(self) -> dict[str, object]:
        """Return canonical non-secret authorization fields."""
        return {
            "account_reset_authorized": self.account_reset_authorized,
            "asset_class": self.asset_class,
            "authorization_id": self.authorization_id,
            "configuration_sha256": self.configuration_sha256,
            "expected_session_date": self.expected_session_date.isoformat(),
            "expires_at_utc": _utc_text(self.expires_at_utc),
            "extended_hours_enabled": self.extended_hours_enabled,
            "issued_at_utc": _utc_text(self.issued_at_utc),
            "maximum_orders": self.maximum_orders,
            "maximum_quantity_per_order": self.maximum_quantity_per_order,
            "mode": self.mode,
            "order_cancellation_authorized": self.order_cancellation_authorized,
            "order_submission_authorized": self.order_submission_authorized,
            "preserve_existing_orders_positions": (
                self.preserve_existing_orders_positions
            ),
            "schema_version": self.schema_version,
            "short_selling_enabled": self.short_selling_enabled,
            "system_path_source_sha256": self.system_path_source_sha256,
            "tool_source_sha256": self.tool_source_sha256,
            "universe_snapshot_sha256": self.universe_snapshot_sha256,
        }


def _canonical_json(value: object) -> bytes:
    return json.dumps(
        value, ensure_ascii=True, separators=(",", ":"), sort_keys=True
    ).encode("ascii")


def _sha256(value: object) -> str:
    payload = value if isinstance(value, bytes) else _canonical_json(value)
    return hashlib.sha256(payload).hexdigest()


def _sha256_file_descriptor(descriptor: int) -> str:
    hasher = hashlib.sha256()
    os.lseek(descriptor, 0, os.SEEK_SET)
    while chunk := os.read(descriptor, 1 << 20):
        hasher.update(chunk)
    return hasher.hexdigest()


def _fnv64(parts: Sequence[bytes]) -> int:
    value = 1_469_598_103_934_665_603
    for part in parts:
        for byte in part:
            value ^= byte
            value = (value * 1_099_511_628_211) & ((1 << 64) - 1)
    return value or 1


def _uint_bytes(value: object, field_name: str, width: int = 8) -> bytes:
    if (
        not isinstance(value, int)
        or isinstance(value, bool)
        or not 0 <= value < 1 << (width * 8)
    ):
        raise CertificationError(
            CertificationErrorCode.SYSTEM_PATH_INVALID,
            f"{field_name} is outside its unsigned integer range",
        )
    return value.to_bytes(width, "little")


def _int64_bytes(value: object, field_name: str) -> bytes:
    if (
        not isinstance(value, int)
        or isinstance(value, bool)
        or not -(1 << 63) <= value < 1 << 63
    ):
        raise CertificationError(
            CertificationErrorCode.SYSTEM_PATH_INVALID,
            f"{field_name} is outside int64 range",
        )
    return value.to_bytes(8, "little", signed=True)


def _fixed_ascii(value: object, field_name: str, width: int) -> bytes:
    if not isinstance(value, str):
        raise CertificationError(
            CertificationErrorCode.SYSTEM_PATH_INVALID,
            f"{field_name} is not text",
        )
    try:
        encoded = value.encode("ascii")
    except UnicodeEncodeError:
        raise CertificationError(
            CertificationErrorCode.SYSTEM_PATH_INVALID,
            f"{field_name} is not ASCII",
        ) from None
    if not encoded or len(encoded) >= width or b"\x00" in encoded:
        raise CertificationError(
            CertificationErrorCode.SYSTEM_PATH_INVALID,
            f"{field_name} is outside its fixed-width contract",
        )
    return encoded + bytes(width - len(encoded))


def _certification_instrument_id(symbol: str) -> tuple[int, int]:
    material = f"AEGIS-ALPACA-PAPER-INSTRUMENT-V1:{symbol}".encode("ascii")
    digest = hashlib.sha256(material).digest()
    return (
        int.from_bytes(digest[:8], "big"),
        int.from_bytes(digest[8:16], "big"),
    )


def _system_path_stable_hash(record: Mapping[str, object]) -> int:
    schema = record.get("schema_version")
    if schema != "1.0.0":
        raise CertificationError(
            CertificationErrorCode.SYSTEM_PATH_INVALID,
            "system-path schema version is unsupported",
        )
    parts = [
        _uint_bytes(1, "schema_major", 2),
        _uint_bytes(0, "schema_minor", 2),
        _uint_bytes(1, "stage", 1),
        _fixed_ascii(record.get("symbol"), "symbol", 17),
        _uint_bytes(record.get("instrument_id_high"), "instrument_id_high"),
        _uint_bytes(record.get("instrument_id_low"), "instrument_id_low"),
        _fixed_ascii(record.get("client_order_id"), "client_order_id", 33),
        _int64_bytes(record.get("price_ticks"), "price_ticks"),
        _uint_bytes(
            record.get("price_increment_currency_nanos"),
            "price_increment_currency_nanos",
        ),
        _uint_bytes(record.get("quantity_units"), "quantity_units"),
    ]
    integer_fields = (
        "router_configuration_hash",
        "routing_request_hash",
        "routing_decision_hash",
        "risk_snapshot_hash",
        "risk_context_hash",
        "risk_decision_hash",
        "risk_journal_sequence",
        "oms_configuration_hash",
        "oms_command_hash",
        "oms_journal_sequence",
        "gateway_configuration_hash",
        "gateway_request_hash",
        "gateway_audit_sequence",
        "gateway_audit_hash",
        "gateway_outbound_sequence",
    )
    parts.extend(
        _uint_bytes(record.get(field_name), field_name) for field_name in integer_fields
    )
    for field_name in (
        "paper_mode",
        "buy_only",
        "regular_hours_only",
        "fresh_pretrade_risk_required",
        "risk_approved",
        "oms_command_valid",
        "gateway_final_gate_accepted",
        "live_trading_compiled",
    ):
        field_value = record.get(field_name)
        if not isinstance(field_value, bool):
            raise CertificationError(
                CertificationErrorCode.SYSTEM_PATH_INVALID,
                f"{field_name} is not boolean",
            )
        parts.append(_uint_bytes(int(field_value), field_name, 4))
    return _fnv64(parts)


def _validate_system_path_evidence(
    record: Mapping[str, object], expected_symbol: str
) -> None:
    required = {
        "build_version",
        "buy_only",
        "client_order_id",
        "fresh_pretrade_risk_required",
        "gateway_audit_hash",
        "gateway_audit_sequence",
        "gateway_configuration_hash",
        "gateway_final_gate_accepted",
        "gateway_outbound_sequence",
        "gateway_request_hash",
        "instrument_id_high",
        "instrument_id_low",
        "live_trading_compiled",
        "mode",
        "oms_command_hash",
        "oms_command_valid",
        "oms_configuration_hash",
        "oms_journal_sequence",
        "paper_mode",
        "price_increment_currency_nanos",
        "price_ticks",
        "quantity_units",
        "regular_hours_only",
        "risk_approved",
        "risk_context_hash",
        "risk_decision_hash",
        "risk_journal_sequence",
        "risk_snapshot_hash",
        "router_configuration_hash",
        "routing_decision_hash",
        "routing_request_hash",
        "schema_version",
        "stable_hash",
        "stage",
        "symbol",
    }
    if set(record) != required:
        raise CertificationError(
            CertificationErrorCode.SYSTEM_PATH_INVALID,
            "system-path evidence has missing or unknown fields",
        )
    client_order_id = record.get("client_order_id")
    expected_instrument_id = _certification_instrument_id(expected_symbol)
    positive_fields = required - {
        "build_version",
        "buy_only",
        "client_order_id",
        "fresh_pretrade_risk_required",
        "gateway_final_gate_accepted",
        "live_trading_compiled",
        "mode",
        "oms_command_valid",
        "paper_mode",
        "price_increment_currency_nanos",
        "price_ticks",
        "quantity_units",
        "regular_hours_only",
        "risk_approved",
        "schema_version",
        "stage",
        "symbol",
    }
    if (
        record.get("symbol") != expected_symbol
        or record.get("instrument_id_high") != expected_instrument_id[0]
        or record.get("instrument_id_low") != expected_instrument_id[1]
        or record.get("mode") != "PAPER"
        or record.get("stage") != "COMPLETE"
        or not isinstance(record.get("build_version"), str)
        or _SYSTEM_CLIENT_ORDER_ID.fullmatch(str(client_order_id)) is None
        or record.get("price_ticks") != 1
        or record.get("price_increment_currency_nanos")
        != SYSTEM_PATH_PRICE_INCREMENT_CURRENCY_NANOS
        or record.get("quantity_units") != 1
        or record.get("paper_mode") is not True
        or record.get("buy_only") is not True
        or record.get("regular_hours_only") is not True
        or record.get("fresh_pretrade_risk_required") is not True
        or record.get("risk_approved") is not True
        or record.get("oms_command_valid") is not True
        or record.get("gateway_final_gate_accepted") is not True
        or record.get("live_trading_compiled") is not False
        or any(
            type(record.get(field_name)) is not int
            or cast("int", record.get(field_name)) <= 0
            for field_name in positive_fields
        )
        or record.get("stable_hash") != _system_path_stable_hash(record)
    ):
        raise CertificationError(
            CertificationErrorCode.SYSTEM_PATH_INVALID,
            "system-path evidence is unsafe, inconsistent, or tampered",
        )


def _utc_text(value: datetime) -> str:
    if value.tzinfo is None or value.utcoffset() is None:
        raise CertificationError(
            CertificationErrorCode.INVALID_ARGUMENT,
            "timestamp must include a UTC offset",
        )
    return value.astimezone(UTC).isoformat().replace("+00:00", "Z")


def _parse_timestamp(value: object, field_name: str) -> datetime:
    if not isinstance(value, str) or len(value) > MAX_TIMESTAMP_BYTES:
        raise CertificationError(
            CertificationErrorCode.RESPONSE_MALFORMED,
            f"{field_name} is not a bounded timestamp",
        )
    try:
        parsed = datetime.fromisoformat(value)
    except ValueError:
        raise CertificationError(
            CertificationErrorCode.RESPONSE_MALFORMED,
            f"{field_name} is malformed",
        ) from None
    if parsed.tzinfo is None or parsed.utcoffset() is None:
        raise CertificationError(
            CertificationErrorCode.RESPONSE_MALFORMED,
            f"{field_name} lacks an explicit offset",
        )
    return parsed


def _validated_host(origin: str) -> str:
    if origin not in {PAPER_ORIGIN, DATA_ORIGIN}:
        raise CertificationError(
            CertificationErrorCode.NON_PAPER_ENDPOINT,
            "origin is outside the fixed Alpaca PAPER allowlist",
        )
    parsed = urlsplit(origin)
    hostname = parsed.hostname
    if (
        parsed.scheme != "https"
        or hostname not in {"paper-api.alpaca.markets", "data.alpaca.markets"}
        or parsed.port is not None
        or parsed.username is not None
        or parsed.password is not None
        or parsed.path
        or parsed.query
        or parsed.fragment
    ):
        raise CertificationError(
            CertificationErrorCode.NON_PAPER_ENDPOINT,
            "origin is not an exact approved HTTPS origin",
        )
    return hostname


def _normalize_configured_endpoint(value: str) -> str:
    parsed = urlsplit(value)
    if (
        parsed.scheme != "https"
        or parsed.hostname != "paper-api.alpaca.markets"
        or parsed.port is not None
        or parsed.username is not None
        or parsed.password is not None
        or parsed.path.rstrip("/") not in {"", "/v2"}
        or parsed.query
        or parsed.fragment
    ):
        raise CertificationError(
            CertificationErrorCode.NON_PAPER_ENDPOINT,
            "configured endpoint is not the approved Alpaca PAPER API",
        )
    return PAPER_ORIGIN


def _read_private_file(path: Path, maximum_bytes: int, label: str) -> bytes:
    """Read an owner-only regular file through a no-follow descriptor."""
    flags = os.O_RDONLY | os.O_CLOEXEC
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    try:
        descriptor = os.open(path, flags)
    except OSError as error:
        raise CertificationError(
            CertificationErrorCode.SECRET_FILE_UNSAFE,
            f"{label} cannot be opened safely ({type(error).__name__})",
        ) from None
    try:
        metadata = os.fstat(descriptor)
        if (
            not stat.S_ISREG(metadata.st_mode)
            or metadata.st_uid != os.getuid()
            or metadata.st_mode & 0o077
            or not 0 < metadata.st_size <= maximum_bytes
        ):
            raise CertificationError(
                CertificationErrorCode.SECRET_FILE_UNSAFE,
                f"{label} must be owner-only, bounded, and regular",
            )
        chunks: list[bytes] = []
        remaining = maximum_bytes + 1
        while remaining:
            chunk = os.read(descriptor, remaining)
            if not chunk:
                break
            chunks.append(chunk)
            remaining -= len(chunk)
        payload = b"".join(chunks)
        final_metadata = os.fstat(descriptor)
    except OSError as error:
        raise CertificationError(
            CertificationErrorCode.SECRET_FILE_UNSAFE,
            f"{label} cannot be read safely ({type(error).__name__})",
        ) from None
    finally:
        os.close(descriptor)
    if (
        len(payload) != metadata.st_size
        or final_metadata.st_dev != metadata.st_dev
        or final_metadata.st_ino != metadata.st_ino
        or final_metadata.st_size != metadata.st_size
        or final_metadata.st_mtime_ns != metadata.st_mtime_ns
    ):
        raise CertificationError(
            CertificationErrorCode.SECRET_FILE_UNSAFE,
            f"{label} changed while it was being read",
        )
    return payload


def load_credentials(path: Path = DEFAULT_SECRET_FILE) -> PaperCredentials:
    """Load the allowlisted key file after ownership and mode checks."""
    try:
        payload = _read_private_file(path, MAX_SECRET_FILE_BYTES, "credential file")
        text = payload.decode("utf-8")
    except UnicodeDecodeError as error:
        raise CertificationError(
            CertificationErrorCode.SECRET_FILE_UNSAFE,
            f"credential file cannot be read safely ({type(error).__name__})",
        ) from None
    values: dict[str, str] = {}
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("export "):
            line = line[7:].strip()
        if "=" not in line:
            raise CertificationError(
                CertificationErrorCode.CREDENTIAL_MALFORMED,
                "credential file contains a malformed assignment",
            )
        key, raw_value = line.split("=", 1)
        key = key.strip()
        if key not in _SECRET_KEYS or key in values:
            raise CertificationError(
                CertificationErrorCode.CREDENTIAL_MALFORMED,
                "credential file contains an unknown or duplicate key",
            )
        value = raw_value.strip()
        if value[:1] in {'"', "'"} and value[-1:] == value[:1]:
            value = value[1:-1]
        values[key] = value
    if set(values) != _SECRET_KEYS:
        raise CertificationError(
            CertificationErrorCode.CREDENTIAL_MALFORMED,
            "credential file does not contain exactly the required paper keys",
        )
    origin = _normalize_configured_endpoint(values["AEGIS_ALPACA_PAPER_ENDPOINT"])
    return PaperCredentials(
        key_id=SecretText(values["AEGIS_ALPACA_PAPER_KEY_ID"]),
        secret_key=SecretText(values["AEGIS_ALPACA_PAPER_SECRET_KEY"]),
        source_path=path,
        origin=origin,
    )


class AlpacaPaperClient:
    """Minimal current public Alpaca API adapter for PAPER certification only."""

    def __init__(
        self,
        credentials: PaperCredentials,
        transport: HttpTransport,
        *,
        timeout_seconds: float = DEFAULT_TIMEOUT_SECONDS,
    ) -> None:
        """Bind credentials and transport to immutable origins and a timeout."""
        if (
            credentials.origin != PAPER_ORIGIN
            or not 0 < timeout_seconds <= MAX_CLIENT_TIMEOUT_SECONDS
        ):
            raise CertificationError(
                CertificationErrorCode.INVALID_ARGUMENT,
                "client endpoint or timeout is outside certification policy",
            )
        self._credentials = credentials
        self._transport = transport
        self._timeout_seconds = timeout_seconds
        self.request_count = 0
        self.mutation_count = 0
        self.order_submission_count = 0
        self.cancel_request_count = 0

    @property
    def credential_file_validated(self) -> bool:
        """Report secret custody validation without exposing a secret-derived hash."""
        return self._credentials.source_path.is_absolute()

    def _headers(self, *, json_body: bool) -> dict[str, str]:
        headers = {
            "Accept": "application/json",
            "APCA-API-KEY-ID": self._credentials.key_id.reveal(),
            "APCA-API-SECRET-KEY": self._credentials.secret_key.reveal(),
            "User-Agent": "Aegis-MX-paper-certification/1.0",
        }
        if json_body:
            headers["Content-Type"] = "application/json"
        return headers

    def _request_json(
        self,
        method: str,
        origin: str,
        path: str,
        *,
        payload: Mapping[str, object] | None = None,
        expected_statuses: frozenset[int] = frozenset({200}),
        mutation: bool = False,
    ) -> object:
        _validated_host(origin)
        body = None if payload is None else _canonical_json(payload)
        self.request_count += 1
        if mutation:
            self.mutation_count += 1
        response = self._transport.request(
            method=method,
            origin=origin,
            path=path,
            headers=self._headers(json_body=body is not None),
            body=body,
            timeout_seconds=self._timeout_seconds,
        )
        if len(response.body) > MAX_RESPONSE_BYTES:
            raise CertificationError(
                CertificationErrorCode.RESPONSE_TOO_LARGE,
                "Alpaca response exceeded the configured byte limit",
            )
        if response.status not in expected_statuses:
            raise CertificationError(
                CertificationErrorCode.PROVIDER_REJECTED,
                (
                    f"Alpaca rejected {method} {path.split('?', 1)[0]} "
                    f"with HTTP {response.status}"
                ),
            )
        if not response.body:
            return None
        try:
            return json.loads(response.body, parse_float=Decimal)
        except (UnicodeDecodeError, json.JSONDecodeError):
            raise CertificationError(
                CertificationErrorCode.RESPONSE_MALFORMED,
                "Alpaca returned malformed JSON",
            ) from None

    def account(self) -> dict[str, object]:
        """Retrieve the PAPER account state."""
        return _object(self._request_json("GET", PAPER_ORIGIN, "/v2/account"))

    def clock(self) -> dict[str, object]:
        """Retrieve the legacy equities market clock with explicit open state."""
        return _object(self._request_json("GET", PAPER_ORIGIN, "/v2/clock"))

    def asset(self, symbol: str) -> dict[str, object]:
        """Retrieve one asset using a validated universe symbol."""
        _require_symbol(symbol)
        return _object(
            self._request_json(
                "GET", PAPER_ORIGIN, f"/v2/assets/{quote(symbol, safe='.-')}"
            )
        )

    def open_orders(self) -> list[object]:
        """Snapshot existing US-equity open orders without mutating them."""
        query = urlencode(
            {
                "asset_class": "us_equity",
                "direction": "asc",
                "limit": "500",
                "status": "open",
            }
        )
        return _array(self._request_json("GET", PAPER_ORIGIN, f"/v2/orders?{query}"))

    def positions(self) -> list[object]:
        """Snapshot all positions without closing or changing them."""
        return _array(self._request_json("GET", PAPER_ORIGIN, "/v2/positions"))

    def latest_iex_quote(self, symbol: str) -> dict[str, object]:
        """Retrieve an ephemeral IEX quote used only as a nonmarketability guard."""
        _require_symbol(symbol)
        query = urlencode({"feed": "iex"})
        return _object(
            self._request_json(
                "GET",
                DATA_ORIGIN,
                f"/v2/stocks/{quote(symbol, safe='.-')}/quotes/latest?{query}",
            )
        )

    def submit_nonmarketable_buy(
        self, *, symbol: str, client_order_id: str
    ) -> dict[str, object]:
        """Submit exactly one share at one cent, never extended hours."""
        _require_symbol(symbol)
        if _CLIENT_ORDER_ID.fullmatch(client_order_id) is None:
            raise CertificationError(
                CertificationErrorCode.INVALID_ARGUMENT,
                "client order ID is outside the certification namespace",
            )
        self.order_submission_count += 1
        return _object(
            self._request_json(
                "POST",
                PAPER_ORIGIN,
                "/v2/orders",
                payload={
                    "client_order_id": client_order_id,
                    "extended_hours": False,
                    "limit_price": format(CERTIFICATION_LIMIT_PRICE, "f"),
                    "order_class": "simple",
                    "position_intent": "buy_to_open",
                    "qty": str(MAX_CERTIFICATION_QUANTITY),
                    "side": "buy",
                    "symbol": symbol,
                    "time_in_force": "day",
                    "type": "limit",
                },
                mutation=True,
            )
        )

    def submit_system_path_buy(
        self, *, symbol: str, client_order_id: str
    ) -> dict[str, object]:
        """Submit only the fixed order emitted by the C++ deterministic path."""
        _require_symbol(symbol)
        if _SYSTEM_CLIENT_ORDER_ID.fullmatch(client_order_id) is None:
            raise CertificationError(
                CertificationErrorCode.SYSTEM_PATH_INVALID,
                "client order ID is not an OMS deterministic identifier",
            )
        self.order_submission_count += 1
        return _object(
            self._request_json(
                "POST",
                PAPER_ORIGIN,
                "/v2/orders",
                payload={
                    "client_order_id": client_order_id,
                    "extended_hours": False,
                    "limit_price": format(CERTIFICATION_LIMIT_PRICE, "f"),
                    "order_class": "simple",
                    "position_intent": "buy_to_open",
                    "qty": str(MAX_CERTIFICATION_QUANTITY),
                    "side": "buy",
                    "symbol": symbol,
                    "time_in_force": "day",
                    "type": "limit",
                },
                mutation=True,
            )
        )

    def order_by_id(self, order_id: str) -> dict[str, object]:
        """Retrieve only the certification-created order."""
        normalized = _require_uuid(order_id)
        return _object(
            self._request_json("GET", PAPER_ORIGIN, f"/v2/orders/{normalized}")
        )

    def order_by_client_id(self, client_order_id: str) -> dict[str, object]:
        """Resolve an ambiguous POST by its deterministic client order ID."""
        if (
            _CLIENT_ORDER_ID.fullmatch(client_order_id) is None
            and _SYSTEM_CLIENT_ORDER_ID.fullmatch(client_order_id) is None
        ):
            raise CertificationError(
                CertificationErrorCode.INVALID_ARGUMENT,
                "client order ID is outside the certification namespace",
            )
        query = urlencode({"client_order_id": client_order_id})
        return _object(
            self._request_json(
                "GET", PAPER_ORIGIN, f"/v2/orders:by_client_order_id?{query}"
            )
        )

    def cancel_order(self, order_id: str) -> None:
        """Cancel only the explicit certification-created order UUID."""
        normalized = _require_uuid(order_id)
        self.cancel_request_count += 1
        response = self._request_json(
            "DELETE",
            PAPER_ORIGIN,
            f"/v2/orders/{normalized}",
            expected_statuses=frozenset({204}),
            mutation=True,
        )
        if response is not None:
            raise CertificationError(
                CertificationErrorCode.RESPONSE_MALFORMED,
                "successful cancel unexpectedly returned a body",
            )


def _object(value: object) -> dict[str, object]:
    if not isinstance(value, dict):
        raise CertificationError(
            CertificationErrorCode.RESPONSE_MALFORMED,
            "Alpaca response is not an object",
        )
    return cast("dict[str, object]", value)


def _array(value: object) -> list[object]:
    if not isinstance(value, list):
        raise CertificationError(
            CertificationErrorCode.RESPONSE_MALFORMED,
            "Alpaca response is not an array",
        )
    return cast("list[object]", value)


def _require_symbol(symbol: str) -> None:
    if len(symbol) > MAX_SYMBOL_BYTES or _SYMBOL.fullmatch(symbol) is None:
        raise CertificationError(
            CertificationErrorCode.INVALID_ARGUMENT,
            "symbol is malformed",
        )


def _require_uuid(value: str) -> str:
    try:
        return str(uuid.UUID(value))
    except (ValueError, AttributeError):
        raise CertificationError(
            CertificationErrorCode.INVALID_ARGUMENT,
            "order ID is not a UUID",
        ) from None


def _boolean(value: object, field_name: str) -> bool:
    if type(value) is not bool:
        raise CertificationError(
            CertificationErrorCode.RESPONSE_MALFORMED,
            f"{field_name} is not boolean",
        )
    return value


def _text(value: object, field_name: str, *, maximum: int = 128) -> str:
    if not isinstance(value, str) or not value or len(value) > maximum:
        raise CertificationError(
            CertificationErrorCode.RESPONSE_MALFORMED,
            f"{field_name} is not bounded text",
        )
    return value


def _decimal(value: object, field_name: str) -> Decimal:
    if not isinstance(value, (str, int, Decimal)) or isinstance(value, bool):
        raise CertificationError(
            CertificationErrorCode.RESPONSE_MALFORMED,
            f"{field_name} is not an exact decimal",
        )
    try:
        result = Decimal(str(value))
    except InvalidOperation:
        raise CertificationError(
            CertificationErrorCode.RESPONSE_MALFORMED,
            f"{field_name} is malformed",
        ) from None
    if not result.is_finite():
        raise CertificationError(
            CertificationErrorCode.RESPONSE_MALFORMED,
            f"{field_name} is not finite",
        )
    return result


def _state_digest(items: Sequence[object], fields: Sequence[str]) -> str:
    projection: list[dict[str, object]] = []
    for item in items:
        record = _object(item)
        projection.append({field: record.get(field) for field in fields})
    projection.sort(key=_canonical_json)
    return _sha256(projection)


def _validate_account(account: Mapping[str, object]) -> str:
    account_id = _text(account.get("id"), "account.id")
    safe = (
        _text(account.get("status"), "account.status") == "ACTIVE"
        and not _boolean(account.get("account_blocked", False), "account_blocked")
        and not _boolean(account.get("trading_blocked", False), "trading_blocked")
        and not _boolean(
            account.get("trade_suspended_by_user", False),
            "trade_suspended_by_user",
        )
    )
    if not safe:
        raise CertificationError(
            CertificationErrorCode.ACCOUNT_UNSAFE,
            "paper account is not active and trading-enabled",
        )
    return _sha256(account_id.encode("utf-8"))


def _validate_asset(asset: Mapping[str, object], symbol: str) -> None:
    if (
        _text(asset.get("symbol"), "asset.symbol") != symbol
        or _text(asset.get("class"), "asset.class") != "us_equity"
        or _text(asset.get("status"), "asset.status") != "active"
        or not _boolean(asset.get("tradable"), "asset.tradable")
    ):
        raise CertificationError(
            CertificationErrorCode.ASSET_INELIGIBLE,
            "selected symbol is not an active, tradable US equity",
        )


def _clock_state(
    clock: Mapping[str, object],
) -> tuple[datetime, bool, datetime, datetime]:
    timestamp = _parse_timestamp(clock.get("timestamp"), "clock.timestamp")
    is_open = _boolean(clock.get("is_open"), "clock.is_open")
    next_open = _parse_timestamp(clock.get("next_open"), "clock.next_open")
    next_close = _parse_timestamp(clock.get("next_close"), "clock.next_close")
    return timestamp, is_open, next_open, next_close


def _universe() -> tuple[tuple[str, ...], str]:
    snapshot = load_ticker_universe(UnresolvedInstrumentResolver())
    symbols = tuple(entry.symbol for entry in snapshot.ordered_entries)
    return symbols, snapshot.universe_snapshot_sha256.hex()


def _configuration_sha256() -> str:
    return _sha256(
        {
            "asset_class": "us_equity",
            "data_feed": "iex",
            "endpoint": PAPER_ORIGIN,
            "extended_hours": False,
            "limit_price": format(CERTIFICATION_LIMIT_PRICE, "f"),
            "maximum_orders": MAX_CERTIFICATION_ORDERS,
            "maximum_quantity": MAX_CERTIFICATION_QUANTITY,
            "minimum_reference_bid": format(MINIMUM_REFERENCE_BID, "f"),
            "short_selling": False,
        }
    )


def _tool_source_sha256() -> str:
    try:
        return _sha256(Path(__file__).read_bytes())
    except OSError as error:
        raise CertificationError(
            CertificationErrorCode.REPORT_WRITE_FAILED,
            f"tool source identity cannot be read ({type(error).__name__})",
        ) from None


def _system_path_source_sha256() -> str:
    repository_root = Path(__file__).resolve().parent.parent
    roots = (
        repository_root / "cpp",
        repository_root / "schemas" / "aegis_mx",
        repository_root / "schemas" / "generated" / "cpp",
    )
    source_files = {
        repository_root / "CMakeLists.txt",
        repository_root / "CMakePresets.json",
    }
    for root in roots:
        source_files.update(
            path
            for path in root.rglob("*")
            if path.name == "CMakeLists.txt"
            or path.suffix in {".cpp", ".fbs", ".h", ".hpp"}
        )
    hasher = hashlib.sha256()
    total_bytes = 0
    try:
        for path in sorted(source_files):
            metadata = path.lstat()
            if (
                path.is_symlink()
                or not stat.S_ISREG(metadata.st_mode)
                or not 0 < metadata.st_size <= MAX_SYSTEM_PATH_SOURCE_FILE_BYTES
            ):
                raise CertificationError(
                    CertificationErrorCode.AUTHORIZATION_INVALID,
                    "system-path source tree contains an unsafe file",
                )
            total_bytes += metadata.st_size
            if total_bytes > MAX_SYSTEM_PATH_SOURCE_TREE_BYTES:
                raise CertificationError(
                    CertificationErrorCode.AUTHORIZATION_INVALID,
                    "system-path source tree exceeds its byte limit",
                )
            relative = path.relative_to(repository_root).as_posix().encode("ascii")
            payload = path.read_bytes()
            hasher.update(len(relative).to_bytes(4, "big"))
            hasher.update(relative)
            hasher.update(len(payload).to_bytes(8, "big"))
            hasher.update(payload)
    except OSError as error:
        raise CertificationError(
            CertificationErrorCode.AUTHORIZATION_INVALID,
            f"system-path source identity cannot be read ({type(error).__name__})",
        ) from None
    return hasher.hexdigest()


def _preflight_state(client: AlpacaPaperClient, symbol: str) -> dict[str, object]:
    account = client.account()
    clock = client.clock()
    asset = client.asset(symbol)
    open_orders = client.open_orders()
    positions = client.positions()
    account_hash = _validate_account(account)
    _validate_asset(asset, symbol)
    timestamp, is_open, next_open, next_close = _clock_state(clock)
    return {
        "account_reference_sha256": account_hash,
        "asset_eligible": True,
        "clock_is_open": is_open,
        "clock_timestamp": _utc_text(timestamp),
        "next_close": _utc_text(next_close),
        "next_open": _utc_text(next_open),
        "open_orders_count": len(open_orders),
        "open_orders_sha256": _state_digest(
            open_orders,
            ("id", "client_order_id", "symbol", "qty", "side", "status"),
        ),
        "positions_count": len(positions),
        "positions_sha256": _state_digest(
            positions, ("asset_id", "symbol", "qty", "side")
        ),
    }


def run_preflight(
    client: AlpacaPaperClient,
    *,
    symbol: str,
    observed_at_utc: datetime,
) -> dict[str, object]:
    """Run an authenticated read-only PAPER preflight and emit no account values."""
    _require_symbol(symbol)
    symbols, universe_sha256 = _universe()
    if symbol not in symbols:
        raise CertificationError(
            CertificationErrorCode.UNIVERSE_MISMATCH,
            "selected symbol is outside ticker.txt",
        )
    state = _preflight_state(client, symbol)
    report: dict[str, object] = {
        "account_reset_performed": False,
        "build_version": BUILD_VERSION,
        "cancel_request_count": client.cancel_request_count,
        "certification_kind": "alpaca-paper-connectivity",
        "configuration_sha256": _configuration_sha256(),
        "credential_file_validated": client.credential_file_validated,
        "endpoint": PAPER_ORIGIN,
        "health_state": "HEALTHY",
        "live_trading_enabled": False,
        "mode": "PAPER",
        "mutation_count": client.mutation_count,
        "observed_at_utc": _utc_text(observed_at_utc),
        "order_submission_count": client.order_submission_count,
        "phase": "PREFLIGHT",
        "provider": "alpaca",
        "readiness_state": "READ_ONLY_READY",
        "request_count": client.request_count,
        "schema_version": REPORT_SCHEMA_VERSION,
        "selected_symbol": symbol,
        "state": state,
        "status": "READY" if state["asset_eligible"] else "BLOCKED",
        "system_order_path_certified": False,
        "ticker_count": len(symbols),
        "ticker_universe_path": str(TICKER_UNIVERSE_PATH),
        "tool_source_sha256": _tool_source_sha256(),
        "universe_snapshot_sha256": universe_sha256,
    }
    report["report_sha256"] = _sha256(report)
    return report


def _authorization_from_dict(value: object) -> PaperAuthorization:
    record = _object(value)
    required = {
        "account_reset_authorized",
        "asset_class",
        "authorization_id",
        "configuration_sha256",
        "expected_session_date",
        "expires_at_utc",
        "extended_hours_enabled",
        "issued_at_utc",
        "maximum_orders",
        "maximum_quantity_per_order",
        "mode",
        "order_cancellation_authorized",
        "order_submission_authorized",
        "preserve_existing_orders_positions",
        "schema_version",
        "short_selling_enabled",
        "system_path_source_sha256",
        "tool_source_sha256",
        "universe_snapshot_sha256",
    }
    if set(record) != required:
        raise CertificationError(
            CertificationErrorCode.AUTHORIZATION_INVALID,
            "paper authorization has missing or unknown fields",
        )
    try:
        session_date = date.fromisoformat(
            _text(record["expected_session_date"], "expected_session_date")
        )
    except ValueError:
        raise CertificationError(
            CertificationErrorCode.AUTHORIZATION_INVALID,
            "expected session date is malformed",
        ) from None
    issued = _parse_timestamp(record["issued_at_utc"], "issued_at_utc")
    expires = _parse_timestamp(record["expires_at_utc"], "expires_at_utc")
    integer_fields = ("maximum_orders", "maximum_quantity_per_order")
    if any(type(record[field]) is not int for field in integer_fields):
        raise CertificationError(
            CertificationErrorCode.AUTHORIZATION_INVALID,
            "authorization bounds must be integers",
        )
    boolean_fields = (
        "account_reset_authorized",
        "extended_hours_enabled",
        "order_cancellation_authorized",
        "order_submission_authorized",
        "preserve_existing_orders_positions",
        "short_selling_enabled",
    )
    if any(type(record[field]) is not bool for field in boolean_fields):
        raise CertificationError(
            CertificationErrorCode.AUTHORIZATION_INVALID,
            "authorization switches must be boolean",
        )
    return PaperAuthorization(
        authorization_id=_text(record["authorization_id"], "authorization_id"),
        expected_session_date=session_date,
        issued_at_utc=issued,
        expires_at_utc=expires,
        universe_snapshot_sha256=_text(
            record["universe_snapshot_sha256"], "universe_snapshot_sha256"
        ),
        configuration_sha256=_text(
            record["configuration_sha256"], "configuration_sha256"
        ),
        system_path_source_sha256=_text(
            record["system_path_source_sha256"], "system_path_source_sha256"
        ),
        tool_source_sha256=_text(record["tool_source_sha256"], "tool_source_sha256"),
        maximum_orders=cast("int", record["maximum_orders"]),
        maximum_quantity_per_order=cast("int", record["maximum_quantity_per_order"]),
        order_submission_authorized=cast("bool", record["order_submission_authorized"]),
        order_cancellation_authorized=cast(
            "bool", record["order_cancellation_authorized"]
        ),
        preserve_existing_orders_positions=cast(
            "bool", record["preserve_existing_orders_positions"]
        ),
        account_reset_authorized=cast("bool", record["account_reset_authorized"]),
        short_selling_enabled=cast("bool", record["short_selling_enabled"]),
        extended_hours_enabled=cast("bool", record["extended_hours_enabled"]),
        asset_class=_text(record["asset_class"], "asset_class"),
        mode=_text(record["mode"], "mode"),
        schema_version=_text(record["schema_version"], "schema_version"),
    )


def load_authorization(path: Path) -> tuple[PaperAuthorization, str]:
    """Load a non-secret scope record and bind it by SHA-256."""
    try:
        payload = _read_private_file(
            path, MAX_AUTHORIZATION_BYTES, "paper authorization file"
        )
        value = json.loads(payload)
    except CertificationError as error:
        raise CertificationError(
            CertificationErrorCode.AUTHORIZATION_INVALID,
            str(error),
        ) from None
    except (UnicodeDecodeError, json.JSONDecodeError):
        raise CertificationError(
            CertificationErrorCode.AUTHORIZATION_INVALID,
            "paper authorization file is empty, oversized, or malformed",
        ) from None
    return _authorization_from_dict(value), _sha256(payload)


def _order_identity(order: Mapping[str, object], symbol: str, client_id: str) -> str:
    order_id = _require_uuid(_text(order.get("id"), "order.id"))
    order_type = order.get("type", order.get("order_type"))
    if (
        _text(order.get("symbol"), "order.symbol") != symbol
        or _text(order.get("client_order_id"), "order.client_order_id") != client_id
        or _decimal(order.get("qty"), "order.qty") != Decimal(1)
        or _text(order.get("side"), "order.side") != "buy"
        or _text(order_type, "order.type") != "limit"
        or _text(order.get("time_in_force"), "order.time_in_force") != "day"
        or _decimal(order.get("limit_price"), "order.limit_price")
        != CERTIFICATION_LIMIT_PRICE
        or _boolean(order.get("extended_hours"), "order.extended_hours")
        or _text(order.get("asset_class"), "order.asset_class") != "us_equity"
    ):
        raise CertificationError(
            CertificationErrorCode.ORDER_MISMATCH,
            "provider order does not match the bounded certification request",
        )
    return order_id


def _quote_bid(quote_record: Mapping[str, object]) -> Decimal:
    quote_value = quote_record.get("quote", quote_record)
    quote_object = _object(quote_value)
    bid = quote_object.get("bp", quote_object.get("bid_price"))
    result = _decimal(bid, "quote.bid_price")
    if result < MINIMUM_REFERENCE_BID:
        raise CertificationError(
            CertificationErrorCode.QUOTE_UNSAFE,
            "IEX reference bid is absent or below the certification safety floor",
        )
    return result


def run_certification(
    client: AlpacaPaperClient,
    *,
    symbol: str,
    authorization: PaperAuthorization,
    authorization_sha256: str,
    execute_paper: bool,
    observed_at_utc: datetime,
    poll_attempts: int = DEFAULT_POLL_ATTEMPTS,
    poll_interval_seconds: float = DEFAULT_POLL_INTERVAL_SECONDS,
    sleeper: Callable[[float], None] = time.sleep,
    system_path_runner: SystemPathRunner | None = None,
    system_path_seed: int = 20_260_911,
) -> dict[str, object]:
    """Submit and cancel one deeply nonmarketable PAPER order."""
    if not execute_paper:
        raise CertificationError(
            CertificationErrorCode.AUTHORIZATION_INVALID,
            "certification mutation requires --execute-paper",
        )
    if (
        not 1 <= poll_attempts <= MAX_POLL_ATTEMPTS
        or not 0 <= poll_interval_seconds <= 1
    ):
        raise CertificationError(
            CertificationErrorCode.INVALID_ARGUMENT,
            "poll policy is outside bounded certification limits",
        )
    symbols, universe_sha256 = _universe()
    if symbol not in symbols:
        raise CertificationError(
            CertificationErrorCode.UNIVERSE_MISMATCH,
            "selected symbol is outside ticker.txt",
        )
    authorization.validate(now_utc=observed_at_utc, universe_sha256=universe_sha256)
    before = _preflight_state(client, symbol)
    clock_time = _parse_timestamp(before["clock_timestamp"], "clock_timestamp")
    session_date = clock_time.astimezone(ZoneInfo("America/New_York")).date()
    if session_date != authorization.expected_session_date:
        raise CertificationError(
            CertificationErrorCode.WRONG_SESSION,
            "provider clock is outside the authorized regular session date",
        )
    if before["clock_is_open"] is not True:
        raise CertificationError(
            CertificationErrorCode.MARKET_CLOSED,
            "provider clock reports that the regular equities session is closed",
        )
    reference_bid = _quote_bid(client.latest_iex_quote(symbol))
    if reference_bid <= CERTIFICATION_LIMIT_PRICE:
        raise CertificationError(
            CertificationErrorCode.QUOTE_UNSAFE,
            "certification limit is not below the IEX reference bid",
        )
    path_execution: SystemPathExecution | None = None
    if system_path_runner is None:
        nonce = hashlib.sha256(
            f"{authorization.authorization_id}:{symbol}:{session_date}".encode("ascii")
        ).hexdigest()[:16]
        client_order_id = f"aegis-paper-{session_date:%Y%m%d}-{nonce}"
    else:
        path_execution = system_path_runner.run(symbol=symbol, seed=system_path_seed)
        _validate_system_path_evidence(path_execution.evidence, symbol)
        client_order_id = _text(
            path_execution.evidence.get("client_order_id"),
            "system_path.client_order_id",
        )
    try:
        if path_execution is None:
            submitted = client.submit_nonmarketable_buy(
                symbol=symbol, client_order_id=client_order_id
            )
        else:
            submitted = client.submit_system_path_buy(
                symbol=symbol, client_order_id=client_order_id
            )
    except CertificationError as error:
        if error.code is not CertificationErrorCode.NETWORK_FAILURE:
            raise
        try:
            submitted = client.order_by_client_id(client_order_id)
        except CertificationError:
            raise CertificationError(
                CertificationErrorCode.ORDER_AMBIGUOUS,
                "order submission outcome is ambiguous; no second submission attempted",
            ) from None
    order_id = _order_identity(submitted, symbol, client_order_id)
    status = _text(submitted.get("status"), "order.status")
    if status in _TERMINAL_ORDER_STATES:
        raise CertificationError(
            CertificationErrorCode.CANCEL_UNCONFIRMED,
            f"certification order became terminal before targeted cancel: {status}",
        )
    client.cancel_order(order_id)
    final_order = submitted
    for _attempt in range(poll_attempts):
        final_order = client.order_by_id(order_id)
        _order_identity(final_order, symbol, client_order_id)
        status = _text(final_order.get("status"), "order.status")
        if status in _TERMINAL_ORDER_STATES:
            break
        sleeper(poll_interval_seconds)
    if status != "canceled":
        raise CertificationError(
            CertificationErrorCode.CANCEL_UNCONFIRMED,
            f"certification order ended in non-canceled state {status}",
        )
    filled_quantity = _decimal(final_order.get("filled_qty", "0"), "filled_qty")
    after_orders = client.open_orders()
    after_positions = client.positions()
    after_orders_hash = _state_digest(
        after_orders,
        ("id", "client_order_id", "symbol", "qty", "side", "status"),
    )
    after_positions_hash = _state_digest(
        after_positions, ("asset_id", "symbol", "qty", "side")
    )
    state_preserved = (
        before["open_orders_sha256"] == after_orders_hash
        and before["positions_sha256"] == after_positions_hash
        and filled_quantity == 0
    )
    if not state_preserved:
        raise CertificationError(
            CertificationErrorCode.STATE_CHANGED,
            "existing order or position state changed during certification",
        )
    report: dict[str, object] = {
        "account_reset_performed": False,
        "authorization_sha256": authorization_sha256,
        "build_version": BUILD_VERSION,
        "cancel_request_count": client.cancel_request_count,
        "cancel_confirmed": True,
        "broker_response_roundtrip_certified": False,
        "certification_kind": (
            "alpaca-paper-connectivity"
            if path_execution is None
            else "alpaca-paper-system-command-path"
        ),
        "client_order_id_sha256": _sha256(client_order_id.encode("ascii")),
        "configuration_sha256": _configuration_sha256(),
        "credential_file_validated": client.credential_file_validated,
        "endpoint": PAPER_ORIGIN,
        "existing_state_preserved": True,
        "filled_quantity": "0",
        "health_state": "HEALTHY",
        "live_trading_enabled": False,
        "mode": "PAPER",
        "mutation_count": client.mutation_count,
        "observed_at_utc": _utc_text(observed_at_utc),
        "order_limit_price": format(CERTIFICATION_LIMIT_PRICE, "f"),
        "order_quantity": MAX_CERTIFICATION_QUANTITY,
        "order_submission_count": client.order_submission_count,
        "phase": "CERTIFICATION",
        "provider": "alpaca",
        "readiness_state": "COMPLETE",
        "request_count": client.request_count,
        "schema_version": REPORT_SCHEMA_VERSION,
        "selected_symbol": symbol,
        "status": "PASSED",
        "system_order_path_certified": path_execution is not None,
        "system_path_source_sha256": authorization.system_path_source_sha256,
        "system_path_binary_sha256": (
            None if path_execution is None else path_execution.binary_sha256
        ),
        "system_path_evidence_sha256": (
            None if path_execution is None else _sha256(path_execution.evidence)
        ),
        "ticker_count": len(symbols),
        "tool_source_sha256": _tool_source_sha256(),
        "universe_snapshot_sha256": universe_sha256,
    }
    report["report_sha256"] = _sha256(report)
    return report


def write_json_atomic(path: Path, value: Mapping[str, object]) -> str:
    """Publish one private JSON record atomically without following a target symlink."""
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.is_symlink():
        raise CertificationError(
            CertificationErrorCode.REPORT_WRITE_FAILED,
            "output path cannot be a symlink",
        )
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", dir=path.parent
    )
    try:
        os.fchmod(descriptor, 0o600)
        payload = json.dumps(value, indent=2, sort_keys=True).encode("ascii") + b"\n"
        with os.fdopen(descriptor, "wb") as output:
            output.write(payload)
            output.flush()
            os.fsync(output.fileno())
        Path(temporary_name).replace(path)
    except OSError as error:
        with suppress(OSError):
            Path(temporary_name).unlink()
        raise CertificationError(
            CertificationErrorCode.REPORT_WRITE_FAILED,
            f"output publication failed ({type(error).__name__})",
        ) from None
    return _sha256(payload)


def prepare_authorization(
    *,
    authorization_id: str,
    expected_session_date: date,
    issued_at_utc: datetime,
    expires_at_utc: datetime,
) -> PaperAuthorization:
    """Construct the exact user-approved bounded PAPER scope."""
    _symbols, universe_sha256 = _universe()
    authorization = PaperAuthorization(
        authorization_id=authorization_id,
        expected_session_date=expected_session_date,
        issued_at_utc=issued_at_utc,
        expires_at_utc=expires_at_utc,
        universe_snapshot_sha256=universe_sha256,
        configuration_sha256=_configuration_sha256(),
        system_path_source_sha256=_system_path_source_sha256(),
        tool_source_sha256=_tool_source_sha256(),
    )
    authorization.validate(now_utc=issued_at_utc, universe_sha256=universe_sha256)
    return authorization


def _date_argument(value: str) -> date:
    try:
        return date.fromisoformat(value)
    except ValueError:
        message = "date must use YYYY-MM-DD"
        raise argparse.ArgumentTypeError(message) from None


def _timestamp_argument(value: str) -> datetime:
    try:
        parsed = datetime.fromisoformat(value)
    except ValueError:
        message = "timestamp must use ISO-8601"
        raise argparse.ArgumentTypeError(message) from None
    if parsed.tzinfo is None or parsed.utcoffset() is None:
        message = "timestamp must include an offset"
        raise argparse.ArgumentTypeError(message)
    return parsed


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="aegis-alpaca-paper",
        description="Fixed-host Alpaca PAPER connectivity certification",
    )
    parser.add_argument("--secret-file", type=Path, default=DEFAULT_SECRET_FILE)
    subcommands = parser.add_subparsers(dest="command", required=True)

    authorize = subcommands.add_parser(
        "prepare-authorization", help="Write a bounded non-secret scope record."
    )
    authorize.add_argument("--authorization-id", required=True)
    authorize.add_argument(
        "--expected-session-date", required=True, type=_date_argument
    )
    authorize.add_argument("--issued-at-utc", required=True, type=_timestamp_argument)
    authorize.add_argument("--expires-at-utc", required=True, type=_timestamp_argument)
    authorize.add_argument("--output", required=True, type=Path)

    preflight = subcommands.add_parser(
        "preflight", help="Perform authenticated read-only PAPER checks."
    )
    preflight.add_argument("--symbol", default="NVDA")
    preflight.add_argument("--report", type=Path)

    certify = subcommands.add_parser(
        "certify", help="Submit and cancel one bounded PAPER order."
    )
    certify.add_argument("--symbol", default="NVDA")
    certify.add_argument("--authorization", required=True, type=Path)
    certify.add_argument("--report", required=True, type=Path)
    certify.add_argument("--execute-paper", action="store_true")

    system_path = subcommands.add_parser(
        "certify-system-path",
        help="Run router/risk/OMS/final-gateway evidence before one PAPER order.",
    )
    system_path.add_argument("--symbol", default="NVDA")
    system_path.add_argument("--authorization", required=True, type=Path)
    system_path.add_argument("--report", required=True, type=Path)
    system_path.add_argument(
        "--path-binary", type=Path, default=DEFAULT_SYSTEM_PATH_BINARY
    )
    system_path.add_argument("--seed", type=int, default=20_260_911)
    system_path.add_argument("--execute-paper", action="store_true")
    return parser


def main(
    arguments: Sequence[str] | None = None,
    *,
    transport: HttpTransport | None = None,
    now_utc: datetime | None = None,
) -> int:
    """Run one operator action and emit only redacted structured output."""
    parsed = _parser().parse_args(arguments)
    observed = datetime.now(UTC) if now_utc is None else now_utc
    try:
        if parsed.command == "prepare-authorization":
            authorization = prepare_authorization(
                authorization_id=parsed.authorization_id,
                expected_session_date=parsed.expected_session_date,
                issued_at_utc=parsed.issued_at_utc,
                expires_at_utc=parsed.expires_at_utc,
            )
            report = authorization.to_dict()
            authorization_hash = write_json_atomic(parsed.output, report)
            output: dict[str, object] = {
                "authorization_sha256": authorization_hash,
                "command": parsed.command,
                "output": str(parsed.output),
                "status": "PREPARED",
            }
        else:
            credentials = load_credentials(parsed.secret_file)
            client = AlpacaPaperClient(
                credentials,
                FixedHttpsTransport() if transport is None else transport,
            )
            if parsed.command == "preflight":
                report = run_preflight(
                    client, symbol=parsed.symbol, observed_at_utc=observed
                )
                if parsed.report is not None:
                    write_json_atomic(parsed.report, report)
            else:
                authorization, authorization_hash = load_authorization(
                    parsed.authorization
                )
                report = run_certification(
                    client,
                    symbol=parsed.symbol,
                    authorization=authorization,
                    authorization_sha256=authorization_hash,
                    execute_paper=parsed.execute_paper,
                    observed_at_utc=observed,
                    system_path_runner=(
                        CppSystemPathRunner(parsed.path_binary)
                        if parsed.command == "certify-system-path"
                        else None
                    ),
                    system_path_seed=(
                        parsed.seed
                        if parsed.command == "certify-system-path"
                        else 20_260_911
                    ),
                )
                write_json_atomic(parsed.report, report)
            output = report
    except CertificationError as error:
        sys.stdout.write(
            json.dumps(
                {
                    "error": {"code": error.code.value, "message": str(error)},
                    "status": "BLOCKED",
                },
                indent=2,
                sort_keys=True,
            )
            + "\n"
        )
        return 1
    sys.stdout.write(json.dumps(output, indent=2, sort_keys=True) + "\n")
    return 0


def cli_main() -> int:
    """Installed ``aegis-alpaca-paper`` entry point."""
    return main()


if __name__ == "__main__":
    raise SystemExit(cli_main())
