from __future__ import annotations

import base64
import hmac
import json
import re

import apps_core as core
import apps_events as events
from organization_rbac import ORG_INSIDER_ROLES


MAX_BODY = 64 * 1024
COMMAND_PATHS = ("/api/apps/commands", "/api/apps/commands/run",
                 "/api/apps/interact", "/api/apps/mentions")
MANAGER_ROLES = ("owner", "admin")
INSTALL_ROLES = tuple(role for role in ORG_INSIDER_ROLES if role != "auditor")

ROUTES = (
    ("apps", re.compile(r"^/api/apps/?$")),
    ("scopes", re.compile(r"^/api/apps/scopes/?$")),
    ("installed", re.compile(r"^/api/apps/installed/?$")),
    ("install", re.compile(r"^/api/apps/installs/([0-9a-f]{32})/?$")),
    ("authorize", re.compile(r"^/api/apps/oauth/authorize/?$")),
    ("token", re.compile(r"^/api/apps/oauth/token/?$")),
    ("revoke", re.compile(r"^/api/apps/oauth/revoke/?$")),
    ("org_policy", re.compile(r"^/api/apps/orgs/([a-z0-9-]{1,64})/policy/?$")),
    ("org_requests",
     re.compile(r"^/api/apps/orgs/([a-z0-9-]{1,64})/requests/?$")),
    ("org_request", re.compile(
        r"^/api/apps/orgs/([a-z0-9-]{1,64})/requests/([0-9a-f]{32})/?$")),
    ("app", re.compile(r"^/api/apps/([0-9a-f]{32})/?$")),
    ("rotate", re.compile(r"^/api/apps/([0-9a-f]{32})/rotate/?$")),
    ("verify", re.compile(r"^/api/apps/([0-9a-f]{32})/verify/?$")),
    ("submit", re.compile(r"^/api/apps/([0-9a-f]{32})/submit/?$")),
    ("review_one", re.compile(r"^/api/apps/([0-9a-f]{32})/review/?$")),
    ("review", re.compile(r"^/api/apps/review/?$")),
    ("directory", re.compile(r"^/api/apps/directory/?$")),
    ("deliveries", re.compile(r"^/api/apps/([0-9a-f]{32})/deliveries/?$")),
)


def _json(runtime, data, status=200, headers=None):
    return runtime.response(data, status=status, cache_control="no-store",
                            extra_headers=headers)


def _err(runtime, error, status=400, **extra):
    body = {"ok": False, "error": error}
    body.update(extra)
    return _json(runtime, body, status)


def _oauth_err(runtime, error, status=400, description=""):
    body = {"error": error}
    if description:
        body["error_description"] = description
    return _json(runtime, body, status, {"pragma": "no-cache"})


def _scopes_from_text(text):
    return core.parse_scopes(str(text or "")) or ()


def _user_target_bi_input(name):
    return "app-target:user:" + str(name or "").strip().lower()


async def _viewer(runtime, data=None):
    account_bi, rec = await runtime.session(data)
    name = str((rec or {}).get("name") or "").strip().lower()
    if not account_bi or not name:
        return "", ""
    return account_bi, name


async def _load_app(runtime, app_id):
    row = await runtime.d1_first("SELECT * FROM apps WHERE app_id=?", app_id)
    if not row:
        return None, None
    record = await runtime.open(row.get("data"))
    if not isinstance(record, dict):
        return None, None
    return row, record


async def _load_app_by_client(runtime, client_id):
    client_id = str(client_id or "").strip()
    if not core.CLIENT_ID_RE.fullmatch(client_id):
        return None, None
    row = await runtime.d1_first(
        "SELECT * FROM apps WHERE client_id=?", client_id)
    if not row:
        return None, None
    record = await runtime.open(row.get("data"))
    if not isinstance(record, dict):
        return None, None
    return row, record


async def _can_manage(runtime, row, account_bi, name):
    if not account_bi:
        return False
    if hmac.compare_digest(str(row.get("owner_bi") or ""), account_bi):
        return True
    org_bi = str(row.get("org_bi") or "")
    if org_bi:
        return await runtime.org_role(org_bi, name) in MANAGER_ROLES
    return False


def _dev(runtime):
    return core.dev_origins(runtime.config("APPS_DEV_ENDPOINT_ORIGINS"))


async def _install_count(runtime, app_id):
    row = await runtime.d1_first(
        "SELECT COUNT(*) AS n FROM app_installs WHERE app_id=? "
        "AND status='active'", app_id)
    return int((row or {}).get("n") or 0)


async def developer_view(runtime, row, record):
    view = core.public_app_view(row, record)
    view.update({
        "redirectUris": list(record.get("redirectUris") or []),
        "eventUrl": str(record.get("eventUrl") or ""),
        "eventUrlVerified": int(row.get("event_url_verified_at") or 0) > 0,
        "eventsDisabled": int(row.get("events_disabled_at") or 0) > 0,
        "eventFailures": int(row.get("event_failures") or 0),
        "interactivityUrl": str(record.get("interactivityUrl") or ""),
        "homepageUrl": str(record.get("homepageUrl") or ""),
        "commands": list(record.get("commands") or []),
        "installCount": await _install_count(
            runtime, str(row.get("app_id") or "")),
        "updatedAt": int(row.get("updated_at") or 0),
        "signature": core.SIGNATURE_SCHEME,
    })
    return view


