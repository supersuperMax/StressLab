"""The golden models of ``tests/models`` and their harness (task 3.4, tests/models/README.md,
docs/adr/0026-golden-models.md).

No real solver runs here: the solvers come with their adapters (CalculiX, task 3.5). Instead the
descriptions are checked (every expected value agrees with its formula, the input files are what
the generator writes), the measuring is checked on hand-made results, and a stand-in solver that
writes the exact solutions (``tests/models/analytic_solver.py``) passes every model while the same
solver 10 % off fails. In ``stresslab --headless`` the input files go the real way: imported by
``fem.format.abaqus-inp`` with nothing left uninterpreted, solved by the stand-in, checked.
"""

from __future__ import annotations

import math
import os
import re
import shutil
import subprocess
import sys
from collections.abc import Callable
from pathlib import Path
from typing import Any

import numpy as np
import pytest
import stresslab

REPO_ROOT = Path(__file__).resolve().parents[2]
MODELS = REPO_ROOT / "tests" / "models"
sys.path.insert(0, str(MODELS))

import generate  # noqa: E402
import golden  # noqa: E402
from analytic_solver import OWNER, AnalyticSolver  # noqa: E402

# What task 3.4 asks for: (problem, element) of each model.
REQUIRED = {
    ("cantilever", "C3D10"),
    ("cantilever", "C3D20"),
    ("cantilever", "B31"),
    ("plate_hole", "S4"),
    ("plate_hole", "C3D8"),
    ("thick_cylinder", "C3D20"),
    ("cube_gravity", "C3D8"),
}
NEUTRAL = {"C3D10": "tet10", "C3D20": "hex20", "C3D8": "hex8", "S4": "quad4", "B31": "line2"}
CASES = golden.load_cases()


def test_every_model_of_the_task_is_described() -> None:
    assert {(case.problem, case.element) for case in CASES} == REQUIRED
    for case in CASES:
        assert case.model.name == f"{case.id}.inp"
        assert case.units == {"length": "mm", "mass": "t", "time": "s", "temperature": "degC"}


@pytest.mark.parametrize("case", CASES, ids=lambda case: case.id)
def test_the_input_file_is_what_the_generator_writes(case: golden.GoldenCase) -> None:
    descriptions = generate.descriptions(MODELS)
    expected = generate.render(case.id, descriptions[case.id]).splitlines()
    actual = case.model.read_text(encoding="utf-8").splitlines()
    assert len(actual) == len(expected), "run tests/models/generate.py"
    # Numbers may differ in the last digit where the mathematical library of the system does.
    for number, (line, wanted) in enumerate(zip(actual, expected, strict=True), start=1):
        assert _same_numbers(line, wanted), f"{case.model.name}:{number}: {line!r} != {wanted!r}"


_NUMBER = re.compile(r"-?\d+\.?\d*(?:e[-+]?\d+)?")


def _same_numbers(line: str, wanted: str) -> bool:
    if _NUMBER.sub("#", line) != _NUMBER.sub("#", wanted):
        return False
    pairs = zip(_NUMBER.findall(line), _NUMBER.findall(wanted), strict=True)
    return all(math.isclose(float(a), float(b), rel_tol=1e-9, abs_tol=1e-9) for a, b in pairs)


# Descriptions ------------------------------------------------------------------------------------

GOOD = """\
title = "t"
problem = "cantilever"
element = "B31"
model = "m.inp"
[parameters]
P = 2.0
[[checks]]
name = "c"
field = "U"
component = "U3"
point = [0.0, 0.0, 0.0]
formula = "P * 3"
expected = 6.0
rel_tol = 0.01
"""


@pytest.mark.parametrize(
    ("change", "message"),
    [
        (lambda text: text + "colour = 1\n", "unknown keys ['colour']"),
        (lambda text: text.replace("expected = 6.0", "expected = 6.1"), "the formula gives 6"),
        (lambda text: text.replace("P * 3", "Q * 3"), "unknown names ['Q']"),
        (lambda text: text.replace("P * 3", "__import__('os')"), "unknown names"),
        (lambda text: text.replace("P * 3", "P *"), "formula 'P *'"),
        (lambda text: text.replace("rel_tol = 0.01", ""), "give rel_tol or abs_tol"),
        (lambda text: text + "box = [[0, 0, 0], [1, 1, 1]]\n", "either point or box"),
        (lambda text: text.replace("point = [0.0, 0.0, 0.0]", "point = [0, 0]"), "[x, y, z]"),
        (
            lambda text: text.replace("point = [0.0, 0.0, 0.0]", "box = [[1, 0, 0], [0, 1, 1]]"),
            "lower one",
        ),
        (lambda text: text + 'reduce = "median"\n', "reduce is one of"),
        (lambda text: text.replace('model = "m.inp"', 'model = "other.inp"'), "no model file"),
        (lambda text: text.replace('field = "U"\n', ""), "field is missing"),
    ],
)
def test_a_wrong_description_is_refused(
    tmp_path: Path, change: Callable[[str], str], message: str
) -> None:
    (tmp_path / "m.inp").write_text("*HEADING\n", encoding="utf-8")
    golden.load_case(_description(tmp_path, GOOD))  # The unchanged one is fine.
    with pytest.raises(golden.GoldenError, match=re.escape(message)):
        golden.load_case(_description(tmp_path, change(GOOD)))


