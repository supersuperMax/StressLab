"""The product was renamed to StressLab in task 1.2a: old names must not come back.

They stay only where CLAUDE.md ("Name and brand") allows them: the naming table in CLAUDE.md,
the ADRs and the original text of the two design documents.
"""

from __future__ import annotations

import re
import shutil
import subprocess
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]

# Old product name, executable, Python module and file name extensions. Matched as whole words,
# so that base64 data in the brand kit SVGs cannot produce a match by chance.
OLD_NAMES = re.compile(
    r"(?<![a-z0-9])(?:fem[-_]app|femapp|femproj|femx|femext|fempy|fem-extension)(?![a-z0-9])",
    re.IGNORECASE,
)

ALLOWED_FILES = frozenset(
    {
        "docs/architecture.md",
        "docs/interface-design.md",
        "python/tests/test_naming.py",
    }
)
ALLOWED_DIRS = ("docs/adr/",)
NAMING_TABLE_FILE = "CLAUDE.md"
NAMING_TABLE_HEADING = "## Название и бренд"


def find_old_names(text: str) -> list[str]:
    return [match.group(0) for match in OLD_NAMES.finditer(text)]


def lines_outside_naming_section(text: str) -> list[tuple[int, str]]:
    result = []
    inside = False
    for line_number, line in enumerate(text.splitlines(), start=1):
        if line.startswith("## "):
            inside = line.strip() == NAMING_TABLE_HEADING
        if not inside:
            result.append((line_number, line))
    return result


def repository_files() -> list[str]:
    """Tracked files plus new ones that are not ignored, so the check also works before a commit."""
    git = shutil.which("git")
    if git is None or not (REPO_ROOT / ".git").exists():
        pytest.skip("needs a git working copy")
    output = subprocess.run(
        [git, "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
        cwd=REPO_ROOT,
        check=True,
        capture_output=True,
    ).stdout
    return sorted({name for name in output.decode("utf-8").split("\0") if name})


@pytest.mark.parametrize(
    ("text", "expected"),
    [
        ("# FEM-APP", ["FEM-APP"]),
        ("project(fem_app)", ["fem_app"]),
        ('femapp.cmd("calculix.run")', ["femapp"]),
        ("wing.femproj/ and wing.femx", ["femproj", "femx"]),
        ("package.femext, journal.fempy", ["femext", "fempy"]),
        ("fem-extension.toml", ["fem-extension"]),
        ('stresslab.cmd("calculix.run")', []),
        ("femcore, fem::core, fem.results.viewer, FEM_BUILD_GUI", []),
        ("AAAWgmp1femxbWIAAAAe", []),
    ],
)
def test_find_old_names(text: str, expected: list[str]) -> None:
    assert find_old_names(text) == expected


def test_lines_outside_naming_section() -> None:
    text = "# Title\n## Стек\na\n## Название и бренд\nb\n## Стиль\nc\n"
    assert [line for _, line in lines_outside_naming_section(text)] == [
        "# Title",
        "## Стек",
        "a",
        "## Стиль",
        "c",
    ]


def test_old_product_names_are_gone() -> None:
    violations = []
    for name in repository_files():
        if name in ALLOWED_FILES or name.startswith(ALLOWED_DIRS):
            continue
        path = REPO_ROOT / name
        if not path.is_file():
            continue
        data = path.read_bytes()
        if b"\0" in data[:8192]:
            continue  # binary file
        text = data.decode("utf-8", errors="replace")
        if name == NAMING_TABLE_FILE:
            lines = lines_outside_naming_section(text)
        else:
            lines = list(enumerate(text.splitlines(), start=1))
        for line_number, line in lines:
            found = find_old_names(line)
            if found:
                violations.append(f"{name}:{line_number}: {', '.join(found)}")
    report = "\n".join(violations)
    assert not violations, f"Old product names (see CLAUDE.md, '{NAMING_TABLE_HEADING}'):\n{report}"
