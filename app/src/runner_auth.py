import re


PROVIDERS = ("claude-code", "codex")
STATES = ("ok", "expiring", "expired", "missing")
ALERT_STATES = ("expiring", "expired")
REASON_RE = re.compile(r"^[a-z_]{0,32}$")
DAY_MS = 24 * 60 * 60 * 1000
MESSAGE_MAX = 240
ALERT_KIND = "operational_alert"
ALERT_SOURCE = "runner-auth"
REQUIRED_ACTION = "reconnect_runner_login"
ERROR_CODE = "runner_login_expired"

_FAILURE_SIGNS = (
    "failed to authenticate",
    "oauth session expired",
    "oauth token has expired",
    "oauth token revoked",
    "could not be refreshed",
    "invalid api key",
    "invalid bearer token",
    "please run /login",
    "authentication_error",
    "api error: 401",
    "401 unauthorized",
    "invalid_grant",
    "refresh_token_reused",
    "refresh token was already used",
    "access token could not be refreshed",
    "provided authentication token is expired",
    "please log out and sign in again",
)


def provider_short(provider):
    return "Codex" if provider == "codex" else "Claude"


def provider_label(provider):
    return "Codex" if provider == "codex" else "Claude Code"


def _whole(value):
    try:
        number = int(value or 0)
    except (TypeError, ValueError):
        return 0
    return number if 0 <= number <= 10 ** 15 else 0


def _text(value, limit):
    return "".join(c for c in str(value or "") if c.isprintable())[:limit]


def clean(value):
    if not isinstance(value, dict):
        return None
    state = str(value.get("state") or "")
    if state not in STATES:
        return None
    reason = str(value.get("reason") or "")
    return {
        "state": state,
        "checkedAt": _whole(value.get("checkedAt")),
        "expiresAt": _whole(value.get("expiresAt")),
        "hasRefresh": value.get("hasRefresh") is True,
        "reason": reason if REASON_RE.fullmatch(reason) else "",
        "message": _text(value.get("message"), MESSAGE_MAX),
        "failedAt": _whole(value.get("failedAt")),
        "failedJob": _whole(value.get("failedJob")),
    }


def auth_failure_text(*texts):
    joined = "\n".join(str(text or "") for text in texts).lower()
    return any(sign in joined for sign in _FAILURE_SIGNS)


def state_of(entry):
    if not isinstance(entry, dict):
        return ""
    if entry.get("loginState") == "expired":
        return "expired"
    auth = entry.get("auth")
    if isinstance(auth, dict) and auth.get("state") in STATES:
        return auth["state"]
    return ""


def needs_login(availability):
    result = []
    if not isinstance(availability, dict):
        return result
    for provider in PROVIDERS:
        entry = availability.get(provider)
        if isinstance(entry, dict) and entry.get("binaryFound") is not False \
                and state_of(entry) == "expired":
            result.append(provider)
    return result


def episode(auth, now):
    auth = auth if isinstance(auth, dict) else {}
    return str(_whole(auth.get("failedAt")) or _whole(auth.get("expiresAt"))
               or int(now) // DAY_MS)


def dedupe_key(runner, provider, state, auth, now):
    return "runner-auth:%s:%s:%s:%s" % (runner, provider, state,
                                          episode(auth, now))


def transitions(old, new):
    changes = []
    old = old if isinstance(old, dict) else {}
    new = new if isinstance(new, dict) else {}
    for provider in PROVIDERS:
        before = state_of(old.get(provider))
        after = state_of(new.get(provider))
        if after in ALERT_STATES and after != before:
            entry = new.get(provider) or {}
            changes.append((provider, after, entry.get("auth") or {}))
    return changes


def _when(ms):
    import datetime
    return datetime.datetime.fromtimestamp(
        int(ms) / 1000, datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC")


def alert_text(runner, provider, state, auth):
    short = provider_short(provider)
    auth = auth if isinstance(auth, dict) else {}
    if state == "expiring":
        title = "%s: %s login expires soon — reconnect" % (runner, short)
        when = _whole(auth.get("expiresAt"))
        body = ("The %s login on runner %s expires%s. Agent jobs for %s will "
                "stop when it does. Reconnect %s from the Runners page now."
                % (provider_label(provider), runner,
                   " at " + _when(when) if when else " within a day",
                   provider_label(provider), short))
        return title, body
    title = "%s: %s login expired — reconnect" % (runner, short)
    body = ("The %s login on runner %s has expired and could not be "
            "refreshed, so the runner has stopped taking %s jobs. Jobs "
            "already queued wait until it is reconnected, and new ones are "
            "refused with this reason. Open Runners and press Connect %s "
            "to sign in again."
            % (provider_label(provider), runner, provider_label(provider),
               short))
    detail = _text(auth.get("message"), MESSAGE_MAX)
    if detail:
        body += "\n\nRunner report: " + detail
    return title, body


def runners_href(runner, provider):
    return "/dashboard/runners?runner=%s&provider=%s" % (runner, provider)


def status_reason(runner, availability):
    blocked = needs_login(availability)
    if not blocked:
        return ""
    names = " and ".join(provider_short(provider) for provider in blocked)
    return "%s: %s login expired — reconnect" % (runner, names)


def submit_message(runner, provider):
    short = provider_short(provider)
    return ("Runner %s needs %s reconnected. An admin can reconnect it from "
            "Runners (Connect %s); the job was not queued."
            % (runner, short, short))


def submit_block(targets, provider):
    for item in targets or []:
        blocked = (item or {}).get("needsLogin") or {}
        runners = blocked.get(provider) or []
        if runners:
            runner = runners[0]
            return {
                "ok": False,
                "error": ERROR_CODE,
                "message": submit_message(runner, provider),
                "requiredAction": REQUIRED_ACTION,
                "provider": provider,
                "targetNode": runner,
            }
    return None


async def notify(runtime, runner, changes, now, registered_by="", scope=""):
    sent = 0
    for provider, state, auth in changes:
        title, body = alert_text(runner, provider, state, auth)
        sent += await runtime.alert_runner_auth(
            runner=runner, provider=provider, state=state, title=title,
            body=body, href=runners_href(runner, provider),
            dedupe=dedupe_key(runner, provider, state, auth, now),
            email=state == "expired" and scope != "pool",
            extra=[registered_by] if scope == "personal" and registered_by
            else [])
    return sent
