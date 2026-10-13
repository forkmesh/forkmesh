"""Pure policy for the ForkMesh Hackathon growth layer.

A hackathon campaign is a judged build contest wrapped in a referral, squad
and mission layer, so that participating in it is itself the marketing. This
module owns every rule that decides a number: what a point is worth, when a
mission completes, when a community milestone unlocks, what a share message
says, and whether an operator-supplied campaign configuration is allowed to
exist at all.

It is deliberately dependency-free -- no ``js``/``workers`` imports, no D1
handle, no ``env`` -- exactly like ``badges.py``, ``microjobs.py`` and
``reward_policy.py``. The Worker imports it through ``hackathon_runtime.py``;
the test suite imports it directly under ordinary CPython.

Three invariants hold everything else together:

``Points are earned, never claimed``
    Every point kind below corresponds to an event the *server* observed: a
    verified account, a published mirror, an accepted pull, a submitted
    project, a referral the attribution ledger credited. Pressing a share
    button is measured (it is the K-factor numerator) and is worth nothing.

``Rewards are deterministic``
    Every reward is a fixed threshold. There are no entries, no draws and no
    random winners, which is what keeps the contest a skill contest rather
    than a sweepstakes, and what lets the page state the reward honestly
    before anybody enters.

``Value waits for the referral ledger``
    A friend who verifies their email counts as *joined* immediately, which
    is what moves progress bars and cosmetic unlocks. Anything with monetary
    value -- Pro months, prize eligibility -- waits for
    ``referral_attributions.decision = 'credited'``, i.e. the seven-day
    maturity, proved-device, burst and network rules the public referral
    board already enforces. This module never re-implements that decision; it
    only reads its outcome.
"""

import json
import re


# --- campaign lifecycle -------------------------------------------------------
# A campaign is invisible until an administrator advances it. Public routes
# 404 while it is a draft, so every deploy during the build weeks is safe:
# pushing to main deploys, and a half-finished campaign must not appear.
STATUSES = ("draft", "registration", "live", "judging", "closed")

# Statuses whose public pages exist at all.
PUBLIC_STATUSES = frozenset({"registration", "live", "judging", "closed"})

# Statuses that still accept new participants.
JOINABLE_STATUSES = frozenset({"registration", "live"})

TRACK_ID_RE = re.compile(r"^[a-z][a-z0-9-]{1,31}$")
SLUG_RE = re.compile(r"^[a-z][a-z0-9-]{1,39}$")
# A squad code is derived from its owner's username (see
# hackathon_runtime._squad_code), so it is as short as the shortest
# username -- one character -- plus at most one collision digit.
SQUAD_CODE_RE = re.compile(r"^[a-z0-9]{1,16}$")
MISSION_KEY_RE = re.compile(r"^[a-z][a-z0-9_]{1,39}$")

MAX_TITLE = 120
MAX_TRACKS = 8
MAX_MISSIONS = 32
MAX_MILESTONES = 16
MAX_PITCH = 280

# Ceilings on anything an operator can configure. A configuration page must
# not be able to mint an unbounded amount of value, so the validator clamps
# rather than trusting the number it was handed.
MAX_MISSION_THRESHOLD = 10000
MAX_MILESTONE_THRESHOLD = 1_000_000
MAX_REWARD_MONTHS = 12


# --- points -------------------------------------------------------------------
# One row per (participant, kind, ref) in hackathon_points, so an event can
# never pay twice however many times its hook fires.
#
# "joined" is worth something small so a participant's very first action puts
# a number on their card; the real weight sits on building (a published
# mirror, an accepted pull, a submitted project) rather than on recruiting.
# A qualified referral is worth less than a submission on purpose: the
# Builders board must not be winnable by recruiting alone.
POINT_KINDS = {
    "joined": {
        "points": 50,
        "label": "Joined the hackathon",
        "unique": True,
    },
    "verified": {
        "points": 150,
        "label": "Verified your email",
        "unique": True,
    },
    "referral_joined": {
        "points": 100,
        "label": "A friend you invited verified their account",
        "unique": False,
    },
    "referral_qualified": {
        "points": 200,
        "label": "A friend you invited became an active member",
        "unique": False,
    },
    "first_mirror": {
        "points": 300,
        "label": "Published your first mirrored repository",
        "unique": True,
    },
    "pull_accepted": {
        "points": 150,
        "label": "Landed a pull request",
        "unique": False,
    },
    "submission": {
        "points": 500,
        "label": "Submitted a project",
        "unique": False,
    },
    "referral_post": {
        "points": 100,
        "label": "Posted your invite link somewhere public",
        "unique": True,
    },
}

# Point kinds a browser may never assert. Every one of them is written by the
# Worker from an event it observed itself, so the list is simply all of them:
# there is no client-reportable point kind and there must never be one.
CLIENT_REPORTABLE_POINT_KINDS = frozenset()


def point_value(kind):
    """Points one event of ``kind`` is worth, or 0 for an unknown kind."""
    entry = POINT_KINDS.get(str(kind or ""))
    return int(entry["points"]) if entry else 0


def point_label(kind):
    entry = POINT_KINDS.get(str(kind or ""))
    return entry["label"] if entry else ""


def is_unique_point_kind(kind):
    """True when a participant may only ever earn this kind once."""
    entry = POINT_KINDS.get(str(kind or ""))
    return bool(entry and entry.get("unique"))


def point_reference(kind, ref=""):
    """Canonical de-duplication reference for one point award.

    A once-per-participant kind collapses to the empty reference so the
    primary key rejects a second award even when the caller passes a
    different repository or pull number.
    """
    if is_unique_point_kind(kind):
        return ""
    return str(ref or "")[:120]


# --- missions -----------------------------------------------------------------
# Progress is data; the definitions are configuration with these defaults.
# ``metric`` names the counter a mission watches:
#   referrals_joined     friends who verified their email
#   referrals_qualified  friends the referral ledger credited
#   squad_size           members in the participant's squad
#   submissions          projects this participant's team submitted
#   mirrors              repositories this participant published
#   shares               share presses (progress only; never a reward)
MISSION_METRICS = (
    "referrals_joined",
    "referrals_qualified",
    "squad_size",
    "submissions",
    "mirrors",
    "shares",
)

# Reward kinds a mission or milestone may hand out. Two are deliberately
# absent:
#
#   cash    follows judging only, so no recruiting mechanic can pay one.
#   points  come only from the events in POINT_KINDS. A mission is *derived*
#           from those same events, so a points reward on it would pay the
#           event twice -- "mirror a repository" would be worth 600, not 300.
REWARD_KINDS = ("pro_months", "supporter_months", "badge", "none")

