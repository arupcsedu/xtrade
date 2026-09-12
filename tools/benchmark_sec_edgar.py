"""Bounded SEC parser, sanitizer, and storage microbenchmarks."""

from __future__ import annotations

import argparse
import json
import platform
import statistics
import sys
import tempfile
import time
from pathlib import Path
from typing import TYPE_CHECKING, Final

from aegis_mx_research.data_repository import (
    DataRepository,
    FileSystemState,
    QuotaEvidence,
)
from aegis_mx_research.forecast_contracts import (
    UnresolvedInstrumentResolver,
    load_ticker_universe,
)
from aegis_mx_research.sec_edgar import (
    SecDataset,
    SecPublication,
    SecRepository,
    parse_company_facts,
    parse_company_tickers,
    parse_submissions,
    primary_document_url,
    sanitize_filing_document,
)

DEFAULT_ITERATIONS: Final = 100
NOW_NS: Final = 1_800_000_000_000_000_000
CIK: Final = "0000320193"
ACCESSION: Final = "0000320193-26-000001"
MAX_ITERATIONS: Final = 10_000

if TYPE_CHECKING:
    from collections.abc import Callable


def _canonical(value: object) -> bytes:
    return json.dumps(value, separators=(",", ":"), sort_keys=True).encode()


def _measure(operation: Callable[[], object], iterations: int) -> dict[str, int]:
    samples: list[int] = []
    for _ in range(iterations):
        started = time.perf_counter_ns()
        operation()
        samples.append(time.perf_counter_ns() - started)
    samples.sort()
    return {
        "iterations": iterations,
        "maximum_ns": samples[-1],
        "p50_ns": int(statistics.median(samples)),
        "p95_ns": samples[min(len(samples) - 1, int(len(samples) * 0.95))],
        "p99_ns": samples[min(len(samples) - 1, int(len(samples) * 0.99))],
    }


def _fixtures() -> tuple[bytes, bytes, bytes, bytes]:
    ticker_map = _canonical(
        {"0": {"cik_str": 320193, "ticker": "AAPL", "title": "Apple Inc."}}
    )
    submissions = _canonical(
        {
            "cik": "320193",
            "filings": {
                "files": [],
                "recent": {
                    "accessionNumber": [ACCESSION],
                    "acceptanceDateTime": ["2026-02-01T12:00:00Z"],
                    "filingDate": ["2026-02-01"],
                    "form": ["10-K"],
                    "primaryDocument": ["form10k.htm"],
                    "reportDate": ["2025-12-31"],
                },
            },
        }
    )
    facts = _canonical(
        {
            "cik": 320193,
            "facts": {
                "us-gaap": {
                    "Revenue": {
                        "units": {
                            "USD": [
                                {
                                    "accn": ACCESSION,
                                    "end": "2025-12-31",
                                    "filed": "2026-02-01",
                                    "form": "10-K",
                                    "val": 1234,
                                }
                            ]
                        }
                    }
                }
            },
        }
    )
    document = (
        b"<html><body><h1>Item 2.02 Results</h1><p>Revenue increased.</p></body></html>"
    )
    return ticker_map, submissions, facts, document


def run(iterations: int) -> dict[str, object]:
    """Run deterministic synthetic microbenchmarks without network access."""
    if not 1 <= iterations <= MAX_ITERATIONS:
        message = "iterations must be within [1, 10000]"
        raise ValueError(message)
    universe = load_ticker_universe(UnresolvedInstrumentResolver())
    ticker_map, submissions, facts, document = _fixtures()
    document_url = primary_document_url(CIK, ACCESSION, "form10k.htm")
    measurements = {
        "company_facts_parse": _measure(
            lambda: parse_company_facts(
                facts,
                expected_cik=CIK,
                received_at_utc_ns=NOW_NS,
                processing_time_utc_ns=NOW_NS,
            ),
            iterations,
        ),
        "company_tickers_parse": _measure(
            lambda: parse_company_tickers(
                ticker_map, universe, received_at_utc_ns=NOW_NS
            ),
            iterations,
        ),
        "filing_sanitize": _measure(
            lambda: sanitize_filing_document(
                document, source_uri=document_url, receipt_time_utc_ns=NOW_NS
            ),
            iterations,
        ),
        "submissions_parse": _measure(
            lambda: parse_submissions(
                submissions,
                expected_cik=CIK,
                received_at_utc_ns=NOW_NS,
                processing_time_utc_ns=NOW_NS,
            ),
            iterations,
        ),
    }
    with tempfile.TemporaryDirectory(prefix="aegis-sec-benchmark-") as directory:
        root = Path(directory) / "data"
        repository = DataRepository(
            root,
            filesystem_probe=lambda _: FileSystemState(
                total_bytes=12_000_000_000_000,
                free_bytes=11_000_000_000_000,
            ),
            git_worktree=None,
        )
        repository.initialize()
        store = SecRepository(
            repository,
            QuotaEvidence(
                limit_bytes=10_995_116_277_760,
                used_bytes=1_000_000_000,
                source="benchmark fixture",
                observed_at_utc="2026-09-11T00:00:00+00:00",
                authoritative=True,
            ),
            universe_sha256=universe.universe_snapshot_sha256.hex(),
        )
        counter = 0

        def publish() -> object:
            nonlocal counter
            counter += 1
            payload = _canonical({"iteration": counter})
            return store.publish(
                SecPublication(
                    dataset=SecDataset.COMPANY_TICKERS,
                    source_url="https://www.sec.gov/files/company_tickers.json",
                    payload=payload,
                    receipt_time_utc_ns=NOW_NS + counter,
                    processing_time_utc_ns=NOW_NS + counter,
                    record_count=1,
                    schema_name="benchmark-sec-object",
                )
            )

        measurements["storage_publication"] = _measure(publish, min(iterations, 100))
        with store.publication_batch():
            measurements["batched_storage_publication"] = _measure(
                publish, min(iterations, 100)
            )
    return {
        "hardware": {
            "machine": platform.machine(),
            "processor": platform.processor() or "unknown",
            "python": platform.python_version(),
        },
        "measurements": measurements,
        "network_access_performed": False,
        "synthetic_fixture": True,
    }


def main(argv: list[str] | None = None) -> int:
    """Run the benchmark and emit canonical JSON."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--iterations", type=int, default=DEFAULT_ITERATIONS)
    arguments = parser.parse_args(argv)
    sys.stdout.write(json.dumps(run(arguments.iterations), sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
