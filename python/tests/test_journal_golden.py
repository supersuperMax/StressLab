"""The session journal is a Python script with the values the core meant (task 1.3).

tests/data/journal/golden.slpy is written by the C++ Command Bus (the C++ test
Journal.GoldenSessionMatchesTheFile checks it byte for byte). Here real Python runs it
against a stand-in `stresslab` module and the recorded calls are compared with the
values the C++ session passed: Cyrillic text, escapes, numbers and paths must mean the same
to Python as to the core.
"""

from __future__ import annotations

import ast
import sys
import types
from collections.abc import Iterator
from contextlib import contextmanager
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]
GOLDEN = REPO_ROOT / "tests" / "data" / "journal" / "golden.slpy"

EXPECTED_CALLS: list[tuple[Any, ...]] = [
    (
        "cmd",
        "model.set_units",
        {"length": "mm", "mass": "t", "time": "s", "temperature": "degC"},
    ),
    (
        "cmd",
        "model.create",
        {
            "type": "material",
            "owner": "test.module",
            "name": "Сталь «Ст3»",
            "properties": {
                "E": 210000.0,
                "alpha": 1.2e-5,
                "note": 'ГОСТ 380-2005\n"кипящая"\tсталь',
                "nu": 0.3,
                "rho": 7.85e-9,
                "table": [[20.0, 210000.0], [400.0, 170000.0]],
            },
        },
    ),
    ("cmd", "model.create", {"type": "part", "owner": "test.module", "name": "Кронштейн"}),
    (
        "cmd",
        "model.create",
        {
            "type": "load",
            "owner": "test.module",
            "name": "P1",
            "parent": 2,
            "dependencies": [1],
            "properties": {"direction": [0.0, 0.0, -1.0], "value": -1500},
        },
    ),
    ("cmd", "model.rename", {"object": 3, "name": "Сила 1"}),
    ("undo",),
    ("redo",),
    ("begin", "Нагрузки"),
    ("cmd", "model.create", {"type": "load", "owner": "test.module", "name": "p", "parent": 2}),
    ("cmd", "model.set_properties", {"object": 4, "properties": {"empty": {}, "value": 0.5}}),
    ("cmd", "model.add_dependency", {"object": 4, "target": 1}),
    ("end", "Нагрузки"),
    (
        "cmd",
        "test.load",
        {"file": Path("D:/Проекты/модель 1.dat"), "options": {"scale": 2}},
    ),
    ("cmd", "model.remove", {"object": 3}),
    ("cmd", "model.remove_dependency", {"object": 4, "target": 1}),
]


def run_journal(text: str) -> list[tuple[Any, ...]]:
    calls: list[tuple[Any, ...]] = []

    def cmd(command: str, /, **kwargs: Any) -> None:
        calls.append(("cmd", command, kwargs))

    @contextmanager
    def transaction(label: str = "") -> Iterator[None]:
        calls.append(("begin", label))
        yield
        calls.append(("end", label))

    fake = types.ModuleType("stresslab")
    fake.cmd = cmd
    fake.undo = lambda: calls.append(("undo",))
    fake.redo = lambda: calls.append(("redo",))
    fake.transaction = transaction

    saved = sys.modules.get("stresslab")
    sys.modules["stresslab"] = fake
    try:
        exec(compile(text, str(GOLDEN), "exec"), {"__name__": "__journal__"})
    finally:
        if saved is None:
            del sys.modules["stresslab"]
        else:
            sys.modules["stresslab"] = saved
    return calls


def test_golden_journal_is_utf8_with_lf_and_valid_python() -> None:
    data = GOLDEN.read_bytes()
    assert not data.startswith(b"\xef\xbb\xbf"), "no BOM"
    assert b"\r" not in data
    assert data.endswith(b"\n")
    ast.parse(data.decode("utf-8"), filename=str(GOLDEN))


def test_golden_journal_means_what_the_core_meant() -> None:
    calls = run_journal(GOLDEN.read_text(encoding="utf-8"))
    assert calls == EXPECTED_CALLS
    # Types matter too: 210000.0 is a float, -1500 and 2 are ints.
    properties = calls[1][2]["properties"]
    assert type(properties["E"]) is float
    assert type(calls[3][2]["properties"]["value"]) is int
    assert type(calls[12][2]["options"]["scale"]) is int


def test_numbers_are_written_as_python_repr() -> None:
    # Every float literal in the journal is spelled exactly as Python's repr() spells it.
    tree = ast.parse(GOLDEN.read_text(encoding="utf-8"))
    source = GOLDEN.read_text(encoding="utf-8")
    floats = [
        node
        for node in ast.walk(tree)
        if isinstance(node, ast.Constant) and isinstance(node.value, float)
    ]
    assert floats
    for node in floats:
        text = ast.get_source_segment(source, node)
        assert text == repr(node.value)
