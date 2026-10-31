import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = ROOT.parent
ASSETS = REPOSITORY / "native" / "android" / "app" / "src" / "main" / "assets" / "group-call"
SYNC = ROOT / "tools" / "sync_android_group_call.py"
HOST = ASSETS / "group-call-host.js"

HARNESS = r'''
import { createRequire } from "node:module";
import { readFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";

const assets = process.argv.at(-1);
const require = createRequire(import.meta.url);
function loadPlaywright() {
  const places = [
    process.env.FM_PLAYWRIGHT_CORE,
    path.join(assets, "..", "..", "..", "..", "..", "..", "..", "app", "browser_tests", "node_modules", "playwright-core"),
    path.join(os.homedir(), "projects", "forkmesh", "src", "app", "browser_tests", "node_modules", "playwright-core"),
  ].filter(Boolean);
  for (const place of places) {
    try {
      return require(place);
    } catch (_) {}
  }
  throw new Error("playwright-core not found");
}

const { chromium } = loadPlaywright();
const browser = await chromium.launch({ args: ["--use-fake-device-for-media-stream", "--use-fake-ui-for-media-stream", "--autoplay-policy=no-user-gesture-required"] });
const context = await browser.newContext({ permissions: ["camera", "microphone"] });
const page = await context.newPage();
const external = [];
const dataServed = [];
const BLOCKS = { "face-photos.js": "fm-call-face-photos", "backgrounds.js": "fm-call-backgrounds", "models.js": "fm-call-models" };
const directPage = await readFile(path.join(assets, "..", "direct-call.html"), "utf8");
await page.route("**/*", async (route) => {
  const url = new URL(route.request().url());
  if (url.host !== "appassets.androidplatform.net" || !url.pathname.startsWith("/group-call/")) {
    external.push(url.href);
    return route.fulfill({ status: 404, body: "" });
  }
  const name = url.pathname.slice("/group-call/".length);
  if (name.startsWith("call-data/")) {
    const id = BLOCKS[name.slice("call-data/".length)];
    const open = `<script id="${id}">`;
    const start = id ? directPage.indexOf(open) : -1;
    if (start < 0) return route.fulfill({ status: 404, body: "" });
    dataServed.push(name);
    const body = directPage.slice(start + open.length, directPage.indexOf("</script>", start));
    return route.fulfill({ status: 200, contentType: "text/javascript", body });
  }
  const type = name.endsWith(".html") ? "text/html" : "text/javascript";
  return route.fulfill({ status: 200, contentType: type, body: await readFile(path.join(assets, name)) });
});
await page.addInitScript(() => {
  window.__posts = [];
  window.__signed = [];
  window.FmGroupBridge = {
    post: (text) => window.__posts.push(JSON.parse(text)),
    sign: (call, data) => { window.__signed.push(call); return ""; },
  };
});
await page.goto("https://appassets.androidplatform.net/group-call/index.html");
const events = (name) => page.evaluate((wanted) => window.__posts.filter((item) => item.event === wanted), name);
await page.waitForFunction(() => window.__posts.some((item) => item.event === "ready"), null, { timeout: 15000 });
const out = { ready: true };
out.mode = await page.evaluate(() => (typeof RTCRtpScriptTransform === "function" ? "script" : (RTCRtpSender.prototype.createEncodedStreams ? "streams" : "")));
await page.evaluate(() => window.fmGroup(1, "frame", [{ type: "group-call-presence", peer: "0123456789abcdef0123456789abcdef", call: "in", sender: "pat" }, "#office/team"]));
out.faceStream = await page.evaluate(() => import(new URL("chat-call-facestream.js", location.href).href).then((m) => typeof m.createFaceStreamView));
out.fmmWorker = await page.evaluate(() => new Promise((resolve) => {
  const worker = new Worker(new URL("fmm/fmm-worker.js", location.href).href, { type: "module" });
  worker.onerror = () => resolve("error");
  setTimeout(() => { worker.terminate(); resolve("loaded"); }, 1500);
}));
out.fmmSupported = await page.evaluate(() => import(new URL("fmm/fmm-call.js", location.href).href).then((m) => m.fmmSupported(window)));
const identity = { call: "chat-channel:team:v1", acct: "jett", kid: "k", idk: "A".repeat(43) };
const look = { face: "", faceImage: "", background: "blur", backgroundImage: "", voice: "" };
await page.evaluate(([who, chosen]) => window.fmGroup(2, "join", ["#office/team", "video", "Jett", "rgba(10,132,255,.35)", "chat-channel:team:v1", who, chosen]), [identity, look]);
await page.waitForFunction(() => window.__posts.some((item) => item.event === "ticket"), null, { timeout: 15000 });
const ticket = (await events("ticket"))[0];
out.ticketChannel = ticket.channel;
out.ticketBody = ticket.body;
await page.evaluate((id) => window.fmGroupTicket(id, true, { ticket: "t", room: "r", peer: "fedcba9876543210fedcba9876543210", url: "wss://127.0.0.1:9/calls/ws" }), ticket.id);
await page.waitForFunction(() => window.__posts.some((item) => item.event === "result" && item.id === 2), null, { timeout: 15000 });
out.join = (await events("result")).find((item) => item.id === 2);
out.states = (await events("state")).map((item) => item.state);
out.engines = (await events("state")).map((item) => item.engine);
out.active = (await events("state")).some((item) => item.active);
out.tiles = await page.evaluate(() => [...document.querySelectorAll(".tile .name")].map((item) => item.textContent));
out.selfMirrored = await page.evaluate(() => document.querySelector(".tile.self") !== null);
out.dataServed = [...dataServed].sort();
out.faceModels = await page.evaluate(() => Boolean(window.FmCallSegmentModels && window.FmCallSegmentModels.detector && window.FmCallSegmentModels.mesh));
await page.evaluate(() => window.fmGroup(5, "rekeyed", ["#office/team", "chat-channel:team:v1"]));
await page.waitForFunction(() => window.__posts.some((item) => item.event === "result" && item.id === 5), null, { timeout: 15000 });
out.sameRoom = (await events("result")).find((item) => item.id === 5).value;
const asked = (await events("ticket")).length;
await page.evaluate(() => window.fmGroup(6, "rekeyed", ["#office/team", "chat-channel:team:v2"]));
await page.waitForFunction((count) => window.__posts.filter((item) => item.event === "ticket").length > count, asked, { timeout: 15000 });
const second = (await events("ticket")).at(-1);
await page.evaluate((id) => window.fmGroupTicket(id, true, { ticket: "t2", room: "r2", peer: "abcdefabcdefabcdefabcdefabcdefab", url: "wss://127.0.0.1:9/calls/ws" }), second.id);
await page.waitForFunction(() => window.__posts.some((item) => item.event === "result" && item.id === 6), null, { timeout: 15000 });
out.rotated = (await events("result")).find((item) => item.id === 6).value;
out.tickets = (await events("ticket")).length;
await page.evaluate(() => window.fmGroup(3, "mute", []));
await page.waitForFunction(() => window.__posts.some((item) => item.event === "state" && item.muted), null, { timeout: 5000 });
out.muted = true;
await page.evaluate(() => window.fmGroup(4, "leave", []));
await page.waitForFunction(() => window.__posts.some((item) => item.event === "state" && !item.active && item.state === ""), null, { timeout: 5000 });
out.left = true;
out.tilesAfter = await page.evaluate(() => document.querySelectorAll(".tile").length);
out.sends = (await events("send")).map((item) => item.type);
out.external = external.filter((url) => !url.startsWith("wss:"));
await browser.close();
console.log(JSON.stringify(out));
'''


