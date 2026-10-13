#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
from pathlib import Path

from PIL import Image, ImageDraw


REPOSITORY = Path(__file__).resolve().parents[2]
DEFAULT_OUTPUT = REPOSITORY / "www" / "public" / "assets" / "land-mask.png"


def decode_arcs(topology: dict) -> list[list[tuple[float, float]]]:
    scale_x, scale_y = topology["transform"]["scale"]
    shift_x, shift_y = topology["transform"]["translate"]
    arcs = []
    for arc in topology["arcs"]:
        x = y = 0
        points = []
        for dx, dy in arc:
            x += dx
            y += dy
            points.append((x * scale_x + shift_x, y * scale_y + shift_y))
        arcs.append(points)
    return arcs


def ring_points(ring: list[int], arcs: list[list[tuple[float, float]]]) -> list[tuple[float, float]]:
    points: list[tuple[float, float]] = []
    for index in ring:
        arc = arcs[index] if index >= 0 else arcs[~index][::-1]
        points.extend(arc if not points else arc[1:])
    return points


def polygons(topology: dict, name: str) -> list[list[list[tuple[float, float]]]]:
    arcs = decode_arcs(topology)
    shapes = []
    for geometry in topology["objects"][name]["geometries"]:
        if geometry["type"] == "Polygon":
            parts = [geometry["arcs"]]
        elif geometry["type"] == "MultiPolygon":
            parts = geometry["arcs"]
        else:
            continue
        for part in parts:
            shapes.append([ring_points(ring, arcs) for ring in part])
    return shapes


def unwrap(ring: list[tuple[float, float]]) -> list[tuple[float, float]]:
    points = []
    offset = 0.0
    previous = None
    for longitude, latitude in ring:
        if previous is not None:
            step = longitude - previous
            if step > 180:
                offset -= 360
            elif step < -180:
                offset += 360
        previous = longitude
        points.append((longitude + offset, latitude))
    return points


def rasterize(shapes, cells_per_degree: int, supersample: int, min_latitude: float) -> Image.Image:
    fine = cells_per_degree * supersample
    width, height = 360 * fine, 180 * fine
    canvas = Image.new("L", (width, height), 0)
    draw = ImageDraw.Draw(canvas)

    def paint(ring, fill):
        if len(ring) < 3:
            return
        points = unwrap(ring)
        for shift in (-360, 0, 360):
            draw.polygon(
                [((longitude + shift + 180) * fine, (90 - latitude) * fine) for longitude, latitude in points],
                fill=fill,
            )

    for rings in shapes:
        paint(rings[0], 255)
        for hole in rings[1:]:
            paint(hole, 0)
    cutoff = int((90 - min_latitude) * fine)
    draw.rectangle((0, cutoff, width, height), fill=0)
    coverage = canvas.resize((360 * cells_per_degree, 180 * cells_per_degree), Image.Resampling.BOX)
    return coverage.point(lambda value: 255 if value >= 128 else 0).convert("1")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("topology", type=Path)
    parser.add_argument("output", type=Path, nargs="?", default=DEFAULT_OUTPUT)
    parser.add_argument("--object", default="land")
    parser.add_argument("--cells-per-degree", type=int, default=4)
    parser.add_argument("--supersample", type=int, default=4)
    parser.add_argument("--min-latitude", type=float, default=-60.0)
    args = parser.parse_args()

    topology = json.loads(args.topology.read_text(encoding="utf-8"))
    mask = rasterize(
        polygons(topology, args.object),
        args.cells_per_degree,
        args.supersample,
        args.min_latitude,
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    mask.save(args.output, optimize=True)
    print(f"{args.output}: {mask.width}x{mask.height}, {args.output.stat().st_size} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
