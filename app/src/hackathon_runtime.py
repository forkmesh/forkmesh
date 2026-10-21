"""Hackathon routes and D1 access, loaded on demand.

Split out of entry.py the same way attribution.py and microjobs_runtime.py
are: Cloudflare validates a Python Worker by compiling and executing its
entrypoint under an isolate memory limit, and none of this belongs in that
budget. entry.py holds a ``_LazyModule`` proxy and ``_bind_runtime`` supplies
the shared Worker/D1 primitives the first time a hackathon route is reached.

Every rule that decides a number lives in the pure ``hackathon`` module next
to this one; this file only reads and writes rows, answers requests, and
calls the hooks the rest of the Worker already owns.

Three things this module deliberately never does:

* It never decides a referral. ``referral_attributions`` owns that, behind
  the maturity, proved-device, burst and network rules the public referral
  board already enforces. Points for a *qualified* referral are written from
  the hook that fires when that ledger credits one.
* It never messages anybody's friends. There is no recipient anywhere in
  this file: a share is a link the participant opens in their own client.
* It never grants itself money. Cash follows judging, which is a later
  phase and a human decision; nothing reachable from here can pay one.
"""

import json
import re

import hackathon as policy
from hidden_unicode import clean_query as clean_hidden_query


def _bind_runtime(runtime):
    """Supply the entrypoint primitives used by this domain."""
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


PREFIX = "/api/hackathon"
BODY_MAX_BYTES = 8 * 1024

# The campaign a bare /hackathon URL means. One campaign runs at a time, so
# the public pages resolve "the current campaign" rather than making every
# link carry a slug; the tables are keyed by slug regardless so a second
# campaign needs no migration.
CAMPAIGN_CACHE_KEY = "https://forkmesh.internal/api/hackathon"
BOARD_CACHE_KEY = "https://forkmesh.internal/api/hackathon/board/%s"
CARD_CACHE_KEY = "%s/api/hackathon/cards/%s/%s.png"
CARD_TTL = 60

SLUG_RE = policy.SLUG_RE
CARD_KINDS = ("invite", "rank", "squad", "challenge", "project")
INVITE_PATH_RE = re.compile(r"^/hackathon/join/([^/]+)/?$")
CARD_PATH_RE = re.compile(
    r"^/api/hackathon/cards/([a-z]+)/([^/]+)\.png$")
BOARD_PATH_RE = re.compile(r"^/api/hackathon/leaderboards/([a-z]+)/?$")
CHALLENGE_ACCEPT_RE = re.compile(
    r"^/api/hackathon/challenges/([A-Za-z0-9_-]{16,40})/accept$")
TEAM_ACTION_RE = re.compile(r"^/api/hackathon/teams(?:/(join|leave))?$")
SUBMISSION_WITHDRAW_RE = re.compile(
    r"^/api/hackathon/submissions/([0-9a-f]{32})/withdraw$")
PROJECT_RE = re.compile(r"^/api/hackathon/projects/([0-9a-f]{32})(/vote)?$")
JUDGING_RE = re.compile(r"^/api/hackathon/judging(?:/([0-9a-f]{32}))?$")
GALLERY_CACHE_KEY = "https://forkmesh.internal/api/hackathon/projects"
GALLERY_LIMIT = 200


# --- small helpers ------------------------------------------------------------

def _now():
    return int(Date.now())


def _name(value):
    return clean_string(value or "", MAX_NODE_NAME).strip().lower()


def _profile_html(value):
    name = _name(value)
    if not valid_node_name(name):
        return _html_escape(name)
    return "<a href=\"/@%s\">%s</a>" % (
        _html_escape(quote(name, safe="")), _html_escape(name))


def _site_origin(env, request=None):
    """The apex origin every share link is spelled on.

    Deliberately the marketing origin rather than the app origin: the
    microjob verifier looks for ``forkmesh.com/r/<name>`` in a posted page,
    so handing a participant an app.forkmesh.com link would make their own
    share fail its claim.
    """
    base = _public_base_url(env, request).rstrip("/")
    return base or "https://forkmesh.com"


def _config(row):
    """The validated configuration for a campaign row, never raising.

    A row whose stored configuration cannot be validated falls back to the
    defaults rather than taking the page down: the configuration decides
    copy and thresholds, and a bad edit must not 500 the landing page.
    """
    try:
        return policy.validate_config((row or {}).get("config") or "{}")
    except (policy.ConfigError, ValueError, TypeError):
        return policy.default_config()


def _counters(row):
    row = row or {}
    return {
        "participants": max(0, int(row.get("participant_count") or 0)),
        "verified": max(0, int(row.get("verified_count") or 0)),
        "squads": max(0, int(row.get("squad_count") or 0)),
        "submissions": max(0, int(row.get("submission_count") or 0)),
    }


async def _campaign(env, slug=""):
    """The campaign row a request is about, or ``None``.

    With no slug this resolves the current campaign: the newest one that is
    not a draft. A draft is reachable only by naming its slug, and only by
    an administrator -- which is what lets the whole feature ship to
    production while it is still being built.
    """
    await ensure_schema(env)
    slug = str(slug or "").strip().lower()
    try:
        if slug:
            if not SLUG_RE.match(slug):
                return None
            return await d1_first(
                env, "SELECT * FROM hackathons WHERE slug=?", slug)
        return await d1_first(
            env,
            "SELECT * FROM hackathons WHERE status<>'draft' "
            "ORDER BY created_at DESC LIMIT 1")
    except Exception:
        # A relay whose migration has not run yet has no hackathons table.
        # "No campaign" is the correct answer there and is exactly the state
        # a draft produces, so every public route answers 404 or a null
        # campaign instead of a 500. This also lets the routes and the
        # schema be accepted in either order.
        return None


async def _public_campaign(env, slug=""):
    """A campaign whose public pages exist, or ``None`` for a 404."""
    row = await _campaign(env, slug)
    if not row or str(row.get("status") or "") not in policy.PUBLIC_STATUSES:
        return None
    return row


async def _milestones(env, slug):
    rows = await d1_all(
        env,
        "SELECT key,metric,threshold,label,description,reward,unlocked_at "
        "FROM hackathon_milestones WHERE hackathon_slug=? "
        "ORDER BY threshold",
        slug)
    return [dict(row) for row in rows or []]


async def _check_milestones(env, slug, now=0):
    """Unlock every community milestone the campaign's counters now reach.

    Called right where a counter a milestone watches moves (a join, a
    verification, a new squad, a submission), so an unlock is immediate and
    costs two reads, instead of waiting for a scheduler slot. Each unlock is
    claimed with UPDATE ... WHERE unlocked_at=0 RETURNING, so concurrent joins
    unlock a milestone exactly once, and an unlocked milestone never re-locks
    when a withdrawal moves a counter back down.
    """
    try:
        now = int(now or _now())
        row = await d1_first(
            env,
            "SELECT participant_count,verified_count,squad_count,"
            "submission_count FROM hackathons WHERE slug=?",
            slug)
        if not row:
            return []
        milestones = await _milestones(env, slug)
        unlocked = []
        for entry in policy.milestones_to_unlock(
                milestones, _counters(row), now):
            claimed = await d1_first(
                env,
                "UPDATE hackathon_milestones SET unlocked_at=? "
                "WHERE hackathon_slug=? AND key=? AND unlocked_at=0 "
                "RETURNING key",
                now, slug, entry["key"])
            if claimed:
                unlocked.append(entry["key"])
        if unlocked:
            await edge_cache_delete(CAMPAIGN_CACHE_KEY)
            await edge_cache_delete(GALLERY_CACHE_KEY)
        return unlocked
    except Exception:
        return []


async def _participant(env, slug, account_bi):
    if not slug or not account_bi:
        return None
    return await d1_first(
        env,
        "SELECT * FROM hackathon_participants "
        "WHERE hackathon_slug=? AND account_bi=?",
        slug, account_bi)


async def _participant_by_name(env, slug, name):
    name = _name(name)
    if not slug or not valid_node_name(name):
        return None
    return await d1_first(
        env,
        "SELECT * FROM hackathon_participants "
        "WHERE hackathon_slug=? AND name=?",
        slug, name)


async def _session_account(env, request, data=None):
    """``(account_bi, name)`` for a proven user session, else ``("", "")``.

    Node sessions are refused for the same reason they own no referral link:
    a hackathon participant is a person with an account, not a machine
    identity.
    """
    try:
        name_bi, rec = await _account_session_record(env, request, data)
    except RuntimeError:
        return "", ""
    if not name_bi or not rec:
        return "", ""
    if rec.get("status") != "active" or _account_kind(rec) != "user":
        return "", ""
    return name_bi, _name(rec.get("name", ""))


# --- points -------------------------------------------------------------------

async def _award_points(env, slug, account_bi, kind, ref="", now=0):
    """Write one points row, at most once, and refresh the cached total.

    The ledger's primary key is the whole anti-double-pay rule: a unique kind
    collapses its reference to the empty string, so a second insert for the
    same participant is rejected by the key rather than by a race-prone
    read-then-write. ``INSERT ... ON CONFLICT DO NOTHING ... RETURNING``
    tells us whether this call was the one that landed, so the cached total
    on the participant row is only moved by the write that actually happened.
    """
    value = policy.point_value(kind)
    if not value or not slug or not account_bi:
        return False
    reference = policy.point_reference(kind, ref)
    now = int(now or _now())
    landed = await d1_first(
        env,
        "INSERT INTO hackathon_points "
        "(hackathon_slug,account_bi,kind,ref,points,created_at) "
        "VALUES (?,?,?,?,?,?) "
        "ON CONFLICT(hackathon_slug,account_bi,kind,ref) DO NOTHING "
        "RETURNING points",
        slug, account_bi, str(kind), reference, value, now)
    if not landed:
        return False
    await d1_run(
        env,
        "UPDATE hackathon_participants SET points=points+? "
        "WHERE hackathon_slug=? AND account_bi=?",
        value, slug, account_bi)
    await d1_run(
        env,
        "UPDATE hackathon_squads SET points=points+? "
        "WHERE hackathon_slug=? AND squad_id=("
        "SELECT squad_id FROM hackathon_participants "
        "WHERE hackathon_slug=? AND account_bi=?) ",
        value, slug, slug, account_bi)
    await _resolve_challenges(env, slug, account_bi, now)
    return True


async def _rank(env, slug, points, account_bi):
    """1-based rank by points, ties broken by who joined first.

    One indexed count rather than a scan for the row: the Builders index is
    ``(hackathon_slug, points DESC, joined_at)``, so this reads the rows
    ahead of the participant and stops.
    """
    row = await d1_first(
        env,
        "SELECT COUNT(*) AS ahead FROM hackathon_participants "
        "WHERE hackathon_slug=? AND ("
        "points>? OR (points=? AND joined_at<("
        "SELECT joined_at FROM hackathon_participants "
        "WHERE hackathon_slug=? AND account_bi=?)))",
        slug, int(points or 0), int(points or 0), slug, account_bi)
    return max(0, int((row or {}).get("ahead") or 0)) + 1


# --- squads -------------------------------------------------------------------

def _squad_code(name):
    """A stable, readable squad code derived from the owner's username."""
    base = re.sub(r"[^a-z0-9]", "", _name(name))[:8] or "squad"
    return base


async def _ensure_squad(env, slug, owner_bi, owner_name, now):
    """The owner's squad id, creating the squad on first use.

    A squad exists only once somebody has actually been brought in, so an
    account that never shares never appears on the Squads board with a squad
    of one.
    """
    existing = await d1_first(
        env,
        "SELECT squad_id FROM hackathon_squads "
        "WHERE hackathon_slug=? AND owner_bi=?",
        slug, owner_bi)
    if existing:
        return str(existing.get("squad_id") or "")
    squad_id = _ap_uuid()
    code = _squad_code(owner_name)
    # The code is the public URL segment, so it must be unique per campaign.
    # A username is already unique, so the first spelling nearly always wins;
    # the suffix only exists for the truncation collision.
    for attempt in range(6):
        candidate = code if attempt == 0 else "%s%d" % (code, attempt + 1)
        # Seeded with the owner's points, not zero: the squad total is the
        # sum of its members' points, and the owner already has some by the
        # time their first invitee arrives.
        landed = await d1_first(
            env,
            "INSERT INTO hackathon_squads "
            "(squad_id,hackathon_slug,code,owner_bi,owner_name,member_count,"
            "points,created_at) VALUES (?,?,?,?,?,1,"
            "COALESCE((SELECT points FROM hackathon_participants "
            "WHERE hackathon_slug=? AND account_bi=?),0),?) "
            "ON CONFLICT DO NOTHING RETURNING squad_id",
            squad_id, slug, candidate, owner_bi, owner_name, slug, owner_bi,
            now)
        if landed:
            await d1_run(
                env,
                "UPDATE hackathon_participants SET squad_id=? "
                "WHERE hackathon_slug=? AND account_bi=? AND squad_id=''",
                squad_id, slug, owner_bi)
            await d1_run(
                env,
                "UPDATE hackathons SET squad_count=squad_count+1,updated_at=? "
                "WHERE slug=?",
                now, slug)
            await _check_milestones(env, slug, now)
            return squad_id
    return ""


async def _squad(env, slug, code):
    code = str(code or "").strip().lower()
    if not slug or not policy.SQUAD_CODE_RE.match(code):
        return None
    return await d1_first(
        env,
        "SELECT * FROM hackathon_squads WHERE hackathon_slug=? AND code=?",
        slug, code)


# --- joining ------------------------------------------------------------------

async def _create_participant(env, slug, account_bi, name, referrer_bi="",
                              referrer_name="", now=0):
    """Create one participant row, idempotently, and place them in a squad.

    Returns True only when this call created the row, so the campaign
    counter and the joining points move exactly once.
    """
    now = int(now or _now())
    landed = await d1_first(
        env,
        "INSERT INTO hackathon_participants "
        "(hackathon_slug,account_bi,name,referrer_bi,referrer_name,squad_id,"
        "track,points,onboarding_step,share_count,referrals_joined,"
        "referrals_qualified,joined_at,verified_at) "
        "VALUES (?,?,?,?,?,'','',0,1,0,0,0,?,0) "
        "ON CONFLICT(hackathon_slug,account_bi) DO NOTHING "
        "RETURNING account_bi",
        slug, account_bi, name, referrer_bi, referrer_name, now)
    if not landed:
        return False
    await d1_run(
        env,
        "UPDATE hackathons SET participant_count=participant_count+1,"
        "updated_at=? WHERE slug=?",
        now, slug)
    await _check_milestones(env, slug, now)
    # Squad placement happens before the first points are awarded, so those
    # points land in the squad total too. _award_points adds to whatever
    # squad the participant is already in, and a participant with none is
    # simply skipped.
    if referrer_bi:
        squad_id = await _ensure_squad(
            env, slug, referrer_bi, referrer_name, now)
        if squad_id:
            await d1_run(
                env,
                "UPDATE hackathon_participants SET squad_id=? "
                "WHERE hackathon_slug=? AND account_bi=?",
                squad_id, slug, account_bi)
            await d1_run(
                env,
                "UPDATE hackathon_squads SET member_count=member_count+1 "
                "WHERE squad_id=?",
                squad_id)
    await _award_points(env, slug, account_bi, "joined", now=now)
    if referrer_bi:
        campaign = await _campaign(env, slug)
        participant = await _participant(env, slug, account_bi)
        squad_id = str((participant or {}).get("squad_id") or "")
        if campaign and squad_id:
            await _evaluate_squad(env, campaign, squad_id, account_bi)
    await edge_cache_delete(CAMPAIGN_CACHE_KEY)
    return True


# --- rewards ------------------------------------------------------------------
# Every membership month and badge goes through the ledger first. The claim is
# one INSERT ... ON CONFLICT DO NOTHING ... RETURNING, so a reward whose event
# is observed twice (a retried hook, a mission re-evaluated on the next
# refresh) is granted exactly once without a read-then-write race.
#
# Membership months are then projected into role_grants under the promotion
# scope, never the platform scope. Patreon revokes platform-scope membership
# rows on every sync -- without filtering by scope_bi -- so a prize written
# there would vanish the next time somebody's subscription renewed. No billing
# provider touches scope_type='promotion'.
REWARD_NOTIFICATION_KIND = "hackathon_reward"


async def _promotion_scope(env, slug):
    return await blind_index(env, "hackathon:" + str(slug))


async def _extend_promotion(env, slug, account_bi, role, months, now):
    """Add ``months`` to one promotional role, atomically, and return its expiry.

    One upsert computes MAX(live expiry, now) + months inside the database, the
    same rule as policy.extend_expiry, so two rewards landing together both
    count instead of the second read overwriting the first write.
    """
    scope_bi = await _promotion_scope(env, slug)
    span = max(0, int(months)) * policy.MONTH_MS
    row = await d1_first(
        env,
        "INSERT INTO role_grants "
        "(account_bi,role,scope_type,scope_bi,granted_by_bi,granted_at,"
        "expires_at,revoked_at) VALUES (?,?,?,?,?,?,?,0) "
        "ON CONFLICT(account_bi,role,scope_type,scope_bi) DO UPDATE SET "
        "granted_by_bi=excluded.granted_by_bi,"
        "granted_at=excluded.granted_at,"
        "expires_at=MAX(CASE WHEN role_grants.revoked_at=0 "
        "THEN role_grants.expires_at ELSE 0 END,?)+?,"
        "revoked_at=0 RETURNING expires_at",
        account_bi, role, policy.PROMOTION_SCOPE_TYPE, scope_bi, scope_bi,
        int(now), int(now) + span, int(now), span)
    return max(0, int((row or {}).get("expires_at") or 0))


async def _grant_rewards(env, slug, account_bi, name, grants, now=0):
    """Grant each ledger row once. Returns the rows this call actually granted."""
    now = int(now or _now())
    granted = []
    for grant in grants or ():
        kind = str(grant.get("kind") or "")
        months = max(0, int(grant.get("months") or 0))
        badge = str(grant.get("badge") or "")
        key = str(grant.get("reward_key") or "")[:160]
        if not key or kind not in ("pro_months", "supporter_months", "badge"):
            continue
        claimed = await d1_first(
            env,
            "INSERT INTO hackathon_reward_grants "
            "(hackathon_slug,account_bi,reward_key,kind,months,badge,"
            "granted_at,expires_at) VALUES (?,?,?,?,?,?,?,0) "
            "ON CONFLICT(hackathon_slug,account_bi,reward_key,kind) "
            "DO NOTHING RETURNING kind",
            slug, account_bi, key, kind, months, badge, now)
        if not claimed:
            continue
        if kind in policy.MEMBERSHIP_ROLES and months:
            expires = 0
            for role in policy.MEMBERSHIP_ROLES[kind]:
                expires = max(expires, await _extend_promotion(
                    env, slug, account_bi, role, months, now))
            await d1_run(
                env,
                "UPDATE hackathon_reward_grants SET expires_at=? "
                "WHERE hackathon_slug=? AND account_bi=? AND reward_key=? "
                "AND kind=?",
                expires, slug, account_bi, key, kind)
        elif kind == "badge" and badge:
            await _award_badge(env, name, account_bi, badge, "hackathon")
        granted.append(dict(grant, expires_at=0))
    if granted and name:
        await _notify_rewards(env, slug, name, granted)
    return granted


async def _notify_rewards(env, slug, name, granted):
    """One notification per reward key, only ever to the person rewarded."""
    by_key = {}
    for grant in granted:
        by_key.setdefault(str(grant.get("reward_key") or ""), []).append(grant)
    for key, rows in by_key.items():
        months = sum(int(r.get("months") or 0) for r in rows
                     if r.get("kind") == "pro_months")
        supporter = sum(int(r.get("months") or 0) for r in rows
                        if r.get("kind") == "supporter_months")
        badges = [r.get("badge") for r in rows if r.get("kind") == "badge"]
        parts = []
        if months:
            parts.append("%d month%s of Pro" % (months, "" if months == 1
                                                 else "s"))
        if supporter:
            parts.append("%d month%s of Supporter" % (
                supporter, "" if supporter == 1 else "s"))
        if badges:
            parts.append("a badge")
        if not parts:
            continue
        try:
            await enqueue_notification(
                env, name, REWARD_NOTIFICATION_KIND,
                "You earned " + " and ".join(parts),
                body="From the ForkMesh Hackathon. See it on your hackathon "
                     "HQ.",
                href="/hackathon/me", source="hackathon:" + slug,
                dedupe="hackathon:%s:reward:%s" % (slug, key))
        except Exception:
            continue


