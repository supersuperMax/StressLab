"""Documents and objects from Python (task 1.8): reading the model, commands, history,
transactions, events and errors."""

from __future__ import annotations

import sys

import pytest
import stresslab
from support import TEST_OWNER


@pytest.fixture
def model(doc: stresslab.Document) -> dict[str, stresslab.Object]:
    """steel <- part/load (load depends on steel)."""
    steel = stresslab.cmd(
        "model.create",
        type="material",
        owner=TEST_OWNER,
        name="Сталь",
        properties={"E": 210000.0, "nu": 0.3, "note": "ГОСТ"},
    )
    part = stresslab.cmd("model.create", type="part", owner=TEST_OWNER, name="Кронштейн")
    load = stresslab.cmd(
        "model.create",
        type="load",
        owner=TEST_OWNER,
        name="P1",
        parent=part,
        dependencies=[steel],
        properties={"value": -1500},
    )
    return {"steel": doc[steel], "part": doc[part], "load": doc[load]}


def test_objects_read_the_model(
    doc: stresslab.Document, model: dict[str, stresslab.Object]
) -> None:
    steel, part, load = model["steel"], model["part"], model["load"]
    assert len(doc) == 3
    assert [o.id for o in doc.objects()] == [steel.id, part.id, load.id]
    assert doc.roots() == [steel, part]
    assert steel.type == "material"
    assert steel.owner == TEST_OWNER
    assert steel.schema_version == 1
    assert steel.name == "Сталь"
    assert steel.properties == {"E": 210000.0, "nu": 0.3, "note": "ГОСТ"}
    assert load.parent == part
    assert part.children == [load]
    assert part.parent is None
    assert load.dependencies == [steel]
    assert steel.dependents == [load]
    assert steel.data is None
    assert not steel.is_opaque
    assert load.document == doc
    assert repr(steel) == f"<stresslab.Object #{steel.id} material 'Сталь'>"


def test_lookup(doc: stresslab.Document, model: dict[str, stresslab.Object]) -> None:
    steel, part, load = model["steel"], model["part"], model["load"]
    assert doc[steel.id] == steel
    assert steel.id in doc and steel in doc
    assert 999 not in doc
    with pytest.raises(KeyError):
        doc[999]
    assert doc.find(999) is None
    assert doc.find(steel.id) == steel
    assert doc.find_by_type("load") == [load]
    assert doc.find_by_owner(TEST_OWNER) == [steel, part, load]
    assert doc.find_child(part, "load", "P1") == load
    assert doc.find_child(None, "part", "Кронштейн") == part
    assert doc.find_child(None, "part", "нет") is None
    assert doc.owners() == [TEST_OWNER]


def test_properties_are_a_copy(model: dict[str, stresslab.Object]) -> None:
    properties = model["steel"].properties
    properties["E"] = 1.0
    assert model["steel"].properties["E"] == 210000.0


def test_objects_follow_changes_and_removal(
    doc: stresslab.Document, model: dict[str, stresslab.Object]
) -> None:
    load = model["load"]
    stresslab.cmd("model.rename", object=load, name="Сила 1")
    assert load.name == "Сила 1"
    removed = stresslab.cmd("model.remove", object=model["part"])
    assert removed == [model["part"].id, load.id]
    assert not load.exists
    with pytest.raises(stresslab.ModelError) as raised:
        _ = load.name
    assert raised.value.code == "unknown_object"
    assert "removed" in repr(load)
    stresslab.undo()
    assert load.exists and load.name == "Сила 1"


def test_units(doc: stresslab.Document) -> None:
    assert doc.units == {"length": "m", "mass": "kg", "time": "s", "temperature": "K"}
    stresslab.cmd("model.set_units", length="mm", mass="t", time="s", temperature="degC")
    assert doc.units == {"length": "mm", "mass": "t", "time": "s", "temperature": "degC"}


def test_undo_and_redo(doc: stresslab.Document, model: dict[str, stresslab.Object]) -> None:
    assert doc.can_undo and not doc.can_redo
    assert doc.undo_label == "model.create"
    assert stresslab.undo() is True
    assert len(doc) == 2
    assert doc.can_redo and doc.redo_label == "model.create"
    assert stresslab.redo() is True
    assert len(doc) == 3
    while stresslab.undo():
        pass
    assert len(doc) == 0
    assert stresslab.undo() is False


def test_transaction_is_one_step(
    doc: stresslab.Document, model: dict[str, stresslab.Object]
) -> None:
    with stresslab.transaction("Нагрузки") as transaction:
        assert doc.in_transaction and transaction.is_open
        stresslab.cmd("model.rename", object=model["load"], name="a")
        stresslab.cmd("model.rename", object=model["part"], name="b")
    assert not transaction.is_open
    assert doc.undo_label == "Нагрузки"
    stresslab.undo()
    assert model["load"].name == "P1" and model["part"].name == "Кронштейн"


def test_transaction_rolls_back_when_the_block_raises(
    doc: stresslab.Document, model: dict[str, stresslab.Object]
) -> None:
    with pytest.raises(ZeroDivisionError), stresslab.transaction("сломано"):
        stresslab.cmd("model.rename", object=model["load"], name="a")
        _ = 1 / 0
    assert model["load"].name == "P1"
    assert not doc.in_transaction
    assert doc.undo_label == "model.create"


