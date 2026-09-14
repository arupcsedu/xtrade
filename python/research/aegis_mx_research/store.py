"""Append-only point-in-time store and deterministic temporal queries."""

from __future__ import annotations

import hashlib
import json
import os
import sqlite3
from typing import TYPE_CHECKING, Final, Self, cast

from aegis_mx_research.types import (
    AnalystEstimateVintage,
    CorporateAction,
    CorporateActionType,
    DataSource,
    Delisting,
    FilingRevision,
    IndexMembership,
    MacroVintage,
    NewsRevision,
    PointInTimeRecord,
    RecordKind,
    RecordPayload,
    SourceKind,
    SymbolMapping,
    ValidityInterval,
)

if TYPE_CHECKING:
    from collections.abc import Callable, Iterable, Mapping
    from pathlib import Path

PERSISTENT_STORE_SCHEMA_VERSION: Final = "1.0.0"
_SQLITE_APPLICATION_ID: Final = 0x414D5850
_MINIMUM_DATABASE_BYTES: Final = 32_768


class PersistentStoreError(RuntimeError):
    """Fail-closed persistent-store integrity or compatibility failure."""


def _canonical_bytes(value: object) -> bytes:
    return json.dumps(
        value, ensure_ascii=True, separators=(",", ":"), sort_keys=True
    ).encode("ascii")


def _validate_timestamp(timestamp_ns: int) -> None:
    if timestamp_ns <= 0:
        raise ValueError("point-in-time query timestamp must be positive")


def _validate_initial_link(record: PointInTimeRecord) -> None:
    payload = record.payload
    if isinstance(payload, NewsRevision) and payload.correction_of_record_id:
        raise ValueError("initial news revision cannot link to a correction parent")
    if isinstance(payload, FilingRevision) and payload.amendment_of_record_id:
        raise ValueError("initial filing revision cannot link to an amendment parent")


def _validate_revision_link(
    record: PointInTimeRecord, previous: PointInTimeRecord
) -> None:
    payload = record.payload
    if (
        isinstance(payload, NewsRevision)
        and payload.correction_of_record_id != previous.record_id
    ):
        raise ValueError("news correction must link to the prior accepted revision")
    if (
        isinstance(payload, FilingRevision)
        and payload.amendment_of_record_id != previous.record_id
    ):
        raise ValueError("filing amendment must link to the prior accepted revision")


def _payload_document(record: PointInTimeRecord) -> dict[str, object]:
    payload: dict[str, object]
    if isinstance(record.payload, CorporateAction):
        payload = {
            "action_id": record.payload.action_id,
            "action_type": int(record.payload.action_type),
            "instrument_id": record.payload.instrument_id,
            "ratio_denominator": record.payload.ratio_denominator,
            "ratio_numerator": record.payload.ratio_numerator,
        }
    elif isinstance(record.payload, SymbolMapping):
        payload = {
            "instrument_id": record.payload.instrument_id,
            "symbol": record.payload.symbol,
            "venue": record.payload.venue,
        }
    elif isinstance(record.payload, Delisting):
        payload = {
            "instrument_id": record.payload.instrument_id,
            "reason": record.payload.reason,
        }
    elif isinstance(record.payload, IndexMembership):
        payload = {
            "included": record.payload.included,
            "index_id": record.payload.index_id,
            "instrument_id": record.payload.instrument_id,
        }
    elif isinstance(record.payload, AnalystEstimateVintage):
        payload = {
            "fiscal_period": record.payload.fiscal_period,
            "instrument_id": record.payload.instrument_id,
            "metric_id": record.payload.metric_id,
            "unit": record.payload.unit,
            "value": record.payload.value,
        }
    elif isinstance(record.payload, MacroVintage):
        payload = {
            "reference_period": record.payload.reference_period,
            "series_id": record.payload.series_id,
            "unit": record.payload.unit,
            "value": record.payload.value,
        }
    elif isinstance(record.payload, NewsRevision):
        payload = {
            "correction_of_record_id": record.payload.correction_of_record_id,
            "document_id": record.payload.document_id,
        }
    else:
        payload = {
            "accession_id": record.payload.accession_id,
            "amendment_of_record_id": record.payload.amendment_of_record_id,
            "form_type": record.payload.form_type,
        }
    return {
        "event_time_ns": record.event_time_ns,
        "kind": int(record.kind),
        "logical_key": record.logical_key,
        "payload": payload,
        "processing_time_ns": record.processing_time_ns,
        "publication_time_ns": record.publication_time_ns,
        "receive_time_ns": record.receive_time_ns,
        "record_id": record.record_id,
        "revision_time_ns": record.revision_time_ns,
        "schema_version": record.schema_version,
        "source": {
            "authenticated": record.source.authenticated,
            "content_sha256": record.source.content_sha256.hex(),
            "document_id": record.source.document_id,
            "kind": int(record.source.kind),
            "provider": record.source.provider,
            "source_id": record.source.source_id,
        },
        "validity": {
            "valid_from_ns": record.validity.valid_from_ns,
            "valid_to_ns": record.validity.valid_to_ns,
        },
        "version": record.version,
    }


