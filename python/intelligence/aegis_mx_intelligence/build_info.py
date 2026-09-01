"""Deterministic build identity for the Python foundation package."""

from dataclasses import dataclass
from typing import Final, Literal

VERSION: Final = "0.1.0"
DEFAULT_TEST_SEED: Final = 20_260_828


@dataclass(frozen=True, slots=True)
class BuildInfo:
    """Immutable package identity with an explicit non-live capability flag."""

    project: str
    version: str
    test_seed: int
    live_trading_capable: Literal[False]


def current_build_info() -> BuildInfo:
    """Return deterministic package metadata without host or wall-clock data."""
    return BuildInfo(
        project="Aegis-MX",
        version=VERSION,
        test_seed=DEFAULT_TEST_SEED,
        live_trading_capable=False,
    )