DEFAULT_MISSIONS = (
    {
        "key": "invite_one",
        "title": "Invite your first builder",
        "description": "Share your invite link. Every friend who verifies "
                       "their account is worth 100 points, and one puts you "
                       "on the Recruiters board.",
        "metric": "referrals_joined",
        "threshold": 1,
        "reward": {"kind": "none"},
    },
    {
        "key": "invite_three",
        "title": "Bring three builders",
        "description": "Three verified friends unlocks three months of Pro.",
        "metric": "referrals_qualified",
        "threshold": 3,
        "reward": {"kind": "pro_months", "months": 3},
    },
    {
        "key": "invite_ten",
        "title": "Bring ten builders",
        "description": "Ten qualified members earns six months of Pro and "
                       "the Connector badge.",
        "metric": "referrals_qualified",
        "threshold": 10,
        "reward": {"kind": "pro_months", "months": 6,
                   "badge": "hackathon_connector"},
    },
    {
        "key": "squad_twentyfive",
        "title": "Build a squad of 25",
        "description": "Everybody in your squad earns three months of Pro.",
        "metric": "squad_size",
        "threshold": 25,
        "reward": {"kind": "pro_months", "months": 3, "scope": "squad"},
    },
    {
        "key": "first_mirror",
        "title": "Mirror a repository",
        "description": "Publish a repository on ForkMesh so it stays "
                       "cloneable when its origin is gone. Worth 300 points.",
        "metric": "mirrors",
        "threshold": 1,
        "reward": {"kind": "none"},
    },
    {
        "key": "submit_project",
        "title": "Submit your project",
        # Progress only. The Pro months and the Builder badge for a valid
        # submission are the published "Every valid submission" prize, paid
        # when results are finalised and only once a project is confirmed
        # valid. A reward here would pay the same entry twice, and would pay
        # it before anybody checked it.
        "description": "Every valid submission earns Pro months, the "
                       "Builder badge and a card in the public gallery when "
                       "results are finalised.",
        "metric": "submissions",
        "threshold": 1,
        "reward": {"kind": "none"},
    },
)


def mission_progress(mission, counters):
    """``(progress, threshold, complete)`` for one mission against counters."""
    threshold = max(1, int(mission.get("threshold") or 1))
    metric = str(mission.get("metric") or "")
    try:
        progress = max(0, int(counters.get(metric) or 0))
    except (TypeError, ValueError):
        progress = 0
    return min(progress, threshold), threshold, progress >= threshold


def missions_state(missions, counters):
    """Every mission with its progress, ordered as configured."""
    out = []
    for mission in missions or ():
        progress, threshold, complete = mission_progress(mission, counters)
        out.append({
            "key": str(mission.get("key") or ""),
            "title": str(mission.get("title") or ""),
            "description": str(mission.get("description") or ""),
            "metric": str(mission.get("metric") or ""),
            "progress": progress,
            "threshold": threshold,
            "complete": complete,
            "reward": reward_summary(mission.get("reward")),
        })
    return out


def next_incomplete_mission(missions, counters):
    """The mission closest to completion, for the "one friend away" nudge."""
    best = None
    best_gap = None
    for mission in missions or ():
        progress, threshold, complete = mission_progress(mission, counters)
        if complete:
            continue
        gap = threshold - progress
        if best_gap is None or gap < best_gap:
            best, best_gap = mission, gap
    if best is None:
        return None
    progress, threshold, _complete = mission_progress(best, counters)
    return {
        "key": str(best.get("key") or ""),
        "title": str(best.get("title") or ""),
        "progress": progress,
        "threshold": threshold,
        "remaining": threshold - progress,
        "reward": reward_summary(best.get("reward")),
    }


def reward_summary(reward):
    """One human sentence for a reward, plus its machine fields."""
    reward = reward if isinstance(reward, dict) else {}
    kind = str(reward.get("kind") or "none")
    scope = "squad" if str(reward.get("scope") or "") == "squad" else "self"
    badge = str(reward.get("badge") or "")
    if kind == "pro_months":
        months = max(0, int(reward.get("months") or 0))
        text = "%d month%s of Pro" % (months, "" if months == 1 else "s")
        if scope == "squad":
            text += " for every squad member"
    elif kind == "supporter_months":
        months = max(0, int(reward.get("months") or 0))
        text = "%d month%s of Supporter" % (months, "" if months == 1 else "s")
    elif kind == "badge":
        text = "a badge"
    else:
        text = ""
    if badge and kind != "badge":
        text += " and a badge"
    return {
        "kind": kind,
        "scope": scope,
        "points": max(0, int(reward.get("points") or 0)),
        "months": max(0, int(reward.get("months") or 0)),
        "badge": badge,
        "text": text,
    }


# --- community milestones -----------------------------------------------------
# The meter on the landing page. Thresholds are set so the first one is
# reached in the opening week: a meter that never moves demotivates, which is
# the opposite of what it is for.
MILESTONE_METRICS = ("participants", "verified", "submissions", "squads")

DEFAULT_MILESTONES = (
    {
        "key": "community_choice",
        "metric": "participants",
        "threshold": 250,
        "label": "Community Choice prize opens",
        "description": "At 250 builders the public gallery gets its own "
                       "community-voted prize.",
        "reward": {"kind": "none"},
    },
    {
        "key": "submission_pro_six",
        "metric": "participants",
        "threshold": 500,
        "label": "Every valid submission earns 6 months of Pro",
        "description": "At 500 builders the per-submission grant doubles "
                       "from three months to six.",
        "reward": {"kind": "pro_months", "months": 6, "scope": "submission"},
    },
    {
        "key": "newcomer_prize",
        "metric": "participants",
        "threshold": 1000,
        "label": "First-time contributor prize unlocks",
        "description": "At 1,000 builders a dedicated prize opens for the "
                       "best project by a first-time contributor.",
        "reward": {"kind": "none"},
    },
)


def milestone_state(milestones, counters):
    """Every milestone with its meter, in threshold order."""
    ordered = sorted(
        (dict(entry or {}) for entry in milestones or ()),
        key=lambda entry: (
            str(entry.get("metric") or ""),
            int(entry.get("threshold") or 0),
        ),
    )
    out = []
    for entry in ordered:
        metric = str(entry.get("metric") or "participants")
        threshold = max(1, int(entry.get("threshold") or 1))
        try:
            value = max(0, int(counters.get(metric) or 0))
        except (TypeError, ValueError):
            value = 0
        unlocked_at = max(0, int(entry.get("unlocked_at") or 0))
        out.append({
            "key": str(entry.get("key") or ""),
            "metric": metric,
            "threshold": threshold,
            "value": min(value, threshold),
            "label": str(entry.get("label") or ""),
            "description": str(entry.get("description") or ""),
            "percent": min(100, int(value * 100 / threshold)) if threshold
            else 0,
            "unlocked": bool(unlocked_at) or value >= threshold,
            "unlockedAt": unlocked_at,
            "reward": reward_summary(entry.get("reward")),
        })
    return out


