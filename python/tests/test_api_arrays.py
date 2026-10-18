"""Named arrays of objects of modules (task 3.1, docs/adr/0023-structural-model-and-results-ir.md):
``stresslab.ArrayData`` copies what it is made of once, hands its arrays out as read-only numpy
views without copying, and is saved in a project and opened again bit for bit."""

from __future__ import annotations

from pathlib import Path
from typing import Any

import numpy as np
import pytest
import stresslab
from support import TEST_OWNER


def address(array: np.ndarray) -> int:
    return array.__array_interface__["data"][0]


def field() -> stresslab.ArrayData:
    """Displacements of three nodes."""
    return stresslab.ArrayData(
        {
            "values": np.arange(9, dtype=np.float64).reshape(3, 3) + 0.5,
            "node": np.array([0, 2, 4], dtype=np.int32),
        }
    )


@pytest.fixture
def field_doc(doc: stresslab.Document) -> stresslab.Document:
    """A document with a command that adds a field of results: bulk data comes from module
    commands, not from the journal."""

    def add_field(ctx: stresslab.CommandContext, name: str = "U") -> int:
        obj = ctx.create_object(
            "result_field",
            TEST_OWNER,
            name=name,
            properties={"position": "nodal", "components": ["U1", "U2", "U3"]},
            data=field(),
        )
        return obj.id

    doc.register_command(
        "test.field",
        add_field,
        owner=TEST_OWNER,
        args=[stresslab.ArgSpec("name", "string", required=False)],
    )
    return doc


def test_arrays_have_rows_names_and_types() -> None:
    data = field()
    assert data.rows == 3
    assert data.names == ["node", "values"]
    assert list(data) == ["node", "values"]
    assert len(data) == 2
    assert "values" in data and "point" not in data
    assert data.dtype("node") == "int32"
    assert data.dtype("values") == "float64"
    assert data["node"].tolist() == [0, 2, 4]
    assert data["values"].shape == (3, 3)
    assert data["values"][1].tolist() == [3.5, 4.5, 5.5]
    with pytest.raises(KeyError):
        data["point"]
    with pytest.raises(KeyError):
        data.dtype("point")


@pytest.mark.parametrize(
    ("values", "dtype"),
    [
        (np.array([1, 2], dtype=np.int8), "int32"),
        (np.array([1, 2], dtype=np.int32), "int32"),
        (np.array([1, 2], dtype=np.uint16), "int32"),
        (np.array([1, 2], dtype=np.uint32), "int64"),
        (np.array([1, 2], dtype=np.int64), "int64"),
        ([1, 2], "int64"),
        (np.array([1, 2], dtype=np.float32), "float64"),
        ([1.5, 2.5], "float64"),
    ],
)
def test_items_keep_their_kind(values: Any, dtype: str) -> None:
    data = stresslab.ArrayData({"a": values})
    assert data.dtype("a") == dtype
    assert data["a"].dtype == np.dtype(dtype)
    assert data["a"].tolist() == [1, 2] or data["a"].tolist() == [1.5, 2.5]


def test_views_are_read_only_and_not_copies() -> None:
    data = field()
    values = data["values"]
    assert not values.flags.writeable
    assert address(values) == address(data["values"])
    with pytest.raises(ValueError):
        values[0, 0] = 1.0
    # A view keeps the data alive.
    del data
    assert values[2].tolist() == [6.5, 7.5, 8.5]


def test_width_one_is_one_dimension() -> None:
    data = stresslab.ArrayData({"a": np.zeros((4, 1)), "b": np.zeros((4, 2))})
    assert data["a"].shape == (4,)
    assert data["b"].shape == (4, 2)
    assert stresslab.ArrayData({}).rows == 0


@pytest.mark.parametrize(
    ("arrays", "error"),
    [
        ({"a": np.zeros((2, 2, 2))}, ValueError),
        ({"a": 5.0}, ValueError),
        ({"a": ["x", "y"]}, TypeError),
        ({"a": np.array([True, False])}, TypeError),
        ({"a": np.array([2**63], dtype=np.uint64)}, ValueError),
        ({1: [1.0]}, TypeError),
        ({"a b": [1.0]}, stresslab.ModelError),
        ({"a": [1.0, 2.0], "b": [1.0]}, stresslab.ModelError),
        ({"a": np.zeros((2, 0))}, stresslab.ModelError),
    ],
)
def test_refuses_what_is_not_named_arrays(arrays: dict[Any, Any], error: type) -> None:
    with pytest.raises(error):
        stresslab.ArrayData(arrays)


def test_objects_of_modules_carry_array_data(field_doc: stresslab.Document) -> None:
    obj = field_doc[stresslab.cmd("test.field")]
    data = obj.data
    assert isinstance(data, stresslab.ArrayData)
    assert data == obj.data
    assert data["values"].shape == (3, 3)
    assert address(data["values"]) == address(obj.data["values"])
    assert stresslab.undo()
    assert len(field_doc) == 0


def test_objects_of_modules_carry_no_other_data(doc: stresslab.Document) -> None:
    def add(ctx: stresslab.CommandContext) -> None:
        ctx.create_object("result_field", TEST_OWNER, data=stresslab.NodeSet([0]))

    doc.register_command("test.bad", add, owner=TEST_OWNER)
    with pytest.raises(stresslab.ModelError):
        stresslab.cmd("test.bad")


def test_saved_and_opened_bit_for_bit(field_doc: stresslab.Document, tmp_path: Path) -> None:
    stresslab.cmd("test.field", name="U")
    folder = tmp_path / "результаты с пробелами.slproj"
    stresslab.project.save_as(folder)
    before = field_doc.objects()[0].data

    stresslab.project.open(folder)
    opened = stresslab.active_document()
    after = opened.objects()[0].data
    assert isinstance(after, stresslab.ArrayData)
    assert after.names == before.names
    for name in before.names:
        assert after.dtype(name) == before.dtype(name)
        assert after[name].tobytes() == before[name].tobytes()
    assert opened.objects()[0].properties["components"] == ["U1", "U2", "U3"]
