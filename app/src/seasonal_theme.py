import json

SETTING_KEY = "season"
SEASONS = ("halloween",)
MANAGER_ROLES = ("platform_administrator", "moderator")
MIN_LEAD_MS = 5 * 60 * 1000
MAX_LEAD_MS = 60 * 24 * 60 * 60 * 1000
CACHE_MS = 30 * 1000
PUBLIC_CACHE = "public, max-age=60, stale-while-revalidate=60"
PRIVATE_CACHE = "private, no-store"
_cache = {}


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


def clean_doc(doc):
    doc = doc if isinstance(doc, dict) else {}
    season = str(doc.get("season") or "")
    try:
        until = max(0, int(doc.get("until") or 0))
        at = max(0, int(doc.get("at") or 0))
    except (TypeError, ValueError):
        until, at = 0, 0
    return {"season": season if season in SEASONS else "",
            "until": until if season in SEASONS else 0,
            "by": str(doc.get("by") or "")[:63], "at": at}


def active(doc, now):
    doc = clean_doc(doc)
    if doc["season"] and doc["until"] > now:
        return doc["season"]
    return ""


def forget():
    _cache.clear()


async def stored(env):
    now = int(Date.now())  # noqa: F821
    if _cache and now - _cache.get("at", 0) < CACHE_MS:
        return _cache["doc"]
    try:
        row = await d1_first(  # noqa: F821
            env, "SELECT doc FROM instance_settings WHERE k=?", SETTING_KEY)
        doc = clean_doc(json.loads((row or {}).get("doc") or "{}"))
    except Exception:
        doc = clean_doc({})
    _cache.clear()
    _cache.update({"at": now, "doc": doc})
    return doc


def _error(code, status=400):
    return json_response({"error": code}, status=status,  # noqa: F821
                         cache_control=PRIVATE_CACHE)


async def _manager(env, request, data=None):
    _account_bi, rec = await _account_session_record(env, request, data)  # noqa: F821
    account = clean_string((rec or {}).get("name"), MAX_NODE_NAME).lower()  # noqa: F821
    if not rec or not account:
        return "", _error("unauthorized", 401)
    for role in MANAGER_ROLES:
        try:
            if await _has_role(env, account, role):  # noqa: F821
                return account, None
        except Exception:
            continue
    return "", _error("forbidden", 403)


def _settings_payload(doc, now):
    return {"ok": True, "season": active(doc, now) or None,
            "until": doc["until"] if active(doc, now) else 0,
            "by": doc["by"], "at": doc["at"], "seasons": list(SEASONS),
            "maxLeadMs": MAX_LEAD_MS}


async def _settings(env, request):
    now = int(Date.now())  # noqa: F821
    if method_name(request) == "GET":  # noqa: F821
        _account, refused = await _manager(env, request)
        if refused is not None:
            return refused
        forget()
        return json_response(_settings_payload(await stored(env), now),  # noqa: F821
                             cache_control=PRIVATE_CACHE)
    try:
        data = await bounded_json_request(request, max_bytes=4096)  # noqa: F821
    except Exception:
        return _error("invalid_json")
    if not isinstance(data, dict):
        return _error("invalid_json")
    account, refused = await _manager(env, request, data)
    if refused is not None:
        return refused
    season = str(data.get("season") or "")
    if season and season not in SEASONS:
        return _error("invalid_season")
    until = 0
    if season:
        try:
            until = int(data.get("until") or 0)
        except (TypeError, ValueError):
            return _error("invalid_until")
        if until < now + MIN_LEAD_MS or until > now + MAX_LEAD_MS:
            return _error("invalid_until")
    doc = {"season": season, "until": until, "by": account, "at": now}
    await d1_run(  # noqa: F821
        env, "INSERT INTO instance_settings (k, doc, updated_at) VALUES (?,?,?) "
        "ON CONFLICT(k) DO UPDATE SET doc=excluded.doc, "
        "updated_at=excluded.updated_at", SETTING_KEY, json.dumps(doc), now)
    forget()
    try:
        await _audit_sensitive_action(  # noqa: F821
            env, account, "admin.season", "instance_settings", SETTING_KEY,
            "success", {"season": season or "off", "until": until})
    except Exception:
        pass
    return json_response(_settings_payload(clean_doc(doc), now),  # noqa: F821
                         cache_control=PRIVATE_CACHE)


async def handler(env, request, rest):
    await ensure_schema(env)  # noqa: F821
    method = method_name(request)  # noqa: F821
    if rest == "settings":
        if method not in ("GET", "POST"):
            return _error("method_not_allowed", 405)
        return await _settings(env, request)
    if rest:
        return _error("not_found", 404)
    if method not in ("GET", "HEAD"):
        return _error("method_not_allowed", 405)
    now = int(Date.now())  # noqa: F821
    doc = await stored(env)
    season = active(doc, now)
    return json_response(  # noqa: F821
        {"ok": True, "season": season or None,
         "until": doc["until"] if season else 0},
        cache_control=PUBLIC_CACHE)
