"""The extension host: runs one python module for the application (docs/architecture.md §4.4).

The application starts ``python -m stresslab_host`` (or ``stresslab --python -m stresslab_host``)
in the folder of the module and talks to it over standard input and output
(docs/extensions/protocol.md). Standard output carries only the protocol: what the module prints
goes to standard error, which the application writes to its log.
"""

from __future__ import annotations

import importlib
import logging
import os
import site
import sys
import threading
import traceback
from collections.abc import Callable, Iterable
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from . import _data, _rpc
from ._data import Data

PROTOCOL_VERSION = 1


class TaskCancelled(Exception):
    """Raised by ``TaskControl.check_cancelled`` when the user cancelled the task."""


class ModuleError(Exception):
    """The application refused what the module asked; ``code`` is the JSON-RPC error code."""

    def __init__(self, code: int, message: str) -> None:
        super().__init__(message)
        self.code = code


def _call(connection: _rpc.Connection, host: Host, method: str, params: Any = None) -> Any:
    try:
        return connection.request(method, params, host.dispatch)
    except _rpc.RpcError as error:
        raise ModuleError(error.code, error.message) from None


def to_json(value: Any) -> Any:
    """A Python value as JSON for the application: None, bool, int, float, str, paths, lists,
    tuples and dicts with str keys; numpy scalars by their value."""
    if value is None or isinstance(value, bool | str):
        return value
    if isinstance(value, int):
        return value
    if isinstance(value, float):
        return value
    if isinstance(value, os.PathLike):
        return os.fspath(value)
    if isinstance(value, list | tuple):
        return [to_json(item) for item in value]
    if isinstance(value, dict):
        result = {}
        for key, item in value.items():
            if not isinstance(key, str):
                raise TypeError(f"keys of a dict must be str, not {type(key).__name__}")
            result[key] = to_json(item)
        return result
    item = getattr(value, "item", None)  # numpy scalars
    if callable(item) and getattr(value, "shape", None) == ():
        return to_json(item())
    raise TypeError(f"{type(value).__name__} cannot be passed to the application")


@dataclass(frozen=True)
class ObjectInfo:
    """An object of the model, as it was when it was read."""

    id: int
    type: str
    owner: str
    name: str
    schema_version: int
    properties: dict[str, Any]
    parent: int | None
    children: list[int]
    dependencies: list[int]
    opaque: bool
    #: "mesh", "nodeSet", "elementSet", "surface" or None.
    data: str | None

    @staticmethod
    def from_json(json: dict[str, Any]) -> ObjectInfo:
        return ObjectInfo(
            id=json["id"],
            type=json["type"],
            owner=json["owner"],
            name=json["name"],
            schema_version=json["schemaVersion"],
            properties=json["properties"],
            parent=json["parent"],
            children=list(json["children"]),
            dependencies=list(json["dependencies"]),
            opaque=json["opaque"],
            data=json["data"],
        )


class _Reader:
    """What both the extension and a call can read from the model."""

    _host: Host

    def object(self, id: int) -> ObjectInfo:
        """An object of the model; ModuleError if there is none."""
        return ObjectInfo.from_json(self._host.request("document/object", {"id": int(id)}))

    def objects(self, *, type: str | None = None, owner: str | None = None) -> list[int]:
        """Ids of the objects of the model, in order, of a type and owner if given."""
        return list(self._host.request("document/objects", {"type": type, "owner": owner}))

    def data(self, id: int) -> Data | None:
        """The bulk data of an object (mesh, set); arrays are read-only views of shared memory."""
        message = self._host.request("document/data", {"id": int(id)})
        return _data.incoming(message, self._host.release)


class Watch:
    """A `when` expression the module watches; ``value`` is its current value."""

    def __init__(self, host: Host, id: int, value: bool) -> None:
        self._host = host
        self.id = id
        self.value = value

    def close(self) -> None:
        if self._host.watches.pop(self.id, None) is not None:
            self._host.notify("context/unwatch", {"watch": self.id})


class Subscription:
    def __init__(self, close: Callable[[], None]) -> None:
        self._close: Callable[[], None] | None = close

    def close(self) -> None:
        if self._close is not None:
            self._close()
            self._close = None


