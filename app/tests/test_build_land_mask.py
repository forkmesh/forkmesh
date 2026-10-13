import importlib.util
from pathlib import Path

import pytest

pytest.importorskip("PIL")

TOOL = Path(__file__).resolve().parents[1] / "tools" / "build_land_mask.py"
SPEC = importlib.util.spec_from_file_location("build_land_mask", TOOL)
build_land_mask = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(build_land_mask)


def topology(arcs, rings):
    return {
        "type": "Topology",
        "transform": {"scale": [1, 1], "translate": [0, 0]},
        "arcs": arcs,
        "objects": {
            "land": {
                "type": "GeometryCollection",
                "geometries": [{"type": "Polygon", "arcs": rings}],
            }
        },
    }


def test_rings_join_delta_decoded_and_reversed_arcs():
    arcs = [
        [[0, 0], [10, 0], [0, 10]],
        [[10, 10], [-10, 0], [0, -10]],
        [[0, 0], [0, 10], [10, 0]],
    ]
    square = [(0, 0), (10, 0), (10, 10), (0, 10), (0, 0)]

    assert build_land_mask.polygons(topology(arcs, [[0, 1]]), "land") == [[square]]
    assert build_land_mask.polygons(topology(arcs, [[0, ~2]]), "land") == [[square]]


def test_a_ring_across_the_antimeridian_stays_at_the_map_edges():
    ring = [(170, 10), (-170, 10), (-170, 20), (170, 20), (170, 10)]
    mask = build_land_mask.rasterize([[ring]], 1, 1, -60)

    assert mask.getpixel((355, 75)) == 255
    assert mask.getpixel((5, 75)) == 255
    assert mask.getpixel((180, 75)) == 0
    assert mask.getpixel((100, 75)) == 0