async def _app_fields(runtime, data, current, account_bi, name):
    record = dict(current or {})
    for key in ("name", "description", "icon", "redirectUris", "scopes",
                "events", "eventUrl", "commands", "interactivityUrl", "bot",
                "homepageUrl"):
        if key in data:
            record[key] = data[key]
    dev = _dev(runtime)
    app_name = core.clean_line(record.get("name"), core.MAX_NAME)
    if len(app_name) < 2:
        return None, "invalid_name"
    description = core.clean_text(record.get("description"),
                                  core.MAX_DESCRIPTION)
    icon = core.clean_icon(record.get("icon"))
    if icon is None:
        return None, "invalid_icon"
    uris = record.get("redirectUris")
    if not isinstance(uris, list) or not uris or \
            len(uris) > core.MAX_REDIRECT_URIS:
        return None, "invalid_redirect_uris"
    clean_uris = []
    for uri in uris:
        cleaned = core.clean_redirect_uri(uri, dev)
        if not cleaned:
            return None, "invalid_redirect_uri"
        if cleaned not in clean_uris:
            clean_uris.append(cleaned)
    scopes = core.parse_scopes(record.get("scopes"))
    if not scopes:
        return None, "invalid_scopes"
    events = core.parse_events(record.get("events") or [])
    if events is None:
        return None, "invalid_events"
    for event in events:
        if not core.event_allowed(event, scopes):
            return None, "event_scope_missing"
    event_url = ""
    if events or record.get("eventUrl"):
        event_url = core.clean_endpoint_url(record.get("eventUrl"), dev)
        if not event_url:
            return None, "invalid_event_url"
    commands = core.clean_commands(record.get("commands"), dev)
    if commands is None:
        return None, "invalid_commands"
    if commands and "commands" not in scopes:
        return None, "commands_scope_missing"
    interactivity = ""
    if record.get("interactivityUrl"):
        interactivity = core.clean_endpoint_url(
            record.get("interactivityUrl"), dev)
        if not interactivity:
            return None, "invalid_interactivity_url"
    import apps_directory
    homepage = apps_directory.clean_homepage(record.get("homepageUrl"), dev)
    if homepage is None:
        return None, "invalid_homepage_url"
    record["homepageUrl"] = homepage
    bot = record.get("bot")
    enabled = bool(bot.get("enabled")) if isinstance(bot, dict) else bool(bot)
    record.update({
        "name": app_name, "description": description, "icon": icon,
        "redirectUris": clean_uris, "scopes": list(scopes),
        "events": list(events), "eventUrl": event_url,
        "commands": commands, "interactivityUrl": interactivity,
        "bot": {"enabled": enabled},
    })
    return record, ""


async def _list_mine(runtime, account_bi, name):
    rows = await runtime.d1_all(
        "SELECT * FROM apps WHERE owner_bi=? ORDER BY created_at DESC LIMIT ?",
        account_bi, core.MAX_APPS_PER_ACCOUNT * 2)
    apps = []
    seen = set()
    for row in rows or []:
        record = await runtime.open(row.get("data"))
        if isinstance(record, dict):
            apps.append(await developer_view(runtime, row, record))
            seen.add(row.get("app_id"))
    for org_name, role in await runtime.orgs_for(name):
        if role not in MANAGER_ROLES:
            continue
        org_bi = await runtime.org_lookup(org_name)
        for row in await runtime.d1_all(
                "SELECT * FROM apps WHERE org_bi=? ORDER BY created_at DESC "
                "LIMIT 50", org_bi) or []:
            if row.get("app_id") in seen:
                continue
            record = await runtime.open(row.get("data"))
            if isinstance(record, dict):
                apps.append(await developer_view(runtime, row, record))
                seen.add(row.get("app_id"))
    return _json(runtime, {
        "ok": True, "apps": apps,
        "scopes": core.scope_view(core.SCOPES),
        "events": list(core.EVENT_TYPES),
        "limits": {"maxApps": core.MAX_APPS_PER_ACCOUNT,
                   "maxCommands": core.MAX_COMMANDS,
                   "maxRedirectUris": core.MAX_REDIRECT_URIS},
    })


async def _create_app(runtime, account_bi, name, data):
    count = await runtime.d1_first(
        "SELECT COUNT(*) AS n FROM apps WHERE owner_bi=?", account_bi)
    if int((count or {}).get("n") or 0) >= core.MAX_APPS_PER_ACCOUNT:
        return _err(runtime, "too_many_apps", 409)
    record, error = await _app_fields(runtime, data, None, account_bi, name)
    if error:
        return _err(runtime, error)
    org_bi = ""
    org_name = str(data.get("organization") or "").strip().lower()
    if org_name:
        org_bi = await runtime.org_lookup(org_name)
        if not org_bi or await runtime.org_role(org_bi, name) not in \
                MANAGER_ROLES:
            return _err(runtime, "not_org_admin", 403)
    slug = str(data.get("slug") or "").strip().lower() or \
        core.slug_from_name(record["name"])
    if not core.valid_slug(slug):
        return _err(runtime, "invalid_slug")
    taken = await runtime.d1_first(
        "SELECT 1 AS one FROM apps WHERE slug=?", slug)
    if taken:
        return _err(runtime, "slug_taken", 409)
    now = runtime.now()
    app_id = runtime.new_hex(16)
    client_id = "fmapp_" + runtime.new_hex(12)
    client_secret = core.CLIENT_SECRET_PREFIX + runtime.new_hex(32)
    signing_secret = core.SIGNING_SECRET_PREFIX + runtime.new_hex(32)
    record.update({"signingSecret": signing_secret, "ownerName": name,
                   "orgName": org_name if org_bi else ""})
    await runtime.d1_run(
        "INSERT INTO apps (app_id, client_id, slug, owner_bi, org_bi, "
        "client_secret_bi, listing, created_at, updated_at, data) "
        "VALUES (?,?,?,?,?,?,'private',?,?,?)",
        app_id, client_id, slug, account_bi, org_bi,
        await runtime.blind(
            core.secret_digest_input("client_secret", client_secret)),
        now, now, await runtime.seal(record))
    await runtime.audit(name, "apps.app.create", "app", app_id,
                        details={"slug": slug})
    row = {"app_id": app_id, "client_id": client_id, "slug": slug,
           "listing": "private", "created_at": now, "updated_at": now}
    return _json(runtime, {
        "ok": True, "app": await developer_view(runtime, row, record),
        "clientSecret": client_secret, "signingSecret": signing_secret,
    }, 201)


