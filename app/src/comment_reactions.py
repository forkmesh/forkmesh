import re
from urllib.parse import parse_qs, urlparse

CATALOG = (
    ("like", "Like"), ("love", "Love"), ("laugh", "Laugh"),
    ("celebrate", "Celebrate"), ("hot", "Hot"), ("thanks", "Thanks"),
    ("smile", "Smile"), ("wink", "Wink"), ("cool", "Cool"),
    ("thinking", "Thinking"), ("surprised", "Surprised"), ("sad", "Sad"),
    ("angry", "Angry"), ("dislike", "Dislike"), ("eyes", "Eyes"),
    ("star", "Star"), ("rocket", "Rocket"), ("check", "Done"),
    ("hearthands", "Heart hands"), ("sparkleheart", "Sparkling heart"),
    ("handshake", "Handshake"), ("glowstar", "Glowing star"),
    ("trophy", "Trophy"), ("thankyou", "Thank you"),
)
EMOJI = {
    "like": "\U0001F44D", "love": "❤️", "laugh": "\U0001F602",
    "celebrate": "\U0001F389", "hot": "\U0001F525", "thanks": "\U0001F64F",
    "smile": "\U0001F642", "wink": "\U0001F609", "cool": "\U0001F60E",
    "thinking": "\U0001F914", "surprised": "\U0001F62E", "sad": "\U0001F622",
    "angry": "\U0001F621", "dislike": "\U0001F44E", "eyes": "\U0001F440",
    "star": "⭐", "rocket": "\U0001F680", "check": "✅",
    "hearthands": "\U0001FAF6", "sparkleheart": "\U0001F496",
    "handshake": "\U0001F91D", "glowstar": "\U0001F31F",
    "trophy": "\U0001F3C6", "thankyou": "\U0001F4AC",
}
# The gratitude group: the picker's second row and the "Say thanks" family.
# "thankyou" is a sticker (its own drawing, /assets/reactions/thank-you.svg); its
# unicode stand-in is only for text surfaces such as the ping summary.
GRATITUDE = ("thanks", "hearthands", "sparkleheart", "handshake", "glowstar",
             "trophy", "thankyou")
STICKERS = {"thankyou": "/assets/reactions/thank-you.svg"}
SLUGS = frozenset(slug for slug, _label in CATALOG)
KINDS = {"issue": ("issues", "issues"), "pull": ("pulls", "pull"),
         "discussion": ("discussions", "discussions")}
CUSTOM_RE = re.compile(r"^custom:[a-z0-9](?:[a-z0-9-]{0,30}[a-z0-9])?$")
COMMENT_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._:@+-]{0,159}$")
MAX_NUMBER = 1_000_000_000
MAX_PEOPLE = 10
MAX_PER_ACCOUNT_PER_COMMENT = 8
MAX_ROWS_PER_RECORD = 5000
MAX_ROWS_PER_ACCOUNT_PER_RECORD = 200
MAX_PINGED_COMMENTS_PER_DAY = 30
RATE_WINDOW_MS = 60 * 1000
RATE_MAX_PER_WINDOW = 60
DAY_MS = 24 * 60 * 60 * 1000
MAX_ADDS_PER_DAY = 1000
PUBLIC_CACHE = "public, max-age=5, stale-while-revalidate=30"
PRIVATE_CACHE = "private, no-store"
VARY = {"vary": "authorization, cookie"}


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


def clean_kind(value):
    kind = str(value or "").strip().lower()
    return kind if kind in KINDS else ""


def clean_number(value):
    try:
        number = int(str(value if value is not None else "").strip())
    except (TypeError, ValueError):
        return 0
    return number if 0 < number <= MAX_NUMBER else 0


def clean_comment(value):
    comment = str(value or "") if isinstance(value, str) else ""
    return comment if COMMENT_RE.fullmatch(comment) else ""


def clean_emoji(value):
    emoji = str(value or "") if isinstance(value, str) else ""
    if emoji in SLUGS or CUSTOM_RE.fullmatch(emoji):
        return emoji
    return ""


def known_emoji(emoji):
    return emoji in SLUGS or bool(CUSTOM_RE.fullmatch(emoji or ""))


def subject_key(kind, number):
    return "%s:%d" % (kind, number)


def _tally_order(item):
    return (item["first"], item["emoji"])


async def _person(env, row):
    try:
        data = await decrypt_row(env, row.get("data"))
    except Exception:
        data = None
    if not isinstance(data, dict):
        return None
    name = clean_string(data.get("name"), MAX_NODE_NAME)
    if not name:
        return None
    return {"name": name, "agent": bool(data.get("agent"))}