def _description(folder: Path, text: str) -> Path:
    path = folder / "описание модели.toml"
    path.write_text(text, encoding="utf-8")
    return path


def test_an_unknown_model_id_is_refused() -> None:
    with pytest.raises(golden.GoldenError, match="no golden models"):
        golden.load_cases(ids=["cantilever_c3d11"])
    assert [case.id for case in golden.load_cases(ids=["cube_gravity"])] == ["cube_gravity"]


# Measuring ---------------------------------------------------------------------------------------


def _model_document(case: golden.GoldenCase) -> stresslab.Document:
    """A model document with the mesh of the case as the generator builds it (no import)."""
    description = generate.descriptions(MODELS)[case.id]
    model = generate.PROBLEMS[case.problem](case.element, case.parameters, description["mesh"])
    nodes = np.array(model.mesh.nodes)
    connectivity = np.array(model.mesh.elements, dtype=np.int64) - 1
    mesh = stresslab.Mesh(
        np.arange(1, len(nodes) + 1),
        nodes[:, 0],
        nodes[:, 1],
        nodes[:, 2],
        blocks=[(NEUTRAL[case.element], np.arange(1, len(connectivity) + 1), connectivity)],
    )
    document = stresslab.Document()
    document.register_command(
        "test.golden_mesh",
        lambda ctx: ctx.create_object("mesh", "fem.core", name=case.id, data=mesh).id,
        owner="test.module",
    )
    document.cmd("test.golden_mesh")
    return document


@pytest.mark.parametrize("case", CASES, ids=lambda case: case.id)
def test_the_exact_solution_passes_and_a_wrong_one_fails(
    case: golden.GoldenCase, tmp_path: Path
) -> None:
    previous = stresslab.active_document()
    try:
        model = _model_document(case)
        good = golden.evaluate(case, AnalyticSolver(case).solve(model, tmp_path), "exact")
        good.assert_passed()
        assert all(result.nodes >= 1 for result in good.results)
        bad = golden.evaluate(case, AnalyticSolver(case, scale=1.1).solve(model, tmp_path), "off")
        assert not bad.passed
        with pytest.raises(AssertionError, match=case.id):
            bad.assert_passed()
        # Only the checks of zero stay within their tolerance when everything is 10 % off.
        for result in bad.results:
            assert result.passed == (result.check.expected == 0), bad.format()
    finally:
        stresslab.set_active_document(previous)


# A line of two quadrilaterals: nodes 0-1-2 at the bottom, 3-4-5 at the top (x = 0, 1, 2; y = 0, 1).
LINE_X = [0.0, 1.0, 2.0, 0.0, 1.0, 2.0]
LINE_Y = [0.0, 0.0, 0.0, 1.0, 1.0, 1.0]
QUADS = [[0, 1, 4, 3], [1, 2, 5, 4]]


def _results(fields: dict[str, dict[str, Any]], steps: int = 1) -> stresslab.Document:
    """Results on the two quadrilaterals; each field: position, components and arrays."""
    previous = stresslab.active_document()
    document = stresslab.Document()
    stresslab.set_active_document(previous)
    document.set_owner_enabled(OWNER)

    def build(ctx: stresslab.CommandContext) -> None:
        mesh = stresslab.Mesh(
            [10, 20, 30, 40, 50, 60], LINE_X, LINE_Y, [0.0] * 6, blocks=[("quad4", [1, 2], QUADS)]
        )
        mesh_object = ctx.create_object("mesh", "fem.core", name="m", data=mesh)
        for number in range(1, steps + 1):
            step = ctx.create_object(
                "result_step",
                OWNER,
                name=f"Step-{number}",
                properties={"number": number, "procedure": "static"},
            )
            for increment in (1, 2):
                frame = ctx.create_object(
                    "result_frame",
                    OWNER,
                    name=f"{number}.{increment}",
                    parent=step,
                    properties={"increment": increment, "time": increment / 2},
                )
                # Only the last frame of the last step has the fields; the others have junk.
                last = number == steps and increment == 2
                for name, spec in fields.items():
                    arrays = dict(spec["arrays"])
                    if not last:
                        arrays["values"] = np.full_like(arrays["values"], 1e9)
                    ctx.create_object(
                        "result_field",
                        OWNER,
                        name=name,
                        parent=frame,
                        dependencies=[mesh_object],
                        data=stresslab.ArrayData(arrays),
                        properties={"position": spec["position"], "components": spec["components"]},
                    )

    document.register_command("test.results", build, owner="test.module")
    document.cmd("test.results")
    return document


