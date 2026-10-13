#!/usr/bin/env python3

from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[2]
OUTPUT = REPOSITORY / "app" / "public" / "assets" / "call-segmentation" / "selfie-landscape.fmnn"
MAGIC = b"FMNN"
VERSION = 1

BUILTIN = {
    0: "ADD", 1: "AVERAGE_POOL_2D", 2: "CONCATENATION", 3: "CONV_2D", 4: "DEPTHWISE_CONV_2D", 6: "DEQUANTIZE", 14: "LOGISTIC",
    18: "MUL", 19: "RELU", 23: "RESIZE_BILINEAR", 32: "CUSTOM", 40: "MEAN", 54: "PRELU", 117: "HARD_SWISH",
}
ACTIVATIONS = {"RELU": "relu", "HARD_SWISH": "hswish", "LOGISTIC": "sigmoid"}
FUSED = {0: "none", 1: "relu"}
FLOAT32 = 0
FLOAT16 = 1
INT32 = 2


class Table:
    def __init__(self, buf: bytes, pos: int):
        self.buf = buf
        self.pos = pos
        self.vtable = pos - struct.unpack_from("<i", buf, pos)[0]
        self.vsize = struct.unpack_from("<H", buf, self.vtable)[0]

    def offset(self, slot: int) -> int:
        at = 4 + 2 * slot
        if at >= self.vsize:
            return 0
        return struct.unpack_from("<H", self.buf, self.vtable + at)[0]

    def scalar(self, slot: int, fmt: str, default=0):
        off = self.offset(slot)
        return struct.unpack_from("<" + fmt, self.buf, self.pos + off)[0] if off else default

    def indirect(self, slot: int) -> int | None:
        off = self.offset(slot)
        if not off:
            return None
        at = self.pos + off
        return at + struct.unpack_from("<I", self.buf, at)[0]

    def table(self, slot: int) -> Table | None:
        at = self.indirect(slot)
        return Table(self.buf, at) if at is not None else None

    def vector(self, slot: int) -> tuple[int, int]:
        at = self.indirect(slot)
        if at is None:
            return 0, 0
        return at + 4, struct.unpack_from("<I", self.buf, at)[0]

    def scalars(self, slot: int, fmt: str) -> list:
        start, count = self.vector(slot)
        size = struct.calcsize("<" + fmt)
        return [struct.unpack_from("<" + fmt, self.buf, start + i * size)[0] for i in range(count)]

    def tables(self, slot: int) -> list[Table]:
        start, count = self.vector(slot)
        out = []
        for i in range(count):
            at = start + 4 * i
            out.append(Table(self.buf, at + struct.unpack_from("<I", self.buf, at)[0]))
        return out

    def bytes(self, slot: int) -> bytes:
        start, count = self.vector(slot)
        return self.buf[start:start + count]


def load(path: Path):
    buf = path.read_bytes()
    model = Table(buf, struct.unpack_from("<I", buf, 0)[0])
    codes = []
    for code in model.tables(1):
        builtin = max(code.scalar(0, "b"), code.scalar(3, "i"))
        name = BUILTIN.get(builtin, f"OP{builtin}")
        custom = code.bytes(1).decode() if name == "CUSTOM" else ""
        codes.append(custom or name)
    buffers = [b.bytes(0) for b in model.tables(4)]
    graph = model.tables(2)[0]
    tensors = []
    for tensor in graph.tables(0):
        tensors.append({
            "shape": tensor.scalars(0, "i"),
            "type": tensor.scalar(1, "b"),
            "data": buffers[tensor.scalar(2, "I")],
        })
    operators = []
    for op in graph.tables(3):
        operators.append({
            "name": codes[op.scalar(0, "I")],
            "inputs": op.scalars(1, "i"),
            "outputs": op.scalars(2, "i"),
            "options": op.table(4),
            "custom": op.bytes(5),
        })
    return tensors, operators, graph.scalars(1, "i"), graph.scalars(2, "i")


