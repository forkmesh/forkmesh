#!/usr/bin/env python3

import ast
import asyncio
import hashlib
import importlib.util
import json
import re
import shutil
import subprocess
from pathlib import Path
from types import SimpleNamespace

import pytest


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
ENTRY_TEXT = ENTRY.read_text(encoding="utf-8")
SIGNUP_JS_PATH = ROOT / "public" / "signup.js"
SIGNUP_JS = SIGNUP_JS_PATH.read_text(encoding="utf-8")
SIGNUP_HTML = (ROOT / "public" / "signup.html").read_text(encoding="utf-8")
SPEC = importlib.util.spec_from_file_location(
    "turnstile_backup_under_test", ROOT / "src" / "turnstile.py")
T = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(T)

CDATA = "0123456789abcdef0123456789abcdef"
ENV = SimpleNamespace(FORKMESH_PLATFORM="vps")


def _solve(cdata, bits=T.BACKUP_BITS):
    nonce = 0
    while True:
        digest = hashlib.sha256(
            (T.BACKUP_PREFIX + cdata + ":" + str(nonce)).encode()).digest()
        if T._leading_zero_bits(digest) >= bits:
            return str(nonce)
        nonce += 1


@pytest.fixture(scope="module")
def nonce():
    return _solve(CDATA)


def test_a_solved_proof_passes_even_with_turnstile_unconfigured(nonce):
    assert T.config(ENV)["mode"] == "unavailable"
    assert T.verify_backup(ENV, {"nonce": nonce}, CDATA) == ("", "")


def test_a_proof_is_bound_to_its_page_claim(nonce):
    other = "f" * 32
    assert T.verify_backup(ENV, {"nonce": nonce}, other)[0] == T.FAILED


def test_malformed_or_short_proofs_are_refused():
    assert T.verify_backup(ENV, None, CDATA) == (T.REQUIRED, "backup_missing")
    for bad in ("", "12a", "1" * 17, 7):
        assert T.verify_backup(ENV, {"nonce": bad}, CDATA) == (
            T.FAILED, "backup_nonce_invalid")
    assert T.verify_backup(ENV, {"nonce": "1"}, "") == (
        T.FAILED, "cdata_invalid")
    weak = next(str(n) for n in range(1000) if T._leading_zero_bits(
        hashlib.sha256((T.BACKUP_PREFIX + CDATA + ":" + str(n)).encode())
        .digest()) < T.BACKUP_BITS)
    assert T.verify_backup(ENV, {"nonce": weak}, CDATA) == (
        T.FAILED, "backup_proof_short")


def test_operators_can_switch_the_backup_check_off(nonce):
    for value in ("0", "off", "false", "no"):
        env = SimpleNamespace(SIGNUP_BACKUP_CHECK=value)
        assert T.verify_backup(env, {"nonce": nonce}, CDATA) == (
            T.REQUIRED, "backup_disabled")


def test_leading_zero_bits_counts_across_bytes():
    assert T._leading_zero_bits(b"\x80") == 0
    assert T._leading_zero_bits(b"\x01") == 7
    assert T._leading_zero_bits(b"\x00\x00\x3f") == 18
    assert T._leading_zero_bits(b"\x00\x00") == 16


def test_the_reason_is_reduced_to_a_short_slug():
    assert T.backup_reason({"reason": "e200100"}) == "e200100"
    assert T.backup_reason({"reason": "<b>X Y</b>" * 9}) == "bxybbxybbxybbxybbxybbxyb"
    assert T.backup_reason(None) == ""


def test_page_and_server_agree_on_the_puzzle():
    assert 'const BACKUP_PREFIX = "%s";' % T.BACKUP_PREFIX in SIGNUP_JS
    assert "const BACKUP_BITS = %d;" % T.BACKUP_BITS in SIGNUP_JS
    assert T.BACKUP_FIELD == "backupProof"
    assert "backupProof: { nonce: backupProof.nonce, reason: backupReason }" in (
        SIGNUP_JS)
    assert ".signup-challenge[hidden]" in SIGNUP_HTML


def _load_gate(limits):
    wanted = {"_signup_backup_gate"}
    consts = {"SIGNUP_BACKUP_MAX_PER_IP", "SIGNUP_BACKUP_MAX_PER_WINDOW",
              "SIGNUP_BACKUP_RATE_KEY", "SIGNUP_PATH"}
    selected = [
        node for node in ast.parse(ENTRY_TEXT, filename=str(ENTRY)).body
        if (isinstance(node, ast.AsyncFunctionDef) and node.name in wanted)
        or (isinstance(node, ast.Assign) and {
            target.id for target in node.targets
            if isinstance(target, ast.Name)} & consts)
    ]
    checked = []
    logged = []

    async def signup_rate_check(_env, key, limit):
        checked.append((key, limit))
        if limits.get(key):
            return SimpleNamespace(status=429, data={"error": "rate_limited"})
        return None

    async def blind_index(_env, value):
        return "bi:" + value

    async def log_blocked_signup(_env, _request, path, status, reason):
        logged.append((path, status, reason))

    namespace = {
        "_turnstile": T,
        "_transient_client_address": lambda _request: "198.51.100.7",
        "blind_index": blind_index,
        "signup_rate_check": signup_rate_check,
        "log_blocked_signup": log_blocked_signup,
        "json_response": lambda data, status=200, cache_control=None:
            SimpleNamespace(status=status, data=data),
    }
    module = ast.fix_missing_locations(ast.Module(selected, type_ignores=[]))
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace, checked, logged


