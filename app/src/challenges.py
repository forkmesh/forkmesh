import re
import unicodedata
from datetime import date, timedelta


KINDS = ("commits", "pulls", "reviews", "issues")
KIND_LABELS = {
    "commits": "Commits",
    "pulls": "Pull requests",
    "reviews": "Reviews",
    "issues": "Issues",
}
METRICS = {
    "points": {
        "label": "Code points",
        "description": "Commits 1, pull requests 5, reviews 2, issues 1.",
        "weights": {"commits": 1, "pulls": 5, "reviews": 2, "issues": 1},
    },
    "commits": {
        "label": "Commits",
        "description": "One point per commit.",
        "weights": {"commits": 1, "pulls": 0, "reviews": 0, "issues": 0},
    },
    "pulls": {
        "label": "Pull requests",
        "description": "One point per pull request.",
        "weights": {"commits": 0, "pulls": 1, "reviews": 0, "issues": 0},
    },
    "reviews": {
        "label": "Reviews",
        "description": "One point per review.",
        "weights": {"commits": 0, "pulls": 0, "reviews": 1, "issues": 0},
    },
    "custom": {
        "label": "Custom points",
        "description": "Your own points for each kind of work.",
        "weights": None,
    },
}
DEFAULT_PRESET = "week"
PRESETS = {
    "week": {
        "label": "7-day code challenge",
        "description": "A week of code points from today, up to 10 people.",
        "settings": {"name": "7-day code challenge", "metric": "points",
                     "days": 7, "start": "today", "dailyCap": 0,
                     "maxMembers": 10},
    },
    "sprint": {
        "label": "3-day commit sprint",
        "description": "Most commits in three days, starting today.",
        "settings": {"name": "3-day commit sprint", "metric": "commits",
                     "days": 3, "start": "today", "dailyCap": 0,
                     "maxMembers": 10},
    },
    "month": {
        "label": "30-day marathon",
        "description": "A month of code points with a 100 point daily cap.",
        "settings": {"name": "30-day marathon", "metric": "points",
                     "days": 30, "start": "today", "dailyCap": 100,
                     "maxMembers": 20},
    },
}
MIN_DAYS = 1
MAX_DAYS = 31
MAX_START_AHEAD_DAYS = 30
MIN_MEMBERS = 2
MAX_MEMBERS = 50
MAX_NAME = 60
MAX_WEIGHT = 100
MAX_DAILY_CAP = 100_000
MAX_OWNED_OPEN = 5
MAX_JOINED_OPEN = 20
ID_RE = re.compile(r"^[0-9a-f]{32}$")
DATE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}$")
SETTING_KEYS = {"name", "metric", "weights", "days", "start", "dailyCap",
                "maxMembers"}


class ChallengeError(ValueError):
    pass


def _fail(code):
    raise ChallengeError(code)


def _int(value, code, low, high):
    if isinstance(value, bool):
        _fail(code)
    if isinstance(value, str) and re.fullmatch(r"\d{1,9}", value.strip()):
        value = int(value.strip())
    if type(value) is not int or value < low or value > high:
        _fail(code)
    return value


def clean_name(value):
    if not isinstance(value, str):
        _fail("invalid_name")
    text = " ".join(unicodedata.normalize("NFC", value).split())
    if not text or len(text) > MAX_NAME:
        _fail("invalid_name")
    if any(unicodedata.category(char)[0] == "C" for char in text):
        _fail("invalid_name")
    return text


def clean_weights(value):
    if not isinstance(value, dict) or set(value) - set(KINDS):
        _fail("invalid_weights")
    weights = {kind: _int(value.get(kind, 0), "invalid_weights", 0,
                          MAX_WEIGHT) for kind in KINDS}
    if not any(weights.values()):
        _fail("invalid_weights")
    return weights


def parse_day(value):
    if not isinstance(value, str) or not DATE_RE.fullmatch(value):
        _fail("invalid_start")
    try:
        return date.fromisoformat(value)
    except ValueError:
        _fail("invalid_start")


def _start_day(value, today):
    if value in (None, "", "today"):
        return today
    if value == "tomorrow":
        return today + timedelta(days=1)
    start = parse_day(value)
    if start < today or start > today + timedelta(days=MAX_START_AHEAD_DAYS):
        _fail("invalid_start")
    return start


def settings_from_request(raw, today):
    if not isinstance(raw, dict):
        _fail("invalid_settings")
    preset = raw.get("preset")
    if preset is not None:
        if preset not in PRESETS or set(raw) - {"preset"}:
            _fail("invalid_preset")
        raw = dict(PRESETS[preset]["settings"])
    if set(raw) - SETTING_KEYS:
        _fail("invalid_settings")
    name = clean_name(raw.get("name"))
    metric = raw.get("metric", "points")
    if metric not in METRICS:
        _fail("invalid_metric")
    if metric == "custom":
        weights = clean_weights(raw.get("weights"))
    else:
        weights = dict(METRICS[metric]["weights"])
    days = _int(raw.get("days", 7), "invalid_days", MIN_DAYS, MAX_DAYS)
    start = _start_day(raw.get("start"), today)
    end = start + timedelta(days=days - 1)
    return {
        "name": name,
        "metric": metric,
        "weights": weights,
        "dailyCap": _int(raw.get("dailyCap", 0), "invalid_daily_cap", 0,
                         MAX_DAILY_CAP),
        "maxMembers": _int(raw.get("maxMembers", 10), "invalid_max_members",
                           MIN_MEMBERS, MAX_MEMBERS),
        "startsOn": start.isoformat(),
        "endsOn": end.isoformat(),
    }


