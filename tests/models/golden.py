"""Runs the golden models of ``tests/models`` through a solver and checks the results
(``tests/models/README.md``, ``docs/adr/0026-golden-models.md``).

A solver here is anything that turns a model into results::

    class Solver(Protocol):
        def solve(self, model: stresslab.Document, workdir: Path) -> stresslab.Document: ...

``model`` is the document the input file was imported into (Model IR); the returned document holds
Results IR (``docs/extensions/fem.model.structural.md``): ``result_step`` -> ``result_frame`` ->
``result_field`` on a mesh of its own. A solver adapter wraps its ``validate``, ``write_input``,
``run`` and ``read_results`` into ``solve``; the checks do not depend on the solver::

    import golden

    for case in golden.load_cases():
        golden.run_case(case, MySolver(), workdir / case.id).assert_passed()

The checks look at the last frame of the last step. A place is given by coordinates (a point or a
box), not by labels or sets, because the mesh of the results is the solver's. Fields and components
have the names of Abaqus (``U``/``U1``, ``S``/``S11``, ``RF``/``RF3``); a field is ``nodal`` or
``elementNodal`` (the values of the elements at a node are averaged).

``run_case`` imports the model into the active document, which must be empty, and undoes the import
afterwards, so the cases can run one after another in one ``stresslab --headless run-script``.
"""

from __future__ import annotations

import math
import tomllib
from collections.abc import Iterable, Mapping
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Protocol

import numpy as np

MODELS = Path(__file__).resolve().parent

REDUCERS = ("mean", "sum", "min", "max", "maxabs")
# Names a formula may use besides the parameters of its model.
_MATH = {name: getattr(math, name) for name in ("pi", "sqrt", "sin", "cos", "tan", "exp", "log")}
_CASE_KEYS = {"title", "problem", "element", "model", "units", "parameters", "mesh", "checks"}
_CHECK_KEYS = {"name", "field", "component", "point", "box", "reduce", "formula", "expected"}
_CHECK_KEYS |= {"rel_tol", "abs_tol"}


class GoldenError(Exception):
    """A description that cannot be read, or results the checks cannot be measured in."""


class Solver(Protocol):
    def solve(self, model: Any, workdir: Path) -> Any:
        """Results IR (a ``stresslab.Document``) of the model in ``model``; files go to
        ``workdir``."""
        ...


@dataclass(frozen=True)
class Check:
    """One value to measure: ``reduce`` of ``field.component`` over the nodes in ``box``."""

    name: str
    field: str
    component: str
    box: tuple[tuple[float, float, float], tuple[float, float, float]]
    reduce: str
    expected: float
    rel_tol: float
    abs_tol: float
    formula: str

    def allowed(self) -> float:
        """The largest deviation that passes."""
        return max(self.abs_tol, self.rel_tol * abs(self.expected))


@dataclass(frozen=True)
class GoldenCase:
    id: str
    title: str
    problem: str
    element: str
    model: Path
    units: dict[str, str]
    parameters: dict[str, float]
    mesh: dict[str, Any]
    checks: tuple[Check, ...]


@dataclass(frozen=True)
class CheckResult:
    check: Check
    measured: float | None = None
    nodes: int = 0
    error: str = ""

    @property
    def deviation(self) -> float | None:
        return None if self.measured is None else self.measured - self.check.expected

    @property
    def passed(self) -> bool:
        deviation = self.deviation
        return not self.error and deviation is not None and abs(deviation) <= self.check.allowed()


@dataclass
class CaseReport:
    case: GoldenCase
    solver: str
    results: list[CheckResult] = field(default_factory=list)

    @property
    def passed(self) -> bool:
        return bool(self.results) and all(result.passed for result in self.results)

    def format(self) -> str:
        lines = [f"{self.case.id} ({self.case.title}) by {self.solver}:"]
        for result in self.results:
            check = result.check
            status = "ok  " if result.passed else "FAIL"
            what = f"{check.field}.{check.component} {check.reduce}"
            if result.error:
                detail = result.error
            else:
                assert result.measured is not None and result.deviation is not None
                relative = f" ({result.deviation / check.expected:+.2%})" if check.expected else ""
                detail = (
                    f"measured {result.measured:.6g}, expected {check.expected:.6g}{relative}, "
                    f"allowed ±{check.allowed():.3g}, {result.nodes} node(s)"
                )
            lines.append(f"  {status} {check.name} [{what}]: {detail}")
        return "\n".join(lines)

    def assert_passed(self) -> None:
        if not self.passed:
            raise AssertionError(self.format())


