"""Container entry point for one explicitly selected time-series service role."""

from __future__ import annotations

import os

from aegis_mx_timeseries.cli import main
from aegis_mx_timeseries.runtime import ServiceRole


def container_main() -> int:
    """Validate container environment and start one non-trading service role."""
    role = ServiceRole(os.environ.get("AEGIS_TIMESERIES_ROLE", ServiceRole.API.value))
    host = os.environ.get("AEGIS_TIMESERIES_HOST", "127.0.0.1")
    port = os.environ.get("AEGIS_TIMESERIES_PORT", "8080")
    arguments = [
        "--host",
        host,
        "--port",
        port,
        "--secret-root",
        os.environ.get("AEGIS_TLS_SECRET_ROOT", "/var/run/aegis-mx/tls"),
        "--service-identity",
        os.environ.get("AEGIS_SERVICE_IDENTITY", ""),
    ]
    bindings = (
        item.strip()
        for item in os.environ.get("AEGIS_TLS_CLIENT_BINDINGS", "").split(";")
    )
    for binding in filter(None, bindings):
        arguments.extend(("--client-binding", binding))
    return main(role, arguments)


if __name__ == "__main__":  # pragma: no cover - container process boundary.
    raise SystemExit(container_main())
