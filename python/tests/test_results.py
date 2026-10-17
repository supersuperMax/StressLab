"""``stresslab.results`` and packages (task 2.5): the cache of results of a project, pinned results
kept in the history, ``stresslab.project.pack`` and ``unpack``, the journal of these calls and a
package that other zip tools read. Folders have Cyrillic letters and spaces."""

from __future__ import annotations

import zipfile
from collections.abc import Iterator
from pathlib import Path

import pytest
import stresslab
from stresslab import history, results
from support import TEST_OWNER, snapshot


@pytest.fixture
def journal(doc: stresslab.Document) -> Iterator[list[str]]:
    lines: list[str] = []
    with doc.on_journal(lines.append):
        yield lines


@pytest.fixture
def folder(tmp_path: Path) -> Path:
    path = tmp_path / "проекты с пробелами"
    path.mkdir()
    return path


def wing(folder: Path) -> int:
    """A skin section of thickness 2, saved and committed."""
    skin = stresslab.cmd(
        "model.create",
        type="section",
        owner=TEST_OWNER,
        name="skin-upper",
        properties={"thickness": 2.0},
    )
    stresslab.project.save_as(folder / "крыло")
    history.commit("Обшивка 2 мм")
    return skin


def solver_output(folder: Path, stresses: str) -> Path:
    out = folder / "расчёт"
    (out / "log").mkdir(parents=True, exist_ok=True)
    (out / "напряжения.frd").write_bytes(stresses.encode())
    (out / "log" / "solver.dat").write_bytes(b" STEP 1\n")
    return out


def test_put_find_and_pin(doc: stresslab.Document, folder: Path, journal: list[str]) -> None:
    wing(folder)
    version = history.snapshot("Before solving")
    key = results.key("calculix", "2.22")
    assert len(key) == 64
    assert results.key("calculix", "2.22", version="main") == key
    assert results.key("calculix", "2.23") != key
    assert results.key("calculix", "2.22", inputs={"threads": "4"}) != key
    assert results.find(key) is None

    put = results.put(key, solver_output(folder, "1 2 3\n"), version, "calculix", "2.22")
    assert put.version == version
    assert (put.solver, put.solver_version) == ("calculix", "2.22")
    assert [file.path for file in put.files] == ["log/solver.dat", "напряжения.frd"]
    assert put.bytes == 6 + 8
    assert put.cached and not put.pinned
    assert put.folder is not None
    assert (put.folder / "напряжения.frd").read_bytes() == b"1 2 3\n"
    assert results.usage() == put.bytes
    assert [result.key for result in results.list()] == [key]

    journal.clear()
    results.pin(key)
    assert results.remove(key)
    assert journal == [f'stresslab.results.pin("{key}")', f'stresslab.results.remove("{key}")']
    listed = results.list()
    assert len(listed) == 1 and listed[0].pinned and not listed[0].cached
    found = results.find(key)
    assert found is not None and found.pinned and found.cached
    assert found.folder is not None
    assert (found.folder / "напряжения.frd").read_bytes() == b"1 2 3\n"

    results.unpin(key)
    assert not results.find(key).pinned  # type: ignore[union-attr]
    with pytest.raises(stresslab.ProjectError) as error:
        results.unpin(key)
    assert error.value.code == "unknown_result"


def test_a_result_belongs_to_the_model_of_its_version(
    doc: stresslab.Document, folder: Path
) -> None:
    skin = wing(folder)
    version = history.current_version()
    assert version is not None
    stresslab.cmd("model.set_property", object=skin, key="thickness", value=3.0)
    with pytest.raises(stresslab.ProjectError) as error:
        results.put(results.key("calculix"), solver_output(folder, "x"), version, "calculix")
    assert error.value.code == "result_mismatch"
    with pytest.raises(stresslab.ProjectError) as error:
        results.pin("0" * 64)
    assert error.value.code == "unknown_result"


def test_without_a_folder(doc: stresslab.Document) -> None:
    assert results.list() == ()
    with pytest.raises(stresslab.ProjectError) as error:
        results.prune_inactive()
    assert error.value.code == "no_folder"


