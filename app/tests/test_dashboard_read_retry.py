"""Dashboard reads ride out a wedged Worker isolate instead of erroring."""

import json
import shutil
import subprocess
from pathlib import Path

import pytest


HELPERS = (Path(__file__).resolve().parents[1] / "public" / "dashboard" / "js"
           / "02-helpers.js").read_text(encoding="utf-8")


def _fetch_json_source():
    start = HELPERS.index("  const FETCH_JSON_RETRY_DELAYS_MS")
    end = HELPERS.index("\n  }\n", HELPERS.index("async function fetchJson("))
    return HELPERS[start:end + 4]


def _run(statuses):
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    script = """
const statuses = %s;
const calls = [];
globalThis.setTimeout = (fn) => fn();
globalThis.fetch = async (path) => {
  const status = statuses[Math.min(calls.length, statuses.length - 1)];
  calls.push(path);
  if (status === "network") throw new TypeError("Failed to fetch");
  return { ok: status < 400, status, json: async () => status < 400 ? { ok: true } : { error: "boom" } };
};
const state = {};
const cloneJson = (value) => JSON.parse(JSON.stringify(value));
const fetchJsonCacheTtl = () => 0;
const cacheBustedPath = (path) => path;
%s
fetchJson("/api/notifications").then(
  (data) => console.log(JSON.stringify({ calls: calls.length, data })),
  (error) => console.log(JSON.stringify({ calls: calls.length, error: String(error.message), status: error.status })));
""" % (json.dumps(statuses), _fetch_json_source())
    out = subprocess.run([node, "-e", script], check=True, capture_output=True,
                         text=True, timeout=30)
    return json.loads(out.stdout.strip().splitlines()[-1])


def test_a_wedged_isolate_500_is_retried_until_a_healthy_answer():
    assert _run([500, "network", 200]) == {"calls": 3, "data": {"ok": True}}


def test_retries_stop_after_the_bounded_delays():
    assert _run([503]) == {"calls": 3, "error": "boom", "status": 503}


def test_definitive_refusals_and_rate_limits_are_not_retried():
    assert _run([401]) == {"calls": 1, "error": "boom", "status": 401}
    assert _run([429]) == {"calls": 1, "error": "boom", "status": 429}