async def _rewards_for(env, slug, account_bi):
    rows = await d1_all(
        env,
        "SELECT reward_key,kind,months,badge,granted_at,expires_at "
        "FROM hackathon_reward_grants WHERE hackathon_slug=? AND account_bi=? "
        "ORDER BY granted_at DESC LIMIT 50",
        slug, account_bi)
    return [{
        "key": str(row.get("reward_key") or ""),
        "kind": str(row.get("kind") or ""),
        "months": max(0, int(row.get("months") or 0)),
        "badge": str(row.get("badge") or ""),
        "grantedAt": max(0, int(row.get("granted_at") or 0)),
        "expiresAt": max(0, int(row.get("expires_at") or 0)),
    } for row in rows or []]


async def _evaluate_missions(env, row, account_bi):
    """Grant whatever one participant's finished missions now owe them.

    Cheap to call again: every grant is keyed, so re-evaluating a participant
    whose missions were already rewarded writes nothing.
    """
    try:
        slug = str(row.get("slug") or "")
        participant = await _participant(env, slug, account_bi)
        if not participant:
            return []
        config = _config(row)
        counters, _squad = await _participant_counters(
            env, slug, participant, config)
        grants = policy.completed_mission_grants(
            config.get("missions"), counters,
            str(participant.get("squad_id") or ""))
        now = _now()
        for mission in policy.missions_state(config.get("missions"), counters):
            if not mission["complete"]:
                continue
            await d1_run(
                env,
                "INSERT INTO hackathon_mission_progress "
                "(hackathon_slug,account_bi,mission_key,progress,completed_at,"
                "rewarded_at) VALUES (?,?,?,?,?,?) "
                "ON CONFLICT(hackathon_slug,account_bi,mission_key) DO NOTHING",
                slug, account_bi, mission["key"], mission["progress"], now,
                now if any(g["reward_key"].startswith(
                    "mission:" + mission["key"]) for g in grants) else 0)
        return await _grant_rewards(
            env, slug, account_bi, _name(participant.get("name", "")),
            grants, now)
    except Exception:
        return []


async def _evaluate_squad(env, row, squad_id, newcomer_bi):
    """Re-evaluate a squad whose size just changed.

    Squad-scoped missions reward every member. Crossing a threshold therefore
    evaluates the whole squad once; after that, only each newcomer needs
    evaluating, because everyone already there was rewarded at the crossing.
    """
    slug = str(row.get("slug") or "")
    squad = await d1_first(
        env, "SELECT member_count FROM hackathon_squads WHERE squad_id=?",
        squad_id)
    size = max(0, int((squad or {}).get("member_count") or 0))
    thresholds = {
        int(m.get("threshold") or 0)
        for m in _config(row).get("missions") or ()
        if str((m.get("reward") or {}).get("scope") or "") == "squad"
    }
    if size in thresholds:
        members = await d1_all(
            env,
            "SELECT account_bi FROM hackathon_participants "
            "WHERE hackathon_slug=? AND squad_id=? LIMIT ?",
            slug, squad_id, max(thresholds) + 50)
        for member in members or []:
            await _evaluate_missions(env, row, str(member.get("account_bi")))
        owner = await d1_first(
            env, "SELECT owner_bi FROM hackathon_squads WHERE squad_id=?",
            squad_id)
        if owner:
            await _evaluate_missions(env, row, str(owner.get("owner_bi")))
    else:
        await _evaluate_missions(env, row, newcomer_bi)


# --- challenge links ------------------------------------------------------------
# "Alex has 1,240 points. Can you beat that?" The target is fixed when the link
# is sent, so a challenge is decided once. Beating one pays nothing: the
# outcome is derived from points already earned, and paying for it would let
# two friends mint points by challenging each other in a loop.

async def _resolve_challenges(env, slug, account_bi, now):
    """Mark every open challenge this participant has now beaten, once each.

    One UPDATE ... RETURNING claims the beaten entries, so a challenge is
    announced exactly once however many point awards land together.
    """
    try:
        participant = await d1_first(
            env,
            "SELECT points,name FROM hackathon_participants "
            "WHERE hackathon_slug=? AND account_bi=?",
            slug, account_bi)
        if not participant:
            return []
        points = max(0, int(participant.get("points") or 0))
        beaten = await d1_all(
            env,
            "UPDATE hackathon_challenge_entries SET beaten_at=? "
            "WHERE hackathon_slug=? AND account_bi=? AND beaten_at=0 "
            "AND token IN (SELECT token FROM hackathon_challenges "
            "WHERE hackathon_slug=? AND expires_at>? AND target_points<?) "
            "RETURNING token",
            int(now), slug, account_bi, slug, int(now), points)
        name = _name(participant.get("name", ""))
        for entry in beaten or []:
            token = str(entry.get("token") or "")
            challenge = await d1_first(
                env,
                "UPDATE hackathon_challenges SET beaten_count=beaten_count+1 "
                "WHERE token=? RETURNING challenger_name,target_points",
                token)
            challenger = _name((challenge or {}).get("challenger_name", ""))
            target = int((challenge or {}).get("target_points") or 0)
            if not challenger:
                continue
            await enqueue_notification(
                env, name, "hackathon_squad",
                "You beat %s's %s points" % (challenger, "{:,}".format(target)),
                body="Now challenge three friends of your own.",
                href="/hackathon/me", source="hackathon:" + slug,
                dedupe="hackathon:%s:beat:%s" % (slug, token))
            await enqueue_notification(
                env, challenger, "hackathon_squad",
                "%s beat your challenge" % name,
                body="They passed the %s points you set. Your move." % (
                    "{:,}".format(target)),
                href="/hackathon/me", source="hackathon:" + slug,
                dedupe="hackathon:%s:beaten:%s:%s" % (slug, token, name))
        return [str(entry.get("token") or "") for entry in beaten or []]
    except Exception:
        return []


def _challenge_link(env, request, token):
    return "%s/hackathon/challenge/%s" % (_site_origin(env, request), token)


async def _challenge(env, slug, token):
    token = str(token or "")
    if not slug or not policy.CHALLENGE_TOKEN_RE.match(token):
        return None
    return await d1_first(
        env,
        "SELECT * FROM hackathon_challenges WHERE hackathon_slug=? AND token=?",
        slug, token)


async def _challenges_for(env, request, slug, account_bi, now):
    """The participant's sent challenges and the ones they accepted."""
    sent = await d1_all(
        env,
        "SELECT * FROM hackathon_challenges WHERE hackathon_slug=? "
        "AND challenger_bi=? ORDER BY created_at DESC LIMIT ?",
        slug, account_bi, policy.MAX_OPEN_CHALLENGES)
    accepted = await d1_all(
        env,
        "SELECT c.*,e.accepted_at,e.beaten_at FROM hackathon_challenge_entries e "
        "JOIN hackathon_challenges c ON c.token=e.token "
        "WHERE e.hackathon_slug=? AND e.account_bi=? "
        "ORDER BY e.accepted_at DESC LIMIT ?",
        slug, account_bi, policy.MAX_OPEN_CHALLENGES)
    out_sent = []
    for row in sent or []:
        state = policy.challenge_state(row, now)
        state["link"] = _challenge_link(env, request, state["token"])
        out_sent.append(state)
    return {
        "sent": out_sent,
        "accepted": [policy.challenge_state(row, now, entry=row)
                     for row in accepted or []],
        "maxOpen": policy.MAX_OPEN_CHALLENGES,
    }


async def challenges_handler(env, request):
    """``GET|POST /api/hackathon/challenges`` -- list, or send a new challenge."""
    method = method_name(request)
    if method not in ("GET", "POST"):
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            extra_headers={"allow": "GET, POST"})
    data = {}
    if method == "POST":
        try:
            data = await bounded_json_request(request, BODY_MAX_BYTES) or {}
        except Exception:
            return json_response({"error": "invalid_json"}, status=400)
    account_bi, name = await _session_account(env, request, data)
    if not account_bi:
        return json_response({"error": "unauthorized"}, status=401,
                             cache_control="no-store")
    row = await _public_campaign(env)
    if not row:
        return json_response({"error": "not_found"}, status=404,
                             cache_control="no-store")
    slug = str(row.get("slug") or "")
    participant = await _participant(env, slug, account_bi)
    if not participant:
        return json_response({"error": "not_joined"}, status=409,
                             cache_control="no-store")
    now = _now()
    if method == "POST":
        if str(row.get("status") or "") not in policy.JOINABLE_STATUSES:
            return json_response({"error": "campaign_closed"}, status=409,
                                 cache_control="no-store")
        open_count = await d1_first(
            env,
            "SELECT COUNT(*) AS n FROM hackathon_challenges "
            "WHERE hackathon_slug=? AND challenger_bi=? AND expires_at>?",
            slug, account_bi, now)
        if int((open_count or {}).get("n") or 0) >= policy.MAX_OPEN_CHALLENGES:
            return json_response({"error": "too_many_open_challenges"},
                                 status=429, cache_control="no-store")
        token = _ap_uuid()
        await d1_run(
            env,
            "INSERT INTO hackathon_challenges (token,hackathon_slug,"
            "challenger_bi,challenger_name,target_points,accepted_count,"
            "beaten_count,created_at,expires_at) VALUES (?,?,?,?,?,0,0,?,?)",
            token, slug, account_bi, name,
            max(0, int(participant.get("points") or 0)), now,
            now + policy.challenge_ttl_ms(_config(row)))
    payload = await _challenges_for(env, request, slug, account_bi, now)
    payload["ok"] = True
    return json_response(payload, status=201 if method == "POST" else 200,
                         cache_control="no-store")


async def challenge_accept_handler(env, request, token):
    """``POST /api/hackathon/challenges/<token>/accept``.

    Accepting joins the campaign if needed, but deliberately places nobody in
    the challenger's squad and records no referrer: a challenge is not a
    referral, and treating it as one would pay the challenger joining points
    the referral ledger never decided.
    """
    if method_name(request) != "POST":
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            extra_headers={"allow": "POST"})
    try:
        data = await bounded_json_request(request, BODY_MAX_BYTES) or {}
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    account_bi, name = await _session_account(env, request, data)
    if not account_bi:
        return json_response({"error": "unauthorized"}, status=401,
                             cache_control="no-store")
    row = await _public_campaign(env)
    if not row:
        return json_response({"error": "not_found"}, status=404,
                             cache_control="no-store")
    slug = str(row.get("slug") or "")
    now = _now()
    challenge = await _challenge(env, slug, token)
    if not challenge:
        return json_response({"error": "not_found"}, status=404,
                             cache_control="no-store")
    if str(challenge.get("challenger_bi") or "") == account_bi:
        return json_response({"error": "own_challenge"}, status=409,
                             cache_control="no-store")
    if int(challenge.get("expires_at") or 0) <= now:
        return json_response({"error": "challenge_expired"}, status=410,
                             cache_control="no-store")
    if not await _participant(env, slug, account_bi):
        if str(row.get("status") or "") not in policy.JOINABLE_STATUSES:
            return json_response({"error": "registration_closed"},
                                 status=409, cache_control="no-store")
        await _create_participant(env, slug, account_bi, name)
        _, rec = await _account_row(env, name)
        if rec and rec.get("email_verified") is True:
            await on_email_verified(env, account_bi, name)
    existing = await d1_first(
        env,
        "SELECT accepted_at FROM hackathon_challenge_entries "
        "WHERE token=? AND account_bi=?",
        str(token), account_bi)
    if not existing:
        claimed = await d1_first(
            env,
            "UPDATE hackathon_challenges SET accepted_count=accepted_count+1 "
            "WHERE token=? AND accepted_count<? RETURNING token",
            str(token), policy.MAX_CHALLENGE_ENTRIES)
        if not claimed:
            return json_response({"error": "challenge_full"}, status=409,
                                 cache_control="no-store")
        await d1_run(
            env,
            "INSERT INTO hackathon_challenge_entries (token,hackathon_slug,"
            "account_bi,name,accepted_at,beaten_at) VALUES (?,?,?,?,?,0) "
            "ON CONFLICT(token,account_bi) DO NOTHING",
            str(token), slug, account_bi, name, now)
        # Somebody already ahead of the target has beaten it on arrival.
        await _resolve_challenges(env, slug, account_bi, now)
    entry = await d1_first(
        env,
        "SELECT accepted_at,beaten_at FROM hackathon_challenge_entries "
        "WHERE token=? AND account_bi=?",
        str(token), account_bi)
    fresh = await _challenge(env, slug, token)
    return json_response({
        "ok": True,
        "challenge": policy.challenge_state(fresh, now, entry=entry),
    }, status=201 if not existing else 200, cache_control="no-store")


# --- teams ---------------------------------------------------------------------
# A Team is the submission unit: at most five people and one project per track.
# It is a different thing from a Squad, which is everybody one participant
# brought in, and the two words are never interchanged.

async def _team_of(env, slug, account_bi):
    """``(team, members)`` for a participant's team, or ``(None, [])``."""
    membership = await d1_first(
        env,
        "SELECT team_id FROM hackathon_team_members "
        "WHERE hackathon_slug=? AND account_bi=?",
        slug, account_bi)
    if not membership:
        return None, []
    team = await d1_first(
        env, "SELECT * FROM hackathon_teams WHERE team_id=?",
        str(membership.get("team_id") or ""))
    if not team:
        return None, []
    members = await d1_all(
        env,
        "SELECT account_bi,name,joined_at FROM hackathon_team_members "
        "WHERE team_id=? ORDER BY joined_at,name LIMIT ?",
        str(team.get("team_id") or ""), policy.MAX_TEAM_MEMBERS)
    return team, [dict(row) for row in members or []]


async def _team_members_bi(env, team_id):
    rows = await d1_all(
        env,
        "SELECT account_bi FROM hackathon_team_members WHERE team_id=? LIMIT ?",
        str(team_id), policy.MAX_TEAM_MEMBERS)
    return [str(row.get("account_bi") or "") for row in rows or []]


async def _team_payload(env, slug, account_bi, config):
    team, members = await _team_of(env, slug, account_bi)
    if not team:
        return None
    submissions = await d1_all(
        env,
        "SELECT * FROM hackathon_submissions WHERE team_id=? "
        "ORDER BY submitted_at",
        str(team.get("team_id") or ""))
    return {
        "id": str(team.get("team_id") or ""),
        "name": str(team.get("name") or ""),
        "code": str(team.get("invite_code") or ""),
        "captain": _name(team.get("captain_name", "")),
        "members": [_name(m.get("name", "")) for m in members],
        "full": len(members) >= policy.MAX_TEAM_MEMBERS,
        "submissions": [_submission_public(row, config, include_review=True)
                        for row in submissions or []],
    }


def _team_editable(row, now):
    return str(row.get("status") or "") in policy.JOINABLE_STATUSES


def _submissions_open(row, now):
    ends = int(row.get("ends_at") or 0)
    return (str(row.get("status") or "") == "live"
            and (not ends or int(now) <= ends))


async def teams_handler(env, request, action=""):
    """``POST /api/hackathon/teams[/join|/leave]`` -- create, join or leave."""
    if method_name(request) != "POST":
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            extra_headers={"allow": "POST"})
    try:
        data = await bounded_json_request(request, BODY_MAX_BYTES) or {}
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    account_bi, name = await _session_account(env, request, data)
    if not account_bi:
        return json_response({"error": "unauthorized"}, status=401,
                             cache_control="no-store")
    row = await _public_campaign(env)
    if not row:
        return json_response({"error": "not_found"}, status=404,
                             cache_control="no-store")
    slug = str(row.get("slug") or "")
    now = _now()
    if not await _participant(env, slug, account_bi):
        return json_response({"error": "not_joined"}, status=409,
                             cache_control="no-store")
    if not _team_editable(row, now):
        return json_response({"error": "teams_closed"}, status=409,
                             cache_control="no-store")
    team, members = await _team_of(env, slug, account_bi)
    if action == "leave":
        if not team:
            return json_response({"error": "no_team"}, status=409,
                                 cache_control="no-store")
        team_id = str(team.get("team_id") or "")
        remaining = [m for m in members if m.get("account_bi") != account_bi]
        if not remaining:
            live = await d1_first(
                env,
                "SELECT COUNT(*) AS n FROM hackathon_submissions "
                "WHERE team_id=? AND status<>'withdrawn'",
                team_id)
            if int((live or {}).get("n") or 0):
                return json_response(
                    {"error": "withdraw_submissions_first"}, status=409,
                    cache_control="no-store")
        await d1_run(
            env,
            "DELETE FROM hackathon_team_members "
            "WHERE hackathon_slug=? AND account_bi=?",
            slug, account_bi)
        if remaining:
            captain = remaining[0]
            await d1_run(
                env,
                "UPDATE hackathon_teams SET member_count=?,captain_bi="
                "CASE WHEN captain_bi=? THEN ? ELSE captain_bi END,"
                "captain_name=CASE WHEN captain_bi=? THEN ? ELSE captain_name "
                "END WHERE team_id=?",
                len(remaining), account_bi, captain["account_bi"],
                account_bi, captain["name"], team_id)
        else:
            await d1_run(env, "DELETE FROM hackathon_teams WHERE team_id=?",
                         team_id)
    elif action == "join":
        if team:
            return json_response({"error": "already_on_a_team"}, status=409,
                                 cache_control="no-store")
        code = str(data.get("code") or "").strip().lower()
        if not policy.TEAM_CODE_RE.match(code):
            return json_response({"error": "invalid_team_code"}, status=400,
                                 cache_control="no-store")
        target = await d1_first(
            env,
            "SELECT * FROM hackathon_teams "
            "WHERE hackathon_slug=? AND invite_code=?",
            slug, code)
        if not target:
            return json_response({"error": "team_not_found"}, status=404,
                                 cache_control="no-store")
        claimed = await d1_first(
            env,
            "UPDATE hackathon_teams SET member_count=member_count+1 "
            "WHERE team_id=? AND member_count<? RETURNING team_id",
            str(target.get("team_id") or ""), policy.MAX_TEAM_MEMBERS)
        if not claimed:
            return json_response({"error": "team_full"}, status=409,
                                 cache_control="no-store")
        await d1_run(
            env,
            "INSERT INTO hackathon_team_members "
            "(hackathon_slug,account_bi,team_id,name,joined_at) "
            "VALUES (?,?,?,?,?)",
            slug, account_bi, str(target.get("team_id") or ""), name, now)
        # Somebody joining a team that already submitted earns that
        # submission's points too, so the building counters agree with the
        # gallery.
        existing = await d1_all(
            env,
            "SELECT submission_id FROM hackathon_submissions "
            "WHERE team_id=? AND status<>'withdrawn'",
            str(target.get("team_id") or ""))
        for entry in existing or []:
            await _award_points(env, slug, account_bi, "submission",
                                ref=str(entry.get("submission_id")), now=now)
        await _evaluate_missions(env, row, account_bi)
    else:
        if team:
            return json_response({"error": "already_on_a_team"}, status=409,
                                 cache_control="no-store")
        team_name = policy.normalize_team_name(data.get("name"))
        if not team_name:
            return json_response({"error": "invalid_team_name"}, status=400,
                                 cache_control="no-store")
        team_id = _ap_uuid()
        created = False
        for _attempt in range(5):
            # A fresh id each attempt, taking its tail: the part of any id
            # least likely to share a prefix with the last one.
            code = _ap_uuid()[-8:]
            landed = await d1_first(
                env,
                "INSERT INTO hackathon_teams (team_id,hackathon_slug,name,"
                "captain_bi,captain_name,invite_code,member_count,created_at) "
                "VALUES (?,?,?,?,?,?,1,?) ON CONFLICT DO NOTHING "
                "RETURNING team_id",
                team_id, slug, team_name, account_bi, name, code, now)
            if landed:
                created = True
                break
        if not created:
            return json_response({"error": "try_again"}, status=503,
                                 cache_control="no-store")
        await d1_run(
            env,
            "INSERT INTO hackathon_team_members "
            "(hackathon_slug,account_bi,team_id,name,joined_at) "
            "VALUES (?,?,?,?,?)",
            slug, account_bi, team_id, name, now)
    return json_response({
        "ok": True,
        "team": await _team_payload(env, slug, account_bi, _config(row)),
    }, cache_control="no-store")


# --- submissions ----------------------------------------------------------------

def _submission_public(row, config, include_review=False, votes_open=False):
    """What anyone may see about one submission, plus review detail for its team."""
    row = row or {}
    tracks = {str(t.get("id") or ""): str(t.get("name") or "")
              for t in (config or {}).get("tracks") or ()}
    out = {
        "id": str(row.get("submission_id") or ""),
        "track": str(row.get("track") or ""),
        "trackName": tracks.get(str(row.get("track") or ""), ""),
        "repo": "%s/%s" % (row.get("repo_owner") or "", row.get("repo_name")
                           or ""),
        "title": str(row.get("title") or ""),
        "pitch": str(row.get("pitch") or ""),
        "demoUrl": str(row.get("demo_url") or ""),
        "videoUrl": str(row.get("video_url") or ""),
        "placement": str(row.get("placement") or ""),
        "submittedAt": max(0, int(row.get("submitted_at") or 0)),
    }
    if votes_open or out["placement"]:
        out["votes"] = max(0, int(row.get("vote_count") or 0))
    if include_review:
        try:
            flags = json.loads(row.get("review_flags") or "[]")
        except ValueError:
            flags = []
        out["status"] = str(row.get("status") or "")
        out["reviewFlags"] = [str(flag) for flag in flags or []]
    return out


