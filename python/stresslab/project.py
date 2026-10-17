"""The project folder of the active document (.slproj, docs/architecture.md §3.1)::

    import stresslab

    stresslab.cmd("model.create", type="note", owner="fem.demo", name="Wing")
    stresslab.project.save_as("wing")           # writes wing.slproj
    stresslab.project.save()                    # only the files that changed
    stresslab.project.open("wing.slproj")       # in place of the document
    stresslab.project.clean()                   # removes blocks of data saved before
    stresslab.project.pack("for a colleague")   # one file: for a colleague.slpack
    stresslab.project.unpack("wing.slpack", "wing")  # into wing.slproj, and opens it

Opening, saving, cleaning, packing, unpacking and closing are written to the session journal as
these same calls, so a journal replays them; they are refused inside a command or a transaction.
Errors of the folder raise `stresslab.ProjectError`, whose `code` says what went wrong
("not_a_project", "invalid_file"...) and `file` and `line` where.
"""

from __future__ import annotations

import os
from pathlib import Path

import _stresslab
from _stresslab import ProjectError

_native = _stresslab.project

__all__ = [
    "ProjectError",
    "clean",
    "close",
    "folder",
    "is_modified",
    "open",
    "pack",
    "save",
    "save_as",
    "unpack",
]


def open(folder: str | os.PathLike[str]) -> None:  # noqa: A001 - the name of the action
    """Opens the project folder in place of the active document."""
    _native.open(folder)


def save() -> None:
    """Saves to the project folder; ProjectError with code "no_folder" if there is none yet."""
    _native.save()


def save_as(folder: str | os.PathLike[str]) -> None:
    """Saves to `folder` (".slproj" is added if missing), which becomes the project folder."""
    _native.save_as(folder)


def clean() -> dict[str, int]:
    """The "Clean project" command: removes the blocks of bulk data in the project folder that
    the saved project no longer refers to, such as the data of meshes saved before. Returns
    ``{"removed": files, "freed_bytes": ..., "kept": blocks, "kept_bytes": ...}``; ProjectError
    with code "no_folder" for a project never saved."""
    return _native.clean()


def pack(package: str | os.PathLike[str], history: bool = True) -> dict[str, int]:
    """Saves the project and packs it into one file to send (".slpack" is added if missing): a
    zip of the project folder, with its versions, variants, labels and pinned results, or with
    ``history=False`` the model alone; never the cache of results. Returns ``{"files": ...,
    "bytes": ..., "package_bytes": ...}``; ProjectError with code "no_folder" for a project never
    saved."""
    return _native.pack(package, history)


def unpack(package: str | os.PathLike[str], folder: str | os.PathLike[str]) -> dict[str, int]:
    """Unpacks a .slpack into ``folder`` (".slproj" is added if missing), which must be new or
    empty, and opens that project in place of the active document. ProjectError with code
    "invalid_archive" for a file that is not a package of a project or is damaged."""
    return _native.unpack(package, folder)


def close() -> None:
    """Replaces the active document with a new empty one without a folder."""
    _native.close()


def folder() -> Path | None:
    """The project folder; None for a project never saved."""
    return _native.folder()


def is_modified() -> bool:
    """True if the model changed since the project was opened, saved or closed."""
    return _native.is_modified()
