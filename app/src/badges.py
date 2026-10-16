"""Pure catalog + validation for ForkMesh achievement badges.

Badges are public recognition marks shown on a profile. Each one is either
awarded automatically at a real, verifiable platform event and / or granted by a
platform administrator; the catalog below is the fixed set of badges that can
exist. The Worker imports this module, but it has no Workers/JavaScript or D1
dependency so the catalog can be unit-tested with normal CPython. D1 reads,
writes, and the event hooks that call into this module live in entry.py.
"""

import re
from datetime import date, timedelta

_SLUG_RE = re.compile(r"^[a-z][a-z0-9_]{1,39}$")
DISTINGUISHED_CONTRIBUTOR_MIN = 15
TIER_NAMES = ("default", "bronze", "silver", "gold")

# Ordered so the catalog and any UI built from it render in a stable,
# meaningful sequence rather than dict-insertion happenstance.
BADGE_DEFINITIONS = {
    "first_100_users": {
        "hidden_until_earned": True,
        "name": "Founding Member",
        "description": "One of the first 100 people to create a ForkMesh account.",
        "icon": "\U0001F947",
    },
    "world_first_hour": {
        "name": "World Explorer",
        "description": "Spent a full hour in the ForkMesh World.",
        "icon": "\U0001F30D",
    },
    "first_referral": {
        "name": "Connector",
        "description": "Brought in the first independently active member through a referral link.",
        "icon": "\U0001F517",
    },
    "mirror_operator": {
        "name": "Mirror Operator",
        "description": "Runs a node that mirrors and helps keep repositories available.",
        "icon": "\U0001FA9E",
    },
    "qa": {
        "name": "QA",
        "description": "Created an issue to help test and improve ForkMesh.",
        "icon": "\U0001F9EA",
    },
    "coder": {
        "name": "Coder",
        "description": "Created a pull request that contributes code to ForkMesh.",
        "icon": "\U0001F4BB",
    },
    "discord": {
        "name": "Discord",
        "description": "Linked a ForkMesh account with Discord.",
        "icon": "\U0001F4AC",
    },
    "poster": {
        "name": "Poster",
        "description": "Started or contributed to a ForkMesh discussion.",
        "icon": "\U0001F4DD",
    },
    "chat": {
        "name": "Chat",
        "description": "Sent a message in ForkMesh chat.",
        "icon": "\U0001F5E8",
    },
    "distinguished_contributor": {
        "name": "Distinguished Contributor",
        "description": "Filed at least %d issues, pull requests or discussions "
                       "that helped build ForkMesh." % DISTINGUISHED_CONTRIBUTOR_MIN,
        "icon": "\U0001F396",
    },
    # Hackathon recognition. Each one is earned at a real, verifiable event:
    # a submitted project, a judged placing, or referrals the attribution
    # ledger actually credited. None of them can be bought, and like every
    # other badge an award is permanent once earned.
    "hackathon_builder": {
        "hidden_until_earned": True,
        "name": "Hackathon Builder",
        "description": "Submitted a valid project to a ForkMesh hackathon.",
        "icon": "\U0001F528",
    },
    "hackathon_connector": {
        "hidden_until_earned": True,
        "name": "Hackathon Connector",
        "description": "Brought ten independently active members into a "
                       "ForkMesh hackathon.",
        "icon": "\U0001F578",
    },
    "hackathon_finalist": {
        "hidden_until_earned": True,
        "name": "Hackathon Finalist",
        "description": "Placed second in a ForkMesh hackathon track.",
        "icon": "\U0001F948",
    },
    "hackathon_ambassador": {
        "hidden_until_earned": True,
        "name": "Hackathon Ambassador",
        "description": "Approved to represent a ForkMesh hackathon as part of "
                       "its street team.",
        "icon": "\U0001F4E3",
    },
    "hackathon_winner": {
        "hidden_until_earned": True,
        "name": "Hackathon Winner",
        "description": "Won a ForkMesh hackathon track.",
        "icon": "\U0001F3C6",
    },
    "stargazer": {
        "name": "Stargazer",
        "description": "Owns a repository that other people starred.",
        "icon": "\u2B50",
        "tiers": (16, 128, 512, 4096),
        "unit": "stars",
    },
    "genies_gift": {
        "name": "Genie's Gift",
        "description": "Paid a ForkMesh contributor, confirmed on chain.",
        "icon": "\U0001FA94",
    },
    "six_eyes": {
        "name": "Six Eyes",
        "description": "Reviewed pull requests.",
        "icon": "\U0001F50D",
        "tiers": (1, 16, 64, 256),
        "unit": "reviews",
    },
    "brood": {
        "name": "Brood",
        "description": "Other people forked repositories you own.",
        "icon": "\U0001F95A",
        "tiers": (2, 16, 64, 256),
        "unit": "forks",
    },
    "fan_club": {
        "name": "Fan Club",
        "description": "Followed by other ForkMesh members.",
        "icon": "\U0001F496",
        "tiers": (10, 50, 250, 1000),
        "unit": "followers",
    },
    "night_watch": {
        "name": "Night Watch",
        "description": "Kept a ForkMesh node online.",
        "icon": "\U0001F319",
        "tiers": (24, 168, 720, 8760),
        "unit": "hours online",
    },
    "daily_spinner": {
        "name": "Daily Spinner",
        "description": "Contributed on many days in a row.",
        "icon": "\U0001F525",
        "tiers": (7, 30, 100, 365),
        "unit": "days in a row",
    },
    "bug_catcher": {
        "name": "Bug Catcher",
        "description": "Reported bugs that were accepted and paid.",
        "icon": "\U0001F41E",
        "tiers": (1, 10, 50, 200),
        "unit": "bug reports",
    },
    "lift_off": {
        "name": "Lift Off",
        "description": "People downloaded releases of repositories you own.",
        "icon": "\U0001F680",
        "tiers": (10, 100, 1000, 10000),
        "unit": "downloads",
    },
    "shopkeeper": {
        "name": "Shopkeeper",
        "description": "Published a marketplace package that other members use.",
        "icon": "\U0001F6CD",
        "tiers": (10, 100, 1000, 10000),
        "unit": "users",
    },
    "critic": {
        "name": "Critic",
        "description": "Wrote marketplace reviews that others found helpful.",
        "icon": "\U0001F58B",
        "tiers": (1, 10, 50, 200),
        "unit": "helpful reviews",
    },
    "bounty_hunter": {
        "name": "Bounty Hunter",
        "description": "Was paid for completed ForkMesh work.",
        "icon": "\U0001FA99",
        "tiers": (1, 5, 25, 100),
        "unit": "paid jobs",
    },
    "web_weaver": {
        "name": "Web Weaver",
        "description": "Had pull requests merged into repositories you do not own.",
        "icon": "\U0001F578",
        "tiers": (2, 16, 128, 1024),
        "unit": "merged pull requests",
    },
    "many_webs": {
        "name": "Many Webs",
        "description": "Had pull requests merged into many repositories you do not own.",
        "icon": "\U0001F310",
        "tiers": (2, 8, 32, 128),
        "unit": "repositories",
    },
    "quick_silk": {
        "name": "Quick Silk",
        "description": "Closed an issue or merged a pull request within five "
                       "minutes of it being opened.",
        "icon": "\u23F1",
    },
    "carpet_ride": {
        "name": "Carpet Ride",
        "description": "Merged a pull request without a review.",
        "icon": "\U0001F576",
    },
    "eight_hands": {
        "name": "Eight Hands",
        "description": "Co-authored commits in merged pull requests.",
        "icon": "\U0001F91D",
        "tiers": (1, 10, 24, 48),
        "unit": "pull requests",
    },
    "oracle": {
        "name": "Oracle",
        "description": "Wrote replies that were accepted as the answer to a "
                       "question.",
        "icon": "\U0001F52E",
        "tiers": (2, 8, 16, 32),
        "unit": "accepted answers",
    },
}