def next_milestone(milestones, counters):
    """The nearest locked milestone, or ``None`` once every one is unlocked."""
    for entry in milestone_state(milestones, counters):
        if not entry["unlocked"]:
            return entry
    return None


def milestones_to_unlock(milestones, counters, now):
    """Milestones whose threshold the counters have just reached.

    Pure: the caller supplies the stored rows and the live counters and
    writes the returned keys back. ``now`` stamps ``unlocked_at`` so the
    announcement can be dated.
    """
    unlocking = []
    for entry in milestones or ():
        if int((entry or {}).get("unlocked_at") or 0):
            continue
        metric = str((entry or {}).get("metric") or "participants")
        threshold = max(1, int((entry or {}).get("threshold") or 1))
        try:
            value = max(0, int(counters.get(metric) or 0))
        except (TypeError, ValueError):
            value = 0
        if value >= threshold:
            unlocking.append({
                "key": str((entry or {}).get("key") or ""),
                "label": str((entry or {}).get("label") or ""),
                "metric": metric,
                "threshold": threshold,
                "unlockedAt": int(now),
            })
    return unlocking


# --- share kit ----------------------------------------------------------------
# Ordered the way ForkMesh's own audience shares: developer-social first,
# messaging after, with the native share sheet offered wherever the browser
# has one. No channel here sends anything on the participant's behalf --
# every one of them opens the participant's own client with a message they
# can still edit, which is why no contact list is ever collected.
SHARE_CHANNELS = (
    {"id": "copy", "label": "Copy link", "kind": "copy"},
    {"id": "x", "label": "X", "kind": "url",
     "template": "https://x.com/intent/tweet?text={text}&url={link}"},
    {"id": "mastodon", "label": "Mastodon", "kind": "mastodon"},
    {"id": "bluesky", "label": "Bluesky", "kind": "url",
     "template": "https://bsky.app/intent/compose?text={textlink}"},
    {"id": "linkedin", "label": "LinkedIn", "kind": "url",
     "template": "https://www.linkedin.com/sharing/share-offsite/?url={link}"},
    {"id": "reddit", "label": "Reddit", "kind": "url",
     "template": "https://www.reddit.com/submit?url={link}&title={text}"},
    {"id": "hackernews", "label": "Hacker News", "kind": "url",
     "template": "https://news.ycombinator.com/submitlink?u={link}&t={text}"},
    {"id": "discord", "label": "Discord", "kind": "copy"},
    {"id": "telegram", "label": "Telegram", "kind": "url",
     "template": "https://t.me/share/url?url={link}&text={text}"},
    {"id": "whatsapp", "label": "WhatsApp", "kind": "url",
     "template": "https://api.whatsapp.com/send?text={textlink}"},
    {"id": "email", "label": "Email", "kind": "url",
     "template": "mailto:?subject={text}&body={textlink}"},
    {"id": "sms", "label": "Messages", "kind": "url", "mobile": True,
     "template": "sms:?&body={textlink}"},
    {"id": "native", "label": "More", "kind": "native"},
    {"id": "qr", "label": "QR code", "kind": "qr"},
    # A 1080x1920 image for Instagram and other Stories, drawn in the
    # participant's browser: a platform with no share URL still gets a share,
    # and rendering it costs the Worker nothing.
    {"id": "story", "label": "Story image", "kind": "story"},
)

SHARE_CHANNEL_IDS = frozenset(entry["id"] for entry in SHARE_CHANNELS)

# Where in the product a share press happened, so the growth dashboard can
# tell an onboarding-step share from an HQ share from a leaderboard share.
SHARE_SURFACES = frozenset({
    "hq", "onboarding", "leaderboard", "squad", "milestone", "project",
    "challenge",
})

# Default message templates. ``{rank}``/``{points}``/``{link}``/``{name}``
# are filled server-side so the copy a participant pastes already carries
# their real standing -- "I'm #12 of 2,431 builders" is shareable, "join our
# contest" is not.
DEFAULT_SHARE_COPY = {
    "default": "I'm building in the ForkMesh Hackathon. "
               "Come build something that outlives its host.",
    "ranked": "I'm #{rank} of {total} builders in the ForkMesh Hackathon "
              "with {points} points. Think you can beat me?",
    "squad": "My ForkMesh Hackathon squad is {size} builders strong. "
             "Join it and let's take the top spot.",
    "invite": "{name} invited you to the ForkMesh Hackathon.",
}


def share_text(copy, counters):
    """Fill one share template from the participant's live standing."""
    templates = dict(DEFAULT_SHARE_COPY)
    templates.update(
        {key: str(value) for key, value in (copy or {}).items()
         if key in DEFAULT_SHARE_COPY and str(value or "").strip()})
    rank = int(counters.get("rank") or 0)
    key = "ranked" if rank > 0 else "default"
    text = templates[key]
    for token, value in (
            ("{rank}", "{:,}".format(rank)),
            ("{total}", "{:,}".format(int(counters.get("total") or 0))),
            ("{points}", "{:,}".format(int(counters.get("points") or 0))),
            ("{size}", "{:,}".format(int(counters.get("size") or 0))),
            ("{name}", str(counters.get("name") or "")),
    ):
        text = text.replace(token, value)
    return text[:240]


def share_channels(text, link, mobile=False):
    """Ready-to-use share targets for one participant's link.

    Channels whose target is a URL arrive with the URL already built; the
    copy/native/QR channels carry only the text and link, because the client
    owns the clipboard, the share sheet and the QR canvas.
    """
    from urllib.parse import quote

    encoded_text = quote(text, safe="")
    encoded_link = quote(link, safe="")
    encoded_both = quote(text + " " + link, safe="")
    out = []
    for entry in SHARE_CHANNELS:
        if entry.get("mobile") and not mobile:
            continue
        item = {
            "id": entry["id"],
            "label": entry["label"],
            "kind": entry["kind"],
            "text": text,
            "link": link,
        }
        template = entry.get("template")
        if template:
            item["url"] = (
                template
                .replace("{text}", encoded_text)
                .replace("{textlink}", encoded_both)
                .replace("{link}", encoded_link)
            )
        out.append(item)
    return out


def mastodon_share_url(instance, text, link):
    """Share URL for a Mastodon instance the participant named themselves.

    The instance is a hostname the visitor typed, so it is validated here
    rather than trusted: anything that is not a plain hostname yields "",
    and the caller renders no link at all.
    """
    from urllib.parse import quote

    host = str(instance or "").strip().lower()
    host = host.replace("https://", "").replace("http://", "").strip("/")
    if not re.fullmatch(r"[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?"
                        r"(\.[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?)+", host):
        return ""
    return "https://%s/share?text=%s" % (
        host, quote(text + "\n\n" + link, safe=""))


