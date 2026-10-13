#!/usr/bin/env python3
"""Web issue submission survives transient delivery failures (issue #685).

Reporting an issue from the web used to make exactly one attempt on the
canonical route after the mirrors: a dropped connection, an edge 5xx/HTML error
page or a momentarily unavailable number reservation surfaced as the bare
"Could not send the issue. Please try again." These tests run the real
assembled fetchRepoMutation under node with a scripted fetch.
"""

import json
import subprocess

from _dashboard_bundle import assembled_dashboard_js


DASHBOARD_JS = assembled_dashboard_js()


def _run(body):
    transformed = DASHBOARD_JS.replace(
        "  void bootDashboard();",
        "  globalThis.__dashboardExports = { fetchRepoMutation };",
    ).replace(
        "const REPO_MUTATION_RETRY_DELAYS_MS = [800, 2000, 4000];",
        "const REPO_MUTATION_RETRY_DELAYS_MS = [1, 1, 1];",
    ).replace(
        "const REPO_MUTATION_MIRROR_TIMEOUT_MS = 20000;",
        "const REPO_MUTATION_MIRROR_TIMEOUT_MS = 30;",
    ).replace(
        "const REPO_MUTATION_CANONICAL_TIMEOUT_MS = 30000;",
        "const REPO_MUTATION_CANONICAL_TIMEOUT_MS = 30;",
    ).replace(
        "const candidates = directRepoRequestCandidates(path, { mutation: true });",
        "const candidates = (globalThis.__mirrorCandidates"
        " || directRepoRequestCandidates)(path, { mutation: true });",
    )
    assert "__dashboardExports" in transformed, "export hook did not apply"
    assert "[1, 1, 1]" in transformed, "retry delays hook did not apply"
    assert "MIRROR_TIMEOUT_MS = 30;" in transformed, "mirror timeout hook did not apply"
    assert "CANONICAL_TIMEOUT_MS = 30;" in transformed, "canonical timeout hook did not apply"
    assert "__mirrorCandidates" in transformed, "candidate hook did not apply"
    script = "const SOURCE = " + json.dumps(transformed) + ";\n" + """
const assert = require("assert");
const vm = require("vm");
global.location = {
  origin: "https://app.forkmesh.com", hostname: "app.forkmesh.com",
  pathname: "/dashboard", search: "",
};
global.localStorage = { getItem() { return null; }, setItem() {}, removeItem() {} };
const emptyClassList = { add() {}, remove() {}, toggle() {}, contains() { return false; } };
global.document = {
  body: { classList: emptyClassList },
  documentElement: { dataset: {}, style: {} },
  cookie: "",
  querySelector() { return null; },
  querySelectorAll() { return []; },
  addEventListener() {},
};
global.window = {
  matchMedia() { return { addEventListener() {}, addListener() {} }; },
  addEventListener() {}, history: { pushState() {} }, lucide: { createIcons() {} },
  setTimeout() {}, setInterval() {}, clearInterval() {},
};
global.navigator = {};
vm.runInThisContext(SOURCE);
const { fetchRepoMutation } = global.__dashboardExports;
const repo = { owner: "jett", name: "forkmesh" };
const payload = { owner: "jett", repo: "forkmesh", number: 0, event: { sig: "s" } };
const calls = [];
const json = (status, data) => ({ ok: status >= 200 && status < 300, status,
  json: async () => data });
const html = (status) => ({ ok: false, status,
  json: async () => { throw new SyntaxError("Unexpected token <"); } });
function script(...steps) {
  calls.length = 0;
  global.fetch = (url, init) => {
    calls.push({ url, body: init.body, signal: init.signal });
    const step = steps.shift();
    return typeof step === "function" ? step(init) : Promise.resolve(step);
  };
}
const offline = () => Promise.reject(new TypeError("Failed to fetch"));
(async () => {
""" + body + """
})().catch((error) => { console.error(error); process.exit(1); });
"""
    result = subprocess.run(
        ["node"], input=script, capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stderr or result.stdout
    assert result.stderr == ""


def test_transient_canonical_failures_retry_the_same_signed_payload():
    _run("""
script(html(502), offline, json(503, { error: "number_unavailable" }),
       json(201, { ok: true, number: 686 }));
const progress = [];
const data = await fetchRepoMutation(repo, "issues", payload, {
  retryTransient: true, onProgress: (p) => progress.push(p) });
assert.equal(data.number, 686);
assert.equal(calls.length, 4);
assert.ok(calls.every((call) => call.body === calls[0].body));
assert.deepEqual(progress.map((p) => p.retry), [false, true, true, true]);
""")


def test_retries_are_bounded_and_report_the_network_failure():
    _run("""
script(offline, offline, offline, offline, offline);
await assert.rejects(
  fetchRepoMutation(repo, "issues", payload, { retryTransient: true }),
  (error) => error.message === "network_error" && error.status === 0);
assert.equal(calls.length, 4);
""")


def test_a_hung_repository_attempt_times_out_and_is_retried():
    # Issue #698: a canonical attempt that never answered left the form on
    # "retrying the same signed issue…" indefinitely.
    _run("""
const hang = (init) => new Promise((_, reject) => {
  init.signal.addEventListener("abort", () => reject(new Error("aborted")));
});
script(hang, json(201, { ok: true, number: 698 }));
const progress = [];
const data = await fetchRepoMutation(repo, "issues", payload, {
  retryTransient: true, onProgress: (p) => progress.push(p) });
assert.equal(data.number, 698);
assert.equal(calls.length, 2);
assert.equal(calls[0].body, calls[1].body);
assert.deepEqual(progress.map((p) => [p.attempt, p.attempts]), [[1, 4], [2, 4]]);
assert.equal(progress[1].lastError.code, "timeout");
""")


def test_a_repository_that_never_answers_fails_with_timeout():
    _run("""
const hang = (init) => new Promise((_, reject) => {
  init.signal.addEventListener("abort", () => reject(new Error("aborted")));
});
script(hang, hang, hang, hang);
await assert.rejects(
  fetchRepoMutation(repo, "issues", payload, { retryTransient: true }),
  (error) => error.code === "timeout" && error.transient === true);
assert.equal(calls.length, 4);
""")


def test_definitive_refusals_are_not_retried():
    _run("""
script(json(429, { error: "author_quota" }), json(201, { ok: true, number: 1 }));
await assert.rejects(
  fetchRepoMutation(repo, "issues", payload, { retryTransient: true }),
  (error) => error.message === "author_quota" && error.status === 429);
assert.equal(calls.length, 1);
""")


def test_callers_that_did_not_opt_in_keep_a_single_attempt():
    _run("""
script(html(502), json(201, { ok: true, number: 1 }));
await assert.rejects(fetchRepoMutation(repo, "issues", payload),
  (error) => error.message === "HTTP 502" && error.status === 502);
assert.equal(calls.length, 1);
""")


def test_a_hung_mirror_times_out_and_falls_through_to_the_repository():
    _run("""
global.__mirrorCandidates = () => ["https://mirror1.example/api/repo/jett/forkmesh/issues"];
const hang = (init) => new Promise((_, reject) => {
  init.signal.addEventListener("abort", () => reject(new Error("aborted")));
});
script(hang, json(201, { ok: true, number: 42 }));
const data = await fetchRepoMutation(repo, "issues", payload, { retryTransient: true });
assert.equal(data.number, 42);
assert.equal(calls.length, 2);
assert.ok(calls[0].url.startsWith("https://mirror1.example/"));
assert.ok(!calls[1].url.startsWith("https://"));
""")


def test_a_mirror_refusal_beats_an_unreachable_repository():
    _run("""
global.__mirrorCandidates = () => ["https://mirror1.example/api/repo/jett/forkmesh/issues"];
script(json(413, { ok: false, error: "issue_too_large" }), offline, offline, offline, offline);
await assert.rejects(
  fetchRepoMutation(repo, "issues", payload, { retryTransient: true }),
  (error) => error.message === "issue_too_large");
assert.equal(calls.length, 5);
""")


def test_issue_form_opts_in_and_explains_each_failure():
    signer = DASHBOARD_JS[
        DASHBOARD_JS.index("async function submitWebIssue("):
        DASHBOARD_JS.index("function issueSubtaskCandidates(")
    ]
    assert "retryTransient: true" in signer
    assert 'new Error("signing_unavailable")' in signer
    submit = DASHBOARD_JS[
        DASHBOARD_JS.index("async function handleIssueComposeSubmit"):
        DASHBOARD_JS.index("const ISSUE_CSV_COLUMNS")
    ]
    for text in (
        "retrying the same signed issue",
        'code === "signing_unavailable"',
        'code === "bad_signature"',
        'code === "network_error"',
        'code === "timeout"',
        "lastError?.code",
        "(HTTP ${status})",
        "Your draft is still here",
    ):
        assert text in submit
