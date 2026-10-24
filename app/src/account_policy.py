import json

SETTING_KEY = "accounts"
SIGNUP_MODES = ("required", "optional", "invite", "closed")
SIGNUP_LABELS = {
    "required": "Open, email required",
    "optional": "Open, email optional",
    "invite": "Invite code only, email optional",
    "closed": "Closed",
}
HANDLE_MODES = ("off", "suggest", "require")
HANDLE_LABELS = {
    "off": "People choose their own username",
    "suggest": "Suggest a random name (people may change it)",
    "require": "Random names only",
}
NUMBER_LIMITS = {
    "newAccountDays": (0, 365),
    "newThreadsPerDay": (0, 1000),
    "newCommentsPerDay": (0, 10000),
}
DEFAULTS = {
    "signup": "required",
    "handles": "off",
    "accountRequired": False,
    "firstPostApproval": False,
    "approvalSince": 0,
    "newAccountDays": 7,
    "newThreadsPerDay": 0,
    "newCommentsPerDay": 0,
    "hideBlockedPosts": True,
    "inviteSetCode": False,
}
RECOMMENDED = {
    "signup": "optional",
    "handles": "suggest",
    "accountRequired": True,
    "firstPostApproval": True,
    "newAccountDays": 7,
    "newThreadsPerDay": 3,
    "newCommentsPerDay": 20,
    "hideBlockedPosts": True,
}
MAX_DOC_BYTES = 4096
CACHE_MS = 30 * 1000
DAY_MS = 24 * 60 * 60 * 1000
_cache = {"at": 0, "value": None}


def _number(value, key):
    low, high = NUMBER_LIMITS[key]
    try:
        number = int(str(value).strip())
    except (TypeError, ValueError):
        return DEFAULTS[key]
    return max(low, min(high, number))


def clean(raw):
    setting = dict(DEFAULTS)
    if not isinstance(raw, dict):
        return setting
    if raw.get("signup") in SIGNUP_MODES:
        setting["signup"] = raw["signup"]
    if raw.get("handles") in HANDLE_MODES:
        setting["handles"] = raw["handles"]
    for key in ("accountRequired", "firstPostApproval", "hideBlockedPosts",
                "inviteSetCode"):
        if isinstance(raw.get(key), bool):
            setting[key] = raw[key]
    for key in NUMBER_LIMITS:
        if key in raw:
            setting[key] = _number(raw[key], key)
    try:
        since = int(raw.get("approvalSince") or 0)
    except (TypeError, ValueError):
        since = 0
    setting["approvalSince"] = max(0, since)
    return setting


def parse(doc):
    if not doc or len(str(doc)) > MAX_DOC_BYTES:
        return dict(DEFAULTS)
    try:
        raw = json.loads(doc)
    except (TypeError, ValueError):
        return dict(DEFAULTS)
    return clean(raw)


def dump(setting):
    return json.dumps(clean(setting), sort_keys=True, separators=(",", ":"))


def _first(form, key):
    value = form.get(key) if isinstance(form, dict) else None
    if isinstance(value, list):
        value = value[0] if value else ""
    return str(value or "")


def from_form(form, before, now):
    before = clean(before)
    after = clean({
        "signup": _first(form, "signup"),
        "handles": _first(form, "handles"),
        "accountRequired": _first(form, "accountRequired") == "1",
        "firstPostApproval": _first(form, "firstPostApproval") == "1",
        "hideBlockedPosts": _first(form, "hideBlockedPosts") == "1",
        "newAccountDays": _first(form, "newAccountDays"),
        "newThreadsPerDay": _first(form, "newThreadsPerDay"),
        "newCommentsPerDay": _first(form, "newCommentsPerDay"),
        "inviteSetCode": before["inviteSetCode"],
        "approvalSince": before["approvalSince"],
    })
    if after["firstPostApproval"] and not before["firstPostApproval"]:
        after["approvalSince"] = int(now)
    if not after["firstPostApproval"]:
        after["approvalSince"] = 0
    return after


def changes(before, after):
    before, after = clean(before), clean(after)
    return {key: after[key] for key in DEFAULTS
            if key not in ("approvalSince", "inviteSetCode")
            and before[key] != after[key]}


def email_optional(setting):
    return clean(setting)["signup"] in ("optional", "invite")


def signup_closed(setting):
    return clean(setting)["signup"] == "closed"


def invite_only(setting):
    return clean(setting)["signup"] == "invite"


def account_created_at(rec):
    try:
        return int((rec or {}).get("created_at") or 0)
    except (TypeError, ValueError):
        return 0


def is_new_account(setting, rec, now):
    setting = clean(setting)
    days = setting["newAccountDays"]
    created = account_created_at(rec)
    if days <= 0 or not created:
        return False
    return int(now) - created < days * DAY_MS


def daily_limit(setting, kind):
    setting = clean(setting)
    if kind == "thread":
        return setting["newThreadsPerDay"]
    if kind == "comment":
        return setting["newCommentsPerDay"]
    return 0


def needs_approval(setting, rec):
    setting = clean(setting)
    if not setting["firstPostApproval"]:
        return False
    if not isinstance(rec, dict):
        return True
    if rec.get("moderation_trusted_at"):
        return False
    created = account_created_at(rec)
    since = setting["approvalSince"]
    return not (since and created and created < since)


def day_number(now):
    return int(now) // DAY_MS


def signup_page_doc(setting, suggestion=""):
    setting = clean(setting)
    return {
        "signup": setting["signup"],
        "emailOptional": email_optional(setting),
        "handles": setting["handles"],
        "suggestion": suggestion if setting["handles"] != "off" else "",
    }


def remember(setting, now):
    _cache["value"] = clean(setting)
    _cache["at"] = int(now)


def cached(now):
    value = _cache.get("value")
    if value is None or int(now) - int(_cache.get("at") or 0) >= CACHE_MS:
        return None
    return dict(value)


def forget():
    _cache["value"] = None
    _cache["at"] = 0