def test_the_gate_spends_the_per_ip_budget_then_the_global_one(nonce):
    ns, checked, logged = _load_gate({})
    outcome = {}
    data = {"backupProof": {"nonce": nonce, "reason": "e200100"}}
    assert asyncio.run(ns["_signup_backup_gate"](
        ENV, object(), data, CDATA, outcome)) is None
    assert outcome == {}
    assert checked == [
        ("bi:signup-backup-v1\n198.51.100.7", ns["SIGNUP_BACKUP_MAX_PER_IP"]),
        ("signup-backup-v1", ns["SIGNUP_BACKUP_MAX_PER_WINDOW"]),
    ]
    assert ns["SIGNUP_BACKUP_MAX_PER_IP"] < ns["SIGNUP_BACKUP_MAX_PER_WINDOW"]
    assert logged == []


def test_an_exhausted_budget_is_a_logged_rate_limit(nonce):
    ns, checked, logged = _load_gate({"signup-backup-v1": True})
    outcome = {}
    response = asyncio.run(ns["_signup_backup_gate"](
        ENV, object(), {"backupProof": {"nonce": nonce}}, CDATA, outcome))
    assert response.status == 429
    assert outcome == {"code": "rate_limited", "detail": "backup_check"}
    assert logged == [("/api/accounts/signup", 429, "rate_limited")]


def test_a_bad_proof_is_refused_before_any_budget_is_spent():
    ns, checked, _logged = _load_gate({})
    outcome = {}
    response = asyncio.run(ns["_signup_backup_gate"](
        ENV, object(), {"backupProof": {"nonce": "x"}}, CDATA, outcome))
    assert response.status == 403
    assert response.data["error"] == T.FAILED
    assert outcome == {"code": T.FAILED, "detail": "backup_nonce_invalid"}
    assert checked == []