async def _update_app(runtime, row, record, name, data):
    updated, error = await _app_fields(runtime, data, record, "", name)
    if error:
        return _err(runtime, error)
    now = runtime.now()
    listing = str(row.get("listing") or "private")
    if listing == "listed" and \
            core.content_hash(updated) != str(row.get("listed_hash") or ""):
        listing = "pending"
    verified = int(row.get("event_url_verified_at") or 0)
    disabled = int(row.get("events_disabled_at") or 0)
    failures = int(row.get("event_failures") or 0)
    if updated.get("eventUrl") != record.get("eventUrl"):
        verified, disabled, failures = 0, 0, 0
    await runtime.d1_run(
        "UPDATE apps SET data=?, listing=?, updated_at=?, "
        "event_url_verified_at=?, events_disabled_at=?, event_failures=? "
        "WHERE app_id=?",
        await runtime.seal(updated), listing, now, verified, disabled,
        failures, row["app_id"])
    removed = set(record.get("scopes") or ()) - set(updated["scopes"])
    if removed:
        await runtime.d1_run(
            "UPDATE app_tokens SET revoked_at=? WHERE app_id=? "
            "AND revoked_at=0", now, row["app_id"])
    await runtime.audit(name, "apps.app.update", "app", row["app_id"],
                        details={"listing": listing})
    fresh = dict(row, listing=listing, updated_at=now,
                 event_url_verified_at=verified,
                 events_disabled_at=disabled, event_failures=failures)
    return _json(runtime, {
        "ok": True, "app": await developer_view(runtime, fresh, updated)})


async def _rotate(runtime, row, record, name, data):
    which = str(data.get("secret") or "").strip().lower()
    now = runtime.now()
    if which == "client":
        secret = core.CLIENT_SECRET_PREFIX + runtime.new_hex(32)
        await runtime.d1_run(
            "UPDATE apps SET client_secret_bi=?, updated_at=? WHERE app_id=?",
            await runtime.blind(
                core.secret_digest_input("client_secret", secret)),
            now, row["app_id"])
        await runtime.audit(name, "apps.app.rotate_client_secret", "app",
                            row["app_id"])
        return _json(runtime, {"ok": True, "clientSecret": secret})
    if which == "signing":
        secret = core.SIGNING_SECRET_PREFIX + runtime.new_hex(32)
        record = dict(record, signingSecret=secret)
        await runtime.d1_run(
            "UPDATE apps SET data=?, updated_at=? WHERE app_id=?",
            await runtime.seal(record), now, row["app_id"])
        await runtime.audit(name, "apps.app.rotate_signing_secret", "app",
                            row["app_id"])
        return _json(runtime, {"ok": True, "signingSecret": secret})
    return _err(runtime, "invalid_secret_kind")


async def _delete_app(runtime, row, name):
    app_id = row["app_id"]
    installs = await runtime.d1_all(
        "SELECT * FROM app_installs WHERE app_id=? AND status='active'",
        app_id)
    for install in installs or []:
        await uninstall(runtime, install, name, "app_deleted", notify=False)
    for table in ("app_auth_codes", "app_tokens", "app_event_outbox",
                  "app_deliveries", "app_messages", "app_response_urls",
                  "app_org_approvals", "app_install_requests", "app_reviews"):
        await runtime.d1_run("DELETE FROM %s WHERE app_id=?" % table, app_id)
    await runtime.d1_run("DELETE FROM app_installs WHERE app_id=?", app_id)
    await runtime.d1_run("DELETE FROM apps WHERE app_id=?", app_id)
    await runtime.audit(name, "apps.app.delete", "app", app_id)
    return _json(runtime, {"ok": True, "deleted": app_id})


async def uninstall(runtime, install, actor, reason, notify=True):
    install_id = str(install.get("install_id") or "")
    now = runtime.now()
    await runtime.d1_run(
        "UPDATE app_installs SET status='revoked', revoked_at=?, "
        "updated_at=? WHERE install_id=?", now, now, install_id)
    await runtime.d1_run(
        "UPDATE app_tokens SET revoked_at=? WHERE install_id=? "
        "AND revoked_at=0", now, install_id)
    for table in ("app_auth_codes", "app_event_outbox", "app_response_urls",
                  "app_messages"):
        await runtime.d1_run(
            "DELETE FROM %s WHERE install_id=?" % table, install_id)
    if install.get("target_kind") == core.TARGET_ORG:
        await runtime.d1_run(
            "DELETE FROM app_org_approvals WHERE org_bi=? AND app_id=?",
            str(install.get("target_bi") or ""),
            str(install.get("app_id") or ""))
    await runtime.audit(actor, "apps.install.revoke", "app_install",
                        install_id, details={"reason": reason,
                                             "appId": install.get("app_id")})
    if notify:
        try:
            await events.emit_uninstalled(runtime, install, reason)
        except Exception:
            pass


async def _viewer_orgs(runtime, name):
    out = []
    for org_name, role in await runtime.orgs_for(name):
        out.append({"name": org_name, "role": role,
                    "canApprove": role in MANAGER_ROLES})
    return out


async def _org_policy(runtime, org_bi):
    row = await runtime.d1_first(
        "SELECT request_policy, allowlist FROM app_org_policies "
        "WHERE org_bi=?", org_bi)
    policy = str((row or {}).get("request_policy") or "any")
    try:
        allowlist = json.loads(str((row or {}).get("allowlist") or "[]"))
    except ValueError:
        allowlist = []
    if policy not in core.ORG_REQUEST_POLICIES:
        policy = "any"
    return {"requestPolicy": policy,
            "allowlist": [str(item) for item in allowlist
                          if core.APP_ID_RE.fullmatch(str(item))]}


async def _app_visible_to(runtime, row, account_bi, name, org_bi=""):
    if str(row.get("listing") or "") == "listed":
        return True
    if str(row.get("listing") or "") == "suspended":
        return False
    if hmac.compare_digest(str(row.get("owner_bi") or ""), account_bi):
        return True
    app_org = str(row.get("org_bi") or "")
    if app_org and await runtime.org_role(app_org, name) in ORG_INSIDER_ROLES:
        return not org_bi or org_bi == app_org
    return False


