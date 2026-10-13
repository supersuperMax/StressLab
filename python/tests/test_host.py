"""Parts of the extension host (task 1.9) without the application: frames, the connection, shared
memory, bulk data and the path of a module. The host with the application is tested by
tests/unit/core/extension/host_runtime_test.cpp."""

from __future__ import annotations

import json
import os
import sys
import threading
from pathlib import Path
from typing import Any

import numpy as np
import pytest
from stresslab_host import ElementBlock, ElementSet, Mesh, NodeSet, Surface, _data, _rpc, _shm
from stresslab_host._host import isolate_path, to_json

# --- Frames --------------------------------------------------------------------------------------


def test_frames_split_anywhere() -> None:
    stream = _rpc.frame({"a": 1}) + _rpc.frame({"текст": "да"})
    for size in range(1, len(stream) + 1):
        reader = _rpc.FrameReader()
        payloads = []
        for start in range(0, len(stream), size):
            payloads += reader.feed(stream[start : start + size])
        assert [json.loads(p) for p in payloads] == [{"a": 1}, {"текст": "да"}]


def test_frame_counts_bytes() -> None:
    assert _rpc.frame({"й": 1}).startswith(b"Content-Length: 8\r\n\r\n")


@pytest.mark.parametrize(
    "header", [b"Content-Length: x\r\n\r\n", b"oops\r\n\r\n", b"Content-Type: a\r\n\r\n"]
)
def test_malformed_headers_are_refused(header: bytes) -> None:
    with pytest.raises(_rpc.ProtocolError):
        _rpc.FrameReader().feed(header)


# --- Connection ----------------------------------------------------------------------------------


class FakeApplication:
    """The other end of a connection: pipes into and out of it."""

    def __init__(self, immediate: Any = None) -> None:
        to_host_read, self.to_host = os.pipe()
        self.from_host, from_host_write = os.pipe()
        self.connection = _rpc.Connection(to_host_read, from_host_write, immediate)
        self.connection.start()
        self.reader = _rpc.FrameReader()
        self.received: list[dict[str, Any]] = []

    def send(self, message: dict[str, Any]) -> None:
        os.write(self.to_host, _rpc.frame({"jsonrpc": "2.0", **message}))

    def receive(self) -> dict[str, Any]:
        while not self.received:
            for payload in self.reader.feed(os.read(self.from_host, 65536)):
                self.received.append(json.loads(payload))
        return self.received.pop(0)

    def close(self) -> None:
        os.close(self.to_host)


def test_request_on_main_thread_does_what_comes_meanwhile() -> None:
    app = FakeApplication()
    done: list[Any] = []

    def answer() -> None:
        request = app.receive()
        assert request["method"] == "ask"
        app.send({"method": "note", "params": {"n": 1}})  # Comes before the answer.
        app.send({"id": request["id"], "result": {"ok": True}})

    thread = threading.Thread(target=answer)
    thread.start()
    result = app.connection.request("ask", {"q": 1}, done.append)
    thread.join()
    assert result == {"ok": True}
    assert [m["method"] for m in done] == ["note"]
    app.close()


def test_request_on_other_thread_waits() -> None:
    app = FakeApplication()
    results: list[Any] = []

    def work() -> None:
        results.append(app.connection.request("bg", None))

    worker = threading.Thread(target=work)
    worker.start()
    request = app.receive()
    assert request["method"] == "bg"
    app.send({"id": request["id"], "result": [1, 2]})
    worker.join()
    assert results == [[1, 2]]
    app.close()


def test_error_answer_raises() -> None:
    app = FakeApplication()

    def answer() -> None:
        request = app.receive()
        app.send({"id": request["id"], "error": {"code": _rpc.NOT_FOUND, "message": "gone"}})

    thread = threading.Thread(target=answer)
    thread.start()
    with pytest.raises(_rpc.RpcError) as error:
        app.connection.request("x", None, lambda message: None)
    thread.join()
    assert error.value.code == _rpc.NOT_FOUND
    app.close()


def test_immediate_messages_skip_the_main_thread() -> None:
    seen: list[str] = []

    def immediate(message: dict[str, Any]) -> bool:
        seen.append(message["method"])
        return message["method"] == "$/ping"

    app = FakeApplication(immediate)
    app.send({"method": "$/ping", "id": 1})
    app.send({"method": "other"})
    assert app.connection.next()["method"] == "other"
    assert seen == ["$/ping", "other"]
    app.close()


