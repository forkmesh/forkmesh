from __future__ import annotations

import re

import apps
import apps_core as core
import apps_events as events


V1_RE = re.compile(r"^/api/apps/v1/([a-z]+\.[A-Za-z]+)/?$")
REPO_READ_OPS = ("tree", "blobs", "blob", "raw", "history", "commit",
                 "compare", "branches", "search", "stats", "sizes", "about",
                 "mirrors", "pending")
WEB_API_ROUTES = (
    (re.compile(r"^/api/repositories/?$"), "repos:read", None),
    (re.compile(r"^/api/orgs/([a-z0-9-]{1,64})/repos/?$"), "repos:read",
     None),
    (re.compile(r"^/api/repo/([^/]+)/([^/]+)/(%s)/?$" % "|".join(
        REPO_READ_OPS)), "repos:read", "repo"),
    (re.compile(r"^/api/notifications/?$"), "pings:read", "pings"),
)
BOT_METHODS = frozenset({"chat.postMessage", "issues.create",
                         "issues.comment", "pulls.comment", "agents.run"})
METHOD_SCOPES = {
    "auth.test": (),
    "chat.postMessage": ("chat:write",),
    "issues.create": ("issues:write",),
    "issues.comment": ("issues:write",),
    "pulls.comment": ("pulls:write",),
    "pings.send": ("pings:write",),
    "agents.run": ("agents:run",),
}
NAME_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{0,63}$")
REPO_RE = re.compile(r"^[A-Za-z0-9._-]{1,100}$")
CHANNEL_RE = re.compile(r"^[0-9a-f]{32}$")
MAX_TITLE = 240
MAX_BODY_TEXT = 16 * 1024


def _json(runtime, data, status=200, headers=None):
    return runtime.response(data, status=status, cache_control="no-store",
                            extra_headers=headers)


def _err(runtime, error, status=400, **extra):
    body = {"ok": False, "error": error}
    body.update(extra)
    return _json(runtime, body, status)


def _repo_scope(path, query):
    target = str((query or {}).get("path") or "").lstrip("/").lower()
    ref = str((query or {}).get("ref") or "").lower()
    if target.startswith(".forkmesh/issues"):
        return "issues:read"
    if target.startswith(".forkmesh/pulls") or "pull" in ref:
        return "pulls:read"
    return "repos:read"


def route_scope(path, query):
    for pattern, scope, kind in WEB_API_ROUTES:
        match = pattern.fullmatch(path)
        if not match:
            continue
        if kind == "repo":
            return _repo_scope(path, query), match.groups()
        return scope, match.groups()
    return "", ()


