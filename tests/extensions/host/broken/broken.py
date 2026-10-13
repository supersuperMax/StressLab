"""activate() fails."""

from __future__ import annotations


def activate(extension) -> None:
    raise RuntimeError("cannot activate on purpose")
