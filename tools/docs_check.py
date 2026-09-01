"""Validate local documentation deterministically without network access."""

from __future__ import annotations

import re
from pathlib import Path

REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
DOCS_ROOT = REPOSITORY_ROOT / "docs"
MARKDOWN_LINK = re.compile(r"!?\[[^\]]*\]\(([^)\s]+)(?:\s+[^)]*)?\)")
MERMAID_STARTS = (
    "architecture-beta",
    "block-beta",
    "classDiagram",
    "erDiagram",
    "flowchart",
    "gantt",
    "gitGraph",
    "graph",
    "journey",
    "mindmap",
    "packet-beta",
    "pie",
    "quadrantChart",
    "requirementDiagram",
    "sankey-beta",
    "sequenceDiagram",
    "stateDiagram",
    "timeline",
    "xychart-beta",
)


def documentation_files() -> list[Path]:
    """Return documentation inputs in a stable order."""
    return sorted(
        path
        for path in DOCS_ROOT.rglob("*")
        if path.is_file() and path.suffix in {".md", ".mmd"}
    )


def check_text(path: Path, text: str) -> list[str]:
    """Check whitespace and Markdown fence invariants."""
    errors: list[str] = []
    for line_number, line in enumerate(text.splitlines(), start=1):
        if line.rstrip() != line:
            errors.append(f"{path}:{line_number}: trailing whitespace")
        if "\t" in line:
            errors.append(f"{path}:{line_number}: tab character")
    if (
        path.suffix == ".md"
        and sum(line.startswith("```") for line in text.splitlines()) % 2
    ):
        errors.append(f"{path}: unbalanced fenced code blocks")
    return errors


def check_links(path: Path, text: str) -> tuple[int, list[str]]:
    """Resolve repository-relative Markdown link targets."""
    checked = 0
    errors: list[str] = []
    for match in MARKDOWN_LINK.finditer(text):
        target = match.group(1)
        if target.startswith("#") or re.match(r"^[A-Za-z][A-Za-z0-9+.-]*:", target):
            continue
        target_path = target.split("#", maxsplit=1)[0].split("?", maxsplit=1)[0]
        if not target_path:
            continue
        resolved = (path.parent / target_path).resolve()
        checked += 1
        if not resolved.is_relative_to(REPOSITORY_ROOT):
            errors.append(f"{path}: link escapes repository: {target}")
        elif not resolved.exists():
            errors.append(f"{path}: missing link target: {target}")
    return checked, errors


def mermaid_sources(path: Path, text: str) -> list[str]:
    """Extract raw or fenced Mermaid sources for structural validation."""
    if path.suffix == ".mmd":
        return [text]
    fence = "`" * 3
    pattern = re.compile(
        rf"^{re.escape(fence)}mermaid\n(.*?)\n{re.escape(fence)}$",
        flags=re.MULTILINE | re.DOTALL,
    )
    return [match.group(1) for match in pattern.finditer(text)]


def check_mermaid(path: Path, source: str) -> list[str]:
    """Check basic Mermaid structure; CI may additionally render diagrams."""
    content_lines = [
        line.strip()
        for line in source.splitlines()
        if line.strip() and not line.lstrip().startswith("%%")
    ]
    if not content_lines:
        return [f"{path}: empty Mermaid source"]
    if not content_lines[0].startswith(MERMAID_STARTS):
        return [f"{path}: unsupported or missing Mermaid diagram declaration"]
    return []


def main() -> int:
    """Run all checks and return a process exit status."""
    errors: list[str] = []
    link_count = 0
    mermaid_count = 0
    files = documentation_files()
    for path in files:
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError as unicode_error:
            errors.append(f"{path}: invalid UTF-8: {unicode_error}")
            continue
        errors.extend(check_text(path, text))
        if path.suffix == ".md":
            checked, link_errors = check_links(path, text)
            link_count += checked
            errors.extend(link_errors)
        for source in mermaid_sources(path, text):
            mermaid_count += 1
            errors.extend(check_mermaid(path, source))

    if errors:
        for error in errors:
            print(error)
        return 1
    print(
        f"PASS: {len(files)} documentation files, {link_count} relative links, "
        f"and {mermaid_count} Mermaid sources validated"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