def same_padding(size: int, kernel: int, stride: int) -> int:
    out = -(-size // stride)
    return max((out - 1) * stride + kernel - size, 0) // 2


def convert(path: Path) -> bytes:
    tensors, operators, inputs, outputs = load(path)
    constants: dict[int, tuple[int, list[int]]] = {}
    blob = bytearray()

    def store(index: int) -> list[int]:
        if index in constants:
            return [constants[index][0], len(constants[index][1])]
        tensor = tensors[index]
        data = tensor["data"]
        if tensor["type"] == FLOAT16:
            raw = data
        elif tensor["type"] == FLOAT32:
            values = struct.unpack(f"<{len(data) // 4}f", data)
            raw = struct.pack(f"<{len(values)}e", *values)
        else:
            raise ValueError(f"tensor {index} has type {tensor['type']}")
        offset = len(blob) // 2
        blob.extend(raw)
        constants[index] = (offset, list(range(len(raw) // 2)))
        return [offset, len(raw) // 2]

    alias = {}
    for op in operators:
        if op["name"] == "DEQUANTIZE":
            alias[op["outputs"][0]] = op["inputs"][0]
    resolve = lambda index: alias.get(index, index)
    consumers: dict[int, list[dict]] = {}
    for op in operators:
        if op["name"] == "DEQUANTIZE":
            continue
        for index in op["inputs"]:
            consumers.setdefault(resolve(index), []).append(op)

    graph = []
    fused_away: dict[int, int] = {}
    produced: dict[int, dict] = {}
    for op in operators:
        name = op["name"]
        if name == "DEQUANTIZE":
            continue
        ins = [fused_away.get(resolve(i), resolve(i)) for i in op["inputs"]]
        out = op["outputs"][0]
        if name in ACTIVATIONS:
            source = produced.get(ins[0])
            if source and source.get("act") == "none" and len(consumers.get(ins[0], [])) == 1:
                source["act"] = ACTIVATIONS[name]
                source["out"] = out
                produced[out] = source
                fused_away[ins[0]] = out
                continue
            node = {"op": "act", "act": ACTIVATIONS[name], "in": [ins[0]], "out": out}
        elif name in ("CONV_2D", "DEPTHWISE_CONV_2D"):
            options = op["options"]
            shape = tensors[ins[0]]["shape"]
            weights = tensors[ins[1]]["shape"]
            depthwise = name == "DEPTHWISE_CONV_2D"
            padding = options.scalar(0, "b")
            stride = [options.scalar(2, "i"), options.scalar(1, "i")]
            fused = options.scalar(4 if depthwise else 3, "b")
            kh, kw = weights[1], weights[2]
            pad = [0, 0]
            if padding == 0:
                pad = [same_padding(shape[1], kh, stride[0]), same_padding(shape[2], kw, stride[1])]
            node = {
                "op": "dw" if depthwise else "conv",
                "in": [ins[0]],
                "out": out,
                "kernel": [kh, kw],
                "stride": stride,
                "pad": pad,
                "act": FUSED[fused],
                "w": store(ins[1]),
                "b": store(ins[2]),
                "cin": shape[3],
                "cout": weights[3] if depthwise else weights[0],
            }
        elif name in ("ADD", "MUL"):
            consts = [i for i in ins if tensors[i]["data"]]
            if consts:
                raise ValueError(f"{name} with a constant operand is not supported")
            fused = op["options"].scalar(0, "b") if op["options"] else 0
            node = {"op": name.lower(), "in": ins, "out": out, "act": FUSED[fused]}
        elif name == "MEAN":
            axes = struct.unpack(f"<{len(tensors[ins[1]]['data']) // 4}i", tensors[ins[1]]["data"])
            if sorted(axes) != [1, 2]:
                raise ValueError("MEAN must reduce height and width")
            node = {"op": "mean", "in": [ins[0]], "out": out, "act": "none"}
        elif name == "AVERAGE_POOL_2D":
            options = op["options"]
            shape = tensors[ins[0]]["shape"]
            if [options.scalar(4, "i"), options.scalar(3, "i")] != shape[1:3] or tensors[out]["shape"][1:3] != [1, 1]:
                raise ValueError("AVERAGE_POOL_2D must pool the whole image")
            node = {"op": "mean", "in": [ins[0]], "out": out, "act": FUSED[options.scalar(5, "b")]}
        elif name == "RESIZE_BILINEAR":
            options = op["options"]
            node = {
                "op": "resize",
                "in": [ins[0]],
                "out": out,
                "align": bool(options.scalar(2, "b")) if options else False,
                "half": bool(options.scalar(3, "b")) if options else False,
                "act": "none",
            }
        elif name == "Convolution2DTransposeBias":
            custom = op["custom"]
            padding, stride_w, stride_h = struct.unpack_from("<iii", custom, 0)
            weights = tensors[ins[1]]["shape"]
            shape = tensors[ins[0]]["shape"]
            result = tensors[out]["shape"]
            pad = [0, 0]
            if padding == 1:
                pad = [
                    max((shape[1] - 1) * stride_h + weights[1] - result[1], 0) // 2,
                    max((shape[2] - 1) * stride_w + weights[2] - result[2], 0) // 2,
                ]
            node = {
                "op": "tconv",
                "in": [ins[0]],
                "out": out,
                "kernel": [weights[1], weights[2]],
                "stride": [stride_h, stride_w],
                "pad": pad,
                "act": "none",
                "w": store(ins[1]),
                "b": store(ins[2]),
                "cin": weights[3],
                "cout": weights[0],
            }
        else:
            raise ValueError(f"unsupported operator {name}")
        graph.append(node)
        produced[out] = node

    used = {inputs[0]}
    for node in graph:
        used.add(node["out"])
    numbering = {index: i for i, index in enumerate(sorted(used))}
    shapes = {}
    for index in used:
        shape = tensors[index]["shape"]
        shapes[numbering[index]] = [shape[1], shape[2], shape[3]]
    for node in graph:
        node["in"] = [numbering[i] for i in node["in"]]
        node["out"] = numbering[node["out"]]
    header = {
        "version": VERSION,
        "input": numbering[inputs[0]],
        "output": numbering[resolve(outputs[0])],
        "shapes": {str(k): v for k, v in sorted(shapes.items())},
        "ops": graph,
    }
    text = json.dumps(header, separators=(",", ":")).encode()
    text += b" " * (-len(text) % 4)
    return MAGIC + struct.pack("<II", VERSION, len(text)) + text + bytes(blob)


def main(argv: list[str]) -> int:
    if not argv:
        print("usage: build_call_segmentation_model.py selfie_segmentation_landscape.tflite [output]")
        return 2
    output = Path(argv[1]) if len(argv) > 1 else OUTPUT
    data = convert(Path(argv[0]))
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(data)
    print(f"wrote {output} ({len(data)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
