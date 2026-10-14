"""Acceptance of stage 1 (task 1.15): the demonstration module fem.demo, which the application
ships, goes away and comes back through ``stresslab.extensions`` in ``stresslab --headless``,
while its notes stay in the model as opaque objects; the journal records it and replays it.

The real executable runs the module's code in the real extension host. The window and its console
are tested by tests/unit/shell/demo_acceptance_test.cpp and tests/unit/python/extensions_test.cpp.
"""

from __future__ import annotations

import os
import subprocess
from pathlib import Path

import pytest
import stresslab

REPO_ROOT = Path(__file__).resolve().parents[2]

pytestmark = pytest.mark.skipif(
    stresslab.embedded, reason="starts the executable itself; the plain run covers it"
)


def _executable() -> Path | None:
    configured = os.environ.get("STRESSLAB_EXECUTABLE")
    if configured:
        return Path(configured)
    built = [*REPO_ROOT.glob("build/*/app/stresslab.exe"), *REPO_ROOT.glob("build/*/app/stresslab")]
    return max(built, key=lambda path: path.stat().st_mtime) if built else None


@pytest.fixture
def folder(tmp_path: Path) -> Path:
    executable = _executable()
    if executable is None or not executable.exists():
        pytest.skip("the stresslab executable is not built")
    result = tmp_path / "приёмка demo"
    result.mkdir()
    return result


def run(folder: Path, script: Path, *options: str | Path) -> subprocess.CompletedProcess[str]:
    executable = _executable()
    assert executable is not None
    result = subprocess.run(
        [executable, "--headless", "--log-level", "warning", *options, "run-script", script],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        stdin=subprocess.DEVNULL,
        cwd=folder,
        timeout=300,
    )
    assert (
        result.returncode == 0
    ), f"exit code {result.returncode}\n--- stdout\n{result.stdout}\n--- stderr\n{result.stderr}"
    return result


ACCEPTANCE = """\
import stresslab

document = stresslab.active_document()
assert stresslab.extensions.get("fem.demo").enabled

# Enabled: the command of the module works; its code reads the setting "demo.defaultText".
note = document[stresslab.demo.add_note()]
assert note.type == "demo_note" and note.owner == "fem.demo" and note.name == "#1", note
assert isinstance(note.properties["text"], str) and note.properties["text"], note.properties
assert not note.is_opaque
print("enabled:", note.name)

# Disabled: its command is gone, its note stays in the model, opaque.
assert stresslab.extensions.disable("fem.demo") == ["fem.demo"]
assert stresslab.extensions.get("fem.demo").state == "disabled"
assert "demo.add_note" not in document.command_ids()
try:
    stresslab.cmd("demo.add_note")
except stresslab.CommandError as error:
    assert error.code == "unknown_command", error.code
else:
    raise AssertionError("the command of a disabled module ran")
assert note.exists and note.is_opaque
assert len(document) == 1
print("disabled: opaque", note.is_opaque)

# Enabled again: the note is editable and the command works, in a new extension host.
assert stresslab.extensions.enable("fem.demo") == ["fem.demo"]
assert not note.is_opaque
second = document[stresslab.demo.add_note(text="вторая")]
assert second.name == "#2" and second.properties["text"] == "вторая", second
assert stresslab.extensions.get("fem.demo").state == "active"
print("enabled again:", [item.name for item in document.objects()])
"""


def test_the_demo_module_goes_away_and_comes_back(folder: Path) -> None:
    script = folder / "приёмка.py"
    script.write_text(ACCEPTANCE, encoding="utf-8")
    result = run(folder, script)
    assert "enabled: #1" in result.stdout
    assert "disabled: opaque True" in result.stdout
    assert "enabled again: ['#1', '#2']" in result.stdout


def test_the_journal_records_the_modules_and_replays_to_the_same_journal(folder: Path) -> None:
    script = folder / "приёмка.py"
    script.write_text(ACCEPTANCE, encoding="utf-8")
    journal = folder / "журнал.slpy"
    run(folder, script, "--journal", journal)
    text = journal.read_text(encoding="utf-8")
    assert text.endswith(
        'stresslab.cmd("demo.add_note")\n'
        'stresslab.extensions.disable("fem.demo")\n'
        'stresslab.extensions.enable("fem.demo")\n'
        'stresslab.cmd("demo.add_note", text="вторая")\n'
    ), text

    replayed = folder / "повтор.slpy"
    run(folder, journal, "--journal", replayed)
    assert replayed.read_bytes() == journal.read_bytes()


def test_modules_are_not_switched_inside_a_transaction(folder: Path) -> None:
    script = folder / "транзакция.py"
    script.write_text(
        """\
import stresslab

with stresslab.transaction("Notes"):
    stresslab.demo.add_note(text="a")
    try:
        stresslab.extensions.disable("fem.demo")
    except stresslab.ExtensionError as error:
        print("refused:", error.code)
assert stresslab.extensions.get("fem.demo").enabled
try:
    stresslab.extensions.enable("fem.nothing")
except stresslab.ExtensionError as error:
    print("unknown:", error.code)
""",
        encoding="utf-8",
    )
    result = run(folder, script)
    assert "refused: invalid_state" in result.stdout
    assert "unknown: unknown_extension" in result.stdout
