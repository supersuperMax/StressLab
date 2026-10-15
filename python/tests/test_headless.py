"""``stresslab --headless run-script`` (task 1.10, ADR 0011), with the real executable: modules,
the script, the exit code, the log on the standard output and the journal.

The executable is STRESSLAB_EXECUTABLE (set by ctest) or the most recently built one under
``build/``. The tests start it themselves, so they run in the plain pytest run only. Scripts,
modules and journals are in a folder with Cyrillic letters and spaces.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
from pathlib import Path

import pytest
import stresslab

REPO_ROOT = Path(__file__).resolve().parents[2]
MODULE = REPO_ROOT / "tests" / "extensions" / "headless"

pytestmark = pytest.mark.skipif(
    stresslab.embedded, reason="starts the executable itself; the plain run covers it"
)


def _executable() -> Path | None:
    configured = os.environ.get("STRESSLAB_EXECUTABLE")
    if configured:
        return Path(configured)
    built = [*REPO_ROOT.glob("build/*/app/stresslab.exe"), *REPO_ROOT.glob("build/*/app/stresslab")]
    return max(built, key=lambda path: path.stat().st_mtime) if built else None


class Headless:
    """A folder with the test module installed in a subfolder, and a way to run scripts."""

    def __init__(self, executable: Path, folder: Path) -> None:
        self.executable = executable
        self.folder = folder
        self.modules = folder / "модули с пробелами"
        shutil.copytree(MODULE, self.modules / "headless")

    def script(self, name: str, text: str) -> Path:
        path = self.folder / name
        path.write_text(text, encoding="utf-8")
        return path

    def run(
        self,
        *args: str | Path,
        modules: bool = True,
        stdin: int | None = subprocess.DEVNULL,
    ) -> subprocess.CompletedProcess[str]:
        options: list[str | Path] = ["--headless", "--log-level", "info"]
        if modules:
            options += ["--extensions", self.modules]
        return subprocess.run(
            [self.executable, *options, *args],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            stdin=stdin,
            cwd=self.folder,
            timeout=300,
        )


@pytest.fixture
def headless(tmp_path: Path) -> Headless:
    executable = _executable()
    if executable is None or not executable.exists():
        pytest.skip("the stresslab executable is not built")
    folder = tmp_path / "headless тест"
    folder.mkdir()
    return Headless(executable, folder)


def explain(result: subprocess.CompletedProcess[str]) -> str:
    return (
        f"exit code {result.returncode}\n--- stdout\n{result.stdout}\n--- stderr\n{result.stderr}"
    )


def log_lines(output: str, level: str) -> list[str]:
    return [line for line in output.splitlines() if f" [{level}] " in line]


BUILD_MODEL = """\
import stresslab

steel = stresslab.cmd("model.create", type="material", owner="test.headless", name="Сталь",
                      properties={"E": 210000.0, "nu": 0.3})
shaft = stresslab.headless.make(name="Вал")
with stresslab.transaction("Rename"):
    stresslab.model.rename(object=shaft, name="Вал 2")