async def _authorize_params(runtime, params):
    row, record = await _load_app_by_client(runtime, params.get("client_id"))
    if not row or str(row.get("listing") or "") == "suspended":
        return None, None, "invalid_client", None
    redirect_uri = str(params.get("redirect_uri") or "")
    if not core.redirect_matches(record.get("redirectUris") or [],
                                 redirect_uri):
        return row, record, "redirect_uri_mismatch", None
    state = str(params.get("state") or "")
    if len(state) > core.STATE_MAX:
        return row, record, "invalid_request", redirect_uri
    if str(params.get("response_type") or "") != "code":
        return row, record, "unsupported_response_type", redirect_uri
    if str(params.get("code_challenge_method") or "") != "S256" or \
            not core.PKCE_CHALLENGE_RE.fullmatch(
                str(params.get("code_challenge") or "")):
        return row, record, "invalid_request", redirect_uri
    scopes = core.parse_scopes(params.get("scope"))
    if not scopes or not set(scopes) <= set(record.get("scopes") or ()):
        return row, record, "invalid_scope", redirect_uri
    return row, record, "", {"scopes": scopes, "redirect_uri": redirect_uri,
                             "state": state,
                             "challenge": params["code_challenge"]}


def _redirect_error(redirect_uri, error, state, description=""):
    return core.with_query(redirect_uri, [
        ("error", error), ("error_description", description),
        ("state", state)])


async def _authorize(runtime, account_bi, name, data, method):
    params = runtime.query_params() if method == "GET" else data
    row, record, error, request = await _authorize_params(runtime, params)
    state = str(params.get("state") or "")[:core.STATE_MAX]
    if error in ("invalid_client", "redirect_uri_mismatch"):
        return _err(runtime, error, 400)
    if error:
        return _err(runtime, error, 400, redirect=_redirect_error(
            request, error, state))
    target = str(params.get("target") or "user").strip().lower()
    if method == "GET":
        return _json(runtime, {
            "ok": True,
            "app": core.public_app_view(row, record),
            "scopes": core.scope_view(request["scopes"]),
            "account": name,
            "organizations": await _viewer_orgs(runtime, name),
            "target": target,
            "redirectHost": _host_of(request["redirect_uri"]),
            "visible": await _app_visible_to(runtime, row, account_bi, name),
        })
    if str(data.get("decision") or "") != "allow":
        await runtime.audit(name, "apps.install.deny", "app", row["app_id"])
        return _json(runtime, {"ok": True, "redirect": _redirect_error(
            request["redirect_uri"], "access_denied", state)})
    if target == "user":
        if not await _app_visible_to(runtime, row, account_bi, name):
            return _err(runtime, "app_not_available", 403)
        target_kind = core.TARGET_USER
        target_bi = await runtime.blind(_user_target_bi_input(name))
        target_name = name
    elif target.startswith("org:"):
        target_name = target[4:]
        org_bi = await runtime.org_lookup(target_name)
        role = await runtime.org_role(org_bi, name) if org_bi else ""
        if role not in INSTALL_ROLES:
            return _err(runtime, "not_org_member", 403)
        if not await _app_visible_to(runtime, row, account_bi, name, org_bi):
            return _err(runtime, "app_not_available", 403)
        target_kind, target_bi = core.TARGET_ORG, org_bi
        if role not in MANAGER_ROLES:
            decision = await _member_org_install(
                runtime, row, record, org_bi, target_name, account_bi, name,
                request["scopes"])
            if decision is not None:
                if decision == "requested":
                    return _json(runtime, {
                        "ok": True, "pending": True,
                        "redirect": _redirect_error(
                            request["redirect_uri"], "access_denied", state,
                            "admin_approval_pending")})
                return _err(runtime, decision, 403)
    else:
        return _err(runtime, "invalid_target")
    install_id = await _upsert_install(
        runtime, row, target_kind, target_bi, target_name, account_bi, name,
        request["scopes"])
    code = core.CODE_PREFIX + runtime.new_hex(24)
    now = runtime.now()
    await runtime.d1_run(
        "INSERT INTO app_auth_codes (code_bi, app_id, install_id, "
        "account_bi, family_id, expires_at, used_at, data) "
        "VALUES (?,?,?,?,?,?,0,?)",
        await runtime.blind(core.secret_digest_input("code", code)),
        row["app_id"], install_id, account_bi, runtime.new_hex(16),
        now + core.CODE_TTL_MS, await runtime.seal({
            "redirectUri": request["redirect_uri"],
            "challenge": request["challenge"],
            "scopes": list(request["scopes"]),
        }))
    await runtime.audit(name, "apps.install.authorize", "app_install",
                        install_id, details={"appId": row["app_id"],
                                             "target": target_kind})
    return _json(runtime, {"ok": True, "installId": install_id,
                           "redirect": core.with_query(
                               request["redirect_uri"],
                               [("code", code), ("state", state)])})


def _host_of(url):
    match = re.match(r"^[a-z]+://([^/?#]+)", str(url or ""))
    return match.group(1) if match else ""


async def _member_org_install(runtime, row, record, org_bi, org_name,
                              account_bi, name, scopes):
    policy = await _org_policy(runtime, org_bi)
    if policy["requestPolicy"] == "none":
        return "org_requests_disabled"
    if policy["requestPolicy"] == "allowlist" and \
            row["app_id"] not in policy["allowlist"]:
        return "app_not_allowed_by_org"
    approval = await runtime.d1_first(
        "SELECT scopes FROM app_org_approvals WHERE org_bi=? AND app_id=?",
        org_bi, row["app_id"])
    if approval and set(scopes) <= set(
            _scopes_from_text(approval.get("scopes"))):
        return None
    pending = await runtime.d1_first(
        "SELECT request_id FROM app_install_requests WHERE org_bi=? "
        "AND app_id=? AND requester_bi=? AND status='pending'",
        org_bi, row["app_id"], account_bi)
    now = runtime.now()
    if pending:
        await runtime.d1_run(
            "UPDATE app_install_requests SET scopes=?, created_at=? "
            "WHERE request_id=?", core.scope_string(scopes), now,
            pending["request_id"])
        return "requested"
    request_id = runtime.new_hex(16)
    await runtime.d1_run(
        "INSERT INTO app_install_requests (request_id, app_id, org_bi, "
        "requester_bi, scopes, status, created_at, data) "
        "VALUES (?,?,?,?,?,'pending',?,?)",
        request_id, row["app_id"], org_bi, account_bi,
        core.scope_string(scopes), now,
        await runtime.seal({"orgName": org_name, "requester": name}))
    for admin in await runtime.org_admins(org_bi):
        await runtime.notify(
            admin, "%s wants to install %s in %s" % (
                name, record.get("name"), org_name),
            "Review the request under Apps in the organization's settings.",
            "/dashboard/apps?org=" + org_name,
            "app-install-request:" + request_id)
    await runtime.audit(name, "apps.install.request", "app", row["app_id"],
                        outcome="requested")
    return "requested"


