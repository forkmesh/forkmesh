#!/usr/bin/env python3

from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_call_segmentation_model import FLOAT16, FLOAT32, INT32, load

REPOSITORY = Path(__file__).resolve().parents[2]
FACES = REPOSITORY / "app" / "public" / "assets" / "call-faces"
SIZE = 512
PAD = 192
SOURCES = {
    "maya": ("This_Person_Does_Not_Exist_example.jpg", 0),
    "theo": ("Male_and_female_deepfake.jpg", 0),
    "kofi": ("Man_2.jpg", 0),
    "ingrid": ("Woman_1.jpg", 0),
    "ren": ("Male_and_female_deepfake.jpg", 1024),
}
OLD_CROP = (160.0, 272.0, 704.0 / 512.0)


def tensor_value(tensor):
    data = tensor["data"]
    shape = tensor["shape"]
    if not data:
        return None
    if tensor["type"] == FLOAT32:
        return np.frombuffer(data, "<f4").reshape(shape).astype(np.float32)
    if tensor["type"] == FLOAT16:
        return np.frombuffer(data, "<f2").reshape(shape).astype(np.float32)
    if tensor["type"] == INT32:
        return np.frombuffer(data, "<i4").reshape(shape)
    raise ValueError(f"tensor type {tensor['type']}")


def activate(x, code):
    if code == 1:
        return np.maximum(x, 0)
    if code == 3:
        return np.clip(x, 0, 6)
    return x


