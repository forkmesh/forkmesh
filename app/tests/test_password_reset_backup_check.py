#!/usr/bin/env python3

import asyncio
import hashlib
import importlib.util
import json
import shutil
import subprocess
from pathlib import Path
from types import SimpleNamespace

import pytest


ROOT = Path(__file__).resolve().parents[1]
ENTRY_TEXT = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
RESET_PATH = ROOT / "src" / "password_reset.py"
RESET_TEXT = RESET_PATH.read_text(encoding="utf-8")
FORGOT_JS_PATH = ROOT / "public" / "forgot-password.js"
FORGOT_JS = FORGOT_JS_PATH.read_text(encoding="utf-8")
FORGOT_HTML = (ROOT / "public" / "forgot-password.html").read_text(
    encoding="utf-8")


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


T = _load("turnstile_reset_backup_under_test", ROOT / "src" / "turnstile.py")
NOW = 1_800_000_000_000
ENV = SimpleNamespace(FORKMESH_PLATFORM="vps")


def _solve(cdata):
    nonce = 0
    while True:
        digest = hashlib.sha256(
            (T.BACKUP_PREFIX + cdata + ":" + str(nonce)).encode()).digest()
        if T._leading_zero_bits(digest) >= T.BACKUP_BITS:
            return str(nonce)
        nonce += 1


def _constant(name):
    for line in ENTRY_TEXT.splitlines():
        if line.startswith(name + " = "):
            return eval(line.split(" = ", 1)[1], {})
    raise AssertionError(name)


class _Clock:
    def __init__(self):
        self.ms = NOW

    def now(self):
        return self.ms


def _module(limits=None, client_ip="198.51.100.7"):
    limits = limits or {}
    clock = _Clock()
    checked = []

    async def blind_index(_env, value):
        return hashlib.sha256(("key\n" + value).encode()).hexdigest()

    async def signup_rate_check(_env, key, limit):
        checked.append((key, limit))
        if limits.get(key):
            return SimpleNamespace(status=429, data={"error": "rate_limited"})
        return None

    module = _load("password_reset_backup_under_test", RESET_PATH)
    module._bind_runtime({
        "_turnstile": T,
        "Date": clock,
        "blind_index": blind_index,
        "signup_rate_check": signup_rate_check,
        "_account_session_client_ip": lambda _request: client_ip,
        "ipaddress": __import__("ipaddress"),
        "json_response": lambda data, status=200, cache_control=None:
            SimpleNamespace(status=status, data=data),
        **{name: _constant(name) for name in (
            "PASSWORD_RESET_BACKUP_RATE_KEY",
            "PASSWORD_RESET_BACKUP_MAX_PER_IP",
            "PASSWORD_RESET_BACKUP_MAX_PER_WINDOW",
            "PASSWORD_RESET_BACKUP_CDATA_TTL_MS",
            "PASSWORD_RESET_REQUESTER_V6_PREFIX")},
    })
    return module, clock, checked


def _challenge(module):
    response = asyncio.run(module._account_forgot_password_challenge(
        ENV, object()))
    assert response.status == 200
    return response.data


@pytest.fixture(scope="module")
def issued():
    module, _clock, _checked = _module()
    cdata = _challenge(module)["cdata"]
    return cdata, _solve(cdata)


def _gate(module, cdata, nonce):
    data = {"backupProof": {"nonce": nonce, "reason": "e200100",
                            "cdata": cdata}}
    return asyncio.run(module._password_reset_backup_gate(
        ENV, object(), data))


def test_the_challenge_answers_the_site_key_and_a_signed_page_value():
    module, _clock, _checked = _module()
    body = _challenge(module)
    assert set(body) == {"siteKey", "enabled", "cdata"}
    assert body["enabled"] is False
    stamp, salt, signature = body["cdata"].split("-")
    assert int(stamp, 16) == NOW // 1000
    assert len(salt) == 16 and len(signature) == 32
    assert T._CDATA_RE.fullmatch(body["cdata"])
    assert _challenge(module)["cdata"] != body["cdata"]


def test_a_solved_proof_passes_and_spends_the_ip_then_platform_budget(issued):
    module, _clock, checked = _module()
    cdata, nonce = issued
    assert _gate(module, cdata, nonce) is None
    key = _constant("PASSWORD_RESET_BACKUP_RATE_KEY")
    assert checked[1] == (key, _constant(
        "PASSWORD_RESET_BACKUP_MAX_PER_WINDOW"))
    assert checked[0][0] != key and checked[0][1] == _constant(
        "PASSWORD_RESET_BACKUP_MAX_PER_IP")


def test_ipv6_requesters_share_one_budget_per_64():
    keys = []
    for address in ("2001:db8::1", "2001:db8::2"):
        module, _clock, checked = _module(client_ip=address)
        cdata = _challenge(module)["cdata"]
        assert _gate(module, cdata, _solve(cdata)) is None
        keys.append(checked[0][0])
    assert keys[0] == keys[1]


