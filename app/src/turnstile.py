"""Cloudflare Turnstile gate for the signed-out account forms.

Production ForkMesh puts /signup* behind a Cloudflare zone WAF challenge. That
only works for proxied traffic, and ForkMesh Next's app.forkmesh.com is
DNS-only (it points at our own nodes), so Next runs the same kind of check in
the app: the pages render a Turnstile widget (managed mode) and the POST that
follows must carry its token, which this module verifies with Cloudflare's
siteverify API.

Gated:

* ``POST /api/accounts/signup`` (the /signup page; action ``signup``, cdata
  bound to the page's one-use signup-intent claim).
* ``POST /api/accounts/forgot-password`` (the /forgot-password page; action
  ``password-reset``).

Not gated: ``/api/accounts/reserve`` + ``/finalize``. Those are Ed25519-signed
node registrations from the desktop and from headless installers, which have
no browser to render a widget. They never create a password web user (that is
enforced separately), and fresh name claims carry a per-IP rate limit.

Mobile and native apps have no signup API of their own: they open /signup in
the system browser, so they solve the widget like any browser. A client that
POSTs the endpoints directly gets ``turnstile_required`` with a message that
says to complete the challenge on the web (``challengeUrl``).

Configuration (docs/OPERATIONS.md, "Signup challenge"):

* ``TURNSTILE_SITE_KEY``: public, app/wrangler.toml [vars].
* ``TURNSTILE_SECRET_KEY``: vault secret (deploy/fleet.toml optional_secrets).
* ``TURNSTILE_HOSTNAMES``: optional comma list; defaults to the widget's
  domains (app.forkmesh.com, forkmesh.com).

Fail closed: on ``FORKMESH_PLATFORM=vps`` a missing secret (or site key)
refuses the gated forms with 503 ``turnstile_unavailable``, as does a
siteverify timeout, transport error or non-200 answer. Off the VPS (tests,
``wrangler dev``, any other platform) a missing secret selects Cloudflare's
documented test pair (site key ``1x00000000000000000000AA``, secret
``1x0000000000000000000000000000000AA``), which always passes. Test secrets
are answered locally without a network call, exactly as Cloudflare answers
them (``1x..`` passes, ``2x..`` fails, ``3x..`` is "already spent"). On a VPS
the test pair is refused unless ``TURNSTILE_TEST_MODE=1`` is also set, so a
dev env file copied to a server cannot silently switch the gate off.
"""

import hashlib
import json
import re
import time
from datetime import datetime, timezone
from urllib.parse import urlsplit
import instance_privacy


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


SITEVERIFY_URL = "https://challenges.cloudflare.com/turnstile/v0/siteverify"
SCRIPT_URL = "https://challenges.cloudflare.com/turnstile/v0/api.js"
CHALLENGE_ORIGIN = "https://challenges.cloudflare.com"
TEST_SITE_KEY = "1x00000000000000000000AA"
TEST_SECRET_KEY = "1x0000000000000000000000000000000AA"  # forkmesh-secret-scan:ignore-line
DEFAULT_HOSTNAMES = ("app.forkmesh.com", "forkmesh.com")
SITEVERIFY_TIMEOUT_SECONDS = 5
# Cloudflare tokens are valid for 300 s; the challenge timestamp is checked
# against the same window so a token held back by a client is not honoured
# past it even if siteverify were lenient.
TOKEN_MAX_AGE_MS = 300 * 1000
TOKEN_FUTURE_SKEW_MS = 60 * 1000
TOKEN_MAX_LENGTH = 2048
ACTION_SIGNUP = "signup"
ACTION_PASSWORD_RESET = "password-reset"
TOKEN_FIELD = "turnstileToken"
BACKUP_FIELD = "backupProof"
BACKUP_PREFIX = "forkmesh-signup-backup-v1:"
BACKUP_BITS = 18
_BACKUP_NONCE_RE = re.compile(r"[0-9]{1,16}")

_TEST_SECRET_RE = re.compile(r"[123]x0{31}A[AB]")
_TEST_SITE_KEY_RE = re.compile(r"[123]x0{20}[AB][ABF]")
_CDATA_RE = re.compile(r"[A-Za-z0-9_-]{1,255}")

REQUIRED = "turnstile_required"
FAILED = "turnstile_failed"
EXPIRED = "turnstile_expired"
UNAVAILABLE = "turnstile_unavailable"
STALE_PAGE = "turnstile_stale_page"

MESSAGES = {
    REQUIRED: (
        "Complete the security challenge on the web page and try again. "
        "Apps without the challenge must open the page in a browser."),
    FAILED: "The security challenge could not be verified. Try it again.",
    EXPIRED: (
        "The security challenge expired or was already used. "
        "Solve it again and resubmit."),
    UNAVAILABLE: (
        "The security challenge is unavailable right now, so this form is "
        "closed. Try again in a few minutes."),
    STALE_PAGE: (
        "This signup page was replaced by a newer one (another tab or a "
        "reload). Reload this page and try again."),
}
STATUS = {REQUIRED: 403, FAILED: 403, EXPIRED: 403, UNAVAILABLE: 503,
          STALE_PAGE: 403}