async def _repository_facts(env, owner, name):
    """``(in_catalog, first_hosted_at)`` for a submitted repository.

    Public means published in the catalog and not private: the gallery links
    to it, and a link a visitor cannot open is not a project they can judge.
    """
    for candidate in (owner + "/" + name, owner + "/" + name.lower()):
        key_bi = await blind_index(env, candidate)
        found = await d1_first(
            env, "SELECT key_bi FROM repositories WHERE key_bi=?", key_bi)
        if not found:
            continue
        private = globals().get("_repo_is_private")
        if callable(private) and await private(
                env, owner, candidate.split("/", 1)[1]):
            return False, 0
        first = await d1_first(
            env, "SELECT ts FROM repo_first_hosted WHERE repo_bi=?", key_bi)
        return True, max(0, int((first or {}).get("ts") or 0))
    return False, 0


async def submissions_handler(env, request):
    """``POST /api/hackathon/submissions`` -- submit, or update, a project.

    One submission per team per track; posting again for the same track edits
    it until the deadline. The catalog check runs on every save, and anything
    it cannot confirm is flagged for a judge rather than rejected.
    """
    if method_name(request) != "POST":
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            extra_headers={"allow": "POST"})
    try:
        data = await bounded_json_request(request, BODY_MAX_BYTES) or {}
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    account_bi, name = await _session_account(env, request, data)
    if not account_bi:
        return json_response({"error": "unauthorized"}, status=401,
                             cache_control="no-store")
    row = await _public_campaign(env)
    if not row:
        return json_response({"error": "not_found"}, status=404,
                             cache_control="no-store")
    slug = str(row.get("slug") or "")
    now = _now()
    if not _submissions_open(row, now):
        return json_response({"error": "submissions_closed"}, status=409,
                             cache_control="no-store")
    team, _members = await _team_of(env, slug, account_bi)
    if not team:
        return json_response({"error": "no_team"}, status=409,
                             cache_control="no-store")
    config = _config(row)
    fields, error = policy.normalize_submission(data, config)
    if error:
        return json_response({"error": error}, status=400,
                             cache_control="no-store")
    in_catalog, first_hosted = await _repository_facts(
        env, fields["repo_owner"], fields["repo_name"])
    flags = policy.submission_review_flags(
        in_catalog, first_hosted, row.get("starts_at"), row.get("ends_at"))
    team_id = str(team.get("team_id") or "")
    existing = await d1_first(
        env,
        "SELECT submission_id,status FROM hackathon_submissions "
        "WHERE hackathon_slug=? AND team_id=? AND track=?",
        slug, team_id, fields["track"])
    status = policy.submission_status_for(flags)
    if existing and str(existing.get("status") or "") == "invalid":
        # An administrator's rejection sticks: editing a project does not
        # quietly re-open it.
        status = "invalid"
    encoded_flags = json.dumps(flags, separators=(",", ":"))
    if existing:
        submission_id = str(existing.get("submission_id") or "")
        was_withdrawn = str(existing.get("status") or "") == "withdrawn"
        await d1_run(
            env,
            "UPDATE hackathon_submissions SET repo_owner=?,repo_name=?,"
            "title=?,pitch=?,demo_url=?,video_url=?,status=?,review_flags=?,"
            "updated_at=? WHERE submission_id=?",
            fields["repo_owner"], fields["repo_name"], fields["title"],
            fields["pitch"], fields["demo_url"], fields["video_url"], status,
            encoded_flags, now, submission_id)
        if was_withdrawn:
            await d1_run(
                env,
                "UPDATE hackathons SET submission_count=submission_count+1 "
                "WHERE slug=?", slug)
            await _check_milestones(env, slug, now)
    else:
        submission_id = _ap_uuid()
        await d1_run(
            env,
            "INSERT INTO hackathon_submissions (submission_id,hackathon_slug,"
            "team_id,track,repo_owner,repo_name,title,pitch,demo_url,"
            "video_url,status,review_flags,placement,score_total,score_count,"
            "vote_count,submitted_at,updated_at) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,'',0,0,0,?,?)",
            submission_id, slug, team_id, fields["track"],
            fields["repo_owner"], fields["repo_name"], fields["title"],
            fields["pitch"], fields["demo_url"], fields["video_url"], status,
            encoded_flags, now, now)
        await d1_run(
            env,
            "UPDATE hackathons SET submission_count=submission_count+1,"
            "updated_at=? WHERE slug=?", now, slug)
        await _check_milestones(env, slug, now)
    for member_bi in await _team_members_bi(env, team_id):
        await _award_points(env, slug, member_bi, "submission",
                            ref=submission_id, now=now)
        await _evaluate_missions(env, row, member_bi)
    await edge_cache_delete(GALLERY_CACHE_KEY)
    await edge_cache_delete(CAMPAIGN_CACHE_KEY)
    return json_response({
        "ok": True,
        "submissionId": submission_id,
        "status": status,
        "reviewFlags": flags,
        "team": await _team_payload(env, slug, account_bi, config),
    }, status=200 if existing else 201, cache_control="no-store")


async def submission_withdraw_handler(env, request, submission_id):
    """``POST /api/hackathon/submissions/<id>/withdraw`` -- before the deadline."""
    if method_name(request) != "POST":
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            extra_headers={"allow": "POST"})
    try:
        data = await bounded_json_request(request, BODY_MAX_BYTES) or {}
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    account_bi, _name_value = await _session_account(env, request, data)
    if not account_bi:
        return json_response({"error": "unauthorized"}, status=401,
                             cache_control="no-store")
    row = await _public_campaign(env)
    if not row:
        return json_response({"error": "not_found"}, status=404,
                             cache_control="no-store")
    slug = str(row.get("slug") or "")
    now = _now()
    if not _submissions_open(row, now):
        return json_response({"error": "submissions_closed"}, status=409,
                             cache_control="no-store")
    team, _members = await _team_of(env, slug, account_bi)
    withdrawn = await d1_first(
        env,
        "UPDATE hackathon_submissions SET status='withdrawn',updated_at=? "
        "WHERE submission_id=? AND team_id=? AND status<>'withdrawn' "
        "RETURNING submission_id",
        now, str(submission_id), str((team or {}).get("team_id") or ""))
    if not withdrawn:
        return json_response({"error": "not_found"}, status=404,
                             cache_control="no-store")
    await d1_run(
        env,
        "UPDATE hackathons SET submission_count=MAX(0,submission_count-1) "
        "WHERE slug=?", slug)
    # Votes cast for a withdrawn project are released for the voter to move.
    await d1_run(env, "DELETE FROM hackathon_votes WHERE submission_id=?",
                 str(submission_id))
    await edge_cache_delete(GALLERY_CACHE_KEY)
    return json_response({
        "ok": True,
        "team": await _team_payload(env, slug, account_bi, _config(row)),
    }, cache_control="no-store")


# --- gallery and Community Choice -------------------------------------------------

async def _unlocked_milestone_rows(env, slug):
    rows = await d1_all(
        env,
        "SELECT key,reward FROM hackathon_milestones "
        "WHERE hackathon_slug=? AND unlocked_at>0",
        slug)
    return [dict(r) for r in rows or []]


async def projects_handler(env, request):
    """``GET /api/hackathon/projects`` -- the public gallery.

    Only valid projects appear: a flagged one may point at a repository a
    visitor cannot open, and it becomes public the moment a judge clears it.
    """
    if method_name(request) not in ("GET", "HEAD"):
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            extra_headers={"allow": "GET, HEAD"})
    cached = await edge_cache_match(GALLERY_CACHE_KEY)
    if cached is not None:
        return cached
    row = await _public_campaign(env)
    if not row:
        return json_response({"ok": True, "projects": []},
                             cache_seconds=policy.BOARD_CACHE_SECONDS)
    slug = str(row.get("slug") or "")
    config = _config(row)
    unlocked = [r["key"] for r in await _unlocked_milestone_rows(env, slug)]
    votes_open = policy.voting_open(row.get("status"), unlocked)
    rows = await d1_all(
        env,
        "SELECT s.*,t.name AS team_name FROM hackathon_submissions s "
        "JOIN hackathon_teams t ON t.team_id=s.team_id "
        "WHERE s.hackathon_slug=? AND s.status='valid' "
        "ORDER BY CASE s.placement WHEN 'first' THEN 0 WHEN 'second' THEN 1 "
        "ELSE 2 END,s.track,s.submitted_at LIMIT ?",
        slug, GALLERY_LIMIT)
    projects = []
    for entry in rows or []:
        item = _submission_public(entry, config, votes_open=votes_open)
        item["team"] = str(entry.get("team_name") or "")
        projects.append(item)
    response = json_response({
        "ok": True,
        "projects": projects,
        "votingOpen": votes_open,
        "status": str(row.get("status") or ""),
    }, cache_seconds=policy.BOARD_CACHE_SECONDS)
    await edge_cache_put(GALLERY_CACHE_KEY, response)
    return response


async def _project(env, slug, submission_id, valid_only=True):
    if not slug or not policy.SUBMISSION_ID_RE.match(str(submission_id or "")):
        return None
    row = await d1_first(
        env,
        "SELECT s.*,t.name AS team_name FROM hackathon_submissions s "
        "JOIN hackathon_teams t ON t.team_id=s.team_id "
        "WHERE s.hackathon_slug=? AND s.submission_id=?",
        slug, str(submission_id))
    if not row or (valid_only and str(row.get("status") or "") != "valid"):
        return None
    return row


async def project_handler(env, request, submission_id):
    """``GET /api/hackathon/projects/<id>`` -- one public project."""
    if method_name(request) not in ("GET", "HEAD"):
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            extra_headers={"allow": "GET, HEAD"})
    row = await _public_campaign(env)
    if not row:
        return json_response({"error": "not_found"}, status=404)
    slug = str(row.get("slug") or "")
    project = await _project(env, slug, submission_id)
    if not project:
        return json_response({"error": "not_found"}, status=404)
    config = _config(row)
    members = await d1_all(
        env,
        "SELECT name FROM hackathon_team_members WHERE team_id=? "
        "ORDER BY joined_at LIMIT ?",
        str(project.get("team_id") or ""), policy.MAX_TEAM_MEMBERS)
    item = _submission_public(project, config)
    item["team"] = str(project.get("team_name") or "")
    item["members"] = [_name(m.get("name", "")) for m in members or []]
    return json_response({"ok": True, "project": item},
                         cache_seconds=policy.BOARD_CACHE_SECONDS)


async def vote_handler(env, request, submission_id):
    """``POST /api/hackathon/projects/<id>/vote`` -- one Community Choice vote.

    One vote per verified participant per track, which they may move. Not for
    a project their own team submitted, and only while Community Choice is
    open: after its milestone unlocks and before judging closes.
    """
    if method_name(request) != "POST":
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            extra_headers={"allow": "POST"})
    try:
        data = await bounded_json_request(request, BODY_MAX_BYTES) or {}
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    account_bi, _name_value = await _session_account(env, request, data)
    if not account_bi:
        return json_response({"error": "unauthorized"}, status=401,
                             cache_control="no-store")
    row = await _public_campaign(env)
    if not row:
        return json_response({"error": "not_found"}, status=404,
                             cache_control="no-store")
    slug = str(row.get("slug") or "")
    unlocked = [r["key"] for r in await _unlocked_milestone_rows(env, slug)]
    if not policy.voting_open(row.get("status"), unlocked):
        return json_response({"error": "voting_closed"}, status=409,
                             cache_control="no-store")
    participant = await _participant(env, slug, account_bi)
    if not participant or not int(participant.get("verified_at") or 0):
        return json_response({"error": "verified_participants_only"},
                             status=403, cache_control="no-store")
    project = await _project(env, slug, submission_id)
    if not project:
        return json_response({"error": "not_found"}, status=404,
                             cache_control="no-store")
    team, _members = await _team_of(env, slug, account_bi)
    if team and str(team.get("team_id")) == str(project.get("team_id")):
        return json_response({"error": "own_project"}, status=409,
                             cache_control="no-store")
    track = str(project.get("track") or "")
    previous = await d1_first(
        env,
        "SELECT submission_id FROM hackathon_votes "
        "WHERE hackathon_slug=? AND voter_bi=? AND track=?",
        slug, account_bi, track)
    now = _now()
    if previous and str(previous.get("submission_id")) == str(submission_id):
        return json_response({"ok": True, "moved": False},
                             cache_control="no-store")
    await d1_run(
        env,
        "INSERT INTO hackathon_votes (hackathon_slug,voter_bi,track,"
        "submission_id,voted_at) VALUES (?,?,?,?,?) "
        "ON CONFLICT(hackathon_slug,voter_bi,track) DO UPDATE SET "
        "submission_id=excluded.submission_id,voted_at=excluded.voted_at",
        slug, account_bi, track, str(submission_id), now)
    for target in {str(submission_id),
                   str((previous or {}).get("submission_id") or "")}:
        if not target:
            continue
        await d1_run(
            env,
            "UPDATE hackathon_submissions SET vote_count=(SELECT COUNT(*) "
            "FROM hackathon_votes WHERE submission_id=?) WHERE submission_id=?",
            target, target)
    await edge_cache_delete(GALLERY_CACHE_KEY)
    return json_response({"ok": True, "moved": bool(previous)},
                         cache_control="no-store")


# --- judging ----------------------------------------------------------------------

async def _is_judge(env, slug, account_bi):
    if not slug or not account_bi:
        return False
    return bool(await d1_first(
        env,
        "SELECT 1 AS one FROM hackathon_judges "
        "WHERE hackathon_slug=? AND account_bi=?",
        slug, account_bi))


def _judging_open(row, now):
    status = str(row.get("status") or "")
    ends = int(row.get("ends_at") or 0)
    return status == "judging" or (
        status == "live" and ends and int(now) > ends)


async def judging_handler(env, request, submission_id=""):
    """``GET /api/hackathon/judging`` and ``POST /api/hackathon/judging/<id>``.

    Judges see every project that is valid or waiting for review, with their
    own scores. A judge who is on a project's team cannot score it.
    """
    method = method_name(request)
    if method not in ("GET", "POST"):
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            extra_headers={"allow": "GET, POST"})
    data = {}
    if method == "POST":
        try:
            data = await bounded_json_request(request, BODY_MAX_BYTES) or {}
        except Exception:
            return json_response({"error": "invalid_json"}, status=400)
    account_bi, _name_value = await _session_account(env, request, data)
    if not account_bi:
        return json_response({"error": "unauthorized"}, status=401,
                             cache_control="no-store")
    row = await _public_campaign(env)
    if not row:
        return json_response({"error": "not_found"}, status=404,
                             cache_control="no-store")
    slug = str(row.get("slug") or "")
    if not await _is_judge(env, slug, account_bi):
        return json_response({"error": "judges_only"}, status=403,
                             cache_control="no-store")
    config = _config(row)
    now = _now()
    if method == "POST":
        if not _judging_open(row, now):
            return json_response({"error": "judging_closed"}, status=409,
                                 cache_control="no-store")
        project = await _project(env, slug, submission_id, valid_only=False)
        if not project or str(project.get("status") or "") not in (
                "valid", "needs_review"):
            return json_response({"error": "not_found"}, status=404,
                                 cache_control="no-store")
        team, _members = await _team_of(env, slug, account_bi)
        if team and str(team.get("team_id")) == str(project.get("team_id")):
            return json_response({"error": "conflict_of_interest"},
                                 status=409, cache_control="no-store")
        scores, error = policy.normalize_score(data)
        if error:
            return json_response({"error": error}, status=400,
                                 cache_control="no-store")
        await d1_run(
            env,
            "INSERT INTO hackathon_scores (submission_id,judge_bi,"
            "hackathon_slug,fit,working,portability,craft,total,scored_at) "
            "VALUES (?,?,?,?,?,?,?,?,?) "
            "ON CONFLICT(submission_id,judge_bi) DO UPDATE SET "
            "fit=excluded.fit,working=excluded.working,"
            "portability=excluded.portability,craft=excluded.craft,"
            "total=excluded.total,scored_at=excluded.scored_at",
            str(submission_id), account_bi, slug, scores["fit"],
            scores["working"], scores["portability"], scores["craft"],
            scores["total"], now)
        # The cached sum and count are recomputed from the scores themselves,
        # so a revised score replaces the old one instead of adding to it.
        await d1_run(
            env,
            "UPDATE hackathon_submissions SET "
            "score_total=(SELECT COALESCE(SUM(total),0) FROM hackathon_scores "
            "WHERE submission_id=?),score_count=(SELECT COUNT(*) FROM "
            "hackathon_scores WHERE submission_id=?) WHERE submission_id=?",
            str(submission_id), str(submission_id), str(submission_id))
    rows = await d1_all(
        env,
        "SELECT s.*,t.name AS team_name,j.fit,j.working,j.portability,"
        "j.craft,j.total FROM hackathon_submissions s "
        "JOIN hackathon_teams t ON t.team_id=s.team_id "
        "LEFT JOIN hackathon_scores j ON j.submission_id=s.submission_id "
        "AND j.judge_bi=? WHERE s.hackathon_slug=? "
        "AND s.status IN ('valid','needs_review') "
        "ORDER BY s.track,s.submitted_at LIMIT ?",
        account_bi, slug, GALLERY_LIMIT)
    projects = []
    for entry in rows or []:
        item = _submission_public(entry, config, include_review=True)
        item["team"] = str(entry.get("team_name") or "")
        item["yourScore"] = (
            {key: int(entry.get(key) or 0)
             for key, _label in policy.SCORE_CRITERIA}
            if entry.get("total") is not None else None)
        item["judges"] = max(0, int(entry.get("score_count") or 0))
        projects.append(item)
    return json_response({
        "ok": True,
        "open": bool(_judging_open(row, now)),
        "criteria": [{"key": k, "label": label}
                     for k, label in policy.SCORE_CRITERIA],
        "projects": projects,
    }, cache_control="no-store")


async def finalize_results(env, slug, actor=""):
    """Decide placements and grant every prize the published table owes.

    Idempotent: placements are recomputed from the same scores, and every
    grant goes through the reward ledger, so running it twice changes
    nothing. Cash, the interview and featured placements are left to people.
    """
    row = await _campaign(env, slug)
    if not row:
        return None, "not_found"
    if str(row.get("status") or "") not in ("judging", "closed"):
        return None, "not_judging"
    config = _config(row)
    submissions = [dict(r) for r in await d1_all(
        env,
        "SELECT submission_id,team_id,track,status,score_total,score_count,"
        "submitted_at FROM hackathon_submissions WHERE hackathon_slug=?",
        slug) or []]
    placed = policy.placements(submissions)
    now = _now()
    await d1_run(
        env,
        "UPDATE hackathon_submissions SET placement='' "
        "WHERE hackathon_slug=?", slug)
    for submission_id, label in placed.items():
        await d1_run(
            env,
            "UPDATE hackathon_submissions SET placement=? WHERE submission_id=?",
            label, submission_id)
    members = {}
    names = {}
    for entry in await d1_all(
            env,
            "SELECT team_id,account_bi,name FROM hackathon_team_members "
            "WHERE hackathon_slug=?", slug) or []:
        members.setdefault(str(entry.get("team_id")), []).append(
            str(entry.get("account_bi")))
        names[str(entry.get("account_bi"))] = _name(entry.get("name", ""))
    recruiters = []
    for entry in await d1_all(
            env,
            "SELECT account_bi,name,referrals_qualified FROM "
            "hackathon_participants WHERE hackathon_slug=? "
            "AND referrals_qualified>0 ORDER BY referrals_qualified DESC,"
            "points DESC,joined_at LIMIT 3", slug) or []:
        recruiters.append({
            "account_bi": str(entry.get("account_bi")),
            "qualified": int(entry.get("referrals_qualified") or 0)})
        names[str(entry.get("account_bi"))] = _name(entry.get("name", ""))
    plan = policy.prize_grants(
        config, submissions, members, recruiters,
        await _unlocked_milestone_rows(env, slug))
    by_account = {}
    for grant in plan:
        by_account.setdefault(grant["account_bi"], []).append(grant)
    granted = 0
    for account_bi, grants in by_account.items():
        granted += len(await _grant_rewards(
            env, slug, account_bi, names.get(account_bi, ""), grants, now))
    await d1_run(
        env,
        "UPDATE hackathons SET status='closed',updated_at=? WHERE slug=?",
        now, slug)
    await edge_cache_delete(GALLERY_CACHE_KEY)
    await edge_cache_delete(CAMPAIGN_CACHE_KEY)
    return {"placements": placed, "planned": len(plan),
            "granted": granted}, ""


