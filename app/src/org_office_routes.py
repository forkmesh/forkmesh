import base64
import hashlib
import hmac
import json
import re
from urllib.parse import parse_qs, quote, urlparse
from hidden_unicode import clean_value as clean_hidden_unicode

import org_office as o
import org_office_room as room


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


TICKET_TTL_MS = 60 * 1000
TICKET_PURPOSE = ":org-office-ticket-v1"
TURN_TTL_S = 10 * 60
MEMBER_ROLES = ("owner", "admin", "maintainer", "member", "auditor")
NO_STORE = "no-store, max-age=0, must-revalidate"
SAFE_HEADERS = {"x-content-type-options": "nosniff"}
SOCKET_PATH = "/api/office/ws"
ROOM_OBJECT_PREFIX = "office:org:"
ROOM_OBJECT_RE = re.compile(r"office:org:([0-9a-f]{64}):v1")
HANDOFF_HEADERS = (
    "upgrade", "connection", "sec-websocket-version", "sec-websocket-key",
)


def _json(data, status=200):
    return json_response(data, status=status, cache_control=NO_STORE,
                         extra_headers=SAFE_HEADERS)


def room_object_name(org_bi):
    return ROOM_OBJECT_PREFIX + org_bi + ":v1"


def room_path(org_bi):
    return "/api/office/org/" + org_bi + "/v1/ws"


def _ticket_key(env):
    return (_require_data_secret(env) + TICKET_PURPOSE).encode()


def mint_ticket(env, org_bi, account_bi, session_id, now, nonce):
    expires = int(now) + TICKET_TTL_MS
    canonical = ".".join(("v1", org_bi, account_bi, session_id,
                          str(expires), nonce))
    signature = hmac.new(_ticket_key(env), canonical.encode(),
                         hashlib.sha256).hexdigest()
    return canonical + "." + signature, expires


def read_ticket(env, token, now):
    parts = str(token or "").split(".")
    if len(parts) != 7 or len(str(token)) > 512:
        return None
    version, org_bi, account_bi, session_id, expires_raw, nonce, sig = parts
    if (version != "v1" or not room.BI_RE.fullmatch(org_bi)
            or not room.BI_RE.fullmatch(account_bi)
            or not room.SESSION_RE.fullmatch(session_id)
            or not room.NONCE_RE.fullmatch(nonce)
            or not re.fullmatch(r"[0-9a-f]{64}", sig)):
        return None
    try:
        expires = int(expires_raw)
    except ValueError:
        return None
    if expires <= now or expires - now > TICKET_TTL_MS:
        return None
    expected = hmac.new(_ticket_key(env), ".".join(parts[:6]).encode(),
                        hashlib.sha256).hexdigest()
    if not hmac.compare_digest(sig, expected):
        return None
    return {"org_bi": org_bi, "account_bi": account_bi,
            "session_id": session_id, "nonce": nonce, "expires": expires}


def _env_text(env, name, default=""):
    value = getattr(env, name, None)
    if value is None:
        return default
    text = str(value).strip()
    return text if text and text not in ("undefined", "null") else default


def _url_list(text, schemes):
    out = []
    for item in str(text or "").split(","):
        url = item.strip()
        if url and url.split(":", 1)[0] in schemes and len(url) < 200 \
                and not re.search(r"[\s\"'<>]", url):
            out.append(url)
    return out[:4]


def ice_servers(env, account_bi, now_seconds):
    servers = []
    stun = _url_list(_env_text(env, "OFFICE_STUN_URLS"), ("stun", "stuns"))
    if stun:
        servers.append({"urls": stun})
    turn = _url_list(_env_text(env, "OFFICE_TURN_URLS"), ("turn", "turns"))
    secret = _env_text(env, "OFFICE_TURN_SECRET")
    if turn and len(secret) >= 16:
        username = "%d:%s" % (int(now_seconds) + TURN_TTL_S,
                              o.alias("a:" + account_bi))
        credential = base64.b64encode(hmac.new(
            secret.encode(), username.encode(), hashlib.sha1).digest()
        ).decode()
        servers.append({"urls": turn, "username": username,
                        "credential": credential})
    return servers


def account_pic(record):
    avatar = str((record or {}).get("avatar_png") or "").strip()
    if not avatar:
        return ""
    return hashlib.sha256(avatar.encode("utf-8")).hexdigest()[:16]


def tasks_org(env):
    return (_env_text(env, "OFFICE_MARKETING_ORG") or "forkmesh").lower()