stresslab.cmd("model.create", type="part", owner="test.headless", name="лишний")
stresslab.undo()
names = sorted(item.name for item in stresslab.active_document().objects())
print("objects:", names)
"""


def test_a_script_builds_a_model_and_its_journal_replays_to_the_same_journal(
    headless: Headless,
) -> None:
    script = headless.script("модель.py", BUILD_MODEL)
    journal = headless.folder / "журнал 1.slpy"
    result = headless.run("--journal", journal, "run-script", script)
    assert result.returncode == 0, explain(result)
    assert "objects: ['Вал 2', 'Сталь']" in result.stdout

    text = journal.read_text(encoding="utf-8")
    assert text.startswith(stresslab.journal_header())
    assert 'stresslab.cmd("headless.make", name="Вал")' in text
    assert 'with stresslab.transaction("Rename"):' in text
    assert "stresslab.undo()" in text
    assert "\r" not in journal.read_bytes().decode("utf-8")

    # The journal is a script: run again, it records itself.
    replayed = headless.folder / "журнал 2.slpy"
    again = headless.run("--journal", replayed, "run-script", journal)
    assert again.returncode == 0, explain(again)
    assert replayed.read_bytes() == journal.read_bytes()


def test_the_log_goes_to_the_standard_output_in_order_with_the_script(headless: Headless) -> None:
    script = headless.script("порядок.py", 'print("from the script")\n')
    result = headless.run("run-script", script)
    assert result.returncode == 0, explain(result)
    lines = result.stdout.splitlines()

    def index(fragment: str) -> int:
        found = [i for i, line in enumerate(lines) if fragment in line]
        assert found, f"{fragment!r} not in the output:\n{result.stdout}"
        return found[0]

    started = index("[info] [headless] StressLab ")
    enabled = index("[info] [headless] module 'test.headless' enabled")
    activated = index("[info] [headless] module 'test.headless' activated")
    printed = lines.index("from the script")
    finished = index("[info] [headless] the script finished with exit code 0")
    assert started < enabled < activated < printed < finished
    assert re.match(r"\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3} \[info\] \[headless\] ", lines[started])


def test_the_log_level_and_the_log_file(headless: Headless) -> None:
    script = headless.script("тихо.py", 'print("quiet")\n')
    log_file = headless.folder / "логи" / "запуск.log"
    log_file.parent.mkdir()
    result = headless.run("--log-level", "warning", "--log-file", log_file, "run-script", script)
    assert result.returncode == 0, explain(result)
    assert result.stdout.splitlines() == ["quiet"]

    loud = headless.run("--log-file", log_file, "run-script", script)
    assert loud.returncode == 0, explain(loud)
    text = log_file.read_text(encoding="utf-8")
    # The file has the thread in brackets; the first run wrote nothing below a warning.
    assert re.search(r"\[info\] \[headless\] \[\d+\] the script finished with exit code 0", text)
    assert text.count("the script finished") == 1


@pytest.mark.parametrize(
    ("text", "code"),
    [
        ("x = 1\n", 0),
        ("import sys\nsys.exit(3)\n", 3),
        ("raise SystemExit(None)\n", 0),
        ("import sys\nsys.exit('gave up')\n", 1),
    ],
)
def test_the_exit_code_is_that_of_the_script(headless: Headless, text: str, code: int) -> None:
    result = headless.run("run-script", headless.script("код.py", text), modules=False)
    assert result.returncode == code, explain(result)
    assert f"the script finished with exit code {code}" in result.stdout


def test_an_exception_gives_one_and_its_traceback(headless: Headless) -> None:
    script = headless.script("сломан.py", "def f():\n    raise ValueError('broken')\n\nf()\n")
    result = headless.run("run-script", script, modules=False)
    assert result.returncode == 1, explain(result)
    assert 'сломан.py", line 2, in f' in result.stderr
    assert "ValueError: broken" in result.stderr


@pytest.mark.parametrize(
    "args",
    [
        ["run-script", "нет такого.py"],
        ["--extensions", "нет такой папки", "run-script", "{script}"],
        ["--profile", "нет такого.json", "run-script", "{script}"],
        ["--verbose", "run-script", "{script}"],
        ["--log-level", "loud", "run-script", "{script}"],
        ["run-script"],
        [],
        ["run", "{script}"],
    ],
)
def test_a_wrong_command_line_gives_two(headless: Headless, args: list[str]) -> None:
    script = headless.script("есть.py", "raise SystemExit(5)\n")
    result = headless.run(*[arg.replace("{script}", str(script)) for arg in args], modules=False)
    assert result.returncode == 2, explain(result)
    assert result.stderr.strip(), explain(result)


def test_the_script_gets_its_arguments_and_its_folder(headless: Headless) -> None:
    headless.script("helper.py", "VALUE = 'beside the script'\n")
    script = headless.script(
        "аргументы.py",
        "import sys\nimport helper\nprint(repr(sys.argv[1:]), helper.VALUE, __name__)\n",
    )
    result = headless.run("run-script", script, "a b", "--journal", "-x", modules=False)
    assert result.returncode == 0, explain(result)
    assert "['a b', '--journal', '-x'] beside the script __main__" in result.stdout


BACKGROUND = """\
import time
import stresslab