def test_the_signup_handler_takes_the_backup_gate_before_any_lookup():
    signup = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _account_signup(env, request):"):
        ENTRY_TEXT.index("async def _account_password_record")]
    gate = signup.index("_signup_backup_gate(")
    assert signup.index("_account_signup_intent_available") < gate
    assert gate < signup.index("_turnstile_gate(")
    for later in ("_admin_signup_referral", "_org_row", "_account_row",
                  "_save_account"):
        assert gate < signup.index(later), later
    assert 'signup_meta["challenge"] = "backup:"' in signup
    assert '"challenge_backup_failed",' in ENTRY_TEXT


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
    "#signup-turnstile": element("signup-turnstile", { dataset: { sitekey: "k", action: "signup", cdata: "a".repeat(32) } }),
    "#signup-view": element("signup-view"),
    "#verify-view": element("verify-view", { hidden: true }),
    "#recap-name": element("recap-name"),
    "#recap-email": element("recap-email"),
    "#mail-links": element("mail-links"),
  };
  const calls = [];
  const widgets = [];
  const signupAnswers = scenario.signupAnswers.slice();
  const json = (status, body) => ({ ok: status >= 200 && status < 300, status, json: async () => body });
  const turnstile = {
    render(box, options) {
      const id = widgets.length;
      widgets.push({ options, removed: false });
      if (scenario.widgetError) {
        for (let i = 0; i < (scenario.errorRepeats || 1); i += 1) {
          setTimeout(() => options["error-callback"](scenario.widgetError), 0);
        }
      }
      else setTimeout(() => options.callback("tok-" + options.cData), 0);
      return id;
    },
    remove(id) { widgets[id].removed = true; },
    reset() {},
    getResponse() { return ""; },
    isExpired() { return false; },
  };
  const sandbox = {
    console,
    setTimeout, clearTimeout, Promise, JSON, URLSearchParams, Math, String, Number, Boolean, Array, Object, RegExp,
    Date,
    location: { protocol: "https:", search: "", href: "" },
    localStorage: { getItem: () => null, setItem() {}, removeItem() {} },
    document: {
      visibilityState: "visible", title: "", cookie: "", listeners: {},
      querySelector: (sel) => elements[sel] || null,
      addEventListener(type, fn) { (this.listeners[type] ||= []).push(fn); },
    },
    fetch: async (url, options = {}) => {
      const body = options.body ? JSON.parse(options.body) : null;
      calls.push({ url, body });
      if (url === "/api/accounts/signup/intent") {
        return json(200, { emailField: "contact_111111111111111111", challengeCdata: "b".repeat(32) });
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
  if (!scenario.noTurnstile) sandbox.turnstile = turnstile;
  sandbox.window = sandbox;
  vm.createContext(sandbox);
  vm.runInContext(source, sandbox);
  await new Promise((resolve) => setTimeout(resolve, 5));
  for (let i = 0; i < (scenario.submits || 1); i += 1) {
    for (const fn of elements["#signup-form"].listeners.submit) {
      await fn({ preventDefault() {} });
    }
  }
  return {
    calls,
    widgets: widgets.map((w) => ({ cdata: w.options.cData, removed: w.removed })),
    boxHidden: elements["#signup-turnstile"].hidden,
    hint: elements["#signup-hint"].textContent,
    verified: !elements["#verify-view"].hidden,
  };
}

const ok = [201, { ok: true, nodeName: "solvingsoft" }];
const out = {};
out.clock = await run({ widgetError: "200100", signupAnswers: [ok] });
out.retriesExhausted = await run({ widgetError: "300030", errorRepeats: 3, signupAnswers: [ok] });
out.noScript = await run({ noTurnstile: true, signupAnswers: [ok] });
out.serverDown = await run({ signupAnswers: [[503, { error: "turnstile_unavailable" }], ok] });
out.renewed = await run({ widgetError: "200100", signupAnswers: [[403, { error: "signup_page_required" }], ok] });
out.healthy = await run({ signupAnswers: [ok] });
console.log(JSON.stringify(out));
"""


@pytest.fixture(scope="module")
def page_runs():
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    result = subprocess.run(
        [node, "--input-type=module", "-", str(SIGNUP_JS_PATH)],
        input=HARNESS, capture_output=True, text=True, timeout=120,
        check=False)
    assert result.returncode == 0, result.stderr[-2000:]
    return json.loads(result.stdout.strip().splitlines()[-1])


def _signups(run):
    return [call["body"] for call in run["calls"]
            if call["url"] == "/api/accounts/signup"]


def _problems(run):
    return [call["body"]["problem"] for call in run["calls"]
            if call["url"] == "/api/accounts/signup/problem"]


def _assert_backup_signup(body, cdata, reason):
    assert body["turnstileToken"] == ""
    assert body["backupProof"]["reason"] == reason
    assert T.verify_backup(ENV, body["backupProof"], cdata) == ("", "")


@pytest.mark.parametrize("case,reason", [
    ("clock", "e200100"),
    ("retriesExhausted", "e300030"),
    ("noScript", "unavailable"),
])
def test_a_visitor_whose_widget_fails_still_signs_up(page_runs, case, reason):
    run = page_runs[case]
    signups = _signups(run)
    assert len(signups) == 1
    _assert_backup_signup(signups[0], "a" * 32, reason)
    assert _problems(run) == []
    assert run["verified"] is True
    assert run["boxHidden"] is True


def test_the_widget_stops_once_the_backup_check_takes_over(page_runs):
    assert all(widget["removed"] for widget in page_runs["clock"]["widgets"])
    assert len(page_runs["retriesExhausted"]["widgets"]) == 1


def test_a_server_without_turnstile_gets_one_backup_resubmission(page_runs):
    run = page_runs["serverDown"]
    signups = _signups(run)
    assert len(signups) == 2
    assert signups[0]["turnstileToken"] == "tok-" + "a" * 32
    assert "backupProof" not in signups[0]
    _assert_backup_signup(signups[1], "a" * 32, "server_unavailable")
    assert run["verified"] is True


def test_a_renewed_claim_gets_a_fresh_proof_and_no_new_widget(page_runs):
    run = page_runs["renewed"]
    signups = _signups(run)
    assert len(signups) == 2
    _assert_backup_signup(signups[0], "a" * 32, "e200100")
    _assert_backup_signup(signups[1], "b" * 32, "e200100")
    assert signups[1]["contact_111111111111111111"] == "person@example.org"
    assert len(run["widgets"]) == 1
    assert run["verified"] is True


def test_a_healthy_widget_never_uses_the_backup_check(page_runs):
    run = page_runs["healthy"]
    signups = _signups(run)
    assert len(signups) == 1
    assert signups[0]["turnstileToken"] == "tok-" + "a" * 32
    assert "backupProof" not in signups[0]
    assert run["boxHidden"] is False


def test_a_missing_token_is_reported_once_then_the_backup_check_runs():
    submit = SIGNUP_JS[SIGNUP_JS.index("async function createAccount"):]
    missing = submit[submit.index("challengeMissed < 1"):]
    missing = missing[:missing.index("if (!backupReason) {")]
    assert 'reportProblem("challenge_token_missing", challengeErrorCode)' in (
        missing)
    assert 'useBackupCheck("token_missing")' in missing
    assert re.search(r"CHALLENGE_LOAD_WAIT_MS = \d+ \* 1000", SIGNUP_JS)
