"""Tasks from Python (task 1.8): progress, messages, cancellation and results of work that runs
in the background, and commands that return a task."""

from __future__ import annotations

import threading

import pytest
import stresslab
from support import TEST_OWNER

TIMEOUT = 30.0


def test_a_task_reports_progress_and_returns_its_result() -> None:
    events: list[tuple[str, str]] = []
    threads: set[int] = set()

    def work(control: stresslab.TaskControl) -> dict[str, float]:
        control.message("Чтение файла")
        control.set_progress(0.5)
        control.message("Предупреждение", severity="warning")
        return {"nodes": 42.0}

    task, control = stresslab.make_task("Импорт")

    def listener(event: stresslab.TaskEvent) -> None:
        events.append((event.kind, event.state))
        threads.add(threading.get_ident())

    def run() -> None:
        if control.start():
            control.succeed(work(control))

    subscription = task.subscribe(listener)
    assert task.state == "pending" and task.title == "Импорт"
    worker = threading.Thread(target=run)
    worker.start()
    assert task.result(timeout=TIMEOUT) == {"nodes": 42.0}
    worker.join()
    subscription.close()
    assert task.state == "succeeded" and task.finished
    assert task.progress == 0.5
    assert task.messages == [("info", "Чтение файла"), ("warning", "Предупреждение")]
    assert task.error is None
    assert ("state", "running") in events and events[-1] == ("state", "succeeded")
    assert threads == {worker.ident}


def test_run_task() -> None:
    task = stresslab.run_task("Счёт", lambda control: sum(range(10)))
    assert task.result(timeout=TIMEOUT) == 45
    assert task.state == "succeeded"


def test_a_failing_task() -> None:
    def work(control: stresslab.TaskControl) -> None:
        raise ValueError("нет файла")

    task = stresslab.run_task("Сбой", work)
    assert task.wait(timeout=TIMEOUT)
    assert task.state == "failed"
    assert task.error == "ValueError: нет файла"
    with pytest.raises(stresslab.TaskError) as raised:
        task.result()
    assert raised.value.code == "failed"


def test_cancelling_a_task() -> None:
    started = threading.Event()

    def work(control: stresslab.TaskControl) -> None:
        started.set()
        while True:
            control.check_cancelled()
            threading.Event().wait(0.001)

    task = stresslab.run_task("Долгий счёт", work)
    assert started.wait(TIMEOUT)
    assert task.state == "running"
    assert task.cancel() is True
    assert task.cancel() is False
    assert task.wait(timeout=TIMEOUT)
    assert task.state == "cancelled" and task.cancel_requested
    with pytest.raises(stresslab.TaskCancelled):
        task.result()


def test_waiting_can_time_out() -> None:
    release = threading.Event()
    task = stresslab.run_task("Ожидание", lambda control: release.wait(TIMEOUT))
    assert task.wait(timeout=0.01) is False
    with pytest.raises(stresslab.TaskError) as raised:
        task.result(timeout=0.01)
    assert raised.value.code == "not_finished"
    release.set()
    assert task.result(timeout=TIMEOUT) is True


def test_a_command_returns_its_task(doc: stresslab.Document) -> None:
    def solve(ctx: stresslab.CommandContext, steps: int) -> stresslab.Task:
        def work(control: stresslab.TaskControl) -> int:
            for step in range(steps):
                control.set_progress((step + 1) / steps)
            return steps

        return stresslab.run_task("Расчёт", work)

    doc.register_command(
        "test.solve", solve, owner=TEST_OWNER, args=[stresslab.ArgSpec("steps", "integer")]
    )
    task = stresslab.cmd("test.solve", steps=4)
    assert isinstance(task, stresslab.Task)
    assert task.result(timeout=TIMEOUT) == 4
    assert task.progress == 1.0
    assert repr(task) == "<stresslab.Task 'Расчёт' succeeded>"


def test_a_failing_task_listener_is_reported(monkeypatch: pytest.MonkeyPatch) -> None:
    reported: list[str] = []
    # Only the text is kept: the report holds frames that refer to the task.
    monkeypatch.setattr("sys.unraisablehook", lambda report: reported.append(str(report.exc_value)))
    task, control = stresslab.make_task("t")

    def fail(event: stresslab.TaskEvent) -> None:
        raise RuntimeError("listener")

    subscription = task.subscribe(fail)
    assert control.start()
    assert control.succeed(1)
    subscription.close()
    assert task.result() == 1
    assert reported == ["listener", "listener"]  # running, succeeded