# Descriptions -------------------------------------------------------------------------------------


def load_cases(folder: Path = MODELS, ids: Iterable[str] = ()) -> list[GoldenCase]:
    """The golden models described by ``<id>.toml`` in ``folder``, sorted by id."""
    wanted = set(ids)
    cases = [load_case(path) for path in sorted(folder.glob("*.toml"))]
    found = {case.id for case in cases}
    if wanted - found:
        raise GoldenError(f"no golden models {sorted(wanted - found)} in {folder}")
    return [case for case in cases if not wanted or case.id in wanted]


def load_case(path: Path) -> GoldenCase:
    try:
        with path.open("rb") as file:
            data = tomllib.load(file)
    except (OSError, tomllib.TOMLDecodeError) as error:
        raise GoldenError(f"{path.name}: {error}") from error
    where = path.name
    _known_keys(data, _CASE_KEYS, where)
    for key in ("title", "problem", "element", "model"):
        _require(data, key, str, where)
    model = path.parent / data["model"]
    if not model.is_file():
        raise GoldenError(f"{where}: no model file {data['model']}")
    parameters = data.get("parameters", {})
    if not all(isinstance(value, int | float) for value in parameters.values()):
        raise GoldenError(f"{where}: parameters must be numbers")
    checks = data.get("checks", [])
    if not checks:
        raise GoldenError(f"{where}: no checks")
    return GoldenCase(
        id=path.stem,
        title=data["title"],
        problem=data["problem"],
        element=data["element"],
        model=model,
        units=dict(data.get("units", {})),
        parameters={key: float(value) for key, value in parameters.items()},
        mesh=dict(data.get("mesh", {})),
        checks=tuple(
            _check(item, parameters, f"{where}, check {n}") for n, item in enumerate(checks, 1)
        ),
    )


def _check(data: Mapping[str, Any], parameters: Mapping[str, float], where: str) -> Check:
    _known_keys(data, _CHECK_KEYS, where)
    for key in ("name", "field", "component", "formula"):
        _require(data, key, str, where)
    _require(data, "expected", int | float, where)
    if ("point" in data) == ("box" in data):
        raise GoldenError(f"{where}: give either point or box")
    if "point" in data:
        point = _vector(data["point"], where)
        box = (point, point)
    else:
        corners = data["box"]
        if not isinstance(corners, list) or len(corners) != 2:
            raise GoldenError(f"{where}: box is [[x, y, z], [x, y, z]]")
        box = (_vector(corners[0], where), _vector(corners[1], where))
        if any(low > high for low, high in zip(*box, strict=True)):
            raise GoldenError(f"{where}: the first corner of the box must be the lower one")
    reduce = data.get("reduce", "mean")
    if reduce not in REDUCERS:
        raise GoldenError(f"{where}: reduce is one of {', '.join(REDUCERS)}")
    rel_tol, abs_tol = float(data.get("rel_tol", 0)), float(data.get("abs_tol", 0))
    if rel_tol < 0 or abs_tol < 0 or rel_tol == abs_tol == 0:
        raise GoldenError(f"{where}: give rel_tol or abs_tol, not negative")
    expected = float(data["expected"])
    exact = evaluate_formula(data["formula"], parameters, where)
    if not math.isclose(expected, exact, rel_tol=1e-5, abs_tol=1e-12):
        raise GoldenError(f"{where}: expected {expected} but the formula gives {exact:.9g}")
    return Check(
        name=data["name"],
        field=data["field"],
        component=data["component"],
        box=box,
        reduce=reduce,
        expected=expected,
        rel_tol=rel_tol,
        abs_tol=abs_tol,
        formula=data["formula"],
    )


