import re
import subprocess
from pathlib import Path


WORLD = Path(__file__).resolve().parents[1] / "public" / "world"


def _run(script):
    completed = subprocess.run(
        ["node", "-e", script], text=True, capture_output=True, check=False,
    )
    assert completed.returncode == 0, completed.stderr + completed.stdout
    return completed.stdout


def test_idle_extras_retry_a_failed_module_fetch_without_an_unhandled_rejection():
    index = (WORLD / "index.html").read_text(encoding="utf-8")
    inline = re.search(
        r'<script type="module">(.*?)</script>', index, re.S).group(1)
    script = r'''
const requested = [];
const timers = [];
const unhandled = [];
process.on("unhandledRejection", (reason) => unhandled.push(String(reason)));
function fakeImport(href) {
  requested.push(href);
  return Promise.reject(new TypeError("Importing a module script failed."));
}
const console = { warn() {} };
const window = {
  addEventListener(type, handler) { this.handler = handler; },
  setTimeout(fn) { timers.push(fn); },
};
''' + inline.replace("import(", "fakeImport(") + r'''
window.handler();
(async () => {
  for (let round = 0; round < 5; round += 1) {
    timers.splice(0).forEach((fn) => fn());
    await new Promise((resolve) => setTimeout(resolve, 10));
  }
  process.stdout.write(JSON.stringify({ requested, unhandled, timers: timers.length }));
})();
'''
    out = _run(script)
    assert '"unhandled":[]' in out
    assert '"/world/world-discord.js?retry=1"' in out
    assert '"/world/world-notes.js?retry=1"' in out
    assert '"timers":0' in out


def test_discord_panel_import_failure_is_handled_and_retryable():
    loader = (WORLD / "world-discord.js").read_text(encoding="utf-8")
    script = r'''
const unhandled = [];
process.on("unhandledRejection", (reason) => unhandled.push(String(reason)));
const requested = [];
let fail = true;
function fakeImport(href) {
  requested.push(href);
  if (fail) return Promise.reject(new TypeError("Importing a module script failed."));
  return Promise.resolve({
    bootWorldDiscord() {},
    showWorldDiscordPanel() { requested.push("shown"); return Promise.resolve(); },
  });
}
const listeners = {};
const console = { warn() {} };
const trigger = {
  setAttribute() {},
  addEventListener(type, handler) { this.click = handler; },
  removeEventListener() {},
};
const document = {
  readyState: "complete",
  querySelector() { return trigger; },
  body: {},
};
const window = {
  location: { href: "https://world.forkmesh.com/world/" },
  addEventListener(type, handler) { listeners[type] = handler; },
  removeEventListener() {},
};
class MutationObserver { observe() {} disconnect() {} }
''' + loader.replace("import(", "fakeImport(") + r'''
trigger.click();
setTimeout(() => {
  fail = false;
  trigger.click();
  setTimeout(() => {
    process.stdout.write(JSON.stringify({ requested, unhandled }));
  }, 20);
}, 20);
'''
    out = _run(script)
    assert '"unhandled":[]' in out
    assert '"./world-discord-panel.js?retry=1"' in out
    assert '"shown"' in out
