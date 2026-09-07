"""Console entry points for the three time-series service roles."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import TYPE_CHECKING

from aegis_mx_timeseries.runtime import (
    RequestSecurity,
    ServiceRole,
    build_reference_application,
    serve,
    status_payload,
)
from aegis_mx_timeseries.transport_security import (
    IdentityPolicy,
    IdentityRateLimiter,
    MountedSecretProvider,
    RotatingTLSContext,
    ServiceIdentity,
    TLSMaterial,
    parse_identity_bindings,
)

if TYPE_CHECKING:
    from collections.abc import Sequence


def main(role: ServiceRole, arguments: Sequence[str] | None = None) -> int:
    """Parse a bounded runtime configuration and run one role."""
    parser = argparse.ArgumentParser(prog=role.value)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", default=8080, type=int)
    parser.add_argument("--allow-insecure-loopback", action="store_true")
    parser.add_argument("--secret-root", type=Path)
    parser.add_argument("--tls-certificate", default="tls.crt")
    parser.add_argument("--tls-private-key", default="tls.key")
    parser.add_argument("--tls-trust-bundle", default="ca.crt")
    parser.add_argument("--service-identity")
    parser.add_argument("--client-binding", action="append", default=[])
    parser.add_argument("--requests-per-second", default=1_000, type=int)
    parser.add_argument("--request-burst", default=2_000, type=int)
    parser.add_argument("--maximum-concurrent-requests", default=64, type=int)
    parsed = parser.parse_args(arguments)
    request_security = None
    if not parsed.allow_insecure_loopback:
        if parsed.secret_root is None or parsed.service_identity is None:
            parser.error(
                "--secret-root and --service-identity are required unless "
                "--allow-insecure-loopback is explicit"
            )
        provider = MountedSecretProvider(parsed.secret_root)
        material = TLSMaterial(
            parsed.tls_certificate,
            parsed.tls_private_key,
            parsed.tls_trust_bundle,
            ServiceIdentity(parsed.service_identity),
        )
        request_security = RequestSecurity(
            RotatingTLSContext(provider, material),
            IdentityPolicy(parse_identity_bindings(parsed.client_binding)),
            IdentityRateLimiter(parsed.requests_per_second, parsed.request_burst),
            parsed.maximum_concurrent_requests,
        )
    application = build_reference_application(role)
    sys.stderr.write(
        json.dumps(status_payload(application), sort_keys=True, default=int) + "\n"
    )
    serve(
        application,
        parsed.host,
        parsed.port,
        request_security=request_security,
        allow_insecure_loopback=parsed.allow_insecure_loopback,
    )
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
