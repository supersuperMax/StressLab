"""Values between Python and the Command Bus (task 1.8): what a call passes is what the command
sees and what the journal writes, Cyrillic text, paths and numbers included."""

from __future__ import annotations

import math
from pathlib import Path, PurePosixPath
from typing import Any

import numpy as np
import pytest
import stresslab
from support import TEST_OWNER


@pytest.fixture
def echo(doc: stresslab.Document) -> stresslab.Document:
    """A command that returns its argument as the command received it."""
    doc.register_command(
        "test.echo",
        lambda ctx, value=None: value,
        owner=TEST_OWNER,
        args=[stresslab.ArgSpec("value", "any", required=False)],
    )
    return doc


@pytest.mark.parametrize(
    "value",
    [
        None,
        True,
        False,
        0,
        -1500,
        2**63 - 1,
        -(2**63),
        0.1,
        1e-5,
        210000.0,
        5e-324,
        "",
        "Сталь «Ст3»",
        'ГОСТ 380-2005\n"кипящая"\tсталь',
        "😀",
        [],
        [1, 2.5, "три", [None, True]],
        {},
        {"E": 210000.0, "таблица": [[20.0, 1.0]], "вложенный": {"a": {}}},
    ],
)
def test_values_come_back_unchanged(echo: stresslab.Document, value: Any) -> None:
    result = stresslab.cmd("test.echo", value=value)
    assert result == value
    assert type(result) is type(value)


def test_ints_stay_ints_and_floats_stay_floats(echo: stresslab.Document) -> None:
    assert type(stresslab.cmd("test.echo", value=2)) is int
    assert type(stresslab.cmd("test.echo", value=2.0)) is float
    assert type(stresslab.cmd("test.echo", value={"x": [1, 1.0]})["x"][0]) is int


def test_paths_come_back_as_paths(echo: stresslab.Document, tmp_path: Path) -> None:
    path = tmp_path / "Проекты" / "модель 1.dat"
    result = stresslab.cmd("test.echo", value=path)
    assert isinstance(result, Path)
    assert result == path
    # Any os.PathLike is a path; its text is kept.
    assert stresslab.cmd("test.echo", value=PurePosixPath("a/b")) == Path("a/b")


def test_tuples_become_lists(echo: stresslab.Document) -> None:
    assert stresslab.cmd("test.echo", value=(1, (2, 3))) == [1, [2, 3]]


def test_numpy_scalars_are_accepted(echo: stresslab.Document) -> None:
    assert stresslab.cmd("test.echo", value=np.int64(7)) == 7
    assert type(stresslab.cmd("test.echo", value=np.int32(7))) is int
    assert stresslab.cmd("test.echo", value=np.float64(0.5)) == 0.5


def test_objects_are_passed_as_their_ids(echo: stresslab.Document) -> None:
    part_id = stresslab.cmd("model.create", type="part", owner=TEST_OWNER)
    part = echo[part_id]
    assert stresslab.cmd("test.echo", value=part) == part_id
    assert stresslab.cmd("test.echo", value=[part]) == [part_id]


def test_journal_writes_python_literals(echo: stresslab.Document, tmp_path: Path) -> None:
    line = echo.format_call(
        "test.echo", value={"n": 1, "x": 0.1, "t": "«а»\n", "p": Path("D:/Проекты/м 1.dat")}
    )
    assert line == (
        'stresslab.cmd("test.echo", value={"n": 1, "p": Path("D:/Проекты/м 1.dat"), '
        '"t": "«а»\\n", "x": 0.1})'
    )


@pytest.mark.parametrize(
    ("value", "error"),
    [
        ({1, 2}, TypeError),
        (object(), TypeError),
        (b"bytes", TypeError),
        ({1: "int key"}, TypeError),
        (2**64, stresslab.CommandError),
        ("\ud800", UnicodeEncodeError),
    ],
)
def test_values_the_journal_cannot_hold_are_refused(
    echo: stresslab.Document, value: Any, error: type[BaseException]
) -> None:
    with pytest.raises(error):
        stresslab.cmd("test.echo", value=value)
    assert not echo.can_undo


@pytest.mark.parametrize("number", [math.nan, math.inf, -math.inf])
def test_non_finite_numbers_are_refused_with_a_code(
    echo: stresslab.Document, number: float
) -> None:
    with pytest.raises(stresslab.CommandError) as raised:
        stresslab.cmd("test.echo", value=number)
    assert raised.value.code == "invalid_value"
