"""Bulk data of model objects (meshes, sets) and how it travels: its arrays go through shared
memory, or inline in the message when they are small or shared memory is off."""

from __future__ import annotations

import mmap
from collections.abc import Iterator, Sequence
from contextlib import contextmanager
from dataclasses import dataclass, field
from typing import Any

from . import _shm

#: Arrays of at least this many bytes in total go through shared memory.
SHARED_MEMORY_THRESHOLD = 4096

_DTYPES = {"f8": "<f8", "i8": "<i8", "i4": "<i4"}


def _numpy() -> Any:
    """numpy, imported when bulk data is first used: the host starts faster without it."""
    try:
        import numpy
    except ImportError as error:  # pragma: no cover - numpy comes with the application
        raise ImportError("bulk data of the model needs numpy") from error
    return numpy


@dataclass(eq=False)
class ElementBlock:
    """Elements of one type: ``type`` as the model names it ("hex8", "tet10", ...), element ids
    and node indices (not labels), one row per element."""

    type: str
    ids: Any
    connectivity: Any


@dataclass(eq=False)
class Mesh:
    """Nodes (labels and coordinates) and blocks of elements. Arrays received from the
    application are read-only views of shared memory."""

    node_ids: Any
    x: Any
    y: Any
    z: Any
    blocks: list[ElementBlock] = field(default_factory=list)

    @property
    def node_count(self) -> int:
        return len(self.node_ids)


@dataclass(eq=False)
class NodeSet:
    """Node indices of a mesh."""

    nodes: Any


@dataclass(eq=False)
class ElementSet:
    """Global element indices of a mesh."""

    elements: Any


@dataclass(eq=False)
class Surface:
    """Element sides: one row (element index, side number) per side."""

    sides: Any


Data = Mesh | NodeSet | ElementSet | Surface


# --- To the application --------------------------------------------------------------------------


class _Arrays:
    """Arrays of one message, placed into one segment or written inline."""

    def __init__(self) -> None:
        self.items: list[tuple[Any, str]] = []

    def add(self, values: Any, dtype: str) -> int:
        numpy = _numpy()
        source = numpy.asarray(values).reshape(-1)
        target = numpy.dtype(_DTYPES[dtype])
        if target.kind == "i" and source.size:
            if source.dtype.kind not in "iub":
                raise TypeError(f"ids and indices must be integers, not {source.dtype}")
            limits = numpy.iinfo(target)
            if source.min() < limits.min or source.max() > limits.max:
                raise ValueError(f"ids and indices must fit into {target}")
        self.items.append((numpy.ascontiguousarray(source, dtype=target), dtype))
        return len(self.items) - 1

    def nbytes(self) -> int:
        return sum((array.nbytes + 7) & ~7 for array, _ in self.items)


def _encode(data: Data, arrays: _Arrays) -> dict[str, Any]:
    if isinstance(data, Mesh):
        return {
            "kind": "mesh",
            "nodes": {
                "ids": arrays.add(data.node_ids, "i8"),
                "x": arrays.add(data.x, "f8"),
                "y": arrays.add(data.y, "f8"),
                "z": arrays.add(data.z, "f8"),
            },
            "blocks": [
                {
                    "type": block.type,
                    "ids": arrays.add(block.ids, "i8"),
                    "connectivity": arrays.add(block.connectivity, "i4"),
                }
                for block in data.blocks
            ],
        }
    if isinstance(data, NodeSet):
        return {"kind": "nodeSet", "nodes": arrays.add(data.nodes, "i4")}
    if isinstance(data, ElementSet):
        return {"kind": "elementSet", "elements": arrays.add(data.elements, "i4")}
    if isinstance(data, Surface):
        return {"kind": "surface", "sides": arrays.add(data.sides, "i4")}
    raise TypeError(f"{type(data).__name__} is not bulk data of the model")


def _place(node: Any, placed: Sequence[Any]) -> Any:
    if isinstance(node, dict):
        return {key: _place(value, placed) for key, value in node.items()}
    if isinstance(node, list):
        return [_place(value, placed) for value in node]
    if isinstance(node, int) and not isinstance(node, bool):
        return placed[node]
    return node


@contextmanager
def outgoing(data: Data, shared_memory: bool) -> Iterator[dict[str, Any]]:
    """The message form of ``data``; its segment of shared memory is removed when the block
    ends, after the application has answered."""
    arrays = _Arrays()
    message = _encode(data, arrays)
    total = arrays.nbytes()
    if not shared_memory or total < SHARED_MEMORY_THRESHOLD:
        yield _place(message, [array.tolist() for array, _ in arrays.items])
        return
    name = _shm.new_name()
    memory = _shm.create(name, total)
    try:
        placed = []
        offset = 0
        for array, dtype in arrays.items:
            if array.nbytes:
                view = _numpy().frombuffer(
                    memory, dtype=array.dtype, count=array.size, offset=offset
                )
                view[:] = array
                del view
            placed.append({"shm": name, "offset": offset, "dtype": dtype, "count": int(array.size)})
            offset += (array.nbytes + 7) & ~7
        yield _place(message, placed)
    finally:
        memory.close()
        _shm.unlink(name)


# --- From the application ------------------------------------------------------------------------


class _Segments:
    """Segments of the application mapped while one message is read; the application is told it
    may drop its names once they are mapped (the mappings stay alive with the arrays)."""

    def __init__(self) -> None:
        self.mapped: dict[str, mmap.mmap] = {}

    def array(self, ref: Any) -> Any:
        numpy = _numpy()
        if isinstance(ref, list):
            return numpy.asarray(ref)
        dtype = numpy.dtype(_DTYPES[ref["dtype"]])
        count = int(ref["count"])
        offset = int(ref.get("offset", 0))
        name = ref["shm"]
        if count == 0:
            return numpy.empty(0, dtype=dtype)
        memory = self.mapped.get(name)
        if memory is None or len(memory) < offset + count * dtype.itemsize:
            memory = _shm.attach(name, offset + count * dtype.itemsize)
            self.mapped[name] = memory
        array = numpy.frombuffer(memory, dtype=dtype, count=count, offset=offset)
        array.flags.writeable = False
        return array


def incoming(message: Any, release: Any) -> Data | None:
    """Bulk data from a message of the application; ``release(name)`` is called for each segment
    once it is mapped."""
    if message is None:
        return None
    segments = _Segments()
    try:
        return _decode(message, segments)
    finally:
        for name in segments.mapped:
            release(name)


def _decode(message: dict[str, Any], segments: _Segments) -> Data:
    kind = message.get("kind")
    if kind == "mesh":
        nodes = message["nodes"]
        blocks = []
        for block in message.get("blocks", []):
            ids = segments.array(block["ids"])
            connectivity = segments.array(block["connectivity"])
            if len(ids):
                connectivity = connectivity.reshape(len(ids), -1)
            blocks.append(ElementBlock(block["type"], ids, connectivity))
        return Mesh(
            segments.array(nodes["ids"]),
            segments.array(nodes["x"]),
            segments.array(nodes["y"]),
            segments.array(nodes["z"]),
            blocks,
        )
    if kind == "nodeSet":
        return NodeSet(segments.array(message["nodes"]))
    if kind == "elementSet":
        return ElementSet(segments.array(message["elements"]))
    if kind == "surface":
        return Surface(segments.array(message["sides"]).reshape(-1, 2))
    raise ValueError(f"unknown kind of data {kind!r}")