# --- squads -------------------------------------------------------------------
# A squad is an invite tree, not a chosen team: it is created the first time
# one participant's link brings somebody in, and every later arrival through
# that participant joins the same squad. That is what makes the brief's
# "mini-contest" free -- a squad page is already the participant's own
# microsite, with their name, their meter and their reward.
#
# "Squad" and "Team" are different words on purpose and must stay that way in
# every string the product shows: a Team is the submission unit (up to five
# people, one project), a Squad is everybody you brought in.
MAX_TEAM_MEMBERS = 5


def squad_goal_tiers(config=None):
    """The goal ladder a squad page draws its meter from."""
    tiers = (config or {}).get("squadGoals")
    if isinstance(tiers, (list, tuple)) and tiers:
        cleaned = []
        for tier in tiers:
            try:
                size = int((tier or {}).get("size") or 0)
            except (TypeError, ValueError):
                continue
            if 2 <= size <= MAX_MISSION_THRESHOLD:
                cleaned.append({
                    "size": size,
                    "label": str((tier or {}).get("label") or "")[:120],
                })
        if cleaned:
            return tuple(sorted(cleaned, key=lambda tier: tier["size"]))
    return (
        {"size": 5, "label": "Five builders"},
        {"size": 25, "label": "Everyone in the squad earns 3 months of Pro"},
        {"size": 50, "label": "Squad listed on the hackathon page"},
    )


def squad_goal(member_count, config=None):
    """``(next_tier, percent)`` for a squad's meter, or ``(None, 100)``."""
    try:
        size = max(0, int(member_count or 0))
    except (TypeError, ValueError):
        size = 0
    for tier in squad_goal_tiers(config):
        if size < tier["size"]:
            return tier, min(100, int(size * 100 / tier["size"]))
    return None, 100


# --- onboarding ---------------------------------------------------------------
# Sharing sits inside onboarding rather than after it: the brief's point is
# that asking people to share once they have finished is asking too late.
# Step three needs no verification -- a participant may share immediately --
# while the credit their friends earn still waits for the referral ledger.
ONBOARDING_STEPS = (
    {"step": 1, "key": "join", "title": "Join the hackathon"},
    {"step": 2, "key": "track", "title": "Choose your track"},
    {"step": 3, "key": "invite", "title": "Invite three builders"},
    {"step": 4, "key": "build", "title": "Build and submit"},
)


def onboarding_state(step, track, counters):
    """Which onboarding step a participant is on, and what it is waiting for."""
    try:
        current = max(1, min(len(ONBOARDING_STEPS), int(step or 1)))
    except (TypeError, ValueError):
        current = 1
    joined = max(0, int(counters.get("referrals_joined") or 0))
    if current == 2 and str(track or ""):
        current = 3
    if current == 3 and joined >= 3:
        current = 4
    return {
        "step": current,
        "steps": [dict(entry) for entry in ONBOARDING_STEPS],
        "complete": current >= len(ONBOARDING_STEPS),
        "invitesJoined": min(3, joined),
        "invitesNeeded": max(0, 3 - joined),
    }


# --- campaign configuration ---------------------------------------------------

def default_config():
    """The configuration a campaign starts with."""
    return {
        "theme": "Build something that outlives its host.",
        "tracks": [
            {
                "id": "preserve",
                "name": "Preserve",
                "summary": "Mirroring, archiving and recovery tooling: "
                           "anything that keeps a repository cloneable once "
                           "its origin is gone.",
            },
            {
                "id": "agents",
                "name": "Agents",
                "summary": "Workflows built on the ForkMesh MCP server and "
                           "agent runs: triage bots, review agents, release "
                           "automation, teammates.",
            },
            {
                "id": "extend",
                "name": "Extend",
                "summary": "Marketplace packages: extensions, themes, "
                           "dashboard plugins and World objects, each "
                           "shipping a signed manifest.",
            },
        ],
        "missions": [dict(entry) for entry in DEFAULT_MISSIONS],
        "milestones": [dict(entry) for entry in DEFAULT_MILESTONES],
        "shareCopy": dict(DEFAULT_SHARE_COPY),
        "squadGoals": [dict(tier) for tier in squad_goal_tiers()],
        "prizes": default_prizes(),
        "challengeHours": CHALLENGE_HOURS_DEFAULT,
    }


def default_prizes():
    """The published prize table.

    Pro is a $10/month membership, so a year of it is $120 of published value
    at close to no marginal cost -- which is why the structure leans on Pro
    months and keeps cash on the one line that can be headlined. Cash follows
    the judged board only: no recruiting mechanic anywhere in this module can
    pay one, by construction.
    """
    return [
        {
            "id": "first",
            "name": "1st per track",
            "cash": "",
            "proMonths": 12,
            "featured": True,
            "interview": True,
            "badge": "hackathon_winner",
            "detail": "Cash prize, a year of Pro, featured on the "
                      "Marketplace and the homepage, and a guaranteed "
                      "interview for an engineering or DevRel role.",
        },
        {
            "id": "second",
            "name": "2nd per track",
            "cash": "",
            "proMonths": 6,
            "featured": True,
            "interview": False,
            "badge": "hackathon_finalist",
            "detail": "Six months of Pro and a featured placement.",
        },
        {
            "id": "submission",
            "name": "Every valid submission",
            "cash": "",
            "proMonths": 3,
            "featured": False,
            "interview": False,
            "badge": "hackathon_builder",
            "detail": "Three months of Pro, the Supporter badge and its "
                      "World outfit perk, plus a card in the public gallery.",
        },
        {
            "id": "recruiters",
            "name": "Top recruiters",
            "cash": "",
            "proMonths": 6,
            "featured": False,
            "interview": False,
            "badge": "hackathon_connector",
            "detail": "Six, three and three months of Pro for the top three "
                      "on the Recruiters board. Deliberately no cash: "
                      "recruiting earns memberships, never prize money.",
        },
    ]


class ConfigError(ValueError):
    """An operator-supplied campaign configuration was refused."""