async def _upsert_install(runtime, row, target_kind, target_bi, target_name,
                          account_bi, name, scopes):
    now = runtime.now()
    existing = await runtime.d1_first(
        "SELECT install_id, scopes FROM app_installs WHERE app_id=? "
        "AND target_kind=? AND target_bi=? AND status='active'",
        row["app_id"], target_kind, target_bi)
    data = await runtime.seal({"targetName": target_name,
                               "installedBy": name})
    if existing:
        install_id = existing["install_id"]
        if not set(_scopes_from_text(existing.get("scopes"))) <= set(scopes):
            await runtime.d1_run(
                "UPDATE app_tokens SET revoked_at=? WHERE install_id=? "
                "AND revoked_at=0", now, install_id)
        await runtime.d1_run(
            "UPDATE app_installs SET scopes=?, installed_by_bi=?, "
            "updated_at=?, data=? WHERE install_id=?",
            core.scope_string(scopes), account_bi, now, data, install_id)
        return install_id
    install_id = runtime.new_hex(16)
    await runtime.d1_run(
        "INSERT INTO app_installs (install_id, app_id, target_kind, "
        "target_bi, installed_by_bi, scopes, status, created_at, updated_at, "
        "data) VALUES (?,?,?,?,?,?,'active',?,?,?)",
        install_id, row["app_id"], target_kind, target_bi, account_bi,
        core.scope_string(scopes), now, now, data)
    return install_id


def _basic_credentials(header):
    header = str(header or "")
    if not header.lower().startswith("basic "):
        return "", ""
    try:
        decoded = base64.b64decode(header[6:].strip()).decode("utf-8")
    except (ValueError, UnicodeDecodeError):
        return "", ""
    client_id, _, secret = decoded.partition(":")
    return client_id, secret


async def _client(runtime, params):
    client_id, secret = _basic_credentials(runtime.header("authorization"))
    if not client_id:
        client_id = str(params.get("client_id") or "")
        secret = str(params.get("client_secret") or "")
    row, record = await _load_app_by_client(runtime, client_id)
    if not row or core.token_kind(secret) != "client_secret":
        return None, None
    digest = await runtime.blind(
        core.secret_digest_input("client_secret", secret))
    if not hmac.compare_digest(digest, str(row.get("client_secret_bi") or "")):
        return None, None
    if str(row.get("listing") or "") == "suspended":
        return None, None
    return row, record


async def _revoke_family(runtime, family_id):
    if family_id:
        await runtime.d1_run(
            "UPDATE app_tokens SET revoked_at=? WHERE family_id=? "
            "AND revoked_at=0", runtime.now(), family_id)


async def _issue_tokens(runtime, row, record, install, scopes, family_id):
    now = runtime.now()
    access = core.ACCESS_PREFIX + runtime.new_hex(32)
    refresh = core.REFRESH_PREFIX + runtime.new_hex(32)
    scope_text = core.scope_string(scopes)
    await runtime.batch([
        ("INSERT INTO app_tokens (token_bi, kind, app_id, install_id, "
         "family_id, scopes, created_at, expires_at) "
         "VALUES (?,?,?,?,?,?,?,?)",
         (await runtime.blind(core.secret_digest_input("access", access)),
          "access", row["app_id"], install["install_id"], family_id,
          scope_text, now, now + core.ACCESS_TTL_MS)),
        ("INSERT INTO app_tokens (token_bi, kind, app_id, install_id, "
         "family_id, scopes, created_at, expires_at) "
         "VALUES (?,?,?,?,?,?,?,?)",
         (await runtime.blind(core.secret_digest_input("refresh", refresh)),
          "refresh", row["app_id"], install["install_id"], family_id,
          scope_text, now, now + core.REFRESH_TTL_MS)),
    ])
    target = await runtime.open(install.get("data")) or {}
    body = {
        "access_token": access,
        "token_type": "Bearer",
        "expires_in": core.ACCESS_TTL_MS // 1000,
        "refresh_token": refresh,
        "refresh_token_expires_in": core.REFRESH_TTL_MS // 1000,
        "scope": scope_text,
        "install": {"id": install["install_id"],
                    "target": install.get("target_kind"),
                    "name": str(target.get("targetName") or "")},
        "app": {"id": row["app_id"], "slug": row.get("slug")},
    }
    if (record.get("bot") or {}).get("enabled"):
        body["bot"] = {"id": core.bot_author(row.get("slug")),
                       "name": core.bot_display_name(record.get("name"))}
    return body


