"""A stand-in solver for the tests of the golden-model harness: it does not solve anything, it
writes the exact solutions of the golden problems as Results IR on the mesh of the model.

It shows that the places of the checks lie on the real meshes and that the whole path works:
input file -> Model IR -> solver -> Results IR -> checks. ``scale`` multiplies every value, so a
wrong solver can be imitated. Fields: U and RF nodal, S element-nodal (plate) or nodal (cylinder),
as a reader of solver files may give them.
"""

from __future__ import annotations

from pathlib import Path
from typing import Any

import numpy as np
import stresslab
from golden import GoldenCase

OWNER = "fem.model.structural"
DISPLACEMENT = ["U1", "U2", "U3"]
REACTION = ["RF1", "RF2", "RF3"]
STRESS = ["S11", "S22", "S33", "S12", "S13", "S23"]


class AnalyticSolver:
    def __init__(self, case: GoldenCase, scale: float = 1.0) -> None:
        self.case = case
        self.scale = scale

    def solve(self, model: stresslab.Document, workdir: Path) -> stresslab.Document:
        meshes = model.find_by_type("mesh")
        assert len(meshes) == 1, f"one mesh expected, found {len(meshes)}"
        mesh = meshes[0].data
        fields = getattr(self, f"_{self.case.problem}")(mesh, self.case.parameters)
        return results_document(meshes[0].name, mesh, fields, model.units, self.scale)

    # Exact solutions: name -> (position, components, quantity, arrays without "values", values).

    @staticmethod
    def _cantilever(mesh: Any, p: dict[str, float]) -> dict[str, tuple]:
        x = np.asarray(mesh.x)
        inertia = p["b"] * p["h"] ** 3 / 12
        deflection = -p["P"] * x**2 * (3 * p["L"] - x) / (6 * p["E"] * inertia)
        root = np.isclose(x, 0.0)
        reaction = np.zeros((mesh.node_count, 3))
        reaction[root, 2] = p["P"] / root.sum()
        return {
            "U": _nodal(DISPLACEMENT, "length", _columns(mesh, z=deflection)),
            "RF": _nodal(REACTION, "force", reaction),
        }

    @staticmethod
    def _plate_hole(mesh: Any, p: dict[str, float]) -> dict[str, tuple]:
        # Kirsch, scaled so that the edge of the hole has the factor of a plate of finite width.
        ratio = 2 * p["a"] / p["W"]
        factor = (3 - 3.14 * ratio + 3.667 * ratio**2 - 1.527 * ratio**3) / (1 - ratio) / 3
        sigma, a = p["sigma"] * factor, p["a"]
        # A row for each node of each element.
        element_parts, local_parts, node_parts = [], [], []
        for n, block in enumerate(mesh.blocks):
            connectivity = np.asarray(block.connectivity)
            count, width = connectivity.shape
            element_parts.append(np.repeat(np.arange(count) + mesh.block_offset(n), width))
            local_parts.append(np.tile(np.arange(width), count))
            node_parts.append(connectivity.reshape(-1))
        elements, local = np.concatenate(element_parts), np.concatenate(local_parts)
        nodes = np.concatenate(node_parts)
        x, y = np.asarray(mesh.x)[nodes], np.asarray(mesh.y)[nodes]
        r2, theta = x**2 + y**2, np.arctan2(y, x)
        q2, q4 = a**2 / r2, a**4 / r2**2
        cos2, sin2 = np.cos(2 * theta), np.sin(2 * theta)
        srr = sigma / 2 * (1 - q2) + sigma / 2 * (1 - 4 * q2 + 3 * q4) * cos2
        stt = sigma / 2 * (1 + q2) - sigma / 2 * (1 + 3 * q4) * cos2
        srt = -sigma / 2 * (1 + 2 * q2 - 3 * q4) * sin2
        stress = _polar_to_cartesian(srr, stt, srt, theta, np.zeros_like(x))
        return {
            "S": ("elementNodal", STRESS, "pressure", {"element": elements, "node": local}, stress)
        }

    @staticmethod
    def _thick_cylinder(mesh: Any, p: dict[str, float]) -> dict[str, tuple]:
        a, b, pressure, nu = p["a"], p["b"], p["p"], p["nu"]
        x, y = np.asarray(mesh.x), np.asarray(mesh.y)
        r, theta = np.hypot(x, y), np.arctan2(y, x)
        big_a = pressure * a**2 / (b**2 - a**2)
        big_b = pressure * a**2 * b**2 / (b**2 - a**2)
        srr, stt = big_a - big_b / r**2, big_a + big_b / r**2
        stress = _polar_to_cartesian(srr, stt, np.zeros_like(r), theta, nu * (srr + stt))
        radial = (1 + nu) / p["E"] * ((1 - 2 * nu) * big_a * r + big_b / r)
        displacement = _columns(mesh, x=radial * np.cos(theta), y=radial * np.sin(theta))
        return {
            "U": _nodal(DISPLACEMENT, "length", displacement),
            "S": _nodal(STRESS, "pressure", stress),
        }

    @staticmethod
    def _cube_gravity(mesh: Any, p: dict[str, float]) -> dict[str, tuple]:
        bottom = np.isclose(np.asarray(mesh.z), 0.0)
        reaction = np.zeros((mesh.node_count, 3))
        reaction[bottom, 2] = p["rho"] * p["g"] * p["s"] ** 3 / bottom.sum()
        return {"RF": _nodal(REACTION, "force", reaction)}