def validate_config(value):
    """Return a normalised campaign configuration, or raise ``ConfigError``.

    Everything an administrator can edit passes through here before it
    reaches D1, because the configuration decides what the campaign hands
    out. Unknown reward kinds, out-of-range thresholds and unknown metrics
    are refused rather than clamped silently, so a typo in the admin form
    fails loudly instead of quietly minting twelve months of Pro.
    """
    if isinstance(value, str):
        try:
            value = json.loads(value or "{}")
        except ValueError as error:
            raise ConfigError("config is not valid JSON") from error
    if not isinstance(value, dict):
        raise ConfigError("config must be an object")

    config = default_config()
    theme = str(value.get("theme") or config["theme"]).strip()
    config["theme"] = theme[:MAX_TITLE]

    if "tracks" in value:
        tracks = value.get("tracks")
        if not isinstance(tracks, (list, tuple)) or not tracks:
            raise ConfigError("tracks must be a non-empty list")
        if len(tracks) > MAX_TRACKS:
            raise ConfigError("too many tracks")
        seen = set()
        cleaned = []
        for track in tracks:
            track = track if isinstance(track, dict) else {}
            track_id = str(track.get("id") or "").strip().lower()
            if not TRACK_ID_RE.match(track_id):
                raise ConfigError("invalid track id: %r" % track_id)
            if track_id in seen:
                raise ConfigError("duplicate track id: %s" % track_id)
            seen.add(track_id)
            cleaned.append({
                "id": track_id,
                "name": str(track.get("name") or track_id)[:MAX_TITLE],
                "summary": str(track.get("summary") or "")[:400],
            })
        config["tracks"] = cleaned

    if "missions" in value:
        config["missions"] = _validate_missions(value.get("missions"))
    if "milestones" in value:
        config["milestones"] = _validate_milestones(value.get("milestones"))
    if "shareCopy" in value:
        copy = value.get("shareCopy")
        if not isinstance(copy, dict):
            raise ConfigError("shareCopy must be an object")
        merged = dict(DEFAULT_SHARE_COPY)
        for key, text in copy.items():
            if key not in DEFAULT_SHARE_COPY:
                raise ConfigError("unknown share copy key: %s" % key)
            merged[key] = str(text or "")[:240]
        config["shareCopy"] = merged
    if "squadGoals" in value:
        config["squadGoals"] = [
            dict(tier) for tier in squad_goal_tiers(
                {"squadGoals": value.get("squadGoals")})
        ]
    if "prizes" in value:
        config["prizes"] = _validate_prizes(value.get("prizes"))
    if "challengeHours" in value:
        try:
            hours = int(value.get("challengeHours"))
        except (TypeError, ValueError) as error:
            raise ConfigError("challengeHours must be a number") from error
        if not CHALLENGE_HOURS_MIN <= hours <= CHALLENGE_HOURS_MAX:
            raise ConfigError("challengeHours out of range: %d" % hours)
        config["challengeHours"] = hours
    return config


def _validate_missions(missions):
    if not isinstance(missions, (list, tuple)):
        raise ConfigError("missions must be a list")
    if len(missions) > MAX_MISSIONS:
        raise ConfigError("too many missions")
    seen = set()
    cleaned = []
    for mission in missions:
        mission = mission if isinstance(mission, dict) else {}
        key = str(mission.get("key") or "").strip().lower()
        if not MISSION_KEY_RE.match(key):
            raise ConfigError("invalid mission key: %r" % key)
        if key in seen:
            raise ConfigError("duplicate mission key: %s" % key)
        seen.add(key)
        metric = str(mission.get("metric") or "")
        if metric not in MISSION_METRICS:
            raise ConfigError("unknown mission metric: %r" % metric)
        try:
            threshold = int(mission.get("threshold") or 0)
        except (TypeError, ValueError) as error:
            raise ConfigError("mission threshold must be a number") from error
        if not 1 <= threshold <= MAX_MISSION_THRESHOLD:
            raise ConfigError("mission threshold out of range: %d" % threshold)
        if metric == "shares":
            # Share presses are the K-factor numerator, never a payout: a
            # mission on them may only award progress, because otherwise
            # pressing a button would mint value.
            reward = _validate_reward(mission.get("reward"), allow_value=False)
        else:
            reward = _validate_reward(mission.get("reward"))
        cleaned.append({
            "key": key,
            "title": str(mission.get("title") or key)[:MAX_TITLE],
            "description": str(mission.get("description") or "")[:400],
            "metric": metric,
            "threshold": threshold,
            "reward": reward,
        })
    return cleaned


def _validate_milestones(milestones):
    if not isinstance(milestones, (list, tuple)):
        raise ConfigError("milestones must be a list")
    if len(milestones) > MAX_MILESTONES:
        raise ConfigError("too many milestones")
    seen = set()
    cleaned = []
    for entry in milestones:
        entry = entry if isinstance(entry, dict) else {}
        key = str(entry.get("key") or "").strip().lower()
        if not MISSION_KEY_RE.match(key):
            raise ConfigError("invalid milestone key: %r" % key)
        if key in seen:
            raise ConfigError("duplicate milestone key: %s" % key)
        seen.add(key)
        metric = str(entry.get("metric") or "")
        if metric not in MILESTONE_METRICS:
            raise ConfigError("unknown milestone metric: %r" % metric)
        try:
            threshold = int(entry.get("threshold") or 0)
        except (TypeError, ValueError) as error:
            raise ConfigError("milestone threshold must be a number") from error
        if not 1 <= threshold <= MAX_MILESTONE_THRESHOLD:
            raise ConfigError(
                "milestone threshold out of range: %d" % threshold)
        cleaned.append({
            "key": key,
            "metric": metric,
            "threshold": threshold,
            "label": str(entry.get("label") or "")[:MAX_TITLE],
            "description": str(entry.get("description") or "")[:400],
            "reward": _validate_reward(entry.get("reward")),
        })
    return cleaned


def _validate_reward(reward, allow_value=True):
    reward = reward if isinstance(reward, dict) else {"kind": "none"}
    kind = str(reward.get("kind") or "none")
    if kind not in REWARD_KINDS:
        raise ConfigError("unknown reward kind: %r" % kind)
    if not allow_value and kind != "none":
        raise ConfigError("this mission may not award %s" % kind)
    out = {"kind": kind}
    if kind in ("pro_months", "supporter_months"):
        months = int(reward.get("months") or 0)
        if not 1 <= months <= MAX_REWARD_MONTHS:
            raise ConfigError("reward months out of range: %d" % months)
        out["months"] = months
    badge = str(reward.get("badge") or "").strip().lower()
    if badge:
        if not MISSION_KEY_RE.match(badge):
            raise ConfigError("invalid reward badge: %r" % badge)
        out["badge"] = badge
    scope = str(reward.get("scope") or "").strip().lower()
    if scope:
        if scope not in ("self", "squad", "submission"):
            raise ConfigError("unknown reward scope: %r" % scope)
        out["scope"] = scope
    return out


def _validate_prizes(prizes):
    if not isinstance(prizes, (list, tuple)):
        raise ConfigError("prizes must be a list")
    cleaned = []
    for prize in prizes[:12]:
        prize = prize if isinstance(prize, dict) else {}
        prize_id = str(prize.get("id") or "").strip().lower()
        if not MISSION_KEY_RE.match(prize_id):
            raise ConfigError("invalid prize id: %r" % prize_id)
        months = int(prize.get("proMonths") or 0)
        if not 0 <= months <= MAX_REWARD_MONTHS:
            raise ConfigError("prize months out of range: %d" % months)
        cleaned.append({
            "id": prize_id,
            "name": str(prize.get("name") or prize_id)[:MAX_TITLE],
            "cash": str(prize.get("cash") or "")[:40],
            "proMonths": months,
            "featured": bool(prize.get("featured")),
            "interview": bool(prize.get("interview")),
            "badge": str(prize.get("badge") or "")[:40],
            "detail": str(prize.get("detail") or "")[:400],
        })
    return cleaned


