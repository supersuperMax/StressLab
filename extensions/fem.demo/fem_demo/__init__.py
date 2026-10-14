"""The code of the module fem.demo (see its manifest and docs/extension-guide.md).

It runs in the extension host, a process of its own, and sees the application through
`stresslab_host`: it reads the model and the settings, and changes the model only through the
`Call` its command gets, so that the change is one step of undo and one line of the journal.
"""

from __future__ import annotations

from stresslab_host import Call, Extension

#: The owner of the objects of the module: its id. While the module is disabled they stay in the
#: model as opaque objects.
OWNER = "fem.demo"
NOTE = "demo_note"


def activate(extension: Extension) -> None:
    """Called once, on the first command of the module (``activation`` in the manifest)."""

    @extension.command("demo.add_note")
    def add_note(call: Call, text: str = "") -> int:
        """Adds a note to the model and returns its id. Without a text the note gets the one of
        the setting "demo.defaultText"."""
        if not text:
            text = call.setting("demo.defaultText")
        number = len(call.objects(type=NOTE, owner=OWNER)) + 1
        return call.create_object(
            NOTE, OWNER, name=f"#{number}", properties={"text": text, "priority": "normal"}
        )