async def _signed_in(env, request):
    try:
        account_bi, record = await _account_session_record(env, request)
    except Exception:
        return "", None
    if (not room.BI_RE.fullmatch(str(account_bi or "")) or not record
            or record.get("status") != "active"
            or _account_kind(record) != "user"):
        return "", None
    return str(account_bi), record


async def _account_orgs(env, account_bi):
    rows = await d1_all(
        env,
        "SELECT o.name AS name, om.role AS role FROM org_members om "
        "INNER JOIN orgs o ON o.org_bi=om.org_bi WHERE om.member_bi=? "
        "AND om.role IN ('owner','admin','maintainer','member','auditor') "
        "ORDER BY o.name LIMIT 50",
        account_bi)
    out = []
    for row in rows or []:
        name = str((row or {}).get("name") or "").strip().lower()
        role = o.clean_role((row or {}).get("role"))
        if room.ORG_NAME_RE.fullmatch(name) and role:
            out.append({"name": name, "role": role})
    return out


async def _membership(env, org_bi, account_bi):
    member = await d1_first(
        env,
        "SELECT om.role AS role, o.name AS name FROM org_members om "
        "INNER JOIN orgs o ON o.org_bi=om.org_bi WHERE om.org_bi=? "
        "AND om.member_bi=? AND om.role IN "
        "('owner','admin','maintainer','member','auditor') LIMIT 1",
        org_bi, account_bi)
    role = o.clean_role((member or {}).get("role"))
    if not role:
        return None
    rows = await d1_all(
        env,
        "SELECT DISTINCT tm.team AS team FROM org_team_members tm "
        "INNER JOIN org_members om ON om.org_bi=tm.org_bi "
        "AND om.member_bi=tm.member_bi INNER JOIN org_teams ot "
        "ON ot.org_bi=tm.org_bi AND ot.team=tm.team "
        "WHERE tm.org_bi=? AND tm.member_bi=? "
        "AND ot.permission IN ('read','write','maintain','admin') "
        "ORDER BY tm.team LIMIT 50",
        org_bi, account_bi)
    teams = o.clean_teams([(r or {}).get("team") for r in rows or []])
    return {"role": role, "teams": teams,
            "org": str((member or {}).get("name") or "").strip().lower()}


async def org_office_member_profile(env, org_bi, account_bi, session_id):
    now = int(Date.now())
    try:
        if session_id.startswith("desktop_"):
            account = await d1_first(
                env, "SELECT data FROM users WHERE user_bi=?", account_bi)
        else:
            account = await d1_first(
                env,
                "SELECT u.data AS data FROM account_sessions s "
                "JOIN users u ON u.user_bi=s.account_bi "
                "WHERE s.session_id=? AND s.account_bi=? "
                "AND s.revoked_at=0 AND s.expires_at>?",
                session_id, account_bi, now)
        if not account:
            return None
        record = await decrypt_row(env, account.get("data"))
        if (not record or record.get("status") != "active"
                or _account_kind(record) != "user"):
            return None
        member = await _membership(env, org_bi, account_bi)
        if not member:
            return None
        member["name"] = str(record.get("name") or "")
        member["pic"] = account_pic(record)
        return member
    except Exception:
        return None


async def _org_by_name(env, name):
    name = str(name or "").strip().lower()
    if not room.ORG_NAME_RE.fullmatch(name):
        return "", None
    org_bi, row = await _org_row(env, name)
    return (str(org_bi), row) if row else ("", None)


def _retention_ms(record):
    try:
        return int(_account_status.account_chat_retention(record)
                   ["defaultMs"])
    except Exception:
        return room.CHAT_RETAIN_MAX_MS


async def _office_chat(env, org_bi, org, account_bi):
    try:
        mapped = await org_office_chat.sync(env, org_bi, org)
        return await org_office_chat.rooms_for(env, org_bi, account_bi,
                                               mapped)
    except Exception:
        return {}