def track_ids(config):
    return tuple(
        str(track.get("id") or "")
        for track in (config or {}).get("tracks") or ()
        if str(track.get("id") or "")
    )


def valid_track(config, track):
    track = str(track or "").strip().lower()
    return track if track in track_ids(config) else ""


# --- public projections -------------------------------------------------------

def public_campaign(row, counters, milestones):
    """The bounded object ``GET /api/hackathon`` answers with.

    Only published campaign facts: no account, no configuration secret, and
    no counter the landing page does not draw.
    """
    row = row or {}
    config = row.get("config") if isinstance(row.get("config"), dict) else {}
    upcoming = next_milestone(milestones, counters)
    return {
        "slug": str(row.get("slug") or ""),
        "title": str(row.get("title") or ""),
        "status": str(row.get("status") or "draft"),
        "theme": str(config.get("theme") or ""),
        "startsAt": int(row.get("starts_at") or 0),
        "endsAt": int(row.get("ends_at") or 0),
        "judgingEndsAt": int(row.get("judging_ends_at") or 0),
        "joinable": str(row.get("status") or "") in JOINABLE_STATUSES,
        "tracks": [dict(track) for track in config.get("tracks") or ()],
        "prizes": [dict(prize) for prize in config.get("prizes") or ()],
        "participants": int(counters.get("participants") or 0),
        "verified": int(counters.get("verified") or 0),
        "squads": int(counters.get("squads") or 0),
        "submissions": int(counters.get("submissions") or 0),
        "nextMilestone": upcoming,
        "milestones": milestone_state(milestones, counters),
    }


def leaderboard_boards():
    """The public boards, in the order the leaderboard page renders them."""
    return (
        {
            "id": "builders",
            "title": "Builders",
            "metric": "points",
            "summary": "Points from building and bringing people in. "
                       "The judged prizes do not follow this board.",
        },
        {
            "id": "recruiters",
            "title": "Recruiters",
            "metric": "qualified",
            "summary": "Members brought in and credited by the referral "
                       "ledger. Earns Pro months, never cash.",
        },
        {
            "id": "squads",
            "title": "Squads",
            "metric": "points",
            "summary": "Everyone one builder brought in, and everyone they "
                       "brought in.",
        },
    )


BOARD_IDS = frozenset(entry["id"] for entry in leaderboard_boards())

# Public boards are bounded reads: fifty rows, one query per cache TTL. The
# D1 row budget is the reason, and the reason the participant's own rank is
# read from their own row instead of by scanning for it.
BOARD_LIMIT = 50
BOARD_CACHE_SECONDS = 60
CAMPAIGN_CACHE_SECONDS = 60


def rank_of(rows, account_bi):
    """1-based rank of one account inside a board page, or 0 when absent."""
    for index, row in enumerate(rows or ()):
        if str(row.get("account_bi") or "") == str(account_bi or ""):
            return index + 1
    return 0


# --- challenge links ----------------------------------------------------------
# "Alex has 1,240 points. Can you beat that?" The target is the challenger's
# points at the moment they sent the link -- the number the page showed -- so
# a challenge is decided once and never flips back when the challenger keeps
# building. Beating one is worth no points: its outcome is derived from points
# already earned, and paying for it would let two friends mint points by
# challenging each other in a loop.
CHALLENGE_HOURS_DEFAULT = 72
CHALLENGE_HOURS_MIN = 1
CHALLENGE_HOURS_MAX = 720
MAX_OPEN_CHALLENGES = 20
MAX_CHALLENGE_ENTRIES = 50
CHALLENGE_TOKEN_RE = re.compile(r"^[A-Za-z0-9_-]{16,40}$")
HOUR_MS = 60 * 60 * 1000


def challenge_ttl_ms(config):
    """How long a new challenge link stays open, from configuration."""
    try:
        hours = int((config or {}).get("challengeHours")
                    or CHALLENGE_HOURS_DEFAULT)
    except (TypeError, ValueError):
        hours = CHALLENGE_HOURS_DEFAULT
    hours = max(CHALLENGE_HOURS_MIN, min(CHALLENGE_HOURS_MAX, hours))
    return hours * HOUR_MS


def challenge_beaten(points, target):
    """Strictly more points than the target. A tie does not beat anybody."""
    try:
        return int(points or 0) > int(target or 0)
    except (TypeError, ValueError):
        return False


def challenge_state(row, now, entry=None):
    """The public shape of one challenge, optionally from one viewer's side."""
    row = row or {}
    expires_at = max(0, int(row.get("expires_at") or 0))
    open_ = expires_at > int(now)
    state = {
        "token": str(row.get("token") or ""),
        "challenger": str(row.get("challenger_name") or ""),
        "target": max(0, int(row.get("target_points") or 0)),
        "accepted": max(0, int(row.get("accepted_count") or 0)),
        "beaten": max(0, int(row.get("beaten_count") or 0)),
        "createdAt": max(0, int(row.get("created_at") or 0)),
        "expiresAt": expires_at,
        "open": open_,
        "full": int(row.get("accepted_count") or 0) >= MAX_CHALLENGE_ENTRIES,
    }
    if entry is not None:
        state["you"] = {
            "accepted": bool(int((entry or {}).get("accepted_at") or 0)),
            "beaten": bool(int((entry or {}).get("beaten_at") or 0)),
            "beatenAt": max(0, int((entry or {}).get("beaten_at") or 0)),
        }
    return state


# --- reward fulfilment ----------------------------------------------------------
# Memberships are granted as months counted in thirty-day blocks from the later
# of now and whatever the same promotion already runs until, so two rewards
# stack instead of the second overwriting the first. A month is thirty days on
# purpose: the grant is a duration, and a calendar month would make the same
# prize worth a different number of days depending on when it was won.
DAY_MS = 24 * HOUR_MS
MONTH_MS = 30 * DAY_MS

# The role_grants scope a promotion is written under. Patreon revokes
# platform-scope membership rows on every sync; a prize must not vanish the
# next time somebody's subscription renews, so it lives in a scope no billing
# provider touches.
PROMOTION_SCOPE_TYPE = "promotion"

# Pro includes Supporter everywhere else in ForkMesh (Patreon's tier table
# grants both), and the World outfit perk reads supporting_member, so a Pro
# prize grants both roles.
MEMBERSHIP_ROLES = {
    "pro_months": ("pro_member", "supporting_member"),
    "supporter_months": ("supporting_member",),
}

# The three places on the Recruiters board that earn memberships. Never cash.
RECRUITER_PRIZE_MONTHS = (6, 3, 3)


