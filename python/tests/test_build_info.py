"""Tests for deterministic and safe Python foundation metadata."""

from dataclasses import FrozenInstanceError

import pytest
from aegis_mx_intelligence import current_build_info


def test_build_info_is_deterministic_and_non_live() -> None:
    """The metadata is stable and cannot advertise live capability."""
    first = current_build_info()
    second = current_build_info()

    assert first == second
    assert first.project == "Aegis-MX"
    assert first.version == "0.1.0"
    assert first.test_seed == 20_260_828
    assert first.live_trading_capable is False


def test_build_info_is_immutable() -> None:
    """A caller cannot mutate foundation safety metadata."""
    with pytest.raises(FrozenInstanceError):
        current_build_info().version = "changed"  # type: ignore[misc]
