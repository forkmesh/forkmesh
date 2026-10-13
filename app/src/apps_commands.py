from __future__ import annotations

import json
import re

import apps
import apps_api
import apps_core as core
import apps_events as events


RESPOND_RE = re.compile(r"^/api/apps/respond/(fmru_[0-9a-f]{48,64})/?$")
MENTION_RE = re.compile(r"(?<![\w@/-])@([a-z][a-z0-9-]{1,30}[a-z0-9])(?![\w-])",
                        re.IGNORECASE)
MAX_MENTIONED_APPS = 5


def _json(runtime, data, status=200):
    return runtime.response(data, status=status, cache_control="no-store")


def _err(runtime, error, status=400, **extra):
    body = {"ok": False, "error": error}
    body.update(extra)
    return _json(runtime, body, status)


def _dev(runtime):
    return core.dev_origins(runtime.config("APPS_DEV_ENDPOINT_ORIGINS"))


async def viewer_installs(runtime, name):
    rows = list(await runtime.d1_all(
        "SELECT * FROM app_installs WHERE target_bi=? AND target_kind='user' "
        "AND status='active' ORDER BY created_at LIMIT 100",
        await runtime.blind("app-target:user:" + name)) or [])
    for org_name, _role in await runtime.orgs_for(name):
        org_bi = await runtime.org_lookup(org_name)
        if org_bi:
            rows += await runtime.d1_all(
                "SELECT * FROM app_installs WHERE target_bi=? "
                "AND target_kind='org' AND status='active' "
                "ORDER BY created_at LIMIT 100", org_bi) or []
    out = []
    cache = {}
    for install in rows:
        app_id = install["app_id"]
        if app_id not in cache:
            cache[app_id] = await apps._load_app(runtime, app_id)
        row, record = cache[app_id]
        if not row or str(row.get("listing") or "") == "suspended":
            continue
        out.append((row, record, install))
    return out


async def list_commands(runtime, name):
    commands = []
    seen = set()
    for row, record, install in await viewer_installs(runtime, name):
        if "commands" not in (core.parse_scopes(install.get("scopes")) or ()):
            continue
        for command in record.get("commands") or []:
            key = command["command"]
            if key in seen:
                continue
            seen.add(key)
            commands.append({
                "command": key,
                "description": command.get("description", ""),
                "usageHint": command.get("usageHint", ""),
                "app": {"name": record.get("name"), "slug": row.get("slug"),
                        "icon": record.get("icon") or {}},
                "installId": install["install_id"],
            })
    return commands


async def mentionable(runtime, name):
    slugs = []
    for row, record, install in await viewer_installs(runtime, name):
        slug = str(row.get("slug") or "")
        if slug in slugs:
            continue
        if "chat:read" in (core.parse_scopes(install.get("scopes")) or ()) \
                and "app.mention" in (record.get("events") or ()):
            slugs.append(slug)
    return slugs


async def _channel(runtime, channel_id):
    if not channel_id:
        return None
    if not apps_api.CHANNEL_RE.fullmatch(str(channel_id)):
        return None
    channel = await runtime.chat_channel(channel_id)
    if not channel or not channel.get("public"):
        return None
    return channel


async def _response_url(runtime, context, account_bi, name, channel_id):
    token = core.RESPONSE_URL_PREFIX + runtime.new_hex(32)
    now = runtime.now()
    await runtime.d1_run(
        "INSERT INTO app_response_urls (token_bi, app_id, install_id, "
        "channel_id, account_bi, uses, expires_at, data) "
        "VALUES (?,?,?,?,?,0,?,?)",
        await runtime.blind(core.secret_digest_input("response_url", token)),
        context["appId"], context["installId"], channel_id or "", account_bi,
        now + core.RESPONSE_URL_TTL_MS, await runtime.seal({"user": name}))
    return runtime.public_origin().rstrip("/") + "/api/apps/respond/" + token


async def _ephemeral_record(runtime, context, account_bi, blocks, actions):
    message_id = runtime.new_hex(16)
    now = runtime.now()
    await runtime.d1_run(
        "INSERT INTO app_messages (message_id, app_id, install_id, "
        "channel_id, created_at, expires_at, data) VALUES (?,?,?,'',?,?,?)",
        message_id, context["appId"], context["installId"], now,
        now + core.INTERACTIVE_MESSAGE_TTL_MS,
        await runtime.seal({"blocks": blocks, "actions": list(actions),
                            "account": account_bi}))
    return message_id


def _parse_reply(text):
    try:
        reply = json.loads(text or "{}")
    except ValueError:
        return {"text": core.clean_text(text, core.MAX_MESSAGE_TEXT)}
    return reply if isinstance(reply, dict) else {}


