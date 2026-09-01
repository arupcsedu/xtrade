"""Provider interfaces and license-clean news/filing source adapters."""

from __future__ import annotations

import json
from collections import deque
from dataclasses import dataclass
from typing import TYPE_CHECKING, Protocol, cast
from urllib.parse import urlparse

from aegis_mx_intelligence.news_types import (
    MAX_DOCUMENT_BYTES,
    AuthenticationEvidence,
    ProviderHealth,
    SourceAuthentication,
    SourceDocument,
    sha256_bytes,
)

if TYPE_CHECKING:
    from pathlib import Path

HTTP_OK = 200
SHA256_BYTES = 32
MAX_PROVIDER_ID_BYTES = 64


class ProviderError(RuntimeError):
    """Fail-closed provider boundary error."""


class DocumentProvider(Protocol):
    """Bounded polling contract implemented by every source provider."""

    @property
    def health(self) -> ProviderHealth:
        """Return provider health independently of pipeline readiness."""

    def poll(self, maximum_documents: int) -> tuple[SourceDocument, ...]:
        """Return at most the requested number of immutable documents."""

    def close(self) -> None:
        """Stop future provider work."""


class MockProvider:
    """Deterministic bounded provider used only for tests and simulation."""

    def __init__(
        self, documents: tuple[SourceDocument, ...], capacity: int = 1_024
    ) -> None:
        """Load a bounded deterministic document sequence."""
        if capacity <= 0 or len(documents) > capacity:
            msg = "mock provider capacity is invalid"
            raise ValueError(msg)
        self._documents = deque(documents)
        self._health = ProviderHealth.HEALTHY

    @property
    def health(self) -> ProviderHealth:
        """Return current mock-provider health."""
        return self._health

    def poll(self, maximum_documents: int) -> tuple[SourceDocument, ...]:
        """Pop a deterministic bounded batch without waiting."""
        if maximum_documents <= 0:
            msg = "maximum_documents must be positive"
            raise ValueError(msg)
        if self._health is ProviderHealth.STOPPED:
            return ()
        return tuple(
            self._documents.popleft()
            for _ in range(min(maximum_documents, len(self._documents)))
        )

    def close(self) -> None:
        """Stop the provider and discard no source documents."""
        self._health = ProviderHealth.STOPPED


class FilesystemReplayProvider:
    """Read deterministic synthetic/public replay envelopes from a directory."""

    _EXPECTED_KEYS = frozenset(
        {
            "provider_id",
            "document_id",
            "source_uri",
            "content_type",
            "content",
            "content_sha256",
            "provider_event_time_utc_ns",
            "received_wall_clock_utc_ns",
        }
    )

    def __init__(self, directory: Path) -> None:
        """Discover regular, nonsymlink JSON envelopes in lexical order."""
        if not directory.is_dir():
            msg = "filesystem replay path is not a directory"
            raise ValueError(msg)
        self._paths = deque(
            path
            for path in sorted(directory.glob("*.json"))
            if path.is_file() and not path.is_symlink()
        )
        self._health = ProviderHealth.HEALTHY

    @property
    def health(self) -> ProviderHealth:
        """Return current replay-provider health."""
        return self._health

    @classmethod
    def _decode(cls, path: Path) -> SourceDocument:
        if path.stat().st_size > MAX_DOCUMENT_BYTES:
            msg = "replay envelope exceeds the byte limit"
            raise ProviderError(msg)
        try:
            value = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
            msg = "malformed replay envelope"
            raise ProviderError(msg) from error
        if not isinstance(value, dict) or set(value) != cls._EXPECTED_KEYS:
            msg = "replay envelope keys do not match the schema"
            raise ProviderError(msg)
        payload = str(value["content"]).encode("utf-8")
        digest = sha256_bytes(payload)
        if digest.hex() != value["content_sha256"]:
            msg = "replay content SHA-256 does not match"
            raise ProviderError(msg)
        authentication = AuthenticationEvidence(
            SourceAuthentication.REPLAY_HASH_VERIFIED,
            "filesystem-replay-sha256",
            sha256_bytes(path.name.encode("utf-8") + digest),
        )
        try:
            return SourceDocument(
                provider_id=str(value["provider_id"]),
                document_id=str(value["document_id"]),
                source_uri=str(value["source_uri"]),
                content_type=str(value["content_type"]),
                payload=payload,
                provider_event_time_utc_ns=int(
                    cast("int | str", value["provider_event_time_utc_ns"])
                ),
                received_wall_clock_utc_ns=int(
                    cast("int | str", value["received_wall_clock_utc_ns"])
                ),
                authentication=authentication,
            )
        except (TypeError, ValueError) as error:
            msg = "replay document fields are invalid"
            raise ProviderError(msg) from error

    def poll(self, maximum_documents: int) -> tuple[SourceDocument, ...]:
        """Read a deterministic bounded batch and stop on malformed input."""
        if maximum_documents <= 0:
            msg = "maximum_documents must be positive"
            raise ValueError(msg)
        if self._health is ProviderHealth.STOPPED:
            return ()
        documents: list[SourceDocument] = []
        try:
            for _ in range(min(maximum_documents, len(self._paths))):
                documents.extend((self._decode(self._paths.popleft()),))
        except ProviderError:
            self._health = ProviderHealth.INVALID
            raise
        return tuple(documents)

    def close(self) -> None:
        """Stop future replay reads."""
        self._health = ProviderHealth.STOPPED


