import json
import re
from urllib.parse import parse_qs, urlencode, urlparse

SETTING_KEY = "giphy"
RATINGS = ("g", "pg", "pg-13", "r")
DEFAULT_RATING = "pg"
KEY_RE = re.compile(r"^[A-Za-z0-9]{8,128}$")
ID_RE = re.compile(r"^[A-Za-z0-9]{4,64}$")
PAGE = 24
MAX_OFFSET = 499
MAX_QUERY = 50
TRENDING_SECONDS = 300
SEARCH_SECONDS = 600
CACHE_ENTRIES = 128
RATE_WINDOW_MS = 60 * 1000
RATE_MAX_PER_WINDOW = 30
TIMEOUT_SECONDS = 5
API = "https://api.giphy.com/v1/gifs/"
MEDIA = "https://media.giphy.com/media/%s/%s"
ATTRIBUTION = "Powered by GIPHY"
CONFIG_CACHE = "public, max-age=60, stale-while-revalidate=300"
PRIVATE_CACHE = "private, no-store"
GIF_URL_RE = re.compile(
    r"^https://media[0-4]?\.giphy\.com/media/(?:v1\.[A-Za-z0-9_=.-]{1,200}/)?"
    r"([A-Za-z0-9]{4,64})/(giphy|200|200w|giphy-downsized)\.gif$")
_cache = {}
_config = {}


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


def gif_still(url):
    match = GIF_URL_RE.fullmatch(str(url or ""))
    if not match:
        return ""
    return url[:-len(".gif")] + "_s.gif"


def clean_rating(value):
    rating = str(value or "").strip().lower()
    return rating if rating in RATINGS else DEFAULT_RATING


def result(item):
    if not isinstance(item, dict):
        return None
    gif_id = str(item.get("id") or "")
    if not ID_RE.fullmatch(gif_id):
        return None
    title = " ".join(str(item.get("title") or "").split())[:100] or "GIF"
    images = item.get("images") if isinstance(item.get("images"), dict) else {}
    preview = images.get("fixed_width") if isinstance(
        images.get("fixed_width"), dict) else {}
    try:
        width = max(1, min(1000, int(preview.get("width") or 200)))
        height = max(1, min(1000, int(preview.get("height") or 200)))
    except (TypeError, ValueError):
        width, height = 200, 200
    return {"id": gif_id, "title": title,
            "url": MEDIA % (gif_id, "200.gif"),
            "preview": MEDIA % (gif_id, "200w.gif"),
            "still": MEDIA % (gif_id, "200w_s.gif"),
            "width": width, "height": height}


async def settings(env):
    now = int(Date.now())  # noqa: F821
    if _config and now - _config.get("at", 0) < 30 * 1000:
        return _config["value"]
    try:
        row = await d1_first(  # noqa: F821
            env, "SELECT doc FROM instance_settings WHERE k=?", SETTING_KEY)
        doc = json.loads((row or {}).get("doc") or "{}")
    except Exception:
        doc = {}
    key = ""
    if isinstance(doc, dict) and doc.get("key"):
        try:
            sealed = await decrypt_row(env, doc["key"])  # noqa: F821
            key = str((sealed or {}).get("key") or "")
        except Exception:
            key = ""
    value = {"key": key if KEY_RE.fullmatch(key) else "",
             "rating": clean_rating((doc or {}).get("rating")
                                    if isinstance(doc, dict) else "")}
    _config.clear()
    _config.update({"at": now, "value": value})
    return value


def forget():
    _config.clear()
    _cache.clear()


async def _giphy(path, params):
    url = API + path + "?" + urlencode(params)
    response = await js_fetch_with_timeout(  # noqa: F821
        url, {"method": "GET", "headers": {"accept": "application/json"}},
        TIMEOUT_SECONDS)
    status = int(getattr(response, "status", 0) or 0)
    if status != 200:
        return None
    try:
        return json.loads(str(await response.text())[:2 * 1024 * 1024])
    except Exception:
        return None


async def _rate_limited(env, account_bi, now):
    row = await d1_first(  # noqa: F821
        env,
        "INSERT INTO comment_extras_rate (k, window_start, count) "
        "VALUES (?,?,1) ON CONFLICT(k) DO UPDATE SET "
        "count=CASE WHEN ?-comment_extras_rate.window_start>=? "
        "THEN 1 ELSE comment_extras_rate.count+1 END, "
        "window_start=CASE WHEN ?-comment_extras_rate.window_start>=? "
        "THEN ? ELSE comment_extras_rate.window_start END "
        "RETURNING count", "gifs:" + account_bi, now, now, RATE_WINDOW_MS,
        now, RATE_WINDOW_MS, now)
    return int((row or {}).get("count") or 1) > RATE_MAX_PER_WINDOW


def _error(code, status=400, headers=None):
    return json_response({"error": code}, status=status,  # noqa: F821
                         cache_control=PRIVATE_CACHE, extra_headers=headers)


