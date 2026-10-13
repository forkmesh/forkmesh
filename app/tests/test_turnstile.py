"""Cloudflare Turnstile gate for signup and password reset (turnstile.py).

No network: siteverify is a fake fetcher throughout.
"""

import asyncio
import importlib.util
import json
import re
from datetime import datetime, timezone
from pathlib import Path
from types import SimpleNamespace

import tomllib

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
SPEC = importlib.util.spec_from_file_location(
    "turnstile_under_test", ROOT / "src" / "turnstile.py")
T = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(T)

ENTRY_TEXT = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
RUNTIME_TEXT = (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
RESET_TEXT = (ROOT / "src" / "password_reset.py").read_text(encoding="utf-8")
SIGNUP_HTML = (ROOT / "public" / "signup.html").read_text(encoding="utf-8")
SIGNUP_JS = (ROOT / "public" / "signup.js").read_text(encoding="utf-8")
FORGOT_HTML = (ROOT / "public" / "forgot-password.html").read_text(
    encoding="utf-8")
FORGOT_JS = (ROOT / "public" / "forgot-password.js").read_text(
    encoding="utf-8")

NOW = 1_800_000_000_000
LIVE_SECRET = "0x4AAAAAAA-live-secret-for-tests"
SITE_KEY = "0x4AAAAAAFB4BWGb6Sgx78UR"


def _iso(ms):
    return datetime.fromtimestamp(ms / 1000, timezone.utc).strftime(
        "%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"


def live_env(**extra):
    values = {"FORKMESH_PLATFORM": "vps", "TURNSTILE_SITE_KEY": SITE_KEY,
              "TURNSTILE_SECRET_KEY": LIVE_SECRET}
    values.update(extra)
    return SimpleNamespace(**values)


class FakeResponse:
    def __init__(self, status, payload):
        self.status = status
        self._payload = payload

    async def text(self):
        if isinstance(self._payload, str):
            return self._payload
        return json.dumps(self._payload)


class FakeSiteverify:
    """Records calls; answers ``payload`` (or raises ``error``)."""

    def __init__(self, payload=None, status=200, error=None):
        self.calls = []
        self.status = status
        self.error = error
        self.payload = payload if payload is not None else {
            "success": True, "challenge_ts": _iso(NOW - 20_000),
            "hostname": "app.forkmesh.com", "action": "signup",
            "cdata": "", "error-codes": []}

    async def __call__(self, url, init, timeout_seconds):
        self.calls.append((url, init, timeout_seconds))
        if self.error is not None:
            raise self.error
        return FakeResponse(self.status, self.payload)


def verify(env, token, action="signup", fetcher=None, **kwargs):
    T._SEEN.clear()
    return asyncio.run(T.verify(env, token, action, fetcher=fetcher,
                                now_ms=NOW, **kwargs))


# --------------------------------------------------------------- config


def test_missing_secret_fails_closed_on_vps_and_uses_test_keys_elsewhere():
    vps = SimpleNamespace(FORKMESH_PLATFORM="vps", TURNSTILE_SITE_KEY=SITE_KEY)
    assert T.config(vps)["mode"] == "unavailable"
    assert verify(vps, "token")[0] == T.UNAVAILABLE
    assert T.STATUS[T.UNAVAILABLE] == 503

    for dev in (SimpleNamespace(FORKMESH_PLATFORM="dev"), SimpleNamespace()):
        settings = T.config(dev)
        assert settings["mode"] == "test"
        assert settings["siteKey"] == T.TEST_SITE_KEY == "1x00000000000000000000AA"
        assert settings["secret"] == T.TEST_SECRET_KEY
        assert T.TEST_SECRET_KEY == "1x0000000000000000000000000000000AA"


def test_test_pair_is_refused_on_a_vps_unless_explicitly_enabled():
    env = SimpleNamespace(FORKMESH_PLATFORM="vps",
                          TURNSTILE_SITE_KEY=T.TEST_SITE_KEY,
                          TURNSTILE_SECRET_KEY=T.TEST_SECRET_KEY)
    assert T.config(env)["mode"] == "unavailable"
    env.TURNSTILE_TEST_MODE = "1"
    assert T.config(env)["mode"] == "test"
    assert verify(env, "XXXX.DUMMY.TOKEN.XXXX") == ("", "")


def test_live_secret_with_test_site_key_is_a_misconfiguration():
    env = live_env(TURNSTILE_SITE_KEY=T.TEST_SITE_KEY)
    assert T.config(env)["mode"] == "unavailable"
    assert T.config(live_env())["mode"] == "live"


def test_site_key_is_the_public_wrangler_var():
    manifest = tomllib.loads((ROOT / "wrangler.toml").read_text("utf-8"))
    assert manifest["vars"]["TURNSTILE_SITE_KEY"] == SITE_KEY
    assert "TURNSTILE_SECRET_KEY" not in manifest["vars"]
    fleet = tomllib.loads((REPO / "deploy" / "fleet.toml").read_text("utf-8"))
    assert "TURNSTILE_SECRET_KEY" in fleet["app"]["optional_secrets"]


# --------------------------------------------------------------- verify


def test_missing_or_oversized_token_is_required_and_never_sent():
    fake = FakeSiteverify()
    for token in ("", "   ", None, 42, "x" * (T.TOKEN_MAX_LENGTH + 1)):
        assert verify(live_env(), token, fetcher=fake)[0] == T.REQUIRED
    assert fake.calls == []
    assert T.STATUS[T.REQUIRED] == 403


def test_live_success_posts_secret_token_and_client_ip_with_a_timeout():
    fake = FakeSiteverify()
    assert verify(live_env(), "tok-1", fetcher=fake,
                  remoteip="203.0.113.9") == ("", "")
    url, init, timeout = fake.calls[0]
    assert url == "https://challenges.cloudflare.com/turnstile/v0/siteverify"
    assert init["method"] == "POST"
    body = json.loads(init["body"])
    assert body == {"secret": LIVE_SECRET, "response": "tok-1",
                    "remoteip": "203.0.113.9"}
    assert 0 < timeout <= 10


def test_siteverify_failure_timeout_and_garbage_fail_closed():
    cases = [
        FakeSiteverify(error=TimeoutError("aborted")),
        FakeSiteverify(error=TypeError("fetch blocked")),
        FakeSiteverify(status=502),
        FakeSiteverify(payload="<html>not json"),
        FakeSiteverify(payload=["not", "a", "dict"]),
        FakeSiteverify(payload={"success": False,
                                "error-codes": ["invalid-input-secret"]}),
        FakeSiteverify(payload={"success": False,
                                "error-codes": ["internal-error"]}),
    ]
    for fake in cases:
        assert verify(live_env(), "tok", fetcher=fake)[0] == T.UNAVAILABLE


def test_rejected_token_is_a_403_failure():
    fake = FakeSiteverify(payload={
        "success": False, "error-codes": ["invalid-input-response"]})
    assert verify(live_env(), "tok", fetcher=fake)[0] == T.FAILED
    # A truthy non-boolean "success" is not success.
    fake = FakeSiteverify(payload={"success": "true"})
    assert verify(live_env(), "tok", fetcher=fake)[0] == T.FAILED


def test_hostname_must_be_one_of_the_widget_domains():
    for hostname, ok in (("app.forkmesh.com", True),
                         ("forkmesh.com", True),
                         ("APP.FORKMESH.COM.", True),
                         ("app.next.forkmesh.com", False),
                         ("evil.example", False),
                         ("", False)):
        fake = FakeSiteverify()
        fake.payload["hostname"] = hostname
        code, detail = verify(live_env(), "tok", fetcher=fake)
        assert (code == "") is ok, (hostname, code, detail)
        if not ok:
            assert (code, detail) == (T.FAILED, "hostname_mismatch")
    fake = FakeSiteverify()
    fake.payload["hostname"] = "staging.example"
    env = live_env(TURNSTILE_HOSTNAMES="staging.example")
    assert verify(env, "tok", fetcher=fake) == ("", "")


def test_action_and_cdata_bind_the_token_to_its_form_and_page_view():
    fake = FakeSiteverify()
    assert verify(live_env(), "tok", action="password-reset",
                  fetcher=fake) == (T.FAILED, "action_mismatch")
    cdata = T.signup_cdata("claim-a")
    assert re.fullmatch(r"[0-9a-f]{32}", cdata)
    assert cdata != T.signup_cdata("claim-b")
    fake = FakeSiteverify()
    fake.payload["cdata"] = T.signup_cdata("claim-b")
    assert verify(live_env(), "tok", fetcher=fake,
                  cdata=cdata) == (T.STALE_PAGE, "cdata_mismatch")
    fake.payload["cdata"] = cdata
    assert verify(live_env(), "tok", fetcher=fake, cdata=cdata) == ("", "")


def test_stale_future_or_missing_challenge_timestamp_is_refused():
    for stamp, code in ((_iso(NOW - T.TOKEN_MAX_AGE_MS - 1), T.EXPIRED),
                        (_iso(NOW + T.TOKEN_FUTURE_SKEW_MS + 1000), T.FAILED),
                        ("", T.FAILED), ("yesterday", T.FAILED),
                        ("2027-01-15T08:00:00", T.FAILED)):
        fake = FakeSiteverify()
        fake.payload["challenge_ts"] = stamp
        assert verify(live_env(), "tok", fetcher=fake)[0] == code, stamp


def test_reused_token_is_refused_locally_and_by_cloudflare():
    fake = FakeSiteverify()
    T._SEEN.clear()
    first = asyncio.run(T.verify(live_env(), "once", "signup",
                                 fetcher=fake, now_ms=NOW))
    again = asyncio.run(T.verify(live_env(), "once", "signup",
                                 fetcher=fake, now_ms=NOW + 1000))
    assert first == ("", "")
    assert again == (T.EXPIRED, "token_reused")
    assert len(fake.calls) == 1
    fake = FakeSiteverify(payload={"success": False,
                                   "error-codes": ["timeout-or-duplicate"]})
    assert verify(live_env(), "spent", fetcher=fake)[0] == T.EXPIRED


def test_test_secrets_are_answered_locally_like_cloudflare():
    def env(secret):
        return SimpleNamespace(FORKMESH_PLATFORM="dev",
                               TURNSTILE_SECRET_KEY=secret)
    fake = FakeSiteverify(error=AssertionError("no network in test mode"))
    assert verify(env("1x0000000000000000000000000000000AA"), "XXXX.DUMMY",
                  fetcher=fake) == ("", "")
    assert verify(env("2x0000000000000000000000000000000AA"), "XXXX.DUMMY",
                  fetcher=fake)[0] == T.FAILED
    assert verify(env("3x0000000000000000000000000000000AA"), "XXXX.DUMMY",
                  fetcher=fake)[0] == T.EXPIRED
    # Still needs a token, and still checks cdata binding shape.
    assert verify(env(T.TEST_SECRET_KEY), "", fetcher=fake)[0] == T.REQUIRED
    assert fake.calls == []


def test_refusal_names_the_web_page_for_clients_without_the_widget():
    payload = T.refusal_payload(T.REQUIRED, "/signup")
    assert payload["error"] == "turnstile_required"
    assert payload["challengeUrl"] == "/signup"
    assert "on the web" in payload["message"]


# --------------------------------------------------------------- egress


def test_host_egress_guard_allows_siteverify(monkeypatch):
    import sys
    sys.path.insert(0, str(ROOT / "host"))
    from forkmesh_host import egress
    monkeypatch.setenv("FORKMESH_PLATFORM", "vps")
    egress.check_url_host("challenges.cloudflare.com", 443, "https")
    for address in ("104.18.94.41", "104.18.95.41", "2606:4700::6812:5e29"):
        assert not egress.address_blocked(address)


# --------------------------------------------------------------- wiring


def test_signup_page_renders_the_widget_and_loads_turnstile_only_there():
    script = ('src="https://challenges.cloudflare.com/turnstile/v0/api.js'
              '?render=explicit&amp;onload=forkmeshSignupChallengeReady"')
    assert script in SIGNUP_HTML
    tag = SIGNUP_HTML[SIGNUP_HTML.index(script) - 40:
                      SIGNUP_HTML.index(script) + len(script) + 40]
    assert "async" in tag and "defer" in tag
    assert 'class="cf-turnstile' in SIGNUP_HTML
    assert 'data-sitekey="__FORKMESH_TURNSTILE_SITE_KEY__"' in SIGNUP_HTML
    assert 'data-cdata="__FORKMESH_TURNSTILE_CDATA__"' in SIGNUP_HTML
    assert 'data-action="signup"' in SIGNUP_HTML
    assert "turnstileToken: challengeToken" in SIGNUP_JS
    assert "turnstile.reset" in SIGNUP_JS
    assert "data-callback" not in SIGNUP_HTML
    assert "callback: challengeDone" in SIGNUP_JS
    assert "window.forkmeshSignupChallengeReady = renderChallenge" in SIGNUP_JS
    assert "turnstile.getResponse(widget)" in SIGNUP_JS
    assert "!widgetToken()" in SIGNUP_JS
    serve = RUNTIME_TEXT[RUNTIME_TEXT.index("async def _serve_signup_page"):
                         RUNTIME_TEXT.index("async def _git_host")]
    assert "_turnstile.site_key(self.env)" in serve
    assert "_turnstile.signup_cdata(claim_id)" in serve
    # Other account pages do not load the widget.
    for page in ("login.html", "reset-password.html", "start.html"):
        text = (ROOT / "public" / page).read_text(encoding="utf-8")
        assert "challenges.cloudflare.com" not in text, page


def test_signup_page_waits_out_turnstile_retries_before_reporting():
    handler = SIGNUP_JS[SIGNUP_JS.index('"error-callback"'):]
    handler = handler[:handler.index("\n        },")]
    assert "const CHALLENGE_RETRY_ERRORS = 3;" in SIGNUP_JS
    assert "/^[36]\\d{5}$/.test(challengeErrorCode)" in handler
    assert "challengeErrors < CHALLENGE_RETRY_ERRORS" in handler
    retry = handler[handler.index("if (retrying)"):]
    retry = retry[:retry.index("return true;")]
    assert "reportProblem" not in retry
    assert '"bad"' not in retry
    assert "if (!challengeErrorReported)" in handler
    assert 'reportProblem("challenge_token_missing", challengeErrorCode)' in (
        SIGNUP_JS)
    assert "retry" not in SIGNUP_JS[SIGNUP_JS.index("turnstile.render("):
                                   SIGNUP_JS.index("callback: challengeDone")]


def test_forgot_password_page_renders_the_widget_from_the_public_key():
    assert "challenges.cloudflare.com/turnstile/v0/api.js?render=explicit" in (
        FORGOT_HTML)
    assert 'id="forgot-turnstile"' in FORGOT_HTML
    assert 'fetch("/api/turnstile"' in FORGOT_JS
    assert 'action: "password-reset"' in FORGOT_JS
    assert "turnstileToken: token" in FORGOT_JS
    route = RUNTIME_TEXT[RUNTIME_TEXT.index('"/api/turnstile"'):]
    route = route[:route.index("\n\n")]
    assert '"siteKey"' in route and "secret" not in route.replace(
        "Never the secret", "")


def test_signup_and_reset_handlers_check_the_token_before_any_lookup():
    signup = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _account_signup(env, request):"):
        ENTRY_TEXT.index("async def _account_password_record")]
    gate = signup.index("_turnstile_gate(")
    assert signup.index("_account_signup_intent_available") < gate
    for later in ("_attribution.emit_from_request", "_admin_signup_referral",
                  "_org_row", "_account_row", "_save_account"):
        assert gate < signup.index(later), later
    assert "_turnstile.signup_cdata(signup_claim" in signup
    reset = RESET_TEXT[RESET_TEXT.index("async def _account_forgot_password"):]
    reset = reset[:reset.index("\n\n\n")]
    assert reset.index("_turnstile_gate(") < reset.index("blind_index")
    assert "ACTION_PASSWORD_RESET" in reset
    # Node registration is not gated but a fresh claim spends a per-IP budget.
    reserve = ENTRY_TEXT[ENTRY_TEXT.index("async def _account_reserve"):]
    reserve = reserve[:reserve.index("\nasync def ")]
    assert "_turnstile" not in reserve
    assert 'signup_rate_check(' in reserve and "node-reserve-v1" in reserve
    assert '"turnstile_unavailable",' in ENTRY_TEXT[
        ENTRY_TEXT.index("BLOCKING_SIGNUP_REFUSALS"):]


def test_gate_answers_json_refusals_with_the_right_status():
    """Run _turnstile_gate from entry.py against the real module."""
    import ast
    tree = ast.parse(ENTRY_TEXT)
    node = next(n for n in tree.body if isinstance(n, ast.AsyncFunctionDef)
                and n.name == "_turnstile_gate")
    logged = []

    async def log_blocked_signup(_env, _request, path, status, reason):
        logged.append((path, status, reason))

    namespace = {
        "_turnstile": T,
        "_account_session_client_ip": lambda request: "198.51.100.7",
        "log_blocked_signup": log_blocked_signup,
        "json_response": lambda payload, status=200, **kw: (status, payload),
    }
    exec(compile(ast.fix_missing_locations(ast.Module([node], [])),
                 "entry.py", "exec"), namespace)
    gate = namespace["_turnstile_gate"]
    T._SEEN.clear()
    vps_missing = SimpleNamespace(FORKMESH_PLATFORM="vps")
    status, payload = asyncio.run(gate(
        vps_missing, None, {"turnstileToken": "t"}, "signup", "/signup",
        log_path="/api/accounts/signup"))
    assert status == 503 and payload["error"] == "turnstile_unavailable"
    assert logged == [("/api/accounts/signup", 503, "turnstile_unavailable")]

    dev = SimpleNamespace(FORKMESH_PLATFORM="dev")
    status, payload = asyncio.run(gate(dev, None, {}, "signup", "/signup"))
    assert (status, payload["error"]) == (403, "turnstile_required")
    assert payload["challengeUrl"] == "/signup"
    assert asyncio.run(gate(dev, None, {"turnstileToken": "XXXX.DUMMY"},
                            "signup", "/signup")) is None

    outcome = {}
    T._SEEN.clear()
    asyncio.run(gate(dev, None, {"turnstileToken": "XXXX.DUMMY"}, "signup",
                     "/signup", cdata="pageA", outcome=outcome))
    assert outcome == {}
    stale = FakeSiteverify({"success": False,
                            "error-codes": ["invalid-input-response"]})
    namespace["_turnstile"] = SimpleNamespace(
        **{k: getattr(T, k) for k in dir(T) if not k.startswith("__")})

    async def live_verify(env, token, action, **kw):
        return await T.verify(env, token, action, fetcher=stale,
                              now_ms=NOW, **kw)

    namespace["_turnstile"].verify = live_verify
    status, payload = asyncio.run(gate(
        live_env(), None, {"turnstileToken": "tok"}, "signup", "/signup",
        outcome=outcome))
    assert (status, payload["error"]) == (403, "turnstile_failed")
    assert outcome == {"code": "turnstile_failed",
                       "detail": "siteverify:invalid-input-response"}


def test_signup_page_refreshes_a_stale_token_before_submitting():
    assert "CHALLENGE_FRESH_MS = 240 * 1000" in SIGNUP_JS
    assert T.TOKEN_MAX_AGE_MS > 240 * 1000
    submit = SIGNUP_JS[SIGNUP_JS.index("async function createAccount"):]
    assert submit.index("challengeStale()") < submit.index("!widgetToken()")
    assert "await refreshChallenge()" in submit
    assert 'reportProblem("challenge_stale"' in submit
    assert "challengeAt = challengeToken ? Date.now() : 0;" in SIGNUP_JS
    assert '"visibilitychange"' in SIGNUP_JS
