"""Mesh arrays as numpy without copying (task 1.8): the arrays Python gets are read-only views of
the data in the model, so the model still changes only through commands."""

from __future__ import annotations

from typing import Any

import numpy as np
import pytest
import stresslab
from support import TEST_OWNER

# Two tet4 elements on five nodes.
NODE_IDS = [10, 20, 30, 40, 50]
X = [0.0, 1.0, 0.0, 0.0, 1.0]
Y = [0.0, 0.0, 1.0, 0.0, 1.0]
Z = [0.0, 0.0, 0.0, 1.0, 1.0]
TETS = [[0, 1, 2, 3], [1, 2, 3, 4]]


def address(array: np.ndarray) -> int:
    return array.__array_interface__["data"][0]


@pytest.fixture
def mesh_doc(doc: stresslab.Document) -> stresslab.Document:
    """A document with a command that creates a mesh with a node set, an element set and a
    surface: bulk data comes from module commands, not from the journal."""

    def make_mesh(ctx: stresslab.CommandContext, nodes: int = 0) -> int:
        if nodes:
            coordinates = np.arange(nodes, dtype=np.float64)
            mesh = stresslab.Mesh(np.arange(1, nodes + 1), coordinates, coordinates, coordinates)
        else:
            mesh = stresslab.Mesh(NODE_IDS, X, Y, Z, blocks=[("tet4", [1, 2], TETS)])
        obj = ctx.create_object("mesh", "fem.core", name="Сетка", data=mesh)
        if not nodes:
            ctx.create_object(
                "node_set", "fem.core", name="опора", parent=obj, data=stresslab.NodeSet([3, 0, 0])
            )
            ctx.create_object(
                "element_set", "fem.core", name="все", parent=obj, data=stresslab.ElementSet([1, 0])
            )
            ctx.create_object(
                "surface",
                "fem.core",
                name="верх",
                parent=obj,
                data=stresslab.Surface([[1, 3], [0, 2]]),
            )
        return obj.id

    doc.register_command(
        "test.mesh",
        make_mesh,
        owner=TEST_OWNER,
        args=[stresslab.ArgSpec("nodes", "integer", required=False)],
    )
    return doc


def test_mesh_arrays(mesh_doc: stresslab.Document) -> None:
    mesh = mesh_doc[stresslab.cmd("test.mesh")].data
    assert isinstance(mesh, stresslab.Mesh)
    assert mesh.node_count == 5 and mesh.element_count == 2
    assert mesh.node_ids.dtype == np.int64 and mesh.node_ids.tolist() == NODE_IDS
    assert mesh.x.dtype == np.float64 and mesh.x.tolist() == X
    assert mesh.y.tolist() == Y and mesh.z.tolist() == Z
    (block,) = mesh.blocks
    assert block.type == "tet4" and block.nodes_per_element == 4 and len(block) == 2
    assert block.ids.tolist() == [1, 2]
    assert block.connectivity.dtype == np.int32
    assert block.connectivity.shape == (2, 4)
    assert block.connectivity.tolist() == TETS
    assert mesh.block_offset(0) == 0 and mesh.block_offset(1) == 2


def test_sets_and_surfaces(mesh_doc: stresslab.Document) -> None:
    mesh = mesh_doc[stresslab.cmd("test.mesh")]
    node_set, element_set, surface = (child.data for child in mesh.children)
    assert isinstance(node_set, stresslab.NodeSet)
    assert node_set.nodes.tolist() == [0, 3] and len(node_set) == 2  # sorted, no duplicates
    assert element_set.elements.tolist() == [0, 1]
    assert surface.sides.shape == (2, 2)
    assert surface.sides.tolist() == [[0, 2], [1, 3]]


def test_arrays_are_views_of_the_model_not_copies(mesh_doc: stresslab.Document) -> None:
    obj = mesh_doc[stresslab.cmd("test.mesh")]
    first, second = obj.data.x, obj.data.x
    assert not first.flags.owndata
    assert address(first) == address(second)
    assert np.shares_memory(first, second)
    connectivity = obj.data.blocks[0].connectivity
    assert address(connectivity) == address(obj.data.blocks[0].connectivity)
    assert address(obj.children[0].data.nodes) == address(obj.children[0].data.nodes)


