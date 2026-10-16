"""The history of the project of the active document: versions, variants, labels and changes put
aside (docs/architecture.md §3.1, docs/interface-design.md §12)::

    import stresslab
    from stresslab import history

    stresslab.project.save_as("wing")
    history.commit("Two ribs")                        # saves and makes a version
    history.branch("Composite skin")                  # a variant at the current version
    history.checkout("Composite skin")                # work on it
    stresslab.model.set_property(object=skin, key="thickness", value=3.0)
    history.commit("Thickness 3 mm")
    history.checkout("main")                          # back to the main variant
    print(history.diff("main", "Composite skin"))     # "Section skin: Thickness 2 → 3 mm"
    history.restore("main~2", types=["material"])     # only the materials, as two versions back
    history.restore_object(material, "main~2")        # one object as it was two versions back
    history.restore_version("main~2")                 # the whole model, as a new version

Versions are named by their ids (40 hex digits, or the start of one), by a variant or a label,
each optionally with ``~N``: N versions back. Actions are written to the session journal as these
same calls, with the names as given; ``restore_object`` and ``restore`` are the commands
``history.restore_object`` and ``history.restore_objects``, each one step of undo. Comparing
(``diff``) only reads the history and is not written to the journal. Errors raise
``stresslab.ProjectError`` whose ``code`` says what went wrong: "uncommitted_changes",
"unknown_version", "detached"...
"""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from typing import Any

import _stresslab

_native = _stresslab.history

__all__ = [
    "Diff",
    "ObjectChange",
    "PropertyChange",
    "Version",
    "author",
    "branch",
    "branches",
    "checkout",
    "commit",
    "current_branch",
    "diff",
    "current_version",
    "has_changes",
    "info",
    "journal",
    "log",
    "resolve",
    "restore",
    "restore_object",
    "restore_version",
    "set_author",
    "snapshot",
    "stash",
    "stash_drop",
    "stash_pop",
    "stashes",
    "tag",
    "tags",
]


@dataclass(frozen=True)
class Version:
    """A version of the project."""

    id: str
    parents: tuple[str, ...]
    message: str
    summary: str
    author: str
    email: str
    time: datetime
    #: "manual", "automatic" (made before something risky), "restore" or "stash".
    kind: str
    #: For an automatic version: the command it was made before, if any.
    before: str
    #: For a restore: the id of the version restored.
    restored: str
    #: For changes put aside: the variant they were put aside on.
    branch: str

    @property
    def automatic(self) -> bool:
        return self.kind == "automatic"


def _version(data: dict[str, Any]) -> Version:
    zone = timezone(timedelta(minutes=data["time_offset"]))
    return Version(
        id=data["id"],
        parents=tuple(data["parents"]),
        message=data["message"],
        summary=data["summary"],
        author=data["author"],
        email=data["email"],
        time=datetime.fromtimestamp(data["time"], zone),
        kind=data["kind"],
        before=data["before"],
        restored=data["restored"],
        branch=data["branch"],
    )


@dataclass(frozen=True)
class PropertyChange:
    """A property of an object that differs between two versions."""

    #: The path of the property, as forms and ``model.set_property`` name it: "solver/tolerance".
    pointer: str
    #: The value before; None if the property is new (``had_before`` tells it from a null value).
    before: Any
    after: Any
    had_before: bool
    has_after: bool


@dataclass(frozen=True)
class ObjectChange:
    """An object that differs between two versions."""

    id: int
    #: "added", "removed" or "changed".
    kind: str
    owner: str
    type: str
    #: The name in the later version, or in the earlier one if the object was removed.
    name: str
    #: For a changed object: the properties that differ, by pointer.
    properties: tuple[PropertyChange, ...]
    #: Everything about the change as JSON: "renamed", "parent", "schemaVersion",
    #: "dependencies", "data" of a changed object; all members of an added or removed one.
    details: dict[str, Any]


@dataclass(frozen=True)
class Diff:
    """How the model of one version differs from that of another, in the terms of the model."""

    objects: tuple[ObjectChange, ...]
    #: (before, after) if the units of the model changed: "mm-t-s-degC".
    units: tuple[str, str] | None
    #: The diff in words, a line per change, in the language of the application.
    lines: tuple[str, ...]

    @property
    def empty(self) -> bool:
        """True if the two models are the same."""
        return not self.objects and self.units is None

    def find(self, object: int | Any) -> ObjectChange | None:  # noqa: A002
        """The change of an object (an id or an object with ``id``), or None."""
        target = object if isinstance(object, int) else object.id
        return next((change for change in self.objects if change.id == target), None)

    def __str__(self) -> str:
        return "; ".join(self.lines)