async def _admin_action(env, actor, slug, action, data):
    """Judges, submission review, ambassadors and results, for administrators.

    Every action is audited. None of them can pay cash: finalizing results
    grants membership months and badges through the reward ledger, and the
    cash, interview and featured placements stay with people.
    """
    row = await _campaign(env, slug)
    if not row:
        return None, "not_found", 404
    now = _now()
    result = {"slug": slug, "action": action}
    if action in ("add_judge", "remove_judge"):
        judge = _name(data.get("name"))
        if not valid_node_name(judge):
            return None, "invalid_name", 400
        judge_bi, rec = await _account_row(env, judge)
        if not rec or _account_kind(rec) != "user":
            return None, "account_not_found", 404
        if action == "add_judge":
            await d1_run(
                env,
                "INSERT INTO hackathon_judges (hackathon_slug,account_bi,name,"
                "added_at) VALUES (?,?,?,?) ON CONFLICT DO NOTHING",
                slug, judge_bi, judge, now)
        else:
            await d1_run(
                env,
                "DELETE FROM hackathon_judges WHERE hackathon_slug=? "
                "AND account_bi=?", slug, judge_bi)
        result["judge"] = judge
    elif action == "review_submission":
        status = str(data.get("status") or "")
        if status not in ("valid", "needs_review", "invalid"):
            return None, "invalid_status", 400
        updated = await d1_first(
            env,
            "UPDATE hackathon_submissions SET status=?,updated_at=? "
            "WHERE hackathon_slug=? AND submission_id=? "
            "AND status<>'withdrawn' RETURNING submission_id",
            status, now, slug, str(data.get("submissionId") or ""))
        if not updated:
            return None, "not_found", 404
        await edge_cache_delete(GALLERY_CACHE_KEY)
        result["status"] = status
    elif action in ("approve_ambassador", "reject_ambassador"):
        who = _name(data.get("name"))
        who_bi = await blind_index(env, who)
        status = "approved" if action == "approve_ambassador" else "rejected"
        updated = await d1_first(
            env,
            "UPDATE hackathon_ambassadors SET status=?,reviewed_at=?,"
            "reviewed_by=? WHERE hackathon_slug=? AND account_bi=? "
            "RETURNING name",
            status, now, actor, slug, who_bi)
        if not updated:
            return None, "not_found", 404
        if status == "approved":
            await _award_badge(env, who, who_bi, "hackathon_ambassador",
                               "hackathon")
            try:
                await enqueue_notification(
                    env, who, REWARD_NOTIFICATION_KIND,
                    "You are a ForkMesh Hackathon ambassador",
                    body="Your poster, QR code and analytics are on your "
                         "hackathon HQ.",
                    href="/hackathon/me", source="hackathon:" + slug,
                    dedupe="hackathon:%s:ambassador" % slug)
            except Exception:
                pass
        result["ambassador"] = who
        result["status"] = status
    elif action == "finalize":
        outcome, error = await finalize_results(env, slug, actor)
        if error:
            return None, error, 409
        result.update(outcome)
    else:
        return None, "unknown_action", 400
    await _audit_sensitive_action(
        env, actor, "hackathon." + action, "hackathon", slug, "success",
        {k: v for k, v in result.items() if k in (
            "judge", "status", "ambassador", "planned", "granted")})
    return result, "", 200


# --- administrator page ----------------------------------------------------------
# /admin/hackathon: the growth dashboard and every control an administrator
# needs, server-rendered with no script. It is the Hackathon section of the
# unified admin and draws inside the shared admin shell (admin_shell.py), with
# the campaign's areas as tabs. Forms post back to the same URL and redirect
# to the tab they came from, so a refresh never repeats an action.

ADMIN_PAGE_PATH = "/admin/hackathon"
ADMIN_FORM_MAX_BYTES = 64 * 1024

ADMIN_NOTICES = {
    "saved": "Campaign saved.",
    "judge_added": "Judge added.",
    "judge_removed": "Judge removed.",
    "reviewed": "Submission review saved.",
    "ambassador_reviewed": "Ambassador application decided.",
    "finalized": "Results finalized and prizes granted.",
}

ADMIN_TABS = (
    ("growth", "Growth"),
    ("campaign", "Campaign"),
    ("judging", "Judges & results"),
    ("submissions", "Submissions"),
    ("ambassadors", "Ambassadors"),
)
ADMIN_TAB_KEYS = tuple(key for key, _label in ADMIN_TABS)
# Which tab each form returns to after its redirect.
ADMIN_FORM_TABS = {
    "configure": "campaign",
    "add_judge": "judging",
    "remove_judge": "judging",
    "finalize": "judging",
    "review_submission": "submissions",
    "approve_ambassador": "ambassadors",
    "reject_ambassador": "ambassadors",
}
_ADMIN_EXTRA_STYLE = """
.pitch{max-width:44ch;white-space:normal}
textarea.config{min-height:320px;font:12px/1.5 ui-monospace,SFMono-Regular,
  Menlo,Consolas,monospace}
.flags{color:var(--muted);font-size:12px}
"""


def _esc(value):
    return _html_escape(str("" if value is None else value))


def _pct(value):
    if value is None:
        return "&ndash;"
    return "%d%%" % round(float(value) * 100)


def _utc_input(ms):
    """``YYYY-MM-DDTHH:MM`` in UTC for a datetime-local field, or ``""``."""
    from datetime import datetime, timezone
    try:
        ms = int(ms or 0)
    except (TypeError, ValueError):
        ms = 0
    if not ms:
        return ""
    return datetime.fromtimestamp(ms / 1000, tz=timezone.utc).strftime(
        "%Y-%m-%dT%H:%M")


def _utc_ms(value):
    from datetime import datetime, timezone
    text = str(value or "").strip()
    if not text:
        return 0
    try:
        stamp = datetime.strptime(text[:16], "%Y-%m-%dT%H:%M")
    except ValueError:
        return 0
    return int(stamp.replace(tzinfo=timezone.utc).timestamp() * 1000)


async def growth_report(env, row):
    """Every number the growth dashboard shows, from bounded aggregate reads."""
    slug = str(row.get("slug") or "")
    since = max(0, int(row.get("starts_at") or row.get("created_at") or 0))

    async def scalar(sql, *args):
        found = await d1_first(env, sql, *args)
        return int((found or {}).get("n") or 0)

    counts = {
        "participants": int(row.get("participant_count") or 0),
        "sharers": await scalar(
            "SELECT COUNT(DISTINCT account_bi) AS n FROM "
            "hackathon_share_events WHERE hackathon_slug=?", slug),
        "shares": await scalar(
            "SELECT COUNT(*) AS n FROM hackathon_share_events "
            "WHERE hackathon_slug=?", slug),
        "clicks": await scalar(
            "SELECT COUNT(*) AS n FROM referral_click_audit a "
            "JOIN hackathon_participants p ON p.account_bi=a.referrer_bi "
            "AND p.hackathon_slug=? WHERE a.decision='accepted' "
            "AND a.first_seen_at>=?", slug, since),
        "referredJoins": await scalar(
            "SELECT COUNT(*) AS n FROM hackathon_participants "
            "WHERE hackathon_slug=? AND referrer_bi<>''", slug),
        "referredVerified": await scalar(
            "SELECT COUNT(*) AS n FROM hackathon_participants "
            "WHERE hackathon_slug=? AND referrer_bi<>'' AND verified_at>0",
            slug),
        "qualified": await scalar(
            "SELECT COALESCE(SUM(referrals_qualified),0) AS n FROM "
            "hackathon_participants WHERE hackathon_slug=?", slug),
        "activated": await scalar(
            "SELECT COUNT(DISTINCT account_bi) AS n FROM hackathon_points "
            "WHERE hackathon_slug=? AND kind IN "
            "('first_mirror','pull_accepted','submission')", slug),
    }
    channels = [dict(r) for r in await d1_all(
        env,
        "SELECT channel,COUNT(*) AS presses,COUNT(DISTINCT account_bi) AS "
        "people FROM hackathon_share_events WHERE hackathon_slug=? "
        "GROUP BY channel ORDER BY presses DESC LIMIT 20", slug) or []]
    surfaces = [dict(r) for r in await d1_all(
        env,
        "SELECT surface,COUNT(*) AS presses FROM hackathon_share_events "
        "WHERE hackathon_slug=? GROUP BY surface ORDER BY presses DESC", slug)
        or []]
    now = _now()
    days = [dict(r) for r in await d1_all(
        env,
        "SELECT joined_at/86400000 AS day,COUNT(*) AS joins,"
        "SUM(CASE WHEN referrer_bi<>'' THEN 1 ELSE 0 END) AS referred "
        "FROM hackathon_participants WHERE hackathon_slug=? AND joined_at>=? "
        "GROUP BY day ORDER BY day", slug, now - 14 * policy.DAY_MS) or []]
    recruiters = [dict(r) for r in await d1_all(
        env,
        "SELECT account_bi,name,referrals_joined,referrals_qualified,"
        "share_count FROM hackathon_participants WHERE hackathon_slug=? "
        "AND referrals_joined>0 ORDER BY referrals_qualified DESC,"
        "referrals_joined DESC LIMIT 20", slug) or []]
    if recruiters:
        decisions = await d1_all(
            env,
            "SELECT referrer_bi,decision,COUNT(*) AS n FROM "
            "referral_attributions WHERE created_at>=? AND referrer_bi IN ("
            + ",".join("?" for _ in recruiters) + ") "
            "GROUP BY referrer_bi,decision",
            since, *[r["account_bi"] for r in recruiters])
        quality = {}
        for entry in decisions or []:
            quality.setdefault(str(entry.get("referrer_bi")), {})[
                str(entry.get("decision"))] = int(entry.get("n") or 0)
        for recruiter in recruiters:
            recruiter["quality"] = quality.get(recruiter["account_bi"], {})
    return {
        "funnel": policy.growth_funnel(counts),
        "channels": channels,
        "surfaces": surfaces,
        "days": days,
        "recruiters": recruiters,
    }


def _admin_page(row, campaigns, report, judges, submissions, ambassadors,
                notice="", error="", tab="growth", admin="", console="",
                theme="system", badges=None, page=ADMIN_PAGE_PATH):
    shell = _admin_shell
    slug = str((row or {}).get("slug") or "")
    tab = tab if (row and tab in ADMIN_TAB_KEYS) else (
        "growth" if row else "campaign")
    status_tone = {"live": "ok", "registration": "info", "judging": "warn",
                   "closed": "done"}
    switcher = []
    for campaign in campaigns:
        campaign_slug = str(campaign.get("slug") or "")
        campaign_status = str(campaign.get("status") or "")
        switcher.append(
            "<a class=\"camp%s\" href=\"%s?slug=%s\"%s>%s %s</a>" % (
                " on" if campaign_slug == slug else "", page,
                _esc(quote(campaign_slug)),
                " aria-current=\"page\"" if campaign_slug == slug else "",
                _esc(campaign_slug),
                shell.pill(campaign_status,
                           status_tone.get(campaign_status, "line"))))
    parts = [
        shell.page_head(
            "Hackathon",
            "Growth, configuration, judging, submissions and ambassadors for "
            "each campaign.", crumb="Growth",
            actions=("<a class=\"btn btn-primary\" href=\"%s?slug=new\">%sNew "
                     "campaign</a>" % (page, shell.icon("plus")))),
        "<nav class=\"campaigns\" aria-label=\"Campaigns\">%s</nav>"
        % "".join(switcher) if switcher else "",
    ]
    if notice:
        parts.append(shell.banner(notice, "ok"))
    if error:
        parts.append(shell.banner(error, "bad"))
    if not row:
        parts.append(_admin_campaign_form(row, page))
        return _admin_render(shell, "".join(parts), admin, console, theme,
                             badges, "%s?slug=new" % page, page)

    def tab_href(key):
        return "%s?slug=%s&amp;tab=%s" % (page, _esc(quote(slug)),
                                          key)

    to_review = sum(1 for entry in submissions
                    if str(entry.get("status") or "") == "needs_review")
    pending = sum(1 for entry in ambassadors
                  if str(entry.get("status") or "") == "pending")
    counts = {"judging": len(judges) or None,
              "submissions": ("%d to review" % to_review) if to_review else (
                  len(submissions) or None),
              "ambassadors": ("%d pending" % pending) if pending else None}
    parts.append(shell.tabs(
        [(key, label, tab_href(key), counts.get(key))
         for key, label in ADMIN_TABS], tab))
    if tab == "growth" and report:
        parts.append(_admin_growth(shell, row, report))
    elif tab == "campaign":
        parts.append(_admin_campaign_form(row, page))
    elif tab == "judging":
        parts.append(_admin_judging(shell, row, judges, page))
    elif tab == "submissions":
        parts.append(_admin_submissions(shell, row, submissions, page))
    elif tab == "ambassadors":
        parts.append(_admin_ambassadors(shell, row, ambassadors, page))
    return _admin_render(
        shell, "".join(parts), admin, console, theme, badges,
        "%s?slug=%s&tab=%s" % (page, quote(slug), tab), page)


def _admin_render(shell, content, admin, console, theme, badges, next_path,
                  page=ADMIN_PAGE_PATH):
    return shell.render_page(
        "Hackathon", content, active="hackathon", console=console,
        theme=theme, badges=badges, account=admin, next_path=next_path,
        subpages=page.rsplit("/", 1)[0],
        head_extra="<style>" + _ADMIN_EXTRA_STYLE + "</style>")


def _admin_hidden(slug):
    return "<input type=\"hidden\" name=\"slug\" value=\"%s\">" % _esc(slug)


def _admin_growth(shell, row, report):
    from datetime import datetime, timezone
    funnel = report["funnel"]
    parts = ["<div class=\"grid cols-4\">"]
    for label, value, hint in (
            ("K-factor", "%.2f" % funnel["kFactor"],
             "Referred joins per participant"),
            ("Invitations per participant",
             "%.2f" % funnel["invitesPerParticipant"],
             "Share presses: intent, not messages sent"),
            ("Invitation conversion", _pct(funnel["invitationConversion"]),
             "Referred joins per share press"),
            ("Participants", "{:,}".format(
                int((row or {}).get("participant_count") or 0)),
             "{:,} submissions".format(
                 int((row or {}).get("submission_count") or 0)))):
        parts.append(
            "<div class=\"card kpi\"><div class=\"l\">%s</div>"
            "<div class=\"n\">%s</div><div class=\"w\">%s</div></div>"
            % (_esc(label), value, _esc(hint)))
    parts.append("</div>")
    top = max([s["value"] for s in funnel["stages"]] + [1])
    stage_rows = [
        "<tr><td>%s</td><td class=\"r\"><b>%s</b></td><td>%s</td>"
        "<td class=\"r muted\">%s</td></tr>" % (
            _esc(stage["label"]), "{:,}".format(stage["value"]),
            shell.hbar(max(1, round(stage["value"] * 100 / top))),
            _pct(stage["fromPrevious"]))
        for stage in funnel["stages"]]
    funnel_card = shell.card(
        title="Viral funnel",
        description=("Each stage, and its share of the stage before it where "
                     "it is a subset of that stage."),
        table=shell.table(
            "<th>Stage</th><th class=\"r\">Count</th>"
            "<th style=\"width:34%\"></th><th class=\"r\">Of previous</th>",
            stage_rows))

    now_day = _now() // policy.DAY_MS
    by_day = {int(entry.get("day") or 0): entry for entry in report["days"]}
    labels, joins, referred = [], [], []
    for day in range(now_day - 13, now_day + 1):
        moment = datetime.fromtimestamp(day * 86400, tz=timezone.utc)
        labels.append(moment.strftime("%b %d"))
        entry = by_day.get(day) or {}
        joins.append(int(entry.get("joins") or 0))
        referred.append(int(entry.get("referred") or 0))
    if report["days"]:
        joins_body = (
            shell.line_chart(labels, [(joins, "accent"), (referred, "accent")],
                             height=190, bars=True,
                             title="Joins per day, last 14 days")
            + "<div class=\"legend\"><span><i class=\"soft\"></i>All joins"
            "</span><span><i class=\"box\"></i>Referred</span></div>")
    else:
        joins_body = shell.empty("No joins in the last 14 days.")
    joins_card = shell.card(
        joins_body, title="Joins, last 14 days",
        description="UTC days. Referred means through somebody's link.")
    channel_rows = [
        "<tr><td>%s</td><td class=\"r\">%s</td><td class=\"r\">%s</td></tr>"
        % (_esc(entry.get("channel")),
           "{:,}".format(int(entry.get("presses") or 0)),
           "{:,}".format(int(entry.get("people") or 0)))
        for entry in report["channels"]]
    channels_card = shell.card(
        title="Share channels",
        description="Presses by channel, and how many people pressed.",
        table=shell.table(
            "<th>Channel</th><th class=\"r\">Presses</th>"
            "<th class=\"r\">People</th>", channel_rows, "No shares yet."))
    recruiter_rows = []
    for entry in report["recruiters"]:
        quality = entry.get("quality") or {}
        rejected = int(quality.get("rejected") or 0)
        recruiter_rows.append(
            "<tr><td>%s</td><td class=\"r\">%s</td><td class=\"r\">%s</td>"
            "<td class=\"r\">%s</td><td class=\"r%s\">%s</td>"
            "<td class=\"r\">%s</td></tr>" % (
                shell.who(str(entry.get("name") or "")),
                int(entry.get("referrals_joined") or 0),
                int(quality.get("credited") or 0),
                int(quality.get("pending") or 0),
                " s5" if rejected else "", rejected,
                int(entry.get("share_count") or 0)))
    recruiters_card = shell.card(
        title="Recruiters",
        description=("Referral quality from the attribution ledger: credited, "
                     "pending review and rejected."),
        table=shell.table(
            "<th>Participant</th><th class=\"r\">Joined</th>"
            "<th class=\"r\">Credited</th><th class=\"r\">Pending</th>"
            "<th class=\"r\">Rejected</th><th class=\"r\">Shares</th>",
            recruiter_rows, "Nobody has brought anyone in yet."))
    return (
        "".join(parts)
        + "<div class=\"grid split\">" + funnel_card
        + "<div class=\"stack\">" + joins_card + channels_card + "</div></div>"
        + recruiters_card)


def _admin_campaign_form(row, page=ADMIN_PAGE_PATH):
    shell = _admin_shell
    slug = str((row or {}).get("slug") or "")
    config = _config(row) if row else policy.default_config()
    statuses = "".join(
        "<option value=\"%s\"%s>%s</option>" % (
            status, " selected" if status == str((row or {}).get("status")
                                                 or "draft") else "",
            status)
        for status in policy.STATUSES)
    return (
        "<section class=\"card\" style=\"max-width:920px\"><div class=\"card-h\">"
        "<div><h2>%s</h2><p>A draft is invisible on every public route. Times "
        "are UTC. The configuration is validated before anything is saved; an "
        "unknown reward or out-of-range threshold is refused, never clamped."
        "</p></div></div>"
        "<form class=\"card-b\" method=\"post\" action=\"%s\">"
        "<input type=\"hidden\" name=\"form\" value=\"configure\">"
        "<div class=\"form-grid\">"
        "<div class=\"field\"><label for=\"hk-slug\">Slug</label>"
        "<input id=\"hk-slug\" class=\"mono\" name=\"slug\" required "
        "pattern=\"[a-z][a-z0-9-]{1,39}\" value=\"%s\"%s></div>"
        "<div class=\"field\"><label for=\"hk-status\">Status</label>"
        "<select id=\"hk-status\" name=\"status\">%s</select></div>"
        "<div class=\"field wide\"><label for=\"hk-title\">Title</label>"
        "<input id=\"hk-title\" name=\"title\" value=\"%s\" maxlength=\"120\">"
        "</div>"
        "<div class=\"field\"><label for=\"hk-start\">Starts (UTC)</label>"
        "<input id=\"hk-start\" type=\"datetime-local\" name=\"startsAt\" "
        "value=\"%s\"></div>"
        "<div class=\"field\"><label for=\"hk-end\">Submissions close (UTC)"
        "</label><input id=\"hk-end\" type=\"datetime-local\" name=\"endsAt\" "
        "value=\"%s\"></div>"
        "<div class=\"field\"><label for=\"hk-judging\">Judging ends (UTC)"
        "</label><input id=\"hk-judging\" type=\"datetime-local\" "
        "name=\"judgingEndsAt\" value=\"%s\"></div>"
        "<div class=\"field wide\"><label for=\"hk-config\">Configuration "
        "(JSON)</label><textarea id=\"hk-config\" class=\"config\" "
        "name=\"config\" spellcheck=\"false\">%s</textarea></div>"
        "<div class=\"wide form-row\" style=\"justify-content:flex-end\">"
        "<button class=\"btn btn-primary\" type=\"submit\">Save campaign"
        "</button></div></div></form></section>" % (
            "Campaign" if row else "New campaign", page,
            _esc(slug), " readonly" if row else "", statuses,
            _esc((row or {}).get("title") or "ForkMesh Hackathon"),
            _utc_input((row or {}).get("starts_at")),
            _utc_input((row or {}).get("ends_at")),
            _utc_input((row or {}).get("judging_ends_at")),
            _esc(json.dumps(config, indent=2))))


