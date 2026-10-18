"""Writes the input files of the golden models (tests/models/README.md) from their descriptions.

Each ``<id>.toml`` next to this script names the problem, the element type, the parameters of the
model and the density of the mesh; this script builds the mesh and writes ``<id>.inp`` in the subset
of the Abaqus input language that both Abaqus and CalculiX read. Run it after changing a
description::

    python tests/models/generate.py            # all models
    python tests/models/generate.py --check    # fails if a file differs from what it would write

The files are committed: solvers and the tests read them, not this script. The test
``test_golden_models.py`` checks that they are what this script writes.
"""

from __future__ import annotations

import argparse
import math
import sys
import tomllib
from collections.abc import Callable, Iterable, Sequence
from dataclasses import dataclass, field
from pathlib import Path

HERE = Path(__file__).resolve().parent

Point = tuple[float, float, float]

# Corners of a hexahedron in the grid of indices, in the order of Abaqus (and of the neutral
# numbering of StressLab): the face z = 0 counter-clockwise seen from +z, then the face z = 1.
HEX_CORNERS = (
    (0, 0, 0),
    (1, 0, 0),
    (1, 1, 0),
    (0, 1, 0),
    (0, 0, 1),
    (1, 0, 1),
    (1, 1, 1),
    (0, 1, 1),
)
# Mid-edge nodes 9..20 of C3D20 by the corners (from 0) they lie between.
HEX20_EDGES = (
    (0, 1), (1, 2), (2, 3), (3, 0), (4, 5), (5, 6), (6, 7), (7, 4), (0, 4), (1, 5), (2, 6), (3, 7),
)  # fmt: skip
# Faces S1..S6 of hexahedra by their corners (from 0).
HEX_FACES = ((0, 1, 2, 3), (4, 7, 6, 5), (0, 4, 5, 1), (1, 5, 6, 2), (2, 6, 7, 3), (3, 7, 4, 0))
# Mid-edge nodes 5..10 of C3D10 by the corners (from 0) they lie between.
TET10_EDGES = ((0, 1), (1, 2), (2, 0), (0, 3), (1, 3), (2, 3))


@dataclass
class Mesh:
    """Nodes merged by their coordinates and elements of one type."""

    element_type: str
    nodes: list[Point] = field(default_factory=list)
    elements: list[list[int]] = field(default_factory=list)  # node numbers from 1
    _index: dict[tuple[int, int, int], int] = field(default_factory=dict)

    def node(self, point: Sequence[float]) -> int:
        # Coordinates are merged at 1e-6 of a millimetre: the meshes have no closer nodes.
        key = (round(point[0] * 1e6), round(point[1] * 1e6), round(point[2] * 1e6))
        number = self._index.get(key)
        if number is None:
            self.nodes.append((_clean(point[0]), _clean(point[1]), _clean(point[2])))
            number = len(self.nodes)
            self._index[key] = number
        return number

    def coordinates(self, number: int) -> Point:
        return self.nodes[number - 1]

    def node_set(self, inside: Callable[[Point], bool]) -> list[int]:
        return [number for number, point in enumerate(self.nodes, start=1) if inside(point)]

    def faces(self, inside: Callable[[Point], bool]) -> list[tuple[int, int]]:
        """(element, face) of the hexahedron faces whose corners are all ``inside``."""
        result = []
        for number, element in enumerate(self.elements, start=1):
            for face, corners in enumerate(HEX_FACES, start=1):
                if all(inside(self.coordinates(element[corner])) for corner in corners):
                    result.append((number, face))
        return result


def _clean(value: float) -> float:
    rounded = round(value, 9)
    return 0.0 if rounded == 0 else rounded


def _lerp(a: float, b: float, t: float) -> float:
    return a + (b - a) * t


# Meshes -------------------------------------------------------------------------------------------