def test_a_forged_or_expired_page_value_is_refused_as_expired(issued):
    module, clock, checked = _module()
    cdata, nonce = issued
    stamp, salt, signature = cdata.split("-")
    forged = "-".join((stamp, salt, ("0" if signature[0] != "0" else "1")
                       + signature[1:]))
    response = _gate(module, forged, _solve(forged))
    assert response.status == 403 and response.data["error"] == T.EXPIRED
    clock.ms = NOW + _constant("PASSWORD_RESET_BACKUP_CDATA_TTL_MS") + 1000
    response = _gate(module, cdata, nonce)
    assert response.data["error"] == T.EXPIRED
    assert checked == []


def test_a_bad_proof_is_refused_before_any_budget_is_spent(issued):
    module, _clock, checked = _module()
    cdata, _nonce = issued
    response = _gate(module, cdata, "x")
    assert response.status == 403 and response.data["error"] == T.FAILED
    assert checked == []


def test_an_exhausted_budget_is_a_rate_limit(issued):
    module, _clock, _checked = _module(
        {_constant("PASSWORD_RESET_BACKUP_RATE_KEY"): True})
    cdata, nonce = issued
    assert _gate(module, cdata, nonce).status == 429


def test_the_backup_switch_also_turns_off_the_reset_backup(issued):
    module, _clock, _checked = _module()
    cdata, nonce = issued
    data = {"backupProof": {"nonce": nonce, "cdata": cdata}}
    response = asyncio.run(module._password_reset_backup_gate(
        SimpleNamespace(SIGNUP_BACKUP_CHECK="off"), object(), data))
    assert response.data["error"] == T.REQUIRED


def test_the_handler_takes_a_gate_before_any_lookup_and_routes_the_challenge():
    start = RESET_TEXT.index("async def _account_forgot_password(env, request)")
    reset = RESET_TEXT[start:RESET_TEXT.index("\n\n\n", start)]
    gate = reset.index("_password_reset_backup_gate(")
    assert gate < reset.index("_turnstile_gate(") < reset.index("blind_index")
    assert '"/api/accounts/forgot-password/challenge"' in ENTRY_TEXT
    assert ".forgot-challenge[hidden]" in FORGOT_HTML
    assert 'const BACKUP_PREFIX = "%s";' % T.BACKUP_PREFIX in FORGOT_JS
    assert "const BACKUP_BITS = %d;" % T.BACKUP_BITS in FORGOT_JS


HARNESS = r"""
import { readFileSync } from "node:fs";
import vm from "node:vm";

const source = readFileSync(process.argv[2], "utf8");

function element(id, extra = {}) {
  const listeners = {};
  return Object.assign({
    id, value: "", textContent: "", className: "", hidden: false, disabled: false, listeners,
    addEventListener(type, fn) { (listeners[type] ||= []).push(fn); },
  }, extra);
}

const later = (fn, ms) => (ms >= 10000 ? 0 : setTimeout(fn, ms));

async function run(scenario) {
  const elements = {
    "#forgot-hint": element("forgot-hint"),
    "#forgot-btn": element("forgot-btn"),
    "#identifier": element("identifier", { value: "person@example.org" }),
    "#forgot-turnstile": element("forgot-turnstile"),
  };
  const calls = [];
  const widgets = [];
  const answers = scenario.answers.slice();
  let cdataCount = 0;
  const json = (status, body) => ({ ok: status >= 200 && status < 300, status, json: async () => body });
  const turnstile = {
    render(box, options) {
      const id = widgets.length;
      widgets.push({ options, removed: false, resets: 0 });
      if (scenario.widgetError) {
        for (let i = 0; i < (scenario.errorRepeats || 1); i += 1) {
          setTimeout(() => options["error-callback"](scenario.widgetError), 0);
        }
        if (scenario.tokenDelay) setTimeout(() => options.callback("tok-" + id), scenario.tokenDelay);
      } else {
        setTimeout(() => options.callback("tok-" + id), scenario.tokenDelay || 0);
      }
      return id;
    },
    remove(id) { widgets[id].removed = true; },
    reset(id) {
      widgets[id].resets += 1;
      setTimeout(() => widgets[id].options.callback("tok-" + id + "-" + widgets[id].resets), 0);
    },
    getResponse() { return ""; },
    isExpired() { return false; },
  };
  const sandbox = {
    console, Promise, JSON, Math, String, Number, Boolean, Array, Object, RegExp, Date, Uint32Array,
    setTimeout: later, clearTimeout,
    document: {
      visibilityState: "visible",
      querySelector: (sel) => elements[sel] || null,
      addEventListener() {},
    },
    addEventListener() {},
    fetch: async (url, options = {}) => {
      const body = options.body ? JSON.parse(options.body) : null;
      calls.push({ url, body });
      if (url === "/api/accounts/forgot-password/challenge") {
        cdataCount += 1;
        return json(200, { siteKey: scenario.noSiteKey ? "" : "k", enabled: true, cdata: "c" + cdataCount });
      }
      const [status, answer] = answers.shift() || [500, {}];
      return json(status, answer);
    },
  };
  if (!scenario.noTurnstile) sandbox.turnstile = turnstile;
  sandbox.window = sandbox;
  vm.createContext(sandbox);
  vm.runInContext(source, sandbox);
  await new Promise((resolve) => setTimeout(resolve, scenario.submitAfter ?? 5));
  for (const fn of elements["#forgot-btn"].listeners.click) await fn();
  return {
    posts: calls.filter((c) => c.url === "/api/accounts/forgot-password").map((c) => c.body),
    configs: cdataCount,
    widgets: widgets.length,
    boxHidden: elements["#forgot-turnstile"].hidden,
    hint: elements["#forgot-hint"].textContent,
    button: elements["#forgot-btn"].textContent,
  };
}

const ok = [200, { ok: true }];
const out = {};
out.healthy = await run({ answers: [ok] });
out.slowToken = await run({ tokenDelay: 200, submitAfter: 1, answers: [ok] });
out.clock = await run({ widgetError: "200100", answers: [ok] });
out.retried = await run({ widgetError: "300030", errorRepeats: 2, tokenDelay: 50, answers: [ok] });
out.exhausted = await run({ widgetError: "600010", errorRepeats: 3, answers: [ok] });
out.noScript = await run({ noTurnstile: true, answers: [ok] });
out.noSiteKey = await run({ noSiteKey: true, answers: [ok] });
out.serverDown = await run({ answers: [[503, { error: "turnstile_unavailable" }], ok] });
out.staleCdata = await run({ widgetError: "200100", answers: [[403, { error: "turnstile_expired" }], ok] });
out.rateLimited = await run({ widgetError: "200100", answers: [[429, { error: "rate_limited" }]] });
console.log(JSON.stringify(out));
"""