def test_the_shared_modules_are_current_copies():
    result = subprocess.run([sys.executable, str(SYNC), "--check"], capture_output=True, text=True, check=False)
    assert result.returncode == 0, result.stdout + result.stderr


def test_the_host_page_runs_fm_media_and_never_touches_storage():
    text = HOST.read_text(encoding="utf-8")
    assert "enabled: false" not in text
    assert "    callSigner,\n" in text
    assert 'workerUrl: asset("fmm/fmm-worker.js")' in text
    assert 'faceStreamUrl: asset("chat-call-facestream.js")' in text
    assert "bridge.sign(current.call," in text
    assert "calls.confirmCode(Boolean(match))" in text
    assert "localStorage" not in text
    page = (ASSETS / "index.html").read_text(encoding="utf-8")
    assert "default-src 'none'" in page and "connect-src 'self' wss: https:;" in page
    assert "img-src data: blob:;" in page


def test_the_page_applies_the_saved_look_and_feeds_face_streams():
    text = HOST.read_text(encoding="utf-8")
    assert "    prepareLocal,\n" in text
    assert "createFaceStage({" in text and "createVoiceStage({" in text
    assert "renderer.trackFace(video, w, h)" in text
    assert 'script.src = asset("call-data/" + name + ".js")' in text
    feature = (REPOSITORY / "native" / "android" / "app" / "src" / "main" / "kotlin" / "com" / "forkmesh"
               / "nativeapp" / "feature" / "chat" / "GroupCallFeature.kt").read_text(encoding="utf-8")
    assert "GroupCallLook.page(FaceChangerFeature.look, video = media == DirectCallKind.VIDEO)" in feature
    direct = (ASSETS.parent / "direct-call.html").read_text(encoding="utf-8")
    for block in ("fm-call-face-photos", "fm-call-backgrounds", "fm-call-models"):
        assert f'<script id="{block}">' in direct


