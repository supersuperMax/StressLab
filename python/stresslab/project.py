"""The project folder of the active document (.slproj, docs/architecture.md §3.1)::

    import stresslab

    stresslab.cmd("model.create", type="note", owner="fem.demo", name="Wing")
    stresslab.project.save_as("wing")           # writes wing.slproj
    stresslab.project.save()                    # only the files that changed
    stresslab.project.open("wing.slproj")       # in place of the document

Opening, saving and closing are written to the session journal as these same calls, so a journal
replays them; they are refused inside a command or a transaction. Errors of the folder raise
`stresslab.ProjectError`, whose `code` says what went wrong ("not_a_project", "invalid_file"...)
and `file` and `line` where.
"""

from __future__ import annotations

import os
from pathlib import Path

import _stresslab
from _stresslab import ProjectError

_native = _stresslab.project

__all__ = ["ProjectError", "close", "folder", "is_modified", "open", "save", "save_as"]


def open(folder: str | os.PathLike[str]) -> None:  # noqa: A001 - the name of the action
    """Opens the project folder in place of the active document."""
    _native.open(folder)


def save() -> None:
    """Saves to the project folder; ProjectError with code "no_folder" if there is none yet."""
    _native.save()


def save_as(folder: str | os.PathLike[str]) -> None:
    """Saves to `folder` (".slproj" is added if missing), which becomes the project folder."""
    _native.save_as(folder)


def close() -> None:
    """Replaces the active document with a new empty one without a folder."""
    _native.close()


def folder() -> Path | None:
    """The project folder; None for a project never saved."""
    return _native.folder()


def is_modified() -> bool:
    """True if the model changed since the project was opened, saved or closed."""
    return _native.is_modified()
