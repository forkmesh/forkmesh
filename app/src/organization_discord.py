"""Organization-scoped Discord connector policy and HTTP surface.

The Worker adapter owns the only Discord bot credential and the network calls.
This module never accepts, stores, returns, or logs a bot token.  It persists
only an encrypted selected guild/channel configuration and limits read/send
operations to those owner-approved public channels.
"""

from __future__ import annotations

import hashlib
import hmac
import re

import organization_rbac


BODY_MAX_BYTES = 8 * 1024
MAX_CHANNELS = 50
MAX_MESSAGES = 50
MAX_MESSAGE_CONTENT = 2_000
MAX_RECENT_CHANNELS = 10
MAX_RECENT_MESSAGES = 200
MAX_RECENT_DAYS = 14
DISCORD_EPOCH_MS = 1_420_070_400_000
PUBLIC_TEXT_CHANNEL_TYPES = frozenset({0, 5})
VIEW_CHANNEL_PERMISSION = 1 << 10
READ_MESSAGE_HISTORY_PERMISSION = 1 << 16
ADMINISTRATOR_PERMISSION = 1 << 3
MANAGE_GUILD_PERMISSION = 1 << 5
OAUTH_STATE_TTL_MS = 10 * 60 * 1000
# We deliberately retain no refresh token. The grant is stable organization
# consent (revocable through Disconnect); fresh OAuth is needed to change the
# bound guild, not for every channel edit.
OAUTH_GRANT_TTL_MS = 0
# Discord linking is an insider surface: guests never reach it, and the
# manager checks below stay owner/admin (organization_rbac, issue #487).
ORG_ROLES = frozenset(organization_rbac.ORG_INSIDER_ROLES)
EXPECTED_DEGRADED_HEADERS = {"x-forkmesh-expected-degraded": "1"}
_SNOWFLAKE_RE = re.compile(r"^[0-9]{17,20}$")
_TASK_ID_RE = re.compile(r"^[a-f0-9]{32}$")
_OAUTH_STATE_RE = re.compile(r"^[a-f0-9]{64}$")


def _response(runtime, data, status=200, allow="", extra_headers=None):
    headers = {"x-content-type-options": "nosniff"}
    if allow:
        headers["allow"] = allow
    if isinstance(extra_headers, dict):
        headers.update({
            str(key): str(value)
            for key, value in extra_headers.items()
            if key and value is not None
        })
    return runtime.response(
        data,
        status=status,
        cache_control="no-store, max-age=0, must-revalidate",
        extra_headers=headers,
    )


def _text(value, maximum, multiline=False):
    """Return bounded plain text without retaining controls or markup."""

    value = str(value or "")
    if multiline:
        value = value.replace("\r\n", "\n").replace("\r", "\n")
        value = "".join(
            char if (char == "\n" or (char.isprintable() and char not in "<>") )
            else " "
            for char in value
        )
        value = "\n".join(line.strip() for line in value.split("\n"))
        value = value.strip()
    else:
        value = "".join(
            char if char.isprintable() and char not in "<>" else " "
            for char in value
        )
        value = " ".join(value.split()).strip()
    return value[:maximum]


def _snowflake(value):
    value = str(value or "").strip()
    return value if _SNOWFLAKE_RE.fullmatch(value) else ""


def _normalize_channel_ids(value):
    if not isinstance(value, list) or not value or len(value) > MAX_CHANNELS:
        return None
    result = []
    for item in value:
        channel_id = _snowflake(item)
        if not channel_id or channel_id in result:
            return None
        result.append(channel_id)
    return result


def _normalize_config(data):
    if not isinstance(data, dict):
        return None, "invalid_configuration"
    if set(data) - {"guildId", "channelIds"}:
        # In particular, a supplied `token` cannot become a future persisted
        # field by accident.  Bot credentials belong exclusively in Worker
        # secrets, never dashboard requests or D1.
        return None, "unsupported_configuration_field"
    guild_id = _snowflake(data.get("guildId"))
    channel_ids = _normalize_channel_ids(data.get("channelIds"))
    if not guild_id:
        return None, "invalid_guild_id"
    if channel_ids is None:
        return None, "invalid_channel_ids"
    return {"guildId": guild_id, "channelIds": channel_ids}, ""


def _stored_config(record):
    if not isinstance(record, dict):
        return None
    config, error = _normalize_config({
        "guildId": record.get("guildId"),
        "channelIds": record.get("channelIds"),
    })
    return config if not error else None


def _setup_task(state):
    if state == "oauth_client_required":
        return {
            "kind": "human_setup",
            "state": state,
            "title": "Configure Discord OAuth for this ForkMesh deployment",
            "instructions": [
                "Set DISCORD_CLIENT_ID and DISCORD_CLIENT_SECRET as Worker secrets; do not paste either value into ForkMesh, a repository, task, or chat message.",
                "Set DISCORD_OAUTH_REDIRECT_URI to the exact deployed callback URL ending in /api/integrations/discord/callback, and add that exact URL to the Discord application's OAuth2 Redirects list.",
                "Return to the Discord connector and start the owner authorization flow. ForkMesh stores only an encrypted verified-guild grant, never an OAuth access or refresh token.",
            ],
        }
    if state == "guild_authorization_required":
        return {
            "kind": "human_setup",
            "state": state,
            "title": "Authorize this organization’s Discord guild",
            "instructions": [
                "Enter the Discord Server ID that this organization should use, then choose Connect Discord server.",
                "Complete the Discord OAuth authorization as that server’s owner, administrator, or Manage Server member. ForkMesh verifies that permission for the exact Server ID before it can list channels or send messages.",
                "The approval is organization-bound. Re-authorize it to change the guild or after Disconnect, and repeat it if the guild ownership changes.",
            ],
            "requiredPermissions": ["Administrator or Manage Server"],
        }
    if state == "bot_install_required":
        return {
            "kind": "human_setup",
            "state": state,
            "title": "Install the Discord bot in the verified guild",
            "instructions": [
                "The organization’s guild consent is verified, but the configured bot cannot find that guild.",
                "Install the bot in the same Server ID with View Channel, Read Message History, and Send Messages. Keep the bot’s permissions limited to those capabilities.",
                "Return here and refresh the connector; ForkMesh will only allow public, non-NSFW text or announcement channels.",
            ],
            "requiredPermissions": [
                "View Channel", "Read Message History", "Send Messages",
            ],
        }
    if state == "secret_required":
        return {
            "kind": "human_setup",
            "state": state,
            "title": "Configure the Discord connector secret",
            "instructions": [
                "Set a freshly rotated or reissued DISCORD_BOT_TOKEN as a Worker secret with the platform secret manager.",
                "Do not paste the bot token into ForkMesh, a repository, dashboard form, task, or chat message.",
                "Return here after the secret is set; the owner can then choose a guild and public channels.",
            ],
        }
    if state == "channel_required":
        return {
            "kind": "human_setup",
            "state": state,
            "title": "Select public Discord channels",
            "instructions": [
                "Choose the installed guild and one or more public text channels.",
                "ForkMesh will read and send only in the selected channels; private channels are rejected.",
            ],
        }
    if state == "authorization_required":
        return {
            "kind": "human_setup",
            "state": state,
            "title": "Restore Discord bot access",
            "instructions": [
                "Confirm the Worker secret is the active bot token and reinstall the bot in the selected guild if needed.",
                "Grant only View Channel, Read Message History, and Send Messages, then reselect public channels.",
            ],
            "requiredPermissions": [
                "View Channel", "Read Message History", "Send Messages",
            ],
        }
    if state == "message_content_required":
        return {
            "kind": "human_setup",
            "state": state,
            "title": "Enable Discord message content access",
            "instructions": [
                "Discord returned channel messages without readable text.",
                "In the Discord Developer Portal, open the application's Bot settings and enable Message Content privileged intent; request approval if Discord requires it for the application.",
                "Keep the channel permissions limited to View Channel, Read Message History, and Send Messages.",
            ],
            "requiredPermissions": [
                "View Channel", "Read Message History", "Send Messages",
            ],
        }
    return None


