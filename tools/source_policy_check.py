"""Validate the bounded forecasting POC source-authorization policy."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from collections.abc import Mapping, Sequence
from datetime import UTC, date, datetime
from pathlib import Path
from typing import cast

SCHEMA_VERSION = "1.0.0"
EXPECTED_SCHEMA_REFERENCE = "../../schemas/data-source-policy-v1.schema.json"
EXPECTED_UNIVERSE_PATH = Path("/scratch/djy8hg/xtrade/ticker.txt")
MAX_LIST_ITEMS = 64
EXPECTED_SOURCE_IDS = frozenset(
    {
        "alpaca_iex_historical_bars",
        "fred_alfred_selected_series",
        "gdelt_2_metadata",
        "massive_stocks_minute_aggregates",
        "nasdaq_trader_symbol_directories",
        "sec_edgar_filing_documents",
        "sec_edgar_structured_data",
    }
)
RIGHT_NAMES = frozenset(
    {
        "access",
        "derived_data",
        "model_training",
        "persistent_storage",
        "redistribution",
        "retention_deletion",
    }
)
RIGHT_STATES = frozenset({"DOCUMENTED", "NOT_APPLICABLE", "PROHIBITED", "UNRESOLVED"})
APPROVED_RIGHT_STATES = frozenset({"DOCUMENTED", "NOT_APPLICABLE"})
AUTHORIZATION_STATES = frozenset(
    {"APPROVED", "BLOCKED", "DENIED", "EXPIRED", "REVIEW_REQUIRED"}
)
AUTHENTICATION_METHODS = frozenset(
    {
        "EXTERNAL_API_KEY",
        "EXTERNAL_KEY_SECRET_PAIR",
        "NONE_FOR_PUBLIC_FILES",
        "NONE_USER_AGENT_REQUIRED",
    }
)
USE_CLASSIFICATIONS = frozenset(
    {
        "ACADEMIC",
        "COMMERCIAL",
        "GOVERNMENTAL",
        "PERSONAL",
        "PROFESSIONAL",
        "UNDECLARED",
    }
)
SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")
SOURCE_ID_PATTERN = re.compile(r"^[a-z][a-z0-9_]{2,127}$")
POLICY_ID_PATTERN = re.compile(r"^[a-z0-9][a-z0-9-]{2,127}$")
SENSITIVE_KEYS = frozenset(
    {"api_key", "credential", "password", "private_key", "secret", "token"}
)


class SourcePolicyError(ValueError):
    """Raised when a source policy fails closed validation."""


def _require_mapping(value: object, field: str) -> Mapping[str, object]:
    if not isinstance(value, Mapping):
        raise SourcePolicyError(f"{field} must be an object")
    return cast("Mapping[str, object]", value)


def _require_bool(value: object, field: str) -> bool:
    if not isinstance(value, bool):
        raise SourcePolicyError(f"{field} must be a boolean")
    return value


def _require_string(value: object, field: str) -> str:
    if not isinstance(value, str) or not value:
        raise SourcePolicyError(f"{field} must be a non-empty string")
    return value


def _require_exact_fields(
    value: Mapping[str, object], expected: frozenset[str], field: str
) -> None:
    actual = frozenset(value)
    if actual != expected:
        missing = sorted(expected - actual)
        unexpected = sorted(actual - expected)
        raise SourcePolicyError(
            f"{field} has missing fields {missing} and unexpected fields {unexpected}"
        )


def _validate_string_list(value: object, field: str) -> list[str]:
    if not isinstance(value, list) or not value or len(value) > MAX_LIST_ITEMS:
        raise SourcePolicyError(f"{field} must be a bounded non-empty array")
    strings = [
        _require_string(item, f"{field}[{index}]") for index, item in enumerate(value)
    ]
    if len(strings) != len(set(strings)):
        raise SourcePolicyError(f"{field} entries must be unique")
    return strings


def _reject_sensitive_keys(value: object, location: str = "policy") -> None:
    if isinstance(value, Mapping):
        for raw_key, child in value.items():
            key = str(raw_key)
            if key.lower() in SENSITIVE_KEYS:
                raise SourcePolicyError(f"{location}.{key} may contain a secret")
            _reject_sensitive_keys(child, f"{location}.{key}")
    elif isinstance(value, Sequence) and not isinstance(value, (str, bytes, bytearray)):
        for index, child in enumerate(value):
            _reject_sensitive_keys(child, f"{location}[{index}]")


def _parse_utc(value: object, field: str) -> datetime:
    text = _require_string(value, field)
    try:
        parsed = datetime.strptime(text, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=UTC)
    except ValueError as error:
        raise SourcePolicyError(f"{field} must be a UTC second timestamp") from error
    return parsed


def _validate_universe(value: object) -> None:
    universe = _require_mapping(value, "universe")
    _require_exact_fields(
        universe,
        frozenset({"observed_unique_symbol_count", "source_path", "source_sha256"}),
        "universe",
    )
    source_path = Path(_require_string(universe["source_path"], "universe.source_path"))
    if source_path != EXPECTED_UNIVERSE_PATH or source_path.is_symlink():
        raise SourcePolicyError(
            "universe.source_path must be the canonical ticker file"
        )
    expected_hash = _require_string(universe["source_sha256"], "universe.source_sha256")
    if SHA256_PATTERN.fullmatch(expected_hash) is None:
        raise SourcePolicyError("universe.source_sha256 must be lowercase SHA-256")
    count = universe["observed_unique_symbol_count"]
    if (
        isinstance(count, bool)
        or not isinstance(count, int)
        or not 1 <= count <= 10_000
    ):
        raise SourcePolicyError(
            "universe.observed_unique_symbol_count must be an integer in [1, 10000]"
        )
    try:
        source_bytes = source_path.read_bytes()
    except OSError as error:
        raise SourcePolicyError(f"cannot read universe source: {error}") from error
    actual_hash = hashlib.sha256(source_bytes).hexdigest()
    if actual_hash != expected_hash:
        raise SourcePolicyError("universe source SHA-256 mismatch")
    try:
        lines = source_bytes.decode("utf-8").splitlines()
    except UnicodeDecodeError as error:
        raise SourcePolicyError("universe source is not UTF-8") from error
    symbols = [line for line in lines if line]
    if len(symbols) != len(set(symbols)) or len(symbols) != count:
        raise SourcePolicyError("universe symbol count or uniqueness mismatch")


def _validate_source(value: object, index: int, use_classification: str) -> str:
    field = f"sources[{index}]"
    source = _require_mapping(value, field)
    _require_exact_fields(
        source,
        frozenset(
            {
                "adapter_implementation_authorized",
                "adjustment_revision_semantics",
                "approval_expires_at_utc",
                "approval_record_sha256",
                "authentication_method",
                "authorization_status",
                "dataset",
                "delivery_mechanism",
                "enabled",
                "evidence_urls",
                "history_coverage",
                "market_coverage",
                "network_download_authorized",
                "provider",
                "published_rate_limit",
                "remote",
                "rights",
                "source_id",
                "source_timestamp_semantics",
                "specification_revision",
                "unsupported_scope_risks",
            }
        ),
        field,
    )
    source_id = _require_string(source["source_id"], f"{field}.source_id")
    if SOURCE_ID_PATTERN.fullmatch(source_id) is None:
        raise SourcePolicyError(f"{field}.source_id is malformed")
    _require_string(source["provider"], f"{field}.provider")
    _require_string(source["dataset"], f"{field}.dataset")
    for name in (
        "adjustment_revision_semantics",
        "delivery_mechanism",
        "history_coverage",
        "market_coverage",
        "published_rate_limit",
        "source_timestamp_semantics",
        "specification_revision",
    ):
        _require_string(source[name], f"{field}.{name}")
    _validate_string_list(
        source["unsupported_scope_risks"], f"{field}.unsupported_scope_risks"
    )
    evidence_urls = _validate_string_list(
        source["evidence_urls"], f"{field}.evidence_urls"
    )
    if any(not url.startswith("https://") for url in evidence_urls):
        raise SourcePolicyError(f"{field}.evidence_urls must use HTTPS")
    if _require_bool(source["remote"], f"{field}.remote") is not True:
        raise SourcePolicyError(f"{field}.remote must be true")
    enabled = _require_bool(source["enabled"], f"{field}.enabled")
    adapter = _require_bool(
        source["adapter_implementation_authorized"],
        f"{field}.adapter_implementation_authorized",
    )
    download = _require_bool(
        source["network_download_authorized"],
        f"{field}.network_download_authorized",
    )
    status = _require_string(
        source["authorization_status"], f"{field}.authorization_status"
    )
    if status not in AUTHORIZATION_STATES:
        raise SourcePolicyError(f"{field}.authorization_status is invalid")
    authentication = _require_string(
        source["authentication_method"], f"{field}.authentication_method"
    )
    if authentication not in AUTHENTICATION_METHODS:
        raise SourcePolicyError(f"{field}.authentication_method is invalid")

    rights = _require_mapping(source["rights"], f"{field}.rights")
    _require_exact_fields(rights, RIGHT_NAMES, f"{field}.rights")
    for name, state_value in rights.items():
        state = _require_string(state_value, f"{field}.rights.{name}")
        if state not in RIGHT_STATES:
            raise SourcePolicyError(f"{field}.rights.{name} is invalid")

    approval_hash = source["approval_record_sha256"]
    expiry = source["approval_expires_at_utc"]
    requests_authority = enabled or adapter or download
    if requests_authority and status != "APPROVED":
        raise SourcePolicyError(f"{field} requests authority without approval")
    if status == "APPROVED":
        if use_classification == "UNDECLARED":
            raise SourcePolicyError(f"{field} is approved with undeclared use")
        if (
            not isinstance(approval_hash, str)
            or SHA256_PATTERN.fullmatch(approval_hash) is None
        ):
            raise SourcePolicyError(f"{field} lacks a valid approval record hash")
        approval_expiry = _parse_utc(expiry, f"{field}.approval_expires_at_utc")
        if approval_expiry <= datetime.now(tz=UTC):
            raise SourcePolicyError(f"{field} approval is expired")
        unresolved = sorted(
            name for name, state in rights.items() if state not in APPROVED_RIGHT_STATES
        )
        if unresolved:
            raise SourcePolicyError(
                f"{field} is approved with unresolved rights {unresolved}"
            )
    elif approval_hash is not None or expiry is not None:
        raise SourcePolicyError(f"{field} has metadata without approved status")
    return source_id


def validate_policy(policy: object, *, require_default_disabled: bool = False) -> int:
    """Validate a source policy and return its source count."""
    _reject_sensitive_keys(policy)
    root = _require_mapping(policy, "policy")
    _require_exact_fields(
        root,
        frozenset(
            {
                "$schema",
                "assessment_date",
                "contains_secrets",
                "live_trading_capable",
                "model_training_authorized",
                "network_downloads_authorized",
                "policy_id",
                "policy_mode",
                "remote_sources_default_enabled",
                "schema_version",
                "sources",
                "universe",
                "use_classification",
            }
        ),
        "policy",
    )
    if root["schema_version"] != SCHEMA_VERSION:
        raise SourcePolicyError("unsupported source-policy schema version")
    if root["$schema"] != EXPECTED_SCHEMA_REFERENCE:
        raise SourcePolicyError("source-policy schema reference is invalid")
    if root["policy_mode"] != "DENY_BY_DEFAULT":
        raise SourcePolicyError("policy mode must deny by default")
    if _require_bool(
        root["remote_sources_default_enabled"],
        "remote_sources_default_enabled",
    ):
        raise SourcePolicyError("remote sources must default to disabled")
    if _require_bool(root["contains_secrets"], "contains_secrets"):
        raise SourcePolicyError("source policy must not contain secrets")
    if _require_bool(root["live_trading_capable"], "live_trading_capable"):
        raise SourcePolicyError("source policy must not convey trading capability")
    downloads_authorized = _require_bool(
        root["network_downloads_authorized"], "network_downloads_authorized"
    )
    training_authorized = _require_bool(
        root["model_training_authorized"], "model_training_authorized"
    )
    policy_id = _require_string(root["policy_id"], "policy_id")
    if POLICY_ID_PATTERN.fullmatch(policy_id) is None:
        raise SourcePolicyError("policy_id is malformed")
    assessment_date = _require_string(root["assessment_date"], "assessment_date")
    try:
        date.fromisoformat(assessment_date)
    except ValueError as error:
        raise SourcePolicyError("assessment_date must be an ISO date") from error
    use_classification = _require_string(
        root["use_classification"], "use_classification"
    )
    if use_classification not in USE_CLASSIFICATIONS:
        raise SourcePolicyError("use_classification is invalid")
    _validate_universe(root["universe"])

    source_values = root["sources"]
    if not isinstance(source_values, list) or not source_values:
        raise SourcePolicyError("sources must be a non-empty array")
    source_ids = [
        _validate_source(source, index, use_classification)
        for index, source in enumerate(source_values)
    ]
    if len(source_ids) != len(set(source_ids)):
        raise SourcePolicyError("source IDs must be unique")
    if frozenset(source_ids) != EXPECTED_SOURCE_IDS:
        raise SourcePolicyError(
            "candidate source inventory is incomplete or unexpected"
        )

    source_objects = [cast("Mapping[str, object]", source) for source in source_values]
    if downloads_authorized and not any(
        source["network_download_authorized"] is True for source in source_objects
    ):
        raise SourcePolicyError("global download authority has no approved source")
    if training_authorized and not any(
        source["enabled"] is True
        and cast("Mapping[str, object]", source["rights"])["model_training"]
        in APPROVED_RIGHT_STATES
        for source in source_objects
    ):
        raise SourcePolicyError("global training authority has no approved source")

    if require_default_disabled:
        if downloads_authorized or training_authorized:
            raise SourcePolicyError(
                "example policy must not authorize downloads or training"
            )
        if use_classification != "UNDECLARED":
            raise SourcePolicyError(
                "example policy must not assume a user classification"
            )
        for index, source in enumerate(source_objects):
            if any(
                source[field] is True
                for field in (
                    "adapter_implementation_authorized",
                    "enabled",
                    "network_download_authorized",
                )
            ):
                raise SourcePolicyError(f"sources[{index}] is not default-disabled")
    return len(source_values)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--policy", type=Path, required=True)
    parser.add_argument("--require-default-disabled", action="store_true")
    return parser


def main() -> int:
    """Validate a source policy from disk."""
    arguments = _parser().parse_args()
    try:
        policy = json.loads(arguments.policy.read_text(encoding="utf-8"))
        count = validate_policy(
            policy,
            require_default_disabled=arguments.require_default_disabled,
        )
    except (OSError, json.JSONDecodeError, SourcePolicyError) as error:
        print(f"FAIL: {error}")
        return 1
    root = cast("Mapping[str, object]", policy)
    sources = cast("list[Mapping[str, object]]", root["sources"])
    enabled = sum(source["enabled"] is True for source in sources)
    downloads = root["network_downloads_authorized"] is True
    print(
        "PASS: source policy is valid; "
        f"sources={count}; enabled={enabled}; "
        f"network_downloads_authorized={str(downloads).lower()}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
