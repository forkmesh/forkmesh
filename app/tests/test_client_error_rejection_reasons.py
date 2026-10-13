import json
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
REPORTER = ROOT.parent / "www" / "public" / "analytics.js"

HARNESS = r'''
const fs = require("fs");
const listeners = {};
const posts = [];
class FakeEvent { constructor(type, target) { this.type = type; this.target = target; } }
class FakeResponse { constructor(status) { this.status = status; } }
global.Event = FakeEvent;
global.Response = FakeResponse;
global.navigator = { doNotTrack: "1" };
global.localStorage = { getItem() { return null; }, setItem() {}, removeItem() {} };
global.sessionStorage = global.localStorage;
global.location = new URL("https://forkmesh.com/");
global.document = {
  readyState: "complete",
  addEventListener() {},
  querySelector() { return null; },
  querySelectorAll() { return []; },
  createElement() { return { setAttribute() {}, addEventListener() {} }; },
  head: { appendChild() {} },
};
global.window = global;
window.addEventListener = (type, handler) => { (listeners[type] ||= []).push(handler); };
window.fetch = (url, init) => { posts.push(JSON.parse(init.body)); return Promise.resolve({ ok: true }); };
global.fetch = window.fetch;
eval(fs.readFileSync(process.argv[1], "utf8"));
let clock = 1_000_000;
Date.now = () => clock;
function reject(reason) {
  clock += 61_000;
  for (const handler of listeners.unhandledrejection || []) handler({ reason });
}
class OtherRealmError { constructor(message) { this.name = "TypeError"; this.message = message; this.stack = "TypeError: " + message + "\n    at load (https://forkmesh.com/x.js:1:1)"; } }
reject(undefined);
reject(null);
reject("plain string reason");
reject(new FakeEvent("error", { tagName: "SCRIPT" }));
reject(new FakeResponse(503));
reject({ code: "E_TIMEOUT", detail: 12345 });
reject(new Error(""));
reject(new OtherRealmError("module load failed"));
reject(new Error("regular failure"));
reject(42);
process.stdout.write(JSON.stringify(posts));
'''


def _reports():
    completed = subprocess.run(
        ["node", "-e", HARNESS, str(REPORTER)],
        cwd=ROOT, text=True, capture_output=True, check=False,
    )
    assert completed.returncode == 0, completed.stderr
    return json.loads(completed.stdout)


def test_rejection_reasons_that_are_not_errors_are_described():
    reports = _reports()
    messages = [report["message"] for report in reports]
    assert all(report["kind"] == "unhandledrejection" for report in reports)
    assert messages == [
        "Unhandled promise rejection: undefined",
        "Unhandled promise rejection: null",
        "plain string reason",
        'Unhandled promise rejection: FakeEvent "error" from script',
        "Unhandled promise rejection: Response status 503",
        "Unhandled promise rejection: Object with keys code, detail; code E_TIMEOUT",
        "Error with no message",
        "module load failed",
        "regular failure",
        "Unhandled promise rejection: number value",
    ]
    assert "12345" not in json.dumps(reports)
    assert all(len(message) <= 900 for message in messages)


def test_error_like_rejections_keep_their_stack():
    reports = {report["message"]: report for report in _reports()}
    assert reports["module load failed"]["stack"].startswith("TypeError: module load failed")
    assert reports["regular failure"]["stack"]