def _nodal(components: list[str], quantity: str, values: np.ndarray) -> tuple:
    return ("nodal", components, quantity, {"node": np.arange(len(values), dtype=np.int32)}, values)


def _columns(mesh: Any, **columns: np.ndarray) -> np.ndarray:
    values = np.zeros((mesh.node_count, 3))
    for n, axis in enumerate("xyz"):
        if axis in columns:
            values[:, n] = columns[axis]
    return values


def _polar_to_cartesian(
    srr: np.ndarray, stt: np.ndarray, srt: np.ndarray, theta: np.ndarray, szz: np.ndarray
) -> np.ndarray:
    c, s = np.cos(theta), np.sin(theta)
    zero = np.zeros_like(srr)
    return np.column_stack(
        (
            srr * c**2 + stt * s**2 - 2 * srt * s * c,
            srr * s**2 + stt * c**2 + 2 * srt * s * c,
            szz,
            (srr - stt) * s * c + srt * (c**2 - s**2),
            zero,
            zero,
        )
    )


def results_document(
    mesh_name: str, mesh: Any, fields: dict[str, tuple], units: Any, scale: float
) -> stresslab.Document:
    """Results IR with one step and one frame on ``mesh`` (shared with the model, not copied)."""
    previous = stresslab.active_document()
    results = stresslab.new_document()
    stresslab.set_active_document(previous)
    results.set_owner_enabled(OWNER)

    def build(ctx: stresslab.CommandContext) -> None:
        mesh_object = ctx.create_object("mesh", "fem.core", name=mesh_name, data=mesh)
        step = ctx.create_object(
            "result_step", OWNER, name="Step-1", properties={"number": 1, "procedure": "static"}
        )
        frame = ctx.create_object(
            "result_frame",
            OWNER,
            name="Increment 1",
            parent=step,
            properties={"increment": 1, "time": 1.0},
        )
        for name, (position, components, quantity, arrays, values) in fields.items():
            data = {key: np.asarray(array, dtype=np.int32) for key, array in arrays.items()}
            data["values"] = np.asarray(values, dtype=np.float64) * scale
            ctx.create_object(
                "result_field",
                OWNER,
                name=name,
                parent=frame,
                dependencies=[mesh_object],
                data=stresslab.ArrayData(data),
                properties={"position": position, "components": components, "quantity": quantity},
            )

    results.register_command("golden.analytic_results", build, owner="golden.analytic")
    if isinstance(units, dict) and units:
        results.cmd("model.set_units", **units)
    results.cmd("golden.analytic_results")
    return results
