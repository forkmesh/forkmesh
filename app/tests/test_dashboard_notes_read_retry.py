import json
import shutil
import subprocess
from pathlib import Path

import pytest


NOTES_JS = (Path(__file__).resolve().parents[1] / "public" / "dashboard" / "js"
            / "09-notes.js").read_text(encoding="utf-8")


def _note_request_source():
    start = NOTES_JS.index("  const NOTE_ERRORS = {")
    end = NOTES_JS.index("\n  }\n", NOTES_JS.index("async function noteRequest("))
    return NOTES_JS[start:end + 4]


def _run(statuses, method="GET"):
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    script = """
const statuses = %s;
const calls = [];
globalThis.setTimeout = (fn) => fn();
globalThis.fetch = async (path, init) => {
  const status = statuses[Math.min(calls.length, statuses.length - 1)];
  calls.push(init.method);
  if (status === "network") throw new TypeError("Failed to fetch");
  if (status === "bare") return { ok: false, status: 502, json: async () => { throw new SyntaxError("html"); } };
  const body = status < 400 ? { ok: true, notes: [] }
    : status === 502 ? { error: "upstream_unavailable" } : { error: "invalid_session" };
  return { ok: status < 400, status, json: async () => body };
};
const state = { session: { sessionToken: "t" } };
%s
noteRequest("/api/notes", %s, %s).then(
  (data) => console.log(JSON.stringify({ calls: calls.length, data })),
  (error) => console.log(JSON.stringify({ calls: calls.length, error: String(error.message), status: error.status })));
""" % (json.dumps(statuses), _note_request_source(), json.dumps(method),
       "null" if method == "GET" else "{}")
    out = subprocess.run([node, "-e", script], check=True, capture_output=True,
                         text=True, timeout=30)
    return json.loads(out.stdout.strip().splitlines()[-1])


def test_a_restarting_app_is_retried_until_it_answers():
    assert _run([502, "network", "bare", 200]) == {
        "calls": 4, "data": {"ok": True, "notes": []}}


def test_retries_stop_with_a_readable_message():
    result = _run([502])
    assert result["calls"] == 4 and result["status"] == 502
    assert result["error"] == "ForkMesh is restarting. Try again in a few seconds."


def test_refusals_and_writes_are_not_retried():
    assert _run([401])["calls"] == 1
    assert _run([502], method="POST")["calls"] == 1