def _admin_judging(shell, row, judges, page):
    slug = str(row.get("slug") or "")
    hidden = _admin_hidden(slug)
    judge_rows = [
        "<tr><td>%s</td><td class=\"r\"><form class=\"inline\" "
        "method=\"post\" action=\"%s\">%s<input type=\"hidden\" "
        "name=\"form\" value=\"remove_judge\"><input type=\"hidden\""
        " name=\"name\" value=\"%s\"><button class=\"btn btn-sm btn-danger\" "
        "type=\"submit\">Remove</button></form></td></tr>" % (
            shell.who(str(judge.get("name") or "")), page, hidden,
            _esc(judge.get("name")))
        for judge in judges]
    judges_card = shell.card(
        title="Judges",
        description="Judges score from their hackathon HQ once submissions close.",
        table=("<table><tbody>%s</tbody></table>" % "".join(judge_rows)
               if judge_rows else ""),
        footer=(
            ("" if judge_rows else shell.empty("No judges yet."))
            + "<form class=\"card-b form-row\" method=\"post\" action=\"%s\">"
            "%s<input type=\"hidden\" name=\"form\" value=\"add_judge\">"
            "<label class=\"sr\" for=\"hk-judge\">Judge username</label>"
            "<input id=\"hk-judge\" class=\"grow\" name=\"name\" required "
            "placeholder=\"username\"><button class=\"btn btn-primary\" "
            "type=\"submit\">%sAdd judge</button></form>" % (
                page, hidden, shell.icon("plus"))))
    can_finalize = str(row.get("status") or "") in ("judging", "closed")
    results_card = shell.card(
        "<form method=\"post\" action=\"%s\">%s<input type=\"hidden\" "
        "name=\"form\" value=\"finalize\"><button class=\"btn btn-primary\" "
        "type=\"submit\"%s>Finalize results</button>%s</form>" % (
            page, hidden, "" if can_finalize else " disabled",
            "" if can_finalize else " <span class=\"muted\">Set the status "
            "to judging first.</span>"),
        title="Results",
        description=(
            "Ranks each track by average judge score, then more judges, then "
            "earlier submission; writes 1st and 2nd; closes the campaign; and "
            "grants the prize table as Pro months and badges. Safe to run "
            "again. Cash, interviews and featured placements are yours to do "
            "by hand."))
    return "<div class=\"grid cols-2\">" + judges_card + results_card + "</div>"


def _admin_submissions(shell, row, submissions, page):
    hidden = _admin_hidden(str(row.get("slug") or ""))
    tones = {"valid": "ok", "needs_review": "warn", "invalid": "bad"}
    rows = []
    for entry in submissions:
        try:
            flags = json.loads(entry.get("review_flags") or "[]")
        except ValueError:
            flags = []
        status = str(entry.get("status") or "")
        count = int(entry.get("score_count") or 0)
        average = ("%.1f" % (int(entry.get("score_total") or 0) / count)
                   if count else "&ndash;")
        options = "".join(
            "<option value=\"%s\"%s>%s</option>" % (
                value, " selected" if value == status else "", value)
            for value in ("valid", "needs_review", "invalid"))
        review = "" if status == "withdrawn" else (
            "<form class=\"form-row\" method=\"post\" action=\"%s\" "
            "style=\"justify-content:flex-end;flex-wrap:nowrap\">"
            "%s<input type=\"hidden\" name=\"form\" "
            "value=\"review_submission\"><input type=\"hidden\" "
            "name=\"submissionId\" value=\"%s\"><label class=\"sr\" "
            "for=\"hk-review-%s\">Review status</label><select id=\"hk-review-%s\" "
            "name=\"status\">%s</select><button class=\"btn btn-sm\" "
            "type=\"submit\">Save</button></form>" % (
                page, hidden, _esc(entry.get("submission_id")),
                _esc(entry.get("submission_id")),
                _esc(entry.get("submission_id")), options))
        rows.append(
            "<tr><td><a href=\"/%s/%s\">%s</a><div class=\"muted mono\">%s"
            "</div></td><td>%s</td><td>%s</td><td>%s%s</td><td class=\"r\">%s "
            "<span class=\"muted\">(%d)</span></td><td class=\"r\">%s</td>"
            "<td class=\"r\">%s</td></tr>"
            % (_esc(quote(str(entry.get("repo_owner") or ""))),
               _esc(quote(str(entry.get("repo_name") or ""))),
               _esc(entry.get("title")), _esc("%s/%s" % (
                   entry.get("repo_owner"), entry.get("repo_name"))),
               shell.pill(str(entry.get("track") or "—"), "line"),
               _esc(entry.get("team_name")),
               shell.pill(status.replace("_", " "), tones.get(status, "")),
               "".join("<div class=\"flags\">%s</div>" % _esc(flag)
                       for flag in flags or []),
               average, count, int(entry.get("vote_count") or 0), review))
    return shell.card(
        title="Submissions",
        description=("Flags are what the catalog check could not confirm. Only "
                     "valid projects are public, judged and eligible."),
        table=shell.table(
            "<th>Project</th><th>Track</th><th>Team</th><th>Status</th>"
            "<th class=\"r\">Score</th><th class=\"r\">Votes</th>"
            "<th class=\"r\">Review</th>", rows, "No submissions yet."))


def _admin_ambassadors(shell, row, ambassadors, page):
    hidden = _admin_hidden(str(row.get("slug") or ""))
    tones = {"approved": "ok", "rejected": "bad", "pending": "info"}
    rows = []
    for entry in ambassadors:
        status = str(entry.get("status") or "")
        decide = "".join(
            "<form class=\"inline\" method=\"post\" action=\"%s\">%s"
            "<input type=\"hidden\" name=\"form\" value=\"%s\">"
            "<input type=\"hidden\" name=\"name\" value=\"%s\">"
            "<button class=\"btn btn-sm%s\" type=\"submit\">%s</button></form>"
            % (page, hidden, action, _esc(entry.get("name")),
               " btn-primary" if action.startswith("approve") else " btn-danger",
               label)
            for action, label in (("reject_ambassador", "Reject"),
                                  ("approve_ambassador", "Approve"))
            if (action == "approve_ambassador" and status != "approved")
            or (action == "reject_ambassador" and status != "rejected"))
        rows.append(
            "<tr><td>%s</td><td class=\"pitch\">%s</td><td>%s</td>"
            "<td class=\"r\"><div class=\"form-row\" "
            "style=\"justify-content:flex-end\">%s</div></td></tr>" % (
                shell.who(str(entry.get("name") or "")),
                _esc(entry.get("pitch")),
                shell.pill(status, tones.get(status, "")), decide))
    return shell.card(
        title="Ambassadors",
        description=("Approval awards the Ambassador badge and unlocks the "
                     "poster and analytics on their HQ."),
        table=shell.table(
            "<th>Applicant</th><th>Pitch</th><th>Status</th>"
            "<th class=\"r\">Decide</th>", rows, "No applications yet."))


async def _admin_page_actor(env, request):
    """The signed-in administrator's name, or ``""``."""
    if not await _check_admin_page_auth(env, request):
        return ""
    return _name(await _account_cookie_name(env, request))


async def admin_page_handler(env, request):
    """``GET|POST /admin/hackathon`` -- the administrator page.

    Account-session and is_admin gated, checked live on every request like
    /admin/acquisition. POST requires a same-origin form, because the session
    here is a cookie; the action runs through exactly the functions the JSON
    API uses, then redirects so a refresh never repeats it.
    """
    method = method_name(request)
    headers = {
        "cache-control": "no-store, max-age=0, must-revalidate",
        "x-robots-tag": "noindex",
    }
    if method not in ("GET", "HEAD", "POST"):
        return Response("", status=405, headers=dict(headers, allow="GET, POST"))
    await ensure_schema(env)
    page = _admin_shell.subpage_base(env) + "/hackathon"
    actor = await _admin_page_actor(env, request)
    if not actor:
        if method == "POST":
            return Response("Administrator session required.", status=403,
                            headers=dict(headers, **{
                                "content-type": "text/plain; charset=utf-8"}))
        return Response("", status=302, headers=dict(headers, location=(
            "/login?next=" + quote(page))))
    params = parse_qs(urlparse(request.url).query)
    if method == "POST":
        if not _request_origin_allowed(env, request):
            return Response("Cross-origin form refused.", status=403,
                            headers=dict(headers, **{
                                "content-type": "text/plain; charset=utf-8"}))
        try:
            raw = await bounded_request_text(request, ADMIN_FORM_MAX_BYTES)
        except Exception:
            raw = ""
        fields = {key: values[-1] for key, values in clean_hidden_query(parse_qs(
            str(raw or ""), keep_blank_values=True)).items()}
        slug = str(fields.get("slug") or "").strip().lower()
        form = str(fields.get("form") or "")
        notice_key = ""
        if not SLUG_RE.match(slug):
            result, error, _status = None, "invalid_slug", 400
        elif form == "configure":
            try:
                config = json.loads(fields.get("config") or "{}")
            except ValueError:
                config = None
            if config is None:
                result, error, _status = None, "invalid_config", 400
            else:
                result, error, _status = await _admin_configure(
                    env, actor, slug, {
                        "title": fields.get("title"),
                        "status": fields.get("status"),
                        "config": config,
                        "startsAt": _utc_ms(fields.get("startsAt")),
                        "endsAt": _utc_ms(fields.get("endsAt")),
                        "judgingEndsAt": _utc_ms(fields.get("judgingEndsAt")),
                    })
                notice_key = "saved"
        elif form in ("add_judge", "remove_judge", "review_submission",
                      "approve_ambassador", "reject_ambassador", "finalize"):
            result, error, _status = await _admin_action(
                env, actor, slug, form, fields)
            notice_key = {
                "add_judge": "judge_added", "remove_judge": "judge_removed",
                "review_submission": "reviewed",
                "approve_ambassador": "ambassador_reviewed",
                "reject_ambassador": "ambassador_reviewed",
                "finalize": "finalized",
            }[form]
        else:
            result, error, _status = None, "unknown_action", 400
        target = "%s?slug=%s" % (page, quote(slug or "new"))
        if form in ADMIN_FORM_TABS and SLUG_RE.match(slug):
            target += "&tab=" + ADMIN_FORM_TABS[form]
        if error:
            detail = str((result or {}).get("detail") or "")
            target += "&error=" + quote((error + (": " + detail if detail
                                                  else ""))[:300])
        else:
            target += "&notice=" + quote(notice_key)
        return Response("", status=303, headers=dict(headers, location=target))

    campaigns = [dict(r) for r in await d1_all(
        env, "SELECT slug,status FROM hackathons ORDER BY created_at DESC "
        "LIMIT 20") or []]
    wanted = str((params.get("slug") or [""])[0]).strip().lower()
    row = None
    if wanted != "new":
        row = await _campaign(env, wanted) if wanted else None
        if row is None and not wanted and campaigns:
            row = await _campaign(env, campaigns[0]["slug"])
    notice = ADMIN_NOTICES.get(str((params.get("notice") or [""])[0]), "")
    error = str((params.get("error") or [""])[0])[:300]
    report = judges = submissions = ambassadors = None
    if row:
        slug = str(row.get("slug") or "")
        report = await growth_report(env, row)
        judges = [dict(r) for r in await d1_all(
            env, "SELECT name FROM hackathon_judges WHERE hackathon_slug=? "
            "ORDER BY name", slug) or []]
        submissions = [dict(r) for r in await d1_all(
            env,
            "SELECT s.*,t.name AS team_name FROM hackathon_submissions s "
            "JOIN hackathon_teams t ON t.team_id=s.team_id "
            "WHERE s.hackathon_slug=? ORDER BY s.track,s.submitted_at LIMIT ?",
            slug, GALLERY_LIMIT) or []]
        ambassadors = [dict(r) for r in await d1_all(
            env,
            "SELECT name,status,pitch,applied_at FROM hackathon_ambassadors "
            "WHERE hackathon_slug=? ORDER BY CASE status WHEN 'pending' "
            "THEN 0 ELSE 1 END,applied_at DESC LIMIT 100", slug) or []]
    shell = _admin_shell
    body = _admin_page(
        row, campaigns, report, judges or [], submissions or [],
        ambassadors or [], notice=notice, error=error,
        tab=str((params.get("tab") or ["growth"])[0]), admin=actor,
        console=shell.console_base(env),
        theme=shell.theme_from_request(request),
        badges=await shell.nav_badges(env), page=page)
    return Response(body, status=200, headers=dict(headers, **{
        "content-type": "text/html; charset=utf-8",
        "content-security-policy": (
            "default-src 'none'; img-src 'self'; style-src 'unsafe-inline'; "
            "form-action 'self'; frame-ancestors 'none'; base-uri 'none'"),
    }))


# --- ambassadors ------------------------------------------------------------------
# The street team. Recruiting fifty motivated people is human work; the product
# takes the application, lets an administrator decide it, and hands an approved
# ambassador the tools and the numbers. Nothing here contacts anybody.

async def _ambassador_payload(env, row, participant):
    slug = str(row.get("slug") or "")
    account_bi = str(participant.get("account_bi") or "")
    record = await d1_first(
        env,
        "SELECT status,pitch,applied_at,reviewed_at FROM hackathon_ambassadors "
        "WHERE hackathon_slug=? AND account_bi=?",
        slug, account_bi)
    if not record:
        return None
    out = {
        "status": str(record.get("status") or ""),
        "pitch": str(record.get("pitch") or ""),
        "appliedAt": max(0, int(record.get("applied_at") or 0)),
        "reviewedAt": max(0, int(record.get("reviewed_at") or 0)),
    }
    if out["status"] == "approved":
        # Their own funnel since the campaign opened: counted visits to their
        # link (the referral click audit's accepted rows, which already
        # exclude duplicates and crawlers), people who joined through them,
        # and members the referral ledger credited.
        since = max(0, int(row.get("starts_at") or row.get("created_at") or 0))
        clicks = await d1_first(
            env,
            "SELECT COUNT(*) AS n FROM referral_click_audit "
            "WHERE referrer_bi=? AND decision='accepted' AND first_seen_at>=?",
            account_bi, since)
        joins = await d1_first(
            env,
            "SELECT COUNT(*) AS n FROM hackathon_participants "
            "WHERE hackathon_slug=? AND referrer_bi=?",
            slug, account_bi)
        out["stats"] = policy.ambassador_stats(
            int((clicks or {}).get("n") or 0),
            int((joins or {}).get("n") or 0),
            int(participant.get("referrals_qualified") or 0))
    return out


async def ambassadors_handler(env, request):
    """``POST /api/hackathon/ambassadors`` -- apply to the street team."""
    if method_name(request) != "POST":
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            extra_headers={"allow": "POST"})
    try:
        data = await bounded_json_request(request, BODY_MAX_BYTES) or {}
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    account_bi, name = await _session_account(env, request, data)
    if not account_bi:
        return json_response({"error": "unauthorized"}, status=401,
                             cache_control="no-store")
    row = await _public_campaign(env)
    if not row:
        return json_response({"error": "not_found"}, status=404,
                             cache_control="no-store")
    slug = str(row.get("slug") or "")
    participant = await _participant(env, slug, account_bi)
    if not participant:
        return json_response({"error": "not_joined"}, status=409,
                             cache_control="no-store")
    if not int(participant.get("verified_at") or 0):
        return json_response({"error": "verify_email_first"}, status=403,
                             cache_control="no-store")
    if str(row.get("status") or "") not in policy.JOINABLE_STATUSES:
        return json_response({"error": "applications_closed"}, status=409,
                             cache_control="no-store")
    pitch = policy.normalize_ambassador_pitch(data.get("pitch"))
    if len(pitch) < 20:
        return json_response({"error": "pitch_too_short"}, status=400,
                             cache_control="no-store")
    existing = await d1_first(
        env,
        "SELECT status FROM hackathon_ambassadors "
        "WHERE hackathon_slug=? AND account_bi=?",
        slug, account_bi)
    if existing and str(existing.get("status")) == "approved":
        return json_response({"error": "already_an_ambassador"}, status=409,
                             cache_control="no-store")
    await d1_run(
        env,
        "INSERT INTO hackathon_ambassadors (hackathon_slug,account_bi,name,"
        "status,pitch,applied_at,reviewed_at,reviewed_by) "
        "VALUES (?,?,?,'pending',?,?,0,'') "
        "ON CONFLICT(hackathon_slug,account_bi) DO UPDATE SET "
        "status='pending',pitch=excluded.pitch,applied_at=excluded.applied_at,"
        "reviewed_at=0,reviewed_by=''",
        slug, account_bi, name, pitch, _now())
    return json_response({
        "ok": True,
        "ambassador": await _ambassador_payload(env, row, participant),
    }, status=201, cache_control="no-store")


# --- hooks called by the rest of the Worker -----------------------------------

async def on_signup(env, name, account_bi, referrer_name=""):
    """Join a new account to the running campaign when a participant sent them.

    Somebody who followed a participant's invite link and created an account
    is in the hackathon: making them press Join again on arrival would lose
    exactly the people the invite page just earned. An account that arrives
    any other way is not joined silently -- they press Join themselves.

    Best effort by contract: this runs inside account creation, and a
    campaign bookkeeping problem must never fail an otherwise good signup.
    """
    try:
        referrer_name = _name(referrer_name)
        if not referrer_name:
            return ""
        row = await _campaign(env)
        if not row or str(row.get("status") or "") not in (
                policy.JOINABLE_STATUSES):
            return ""
        slug = str(row.get("slug") or "")
        referrer = await _participant_by_name(env, slug, referrer_name)
        if not referrer:
            return ""
        created = await _create_participant(
            env, slug, account_bi, _name(name),
            referrer_bi=str(referrer.get("account_bi") or ""),
            referrer_name=referrer_name)
        return slug if created else ""
    except Exception:
        return ""


async def on_email_verified(env, account_bi, name=""):
    """Mark a participant verified and tell whoever invited them.

    This is the moment a friend counts as *joined*: progress bars move, the
    invite mission advances and the referrer gets a notification. It is not
    the moment anything of value is granted -- that waits for the referral
    ledger to credit the referral, which happens later and on its own terms.
    """
    try:
        row = await _campaign(env)
        if not row:
            return False
        slug = str(row.get("slug") or "")
        participant = await _participant(env, slug, account_bi)
        if not participant or int(participant.get("verified_at") or 0):
            return False
        now = _now()
        await d1_run(
            env,
            "UPDATE hackathon_participants SET verified_at=? "
            "WHERE hackathon_slug=? AND account_bi=? AND verified_at=0",
            now, slug, account_bi)
        await d1_run(
            env,
            "UPDATE hackathons SET verified_count=verified_count+1,"
            "updated_at=? WHERE slug=?",
            now, slug)
        await _check_milestones(env, slug, now)
        await _award_points(env, slug, account_bi, "verified", now=now)
        referrer_bi = str(participant.get("referrer_bi") or "")
        referrer_name = _name(participant.get("referrer_name", ""))
        if referrer_bi and referrer_name:
            await d1_run(
                env,
                "UPDATE hackathon_participants "
                "SET referrals_joined=referrals_joined+1 "
                "WHERE hackathon_slug=? AND account_bi=?",
                slug, referrer_bi)
            await _award_points(
                env, slug, referrer_bi, "referral_joined",
                ref=str(account_bi), now=now)
            await _notify_referrer(
                env, slug, referrer_bi, referrer_name,
                _name(name or participant.get("name", "")))
            await _evaluate_missions(env, row, referrer_bi)
        await edge_cache_delete(CAMPAIGN_CACHE_KEY)
        return True
    except Exception:
        return False