async def oauth_token(runtime):
    params, error = await runtime.form_body(MAX_BODY)
    if error or not isinstance(params, dict):
        return _oauth_err(runtime, "invalid_request")
    row, record = await _client(runtime, params)
    if not row:
        return _oauth_err(runtime, "invalid_client", 401)
    grant = str(params.get("grant_type") or "")
    now = runtime.now()
    if grant == "authorization_code":
        code = str(params.get("code") or "")
        if core.token_kind(code) != "code":
            return _oauth_err(runtime, "invalid_grant")
        code_bi = await runtime.blind(core.secret_digest_input("code", code))
        claimed = await runtime.d1_first(
            "UPDATE app_auth_codes SET used_at=? WHERE code_bi=? "
            "AND used_at=0 AND expires_at>? RETURNING app_id, install_id, "
            "family_id, data", now, code_bi, now)
        if not claimed:
            replay = await runtime.d1_first(
                "SELECT family_id, used_at FROM app_auth_codes "
                "WHERE code_bi=?", code_bi)
            if replay and int(replay.get("used_at") or 0):
                await _revoke_family(runtime, replay.get("family_id"))
                await runtime.audit(
                    "app:" + str(row.get("slug")), "apps.oauth.code_replay",
                    "app", row["app_id"], outcome="denied")
            return _oauth_err(runtime, "invalid_grant")
        if claimed.get("app_id") != row["app_id"]:
            return _oauth_err(runtime, "invalid_grant")
        stored = await runtime.open(claimed.get("data")) or {}
        if not hmac.compare_digest(
                str(stored.get("redirectUri") or ""),
                str(params.get("redirect_uri") or "")):
            return _oauth_err(runtime, "invalid_grant",
                              description="redirect_uri_mismatch")
        if not core.verify_pkce(params.get("code_verifier"),
                                stored.get("challenge")):
            return _oauth_err(runtime, "invalid_grant",
                              description="pkce_verification_failed")
        install = await runtime.d1_first(
            "SELECT * FROM app_installs WHERE install_id=? "
            "AND status='active'", claimed.get("install_id"))
        if not install:
            return _oauth_err(runtime, "invalid_grant")
        scopes = core.parse_scopes(stored.get("scopes")) or ()
        body = await _issue_tokens(runtime, row, record, install, scopes,
                                   claimed.get("family_id"))
        return _json(runtime, body, 200, {"pragma": "no-cache"})
    if grant == "refresh_token":
        token = str(params.get("refresh_token") or "")
        if core.token_kind(token) != "refresh":
            return _oauth_err(runtime, "invalid_grant")
        token_bi = await runtime.blind(
            core.secret_digest_input("refresh", token))
        stored = await runtime.d1_first(
            "SELECT * FROM app_tokens WHERE token_bi=? AND kind='refresh'",
            token_bi)
        if not stored or stored.get("app_id") != row["app_id"]:
            return _oauth_err(runtime, "invalid_grant")
        if int(stored.get("revoked_at") or 0) or \
                int(stored.get("rotated_at") or 0):
            await _revoke_family(runtime, stored.get("family_id"))
            await runtime.audit(
                "app:" + str(row.get("slug")), "apps.oauth.refresh_reuse",
                "app", row["app_id"], outcome="denied")
            return _oauth_err(runtime, "invalid_grant")
        if int(stored.get("expires_at") or 0) <= now:
            return _oauth_err(runtime, "invalid_grant")
        scopes = _scopes_from_text(stored.get("scopes"))
        if params.get("scope"):
            narrowed = core.parse_scopes(params.get("scope"))
            if narrowed is None or not set(narrowed) <= set(scopes):
                return _oauth_err(runtime, "invalid_scope")
            scopes = narrowed
        install = await runtime.d1_first(
            "SELECT * FROM app_installs WHERE install_id=? "
            "AND status='active'", stored.get("install_id"))
        if not install:
            return _oauth_err(runtime, "invalid_grant")
        install_scopes = set(_scopes_from_text(install.get("scopes")))
        scopes = tuple(scope for scope in scopes if scope in install_scopes)
        rotated = await runtime.d1_first(
            "UPDATE app_tokens SET rotated_at=? WHERE token_bi=? "
            "AND rotated_at=0 AND revoked_at=0 RETURNING token_bi",
            now, token_bi)
        if not rotated:
            await _revoke_family(runtime, stored.get("family_id"))
            return _oauth_err(runtime, "invalid_grant")
        body = await _issue_tokens(runtime, row, record, install, scopes,
                                   stored.get("family_id"))
        return _json(runtime, body, 200, {"pragma": "no-cache"})
    return _oauth_err(runtime, "unsupported_grant_type")


async def oauth_revoke(runtime):
    params, error = await runtime.form_body(MAX_BODY)
    if error or not isinstance(params, dict):
        return _oauth_err(runtime, "invalid_request")
    row, _record = await _client(runtime, params)
    if not row:
        return _oauth_err(runtime, "invalid_client", 401)
    token = str(params.get("token") or "")
    kind = core.token_kind(token)
    if kind in ("access", "refresh"):
        stored = await runtime.d1_first(
            "SELECT * FROM app_tokens WHERE token_bi=?",
            await runtime.blind(core.secret_digest_input(kind, token)))
        if stored and stored.get("app_id") == row["app_id"]:
            if kind == "refresh":
                await _revoke_family(runtime, stored.get("family_id"))
            else:
                await runtime.d1_run(
                    "UPDATE app_tokens SET revoked_at=? WHERE token_bi=?",
                    runtime.now(), stored["token_bi"])
    return _json(runtime, {"ok": True})


async def authenticate(runtime, token):
    if core.token_kind(token) != "access":
        return None
    stored = await runtime.d1_first(
        "SELECT * FROM app_tokens WHERE token_bi=? AND kind='access'",
        await runtime.blind(core.secret_digest_input("access", token)))
    now = runtime.now()
    if not stored or int(stored.get("revoked_at") or 0) or \
            int(stored.get("expires_at") or 0) <= now:
        return None
    install = await runtime.d1_first(
        "SELECT * FROM app_installs WHERE install_id=? AND status='active'",
        stored.get("install_id"))
    if not install:
        return None
    row, record = await _load_app(runtime, str(stored.get("app_id") or ""))
    if not row or str(row.get("listing") or "") == "suspended":
        return None
    install_scopes = set(_scopes_from_text(install.get("scopes")))
    scopes = tuple(scope for scope in _scopes_from_text(stored.get("scopes"))
                   if scope in install_scopes)
    if now - int(install.get("last_used_at") or 0) >= 60 * 1000:
        await runtime.d1_run(
            "UPDATE app_installs SET last_used_at=? WHERE install_id=?",
            now, install["install_id"])
    return await install_context(runtime, row, record, install, scopes)


async def install_context(runtime, row, record, install, scopes=None):
    if scopes is None:
        scopes = _scopes_from_text(install.get("scopes"))
    target = await runtime.open(install.get("data")) or {}
    return {
        "app": row, "record": record, "install": install, "scopes": scopes,
        "appId": row["app_id"], "installId": install["install_id"],
        "slug": str(row.get("slug") or ""),
        "targetKind": str(install.get("target_kind") or ""),
        "targetName": str(target.get("targetName") or ""),
        "installedBy": str(target.get("installedBy") or ""),
        "installedByBi": str(install.get("installed_by_bi") or ""),
        "actor": core.bot_author(row.get("slug")),
    }


