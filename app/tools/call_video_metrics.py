#!/usr/bin/env python3

from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image


def eye_span(points):
    p = np.array(points)
    return float(np.linalg.norm(p[42:48].mean(0) - p[36:42].mean(0)))


def jitter(marks, pick):
    accel, speed = [], []
    for t in range(1, len(marks) - 1):
        trio = [pick(marks[t + k]) for k in (-1, 0, 1)]
        if any(entry is None for entry in trio) or not marks[t].get("live"):
            continue
        a, b, c = (np.array(entry) for entry in trio)
        span = eye_span(marks[t]["live"])
        accel.append(np.median(np.linalg.norm(a - 2 * b + c, axis=1)) / span)
        speed.append(np.median(np.linalg.norm(c - a, axis=1)) / 2 / span)
    if not accel:
        return None
    return {
        "second_difference_pct": round(float(np.mean(accel)) * 100, 2),
        "second_difference_p95_pct": round(float(np.percentile(accel, 95)) * 100, 2),
        "speed_pct": round(float(np.mean(speed)) * 100, 2),
        "frames": len(accel),
    }


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
        a = np.asarray(Image.open(o0).convert("L"), np.float32)
        b = np.asarray(Image.open(o1).convert("L"), np.float32)
        c = np.asarray(Image.open(cams[i0]).convert("L").resize(a.shape[::-1]), np.float32)
        d = np.asarray(Image.open(cams[i1]).convert("L").resize(a.shape[::-1]), np.float32)
        change_out.append(float(np.abs(b - a).mean()))
        change_cam.append(float(np.abs(d - c).mean()))
    return {"output_grey_levels": round(float(np.mean(change_out)), 2), "camera_grey_levels": round(float(np.mean(change_cam)), 2)}


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
