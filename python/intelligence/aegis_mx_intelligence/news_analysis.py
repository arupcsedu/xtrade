"""Deterministic entity resolution, event classification, and scoring."""

from __future__ import annotations

import re
from collections import OrderedDict
from dataclasses import dataclass
from typing import Final

from aegis_mx_intelligence.news_types import (
    MAX_ENTITIES,
    MAX_FACTS,
    AuthenticationEvidence,
    Classification,
    EntityDefinition,
    EventType,
    EvidenceExcerpt,
    ResolvedEntity,
    SanitizedDocument,
    SourceAuthentication,
    StructuredFact,
)

MAX_ENTITY_DEFINITIONS: Final = 10_000

_EVENT_PATTERNS: Final = (
    (EventType.CORRECTION, (r"\bcorrection\b", r"\bcorrected\b")),
    (EventType.RUMOR, (r"\brumou?r\b", r"\bunconfirmed\b")),
    (
        EventType.EARNINGS_RELEASE,
        (r"\bearnings\b", r"\bquarterly results\b", r"\bnet income\b"),
    ),
    (EventType.GUIDANCE_UPDATE, (r"\bguidance\b", r"\boutlook\b")),
    (
        EventType.MERGER_ACQUISITION,
        (r"\bacqui(?:re[ds]?|sition)\b", r"\bmerger\b", r"\btakeover\b"),
    ),
    (EventType.PRODUCT_RECALL, (r"\bproduct recall\b", r"\brecall(?:ed|s)?\b")),
    (
        EventType.REGULATORY_ACTION,
        (r"\benforcement action\b", r"\bregulator\b", r"\bcivil penalty\b"),
    ),
    (
        EventType.EXECUTIVE_CHANGE,
        (r"\bchief executive\b", r"\bceo\b.{0,48}\b(resign|appoint|depart)"),
    ),
    (
        EventType.FINANCING,
        (r"\bfinancing\b", r"\bdebt offering\b", r"\bequity offering\b"),
    ),
    (EventType.BANKRUPTCY, (r"\bbankruptcy\b", r"\bchapter 11\b", r"\binsolvency\b")),
    (EventType.LITIGATION, (r"\blawsuit\b", r"\blitigation\b", r"\bsued\b")),
    (
        EventType.CYBERSECURITY_INCIDENT,
        (r"\bcyber(?:security)? incident\b", r"\bdata breach\b", r"\bransomware\b"),
    ),
    (
        EventType.ANALYST_ACTION,
        (r"\b(upgrade|downgrade)[ds]?\b", r"\bprice target\b"),
    ),
    (
        EventType.SUPPLY_CHAIN_DISRUPTION,
        (r"\bsupply.chain disruption\b", r"\bshipment delay\b", r"\bshortage\b"),
    ),
    (EventType.TRADING_HALT, (r"\btrading halt(?:ed)?\b", r"\bshares halted\b")),
    (
        EventType.MACRO_RELEASE,
        (r"\bmacro release\b", r"\beconomic release\b", r"\bconsumer price index\b"),
    ),
)
_MATERIALITY: Final = {
    EventType.EARNINGS_RELEASE: 750_000,
    EventType.GUIDANCE_UPDATE: 800_000,
    EventType.MERGER_ACQUISITION: 950_000,
    EventType.PRODUCT_RECALL: 800_000,
    EventType.REGULATORY_ACTION: 850_000,
    EventType.EXECUTIVE_CHANGE: 650_000,
    EventType.FINANCING: 650_000,
    EventType.BANKRUPTCY: 1_000_000,
    EventType.LITIGATION: 650_000,
    EventType.CYBERSECURITY_INCIDENT: 850_000,
    EventType.ANALYST_ACTION: 450_000,
    EventType.SUPPLY_CHAIN_DISRUPTION: 700_000,
    EventType.TRADING_HALT: 1_000_000,
    EventType.MACRO_RELEASE: 800_000,
    EventType.RUMOR: 250_000,
    EventType.CORRECTION: 700_000,
}
_FACT_PATTERNS: Final = (
    (
        "currency_amount",
        re.compile(
            r"\$\s*([0-9]+(?:\.[0-9]+)?)\s*(million|billion|trillion)?",
            re.IGNORECASE,
        ),
        "USD",
    ),
    (
        "percentage",
        re.compile(r"([+-]?[0-9]+(?:\.[0-9]+)?)\s*(percent|%)", re.IGNORECASE),
        "PERCENT",
    ),
    (
        "filing_form",
        re.compile(r"\b(10-k|10-q|8-k|chapter 11)\b", re.IGNORECASE),
        "CODE",
    ),
)
_TOKEN: Final = re.compile(r"[a-z0-9]+")


