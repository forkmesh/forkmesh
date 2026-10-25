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
await page.route("**/*", async (route) => {
  const url = new URL(route.request().url());
  if (url.host !== "appassets.androidplatform.net" || !url.pathname.startsWith("/group-call/")) {
    external.push(url.href);
    return route.fulfill({ status: 404, body: "" });
  }
  const name = url.pathname.slice("/group-call/".length);
  const type = name.endsWith(".html") ? "text/html" : "text/javascript";
  return route.fulfill({ status: 200, contentType: type, body: await readFile(path.join(assets, name)) });
});
await page.addInitScript(() => {
  window.__posts = [];
  window.FmGroupBridge = { post: (text) => window.__posts.push(JSON.parse(text)) };
});
await page.goto("https://appassets.androidplatform.net/group-call/index.html");
const events = (name) => page.evaluate((wanted) => window.__posts.filter((item) => item.event === wanted), name);
await page.waitForFunction(() => window.__posts.some((item) => item.event === "ready"), null, { timeout: 15000 });
const out = { ready: true };
out.mode = await page.evaluate(() => (typeof RTCRtpScriptTransform === "function" ? "script" : (RTCRtpSender.prototype.createEncodedStreams ? "streams" : "")));
await page.evaluate(() => window.fmGroup(1, "frame", [{ type: "group-call-presence", peer: "0123456789abcdef0123456789abcdef", call: "in", sender: "pat" }, "#office/team"]));
await page.evaluate(() => window.fmGroup(2, "join", ["#office/team", "video", "Jett", "rgba(10,132,255,.35)"]));
await page.waitForFunction(() => window.__posts.some((item) => item.event === "ticket"), null, { timeout: 15000 });
const ticket = (await events("ticket"))[0];
out.ticketChannel = ticket.channel;
out.ticketBody = ticket.body;
await page.evaluate((id) => window.fmGroupTicket(id, true, { ticket: "t", room: "r", peer: "fedcba9876543210fedcba9876543210", url: "wss://127.0.0.1:9/calls/ws" }), ticket.id);
await page.waitForFunction(() => window.__posts.some((item) => item.event === "result" && item.id === 2), null, { timeout: 15000 });
out.join = (await events("result")).find((item) => item.id === 2);
out.states = (await events("state")).map((item) => item.state);
out.active = (await events("state")).some((item) => item.active);
out.tiles = await page.evaluate(() => [...document.querySelectorAll(".tile .name")].map((item) => item.textContent));
out.selfMirrored = await page.evaluate(() => document.querySelector(".tile.self") !== null);
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


def test_the_host_page_turns_fm_media_off_and_never_touches_storage():
    text = HOST.read_text(encoding="utf-8")
    assert "fmm: { enabled: false }" in text
    assert "callSigner: null" in text
    assert "localStorage" not in text
    page = (ASSETS / "index.html").read_text(encoding="utf-8")
    assert "default-src 'none'" in page and "connect-src 'self' wss:" in page


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
    assert out["ticketBody"] == {}
    assert out["join"]["ok"] is True
    assert out["active"]
    assert "joining" in out["states"]
    assert out["tiles"][0] == "You"
    assert out["left"]
    assert out["tilesAfter"] == 0
    assert out["external"] == []