async def on_referral_credited(env, referrer_bi, referrer_name, referred_bi):
    """Award the qualified-referral points once the referral ledger credits.

    Called from the referral finalizer, so the gate is exactly the gate the
    public referral board already applies. Nothing here re-checks it.
    """
    try:
        row = await _campaign(env)
        if not row:
            return False
        slug = str(row.get("slug") or "")
        participant = await _participant(env, slug, referrer_bi)
        if not participant:
            return False
        now = _now()
        awarded = await _award_points(
            env, slug, referrer_bi, "referral_qualified",
            ref=str(referred_bi), now=now)
        if awarded:
            await d1_run(
                env,
                "UPDATE hackathon_participants "
                "SET referrals_qualified=referrals_qualified+1 "
                "WHERE hackathon_slug=? AND account_bi=?",
                slug, referrer_bi)
            await edge_cache_delete(BOARD_CACHE_KEY % "recruiters")
            # The recruiting missions that pay membership months read the
            # qualified count, so this is the moment they can complete.
            await _evaluate_missions(env, row, referrer_bi)
        return awarded
    except Exception:
        return False


async def on_first_mirror(env, account_bi, repo=""):
    """Builder points for an owner's first published repository."""
    return await _builder_points(env, account_bi, "first_mirror", repo)


async def on_pull_accepted(env, account_bi, reference=""):
    """Builder points for a pull accepted into a repository."""
    return await _builder_points(env, account_bi, "pull_accepted", reference)


async def _builder_points(env, account_bi, kind, reference):
    try:
        row = await _campaign(env)
        if not row or str(row.get("status") or "") not in (
                policy.PUBLIC_STATUSES):
            return False
        slug = str(row.get("slug") or "")
        if not await _participant(env, slug, account_bi):
            return False
        awarded = await _award_points(
            env, slug, account_bi, kind, ref=reference)
        if awarded:
            await _evaluate_missions(env, row, account_bi)
        return awarded
    except Exception:
        return False


async def _notify_referrer(env, slug, referrer_bi, referrer_name, joined_name):
    """Tell one participant that somebody they invited has arrived.

    Only the participant is told, and only about their own progress. The
    person who just joined is never told who invited them beyond what they
    already saw on the invite page they followed.
    """
    try:
        row = await d1_first(
            env,
            "SELECT referrals_joined FROM hackathon_participants "
            "WHERE hackathon_slug=? AND account_bi=?",
            slug, referrer_bi)
        joined = max(1, int((row or {}).get("referrals_joined") or 1))
        if joined < 3:
            title = "%d of 3 friends joined your hackathon squad" % joined
            body = ("%s just verified their ForkMesh account. One more "
                    "invite unlocks your next mission." % joined_name
                    if joined == 2 else
                    "%s just verified their ForkMesh account." % joined_name)
        else:
            title = "%d builders have joined through your link" % joined
            body = "%s just verified their ForkMesh account." % joined_name
        await enqueue_notification(
            env, referrer_name, "hackathon_squad", title, body=body,
            href="/hackathon/me", source="hackathon:" + slug,
            dedupe="hackathon:%s:joined:%d" % (slug, joined))
    except Exception:
        return


# --- public reads -------------------------------------------------------------

async def campaign_handler(env, request):
    """``GET /api/hackathon`` -- the landing page's whole data source.

    Edge-cached for a minute: the milestone meter is the hottest read in the
    feature and D1 bills rows read, so a burst of landing-page loads must
    collapse to one query per colo per TTL rather than one per visitor.
    """
    if method_name(request) not in ("GET", "HEAD"):
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            extra_headers={"allow": "GET, HEAD"})
    cached = await edge_cache_match(CAMPAIGN_CACHE_KEY)
    if cached is not None:
        return cached
    row = await _public_campaign(env)
    if not row:
        return json_response(
            {"ok": True, "campaign": None},
            cache_seconds=policy.CAMPAIGN_CACHE_SECONDS)
    slug = str(row.get("slug") or "")
    record = dict(row)
    record["config"] = _config(row)
    payload = policy.public_campaign(
        record, _counters(row), await _milestones(env, slug))
    response = json_response(
        {"ok": True, "campaign": payload},
        cache_seconds=policy.CAMPAIGN_CACHE_SECONDS)
    await edge_cache_put(CAMPAIGN_CACHE_KEY, response)
    return response


async def board_handler(env, request, board):
    """``GET /api/hackathon/leaderboards/<board>`` -- one public board.

    Fifty rows, one query per cache TTL. Public usernames only, exactly the
    trust level the existing referral board publishes; a pending referral is
    reported separately from a credited one so nobody reads an unqualified
    count as earned.
    """
    if method_name(request) not in ("GET", "HEAD"):
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            extra_headers={"allow": "GET, HEAD"})
    board = str(board or "").strip().lower()
    if board not in policy.BOARD_IDS:
        return json_response({"error": "not_found"}, status=404)
    cache_key = BOARD_CACHE_KEY % board
    cached = await edge_cache_match(cache_key)
    if cached is not None:
        return cached
    row = await _public_campaign(env)
    if not row:
        return json_response(
            {"ok": True, "board": board, "rows": []},
            cache_seconds=policy.BOARD_CACHE_SECONDS)
    slug = str(row.get("slug") or "")
    if board == "squads":
        rows = await d1_all(
            env,
            "SELECT code,owner_name,member_count,points FROM hackathon_squads "
            "WHERE hackathon_slug=? AND member_count>1 "
            "ORDER BY points DESC,member_count DESC,owner_name LIMIT ?",
            slug, policy.BOARD_LIMIT)
        entries = [{
            "code": str(entry.get("code") or ""),
            "name": _name(entry.get("owner_name", "")),
            "members": max(0, int(entry.get("member_count") or 0)),
            "points": max(0, int(entry.get("points") or 0)),
        } for entry in rows or []]
    elif board == "recruiters":
        rows = await d1_all(
            env,
            "SELECT name,referrals_qualified,referrals_joined,points "
            "FROM hackathon_participants WHERE hackathon_slug=? "
            "AND (referrals_qualified>0 OR referrals_joined>0) "
            "ORDER BY referrals_qualified DESC,points DESC,name LIMIT ?",
            slug, policy.BOARD_LIMIT)
        entries = [{
            "name": _name(entry.get("name", "")),
            "qualified": max(0, int(entry.get("referrals_qualified") or 0)),
            "joined": max(0, int(entry.get("referrals_joined") or 0)),
            "points": max(0, int(entry.get("points") or 0)),
        } for entry in rows or []]
    else:
        rows = await d1_all(
            env,
            "SELECT name,points,track FROM hackathon_participants "
            "WHERE hackathon_slug=? AND points>0 "
            "ORDER BY points DESC,joined_at,name LIMIT ?",
            slug, policy.BOARD_LIMIT)
        entries = [{
            "name": _name(entry.get("name", "")),
            "points": max(0, int(entry.get("points") or 0)),
            "track": str(entry.get("track") or ""),
        } for entry in rows or []]
    response = json_response(
        {"ok": True, "board": board, "rows": entries,
         "limit": policy.BOARD_LIMIT},
        cache_seconds=policy.BOARD_CACHE_SECONDS)
    await edge_cache_put(cache_key, response)
    return response


async def squad_handler(env, request, code):
    """``GET /api/hackathon/squads/<code>`` -- one squad's public page data."""
    if method_name(request) not in ("GET", "HEAD"):
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            extra_headers={"allow": "GET, HEAD"})
    row = await _public_campaign(env)
    if not row:
        return json_response({"error": "not_found"}, status=404)
    slug = str(row.get("slug") or "")
    squad = await _squad(env, slug, code)
    if not squad:
        return json_response({"error": "not_found"}, status=404)
    config = _config(row)
    members = await d1_all(
        env,
        "SELECT name,points FROM hackathon_participants "
        "WHERE hackathon_slug=? AND squad_id=? "
        "ORDER BY points DESC,joined_at LIMIT ?",
        slug, str(squad.get("squad_id") or ""), policy.BOARD_LIMIT)
    member_count = max(0, int(squad.get("member_count") or 0))
    goal, percent = policy.squad_goal(member_count, config)
    return json_response({
        "ok": True,
        "squad": {
            "code": str(squad.get("code") or ""),
            "owner": _name(squad.get("owner_name", "")),
            "members": member_count,
            "points": max(0, int(squad.get("points") or 0)),
            "goal": goal,
            "percent": percent,
            "roster": [{
                "name": _name(entry.get("name", "")),
                "points": max(0, int(entry.get("points") or 0)),
            } for entry in members or []],
        },
    }, cache_seconds=policy.BOARD_CACHE_SECONDS)


# --- the participant's own view ----------------------------------------------

async def me_handler(env, request):
    """``GET|POST /api/hackathon/me`` -- the participant HQ's data source.

    Never cached and never public: it carries one account's own standing.
    POST sets the track and advances onboarding; there is nothing else a
    browser may change, because everything else is earned.
    """
    method = method_name(request)
    if method not in ("GET", "POST"):
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            extra_headers={"allow": "GET, POST"})
    data = {}
    if method == "POST":
        try:
            data = await bounded_json_request(request, BODY_MAX_BYTES) or {}
        except Exception:
            return json_response({"error": "invalid_json"}, status=400)
        if not isinstance(data, dict):
            return json_response({"error": "invalid_json"}, status=400)
    account_bi, name = await _session_account(env, request, data)
    if not account_bi:
        return json_response({"error": "unauthorized"}, status=401,
                             cache_control="no-store")
    row = await _public_campaign(env)
    if not row:
        return json_response({"error": "not_found"}, status=404,
                             cache_control="no-store")
    slug = str(row.get("slug") or "")
    config = _config(row)
    participant = await _participant(env, slug, account_bi)
    if method == "POST" and participant:
        track = policy.valid_track(config, data.get("track"))
        if track and track != str(participant.get("track") or ""):
            await d1_run(
                env,
                "UPDATE hackathon_participants SET track=? "
                "WHERE hackathon_slug=? AND account_bi=?",
                track, slug, account_bi)
            participant = await _participant(env, slug, account_bi)
    if not participant:
        return json_response({
            "ok": True, "joined": False,
            "campaign": {"slug": slug, "status": str(row.get("status") or ""),
                         "joinable": str(row.get("status") or "")
                         in policy.JOINABLE_STATUSES},
        }, cache_control="no-store")
    return json_response(
        await _me_payload(env, request, row, config, participant, name),
        cache_control="no-store")


async def _participant_counters(env, slug, participant, config):
    """``(counters, squad)`` for one participant, as missions read them.

    The HQ and mission evaluation share this, so the number a progress bar
    shows is the number a reward is decided on.
    """
    account_bi = str(participant.get("account_bi") or "")
    name = _name(participant.get("name", ""))
    counters = {
        "referrals_joined": max(
            0, int(participant.get("referrals_joined") or 0)),
        "referrals_qualified": max(
            0, int(participant.get("referrals_qualified") or 0)),
        "shares": max(0, int(participant.get("share_count") or 0)),
        "mirrors": 0,
        "submissions": 0,
        "squad_size": 0,
    }
    squad = None
    squad_id = str(participant.get("squad_id") or "")
    if squad_id:
        squad_row = await d1_first(
            env,
            "SELECT code,owner_name,member_count,points FROM hackathon_squads "
            "WHERE squad_id=?",
            squad_id)
        if squad_row:
            members = max(0, int(squad_row.get("member_count") or 0))
            counters["squad_size"] = members
            goal, percent = policy.squad_goal(members, config)
            squad = {
                "code": str(squad_row.get("code") or ""),
                "owner": _name(squad_row.get("owner_name", "")),
                "members": members,
                "points": max(0, int(squad_row.get("points") or 0)),
                "goal": goal,
                "percent": percent,
                "yours": _name(squad_row.get("owner_name", "")) == name,
            }
    # Points earned through the unique kinds double as the building
    # counters, so a mission on "mirrors" reads the same ledger the points
    # came from instead of a second, driftable tally.
    earned = await d1_all(
        env,
        "SELECT kind,COUNT(*) AS n FROM hackathon_points "
        "WHERE hackathon_slug=? AND account_bi=? GROUP BY kind",
        slug, account_bi)
    for entry in earned or []:
        kind = str(entry.get("kind") or "")
        count = max(0, int(entry.get("n") or 0))
        if kind == "first_mirror":
            counters["mirrors"] = count
        elif kind == "submission":
            counters["submissions"] = count
    return counters, squad


async def _me_payload(env, request, row, config, participant, name):
    slug = str(row.get("slug") or "")
    account_bi = str(participant.get("account_bi") or "")
    points = max(0, int(participant.get("points") or 0))
    counters, squad = await _participant_counters(
        env, slug, participant, config)
    rewards = await _rewards_for(env, slug, account_bi)
    challenges = await _challenges_for(env, request, slug, account_bi, _now())
    team = await _team_payload(env, slug, account_bi, config)
    milestone_rows = await _milestones(env, slug)
    ambassador = await _ambassador_payload(env, row, participant)
    judge = await _is_judge(env, slug, account_bi)
    unlocked = [r["key"] for r in await _unlocked_milestone_rows(env, slug)]
    rank = await _rank(env, slug, points, account_bi) if points else 0
    total = max(0, int(row.get("participant_count") or 0))
    link = "%s/hackathon/join/%s" % (
        _site_origin(env, request), quote(name))
    text = policy.share_text(config.get("shareCopy"), {
        "rank": rank, "total": total, "points": points,
        "size": counters["squad_size"], "name": name,
    })
    return {
        "ok": True,
        "joined": True,
        "campaign": {
            "slug": slug,
            "title": str(row.get("title") or ""),
            "status": str(row.get("status") or ""),
            "theme": str(config.get("theme") or ""),
            "endsAt": int(row.get("ends_at") or 0),
            "tracks": [dict(track) for track in config.get("tracks") or ()],
            "participants": total,
        },
        "you": {
            "name": name,
            "points": points,
            "rank": rank,
            "track": str(participant.get("track") or ""),
            "referralsJoined": counters["referrals_joined"],
            "referralsQualified": counters["referrals_qualified"],
            "shares": counters["shares"],
            "joinedAt": int(participant.get("joined_at") or 0),
            "verified": bool(int(participant.get("verified_at") or 0)),
        },
        "squad": squad,
        "rewards": rewards,
        "challenges": challenges,
        "team": team,
        "judge": judge,
        "submissionsOpen": bool(_submissions_open(row, _now())),
        "teamsOpen": bool(_team_editable(row, _now())),
        "votingOpen": policy.voting_open(row.get("status"), unlocked),
        "milestones": policy.milestone_state(milestone_rows, _counters(row)),
        "ambassador": ambassador,
        "missions": policy.missions_state(config.get("missions"), counters),
        "nextMission": policy.next_incomplete_mission(
            config.get("missions"), counters),
        "onboarding": policy.onboarding_state(
            participant.get("onboarding_step"),
            participant.get("track"), counters),
        "share": {
            "link": link,
            "text": text,
            "channels": policy.share_channels(text, link),
            "card": "%s/api/hackathon/cards/invite/%s.png" % (
                _site_origin(env, request), quote(name)),
        },
    }


async def join_handler(env, request):
    """``POST /api/hackathon/join`` -- join the running campaign."""
    if method_name(request) != "POST":
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            extra_headers={"allow": "POST"})
    try:
        data = await bounded_json_request(request, BODY_MAX_BYTES) or {}
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    if not isinstance(data, dict):
        return json_response({"error": "invalid_json"}, status=400)
    account_bi, name = await _session_account(env, request, data)
    if not account_bi:
        return json_response({"error": "unauthorized"}, status=401,
                             cache_control="no-store")
    row = await _public_campaign(env)
    if not row:
        return json_response({"error": "not_found"}, status=404,
                             cache_control="no-store")
    if str(row.get("status") or "") not in policy.JOINABLE_STATUSES:
        return json_response({"error": "registration_closed"}, status=409,
                             cache_control="no-store")
    slug = str(row.get("slug") or "")
    config = _config(row)
    existing = await _participant(env, slug, account_bi)
    if not existing:
        # An explicit join still honours whoever's link brought this account
        # to ForkMesh in the first place, so pressing Join on the landing
        # page does not quietly orphan the invite that earned the signup.
        referrer_name, referrer_bi = await _signup_referrer(env, account_bi)
        if referrer_bi and not await _participant(env, slug, referrer_bi):
            referrer_bi, referrer_name = "", ""
        await _create_participant(
            env, slug, account_bi, name,
            referrer_bi=referrer_bi, referrer_name=referrer_name)
        _, rec = await _account_row(env, name)
        if rec and rec.get("email_verified") is True:
            await on_email_verified(env, account_bi, name)
        existing = await _participant(env, slug, account_bi)
    track = policy.valid_track(config, data.get("track"))
    if track and existing and not str(existing.get("track") or ""):
        await d1_run(
            env,
            "UPDATE hackathon_participants SET track=? "
            "WHERE hackathon_slug=? AND account_bi=?",
            track, slug, account_bi)
        existing = await _participant(env, slug, account_bi)
    return json_response(
        await _me_payload(env, request, row, config, existing, name),
        status=201, cache_control="no-store")


async def _signup_referrer(env, account_bi):
    """``(name, account_bi)`` of whoever's link brought this account here.

    Read from the referral attribution ledger regardless of its decision:
    squad placement is a cosmetic relationship, and making it wait for
    credit would leave a squad empty for a week. Credit itself is a separate
    question this function deliberately does not ask.
    """
    try:
        row = await d1_first(
            env,
            "SELECT referrer_bi,referrer_name FROM referral_attributions "
            "WHERE referred_bi=?",
            account_bi)
        name = _name((row or {}).get("referrer_name", ""))
        referrer_bi = str((row or {}).get("referrer_bi") or "")
        return (name, referrer_bi) if name and referrer_bi else ("", "")
    except Exception:
        return "", ""


