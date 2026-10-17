"""The cache of results of the project of the active document (docs/architecture.md §3.1)::

    import stresslab
    from stresslab import history, results

    version = history.snapshot("Before solving")         # the version the result belongs to
    key = results.key("calculix", "2.22")                 # model + mesh + solver and its version
    found = results.find(key)                             # computed before? then no run is needed
    if found is None:
        ...                                               # run the solver into "out/"
        found = results.put(key, "out", version, "calculix", "2.22")
    print(found.folder)                                   # the files of the result
    results.pin(key)                                      # kept in the history and in packages
    results.prune_inactive()                              # frees the results of other variants

Results are not in the history: they are kept in ``.fem/cache/results`` of the project folder
under their key, each with the version of the model it was computed from, and the ones used
longest ago are removed when the cache grows past its limit (``set_limit``; the setting
"results.cacheLimit" in the application). A pinned result is kept in the history: it is never
removed to make room, travels in packages with the history and comes back to the cache when it
is found. For the core a result is a set of files; reading them is the work of the modules.

Pinning, unpinning, removing and pruning are written to the session journal as these same calls;
keys, finding, listing and putting are not. Errors raise ``stresslab.ProjectError`` whose
``code`` says what went wrong: "no_folder", "unknown_result", "result_mismatch"...
"""

from __future__ import annotations

import os
from dataclasses import dataclass
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

import _stresslab

_native = _stresslab.results

__all__ = [
    "Result",
    "ResultFile",
    "find",
    "key",
    "limit",
    "list",
    "pin",
    "prune_inactive",
    "put",
    "remove",
    "set_limit",
    "unpin",
    "usage",
]


@dataclass(frozen=True)
class ResultFile:
    """A file of a result."""

    #: Relative to the folder of the result, with "/".
    path: str
    size: int
    #: 64 hex digits of its bytes.
    sha256: str


@dataclass(frozen=True)
class Result:
    """A result in the cache or kept in the history."""

    key: str
    #: The version of the model it was computed from.
    version: str
    solver: str
    solver_version: str
    inputs: dict[str, str]
    created: datetime
    used: datetime
    files: tuple[ResultFile, ...]
    #: The size of its files together.
    bytes: int
    #: Its files are in the cache.
    cached: bool
    #: It is kept in the history.
    pinned: bool
    #: Where its files are, if they are in the cache.
    folder: Path | None


def _result(data: dict[str, Any]) -> Result:
    return Result(
        key=data["key"],
        version=data["version"],
        solver=data["solver"],
        solver_version=data["solver_version"],
        inputs=dict(data["inputs"]),
        created=datetime.fromtimestamp(data["created"], UTC),
        used=datetime.fromtimestamp(data["used"], UTC),
        files=tuple(
            ResultFile(item["path"], item["size"], item["sha256"]) for item in data["files"]
        ),
        bytes=data["bytes"],
        cached=data["cached"],
        pinned=data["pinned"],
        folder=data["folder"],
    )


def key(
    solver: str,
    solver_version: str = "",
    inputs: dict[str, str] | None = None,
    version: str | None = None,
) -> str:
    """The key of the results of the solver for the model now (its changes not in the history
    included) or, with ``version``, for the model of that version: 64 hex digits, the same on
    every computer. ``inputs`` are options of the run that are not a part of the model."""
    return _native.key(solver, solver_version, inputs, version)


def put(
    key: str,
    folder: str | os.PathLike[str],
    version: str,
    solver: str,
    solver_version: str = "",
    inputs: dict[str, str] | None = None,
) -> Result:
    """Puts the files of ``folder`` in the cache as the result of ``key``, computed by the solver
    from ``version``; ProjectError with code "result_mismatch" if the key is not that of the model
    of that version."""
    return _result(_native.put(key, folder, version, solver, solver_version, inputs))


def find(key: str) -> Result | None:
    """The result of the key, which becomes the one used last; a pinned result comes back to the
    cache from the history. None if there is none."""
    data = _native.find(key)
    return None if data is None else _result(data)


def list() -> tuple[Result, ...]:  # noqa: A001 - the name of the action
    """Every result, in the cache or pinned, used last first."""
    return tuple(_result(data) for data in _native.list())


def pin(key: str) -> None:
    """Keeps the result, which must be in the cache, in the history."""
    _native.pin(key)


def unpin(key: str) -> None:
    """The result is not kept in the history any more; its files stay in the cache."""
    _native.unpin(key)


def remove(key: str) -> bool:
    """Removes the files of the result from the cache; False if they were not there. A pinned
    result stays in the history."""
    return _native.remove(key)


def prune_inactive() -> dict[str, int]:
    """Removes the results of inactive variants: every result in the cache whose version is not
    one of the current variant. Returns ``{"removed": ..., "freed_bytes": ...}``."""
    return _native.prune_inactive()


def limit() -> int:
    """The size in bytes the files in the cache may take; 0 is no limit."""
    return _native.limit()


def set_limit(bytes: int) -> None:  # noqa: A002 - as the setting names it
    """Sets the size in bytes the files in the cache may take; 0 is no limit."""
    _native.set_limit(bytes)


def usage() -> int:
    """The size in bytes the files in the cache take now."""
    return _native.usage()
