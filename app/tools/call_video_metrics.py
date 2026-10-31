#!/usr/bin/env python3

from __future__ import annotations

import json
import math
import statistics
import sys
from pathlib import Path

from PIL import Image, ImageChops, ImageStat


def centroid(points):
    return [sum(axis) / len(points) for axis in zip(*points)]


def eye_span(points):
    return math.dist(centroid(points[42:48]), centroid(points[36:42]))


def percentile(values, q):
    ordered = sorted(values)
    rank = (len(ordered) - 1) * q / 100
    low = math.floor(rank)
    high = min(low + 1, len(ordered) - 1)
    return ordered[low] + (ordered[high] - ordered[low]) * (rank - low)


def jitter(marks, pick):
    accel, speed = [], []
    for t in range(1, len(marks) - 1):
        trio = [pick(marks[t + k]) for k in (-1, 0, 1)]
        if any(entry is None for entry in trio) or not marks[t].get("live"):
            continue
        a, b, c = trio
        span = eye_span(marks[t]["live"])
        accel.append(statistics.median(
            math.hypot(*(pa - 2 * pb + pc for pa, pb, pc in zip(*point)))
            for point in zip(a, b, c)) / span)
        speed.append(statistics.median(
            math.dist(pa, pc) for pa, pc in zip(a, c)) / 2 / span)
    if not accel:
        return None
    return {
        "second_difference_pct": round(statistics.fmean(accel) * 100, 2),
        "second_difference_p95_pct": round(percentile(accel, 95) * 100, 2),
        "speed_pct": round(statistics.fmean(speed) * 100, 2),
        "frames": len(accel),
    }


def grey_change(first, second):
    return ImageStat.Stat(ImageChops.difference(first, second)).mean[0]


def flicker(out_dir: Path, frames_dir: Path):
    outs = sorted(out_dir.glob("out_*.png"))
    if len(outs) < 3:
        return None
    cams = sorted(p for p in frames_dir.iterdir() if p.suffix in (".png", ".jpg"))
    indices = [int(p.stem.split("_")[1]) for p in outs]
    if any(b - a != 1 for a, b in zip(indices, indices[1:])):
        return None
    change_out, change_cam = [], []
    for (i0, o0), (i1, o1) in zip(zip(indices, outs), zip(indices[1:], outs[1:])):
        a = Image.open(o0).convert("L")
        b = Image.open(o1).convert("L")
        c = Image.open(cams[i0]).convert("L").resize(a.size)
        d = Image.open(cams[i1]).convert("L").resize(a.size)
        change_out.append(grey_change(a, b))
        change_cam.append(grey_change(c, d))
    return {"output_grey_levels": round(statistics.fmean(change_out), 2), "camera_grey_levels": round(statistics.fmean(change_cam), 2)}


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print("usage: call_video_metrics.py OUT_DIR FRAMES_DIR")
        return 2
    out_dir, frames_dir = Path(argv[0]), Path(argv[1])
    marks = json.loads((out_dir / "marks.json").read_text())
    report = {
        "landmarks": jitter(marks, lambda m: m.get("live")),
        "head_ring": jitter(marks, lambda m: m["head"]["points"][78:] if m.get("head") else None),
        "frame_change": flicker(out_dir, frames_dir),
    }
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