def _setup_task_record(task, actor):
    """Build the encrypted organization_tasks payload for a human setup card."""

    instructions = task.get("instructions") if isinstance(task, dict) else []
    details = "\n".join(
        "- " + _text(item, 800, multiline=True)
        for item in instructions if _text(item, 800, multiline=True)
    )
    return {
        "kind": "task",
        "title": _text(task.get("title"), 160, multiline=True),
        "details": _text(details, 4_000, multiline=True),
        "completionNote": "",
        "assignee": "",
        "createdBy": _text(actor, 64).lower(),
        "bountyRequest": None,
        "repository": "",
        "howToTest": (
            "Refresh the Discord connector. Confirm it no longer shows this "
            "setup requirement and only the selected public channels are available."
        ),
        "qaReviewer": "",
    }


async def _ensure_setup_task(runtime, context, state):
    """Idempotently create/reopen one unassigned setup task for this org.

    The marker table supplies a stable task id even if concurrent owner/admin
    requests observe the same missing configuration.  The task's words are
    encrypted in the normal organization task catalog, while the marker keeps
    only opaque ids.  This is intentionally callable only by owner/admin
    contexts, not by a regular member polling a connector status endpoint.
    """

    task = _setup_task(state)
    if not task or not _send_allowed(context):
        return ""
    org_bi = context["orgBi"]
    marker = await runtime.d1_first(
        "SELECT task_id FROM organization_discord_setup_tasks WHERE org_bi=?",
        org_bi,
    )
    created_marker = False
    if marker:
        task_id = str(marker.get("task_id") or "").lower()
    else:
        candidate = str(runtime.new_id() or "").lower()
        if not _TASK_ID_RE.fullmatch(candidate):
            return ""
        now = runtime.now()
        await runtime.d1_run(
            "INSERT OR IGNORE INTO organization_discord_setup_tasks "
            "(org_bi,task_id,created_at,updated_at) VALUES (?,?,?,?)",
            org_bi, candidate, now, now,
        )
        marker = await runtime.d1_first(
            "SELECT task_id FROM organization_discord_setup_tasks WHERE org_bi=?",
            org_bi,
        )
        task_id = str((marker or {}).get("task_id") or "").lower()
        created_marker = task_id == candidate
    if not _TASK_ID_RE.fullmatch(task_id):
        return ""
    now = runtime.now()
    sealed = await runtime.seal(_setup_task_record(task, context["actor"]))
    # Repair a marker that survived a transient task insert failure, then
    # reopen/update the one card to the newest setup state.  INSERT OR IGNORE
    # makes concurrent repair safe, while the fixed primary key prevents a
    # duplicate task even under a stale read.
    await runtime.d1_run(
        "INSERT OR IGNORE INTO organization_tasks "
        "(task_id,org_bi,department,team,destination,assignee_kind,status,"
        "assignee_bi,active_assignee_bi,data,created_by_bi,created_at,"
        "updated_at,elapsed_ms,started_at,next_checkin_at,completed_at,"
        "qa_status,qa_reviewer_bi,qa_reviewed_at,qa_requested_at,agent_session_id) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
        task_id, org_bi, "infrastructure", "", "department", "unassigned",
        "idle", "", "", sealed, context["accountBi"], now, now, 0, 0, 0,
        0, "unknown", "", 0, 0, "",
    )
    await runtime.d1_run(
        "UPDATE organization_tasks SET department=?,team='',"
        "destination='department',assignee_kind='unassigned',status='idle',"
        "assignee_bi='',active_assignee_bi='',data=?,updated_at=?,"
        "elapsed_ms=0,started_at=0,next_checkin_at=0,completed_at=0,"
        "qa_status='unknown',qa_reviewer_bi='',qa_reviewed_at=0,"
        "qa_requested_at=0,agent_session_id='' "
        "WHERE task_id=? AND org_bi=?",
        "infrastructure", sealed, now, task_id, org_bi,
    )
    await runtime.d1_run(
        "UPDATE organization_discord_setup_tasks SET updated_at=? "
        "WHERE org_bi=? AND task_id=?",
        now, org_bi, task_id,
    )
    if created_marker:
        await runtime.audit(
            context["actor"], "organization.discord_setup_task_create",
            "organization_task", task_id, "success", {"state": state},
        )
    return task_id


async def _resolve_setup_task(runtime, context):
    """Mark the deduplicated setup card done once connector setup succeeds."""

    if not _send_allowed(context):
        return
    row = await runtime.d1_first(
        "SELECT s.task_id,t.completed_at "
        "FROM organization_discord_setup_tasks s "
        "LEFT JOIN organization_tasks t "
        "ON t.task_id=s.task_id AND t.org_bi=s.org_bi "
        "WHERE s.org_bi=?",
        context["orgBi"],
    )
    task_id = str((row or {}).get("task_id") or "").lower()
    if not _TASK_ID_RE.fullmatch(task_id):
        return
    if int((row or {}).get("completed_at") or 0) > 0:
        return
    now = runtime.now()
    await runtime.d1_run(
        "UPDATE organization_tasks SET status='idle',active_assignee_bi='',"
        "started_at=0,next_checkin_at=0,completed_at=?,updated_at=? "
        "WHERE task_id=? AND org_bi=?",
        now, now, task_id, context["orgBi"],
    )
    await runtime.d1_run(
        "UPDATE organization_discord_setup_tasks SET updated_at=? "
        "WHERE org_bi=? AND task_id=?",
        now, context["orgBi"], task_id,
    )


async def _attach_setup_task(runtime, context, payload):
    """Attach the durable task id to an API setup notice for owner/admin UI."""

    task = payload.get("humanTask") if isinstance(payload, dict) else None
    state = str((task or {}).get("state") or "")
    task_id = await _ensure_setup_task(runtime, context, state)
    if task_id and isinstance(task, dict):
        payload = dict(payload)
        payload["humanTask"] = dict(task, organizationTaskId=task_id)
    return payload