def hex_grid(
    element_type: str, divisions: Sequence[int], at: Callable[[float, float, float], Point]
) -> Mesh:
    """Hexahedra (C3D8, C3D20) or tetrahedra (C3D10) of a grid of indices mapped by ``at``:
    indices (i, j, k) with 0 <= i <= divisions[0]... ; mid-edge nodes are mapped from half indices,
    so they lie on curved edges. ``at`` must keep the orientation (a right-handed map)."""
    mesh = Mesh(element_type)
    ni, nj, nk = divisions
    for k in range(nk):
        for j in range(nj):
            for i in range(ni):
                corners = [(i + di, j + dj, k + dk) for di, dj, dk in HEX_CORNERS]
                if element_type == "C3D10":
                    for tet in _kuhn_tetrahedra(corners):
                        mesh.elements.append(_quadratic(mesh, tet, TET10_EDGES, at))
                elif element_type == "C3D20":
                    mesh.elements.append(_quadratic(mesh, corners, HEX20_EDGES, at))
                else:
                    mesh.elements.append([mesh.node(at(*corner)) for corner in corners])
    return mesh


def _kuhn_tetrahedra(corners: list[tuple[int, int, int]]) -> list[list[tuple[int, int, int]]]:
    """Six tetrahedra around the diagonal from the first to the seventh corner; the same split of
    every cube of a grid gives a conforming mesh. Each is ordered for a positive volume."""
    origin = corners[0]
    result = []
    for axes in ((0, 1, 2), (0, 2, 1), (1, 0, 2), (1, 2, 0), (2, 0, 1), (2, 1, 0)):
        path = [origin]
        point = list(origin)
        for axis in axes:
            point[axis] += 1
            path.append((point[0], point[1], point[2]))
        if _volume(*path) < 0:
            path[1], path[2] = path[2], path[1]
        result.append(path)
    return result


def _volume(*corners: Sequence[float]) -> float:
    a, b, c, d = corners
    u = [b[n] - a[n] for n in range(3)]
    v = [c[n] - a[n] for n in range(3)]
    w = [d[n] - a[n] for n in range(3)]
    return (
        u[0] * (v[1] * w[2] - v[2] * w[1])
        - u[1] * (v[0] * w[2] - v[2] * w[0])
        + u[2] * (v[0] * w[1] - v[1] * w[0])
    )


def _quadratic(
    mesh: Mesh,
    corners: Sequence[tuple[int, int, int]],
    edges: Iterable[tuple[int, int]],
    at: Callable[[float, float, float], Point],
) -> list[int]:
    nodes = [mesh.node(at(*corner)) for corner in corners]
    for first, second in edges:
        middle = [(corners[first][n] + corners[second][n]) / 2 for n in range(3)]
        nodes.append(mesh.node(at(*middle)))
    return nodes


def plate_quads(parameters: dict[str, float], density: dict[str, int]) -> Mesh:
    """Quadrilaterals of a quarter of a plate with a hole (S4): an O-grid from the hole to the
    square of the half width around it, graded towards the hole, then a regular grid to the loaded
    edge."""
    a = parameters["a"]
    half_width = parameters["W"] / 2
    half_length = parameters["Lp"] / 2
    around, radial, outer = density["around"], density["radial"], density["outer"]
    # Radial spacing grows geometrically; the first element on the x axis is square.
    first = a * (math.pi / 4) / around
    ratio = _growth(first / (half_width - a), radial)
    fractions = [(ratio**k - 1) / (ratio**radial - 1) for k in range(radial + 1)]

    mesh = Mesh("S4")
    for octant in range(2):
        rays = []
        for i in range(around + 1):
            angle = math.pi / 4 * (octant + i / around)
            hole = (a * math.cos(angle), a * math.sin(angle))
            if octant == 0:
                edge = (half_width, half_width * i / around)
            else:
                edge = (half_width * (1 - i / around), half_width)
            rays.append(
                [(_lerp(hole[0], edge[0], s), _lerp(hole[1], edge[1], s)) for s in fractions]
            )
        for i in range(around):
            for k in range(radial):
                quad = [rays[i][k], rays[i][k + 1], rays[i + 1][k + 1], rays[i + 1][k]]
                _add_quad(mesh, quad)
    for i in range(outer):
        for j in range(around):
            x0 = _lerp(half_width, half_length, i / outer)
            x1 = _lerp(half_width, half_length, (i + 1) / outer)
            y0, y1 = half_width * j / around, half_width * (j + 1) / around
            _add_quad(mesh, [(x0, y0), (x1, y0), (x1, y1), (x0, y1)])
    return mesh


