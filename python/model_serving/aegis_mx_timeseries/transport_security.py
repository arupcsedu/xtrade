"""Fail-closed service identity, secret loading, mTLS, and request admission."""

from __future__ import annotations

import hashlib
import re
import ssl
import stat
import threading
import time
from dataclasses import dataclass
from enum import StrEnum
from typing import TYPE_CHECKING, Final, Protocol

if TYPE_CHECKING:
    from collections.abc import Callable, Mapping, Sequence
    from pathlib import Path

NANOSECONDS_PER_SECOND: Final = 1_000_000_000
MAX_SECRET_BYTES: Final = 1 << 20
MAX_IDENTITY_BINDINGS: Final = 256
MAX_RATE_PER_SECOND: Final = 100_000
MAX_BURST: Final = 100_000
SUBJECT_ALTERNATIVE_NAME_PARTS: Final = 2
_SECRET_NAME: Final = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]{0,127}$")
_SPIFFE_ID: Final = re.compile(
    r"^spiffe://aegis-mx/[a-z0-9][a-z0-9-]{0,31}/"
    r"[a-z0-9][a-z0-9-]{0,63}$"
)


class TransportSecurityError(RuntimeError):
    """Raised when identity, secret, TLS, or admission validation fails."""


class ServicePermission(StrEnum):
    """Narrow HTTP capabilities granted to authenticated service identities."""

    READ_STATUS = "read_status"
    READ_METRICS = "read_metrics"
    FORECAST = "forecast"
    BUILD_CONTEXT = "build_context"


@dataclass(frozen=True, slots=True)
class ServiceIdentity:
    """One canonical SPIFFE-style identity carried in a certificate URI SAN."""

    uri: str

    def __post_init__(self) -> None:
        """Reject identities outside the Aegis-MX trust domain."""
        if _SPIFFE_ID.fullmatch(self.uri) is None:
            msg = "service identity is outside the Aegis-MX trust domain"
            raise ValueError(msg)


@dataclass(frozen=True, slots=True)
class IdentityBinding:
    """Bind one service identity to an immutable least-privilege permission set."""

    identity: ServiceIdentity
    permissions: frozenset[ServicePermission]

    def __post_init__(self) -> None:
        """Require at least one explicit permission."""
        if not self.permissions:
            msg = "identity binding must grant at least one permission"
            raise ValueError(msg)


def permission_for_request(method: str, path: str) -> ServicePermission:
    """Map an HTTP operation to one stable authorization capability."""
    if method == "GET" and path == "/metrics":
        return ServicePermission.READ_METRICS
    if method == "GET" and path in {
        "/healthz",
        "/readyz",
        "/version",
        "/configuration",
    }:
        return ServicePermission.READ_STATUS
    if method == "POST" and path == "/v1/forecast":
        return ServicePermission.FORECAST
    if method == "POST" and path == "/v1/context":
        return ServicePermission.BUILD_CONTEXT
    msg = "request has no authorized service capability"
    raise TransportSecurityError(msg)


def peer_service_identity(certificate: Mapping[str, object]) -> ServiceIdentity:
    """Extract exactly one valid Aegis-MX URI SAN from a verified peer certificate."""
    raw_names = certificate.get("subjectAltName")
    if not isinstance(raw_names, tuple):
        msg = "peer certificate has no subject alternative names"
        raise TransportSecurityError(msg)
    identities: list[ServiceIdentity] = []
    for raw_name in raw_names:
        if (
            isinstance(raw_name, tuple)
            and len(raw_name) == SUBJECT_ALTERNATIVE_NAME_PARTS
            and raw_name[0] == "URI"
            and isinstance(raw_name[1], str)
            and raw_name[1].startswith("spiffe://")
        ):
            try:
                identities.append(ServiceIdentity(raw_name[1]))
            except ValueError as error:
                raise TransportSecurityError(str(error)) from error
    if len(identities) != 1:
        msg = "peer certificate must contain exactly one Aegis-MX service identity"
        raise TransportSecurityError(msg)
    return identities[0]


def require_peer_service_identity(
    certificate: Mapping[str, object], expected: ServiceIdentity
) -> ServiceIdentity:
    """Require a verified peer certificate to carry one exact expected identity."""
    actual = peer_service_identity(certificate)
    if actual != expected:
        msg = "peer service identity does not match the expected endpoint"
        raise TransportSecurityError(msg)
    return actual