async def rate_check(runtime, context):
    now = runtime.now()
    cutoff = now - core.RATE_WINDOW_MS
    retry = 0
    for key, limit in (("install:" + context["installId"],
                        core.RATE_PER_INSTALL),
                       ("app:" + context["appId"], core.RATE_PER_APP)):
        row = await runtime.d1_first(
            "INSERT INTO app_rate (rate_key, count, window_start) "
            "VALUES (?,1,?) ON CONFLICT(rate_key) DO UPDATE SET "
            "count=CASE WHEN app_rate.window_start<=? THEN 1 "
            "ELSE app_rate.count+1 END, "
            "window_start=CASE WHEN app_rate.window_start<=? "
            "THEN excluded.window_start ELSE app_rate.window_start END "
            "RETURNING count, window_start", key, now, cutoff, cutoff)
        count = int((row or {}).get("count") or 0)
        if count > limit:
            start = int((row or {}).get("window_start") or now)
            retry = max(retry, 1 + (start + core.RATE_WINDOW_MS - now) // 1000)
    if retry:
        return _err(runtime, "rate_limited", 429,
                    retryAfter=int(retry)), int(retry)
    return None, 0


async def covered(runtime, context, owner):
    owner = str(owner or "").strip().lower()
    if not owner:
        return False
    if context["targetKind"] == core.TARGET_ORG:
        return owner == context["targetName"]
    return await runtime.account_owns(context["targetName"], owner)


async def gate(runtime, token, method, path, query):
    context = await apps.authenticate(runtime, token)
    if context is None:
        return None, _err(runtime, "invalid_token", 401)
    if method not in ("GET", "HEAD"):
        return None, _err(runtime, "app_token_not_allowed", 403)
    scope, groups = route_scope(path, query)
    if not scope:
        return None, _err(runtime, "app_token_not_allowed", 403)
    if scope not in context["scopes"]:
        return None, _err(runtime, "missing_scope", 403, needed=scope)
    limited, retry = await rate_check(runtime, context)
    if limited is not None:
        return None, limited
    identity = ""
    if context["targetKind"] == core.TARGET_USER:
        if path.startswith("/api/repo/"):
            if await covered(runtime, context, groups[0]):
                identity = context["installedByBi"]
        elif path.startswith("/api/notifications"):
            if str((query or {}).get("node") or "").lower() != \
                    context["targetName"]:
                return None, _err(runtime, "missing_scope", 403,
                                  needed=scope)
            identity = context["installedByBi"]
        else:
            identity = context["installedByBi"]
    context["identity"] = identity
    return context, None


def _clean_body(value, limit=MAX_BODY_TEXT):
    return core.clean_text(value, limit)


async def _repo_args(runtime, context, data):
    owner = str(data.get("owner") or "").strip().lower()
    repo = str(data.get("repo") or "").strip()
    if not NAME_RE.fullmatch(owner) or not REPO_RE.fullmatch(repo):
        return None, _err(runtime, "invalid_repository")
    if not await covered(runtime, context, owner):
        return None, _err(runtime, "repository_not_covered", 403)
    return (owner, repo), None


def _number(data):
    try:
        number = int(data.get("number") or 0)
    except (TypeError, ValueError):
        return 0
    return number if 0 < number < 10 ** 9 else 0


async def _audit(runtime, context, action, target_type, target, details=None):
    await runtime.audit(context["actor"], "apps." + action, target_type,
                        target, details=dict(details or {},
                                             installId=context["installId"]))


async def chat_post(runtime, context, channel_id, text, blocks_in,
                    kind="chat.postMessage"):
    if not CHANNEL_RE.fullmatch(str(channel_id or "")):
        return None, "invalid_channel"
    channel = await runtime.chat_channel(channel_id)
    if not channel:
        return None, "channel_not_found"
    if not channel.get("public"):
        return None, "public_channel_required"
    blocks, actions = core.clean_blocks(blocks_in)
    if blocks is None:
        return None, actions
    text = _clean_body(text, core.MAX_MESSAGE_TEXT)
    if not text:
        text = core.blocks_plain_text(blocks)
    if not text:
        return None, "empty_message"
    record = context["record"]
    message_id = runtime.new_hex(16)
    now = runtime.now()
    plain = {
        "type": "chat",
        "id": message_id,
        "senderId": context["actor"],
        "sender": core.bot_display_name(record.get("name")),
        "accountKind": "user",
        "ts": now,
        "channel": "#" + str(channel.get("name") or "channel"),
        "text": text,
        "app": {"id": context["appId"], "slug": context["slug"],
                "name": record.get("name"), "icon": record.get("icon")},
    }
    if blocks:
        plain["blocks"] = blocks
        plain["appMessageId"] = message_id
        await runtime.d1_run(
            "INSERT INTO app_messages (message_id, app_id, install_id, "
            "channel_id, created_at, expires_at, data) "
            "VALUES (?,?,?,?,?,?,?)",
            message_id, context["appId"], context["installId"], channel_id,
            now, now + core.INTERACTIVE_MESSAGE_TTL_MS,
            await runtime.seal({"blocks": blocks, "actions": list(actions)}))
    await runtime.chat_post(channel_id, channel.get("key_version") or 1,
                            plain)
    await _audit(runtime, context, "chat.post", "chat_channel", channel_id,
                 {"method": kind})
    try:
        await events.emit_chat(runtime, channel_id, channel.get("name"),
                               plain, source_actor=context["actor"])
    except Exception:
        pass
    return {"messageId": message_id, "channel": channel_id, "ts": now}, ""


async def _pings_send(runtime, context, data):
    title = core.clean_line(data.get("title"), 120)
    if not title:
        return _err(runtime, "invalid_title")
    body = core.clean_text(data.get("body"), 600)
    href = str(data.get("href") or "")
    if not re.fullmatch(r"/(?!/)[A-Za-z0-9._~!$&'()*+,;=:@/?%#-]{0,300}", href):
        href = "/dashboard/apps"
    if context["targetKind"] == core.TARGET_USER:
        recipients = [context["targetName"]]
    else:
        recipients = await runtime.org_admins(
            await runtime.org_lookup(context["targetName"]))
    name = str(context["record"].get("name") or "")
    ping_id = runtime.new_hex(8)
    for recipient in recipients:
        await runtime.notify(recipient, "%s: %s" % (name, title), body, href,
                             "app-ping:%s:%s" % (context["installId"],
                                                 ping_id))
    await _audit(runtime, context, "pings.send", "app_install",
                 context["installId"], {"recipients": len(recipients)})
    return _json(runtime, {"ok": True, "sent": len(recipients)})


async def v1(runtime, path, token):
    match = V1_RE.fullmatch(str(path or ""))
    if not match:
        return _err(runtime, "unknown_method", 404)
    method = match.group(1)
    if method not in METHOD_SCOPES:
        return _err(runtime, "unknown_method", 404)
    if runtime.method() != "POST":
        return _err(runtime, "method_not_allowed", 405)
    context = await apps.authenticate(runtime, token)
    if context is None:
        return _err(runtime, "invalid_token", 401)
    needed = METHOD_SCOPES[method]
    if needed and not core.has_scope(context["scopes"], needed):
        return _err(runtime, "missing_scope", 403, needed=needed[0])
    if method in BOT_METHODS and \
            not (context["record"].get("bot") or {}).get("enabled"):
        return _err(runtime, "bot_user_required", 403)
    limited, _retry = await rate_check(runtime, context)
    if limited is not None:
        return limited
    data, error = await runtime.json_body(apps.MAX_BODY)
    if error:
        return _err(runtime, error)
    if method == "auth.test":
        record = context["record"]
        body = {"ok": True,
                "app": {"id": context["appId"], "slug": context["slug"],
                        "name": record.get("name")},
                "install": {"id": context["installId"],
                            "target": context["targetKind"],
                            "name": context["targetName"]},
                "scopes": list(context["scopes"])}
        if (record.get("bot") or {}).get("enabled"):
            body["bot"] = {"id": context["actor"],
                           "name": core.bot_display_name(record.get("name"))}
        return _json(runtime, body)
    if method == "chat.postMessage":
        result, error = await chat_post(runtime, context, data.get("channel"),
                                        data.get("text"), data.get("blocks"))
        if error:
            return _err(runtime, error, 404 if error == "channel_not_found"
                        else 400)
        return _json(runtime, dict(result, ok=True))
    if method == "pings.send":
        return await _pings_send(runtime, context, data)
    repo, denied = await _repo_args(runtime, context, data)
    if denied is not None:
        return denied
    owner, name = repo
    author = context["actor"]
    author_name = core.bot_display_name(context["record"].get("name"))
    if method == "issues.create":
        title = core.clean_line(data.get("title"), MAX_TITLE)
        if not title:
            return _err(runtime, "invalid_title")
        ok, result = await runtime.issue_open(
            owner, name, title, _clean_body(data.get("body")), author,
            author_name, context["slug"])
        if not ok:
            return _err(runtime, result, 429 if result in (
                "inbox_full", "author_quota") else 400)
        await _audit(runtime, context, "issues.create", "repository",
                     owner + "/" + name, {"number": result})
        return _json(runtime, {"ok": True, "number": result}, 201)
    number = _number(data)
    if not number:
        return _err(runtime, "invalid_number")
    if method == "agents.run":
        ok, result = await runtime.issue_comment(
            owner, name, number,
            "%s asked the host to start a coding agent on this issue."
            % author_name, author, author_name, wants_agent=True)
    else:
        body = _clean_body(data.get("body"))
        if not body:
            return _err(runtime, "empty_body")
        writer = runtime.issue_comment if method == "issues.comment" \
            else runtime.pull_comment
        ok, result = await writer(owner, name, number, body, author,
                                  author_name)
    if not ok:
        return _err(runtime, result, 429 if result in (
            "inbox_full", "author_quota") else 400)
    await _audit(runtime, context, method, "repository",
                 owner + "/" + name, {"number": number})
    return _json(runtime, {"ok": True, "number": number}, 201)