@dataclass(frozen=True, slots=True)
class PublicSourceResponse:
    """Response supplied by an injected, legally reviewed HTTPS client."""

    status_code: int
    final_uri: str
    content_type: str
    body: bytes
    document_id: str
    provider_event_time_utc_ns: int
    received_wall_clock_utc_ns: int
    tls_peer_verified: bool
    peer_certificate_sha256: bytes


class PublicSourceClient(Protocol):
    """Least-privilege client; it exposes no tools, credentials, or redirects."""

    def fetch(self, uri: str, maximum_bytes: int) -> PublicSourceResponse:
        """Fetch one bounded official-public response."""


class OfficialPublicSourceProvider:
    """Authenticated adapter around an injected approved public-source client."""

    def __init__(
        self,
        provider_id: str,
        endpoint: str,
        allowed_hosts: frozenset[str],
        client: PublicSourceClient,
    ) -> None:
        """Bind an approved endpoint and injected least-privilege client."""
        parsed = urlparse(endpoint)
        if (
            parsed.scheme != "https"
            or not parsed.hostname
            or parsed.hostname not in allowed_hosts
            or parsed.username is not None
            or parsed.password is not None
        ):
            msg = "official source endpoint must be credential-free allowed HTTPS"
            raise ValueError(msg)
        if not provider_id or len(provider_id.encode("utf-8")) > MAX_PROVIDER_ID_BYTES:
            msg = "official provider identifier is invalid"
            raise ValueError(msg)
        self._provider_id = provider_id
        self._endpoint = endpoint
        self._allowed_hosts = allowed_hosts
        self._client = client
        self._health = ProviderHealth.STARTING

    @property
    def health(self) -> ProviderHealth:
        """Return health of the official-public boundary."""
        return self._health

    def poll(self, maximum_documents: int) -> tuple[SourceDocument, ...]:
        """Fetch and authenticate one response; never follow an unapproved host."""
        if maximum_documents <= 0:
            msg = "maximum_documents must be positive"
            raise ValueError(msg)
        if self._health is ProviderHealth.STOPPED:
            return ()
        response = self._client.fetch(self._endpoint, MAX_DOCUMENT_BYTES)
        parsed = urlparse(response.final_uri)
        if (
            response.status_code != HTTP_OK
            or parsed.scheme != "https"
            or parsed.hostname not in self._allowed_hosts
            or parsed.username is not None
            or parsed.password is not None
            or not response.tls_peer_verified
            or len(response.peer_certificate_sha256) != SHA256_BYTES
            or not any(response.peer_certificate_sha256)
            or not response.body
            or len(response.body) > MAX_DOCUMENT_BYTES
        ):
            self._health = ProviderHealth.INVALID
            msg = "official-public response failed authentication"
            raise ProviderError(msg)
        evidence = AuthenticationEvidence(
            SourceAuthentication.PUBLIC_TLS_VERIFIED,
            "https-peer-certificate-sha256",
            sha256_bytes(
                response.peer_certificate_sha256
                + response.final_uri.encode("utf-8")
                + sha256_bytes(response.body)
            ),
        )
        try:
            document = SourceDocument(
                provider_id=self._provider_id,
                document_id=response.document_id,
                source_uri=response.final_uri,
                content_type=response.content_type,
                payload=response.body,
                provider_event_time_utc_ns=response.provider_event_time_utc_ns,
                received_wall_clock_utc_ns=response.received_wall_clock_utc_ns,
                authentication=evidence,
            )
        except ValueError as error:
            self._health = ProviderHealth.INVALID
            msg = "official-public document fields are invalid"
            raise ProviderError(msg) from error
        self._health = ProviderHealth.HEALTHY
        return (document,)

    def close(self) -> None:
        """Stop future network-client invocations."""
        self._health = ProviderHealth.STOPPED