async def share_handler(env, request):
    """``POST /api/hackathon/share`` -- record one share press.

    This is the K-factor numerator and nothing more. It stores the channel
    and the surface it was pressed on; it does not learn, store or transmit
    who the message went to, because the message never leaves the
    participant's own client.
    """
    if method_name(request) != "POST":
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            extra_headers={"allow": "POST"})
    try:
        data = await bounded_json_request(request, BODY_MAX_BYTES) or {}
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    if not isinstance(data, dict):
        return json_response({"error": "invalid_json"}, status=400)
    account_bi, _name_value = await _session_account(env, request, data)
    if not account_bi:
        return json_response({"error": "unauthorized"}, status=401,
                             cache_control="no-store")
    channel = str(data.get("channel") or "").strip().lower()[:32]
    surface = str(data.get("surface") or "hq").strip().lower()[:32]
    if channel not in policy.SHARE_CHANNEL_IDS:
        return json_response({"error": "unknown_channel"}, status=400,
                             cache_control="no-store")
    if surface not in policy.SHARE_SURFACES:
        surface = "hq"
    row = await _public_campaign(env)
    if not row:
        return json_response({"error": "not_found"}, status=404,
                             cache_control="no-store")
    slug = str(row.get("slug") or "")
    if not await _participant(env, slug, account_bi):
        return json_response({"error": "not_joined"}, status=409,
                             cache_control="no-store")
    now = _now()
    # One press per channel per minute. A jammed button must not inflate the
    # numerator of the one ratio this whole feature is measured by.
    landed = await d1_first(
        env,
        "INSERT INTO hackathon_share_events "
        "(hackathon_slug,account_bi,channel,surface,minute_bucket,created_at) "
        "VALUES (?,?,?,?,?,?) "
        "ON CONFLICT(hackathon_slug,account_bi,channel,minute_bucket) "
        "DO NOTHING RETURNING channel",
        slug, account_bi, channel, surface, now // 60000, now)
    if landed:
        await d1_run(
            env,
            "UPDATE hackathon_participants SET share_count=share_count+1 "
            "WHERE hackathon_slug=? AND account_bi=?",
            slug, account_bi)
    return json_response({"ok": True, "counted": bool(landed)},
                         cache_control="no-store")


async def api_handler(env, request, path):
    """Dispatch for the whole ``/api/hackathon`` subtree."""
    path = (str(path or "").rstrip("/") or PREFIX)
    if path == PREFIX:
        return await campaign_handler(env, request)
    if path == PREFIX + "/join":
        return await join_handler(env, request)
    if path == PREFIX + "/me":
        return await me_handler(env, request)
    if path == PREFIX + "/share":
        return await share_handler(env, request)
    if path == PREFIX + "/challenges":
        return await challenges_handler(env, request)
    accept = CHALLENGE_ACCEPT_RE.match(path)
    if accept:
        return await challenge_accept_handler(env, request, accept.group(1))
    team_action = TEAM_ACTION_RE.match(path)
    if team_action:
        return await teams_handler(env, request, team_action.group(1) or "")
    if path == PREFIX + "/submissions":
        return await submissions_handler(env, request)
    withdraw = SUBMISSION_WITHDRAW_RE.match(path)
    if withdraw:
        return await submission_withdraw_handler(
            env, request, withdraw.group(1))
    if path == PREFIX + "/ambassadors":
        return await ambassadors_handler(env, request)
    if path == PREFIX + "/projects":
        return await projects_handler(env, request)
    project = PROJECT_RE.match(path)
    if project:
        if project.group(2):
            return await vote_handler(env, request, project.group(1))
        return await project_handler(env, request, project.group(1))
    judging = JUDGING_RE.match(path)
    if judging:
        return await judging_handler(env, request, judging.group(1) or "")
    board = BOARD_PATH_RE.match(path + "/")
    if board:
        return await board_handler(env, request, board.group(1))
    if path.startswith(PREFIX + "/squads/"):
        return await squad_handler(
            env, request, unquote(path[len(PREFIX + "/squads/"):]))
    card = CARD_PATH_RE.match(path)
    if card:
        return await card_handler(
            env, request, card.group(1), unquote(card.group(2)))
    return json_response({"error": "not_found"}, status=404)


# --- personalised invite page -------------------------------------------------
# The brief's central mechanic: a friend does not land on "join our contest",
# they land on "Sarah invited you, she is #7, can you beat her?". That page
# has to be rendered by this Worker because it carries live numbers, and it
# is proxied by the marketing Worker so the URL a participant shares stays on
# the apex.
#
# It is deliberately one self-contained document with its own inline CSS
# rather than the composed landing page: it must render the invitation in a
# single request, with no script, no blocking bundle, and no second round
# trip before the person who was invited sees who invited them.

INVITE_STYLE = """
:root{color-scheme:light;--bg:#fff;--ink:#101012;--muted:#5f6270;
--line:#e4e5ea;--surface:#fff;--surface-2:#f4f4f6;--accent:#1a7f37;
--accent-ink:#fff;--ring:#0969da;
--body:ui-sans-serif,system-ui,-apple-system,"Segoe UI",sans-serif;
--display:ABCFavorit,Inter,ui-sans-serif,system-ui,sans-serif;
--serif:Domine,Georgia,serif;
--shadow:0 1px 2px rgba(16,16,18,.05),0 14px 36px -22px rgba(16,16,18,.3)}
html.dark{color-scheme:dark;--bg:#090909;--ink:#f5f5f5;--muted:#a3a3a3;
--line:#2a2a2e;--surface:#121214;--surface-2:#1b1b1f;--accent:#3fb950;
--accent-ink:#052e16;--ring:#58a6ff;
--shadow:0 1px 2px rgba(0,0,0,.5),0 16px 40px -22px rgba(0,0,0,.9)}
@font-face{font-family:Domine;src:url(/assets/fonts/Domine-Bold.woff2)
format("woff2");font-weight:700;font-display:swap}
@font-face{font-family:ABCFavorit;src:url(/assets/fonts/ABCFavorit-Bold.woff2)
format("woff2");font-weight:400 700;font-display:swap}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);font-family:var(--body);
-webkit-font-smoothing:antialiased}
a{color:inherit}
.wrap{max-width:60rem;margin:0 auto;padding:1.25rem 1.25rem 4rem}
.bar{display:flex;align-items:center;justify-content:space-between;
gap:1rem;padding:.65rem .9rem;border:1px solid var(--line);
border-radius:.875rem;background:var(--surface);box-shadow:var(--shadow)}
.brand{display:inline-flex;align-items:center;gap:.5rem;
text-decoration:none;font-family:var(--display);font-weight:700;
font-size:1.2rem;letter-spacing:-.03em}
.brand img{height:1.6rem;width:auto}
html.light .brand img{filter:invert(1) brightness(.2)}
.bar a.quiet{color:var(--muted);text-decoration:none;font-size:.9rem}
.bar a.quiet:hover{color:var(--ink)}
.hero{margin-top:clamp(2.5rem,6vw,4.5rem);text-align:center}
.kicker{display:inline-flex;align-items:center;gap:.5rem;
padding:.3rem .75rem;border:1px solid var(--line);border-radius:999px;
background:var(--surface-2);color:var(--muted);font-size:.8rem;
font-weight:600;letter-spacing:.02em}
.kicker b{color:var(--accent)}
h1{margin:1.25rem auto 0;max-width:18ch;font-family:var(--display);
font-weight:700;font-size:clamp(2.1rem,6vw,4rem);line-height:1.05;
letter-spacing:-.04em;text-wrap:balance}
.sub{margin:1.1rem auto 0;max-width:56ch;color:var(--muted);
font-size:clamp(1rem,1.5vw,1.2rem);line-height:1.55;text-wrap:balance}
.stats{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:.75rem;
max-width:34rem;margin:clamp(1.75rem,4vw,2.5rem) auto 0}
.stat{padding:.9rem .75rem;border:1px solid var(--line);
border-radius:.9rem;background:var(--surface)}
.stat .v{font-family:var(--serif);font-size:1.6rem;font-weight:700;
line-height:1.1;font-variant-numeric:tabular-nums}
.stat .k{margin-top:.2rem;color:var(--muted);font-size:.75rem;
font-weight:600;letter-spacing:.04em;text-transform:uppercase}
.cta{display:flex;flex-wrap:wrap;gap:.7rem;justify-content:center;
margin-top:clamp(1.75rem,4vw,2.5rem)}
.btn{display:inline-flex;align-items:center;justify-content:center;
min-height:2.9rem;padding:0 1.35rem;border-radius:.65rem;
font-size:.95rem;font-weight:600;text-decoration:none}
.btn.primary{background:var(--ink);color:var(--bg)}
html.dark .btn.primary{background:#e8e8e8;color:#1f1f1f}
.btn.ghost{border:1px solid var(--line);background:var(--surface);
color:var(--ink)}
.btn:focus-visible{outline:2px solid var(--ring);outline-offset:2px}
.note{margin-top:1rem;color:var(--muted);font-size:.8rem}
.cards{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));
gap:.9rem;margin-top:clamp(3rem,7vw,4.5rem)}
.card{padding:1.4rem;border:1px solid var(--line);border-radius:1.125rem;
background:var(--surface)}
.card h2{margin:0;font-family:var(--display);font-size:1rem;font-weight:700;
letter-spacing:-.01em}
.card p{margin:.5rem 0 0;color:var(--muted);font-size:.9rem;line-height:1.5}
.foot{margin-top:clamp(3rem,7vw,4.5rem);padding-top:1.25rem;
border-top:1px solid var(--line);color:var(--muted);font-size:.8rem;
display:flex;flex-wrap:wrap;gap:.5rem 1.25rem;justify-content:space-between}
.foot a{color:var(--muted)}
@media(max-width:720px){.stats,.cards{grid-template-columns:1fr}}
"""


def _invite_page(origin, campaign, config, inviter, stats, card_url):
    """One self-contained personalised invitation document."""
    name = _name(inviter)
    title = "%s invited you to the ForkMesh Hackathon" % name
    rank = int(stats.get("rank") or 0)
    points = int(stats.get("points") or 0)
    participants = int(stats.get("participants") or 0)
    if rank:
        summary = ("%s is #%s of %s builders with %s points. "
                   "Think you can beat them?" % (
                       name, "{:,}".format(rank),
                       "{:,}".format(participants), "{:,}".format(points)))
    else:
        summary = ("%s is building in the ForkMesh Hackathon and wants you "
                   "in it. %s builders have joined so far." % (
                       name, "{:,}".format(participants)))
    theme = str(config.get("theme") or "")
    tracks = list(config.get("tracks") or ())[:3]
    join_url = "/r/" + quote(name)
    share_url = "%s/hackathon/join/%s" % (origin, quote(name))
    cards = "".join(
        "<div class=\"card\"><h2>%s</h2><p>%s</p></div>" % (
            _html_escape(str(track.get("name") or "")),
            _html_escape(str(track.get("summary") or "")))
        for track in tracks)
    stat_cells = "".join(
        "<div class=\"stat\"><div class=\"v\">%s</div>"
        "<div class=\"k\">%s</div></div>" % (value, label)
        for value, label in (
            ("#" + "{:,}".format(rank) if rank else "New",
             "%s's rank" % _html_escape(name)),
            ("{:,}".format(points), "Their points"),
            ("{:,}".format(participants), "Builders so far"),
        ))
    return (
        "<!doctype html><html lang=\"en\" class=\"light\">"
        "<head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,"
        "initial-scale=1\">"
        "<title>%(title)s</title>"
        "<meta name=\"description\" content=\"%(summary)s\">"
        "<link rel=\"canonical\" href=\"%(share)s\">"
        "<link rel=\"icon\" href=\"/favicon/favicon.ico\" sizes=\"any\">"
        "<link rel=\"icon\" href=\"/favicon/favicon.svg\" type=\"image/svg+xml\">"
        "<meta property=\"og:type\" content=\"website\">"
        "<meta property=\"og:site_name\" content=\"ForkMesh\">"
        "<meta property=\"og:url\" content=\"%(share)s\">"
        "<meta property=\"og:title\" content=\"%(title)s\">"
        "<meta property=\"og:description\" content=\"%(summary)s\">"
        "<meta property=\"og:image\" content=\"%(card)s\">"
        "<meta property=\"og:image:width\" content=\"1200\">"
        "<meta property=\"og:image:height\" content=\"630\">"
        "<meta property=\"og:image:alt\" content=\"%(title)s\">"
        "<meta name=\"twitter:card\" content=\"summary_large_image\">"
        "<meta name=\"twitter:title\" content=\"%(title)s\">"
        "<meta name=\"twitter:description\" content=\"%(summary)s\">"
        "<meta name=\"twitter:image\" content=\"%(card)s\">"
        "<style>%(style)s</style></head><body><div class=\"wrap\">"
        "<div class=\"bar\">"
        "<a class=\"brand\" href=\"/\">"
        "<img src=\"/assets/logo.svg\" alt=\"\" width=\"208\" height=\"238\">"
        "forkmesh</a>"
        "<a class=\"quiet\" href=\"/hackathon\">About the hackathon &rarr;</a>"
        "</div>"
        "<div class=\"hero\">"
        "<span class=\"kicker\"><b>%(campaign)s</b> &middot; invitation</span>"
        "<h1>%(inviter)s invited you to build.</h1>"
        "<p class=\"sub\">%(summary)s</p>"
        "<div class=\"stats\">%(stats)s</div>"
        "<div class=\"cta\">"
        "<a class=\"btn primary\" href=\"%(join)s\">Accept the invitation</a>"
        "<a class=\"btn ghost\" href=\"/hackathon\">See the prizes</a>"
        "</div>"
        "<p class=\"note\">%(theme)s</p>"
        "</div>"
        "<div class=\"cards\">%(cards)s</div>"
        "<div class=\"foot\"><span>ForkMesh &middot; collaborate, mirror and "
        "preserve source code</span><span>"
        "<a href=\"/hackathon/rules\">Official rules</a> &middot; "
        "<a href=\"/privacy\">Privacy</a></span></div>"
        "</div></body></html>"
    ) % {
        "title": _html_escape(title),
        "summary": _html_escape(summary),
        "share": _html_escape(share_url),
        "card": _html_escape(card_url),
        "style": INVITE_STYLE,
        "campaign": _html_escape(str(campaign or "Hackathon")),
        "inviter": _profile_html(name),
        "stats": stat_cells,
        "join": _html_escape(join_url),
        "theme": _html_escape(theme),
        "cards": cards,
    }


async def invite_page_handler(env, request, raw_name):
    """``GET /hackathon/join/<name>`` -- the personalised invitation.

    A real page rather than a bounce: the whole point of the mechanic is
    that the person who was invited sees *who* invited them and where that
    person stands before deciding. Accepting goes through the ordinary
    ``/r/<name>`` share link, so the click is counted once, the signed
    one-time claim is minted, and the referral is attributed by the same
    ledger every other ForkMesh referral uses.
    """
    if method_name(request) not in ("GET", "HEAD"):
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            extra_headers={"allow": "GET, HEAD"})
    name = _name(raw_name)
    row = await _public_campaign(env)
    if not row or not valid_node_name(name):
        return None
    slug = str(row.get("slug") or "")
    participant = await _participant_by_name(env, slug, name)
    if not participant:
        # Not a participant (or not a real account): the invitation does not
        # exist, but their plain referral link still might, so hand the
        # visitor to the campaign page instead of a dead end.
        return Response("", status=302, headers={
            "location": "/hackathon",
            "cache-control": "no-store, max-age=0, must-revalidate"})
    origin = _site_origin(env, request)
    points = max(0, int(participant.get("points") or 0))
    stats = {
        "points": points,
        "rank": await _rank(
            env, slug, points, str(participant.get("account_bi") or ""))
        if points else 0,
        "participants": max(0, int(row.get("participant_count") or 0)),
    }
    body = _invite_page(
        origin, str(row.get("title") or "ForkMesh Hackathon"), _config(row),
        name, stats,
        "%s/api/hackathon/cards/invite/%s.png" % (origin, quote(name)))
    return Response(body, status=200, headers={
        "content-type": "text/html; charset=utf-8",
        # Live numbers: a shared cache must never freeze the standing the
        # invitation is built around, but a re-crawl a minute later is fine.
        "cache-control": "public, max-age=60",
    })


def _challenge_page(origin, campaign, challenge, card_url, now):
    """The "can you beat this?" document a challenge link opens."""
    state = policy.challenge_state(challenge, now)
    who = _name(state["challenger"])
    target = "{:,}".format(state["target"])
    title = "%s challenged you: beat %s points" % (who, target)
    if state["open"]:
        summary = ("%s has %s points in the ForkMesh Hackathon and bets you "
                   "cannot pass them. %s builders have tried; %s did." % (
                       who, target, "{:,}".format(state["accepted"]),
                       "{:,}".format(state["beaten"])))
    else:
        summary = ("This challenge from %s has closed. The hackathon is "
                   "still open to join." % who)
    share_url = "%s/hackathon/challenge/%s" % (origin, state["token"])
    accept_url = "/hackathon/me?accept=" + quote(state["token"])
    invite_url = "/r/" + quote(who)
    cells = "".join(
        "<div class=\"stat\"><div class=\"v\">%s</div>"
        "<div class=\"k\">%s</div></div>" % (value, label)
        for value, label in (
            (target, "Points to beat"),
            ("{:,}".format(state["accepted"]), "Accepted"),
            ("{:,}".format(state["beaten"]), "Beaten"),
        ))
    if state["open"]:
        actions = (
            "<a class=\"btn primary\" href=\"%s\">Accept the challenge</a>"
            "<a class=\"btn ghost\" href=\"%s\">New to ForkMesh? Join "
            "through %s</a>" % (
                _html_escape(accept_url), _html_escape(invite_url),
                _html_escape(who)))
        note = ("Accepting joins you to the hackathon. Points come from "
                "building and bringing people in; beating a challenge is for "
                "bragging rights and pays no points.")
    else:
        actions = ("<a class=\"btn primary\" href=\"/hackathon\">See the "
                   "hackathon</a>")
        note = ""
    return (
        "<!doctype html><html lang=\"en\" class=\"light\">"
        "<head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,"
        "initial-scale=1\">"
        "<title>%(title)s</title>"
        "<meta name=\"description\" content=\"%(summary)s\">"
        "<meta name=\"robots\" content=\"noindex\">"
        "<link rel=\"canonical\" href=\"%(share)s\">"
        "<link rel=\"icon\" href=\"/favicon/favicon.ico\" sizes=\"any\">"
        "<link rel=\"icon\" href=\"/favicon/favicon.svg\" type=\"image/svg+xml\">"
        "<meta property=\"og:type\" content=\"website\">"
        "<meta property=\"og:site_name\" content=\"ForkMesh\">"
        "<meta property=\"og:url\" content=\"%(share)s\">"
        "<meta property=\"og:title\" content=\"%(title)s\">"
        "<meta property=\"og:description\" content=\"%(summary)s\">"
        "<meta property=\"og:image\" content=\"%(card)s\">"
        "<meta property=\"og:image:width\" content=\"1200\">"
        "<meta property=\"og:image:height\" content=\"630\">"
        "<meta name=\"twitter:card\" content=\"summary_large_image\">"
        "<meta name=\"twitter:title\" content=\"%(title)s\">"
        "<meta name=\"twitter:description\" content=\"%(summary)s\">"
        "<meta name=\"twitter:image\" content=\"%(card)s\">"
        "<style>%(style)s</style></head><body><div class=\"wrap\">"
        "<div class=\"bar\">"
        "<a class=\"brand\" href=\"/\">"
        "<img src=\"/assets/logo.svg\" alt=\"\" width=\"208\" "
        "height=\"238\">forkmesh</a>"
        "<a class=\"quiet\" href=\"/hackathon\">About the hackathon "
        "&rarr;</a></div>"
        "<div class=\"hero\">"
        "<span class=\"kicker\"><b>%(campaign)s</b> &middot; challenge</span>"
        "<h1>%(who)s challenged you.</h1>"
        "<p class=\"sub\">%(summary)s</p>"
        "<div class=\"stats\">%(cells)s</div>"
        "<div class=\"cta\">%(actions)s</div>"
        "<p class=\"note\">%(note)s</p>"
        "</div>"
        "<div class=\"foot\"><span>ForkMesh &middot; collaborate, mirror "
        "and preserve source code</span><span>"
        "<a href=\"/hackathon/rules\">Official rules</a> &middot; "
        "<a href=\"/privacy\">Privacy</a></span></div>"
        "</div></body></html>"
    ) % {
        "title": _html_escape(title),
        "summary": _html_escape(summary),
        "share": _html_escape(share_url),
        "card": _html_escape(card_url),
        "style": INVITE_STYLE,
        "campaign": _html_escape(str(campaign or "Hackathon")),
        "who": _profile_html(who),
        "cells": cells,
        "actions": actions,
        "note": _html_escape(note),
    }


def _project_page(origin, campaign, project, members, card_url):
    """One public project: what it is, who built it, where to clone it."""
    title_text = str(project.get("title") or "")
    team = str(project.get("team") or "")
    title = "%s - %s" % (title_text, campaign or "ForkMesh Hackathon")
    summary = str(project.get("pitch") or "") or (
        "%s, built by %s for the %s track." % (
            title_text, team, project.get("trackName") or project.get("track")))
    share_url = "%s/hackathon/projects/%s" % (origin, project.get("id"))
    repo = str(project.get("repo") or "")
    repo_href = "/" + "/".join(quote(part) for part in repo.split("/"))
    placement = {"first": "1st place", "second": "2nd place"}.get(
        str(project.get("placement") or ""), "")
    links = [
        "<a class=\"btn primary\" href=\"%s\">Open the repository</a>" % (
            _html_escape(repo_href)),
    ]
    for key, label in (("demoUrl", "Try the demo"), ("videoUrl", "Watch")):
        url = str(project.get(key) or "")
        if url.startswith("https://"):
            links.append(
                "<a class=\"btn ghost\" href=\"%s\" rel=\"nofollow ugc "
                "noopener noreferrer\" target=\"_blank\">%s</a>" % (
                    _html_escape(url), label))
    cells = "".join(
        "<div class=\"stat\"><div class=\"v\">%s</div>"
        "<div class=\"k\">%s</div></div>" % (_html_escape(value), label)
        for value, label in (
            (str(project.get("trackName") or project.get("track") or ""),
             "Track"),
            (str(len(members)), "Builders"),
            (placement or ("{:,}".format(int(project.get("votes") or 0))
                           if "votes" in project else "Entered"),
             "Result" if placement else (
                 "Community votes" if "votes" in project else "Status")),
        ))
    return (
        "<!doctype html><html lang=\"en\" class=\"light\">"
        "<head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,"
        "initial-scale=1\">"
        "<title>%(title)s</title>"
        "<meta name=\"description\" content=\"%(summary)s\">"
        "<link rel=\"canonical\" href=\"%(share)s\">"
        "<link rel=\"icon\" href=\"/favicon/favicon.ico\" sizes=\"any\">"
        "<link rel=\"icon\" href=\"/favicon/favicon.svg\" type=\"image/svg+xml\">"
        "<meta property=\"og:type\" content=\"website\">"
        "<meta property=\"og:site_name\" content=\"ForkMesh\">"
        "<meta property=\"og:url\" content=\"%(share)s\">"
        "<meta property=\"og:title\" content=\"%(title)s\">"
        "<meta property=\"og:description\" content=\"%(summary)s\">"
        "<meta property=\"og:image\" content=\"%(card)s\">"
        "<meta property=\"og:image:width\" content=\"1200\">"
        "<meta property=\"og:image:height\" content=\"630\">"
        "<meta name=\"twitter:card\" content=\"summary_large_image\">"
        "<meta name=\"twitter:title\" content=\"%(title)s\">"
        "<meta name=\"twitter:description\" content=\"%(summary)s\">"
        "<meta name=\"twitter:image\" content=\"%(card)s\">"
        "<style>%(style)s</style></head><body><div class=\"wrap\">"
        "<div class=\"bar\">"
        "<a class=\"brand\" href=\"/\">"
        "<img src=\"/assets/logo.svg\" alt=\"\" width=\"208\" "
        "height=\"238\">forkmesh</a>"
        "<a class=\"quiet\" href=\"/hackathon/projects\">All projects "
        "&rarr;</a></div>"
        "<div class=\"hero\">"
        "<span class=\"kicker\"><b>%(campaign)s</b> &middot; %(kicker)s</span>"
        "<h1>%(heading)s</h1>"
        "<p class=\"sub\">%(summary)s</p>"
        "<div class=\"stats\">%(cells)s</div>"
        "<div class=\"cta\">%(links)s</div>"
        "<p class=\"note\">Built by %(team)s: %(members)s &middot; "
        "<span style=\"font-family:var(--mono,ui-monospace,monospace)\">"
        "%(repo)s</span></p>"
        "</div>"
        "<div class=\"foot\"><span>ForkMesh &middot; collaborate, mirror "
        "and preserve source code</span><span>"
        "<a href=\"/hackathon/rules\">Official rules</a> &middot; "
        "<a href=\"/privacy\">Privacy</a></span></div>"
        "</div></body></html>"
    ) % {
        "title": _html_escape(title),
        "summary": _html_escape(summary),
        "share": _html_escape(share_url),
        "card": _html_escape(card_url),
        "style": INVITE_STYLE,
        "campaign": _html_escape(str(campaign or "Hackathon")),
        "kicker": _html_escape(placement or "project"),
        "heading": _html_escape(title_text),
        "cells": cells,
        "links": "".join(links),
        "team": _html_escape(team),
        "members": ", ".join(_profile_html(member) for member in members),
        "repo": _html_escape(repo),
    }