def _growth(first: float, count: int) -> float:
    """The ratio q with (q - 1) / (q**count - 1) = first, by bisection."""
    low, high = 1.0 + 1e-12, 2.0
    for _ in range(200):
        middle = (low + high) / 2
        if (middle - 1) / (middle**count - 1) > first:
            low = middle
        else:
            high = middle
    return (low + high) / 2


def _add_quad(mesh: Mesh, corners: list[tuple[float, float]]) -> None:
    area = sum(
        corners[n][0] * corners[(n + 1) % 4][1] - corners[(n + 1) % 4][0] * corners[n][1]
        for n in range(4)
    )
    if area < 0:
        corners = corners[::-1]
    mesh.elements.append([mesh.node((x, y, 0.0)) for x, y in corners])


def extrude(plate: Mesh, thickness: float, layers: int) -> Mesh:
    """C3D8 from S4: each quadrilateral (counter-clockwise from +z) becomes ``layers`` hexahedra."""
    mesh = Mesh("C3D8")
    for quad in plate.elements:
        bottom = [plate.coordinates(number) for number in quad]
        for layer in range(layers):
            z0, z1 = thickness * layer / layers, thickness * (layer + 1) / layers
            mesh.elements.append(
                [mesh.node((x, y, z0)) for x, y, _ in bottom]
                + [mesh.node((x, y, z1)) for x, y, _ in bottom]
            )
    return mesh


# Models -------------------------------------------------------------------------------------------


@dataclass
class Model:
    """What goes into the file besides nodes and elements."""

    mesh: Mesh
    element_set: str
    node_sets: dict[str, list[int]] = field(default_factory=dict)
    surfaces: dict[str, list[tuple[int, int]]] = field(default_factory=dict)
    section: list[str] = field(default_factory=list)
    boundary: list[str] = field(default_factory=list)
    loads: list[str] = field(default_factory=list)


def _near(value: float, target: float) -> bool:
    return abs(value - target) < 1e-6


def cantilever(element: str, p: dict[str, float], density: dict[str, list[int]]) -> Model:
    length, width, height = p["L"], p["b"], p["h"]
    divisions = density["divisions"]
    if element == "B31":
        mesh = Mesh("B31")
        count = divisions[0]
        for i in range(count):
            mesh.elements.append(
                [
                    mesh.node((length * i / count, 0.0, 0.0)),
                    mesh.node((length * (i + 1) / count, 0, 0)),
                ]
            )
        section = [
            "*BEAM SECTION, ELSET=BEAM, MATERIAL=STEEL, SECTION=RECT",
            f"{_number(width)}, {_number(height)}",
            "0., 0., -1.",
        ]
    else:
        ni, nj, nk = divisions

        def at(i: float, j: float, k: float) -> Point:
            return (length * i / ni, width * (j / nj - 0.5), height * (k / nk - 0.5))

        mesh = hex_grid(element, divisions, at)
        section = ["*SOLID SECTION, ELSET=BEAM, MATERIAL=STEEL"]
    root = mesh.node_set(lambda point: _near(point[0], 0.0))
    tip = mesh.node_set(lambda point: _near(point[0], length))
    return Model(
        mesh,
        "BEAM",
        node_sets={"ROOT": root, "TIP": tip},
        section=section,
        boundary=["ROOT, ENCASTRE"],
        loads=["*CLOAD", f"TIP, 3, {_number(-p['P'] / len(tip))}"],
    )