async def _body(runtime):
    data, error = await runtime.json_body(BODY_MAX_BYTES)
    if error:
        return None, _response(
            runtime,
            {"error": error},
            status=413 if error == "payload_too_large" else 400,
        )
    return data, None


async def _context(runtime, org, data=None):
    account_bi, account = await runtime.session(data or {})
    actor = _text((account or {}).get("name"), 64).lower()
    if not account_bi or not actor:
        return None, _response(runtime, {"error": "invalid_session"}, 401)
    org_bi, row = await runtime.organization(org)
    if not row:
        return None, _response(runtime, {"error": "not_found"}, 404)
    role = await runtime.org_role(org_bi, actor)
    if role not in ORG_ROLES:
        return None, _response(runtime, {"error": "forbidden"}, 403)
    return {
        "accountBi": str(account_bi),
        "actor": actor,
        "org": _text(row.get("name") or org, 64).lower(),
        "orgBi": str(org_bi),
        "role": role,
    }, None


def _owner(context):
    return str((context or {}).get("role") or "") == "owner"


def _send_allowed(context):
    return str((context or {}).get("role") or "") in {"owner", "admin"}


async def _load_config(runtime, org_bi):
    row = await runtime.d1_first(
        "SELECT data,updated_by_bi,updated_at "
        "FROM organization_discord_connectors WHERE org_bi=?",
        org_bi,
    )
    if not row:
        return None, None
    try:
        record = await runtime.open(row.get("data"))
    except Exception:
        record = None
    return _stored_config(record), row


def _stored_grant(record, now=0):
    """Validate the minimal, encrypted proof that binds an org to one guild.

    OAuth access and refresh tokens never enter this record.  The grant is
    deliberately useful only as evidence of a recent authorization-code flow
    for one Discord user and one requested guild.
    """

    if not isinstance(record, dict):
        return None
    guild_id = _snowflake(record.get("guildId"))
    discord_user_id = _snowflake(record.get("discordUserId"))
    proof = str(record.get("permissionProof") or "")
    try:
        verified_at = int(record.get("verifiedAt") or 0)
        expires_at = int(record.get("expiresAt") or 0)
    except (TypeError, ValueError, OverflowError):
        return None
    if (
        not guild_id
        or not discord_user_id
        or proof not in {"guild_owner", "administrator", "manage_guild"}
        or verified_at <= 0
        or (expires_at and expires_at <= verified_at)
        or (now and expires_at and expires_at <= int(now))
    ):
        return None
    return {
        "guildId": guild_id,
        "discordUserId": discord_user_id,
        "permissionProof": proof,
        "verifiedAt": verified_at,
        "expiresAt": expires_at,
    }


async def _load_grant(runtime, org_bi, now=0):
    row = await runtime.d1_first(
        "SELECT data,verified_by_bi,verified_at,updated_at "
        "FROM organization_discord_oauth_grants WHERE org_bi=?",
        org_bi,
    )
    if not row:
        return None, None
    try:
        record = await runtime.open(row.get("data"))
    except Exception:
        record = None
    grant = _stored_grant(record, now)
    verifier = str(row.get("verified_by_bi") or "")
    membership = await runtime.d1_first(
        "SELECT role FROM org_members WHERE org_bi=? AND member_bi=?",
        org_bi, verifier,
    )
    if grant and membership and membership.get("role") == "owner":
        return grant, row
    # Consent belongs to the current organization owner, not merely to the
    # account that happened to be owner when OAuth completed. Revoke all
    # connector state immediately after a handoff, demotion, or membership
    # removal so a former owner cannot leave a live Discord bridge behind.
    await runtime.d1_run(
        "DELETE FROM organization_discord_connectors WHERE org_bi=?", org_bi)
    await runtime.d1_run(
        "DELETE FROM organization_discord_oauth_grants WHERE org_bi=?", org_bi)
    await runtime.d1_run(
        "DELETE FROM organization_discord_oauth_states WHERE org_bi=?", org_bi)
    return None, row


def _oauth_state_hash(value):
    return hashlib.sha256(str(value or "").encode("utf-8")).hexdigest()


def _new_oauth_secret(runtime):
    """Build one 256-bit opaque state/verifier value from Worker randomness."""

    first = str(runtime.new_id() or "").lower()
    second = str(runtime.new_id() or "").lower()
    value = first + second
    return value if _OAUTH_STATE_RE.fullmatch(value) else ""


def _code_challenge(verifier):
    digest = hashlib.sha256(str(verifier or "").encode("utf-8")).digest()
    # RFC 7636 base64url encoding without padding.
    import base64
    return base64.urlsafe_b64encode(digest).decode("ascii").rstrip("=")


async def _oauth_grant_required(runtime, context):
    """Fail closed before every bot API call until a recent owner grant exists."""

    # Existing encrypted organization consent stays usable if OAuth client
    # secrets rotate. OAuth configuration is required only to create/change a
    # grant, never as a dependency for an already verified bridge.
    grant, _row = await _load_grant(runtime, context["orgBi"], runtime.now())
    if grant:
        return grant, None
    if not runtime.discord_oauth_ready():
        payload = {
            "ok": True,
            "organization": context["org"],
            "role": context["role"],
            "configured": False,
            "connector": None,
            "error": "discord_oauth_client_required",
            "state": "oauth_client_required",
            "humanTask": _setup_task("oauth_client_required"),
        }
        return None, _response(
            runtime, await _attach_setup_task(runtime, context, payload), 409)
    payload = {
        "ok": True,
        "organization": context["org"],
        "role": context["role"],
        "configured": False,
        "connector": None,
        "error": "discord_guild_authorization_required",
        "state": "guild_authorization_required",
        "humanTask": _setup_task("guild_authorization_required"),
    }
    return None, _response(
        runtime, await _attach_setup_task(runtime, context, payload), 409)


async def _store_oauth_state(runtime, context, guild_id):
    """Persist a one-time encrypted PKCE state bound to this login/session."""

    state = _new_oauth_secret(runtime)
    verifier = _new_oauth_secret(runtime)
    if not state or not verifier:
        return "", "", ""
    # Keep the same one-time state in the secure HttpOnly callback cookie.
    # Discord normally returns `state` in the query, but its advanced bot
    # installation flow can return a malformed/empty state for some clients.
    # The cookie provides a same-browser fallback without weakening the D1
    # one-time claim, PKCE, session, owner, redirect, or guild checks.
    transaction = state
    session_id = str(await runtime.session_id() or "")
    if not session_id:
        return "", "", ""
    now = runtime.now()
    data = {
        "state": state,
        "verifier": verifier,
        "orgBi": context["orgBi"],
        "accountBi": context["accountBi"],
        "actor": context["actor"],
        "sessionId": session_id,
        "transaction": transaction,
        "guildId": guild_id,
        "redirectUri": runtime.discord_oauth_redirect_uri(),
        "createdAt": now,
    }
    sealed = await runtime.seal(data)
    await runtime.d1_run(
        "DELETE FROM organization_discord_oauth_states WHERE expires_at<=?",
        now,
    )
    await runtime.d1_run(
        "INSERT OR REPLACE INTO organization_discord_oauth_states "
        "(state_hash,org_bi,data,created_at,expires_at) VALUES (?,?,?,?,?)",
        _oauth_state_hash(state), context["orgBi"], sealed, now,
        now + OAUTH_STATE_TTL_MS,
    )
    return state, _code_challenge(verifier), transaction