class ContextKeys:
    """The context keys of the application (docs/architecture.md §4.3)."""

    def __init__(self, host: Host) -> None:
        self._host = host

    def declare(self, name: str, type: str, default: Any = None) -> bool:
        """Declares a key of the module: type "boolean", "number", "string" or "list"."""
        return bool(
            self._host.request(
                "context/declare", {"name": name, "type": type, "default": to_json(default)}
            )
        )

    def set(self, name: str, value: Any) -> None:
        """Sets a key of the module (from its manifest or `declare`)."""
        self._host.request("context/set", {"name": name, "value": to_json(value)})

    def evaluate(self, when: str) -> bool:
        return bool(self._host.request("context/evaluate", {"when": when}))

    def watch(self, when: str, callback: Callable[[bool], None]) -> Watch:
        """Calls ``callback(value)`` on the main thread whenever the value of ``when`` changes."""
        result = self._host.request("context/watch", {"when": when})
        watch = Watch(self._host, int(result["watch"]), bool(result["value"]))
        self._host.watches[watch.id] = (watch, callback)
        return watch


class TaskControl:
    """What the work of a task uses: progress, messages, cancellation."""

    def __init__(self, task: Task) -> None:
        self._task = task

    def set_progress(self, fraction: float | None) -> None:
        """From 0 to 1; None while it is not known how far the work is."""
        self._task.host.notify(
            "task/progress",
            {"task": self._task.id, "progress": None if fraction is None else float(fraction)},
        )

    def message(self, text: str, severity: str = "info") -> None:
        """A line for the user: severity "info", "warning" or "error"."""
        self._task.host.notify(
            "task/message", {"task": self._task.id, "text": str(text), "severity": severity}
        )

    @property
    def cancelled(self) -> bool:
        return self._task.cancel_event.is_set()

    def check_cancelled(self) -> None:
        if self.cancelled:
            raise TaskCancelled()


class Task:
    """Work a command goes on with in the background: return it from the command. It starts on a
    thread of its own once the application knows about it."""

    def __init__(self, host: Host, id: str, title: str, work: Callable[[TaskControl], Any]) -> None:
        self.host = host
        self.id = id
        self.title = title
        self.cancel_event = threading.Event()
        self._work = work
        self._thread: threading.Thread | None = None

    def start(self) -> None:
        self._thread = threading.Thread(target=self._run, name=f"task {self.id}", daemon=True)
        self._thread.start()

    def join(self, timeout: float | None = None) -> None:
        if self._thread is not None:
            self._thread.join(timeout)

    def _run(self) -> None:
        params: dict[str, Any]
        try:
            value = self._work(TaskControl(self))
            params = {"task": self.id, "state": "succeeded", "value": to_json(value)}
        except TaskCancelled:
            params = {"task": self.id, "state": "cancelled"}
        except _rpc.ConnectionClosed:
            return
        except Exception as error:  # noqa: BLE001 - the error is the result of the task
            traceback.print_exc()
            params = {"task": self.id, "state": "failed", "error": _describe(error)}
        try:
            self.host.notify("task/finished", params)
        except _rpc.ConnectionClosed:
            pass
        finally:
            self.host.tasks.pop(self.id, None)


class Call(_Reader):
    """A running command of the module: what its handler gets. Changes of the model made through
    it are one step of undo with the command; an exception reverts them."""

    def __init__(self, host: Host, id: int, command: str, args: dict[str, Any]) -> None:
        self._host = host
        self.id = id
        self.command = command
        self.args = args

    def execute(self, command: str, /, **args: Any) -> Any:
        """Runs another command as a part of this one."""
        result = self._host.request(
            "call/execute", {"call": self.id, "command": command, "args": to_json(args)}
        )
        return result.get("value")

    def create_object(
        self,
        type: str,
        owner: str,
        *,
        name: str = "",
        properties: dict[str, Any] | None = None,
        parent: int | None = None,
        dependencies: Iterable[int] | None = None,
        schema_version: int = 1,
        data: Data | None = None,
    ) -> int:
        """Creates an object, with bulk data if given; returns its id."""
        params: dict[str, Any] = {
            "call": self.id,
            "type": type,
            "owner": owner,
            "name": name,
            "properties": to_json(properties) if properties is not None else None,
            "parent": parent,
            "dependencies": [int(d) for d in dependencies] if dependencies is not None else None,
            "schemaVersion": schema_version,
        }
        if data is None:
            return int(self._host.request("call/createObject", params)["id"])
        with _data.outgoing(data, self._host.shared_memory) as message:
            params["data"] = message
            return int(self._host.request("call/createObject", params)["id"])

    def set_data(self, object: int, data: Data) -> None:
        """Replaces the bulk data of an object."""
        with _data.outgoing(data, self._host.shared_memory) as message:
            self._host.request(
                "call/setData", {"call": self.id, "object": int(object), "data": message}
            )

    def run_task(self, title: str, work: Callable[[TaskControl], Any]) -> Task:
        """A task for ``work(control)``; return it from the command. What ``work`` returns is the
        result of the task, an exception fails it, TaskCancelled cancels it."""
        self._host.next_task += 1
        return Task(self._host, f"{self._host.extension_id}/{self._host.next_task}", title, work)


