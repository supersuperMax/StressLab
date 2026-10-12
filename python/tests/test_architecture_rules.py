"""Checks of architecture rules from CLAUDE.md that can be verified by reading the sources."""

from __future__ import annotations

import re
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]

# Rule 1: the core and the shell know nothing about specific solvers and file formats.
FEM_AGNOSTIC_DIRS = ("core", "shell")
# Product names are unambiguous anywhere in the text; "inp" only as a separate word,
# so that "input" is allowed.
FORBIDDEN_SUBSTRINGS = ("calculix", "abaqus")
FORBIDDEN_WORDS = frozenset({"inp"})

# Splits identifiers into words: "InpReader" -> Inp, Reader; "read_inp" -> read, inp.
_WORD = re.compile(r"[A-Z]+(?![a-z])|[A-Z]?[a-z]+|\d+")


def find_forbidden_words(text: str) -> set[str]:
    lowered = text.lower()
    found = {name for name in FORBIDDEN_SUBSTRINGS if name in lowered}
    found |= {word.lower() for word in _WORD.findall(text)} & FORBIDDEN_WORDS
    return found


@pytest.mark.parametrize(
    ("text", "expected"),
    [
        ("class InpReader;", {"inp"}),
        ("void read_inp_file();", {"inp"}),
        ("open('model.inp')", {"inp"}),
        ("# CalculiX adapter", {"calculix"}),
        ("ABAQUS_HOME", {"abaqus"}),
        ("std::istream& input; // INPUT_FILE", set()),
    ],
)
def test_find_forbidden_words(text: str, expected: set[str]) -> None:
    assert find_forbidden_words(text) == expected


def test_core_and_shell_are_free_of_solver_and_format_names() -> None:
    violations = []
    for directory in FEM_AGNOSTIC_DIRS:
        for path in sorted((REPO_ROOT / directory).rglob("*")):
            if not path.is_file():
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            for line_number, line in enumerate(text.splitlines(), start=1):
                found = find_forbidden_words(line)
                if found:
                    relative = path.relative_to(REPO_ROOT).as_posix()
                    violations.append(f"{relative}:{line_number}: {', '.join(sorted(found))}")
    assert not violations, "Solver or format names in core/shell:\n" + "\n".join(violations)
