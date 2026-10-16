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
    history.restore_object(material, "main~2")        # one object as it was two versions back
    history.restore_version("main~2")                 # the whole model, as a new version

Versions are named by their ids (40 hex digits, or the start of one), by a variant or a label,
each optionally with ``~N``: N versions back. Actions are written to the session journal as these
same calls, with the names as given; ``restore_object`` is the command
``history.restore_object`` and one step of undo. Errors raise ``stresslab.ProjectError`` whose
``code`` says what went wrong: "uncommitted_changes", "unknown_version", "detached"...
"""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from typing import Any

import _stresslab

_native = _stresslab.history

__all__ = [
    "Version",
    "author",
    "branch",
    "branches",
    "checkout",
    "commit",
    "current_branch",
    "current_version",
    "has_changes",
    "info",
    "journal",
    "log",
    "resolve",
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


def set_author(name: str, email: str) -> None:
    """Who signs new versions."""
    _native.set_author(name, email)


def author() -> tuple[str, str]:
    """Who signs new versions: (name, email)."""
    return _native.author()