def plate_hole(element: str, p: dict[str, float], density: dict[str, int]) -> Model:
    half_length, half_width, thickness = p["Lp"] / 2, p["W"] / 2, p["t"]
    plate = plate_quads(p, density)
    if element == "S4":
        mesh = plate
        loaded = mesh.node_set(lambda point: _near(point[0], half_length))
        ends = [
            n
            for n in loaded
            if _near(mesh.coordinates(n)[1], 0) or _near(mesh.coordinates(n)[1], half_width)
        ]
        middle = [n for n in loaded if n not in ends]
        # The edge force of the stress, consistent for linear edges: half on the end nodes.
        force = p["sigma"] * thickness * half_width / density["around"]
        return Model(
            mesh,
            "PLATE",
            node_sets={
                "XSYM": mesh.node_set(lambda point: _near(point[0], 0.0)),
                "YSYM": mesh.node_set(lambda point: _near(point[1], 0.0)),
                "ALLN": mesh.node_set(lambda point: True),
                "EDGE_MID": middle,
                "EDGE_END": ends,
            },
            section=["*SHELL SECTION, ELSET=PLATE, MATERIAL=STEEL", _number(thickness)],
            boundary=["XSYM, XSYMM", "YSYM, YSYMM", "ALLN, 3, 3"],
            loads=[
                "*CLOAD",
                f"EDGE_MID, 1, {_number(force)}",
                f"EDGE_END, 1, {_number(force / 2)}",
            ],
        )
    mesh = extrude(plate, thickness / 2, density["thickness"])
    return Model(
        mesh,
        "PLATE",
        node_sets={
            "XSYM": mesh.node_set(lambda point: _near(point[0], 0.0)),
            "YSYM": mesh.node_set(lambda point: _near(point[1], 0.0)),
            "ZSYM": mesh.node_set(lambda point: _near(point[2], 0.0)),
        },
        surfaces={"LOADED": mesh.faces(lambda point: _near(point[0], half_length))},
        section=["*SOLID SECTION, ELSET=PLATE, MATERIAL=STEEL"],
        boundary=["XSYM, XSYMM", "YSYM, YSYMM", "ZSYM, ZSYMM"],
        loads=["*DSLOAD", f"LOADED, P, {_number(-p['sigma'])}"],
    )


def thick_cylinder(element: str, p: dict[str, float], density: dict[str, int]) -> Model:
    inner, outer, length = p["a"], p["b"], p["t"]
    divisions = (density["radial"], density["around"], density["axial"])

    def at(i: float, j: float, k: float) -> Point:
        radius = _lerp(inner, outer, i / divisions[0])
        angle = math.pi / 2 * j / divisions[1]
        return (radius * math.cos(angle), radius * math.sin(angle), length * k / divisions[2])

    mesh = hex_grid(element, divisions, at)
    return Model(
        mesh,
        "WALL",
        node_sets={
            "XSYM": mesh.node_set(lambda point: _near(point[0], 0.0)),
            "YSYM": mesh.node_set(lambda point: _near(point[1], 0.0)),
            "ENDS": mesh.node_set(lambda point: _near(point[2], 0.0) or _near(point[2], length)),
        },
        surfaces={"INNER": mesh.faces(lambda point: _near(math.hypot(point[0], point[1]), inner))},
        section=["*SOLID SECTION, ELSET=WALL, MATERIAL=STEEL"],
        boundary=["XSYM, XSYMM", "YSYM, YSYMM", "ENDS, 3, 3"],
        loads=["*DSLOAD", f"INNER, P, {_number(p['p'])}"],
    )