Handler = Callable[..., Any]


class Extension(_Reader):
    """What ``activate(extension)`` of a module gets."""

    def __init__(self, host: Host, id: str, version: str, folder: Path) -> None:
        self._host = host
        self.id = id
        self.version = version
        self.folder = folder
        self.context = ContextKeys(host)
        self.log = logging.getLogger(id)

    def register_command(self, id: str, handler: Handler) -> None:
        """Gives the code of a command declared in the manifest: ``handler(call, **args)``. What
        it returns (or a Task) is the result of the command."""
        self._host.handlers[id] = handler
        self._host.request("contributions/register", {"commands": [id]})

    def command(self, id: str) -> Callable[[Handler], Handler]:
        """Decorator form of `register_command`."""

        def register(handler: Handler) -> Handler:
            self.register_command(id, handler)
            return handler

        return register

    def on_model_changed(self, callback: Callable[[dict[str, Any]], None]) -> Subscription:
        """Calls ``callback(change)`` on the main thread after every step that changed the model:
        ``{"cause", "label", "created", "updated", "removed", "units"}``."""
        host = self._host
        if not host.model_listeners:
            host.request("model/subscribe")
        host.model_listeners.append(callback)

        def close() -> None:
            host.model_listeners.remove(callback)
            if not host.model_listeners:
                host.request("model/unsubscribe")

        return Subscription(close)

    def execute(self, command: str, /, **args: Any) -> Any:
        """Runs a command of the application on its own (not as a part of a command of this
        module), e.g. from background work; returns its value."""
        result = self._host.request("command/execute", {"command": command, "args": to_json(args)})
        return result.get("value")


def _describe(error: BaseException) -> str:
    text = str(error)
    return f"{type(error).__name__}: {text}" if text else type(error).__name__


def isolate_path(folder: Path, site_packages: Iterable[str]) -> None:
    """Puts the folder of the module and its own site-packages first on ``sys.path``, and drops
    what does not belong to the module: entries from PYTHONPATH (except the host's own folder),
    the user's site-packages and the current folder."""
    own = str(Path(__file__).resolve().parent.parent)
    dropped = {
        os.path.normcase(os.path.abspath(p))
        for p in os.environ.get("PYTHONPATH", "").split(os.pathsep)
        if p
    }
    dropped.add(os.path.normcase(os.path.abspath(os.getcwd())))
    user_site = site.getusersitepackages() if site.ENABLE_USER_SITE else None
    if user_site:
        dropped.add(os.path.normcase(os.path.abspath(user_site)))
    kept = []
    for entry in sys.path:
        if not entry:
            continue
        normalized = os.path.normcase(os.path.abspath(entry))
        if normalized in dropped and normalized != os.path.normcase(own):
            continue
        kept.append(entry)
    first = [str(folder), *[str(p) for p in site_packages]]
    if own not in kept:
        first.append(own)
    sys.path[:] = first + [entry for entry in kept if entry not in first]


