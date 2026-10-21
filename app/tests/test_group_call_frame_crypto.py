import json
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
WORKER = ROOT / "app" / "public" / "chat-group-call-crypto.js"
NATIVE = ROOT / "native" / "webrtc" / "fm"
KEY = bytes(range(16))

HARNESS = r"""
const fs = require("fs");
const vm = require("vm");
const [workerPath, requestText] = process.argv.slice(1);
const request = JSON.parse(requestText);
const context = {
  crypto: globalThis.crypto, TransformStream, ReadableStream, WritableStream,
  Uint8Array, DataView, BigInt, Map, Math, Number, Promise, console,
};
context.self = context;
vm.createContext(context);
vm.runInContext(fs.readFileSync(workerPath, "utf8"), context);
const hex = (bytes) => Buffer.from(bytes).toString("hex");
const raw = (text) => Uint8Array.from(Buffer.from(text, "hex")).buffer;
(async () => {
  if (request.op === "encrypt") {
    await context.onmessage({data: {op: "send-key", raw: raw(request.key), keyId: request.keyId}});
  } else {
    await context.onmessage({data: {op: "key", peer: "p", raw: raw(request.key), keyId: request.keyId}});
  }
  const out = [];
  const readable = new ReadableStream({
    start(controller) {
      for (const frame of request.frames) controller.enqueue({data: raw(frame.data), type: frame.type});
      controller.close();
    },
  });
  let done;
  const finished = new Promise((resolve) => { done = resolve; });
  const writable = new WritableStream({
    write(frame) { out.push(hex(new Uint8Array(frame.data))); },
    close() { done(); },
  });
  context.onmessage({data: {op: "stream", direction: request.op, kind: request.kind, peer: "p", readable, writable}});
  await finished;
  process.stdout.write(JSON.stringify(out));
})();
"""


def _needs(*tools):
    for tool in tools:
        if shutil.which(tool) is None:
            pytest.skip(tool + " is not installed")


@pytest.fixture(scope="module")
def native_tool(tmp_path_factory):
    _needs("g++", "node")
    if not Path("/usr/include/openssl/evp.h").exists():
        pytest.skip("OpenSSL headers are not installed")
    binary = tmp_path_factory.mktemp("fmcrypto") / "fmcrypto"
    subprocess.run(
        ["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O1", "-o", str(binary),
         "-I", str(NATIVE.parent), str(NATIVE / "fm_frame_crypto.cc"), str(NATIVE / "fm_frame_crypto_tool.cc"),
         "-lcrypto"],
        check=True, capture_output=True, text=True)
    return binary


def _worker(op, kind, frames, key_id=3):
    request = json.dumps({"op": op, "kind": kind, "keyId": key_id, "key": KEY.hex(), "frames": frames})
    result = subprocess.run(
        ["node", "-e", HARNESS, str(WORKER), request],
        check=True, capture_output=True, text=True, timeout=60)
    return json.loads(result.stdout)


def _native(binary, *args):
    return subprocess.run(
        [str(binary), *args], capture_output=True, text=True, timeout=30).stdout.strip()


FRAMES = [
    ("video", "key", bytes([0x10, 0x02, 0x00, 0x9d, 0x01, 0x2a, 0x40, 0x01, 0xf0, 0x00]) + bytes(range(200))),
    ("video", "delta", bytes([0x31, 0x55, 0x00]) + bytes(range(90))),
    ("audio", None, bytes([0xfc]) + bytes(range(60))),
    ("video", "delta", bytes([0x01, 0x02])),
]


@pytest.mark.parametrize("kind,frame_type,frame", FRAMES)
def test_a_browser_frame_decrypts_on_the_phone(native_tool, kind, frame_type, frame):
    sealed = _worker("encrypt", kind, [{"data": frame.hex(), "type": frame_type}])
    assert len(sealed) == 1
    encrypted = bytes.fromhex(sealed[0])
    clear = 1 if kind == "audio" else (10 if frame_type == "key" else 3)
    clear = min(clear, len(frame))
    assert encrypted[:clear] == frame[:clear]
    assert frame[clear:] not in encrypted or len(frame) == clear
    assert encrypted[-1] == 3
    assert _native(native_tool, "decrypt", kind, KEY.hex(), sealed[0]) == frame.hex()


@pytest.mark.parametrize("kind,frame_type,frame", FRAMES)
def test_a_phone_frame_decrypts_in_the_browser(native_tool, kind, frame_type, frame):
    sealed = _native(native_tool, "encrypt", kind, KEY.hex(), "9", "41", frame.hex())
    opened = _worker("decrypt", kind, [{"data": sealed, "type": frame_type}], key_id=9)
    assert opened == [frame.hex()]


def test_a_tampered_or_unkeyed_frame_is_dropped(native_tool):
    frame = bytes([0x31, 0x55, 0x00]) + bytes(range(40))
    sealed = bytearray.fromhex(_native(native_tool, "encrypt", "video", KEY.hex(), "9", "5", frame.hex()))
    tampered = bytes(sealed[:20]) + bytes([sealed[20] ^ 1]) + bytes(sealed[21:])
    assert _native(native_tool, "decrypt", "video", KEY.hex(), tampered.hex()) == "fail"
    header = bytes([sealed[0] ^ 2]) + bytes(sealed[1:])
    assert _native(native_tool, "decrypt", "video", KEY.hex(), header.hex()) == "fail"
    assert _worker("decrypt", "video", [{"data": tampered.hex(), "type": "delta"}], key_id=9) == []
    assert _worker("decrypt", "video", [{"data": bytes(sealed).hex(), "type": "delta"}], key_id=8) == []