# Tokens this process has already sent to siteverify (sha256 -> expiry ms).
# Cloudflare rejects a reused token itself (timeout-or-duplicate); this local
# memo answers a replay without a network call and covers the window where a
# replay races the first verification. Per process only, so the other nodes
# still rely on Cloudflare's answer.
_SEEN = {}
_SEEN_MAX = 4096


def _now_ms():
    return int(time.time() * 1000)


def _var(env, name):
    return str(getattr(env, name, "") or "").strip()


def _truthy(value):
    return str(value or "").strip().lower() in ("1", "true", "yes", "on")


def is_vps(env):
    return _var(env, "FORKMESH_PLATFORM").lower() == "vps"


def is_test_secret(secret):
    return bool(_TEST_SECRET_RE.fullmatch(str(secret or "")))


def is_test_site_key(site_key):
    return bool(_TEST_SITE_KEY_RE.fullmatch(str(site_key or "")))


def config(env):
    """The widget site key, the secret and the mode for this deployment.

    mode is ``live`` (real siteverify), ``test`` (Cloudflare test secret,
    answered locally) or ``unavailable`` (fail closed).
    """
    site_key = _var(env, "TURNSTILE_SITE_KEY")
    secret = _var(env, "TURNSTILE_SECRET_KEY")
    vps = is_vps(env)
    if not secret:
        if vps:
            return {"siteKey": site_key, "secret": "", "mode": "unavailable",
                    "reason": "secret_missing"}
        return {"siteKey": TEST_SITE_KEY, "secret": TEST_SECRET_KEY,
                "mode": "test", "reason": "dev_default"}
    if is_test_secret(secret):
        if vps and not _truthy(getattr(env, "TURNSTILE_TEST_MODE", "")):
            return {"siteKey": site_key, "secret": "", "mode": "unavailable",
                    "reason": "test_secret_on_vps"}
        return {"siteKey": site_key if is_test_site_key(site_key)
                else TEST_SITE_KEY, "secret": secret, "mode": "test",
                "reason": "test_keys"}
    if not site_key or is_test_site_key(site_key):
        return {"siteKey": site_key, "secret": "", "mode": "unavailable",
                "reason": "site_key_mismatch"}
    return {"siteKey": site_key, "secret": secret, "mode": "live",
            "reason": ""}


def site_key(env):
    return config(env)["siteKey"]


def _origin_host(env):
    origin = _var(env, "APP_ORIGIN")
    try:
        parsed = urlsplit(origin)
        host = parsed.hostname or ""
    except ValueError:
        return ""
    return host.strip().lower().rstrip(".")


def allowed_hostnames(env):
    raw = _var(env, "TURNSTILE_HOSTNAMES")
    names = [item.strip().lower().rstrip(".") for item in raw.split(",")]
    names = [name for name in names if name]
    if not names and _var(env, "INSTANCE_NAME") and _origin_host(env):
        names = [_origin_host(env)]
    return frozenset(names or DEFAULT_HOSTNAMES)


def signup_cdata(claim_id):
    """Opaque widget cdata that binds a token to one signup page view."""
    digest = hashlib.sha256(
        ("forkmesh-turnstile-signup-v1\n" + str(claim_id or "")).encode())
    return digest.hexdigest()[:32]


def _token_key(token):
    return hashlib.sha256(token.encode("utf-8", "replace")).hexdigest()


def _seen_before(token, now):
    key = _token_key(token)
    if _SEEN.get(key, 0) > now:
        return True
    if len(_SEEN) >= _SEEN_MAX:
        for stale in [k for k, exp in _SEEN.items() if exp <= now]:
            _SEEN.pop(stale, None)
        while len(_SEEN) >= _SEEN_MAX:
            _SEEN.pop(next(iter(_SEEN)))
    _SEEN[key] = now + TOKEN_MAX_AGE_MS + TOKEN_FUTURE_SKEW_MS
    return False


def _challenge_ms(value):
    text = str(value or "").strip()
    if not text:
        return None
    if text.endswith("Z") or text.endswith("z"):
        text = text[:-1] + "+00:00"
    try:
        parsed = datetime.fromisoformat(text)
    except ValueError:
        return None
    if parsed.tzinfo is None:
        return None
    return int(parsed.timestamp() * 1000)


def _test_answer(secret, hostname, action, cdata, now):
    stamp = datetime.fromtimestamp(now / 1000, timezone.utc).strftime(
        "%Y-%m-%dT%H:%M:%S.000Z")
    if secret.startswith("2x"):
        return {"success": False, "error-codes": ["invalid-input-response"]}
    if secret.startswith("3x"):
        return {"success": False, "error-codes": ["timeout-or-duplicate"]}
    return {"success": True, "challenge_ts": stamp, "hostname": hostname,
            "action": action, "cdata": cdata, "error-codes": []}