def _require_mapping(value: object, field: str) -> Mapping[str, object]:
    if not isinstance(value, dict) or not all(isinstance(key, str) for key in value):
        raise PersistentStoreError(f"{field} is not an object")
    return value


def _require_str(value: object, field: str) -> str:
    if not isinstance(value, str):
        raise PersistentStoreError(f"{field} is not a string")
    return value


def _require_int(value: object, field: str) -> int:
    if type(value) is not int:
        raise PersistentStoreError(f"{field} is not an integer")
    return value


def _record_from_bytes(encoded: bytes) -> PointInTimeRecord:  # noqa: C901, PLR0912
    try:
        document = _require_mapping(json.loads(encoded), "record")
        payload_value = _require_mapping(document["payload"], "payload")
        source_value = _require_mapping(document["source"], "source")
        validity_value = _require_mapping(document["validity"], "validity")
        kind = RecordKind(_require_int(document["kind"], "kind"))
        source_kind = SourceKind(_require_int(source_value["kind"], "source.kind"))
        digest = bytes.fromhex(
            _require_str(source_value["content_sha256"], "source.content_sha256")
        )
        payload: RecordPayload
        if kind is RecordKind.CORPORATE_ACTION:
            payload = CorporateAction(
                _require_str(payload_value["action_id"], "action_id"),
                _require_str(payload_value["instrument_id"], "instrument_id"),
                CorporateActionType(
                    _require_int(payload_value["action_type"], "action_type")
                ),
                _require_int(payload_value["ratio_numerator"], "ratio_numerator"),
                _require_int(payload_value["ratio_denominator"], "ratio_denominator"),
            )
        elif kind is RecordKind.SYMBOL_MAPPING:
            payload = SymbolMapping(
                _require_str(payload_value["symbol"], "symbol"),
                _require_str(payload_value["instrument_id"], "instrument_id"),
                _require_str(payload_value["venue"], "venue"),
            )
        elif kind is RecordKind.DELISTING:
            payload = Delisting(
                _require_str(payload_value["instrument_id"], "instrument_id"),
                _require_str(payload_value["reason"], "reason"),
            )
        elif kind is RecordKind.INDEX_MEMBERSHIP:
            included = payload_value["included"]
            if not isinstance(included, bool):
                raise PersistentStoreError("included is not a boolean")
            payload = IndexMembership(
                _require_str(payload_value["index_id"], "index_id"),
                _require_str(payload_value["instrument_id"], "instrument_id"),
                included,
            )
        elif kind is RecordKind.ANALYST_ESTIMATE:
            payload = AnalystEstimateVintage(
                _require_str(payload_value["instrument_id"], "instrument_id"),
                _require_str(payload_value["metric_id"], "metric_id"),
                _require_str(payload_value["fiscal_period"], "fiscal_period"),
                _require_int(payload_value["value"], "value"),
                _require_str(payload_value["unit"], "unit"),
            )
        elif kind is RecordKind.MACRO_VINTAGE:
            payload = MacroVintage(
                _require_str(payload_value["series_id"], "series_id"),
                _require_str(payload_value["reference_period"], "reference_period"),
                _require_int(payload_value["value"], "value"),
                _require_str(payload_value["unit"], "unit"),
            )
        elif kind is RecordKind.NEWS_REVISION:
            correction = payload_value["correction_of_record_id"]
            if correction is not None and not isinstance(correction, str):
                raise PersistentStoreError("correction parent is malformed")
            payload = NewsRevision(
                _require_str(payload_value["document_id"], "document_id"), correction
            )
        else:
            amendment = payload_value["amendment_of_record_id"]
            if amendment is not None and not isinstance(amendment, str):
                raise PersistentStoreError("amendment parent is malformed")
            payload = FilingRevision(
                _require_str(payload_value["accession_id"], "accession_id"),
                _require_str(payload_value["form_type"], "form_type"),
                amendment,
            )
        valid_to = validity_value["valid_to_ns"]
        if valid_to is not None:
            valid_to = _require_int(valid_to, "valid_to_ns")
        authenticated = source_value["authenticated"]
        if not isinstance(authenticated, bool):
            raise PersistentStoreError("source.authenticated is not a boolean")
        return PointInTimeRecord(
            record_id=_require_str(document["record_id"], "record_id"),
            logical_key=_require_str(document["logical_key"], "logical_key"),
            kind=kind,
            event_time_ns=_require_int(document["event_time_ns"], "event_time_ns"),
            publication_time_ns=_require_int(
                document["publication_time_ns"], "publication_time_ns"
            ),
            receive_time_ns=_require_int(
                document["receive_time_ns"], "receive_time_ns"
            ),
            processing_time_ns=_require_int(
                document["processing_time_ns"], "processing_time_ns"
            ),
            revision_time_ns=_require_int(
                document["revision_time_ns"], "revision_time_ns"
            ),
            validity=ValidityInterval(
                _require_int(validity_value["valid_from_ns"], "valid_from_ns"),
                valid_to,
            ),
            source=DataSource(
                source_id=_require_str(source_value["source_id"], "source_id"),
                kind=source_kind,
                provider=_require_str(source_value["provider"], "provider"),
                document_id=_require_str(
                    source_value["document_id"], "source.document_id"
                ),
                content_sha256=digest,
                authenticated=authenticated,
            ),
            version=_require_int(document["version"], "version"),
            payload=payload,
            schema_version=_require_str(document["schema_version"], "schema_version"),
        )
    except (KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
        raise PersistentStoreError("stored point-in-time record is corrupt") from error


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

    _validate_timestamp = staticmethod(_validate_timestamp)
    _validate_initial_link = staticmethod(_validate_initial_link)
    _validate_revision_link = staticmethod(_validate_revision_link)


class PersistentPointInTimeStore:
    """Bounded SQLite-backed store with exact ``PointInTimeStore`` semantics."""

    def __init__(
        self,
        path: Path,
        *,
        capacity: int = 10_000_000,
        maximum_bytes: int = 4_000_000_000,
    ) -> None:
        """Open or create one owner-only, crash-safe point-in-time database."""
        if capacity <= 0:
            raise ValueError("point-in-time store capacity must be positive")
        if maximum_bytes < _MINIMUM_DATABASE_BYTES:
            raise ValueError("persistent store maximum_bytes is too small")
        self.path = path
        self._capacity = capacity
        self._maximum_bytes = maximum_bytes
        self._closed = False
        self._validate_path()
        path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        self._validate_path()
        try:
            self._connection = sqlite3.connect(path, isolation_level=None)
            self._configure()
            self._initialize_or_validate()
        except (OSError, sqlite3.DatabaseError, PersistentStoreError) as error:
            connection = getattr(self, "_connection", None)
            if connection is not None:
                connection.close()
            if isinstance(error, PersistentStoreError):
                raise
            raise PersistentStoreError(
                "persistent point-in-time store cannot be opened safely"
            ) from error

    def __enter__(self) -> Self:
        """Return the active store for a context-managed lifetime."""
        return self

    def __exit__(self, *_args: object) -> None:
        """Close the database on context exit."""
        self.close()

    def _validate_path(self) -> None:
        if self.path.is_symlink():
            raise PersistentStoreError("persistent store path cannot be a symlink")
        parent = self.path.parent
        if parent.exists() and parent.is_symlink():
            raise PersistentStoreError("persistent store parent cannot be a symlink")
        if self.path.exists() and not self.path.is_file():
            raise PersistentStoreError("persistent store path is not a regular file")

    def _configure(self) -> None:
        self._connection.execute("PRAGMA journal_mode=DELETE")
        self._connection.execute("PRAGMA synchronous=FULL")
        self._connection.execute("PRAGMA foreign_keys=ON")
        self._connection.execute("PRAGMA trusted_schema=OFF")
        self._connection.execute("PRAGMA temp_store=MEMORY")
        page_size = cast(
            "int", self._connection.execute("PRAGMA page_size").fetchone()[0]
        )
        maximum_pages = self._maximum_bytes // page_size
        if maximum_pages < 1:
            raise PersistentStoreError("persistent store page budget is empty")
        self._connection.execute(f"PRAGMA max_page_count={maximum_pages}")

    def _initialize_or_validate(self) -> None:
        application_id = cast(
            "int", self._connection.execute("PRAGMA application_id").fetchone()[0]
        )
        user_version = cast(
            "int", self._connection.execute("PRAGMA user_version").fetchone()[0]
        )
        if application_id not in {0, _SQLITE_APPLICATION_ID}:
            raise PersistentStoreError("incompatible SQLite application identifier")
        if user_version not in {0, 1}:
            raise PersistentStoreError("unsupported persistent-store schema version")
        if application_id == 0:
            self._connection.executescript(
                f"""
                PRAGMA application_id={_SQLITE_APPLICATION_ID};
                PRAGMA user_version=1;
                CREATE TABLE metadata(
                  key TEXT PRIMARY KEY,
                  value TEXT NOT NULL
                ) WITHOUT ROWID;
                INSERT INTO metadata VALUES(
                  'schema_version', '{PERSISTENT_STORE_SCHEMA_VERSION}'
                );
                CREATE TABLE records(
                  sequence INTEGER PRIMARY KEY AUTOINCREMENT,
                  record_id TEXT NOT NULL UNIQUE,
                  kind INTEGER NOT NULL,
                  logical_key TEXT NOT NULL,
                  version INTEGER NOT NULL,
                  event_time_ns INTEGER NOT NULL,
                  revision_time_ns INTEGER NOT NULL,
                  available_at_ns INTEGER NOT NULL,
                  record_json BLOB NOT NULL,
                  record_sha256 BLOB NOT NULL CHECK(length(record_sha256) = 32),
                  UNIQUE(kind, logical_key, version)
                );
                CREATE INDEX records_series_idx
                  ON records(kind, logical_key, version);
                CREATE INDEX records_available_idx
                  ON records(available_at_ns, kind, logical_key, version);
                CREATE INDEX records_revision_idx
                  ON records(revision_time_ns, kind, logical_key, version);
                """
            )
        result = self._connection.execute("PRAGMA quick_check").fetchone()
        if result is None or result[0] != "ok":
            raise PersistentStoreError("persistent store integrity check failed")
        metadata = self._connection.execute(
            "SELECT value FROM metadata WHERE key='schema_version'"
        ).fetchone()
        if metadata is None or metadata[0] != PERSISTENT_STORE_SCHEMA_VERSION:
            raise PersistentStoreError("persistent store metadata is incompatible")

    def close(self) -> None:
        """Flush and close the database idempotently."""
        if not self._closed:
            self._connection.close()
            self._closed = True

    def __len__(self) -> int:
        """Return the committed immutable record count."""
        self._ensure_open()
        return cast(
            "int",
            self._connection.execute("SELECT count(*) FROM records").fetchone()[0],
        )

    def append(  # noqa: C901, PLR0912
        self,
        record: PointInTimeRecord,
        *,
        fault_injector: Callable[[str], None] | None = None,
    ) -> None:
        """Atomically append one validated revision without replacing bytes."""
        self._ensure_open()
        encoded = _canonical_bytes(_payload_document(record))
        digest = hashlib.sha256(encoded).digest()
        try:
            self._connection.execute("BEGIN IMMEDIATE")
            count = cast(
                "int",
                self._connection.execute("SELECT count(*) FROM records").fetchone()[0],
            )
            if count >= self._capacity:
                raise OverflowError("point-in-time store capacity exhausted")
            if self._connection.execute(
                "SELECT 1 FROM records WHERE record_id=?", (record.record_id,)
            ).fetchone():
                raise ValueError("duplicate point-in-time record_id")
            previous_row = self._connection.execute(
                """
                SELECT record_json FROM records
                WHERE kind=? AND logical_key=? ORDER BY version DESC LIMIT 1
                """,
                (int(record.kind), record.logical_key),
            ).fetchone()
            if previous_row is None:
                if record.version != 1:
                    raise ValueError("first point-in-time record version must be one")
                _validate_initial_link(record)
            else:
                previous = _record_from_bytes(cast("bytes", previous_row[0]))
                if record.version != previous.version + 1:
                    raise ValueError("point-in-time record versions must be contiguous")
                if record.revision_time_ns <= previous.revision_time_ns:
                    raise ValueError("point-in-time revision time must increase")
                _validate_revision_link(record, previous)
            if fault_injector is not None:
                fault_injector("BEFORE_INSERT")
            self._connection.execute(
                """
                INSERT INTO records(
                  record_id, kind, logical_key, version, event_time_ns,
                  revision_time_ns, available_at_ns, record_json, record_sha256
                ) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    record.record_id,
                    int(record.kind),
                    record.logical_key,
                    record.version,
                    record.event_time_ns,
                    record.revision_time_ns,
                    record.available_at_ns,
                    encoded,
                    digest,
                ),
            )
            if fault_injector is not None:
                fault_injector("AFTER_INSERT_BEFORE_COMMIT")
            self._connection.execute("COMMIT")
            self._sync_parent()
        except (OverflowError, ValueError):
            self._rollback()
            raise
        except sqlite3.DatabaseError as error:
            self._rollback()
            if "full" in str(error).lower():
                raise OverflowError(
                    "persistent point-in-time byte limit reached"
                ) from error
            raise PersistentStoreError("persistent append failed") from error
        except Exception:
            self._rollback()
            raise

    def extend(self, records: Iterable[PointInTimeRecord]) -> None:
        """Append records in caller-supplied deterministic order."""
        for record in records:
            self.append(record)

    def record_by_id(self, record_id: str) -> PointInTimeRecord | None:
        """Return one verified immutable record by identity."""
        self._ensure_open()
        row = self._connection.execute(
            "SELECT record_json, record_sha256 FROM records WHERE record_id=?",
            (record_id,),
        ).fetchone()
        return None if row is None else self._decode_row(row)

    def records_for(
        self, kind: RecordKind, logical_key: str
    ) -> tuple[PointInTimeRecord, ...]:
        """Return complete append-order revision history for one series."""
        self._ensure_open()
        rows = self._connection.execute(
            """
            SELECT record_json, record_sha256 FROM records
            WHERE kind=? AND logical_key=? ORDER BY version
            """,
            (int(kind), logical_key),
        )
        return tuple(self._decode_row(row) for row in rows)

    def as_known_at(self, timestamp_ns: int) -> tuple[PointInTimeRecord, ...]:
        """Return the same inclusive knowledge snapshot as the reference store."""
        return self._snapshot(timestamp_ns, inclusive=True)

    def latest_available_before(
        self, timestamp_ns: int
    ) -> tuple[PointInTimeRecord, ...]:
        """Return the same strict knowledge snapshot as the reference store."""
        return self._snapshot(timestamp_ns, inclusive=False)

    def revisions_after(self, timestamp_ns: int) -> tuple[PointInTimeRecord, ...]:
        """Return revisions after the cutoff in reference-store sort order."""
        _validate_timestamp(timestamp_ns)
        rows = self._connection.execute(
            """
            SELECT record_json, record_sha256 FROM records
            WHERE revision_time_ns > ?
            ORDER BY revision_time_ns, kind, logical_key, version
            """,
            (timestamp_ns,),
        )
        return tuple(self._decode_row(row) for row in rows)

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
        """Resolve symbology with exact in-memory-store semantics."""
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
        """Return latest known action revisions for one instrument."""
        return tuple(
            record
            for record in self.as_known_at(known_at_ns)
            if isinstance(record.payload, CorporateAction)
            and record.payload.instrument_id == instrument_id
        )

    def deterministic_hash(self) -> str:
        """Hash committed logical records independently of SQLite page layout."""
        self._ensure_open()
        digest = hashlib.sha256()
        for row in self._connection.execute(
            "SELECT record_sha256 FROM records ORDER BY sequence"
        ):
            digest.update(cast("bytes", row[0]))
        return digest.hexdigest()

    def _snapshot(
        self, timestamp_ns: int, *, inclusive: bool
    ) -> tuple[PointInTimeRecord, ...]:
        _validate_timestamp(timestamp_ns)
        query = (
            """
            WITH eligible AS (
              SELECT record_json, record_sha256, kind, logical_key,
                     row_number() OVER (
                       PARTITION BY kind, logical_key
                       ORDER BY available_at_ns DESC, sequence DESC
                     ) AS rank
              FROM records WHERE available_at_ns <= ?
            )
            SELECT record_json, record_sha256 FROM eligible
            WHERE rank=1 ORDER BY kind, logical_key
            """
            if inclusive
            else """
            WITH eligible AS (
              SELECT record_json, record_sha256, kind, logical_key,
                     row_number() OVER (
                       PARTITION BY kind, logical_key
                       ORDER BY available_at_ns DESC, sequence DESC
                     ) AS rank
              FROM records WHERE available_at_ns < ?
            )
            SELECT record_json, record_sha256 FROM eligible
            WHERE rank=1 ORDER BY kind, logical_key
            """
        )
        rows = self._connection.execute(
            query,
            (timestamp_ns,),
        )
        return tuple(self._decode_row(row) for row in rows)

    @staticmethod
    def _decode_row(row: tuple[object, ...]) -> PointInTimeRecord:
        encoded_value, expected_value = row
        if not isinstance(encoded_value, bytes) or not isinstance(
            expected_value, bytes
        ):
            raise PersistentStoreError("stored record encoding is not binary")
        encoded = encoded_value
        expected = expected_value
        if hashlib.sha256(encoded).digest() != expected:
            raise PersistentStoreError("stored record hash does not match")
        return _record_from_bytes(encoded)

    def _rollback(self) -> None:
        if self._connection.in_transaction:
            self._connection.execute("ROLLBACK")

    def _ensure_open(self) -> None:
        if self._closed:
            raise PersistentStoreError("persistent point-in-time store is closed")

    def _sync_parent(self) -> None:
        descriptor = os.open(self.path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(descriptor)
        finally:
            os.close(descriptor)