def test_a_million_nodes_are_not_copied(mesh_doc: stresslab.Document) -> None:
    obj = mesh_doc[stresslab.cmd("test.mesh", nodes=1_000_000)]
    arrays = [obj.data.x for _ in range(50)]  # 400 MB if each one were a copy
    assert len({address(array) for array in arrays}) == 1
    assert arrays[0][999_999] == 999_999.0


def test_arrays_are_read_only(mesh_doc: stresslab.Document) -> None:
    mesh = mesh_doc[stresslab.cmd("test.mesh")].data
    assert not mesh.x.flags.writeable
    with pytest.raises(ValueError):
        mesh.x[0] = 5.0
    with pytest.raises(ValueError):
        mesh.blocks[0].connectivity[0, 0] = 4
    with pytest.raises(ValueError):
        mesh.x.flags.writeable = True


def test_views_keep_the_data_alive(mesh_doc: stresslab.Document) -> None:
    obj = mesh_doc[stresslab.cmd("test.mesh")]
    x = obj.data.x
    mesh = obj.data
    stresslab.undo()  # the object and its data leave the model
    assert not obj.exists
    assert x.tolist() == X
    assert mesh.blocks[0].connectivity.tolist() == TETS


def test_undo_and_redo_keep_the_same_arrays(mesh_doc: stresslab.Document) -> None:
    obj = mesh_doc[stresslab.cmd("test.mesh")]
    before = address(obj.data.x)
    stresslab.undo()
    stresslab.redo()
    assert address(obj.data.x) == before
    assert obj.data == obj.data


def test_mesh_input_is_converted_or_refused() -> None:
    mesh = stresslab.Mesh(
        np.array([1, 2], dtype=np.int32),
        np.array([0, 1], dtype=np.float32),
        [0, 0],
        (0.0, 0.0),
        blocks=[("line2", np.array([7]), np.array([0, 1], dtype=np.uint8))],
    )
    assert mesh.x.dtype == np.float64 and mesh.blocks[0].connectivity.tolist() == [[0, 1]]
    with pytest.raises(TypeError):
        stresslab.Mesh([1.5, 2.5], [0, 1], [0, 0], [0, 0])  # labels must be integers
    with pytest.raises(ValueError):
        stresslab.Mesh([1, 2], [0, 1], [0, 0], [0, 0], blocks=[("line2", [1], [0, 2**40])])
    with pytest.raises(ValueError):
        stresslab.Mesh([1], [0], [0], [0], blocks=[("brick", [1], [0])])
    with pytest.raises(stresslab.ModelError) as raised:
        stresslab.Mesh([1, 2], [0, 1], [0], [0, 0])  # arrays of different length
    assert raised.value.code == "invalid_mesh"
    with pytest.raises(stresslab.ModelError):
        stresslab.Mesh([1, 2], [0, 1], [0, 0], [0, 0], blocks=[("line2", [1], [0, 5])])


def test_bulk_data_is_checked_against_its_mesh(mesh_doc: stresslab.Document) -> None:
    mesh = stresslab.cmd("test.mesh")

    def bad_set(ctx: stresslab.CommandContext, **_: Any) -> None:
        ctx.create_object(
            "node_set", "fem.core", name="лишний", parent=mesh, data=stresslab.NodeSet([99])
        )

    mesh_doc.register_command("test.bad_set", bad_set, owner=TEST_OWNER)
    with pytest.raises(stresslab.ModelError) as raised:
        stresslab.cmd("test.bad_set")
    assert raised.value.code == "invalid_data"
    mesh_doc.register_command(
        "test.not_data",
        lambda ctx: ctx.create_object("mesh", "fem.core", data=[1, 2]),
        owner=TEST_OWNER,
    )
    with pytest.raises(TypeError):
        stresslab.cmd("test.not_data")
