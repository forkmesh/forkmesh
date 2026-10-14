#!/usr/bin/env python3

import ast
import asyncio
import json
import re
import shutil
import subprocess
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import urlparse

import pytest


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
ENTRY_TEXT = ENTRY.read_text(encoding="utf-8")
SIGNUP_JS = ROOT / "public" / "signup.js"


def _load_renew(origin_ok=True):
    wanted = {"_account_signup_intent_renew"}
    consts = {"SIGNUP_INTENT_PATH", "ACCOUNT_SIGNUP_INTENT_TTL_MS"}
    selected = [
        node for node in ast.parse(ENTRY_TEXT, filename=str(ENTRY)).body
        if (isinstance(node, ast.AsyncFunctionDef) and node.name in wanted)
        or (isinstance(node, ast.Assign) and {
            target.id for target in node.targets
            if isinstance(target, ast.Name)} & consts)
    ]
    issued = []

    async def issue(_env):
        issued.append(1)
        return "c" * 36, 1000, 1000 + 15 * 60 * 1000, "contact_" + "ab" * 9

    def cookie(_env, claim_id, issued_at, expires_at, secure=True):
        return "forkmesh_signup_intent=%s.%d.%d; secure=%s" % (
            claim_id, issued_at, expires_at, secure)

    def json_response(data, status=200, cache_control=None,
                      extra_headers=None):
        return SimpleNamespace(
            data=data, status=status, cache_control=cache_control,
            headers=dict(extra_headers or {}))

    namespace = {
        "_request_origin_allowed": lambda _env, _request: origin_ok,
        "_issue_account_signup_intent": issue,
        "_account_signup_intent_cookie": cookie,
        "_turnstile": SimpleNamespace(
            signup_cdata=lambda claim_id: "cd" + claim_id[:8]),
        "json_response": json_response,
        "urlparse": urlparse,
    }
    module = ast.fix_missing_locations(ast.Module(selected, type_ignores=[]))
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace, issued


def test_renewal_mints_a_fresh_claim_cookie_and_page_fields():
    ns, issued = _load_renew()
    request = SimpleNamespace(url="https://forkmesh.com/api/accounts/signup/intent")
    response = asyncio.run(ns["_account_signup_intent_renew"](object(), request))
    assert response.status == 200
    assert issued == [1]
    assert response.data == {
        "emailField": "contact_" + "ab" * 9,
        "challengeCdata": "cdcccccccc",
    }
    assert response.headers["set-cookie"].startswith(
        "forkmesh_signup_intent=" + "c" * 36)
    assert "secure=True" in response.headers["set-cookie"]
    assert "no-store" in response.cache_control
    assert ns["SIGNUP_INTENT_PATH"] == "/api/accounts/signup/intent"


def test_renewal_refuses_cross_origin_callers_without_minting():
    ns, issued = _load_renew(origin_ok=False)
    request = SimpleNamespace(url="https://forkmesh.com/api/accounts/signup/intent")
    response = asyncio.run(ns["_account_signup_intent_renew"](object(), request))
    assert response.status == 403
    assert issued == []
    assert "set-cookie" not in response.headers


def test_renewal_is_dispatched_by_the_accounts_handler():
    dispatch = ENTRY_TEXT[ENTRY_TEXT.index("async def accounts_handler"):]
    dispatch = dispatch[:dispatch.index("/api/accounts/reserve")]
    assert "url.path == SIGNUP_INTENT_PATH" in dispatch
    assert "_account_signup_intent_renew(env, request)" in dispatch


def test_page_renews_before_the_server_claim_expires():
    ttl = re.search(
        r"^ACCOUNT_SIGNUP_INTENT_TTL_MS = (\d+) \* (\d+) \* (\d+)$",
        ENTRY_TEXT, re.M)
    server_ttl = int(ttl[1]) * int(ttl[2]) * int(ttl[3])
    renew = re.search(
        r"const SIGNUP_INTENT_RENEW_MS = (\d+) \* (\d+) \* (\d+);",
        SIGNUP_JS.read_text(encoding="utf-8"))
    page_renew = int(renew[1]) * int(renew[2]) * int(renew[3])
    assert page_renew <= server_ttl - 2 * 60 * 1000


