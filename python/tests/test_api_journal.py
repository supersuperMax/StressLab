"""The session journal is a Python script (task 1.8): run with `runpy` against a new document, a
journal recorded in Python and the golden journal of the core rebuild the same document and are
journaled again line for line."""

from __future__ import annotations

import runpy
from collections.abc import Iterator
from pathlib import Path

import pytest
import stresslab
from support import TEST_OWNER, new_session, snapshot

REPO_ROOT = Path(__file__).resolve().parents[2]
GOLDEN = REPO_ROOT / "tests" / "data" / "journal" / "golden.slpy"


class Recorder:
    """Journal entries of a document, as the text of a .slpy file."""

    def __init__(self, document: stresslab.Document) -> None:
        self.entries: list[str] = []
        self._subscription = document.on_journal(self.entries.append)

    def file(self) -> str:
        return stresslab.journal_header() + "".join(entry + "\n" for entry in self.entries)


@pytest.fixture
def active() -> Iterator[None]:
    previous = stresslab.active_document()
    try:
        yield
    finally:
        stresslab.set_active_document(previous)


def run_journal(path: Path) -> tuple[stresslab.Document, Recorder]:
    """Runs a journal file as a script against a new document."""
    document = new_session()
    recorder = Recorder(document)
    stresslab.set_active_document(document)
    runpy.run_path(str(path), run_name="__main__")
    return document, recorder


def record_session() -> tuple[stresslab.Document, Recorder]:
    """A session with Cyrillic text, paths, nested values, undo, redo and transactions, written
    with both `cmd` and the typed wrappers."""
    document = new_session()
    recorder = Recorder(document)
    stresslab.set_active_document(document)
    stresslab.model.set_units(length="mm", mass="t", time="s", temperature="degC")
    steel = stresslab.cmd(
        "model.create",
        type="material",
        owner=TEST_OWNER,
        name="Сталь «Ст3»",
        properties={"E": 210000.0, "nu": 0.3, "note": 'ГОСТ\n"кипящая"', "t": [[20.0, 1.5e-7]]},
    )
    part = stresslab.model.create(type="part", owner=TEST_OWNER, name="Кронштейн")
    load = stresslab.model.create(
        type="load", owner=TEST_OWNER, name="P1", parent=part, dependencies=[steel]
    )
    stresslab.model.rename(object=load, name="Сила 1")
    stresslab.undo()
    stresslab.redo()
    with stresslab.transaction("Нагрузки"):
        pressure = stresslab.cmd("model.create", type="load", owner=TEST_OWNER, parent=part)
        stresslab.model.set_properties(object=pressure, properties={"value": 0.5, "empty": {}})
    with pytest.raises(ZeroDivisionError), stresslab.transaction("отменено"):
        stresslab.cmd("model.create", type="load", owner=TEST_OWNER, name="lost")
        _ = 1 / 0
    stresslab.test.load(file=Path("D:/Проекты/модель 1.dat"), options={"scale": 2})
    stresslab.cmd("model.remove", object=load)
    stresslab.undo()
    return document, recorder


def test_a_journal_recorded_in_python_runs_as_a_script(active: None, tmp_path: Path) -> None:
    original, recorded = record_session()
    journal = recorded.file()
    assert "stresslab.undo()" in journal and "with stresslab.transaction(" in journal
    assert "отменено" not in journal
    file = tmp_path / "журнал с пробелами" / "сессия 1.slpy"
    file.parent.mkdir()
    file.write_text(journal, encoding="utf-8", newline="\n")

    replayed, rerecorded = run_journal(file)
    assert snapshot(replayed) == snapshot(original)
    assert rerecorded.file() == journal
    # The history replays too.
    while original.can_undo:
        assert replayed.undo()
        original.undo()
        assert snapshot(replayed) == snapshot(original)
    assert not replayed.can_undo


def test_the_golden_journal_runs_as_a_script(active: None) -> None:
    golden = GOLDEN.read_text(encoding="utf-8")
    replayed, rerecorded = run_journal(GOLDEN)
    assert rerecorded.file() == golden
    names = [item.name for item in replayed.objects()]
    assert names == ["Сталь «Ст3»", "Кронштейн", "p", "модель 1"]
    steel = replayed.objects()[0]
    assert steel.properties["note"] == 'ГОСТ 380-2005\n"кипящая"\tсталь'
    assert replayed.units["length"] == "mm"
