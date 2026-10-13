"""The API of python modules of StressLab, as their code sees it in the extension host.

A python module is a package in its folder; ``entry = "package.module:activate"`` in its
manifest names the function the host calls with an `Extension`::

    def activate(extension):
        @extension.command("demo.hello")
        def hello(call, name):
            return f"Hello, {name}"

The host runs in a process of its own (docs/architecture.md §4.4): a crash of the module does not
touch the application, which restarts the host. The model is changed only through the `Call` a
command gets (its changes are one undo step with the command) or by running commands.
"""

from __future__ import annotations

from ._data import ElementBlock, ElementSet, Mesh, NodeSet, Surface
from ._host import (
    PROTOCOL_VERSION,
    Call,
    ContextKeys,
    Extension,
    ModuleError,
    ObjectInfo,
    Subscription,
    Task,
    TaskCancelled,
    TaskControl,
    Watch,
)

__all__ = [
    "PROTOCOL_VERSION",
    "Call",
    "ContextKeys",
    "ElementBlock",
    "ElementSet",
    "Extension",
    "Mesh",
    "ModuleError",
    "NodeSet",
    "ObjectInfo",
    "Subscription",
    "Surface",
    "Task",
    "TaskCancelled",
    "TaskControl",
    "Watch",
]