HARNESS = r"""
import { readFileSync } from "node:fs";
import vm from "node:vm";

const source = readFileSync(process.argv[2], "utf8");

function element(id, extra = {}) {
  const listeners = {};
  return Object.assign({
    id, value: "", checked: false, name: "", textContent: "", className: "",
    hidden: false, disabled: false, dataset: {}, attrs: {}, listeners,
    addEventListener(type, fn) { (listeners[type] ||= []).push(fn); },
    setAttribute(key, value) { this.attrs[key] = String(value); },
    getAttribute(key) { return this.attrs[key] ?? null; },
    querySelectorAll() { return []; },
    insertBefore() {},
  }, extra);
}

async function run(scenario) {
  const elements = {
    "#signup-form": element("signup-form"),
    "#node-name": element("node-name", { value: "solvingsoft" }),
    "#acct-email": element("acct-email", { value: "person@example.org", name: "contact_000000000000000000" }),
    "#acct-pass": element("acct-pass", { value: "correct horse battery" }),
    "#terms-agree": element("terms-agree", { checked: true }),
    "#updates-agree": element("updates-agree"),
    "#signup-create": element("signup-create"),
    "#name-hint": element("name-hint"),
    "#email-hint": element("email-hint"),
    "#signup-hint": element("signup-hint"),
    "#signup-turnstile": element("signup-turnstile", { dataset: { sitekey: "k", action: "signup", cdata: "cdata0" } }),
    "#signup-view": element("signup-view"),
    "#verify-view": element("verify-view", { hidden: true }),
    "#recap-name": element("recap-name"),
    "#recap-email": element("recap-email"),
    "#mail-links": element("mail-links"),
  };
  const calls = [];
  const widgets = [];
  let signupAnswers = scenario.signupAnswers.slice();
  const json = (status, body) => ({ ok: status >= 200 && status < 300, status, json: async () => body });
  const realNow = Date.now;
  let shift = 0;
  const sandbox = {
    console,
    setTimeout, clearTimeout, Promise, JSON, URLSearchParams, Math, String, Number, Boolean, Array, Object, RegExp,
    Date: Object.assign(function () {}, { now: () => realNow() + shift }),
    location: { protocol: "https:", search: "", href: "" },
    localStorage: { getItem: () => null, setItem() {}, removeItem() {} },
    document: {
      visibilityState: "visible", title: "", cookie: "", listeners: {},
      querySelector: (sel) => elements[sel] || null,
      addEventListener(type, fn) { (this.listeners[type] ||= []).push(fn); },
    },
    turnstile: {
      render(box, options) {
        const id = widgets.length;
        widgets.push({ options, removed: false });
        setTimeout(() => options.callback("tok-" + options.cData), 0);
        return id;
      },
      remove(id) { widgets[id].removed = true; },
      reset(id) { setTimeout(() => widgets[id].options.callback("tok-reset-" + widgets[id].options.cData), 0); },
      getResponse() { return ""; },
      isExpired() { return false; },
    },
    fetch: async (url, options = {}) => {
      const body = options.body ? JSON.parse(options.body) : null;
      calls.push({ url, body });
      if (url === "/api/accounts/signup/intent") {
        if (scenario.renewFails) return json(503, { error: "unavailable" });
        return json(200, { emailField: "contact_111111111111111111", challengeCdata: "cdata1" });
      }
      if (url === "/api/accounts/signup") {
        const [status, answer] = signupAnswers.shift() || [500, {}];
        return json(status, answer);
      }
      return json(202, { ok: true });
    },
    scrollTo() {},
    addEventListener() {},
  };
  sandbox.window = sandbox;
  vm.createContext(sandbox);
  vm.runInContext(source, sandbox);
  await new Promise((resolve) => setTimeout(resolve, 5));
  shift = scenario.shiftMs || 0;
  for (const fn of elements["#signup-form"].listeners.submit) {
    await fn({ preventDefault() {} });
  }
  return {
    calls,
    widgets: widgets.map((w) => ({ cdata: w.options.cData, removed: w.removed })),
    hint: elements["#signup-hint"].textContent,
    verified: !elements["#verify-view"].hidden,
    emailName: elements["#acct-email"].name,
  };
}

const out = {};
out.refused = await run({ signupAnswers: [[403, { error: "signup_page_required" }], [201, { ok: true, nodeName: "solvingsoft" }]] });
out.stalePage = await run({ signupAnswers: [[403, { error: "turnstile_stale_page" }], [201, { ok: true, nodeName: "solvingsoft" }]] });
out.idle = await run({ shiftMs: 13 * 60 * 1000, signupAnswers: [[201, { ok: true, nodeName: "solvingsoft" }]] });
out.renewFails = await run({ renewFails: true, signupAnswers: [[403, { error: "signup_page_required" }]] });
out.twice = await run({ signupAnswers: [[403, { error: "signup_page_required" }], [403, { error: "signup_page_required" }]] });
console.log(JSON.stringify(out));
"""