def test_the_app_signs_only_through_the_call_identity():
    page = (REPOSITORY / "native" / "android" / "app" / "src" / "main" / "kotlin" / "com" / "forkmesh"
            / "nativeapp" / "feature" / "chat" / "GroupCallPage.kt").read_text(encoding="utf-8")
    assert "fun sign(call: String?, data: String?): String" in page
    feature = (REPOSITORY / "native" / "android" / "app" / "src" / "main" / "kotlin" / "com" / "forkmesh"
               / "nativeapp" / "feature" / "chat" / "GroupCallFeature.kt").read_text(encoding="utf-8")
    assert "identity?.sign(call, message)" in feature
    core = (REPOSITORY / "native" / "android" / "core" / "src" / "main" / "kotlin" / "com" / "forkmesh"
            / "nativeapp" / "chat" / "OfficeChannels.kt").read_text(encoding="utf-8")
    assert "GroupCallE2ee.annotate(plain, opened.senderKid, opened.senderAcct)" in core


def test_the_page_joins_reports_state_and_leaves_in_chromium(tmp_path):
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    script = tmp_path / "harness.mjs"
    script.write_text(HARNESS, encoding="utf-8")
    env = {k: v for k, v in os.environ.items() if k not in ("FORKMESH_HOME", "FORKMESH_PROJECTS_DIR")}
    result = subprocess.run([node, str(script), str(ASSETS)], capture_output=True, text=True, timeout=180, check=False, env=env)
    if result.returncode != 0 and "playwright-core not found" in result.stderr:
        pytest.skip("playwright-core is not installed")
    assert result.returncode == 0, result.stderr[-2000:]
    out = json.loads(result.stdout.strip().splitlines()[-1])
    assert out["mode"] in ("script", "streams")
    assert out["ticketChannel"] == "#office/team"
    assert out["faceStream"] == "function"
    assert out["fmmWorker"] == "loaded"
    assert out["ticketBody"] == ({"engines": ["fmm", "webrtc"]} if out["fmmSupported"] else {})
    assert "webrtc" in out["engines"]
    assert out["join"]["ok"] is True
    assert out["sameRoom"] is False
    assert out["rotated"] is True
    assert out["tickets"] >= 2
    assert out["active"]
    assert "joining" in out["states"]
    assert out["tiles"][0] == "You"
    assert out["left"]
    assert out["tilesAfter"] == 0
    assert out["external"] == []
    assert out["dataServed"] == ["call-data/backgrounds.js", "call-data/face-photos.js", "call-data/models.js"]
    assert out["faceModels"]


def test_the_app_tells_the_page_when_the_channel_room_changes():
    text = HOST.read_text(encoding="utf-8")
    assert "calls.channelRekeyed(String(channel), String(chatRoom" in text
    assert "callId: () => joinRoom" in text
    feature = (REPOSITORY / "native" / "android" / "app" / "src" / "main" / "kotlin" / "com" / "forkmesh"
               / "nativeapp" / "feature" / "chat" / "GroupCallFeature.kt").read_text(encoding="utf-8")
    assert "service.onRoomOpened = { conversation, roomName -> roomOpened(conversation, roomName) }" in feature
    assert 'current.post("rekeyed", listOf(conversation, roomName, fresh?.page()))' in feature
    assert "chatRoomName(conversation), identity?.page(), look)" in feature
    core = (REPOSITORY / "native" / "android" / "core" / "src" / "main" / "kotlin" / "com" / "forkmesh"
            / "nativeapp" / "chat" / "OfficeChannels.kt").read_text(encoding="utf-8")
    assert "onRoomOpened?.invoke(room.conversation, roomName)" in core