class IdentityPolicy:
    """Authorize verified certificate identities against bounded static RBAC."""

    def __init__(self, bindings: tuple[IdentityBinding, ...]) -> None:
        """Create a deterministic, duplicate-free identity map."""
        if not bindings or len(bindings) > MAX_IDENTITY_BINDINGS:
            msg = "identity binding count is invalid"
            raise ValueError(msg)
        self._bindings = {item.identity.uri: item.permissions for item in bindings}
        if len(self._bindings) != len(bindings):
            msg = "duplicate service identity binding"
            raise ValueError(msg)

    def authorize(
        self, certificate: Mapping[str, object], method: str, path: str
    ) -> ServiceIdentity:
        """Return the authenticated identity only when its capability permits access."""
        identity = peer_service_identity(certificate)
        required = permission_for_request(method, path)
        if required not in self._bindings.get(identity.uri, frozenset()):
            msg = "service identity is not authorized for this operation"
            raise TransportSecurityError(msg)
        return identity


def parse_identity_bindings(values: Sequence[str]) -> tuple[IdentityBinding, ...]:
    """Parse `identity=permission,permission` bindings from immutable config."""
    bindings: list[IdentityBinding] = []
    for value in values:
        identity_text, separator, raw_permissions = value.partition("=")
        if not separator:
            msg = "client identity binding is malformed"
            raise ValueError(msg)
        try:
            permissions = frozenset(
                ServicePermission(item) for item in raw_permissions.split(",") if item
            )
        except ValueError as error:
            msg = "client identity binding contains an invalid permission"
            raise ValueError(msg) from error
        bindings.append(IdentityBinding(ServiceIdentity(identity_text), permissions))
    return tuple(bindings)


class MountedSecretProvider:
    """Read bounded secrets from a CSI/secret-manager mounted directory."""

    def __init__(self, root: Path, maximum_bytes: int = MAX_SECRET_BYTES) -> None:
        """Bind one absolute secret root; secret values never enter configuration."""
        if (
            not root.is_absolute()
            or maximum_bytes <= 0
            or maximum_bytes > MAX_SECRET_BYTES
        ):
            msg = "secret root or byte limit is invalid"
            raise ValueError(msg)
        try:
            self._root = root.resolve(strict=True)
        except OSError as error:
            msg = "secret root is unavailable"
            raise TransportSecurityError(msg) from error
        if not self._root.is_dir():
            msg = "secret root is not a directory"
            raise TransportSecurityError(msg)
        self._maximum_bytes = maximum_bytes

    def path(self, name: str, *, private: bool = False) -> Path:
        """Resolve one regular mounted file without permitting root escape."""
        if _SECRET_NAME.fullmatch(name) is None:
            msg = "secret name is invalid"
            raise TransportSecurityError(msg)
        try:
            resolved = (self._root / name).resolve(strict=True)
            info = resolved.stat()
        except OSError as error:
            msg = "secret is unavailable"
            raise TransportSecurityError(msg) from error
        if not resolved.is_relative_to(self._root) or not stat.S_ISREG(info.st_mode):
            msg = "secret path escapes its mount or is not a regular file"
            raise TransportSecurityError(msg)
        if info.st_size <= 0 or info.st_size > self._maximum_bytes:
            msg = "secret is empty or exceeds its byte limit"
            raise TransportSecurityError(msg)
        if stat.S_IMODE(info.st_mode) & 0o022:
            msg = "secret is writable outside its owner"
            raise TransportSecurityError(msg)
        if private and stat.S_IMODE(info.st_mode) & 0o007:
            msg = "private secret is world accessible"
            raise TransportSecurityError(msg)
        return resolved

    def read(self, name: str, *, private: bool = False) -> bytes:
        """Read one previously validated secret without logging its value."""
        path = self.path(name, private=private)
        try:
            payload = path.read_bytes()
        except OSError as error:
            msg = "secret could not be read"
            raise TransportSecurityError(msg) from error
        if not payload or len(payload) > self._maximum_bytes:
            msg = "secret changed outside its byte bound"
            raise TransportSecurityError(msg)
        return payload


@dataclass(frozen=True, slots=True)
class TLSMaterial:
    """Names and identity for one rotatable mounted certificate bundle."""

    certificate_name: str
    private_key_name: str
    trust_bundle_name: str
    service_identity: ServiceIdentity
    reload_interval_ns: int = NANOSECONDS_PER_SECOND

    def __post_init__(self) -> None:
        """Validate secret names and bound the reload cadence."""
        names = (
            self.certificate_name,
            self.private_key_name,
            self.trust_bundle_name,
        )
        if any(_SECRET_NAME.fullmatch(name) is None for name in names):
            msg = "TLS material contains an invalid secret name"
            raise ValueError(msg)
        if not 0 < self.reload_interval_ns <= 60 * NANOSECONDS_PER_SECOND:
            msg = "TLS reload interval is invalid"
            raise ValueError(msg)


