"""Compare two artifact trees by deterministic relative path and SHA-256."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path


def tree_manifest(root: Path) -> dict[str, str]:
    """Hash every regular artifact without following symlinks."""
    if not root.is_dir():
        msg = f"artifact directory is unavailable: {root}"
        raise ValueError(msg)
    manifest: dict[str, str] = {}
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            msg = f"artifact tree contains a symlink: {path}"
            raise ValueError(msg)
        if path.is_file():
            relative = path.relative_to(root).as_posix()
            manifest[relative] = hashlib.sha256(path.read_bytes()).hexdigest()
    if not manifest:
        msg = "artifact tree is empty"
        raise ValueError(msg)
    return manifest


def main() -> int:
    """Fail unless both complete artifact manifests are byte-identical."""
    parser = argparse.ArgumentParser()
    parser.add_argument("left", type=Path)
    parser.add_argument("right", type=Path)
    arguments = parser.parse_args()
    left = tree_manifest(arguments.left)
    right = tree_manifest(arguments.right)
    if left != right:
        missing_left = sorted(right.keys() - left.keys())
        missing_right = sorted(left.keys() - right.keys())
        changed = sorted(
            name for name in left.keys() & right.keys() if left[name] != right[name]
        )
        print(
            "FAIL: artifact trees differ: "
            f"missing_left={missing_left}, missing_right={missing_right}, "
            f"changed={changed}"
        )
        return 1
    print(f"PASS: {len(left)} reproducible artifacts matched")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