def cube_gravity(element: str, p: dict[str, float], density: dict[str, list[int]]) -> Model:
    side = p["s"]
    ni, nj, nk = density["divisions"]

    def at(i: float, j: float, k: float) -> Point:
        return (side * i / ni, side * j / nj, side * k / nk)

    mesh = hex_grid(element, (ni, nj, nk), at)
    return Model(
        mesh,
        "CUBE",
        node_sets={"BOTTOM": mesh.node_set(lambda point: _near(point[2], 0.0))},
        section=["*SOLID SECTION, ELSET=CUBE, MATERIAL=STEEL"],
        boundary=["BOTTOM, ENCASTRE"],
        loads=["*DLOAD", f"CUBE, GRAV, {_number(p['g'])}, 0., 0., -1."],
    )


PROBLEMS: dict[str, Callable[..., Model]] = {
    "cantilever": cantilever,
    "plate_hole": plate_hole,
    "thick_cylinder": thick_cylinder,
    "cube_gravity": cube_gravity,
}


# Writing ------------------------------------------------------------------------------------------


def _number(value: float) -> str:
    text = format(value, ".12g")
    if "e" not in text and "." not in text:
        text += "."
    return "0." if text in ("-0.", "-0") else text


def _rows(values: Sequence[object], per_line: int) -> list[str]:
    return [
        ", ".join(str(value) for value in values[start : start + per_line])
        for start in range(0, len(values), per_line)
    ]


def render(model_id: str, description: dict) -> str:
    problem, element = description["problem"], description["element"]
    p = description["parameters"]
    model = PROBLEMS[problem](element, p, description["mesh"])
    mesh = model.mesh
    lines = [
        "*HEADING",
        f"StressLab golden model {model_id}: {description['title']}",
        "** Written by tests/models/generate.py from " + f"{model_id}.toml; do not edit by hand.",
        "** Units: mm, t, s (N, MPa).",
        "*NODE",
    ]
    for number, (x, y, z) in enumerate(mesh.nodes, start=1):
        lines.append(f"{number}, {_number(x)}, {_number(y)}, {_number(z)}")
    lines.append(f"*ELEMENT, TYPE={mesh.element_type}, ELSET={model.element_set}")
    for number, nodes in enumerate(mesh.elements, start=1):
        # At most 16 entries a line; longer elements go on in the next line.
        rows = _rows([number, *nodes], 16)
        lines.extend(row + "," for row in rows[:-1])
        lines.append(rows[-1])
    for name, members in model.node_sets.items():
        lines.append(f"*NSET, NSET={name}")
        lines.extend(_rows(members, 16))
    for name, faces in model.surfaces.items():
        lines.append(f"*SURFACE, NAME={name}, TYPE=ELEMENT")
        lines.extend(f"{number}, S{face}" for number, face in faces)
    lines += [
        "*MATERIAL, NAME=STEEL",
        "*ELASTIC",
        f"{_number(p['E'])}, {_number(p['nu'])}",
    ]
    if "rho" in p:
        lines += ["*DENSITY", _number(p["rho"])]
    lines += model.section
    lines += ["*BOUNDARY", *model.boundary]
    lines += ["*STEP, NAME=LOAD", "*STATIC", "1., 1.", *model.loads]
    lines += ["*OUTPUT, FIELD", "*NODE OUTPUT", "U, RF", "*ELEMENT OUTPUT", "S", "*END STEP"]
    return "\n".join(lines) + "\n"


def descriptions(folder: Path = HERE) -> dict[str, dict]:
    result = {}
    for path in sorted(folder.glob("*.toml")):
        with path.open("rb") as file:
            result[path.stem] = tomllib.load(file)
    return result


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="only compare with the files")
    parser.add_argument("models", nargs="*", help="ids of the models (default: all)")
    options = parser.parse_args(argv)
    stale = []
    for model_id, description in descriptions().items():
        if options.models and model_id not in options.models:
            continue
        text = render(model_id, description)
        path = HERE / description["model"]
        if options.check:
            if not path.exists() or path.read_text(encoding="utf-8") != text:
                stale.append(path.name)
        else:
            path.write_text(text, encoding="utf-8", newline="\n")
            print(f"{path.name}: {text.count(chr(10))} lines")
    if stale:
        print("differ from generate.py:", ", ".join(stale), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
