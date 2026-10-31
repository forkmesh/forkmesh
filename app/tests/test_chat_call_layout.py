import json
import shutil
import subprocess
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
PUBLIC = ROOT / "public"
CSS = (PUBLIC / "chat.css").read_text(encoding="utf-8")
CALLS = (PUBLIC / "chat-calls.js").read_text(encoding="utf-8")
IOS_CALLS = ROOT.parent / "native" / "ios" / "ForkMesh" / "Features" / "Chat" / "Calls"
NATIVE_PEER = (IOS_CALLS / "Native" / "NativeCallPeer.swift").read_text(encoding="utf-8")
NATIVE_SURFACE = (IOS_CALLS / "Native" / "NativeCallPeerFactory.swift").read_text(encoding="utf-8")
IOS_VIEWS = (IOS_CALLS / "DirectCallViews.swift").read_text(encoding="utf-8")

DRAG_HARNESS = r'''
import { pathToFileURL } from "node:url";
const { createChatCalls } = await import(pathToFileURL(process.argv[2]).href);
class FakeStream {
  constructor(tracks = []) { this.tracks = [...tracks]; }
  getTracks() { return this.tracks; }
  getAudioTracks() { return this.tracks.filter((t) => t.kind === "audio"); }
  getVideoTracks() { return this.tracks.filter((t) => t.kind === "video"); }
  addTrack(t) { this.tracks.push(t); }
}
globalThis.MediaStream = FakeStream;
const track = (kind, id) => ({ kind, id, enabled: true, readyState: "live", stop() {} });
const box = { left: 100, top: 50, width: 360, height: 200 };
function element(id) {
  const listeners = {};
  const attributes = {};
  const el = {
    id, hidden: true, textContent: "", value: "", options: [], children: [], srcObject: null, style: {}, src: "",
    dataset: {}, listeners, captured: null,
    rect: { left: box.left + 268, top: box.top + 100, width: 84, height: 92 },
    classList: { on: new Set(), add(n) { this.on.add(n); }, remove(n) { this.on.delete(n); }, toggle(n, on) { if (on) this.on.add(n); else this.on.delete(n); } },
    setAttribute(name, value) { attributes[name] = String(value); },
    getAttribute(name) { return attributes[name] ?? null; },
    removeAttribute(name) { delete attributes[name]; },
    addEventListener(type, fn) { listeners[type] = fn; },
    append(...items) { this.options.push(...items); this.children.push(...items); },
    remove() {}, pause() {}, play() { return Promise.resolve(); },
    querySelector() { return null; },
    querySelectorAll() { return this.children; },
    click() { return listeners.click?.(); },
    setPointerCapture(id) { this.captured = id; },
    getBoundingClientRect() {
      if (this.style.left && this.style.left !== "") {
        return { ...this.rect, left: box.left + parseFloat(this.style.left), top: box.top + parseFloat(this.style.top) };
      }
      return this.rect;
    },
  };
  el.parentElement = { getBoundingClientRect: () => box };
  return el;
}
const elements = {};
const documentRef = {
  title: "Chat", hidden: true, hasFocus: () => false,
  querySelector(selector) {
    const id = selector.replace(/^#/, "");
    elements[id] ||= element(id);
    return elements[id];
  },
  createElement(tag) { return element(tag); },
};
const saved = { "fm.call.previewCorner": "top-right" };
const storage = { getItem: (k) => saved[k] ?? null, setItem(k, v) { saved[k] = v; }, removeItem(k) { delete saved[k]; } };
const calls = createChatCalls({
  selfId: "me",
  sendSignal: async () => true,
  getUserMediaImpl: async (c) => new FakeStream(c.video ? [track("audio", "mic"), track("video", "cam")] : [track("audio", "mic")]),
  RTCPeerConnectionImpl: class { getTransceivers() { return []; } close() {} },
  documentRef, storage,
  facesSupported: () => false, backgroundsSupported: () => false, voicesSupported: () => false,
  createVoicePreviewImpl: async ({ micStream }) => ({ stream: micStream, supported: false, headphones: false,
    choice: () => "off", voice: () => "", monitoring: () => false, setFace() {}, setChoice() {}, monitor() { return false; },
    track: () => micStream.getAudioTracks()[0], stop() {} }),
  setTimeoutImpl: () => 1, clearTimeoutImpl() {}, setIntervalImpl: () => 1, clearIntervalImpl() {},
  tones: { ring() {}, ringback() {}, stop() {} },
  NotificationImpl: undefined,
  navigatorRef: {},
  readRouteImpl: async () => null,
  URLImpl: { createObjectURL: () => "blob:x", revokeObjectURL() {} },
});
const local = elements["chat-call-local"];
const out = { restored: local.dataset.corner };
const press = (x, y) => local.listeners.pointerdown({ pointerId: 7, clientX: x, clientY: y, preventDefault() {} });
press(400, 180);
out.idleDrag = local.classList.on.has("is-dragging");
await calls.start("dm:bob", "bob", "video");
out.localShown = !local.hidden;
press(box.left + 300, box.top + 140);
out.dragging = local.classList.on.has("is-dragging");
out.captured = local.captured;
local.listeners.pointermove({ pointerId: 7, clientX: box.left + 30, clientY: box.top + 20 });
out.moving = { left: local.style.left, top: local.style.top, right: local.style.right };
local.listeners.pointermove({ pointerId: 7, clientX: box.left - 500, clientY: box.top - 500 });
out.clamped = { left: local.style.left, top: local.style.top };
local.listeners.pointerup({ pointerId: 7 });
out.dropped = { corner: local.dataset.corner, dragging: local.classList.on.has("is-dragging"), left: local.style.left, saved: saved["fm.call.previewCorner"] };
press(box.left + 40, box.top + 40);
local.listeners.pointermove({ pointerId: 7, clientX: box.left + 340, clientY: box.top + 190 });
local.listeners.pointercancel({ pointerId: 7 });
out.second = local.dataset.corner;
console.log(JSON.stringify(out));
'''


