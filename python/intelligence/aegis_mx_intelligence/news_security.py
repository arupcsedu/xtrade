"""Fail-closed active-content and prompt-injection isolation."""

from __future__ import annotations

import base64
import binascii
import html
import re
from html.parser import HTMLParser
from typing import Final

from aegis_mx_intelligence.news_types import (
    MAX_TEXT_CHARACTERS,
    DocumentType,
    SanitizedDocument,
    SourceDocument,
    sha256_bytes,
)

_BLOCKED_TAGS: Final = frozenset(
    {
        "script",
        "style",
        "iframe",
        "object",
        "embed",
        "template",
        "svg",
        "math",
        "form",
        "button",
        "input",
        "textarea",
        "select",
        "option",
        "link",
        "meta",
    }
)
_BREAK_TAGS: Final = frozenset(
    {"article", "br", "div", "h1", "h2", "h3", "li", "p", "section", "td", "tr"}
)
_INJECTION_PATTERNS: Final = tuple(
    re.compile(pattern, re.IGNORECASE)
    for pattern in (
        r"\bignore\s+(all\s+)?(previous\s+|prior\s+)?(system\s+)?instructions?\b",
        r"\b(system|developer|assistant)\s*prompt\b",
        r"\b(reveal|print|return|exfiltrate)\b.{0,48}\b(secret|credential|token|prompt)\b",
        r"\b(call|invoke|use)\b.{0,32}\b(tool|function|shell|terminal)\b",
        r"\bchange\b.{0,32}\b(configuration|policy|instructions?)\b",
        r"\bdo\s+not\s+follow\b.{0,32}\b(policy|instructions?)\b",
        r"\b(begin|end)\s+(system|developer|assistant)\s+message\b",
        r"\b(javascript|data\s*:\s*text/html)\s*:",
    )
)
_BASE64_TOKEN: Final = re.compile(r"(?<![A-Za-z0-9+/=])[A-Za-z0-9+/]{20,}={0,2}")
_WHITESPACE: Final = re.compile(r"\s+")
_SENTENCE: Final = re.compile(r"(?<=[.!?])\s+|[\r\n]+")
MIN_LANGUAGE_MARKERS: Final = 2


class UnsafeDocumentError(ValueError):
    """Raised when untrusted text cannot be decoded or safely bounded."""


class _PlainTextExtractor(HTMLParser):
    """Minimal allow-nothing HTML parser that returns inert visible text."""

    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self._blocked_depth = 0
        self._parts: list[str] = []

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        """Drop active elements and all attributes."""
        del attrs
        normalized = tag.casefold()
        if normalized in _BLOCKED_TAGS:
            self._blocked_depth += 1
        elif self._blocked_depth == 0 and normalized in _BREAK_TAGS:
            self._parts.append("\n")

    def handle_startendtag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        """Handle void tags without leaving a blocked-depth marker."""
        del attrs
        if self._blocked_depth == 0 and tag.casefold() in _BREAK_TAGS:
            self._parts.append("\n")

    def handle_endtag(self, tag: str) -> None:
        """Close active-content suppression or append a text boundary."""
        normalized = tag.casefold()
        if normalized in _BLOCKED_TAGS and self._blocked_depth > 0:
            self._blocked_depth -= 1
        elif self._blocked_depth == 0 and normalized in _BREAK_TAGS:
            self._parts.append("\n")

    def handle_data(self, data: str) -> None:
        """Retain visible data only outside blocked elements."""
        if self._blocked_depth == 0:
            self._parts.append(data)

    def text(self) -> str:
        """Return the accumulated visible text."""
        return "".join(self._parts)


def _contains_instruction(value: str) -> bool:
    return any(pattern.search(value) is not None for pattern in _INJECTION_PATTERNS)


def _decoded_instruction(value: str) -> bool:
    padded = value + ("=" * ((4 - (len(value) % 4)) % 4))
    try:
        decoded = base64.b64decode(padded, validate=True).decode("utf-8", "strict")
    except (binascii.Error, UnicodeDecodeError, ValueError):
        return False
    return _contains_instruction(decoded)


def _is_suspicious_segment(segment: str) -> bool:
    if _contains_instruction(segment):
        return True
    return any(
        _decoded_instruction(match.group(0))
        for match in _BASE64_TOKEN.finditer(segment)
    )


def _normalize_visible_text(value: str) -> str:
    lines = []
    for line in value.replace("\x00", " ").splitlines():
        normalized = _WHITESPACE.sub(" ", line).strip()
        if normalized:
            lines.append(normalized)
    return "\n".join(lines)


def detect_language(value: str) -> str:
    """Return a deterministic coarse language code or `und`."""
    words = re.findall(r"[A-Za-zÀ-ÿ]+", value.casefold())[:2_000]
    if not words:
        return "und"
    markers = {
        "en": {"the", "and", "company", "said", "will", "with"},
        "es": {"el", "la", "empresa", "dijo", "con", "para"},
        "fr": {"le", "la", "entreprise", "avec", "pour", "est"},
    }
    scores = {
        code: sum(word in terms for word in words) for code, terms in markers.items()
    }
    best = max(scores, key=scores.__getitem__)
    return best if scores[best] >= MIN_LANGUAGE_MARKERS else "und"


def sanitize_document(source: SourceDocument) -> SanitizedDocument:
    """Decode strictly, strip active content, and isolate instruction-like text."""
    try:
        decoded = source.payload.decode("utf-8", "strict")
    except UnicodeDecodeError as error:
        msg = "document is not valid UTF-8"
        raise UnsafeDocumentError(msg) from error
    if len(decoded) > MAX_TEXT_CHARACTERS:
        msg = "decoded document exceeds the character limit"
        raise UnsafeDocumentError(msg)

    if "html" in source.content_type.casefold() or "<" in decoded:
        parser = _PlainTextExtractor()
        try:
            parser.feed(decoded)
            parser.close()
        except (AssertionError, ValueError) as error:
            msg = "malformed active-content document"
            raise UnsafeDocumentError(msg) from error
        visible = parser.text()
    else:
        visible = html.unescape(decoded)
    retained = _normalize_visible_text(visible)
    if not retained:
        msg = "document has no inert visible text"
        raise UnsafeDocumentError(msg)

    segments = [item.strip() for item in _SENTENCE.split(retained) if item.strip()]
    suspicious = tuple(item for item in segments if _is_suspicious_segment(item))
    analysis_segments = [item for item in segments if item not in suspicious]
    analysis = "\n".join(analysis_segments) or "[UNTRUSTED_INSTRUCTION_REMOVED]"
    return SanitizedDocument(
        source=source,
        retained_text=retained,
        analysis_text=analysis,
        sanitized_sha256=sha256_bytes(retained),
        prompt_injection_detected=bool(suspicious),
        language_code=detect_language(analysis),
    )


def detect_document_type(document: SanitizedDocument) -> DocumentType:
    """Detect a bounded provider-neutral document type."""
    content_type = document.source.content_type.casefold()
    text = document.analysis_text.casefold()
    if "correction" in text or "corrected" in text:
        return DocumentType.CORRECTION
    if "10-k" in text or "10-q" in text or "8-k" in text or "filing" in content_type:
        return DocumentType.FILING
    if "regulatory notice" in text or "enforcement notice" in text:
        return DocumentType.REGULATORY_NOTICE
    if "press release" in text or "newswire" in content_type:
        return DocumentType.PRESS_RELEASE
    if "economic release" in text or "macro release" in text:
        return DocumentType.MACRO_RELEASE
    return DocumentType.NEWS_ARTICLE