def test_closing_ends_waiting() -> None:
    app = FakeApplication()
    app.close()
    with pytest.raises(_rpc.ConnectionClosed):
        app.connection.next()
    with pytest.raises(_rpc.ConnectionClosed):
        app.connection.request("late", None, lambda message: None)


# --- Shared memory and bulk data -----------------------------------------------------------------


def test_shared_memory_is_shared() -> None:
    name = _shm.new_name()
    assert len(name) <= 30
    created = _shm.create(name, 4096)
    try:
        created[:5] = b"hello"
        attached = _shm.attach(name, 4096)
        assert attached[:5] == b"hello"
        attached.close()
    finally:
        created.close()
        _shm.unlink(name)


def chain(nodes: int) -> Mesh:
    return Mesh(
        np.arange(1, nodes + 1),
        np.arange(nodes, dtype=float),
        np.zeros(nodes),
        np.ones(nodes),
        [
            ElementBlock(
                "line2",
                np.arange(1, nodes),
                np.stack([np.arange(nodes - 1), np.arange(1, nodes)], axis=1),
            )
        ],
    )


@pytest.mark.parametrize("shared", [True, False])
def test_mesh_goes_out_and_comes_back(shared: bool) -> None:
    released: list[str] = []
    with _data.outgoing(chain(1000), shared) as message:
        ids = message["nodes"]["ids"]
        assert isinstance(ids, dict if shared else list)
        if shared:
            assert ids["dtype"] == "i8" and ids["count"] == 1000
            assert message["blocks"][0]["connectivity"]["dtype"] == "i4"
        mesh = _data.incoming(message, released.append)
    assert isinstance(mesh, Mesh)
    assert mesh.node_count == 1000
    assert mesh.x[999] == 999.0
    assert mesh.blocks[0].connectivity.shape == (999, 2)
    assert list(mesh.blocks[0].connectivity[-1]) == [998, 999]
    if shared:
        assert len(released) == 1  # Mapped, so the application may drop its name.
        assert not mesh.x.flags.writeable


def test_small_data_goes_inline() -> None:
    with _data.outgoing(NodeSet([3, 1, 2]), True) as message:
        assert message == {"kind": "nodeSet", "nodes": [3, 1, 2]}


@pytest.mark.parametrize(
    "data", [ElementSet(np.arange(5000)), Surface(np.zeros((3000, 2), dtype=np.int32))]
)
def test_sets_come_back(data: Any) -> None:
    with _data.outgoing(data, True) as message:
        back = _data.incoming(message, lambda name: None)
    assert type(back) is type(data)
    values = back.elements if isinstance(back, ElementSet) else back.sides
    original = data.elements if isinstance(data, ElementSet) else data.sides
    assert np.array_equal(values, original)


def test_bad_arrays_are_refused() -> None:
    with pytest.raises(TypeError):
        with _data.outgoing(NodeSet([1.5]), False):
            pass
    with pytest.raises(ValueError):
        with _data.outgoing(NodeSet([2**40]), False):
            pass
    with pytest.raises(TypeError):
        with _data.outgoing("not data", False):  # type: ignore[arg-type]
            pass


# --- Values and the path -------------------------------------------------------------------------


def test_values_for_the_application() -> None:
    assert to_json({"a": (1, 2.5, None, True), "p": Path("x")}) == {
        "a": [1, 2.5, None, True],
        "p": "x",
    }
    assert to_json(np.int32(7)) == 7
    assert to_json(np.float64(0.5)) == 0.5
    with pytest.raises(TypeError):
        to_json({1: "key is not str"})
    with pytest.raises(TypeError):
        to_json(object())


def test_module_comes_first_on_the_path(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    foreign = tmp_path / "foreign"
    own = str(Path(_host_file()).resolve().parent.parent)
    monkeypatch.setenv("PYTHONPATH", os.pathsep.join([str(foreign), own]))
    monkeypatch.setattr(sys, "path", ["", str(foreign), own, *sys.path])
    monkeypatch.chdir(tmp_path)
    module = tmp_path / "модуль"
    isolate_path(module, [str(module / "site-packages")])
    assert sys.path[:3] == [str(module), str(module / "site-packages"), own]
    assert str(foreign) not in sys.path
    assert "" not in sys.path


def _host_file() -> str:
    from stresslab_host import _host

    return _host.__file__
