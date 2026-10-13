"""The code of the module test.faulty (see its manifest)."""

from __future__ import annotations

import os
import sys
import threading
import time
from typing import Any

import numpy as np
from stresslab_host import Call, ElementBlock, Extension, Mesh, TaskControl

OWNER = "test.faulty"
seen: dict[str, list[Any]] = {"watched": [], "changes": []}


def activate(extension: Extension) -> None:
    print("faulty: activated", flush=True)  # To standard error, not into the protocol.

    @extension.command("faulty.echo")
    def echo(call: Call, value: Any = None) -> Any:
        return {"value": value, "command": call.command}

    @extension.command("faulty.create")
    def create(call: Call, name: str) -> int:
        return call.create_object("thing", OWNER, name=name, properties={"made": "here"})

    @extension.command("faulty.nested")
    def nested(call: Call, name: str) -> list[int]:
        first = call.execute("model.create", type="thing", owner=OWNER, name=name)
        # A command of this module again: the application calls back while it waits.
        second = call.execute("faulty.create", name=name + " 2")
        return [first, second]

    @extension.command("faulty.raise")
    def fail(call: Call) -> None:
        call.create_object("thing", OWNER, name="reverted")
        raise ValueError("broken on purpose")

    @extension.command("faulty.crash")
    def crash(call: Call, code: int = 3) -> None:
        call.create_object("thing", OWNER, name="lost")
        sys.stderr.write("faulty: crashing now\n")
        sys.stderr.flush()
        os._exit(code)

    @extension.command("faulty.hang")
    def hang(call: Call) -> None:
        while True:
            time.sleep(0.01)

    @extension.command("faulty.task")
    def task(call: Call, steps: int) -> Any:
        def work(control: TaskControl) -> int:
            control.message("starting")
            for step in range(steps):
                control.check_cancelled()
                control.set_progress(step / steps)
                time.sleep(0.005)
            control.set_progress(1.0)
            return steps * 2

        return call.run_task("Counting", work)

    @extension.command("faulty.wait_cancel")
    def wait_cancel(call: Call) -> Any:
        def work(control: TaskControl) -> None:
            control.set_progress(None)
            while True:
                control.check_cancelled()
                time.sleep(0.005)

        return call.run_task("Waiting", work)

    @extension.command("faulty.task_crash")
    def task_crash(call: Call) -> Any:
        def work(control: TaskControl) -> None:
            control.set_progress(0.5)
            time.sleep(0.05)
            os._exit(5)

        return call.run_task("Crashing", work)

    @extension.command("faulty.mesh_in")
    def mesh_in(call: Call, nodes: int) -> int:
        coordinates = np.arange(nodes, dtype=np.float64)
        mesh = Mesh(
            node_ids=np.arange(1, nodes + 1),
            x=coordinates,
            y=coordinates * 2.0,
            z=np.zeros(nodes),
            blocks=[
                ElementBlock(
                    "line2",
                    np.arange(1, nodes),
                    np.stack([np.arange(nodes - 1), np.arange(1, nodes)], axis=1),
                )
            ],
        )
        return call.create_object("mesh", "fem.core", name="chain", data=mesh)

    @extension.command("faulty.mesh_out")
    def mesh_out(call: Call, object: int) -> dict[str, Any]:
        mesh = call.data(object)
        assert isinstance(mesh, Mesh)
        block = mesh.blocks[0]
        return {
            "nodes": mesh.node_count,
            "sum_x": float(mesh.x.sum()),
            "sum_y": float(mesh.y.sum()),
            "last_id": int(mesh.node_ids[-1]),
            "elements": len(block.ids),
            "last_element": [int(n) for n in block.connectivity[-1]],
            "writeable": bool(mesh.x.flags.writeable),
            "info": call.object(object).data,
        }

    @extension.command("faulty.set_key")
    def set_key(call: Call, value: float) -> None:
        extension.context.set("faulty.value", value)

    @extension.command("faulty.watch")
    def watch(call: Call, when: str) -> bool:
        handle = extension.context.watch(when, seen["watched"].append)
        return handle.value

    @extension.command("faulty.seen")
    def report(call: Call) -> dict[str, Any]:
        return seen

    @extension.command("faulty.background")
    def background(call: Call, name: str) -> None:
        def run() -> None:
            extension.execute("model.create", type="thing", owner=OWNER, name=name)

        threading.Thread(target=run).start()

    @extension.command("faulty.info")
    def info(call: Call) -> dict[str, Any]:
        return {"pid": os.getpid(), "path": sys.path[:4], "stdout": sys.stdout is sys.stderr}

    extension.on_model_changed(lambda change: seen["changes"].append(change["label"]))