def _object_change(data: dict[str, Any]) -> ObjectChange:
    properties: tuple[PropertyChange, ...] = ()
    if data["kind"] == "changed":
        properties = tuple(
            PropertyChange(
                pointer=item["pointer"],
                before=item.get("before"),
                after=item.get("after"),
                had_before="before" in item,
                has_after="after" in item,
            )
            for item in data.get("properties", [])
        )
    common = ("id", "kind", "owner", "type", "name")
    return ObjectChange(
        id=data["id"],
        kind=data["kind"],
        owner=data["owner"],
        type=data["type"],
        name=data["name"],
        properties=properties,
        details={key: value for key, value in data.items() if key not in common},
    )


def current_branch() -> str | None:
    """The variant the next version goes to; None while a version is viewed on its own."""
    return _native.current_branch()


def current_version() -> str | None:
    """The id of the version the model is from; None before the first version."""
    return _native.current_version()


def has_changes() -> bool:
    """True if the model differs from its version: changes not in the history yet."""
    return _native.has_changes()


def log(
    target: str | None = None, *, all: bool = False, limit: int = 0
) -> list[Version]:  # noqa: A002
    """Versions newest first: of `target`, of the current version, or of all variants and labels."""
    return [_version(data) for data in _native.log(target, all, limit)]


def info(target: str) -> Version:
    """One version."""
    return _version(_native.info(target))


def journal(target: str) -> str:
    """The journal of a version: what was done since the version before it, as a script."""
    return _native.journal(target)


def resolve(target: str) -> str:
    """The id of a version named by a variant, a label, an id or the start of one, with ``~N``."""
    return _native.resolve(target)


def branches() -> dict[str, str]:
    """Variants by name with the ids of their last versions."""
    return _native.branches()


def tags() -> dict[str, str]:
    """Labels by name with the ids of their versions."""
    return _native.tags()


def stashes() -> list[Version]:
    """Changes put aside, newest first."""
    return [_version(data) for data in _native.stashes()]


def diff(before: str, after: str | None = None) -> Diff:
    """How the model of `after` differs from that of `before`; without `after`, the model now with
    its changes not in the history yet. Versions are named as in ``resolve``."""
    data = _native.diff(before, after)
    units = data.get("units")
    return Diff(
        objects=tuple(_object_change(item) for item in data["objects"]),
        units=(units["before"], units["after"]) if units else None,
        lines=tuple(data["lines"]),
    )


def commit(message: str) -> str:
    """Saves the project and makes a version of it on the current variant; returns its id."""
    return _native.commit(message)


def snapshot(reason: str = "") -> str:
    """An automatic version if the model has changes; returns the id of the version the model now
    is, so that a result can be tied to it."""
    return _native.snapshot(reason)


def branch(name: str, at: str | None = None) -> None:
    """A new variant at `at` (the current version if None); the current variant stays."""
    _native.branch(name, at)


def checkout(target: str) -> None:
    """Opens a variant to work on, or a label or a version to look at."""
    _native.checkout(target)


def tag(name: str, at: str | None = None) -> None:
    """A label at `at` (the current version if None)."""
    _native.tag(name, at)


def stash(message: str = "") -> None:
    """Puts the changes aside and brings back the current version."""
    _native.stash(message)


def stash_pop(index: int = 0) -> None:
    """Brings back the changes put aside at `index` (0 is the newest) and forgets them."""
    _native.stash_pop(index)


def stash_drop(index: int = 0) -> None:
    """Forgets the changes put aside at `index`."""
    _native.stash_drop(index)


def restore_version(target: str) -> str:
    """A new version with the model of `target`; nothing is lost. Returns its id."""
    return _native.restore_version(target)


def restore_object(object: int | Any, version: str) -> int:  # noqa: A002 - the name of the argument
    """One object as it was in `version`, in place of what it is now or back after its removal;
    one step of undo."""
    from . import cmd

    target = object if isinstance(object, int) else object.id
    return cmd("history.restore_object", object=target, version=version)


def restore(
    version: str,
    *,
    types: list[str] | None = None,
    objects: list[int | Any] | None = None,
) -> list[int]:
    """A part of the model as it was in `version`: the objects of `types` ("material", or
    "owner/material" for those of one module) and the `objects` named. Changed ones get back what
    they had, removed ones come back with their ids, ones made since are removed; the rest of the
    model stays. One step of undo. Returns the ids of the objects it changed."""
    from . import cmd

    arguments: dict[str, Any] = {"version": version}
    if types is not None:
        arguments["types"] = list(types)
    if objects is not None:
        arguments["objects"] = [item if isinstance(item, int) else item.id for item in objects]
    return cmd("history.restore_objects", **arguments)


def set_author(name: str, email: str) -> None:
    """Who signs new versions."""
    _native.set_author(name, email)


def author() -> tuple[str, str]:
    """Who signs new versions: (name, email)."""
    return _native.author()