def evaluate_formula(formula: str, parameters: Mapping[str, float], where: str = "") -> float:
    """The value of an arithmetic formula of the parameters (Python syntax, ``math`` functions)."""
    try:
        code = compile(formula, "<formula>", "eval")
    except SyntaxError as error:
        raise GoldenError(f"{where}: formula {formula!r}: {error.msg}") from error
    names = {**_MATH, **parameters}
    unknown = sorted(set(code.co_names) - names.keys())
    if unknown:
        raise GoldenError(f"{where}: formula {formula!r} uses unknown names {unknown}")
    return float(eval(code, {"__builtins__": {}}, names))  # noqa: S307 - only known names


def _known_keys(data: Mapping[str, Any], known: set[str], where: str) -> None:
    unknown = sorted(set(data) - known)
    if unknown:
        raise GoldenError(f"{where}: unknown keys {unknown}")


def _require(data: Mapping[str, Any], key: str, kind: Any, where: str) -> None:
    if not isinstance(data.get(key), kind) or isinstance(data.get(key), bool):
        raise GoldenError(f"{where}: {key} is missing or of a wrong type")


def _vector(value: Any, where: str) -> tuple[float, float, float]:
    if (
        not isinstance(value, list)
        or len(value) != 3
        or not all(isinstance(item, int | float) for item in value)
    ):
        raise GoldenError(f"{where}: a point is [x, y, z]")
    return (float(value[0]), float(value[1]), float(value[2]))


# Running ------------------------------------------------------------------------------------------


def run_case(case: GoldenCase, solver: Solver, workdir: Path, document: Any = None) -> CaseReport:
    """Imports the model into ``document`` (the active one by default; it must be empty), solves it
    with ``solver`` in ``workdir`` and measures the checks. The document is empty again afterwards.
    """
    import stresslab

    model = document if document is not None else stresslab.active_document()
    if len(model) != 0:
        raise GoldenError(
            f"{case.id}: the model document must be empty, it has {len(model)} objects"
        )
    workdir.mkdir(parents=True, exist_ok=True)
    stresslab.set_active_document(model)
    with model.transaction(f"Golden model {case.id}"):
        if case.units:
            model.cmd("model.set_units", **case.units)
        model.cmd("inp.import", path=str(case.model))
    try:
        results = solver.solve(model, workdir)
    finally:
        stresslab.set_active_document(model)
        model.undo()
    return evaluate(case, results, solver=type(solver).__name__)


def evaluate(case: GoldenCase, results: Any, solver: str = "") -> CaseReport:
    """Measures the checks of ``case`` in a document of results."""
    report = CaseReport(case, solver)
    try:
        frame = last_frame(results)
    except GoldenError as error:
        report.results = [CheckResult(check, error=str(error)) for check in case.checks]
        return report
    for check in case.checks:
        try:
            measured, nodes = measure(frame, check)
            report.results.append(CheckResult(check, measured, nodes))
        except GoldenError as error:
            report.results.append(CheckResult(check, error=str(error)))
    return report


def last_frame(results: Any) -> Any:
    """The ``result_frame`` with the largest increment in the step with the largest number."""
    steps = results.find_by_type("result_step")
    if not steps:
        raise GoldenError("the results have no result_step")
    step = max(steps, key=lambda item: item.properties.get("number", 0))
    frames = [child for child in step.children if child.type == "result_frame"]
    if not frames:
        raise GoldenError(f"result step {step.name!r} has no frames")
    return max(
        frames,
        key=lambda item: (item.properties.get("increment", 0), item.properties.get("time", 0.0)),
    )