async def _search(env, request, kind):
    config = await settings(env)
    if not config["key"]:
        return _error("gifs_disabled", 404)
    account_bi, rec = await _account_session_record(env, request, None)  # noqa: F821
    if not rec or not account_bi:
        return _error("unauthorized", 401)
    query = parse_qs(urlparse(str(request.url)).query)
    text = " ".join(str((query.get("q") or [""])[0] or "").split())[:MAX_QUERY]
    try:
        offset = max(0, min(MAX_OFFSET, int((query.get("offset") or ["0"])[0])))
    except (TypeError, ValueError):
        offset = 0
    if kind == "search" and not text:
        return _error("query_required")
    key = (kind, text.lower(), offset, config["rating"])
    now = int(Date.now())  # noqa: F821
    kept = _cache.get(key)
    ttl = (TRENDING_SECONDS if kind == "trending" else SEARCH_SECONDS) * 1000
    if kept and now - kept[0] < ttl:
        payload = kept[1]
    else:
        if await _rate_limited(env, account_bi, now):
            return _error("rate_limited", 429, {"Retry-After": "30"})
        params = {"api_key": config["key"], "limit": PAGE, "offset": offset,
                  "rating": config["rating"]}
        if kind == "search":
            params.update({"q": text, "lang": "en"})
        data = await _giphy(kind, params)
        if not isinstance(data, dict) or not isinstance(data.get("data"), list):
            return _error("gifs_unavailable", 502)
        gifs = [item for item in (result(row) for row in data["data"]) if item]
        pagination = data.get("pagination") if isinstance(
            data.get("pagination"), dict) else {}
        try:
            total = int(pagination.get("total_count") or 0)
        except (TypeError, ValueError):
            total = 0
        following = offset + PAGE
        payload = {"ok": True, "kind": kind, "query": text, "gifs": gifs,
                   "next": following if following < min(total, MAX_OFFSET + 1)
                   and gifs else None,
                   "attribution": ATTRIBUTION}
        if len(_cache) >= CACHE_ENTRIES:
            _cache.clear()
        _cache[key] = (now, payload)
    return json_response(payload, cache_control="private, max-age=300")  # noqa: F821


async def _admin(env, request, data=None):
    account_bi, rec = await _account_session_record(env, request, data)  # noqa: F821
    account = clean_string((rec or {}).get("name"), MAX_NODE_NAME).lower()  # noqa: F821
    if not rec or not account:
        return "", _error("unauthorized", 401)
    if not await _is_admin(env, account):  # noqa: F821
        return "", _error("forbidden", 403)
    return account, None


async def _settings(env, request):
    if method_name(request) == "GET":  # noqa: F821
        _account, refused = await _admin(env, request)
        if refused is not None:
            return refused
        config = await settings(env)
        return json_response(  # noqa: F821
            {"ok": True, "configured": bool(config["key"]),
             "rating": config["rating"], "ratings": list(RATINGS)},
            cache_control=PRIVATE_CACHE)
    try:
        data = await bounded_json_request(request)  # noqa: F821
    except Exception:
        return _error("invalid_json")
    if not isinstance(data, dict):
        return _error("invalid_json")
    account, refused = await _admin(env, request, data)
    if refused is not None:
        return refused
    rating = str(data.get("rating") or DEFAULT_RATING).strip().lower()
    if rating not in RATINGS:
        return _error("invalid_rating")
    try:
        row = await d1_first(  # noqa: F821
            env, "SELECT doc FROM instance_settings WHERE k=?", SETTING_KEY)
        doc = json.loads((row or {}).get("doc") or "{}")
    except Exception:
        doc = {}
    doc = doc if isinstance(doc, dict) else {}
    key = data.get("key")
    if data.get("clear") is True:
        doc.pop("key", None)
    elif isinstance(key, str) and key.strip():
        key = key.strip()
        if not KEY_RE.fullmatch(key):
            return _error("invalid_key")
        doc["key"] = await encrypt_row(env, {"key": key})  # noqa: F821
    doc["rating"] = rating
    await d1_run(  # noqa: F821
        env, "INSERT INTO instance_settings (k, doc, updated_at) VALUES (?,?,?) "
        "ON CONFLICT(k) DO UPDATE SET doc=excluded.doc, "
        "updated_at=excluded.updated_at", SETTING_KEY, json.dumps(doc),
        int(Date.now()))  # noqa: F821
    forget()
    try:
        await _audit_sensitive_action(  # noqa: F821
            env, account, "admin.gifs_settings", "instance_settings",
            SETTING_KEY, "success",
            {"configured": bool(doc.get("key")), "rating": rating})
    except Exception:
        pass
    return json_response(  # noqa: F821
        {"ok": True, "configured": bool(doc.get("key")), "rating": rating},
        cache_control=PRIVATE_CACHE)


async def handler(env, request, rest):
    await ensure_schema(env)  # noqa: F821
    method = method_name(request)  # noqa: F821
    if rest == "settings":
        if method not in ("GET", "POST"):
            return _error("method_not_allowed", 405)
        return await _settings(env, request)
    if method != "GET":
        return _error("method_not_allowed", 405)
    if rest == "config":
        config = await settings(env)
        return json_response(  # noqa: F821
            {"ok": True, "enabled": bool(config["key"]),
             "rating": config["rating"], "attribution": ATTRIBUTION},
            cache_control=CONFIG_CACHE)
    if rest in ("search", "trending"):
        return await _search(env, request, rest)
    return _error("not_found", 404)