async def org_office_session_handler(env, request):
    if method_name(request) != "GET":
        return _json({"error": "method_not_allowed"}, 405)
    account_bi, record = await _signed_in(env, request)
    if not record:
        return _json({"ok": True, "signedIn": False,
                      "signIn": "/login?next=%2Foffice"})
    await ensure_schema(env)
    orgs = await _account_orgs(env, account_bi)
    wanted = ""
    try:
        wanted = (parse_qs(urlparse(request.url).query).get("org")
                  or [""])[0].strip().lower()
    except Exception:
        wanted = ""
    chosen = next((org for org in orgs if org["name"] == wanted), None) \
        or (orgs[0] if orgs else None)
    body = {
        "ok": True,
        "signedIn": True,
        "account": {"name": str(record.get("name") or ""),
                    "av": o.avatar_for(account_bi),
                    "pic": account_pic(record)},
        "orgs": orgs,
        "org": None,
        "rooms": o.GATED_ZONES,
    }
    if chosen:
        org_bi, _row = await _org_by_name(env, chosen["name"])
        member = await _membership(env, org_bi, account_bi) if org_bi \
            else None
        if member:
            body.update({
                "org": chosen["name"],
                "role": member["role"],
                "teams": member["teams"],
                "unlocked": o.unlocked_zones(member["teams"],
                                             member["role"]),
                "tasks": chosen["name"] == tasks_org(env),
                "chat": await _office_chat(env, org_bi, chosen["name"],
                                           account_bi),
            })
    return _json(body)


async def org_office_entry_handler(env, request):
    if method_name(request) != "POST":
        return _json({"error": "method_not_allowed"}, 405)
    if not _request_origin_allowed(env, request):
        return _json({"error": "origin_not_allowed"}, 403)
    account_bi, record = await _signed_in(env, request)
    session_id = _request_account_session_id(request) if record else ""
    if not record or not room.SESSION_RE.fullmatch(session_id or ""):
        return _json({"error": "login_required"}, 401)
    try:
        raw = await request.text()
        data = clean_hidden_unicode(json.loads(raw)) if raw and len(raw) <= 2048 else {}
    except Exception:
        data = {}
    if not isinstance(data, dict):
        data = {}
    await ensure_schema(env)
    org_bi, _row = await _org_by_name(env, data.get("org"))
    member = await _membership(env, org_bi, account_bi) if org_bi else None
    if not member:
        return _json({"error": "org_member_required"}, 403)
    now = int(Date.now())
    ticket, expires = mint_ticket(env, org_bi, account_bi, session_id, now,
                                  new_world_peer_id())
    return _json({
        "ok": True,
        "org": member["org"],
        "ticket": ticket,
        "expiresAt": expires,
        "ws": SOCKET_PATH + "?ticket=" + quote(ticket, safe=""),
        "iceServers": ice_servers(env, account_bi, now // 1000),
    })


async def org_office_socket_handler(env, request):
    if (method_name(request) != "GET"
            or str(request.headers.get("upgrade") or "").lower()
            != "websocket"
            or not world_websocket_origin_allowed(request, env)):
        return _json({"error": "not_found"}, 404)
    now = int(Date.now())
    values = parse_qs(urlparse(request.url).query,
                      keep_blank_values=False).get("ticket") or []
    ticket = read_ticket(env, values[0], now) if len(values) == 1 else None
    if not ticket:
        return _json({"error": "not_found"}, 404)
    account_bi, record = await _signed_in(env, request)
    session_id = _request_account_session_id(request) if record else ""
    if (not record
            or not hmac.compare_digest(account_bi, ticket["account_bi"])
            or not hmac.compare_digest(session_id or "",
                                       ticket["session_id"])):
        return _json({"error": "not_found"}, 404)
    member = await _membership(env, ticket["org_bi"], account_bi)
    if not member or not room.ORG_NAME_RE.fullmatch(member["org"]):
        return _json({"error": "not_found"}, 404)
    profile = room.clean_profile({
        "org_bi": ticket["org_bi"],
        "org": member["org"],
        "account_bi": account_bi,
        "session_id": session_id,
        "name": str(record.get("name") or ""),
        "teams": member["teams"],
        "role": member["role"],
        "nonce": ticket["nonce"],
        "retention_ms": _retention_ms(record),
        "pic": account_pic(record),
    })
    if not profile:
        return _json({"error": "not_found"}, 404)
    headers = {}
    for name in HANDOFF_HEADERS:
        value = request.headers.get(name)
        if value:
            headers[name] = str(value)
    headers[room.CLAIM_HEADER] = room.encode_claim(profile)
    source = urlparse(request.url)
    target = source.scheme + "://" + source.netloc + room_path(
        ticket["org_bi"])
    binding = env.FORKMESH_ORG_OFFICE
    object_id = binding.idFromName(room_object_name(ticket["org_bi"]))
    last_error = None
    for _attempt in range(2):
        try:
            forwarded = JsRequest.new(
                target, to_js({"method": "GET", "headers": headers}))
            return await binding.get(object_id).fetch(forwarded)
        except Exception as error:
            last_error = error
    await log_durable_object_abort(env, request, SOCKET_PATH, last_error)
    return json_response({"error": "unavailable"}, status=503,
                         extra_headers=EXPECTED_DEGRADED_HEADERS)
