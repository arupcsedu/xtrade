"""Process-isolated, resource-bounded news-document sanitization."""

from __future__ import annotations

import json
import multiprocessing
import os
import resource
import socket
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, NoReturn, Protocol

from aegis_mx_intelligence.news_security import (
    UnsafeDocumentError,
    sanitize_document,
)
from aegis_mx_intelligence.news_types import (
    MAX_TEXT_CHARACTERS,
    SanitizedDocument,
    SourceDocument,
    sha256_bytes,
)

if TYPE_CHECKING:
    from multiprocessing.connection import Connection

MAX_SANDBOX_OUTPUT_BYTES = (MAX_TEXT_CHARACTERS * 2) + 4_096
MIN_WALL_TIMEOUT_SECONDS = 0.05
MAX_WALL_TIMEOUT_SECONDS = 30.0
MAX_CPU_SECONDS = 10
MIN_MEMORY_BYTES = 32 * 1024 * 1024
MAX_MEMORY_BYTES = 1024 * 1024 * 1024
MIN_OPEN_FILES = 8
MAX_OPEN_FILES = 64


@dataclass(frozen=True, slots=True)
class DocumentSandboxPolicy:
    """Hard resource and completion bounds for one untrusted document."""

    wall_timeout_seconds: float = 2.0
    cpu_seconds: int = 1
    memory_bytes: int = 256 * 1024 * 1024
    maximum_open_files: int = 16

    def __post_init__(self) -> None:
        """Reject ineffective or denial-of-service-prone limits."""
        if (
            not MIN_WALL_TIMEOUT_SECONDS
            <= self.wall_timeout_seconds
            <= MAX_WALL_TIMEOUT_SECONDS
            or not 1 <= self.cpu_seconds <= MAX_CPU_SECONDS
            or not MIN_MEMORY_BYTES <= self.memory_bytes <= MAX_MEMORY_BYTES
            or not MIN_OPEN_FILES <= self.maximum_open_files <= MAX_OPEN_FILES
        ):
            msg = "document sandbox policy is outside permitted bounds"
            raise ValueError(msg)


class DocumentSanitizer(Protocol):
    """Isolation boundary consumed by the intelligence pipeline."""

    def sanitize(self, source: SourceDocument) -> SanitizedDocument:
        """Return inert validated text or fail closed."""


class InlineDocumentSanitizer:
    """Explicit test/replay-only sanitizer with no operating-system isolation."""

    def sanitize(self, source: SourceDocument) -> SanitizedDocument:
        """Sanitize directly for deterministic unit tests and offline replay."""
        return sanitize_document(source)


def _deny_network(*_args: object, **_kwargs: object) -> NoReturn:
    msg = "network access is disabled in the document sandbox"
    raise PermissionError(msg)


def _sandbox_worker(
    sender: Connection,
    source: SourceDocument,
    working_directory: str,
    policy: DocumentSandboxPolicy,
) -> None:  # pragma: no cover - executed in an isolated spawned interpreter.
    try:
        os.chdir(working_directory)
        os.environ.clear()
        os.umask(0o077)
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        resource.setrlimit(
            resource.RLIMIT_CPU, (policy.cpu_seconds, policy.cpu_seconds)
        )
        resource.setrlimit(
            resource.RLIMIT_AS, (policy.memory_bytes, policy.memory_bytes)
        )
        resource.setrlimit(
            resource.RLIMIT_NOFILE,
            (policy.maximum_open_files, policy.maximum_open_files),
        )
        resource.setrlimit(
            resource.RLIMIT_FSIZE,
            (MAX_SANDBOX_OUTPUT_BYTES, MAX_SANDBOX_OUTPUT_BYTES),
        )
        setattr(socket, "socket", _deny_network)  # noqa: B010
        socket.create_connection = _deny_network
        document = sanitize_document(source)
        response = {
            "analysis_text": document.analysis_text,
            "language_code": document.language_code,
            "prompt_injection_detected": document.prompt_injection_detected,
            "retained_text": document.retained_text,
            "sanitized_sha256": document.sanitized_sha256.hex(),
        }
    except (OSError, RuntimeError, UnsafeDocumentError, ValueError):
        response = {"error": "document_rejected"}
    try:
        sender.send_bytes(
            json.dumps(response, sort_keys=True, separators=(",", ":")).encode("utf-8")
        )
    finally:
        sender.close()


class ProcessDocumentSanitizer:
    """Spawn a clean resource-limited process for every untrusted document."""

    def __init__(self, policy: DocumentSandboxPolicy | None = None) -> None:
        """Use spawn semantics so ambient descriptors are not inherited."""
        self._policy = policy or DocumentSandboxPolicy()
        self._context = multiprocessing.get_context("spawn")

    def sanitize(self, source: SourceDocument) -> SanitizedDocument:
        """Sanitize in isolation and strictly validate the bounded child response."""
        with tempfile.TemporaryDirectory(prefix="aegis-news-sandbox-") as directory:
            Path(directory).chmod(0o700)
            receiver, sender = self._context.Pipe(duplex=False)
            process = self._context.Process(
                target=_sandbox_worker,
                args=(sender, source, directory, self._policy),
                daemon=True,
            )
            process.start()
            sender.close()
            if not receiver.poll(self._policy.wall_timeout_seconds):
                process.terminate()
                process.join()
                receiver.close()
                msg = "document sandbox exceeded its wall deadline"
                raise UnsafeDocumentError(msg)
            try:
                payload = receiver.recv_bytes(MAX_SANDBOX_OUTPUT_BYTES)
            except (EOFError, OSError) as error:
                msg = "document sandbox returned no bounded result"
                raise UnsafeDocumentError(msg) from error
            finally:
                receiver.close()
            process.join(self._policy.wall_timeout_seconds)
            if process.is_alive():
                process.terminate()
                process.join()
                msg = "document sandbox did not exit"
                raise UnsafeDocumentError(msg)
            if process.exitcode != 0:
                msg = "document sandbox failed"
                raise UnsafeDocumentError(msg)
        try:
            value = json.loads(payload)
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            msg = "document sandbox output is malformed"
            raise UnsafeDocumentError(msg) from error
        expected = {
            "analysis_text",
            "language_code",
            "prompt_injection_detected",
            "retained_text",
            "sanitized_sha256",
        }
        if not isinstance(value, dict) or set(value) != expected:
            msg = "document sandbox rejected the document"
            raise UnsafeDocumentError(msg)
        try:
            digest = bytes.fromhex(value["sanitized_sha256"])
            document = SanitizedDocument(
                source=source,
                retained_text=value["retained_text"],
                analysis_text=value["analysis_text"],
                sanitized_sha256=digest,
                prompt_injection_detected=value["prompt_injection_detected"],
                language_code=value["language_code"],
            )
        except (TypeError, ValueError) as error:
            msg = "document sandbox output violates the schema"
            raise UnsafeDocumentError(msg) from error
        if document.sanitized_sha256 != sha256_bytes(document.retained_text):
            msg = "document sandbox output hash does not match"
            raise UnsafeDocumentError(msg)
        return document