class Host:
    """The state of the host and what it does with the messages of the application."""

    def __init__(self, connection: _rpc.Connection) -> None:
        self.connection = connection
        self.extension: Extension | None = None
        self.extension_id = ""
        self.module: Any = None
        self.shared_memory = True
        self.handlers: dict[str, Handler] = {}
        self.tasks: dict[str, Task] = {}
        self.next_task = 0
        self.watches: dict[int, tuple[Watch, Callable[[bool], None]]] = {}
        self.model_listeners: list[Callable[[dict[str, Any]], None]] = []
        self.calls = 0
        self.stopping = False

    # --- For the API -----------------------------------------------------------------------------

    def request(self, method: str, params: Any = None) -> Any:
        return _call(self.connection, self, method, params)

    def notify(self, method: str, params: Any = None) -> None:
        self.connection.notify(method, params)

    def release(self, name: str) -> None:
        self.connection.notify("shm/release", {"name": name})

    # --- Messages of the application -------------------------------------------------------------

    def immediate(self, message: dict[str, Any]) -> bool:
        """Handled on the reader thread: pings (they tell the application the host is alive) and
        cancelling of tasks (the main thread may be busy)."""
        method = message.get("method")
        if method == "$/ping" and "id" in message:
            self.connection.respond(message["id"], {})
            return True
        if method == "task/cancel":
            task = self.tasks.get(str((message.get("params") or {}).get("task")))
            if task is not None:
                task.cancel_event.set()
            return True
        return False

    def run(self) -> int:
        while not self.stopping:
            try:
                message = self.connection.next()
            except _rpc.ConnectionClosed:
                break
            try:
                self.dispatch(message)
            except _rpc.ConnectionClosed:
                break
        return 0

    def dispatch(self, message: dict[str, Any]) -> None:
        method = message.get("method", "")
        params = message.get("params") or {}
        id = message.get("id")
        handlers = {
            "initialize": self.initialize,
            "command/execute": self.execute,
            "shutdown": self.shutdown,
        }
        if method == "exit":
            self.stopping = True
            return
        if method == "context/changed":
            entry = self.watches.get(int(params.get("watch", 0)))
            if entry is not None:
                watch, callback = entry
                watch.value = bool(params.get("value"))
                self._safely(callback, watch.value)
            return
        if method == "model/changed":
            for listener in list(self.model_listeners):
                self._safely(listener, params)
            return
        handler = handlers.get(method)
        if handler is None:
            if id is not None:
                self.connection.respond_error(
                    id, _rpc.METHOD_NOT_FOUND, f"unknown method {method!r}"
                )
            return
        try:
            result = handler(params)
        except _rpc.ConnectionClosed:
            raise
        except ModuleError as error:
            if id is not None:
                self.connection.respond_error(id, _rpc.COMMAND_FAILED, str(error))
            return
        except Exception as error:  # noqa: BLE001 - reported to the application
            traceback.print_exc()
            if id is not None:
                self.connection.respond_error(
                    id, _rpc.COMMAND_FAILED, _describe(error), traceback.format_exc()
                )
            return
        if id is None:
            return
        if isinstance(result, Task):
            self.tasks[result.id] = result
            self.connection.respond(id, {"task": {"id": result.id, "title": result.title}})
            result.start()  # After the answer: its notifications come after it.
            return
        self.connection.respond(id, result)

    @staticmethod
    def _safely(callback: Callable[..., Any], *args: Any) -> None:
        try:
            callback(*args)
        except _rpc.ConnectionClosed:
            raise
        except Exception:  # noqa: BLE001 - a listener does not break the host
            traceback.print_exc()

    def initialize(self, params: dict[str, Any]) -> Any:
        version = params.get("protocolVersion")
        if version != PROTOCOL_VERSION:
            raise ValueError(
                f"protocol version {version} is not supported (only {PROTOCOL_VERSION})"
            )
        extension = params["extension"]
        folder = Path(extension["folder"])
        isolate_path(folder, params.get("sitePackages", []))
        self.extension_id = extension["id"]
        self.extension = Extension(self, extension["id"], extension["version"], folder)
        module_name, _, function = extension["entry"].partition(":")
        self.module = importlib.import_module(module_name)
        activate = getattr(self.module, function)
        activate(self.extension)
        return {"capabilities": {"sharedMemory": self.shared_memory}}

    def execute(self, params: dict[str, Any]) -> Any:
        command = params["command"]
        handler = self.handlers.get(command)
        if handler is None:
            raise ModuleError(_rpc.METHOD_NOT_FOUND, f"no code for command {command!r}")
        args = params.get("args") or {}
        call = Call(self, int(params["call"]), command, args)
        result = handler(call, **args)
        if isinstance(result, Task):
            return result
        return {"value": to_json(result)}

    def shutdown(self, params: dict[str, Any]) -> Any:
        for task in list(self.tasks.values()):
            task.cancel_event.set()
        deactivate = getattr(self.module, "deactivate", None) if self.module is not None else None
        if callable(deactivate):
            deactivate()
        return None


def _binary(fd: int) -> None:
    if os.name == "nt":
        import msvcrt

        msvcrt.setmode(fd, os.O_BINARY)


def main() -> int:
    # Standard output is the protocol; everything printed goes to standard error.
    sys.stdout.flush()
    protocol_out = os.dup(1)
    os.dup2(2, 1)
    sys.stdout = sys.stderr
    _binary(0)
    _binary(protocol_out)
    logging.basicConfig(stream=sys.stderr, level=logging.INFO, format="%(name)s: %(message)s")

    connection = _rpc.Connection(0, protocol_out)
    host = Host(connection)
    connection.immediate = host.immediate
    connection.start()
    code = host.run()
    sys.stderr.flush()
    # Threads of tasks and of the module do not keep the process alive after "exit".
    os._exit(code)
