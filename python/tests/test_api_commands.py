"""Typed wrappers made from the registry and commands written in Python (task 1.8)."""

from __future__ import annotations

import inspect
from pathlib import Path
from typing import Any

import pytest
import stresslab
from support import TEST_OWNER

# --- Typed wrappers ------------------------------------------------------------------------------


def test_wrappers_of_the_core_commands(doc: stresslab.Document) -> None:
    create = stresslab.model.create
    assert create.__name__ == "create"
    assert create.__qualname__ == "stresslab.model.create"
    assert "model.create" in create.__doc__
    parameters = inspect.signature(create).parameters
    assert list(parameters) == [
        "type",
        "owner",
        "name",
        "parent",
        "dependencies",
        "schema_version",
        "properties",
    ]
    assert all(p.kind is inspect.Parameter.KEYWORD_ONLY for p in parameters.values())
    assert parameters["type"].default is inspect.Parameter.empty
    assert parameters["name"].default == ""
    assert parameters["parent"].default is None  # optional, no default
    assert parameters["schema_version"].default == 1
    assert parameters["parent"].annotation == "stresslab.Object | int"


def test_a_wrapper_call_is_journaled_as_cmd(doc: stresslab.Document) -> None:
    entries: list[str] = []
    doc.on_journal(entries.append)
    part = stresslab.model.create(type="part", owner=TEST_OWNER, name="Деталь")
    assert doc[part].name == "Деталь"
    # Only what the caller passed is written; defaults are not.
    assert entries == [
        'stresslab.cmd("model.create", type="part", owner="test.module", name="Деталь")'
    ]


def test_wrappers_check_arguments_like_python_functions(doc: stresslab.Document) -> None:
    with pytest.raises(TypeError):
        stresslab.model.create(type="part")  # missing owner
    with pytest.raises(TypeError):
        stresslab.model.create(type="part", owner=TEST_OWNER, colour="red")
    with pytest.raises(TypeError):
        stresslab.model.create("part", TEST_OWNER)  # keyword-only
    assert len(doc) == 0


def test_namespaces_list_their_commands(doc: stresslab.Document) -> None:
    assert "model" in dir(stresslab)
    assert {"create", "rename", "remove", "set_units"} <= set(dir(stresslab.model))
    assert repr(stresslab.model) == "<stresslab commands model>"
    with pytest.raises(AttributeError):
        _ = stresslab.model.no_such_command
    with pytest.raises(AttributeError):
        _ = stresslab.no_such_module


def test_names_with_dashes_and_keywords(doc: stresslab.Document) -> None:
    doc.register_command(
        "fem.format.ansys-cdb.import", lambda ctx: "imported", owner="fem.format.ansys-cdb"
    )
    assert stresslab.python_name("ansys-cdb") == "ansys_cdb"
    assert stresslab.python_name("import") == "import_"
    assert stresslab.fem.format.ansys_cdb.import_() == "imported"
    assert "ansys_cdb" in dir(stresslab.fem.format)


def test_ambiguous_names_are_refused(doc: stresslab.Document) -> None:
    doc.register_command("test.do-it", lambda ctx: 1, owner=TEST_OWNER)
    doc.register_command("test.do_it", lambda ctx: 2, owner=TEST_OWNER)
    with pytest.raises(AttributeError, match="ambiguous"):
        stresslab.test.do_it()
    # Both still run through cmd.
    assert stresslab.cmd("test.do-it") == 1
    assert stresslab.cmd("test.do_it") == 2


def test_a_command_with_commands_below_it(doc: stresslab.Document) -> None:
    doc.register_command("test.run", lambda ctx: "run", owner=TEST_OWNER)
    doc.register_command("test.run.fast", lambda ctx: "fast", owner=TEST_OWNER)
    assert stresslab.test.run() == "run"
    assert stresslab.test.run.fast() == "fast"


def test_wrappers_follow_the_registry(doc: stresslab.Document) -> None:
    with pytest.raises(AttributeError):
        _ = stresslab.test.later
    doc.register_command("test.later", lambda ctx: "here", owner=TEST_OWNER)
    assert stresslab.test.later() == "here"
    assert doc.unregister_command("test.later")
    with pytest.raises(AttributeError):
        _ = stresslab.test.later


def test_wrappers_of_a_given_document(doc: stresslab.Document) -> None:
    other = stresslab.Document()
    other.commands.model.create(type="part", owner=TEST_OWNER)
    assert len(other) == 1 and len(doc) == 0
    stresslab.commands.model.create(type="part", owner=TEST_OWNER)
    assert len(doc) == 1


