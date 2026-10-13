import json
import os
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CONTROLLER = ROOT.parent / "www" / "public" / "analytics.js"

HARNESS = r'''
const fs = require("fs");
const [source, path, consent, dnt, framed] = process.argv.slice(1);
const store = consent ? { "forkmesh.analyticsConsent.v1": consent } : {};
const listeners = {};
const scripts = [];
function element(tag) {
  const node = { tagName: tag.toUpperCase(), children: [], dataset: {}, attributes: {},
    handlers: {}, parent: null, textContent: "", id: "" };
  node.setAttribute = (name, value) => { node.attributes[name] = value; };
  node.addEventListener = (type, handler) => { (node.handlers[type] ||= []).push(handler); };
  node.append = (...items) => { for (const item of items) { item.parent = node; node.children.push(item); } };
  node.appendChild = (item) => { node.append(item); return item; };
  node.remove = () => { if (node.parent) node.parent.children = node.parent.children.filter((item) => item !== node); node.parent = null; };
  node.click = () => { for (const handler of node.handlers.click || []) handler({ target: node }); };
  return node;
}
function find(node, test) {
  for (const child of node.children) { if (test(child)) return child; const hit = find(child, test); if (hit) return hit; }
  return null;
}
const body = element("body");
const head = element("head");
head.appendChild = (item) => { scripts.push(item.src); return item; };
global.navigator = { doNotTrack: dnt };
global.localStorage = { getItem: (key) => store[key] ?? null, setItem: (key, value) => { store[key] = value; }, removeItem: (key) => { delete store[key]; } };
global.sessionStorage = global.localStorage;
global.location = new URL("https://forkmesh.com" + path);
global.document = { readyState: "complete", body, head, addEventListener() {},
  createElement: element, getElementById: (id) => find(body, (node) => node.id === id) };
global.window = global;
window.addEventListener = (type, handler) => { (listeners[type] ||= []).push(handler); };
window.dispatchEvent = (event) => { for (const handler of listeners[event.type] || []) handler(event); };
global.CustomEvent = class { constructor(type, init) { this.type = type; this.detail = init?.detail; } };
if (framed === "1") Object.defineProperty(window, "top", { value: {} });
else window.top = window;
eval(fs.readFileSync(source, "utf8"));
const prompt = document.getElementById("fm-analytics-consent");
const result = { shown: Boolean(prompt), consent: null, scripts: [], remaining: false };
if (prompt) {
  const buttons = [];
  (function walk(node) { for (const child of node.children) { if (child.tagName === "BUTTON") buttons.push(child); walk(child); } })(prompt);
  result.buttons = buttons.map((button) => [button.dataset.choice, button.textContent]);
  result.link = find(prompt, (node) => node.tagName === "A").href;
  result.text = find(prompt, (node) => node.tagName === "P").textContent;
  const wanted = process.env.CHOICE;
  if (wanted) buttons.find((button) => button.dataset.choice === wanted).click();
  result.consent = store["forkmesh.analyticsConsent.v1"] ?? null;
  result.remaining = Boolean(document.getElementById("fm-analytics-consent"));
  result.scripts = scripts;
}
process.stdout.write(JSON.stringify(result));
'''


def _run(path="/", consent="", dnt="", framed="0", choice=""):
    completed = subprocess.run(
        ["node", "-e", HARNESS, str(CONTROLLER), path, consent, dnt, framed],
        cwd=ROOT, text=True, capture_output=True, check=False,
        env={**os.environ, "CHOICE": choice},
    )
    assert completed.returncode == 0, completed.stderr
    return json.loads(completed.stdout)


def test_public_pages_ask_once_and_allowing_starts_the_first_party_sdk():
    shown = _run("/", choice="grant")
    assert shown["shown"] is True
    assert shown["buttons"] == [["deny", "No thanks"], ["grant", "Allow analytics"]]
    assert shown["link"] == "/privacy#analytics-choice-heading"
    assert "stores no IP address" in shown["text"]
    assert shown["consent"] == "granted" and shown["remaining"] is False
    assert shown["scripts"] == ["/analytics-sdk.js"]


def test_declining_is_stored_and_sends_nothing():
    declined = _run("/docs/install/", choice="deny")
    assert declined["consent"] == "denied" and declined["remaining"] is False
    assert declined["scripts"] == []
    assert _run("/docs/install/", consent="denied")["shown"] is False


def test_the_prompt_never_appears_where_it_should_not():
    assert _run("/dashboard/marketing")["shown"] is False
    assert _run("/alice/repo")["shown"] is False
    assert _run("/privacy")["shown"] is False
    assert _run("/", dnt="1")["shown"] is False
    assert _run("/", framed="1")["shown"] is False
    assert _run("/blog/", consent="granted")["shown"] is False
