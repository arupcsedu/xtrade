"""Tests for the deny-by-default forecasting POC source policy."""

from __future__ import annotations

import json
from pathlib import Path
from typing import cast

import pytest

from tools.source_policy_check import SourcePolicyError, validate_policy

REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
POLICY_PATH = REPOSITORY_ROOT / "infra/data_poc/source-policy.example.json"
SCHEMA_PATH = REPOSITORY_ROOT / "schemas/data-source-policy-v1.schema.json"


def _policy() -> dict[str, object]:
    return cast(
        "dict[str, object]", json.loads(POLICY_PATH.read_text(encoding="utf-8"))
    )


def _sources(policy: dict[str, object]) -> list[dict[str, object]]:
    return cast("list[dict[str, object]]", policy["sources"])


def test_example_policy_is_complete_secret_free_and_default_disabled() -> None:
    policy = _policy()

    assert validate_policy(policy, require_default_disabled=True) == 7
    assert policy["network_downloads_authorized"] is False
    assert policy["model_training_authorized"] is False
    assert all(source["enabled"] is False for source in _sources(policy))


@pytest.mark.parametrize(
    "authority_field",
    ["enabled", "adapter_implementation_authorized", "network_download_authorized"],
)
def test_unapproved_source_authority_is_rejected(authority_field: str) -> None:
    policy = _policy()
    _sources(policy)[0][authority_field] = True

    with pytest.raises(SourcePolicyError, match="without approval"):
        validate_policy(policy)


def test_remote_enabled_default_is_rejected() -> None:
    policy = _policy()
    policy["remote_sources_default_enabled"] = True

    with pytest.raises(SourcePolicyError, match="default to disabled"):
        validate_policy(policy)


def test_unknown_or_secret_bearing_fields_are_rejected() -> None:
    policy = _policy()
    _sources(policy)[0]["api_key"] = "not-a-real-key"  # pragma: allowlist secret

    with pytest.raises(SourcePolicyError, match="may contain a secret"):
        validate_policy(policy)


def test_universe_hash_mismatch_is_rejected() -> None:
    policy = _policy()
    universe = cast("dict[str, object]", policy["universe"])
    universe["source_sha256"] = "0" * 64

    with pytest.raises(SourcePolicyError, match="SHA-256 mismatch"):
        validate_policy(policy)


def test_noncanonical_universe_path_is_rejected() -> None:
    policy = _policy()
    universe = cast("dict[str, object]", policy["universe"])
    universe["source_path"] = "/noncanonical/ticker.txt"

    with pytest.raises(SourcePolicyError, match="canonical ticker file"):
        validate_policy(policy)


def test_json_schema_encodes_fail_closed_policy_invariants() -> None:
    schema = cast(
        "dict[str, object]", json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))
    )
    properties = cast("dict[str, object]", schema["properties"])
    assert (
        cast("dict[str, object]", properties["remote_sources_default_enabled"])["const"]
        is False
    )
    assert cast("dict[str, object]", properties["contains_secrets"])["const"] is False
    assert (
        cast("dict[str, object]", properties["live_trading_capable"])["const"] is False
    )
    source_definition = cast(
        "dict[str, object]", cast("dict[str, object]", schema["$defs"])["source"]
    )
    required_source_fields = cast("list[str]", source_definition["required"])
    assert {
        "adjustment_revision_semantics",
        "evidence_urls",
        "history_coverage",
        "market_coverage",
        "published_rate_limit",
        "source_timestamp_semantics",
        "unsupported_scope_risks",
    } <= set(required_source_fields)