def test_the_limit_and_inactive_variants(doc: stresslab.Document, folder: Path) -> None:
    skin = wing(folder)
    base = results.put(
        results.key("calculix"),
        solver_output(folder, "base"),
        history.current_version(),
        "calculix",
    )
    history.branch("Композитная обшивка")
    history.checkout("Композитная обшивка")
    stresslab.cmd("model.set_property", object=skin, key="thickness", value=3.0)
    version = history.commit("Обшивка 3 мм")
    composite = results.put(
        results.key("calculix"), solver_output(folder, "3 mm"), version, "calculix"
    )
    history.checkout("main")
    assert results.prune_inactive() == {"removed": 1, "freed_bytes": composite.bytes}
    assert [result.key for result in results.list()] == [base.key]

    assert results.limit() == 20480 * 1024 * 1024
    results.set_limit(1)
    assert results.limit() == 1


def test_pack_and_unpack(doc: stresslab.Document, folder: Path, journal: list[str]) -> None:
    skin = wing(folder)
    key = results.key("calculix")
    results.put(key, solver_output(folder, "pinned"), history.current_version(), "calculix")
    results.pin(key)
    stresslab.cmd("model.set_property", object=skin, key="thickness", value=2.5)
    expected = snapshot(doc)
    expected_log = [version.summary for version in history.log()]

    journal.clear()
    package = folder / "для коллеги"
    packed = stresslab.project.pack(package)
    model = stresslab.project.pack(folder / "модель.slpack", history=False)
    assert packed["files"] > model["files"]
    assert (folder / "для коллеги.slpack").stat().st_size == packed["package_bytes"]
    assert journal == [
        f'stresslab.project.pack("{(folder / "для коллеги.slpack").as_posix()}", history=True)',
        f'stresslab.project.pack("{(folder / "модель.slpack").as_posix()}", history=False)',
    ]

    # Any zip tool reads it, with the names in UTF-8.
    with zipfile.ZipFile(folder / "для коллеги.slpack") as archive:
        assert archive.testzip() is None
        names = archive.namelist()
    assert "project.json" in names
    assert any(name.startswith(".fem/git/refs/results/") for name in names)
    assert not any(name.startswith(".fem/cache") for name in names)

    other = stresslab.new_document()
    other.set_owner_enabled(TEST_OWNER)
    stresslab.project.unpack(folder / "для коллеги.slpack", folder / "у коллеги")
    assert stresslab.project.folder() == folder / "у коллеги.slproj"
    assert snapshot(other) == expected
    assert [version.summary for version in history.log()] == expected_log
    assert history.has_changes()
    found = results.find(key)
    assert found is not None and found.pinned
    assert (found.folder / "напряжения.frd").read_bytes() == b"pinned"  # type: ignore[operator]

    with pytest.raises(stresslab.ProjectError) as error:
        stresslab.project.unpack(folder / "для коллеги.slpack", folder / "у коллеги")
    assert error.value.code == "not_a_project"
    (folder / "не пакет.slpack").write_bytes(b"not a zip")
    with pytest.raises(stresslab.ProjectError) as error:
        stresslab.project.unpack(folder / "не пакет.slpack", folder / "куда-то")
    assert error.value.code == "invalid_archive"


def test_the_journal_replays_the_unpacking(doc: stresslab.Document, folder: Path) -> None:
    wing(folder)
    stresslab.project.pack(folder / "крыло")
    expected = snapshot(doc)
    lines: list[str] = []
    replayed = stresslab.new_document()
    replayed.set_owner_enabled(TEST_OWNER)
    with replayed.on_journal(lines.append):
        stresslab.project.unpack(folder / "крыло.slpack", folder / "первый")
    assert lines == [
        f'stresslab.project.unpack("{(folder / "крыло.slpack").as_posix()}", '
        f'"{(folder / "первый.slproj").as_posix()}")'
    ]

    again = stresslab.new_document()
    again.set_owner_enabled(TEST_OWNER)
    script = stresslab.journal_header() + lines[0].replace("первый", "второй") + "\n"
    exec(compile(script, "journal.slpy", "exec"), {})  # noqa: S102 - a journal is a script
    assert snapshot(again) == expected
    assert stresslab.project.folder() == folder / "второй.slproj"
