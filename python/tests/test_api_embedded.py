"""The same API inside the application and in a plain Python process (task 1.8). The suite runs
twice: ``python -m pytest`` and ``stresslab --python -m pytest`` (see tests/CMakeLists.txt)."""

from __future__ import annotations

import sys

import pytest
import stresslab

embedded_only = pytest.mark.skipif(not stresslab.embedded, reason="needs the stresslab executable")
plain_only = pytest.mark.skipif(stresslab.embedded, reason="needs a plain Python process")


def test_the_module_reports_where_it_runs() -> None:
    assert stresslab.embedded == ("_stresslab" in sys.builtin_module_names)
    assert stresslab.__version__.count(".") == 2
    assert sys.version_info[:2] == (3, 12)


@embedded_only
def test_python_works_with_the_document_of_the_application() -> None:
    attached = stresslab.attached_document()
    assert attached is not None and attached.is_attached
    assert "model.create" in attached.command_ids()
    # The application keeps the keys of the core of its document.
    assert "history.canUndo" in stresslab.context
    assert "model.hasMesh" in stresslab.context


@embedded_only
def test_the_python_of_the_build_is_used() -> None:
    import numpy  # site-packages of the build Python are on the path

    assert numpy.__version__


@plain_only
def test_a_plain_process_has_a_document_of_its_own() -> None:
    assert stresslab.attached_document() is None
    document = stresslab.active_document()
    assert document.is_open and not document.is_attached
    assert "_stresslab" not in sys.builtin_module_names
