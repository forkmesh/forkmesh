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
async function run(step) {
  const out = [];
  const readable = new ReadableStream({
    start(controller) {
      for (const frame of step.frames) controller.enqueue({data: raw(frame.data), type: frame.type});
      controller.close();
    },
  });
  let done;
  const finished = new Promise((resolve) => { done = resolve; });
  const writable = new WritableStream({
    write(frame) { out.push(hex(new Uint8Array(frame.data))); },
    close() { done(); },
  });
  context.onmessage({data: {op: "stream", direction: step.op, kind: step.kind, peer: "p", readable, writable}});
  await finished;
  return out;
}
(async () => {
  const results = [];
  for (const step of request.steps) {
    if (step.op === "send-key" || step.op === "key") {
      await context.onmessage({data: {op: step.op, peer: "p", raw: raw(step.key), keyId: step.keyId}});
    } else {
      results.push(await run(step));
    }
  }
  process.stdout.write(JSON.stringify(results));
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
        ["g++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-O1", "-o", str(binary),
         "-I", str(NATIVE.parent), str(NATIVE / "fm_frame_crypto.cc"), str(NATIVE / "fm_frame_crypto_tool.cc"),
         "-lcrypto"],
        check=True, capture_output=True, text=True)
    return binary


def _worker_steps(steps):
    request = json.dumps({"steps": steps})
    result = subprocess.run(
        ["node", "-e", HARNESS, str(WORKER), request],
        check=True, capture_output=True, text=True, timeout=60)
    return json.loads(result.stdout)


def _worker(op, kind, frames, key_id=3):
    setup = {"op": "send-key" if op == "encrypt" else "key", "key": KEY.hex(), "keyId": key_id}
    return _worker_steps([setup, {"op": op, "kind": kind, "frames": frames}])[0]


def _native(binary, *args):
    return subprocess.run(
        [str(binary), *args], capture_output=True, text=True, timeout=30).stdout.strip()


def _native_lines(binary, *args):
    return _native(binary, *args).splitlines()


def _counter(sealed_hex):
    return int.from_bytes(bytes.fromhex(sealed_hex)[-9:-1], "big")


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


OTHER_KEY = bytes(range(100, 116))
DELTA = bytes([0x31, 0x55, 0x00]) + bytes(range(40))
VOICE = bytes([0xfc]) + bytes(range(30))


def _delta(sealed):
    return {"op": "decrypt", "kind": "video", "frames": [{"data": sealed, "type": "delta"}]}


def _phone_frames(binary, count, start=1):
    return [_native(binary, "encrypt", "video", KEY.hex(), "9", str(start + i), DELTA.hex()) for i in range(count)]


def test_a_replayed_frame_is_dropped_by_the_worker_and_the_phone(native_tool):
    first, second = _phone_frames(native_tool, 2)
    worker = _worker_steps([
        {"op": "key", "key": KEY.hex(), "keyId": 9},
        _delta(first), _delta(second), _delta(first), _delta(second),
    ])
    assert worker == [[DELTA.hex()], [DELTA.hex()], [], []]
    phone = _native_lines(native_tool, "receive", f"key:{KEY.hex()}:9",
                          f"video:{first}", f"video:{second}", f"video:{first}", f"video:{second}")
    assert phone == [DELTA.hex(), DELTA.hex(), "fail", "fail"]


def test_reordered_frames_inside_the_window_are_accepted_and_stale_ones_are_not(native_tool):
    early = _native(native_tool, "encrypt", "video", KEY.hex(), "9", "3", DELTA.hex())
    late = _native(native_tool, "encrypt", "video", KEY.hex(), "9", "1000", DELTA.hex())
    middle = _native(native_tool, "encrypt", "video", KEY.hex(), "9", "500", DELTA.hex())
    newest = _native(native_tool, "encrypt", "video", KEY.hex(), "9", "1030", DELTA.hex())
    stale = _native(native_tool, "encrypt", "video", KEY.hex(), "9", "4", DELTA.hex())
    order = [early, late, middle, newest, stale, middle]
    expected = [[DELTA.hex()], [DELTA.hex()], [DELTA.hex()], [DELTA.hex()], [], []]
    worker = _worker_steps([{"op": "key", "key": KEY.hex(), "keyId": 9}, *[_delta(item) for item in order]])
    assert worker == expected
    phone = _native_lines(native_tool, "receive", f"key:{KEY.hex()}:9", *[f"video:{item}" for item in order])
    assert phone == [item[0] if item else "fail" for item in expected]


def test_a_tampered_replay_does_not_spend_the_counter(native_tool):
    sealed = _native(native_tool, "encrypt", "video", KEY.hex(), "9", "7", DELTA.hex())
    data = bytearray.fromhex(sealed)
    data[20] ^= 1
    worker = _worker_steps([{"op": "key", "key": KEY.hex(), "keyId": 9}, _delta(bytes(data).hex()), _delta(sealed)])
    assert worker == [[], [DELTA.hex()]]
    phone = _native_lines(native_tool, "receive", f"key:{KEY.hex()}:9", f"video:{bytes(data).hex()}", f"video:{sealed}")
    assert phone == ["fail", DELTA.hex()]


def test_resending_the_same_peer_key_keeps_its_replay_window(native_tool):
    first = _phone_frames(native_tool, 1)[0]
    worker = _worker_steps([
        {"op": "key", "key": KEY.hex(), "keyId": 9}, _delta(first),
        {"op": "key", "key": KEY.hex(), "keyId": 9}, _delta(first),
    ])
    assert worker == [[DELTA.hex()], []]
    phone = _native_lines(native_tool, "receive", f"key:{KEY.hex()}:9", f"video:{first}",
                          f"key:{KEY.hex()}:9", f"video:{first}")
    assert phone == [DELTA.hex(), "fail"]


def test_reinstalling_the_same_send_key_keeps_the_counter_running(native_tool):
    worker = _worker_steps([
        {"op": "send-key", "key": KEY.hex(), "keyId": 1},
        {"op": "encrypt", "kind": "video", "frames": [{"data": DELTA.hex(), "type": "delta"}] * 2},
        {"op": "send-key", "key": KEY.hex(), "keyId": 1},
        {"op": "encrypt", "kind": "video", "frames": [{"data": DELTA.hex(), "type": "delta"}]},
        {"op": "send-key", "key": OTHER_KEY.hex(), "keyId": 2},
        {"op": "encrypt", "kind": "video", "frames": [{"data": DELTA.hex(), "type": "delta"}]},
    ])
    assert [_counter(item) for step in worker for item in step] == [1, 2, 3, 1]
    assert worker[2][0][-2:] == "02"
    phone = _native_lines(native_tool, "send", f"key:{KEY.hex()}:1", f"video:{DELTA.hex()}", f"audio:{VOICE.hex()}",
                          f"key:{KEY.hex()}:1", f"video:{DELTA.hex()}",
                          f"key:{OTHER_KEY.hex()}:2", f"video:{DELTA.hex()}")
    assert [_counter(item) for item in phone] == [1, 2, 3, 1]
    assert phone[3][-2:] == "02"
    opened = _native_lines(native_tool, "receive", f"key:{OTHER_KEY.hex()}:2", f"video:{phone[3]}")
    assert opened == [DELTA.hex()]


def test_audio_and_video_through_one_encryptor_never_repeat_a_nonce(native_tool):
    steps = []
    for i in range(40):
        steps.append(f"audio:{VOICE.hex()}" if i % 3 else f"video:{DELTA.hex()}")
    phone = _native_lines(native_tool, "send", f"key:{KEY.hex()}:4", *steps)
    counters = [_counter(item) for item in phone]
    assert counters == list(range(1, 41))
    receive = [f"{'audio' if step.startswith('audio') else 'video'}:{sealed}" for step, sealed in zip(steps, phone)]
    opened = _native_lines(native_tool, "receive", f"key:{KEY.hex()}:4", *receive)
    assert opened == [bytes.fromhex(step.split(":")[1]).hex() for step in steps]
    worker = _worker_steps([
        {"op": "send-key", "key": KEY.hex(), "keyId": 4},
        {"op": "encrypt", "kind": "audio", "frames": [{"data": VOICE.hex(), "type": None}] * 3},
        {"op": "encrypt", "kind": "video", "frames": [{"data": DELTA.hex(), "type": "delta"}] * 3},
        {"op": "encrypt", "kind": "audio", "frames": [{"data": VOICE.hex(), "type": None}]},
    ])
    assert [_counter(item) for step in worker for item in step] == [1, 2, 3, 4, 5, 6, 7]

