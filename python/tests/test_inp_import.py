"""The import of Abaqus input files (task 3.2) without the window: ``stresslab --headless`` with the
built-in module ``fem.format.abaqus-inp``. The script imports the fixtures of ``tests/data/inp``
by the command of the module, as a journal does; warnings of the import go to the standard error
stream (where the window shows "Problems"), the journal replays to the same model, and the code
pages of the module agree with Python's codecs.

The executable is STRESSLAB_EXECUTABLE (set by ctest) or the most recently built one under
``build/``; the tests start it themselves, so they run in the plain pytest run only.
"""

from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path

import pytest
import stresslab

REPO_ROOT = Path(__file__).resolve().parents[2]
FIXTURES = REPO_ROOT / "tests" / "data" / "inp"

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
    result = tmp_path / "импорт inp"
    shutil.copytree(FIXTURES, result / "модели")
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


IMPORT = """\
import stresslab

document = stresslab.active_document()
assert stresslab.extensions.get("fem.format.abaqus-inp").enabled
source = document[stresslab.cmd("inp.import", path="модели/assembly.inp")]
assert source.type == "abaqus_input" and source.name == "assembly.inp", source
meshes = sorted(o.name for o in document.find_by_type("mesh"))
assert meshes == ["Assembly", "Beam-1", "Beam-2"], meshes
step = document.find_by_type("static_step")[0]
assert step.name == "Step-1"
kinds = sorted({child.type for child in step.children})
expected = ["abaqus_keyword", "concentrated_load", "gravity", "output_request", "pressure"]
assert kinds == expected, kinds
print("objects:", len(document))
"""


def test_a_script_imports_a_file_and_the_journal_replays_it(folder: Path) -> None:
    script = folder / "импорт.py"
    script.write_text(IMPORT, encoding="utf-8")
    journal = folder / "журнал.slpy"
    first = run(folder, script, "--journal", journal)
    objects = next(line for line in first.stdout.splitlines() if line.startswith("objects:"))

    # The warnings of the import, with their places in the file.
    assert "assembly.inp:4: " in first.stderr and "*PREPRINT" in first.stderr, first.stderr
    assert "*RESTART" in first.stderr, first.stderr

    # The journal has the command of the module and gives the same model when run again.
    text = journal.read_text(encoding="utf-8")
    assert 'stresslab.cmd("inp.import", path=' in text, text
    check = folder / "проверка.py"
    check.write_text(
        f"exec(open({str(journal)!r}, encoding='utf-8').read())\n"
        "import stresslab\n"
        "print('objects:', len(stresslab.active_document()))\n",
        encoding="utf-8",
    )
    second = run(folder, check)
    assert objects in second.stdout.splitlines(), second.stdout


def test_a_broken_file_fails_the_script_and_says_where(folder: Path) -> None:
    script = folder / "ошибка.py"
    script.write_text(
        "import stresslab\n"
        "try:\n"
        "    stresslab.cmd('inp.import', path='модели/errors/unknown_node.inp')\n"
        "except stresslab.ExtensionError as error:\n"
        "    print('refused:', error)\n"
        "else:\n"
        "    raise AssertionError('imported')\n"
        "assert len(stresslab.active_document()) == 0\n",
        encoding="utf-8",
    )
    result = run(folder, script)
    assert "unknown_node.inp:8:" in result.stderr, result.stderr
    assert "unknown_node.inp:8: Element 2 refers to node 99" in result.stdout, result.stdout


@pytest.mark.parametrize("codec", ["cp1251", "cp1252"])
def test_code_pages_read_as_python_reads_them(folder: Path, codec: str) -> None:
    # Every byte above 0x7F the code page defines, in the heading of a file.
    letters = bytes(b for b in range(0x80, 0x100) if _defined(b, codec))
    model = folder / f"{codec}.inp"
    model.write_bytes(b"*HEADING\n" + letters + b"\n*NODE\n1, 0., 0., 0.\n")
    encoding = {"cp1251": "windows-1251", "cp1252": "windows-1252"}[codec]
    script = folder / "кодировка.py"
    script.write_text(
        "import stresslab\n"
        f"source = stresslab.active_document()[stresslab.cmd('inp.import', path={str(model)!r},"
        f" encoding={encoding!r})]\n"
        "print('heading:', source.properties['heading'].encode('utf-8').hex())\n",
        encoding="utf-8",
    )
    result = run(folder, script)
    heading = next(line for line in result.stdout.splitlines() if line.startswith("heading:"))
    assert bytes.fromhex(heading.split()[1]).decode("utf-8") == letters.decode(codec)


def _defined(byte: int, codec: str) -> bool:
    try:
        bytes([byte]).decode(codec)
    except UnicodeDecodeError:
        return False
    return True


SUMMARY = """\
import stresslab

document = stresslab.active_document()
stresslab.cmd("inp.import", path={source!r})
objects = [o for o in document.objects() if o.type != "abaqus_keyword"]
kinds = sorted(f"{{o.type}} {{o.name}}" for o in objects)
kept = sorted(o.properties["text"] for o in document.find_by_type("abaqus_keyword"))
print("summary:", kinds, kept)
"""


def test_a_script_saves_the_model_and_the_file_opens_to_the_same_model(folder: Path) -> None:
    # Task 3.3: what the export loses is known before, and the saved file reads back the same.
    saved = folder / "сохранено" / "flat.inp"
    saved.parent.mkdir()
    script = folder / "экспорт.py"
    script.write_text(
        SUMMARY.format(source="модели/flat.inp")
        + f"for loss in stresslab.cmd('inp.describe_losses', path={str(saved)!r}):\n"
        "    print('loss:', loss['message'], loss['text'])\n"
        f"print('keywords:', stresslab.cmd('inp.export', path={str(saved)!r}))\n",
        encoding="utf-8",
    )
    first = run(folder, script)
    # A file without parts saved under its name loses only the units, which .inp does not have.
    losses = [line for line in first.stdout.splitlines() if line.startswith("loss:")]
    assert len(losses) == 1 and "%loss.units%" in losses[0], losses
    assert int(first.stdout.split("keywords:")[1].split()[0]) > 20, first.stdout

    check = folder / "проверка экспорта.py"
    check.write_text(SUMMARY.format(source=str(saved)), encoding="utf-8")
    second = run(folder, check)
    summary = [line for line in first.stdout.splitlines() if line.startswith("summary:")]
    assert summary and summary[0] in second.stdout.splitlines(), (first.stdout, second.stdout)
