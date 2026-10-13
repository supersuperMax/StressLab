"""The code of the module test.headless (see its manifest)."""

from __future__ import annotations

import os
import sys
import threading
import time
from typing import Any

from stresslab_host import Call, Extension, TaskControl

OWNER = "test.headless"


def activate(extension: Extension) -> None:
    @extension.command("headless.make")
    def make(call: Call, name: str) -> int:
        return call.create_object("part", OWNER, name=name)

    @extension.command("headless.background")
    def background(call: Call, name: str) -> None:
        def run() -> None:
            time.sleep(0.05)  # After the command has returned.
            extension.execute("model.create", type="part", owner=OWNER, name=name)

        threading.Thread(target=run).start()

    @extension.command("headless.task_make")
    def task_make(call: Call, name: str) -> Any:
        def work(control: TaskControl) -> Any:
            control.set_progress(0.5)
            # Outside of a command: waits until the application polls.
            return extension.execute("model.create", type="part", owner=OWNER, name=name)

        return call.run_task("Making a part", work)

    @extension.command("headless.crash")
    def crash(call: Call) -> None:
        sys.stderr.write("headless: crashing now\n")
        sys.stderr.flush()
        os._exit(3)