def _word_pattern(value: str) -> re.Pattern[str]:
    return re.compile(
        rf"(?<![A-Za-z0-9]){re.escape(value)}(?![A-Za-z0-9])",
        re.IGNORECASE,
    )


class EntityRegistry:
    """Registry resolver that ignores unknown ticker-looking strings."""

    def __init__(self, definitions: tuple[EntityDefinition, ...]) -> None:
        """Index a bounded immutable collection of entity definitions."""
        if not definitions or len(definitions) > MAX_ENTITY_DEFINITIONS:
            msg = "entity registry is empty or exceeds its bound"
            raise ValueError(msg)
        by_name = {item.canonical_name.casefold(): item for item in definitions}
        if len(by_name) != len(definitions):
            msg = "entity canonical names must be unique"
            raise ValueError(msg)
        self._definitions = definitions
        self._by_name = by_name

    def resolve(self, text: str) -> tuple[ResolvedEntity, ...]:
        """Resolve only registered aliases/tickers and expand one relationship hop."""
        primary: list[EntityDefinition] = []
        seen: set[str] = set()
        for definition in self._definitions:
            alias_match = any(
                _word_pattern(alias).search(text) for alias in definition.aliases
            )
            ticker_match = any(
                _word_pattern(ticker).search(text)
                or re.search(rf"\${re.escape(ticker)}\b", text, re.IGNORECASE)
                for ticker in definition.tickers
            )
            if (alias_match or ticker_match) and definition.canonical_name not in seen:
                primary.append(definition)
                seen.add(definition.canonical_name)

        resolved: list[ResolvedEntity] = [
            ResolvedEntity(
                item.entity_sha256,
                item.canonical_name,
                "PRIMARY",
                item.instrument_id,
                950_000,
            )
            for item in primary
        ]
        for item in primary:
            for relationship, target_name in item.relationships:
                target = self._by_name.get(target_name.casefold())
                if target is None or target.canonical_name in seen:
                    continue
                resolved.append(
                    ResolvedEntity(
                        target.entity_sha256,
                        target.canonical_name,
                        relationship.upper(),
                        target.instrument_id,
                        650_000,
                    )
                )
                seen.add(target.canonical_name)
                if len(resolved) >= MAX_ENTITIES:
                    return tuple(resolved)
        return tuple(resolved)


class NoveltyIndex:
    """Bounded deterministic token-set history used for novelty scoring."""

    def __init__(self, capacity: int = 4_096) -> None:
        """Create a fixed-capacity novelty history."""
        if capacity <= 0:
            msg = "novelty index capacity must be positive"
            raise ValueError(msg)
        self._capacity = capacity
        self._tokens: OrderedDict[bytes, frozenset[str]] = OrderedDict()

    def score(self, text: str) -> int:
        """Return one-minus maximum Jaccard similarity without mutating state."""
        tokens = frozenset(_TOKEN.findall(text.casefold()))
        maximum_similarity_ppm = 0
        for previous in self._tokens.values():
            union = len(tokens | previous)
            similarity = (
                0 if union == 0 else (len(tokens & previous) * 1_000_000) // union
            )
            maximum_similarity_ppm = max(maximum_similarity_ppm, similarity)
        return 1_000_000 - maximum_similarity_ppm

    def record(self, digest: bytes, text: str) -> None:
        """Retain one bounded token set after publication succeeds."""
        tokens = frozenset(_TOKEN.findall(text.casefold()))
        self._tokens.pop(digest, None)
        while len(self._tokens) >= self._capacity:
            self._tokens.popitem(last=False)
        self._tokens[digest] = tokens