async def _install_view(runtime, install, name, can_revoke):
    row, record = await _load_app(runtime, str(install.get("app_id") or ""))
    if not row:
        return None
    target = await runtime.open(install.get("data")) or {}
    return {
        "installId": install["install_id"],
        "app": core.public_app_view(row, record),
        "target": {"kind": install.get("target_kind"),
                   "name": str(target.get("targetName") or "")},
        "scopes": core.scope_view(_scopes_from_text(install.get("scopes"))),
        "installedBy": str(target.get("installedBy") or ""),
        "createdAt": int(install.get("created_at") or 0),
        "lastUsedAt": int(install.get("last_used_at") or 0),
        "canRevoke": can_revoke,
    }


async def _installed(runtime, account_bi, name):
    user_bi = await runtime.blind(_user_target_bi_input(name))
    installs = []
    for install in await runtime.d1_all(
            "SELECT * FROM app_installs WHERE target_bi=? "
            "AND target_kind='user' AND status='active' "
            "ORDER BY created_at DESC LIMIT 100", user_bi) or []:
        view = await _install_view(runtime, install, name, True)
        if view:
            installs.append(view)
    orgs = []
    for org_name, role in await runtime.orgs_for(name):
        org_bi = await runtime.org_lookup(org_name)
        manager = role in MANAGER_ROLES
        entry = {"name": org_name, "role": role, "canManage": manager,
                 "installs": [], "requests": []}
        for install in await runtime.d1_all(
                "SELECT * FROM app_installs WHERE target_bi=? "
                "AND target_kind='org' AND status='active' "
                "ORDER BY created_at DESC LIMIT 100", org_bi) or []:
            view = await _install_view(runtime, install, name, manager)
            if view:
                entry["installs"].append(view)
        if manager:
            entry["requests"] = await _request_views(runtime, org_bi)
            entry["policy"] = await _org_policy(runtime, org_bi)
        orgs.append(entry)
    return _json(runtime, {"ok": True, "account": name, "installs": installs,
                           "organizations": orgs})


async def _request_views(runtime, org_bi):
    out = []
    for request in await runtime.d1_all(
            "SELECT * FROM app_install_requests WHERE org_bi=? "
            "AND status='pending' ORDER BY created_at DESC LIMIT 50",
            org_bi) or []:
        row, record = await _load_app(runtime, str(request.get("app_id")))
        if not row:
            continue
        data = await runtime.open(request.get("data")) or {}
        out.append({
            "id": request["request_id"],
            "app": core.public_app_view(row, record),
            "scopes": core.scope_view(_scopes_from_text(request.get("scopes"))),
            "requester": str(data.get("requester") or ""),
            "createdAt": int(request.get("created_at") or 0),
        })
    return out


async def _revoke_install(runtime, account_bi, name, install_id):
    install = await runtime.d1_first(
        "SELECT * FROM app_installs WHERE install_id=? AND status='active'",
        install_id)
    if not install:
        return _err(runtime, "not_found", 404)
    allowed = False
    if install.get("target_kind") == core.TARGET_USER:
        allowed = hmac.compare_digest(
            str(install.get("target_bi") or ""),
            await runtime.blind(_user_target_bi_input(name)))
    elif install.get("target_kind") == core.TARGET_ORG:
        allowed = await runtime.org_role(
            str(install.get("target_bi") or ""), name) in MANAGER_ROLES
    if not allowed and not await runtime.is_admin(name):
        await runtime.audit(name, "apps.install.revoke", "app_install",
                            install_id, outcome="denied")
        return _err(runtime, "forbidden", 403)
    await uninstall(runtime, install, name, "revoked_by_" + (
        "installer" if allowed else "admin"))
    return _json(runtime, {"ok": True, "revoked": install_id})


async def _org_manager(runtime, org_name, name):
    org_bi = await runtime.org_lookup(org_name)
    if not org_bi:
        return "", _err(runtime, "not_found", 404)
    if await runtime.org_role(org_bi, name) not in MANAGER_ROLES:
        return "", _err(runtime, "not_org_admin", 403)
    return org_bi, None


async def _set_policy(runtime, org_bi, account_bi, name, data):
    policy = str(data.get("requestPolicy") or "any")
    if policy not in core.ORG_REQUEST_POLICIES:
        return _err(runtime, "invalid_policy")
    allowlist = data.get("allowlist") or []
    if not isinstance(allowlist, list) or len(allowlist) > 200 or not all(
            core.APP_ID_RE.fullmatch(str(item)) for item in allowlist):
        return _err(runtime, "invalid_allowlist")
    await runtime.d1_run(
        "INSERT INTO app_org_policies (org_bi, request_policy, allowlist, "
        "updated_by_bi, updated_at) VALUES (?,?,?,?,?) "
        "ON CONFLICT(org_bi) DO UPDATE SET request_policy=excluded."
        "request_policy, allowlist=excluded.allowlist, "
        "updated_by_bi=excluded.updated_by_bi, updated_at=excluded."
        "updated_at", org_bi, policy,
        json.dumps(sorted(set(str(item) for item in allowlist))),
        account_bi, runtime.now())
    await runtime.audit(name, "apps.org.policy", "organization", org_bi,
                        details={"policy": policy})
    return _json(runtime, {"ok": True,
                           "policy": await _org_policy(runtime, org_bi)})


