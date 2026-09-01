"""Reproduce or verify generated FlatBuffers bindings."""

from __future__ import annotations

import argparse
import filecmp
import shutil
import subprocess
import tempfile
from pathlib import Path

REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
EXPECTED_FLATC_VERSION = "flatc version 25.12.19"
CPP_OUTPUT = REPOSITORY_ROOT / "schemas/generated/cpp/aegis/mx/contracts/v1"
PYTHON_OUTPUT = REPOSITORY_ROOT / "python/intelligence/aegis"
SCHEMAS = [
    "schemas/aegis_mx/v1/common.fbs",
    "schemas/aegis_mx/v1/records.fbs",
    "schemas/aegis_mx/v1/contract_record.fbs",
    "schemas/aegis_mx/v1/audit_envelope.fbs",
]
COMPATIBILITY_SCHEMAS = [
    "schemas/tests/compatibility_v1.fbs",
    "schemas/tests/compatibility_v1_1.fbs",
    "schemas/tests/compatibility_v1_2.fbs",
    "schemas/tests/compatibility_v1_3.fbs",
    "schemas/tests/compatibility_v1_4.fbs",
    "schemas/tests/compatibility_v1_5.fbs",
    "schemas/tests/compatibility_v1_6.fbs",
    "schemas/tests/compatibility_v1_7.fbs",
    "schemas/tests/compatibility_v1_8.fbs",
]


def _run(command: list[str]) -> None:
    subprocess.run(command, cwd=REPOSITORY_ROOT, check=True)


def _files(directory: Path) -> dict[Path, bytes]:
    return {
        path.relative_to(directory): path.read_bytes()
        for path in directory.rglob("*")
        if path.is_file()
        and "__pycache__" not in path.parts
        and path.suffix not in {".pyc", ".pyo"}
    }


def _check_equal(actual: Path, expected: Path) -> list[str]:
    actual_files = _files(actual) if actual.exists() else {}
    expected_files = _files(expected) if expected.exists() else {}
    drift = sorted(set(actual_files) ^ set(expected_files))
    drift.extend(
        path
        for path in sorted(set(actual_files) & set(expected_files))
        if not filecmp.cmp(actual / path, expected / path, shallow=False)
    )
    return [str(path) for path in drift]


def main() -> int:
    """Generate into a temporary directory, then compare or replace outputs."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--flatc", required=True, type=Path)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--check", action="store_true")
    action.add_argument("--write", action="store_true")
    args = parser.parse_args()

    version = subprocess.run(
        [str(args.flatc), "--version"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()
    if version != EXPECTED_FLATC_VERSION:
        parser.error(
            f"expected {EXPECTED_FLATC_VERSION!r}, received {version!r}",
        )

    _run(
        [
            str(args.flatc),
            "--conform",
            COMPATIBILITY_SCHEMAS[0],
            COMPATIBILITY_SCHEMAS[1],
        ]
    )
    _run(
        [
            str(args.flatc),
            "--conform",
            COMPATIBILITY_SCHEMAS[1],
            COMPATIBILITY_SCHEMAS[2],
        ]
    )
    _run(
        [
            str(args.flatc),
            "--conform",
            COMPATIBILITY_SCHEMAS[2],
            COMPATIBILITY_SCHEMAS[3],
        ]
    )
    _run(
        [
            str(args.flatc),
            "--conform",
            COMPATIBILITY_SCHEMAS[3],
            COMPATIBILITY_SCHEMAS[4],
        ]
    )
    _run(
        [
            str(args.flatc),
            "--conform",
            COMPATIBILITY_SCHEMAS[4],
            COMPATIBILITY_SCHEMAS[5],
        ]
    )
    _run(
        [
            str(args.flatc),
            "--conform",
            COMPATIBILITY_SCHEMAS[5],
            COMPATIBILITY_SCHEMAS[6],
        ]
    )
    _run(
        [
            str(args.flatc),
            "--conform",
            COMPATIBILITY_SCHEMAS[6],
            COMPATIBILITY_SCHEMAS[7],
        ]
    )
    _run(
        [
            str(args.flatc),
            "--conform",
            COMPATIBILITY_SCHEMAS[7],
            COMPATIBILITY_SCHEMAS[8],
        ]
    )
    print(
        "FlatBuffers compatibility conformance passed: "
        "v1 -> v1.1 -> v1.2 -> v1.3 -> v1.4 -> v1.5 -> v1.6 -> v1.7 -> v1.8"
    )

    with tempfile.TemporaryDirectory(prefix="aegis-schema-codegen-") as temp:
        temp_root = Path(temp)
        cpp_output = temp_root / "cpp"
        python_output = temp_root / "python"
        cpp_output.mkdir()
        python_output.mkdir()
        _run(
            [
                str(args.flatc),
                "--cpp",
                "--scoped-enums",
                "--gen-compare",
                "--cpp-std",
                "c++17",
                "-I",
                "schemas",
                "-o",
                str(cpp_output),
                *SCHEMAS,
                *COMPATIBILITY_SCHEMAS,
            ]
        )
        _run(
            [
                str(args.flatc),
                "--python",
                "--python-typing",
                "--python-version",
                "3.12",
                "-I",
                "schemas",
                "-o",
                str(python_output),
                *SCHEMAS,
            ]
        )

        generated_cpp = cpp_output
        generated_python = python_output / "aegis"
        if args.check:
            drift = [
                *(f"cpp/{item}" for item in _check_equal(generated_cpp, CPP_OUTPUT)),
                *(
                    f"python/{item}"
                    for item in _check_equal(generated_python, PYTHON_OUTPUT)
                ),
            ]
            if drift:
                parser.error("generated schema drift: " + ", ".join(drift))
            print(
                "Generated schema bindings are current: "
                f"{len(_files(CPP_OUTPUT)) + len(_files(PYTHON_OUTPUT))} files"
            )
            return 0

        shutil.rmtree(CPP_OUTPUT, ignore_errors=True)
        shutil.rmtree(PYTHON_OUTPUT, ignore_errors=True)
        CPP_OUTPUT.parent.mkdir(parents=True, exist_ok=True)
        PYTHON_OUTPUT.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(generated_cpp, CPP_OUTPUT)
        shutil.copytree(generated_python, PYTHON_OUTPUT)
        print(
            "Generated schema bindings: "
            f"{len(_files(CPP_OUTPUT)) + len(_files(PYTHON_OUTPUT))} files"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
