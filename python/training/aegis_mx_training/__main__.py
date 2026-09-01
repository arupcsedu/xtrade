"""Command-line export for deterministic microstructure artifacts."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from aegis_mx_training.microstructure import TrainingConfig, export_suite, load_dataset


def main() -> int:
    """Train seven models and emit machine-readable infrastructure evidence."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--dataset", required=True, type=Path)
    parser.add_argument("--output-directory", required=True, type=Path)
    parser.add_argument("--seed", required=True, type=int)
    parser.add_argument("--expires-wall-clock-utc-ns", required=True, type=int)
    arguments = parser.parse_args()
    dataset = load_dataset(arguments.dataset)
    reports = export_suite(
        dataset,
        arguments.output_directory,
        TrainingConfig(
            seed=arguments.seed,
            expires_wall_clock_utc_ns=arguments.expires_wall_clock_utc_ns,
        ),
    )
    print(  # noqa: T201
        json.dumps(
            {
                "dataset_sha256": dataset.sha256,
                "economic_value_claim": False,
                "reports": [
                    {
                        "artifact_sha256": report.artifact_sha256,
                        "final_loss_ppm": report.final_loss_ppm,
                        "initial_loss_ppm": report.initial_loss_ppm,
                        "role": report.role,
                        "sample_count": report.sample_count,
                        "seed": report.seed,
                    }
                    for report in reports
                ],
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":  # pragma: no cover - exercised as an integration command.
    raise SystemExit(main())