QUICK_SILK_MS = 5 * 60 * 1000
CO_AUTHORS_MAX = 16
_REVIEW_FILE_RE = re.compile(r"^[0-9]{4,}-review\.md$")
_EMAIL_RE = re.compile(r"^[^@\s,<>]{1,64}@[^@\s,<>]{1,189}$")


def normalize_badge_slug(value):
    """A known catalog slug, lowercased, or "" for anything else."""
    slug = str(value or "").strip().lower()
    return slug if _SLUG_RE.match(slug) and slug in BADGE_DEFINITIONS else ""


def badge_definition(slug):
    return BADGE_DEFINITIONS.get(normalize_badge_slug(slug))


def badge_tiers(slug):
    info = badge_definition(slug)
    return tuple(info.get("tiers") or ()) if info else ()


def tier_for_value(slug, value):
    try:
        value = int(value or 0)
    except (TypeError, ValueError):
        return -1
    reached = -1
    for level, threshold in enumerate(badge_tiers(slug)):
        if value >= threshold:
            reached = level
    return reached


def tier_name(level):
    try:
        level = int(level or 0)
    except (TypeError, ValueError):
        level = 0
    return TIER_NAMES[max(0, min(level, len(TIER_NAMES) - 1))]


def tier_summary(slug, level, progress):
    tiers = badge_tiers(slug)
    if not tiers:
        return {}
    level = max(0, min(int(level or 0), len(tiers) - 1))
    progress = max(0, int(progress or 0))
    following = level + 1 if level + 1 < len(tiers) else -1
    return {
        "tier": tier_name(level),
        "tierLevel": level,
        "progress": progress,
        "unit": BADGE_DEFINITIONS[normalize_badge_slug(slug)].get("unit", ""),
        "tiers": [{"tier": tier_name(i), "threshold": t}
                  for i, t in enumerate(tiers)],
        "nextTier": tier_name(following) if following >= 0 else "",
        "nextThreshold": tiers[following] if following >= 0 else 0,
    }