@pytest.fixture(scope="module")
def page_runs():
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    result = subprocess.run(
        [node, "--input-type=module", "-", str(FORGOT_JS_PATH)],
        input=HARNESS, capture_output=True, text=True, timeout=120,
        check=False)
    assert result.returncode == 0, result.stderr[-2000:]
    return json.loads(result.stdout.strip().splitlines()[-1])


SENT = "If that account exists, a reset link is on its way. Check your email."


def _assert_backup(body, cdata, reason):
    assert "turnstileToken" not in body
    proof = body["backupProof"]
    assert proof["reason"] == reason and proof["cdata"] == cdata
    assert T.verify_backup(ENV, proof, cdata) == ("", "")


def test_a_healthy_widget_sends_its_token(page_runs):
    run = page_runs["healthy"]
    assert run["posts"] == [{"identifier": "person@example.org",
                             "turnstileToken": "tok-0"}]
    assert run["hint"] == SENT and run["button"] == "Send reset link"


def test_a_submit_before_the_widget_finishes_waits_for_its_token(page_runs):
    run = page_runs["slowToken"]
    assert [p["turnstileToken"] for p in run["posts"]] == ["tok-0"]
    assert run["hint"] == SENT


def test_retryable_widget_errors_keep_the_widget(page_runs):
    run = page_runs["retried"]
    assert run["boxHidden"] is False
    assert run["posts"][0]["turnstileToken"] == "tok-0"


@pytest.mark.parametrize("case,reason", [
    ("clock", "e200100"),
    ("exhausted", "e600010"),
    ("noScript", "unavailable"),
    ("noSiteKey", "unavailable"),
])
def test_a_widget_that_cannot_answer_falls_back_to_the_backup(
        page_runs, case, reason):
    run = page_runs[case]
    assert len(run["posts"]) == 1
    _assert_backup(run["posts"][0], "c1", reason)
    assert run["boxHidden"] is True
    assert run["hint"] == SENT


def test_an_offline_verifier_resends_once_with_the_backup(page_runs):
    run = page_runs["serverDown"]
    assert run["posts"][0]["turnstileToken"] == "tok-0"
    _assert_backup(run["posts"][1], "c1", "server_unavailable")
    assert run["hint"] == SENT


def test_an_expired_page_value_is_refetched_and_resent_once(page_runs):
    run = page_runs["staleCdata"]
    assert run["configs"] == 2
    _assert_backup(run["posts"][0], "c1", "e200100")
    _assert_backup(run["posts"][1], "c2", "e200100")
    assert run["hint"] == SENT


def test_a_rate_limit_is_explained(page_runs):
    run = page_runs["rateLimited"]
    assert len(run["posts"]) == 1
    assert run["hint"] == "Too many reset requests right now. Try again later."