@pytest.fixture(scope="module")
def page_runs():
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    result = subprocess.run(
        [node, "--input-type=module", "-", str(SIGNUP_JS)],
        input=HARNESS, capture_output=True, text=True, timeout=60,
        check=False)
    assert result.returncode == 0, result.stderr[-2000:]
    return json.loads(result.stdout.strip().splitlines()[-1])


def _signups(run):
    return [call["body"] for call in run["calls"]
            if call["url"] == "/api/accounts/signup"]


def _urls(run):
    return [call["url"] for call in run["calls"]]


@pytest.mark.parametrize("case", ["refused", "stalePage"])
def test_a_refused_page_renews_its_claim_and_resubmits_once(page_runs, case):
    run = page_runs[case]
    signups = _signups(run)
    assert len(signups) == 2
    assert signups[0]["contact_000000000000000000"] == "person@example.org"
    assert signups[0]["turnstileToken"] == "tok-cdata0"
    assert signups[1]["contact_111111111111111111"] == "person@example.org"
    assert "contact_000000000000000000" not in signups[1]
    assert signups[1]["turnstileToken"] == "tok-cdata1"
    assert _urls(run).count("/api/accounts/signup/intent") == 1
    assert run["widgets"] == [
        {"cdata": "cdata0", "removed": True},
        {"cdata": "cdata1", "removed": False},
    ]
    assert run["verified"] is True


def test_an_idle_page_renews_before_submitting(page_runs):
    run = page_runs["idle"]
    assert _urls(run).index("/api/accounts/signup/intent") < _urls(
        run).index("/api/accounts/signup")
    signups = _signups(run)
    assert len(signups) == 1
    assert signups[0]["contact_111111111111111111"] == "person@example.org"
    assert signups[0]["turnstileToken"] == "tok-cdata1"
    assert run["verified"] is True


def test_a_failed_renewal_is_reported_and_not_retried(page_runs):
    run = page_runs["renewFails"]
    assert len(_signups(run)) == 1
    problems = [call["body"]["problem"] for call in run["calls"]
                if call["url"] == "/api/accounts/signup/problem"]
    assert problems == ["signup_page_renew_failed"]
    assert "refresh the signup page" in run["hint"]
    assert run["verified"] is False


def test_a_second_refusal_is_shown_instead_of_looping(page_runs):
    run = page_runs["twice"]
    assert len(_signups(run)) == 2
    assert _urls(run).count("/api/accounts/signup/intent") == 1
    assert "Reload /signup" in run["hint"]
    assert run["verified"] is False
