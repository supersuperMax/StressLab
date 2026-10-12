"""Context keys from Python (task 1.8): values, declarations, `when` expressions and watches."""

from __future__ import annotations

from collections.abc import Iterator

import pytest
import stresslab
from support import TEST_OWNER

OWNER = "test.context"


@pytest.fixture
def keys() -> Iterator[stresslab.Context]:
    context = stresslab.context
    context.declare("test.stage", owner=OWNER, type="string", initial="mesh")
    context.declare("test.count", owner=OWNER, type="number")
    context.declare("test.flag", owner=OWNER, type="boolean", initial=False)
    context.declare("test.stages", owner=OWNER, type="list", initial=["mesh", "solve"])
    try:
        yield context
    finally:
        context.remove_owner(OWNER)


def test_values(keys: stresslab.Context) -> None:
    assert keys["test.stage"] == "mesh"
    assert keys["test.count"] is None
    assert keys["test.stages"] == ["mesh", "solve"]
    keys["test.count"] = 3
    assert keys["test.count"] == 3.0 and type(keys["test.count"]) is float
    keys.set("test.stage", "решение")
    assert keys["test.stage"] == "решение"
    keys["test.stage"] = None
    assert keys.get("test.stage", "нет") == "нет"
    assert keys.get("not.declared") is None
    assert keys["not.declared"] is None


def test_declarations(keys: stresslab.Context) -> None:
    assert "test.flag" in keys
    assert "test.flag" in keys.keys()
    assert keys.spec("test.flag") == {
        "name": "test.flag",
        "owner": OWNER,
        "type": "boolean",
        "initial": False,
    }
    assert keys.spec("not.declared") is None
    assert keys.declare("test.flag", owner=OWNER, type="boolean", initial=False) is False
    with pytest.raises(stresslab.ContextError) as raised:
        keys.declare("test.flag", owner=OWNER, type="number")
    assert raised.value.code == "duplicate_key"
    with pytest.raises(ValueError):
        keys.declare("test.other", owner=OWNER, type="colour")
    assert keys.remove_owner(OWNER) == 4
    assert "test.flag" not in keys


def test_bad_values_are_refused(keys: stresslab.Context) -> None:
    with pytest.raises(stresslab.ContextError) as raised:
        keys["test.flag"] = "text"
    assert raised.value.code == "invalid_value"
    with pytest.raises(stresslab.ContextError) as raised:
        keys["not.declared"] = 1
    assert raised.value.code == "unknown_key"
    with pytest.raises(TypeError):
        keys["test.stages"] = [1, 2]
    with pytest.raises(TypeError):
        keys["test.count"] = {"a": 1}


def test_when_expressions(keys: stresslab.Context) -> None:
    assert keys.evaluate("test.stage == 'mesh' && !test.flag")
    assert keys.evaluate("test.stage in test.stages")
    keys["test.flag"] = True
    assert not keys.evaluate("test.stage == 'mesh' && !test.flag")
    with pytest.raises(stresslab.WhenSyntaxError) as raised:
        keys.evaluate("test.stage == ")
    assert raised.value.column == 15
    assert raised.value.reason
    assert isinstance(raised.value, stresslab.ContextError)


def test_watch(keys: stresslab.Context) -> None:
    seen: list[bool] = []
    subscription = keys.watch("test.flag || test.count == 2", seen.append)
    keys["test.flag"] = True
    keys["test.count"] = 2  # still true: no call
    keys.update({"test.flag": False, "test.count": 1})  # one change, one call
    subscription.close()
    keys["test.flag"] = True
    assert seen == [True, False]
    assert not subscription.active


def test_core_keys_follow_the_active_document(doc: stresslab.Document) -> None:
    if stresslab.attached_document() is not None:
        pytest.skip("inside the application its own document drives the keys of the core")
    context = stresslab.context
    assert context["history.canUndo"] is False
    stresslab.cmd("model.create", type="part", owner=TEST_OWNER)
    assert context["history.canUndo"] is True
    other = stresslab.new_document()
    assert context["history.canUndo"] is False
    stresslab.set_active_document(doc)
    assert context["history.canUndo"] is True
    assert other.can_undo is False
