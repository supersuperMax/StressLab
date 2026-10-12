"""Helpers of the tests of the Python API (task 1.8)."""

from __future__ import annotations

from typing import Any

import stresslab

TEST_OWNER = "test.module"


def snapshot(document: stresslab.Document) -> dict[str, Any]:
    """Everything observable about a document; two documents with the same snapshot are the same
    model (like `dump` of the C++ tests)."""

    def ids(objects: list[stresslab.Object]) -> list[int]:
        return [item.id for item in objects]

    return {
        "units": document.units,
        "roots": ids(document.roots()),
        "objects": [
            {
                "id": item.id,
                "type": item.type,
                "owner": item.owner,
                "schema_version": item.schema_version,
                "name": item.name,
                "properties": item.properties,
                "parent": item.parent.id if item.parent is not None else 0,
                "children": ids(item.children),
                "dependencies": ids(item.dependencies),
                "missing": ids(item.missing_dependencies),
                "dependents": ids(item.dependents),
                "has_data": item.data is not None,
            }
            for item in document.objects()
        ],
    }


def register_load(document: stresslab.Document) -> None:
    """``test.load`` as tests/unit/core/command/journal_test_support.h has it, written in Python:
    creates an object named after the file."""

    def load(
        ctx: stresslab.CommandContext, file: Any, options: dict[str, Any] | None = None
    ) -> stresslab.Object:
        properties = {key: float(value) for key, value in (options or {}).items()}
        return ctx.create_object("import", TEST_OWNER, name=file.stem, properties=properties)

    document.register_command(
        "test.load",
        load,
        owner=TEST_OWNER,
        args=[
            stresslab.ArgSpec("file", "path"),
            stresslab.ArgSpec("options", "dict", required=False),
        ],
    )


def new_session() -> stresslab.Document:
    """A document as the journal tests have it: the test module enabled and ``test.load``."""
    document = stresslab.Document()
    document.set_owner_enabled(TEST_OWNER)
    register_load(document)
    return document