def _node():
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    return node


def test_answering_iphone_sends_its_camera_and_microphone():
    assert "connection.add(self.audioTrack, streamIds: Self.streamIds)" in NATIVE_PEER
    assert "connection.add(videoTrack, streamIds: Self.streamIds)" in NATIVE_PEER
    assert "addTransceiver(with:" not in NATIVE_PEER


def test_iphone_call_buttons_wrap_on_small_screens():
    controls = IOS_VIEWS.split("private var controls: some View", 1)[1].split("@ViewBuilder", 1)[0]
    assert "ViewThatFits(in: .horizontal)" in controls
    assert "VStack(spacing: FmSpace.x4)" in controls
    assert "HStack(spacing: FmSpace.x5) { looks }" in controls
    assert "HStack(spacing: FmSpace.x6) { essentials }" in controls


def test_iphone_own_video_can_be_dragged_to_a_corner():
    assert "DraggableCallPreview {" in NATIVE_SURFACE
    assert ".padding(.top, 90)" not in NATIVE_SURFACE
    assert "struct DraggableCallPreview<Content: View>: View" in IOS_VIEWS
    assert '@AppStorage("fm.call.previewCorner")' in IOS_VIEWS
    assert "DragGesture(minimumDistance: 4)" in IOS_VIEWS
    assert "Self.nearest(to: end, in: proxy.size)" in IOS_VIEWS


def test_web_call_panel_fits_small_screens():
    small = CSS.split("@media (max-width: 480px) {\n  .chat-call-panel", 1)[1].split("\n}\n", 1)[0]
    assert "left: 8px; right: 8px;" in small
    assert "width: auto;" in small
    assert ".chat-call-actions button { flex: 1 1 0; min-width: 0; min-height: 44px;" in small
    for corner in ("top-left", "top-right", "bottom-left"):
        assert f'#chat-call-local[data-corner="{corner}"]' in CSS
    assert "touch-action: none" in CSS.split("#chat-call-local {", 1)[1].split("}", 1)[0]


def test_web_own_video_drags_and_snaps_to_the_nearest_corner(tmp_path):
    harness = tmp_path / "drag.mjs"
    harness.write_text(DRAG_HARNESS, encoding="utf-8")
    result = subprocess.run(
        [_node(), str(harness), str(PUBLIC / "chat-calls.js")],
        capture_output=True, text=True, timeout=60, check=False,
    )
    assert result.returncode == 0, result.stderr
    out = json.loads(result.stdout.strip().splitlines()[-1])
    assert out["restored"] == "top-right"
    assert out["idleDrag"] is False
    assert out["localShown"] is True
    assert out["dragging"] is True
    assert out["captured"] == 7
    assert out["moving"] == {"left": "0px", "top": "0px", "right": "auto"}
    assert out["clamped"] == {"left": "0px", "top": "0px"}
    assert out["dropped"] == {"corner": "top-left", "dragging": False, "left": "", "saved": "top-left"}
    assert out["second"] == "bottom-right"
