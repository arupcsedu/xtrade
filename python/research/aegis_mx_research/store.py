"""Append-only point-in-time store and deterministic temporal queries."""

from __future__ import annotations

from typing import TYPE_CHECKING

from aegis_mx_research.types import (
    CorporateAction,
    FilingRevision,
    IndexMembership,
    NewsRevision,
    PointInTimeRecord,
    RecordKind,
    SymbolMapping,
)

if TYPE_CHECKING:
    from collections.abc import Iterable


class PointInTimeStore:
    """Bounded in-memory reference store for offline research and replay."""

    def __init__(self, capacity: int = 100_000) -> None:
        """Create an empty store with an explicit maximum record count."""
        if capacity <= 0:
            msg = "point-in-time store capacity must be positive"
            raise ValueError(msg)
        self._capacity = capacity
        self._records: list[PointInTimeRecord] = []
        self._by_id: dict[str, PointInTimeRecord] = {}
        self._by_series: dict[tuple[RecordKind, str], list[PointInTimeRecord]] = {}

    def append(self, record: PointInTimeRecord) -> None:
        """Append one immutable revision after validating version lineage."""
        if len(self._records) >= self._capacity:
            msg = "point-in-time store capacity exhausted"
            raise OverflowError(msg)
        if record.record_id in self._by_id:
            msg = "duplicate point-in-time record_id"
            raise ValueError(msg)
        key = (record.kind, record.logical_key)
        history = self._by_series.get(key)
        if history is None:
            if record.version != 1:
                msg = "first point-in-time record version must be one"
                raise ValueError(msg)
        else:
            previous = history[-1]
            if record.version != previous.version + 1:
                msg = "point-in-time record versions must be contiguous"
                raise ValueError(msg)
            if record.revision_time_ns <= previous.revision_time_ns:
                msg = "point-in-time revision time must increase"
                raise ValueError(msg)
            self._validate_revision_link(record, previous)
        if history is None:
            self._validate_initial_link(record)
            history = []
            self._by_series[key] = history
        history.append(record)
        self._records.append(record)
        self._by_id[record.record_id] = record

    def extend(self, records: Iterable[PointInTimeRecord]) -> None:
        """Append records in caller-supplied deterministic order."""
        for record in records:
            self.append(record)

    def record_by_id(self, record_id: str) -> PointInTimeRecord | None:
        """Look up exact immutable provenance without applying revisions."""
        return self._by_id.get(record_id)

    def records_for(
        self, kind: RecordKind, logical_key: str
    ) -> tuple[PointInTimeRecord, ...]:
        """Return complete append-order revision history for one series."""
        return tuple(self._by_series.get((kind, logical_key), ()))

    def as_known_at(self, timestamp_ns: int) -> tuple[PointInTimeRecord, ...]:
        """Return the latest revision per series available at or before cutoff."""
        return self._snapshot(timestamp_ns, inclusive=True)

    def latest_available_before(
        self, timestamp_ns: int
    ) -> tuple[PointInTimeRecord, ...]:
        """Return latest revisions strictly available before the cutoff."""
        return self._snapshot(timestamp_ns, inclusive=False)

    def revisions_after(self, timestamp_ns: int) -> tuple[PointInTimeRecord, ...]:
        """Return every immutable revision whose revision time is later."""
        self._validate_timestamp(timestamp_ns)
        return tuple(
            sorted(
                (
                    record
                    for record in self._records
                    if record.revision_time_ns > timestamp_ns
                ),
                key=lambda record: (
                    record.revision_time_ns,
                    record.kind.value,
                    record.logical_key,
                    record.version,
                ),
            )
        )

    def membership_at(
        self,
        timestamp_ns: int,
        *,
        index_id: str,
        known_at_ns: int | None = None,
    ) -> tuple[str, ...]:
        """Return historical constituents using only knowledge at the cutoff."""
        cutoff = timestamp_ns if known_at_ns is None else known_at_ns
        members: set[str] = set()
        for record in self.as_known_at(cutoff):
            payload = record.payload
            if (
                isinstance(payload, IndexMembership)
                and payload.index_id == index_id
                and record.validity.contains(timestamp_ns)
            ):
                if payload.included:
                    members.add(payload.instrument_id)
                else:
                    members.discard(payload.instrument_id)
        return tuple(sorted(members))

    def symbol_mapping_at(
        self,
        timestamp_ns: int,
        *,
        symbol: str,
        known_at_ns: int | None = None,
    ) -> SymbolMapping | None:
        """Resolve historical symbology without consulting a future mapping."""
        cutoff = timestamp_ns if known_at_ns is None else known_at_ns
        for record in self.as_known_at(cutoff):
            payload = record.payload
            if (
                isinstance(payload, SymbolMapping)
                and payload.symbol == symbol
                and record.validity.contains(timestamp_ns)
            ):
                return payload
        return None

    def corporate_action_history(
        self, instrument_id: str, *, known_at_ns: int
    ) -> tuple[PointInTimeRecord, ...]:
        """Return latest known revisions for all actions on one instrument."""
        return tuple(
            record
            for record in self.as_known_at(known_at_ns)
            if isinstance(record.payload, CorporateAction)
            and record.payload.instrument_id == instrument_id
        )

    def _snapshot(
        self, timestamp_ns: int, *, inclusive: bool
    ) -> tuple[PointInTimeRecord, ...]:
        self._validate_timestamp(timestamp_ns)
        latest: dict[tuple[RecordKind, str], PointInTimeRecord] = {}
        for record in self._records:
            available = record.available_at_ns
            if available < timestamp_ns or (inclusive and available == timestamp_ns):
                latest[(record.kind, record.logical_key)] = record
        return tuple(
            latest[key]
            for key in sorted(latest, key=lambda item: (item[0].value, item[1]))
        )

    @staticmethod
    def _validate_timestamp(timestamp_ns: int) -> None:
        if timestamp_ns <= 0:
            msg = "point-in-time query timestamp must be positive"
            raise ValueError(msg)

    @staticmethod
    def _validate_initial_link(record: PointInTimeRecord) -> None:
        payload = record.payload
        if isinstance(payload, NewsRevision) and payload.correction_of_record_id:
            msg = "initial news revision cannot link to a correction parent"
            raise ValueError(msg)
        if isinstance(payload, FilingRevision) and payload.amendment_of_record_id:
            msg = "initial filing revision cannot link to an amendment parent"
            raise ValueError(msg)

    @staticmethod
    def _validate_revision_link(
        record: PointInTimeRecord, previous: PointInTimeRecord
    ) -> None:
        payload = record.payload
        if (
            isinstance(payload, NewsRevision)
            and payload.correction_of_record_id != previous.record_id
        ):
            msg = "news correction must link to the prior accepted revision"
            raise ValueError(msg)
        if (
            isinstance(payload, FilingRevision)
            and payload.amendment_of_record_id != previous.record_id
        ):
            msg = "filing amendment must link to the prior accepted revision"
            raise ValueError(msg)