async def summarize(env, rows, viewer_bi="", custom=None):
    """Tallies per comment. A custom reaction's tally carries ``custom``: its
    label and image from ``custom`` (slug -> item), or None once it was
    deleted, so every surface can draw a fallback chip."""
    comments = {}
    for row in rows or []:
        comment = str(row.get("comment") or "")
        emoji = str(row.get("emoji") or "")
        if not clean_comment(comment) or not known_emoji(emoji):
            continue
        tallies = comments.setdefault(comment, {})
        tally = tallies.get(emoji)
        created = int(row.get("created_at") or 0)
        if tally is None:
            tally = {"emoji": emoji, "count": 0, "mine": False, "people": [],
                     "first": created}
            tallies[emoji] = tally
        tally["count"] += 1
        tally["first"] = min(tally["first"], created)
        if viewer_bi and str(row.get("account_bi") or "") == viewer_bi:
            tally["mine"] = True
        if len(tally["people"]) < MAX_PEOPLE:
            person = await _person(env, row)
            if person:
                tally["people"].append(person)
    out = {}
    for comment, tallies in comments.items():
        ordered = sorted(tallies.values(), key=_tally_order)
        out[comment] = [_public_tally(tally, custom) for tally in ordered]
    return out


def _public_tally(tally, custom):
    out = {key: tally[key] for key in ("emoji", "count", "mine", "people")}
    if CUSTOM_RE.fullmatch(tally["emoji"]):
        item = (custom or {}).get(tally["emoji"][len("custom:"):])
        out["custom"] = {"label": item["label"], "src": item["src"],
                         "animatedSrc": item.get("animatedSrc") or ""} \
            if item else None
    return out


def _has_custom(rows):
    return any(CUSTOM_RE.fullmatch(str(row.get("emoji") or ""))
               for row in rows or [])


async def _custom_catalog(env, owner, rows=None):
    if rows is not None and not _has_custom(rows):
        return {}
    try:
        return await custom_reactions_catalog(env, owner)
    except Exception:
        return {}


async def tip_counts(env, repo_bi, subject):
    """comment -> number of confirmed tips on it (comment_tips)."""
    try:
        rows = await d1_all(
            env, "SELECT comment, COUNT(*) AS n FROM comment_tips WHERE "
            "repo_bi=? AND subject=? AND status='confirmed' GROUP BY comment",
            repo_bi, subject)
    except Exception:
        return {}
    return {str(row.get("comment")): int(row.get("n") or 0)
            for row in rows or [] if clean_comment(str(row.get("comment") or ""))}


async def _rows(env, repo_bi, subject, comment=""):
    if comment:
        return await d1_all(
            env,
            "SELECT comment, emoji, account_bi, created_at, data "
            "FROM comment_reactions WHERE repo_bi=? AND subject=? "
            "AND comment=? ORDER BY created_at LIMIT ?",
            repo_bi, subject, comment, MAX_ROWS_PER_RECORD)
    return await d1_all(
        env,
        "SELECT comment, emoji, account_bi, created_at, data "
        "FROM comment_reactions WHERE repo_bi=? AND subject=? "
        "ORDER BY created_at LIMIT ?",
        repo_bi, subject, MAX_ROWS_PER_RECORD)