async def apply_reply(runtime, context, account_bi, name, channel_id, reply):
    blocks, actions = core.clean_blocks(reply.get("blocks"))
    if blocks is None:
        blocks, actions = [], ()
    text = core.clean_text(reply.get("text"), core.MAX_MESSAGE_TEXT)
    if not text and not blocks:
        return {"responseType": "none"}
    wanted = str(reply.get("responseType") or reply.get("response_type")
                 or "ephemeral")
    record = context["record"]
    can_post = (
        wanted == "in_channel" and channel_id
        and "chat:write" in context["scopes"]
        and (record.get("bot") or {}).get("enabled"))
    if can_post:
        posted, error = await apps_api.chat_post(
            runtime, context, channel_id, text, blocks,
            kind="command.in_channel")
        if not error:
            return {"responseType": "in_channel", "messageId":
                    posted["messageId"]}
    ephemeral = {
        "responseType": "ephemeral",
        "text": text or core.blocks_plain_text(blocks),
        "app": {"name": record.get("name"),
                "botName": core.bot_display_name(record.get("name")),
                "slug": context["slug"], "icon": record.get("icon") or {}},
    }
    if blocks:
        ephemeral["blocks"] = blocks
        ephemeral["appMessageId"] = await _ephemeral_record(
            runtime, context, account_bi, blocks, actions)
    return ephemeral


async def run_command(runtime, account_bi, name, data):
    command, args = core.parse_slash(data.get("text"))
    if not command:
        return _err(runtime, "not_a_command")
    match = None
    for row, record, install in await viewer_installs(runtime, name):
        if "commands" not in (core.parse_scopes(install.get("scopes")) or ()):
            continue
        for entry in record.get("commands") or []:
            if entry["command"] == command:
                match = (row, record, install, entry)
                break
        if match:
            break
    if not match:
        return _err(runtime, "unknown_command", 404)
    row, record, install, entry = match
    context = await apps.install_context(runtime, row, record, install)
    limited, _retry = await apps_api.rate_check(runtime, context)
    if limited is not None:
        return limited
    channel_id = str(data.get("channelId") or "")
    channel = await _channel(runtime, channel_id)
    if channel is None:
        channel_id = ""
    url = core.clean_endpoint_url(entry.get("url"), _dev(runtime))
    if not url:
        return _err(runtime, "command_unavailable", 502)
    request_id = runtime.new_hex(16)
    payload = {
        "type": "slash_command",
        "requestId": request_id,
        "command": command,
        "text": args,
        "user": {"name": name},
        "channel": {"id": channel_id,
                    "name": (channel or {}).get("name", "")},
        "appId": context["appId"],
        "installId": context["installId"],
        "target": {"kind": context["targetKind"],
                   "name": context["targetName"]},
        "responseUrl": await _response_url(runtime, context, account_bi,
                                           name, channel_id),
        "triggerId": runtime.new_hex(12),
    }
    status, text = await events.signed_post(
        runtime, record, url, payload, "slash_command", 1,
        core.COMMAND_TIMEOUT_SECONDS)
    ok = 200 <= int(status or 0) < 300
    await events.record_delivery(runtime, context["appId"], "command",
                                 "/" + command, status, 1,
                                 "delivered" if ok else "failed")
    await runtime.audit(name, "apps.command.run", "app", context["appId"],
                        details={"command": command,
                                 "installId": context["installId"]})
    if not ok:
        return _json(runtime, {"ok": False, "error": "app_unavailable",
                               "app": record.get("name")}, 502)
    result = await apply_reply(runtime, context, account_bi, name,
                               channel_id, _parse_reply(text))
    return _json(runtime, dict(result, ok=True))


async def respond(runtime, token):
    if runtime.method() != "POST":
        return _err(runtime, "method_not_allowed", 405)
    token_bi = await runtime.blind(
        core.secret_digest_input("response_url", token))
    now = runtime.now()
    claimed = await runtime.d1_first(
        "UPDATE app_response_urls SET uses=uses+1 WHERE token_bi=? "
        "AND expires_at>? AND uses<? RETURNING app_id, install_id, "
        "channel_id, account_bi, data", token_bi, now,
        core.RESPONSE_URL_MAX_USES)
    if not claimed:
        return _err(runtime, "invalid_response_url", 404)
    install = await runtime.d1_first(
        "SELECT * FROM app_installs WHERE install_id=? AND status='active'",
        claimed["install_id"])
    row, record = await apps._load_app(runtime, claimed["app_id"])
    if not install or not row or \
            str(row.get("listing") or "") == "suspended":
        return _err(runtime, "invalid_response_url", 404)
    data, error = await runtime.json_body(apps.MAX_BODY)
    if error:
        return _err(runtime, error)
    context = await apps.install_context(runtime, row, record, install)
    user = (await runtime.open(claimed.get("data")) or {}).get("user", "")
    result = await apply_reply(runtime, context, claimed["account_bi"], user,
                               claimed.get("channel_id") or "", data)
    if result.get("responseType") == "ephemeral" and user:
        await runtime.notify(
            user, str(record.get("name") or "App"),
            result.get("text", "")[:600], "/chat",
            "app-respond:%s:%s" % (claimed["install_id"], runtime.new_hex(6)))
    await events.record_delivery(runtime, context["appId"], "respond",
                                 result.get("responseType", ""), 200, 1,
                                 "delivered")
    return _json(runtime, {"ok": True,
                           "responseType": result.get("responseType")})