def test_command_specs(doc: stresslab.Document) -> None:
    assert "model.create" in doc.command_ids()
    spec = doc.command_spec("model.create")
    assert spec.id == "model.create" and spec.owner == "fem.core"
    assert [a.name for a in spec.args][:2] == ["type", "owner"]
    name = spec.args[2]
    assert (name.type, name.required, name.has_default, name.default) == ("string", False, True, "")
    assert doc.command_spec("no.such") is None


# --- Commands written in Python ------------------------------------------------------------------


def test_a_python_command_with_typed_arguments(doc: stresslab.Document, tmp_path: Path) -> None:
    received: dict[str, Any] = {}

    @stresslab.command(
        "test.import",
        owner=TEST_OWNER,
        args=[
            stresslab.ArgSpec("file", "path"),
            stresslab.ArgSpec("scale", "number", required=False, default=1),
            stresslab.ArgSpec("note", "string", required=False),
        ],
    )
    def import_file(ctx: stresslab.CommandContext, **args: Any) -> str:
        received.update(args)
        assert ctx.spec.id == "test.import"
        assert ctx.args == args
        assert ctx.document == doc
        return "ok"

    file = tmp_path / "модель 1.dat"
    assert stresslab.test.import_(file=str(file)) == "ok"
    # Paths become paths, the integer default of a number argument a float; `note` is absent.
    assert received == {"file": file, "scale": 1.0}
    assert type(received["scale"]) is float
    sig = inspect.signature(stresslab.test.import_)
    assert sig.parameters["scale"].default == 1.0
    assert sig.parameters["note"].default is None


def test_a_python_command_changes_the_model_in_one_step(doc: stresslab.Document) -> None:
    def frame(ctx: stresslab.CommandContext, name: str) -> list[int]:
        part = ctx.execute("model.create", type="part", owner=TEST_OWNER, name=name)
        beam = ctx.create_object("beam", TEST_OWNER, name="балка", parent=part, properties={"L": 2})
        return [part, beam.id]

    doc.register_command(
        "test.frame", frame, owner=TEST_OWNER, args=[stresslab.ArgSpec("name", "string")]
    )
    entries: list[str] = []
    doc.on_journal(entries.append)
    part, beam = stresslab.cmd("test.frame", name="Рама")
    assert doc[beam].parent == doc[part]
    assert doc[beam].properties == {"L": 2}
    assert entries == ['stresslab.cmd("test.frame", name="Рама")']
    assert doc.undo_label == "test.frame"
    stresslab.undo()
    assert len(doc) == 0


def test_a_failing_python_command_changes_nothing(doc: stresslab.Document) -> None:
    def broken(ctx: stresslab.CommandContext) -> None:
        ctx.execute("model.create", type="part", owner=TEST_OWNER)
        raise ValueError("сломано")

    doc.register_command("test.broken", broken, owner=TEST_OWNER)
    with pytest.raises(ValueError, match="сломано"):
        stresslab.cmd("test.broken")
    assert len(doc) == 0
    assert not doc.can_undo


def test_the_context_is_valid_only_while_the_command_runs(doc: stresslab.Document) -> None:
    kept: list[stresslab.CommandContext] = []
    doc.register_command("test.keep", lambda ctx: kept.append(ctx), owner=TEST_OWNER)
    stresslab.cmd("test.keep")
    with pytest.raises(stresslab.ClosedError):
        kept[0].execute("model.create", type="part", owner=TEST_OWNER)


def test_bad_registrations_are_refused(doc: stresslab.Document) -> None:
    with pytest.raises(ValueError):
        stresslab.ArgSpec("x", "colour")
    with pytest.raises(stresslab.CommandError) as raised:
        doc.register_command("bad id", lambda ctx: None, owner=TEST_OWNER)
    assert raised.value.code == "invalid_spec"
    with pytest.raises(stresslab.CommandError) as raised:
        doc.register_command("model.create", lambda ctx: None, owner=TEST_OWNER)
    assert raised.value.code == "duplicate_command"
    with pytest.raises(TypeError):
        doc.register_command("test.not_callable", 42, owner=TEST_OWNER)
    assert repr(stresslab.ArgSpec("n", "number", required=False)) == (
        "<stresslab.ArgSpec n: number (optional)>"
    )
