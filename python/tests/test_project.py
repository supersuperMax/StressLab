"""``stresslab.project`` (task 2.1): saving the active document to a project folder (.slproj),
opening it again, the journal of these calls and the errors of folders. Folders have Cyrillic
letters and spaces."""

from __future__ import annotations

import json
from collections.abc import Iterator
from pathlib import Path

import pytest
import stresslab
from support import TEST_OWNER, snapshot


@pytest.fixture
def folder(tmp_path: Path) -> Path:
    path = tmp_path / "проекты с пробелами"
    path.mkdir()
    return path


@pytest.fixture
def journal(doc: stresslab.Document) -> Iterator[list[str]]:
    lines: list[str] = []
    with doc.on_journal(lines.append):
        yield lines


def build_model(doc: stresslab.Document) -> None:
    """A mesh with a node set (bulk data, from a command of the test) and a material that
    depends on the mesh."""

    def make_mesh(ctx: stresslab.CommandContext) -> int:
        mesh = stresslab.Mesh(
            [1, 2, 3, 4],
            [0.0, 1.0, 0.0, 0.0],
            [0.0, 0.0, 1.0, 0.0],
            [0.0, 0.0, 0.0, 1.0],
            blocks=[("tet4", [10], [[0, 1, 2, 3]])],
        )
        obj = ctx.create_object("mesh", "fem.core", name="Кронштейн", data=mesh)
        ctx.create_object(
            "node_set", "fem.core", name="опора", parent=obj, data=stresslab.NodeSet([0, 1])
        )
        return obj.id

    if "test.mesh" not in doc.command_ids():
        doc.register_command("test.mesh", make_mesh, owner=TEST_OWNER)
    part = stresslab.cmd("test.mesh")
    stresslab.cmd(
        "model.create",
        type="material",
        owner=TEST_OWNER,
        name="Сталь",
        properties={"E": 210000.0, "nu": 0.3, "count": 2, "note": "строка\nвторая"},
        dependencies=[part],
    )


def test_save_as_then_open_gives_the_same_model(
    doc: stresslab.Document, folder: Path, journal: list[str]
) -> None:
    build_model(doc)
    assert stresslab.project.folder() is None
    assert stresslab.project.is_modified()
    before = snapshot(doc)

    stresslab.project.save_as(folder / "крыло")
    target = folder / "крыло.slproj"
    assert stresslab.project.folder() == target
    assert not stresslab.project.is_modified()
    assert (target / "project.json").is_file()
    assert (target / "refs" / "blobs.json").is_file()
    project = json.loads((target / "project.json").read_text(encoding="utf-8"))
    assert project["format"] == 2
    assert sorted(project["modules"]) == ["fem.core", TEST_OWNER]

    stresslab.cmd("model.create", type="part", owner=TEST_OWNER, name="лишняя")
    assert stresslab.project.is_modified()
    stresslab.project.open(target)
    assert snapshot(doc) == before
    assert not stresslab.project.is_modified()
    assert not doc.can_undo
    assert journal[-1] == f'stresslab.project.open("{target.as_posix()}")'
    assert f'stresslab.project.save_as("{target.as_posix()}")' in journal


def test_save_writes_only_what_changed(doc: stresslab.Document, folder: Path) -> None:
    build_model(doc)
    stresslab.project.save_as(folder / "p")
    files = {path: path.stat().st_mtime_ns for path in (folder / "p.slproj").rglob("*.json")}
    stresslab.project.save()
    assert {path: path.stat().st_mtime_ns for path in files} == files


def test_save_without_a_folder(doc: stresslab.Document) -> None:
    with pytest.raises(stresslab.ProjectError) as error:
        stresslab.project.save()
    assert error.value.code == "no_folder"


def test_errors_name_the_file(doc: stresslab.Document, folder: Path) -> None:
    build_model(doc)
    stresslab.project.save_as(folder / "p")
    broken = folder / "p.slproj" / "model" / "mesh" / "1.json"
    broken.write_text('{\n  "id": 1,\n  oops\n}\n', encoding="utf-8")
    with pytest.raises(stresslab.ProjectError) as error:
        stresslab.project.open(folder / "p.slproj")
    assert error.value.code == "invalid_file"
    assert error.value.file == "model/mesh/1.json"
    assert error.value.line == 3
    assert isinstance(error.value, stresslab.Error)

    with pytest.raises(stresslab.ProjectError) as error:
        stresslab.project.open(folder / "нет такой")
    assert error.value.code == "not_a_project"


def test_refused_inside_a_transaction(doc: stresslab.Document, folder: Path) -> None:
    with stresslab.transaction("half"):
        with pytest.raises(stresslab.CommandError):
            stresslab.project.save_as(folder / "p")


def test_close_gives_an_empty_document(doc: stresslab.Document, folder: Path) -> None:
    build_model(doc)
    stresslab.project.save_as(folder / "p")
    stresslab.project.close()
    assert len(doc) == 0
    assert stresslab.project.folder() is None
    assert not stresslab.project.is_modified()


def test_the_cause_of_the_change_is_load(doc: stresslab.Document, folder: Path) -> None:
    build_model(doc)
    stresslab.project.save_as(folder / "p")
    causes: list[str] = []
    with doc.on_model_changed(lambda change: causes.append(change.cause)):
        stresslab.project.open(folder / "p.slproj")
    assert causes == ["load"]


def test_clean_removes_the_data_of_earlier_saves(
    doc: stresslab.Document, folder: Path, journal: list[str]
) -> None:
    """Task 2.2: the blocks of a mesh saved before go, those of the saved project stay."""
    import numpy as np

    count = 20000
    xs = np.linspace(0.0, 1.0, count)

    def make(ctx: stresslab.CommandContext) -> int:
        mesh = stresslab.Mesh(np.arange(1, count + 1), xs, xs * 2.0, xs * 3.0)
        return ctx.create_object("mesh", "fem.core", name="сетка", data=mesh).id

    def move(ctx: stresslab.CommandContext) -> None:
        moved = xs.copy()
        moved[:100] += 0.5
        ctx.set_data(mesh, stresslab.Mesh(np.arange(1, count + 1), moved, xs * 2.0, xs * 3.0))

    doc.register_command("test.big_mesh", make, owner=TEST_OWNER)
    doc.register_command("test.move_nodes", move, owner=TEST_OWNER)
    mesh = stresslab.cmd("test.big_mesh")
    stresslab.project.save_as(folder / "p")
    stresslab.cmd("test.move_nodes")
    stresslab.project.save()
    journal.clear()

    result = stresslab.project.clean()
    assert set(result) == {"removed", "freed_bytes", "kept", "kept_bytes"}
    assert result["removed"] > 0
    assert result["freed_bytes"] > 0
    assert result["kept"] > 0
    assert journal == ["stresslab.project.clean()"]
    assert stresslab.project.clean()["removed"] == 0

    before = snapshot(doc)
    stresslab.project.open(folder / "p.slproj")
    assert snapshot(doc) == before
    x = doc.objects()[0].data.x
    assert x[0] == 0.5 and x[100] == xs[100]


def test_clean_without_a_folder(doc: stresslab.Document) -> None:
    with pytest.raises(stresslab.ProjectError) as error:
        stresslab.project.clean()
    assert error.value.code == "no_folder"
