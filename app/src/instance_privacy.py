import hashlib
import hmac
import ipaddress
import json
import os
import time

SETTING_KEY = "privacy"
ENV_NAME = "INSTANCE_PRIVACY_MODE"
STRICT = "strict"
OFF = "off"
MODES = (OFF, STRICT)
CACHE_MS = 30 * 1000
SALT_ROTATE_S = 24 * 60 * 60
PSEUDONYM_PREFIX = b"\xfd\x3a\x9c\x41\x7e"
PSEUDONYM_NETWORK = ipaddress.ip_network("fd3a:9c41:7e00::/40")
ON_WORDS = ("1", "true", "yes", "on", STRICT)
NOTE = "Privacy mode: no visitor logs"
DISCARD_PATHS = frozenset({
    "/api/client-errors", "/api/desktop-errors", "/api/attribution/capture",
    "/api/attribution/events", "/api/not-found",
})
DISCARD_PREFIXES = ("/api/analytics/_/",)

_cache = {"at": -CACHE_MS, "mode": None, "loaded": False}
_salt = {"slot": None, "key": b""}


def forced(env):
    value = str(getattr(env, ENV_NAME, "") or "").strip().lower()
    return value in ON_WORDS


def parse(doc):
    try:
        data = json.loads(doc) if doc else None
    except (TypeError, ValueError):
        return None
    if not isinstance(data, dict):
        return None
    mode = str(data.get("mode") or "").strip().lower()
    return mode if mode in MODES else None


def dump(mode):
    return json.dumps({"mode": STRICT if mode == STRICT else OFF},
                      separators=(",", ":"))


def remember(mode, now_ms):
    _cache["mode"] = mode if mode in MODES else None
    _cache["at"] = now_ms
    _cache["loaded"] = True


def forget():
    _cache["mode"] = None
    _cache["at"] = -CACHE_MS
    _cache["loaded"] = False


def fresh(now_ms):
    return _cache["loaded"] and now_ms - _cache["at"] < CACHE_MS


def stored():
    return _cache["mode"]


def active(env):
    return forced(env) or _cache["mode"] == STRICT


def _row_doc(row):
    if row is None:
        return ""
    if hasattr(row, "to_py"):
        row = row.to_py()
    if isinstance(row, dict):
        return str(row.get("doc") or "")
    return str(row or "")


async def load(env, now_ms=None):
    now_ms = int(time.time() * 1000) if now_ms is None else now_ms
    if fresh(now_ms):
        return active(env)
    database = getattr(env, "DB", None)
    if database is None:
        remember(_cache["mode"], now_ms)
        return active(env)
    try:
        row = await database.prepare(
            "SELECT doc FROM instance_settings WHERE k=?").bind(
            SETTING_KEY).first()
    except Exception:
        remember(_cache["mode"], now_ms)
        return active(env)
    remember(parse(_row_doc(row)), now_ms)
    return active(env)


def _key(now_s):
    slot = int(now_s // SALT_ROTATE_S)
    if _salt["slot"] != slot or not _salt["key"]:
        _salt["slot"] = slot
        _salt["key"] = os.urandom(32)
    return _salt["key"]


def _digest(key, data, size):
    return hmac.new(key, data, hashlib.sha256).digest()[:size]


def _networks(env):
    networks = []
    raw = str(getattr(env, "SSH_GATEWAY_ALLOWED_ADDRESSES", "") or "")
    for item in raw.split(","):
        item = item.strip()
        if not item:
            continue
        try:
            networks.append(ipaddress.ip_network(item, strict=False))
        except ValueError:
            continue
    return networks


def pseudonym(address, env=None, now_s=None):
    text = str(address or "").strip().strip("[]").split("%")[0]
    try:
        parsed = ipaddress.ip_address(text)
    except ValueError:
        return ""
    parsed = getattr(parsed, "ipv4_mapped", None) or parsed
    if parsed.is_loopback:
        return str(parsed)
    if parsed.version == 6 and parsed in PSEUDONYM_NETWORK:
        return str(parsed)
    for network in _networks(env) if env is not None else ():
        if parsed.version == network.version and parsed in network:
            return str(parsed)
    key = _key(time.time() if now_s is None else now_s)
    if parsed.version == 6:
        prefix = parsed.packed[:8]
        head = _digest(key, b"6p" + prefix, 3)
        tail = _digest(key, b"6a" + parsed.packed, 8)
    else:
        head = _digest(key, b"4p" + parsed.packed, 3)
        tail = _digest(key, b"4a" + parsed.packed, 8)
    return str(ipaddress.IPv6Address(PSEUDONYM_PREFIX + head + tail))


def describe(env):
    return {"on": active(env), "forced": forced(env),
            "note": NOTE if active(env) else ""}


def is_pseudonym(address):
    try:
        parsed = ipaddress.ip_address(
            str(address or "").strip().strip("[]").split("%")[0])
    except ValueError:
        return False
    return parsed.version == 6 and parsed in PSEUDONYM_NETWORK


def discards(env, method, path):
    if str(method or "").upper() != "POST" or not active(env):
        return False
    path = str(path or "")
    return path.rstrip("/") in DISCARD_PATHS or path.startswith(
        DISCARD_PREFIXES)