def measure(frame: Any, check: Check) -> tuple[float, int]:
    """``check.reduce`` of the component over the nodes in the box, and the number of the nodes."""
    fields = [
        child
        for child in frame.children
        if child.type == "result_field" and child.name == check.field
    ]
    if not fields:
        names = sorted(child.name for child in frame.children if child.type == "result_field")
        raise GoldenError(f"no field {check.field!r} in frame {frame.name!r} (there are {names})")
    result_field = fields[0]
    components = list(result_field.properties.get("components", []))
    if check.component not in components:
        raise GoldenError(
            f"field {check.field!r} has no component {check.component!r} ({components})"
        )
    meshes = [item for item in result_field.dependencies if item.type == "mesh"]
    if not meshes or result_field.data is None:
        raise GoldenError(f"field {check.field!r} has no mesh or no data")
    mesh = meshes[0].data
    selected = nodes_in_box(mesh, check.box)
    if selected.size == 0:
        raise GoldenError(f"no node in {_box_text(check.box)}")
    values = nodal_values(mesh, result_field, components.index(check.component))
    found = values[selected]
    missing = np.isnan(found)
    if missing.any():
        labels = mesh.node_ids[selected[missing]][:10].tolist()
        raise GoldenError(f"field {check.field!r} has no value at nodes {labels}")
    reducers = {
        "mean": np.mean,
        "sum": np.sum,
        "min": np.min,
        "max": np.max,
        "maxabs": lambda array: array[np.argmax(np.abs(array))],
    }
    return float(reducers[check.reduce](found)), int(selected.size)


def nodes_in_box(mesh: Any, box: tuple[tuple[float, ...], tuple[float, ...]]) -> np.ndarray:
    """Indices of the nodes in the box, its faces widened by 1e-6 of the size of the mesh."""
    coordinates = np.column_stack((mesh.x, mesh.y, mesh.z))
    if len(coordinates) == 0:
        return np.zeros(0, dtype=np.int64)
    size = float(np.linalg.norm(coordinates.max(axis=0) - coordinates.min(axis=0)))
    tolerance = max(size * 1e-6, 1e-12)
    low = np.asarray(box[0]) - tolerance
    high = np.asarray(box[1]) + tolerance
    inside = np.all((coordinates >= low) & (coordinates <= high), axis=1)
    return np.flatnonzero(inside)


def nodal_values(mesh: Any, result_field: Any, column: int) -> np.ndarray:
    """One value of a component per node of the mesh, NaN where the field has none."""
    data = result_field.data
    position = result_field.properties.get("position")
    values = np.asarray(data["values"], dtype=np.float64)
    if values.ndim == 1:
        values = values.reshape(-1, 1)
    component = values[:, column]
    result = np.full(mesh.node_count, np.nan)
    if position == "nodal":
        result[np.asarray(data["node"], dtype=np.int64)] = component
        return result
    if position == "elementNodal":
        nodes = _element_nodes(mesh, np.asarray(data["element"]), np.asarray(data["node"]))
        sums = np.bincount(nodes, weights=component, minlength=mesh.node_count)
        counts = np.bincount(nodes, minlength=mesh.node_count)
        has = counts > 0
        result[has] = sums[has] / counts[has]
        return result
    raise GoldenError(
        f"field {result_field.name!r} is {position!r}; the checks need nodal or elementNodal values"
    )


def _element_nodes(mesh: Any, elements: np.ndarray, local: np.ndarray) -> np.ndarray:
    """Mesh node indices of (global element index, node of the element) pairs."""
    blocks = mesh.blocks
    offsets = np.array([mesh.block_offset(n) for n in range(len(blocks) + 1)], dtype=np.int64)
    if elements.size and (elements.min() < 0 or elements.max() >= offsets[-1]):
        raise GoldenError("an element index of the field is outside the mesh")
    which = np.searchsorted(offsets, elements, side="right") - 1
    result = np.empty(len(elements), dtype=np.int64)
    for n, block in enumerate(blocks):
        rows = which == n
        if rows.any():
            result[rows] = block.connectivity[elements[rows] - offsets[n], local[rows]]
    return result


def _box_text(box: tuple[tuple[float, ...], tuple[float, ...]]) -> str:
    if box[0] == box[1]:
        return f"point {list(box[0])}"
    return f"box {list(box[0])}..{list(box[1])}"
