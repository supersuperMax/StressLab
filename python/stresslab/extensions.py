"""The modules of the application (docs/architecture.md §4.1–4.6), as the module manager sees
them::

    import stresslab

    stresslab.extensions.disable("fem.demo")   # its buttons, stage and tab go away
    stresslab.extensions.enable("fem.demo")    # and come back

Enabling and disabling a module is written to the session journal as these same calls, so the
journal replays with the same modules; it is not a step of undo and is refused inside a command or
a transaction. Modules are managed by the application: in a plain Python process, and in
``stresslab --python``, every function raises `stresslab.ExtensionError`.
"""

from __future__ import annotations

import _stresslab
from _stresslab import ExtensionError, ExtensionInfo

_native = _stresslab.extensions

__all__ = ["ExtensionError", "ExtensionInfo", "disable", "enable", "get", "installed"]


def installed() -> list[ExtensionInfo]:
    """The installed modules, sorted by id."""
    return _native.installed()


def get(id: str) -> ExtensionInfo:
    """The installed module; ExtensionError if there is none."""
    return _native.get(id)


def enable(id: str) -> list[str]:
    """Enables a module and the modules it requires; returns those it enabled, required first
    (empty if it was enabled already)."""
    return _native.enable(id)


def disable(id: str) -> list[str]:
    """Disables a module and the modules that need it; returns those it disabled. Its objects
    stay in the model, opaque, until it is enabled again."""
    return _native.disable(id)