def _config_projection(config):
    if not config:
        return None
    return {
        "guildId": config["guildId"],
        "channelIds": list(config["channelIds"]),
    }


def _permission_value(value):
    try:
        value = int(str(value or "0"))
    except (TypeError, ValueError, OverflowError):
        return 0
    return max(0, value)


def _everyone_permissions(roles, guild_id):
    """Return the base @everyone role permissions, failing closed if absent."""

    if not isinstance(roles, list):
        return None
    for role in roles:
        if isinstance(role, dict) and _snowflake(role.get("id")) == guild_id:
            return _permission_value(role.get("permissions"))
    return None


def _apply_everyone_overwrite(permissions, overwrites, guild_id):
    """Apply one category/channel @everyone overwrite in Discord order."""

    if not isinstance(overwrites, list):
        return permissions
    for item in overwrites:
        if not isinstance(item, dict):
            continue
        if str(item.get("type")) not in {"0", "role"}:
            continue
        if _snowflake(item.get("id")) != guild_id:
            continue
        denied = _permission_value(item.get("deny"))
        allowed = _permission_value(item.get("allow"))
        return (permissions & ~denied) | allowed
    return permissions


def _is_public_channel(item, parent, guild_id, everyone_permissions):
    """Determine whether Discord's @everyone role can view a channel.

    A deny-only check is not enough: a guild can start with View Channel
    disabled and explicitly allow one channel, or it can start enabled and
    revoke it there. Discord copies category overwrites into a synchronized
    child; it does not dynamically inherit them. Applying the parent again
    would therefore expose a de-synchronized private child. We apply only the
    child's explicit @everyone overwrite and ignore member-specific grants.
    """

    if (
        everyone_permissions is None
        or bool(item.get("nsfw"))
        or (parent and bool(parent.get("nsfw")))
    ):
        return False
    permissions = everyone_permissions
    permissions = _apply_everyone_overwrite(
        permissions, item.get("permission_overwrites"), guild_id)
    if permissions & ADMINISTRATOR_PERMISSION:
        return True
    return bool(
        permissions & VIEW_CHANNEL_PERMISSION
        and permissions & READ_MESSAGE_HISTORY_PERMISSION)


def _public_channels(payload, roles, guild_id):
    """Project only public, non-NSFW text/announcement channels.

    Threads (including public/private/news threads) are excluded by the small
    type allowlist.  The guild roles endpoint is required so the @everyone
    base permissions and category/channel overrides can be evaluated rather
    than guessed from a deny bit alone.
    """

    if not isinstance(payload, list):
        return []
    everyone_permissions = _everyone_permissions(roles, guild_id)
    by_id = {
        _snowflake(item.get("id")): item
        for item in payload
        if isinstance(item, dict) and _snowflake(item.get("id"))
    }
    public = []
    for channel_id, item in by_id.items():
        try:
            channel_type = int(item.get("type"))
        except (TypeError, ValueError):
            continue
        if channel_type not in PUBLIC_TEXT_CHANNEL_TYPES:
            continue
        parent_id = _snowflake(item.get("parent_id"))
        parent = by_id.get(parent_id) if parent_id else None
        # A partial provider payload must never turn an attached channel into
        # an apparently parentless public channel.
        if parent_id and parent is None:
            continue
        if not _is_public_channel(
                item, parent, guild_id, everyone_permissions):
            continue
        public.append({
            "id": channel_id,
            "name": _text(item.get("name"), 100),
            "type": channel_type,
        })
    return sorted(public, key=lambda item: (item["name"].lower(), item["id"]))