async def _rate_limited(env, account_bi, now):
    row = await d1_first(
        env,
        "INSERT INTO comment_reaction_rate (account_bi, window_start, count) "
        "VALUES (?,?,1) ON CONFLICT(account_bi) DO UPDATE SET "
        "count=CASE WHEN ?-comment_reaction_rate.window_start>=? "
        "THEN 1 ELSE comment_reaction_rate.count+1 END, "
        "window_start=CASE WHEN ?-comment_reaction_rate.window_start>=? "
        "THEN ? ELSE comment_reaction_rate.window_start END "
        "RETURNING count, window_start",
        account_bi, now, now, RATE_WINDOW_MS, now, RATE_WINDOW_MS, now)
    count = int((row or {}).get("count") or 1)
    start = int((row or {}).get("window_start") or now)
    if count <= RATE_MAX_PER_WINDOW:
        return None
    retry_ms = max(1000, RATE_WINDOW_MS - (now - start))
    return json_response(
        {"error": "rate_limited", "retryAfterMs": retry_ms}, status=429,
        cache_control=PRIVATE_CACHE,
        extra_headers={"Retry-After": str(max(1, (retry_ms + 999) // 1000))})


async def _ping_budget_left(env, account_bi, now):
    return await _count(
        env, "SELECT COUNT(DISTINCT repo_bi || '/' || subject || '/' || comment) "
        "AS n FROM comment_reactions WHERE account_bi=? AND created_at>=?",
        account_bi, now - DAY_MS) <= MAX_PINGED_COMMENTS_PER_DAY


async def _count(env, sql, *args):
    row = await d1_first(env, sql, *args)
    return int((row or {}).get("n") or 0)


async def _refusal(env, repo_bi, subject, comment, account_bi, now):
    if await _count(
            env, "SELECT COUNT(*) AS n FROM comment_reactions WHERE repo_bi=? "
            "AND subject=? AND comment=? AND account_bi=?",
            repo_bi, subject, comment, account_bi) >= MAX_PER_ACCOUNT_PER_COMMENT:
        return "too_many_reactions", 409
    if await _count(
            env, "SELECT COUNT(*) AS n FROM comment_reactions WHERE repo_bi=? "
            "AND subject=? AND account_bi=?",
            repo_bi, subject, account_bi) >= MAX_ROWS_PER_ACCOUNT_PER_RECORD:
        return "too_many_reactions", 409
    if await _count(
            env, "SELECT COUNT(*) AS n FROM comment_reactions WHERE repo_bi=? "
            "AND subject=?", repo_bi, subject) >= MAX_ROWS_PER_RECORD:
        return "record_full", 409
    if await _count(
            env, "SELECT COUNT(*) AS n FROM comment_reactions WHERE "
            "account_bi=? AND created_at>=?",
            account_bi, now - DAY_MS) >= MAX_ADDS_PER_DAY:
        return "daily_limit", 429
    return None


def _is_agent(request, data):
    try:
        token, cookie_auth = _request_account_session_token(request, data)
    except Exception:
        return False
    return not cookie_auth and str(token or "").startswith("fmat_")


def names_sentence(names, total):
    names = list(names)[:2]
    rest = max(0, int(total) - len(names))
    if not rest:
        return " and ".join(names)
    return "%s and %d other%s" % (
        ", ".join(names), rest, "" if rest == 1 else "s")


async def notify_author(env, request, owner, repo, access, kind, number,
                        comment, actor, author):
    recipient = clean_string(author, MAX_NODE_NAME).strip().lower()
    if not valid_node_name(recipient) or recipient == actor:
        return False
    if access.get("visibility") != "public" and await _repository_access_context(
            env, request, owner, repo, viewer=recipient) is None:
        return False
    rows = await _rows(env, access["repoBi"], subject_key(kind, number), comment)
    recipient_bi = await blind_index(env, recipient)
    names, counts = [], {}
    for row in reversed(rows or []):
        if str(row.get("account_bi") or "") == recipient_bi:
            continue
        emoji = str(row.get("emoji") or "")
        if not known_emoji(emoji):
            continue
        counts[emoji] = counts.get(emoji, 0) + 1
        if len(names) < 2:
            person = await _person(env, row)
            if person and person["name"].lower() not in [n.lower() for n in names]:
                names.append(person["name"])
    if not names:
        return False
    people = await _count(
        env, "SELECT COUNT(DISTINCT account_bi) AS n FROM comment_reactions "
        "WHERE repo_bi=? AND subject=? AND comment=? AND account_bi<>?",
        access["repoBi"], subject_key(kind, number), comment, recipient_bi)
    what = "comment" if comment != "body" else {
        "issue": "issue", "pull": "pull request",
        "discussion": "discussion"}[kind]
    title = "%s reacted to your %s on %s/%s#%d" % (
        names_sentence(names, max(people, len(names))), what, owner, repo,
        number)
    order = [slug for slug, _label in CATALOG]
    custom = await _custom_catalog(env, owner) if any(
        slug not in EMOJI for slug in counts) else {}

    def glyph(slug):
        if slug in EMOJI:
            return EMOJI[slug]
        item = custom.get(slug[len("custom:"):])
        return ":%s:" % (item["label"] if item else slug[len("custom:"):])

    summary = " ".join("%s %d" % (glyph(slug), counts[slug]) for slug in sorted(
        counts, key=lambda slug: (-counts[slug], order.index(slug)
                                  if slug in order else len(order), slug)))
    href = repo_web_href(owner, repo) + "/%s/%d" % (KINDS[kind][1], number)
    return await enqueue_notification(
        env, recipient, "reaction", title, body=summary,
        repo=owner + "/" + repo, href=href, actor=actor, source="reaction",
        dedupe="reaction:%s/%s:%s:%d:%s" % (
            owner.lower(), repo.lower(), kind, number, comment),
        meta={"kind": kind, "number": number, "comment": comment},
        push=False)


async def _read(env, request, owner, repo):
    query = parse_qs(urlparse(str(request.url)).query)
    kind = clean_kind((query.get("kind") or [""])[0])
    number = clean_number((query.get("number") or [""])[0])
    if not kind or not number:
        return json_response({"error": "invalid_target"}, status=400,
                             cache_control=PRIVATE_CACHE)
    access = await _repository_access_context(env, request, owner, repo)
    if access is None:
        return json_response({"error": "not_found"}, status=404,
                             cache_control=PRIVATE_CACHE)
    viewer = str(access.get("viewer") or "")
    viewer_bi = await blind_index(env, viewer) if viewer else ""
    rows = await _rows(env, access["repoBi"], subject_key(kind, number))
    shared = not viewer and access.get("visibility") == "public"
    custom = await _custom_catalog(env, owner, rows)
    return json_response(
        {"ok": True, "kind": kind, "number": number, "signedIn": bool(viewer),
         "reactions": await summarize(env, rows, viewer_bi, custom),
         "tips": await tip_counts(env, access["repoBi"],
                                  subject_key(kind, number))},
        cache_control=PUBLIC_CACHE if shared else PRIVATE_CACHE,
        extra_headers=VARY)


async def _write(env, request, owner, repo):
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response({"error": "invalid_json"}, status=400,
                             cache_control=PRIVATE_CACHE)
    if not isinstance(data, dict):
        return json_response({"error": "invalid_json"}, status=400,
                             cache_control=PRIVATE_CACHE)
    account_bi, rec = await _account_session_record(env, request, data)
    account = clean_string((rec or {}).get("name"), MAX_NODE_NAME).lower()
    if not rec or not account_bi or not account:
        return json_response({"error": "unauthorized"}, status=401,
                             cache_control=PRIVATE_CACHE)
    kind = clean_kind(data.get("kind"))
    number = clean_number(data.get("number"))
    comment = clean_comment(data.get("comment"))
    emoji = clean_emoji(data.get("emoji"))
    on = data.get("on")
    if not kind or not number or not comment:
        error = "invalid_target"
    elif not emoji:
        error = "invalid_reaction"
    elif not isinstance(on, bool):
        error = "invalid_state"
    else:
        error = ""
    if error:
        return json_response({"error": error}, status=400,
                             cache_control=PRIVATE_CACHE)
    access = await _repository_access_context(env, request, owner, repo,
                                           viewer=account)
    if access is None:
        return json_response({"error": "not_found"}, status=404,
                             cache_control=PRIVATE_CACHE)
    disabled = await _repo_feature_off(env, owner, repo, KINDS[kind][0])
    if disabled is not None:
        return disabled
    custom = {}
    if CUSTOM_RE.fullmatch(emoji):
        custom = await _custom_catalog(env, owner)
        # A deleted or disabled custom reaction can still be taken back.
        if on and emoji[len("custom:"):] not in custom:
            return json_response({"error": "invalid_reaction"}, status=400,
                                 cache_control=PRIVATE_CACHE)
    now = int(Date.now())
    limited = await _rate_limited(env, account_bi, now)
    if limited is not None:
        return limited
    repo_bi = access["repoBi"]
    subject = subject_key(kind, number)
    added = False
    if on:
        exists = await d1_first(
            env, "SELECT 1 AS yes FROM comment_reactions WHERE repo_bi=? AND "
            "subject=? AND comment=? AND emoji=? AND account_bi=?",
            repo_bi, subject, comment, emoji, account_bi)
        if not exists:
            refused = await _refusal(env, repo_bi, subject, comment,
                                     account_bi, now)
            if refused is not None:
                return json_response({"error": refused[0]}, status=refused[1],
                                     cache_control=PRIVATE_CACHE)
            person = {"name": clean_string(rec.get("name"), MAX_NODE_NAME)
                      or account, "agent": _is_agent(request, data)}
            await d1_run(
                env,
                "INSERT INTO comment_reactions (repo_bi, subject, comment, "
                "emoji, account_bi, created_at, data) VALUES (?,?,?,?,?,?,?) "
                "ON CONFLICT(repo_bi, subject, comment, emoji, account_bi) "
                "DO NOTHING",
                repo_bi, subject, comment, emoji, account_bi, now,
                await encrypt_row(env, person))
            added = True
    else:
        await d1_run(
            env, "DELETE FROM comment_reactions WHERE repo_bi=? AND subject=? "
            "AND comment=? AND emoji=? AND account_bi=?",
            repo_bi, subject, comment, emoji, account_bi)
    rows = await _rows(env, repo_bi, subject, comment)
    if not custom and _has_custom(rows):
        custom = await _custom_catalog(env, owner)
    tallies = (await summarize(env, rows, account_bi, custom)).get(comment, [])
    if added and data.get("author") and await _ping_budget_left(
            env, account_bi, now):
        try:
            await notify_author(env, request, owner, repo, access, kind, number,
                                comment, account, data.get("author"))
        except Exception:
            pass
    return json_response(
        {"ok": True, "kind": kind, "number": number, "comment": comment,
         "emoji": emoji, "on": on, "reactions": tallies},
        cache_control=PRIVATE_CACHE)


async def handler(env, request, owner, repo):
    await ensure_schema(env)
    method = method_name(request)
    if method == "GET":
        return await _read(env, request, owner, repo)
    if method == "POST":
        return await _write(env, request, owner, repo)
    return json_response({"error": "method_not_allowed"}, status=405,
                         cache_control=PRIVATE_CACHE)
