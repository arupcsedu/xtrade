"""Console entry points for the three time-series service roles."""

from __future__ import annotations

import argparse
import json
import sys
from typing import TYPE_CHECKING

from aegis_mx_timeseries.runtime import (
    ServiceRole,
    build_reference_application,
    serve,
    status_payload,
)

if TYPE_CHECKING:
    from collections.abc import Sequence


def main(role: ServiceRole, arguments: Sequence[str] | None = None) -> int:
    """Parse a bounded runtime configuration and run one role."""
    parser = argparse.ArgumentParser(prog=role.value)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", default=8080, type=int)
    parsed = parser.parse_args(arguments)
    application = build_reference_application(role)
    sys.stderr.write(
        json.dumps(status_payload(application), sort_keys=True, default=int) + "\n"
    )
    serve(application, parsed.host, parsed.port)
    return 0


def api_main() -> int:
    """Run `timeseries-forecast-api`."""
    return main(ServiceRole.API)


def worker_main() -> int:
    """Run `timeseries-forecast-worker`."""
    return main(ServiceRole.WORKER)


def context_builder_main() -> int:
    """Run `timeseries-context-builder`."""
    return main(ServiceRole.CONTEXT_BUILDER)
