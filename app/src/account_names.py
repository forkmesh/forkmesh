import re

FIELD = "nameCase"
MAX_BATCH = 50
CACHE_MS = 5 * 60 * 1000
NAME_RE = re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")
CASE_RE = re.compile(r"^[A-Za-z](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?$")

_cache = {}


def account_key(name):
    return str(name or "").strip().lower()


def clean_case(name, requested):
    key = account_key(name)
    text = str(requested or "").strip()
    if not key or not NAME_RE.match(key) or not CASE_RE.match(text):
        return ""
    return text if text.lower() == key else ""


def display_name(rec, fallback=""):
    rec = rec if isinstance(rec, dict) else {}
    name = account_key(rec.get("name") or fallback)
    chosen = clean_case(name, rec.get(FIELD, ""))
    return chosen or name


def apply_case(rec, requested):
    name = account_key(rec.get("name", ""))
    text = str(requested or "").strip()
    if text and not clean_case(name, text):
        return False, "bad_name_case"
    chosen = clean_case(name, text)
    before = rec.get(FIELD, "")
    if chosen and chosen != name:
        rec[FIELD] = chosen
    else:
        rec.pop(FIELD, None)
    return rec.get(FIELD, "") != before, ""


def batch_names(values):
    names = []
    for value in values:
        for part in str(value or "").split(","):
            key = account_key(part)
            if key and NAME_RE.match(key) and key not in names:
                names.append(key)
            if len(names) >= MAX_BATCH:
                return names
    return names


def publicly_named(rec):
    rec = rec if isinstance(rec, dict) else {}
    return rec.get("status") == "active" and not rec.get("profile_private")


def cached(name, now):
    hit = _cache.get(account_key(name))
    if hit and now - hit[0] < CACHE_MS:
        return hit[1]
    return None


def remember(name, value, now):
    if len(_cache) > 2000:
        _cache.clear()
    _cache[account_key(name)] = (now, value)


def forget(name=""):
    if name:
        _cache.pop(account_key(name), None)
    else:
        _cache.clear()
