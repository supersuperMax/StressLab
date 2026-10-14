"""The Python console of the window (task 1.14).

The shell sends what the user types line by line; a statement of several lines (``for``, ``def``,
``with``) is run once it is complete, like at the interactive prompt of Python. What the code
prints is collected and returned, standard error apart, so the shell can show it. ``stresslab`` is
imported in the namespace of the console, and Tab completes names and attributes in it, including
the typed wrappers of commands (``stresslab.model.cr`` → ``stresslab.model.create(``).
"""

from __future__ import annotations

import code
import io
import re
import rlcompleter
import sys
from typing import Any, TextIO

import stresslab

from ._commands import CommandNamespace

#: The start of the text Tab completes: a name and attributes after it, at the end of the line.
_COMPLETED = re.compile(r"[A-Za-z_][\w.]*$|$")


class _Completer(rlcompleter.Completer):
    def _callable_postfix(self, val: Any, word: str) -> str:
        # A namespace of commands (stresslab.model) is callable only if it is a command too.
        if isinstance(val, CommandNamespace) and val._command() is None:
            return word
        return super()._callable_postfix(val, word)  # type: ignore[misc]


class _Output(io.TextIOBase):
    """A stream that collects what is written to it as (is_error, text) pieces."""

    def __init__(self, pieces: list[tuple[bool, str]], error: bool) -> None:
        super().__init__()
        self._pieces = pieces
        self._error = error

    @property
    def encoding(self) -> str:  # type: ignore[override]
        return "utf-8"

    def writable(self) -> bool:
        return True

    def write(self, text: str) -> int:
        if text:
            if self._pieces and self._pieces[-1][0] == self._error:
                self._pieces[-1] = (self._error, self._pieces[-1][1] + text)
            else:
                self._pieces.append((self._error, text))
        return len(text)


class _Interpreter(code.InteractiveConsole):
    def __init__(self, namespace: dict[str, Any], pieces: list[tuple[bool, str]]) -> None:
        super().__init__(namespace, filename="<console>")
        self._pieces = pieces

    def write(self, data: str) -> None:
        # Tracebacks and syntax errors.
        _Output(self._pieces, True).write(data)

    def runcode(self, code_object: Any) -> None:
        try:
            exec(code_object, self.locals)
        except BaseException:
            # Also SystemExit: exit() would end the application, it is shown as an error instead.
            self.showtraceback()


class Console:
    """An interactive console on its own namespace, in which ``stresslab`` is imported."""

    def __init__(self) -> None:
        self.namespace: dict[str, Any] = {
            "__name__": "__console__",
            "__doc__": None,
            "stresslab": stresslab,
        }
        self._pieces: list[tuple[bool, str]] = []
        self._interpreter = _Interpreter(self.namespace, self._pieces)
        self._completer = _Completer(self.namespace)

    @property
    def banner(self) -> str:
        """The versions of Python and of ``stresslab``, for the first line of the console."""
        return f"Python {sys.version.split()[0]}, stresslab {stresslab.__version__}"

    def push(self, line: str) -> tuple[bool, list[tuple[bool, str]]]:
        """Runs a line, or keeps it until the statement it starts is complete.

        Returns whether more lines are needed and what was printed meanwhile, as (is_error, text)
        pieces in the order they were printed.
        """
        self._pieces.clear()
        out: TextIO = _Output(self._pieces, False)  # type: ignore[assignment]
        err: TextIO = _Output(self._pieces, True)  # type: ignore[assignment]
        saved = sys.stdout, sys.stderr
        sys.stdout, sys.stderr = out, err
        try:
            more = self._interpreter.push(line)
        finally:
            sys.stdout, sys.stderr = saved
        pieces = list(self._pieces)
        self._pieces.clear()
        return more, pieces

    def reset(self) -> None:
        """Drops the lines of a statement that is not complete yet."""
        self._interpreter.resetbuffer()

    def complete(self, line: str) -> list[str]:
        """The lines Tab can make of ``line``: the name or attribute at its end completed, in the
        order of ``rlcompleter`` (callables end with ``(``, or ``()`` without parameters), each
        once."""
        match = _COMPLETED.search(line)
        assert match is not None
        start, text = match.start(), match.group()
        if not text:
            return []
        matches: list[str] = []
        state = 0
        saved = sys.stdout, sys.stderr
        # Evaluating the object before the last dot may print; that is not output of the console.
        sys.stdout = sys.stderr = io.StringIO()
        try:
            while True:
                try:
                    candidate = self._completer.complete(text, state)
                except Exception:
                    break
                if candidate is None:
                    break
                state += 1
                if not candidate.startswith(text):
                    continue
                full = line[:start] + candidate
                if full not in matches:
                    matches.append(full)
        finally:
            sys.stdout, sys.stderr = saved
        # Private names only when asked for.
        last = text.rpartition(".")[2]
        if not last.startswith("_"):
            matches = [m for m in matches if not m[start:].rpartition(".")[2].startswith("_")]
        return matches