def extend_expiry(current_expires_at, now, months):
    """The new expiry after adding ``months`` to a promotion that may be live."""
    start = max(int(now), max(0, int(current_expires_at or 0)))
    return start + max(0, int(months or 0)) * MONTH_MS


def reward_grants(reward_key, reward):
    """The ledger rows one reward produces, before any account is attached.

    A reward with both months and a badge becomes two rows, each keyed by the
    same ``reward_key`` so the pair is granted exactly once together.
    """
    reward = reward if isinstance(reward, dict) else {}
    kind = str(reward.get("kind") or "none")
    rows = []
    if kind in MEMBERSHIP_ROLES:
        months = max(0, min(MAX_REWARD_MONTHS, int(reward.get("months") or 0)))
        if months:
            rows.append({"reward_key": reward_key, "kind": kind,
                         "months": months, "badge": ""})
    badge = str(reward.get("badge") or "").strip().lower()
    if kind == "badge" and not badge:
        badge = ""
    if badge and MISSION_KEY_RE.match(badge):
        rows.append({"reward_key": reward_key, "kind": "badge",
                     "months": 0, "badge": badge})
    return rows


def mission_reward_key(mission, squad_id=""):
    """Ledger key for one mission's reward.

    A squad-scoped mission is keyed by the squad as well, so every member of
    the squad that reached the goal is rewarded once for *that* squad.
    """
    key = "mission:" + str((mission or {}).get("key") or "")
    scope = str(((mission or {}).get("reward") or {}).get("scope") or "")
    if scope == "squad" and squad_id:
        key += ":" + str(squad_id)
    return key


def completed_mission_grants(missions, counters, squad_id=""):
    """Every ledger row a participant has earned from the missions they finished."""
    rows = []
    for mission in missions or ():
        _progress, _threshold, complete = mission_progress(mission, counters)
        if not complete:
            continue
        scope = str(((mission or {}).get("reward") or {}).get("scope") or "")
        if scope == "squad" and not squad_id:
            continue
        rows.extend(reward_grants(
            mission_reward_key(mission, squad_id), mission.get("reward")))
    return rows


# --- teams and submissions ------------------------------------------------------
TEAM_NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9 ._'&-]{1,39}$")
TEAM_CODE_RE = re.compile(r"^[a-z0-9]{8}$")
REPO_SEGMENT_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,99}$")
SUBMISSION_ID_RE = re.compile(r"^[0-9a-f]{32}$")
MAX_SUBMISSION_TITLE = 80
MIN_SUBMISSION_TITLE = 3
MAX_URL = 400
SUBMISSION_STATUSES = ("valid", "needs_review", "invalid", "withdrawn")
# A campaign's own window, plus this grace before it opens, is when a project
# is expected to have first appeared on ForkMesh. Earlier is flagged for a
# judge, not rejected: plenty of honest entries grow out of an older repository.
HOSTING_GRACE_MS = 7 * DAY_MS


def normalize_team_name(value):
    name = " ".join(str(value or "").split())
    return name if TEAM_NAME_RE.match(name) else ""


def _https_url(value):
    url = str(value or "").strip()
    if not url:
        return ""
    if len(url) > MAX_URL or not url.lower().startswith("https://"):
        return None
    if any(ch in url for ch in ' "<>\\\x00'):
        return None
    return url


def normalize_submission(data, config):
    """``(fields, error)`` for a submission form, never both.

    The repository is named as ``owner/name`` because that is what a ForkMesh
    repository URL already is; the runtime checks it against the catalog.
    Links must be https so a gallery never embeds a plain-http or script URL.
    """
    data = data if isinstance(data, dict) else {}
    track = valid_track(config, data.get("track"))
    if not track:
        return None, "invalid_track"
    repo = str(data.get("repo") or "").strip().strip("/")
    for prefix in ("https://forkmesh.com/", "https://app.forkmesh.com/"):
        if repo.lower().startswith(prefix):
            repo = repo[len(prefix):]
    parts = repo.split("/")
    if len(parts) != 2 or not all(REPO_SEGMENT_RE.match(p) for p in parts):
        return None, "invalid_repository"
    title = " ".join(str(data.get("title") or "").split())
    if not MIN_SUBMISSION_TITLE <= len(title) <= MAX_SUBMISSION_TITLE:
        return None, "invalid_title"
    pitch = " ".join(str(data.get("pitch") or "").split())
    if len(pitch) > MAX_PITCH:
        return None, "pitch_too_long"
    demo = _https_url(data.get("demoUrl"))
    if demo is None:
        return None, "invalid_demo_url"
    video = _https_url(data.get("videoUrl"))
    if video is None:
        return None, "invalid_video_url"
    return {
        "track": track,
        "repo_owner": parts[0].lower(),
        "repo_name": parts[1],
        "title": title,
        "pitch": pitch,
        "demo_url": demo,
        "video_url": video,
    }, ""


def submission_review_flags(in_catalog, first_hosted_at, starts_at, ends_at):
    """What a judge should look at before a submission counts as valid.

    An empty list means the automatic checks passed. Anything the checks
    cannot confirm becomes a flag rather than a rejection: a false rejection
    costs a builder their entry, a false flag costs a judge one look.
    """
    flags = []
    if not in_catalog:
        flags.append("repository_not_public")
    first = max(0, int(first_hosted_at or 0))
    starts = max(0, int(starts_at or 0))
    ends = max(0, int(ends_at or 0))
    if in_catalog and starts and first and first < starts - HOSTING_GRACE_MS:
        flags.append("hosted_before_campaign")
    if in_catalog and ends and first and first > ends:
        flags.append("hosted_after_deadline")
    return flags


def submission_status_for(flags):
    return "needs_review" if flags else "valid"


# --- judging ------------------------------------------------------------------
SCORE_CRITERIA = (
    ("fit", "Fits the track"),
    ("working", "Working software"),
    ("portability", "Portable and self-hostable"),
    ("craft", "Craft"),
)
SCORE_MIN = 1
SCORE_MAX = 5


def normalize_score(data):
    """``(scores, error)``: every criterion an integer from 1 to 5."""
    data = data if isinstance(data, dict) else {}
    out = {}
    for key, _label in SCORE_CRITERIA:
        try:
            value = int(data.get(key))
        except (TypeError, ValueError):
            return None, "invalid_score"
        if not SCORE_MIN <= value <= SCORE_MAX:
            return None, "invalid_score"
        out[key] = value
    out["total"] = sum(out[key] for key, _label in SCORE_CRITERIA)
    return out, ""


def rank_track(submissions):
    """Scored, valid submissions of one track, best first.

    Ordered by average judge total. Ties go to the submission more judges
    scored, then to the one submitted first; a submission nobody scored
    cannot place, because an unscored project has not been judged.
    """
    ranked = []
    for row in submissions or ():
        if str(row.get("status") or "") != "valid":
            continue
        count = max(0, int(row.get("score_count") or 0))
        if not count:
            continue
        total = max(0, int(row.get("score_total") or 0))
        ranked.append((total / count, count, -int(row.get("submitted_at") or 0),
                       row))
    ranked.sort(key=lambda item: (item[0], item[1], item[2]), reverse=True)
    return [item[3] for item in ranked]


