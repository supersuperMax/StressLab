"""``stresslab.history`` (task 2.3): versions, variants, labels, changes put aside, restoring a
version and one object, the journal of these calls, and a journal that replays into the same
history. Folders have Cyrillic letters and spaces."""

from __future__ import annotations

from collections.abc import Iterator
from datetime import datetime
from pathlib import Path

import pytest
import stresslab
from stresslab import history
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


def thickness(item: int) -> float:
    return stresslab.active_document()[item].properties["thickness"]


def set_thickness(item: int, value: float) -> None:
    stresslab.cmd("model.set_property", object=item, key="thickness", value=value)


def wing(folder: Path) -> tuple[int, int]:
    """The wing of interface-design §12: a material and a skin section that uses it, saved."""
    material = stresslab.cmd(
        "model.create", type="material", owner=TEST_OWNER, name="Д16Т", properties={"E": 72000.0}
    )
    skin = stresslab.cmd(
        "model.create",
        type="section",
        owner=TEST_OWNER,
        name="skin-upper",
        properties={"thickness": 2.0},
        dependencies=[material],
    )
    stresslab.project.save_as(folder / "крыло")
    return material, skin


def test_a_project_without_versions(doc: stresslab.Document, folder: Path) -> None:
    assert history.current_branch() == "main"
    assert history.current_version() is None
    assert history.log() == []
    assert not history.has_changes()
    with pytest.raises(stresslab.ProjectError) as error:
        history.commit("No folder yet")
    assert error.value.code == "no_folder"


def test_the_wing_scenario_without_merging(
    doc: stresslab.Document, folder: Path, journal: list[str]
) -> None:
    material, skin = wing(folder)
    base = history.commit("Обшивка 2 мм")
    assert history.current_version() == base
    assert not stresslab.project.is_modified()

    # A variant with a thicker skin.
    history.branch("Композитная обшивка")
    history.checkout("Композитная обшивка")
    assert history.current_branch() == "Композитная обшивка"
    set_thickness(skin, 3.0)
    stresslab.cmd("model.set_property", object=material, key="E", value=60000.0)
    composite = history.commit("Обшивка 3 мм, КМ-1")
    assert history.branches() == {"main": base, "Композитная обшивка": composite}

    # Back to the main variant, which did not change.
    history.checkout("main")
    assert thickness(skin) == 2.0

    # The material of the variant, brought over as one object.
    history.restore_object(material, "Композитная обшивка")
    assert doc[material].properties == {"E": 60000.0}
    assert history.has_changes()
    stresslab.undo()
    assert doc[material].properties == {"E": 72000.0}
    assert not history.has_changes()

    assert [line for line in journal if "history" in line] == [
        'stresslab.history.commit("Обшивка 2 мм")',
        'stresslab.history.branch("Композитная обшивка")',
        'stresslab.history.checkout("Композитная обшивка")',
        'stresslab.history.commit("Обшивка 3 мм, КМ-1")',
        'stresslab.history.checkout("main")',
        f'stresslab.cmd("history.restore_object", object={material}, '
        'version="Композитная обшивка")',
    ]


def test_comparing_variants_and_restoring_only_materials(
    doc: stresslab.Document, folder: Path, journal: list[str]
) -> None:
    material, skin = wing(folder)
    history.commit("Обшивка 2 мм")
    history.branch("Композитная обшивка")
    history.checkout("Композитная обшивка")
    set_thickness(skin, 3.0)
    stresslab.cmd("model.set_property", object=material, key="E", value=60000.0)
    gust = stresslab.cmd(
        "model.create",
        type="load",
        owner=TEST_OWNER,
        name="gust-2",
        properties={"magnitude": 0.08},
    )
    history.commit("Обшивка 3 мм, КМ-1, порыв")
    history.checkout("main")
    journal.clear()

    # Compare the variants in the terms of the model (task 2.4).
    diff = history.diff("main", "Композитная обшивка")
    assert isinstance(diff, history.Diff)
    assert not diff.empty and diff.units is None
    assert [(change.id, change.kind) for change in diff.objects] == [
        (material, "changed"),
        (skin, "changed"),
        (gust, "added"),
    ]
    change = diff.find(skin)
    assert change is not None and change.type == "section" and change.name == "skin-upper"
    assert change.properties == (history.PropertyChange("thickness", 2.0, 3.0, True, True),)
    assert diff.find(gust).details["properties"] == {"magnitude": 0.08}
    # Outside the application there are no titles of modules: types and properties by their ids.
    assert diff.lines == (
        "material Д16Т: E 72000 → 60000",
        "section skin-upper: thickness 2 → 3",
        "Added: load gust-2",
    )
    assert str(diff) == "; ".join(diff.lines)
    assert history.diff("main").empty

    # Only the materials of the variant.
    assert history.restore("Композитная обшивка", types=["material"]) == [material]
    assert doc[material].properties == {"E": 60000.0}
    assert thickness(skin) == 2.0
    assert gust not in doc
    assert [change.id for change in history.diff("main").objects] == [material]
    stresslab.undo()
    assert history.diff("main").empty

    # The load made in the variant comes over with its id; one object by itself.
    assert history.restore("Композитная обшивка", types=[f"{TEST_OWNER}/load"]) == [gust]
    assert doc[gust].name == "gust-2"
    assert history.restore("Композитная обшивка", objects=[skin]) == [skin]
    assert thickness(skin) == 3.0

    # Comparing is not journaled; restoring is a command.
    assert journal == [
        'stresslab.cmd("history.restore_objects", version="Композитная обшивка", '
        'types=["material"])',
        "stresslab.undo()",
        'stresslab.cmd("history.restore_objects", version="Композитная обшивка", '
        f'types=["{TEST_OWNER}/load"])',
        'stresslab.cmd("history.restore_objects", version="Композитная обшивка", '
        f"objects=[{skin}])",
    ]


