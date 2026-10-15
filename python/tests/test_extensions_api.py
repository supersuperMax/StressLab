"""``stresslab.extensions`` where the application lends no Extension Manager: in a plain Python
process and in ``stresslab --python`` modules are not managed from Python (task 1.15). With the
manager of the application it is tested by tests/unit/python/extensions_test.cpp and
python/tests/test_demo.py.
"""

from __future__ import annotations

from collections.abc import Callable
from typing import Any

import pytest
import stresslab


@pytest.mark.parametrize(
    "call",
    [
        stresslab.extensions.installed,
        lambda: stresslab.extensions.get("fem.demo"),
        lambda: stresslab.extensions.enable("fem.demo"),
        lambda: stresslab.extensions.disable("fem.demo"),
    ],
    ids=["installed", "get", "enable", "disable"],
)
def test_without_the_application_modules_are_not_managed(call: Callable[[], Any]) -> None:
    with pytest.raises(stresslab.ExtensionError, match="managed by the application") as raised:
        call()
    assert raised.value.code == "invalid_state"
    assert isinstance(raised.value, stresslab.Error)


def test_the_api_is_a_submodule_with_its_names() -> None:
    import stresslab.extensions as extensions

    assert extensions is stresslab.extensions
    assert stresslab.ExtensionError is extensions.ExtensionError
    assert set(extensions.__all__) == {
        "ExtensionError",
        "ExtensionInfo",
        "disable",
        "enable",
        "get",
        "installed",
    }
