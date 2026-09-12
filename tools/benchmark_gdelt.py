"""Socket-free benchmark for the bounded GDELT metadata pipeline."""

from __future__ import annotations

import argparse
import json
import time
import tracemalloc
from datetime import UTC, date, datetime
from pathlib import Path
from typing import Final

from aegis_mx_research.forecast_contracts import (
    UnresolvedInstrumentResolver,
    load_ticker_universe,
)
from aegis_mx_research.gdelt import (
    GDELT_ATTRIBUTION,
    GdeltAdapter,
    GdeltAuthorization,
    GdeltConfig,
    GdeltEntityResolver,
    GdeltQueryResult,
    ScriptedGdeltQueryClient,
    build_query_plans,
    load_issuer_resolver,
)

SEED: Final = 20260911
DEFAULT_ROWS: Final = 10_000
MAX_ROWS: Final = 25_000
WINDOW_DATE: Final = date(2026, 9, 10)
NOW_NS: Final = int(datetime(2026, 9, 11, tzinfo=UTC).timestamp() * 1_000_000_000)


def main(argv: list[str] | None = None) -> int:
    """Measure parse/entity/classifier/dedup throughput and traced memory."""
    parser = argparse.ArgumentParser(prog="benchmark-gdelt")
    parser.add_argument("--rows", type=int, default=DEFAULT_ROWS)
    parser.add_argument(
        "--output", type=Path, default=Path("build/benchmarks/gdelt-prompt-54.json")
    )
    arguments = parser.parse_args(argv)
    if not 1 <= arguments.rows <= MAX_ROWS:
        parser.error(f"--rows must be between 1 and {MAX_ROWS}")

    universe = load_ticker_universe(UnresolvedInstrumentResolver())
    resolver = load_issuer_resolver(universe)
    resolver = GdeltEntityResolver(
        (resolver.identities[0],), resolver.unresolved_tickers
    )
    config = GdeltConfig(
        WINDOW_DATE,
        WINDOW_DATE,
        row_limit=arguments.rows,
        maximum_bytes_billed_per_query=1_000_000_000,
    )
    plans = build_query_plans(config, resolver)
    plan = plans[0]
    issuer_name = resolver.identities[0].canonical_name
    rows = tuple(
        {
            "gdelt_record_time": "20260910000000",
            "organizations": f"{issuer_name},10",
            "provider_record_id": f"20260910000000-{index}",
            "source_collection_identifier": "1",
            "source_common_name": "benchmark.invalid",
            "source_language": "eng",
            "source_url": f"https://benchmark.invalid/{index}",
            "themes": "EARNINGS,1",
        }
        for index in range(arguments.rows)
    )
    result = GdeltQueryResult(
        plan_id=plan.plan_id,
        rows=rows,
        receipt_time_utc_ns=NOW_NS,
        bytes_processed=0,
        request_count=1,
        network_access_performed=False,
    )
    client = ScriptedGdeltQueryClient({plan.plan_id: result})
    authorization = GdeltAuthorization(
        approval_id="AEGIS-GDELT-BENCHMARK",
        universe_snapshot_sha256=universe.universe_snapshot_sha256.hex(),
        valid_from=WINDOW_DATE,
        valid_through=WINDOW_DATE,
        expires_at_utc_ns=NOW_NS + 1_000_000_000,
        metadata_storage_authorized=True,
        derived_data_authorized=True,
        publisher_full_text_authorized=False,
        bigquery_execution_authorized=False,
        storage_limit_bytes=5_000_000_000,
        attribution=GDELT_ATTRIBUTION,
        approval_sha256="b" * 64,
    )
    clock = lambda: NOW_NS  # noqa: E731
    adapter = GdeltAdapter(
        config, universe, resolver, authorization, wall_clock_ns=clock
    )
    tracemalloc.start()
    started = time.perf_counter_ns()
    report = adapter.run((plan,), client)
    elapsed_ns = time.perf_counter_ns() - started
    _, peak_bytes = tracemalloc.get_traced_memory()
    tracemalloc.stop()
    output = {
        "accepted_records": report.accepted_records,
        "attribution": GDELT_ATTRIBUTION,
        "elapsed_ns": elapsed_ns,
        "infrastructure_validation_only": True,
        "network_access_performed": False,
        "peak_traced_python_bytes": peak_bytes,
        "rows_per_second": (arguments.rows * 1_000_000_000) // max(elapsed_ns, 1),
        "schema_version": "1.0.0",
        "seed": SEED,
    }
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(
        json.dumps(output, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(json.dumps(output, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