def test_restoring_a_part_needs_a_part(doc: stresslab.Document, folder: Path) -> None:
    wing(folder)
    history.commit("Первая")
    with pytest.raises(stresslab.ModelError):
        history.restore("main")
    with pytest.raises(stresslab.ModelError):
        history.restore("main", objects=[999])
    with pytest.raises(stresslab.ProjectError) as error:
        history.diff("нет такой")
    assert error.value.code == "unknown_version"


def test_versions_tell_what_and_when(doc: stresslab.Document, folder: Path) -> None:
    _, skin = wing(folder)
    history.set_author("Инженер", "engineer@example.com")
    first = history.commit("Первая")
    set_thickness(skin, 3.0)
    automatic = history.snapshot("Before solving")
    assert history.snapshot("Before solving") == automatic

    versions = history.log()
    assert [version.id for version in versions] == [automatic, first]
    latest = versions[0]
    assert isinstance(latest, history.Version)
    assert latest.automatic and latest.kind == "automatic"
    assert latest.parents == (first,)
    assert latest.summary == "Before solving"
    assert versions[1].author == "Инженер"
    assert versions[1].email == "engineer@example.com"
    assert isinstance(latest.time, datetime) and latest.time.tzinfo is not None
    assert history.info("main~1") == versions[1]
    assert history.resolve(first[:10]) == first
    assert "model.set_property" in history.journal(automatic)
    assert history.author() == ("Инженер", "engineer@example.com")


def test_labels_and_viewing_a_version(doc: stresslab.Document, folder: Path) -> None:
    _, skin = wing(folder)
    first = history.commit("Первая")
    history.tag("Отправлено на согласование 12.10")
    set_thickness(skin, 3.0)
    history.commit("Вторая")

    history.checkout("Отправлено на согласование 12.10")
    assert history.current_branch() is None
    assert history.current_version() == first
    assert history.tags() == {"Отправлено на согласование 12.10": first}
    set_thickness(skin, 5.0)
    with pytest.raises(stresslab.ProjectError) as error:
        history.commit("Нельзя")
    assert error.value.code == "detached"


def test_changes_put_aside(doc: stresslab.Document, folder: Path) -> None:
    _, skin = wing(folder)
    history.commit("Первая")
    history.branch("другой")
    set_thickness(skin, 4.0)
    with pytest.raises(stresslab.ProjectError) as error:
        history.checkout("другой")
    assert error.value.code == "uncommitted_changes"

    history.stash("Толщина 4")
    assert thickness(skin) == 2.0
    [aside] = history.stashes()
    assert aside.kind == "stash" and aside.message == "Толщина 4" and aside.branch == "main"
    history.checkout("другой")
    history.checkout("main")
    history.stash_pop()
    assert thickness(skin) == 4.0
    assert history.stashes() == []


def test_restoring_a_version_keeps_every_version(doc: stresslab.Document, folder: Path) -> None:
    _, skin = wing(folder)
    first = history.commit("2 мм")
    set_thickness(skin, 3.0)
    history.commit("3 мм")
    set_thickness(skin, 4.0)

    restored = history.restore_version(first)
    assert thickness(skin) == 2.0
    kinds = [version.kind for version in history.log()]
    assert kinds == ["restore", "automatic", "manual", "manual"]
    assert history.info(restored).restored == first


def test_errors_have_codes(doc: stresslab.Document, folder: Path) -> None:
    wing(folder)
    history.commit("Первая")
    for call, code in [
        (lambda: history.checkout("нет такого"), "unknown_version"),
        (lambda: history.commit("Снова"), "nothing_to_commit"),
        (lambda: history.branch("main"), "already_exists"),
        (lambda: history.branch("две\nстроки"), "invalid_name"),
        (lambda: history.stash(), "nothing_to_stash"),
        (lambda: history.stash_pop(), "unknown_version"),
    ]:
        with pytest.raises(stresslab.ProjectError) as error:
            call()
        assert error.value.code == code


def test_the_journal_replays_into_the_same_history(
    doc: stresslab.Document, folder: Path, journal: list[str]
) -> None:
    material, skin = wing(folder)
    history.commit("Первая")
    history.branch("вариант")
    history.checkout("вариант")
    set_thickness(skin, 3.0)
    history.commit("Вторая")
    history.checkout("main")
    history.restore_object(material, "вариант")
    history.restore("main~0", objects=[skin])
    set_thickness(skin, 4.0)
    history.restore("вариант", types=["section"])
    history.tag("метка", "вариант")
    expected = snapshot(doc)
    expected_log = sorted((v.summary, v.kind) for v in history.log(all=True))
    expected_branches = sorted(history.branches())

    other_folder = folder / "повтор"
    lines = [
        (
            f"stresslab.project.save_as({str(other_folder / 'крыло')!r})"
            if line.startswith("stresslab.project.save_as(")
            else line
        )
        for line in journal
    ]
    script = stresslab.journal_header() + "\n".join(lines) + "\n"
    replayed = stresslab.new_document()
    replayed.set_owner_enabled(TEST_OWNER)
    exec(compile(script, "journal.slpy", "exec"), {})  # noqa: S102 - a journal is a script

    assert snapshot(replayed) == expected
    assert sorted((v.summary, v.kind) for v in history.log(all=True)) == expected_log
    assert sorted(history.branches()) == expected_branches
    assert sorted(history.tags()) == ["метка"]