def source_trust_score(authentication: AuthenticationEvidence) -> int:
    """Map authenticated source evidence to a conservative fixed-point score."""
    return {
        SourceAuthentication.INVALID: 0,
        SourceAuthentication.MOCK_VERIFIED: 500_000,
        SourceAuthentication.REPLAY_HASH_VERIFIED: 650_000,
        SourceAuthentication.PUBLIC_TLS_VERIFIED: 850_000,
        SourceAuthentication.SIGNATURE_VERIFIED: 1_000_000,
    }[authentication.status]


def detect_event_type(text: str) -> tuple[EventType, re.Match[str]] | None:
    """Return the first deterministic classification and its evidence match."""
    for event_type, patterns in _EVENT_PATTERNS:
        for pattern in patterns:
            match = re.search(pattern, text, re.IGNORECASE | re.DOTALL)
            if match is not None:
                return event_type, match
    return None


def _excerpt(text: str, match: re.Match[str], excerpt_id: int) -> EvidenceExcerpt:
    sentence_start = max(text.rfind("\n", 0, match.start()) + 1, match.start() - 160)
    newline = text.find("\n", match.end())
    sentence_end = min(
        len(text) if newline < 0 else newline,
        match.end() + 320,
    )
    exact = text[sentence_start:sentence_end].strip()
    start = text.find(exact, sentence_start, sentence_end + 1)
    return EvidenceExcerpt(excerpt_id, start, start + len(exact), exact)


def extract_facts(
    retained_text: str, event_type: EventType, event_match_text: str
) -> tuple[tuple[StructuredFact, ...], tuple[EvidenceExcerpt, ...]]:
    """Extract bounded structured facts with exact retained-text evidence."""
    facts: list[StructuredFact] = []
    evidence: list[EvidenceExcerpt] = []
    event_match = re.search(re.escape(event_match_text), retained_text, re.IGNORECASE)
    if event_match is not None:
        excerpt = _excerpt(retained_text, event_match, 1)
        evidence.append(excerpt)
        facts.append(
            StructuredFact(
                "event_assertion",
                event_type.name,
                "CLASSIFICATION",
                250_000,
                (excerpt.excerpt_id,),
            )
        )
    for name, pattern, unit in _FACT_PATTERNS:
        for match in pattern.finditer(retained_text):
            excerpt_id = len(evidence) + 1
            excerpt = _excerpt(retained_text, match, excerpt_id)
            evidence.append(excerpt)
            facts.append(
                StructuredFact(
                    name,
                    " ".join(group for group in match.groups() if group).upper(),
                    unit,
                    150_000,
                    (excerpt_id,),
                )
            )
            if len(facts) >= MAX_FACTS:
                return tuple(facts), tuple(evidence)
    return tuple(facts), tuple(evidence)


@dataclass(frozen=True, slots=True)
class RuleEngine:
    """Low-latency deterministic classifier with no model or network calls."""

    minimum_relevance_ppm: int = 300_000

    def classify(
        self,
        document: SanitizedDocument,
        novelty_ppm: int,
    ) -> Classification | None:
        """Classify one sanitized analysis view or explicitly return no alert."""
        detected = detect_event_type(document.analysis_text)
        if detected is None or document.language_code not in {"en", "und"}:
            return None
        event_type, match = detected
        facts, evidence = extract_facts(
            document.retained_text,
            event_type,
            match.group(0),
        )
        relevance = max(self.minimum_relevance_ppm, 850_000 if facts else 500_000)
        uncertainty = 700_000 if document.prompt_injection_detected else 300_000
        return Classification(
            event_type=event_type,
            relevance_ppm=relevance,
            novelty_ppm=novelty_ppm,
            materiality_ppm=_MATERIALITY[event_type],
            source_trust_ppm=source_trust_score(document.source.authentication),
            uncertainty_ppm=uncertainty,
            facts=facts,
            evidence=evidence,
        )