def _nodal(values: list[list[float]], nodes: list[int] | None = None) -> dict[str, Any]:
    return {
        "position": "nodal",
        "components": ["U1", "U2"],
        "arrays": {
            "node": np.array(nodes if nodes is not None else range(len(values)), dtype=np.int32),
            "values": np.array(values, dtype=np.float64),
        },
    }


def _check(**changes: Any) -> golden.Check:
    values: dict[str, Any] = {
        "name": "c",
        "field": "U",
        "component": "U2",
        "box": ((0.0, 0.0, 0.0), (2.0, 1.0, 0.0)),
        "reduce": "mean",
        "expected": 0.0,
        "rel_tol": 0.0,
        "abs_tol": 1e-9,
        "formula": "0",
    }
    values.update(changes)
    return golden.Check(**values)


def _measure(document: stresslab.Document, check: golden.Check) -> tuple[float, int]:
    return golden.measure(golden.last_frame(document), check)


U = [[0.0, 1.0], [0.0, -2.0], [0.0, 3.0], [0.0, 4.0], [0.0, 5.0], [0.0, -6.0]]


@pytest.mark.parametrize(
    ("reduce", "box", "value", "count"),
    [
        ("mean", ((0, 0, 0), (2, 1, 0)), 5 / 6, 6),
        ("sum", ((0, 0, 0), (2, 1, 0)), 5.0, 6),
        ("min", ((0, 0, 0), (2, 1, 0)), -6.0, 6),
        ("max", ((0, 0, 0), (2, 1, 0)), 5.0, 6),
        ("maxabs", ((0, 0, 0), (2, 1, 0)), -6.0, 6),
        ("sum", ((0, 0, 0), (2, 0, 0)), 2.0, 3),  # The bottom edge.
        ("mean", ((1, 1, 0), (1, 1, 0)), 5.0, 1),  # A point.
        ("mean", ((1 + 1e-7, 1, 0), (1 + 1e-7, 1, 0)), 5.0, 1),  # Within 1e-6 of the size.
    ],
)
def test_a_check_reduces_the_component_over_the_nodes_in_its_box(
    reduce: str, box: Any, value: float, count: int
) -> None:
    document = _results({"U": _nodal(U)})
    measured, nodes = _measure(document, _check(reduce=reduce, box=box))
    assert measured == pytest.approx(value) and nodes == count


def test_element_nodal_values_are_averaged_at_a_node() -> None:
    # Node 1 (x = 1, y = 0) is in both elements: 2 from the first, 4 from the second.
    elements = [0, 0, 0, 0, 1, 1, 1, 1]
    local = [0, 1, 2, 3, 0, 1, 2, 3]
    s11 = [1.0, 2.0, 3.0, 4.0, 4.0, 5.0, 6.0, 7.0]
    field = {
        "position": "elementNodal",
        "components": ["S11", "S22"],
        "arrays": {
            "element": np.array(elements, dtype=np.int32),
            "node": np.array(local, dtype=np.int32),
            "values": np.column_stack((s11, np.zeros(8))),
        },
    }
    document = _results({"S": field})
    point = ((1.0, 0.0, 0.0), (1.0, 0.0, 0.0))
    assert _measure(document, _check(field="S", component="S11", box=point)) == (3.0, 1)
    top = ((1.0, 1.0, 0.0), (1.0, 1.0, 0.0))  # 3 from the first, 7 from the second.
    assert _measure(document, _check(field="S", component="S11", box=top)) == (5.0, 1)


def test_the_last_frame_of_the_last_step_is_measured() -> None:
    document = _results({"U": _nodal(U)}, steps=3)
    frame = golden.last_frame(document)
    assert frame.name == "3.2"
    assert golden.measure(frame, _check(reduce="sum"))[0] == pytest.approx(5.0)


