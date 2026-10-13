"""Pure catalog + validation for ForkMesh achievement badges.

Badges are public recognition marks shown on a profile. Each one is either
awarded automatically at a real, verifiable platform event and / or granted by a
platform administrator; the catalog below is the fixed set of badges that can
exist. The Worker imports this module, but it has no Workers/JavaScript or D1
dependency so the catalog can be unit-tested with normal CPython. D1 reads,
writes, and the event hooks that call into this module live in entry.py.
"""

import re

_SLUG_RE = re.compile(r"^[a-z][a-z0-9_]{1,39}$")
DISTINGUISHED_CONTRIBUTOR_MIN = 15

# Ordered so the catalog and any UI built from it render in a stable,
# meaningful sequence rather than dict-insertion happenstance.
BADGE_DEFINITIONS = {
    "first_100_users": {
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
        "name": "Hackathon Builder",
        "description": "Submitted a valid project to a ForkMesh hackathon.",
        "icon": "\U0001F528",
    },
    "hackathon_connector": {
        "name": "Hackathon Connector",
        "description": "Brought ten independently active members into a "
                       "ForkMesh hackathon.",
        "icon": "\U0001F578",
    },
    "hackathon_finalist": {
        "name": "Hackathon Finalist",
        "description": "Placed second in a ForkMesh hackathon track.",
        "icon": "\U0001F948",
    },
    "hackathon_ambassador": {
        "name": "Hackathon Ambassador",
        "description": "Approved to represent a ForkMesh hackathon as part of "
                       "its street team.",
        "icon": "\U0001F4E3",
    },
    "hackathon_winner": {
        "name": "Hackathon Winner",
        "description": "Won a ForkMesh hackathon track.",
        "icon": "\U0001F3C6",
    },
}


def normalize_badge_slug(value):
    """A known catalog slug, lowercased, or "" for anything else."""
    slug = str(value or "").strip().lower()
    return slug if _SLUG_RE.match(slug) and slug in BADGE_DEFINITIONS else ""


def badge_definition(slug):
    return BADGE_DEFINITIONS.get(normalize_badge_slug(slug))


def public_catalog():
    return [
        {"slug": slug, "name": info["name"], "description": info["description"],
         "icon": info["icon"]}
        for slug, info in BADGE_DEFINITIONS.items()
    ]


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
