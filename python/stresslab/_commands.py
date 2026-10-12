"""Typed wrappers of commands, made from the registry of the Command Bus.

``stresslab.model.create(type="material", owner="...")`` is the same call as
``stresslab.cmd("model.create", type="material", owner="...")``, and the journal records it that
way. The wrappers are looked up every time, so they follow the commands that modules register
and unregister while the program runs.

A command id becomes a Python path segment by segment: ``-`` becomes ``_``, and a segment that is
a Python keyword gets a trailing ``_`` (``fem.format.ansys-cdb.import`` is
``stresslab.fem.format.ansys_cdb.import_``). If two ids give the same path, the wrapper is
refused as ambiguous; ``stresslab.cmd`` still runs either command.
"""

from __future__ import annotations

import inspect
import keyword
from collections.abc import Callable
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from _stresslab import CommandSpec, Document

# How the argument types of the Command Bus read in a signature.
_ANNOTATIONS = {
    "boolean": "bool",
    "integer": "int",
    "number": "float",
    "string": "str",
    "path": "str | os.PathLike[str]",
    "object": "stresslab.Object | int",
    "list": "list",
    "dict": "dict",
    "any": "Any",
}


def python_name(segment: str) -> str:
    """The Python name of one segment of a command id."""
    name = segment.replace("-", "_")
    return name + "_" if keyword.iskeyword(name) else name


def python_path(command_id: str) -> tuple[str, ...]:
    """The Python path of a command id: ``"a.b-c"`` -> ``("a", "b_c")``."""
    return tuple(python_name(segment) for segment in command_id.split("."))


def signature(spec: CommandSpec) -> inspect.Signature:
    """Keyword-only parameters in the order of the spec. An optional argument shows its default,
    or None if the command has none (None means "omitted")."""
    parameters = []
    for arg in spec.args:
        if arg.required:
            default: Any = inspect.Parameter.empty
        else:
            default = arg.default if arg.has_default else None
        parameters.append(
            inspect.Parameter(
                arg.name,
                inspect.Parameter.KEYWORD_ONLY,
                default=default,
                annotation=_ANNOTATIONS.get(arg.type, "Any"),
            )
        )
    return inspect.Signature(parameters, return_annotation="Any")


def make_wrapper(get_document: Callable[[], Document], spec: CommandSpec) -> Callable[..., Any]:
    """A function that runs the command with checked keyword arguments."""
    command_id = spec.id
    sig = signature(spec)

    def wrapper(*args: Any, **kwargs: Any) -> Any:
        # Arguments the caller did not pass stay omitted: defaults are not written to the journal.
        bound = sig.bind(*args, **kwargs)
        return get_document().cmd(command_id, **bound.arguments)

    path = python_path(command_id)
    wrapper.__name__ = path[-1]
    wrapper.__qualname__ = "stresslab." + ".".join(path)
    wrapper.__module__ = "stresslab"
    wrapper.__signature__ = sig  # type: ignore[attr-defined]
    wrapper.__doc__ = (
        f"Runs the command {command_id!r} of the module {spec.owner!r}.\n\n"
        f"Same as stresslab.cmd({command_id!r}, ...); the journal records that call."
    )
    return wrapper


class CommandNamespace:
    """Commands whose ids start with a prefix: ``stresslab.model`` has ``create``, ``rename``...

    It is callable if a command has exactly this path too.
    """

    def __init__(self, get_document: Callable[[], Document], path: tuple[str, ...]) -> None:
        self._get_document = get_document
        self._path = path

    def _matches(self) -> list[str]:
        """Ids of the commands at this path or below it."""
        depth = len(self._path)
        return [
            command_id
            for command_id in self._get_document().command_ids()
            if python_path(command_id)[:depth] == self._path
        ]

    def _command(self) -> str | None:
        exact = [i for i in self._matches() if python_path(i) == self._path]
        if len(exact) > 1:
            raise AttributeError(
                f"stresslab.{'.'.join(self._path)} is ambiguous: commands {', '.join(exact)}; "
                f"run one with stresslab.cmd"
            )
        return exact[0] if exact else None

    def __getattr__(self, name: str) -> Any:
        if name.startswith("__"):
            raise AttributeError(name)
        return resolve(self._get_document, (*self._path, name))

    def __dir__(self) -> list[str]:
        depth = len(self._path)
        return sorted(
            {python_path(i)[depth] for i in self._matches() if len(python_path(i)) > depth}
        )

    def __call__(self, *args: Any, **kwargs: Any) -> Any:
        command_id = self._command()
        if command_id is None:
            raise TypeError(f"stresslab.{'.'.join(self._path)} is not a command")
        spec = self._get_document().command_spec(command_id)
        return make_wrapper(self._get_document, spec)(*args, **kwargs)

    def __repr__(self) -> str:
        return f"<stresslab commands {'.'.join(self._path)}>"


def resolve(get_document: Callable[[], Document], path: tuple[str, ...]) -> Any:
    """The wrapper of the command at `path`, or a namespace if commands lie below it."""
    namespace = CommandNamespace(get_document, path)
    matches = namespace._matches()
    if not matches:
        raise AttributeError(f"no command stresslab.{'.'.join(path)}")
    command_id = namespace._command()
    deeper = any(len(python_path(i)) > len(path) for i in matches)
    if command_id is not None and not deeper:
        return make_wrapper(get_document, get_document().command_spec(command_id))
    return namespace


class Commands:
    """The root of the wrappers: ``commands.model.create(...)``. Bound to a document, or to the
    active document if made with ``stresslab.active_document``."""

    def __init__(self, get_document: Callable[[], Document]) -> None:
        self._get_document = get_document

    def __getattr__(self, name: str) -> Any:
        if name.startswith("__"):
            raise AttributeError(name)
        return resolve(self._get_document, (name,))

    def __dir__(self) -> list[str]:
        return sorted({python_path(i)[0] for i in self._get_document().command_ids()})

    def __repr__(self) -> str:
        return "<stresslab commands>"