@pytest.mark.parametrize(
    ("fields", "check", "message"),
    [
        ({"U": _nodal(U)}, _check(field="S"), "no field 'S' in frame '1.2' (there are ['U'])"),
        ({"U": _nodal(U)}, _check(component="U3"), "has no component 'U3'"),
        ({"U": _nodal(U)}, _check(box=((5, 5, 5), (6, 6, 6))), "no node in box"),
        (
            {"U": _nodal(U)},
            _check(box=((3.0, 0.0, 0.0), (3.0, 0.0, 0.0))),
            "no node in point [3.0, 0.0, 0.0]",
        ),
        ({"U": _nodal(U[:2], [0, 1])}, _check(), "has no value at nodes [30, 40, 50, 60]"),
        (
            {"U": {**_nodal(U), "position": "integrationPoint"}},
            _check(),
            "is 'integrationPoint'; the checks need nodal or elementNodal values",
        ),
    ],
)
def test_what_cannot_be_measured_fails_the_check_with_the_reason(
    fields: dict[str, Any], check: golden.Check, message: str
) -> None:
    document = _results(fields)
    case = golden.load_cases(ids=["cube_gravity"])[0]
    report = golden.evaluate(
        golden.GoldenCase(**{**case.__dict__, "checks": (check,)}), document, "s"
    )
    assert not report.passed
    assert message in report.results[0].error, report.results[0].error
    assert message in report.format()


def test_results_without_steps_fail_every_check() -> None:
    previous = stresslab.active_document()
    empty = stresslab.Document()
    stresslab.set_active_document(previous)
    case = golden.load_cases(ids=["thick_cylinder"])[0]
    report = golden.evaluate(case, empty, "nothing")
    assert len(report.results) == 3 and not report.passed
    assert all(result.error == "the results have no result_step" for result in report.results)


def test_a_report_shows_each_check() -> None:
    document = _results({"U": _nodal(U)})
    case = golden.load_cases(ids=["cube_gravity"])[0]
    checks = (_check(name="near", expected=1.0, abs_tol=0.2), _check(name="far", expected=0.0))
    report = golden.evaluate(
        golden.GoldenCase(**{**case.__dict__, "checks": checks}), document, "s"
    )
    text = report.format()
    assert text.splitlines()[0] == "cube_gravity (Cube under gravity, C3D8) by s:"
    assert "  ok   near [U.U2 mean]: measured 0.833333, expected 1 (-16.67%), allowed ±0.2" in text
    assert "  FAIL far [U.U2 mean]: measured 0.833333, expected 0, allowed ±1e-09" in text


# The real path, in the application ---------------------------------------------------------------


def _executable() -> Path | None:
    configured = os.environ.get("STRESSLAB_EXECUTABLE")
    if configured:
        return Path(configured)
    built = [*REPO_ROOT.glob("build/*/app/stresslab.exe"), *REPO_ROOT.glob("build/*/app/stresslab")]
    return max(built, key=lambda path: path.stat().st_mtime) if built else None


HEADLESS = """\
import sys
from pathlib import Path

sys.path.insert(0, {models!r})
import stresslab
import golden
from analytic_solver import AnalyticSolver

document = stresslab.active_document()
for case in golden.load_cases(Path({models!r})):
    # Everything in the file reaches the model: nothing is kept as an uninterpreted keyword.
    with document.transaction("check"):
        document.cmd("model.set_units", **case.units)
        document.cmd("inp.import", path=str(case.model))
    kinds = sorted({{item.type for item in document.objects()}})
    kept = [item.properties["text"] for item in document.find_by_type("abaqus_keyword")]
    document.undo()
    assert not kept, (case.id, kept)
    print("kinds:", case.id, " ".join(kinds))

    work = Path("работа") / case.id
    golden.run_case(case, AnalyticSolver(case), work).assert_passed()
    assert len(document) == 0
    assert not golden.run_case(case, AnalyticSolver(case, scale=1.1), work).passed
    print("passed:", case.id)
"""


@pytest.mark.skipif(stresslab.embedded, reason="starts the executable; the plain run covers it")
def test_the_input_files_go_through_the_application(tmp_path: Path) -> None:
    executable = _executable()
    if executable is None or not executable.exists():
        pytest.skip("the stresslab executable is not built")
    folder = tmp_path / "эталонные модели"
    models = folder / "модели и обвязка"
    shutil.copytree(MODELS, models, ignore=shutil.ignore_patterns("__pycache__"))
    script = folder / "прогон.py"
    script.write_text(HEADLESS.format(models=str(models)), encoding="utf-8")
    result = subprocess.run(
        [executable, "--headless", "--log-level", "warning", "run-script", script],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        stdin=subprocess.DEVNULL,
        cwd=folder,
        timeout=600,
    )
    assert result.returncode == 0, f"{result.returncode}\n{result.stdout}\n{result.stderr}"
    passed = [line.split()[1] for line in result.stdout.splitlines() if line.startswith("passed:")]
    assert passed == [case.id for case in CASES], result.stdout
    common = {"material", "mesh", "node_set", "element_set", "static_step", "output_request"}
    common |= {"boundary_condition", "abaqus_input", "abaqus_element_types"}
    for line in result.stdout.splitlines():
        if line.startswith("kinds:"):
            kinds = set(line.split()[2:])
            assert common <= kinds, line
            assert kinds & {"concentrated_load", "pressure", "gravity"}, line
            assert kinds & {"solid_section", "shell_section", "beam_section"}, line
