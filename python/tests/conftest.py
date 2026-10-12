"""Makes the `stresslab` package of the build importable for ``python -m pytest``.

Inside ``stresslab --python -m pytest`` the native module is built in and the package is on the
path already. Otherwise STRESSLAB_PYTHON_SITE names the folder the build put the package in
(``build/<preset>/python/site``); without it the most recently built one under ``build/`` is used.
"""

from __future__ import annotations

import importlib.util
import os
import sys
from collections.abc import Iterator
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]


def _built_site() -> Path | None:
    configured = os.environ.get("STRESSLAB_PYTHON_SITE")
    if configured:
        return Path(configured)
    modules = list((REPO_ROOT / "build").glob("*/python/site/_stresslab*"))
    if not modules:
        return None
    return max(modules, key=lambda module: module.stat().st_mtime).parent


if importlib.util.find_spec("_stresslab") is None:
    _site = _built_site()
    if _site is not None:
        sys.path.insert(0, str(_site))


@pytest.fixture
def doc() -> Iterator[object]:
    """A new active document with the objects of "test.module" editable; the document active
    before comes back afterwards."""
    import stresslab

    previous = stresslab.active_document()
    document = stresslab.new_document()
    document.set_owner_enabled("test.module")
    try:
        yield document
    finally:
        stresslab.set_active_document(previous)
