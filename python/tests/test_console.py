"""The Python console of the window: running lines, output and completion (task 1.14)."""

from __future__ import annotations

import sys

import pytest
import stresslab
from stresslab._console import Console
from support import TEST_OWNER


@pytest.fixture
def console() -> Console:
    return Console()


def test_stresslab_is_imported_and_expressions_are_echoed(console: Console) -> None:
    assert console.push("1 + 2") == (False, [(False, "3\n")])
    assert console.push("stresslab.embedded") == (False, [(False, f"{stresslab.embedded!r}\n")])
    assert console.push("x = 5") == (False, [])
    assert console.push("x * 2") == (False, [(False, "10\n")])
    assert console.namespace["x"] == 5


def test_banner_names_the_versions(console: Console) -> None:
    assert console.banner == (f"Python {sys.version.split()[0]}, stresslab {stresslab.__version__}")


def test_statements_of_several_lines_wait_until_complete(console: Console) -> None:
    assert console.push("for i in range(3):") == (True, [])
    assert console.push("    print('строка', i)") == (True, [])
    assert console.push("") == (False, [(False, "строка 0\nстрока 1\nстрока 2\n")])

    assert console.push("def f(") == (True, [])
    console.reset()
    assert console.push("'after reset'") == (False, [(False, "'after reset'\n")])


def test_errors_go_to_standard_error_without_frames_of_the_console(console: Console) -> None:
    more, output = console.push("1 / 0")
    assert not more
    assert len(output) == 1
    error, text = output[0]
    assert error
    assert text.startswith("Traceback (most recent call last):\n")
    assert 'File "<console>", line 1' in text
    assert text.endswith("ZeroDivisionError: division by zero\n")
    assert "_console.py" not in text and "code.py" not in text

    more, output = console.push("print(")
    assert more
    more, output = console.push("))")
    assert not more
    assert output[0][0] and "SyntaxError" in output[0][1]


def test_output_and_errors_keep_their_order(console: Console) -> None:
    more, output = console.push("import sys; print('a'); print('b', file=sys.stderr); print('c')")
    assert not more
    assert output == [(False, "a\n"), (True, "b\n"), (False, "c\n")]
    # The streams of the process are back.
    assert type(sys.stdout).__module__ != "stresslab._console"
    assert type(sys.stderr).__module__ != "stresslab._console"


def test_exit_does_not_end_the_application(console: Console) -> None:
    more, output = console.push("exit(3)")
    assert not more
    assert output[0][0] and output[0][1].rstrip().endswith("SystemExit: 3")
    assert console.push("'still here'") == (False, [(False, "'still here'\n")])


def test_commands_run_from_the_console_are_journaled(console: Console, doc: object) -> None:
    entries: list[str] = []
    subscription = stresslab.active_document().on_journal(entries.append)
    assert console.push(
        f"steel = stresslab.cmd('model.create', type='material', owner={TEST_OWNER!r}, "
        "name='Сталь')"
    ) == (False, [])
    del subscription
    assert len(entries) == 1
    assert entries[0].startswith('stresslab.cmd("model.create"')


# --- Completion ----------------------------------------------------------------------------------


def test_completes_names_of_the_namespace(console: Console) -> None:
    assert "stresslab" in console.complete("stres")
    assert console.complete("print(stres") == ["print(stresslab"]
    console.push("my_value = 1")
    assert console.complete("x = my_") == ["x = my_value"]
    assert console.complete("") == []
    assert console.complete("x = ") == []


def test_completes_the_api_and_the_wrappers_of_commands(console: Console, doc: object) -> None:
    matches = console.complete("stresslab.")
    assert "stresslab.cmd(" in matches
    assert "stresslab.undo()" in matches  # No arguments: rlcompleter closes the call.
    assert "stresslab.model" in matches
    assert not any(m.startswith("stresslab._") for m in matches)
    assert console.complete("stresslab.un") == ["stresslab.undo()"]
    assert "stresslab.model.create(" in console.complete("stresslab.model.cr")
    assert console.complete("  stresslab.model.ren") == ["  stresslab.model.rename("]


def test_private_names_only_when_asked(console: Console) -> None:
    assert any(m.startswith("stresslab._") for m in console.complete("stresslab._"))


def test_completion_errors_and_output_stay_quiet(
    console: Console, capsys: pytest.CaptureFixture[str]
) -> None:
    for line in [
        "class Loud:",
        "    def __getattr__(self, name):",
        "        print('loud')",
        "        raise AttributeError(name)",
        "",
        "loud = Loud()",
    ]:
        console.push(line)
    assert console.complete("missing_name.attr") == []
    console.complete("loud.x")
    assert capsys.readouterr().out == ""