async def verify(env, token, action, remoteip="", cdata="", fetcher=None,
                 now_ms=None):
    """Return ``(error_code, detail)``; ``("", "")`` means the token passed.

    ``fetcher(url, init, timeout_seconds)`` defaults to the runtime's
    ``js_fetch_with_timeout`` (native AbortSignal timeout).
    """
    now = int(now_ms if now_ms is not None else _now_ms())
    settings = config(env)
    if settings["mode"] == "unavailable":
        return UNAVAILABLE, "config:" + settings["reason"]
    token = token if isinstance(token, str) else ""
    token = token.strip()
    if not token or len(token) > TOKEN_MAX_LENGTH:
        return REQUIRED, "token_missing"
    if cdata and not _CDATA_RE.fullmatch(cdata):
        return FAILED, "cdata_invalid"
    hostnames = allowed_hostnames(env)
    if settings["mode"] == "test":
        # Cloudflare's test secrets answer the same fixed result for every
        # (dummy) token and do not reject reuse, so neither does this path.
        payload = _test_answer(settings["secret"], sorted(hostnames)[0],
                               action, cdata, now)
    else:
        if _seen_before(token, now):
            return EXPIRED, "token_reused"
        body = {"secret": settings["secret"], "response": token}
        if remoteip and not instance_privacy.is_pseudonym(remoteip):
            body["remoteip"] = str(remoteip)[:64]
        fetch = fetcher or globals().get("js_fetch_with_timeout")
        try:
            response = await fetch(SITEVERIFY_URL, {
                "method": "POST",
                "headers": {"content-type": "application/json",
                            "accept": "application/json"},
                "body": json.dumps(body, separators=(",", ":")),
                "redirect": "error",
            }, SITEVERIFY_TIMEOUT_SECONDS)
            status = int(getattr(response, "status", 0) or 0)
            if status != 200:
                return UNAVAILABLE, "siteverify_http_%d" % status
            payload = json.loads(str(await response.text()))
        except Exception as error:  # timeout, egress refusal, bad JSON
            return UNAVAILABLE, "siteverify_error:" + type(error).__name__
    if not isinstance(payload, dict):
        return UNAVAILABLE, "siteverify_payload"
    if payload.get("success") is not True:
        codes = payload.get("error-codes")
        codes = [str(c) for c in codes] if isinstance(codes, list) else []
        if "timeout-or-duplicate" in codes:
            return EXPIRED, "timeout-or-duplicate"
        if any(code in ("missing-input-secret", "invalid-input-secret",
                        "internal-error") for code in codes):
            return UNAVAILABLE, "siteverify:" + ",".join(codes)[:80]
        return FAILED, "siteverify:" + (",".join(codes)[:80] or "rejected")
    hostname = str(payload.get("hostname") or "").strip().lower().rstrip(".")
    if hostname not in hostnames:
        return FAILED, "hostname_mismatch"
    if action and str(payload.get("action") or "") != action:
        return FAILED, "action_mismatch"
    if cdata and str(payload.get("cdata") or "") != cdata:
        return STALE_PAGE, "cdata_mismatch"
    issued = _challenge_ms(payload.get("challenge_ts"))
    if issued is None:
        return FAILED, "challenge_ts_missing"
    if now - issued > TOKEN_MAX_AGE_MS:
        return EXPIRED, "challenge_stale"
    if issued - now > TOKEN_FUTURE_SKEW_MS:
        return FAILED, "challenge_in_future"
    return "", ""


def backup_enabled(env):
    return _var(env, "SIGNUP_BACKUP_CHECK").lower() not in (
        "0", "off", "false", "no")


def backup_reason(proof):
    reason = proof.get("reason", "") if isinstance(proof, dict) else ""
    return re.sub(r"[^a-z0-9_]", "", str(reason or "").lower())[:24]


def _leading_zero_bits(digest):
    bits = 0
    for byte in digest:
        if byte:
            return bits + 8 - byte.bit_length()
        bits += 8
    return bits


def verify_backup(env, proof, cdata):
    if not backup_enabled(env):
        return REQUIRED, "backup_disabled"
    if not isinstance(proof, dict):
        return REQUIRED, "backup_missing"
    nonce = proof.get("nonce", "")
    if not isinstance(nonce, str) or not _BACKUP_NONCE_RE.fullmatch(nonce):
        return FAILED, "backup_nonce_invalid"
    if not cdata or not _CDATA_RE.fullmatch(cdata):
        return FAILED, "cdata_invalid"
    digest = hashlib.sha256(
        (BACKUP_PREFIX + cdata + ":" + nonce).encode()).digest()
    if _leading_zero_bits(digest) < BACKUP_BITS:
        return FAILED, "backup_proof_short"
    return "", ""


def refusal_payload(code, challenge_url):
    return {
        "error": code,
        "message": MESSAGES.get(code, MESSAGES[FAILED]),
        "challengeUrl": challenge_url,
    }
