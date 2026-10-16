"""StressLab from Python (docs/architecture.md §4.7).

Every change of the model is a command of the Command Bus::

    import stresslab

    steel = stresslab.cmd("model.create", type="material", owner="fem.model.structural",
                          name="Steel", properties={"E": 210000.0, "nu": 0.3})
    stresslab.model.rename(object=steel, name="Steel S235")   # the same, as a typed wrapper
    with stresslab.transaction("Loads"):
        ...
    stresslab.undo()

Calls are recorded in the session journal (.slpy) as ``stresslab.cmd(...)`` lines, so a journal
is a script that rebuilds the same document. Modules are enabled and disabled with
``stresslab.extensions.enable("fem.demo")`` and ``disable``, and the document is saved to and
opened from a project folder with ``stresslab.project.save_as("wing")`` and
``stresslab.project.open("wing.slproj")``, journaled too, and its versions and variants are kept
with ``stresslab.history.commit("Two ribs")`` and the other calls of ``stresslab.history``.
Inside the application ``stresslab`` works with the application's document; in a plain Python
process it makes a document of its own.
"""

from __future__ import annotations

import sys
import threading
from collections.abc import Callable, Iterable
from typing import Any

import _stresslab as _native
from _stresslab import (
    ArgSpec,
    ClosedError,
    CommandContext,
    CommandError,
    CommandSpec,
    Context,
    ContextError,
    Document,
    ElementBlock,
    ElementSet,
    Error,
    ExtensionError,
    ExtensionInfo,
    JobError,
    Mesh,
    ModelChange,
    ModelError,
    NodeSet,
    Object,
    ProjectError,
    Subscription,
    Surface,
    Task,
    TaskCancelled,
    TaskControl,
    TaskError,
    TaskEvent,
    Transaction,
    WhenSyntaxError,
    active_document,
    attached_document,
    journal_header,
    make_task,
    set_active_document,
)

from . import extensions, history, project
from ._commands import CommandNamespace, Commands, python_name

__version__: str = _native.__version__

#: True inside the StressLab executable, False in a plain Python process.
embedded: bool = "_stresslab" in sys.builtin_module_names

#: The context keys of the application (docs/architecture.md §4.3).
context: Context = _native.context

#: Typed wrappers of the commands of the active document: ``stresslab.commands.model.create``.
#: The same wrappers are attributes of the module (``stresslab.model.create``) unless a name of
#: the module hides them.
commands = Commands(active_document)

Document.commands = property(  # type: ignore[attr-defined]
    lambda self: Commands(lambda: self),
    doc="Typed wrappers of the commands of this document.",
)

__all__ = [
    "ArgSpec",
    "ClosedError",
    "CommandContext",
    "CommandError",
    "CommandNamespace",
    "CommandSpec",
    "Commands",
    "Context",
    "ContextError",
    "Document",
    "ElementBlock",
    "ElementSet",
    "Error",
    "ExtensionError",
    "ExtensionInfo",
    "JobError",
    "Mesh",
    "ModelChange",
    "ModelError",
    "NodeSet",
    "Object",
    "ProjectError",
    "Subscription",
    "Surface",
    "Task",
    "TaskCancelled",
    "TaskControl",
    "TaskError",
    "TaskEvent",
    "Transaction",
    "WhenSyntaxError",
    "active_document",
    "attached_document",
    "cmd",
    "command",
    "commands",
    "context",
    "embedded",
    "extensions",
    "history",
    "journal_header",
    "make_task",
    "new_document",
    "project",
    "python_name",
    "redo",
    "register_command",
    "run_task",
    "set_active_document",
    "transaction",
    "undo",
]


def cmd(command: str, /, **kwargs: Any) -> Any:
    """Runs a command on the active document. Returns its value, or its Task if it goes on in the
    background."""
    return active_document().cmd(command, **kwargs)


def undo() -> bool:
    """Reverts the last step of the active document; False if there is none."""
    return active_document().undo()


def redo() -> bool:
    """Repeats the last undone step of the active document; False if there is none."""
    return active_document().redo()


def transaction(label: str = "") -> Transaction:
    """Commands of a ``with`` block become one undo step and one journal entry."""
    return active_document().transaction(label)


def new_document() -> Document:
    """A new empty document, made the active one."""
    document = Document()
    set_active_document(document)
    return document


def register_command(
    id: str,
    handler: Callable[..., Any],
    *,
    owner: str,
    args: Iterable[ArgSpec] = (),
    document: Document | None = None,
) -> None:
    """Registers a command whose code is Python: ``handler(context, **arguments)``.

    The handler changes the model through ``context`` (``execute``, ``create_object``,
    ``set_data``); its return value, or a Task, is the result of the command.
    """
    target = document if document is not None else active_document()
    target.register_command(id, handler, owner=owner, args=list(args))


def command(
    id: str,
    *,
    owner: str,
    args: Iterable[ArgSpec] = (),
    document: Document | None = None,
) -> Callable[[Callable[..., Any]], Callable[..., Any]]:
    """Decorator form of `register_command`."""

    def register(handler: Callable[..., Any]) -> Callable[..., Any]:
        register_command(id, handler, owner=owner, args=args, document=document)
        return handler

    return register


def run_task(title: str, work: Callable[[TaskControl], Any]) -> Task:
    """Runs ``work(control)`` on a thread of its own as a Task and returns the task at once.

    The work reports progress through ``control`` and stops when ``control.check_cancelled()``
    raises; what it returns is the result of the task, what it raises fails the task.
    """
    task, control = make_task(title)

    def run() -> None:
        if not control.start():
            return  # Cancelled while pending.
        try:
            value = work(control)
        except TaskCancelled:
            control.finish_cancelled()
            return
        except Exception as error:
            control.fail(f"{type(error).__name__}: {error}")
            return
        try:
            control.succeed(value)
        except Exception as error:
            control.fail(f"{type(error).__name__}: {error}")

    threading.Thread(target=run, name=f"stresslab task {title!r}", daemon=True).start()
    return task


def __getattr__(name: str) -> Any:
    # Typed wrappers of commands: stresslab.<module>.<command>(...)
    if name.startswith("_"):
        raise AttributeError(f"module 'stresslab' has no attribute {name!r}")
    try:
        return getattr(commands, name)
    except AttributeError:
        raise AttributeError(f"module 'stresslab' has no attribute {name!r}") from None


def __dir__() -> list[str]:
    return sorted(set(globals()) | set(dir(commands)))