async def project_page_handler(env, request, submission_id):
    """``GET /hackathon/projects/<id>`` -- one valid project's public page."""
    if method_name(request) not in ("GET", "HEAD"):
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            extra_headers={"allow": "GET, HEAD"})
    row = await _public_campaign(env)
    if not row:
        return None
    slug = str(row.get("slug") or "")
    project = await _project(env, slug, submission_id)
    if not project:
        return None
    config = _config(row)
    unlocked = [r["key"] for r in await _unlocked_milestone_rows(env, slug)]
    item = _submission_public(
        project, config,
        votes_open=policy.voting_open(row.get("status"), unlocked))
    item["team"] = str(project.get("team_name") or "")
    members = [_name(m.get("name", "")) for m in await d1_all(
        env,
        "SELECT name FROM hackathon_team_members WHERE team_id=? "
        "ORDER BY joined_at LIMIT ?",
        str(project.get("team_id") or ""), policy.MAX_TEAM_MEMBERS) or []]
    origin = _site_origin(env, request)
    body = _project_page(
        origin, str(row.get("title") or "ForkMesh Hackathon"), item, members,
        "%s/api/hackathon/cards/project/%s.png" % (origin, item["id"]))
    return Response(body, status=200, headers={
        "content-type": "text/html; charset=utf-8",
        "cache-control": "public, max-age=60",
    })


async def challenge_page_handler(env, request, token):
    """``GET /hackathon/challenge/<token>`` -- the challenge a link opens.

    Marked noindex: a challenge is a private dare between people, and a search
    engine has no business ranking one.
    """
    if method_name(request) not in ("GET", "HEAD"):
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            extra_headers={"allow": "GET, HEAD"})
    row = await _public_campaign(env)
    if not row:
        return None
    challenge = await _challenge(env, str(row.get("slug") or ""), token)
    if not challenge:
        return None
    origin = _site_origin(env, request)
    body = _challenge_page(
        origin, str(row.get("title") or "ForkMesh Hackathon"), challenge,
        "%s/api/hackathon/cards/challenge/%s.png" % (origin, quote(token)),
        _now())
    return Response(body, status=200, headers={
        "content-type": "text/html; charset=utf-8",
        "cache-control": "public, max-age=60",
    })


# --- share cards --------------------------------------------------------------

async def card_handler(env, request, kind, raw_name):
    """``GET /api/hackathon/cards/<kind>/<name>.png`` -- an unfurl image.

    Rendered in-process by og_card.py (Pyodide has no imaging library) and
    edge-cached for a minute, exactly like the repository and referral
    cards, so a post that fans out to hundreds of fediverse servers costs
    one render per colo rather than one per server.
    """
    if method_name(request) not in ("GET", "HEAD"):
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            extra_headers={"allow": "GET, HEAD"})
    kind = str(kind or "").strip().lower()
    if kind not in CARD_KINDS:
        return json_response({"error": "not_found"}, status=404)
    row = await _public_campaign(env)
    if not row:
        return json_response({"error": "not_found"}, status=404)
    slug = str(row.get("slug") or "")
    origin = _ap_origin(env, request)
    key = CARD_CACHE_KEY % (origin, kind, quote(str(raw_name or "")))
    cached = await edge_cache_match_media(key, "image/png")
    if cached is not None:
        return cached
    participants = max(0, int(row.get("participant_count") or 0))
    title = str(row.get("title") or "ForkMesh Hackathon")
    if kind == "project":
        project = await _project(env, slug, raw_name)
        if not project:
            return json_response({"error": "not_found"}, status=404)
        placement = {"first": "1ST PLACE", "second": "2ND PLACE"}.get(
            str(project.get("placement") or ""), "ENTERED")
        members = await d1_first(
            env, "SELECT member_count FROM hackathon_teams WHERE team_id=?",
            str(project.get("team_id") or ""))
        info = {
            "kind": "project",
            "title": title,
            "name": str(project.get("team_name") or ""),
            "headline": str(project.get("title") or ""),
            "cells": (
                ("TRACK", str(project.get("track") or "").upper()),
                ("BUILDERS", int((members or {}).get("member_count") or 0)),
                ("RESULT", placement),
            ),
            "host": urlparse(origin).netloc or "forkmesh.com",
            "path": "/hackathon/projects/" + str(project.get("submission_id")),
        }
    elif kind == "challenge":
        challenge = await _challenge(env, slug, raw_name)
        if not challenge:
            return json_response({"error": "not_found"}, status=404)
        state = policy.challenge_state(challenge, _now())
        info = {
            "kind": "challenge",
            "title": title,
            "name": _name(state["challenger"]),
            "headline": "@%s challenged you" % _name(state["challenger"]),
            "cells": (
                ("POINTS TO BEAT", state["target"]),
                ("ACCEPTED", state["accepted"]),
                ("BEATEN", state["beaten"]),
            ),
            "host": urlparse(origin).netloc or "forkmesh.com",
            "path": "/hackathon/challenge/" + state["token"],
        }
    elif kind == "squad":
        squad = await _squad(env, slug, raw_name)
        if not squad:
            return json_response({"error": "not_found"}, status=404)
        info = {
            "kind": "squad",
            "title": title,
            "name": _name(squad.get("owner_name", "")),
            "headline": "%s's squad" % _name(squad.get("owner_name", "")),
            "cells": (
                ("MEMBERS", int(squad.get("member_count") or 0)),
                ("SQUAD POINTS", int(squad.get("points") or 0)),
                ("BUILDERS", participants),
            ),
            "host": urlparse(origin).netloc or "forkmesh.com",
            "path": "/hackathon/squad/" + str(squad.get("code") or ""),
        }
    else:
        name = _name(raw_name)
        participant = await _participant_by_name(env, slug, name)
        if not participant:
            return json_response({"error": "not_found"}, status=404)
        points = max(0, int(participant.get("points") or 0))
        rank = await _rank(
            env, slug, points,
            str(participant.get("account_bi") or "")) if points else 0
        info = {
            "kind": kind,
            "title": title,
            "name": name,
            "headline": (
                "@%s invites you to build" % name if kind == "invite"
                else "@%s is #%s" % (name, "{:,}".format(rank) if rank
                                     else "unranked")),
            "cells": (
                ("RANK", ("#%d" % rank) if rank else "-"),
                ("POINTS", points),
                ("BUILDERS", participants),
            ),
            "host": urlparse(origin).netloc or "forkmesh.com",
            "path": "/hackathon/join/" + name,
        }
    png = og_card.render_hackathon_card(info)
    # Uint8Array.new copies into a JS-owned buffer: a bare view into WASM
    # memory read off the GIL crashes the isolate (see repo_card_handler).
    response = JsResponse.new(Uint8Array.new(_to_js(bytes(png))), to_js({
        "status": 200,
        "headers": {
            "content-type": "image/png",
            "cache-control": "public, max-age=%d" % CARD_TTL,
        },
    }))
    await edge_cache_put(key, response)
    return response


# --- administration -----------------------------------------------------------

async def _admin_configure(env, actor, slug, data):
    """Create or update a campaign. ``(result, error, status)``.

    Shared by the JSON API and the administrator page, so the two can never
    disagree about what a valid campaign is.
    """
    now = _now()
    existing = await d1_first(
        env, "SELECT slug,status FROM hackathons WHERE slug=?", slug)
    status = str(data.get("status") or "").strip().lower()
    if status and status not in policy.STATUSES:
        return None, "invalid_status", 400
    try:
        config = policy.validate_config(
            data.get("config") if "config" in data
            else policy.default_config())
    except policy.ConfigError as error:
        return {"detail": str(error)}, "invalid_config", 400
    encoded = json.dumps(config, separators=(",", ":"))
    title = str(data.get("title") or "ForkMesh Hackathon")[:policy.MAX_TITLE]

    def _stamp(key):
        try:
            return max(0, int(data.get(key) or 0))
        except (TypeError, ValueError):
            return 0

    if existing:
        await d1_run(
            env,
            "UPDATE hackathons SET title=?,status=COALESCE(NULLIF(?,''),"
            "status),config=?,starts_at=COALESCE(NULLIF(?,0),starts_at),"
            "ends_at=COALESCE(NULLIF(?,0),ends_at),"
            "judging_ends_at=COALESCE(NULLIF(?,0),judging_ends_at),"
            "updated_at=? WHERE slug=?",
            title, status, encoded, _stamp("startsAt"), _stamp("endsAt"),
            _stamp("judgingEndsAt"), now, slug)
    else:
        await d1_run(
            env,
            "INSERT INTO hackathons (slug,title,status,config,starts_at,"
            "ends_at,judging_ends_at,created_at,updated_at) "
            "VALUES (?,?,?,?,?,?,?,?,?)",
            slug, title, status or "draft", encoded, _stamp("startsAt"),
            _stamp("endsAt"), _stamp("judgingEndsAt"), now, now)
    # Milestone rows mirror the configured ladder. An already-unlocked
    # milestone keeps its timestamp: retuning a future threshold must never
    # re-announce something the community already reached.
    for entry in config.get("milestones") or ():
        await d1_run(
            env,
            "INSERT INTO hackathon_milestones "
            "(hackathon_slug,key,metric,threshold,label,description,reward,"
            "unlocked_at) VALUES (?,?,?,?,?,?,?,0) "
            "ON CONFLICT(hackathon_slug,key) DO UPDATE SET "
            "metric=excluded.metric,threshold=excluded.threshold,"
            "label=excluded.label,description=excluded.description,"
            "reward=excluded.reward",
            slug, str(entry.get("key") or ""), str(entry.get("metric") or ""),
            int(entry.get("threshold") or 0), str(entry.get("label") or ""),
            str(entry.get("description") or ""),
            json.dumps(entry.get("reward") or {}, separators=(",", ":")))
    await _check_milestones(env, slug, now)
    await edge_cache_delete(CAMPAIGN_CACHE_KEY)
    for board in policy.BOARD_IDS:
        await edge_cache_delete(BOARD_CACHE_KEY % board)
    await _audit_sensitive_action(
        env, actor, "hackathon.configure", "hackathon", slug, "success",
        {"status": status or "unchanged", "created": not existing})
    return {"slug": slug, "created": not existing}, "", 200


async def admin_handler(env, request):
    """``/api/admin/hackathon`` -- create a campaign and advance its status.

    The only surface that can bring a campaign into existence or make it
    public. Everything it accepts passes through the pure validator first,
    because the configuration decides what the campaign hands out; an
    unknown reward kind or an out-of-range threshold is refused rather than
    clamped, so a typo fails loudly instead of quietly minting Pro months.
    """
    method = method_name(request)
    if method not in ("GET", "POST"):
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            extra_headers={"allow": "GET, POST"})
    data = {}
    if method == "POST":
        try:
            data = await bounded_json_request(request, 64 * 1024) or {}
        except Exception:
            return json_response({"error": "invalid_json"}, status=400)
        if not isinstance(data, dict):
            return json_response({"error": "invalid_json"}, status=400)
    _account_bi, name = await _session_account(env, request, data)
    if not name or not await _is_admin(env, name):
        return json_response({"error": "admin_required"}, status=403,
                             cache_control="no-store")
    await ensure_schema(env)
    if method == "GET":
        rows = await d1_all(
            env,
            "SELECT slug,title,status,starts_at,ends_at,judging_ends_at,"
            "participant_count,verified_count,squad_count,submission_count "
            "FROM hackathons ORDER BY created_at DESC LIMIT 20")
        return json_response(
            {"ok": True, "campaigns": [dict(row) for row in rows or []]},
            cache_control="no-store")

    slug = str(data.get("slug") or "").strip().lower()
    if not SLUG_RE.match(slug):
        return json_response({"error": "invalid_slug"}, status=400,
                             cache_control="no-store")
    action = str(data.get("action") or "").strip().lower()
    if action:
        result, error, status = await _admin_action(
            env, name, slug, action, data)
    else:
        result, error, status = await _admin_configure(env, name, slug, data)
    if error:
        payload = {"error": error}
        payload.update(result or {})
        return json_response(payload, status=status, cache_control="no-store")
    payload = {"ok": True}
    payload.update(result or {})
    return json_response(payload, cache_control="no-store")



def _squad_page(origin, campaign, config, squad, roster):
    """One squad's public microsite.

    This is the brief's "let people create their own mini-contest", with no
    second concept to explain and nothing for a participant to set up: a
    squad already exists the moment their link brings somebody in, so the
    page is simply that squad, with its own name, meter and reward.
    """
    owner = _name(squad.get("owner"))
    members = int(squad.get("members") or 0)
    goal = squad.get("goal") or {}
    percent = int(squad.get("percent") or 0)
    title = "%s's squad in the ForkMesh Hackathon" % owner
    if goal:
        summary = ("%s of %s builders. %s" % (
            "{:,}".format(members), "{:,}".format(int(goal.get("size") or 0)),
            str(goal.get("label") or "")))
    else:
        summary = "%s builders and every goal reached." % (
            "{:,}".format(members))
    names = "".join(
        "<div class=\"card\"><h2>%s</h2><p>%s points</p></div>" % (
            _profile_html(entry.get("name")),
            "{:,}".format(int(entry.get("points") or 0)))
        for entry in list(roster or ())[:9])
    return (
        "<!doctype html><html lang=\"en\" class=\"light\">"
        "<head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,"
        "initial-scale=1\">"
        "<title>%(title)s</title>"
        "<meta name=\"description\" content=\"%(summary)s\">"
        "<link rel=\"canonical\" href=\"%(share)s\">"
        "<link rel=\"icon\" href=\"/favicon/favicon.ico\" sizes=\"any\">"
        "<link rel=\"icon\" href=\"/favicon/favicon.svg\" type=\"image/svg+xml\">"
        "<meta property=\"og:type\" content=\"website\">"
        "<meta property=\"og:site_name\" content=\"ForkMesh\">"
        "<meta property=\"og:url\" content=\"%(share)s\">"
        "<meta property=\"og:title\" content=\"%(title)s\">"
        "<meta property=\"og:description\" content=\"%(summary)s\">"
        "<meta property=\"og:image\" content=\"%(card)s\">"
        "<meta property=\"og:image:width\" content=\"1200\">"
        "<meta property=\"og:image:height\" content=\"630\">"
        "<meta name=\"twitter:card\" content=\"summary_large_image\">"
        "<meta name=\"twitter:title\" content=\"%(title)s\">"
        "<meta name=\"twitter:description\" content=\"%(summary)s\">"
        "<meta name=\"twitter:image\" content=\"%(card)s\">"
        "<style>%(style)s"
        ".meter{max-width:34rem;margin:2rem auto 0;height:.6rem;"
        "border-radius:999px;background:var(--surface-2);overflow:hidden}"
        ".meter i{display:block;height:100%%;background:var(--accent)}"
        "</style></head><body><div class=\"wrap\">"
        "<div class=\"bar\">"
        "<a class=\"brand\" href=\"/\">"
        "<img src=\"/assets/logo.svg\" alt=\"\" width=\"208\" height=\"238\">"
        "forkmesh</a>"
        "<a class=\"quiet\" href=\"/hackathon\">About the hackathon &rarr;</a>"
        "</div>"
        "<div class=\"hero\">"
        "<span class=\"kicker\"><b>%(campaign)s</b> &middot; squad</span>"
        "<h1>%(owner)s's squad</h1>"
        "<p class=\"sub\">%(summary)s</p>"
        "<div class=\"meter\"><i style=\"width:%(percent)d%%\"></i></div>"
        "<div class=\"stats\">"
        "<div class=\"stat\"><div class=\"v\">%(members)s</div>"
        "<div class=\"k\">Builders</div></div>"
        "<div class=\"stat\"><div class=\"v\">%(points)s</div>"
        "<div class=\"k\">Squad points</div></div>"
        "<div class=\"stat\"><div class=\"v\">%(goal)s</div>"
        "<div class=\"k\">Next goal</div></div>"
        "</div>"
        "<div class=\"cta\">"
        "<a class=\"btn primary\" href=\"%(join)s\">Join this squad</a>"
        "<a class=\"btn ghost\" href=\"/hackathon/leaderboard\">"
        "See the boards</a>"
        "</div>"
        "<p class=\"note\">%(theme)s</p>"
        "</div>"
        "<div class=\"cards\">%(names)s</div>"
        "<div class=\"foot\"><span>ForkMesh &middot; collaborate, mirror and "
        "preserve source code</span><span>"
        "<a href=\"/hackathon/rules\">Official rules</a> &middot; "
        "<a href=\"/privacy\">Privacy</a></span></div>"
        "</div></body></html>"
    ) % {
        "title": _html_escape(title),
        "summary": _html_escape(summary),
        "share": _html_escape(
            "%s/hackathon/squad/%s" % (
                origin, quote(str(squad.get("code") or "")))),
        "card": _html_escape(
            "%s/api/hackathon/cards/squad/%s.png" % (
                origin, quote(str(squad.get("code") or "")))),
        "style": INVITE_STYLE,
        "campaign": _html_escape(str(campaign or "Hackathon")),
        "owner": _profile_html(owner),
        "percent": max(0, min(100, percent)),
        "members": "{:,}".format(members),
        "points": "{:,}".format(int(squad.get("points") or 0)),
        "goal": ("{:,}".format(int(goal.get("size") or 0)) if goal
                 else "Reached"),
        "join": _html_escape("/r/" + quote(owner)),
        "theme": _html_escape(str(config.get("theme") or "")),
        "names": names,
    }


async def squad_page_handler(env, request, code):
    """``GET /hackathon/squad/<code>`` -- a squad's public page."""
    if method_name(request) not in ("GET", "HEAD"):
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            extra_headers={"allow": "GET, HEAD"})
    row = await _public_campaign(env)
    if not row:
        return None
    slug = str(row.get("slug") or "")
    squad = await _squad(env, slug, code)
    if not squad:
        return None
    config = _config(row)
    members = max(0, int(squad.get("member_count") or 0))
    goal, percent = policy.squad_goal(members, config)
    roster = await d1_all(
        env,
        "SELECT name,points FROM hackathon_participants "
        "WHERE hackathon_slug=? AND squad_id=? "
        "ORDER BY points DESC,joined_at LIMIT 9",
        slug, str(squad.get("squad_id") or ""))
    body = _squad_page(
        _site_origin(env, request), str(row.get("title") or ""), config,
        {
            "code": str(squad.get("code") or ""),
            "owner": _name(squad.get("owner_name", "")),
            "members": members,
            "points": max(0, int(squad.get("points") or 0)),
            "goal": goal,
            "percent": percent,
        },
        [dict(entry) for entry in roster or []])
    return Response(body, status=200, headers={
        "content-type": "text/html; charset=utf-8",
        "cache-control": "public, max-age=60",
    })