async def _discord_error(runtime, result, context=None):
    status = int((result or {}).get("status") or 0)
    retry_after = int((result or {}).get("retryAfterMs") or 0)
    if status == 429:
        retry_ms = max(0, min(retry_after, 3_600_000))
        return _response(
            runtime,
            {
                "error": "discord_rate_limited",
                "retryAfterMs": retry_ms,
            },
            429,
            extra_headers={
                "retry-after": str(max(1, (retry_ms + 999) // 1000)),
            },
        )
    if status in {401, 403}:
        payload = {
            "error": "discord_authorization_required",
            "state": "authorization_required",
            "humanTask": _setup_task("authorization_required"),
        }
        if context:
            payload = await _attach_setup_task(runtime, context, payload)
        return _response(
            runtime, payload, 503,
            extra_headers=EXPECTED_DEGRADED_HEADERS)
    return _response(
        runtime, {"error": "discord_unavailable"}, 502,
        extra_headers=EXPECTED_DEGRADED_HEADERS)


async def _guild_missing(runtime, context):
    """Explain a verified guild that is no longer installed/reachable."""

    payload = {
        "error": "discord_guild_missing",
        "state": "bot_install_required",
        "humanTask": _setup_task("bot_install_required"),
    }
    return _response(
        runtime, await _attach_setup_task(runtime, context, payload), 409)


async def _read_public_channels(runtime, guild_id, use_cache=True):
    if use_cache:
        cached = runtime.discord_public_channels_cache(guild_id)
        if isinstance(cached, list):
            return cached, None
    channel_result = await runtime.discord_channels(guild_id)
    if int(channel_result.get("status") or 0) != 200:
        return None, channel_result
    roles_result = await runtime.discord_guild_roles(guild_id)
    if int(roles_result.get("status") or 0) != 200:
        return None, roles_result
    channels = _public_channels(
        channel_result.get("data"), roles_result.get("data"), guild_id)
    runtime.cache_discord_public_channels(guild_id, channels)
    return channels, None


async def _public_guild_channels(runtime, context, guild_id, use_cache=True):
    """Fetch a bounded, fail-closed public-channel projection for one grant."""

    channels, failed = await _read_public_channels(runtime, guild_id, use_cache)
    if failed is None:
        return channels, None
    if int(failed.get("status") or 0) == 404:
        return None, await _guild_missing(runtime, context)
    return None, await _discord_error(runtime, failed, context)


async def _owner_status(runtime, context):
    """Return a preview only after an org-bound OAuth grant is verified.

    This deliberately has no ``?guildId=`` escape hatch.  A bot's ability to
    see a guild is global to that bot and is not proof that this ForkMesh
    organization has consent to read or send there.
    """

    grant, grant_error = await _oauth_grant_required(runtime, context)
    if grant_error:
        return None, grant_error
    config, _row = await _load_config(runtime, context["orgBi"])
    if config and config["guildId"] != grant["guildId"]:
        config = None
    if not runtime.discord_ready():
        payload = {
            "ok": True,
            "organization": context["org"],
            "role": context["role"],
            "configured": bool(config),
            "state": "secret_required",
            "connector": _config_projection(config),
            "guildId": grant["guildId"],
            "channels": [],
            "humanTask": _setup_task("secret_required"),
        }
        return await _attach_setup_task(runtime, context, payload), None
    channels, error = await _public_guild_channels(
        runtime, context, grant["guildId"])
    if error:
        return None, error
    state = "configured" if config else "channel_required"
    human_task = None if config else _setup_task("channel_required")
    if config and not set(config["channelIds"]).issubset(
            {item["id"] for item in channels}):
        state = "channel_required"
        human_task = _setup_task("channel_required")
    payload = {
        "ok": True,
        "organization": context["org"],
        "role": context["role"],
        "configured": bool(config) and state == "configured",
        "state": state,
        "connector": _config_projection(config),
        "guildId": grant["guildId"],
        "channels": channels,
        "humanTask": human_task,
        "grant": {
            "verifiedAt": grant["verifiedAt"],
            "expiresAt": grant["expiresAt"],
        },
    }
    if state == "configured":
        await _resolve_setup_task(runtime, context)
    return await _attach_setup_task(runtime, context, payload), None


async def _member_status(runtime, context):
    config, _row = await _load_config(runtime, context["orgBi"])
    grant, _grant_row = await _load_grant(
        runtime, context["orgBi"], runtime.now())
    state = "configured" if grant and config and (
        config["guildId"] == grant["guildId"]) else "guild_authorization_required"
    return {
        "ok": True,
        "organization": context["org"],
        "role": context["role"],
        "configured": bool(config) and state == "configured",
        "state": state,
        "connector": _config_projection(config),
        # A non-owner can see that a setup is pending, but cannot enumerate a
        # guild or change the organization's connector policy.
        "humanTask": None,
    }


def _oauth_code(value):
    """Keep the transient authorization code bounded without normalizing it."""

    value = str(value or "").strip()
    if (
        len(value) < 8
        or len(value) > 2_048
        or any(not char.isprintable() or char.isspace() for char in value)
    ):
        return ""
    return value


async def _oauth_start(runtime, context, data):
    """Create a one-time owner/session-bound PKCE authorization request."""

    if not _owner(context):
        return _response(runtime, {"error": "owner_required"}, 403)
    if not runtime.discord_oauth_ready():
        payload = {
            "error": "discord_oauth_client_required",
            "state": "oauth_client_required",
            "humanTask": _setup_task("oauth_client_required"),
        }
        return _response(
            runtime, await _attach_setup_task(runtime, context, payload), 409)
    body = dict(data or {})
    body.pop("sessionToken", None)
    if set(body) - {"guildId"}:
        return _response(runtime, {"error": "unsupported_oauth_start_field"}, 400)
    guild_id = _snowflake(body.get("guildId"))
    if not guild_id:
        return _response(runtime, {"error": "invalid_guild_id"}, 400)
    state, challenge, transaction = await _store_oauth_state(
        runtime, context, guild_id)
    if not state or not challenge or not transaction:
        # A state missing the validated ForkMesh session binding must never be
        # sent to Discord; a later callback would otherwise be ambiguous.
        return _response(runtime, {"error": "session_binding_required"}, 401)
    await runtime.audit(
        context["actor"], "organization.discord_oauth_start",
        "organization_discord", context["org"], "success", {})
    return runtime.oauth_start_response({
        "ok": True,
        "organization": context["org"],
        "authorizationUrl": runtime.discord_oauth_authorization_url(
            state, challenge),
        "expiresAt": runtime.now() + OAUTH_STATE_TTL_MS,
    }, transaction)


async def _consume_oauth_state(runtime):
    """Load and immediately consume a single short-lived encrypted state."""

    state = str(runtime.query("state") or "").lower()
    if not _OAUTH_STATE_RE.fullmatch(state):
        state = str(runtime.oauth_transaction_cookie() or "").lower()
    if not _OAUTH_STATE_RE.fullmatch(state):
        return None, "invalid_state_format"
    state_hash = _oauth_state_hash(state)
    row = await runtime.d1_first(
        "SELECT data,expires_at FROM organization_discord_oauth_states "
        "WHERE state_hash=?", state_hash)
    if not row:
        return None, "invalid_state_missing"
    if int(row.get("expires_at") or 0) <= runtime.now():
        await runtime.d1_run(
            "DELETE FROM organization_discord_oauth_states "
            "WHERE state_hash=? AND data=?", state_hash, row.get("data"))
        return None, "invalid_state_expired"
    encrypted = str(row.get("data") or "")
    claim = "consumed:" + _new_oauth_secret(runtime)
    if not encrypted or claim == "consumed:":
        return None, "invalid_record_storage"
    # D1's Worker API documents write-operation result sets as empty, so
    # DELETE ... RETURNING cannot be consumed through PreparedStatement.first.
    # Claim with a compare-and-swap, then read the marker back: only one
    # concurrent callback can own this exact encrypted row.
    await runtime.d1_run(
        "UPDATE organization_discord_oauth_states SET data=?,expires_at=0 "
        "WHERE state_hash=? AND data=? AND expires_at=?",
        claim, state_hash, encrypted, int(row.get("expires_at") or 0))
    claimed = await runtime.d1_first(
        "SELECT data FROM organization_discord_oauth_states "
        "WHERE state_hash=?", state_hash)
    if not claimed or not hmac.compare_digest(
            str(claimed.get("data") or ""), claim):
        return None, "invalid_state_claim"
    await runtime.d1_run(
        "DELETE FROM organization_discord_oauth_states "
        "WHERE state_hash=? AND data=?", state_hash, claim)
    try:
        record = await runtime.open(encrypted)
    except Exception:
        record = None
    if not isinstance(record, dict):
        return None, "invalid_record_decrypt"
    if not hmac.compare_digest(str(record.get("state") or ""), state):
        return None, "invalid_record_state"
    verifier = str(record.get("verifier") or "")
    if not _OAUTH_STATE_RE.fullmatch(verifier):
        return None, "invalid_record_verifier"
    return record, ""


def _oauth_callback_browser_bound(runtime, record):
    state = str(record.get("state") or "").lower()
    cookie = str(runtime.oauth_transaction_cookie(state) or "").lower()
    return bool(state) and hmac.compare_digest(
        cookie.encode("utf-8"), state.encode("utf-8"))


async def _oauth_callback_context(runtime, record):
    account_bi = str(record.get("accountBi") or "")
    actor = _text(record.get("actor"), 64).lower()
    session_id = str(record.get("sessionId") or "")
    if not account_bi or not actor or not session_id:
        return None
    if (
        str(record.get("redirectUri") or "")
        != str(runtime.discord_oauth_redirect_uri() or "")
    ):
        return None
    org_bi = str(record.get("orgBi") or "")
    if (
        not org_bi
        or not await runtime.session_active(account_bi, session_id)
        or await runtime.org_role(org_bi, actor) != "owner"
    ):
        return None
    return {
        "accountBi": str(account_bi),
        "actor": actor,
        "orgBi": org_bi,
        # The callback's safe redirect does not need the org alias.  Keep this
        # field opaque in audit calls rather than trusting a URL segment.
        "org": "organization",
        "role": "owner",
    }


def _oauth_permission_proof(guilds, requested_guild_id):
    """Return the exact OAuth permission proof for the requested guild only."""

    if not isinstance(guilds, list):
        return ""
    for guild in guilds:
        if not isinstance(guild, dict):
            continue
        if _snowflake(guild.get("id")) != requested_guild_id:
            continue
        if bool(guild.get("owner")):
            return "guild_owner"
        permissions = _permission_value(
            guild.get("permissions_new") or guild.get("permissions"))
        if permissions & ADMINISTRATOR_PERMISSION:
            return "administrator"
        if permissions & MANAGE_GUILD_PERMISSION:
            return "manage_guild"
        return ""
    return ""


async def handle_oauth_callback(runtime):
    """Complete OAuth in a no-store, query-stripping callback response.

    The short-lived OAuth bearer exists only in local variables during the
    three fixed Discord calls below.  It is never sealed, audited, returned, or
    logged.  Only the resulting encrypted guild/user/permission proof survives.
    """

    await runtime.ensure_schema()
    if str(runtime.method() or "GET").upper() != "GET":
        return runtime.oauth_callback_response("invalid")
    # Consume an otherwise valid state on *every* callback, including a user
    # denial or a just-rotated OAuth client secret, so a stale authorization
    # attempt cannot later be replayed.
    record, invalid_outcome = await _consume_oauth_state(runtime)
    if str(runtime.query("error") or ""):
        return runtime.oauth_callback_response("denied")
    if not runtime.discord_oauth_ready():
        return runtime.oauth_callback_response("setup")
    code = _oauth_code(runtime.query("code"))
    if not code:
        return runtime.oauth_callback_response("invalid_code")
    if not record:
        return runtime.oauth_callback_response(
            invalid_outcome or "invalid_record_storage")
    context = await _oauth_callback_context(runtime, record)
    if not context or not _oauth_callback_browser_bound(runtime, record):
        return runtime.oauth_callback_response("invalid_context")
    requested_guild_id = _snowflake(record.get("guildId"))
    if not requested_guild_id:
        return runtime.oauth_callback_response("invalid_record_guild")
    exchanged = await runtime.discord_oauth_exchange(code, record["verifier"])
    access_token = str((exchanged or {}).get("accessToken") or "")
    if int((exchanged or {}).get("status") or 0) != 200 or not access_token:
        return runtime.oauth_callback_response("failed")
    identity = await runtime.discord_oauth_identity(access_token)
    discord_user_id = _snowflake((identity.get("data") or {}).get("id"))
    if int(identity.get("status") or 0) != 200 or not discord_user_id:
        return runtime.oauth_callback_response("failed")
    guilds = await runtime.discord_oauth_guilds(access_token)
    proof = _oauth_permission_proof(guilds.get("data"), requested_guild_id)
    if int(guilds.get("status") or 0) != 200 or not proof:
        return runtime.oauth_callback_response("denied")
    now = runtime.now()
    grant = {
        "guildId": requested_guild_id,
        "discordUserId": discord_user_id,
        "permissionProof": proof,
        "verifiedAt": now,
        "expiresAt": (now + OAUTH_GRANT_TTL_MS) if OAUTH_GRANT_TTL_MS else 0,
    }
    sealed = await runtime.seal(grant)
    await runtime.d1_run(
        "INSERT INTO organization_discord_oauth_grants "
        "(org_bi,data,verified_by_bi,verified_at,updated_at) "
        "VALUES (?,?,?,?,?) ON CONFLICT(org_bi) DO UPDATE SET "
        "data=excluded.data,verified_by_bi=excluded.verified_by_bi,"
        "verified_at=excluded.verified_at,updated_at=excluded.updated_at",
        context["orgBi"], sealed, context["accountBi"], now, now,
    )
    config, _row = await _load_config(runtime, context["orgBi"])
    if config and config["guildId"] != requested_guild_id:
        await runtime.d1_run(
            "DELETE FROM organization_discord_connectors WHERE org_bi=?",
            context["orgBi"],
        )
    await runtime.audit(
        context["actor"], "organization.discord_oauth_grant",
        "organization_discord", context["org"], "success",
        {"permissionProof": proof},
    )
    next_state = "secret_required" if not runtime.discord_ready() else "channel_required"
    await _attach_setup_task(runtime, context, {
        "humanTask": _setup_task(next_state),
    })
    return runtime.oauth_callback_response("connected")


async def _get_config(runtime, context):
    if _owner(context):
        value, error = await _owner_status(runtime, context)
        return error or _response(runtime, value)
    return _response(runtime, await _member_status(runtime, context))


async def _put_config(runtime, context, data):
    if not _owner(context):
        await runtime.audit(
            context["actor"], "organization.discord_configure",
            "organization_discord", context["org"], "denied",
            {"reason": "owner_required"},
        )
        return _response(runtime, {"error": "owner_required"}, 403)
    config_data = dict(data or {})
    # Session compatibility payloads may carry this existing ForkMesh bearer
    # marker.  It is consumed only by the runtime session resolver and is
    # explicitly removed before validation/encryption.
    config_data.pop("sessionToken", None)
    config, error = _normalize_config(config_data)
    if error:
        return _response(runtime, {"error": error}, 400)
    grant, grant_error = await _oauth_grant_required(runtime, context)
    if grant_error:
        return grant_error
    if config["guildId"] != grant["guildId"]:
        return _response(runtime, {
            "error": "guild_grant_mismatch",
            "state": "guild_authorization_required",
        }, 409)
    if not runtime.discord_ready():
        payload = {
            "error": "discord_secret_required",
            "state": "secret_required",
            "humanTask": _setup_task("secret_required"),
        }
        return _response(
            runtime, await _attach_setup_task(runtime, context, payload), 409)
    channels, channel_error = await _public_guild_channels(
        runtime, context, config["guildId"], use_cache=False)
    if channel_error:
        return channel_error
    public_ids = {
        item["id"] for item in channels
    }
    if not set(config["channelIds"]).issubset(public_ids):
        return _response(runtime, {"error": "selected_channel_not_public"}, 400)
    now = runtime.now()
    sealed = await runtime.seal(config)
    await runtime.d1_run(
        "INSERT INTO organization_discord_connectors "
        "(org_bi,data,updated_by_bi,updated_at) VALUES (?,?,?,?) "
        "ON CONFLICT(org_bi) DO UPDATE SET data=excluded.data,"
        "updated_by_bi=excluded.updated_by_bi,updated_at=excluded.updated_at",
        context["orgBi"], sealed, context["accountBi"], now,
    )
    await runtime.audit(
        context["actor"], "organization.discord_configure",
        "organization_discord", context["org"], "success",
        {"selectedChannels": len(config["channelIds"])},
    )
    await _resolve_setup_task(runtime, context)
    return _response(runtime, {
        "ok": True,
        "organization": context["org"],
        "configured": True,
        "state": "configured",
        "connector": _config_projection(config),
    })


async def _delete_config(runtime, context):
    if not _owner(context):
        return _response(runtime, {"error": "owner_required"}, 403)
    await runtime.d1_run(
        "DELETE FROM organization_discord_connectors WHERE org_bi=?",
        context["orgBi"],
    )
    # Disconnect revokes organization consent as well as the channel allowlist.
    # A later reconnect must complete a new owner OAuth authorization.
    await runtime.d1_run(
        "DELETE FROM organization_discord_oauth_grants WHERE org_bi=?",
        context["orgBi"],
    )
    await runtime.d1_run(
        "DELETE FROM organization_discord_oauth_states WHERE org_bi=?",
        context["orgBi"],
    )
    await runtime.audit(
        context["actor"], "organization.discord_disconnect",
        "organization_discord", context["org"], "success", {},
    )
    payload = {
        "ok": True,
        "organization": context["org"],
        "configured": False,
        "state": "guild_authorization_required",
        "humanTask": _setup_task("guild_authorization_required"),
    }
    return _response(runtime, await _attach_setup_task(runtime, context, payload))


async def _usable_config(runtime, context):
    config, _row = await _load_config(runtime, context["orgBi"])
    grant, grant_error = await _oauth_grant_required(runtime, context)
    if grant_error:
        return None, grant_error
    if not config:
        payload = {
            "error": "discord_not_configured",
            "state": "channel_required",
            "humanTask": _setup_task("channel_required") if _owner(context) else None,
        }
        return None, _response(
            runtime, await _attach_setup_task(runtime, context, payload), 409)
    if config["guildId"] != grant["guildId"]:
        payload = {
            "error": "guild_grant_mismatch",
            "state": "guild_authorization_required",
            "humanTask": (
                _setup_task("guild_authorization_required")
                if _owner(context) else None),
        }
        return None, _response(
            runtime, await _attach_setup_task(runtime, context, payload), 409)
    if not runtime.discord_ready():
        payload = {
            "error": "discord_secret_required",
            "state": "secret_required",
            "humanTask": _setup_task("secret_required") if _owner(context) else None,
        }
        return None, _response(
            runtime, await _attach_setup_task(runtime, context, payload), 503)
    return config, None


def _count(value):
    try:
        return max(0, int(value or 0))
    except (TypeError, ValueError, OverflowError):
        return 0


def _summary_error(result):
    return {
        401: "discord_authorization_required",
        403: "discord_authorization_required",
        404: "discord_guild_missing",
        429: "discord_rate_limited",
    }.get(int((result or {}).get("status") or 0), "discord_unavailable")


async def guild_summary(runtime, org_bi):
    grant, _row = await _load_grant(runtime, org_bi, runtime.now())
    if not grant:
        return {"ok": False, "error": "discord_connector_not_connected"}
    if not runtime.discord_ready():
        return {"ok": False, "error": "discord_secret_required"}
    result = await runtime.discord_guild(grant["guildId"])
    status = int((result or {}).get("status") or 0)
    if status != 200:
        return {"ok": False, "error": _summary_error(result)}
    guild = result.get("data") if isinstance(result.get("data"), dict) else {}
    if _snowflake(guild.get("id")) != grant["guildId"]:
        return {"ok": False, "error": "discord_unavailable"}
    vanity = str(guild.get("vanity_url_code") or "")
    return {
        "ok": True,
        "guildId": grant["guildId"],
        "name": _text(guild.get("name"), 100),
        "description": _text(guild.get("description"), 1000),
        "vanityCode": vanity if re.fullmatch(r"[A-Za-z0-9-]{1,32}", vanity) else "",
        "memberCount": _count(guild.get("approximate_member_count")),
        "presenceCount": _count(guild.get("approximate_presence_count")),
    }


def _message_projection(payload, channel_id):
    if not isinstance(payload, dict):
        return None
    message_id = _snowflake(payload.get("id"))
    if not message_id:
        return None
    author = payload.get("author") if isinstance(payload.get("author"), dict) else {}
    author_id = _snowflake(author.get("id"))
    avatar_hash = _text(author.get("avatar"), 128)
    avatar_url = ""
    if (
        author_id
        and avatar_hash
        and re.fullmatch(r"[A-Za-z0-9_-]{2,128}", avatar_hash)
    ):
        avatar_url = (
            f"https://cdn.discordapp.com/avatars/{author_id}/"
            f"{avatar_hash}.webp?size=64"
        )
    elif author_id:
        avatar_url = (
            "https://cdn.discordapp.com/embed/avatars/"
            f"{(int(author_id) >> 22) % 6}.png"
        )
    projected_author = {
        "name": _text(
            author.get("global_name") or author.get("username"), 100),
        "bot": bool(author.get("bot")),
    }
    if avatar_url:
        projected_author["avatarUrl"] = avatar_url
    return {
        "id": message_id,
        "channelId": channel_id,
        "author": projected_author,
        "content": _text(payload.get("content"), MAX_MESSAGE_CONTENT, multiline=True),
        "createdAt": _text(payload.get("timestamp"), 40),
        "editedAt": _text(payload.get("edited_timestamp"), 40),
    }


def _posted_at(message_id):
    return (int(message_id) >> 22) + DISCORD_EPOCH_MS


async def recent_messages(runtime, org_bi, days):
    try:
        days = max(1, min(MAX_RECENT_DAYS, int(days)))
    except (TypeError, ValueError, OverflowError):
        days = MAX_RECENT_DAYS
    grant, _row = await _load_grant(runtime, org_bi, runtime.now())
    if not grant:
        return {"ok": False, "error": "discord_connector_not_connected"}
    if not runtime.discord_ready():
        return {"ok": False, "error": "discord_secret_required"}
    config, _row = await _load_config(runtime, org_bi)
    if not config or config["guildId"] != grant["guildId"]:
        return {"ok": False, "error": "discord_channel_required"}
    channels, failed = await _read_public_channels(runtime, grant["guildId"])
    if failed is not None:
        return {"ok": False, "error": _summary_error(failed)}
    public = {item["id"]: item for item in channels}
    selected = [public[channel_id] for channel_id in config["channelIds"]
                if channel_id in public][:MAX_RECENT_CHANNELS]
    if not selected:
        return {"ok": False, "error": "discord_channel_required"}
    since = int(runtime.now()) - days * 86_400_000
    messages = []
    truncated = False
    for channel in selected:
        result = await runtime.discord_messages(channel["id"], MAX_MESSAGES)
        if int((result or {}).get("status") or 0) != 200:
            return {"ok": False, "error": _summary_error(result)}
        raw = result.get("data") if isinstance(result.get("data"), list) else []
        in_window = 0
        for item in raw:
            projected = _message_projection(item, channel["id"])
            if not projected or _posted_at(projected["id"]) < since:
                continue
            in_window += 1
            projected["channelName"] = _text(channel.get("name"), 100)
            projected["postedAt"] = _posted_at(projected["id"])
            projected["url"] = (
                "https://discord.com/channels/" + grant["guildId"] + "/"
                + channel["id"] + "/" + projected["id"])
            messages.append(projected)
        truncated = truncated or (len(raw) >= MAX_MESSAGES and in_window == len(raw))
    messages.sort(key=lambda item: (item["postedAt"], item["id"]), reverse=True)
    return {
        "ok": True,
        "guildId": grant["guildId"],
        "days": days,
        "since": since,
        "channels": [{"id": item["id"], "name": _text(item.get("name"), 100)}
                     for item in selected],
        "messages": messages[:MAX_RECENT_MESSAGES],
        "truncated": truncated or len(messages) > MAX_RECENT_MESSAGES,
        "contentMissing": bool(messages) and all(
            not item["content"] for item in messages),
    }


async def _channel_available(runtime, context, config, channel_id):
    channels, error = await _public_guild_channels(
        runtime, context, config["guildId"], use_cache=False)
    if error:
        return None, error
    available = {
        item["id"]: item for item in channels
    }
    if channel_id not in available:
        payload = {
            "error": "selected_channel_unavailable",
            "state": "channel_required",
            "humanTask": (
                _setup_task("channel_required") if _owner(context) else None),
        }
        return None, _response(
            runtime, await _attach_setup_task(runtime, context, payload), 409)
    return available[channel_id], None


async def _list_messages(runtime, context):
    config, error = await _usable_config(runtime, context)
    if error:
        return error
    requested = _snowflake(runtime.query("channelId"))
    channel_id = requested or config["channelIds"][0]
    if channel_id not in config["channelIds"]:
        return _response(runtime, {"error": "channel_not_selected"}, 403)
    channel, error = await _channel_available(
        runtime, context, config, channel_id)
    if error:
        return error
    channel_id = channel["id"]
    result = await runtime.discord_messages(channel_id, MAX_MESSAGES)
    if int(result.get("status") or 0) != 200:
        return await _discord_error(runtime, result, context)
    raw_messages = (
        result.get("data") if isinstance(result.get("data"), list) else [])
    messages = []
    for item in raw_messages:
        projected = _message_projection(item, channel_id)
        if projected:
            messages.append(projected)
    # A list of messages with no readable content can be an expected
    # attachment-only history, but it is also the practical signal Discord
    # gives when Message Content privileged intent is unavailable.  Surface a
    # precise, non-fatal configuration task instead of rendering an unexplained
    # empty chat pane.  We do not infer or persist anything about the messages.
    content_missing = bool(messages) and all(
        not str(item.get("content") or "") for item in messages)
    payload = {
        "ok": True,
        "organization": context["org"],
        "channelId": channel_id,
        "channel": {
            "id": channel_id,
            "name": _text(channel.get("name"), 100),
        },
        "messages": messages[:MAX_MESSAGES],
    }
    if content_missing:
        payload["messageContentDiagnostic"] = {
            "state": "message_content_required",
            "humanTask": _setup_task("message_content_required"),
        }
        diagnostic = await _attach_setup_task(
            runtime, context,
            {"humanTask": payload["messageContentDiagnostic"]["humanTask"]},
        )
        payload["messageContentDiagnostic"]["humanTask"] = diagnostic[
            "humanTask"]
    return _response(runtime, payload)


async def _send_message(runtime, context, data):
    if not _send_allowed(context):
        return _response(runtime, {"error": "send_permission_required"}, 403)
    config, error = await _usable_config(runtime, context)
    if error:
        return error
    message_data = dict(data or {})
    message_data.pop("sessionToken", None)
    if set(message_data) - {"channelId", "content"}:
        return _response(runtime, {"error": "unsupported_message_field"}, 400)
    channel_id = _snowflake(message_data.get("channelId"))
    content = _text(
        message_data.get("content"), MAX_MESSAGE_CONTENT, multiline=True)
    if not channel_id or channel_id not in config["channelIds"]:
        return _response(runtime, {"error": "channel_not_selected"}, 403)
    if not content:
        return _response(runtime, {"error": "message_required"}, 400)
    channel, error = await _channel_available(
        runtime, context, config, channel_id)
    if error:
        return error
    channel_id = channel["id"]
    result = await runtime.discord_send(channel_id, content)
    if int(result.get("status") or 0) not in {200, 201}:
        return await _discord_error(runtime, result, context)
    message = _message_projection(result.get("data"), channel_id)
    if not message:
        return _response(runtime, {"error": "discord_unavailable"}, 502)
    await runtime.audit(
        context["actor"], "organization.discord_message_send",
        "organization_discord_channel", context["org"] + "/" + channel_id,
        "success", {"contentStored": False},
    )
    return _response(runtime, {"ok": True, "message": message}, 201)


async def handle(runtime, org, action=""):
    """Dispatch organization Discord configuration and OAuth start requests."""

    await runtime.ensure_schema()
    action = str(action or "").strip().lower()
    method = str(runtime.method() or "GET").upper()
    if action not in {"", "messages", "oauth/start"}:
        return _response(runtime, {"error": "not_found"}, 404)
    allowed = {
        "": {"GET", "PUT", "DELETE"},
        "messages": {"GET", "POST"},
        "oauth/start": {"POST"},
    }[action]
    if method not in allowed:
        return _response(
            runtime, {"error": "method_not_allowed"}, 405,
            allow=", ".join(sorted(allowed)),
        )
    if method in {"PUT", "POST", "DELETE"} and not runtime.same_origin():
        return _response(runtime, {"error": "origin_not_allowed"}, 403)
    data = {}
    if method in {"PUT", "POST"}:
        data, error = await _body(runtime)
        if error:
            return error
    context, error = await _context(runtime, org, data)
    if error:
        return error
    if action == "oauth/start":
        return await _oauth_start(runtime, context, data)
    if action == "":
        if method == "GET":
            return await _get_config(runtime, context)
        if method == "PUT":
            return await _put_config(runtime, context, data)
        return await _delete_config(runtime, context)
    if method == "GET":
        return await _list_messages(runtime, context)
    return await _send_message(runtime, context, data)