def stored_settings(data):
    data = data if isinstance(data, dict) else {}
    metric = data.get("metric") if data.get("metric") in METRICS else "points"
    try:
        weights = clean_weights(data.get("weights"))
    except ChallengeError:
        weights = dict(METRICS[metric]["weights"] or METRICS["points"]["weights"])
    try:
        cap = _int(data.get("dailyCap", 0), "x", 0, MAX_DAILY_CAP)
    except ChallengeError:
        cap = 0
    try:
        members = _int(data.get("maxMembers", 10), "x", MIN_MEMBERS,
                       MAX_MEMBERS)
    except ChallengeError:
        members = 10
    try:
        name = clean_name(data.get("name"))
    except ChallengeError:
        name = "Code challenge"
    return {"name": name, "metric": metric, "weights": weights,
            "dailyCap": cap, "maxMembers": members}


def phase(starts_on, ends_on, today):
    today = today.isoformat()
    if today < starts_on:
        return "upcoming"
    if today <= ends_on:
        return "active"
    return "finished"


def window_days(starts_on, ends_on, today):
    start = date.fromisoformat(starts_on)
    end = min(date.fromisoformat(ends_on), today)
    days = []
    while start <= end:
        days.append(start.isoformat())
        start += timedelta(days=1)
    return days


def day_score(row, weights, cap):
    total = 0
    for kind in KINDS:
        try:
            count = max(0, int(row.get(kind) or 0))
        except (TypeError, ValueError):
            count = 0
        total += count * weights.get(kind, 0)
    return min(total, cap) if cap else total


def board(challenge, members, rows, today, viewer_bi=""):
    settings = challenge["settings"]
    days = window_days(challenge["startsOn"], challenge["endsOn"], today)
    scores = {}
    counts = {}
    for row in rows or []:
        user_bi = row.get("user_bi")
        day = row.get("day")
        if user_bi is None or day not in days:
            continue
        scores.setdefault(user_bi, {})[day] = day_score(
            row, settings["weights"], settings["dailyCap"])
        tally = counts.setdefault(user_bi, dict.fromkeys(KINDS, 0))
        for kind in KINDS:
            try:
                tally[kind] += max(0, int(row.get(kind) or 0))
            except (TypeError, ValueError):
                pass
    entries = []
    for member in members or []:
        user_bi = member.get("account_bi")
        per_day = [scores.get(user_bi, {}).get(day, 0) for day in days]
        entries.append({
            "name": member.get("name") or "",
            "you": bool(viewer_bi) and user_bi == viewer_bi,
            "owner": user_bi == challenge.get("ownerBi"),
            "total": sum(per_day),
            "today": per_day[-1] if per_day and days[-1] == today.isoformat()
            else 0,
            "days": per_day,
            "counts": counts.get(user_bi, dict.fromkeys(KINDS, 0)),
            "joinedAt": int(member.get("joined_at") or 0),
        })
    entries.sort(key=lambda item: (-item["total"], item["joinedAt"],
                                   item["name"]))
    rank = 0
    previous = None
    for index, entry in enumerate(entries):
        if entry["total"] != previous:
            rank = index + 1
            previous = entry["total"]
        entry["rank"] = rank
    current = phase(challenge["startsOn"], challenge["endsOn"], today)
    winners = []
    if current == "finished" and entries and entries[0]["total"] > 0:
        winners = [entry["name"] for entry in entries if entry["rank"] == 1]
    return {"days": days, "entries": entries, "phase": current,
            "winners": winners}


def days_left(starts_on, ends_on, today):
    current = phase(starts_on, ends_on, today)
    if current == "upcoming":
        return (date.fromisoformat(starts_on) - today).days
    if current == "active":
        return (date.fromisoformat(ends_on) - today).days + 1
    return 0


def public_settings(settings):
    metric = METRICS[settings["metric"]]
    return {
        "name": settings["name"],
        "metric": settings["metric"],
        "metricLabel": metric["label"],
        "weights": dict(settings["weights"]),
        "dailyCap": settings["dailyCap"],
        "maxMembers": settings["maxMembers"],
    }


def options():
    return {
        "defaultPreset": DEFAULT_PRESET,
        "presets": [
            {"id": key, "label": value["label"],
             "description": value["description"],
             "settings": dict(value["settings"])}
            for key, value in PRESETS.items()
        ],
        "metrics": [
            {"id": key, "label": value["label"],
             "description": value["description"],
             "weights": dict(value["weights"]) if value["weights"] else None}
            for key, value in METRICS.items()
        ],
        "kinds": [{"id": kind, "label": KIND_LABELS[kind]} for kind in KINDS],
        "limits": {
            "minDays": MIN_DAYS, "maxDays": MAX_DAYS,
            "maxStartAheadDays": MAX_START_AHEAD_DAYS,
            "minMembers": MIN_MEMBERS, "maxMembers": MAX_MEMBERS,
            "maxName": MAX_NAME, "maxWeight": MAX_WEIGHT,
            "maxDailyCap": MAX_DAILY_CAP,
            "maxOwnedOpen": MAX_OWNED_OPEN,
            "maxJoinedOpen": MAX_JOINED_OPEN,
        },
    }