async def interact(runtime, account_bi, name, data):
    message_id = str(data.get("appMessageId") or "")
    if not re.fullmatch(r"[0-9a-f]{32}", message_id):
        return _err(runtime, "invalid_message")
    message = await runtime.d1_first(
        "SELECT * FROM app_messages WHERE message_id=? AND expires_at>?",
        message_id, runtime.now())
    if not message:
        return _err(runtime, "message_expired", 404)
    stored = await runtime.open(message.get("data")) or {}
    channel_id = str(message.get("channel_id") or "")
    if channel_id:
        if str(data.get("channelId") or "") != channel_id or \
                await _channel(runtime, channel_id) is None:
            return _err(runtime, "invalid_message", 403)
    elif str(stored.get("account") or "") != account_bi:
        return _err(runtime, "invalid_message", 403)
    action_id = str(data.get("actionId") or "")
    value = core.clean_text(data.get("value"), 200)
    if action_id not in (stored.get("actions") or []) or \
            not core.action_value_allowed(stored.get("blocks"), action_id,
                                          value):
        await runtime.audit(name, "apps.interact", "app", message["app_id"],
                            outcome="denied")
        return _err(runtime, "invalid_action", 400)
    install = await runtime.d1_first(
        "SELECT * FROM app_installs WHERE install_id=? AND status='active'",
        message["install_id"])
    row, record = await apps._load_app(runtime, message["app_id"])
    if not install or not row or \
            str(row.get("listing") or "") == "suspended":
        return _err(runtime, "app_unavailable", 404)
    context = await apps.install_context(runtime, row, record, install)
    limited, _retry = await apps_api.rate_check(runtime, context)
    if limited is not None:
        return limited
    url = core.clean_endpoint_url(record.get("interactivityUrl"),
                                  _dev(runtime))
    if not url:
        return _err(runtime, "interactivity_not_configured", 409)
    payload = {
        "type": "block_actions",
        "requestId": runtime.new_hex(16),
        "user": {"name": name},
        "channel": {"id": channel_id},
        "message": {"id": message_id},
        "action": {"actionId": action_id, "value": value},
        "appId": context["appId"],
        "installId": context["installId"],
        "responseUrl": await _response_url(runtime, context, account_bi,
                                           name, channel_id),
    }
    status, text = await events.signed_post(
        runtime, record, url, payload, "block_actions", 1,
        core.COMMAND_TIMEOUT_SECONDS)
    ok = 200 <= int(status or 0) < 300
    await events.record_delivery(runtime, context["appId"], "interaction",
                                 action_id, status, 1,
                                 "delivered" if ok else "failed")
    if not ok:
        return _json(runtime, {"ok": False, "error": "app_unavailable"}, 502)
    result = await apply_reply(runtime, context, account_bi, name,
                               channel_id, _parse_reply(text))
    return _json(runtime, dict(result, ok=True))


async def forward_mentions(runtime, account_bi, name, data):
    channel_id = str(data.get("channelId") or "")
    channel = await _channel(runtime, channel_id)
    if channel is None:
        return _err(runtime, "public_channel_required", 409)
    text = core.clean_text(data.get("text"), core.MAX_MESSAGE_TEXT)
    slugs = []
    for match in MENTION_RE.finditer(text):
        slug = match.group(1).lower()
        if slug not in slugs:
            slugs.append(slug)
    slugs = slugs[:MAX_MENTIONED_APPS]
    if not slugs:
        return _json(runtime, {"ok": True, "forwarded": 0})
    forwarded = 0
    seen = set()
    for row, record, install in await viewer_installs(runtime, name):
        slug = str(row.get("slug") or "")
        if slug not in slugs or row["app_id"] in seen:
            continue
        scopes = core.parse_scopes(install.get("scopes")) or ()
        if "chat:read" not in scopes or \
                "app.mention" not in (record.get("events") or ()) or \
                not events._deliverable(row):
            continue
        seen.add(row["app_id"])
        await events.queue(runtime, row, record, install, "app.mention", {
            "source": "chat.message",
            "channel": {"id": channel_id, "name": channel.get("name")},
            "messageId": core.clean_line(data.get("messageId"), 64),
            "sender": {"name": name},
            "text": text,
        }, await events._target(runtime, install))
        forwarded += 1
    if forwarded:
        await events.drain(runtime, min(forwarded, events.DRAIN_IMMEDIATE))
    return _json(runtime, {"ok": True, "forwarded": forwarded})


async def handle(runtime, path, account_bi, name, data, method):
    if path.rstrip("/") == "/api/apps/commands":
        if method != "GET":
            return _err(runtime, "method_not_allowed", 405)
        return _json(runtime, {"ok": True,
                               "commands": await list_commands(runtime, name),
                               "mentionable": await mentionable(runtime,
                                                                name)})
    if method != "POST":
        return _err(runtime, "method_not_allowed", 405)
    if path.rstrip("/") == "/api/apps/commands/run":
        return await run_command(runtime, account_bi, name, data)
    if path.rstrip("/") == "/api/apps/interact":
        return await interact(runtime, account_bi, name, data)
    if path.rstrip("/") == "/api/apps/mentions":
        return await forward_mentions(runtime, account_bi, name, data)
    return None