document = stresslab.active_document()

def names():
    return [item.name for item in document.objects()]

# The module creates the part from a thread of its own, after its command has returned.
stresslab.headless.background(name="фон")
deadline = time.monotonic() + 60
while "фон" not in names():
    assert time.monotonic() < deadline, names()
    time.sleep(0.01)

# The work of the task asks the application for a command while the script waits for it.
task = stresslab.headless.task_make(name="из задачи")
created = task.result(timeout=60)
assert document[created].name == "из задачи", created
print("background:", sorted(names()))
"""


def test_modules_are_served_while_the_script_runs(headless: Headless) -> None:
    result = headless.run("run-script", headless.script("фон.py", BACKGROUND))
    assert result.returncode == 0, explain(result)
    assert "background: ['из задачи', 'фон']" in result.stdout


CRASH = """\
import stresslab

stresslab.headless.make(name="before")
try:
    stresslab.cmd("headless.crash")
except Exception as error:
    print("the command failed:", type(error).__name__)
else:
    raise SystemExit("the crash went unnoticed")
stresslab.headless.make(name="after")  # The module was restarted.
print("parts:", sorted(item.name for item in stresslab.active_document().objects()))
"""


def test_a_crash_of_a_module_is_reported_and_the_model_is_intact(headless: Headless) -> None:
    result = headless.run("run-script", headless.script("падение.py", CRASH))
    assert result.returncode == 0, explain(result)
    assert "the command failed:" in result.stdout
    assert "parts: ['after', 'before']" in result.stdout
    warnings = log_lines(result.stdout, "warning")
    assert any("Headless test module" in line for line in warnings), explain(result)


SUBPROCESS = """\
import subprocess
import sys

child = subprocess.run([sys.executable, "-c", "print('from the child')"],
                       capture_output=True, text=True, check=True)
print(child.stdout.strip())
"""


def test_a_script_may_start_processes_with_the_inherited_input(headless: Headless) -> None:
    script = headless.script("процесс.py", SUBPROCESS)
    result = headless.run("run-script", script, modules=False, stdin=None)
    assert result.returncode == 0, explain(result)
    assert "from the child" in result.stdout.splitlines()


SAVE_PROJECT = """\
import stresslab

stresslab.headless.make(name="Вал")
stresslab.project.save_as("проект вала")
print("saved:", stresslab.project.folder().name)
"""

OPEN_PROJECT = """\
import stresslab

stresslab.project.open("проект вала.slproj")
print("opened:", [item.name for item in stresslab.active_document().objects()])
print("modified:", stresslab.project.is_modified())
"""


def test_a_script_saves_a_project_that_another_run_opens(headless: Headless) -> None:
    saved = headless.run("run-script", headless.script("сохранить.py", SAVE_PROJECT))
    assert saved.returncode == 0, explain(saved)
    assert "saved: проект вала.slproj" in saved.stdout
    project = headless.folder / "проект вала.slproj" / "project.json"
    assert '"test.headless": "1.0.0"' in project.read_text(encoding="utf-8")

    journal = headless.folder / "открытие.slpy"
    opened = headless.run(
        "--journal", journal, "run-script", headless.script("открыть.py", OPEN_PROJECT)
    )
    assert opened.returncode == 0, explain(opened)
    assert "opened: ['Вал']" in opened.stdout
    assert "modified: False" in opened.stdout
    assert 'stresslab.project.open("проект вала.slproj")' in journal.read_text(encoding="utf-8")