def placements(submissions):
    """``{submission_id: "first" | "second"}`` across every track."""
    by_track = {}
    for row in submissions or ():
        by_track.setdefault(str(row.get("track") or ""), []).append(row)
    out = {}
    for rows in by_track.values():
        ranked = rank_track(rows)
        for index, label in ((0, "first"), (1, "second")):
            if len(ranked) > index:
                out[str(ranked[index].get("submission_id") or "")] = label
    return out


def submission_grant_months(config, unlocked_milestones):
    """Pro months every valid submission earns, after community milestones.

    The published default is the "Every valid submission" prize; a milestone
    whose reward is scoped to submissions raises it once unlocked, and only
    ever raises it.
    """
    months = 0
    for prize in (config or {}).get("prizes") or ():
        if str(prize.get("id") or "") == "submission":
            months = max(0, int(prize.get("proMonths") or 0))
    for entry in unlocked_milestones or ():
        reward = (entry or {}).get("reward") or {}
        if isinstance(reward, str):
            try:
                reward = json.loads(reward)
            except ValueError:
                reward = {}
        if (str(reward.get("scope") or "") == "submission"
                and str(reward.get("kind") or "") == "pro_months"):
            months = max(months, int(reward.get("months") or 0))
    return min(MAX_REWARD_MONTHS, months)


def prize_grants(config, submissions, team_members, recruiters,
                 unlocked_milestones):
    """Every ledger row the published prize table owes, as one plan.

    ``team_members`` maps a team id to the account blind indexes on it;
    ``recruiters`` is the Recruiters board, best first. Cash, the interview
    and featured placements are deliberately not in the plan: those are human
    decisions and are paid by a person, not by this function.
    """
    prizes = {str(p.get("id") or ""): p
              for p in (config or {}).get("prizes") or ()}
    placed = placements(submissions)
    each_months = submission_grant_months(config, unlocked_milestones)
    rows = []

    def add(account_bi, key, reward):
        for grant in reward_grants(key, reward):
            grant["account_bi"] = account_bi
            rows.append(grant)

    for row in submissions or ():
        if str(row.get("status") or "") != "valid":
            continue
        submission_id = str(row.get("submission_id") or "")
        members = team_members.get(str(row.get("team_id") or ""), ())
        base_badge = str((prizes.get("submission") or {}).get("badge") or "")
        for account_bi in members:
            add(account_bi, "prize:submission:" + submission_id,
                {"kind": "pro_months", "months": each_months,
                 "badge": base_badge})
        label = placed.get(submission_id)
        if label:
            prize = prizes.get(label) or {}
            for account_bi in members:
                add(account_bi, "prize:%s:%s" % (label, row.get("track")),
                    {"kind": "pro_months",
                     "months": int(prize.get("proMonths") or 0),
                     "badge": str(prize.get("badge") or "")})
    recruiter_badge = str((prizes.get("recruiters") or {}).get("badge") or "")
    ranked = [r for r in recruiters or () if int(r.get("qualified") or 0) > 0]
    for index, months in enumerate(RECRUITER_PRIZE_MONTHS):
        if index >= len(ranked) or "recruiters" not in prizes:
            break
        add(str(ranked[index].get("account_bi") or ""),
            "prize:recruiter:%d" % (index + 1),
            {"kind": "pro_months", "months": months,
             "badge": recruiter_badge})
    return [row for row in rows if row.get("account_bi")]


def voting_open(status, unlocked_keys):
    """Community Choice is open once its milestone unlocks, until judging ends."""
    return (str(status or "") in ("live", "judging")
            and "community_choice" in set(unlocked_keys or ()))


# --- growth dashboard -----------------------------------------------------------

def _ratio(numerator, denominator, places=4):
    try:
        denominator = float(denominator)
        return round(float(numerator) / denominator, places) if denominator \
            else 0.0
    except (TypeError, ValueError):
        return 0.0


def growth_funnel(counts):
    """The viral funnel and its K-factor, from counters the runtime collected.

    K = invitations per participant x the share of invitations that became a
    participant. ForkMesh never sees a message being sent, so "invitations"
    is share presses: intent, labelled as intent wherever it is shown. The
    product therefore equals referred joins per participant, which is the
    number that actually says whether the loop grows the campaign.
    """
    counts = counts or {}
    participants = max(0, int(counts.get("participants") or 0))
    shares = max(0, int(counts.get("shares") or 0))
    joins = max(0, int(counts.get("referredJoins") or 0))
    # (key, label, value, previous key it is a subset of). A share of the
    # previous stage only means something when that stage contains this one:
    # presses are counted per press, not per person, and "built something"
    # includes people nobody invited, so neither gets a percentage.
    stages = [
        ("participants", "Participants", participants, None),
        ("sharers", "Shared at least once", counts.get("sharers"),
         "participants"),
        ("shares", "Share presses (intent)", shares, None),
        ("clicks", "Invite link clicks", counts.get("clicks"), None),
        ("referredJoins", "Joined through an invite", joins, "clicks"),
        ("referredVerified", "Verified their account",
         counts.get("referredVerified"), "referredJoins"),
        ("qualified", "Credited by the referral ledger",
         counts.get("qualified"), "referredVerified"),
        ("activated", "Built something", counts.get("activated"), None),
    ]
    values = {}
    out = []
    for key, label, value, parent in stages:
        value = max(0, int(value or 0))
        values[key] = value
        out.append({
            "key": key, "label": label, "value": value,
            "fromPrevious": (_ratio(value, values[parent])
                             if parent and values.get(parent) else None),
        })
    invites_per = _ratio(shares, participants)
    conversion = _ratio(joins, shares)
    return {
        "stages": out,
        "invitesPerParticipant": invites_per,
        "invitationConversion": conversion,
        "kFactor": _ratio(joins, participants),
    }


# --- ambassadors ----------------------------------------------------------------
AMBASSADOR_STATUSES = ("pending", "approved", "rejected")
MAX_AMBASSADOR_PITCH = 400


def normalize_ambassador_pitch(value):
    pitch = " ".join(str(value or "").split())
    return pitch[:MAX_AMBASSADOR_PITCH]


def ambassador_stats(clicks, joins, qualified):
    """An ambassador's own funnel: link visits, joins, qualified members."""
    clicks = max(0, int(clicks or 0))
    joins = max(0, int(joins or 0))
    qualified = max(0, int(qualified or 0))
    return {
        "clicks": clicks,
        "joins": joins,
        "qualified": qualified,
        "joinRate": _ratio(joins, clicks),
        "qualifiedRate": _ratio(qualified, joins),
    }