def same_pad(size, kernel, stride, dilation=1):
    effective = (kernel - 1) * dilation + 1
    out = -(-size // stride)
    total = max((out - 1) * stride + effective - size, 0)
    return out, total // 2, total - total // 2


def conv(x, w, b, stride, padding, dilation, depthwise):
    h, wd, c = x.shape
    if depthwise:
        kh, kw, oc = w.shape[1], w.shape[2], w.shape[3]
    else:
        oc, kh, kw = w.shape[0], w.shape[1], w.shape[2]
    if padding == 0:
        oh, pt, pb = same_pad(h, kh, stride[0], dilation[0])
        ow, pl, pr = same_pad(wd, kw, stride[1], dilation[1])
    else:
        oh = (h - ((kh - 1) * dilation[0] + 1)) // stride[0] + 1
        ow = (wd - ((kw - 1) * dilation[1] + 1)) // stride[1] + 1
        pt = pb = pl = pr = 0
    xp = np.pad(x, ((pt, pb + stride[0]), (pl, pr + stride[1]), (0, 0)))
    out = np.zeros((oh, ow, oc), np.float32) + b
    for ky in range(kh):
        for kx in range(kw):
            y0 = ky * dilation[0]
            x0 = kx * dilation[1]
            patch = xp[y0:y0 + oh * stride[0]:stride[0], x0:x0 + ow * stride[1]:stride[1]][:oh, :ow]
            if depthwise:
                out += patch * w[0, ky, kx]
            else:
                out += patch @ w[:, ky, kx, :].T
    return out


def transpose_conv(x, w, b, stride, padding):
    h, wd, _ = x.shape
    oc, kh, kw = w.shape[0], w.shape[1], w.shape[2]
    oh, ow = h * stride[0], wd * stride[1]
    pt = pl = 0
    if padding == 1:
        pt = max((h - 1) * stride[0] + kh - oh, 0) // 2
        pl = max((wd - 1) * stride[1] + kw - ow, 0) // 2
    full = np.zeros(((h - 1) * stride[0] + kh, (wd - 1) * stride[1] + kw, oc), np.float32)
    for ky in range(kh):
        for kx in range(kw):
            full[ky:ky + h * stride[0]:stride[0], kx:kx + wd * stride[1]:stride[1]] += x @ w[:, ky, kx, :].T
    return full[pt:pt + oh, pl:pl + ow] + b


def resize_bilinear(x, oh, ow, align, half):
    h, w, _ = x.shape

    def coords(o, i):
        if align and o > 1:
            return np.arange(o) * (i - 1) / (o - 1)
        scale = i / o
        return np.maximum((np.arange(o) + 0.5) * scale - 0.5, 0) if half else np.arange(o) * scale

    ys, xs = coords(oh, h), coords(ow, w)
    y0 = np.floor(ys).astype(int)
    x0 = np.floor(xs).astype(int)
    y1 = np.minimum(y0 + 1, h - 1)
    x1 = np.minimum(x0 + 1, w - 1)
    fy = (ys - y0)[:, None, None]
    fx = (xs - x0)[None, :, None]
    top = x[y0][:, x0] * (1 - fx) + x[y0][:, x1] * fx
    bottom = x[y1][:, x0] * (1 - fx) + x[y1][:, x1] * fx
    return top * (1 - fy) + bottom * fy


def run_tflite(path: Path, image: np.ndarray) -> np.ndarray:
    tensors, operators, inputs, outputs = load(path)
    values = {i: tensor_value(t) for i, t in enumerate(tensors)}
    values[inputs[0]] = image.astype(np.float32)
    for op in operators:
        name = op["name"]
        ins = [values.get(i) if i >= 0 else None for i in op["inputs"]]
        opt = op["options"]
        x = ins[0][0] if ins[0] is not None and ins[0].ndim == 4 else ins[0]
        if name == "DEQUANTIZE":
            result = ins[0].astype(np.float32)
        elif name in ("CONV_2D", "DEPTHWISE_CONV_2D"):
            depthwise = name == "DEPTHWISE_CONV_2D"
            stride = (opt.scalar(2, "i", 1), opt.scalar(1, "i", 1))
            dilation = (opt.scalar(6 if depthwise else 5, "i", 1), opt.scalar(5 if depthwise else 4, "i", 1))
            fused = opt.scalar(4 if depthwise else 3, "b")
            result = activate(conv(x, ins[1], ins[2], stride, opt.scalar(0, "b"), dilation, depthwise), fused)[None]
        elif name in ("ADD", "MUL"):
            a, b = ins
            result = activate(a + b if name == "ADD" else a * b, opt.scalar(0, "b") if opt else 0)
        elif name == "PRELU":
            alpha = ins[1].reshape((1,) * (ins[0].ndim - ins[1].ndim) + ins[1].shape)
            result = np.where(ins[0] >= 0, ins[0], ins[0] * alpha)
        elif name == "RELU":
            result = np.maximum(ins[0], 0)
        elif name == "HARD_SWISH":
            result = ins[0] * np.clip(ins[0] + 3, 0, 6) / 6
        elif name == "LOGISTIC":
            result = 1 / (1 + np.exp(-np.clip(ins[0], -60, 60)))
        elif name in ("MEAN", "AVERAGE_POOL_2D"):
            result = ins[0].mean(axis=(1, 2), keepdims=True)
        elif name == "CONCATENATION":
            result = np.concatenate([v for v in ins], axis=opt.scalar(0, "i"))
        elif name == "RESIZE_BILINEAR":
            oh, ow = [int(v) for v in ins[1]]
            result = resize_bilinear(x, oh, ow, bool(opt.scalar(2, "b")), bool(opt.scalar(3, "b")))[None]
        elif name == "Convolution2DTransposeBias":
            padding, stride_w, stride_h = struct.unpack_from("<iii", op["custom"], 0)
            result = transpose_conv(x, ins[1], ins[2], (stride_h, stride_w), padding)[None]
        elif name == "MaxPoolingWithArgmax2D":
            h, w, c = x.shape
            blocks = x[: h // 2 * 2, : w // 2 * 2].reshape(h // 2, 2, w // 2, 2, c).transpose(0, 2, 1, 3, 4).reshape(h // 2, w // 2, 4, c)
            index = blocks.argmax(axis=2)
            values[op["outputs"][0]] = blocks.max(axis=2)[None]
            values[op["outputs"][1]] = index.astype(np.float32)[None]
            continue
        elif name == "MaxUnpooling2D":
            pooled, index = x, ins[1][0].astype(int)
            h, w, c = pooled.shape
            out = np.zeros((h, w, 4, c), np.float32)
            np.put_along_axis(out, index[:, :, None, :], pooled[:, :, None, :], axis=2)
            result = out.reshape(h, w, 2, 2, c).transpose(0, 2, 1, 3, 4).reshape(h * 2, w * 2, c)[None]
        else:
            raise ValueError(f"unsupported operator {name}")
        values[op["outputs"][0]] = result
    return values[outputs[0]][0]


def box(x, r):
    pad = np.pad(x, ((r + 1, r), (r + 1, r)) + ((0, 0),) * (x.ndim - 2), mode="edge")
    c = pad.cumsum(0).cumsum(1)
    k = 2 * r + 1
    return (c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]) / (k * k)


def guided_filter(guide, p, r, eps):
    mean_i = box(guide, r)
    mean_p = box(p, r)
    h, w, _ = guide.shape
    cov = box(guide * p[..., None], r) - mean_i * mean_p[..., None]
    var = np.zeros((h, w, 3, 3), np.float32)
    for i in range(3):
        for j in range(3):
            var[..., i, j] = box(guide[..., i] * guide[..., j], r) - mean_i[..., i] * mean_i[..., j]
    var += eps * np.eye(3, dtype=np.float32)
    a = np.linalg.solve(var, cov[..., None])[..., 0]
    b = mean_p - (a * mean_i).sum(-1)
    return (box(a, r) * guide).sum(-1) + box(b, r)


def push_pull(color, weight, levels=9):
    pyramid = [(color * weight[..., None], weight)]
    for _ in range(levels):
        c, w = pyramid[-1]
        h, wd = w.shape
        if h < 2 or wd < 2:
            break
        c = c[: h // 2 * 2, : wd // 2 * 2].reshape(h // 2, 2, wd // 2, 2, 3).sum((1, 3))
        w = w[: h // 2 * 2, : wd // 2 * 2].reshape(h // 2, 2, wd // 2, 2).sum((1, 3))
        pyramid.append((c, w))
    c, w = pyramid[-1]
    filled = c / np.maximum(w, 1e-6)[..., None]
    for c, w in reversed(pyramid[:-1]):
        up = np.repeat(np.repeat(filled, 2, 0), 2, 1)
        up = np.pad(up, ((0, c.shape[0] - up.shape[0]), (0, c.shape[1] - up.shape[1]), (0, 0)), mode="edge")
        own = c / np.maximum(w, 1e-6)[..., None]
        trust = np.clip(w, 0, 1)[..., None]
        filled = own * trust + up * (1 - trust)
    return filled


def decontaminate(image, alpha):
    background = push_pull(image, (alpha < 0.02).astype(np.float32))
    a = np.maximum(alpha, 1e-3)[..., None]
    foreground = np.clip((image - (1 - a) * background) / a, 0, 1)
    solid = (alpha > 0.97).astype(np.float32)
    confident = np.clip((alpha - 0.15) / 0.5, 0, 1)
    estimate = push_pull(foreground, np.maximum(solid, confident ** 2))
    mix = np.clip((alpha - 0.3) / 0.6, 0, 1)[..., None]
    out = foreground * mix + estimate * (1 - mix)
    return np.where(alpha[..., None] > 0.995, image, out)


RING = 28


def head_ring(matte, mid, span):
    size = matte.shape[0]
    center = mid + np.array([0.0, -0.35 * span])
    reaches = []
    directions = []
    for k in range(RING):
        angle = 2 * np.pi * k / RING - np.pi / 2
        direction = np.array([np.cos(angle), np.sin(angle)])
        reach = 0.0
        r = 0.0
        while True:
            x, y = center + direction * r
            if not (0 <= x < size - 1 and 0 <= y < size - 1):
                break
            if matte[int(y), int(x)] > 0.03:
                reach = r
            r += 1.0
        reaches.append(reach)
        directions.append(direction)
    reaches = np.array(reaches)
    widened = np.max([np.roll(reaches, k) for k in (-1, 0, 1)], axis=0)
    smooth = np.mean([np.roll(widened, k) for k in (-2, -1, 0, 1, 2)], axis=0)
    radius = np.maximum(smooth, reaches) + 0.18 * span
    out = []
    for direction, r in zip(directions, radius):
        x, y = center + direction * r
        out += [round(float(np.clip(x, 0, size)), 1), round(float(np.clip(y, 0, size)), 1)]
    return out


def chroma(image):
    total = image.sum(-1, keepdims=True) + 0.03
    return (image / total)[..., :2]


def skin_likeness(image, p, span):
    samples = []
    radius = int(span * 0.12)
    for a, b in ((2, 31), (14, 35), (3, 48), (13, 54)):
        cx, cy = (p[a] + p[b]) / 2
        y0, y1 = int(cy - radius), int(cy + radius)
        x0, x1 = int(cx - radius), int(cx + radius)
        samples.append(image[y0:y1, x0:x1].reshape(-1, 3))
    data = chroma(np.concatenate(samples)[None])[0]
    mean = data.mean(0)
    cov = np.cov(data.T) + np.eye(2) * 2e-5
    inverse = np.linalg.inv(cov)
    d = chroma(image) - mean
    distance = np.einsum("...i,ij,...j->...", d, inverse, d)
    dark = smoothstep(0.03, 0.12, image.mean(-1))
    return np.exp(-0.5 * distance / 9.0) * dark


def fit_ellipse(points):
    x = points[:, 0]
    y = points[:, 1]
    design = np.stack([x * x, y * y, x, y], 1)
    a, c, d, e = np.linalg.lstsq(design, np.ones(len(x)), rcond=None)[0]
    cx = -d / (2 * a)
    cy = -e / (2 * c)
    k = 1 + a * cx * cx + c * cy * cy
    return cx, cy, np.sqrt(k / a), np.sqrt(k / c)


def complete_top(color, aux, eye_y):
    matte = aux[..., 0]
    size = matte.shape[0]
    points = []
    for x in range(4, size - 4, 4):
        rows = np.nonzero(matte[:, x] > 0.5)[0]
        if len(rows) and 6 < rows[0] < eye_y:
            points.append((x, rows[0]))
    for y in range(6, int(eye_y), 4):
        cols = np.nonzero(matte[y] > 0.5)[0]
        if len(cols):
            if cols[0] > 4:
                points.append((cols[0], y))
            if cols[-1] < size - 5:
                points.append((cols[-1], y))
    cx, cy, rx, ry = fit_ellipse(np.array(points, np.float64))
    canvas = size + PAD
    side = PAD // 2
    out_color = np.zeros((canvas, canvas, 3), np.float32)
    out_aux = np.zeros((canvas, canvas, 3), np.float32)
    out_color[PAD:, side:side + size] = color
    out_aux[PAD:, side:side + size] = aux
    mirror = color[PAD - 1::-1]
    out_color[:PAD, side:side + size] = mirror
    yy, xx = np.mgrid[-PAD:0, 0:size].astype(np.float32)
    inside = np.hypot((xx - cx) / rx, (yy - cy) / ry)
    feather = 10.0 / min(rx, ry)
    edge = 1 - smoothstep(1 - feather, 1 + feather, inside)
    column = box(matte[:3].mean(0, keepdims=True).repeat(3, 0), 2)[1]
    top = column[None, :] * edge
    out_aux[:PAD, side:side + size, 0] = top
    out_aux[:PAD, side:side + size, 2] = top
    out_color[:, :side] = out_color[:, side:side + 1]
    out_color[:, side + size:] = out_color[:, side + size - 1:side + size]
    return out_color, out_aux


def upscale(mask, size):
    return np.asarray(Image.fromarray(mask.astype(np.float32), "F").resize((size, size), Image.BICUBIC), np.float32)


def polygon_mask(points, size, blur=0):
    image = Image.new("L", (size, size), 0)
    ImageDraw.Draw(image).polygon([tuple(map(float, p)) for p in points], fill=255)
    mask = np.asarray(image, np.float32) / 255
    return box(mask, blur) if blur else mask


def smoothstep(edge0, edge1, x):
    t = np.clip((x - edge0) / (edge1 - edge0), 0, 1)
    return t * t * (3 - 2 * t)


def build_face(name, original: Path, offset: int, points, selfie: Path, hair: Path, out_dir: Path, debug: Path | None):
    full = Image.open(original).convert("RGB").crop((offset, 0, offset + 1024, 1024))
    image = np.asarray(full, np.float32) / 255
    person = run_tflite(selfie, np.asarray(full.resize((256, 256), Image.BILINEAR), np.float32)[None] / 255)[..., 0]
    hair_input = np.concatenate([np.asarray(full.resize((512, 512), Image.BILINEAR), np.float32) / 255, np.zeros((512, 512, 1), np.float32)], -1)
    logits = run_tflite(hair, hair_input[None])
    hair_prob = 1 / (1 + np.exp(np.clip(logits[..., 0] - logits[..., 1], -60, 60)))
    person = upscale(person, 1024)
    hair_prob = upscale(hair_prob, 1024)
    pts = [((x * OLD_CROP[2]) + OLD_CROP[0], (y * OLD_CROP[2]) + OLD_CROP[1]) for x, y in points]
    p = np.array(pts)
    eyes_l = p[36:42].mean(0)
    eyes_r = p[42:48].mean(0)
    span = np.linalg.norm(eyes_r - eyes_l)
    direction = (eyes_r - eyes_l) / span
    up = np.array([direction[1], -direction[0]])
    jaw = p[0:17]
    brows = p[17:27]
    forehead = [tuple(b + up * span * 1.2) for b in brows[::-1]]
    face_poly = [tuple(v) for v in jaw] + forehead
    face = polygon_mask(face_poly, 1024, 6)
    chin = p[8]
    neck_half = np.linalg.norm(p[4] - p[12]) * 0.5
    neck_poly = [tuple(p[4]), tuple(p[12]), (p[12][0] + neck_half * 0.08, chin[1] + span * 1.1), (p[4][0] - neck_half * 0.08, chin[1] + span * 1.1)]
    neck = polygon_mask(neck_poly, 1024, 10)
    yy = np.arange(1024, dtype=np.float32)[:, None] * np.ones((1, 1024), np.float32)
    neck *= 1 - smoothstep(chin[1] + span * 0.25, chin[1] + span * 0.8, yy)
    head_zone = np.maximum(face, neck)
    xx = np.arange(1024, dtype=np.float32)[None, :] * np.ones((1024, 1), np.float32)
    center = (eyes_l + eyes_r) / 2
    reach = np.hypot((xx - center[0]) / (span * 1.75), (yy - center[1] + span * 0.4) / (span * 2.3))
    near = 1 - smoothstep(0.85, 1.05, reach)
    above = (1 - smoothstep(p[4][1], p[5][1] + span * 0.25, yy)) * near
    side = np.maximum(above, head_zone)
    hair_reach = np.hypot((xx - center[0]) / (span * 1.9), (yy - center[1] - span * 0.6) / (span * 3.4))
    hair_keep = smoothstep(0.35, 0.75, hair_prob) * (1 - smoothstep(1024 - span * 0.9, 1024 - span * 0.15, yy))
    hair_keep *= 1 - smoothstep(0.85, 1.05, hair_reach)
    region = np.maximum(side, hair_keep)
    coarse = np.clip(person * region, 0, 1)
    matte = np.clip(guided_filter(image, coarse, 6, 1e-3), 0, 1)
    matte = np.clip((matte - 0.04) / 0.92, 0, 1)
    matte = np.maximum(matte, smoothstep(0.9, 1.0, -box(-coarse, 6)) * coarse)
    interior = smoothstep(0.92, 1.0, box(coarse, 10))
    matte = np.maximum(matte, interior)
    likeness = skin_likeness(image, p, span)
    below = smoothstep(chin[1] - span * 0.1, chin[1] + span * 0.2, yy)
    keep_neck = np.maximum(smoothstep(0.08, 0.35, box(likeness, 4)), hair_keep)
    matte = matte * (1 - below + below * keep_neck)
    hair_soft = np.clip(guided_filter(image, smoothstep(0.3, 0.7, hair_prob) * matte, 6, 4e-4), 0, 1)
    skin = np.clip(matte - hair_soft, 0, 1) * np.maximum(box(face, 12), neck)
    skin = np.clip(box(skin, 3), 0, 1)
    color = decontaminate(image, matte)
    aux = np.stack([matte, skin, hair_soft], -1)
    color, aux = complete_top(color, aux, eyes_l[1])
    canvas = color.shape[0]
    photo = Image.fromarray((np.clip(color, 0, 1) * 255 + 0.5).astype(np.uint8)).resize((SIZE, SIZE), Image.LANCZOS)
    aux_image = Image.fromarray((np.clip(aux, 0, 1) * 255 + 0.5).astype(np.uint8), "RGB").resize((SIZE, SIZE), Image.LANCZOS)
    photo.save(out_dir / f"{name}.jpg", quality=88, optimize=True, progressive=True)
    aux_image.save(out_dir / f"{name}-matte.png", optimize=True)
    if debug:
        debug.mkdir(parents=True, exist_ok=True)
        rgb = np.asarray(photo, np.float32) / 255
        a = np.asarray(aux_image, np.float32)[..., :1] / 255
        checker = ((np.indices((SIZE, SIZE)).sum(0) // 16) % 2)[..., None] * 0.3 + 0.5
        Image.fromarray(((rgb * a + checker * (1 - a)) * 255).astype(np.uint8)).save(debug / f"{name}-cutout.png")
        Image.fromarray((upscale(hair_prob, SIZE) * 255).clip(0, 255).astype(np.uint8)).save(debug / f"{name}-hair.png")
        Image.fromarray((upscale(person, SIZE) * 255).clip(0, 255).astype(np.uint8)).save(debug / f"{name}-person.png")
    scale = SIZE / canvas
    landmarks = [round(v * scale, 1) for x, y in pts for v in (x + PAD / 2, y + PAD)]
    small = np.asarray(aux_image, np.float32)[..., 0] / 255
    mid = ((eyes_l + eyes_r) / 2 + np.array([PAD / 2, PAD])) * scale
    return {"points": landmarks, "head": head_ring(small, mid, span * scale)}


def main(argv: list[str]) -> int:
    if len(argv) < 4:
        print("usage: build_call_face_assets.py ORIGINALS_DIR selfie_segmentation.tflite hair_segmentation.tflite OLD_POINTS.json [DEBUG_DIR]")
        return 2
    originals, selfie, hair, old_points = Path(argv[0]), Path(argv[1]), Path(argv[2]), Path(argv[3])
    debug = Path(argv[4]) if len(argv) > 4 else None
    points = json.loads(old_points.read_text())
    result = {}
    for name, (file, offset) in SOURCES.items():
        flat = points[name]
        pairs = [(flat[i], flat[i + 1]) for i in range(0, len(flat), 2)]
        result[name] = build_face(name, originals / file, offset, pairs, selfie, hair, FACES, debug)
        print(name, "done")
    print(json.dumps(result))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