class TLSContextFactory(Protocol):
    """Injectable context builder used to test rotation without test keys."""

    def __call__(
        self, certificate: Path, private_key: Path, trust_bundle: bytes
    ) -> ssl.SSLContext:
        """Build a fresh context from one coherent material snapshot."""


def build_server_context(
    certificate: Path, private_key: Path, trust_bundle: bytes
) -> ssl.SSLContext:
    """Build a TLS 1.3 mutual-authentication server context."""
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_3
    context.maximum_version = ssl.TLSVersion.TLSv1_3
    context.verify_mode = ssl.CERT_REQUIRED
    context.load_verify_locations(cadata=trust_bundle.decode("ascii", "strict"))
    context.load_cert_chain(certificate, private_key)
    return context


def build_client_context(
    certificate: Path, private_key: Path, trust_bundle: bytes
) -> ssl.SSLContext:
    """Build mTLS client chain validation; require the peer URI after handshake."""
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    context.minimum_version = ssl.TLSVersion.TLSv1_3
    context.maximum_version = ssl.TLSVersion.TLSv1_3
    context.verify_mode = ssl.CERT_REQUIRED
    context.check_hostname = False
    context.load_verify_locations(cadata=trust_bundle.decode("ascii", "strict"))
    context.load_cert_chain(certificate, private_key)
    return context


class RotatingTLSContext:
    """Atomically replace an mTLS context when mounted material changes."""

    def __init__(
        self,
        provider: MountedSecretProvider,
        material: TLSMaterial,
        *,
        factory: TLSContextFactory = build_server_context,
        clock: Callable[[], int] = time.monotonic_ns,
    ) -> None:
        """Create a fail-closed rotator with an injected monotonic clock."""
        self._provider = provider
        self._material = material
        self._factory = factory
        self._clock = clock
        self._lock = threading.Lock()
        self._context: ssl.SSLContext | None = None
        self._digest = b""
        self._next_check_ns = 0
        self._generation = 0

    @property
    def generation(self) -> int:
        """Return the number of successfully loaded certificate generations."""
        with self._lock:
            return self._generation

    def current(self) -> ssl.SSLContext:
        """Return current material, reloading changed files before new handshakes."""
        now_ns = self._clock()
        with self._lock:
            if self._context is not None and now_ns < self._next_check_ns:
                return self._context
            certificate = self._provider.path(self._material.certificate_name)
            private_key = self._provider.path(
                self._material.private_key_name, private=True
            )
            trust_bundle = self._provider.read(self._material.trust_bundle_name)
            digest = hashlib.sha256(
                self._provider.read(self._material.certificate_name)
                + self._provider.read(self._material.private_key_name, private=True)
                + trust_bundle
            ).digest()
            if self._context is None or digest != self._digest:
                self._context = self._factory(certificate, private_key, trust_bundle)
                self._digest = digest
                self._generation += 1
            self._next_check_ns = now_ns + self._material.reload_interval_ns
            return self._context


@dataclass(slots=True)
class _Bucket:
    token_nanos: int
    updated_ns: int


class IdentityRateLimiter:
    """Bounded integer token buckets keyed only by authenticated identity."""

    def __init__(
        self,
        rate_per_second: int,
        burst: int,
        *,
        maximum_identities: int = MAX_IDENTITY_BINDINGS,
        clock: Callable[[], int] = time.monotonic_ns,
    ) -> None:
        """Create deterministic buckets with no floating-point accounting."""
        if (
            not 0 < rate_per_second <= MAX_RATE_PER_SECOND
            or not 0 < burst <= MAX_BURST
            or not 0 < maximum_identities <= MAX_IDENTITY_BINDINGS
        ):
            msg = "rate limiter configuration is invalid"
            raise ValueError(msg)
        self._rate = rate_per_second
        self._capacity = burst * NANOSECONDS_PER_SECOND
        self._maximum_identities = maximum_identities
        self._clock = clock
        self._buckets: dict[str, _Bucket] = {}
        self._lock = threading.Lock()

    def allow(self, identity: ServiceIdentity) -> bool:
        """Consume one token or reject without sleeping or queueing."""
        now_ns = self._clock()
        with self._lock:
            bucket = self._buckets.get(identity.uri)
            if bucket is None:
                if len(self._buckets) >= self._maximum_identities:
                    return False
                bucket = _Bucket(self._capacity, now_ns)
                self._buckets[identity.uri] = bucket
            if now_ns < bucket.updated_ns:
                return False
            elapsed = now_ns - bucket.updated_ns
            bucket.token_nanos = min(
                self._capacity, bucket.token_nanos + (elapsed * self._rate)
            )
            bucket.updated_ns = now_ns
            if bucket.token_nanos < NANOSECONDS_PER_SECOND:
                return False
            bucket.token_nanos -= NANOSECONDS_PER_SECOND
            return True