async def _decide_request(runtime, org_bi, org_name, account_bi, name,
                          request_id, data):
    request = await runtime.d1_first(
        "SELECT * FROM app_install_requests WHERE request_id=? AND org_bi=? "
        "AND status='pending'", request_id, org_bi)
    if not request:
        return _err(runtime, "not_found", 404)
    action = str(data.get("action") or "")
    if action not in ("approve", "deny"):
        return _err(runtime, "invalid_action")
    now = runtime.now()
    status = "approved" if action == "approve" else "denied"
    await runtime.d1_run(
        "UPDATE app_install_requests SET status=?, decided_at=?, "
        "decided_by_bi=? WHERE request_id=?", status, now, account_bi,
        request_id)
    row, record = await _load_app(runtime, str(request.get("app_id")))
    if action == "approve" and row:
        await runtime.d1_run(
            "INSERT INTO app_org_approvals (org_bi, app_id, scopes, "
            "approved_by_bi, approved_at) VALUES (?,?,?,?,?) "
            "ON CONFLICT(org_bi, app_id) DO UPDATE SET scopes=excluded."
            "scopes, approved_by_bi=excluded.approved_by_bi, "
            "approved_at=excluded.approved_at",
            org_bi, row["app_id"], request.get("scopes"), account_bi, now)
    data_row = await runtime.open(request.get("data")) or {}
    requester = str(data_row.get("requester") or "")
    if requester and row:
        await runtime.notify(
            requester, "%s was %s for %s" % (
                record.get("name"), status, org_name),
            "Install it again to finish." if status == "approved" else "",
            "/dashboard/apps", "app-install-decision:" + request_id)
    await runtime.audit(name, "apps.install.request_" + action, "app",
                        str(request.get("app_id")))
    return _json(runtime, {"ok": True, "status": status})


def _match(path):
    for name, pattern in ROUTES:
        match = pattern.fullmatch(path)
        if match:
            return name, match.groups()
    return "", ()


def bearer(runtime):
    header = str(runtime.header("authorization") or "").strip()
    if header.lower().startswith("bearer "):
        return header[7:].strip()
    return ""


async def handle(runtime, path):
    if str(path or "").startswith("/api/apps/v1/"):
        import apps_api
        await runtime.ensure_schema()
        return await apps_api.v1(runtime, path, bearer(runtime))
    if str(path or "").startswith("/api/apps/respond/"):
        import apps_commands
        match = apps_commands.RESPOND_RE.fullmatch(str(path))
        if not match:
            return _err(runtime, "invalid_response_url", 404)
        await runtime.ensure_schema()
        return await apps_commands.respond(runtime, match.group(1))
    route, groups = _match(str(path or ""))
    if not route and str(path or "").rstrip("/") in COMMAND_PATHS:
        route = "commands"
    if not route:
        extra = getattr(runtime, "extra_routes", None)
        if extra is not None:
            return await extra(path)
        return _err(runtime, "not_found", 404)
    await runtime.ensure_schema()
    method = runtime.method()
    if route == "token":
        if method != "POST":
            return _err(runtime, "method_not_allowed", 405)
        return await oauth_token(runtime)
    if route == "revoke":
        if method != "POST":
            return _err(runtime, "method_not_allowed", 405)
        return await oauth_revoke(runtime)
    if route == "directory":
        import apps_directory
        return await apps_directory.directory(runtime)
    if route == "scopes":
        return _json(runtime, {"ok": True,
                               "scopes": core.scope_view(core.SCOPES),
                               "events": list(core.EVENT_TYPES)})
    data = {}
    if method in ("POST", "PUT", "PATCH", "DELETE"):
        data, error = await runtime.json_body(MAX_BODY)
        if error:
            return _err(runtime, error, 400)
    account_bi, name = await _viewer(runtime, data)
    if not account_bi:
        return _err(runtime, "invalid_session", 401)
    if route == "commands":
        import apps_commands
        return await apps_commands.handle(runtime, str(path), account_bi,
                                          name, data, method)
    if route == "apps":
        if method == "GET":
            return await _list_mine(runtime, account_bi, name)
        if method == "POST":
            return await _create_app(runtime, account_bi, name, data)
        return _err(runtime, "method_not_allowed", 405)
    if route == "installed":
        return await _installed(runtime, account_bi, name)
    if route in ("review", "review_one"):
        import apps_directory
        if route == "review":
            return await apps_directory.review_queue(runtime, name)
        if method != "POST":
            return _err(runtime, "method_not_allowed", 405)
        row, record = await _load_app(runtime, groups[0])
        if not row:
            return _err(runtime, "not_found", 404)
        return await apps_directory.review(runtime, account_bi, name, row,
                                           record, data)
    if route == "install":
        if method != "DELETE":
            return _err(runtime, "method_not_allowed", 405)
        return await _revoke_install(runtime, account_bi, name, groups[0])
    if route == "authorize":
        if method not in ("GET", "POST"):
            return _err(runtime, "method_not_allowed", 405)
        return await _authorize(runtime, account_bi, name, data, method)
    if route in ("org_policy", "org_requests", "org_request"):
        org_bi, denied = await _org_manager(runtime, groups[0], name)
        if denied is not None:
            return denied
        if route == "org_policy":
            if method == "GET":
                return _json(runtime, {
                    "ok": True, "policy": await _org_policy(runtime, org_bi)})
            if method == "PUT":
                return await _set_policy(runtime, org_bi, account_bi, name,
                                         data)
            return _err(runtime, "method_not_allowed", 405)
        if route == "org_requests":
            return _json(runtime, {
                "ok": True, "requests": await _request_views(runtime, org_bi)})
        if method != "POST":
            return _err(runtime, "method_not_allowed", 405)
        return await _decide_request(runtime, org_bi, groups[0], account_bi,
                                     name, groups[1], data)
    row, record = await _load_app(runtime, groups[0])
    if not row or not await _can_manage(runtime, row, account_bi, name):
        return _err(runtime, "not_found", 404)
    if route == "app":
        if method == "GET":
            return _json(runtime, {
                "ok": True, "app": await developer_view(runtime, row, record)})
        if method == "PATCH":
            return await _update_app(runtime, row, record, name, data)
        if method == "DELETE":
            return await _delete_app(runtime, row, name)
        return _err(runtime, "method_not_allowed", 405)
    if route == "rotate" and method == "POST":
        return await _rotate(runtime, row, record, name, data)
    if route == "submit" and method == "POST":
        import apps_directory
        return await apps_directory.submit(runtime, row, record, name)
    if route == "verify" and method == "POST":
        ok, error = await events.verify_url(runtime, row, record, name)
        if not ok:
            return _err(runtime, error, 400)
        row, record = await _load_app(runtime, groups[0])
        return _json(runtime, {
            "ok": True, "app": await developer_view(runtime, row, record)})
    if route == "deliveries" and method == "GET":
        return _json(runtime, dict(await events.deliveries(
            runtime, row["app_id"]), ok=True))
    return _err(runtime, "method_not_allowed", 405)