def test_explicit_commit_and_rollback(
    doc: stresslab.Document, model: dict[str, stresslab.Object]
) -> None:
    transaction = doc.transaction("вручную")
    stresslab.cmd("model.rename", object=model["load"], name="a")
    transaction.rollback()
    assert model["load"].name == "P1"
    with pytest.raises(stresslab.CommandError) as raised:
        transaction.commit()
    assert raised.value.code == "invalid_state"


def test_undo_inside_a_transaction_is_refused(doc: stresslab.Document) -> None:
    with stresslab.transaction(), pytest.raises(stresslab.CommandError) as raised:
        stresslab.undo()
    assert raised.value.code == "invalid_state"


def test_errors_of_the_bus_have_codes(doc: stresslab.Document) -> None:
    with pytest.raises(stresslab.CommandError) as raised:
        stresslab.cmd("no.such_command")
    assert raised.value.code == "unknown_command"
    assert isinstance(raised.value, stresslab.Error)
    assert isinstance(raised.value, RuntimeError)
    with pytest.raises(stresslab.CommandError) as raised:
        stresslab.cmd("model.create", type="part")
    assert raised.value.code == "invalid_arguments"
    with pytest.raises(stresslab.ModelError) as raised:
        stresslab.cmd("model.rename", object=42, name="x")
    assert raised.value.code == "unknown_object"


def test_a_command_named_like_a_parameter_of_cmd(doc: stresslab.Document) -> None:
    doc.register_command(
        "test.kw",
        lambda ctx, command: command,
        owner=TEST_OWNER,
        args=[stresslab.ArgSpec("command", "string")],
    )
    assert stresslab.cmd("test.kw", command="x") == "x"


def test_model_change_listener(doc: stresslab.Document, model: dict[str, stresslab.Object]) -> None:
    changes: list[stresslab.ModelChange] = []
    with doc.on_model_changed(changes.append) as subscription:
        stresslab.cmd("model.rename", object=model["load"], name="a")
        stresslab.undo()
        stresslab.redo()
    assert not subscription.active
    stresslab.cmd("model.rename", object=model["load"], name="b")
    assert [(c.cause, c.label) for c in changes] == [
        ("execute", "model.rename"),
        ("undo", "model.rename"),
        ("redo", "model.rename"),
    ]
    assert changes[0].updated == [model["load"].id]
    assert changes[0].created == [] and changes[0].removed == [] and not changes[0].units


def test_journal_listener(doc: stresslab.Document) -> None:
    entries: list[str] = []
    subscription = doc.on_journal(entries.append)
    stresslab.cmd("model.create", type="part", owner=TEST_OWNER, name="Деталь")
    with stresslab.transaction("T"):
        stresslab.cmd("model.create", type="part", owner=TEST_OWNER, name="b")
    stresslab.undo()
    subscription.close()
    stresslab.redo()
    assert entries == [
        'stresslab.cmd("model.create", type="part", owner="test.module", name="Деталь")',
        'with stresslab.transaction("T"):\n'
        '    stresslab.cmd("model.create", type="part", owner="test.module", name="b")',
        "stresslab.undo()",
    ]


def test_a_failing_listener_does_not_break_the_command(
    doc: stresslab.Document, monkeypatch: pytest.MonkeyPatch
) -> None:
    reported: list[str] = []
    # Only the text is kept: the report holds frames that refer to the document.
    monkeypatch.setattr(
        sys, "unraisablehook", lambda report: reported.append(str(report.exc_value))
    )

    def fail(change: stresslab.ModelChange) -> None:
        raise ValueError("listener failed")

    subscription = doc.on_model_changed(fail)
    stresslab.cmd("model.create", type="part", owner=TEST_OWNER)
    subscription.close()
    assert len(doc) == 1
    assert reported == ["listener failed"]


def test_documents_are_separate(doc: stresslab.Document) -> None:
    other = stresslab.Document()
    other.cmd("model.create", type="part", owner=TEST_OWNER)
    assert len(other) == 1 and len(doc) == 0
    assert other != doc
    assert stresslab.active_document() == doc
    stresslab.set_active_document(other)
    assert stresslab.active_document() == other
    assert hash(stresslab.active_document()) == hash(other)


def test_owners_are_enabled_only_in_documents_made_in_python() -> None:
    doc = stresslab.Document()
    assert not doc.is_owner_enabled(TEST_OWNER)
    part = doc[doc.cmd("model.create", type="part", owner=TEST_OWNER)]
    assert part.is_opaque
    with pytest.raises(stresslab.ModelError) as raised:
        doc.cmd("model.rename", object=part, name="x")
    assert raised.value.code == "opaque_object"
    doc.set_owner_enabled(TEST_OWNER)
    assert doc.is_owner_enabled(TEST_OWNER) and not part.is_opaque
    doc.cmd("model.rename", object=part, name="x")
    doc.set_owner_enabled(TEST_OWNER, False)
    assert part.is_opaque
    attached = stresslab.attached_document()
    if attached is not None:
        with pytest.raises(RuntimeError):
            attached.set_owner_enabled(TEST_OWNER)