def public_catalog():
    out = []
    for slug, info in BADGE_DEFINITIONS.items():
        entry = {"slug": slug, "name": info["name"],
                 "description": info["description"], "icon": info["icon"]}
        if info.get("hidden_until_earned"):
            entry["hiddenUntilEarned"] = True
        if info.get("tiers"):
            entry["unit"] = info.get("unit", "")
            entry["tiers"] = [{"tier": tier_name(i), "threshold": t}
                              for i, t in enumerate(info["tiers"])]
        out.append(entry)
    return out


def longest_streak(days):
    parsed = set()
    for day in days or ():
        try:
            parsed.add(date.fromisoformat(str(day)[:10]))
        except ValueError:
            continue
    best = 0
    for day in parsed:
        if day - timedelta(days=1) in parsed:
            continue
        run = 1
        while day + timedelta(days=run) in parsed:
            run += 1
        best = max(best, run)
    return best


def contribution_count(row):
    total = 0
    for key in ("issues", "pulls", "discussions"):
        try:
            total += max(0, int((row or {}).get(key) or 0))
        except (TypeError, ValueError):
            pass
    return total


def distinguished_contributor_earned(row):
    return contribution_count(row) >= DISTINGUISHED_CONTRIBUTOR_MIN


def _record_int(value):
    try:
        number = int(str(value if value is not None else "0").strip() or 0)
    except (TypeError, ValueError):
        return 0
    return max(0, number)


def _record_ms(value):
    stamp = _record_int(value)
    return stamp * 1000 if 0 < stamp < 10_000_000_000 else stamp


def _record_key(value):
    return str(value or "").strip()[:120]


def co_author_emails(value):
    emails = []
    for part in str(value or "").split(","):
        email = part.strip().lower()
        if _EMAIL_RE.match(email) and email not in emails:
            emails.append(email)
        if len(emails) >= CO_AUTHORS_MAX:
            break
    return emails


def pull_record_fact(fields, number):
    fields = fields or {}
    status = str(fields.get("status") or "").strip().lower()
    if _record_int(fields.get("number")) != number:
        return None
    merged = status in ("merged", "closed-merged")
    return {
        "author_key": _record_key(fields.get("author")),
        "closer_key": _record_key(fields.get("mergedBy")) if merged else "",
        "opened_at": _record_ms(fields.get("ts")),
        "closed_at": _record_ms(fields.get("mergedAt")) if merged else 0,
        "merged": merged,
        "co_authors": co_author_emails(fields.get("coAuthors")) if merged else [],
    }


def issue_record_fact(record):
    events = record.get("events") if isinstance(record, dict) else None
    if not isinstance(events, list):
        return None
    opened = None
    for event in events:
        if not isinstance(event, dict):
            continue
        kind = str(event.get("type") or "")
        if kind == "open" and opened is None:
            opened = event
            continue
        status = str(event.get("status") or "").strip().lower()
        if opened is not None and kind == "status" and status and status != "open":
            return {
                "author_key": _record_key(opened.get("author")),
                "closer_key": _record_key(event.get("author")),
                "opened_at": _record_ms(opened.get("ts")),
                "closed_at": _record_ms(event.get("ts")),
            }
    return None


def quick_close(opened_at, closed_at):
    opened_at, closed_at = _record_int(opened_at), _record_int(closed_at)
    return 0 < opened_at <= closed_at and closed_at - opened_at <= QUICK_SILK_MS


def has_review_file(entries):
    for entry in entries or ():
        if isinstance(entry, dict) and _REVIEW_FILE_RE.match(
                str(entry.get("name") or "")):
            return True
    return False


def answer_record_fact(discussion_fields, answer_fields):
    discussion_fields = discussion_fields or {}
    answer_fields = answer_fields or {}
    category = str(discussion_fields.get("category") or "").strip().lower()
    opener_key = _record_key(discussion_fields.get("author"))
    answerer_key = ""
    if (category in ("q&a", "qa", "q and a", "q-and-a")
            and str(answer_fields.get("target") or "").strip()):
        answerer_key = _record_key(answer_fields.get("targetAuthor"))
    return {
        "opener_key": opener_key,
        "answerer_key": answerer_key,
        "marked_at": _record_ms(answer_fields.get("ts")) if answerer_key else 0,
    }
