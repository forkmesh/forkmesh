import datetime
import fnmatch
import json
import re
from urllib.parse import parse_qs, quote, urlparse


MEMBER_STATS_PATH = "/api/chat/member-stats"
MEMBER_STATS_MAX_NAMES = 50
MEMBER_STATS_WINDOW_DAYS = 365
MEMBER_STATS_TTL_MS = 5 * 60 * 1000
MEMBER_STATS_CACHE_SECONDS = 300
MEMBER_STATS_MEMO_LIMIT = 2000
MEMBER_STATS_CACHE_PREFIX = "https://forkmesh.internal/api/chat/member-stats"
MEMBER_STATS_EXCLUDE_ENV = "ACTIVITY_EXCLUDE_AUTHORS"
MEMBER_STATS_KINDS = ("pulls", "commits", "issues", "reviews")
MEMBER_NAME_RE = re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")
AUTOMATED_MEMBER_RE = re.compile(
    r"^(?:"
    r"bot|[a-z0-9-]*-bot|forkbot|agent|[a-z0-9-]+-agent|[a-z0-9-]+-actions"
    r"|github-actions?|dependabot|renovate|[a-z0-9-]+-reconciler"
    r"|claude(?:-(?:code|opus|sonnet|haiku|instant|agent|\d)[a-z0-9-]*)?"
    r"|(?:openai-)?codex(?:-cli|-agent)?|(?:chat)?gpt(?:-[a-z0-9-]+)?"
    r"|o\d(?:-mini)?|(?:google-)?gemini(?:-(?:code-assist|cli|agent))?"
    r"|google-jules|(?:github-)?copilot(?:-[a-z0-9-]+)?|llama-\d[a-z0-9-]*"
    r"|devin-ai|cursor-agent|openhands|sweep-ai"
    r"|fmnext-[a-z0-9-]+|mirror\d+|[a-z0-9-]+-mirror"
    r")$")

# Repository source: the instance's own public repositories, read through
# their Pulse ledger (``pulse?view=people``) by a cron phase and kept in
# member_repo_activity. The request path never reaches a repository.
MEMBER_ACTIVITY_PULSE_QUERY = "pulse?view=people"
MEMBER_ACTIVITY_REPOS_PER_RUN = 6
MEMBER_ACTIVITY_DROPS_PER_RUN = 20
MEMBER_ACTIVITY_REFRESH_MS = 3 * 60 * 60 * 1000
MEMBER_ACTIVITY_FETCH_SECONDS = 15
MEMBER_ACTIVITY_PEOPLE_LIMIT = 1000
MEMBER_ACTIVITY_MASK_RE = re.compile(r"^(?:[0-9a-f]{2}){1,64}$")
# Only repositories hosted on this instance count, not catalog entries that
# mirror a repository hosted elsewhere (hosted_git.HOSTED_SOURCE).
MEMBER_ACTIVITY_HOSTED_SOURCE = "hosted-web"
MEMBER_ACTIVITY_ROOT_RE = re.compile(r"^[0-9a-f]{40}(?:[0-9a-f]{24})?$")
MEMBER_ACTIVITY_EMAILS_PER_PERSON = 5
MEMBER_ACTIVITY_EMAILS_PER_REPO = 2000
MEMBER_ACTIVITY_EMAIL_RE = re.compile(r"^[^@\s]{1,64}@[^@\s]{1,190}$")
DAY_RE = re.compile(r"^\d{4}-\d{2}-\d{2}$")

_MEMBER_STATS_MEMO = {}
_MEMBER_NAME_BI = {}


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


def member_stats_window(now_ms):
    today = datetime.datetime.fromtimestamp(
        int(now_ms) / 1000, tz=datetime.timezone.utc).date()
    start = today - datetime.timedelta(days=MEMBER_STATS_WINDOW_DAYS - 1)
    return start.isoformat(), today.isoformat()


def parse_member_names(url):
    params = parse_qs(urlparse(str(url or "")).query, keep_blank_values=True)
    if set(params) - {"names"} or len(params.get("names") or [""]) != 1:
        raise ValueError("invalid_query")
    names = []
    for raw in str((params.get("names") or [""])[0]).split(","):
        name = raw.strip().lower()
        if name and name not in names and MEMBER_NAME_RE.fullmatch(name):
            names.append(name)
    if len(names) > MEMBER_STATS_MAX_NAMES:
        raise ValueError("too_many_names")
    return names


def member_exclusion_rules(env):
    raw = str(getattr(env, MEMBER_STATS_EXCLUDE_ENV, "") or "")
    rules = {item.strip().lower() for item in raw.split(",")}
    return tuple(sorted(rule for rule in rules if rule))[:200]


def is_automated_member(name, rules=()):
    key = str(name or "").strip().lower()
    if not key or AUTOMATED_MEMBER_RE.fullmatch(key):
        return True
    return any(fnmatch.fnmatchcase(key, rule) for rule in rules)


def _count(value):
    try:
        return max(0, int(value or 0))
    except (TypeError, ValueError):
        return 0


def member_stats_entry(name, row):
    row = row or {}
    entry = {"name": name}
    for kind in MEMBER_STATS_KINDS:
        entry[kind] = _count(row.get(kind))
    entry["activeDays"] = _count(row.get("active_days"))
    last = str(row.get("last_day") or "")
    entry["lastActive"] = last if DAY_RE.fullmatch(last) else ""
    return entry


def _ordinal(day):
    try:
        return datetime.date.fromisoformat(str(day or "")).toordinal()
    except ValueError:
        return 0


def _mask_days(day_start, day_mask):
    """The day ordinals set in one stored activity mask (bit 0 = day_start)."""
    first = _ordinal(day_start)
    mask = str(day_mask or "")
    if not first or not MEMBER_ACTIVITY_MASK_RE.fullmatch(mask):
        return set()
    days = set()
    for index, value in enumerate(bytes.fromhex(mask)):
        for bit in range(8):
            if value >> bit & 1:
                days.add(first + index * 8 + bit)
    return days


def combine_member_activity(profile, repos, since, through):
    """One person's figures from both sources, never double counted.

    The profile contribution rows and the instance's repositories can
    describe the same work (a profile project is often one of the instance's
    repositories), so each figure is the larger of the two sources, not their
    sum. Repositories are summed among themselves: the sweep keeps one
    repository per fork family, so two rows never hold the same commits.
    """
    profile = profile or {}
    low, high = _ordinal(since), _ordinal(through)
    totals = {kind: 0 for kind in MEMBER_STATS_KINDS}
    days = set()
    last = ""
    for row in repos or ():
        for kind in MEMBER_STATS_KINDS:
            totals[kind] += _count(row.get(kind))
        days.update(day for day in _mask_days(
            row.get("day_start"), row.get("day_mask")) if low <= day <= high)
        day = str(row.get("last_day") or "")
        if DAY_RE.fullmatch(day) and since <= day <= through and day > last:
            last = day
    combined = {
        kind: max(_count(profile.get(kind)), totals[kind])
        for kind in MEMBER_STATS_KINDS}
    combined["active_days"] = max(
        _count(profile.get("active_days")), len(days))
    profile_last = str(profile.get("last_day") or "")
    if DAY_RE.fullmatch(profile_last) and profile_last > last:
        last = profile_last
    combined["last_day"] = last
    return combined


def _memo_generation():
    generations = globals().get("_D1_WATCH_GENERATIONS")
    if isinstance(generations, dict):
        return int(generations.get("contributions") or 0)
    return 0


def _memo_get(user_bi, now_ms, generation, through):
    hit = _MEMBER_STATS_MEMO.get(user_bi)
    if (hit is None or hit[0] <= now_ms or hit[1] != generation
            or hit[2] != through):
        return False, None
    return True, hit[3]


def _memo_put(user_bi, now_ms, generation, through, entry):
    _MEMBER_STATS_MEMO.pop(user_bi, None)
    _MEMBER_STATS_MEMO[user_bi] = (
        now_ms + MEMBER_STATS_TTL_MS, generation, through, entry)
    while len(_MEMBER_STATS_MEMO) > MEMBER_STATS_MEMO_LIMIT:
        _MEMBER_STATS_MEMO.pop(next(iter(_MEMBER_STATS_MEMO)))


async def _member_bi(env, name):
    known = _MEMBER_NAME_BI.get(name)
    if known:
        return known
    value = await blind_index(env, name)
    if len(_MEMBER_NAME_BI) >= MEMBER_STATS_MEMO_LIMIT:
        _MEMBER_NAME_BI.clear()
    _MEMBER_NAME_BI[name] = value
    return value


async def _public_members(env, wanted):
    if not wanted:
        return {}
    keys = list(wanted)
    rows = await d1_all(
        env,
        "SELECT user_bi,data FROM users WHERE user_bi IN (%s)"
        % ",".join("?" * len(keys)),
        *keys,
    )
    members = {}
    for row in rows or []:
        key = str(row.get("user_bi") or "")
        if key not in wanted or key in members:
            continue
        rec = await decrypt_row(env, row.get("data", ""))
        if not _is_public_roster_member(rec):
            continue
        if str((rec or {}).get("name") or "").strip().lower() != wanted[key]:
            continue
        members[key] = wanted[key]
    return members


async def _member_activity_rows(env, user_bis, since, through):
    """Both sources for a batch in one statement.

    The profile half binds the batch as a list; the repository half takes it
    as one JSON parameter, so a batch of 50 stays under D1's 100 bound
    parameters. A repository counts only while it is public right now: the
    join drops rows of a repository made private or deleted since the sweep.
    """
    if not user_bis:
        return []
    return await d1_all(
        env,
        """WITH ranked_sources AS (
               SELECT projects.source_repo_bi,
                      projects.active_generation_bi,
                      projects.verified_from,
                      ROW_NUMBER() OVER (
                        PARTITION BY projects.project_bi
                        ORDER BY projects.captured_at DESC,
                                 projects.source_repo_bi ASC) AS source_rank
                 FROM profile_contribution_projects AS projects
                 JOIN repositories
                   ON repositories.key_bi=projects.source_repo_bi
                WHERE projects.is_public=1 AND repositories.is_private=0
                  AND projects.active_generation_bi IS NOT NULL
             ), selected_sources AS (
               SELECT * FROM ranked_sources WHERE source_rank=1
             )
             SELECT 'profile' AS source,
                    days.subject_user_bi AS user_bi,
                    SUM(days.pulls) AS pulls,
                    SUM(days.commits) AS commits,
                    SUM(days.issues) AS issues,
                    SUM(days.reviews) AS reviews,
                    COUNT(DISTINCT days.day) AS active_days,
                    MAX(days.day) AS last_day,
                    '' AS day_start, '' AS day_mask
               FROM selected_sources AS sources
               JOIN profile_contribution_days AS days
                 ON days.source_repo_bi=sources.source_repo_bi
                AND days.generation_bi=sources.active_generation_bi
              WHERE days.subject_user_bi IN (%s)
                AND days.day>=? AND days.day<=?
                AND sources.verified_from<=days.day
              GROUP BY days.subject_user_bi
             UNION ALL
             SELECT 'repos' AS source,
                    activity.user_bi AS user_bi,
                    activity.pulls, activity.commits, activity.issues,
                    activity.reviews, 0 AS active_days, activity.last_day,
                    activity.day_start, activity.day_mask
               FROM member_repo_activity AS activity
               JOIN repositories
                 ON repositories.key_bi=activity.repo_bi
                AND repositories.is_private=0
              WHERE activity.user_bi IN (
                    SELECT CAST(value AS TEXT) FROM json_each(?))"""
        % ",".join("?" * len(user_bis)),
        *user_bis, since, through, json.dumps(list(user_bis)),
    )


def _member_stats_cache_key(names, through):
    return (MEMBER_STATS_CACHE_PREFIX + "?names="
            + quote(",".join(sorted(names)), safe=",") + "&to=" + through)


def _member_stats_response(payload):
    return json_response(
        payload,
        cache_control="public, max-age=%d" % MEMBER_STATS_CACHE_SECONDS)


async def member_stats_payload(env, names, now_ms):
    since, through = member_stats_window(now_ms)
    generation = _memo_generation()
    rules = member_exclusion_rules(env)
    people = [name for name in names if not is_automated_member(name, rules)]
    entries = {}
    pending = {}
    for name in people:
        user_bi = await _member_bi(env, name)
        found, entry = _memo_get(user_bi, now_ms, generation, through)
        if found:
            if entry is not None:
                entries[name] = entry
        else:
            pending[user_bi] = name
    members = await _public_members(env, pending)
    rows = await _member_activity_rows(env, list(members), since, through)
    profiles, repos = {}, {}
    for row in rows or []:
        user_bi = str(row.get("user_bi") or "")
        if row.get("source") == "repos":
            repos.setdefault(user_bi, []).append(row)
        else:
            profiles[user_bi] = row
    for user_bi, name in pending.items():
        entry = None
        if user_bi in members:
            entry = member_stats_entry(name, combine_member_activity(
                profiles.get(user_bi), repos.get(user_bi), since, through))
            entries[name] = entry
        _memo_put(user_bi, now_ms, generation, through, entry)
    return {
        "ok": True,
        "window": {
            "from": since, "to": through, "days": MEMBER_STATS_WINDOW_DAYS},
        "members": [entries[name] for name in people if name in entries],
    }


async def member_stats_route(env, request):
    if method_name(request) not in ("GET", "HEAD"):
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            extra_headers={"allow": "GET, HEAD"})
    try:
        names = parse_member_names(getattr(request, "url", ""))
    except ValueError as error:
        return json_response(
            {"error": str(error)}, status=400, cache_control="no-store")
    now_ms = int(Date.now())
    _, through = member_stats_window(now_ms)
    cache_key = _member_stats_cache_key(names, through)
    cached = await edge_cache_match(cache_key)
    if cached is not None:
        return cached
    await ensure_schema(env)
    response = _member_stats_response(
        await member_stats_payload(env, names, now_ms))
    await edge_cache_put(cache_key, response)
    return response


# ---------------------------------------------------------------------------
# Repository source sweep (cron phase "member-activity")
# ---------------------------------------------------------------------------

def _repo_aliases(record):
    name = str(record.get("name") or record.get("repo") or "").strip().lower()
    owners = {str(record.get("owner") or "").strip().lower(),
              str(record.get("logicalOwner") or "").strip().lower()}
    for item in record.get("logicalOwners") or ():
        if isinstance(item, dict):
            owners.add(str(item.get("owner") or "").strip().lower())
    return {owner + "/" + name for owner in owners if owner and name}


def member_activity_sources(catalog_rows):
    """The public repositories whose activity counts, one per fork family.

    Only repositories hosted on this instance count; a catalog entry that
    mirrors a repository hosted elsewhere does not. Among them, a fork whose
    parent is also a counted repository here is skipped (its
    history is the parent's, as on GitHub, where commits in a fork do not
    count), and of several repositories with the same root commit only one
    is kept: not a fork first, then the longest history.
    """
    candidates = []
    aliases = set()
    for row in catalog_rows or ():
        record = (row or {}).get("data")
        if not isinstance(record, dict):
            continue
        if (int((row or {}).get("is_private") or 0)
                or record.get("visibility") != "public"
                or record.get("isPrivate")
                or record.get("source") != MEMBER_ACTIVITY_HOSTED_SOURCE):
            continue
        owner = str(record.get("owner") or "").strip()
        name = str(record.get("name") or record.get("repo") or "").strip()
        key_bi = str((row or {}).get("key_bi") or "")
        if not owner or not name or not key_bi:
            continue
        candidates.append((key_bi, owner, name, record))
        aliases.update(_repo_aliases(record))
    kept = []
    for key_bi, owner, name, record in candidates:
        parent = str(record.get("forkedFrom") or "").strip().lower()
        if parent and parent in aliases and parent not in _repo_aliases(
                record):
            continue
        kept.append((key_bi, owner, name, record))
    kept.sort(key=lambda item: (
        bool(item[3].get("forkedFrom")),
        -_count(item[3].get("commitCount")), item[0]))
    roots = set()
    sources = []
    for key_bi, owner, name, record in kept:
        root = str(record.get("rootCommit") or "").strip().lower()
        if MEMBER_ACTIVITY_ROOT_RE.fullmatch(root):
            if root in roots:
                continue
            roots.add(root)
        sources.append({"key_bi": key_bi, "owner": owner, "name": name})
    sources.sort(key=lambda item: item["key_bi"])
    return sources


def _clean_emails(values):
    found = []
    for value in values if isinstance(values, list) else ():
        email = str(value or "").strip().lower()
        if (MEMBER_ACTIVITY_EMAIL_RE.fullmatch(email) and email not in found
                and len(found) < MEMBER_ACTIVITY_EMAILS_PER_PERSON):
            found.append(email)
    return found


def member_activity_people(data, rules=()):
    """The people in one ``pulse?view=people`` answer, ready to map.

    Each entry is ``(key, row)``: Pulse's case-folded identity, and the
    figures plus ``name`` (the identity when it is shaped like an account
    name, else "") and ``emails`` (commit addresses, present only in the
    capability-signed answer). Identities that can match no account, and
    automated account-shaped names, are left out.
    """
    if not isinstance(data, dict) or data.get("view") != "people":
        return []
    day_start = str(data.get("dayStart") or "")
    if not DAY_RE.fullmatch(day_start):
        return []
    people = data.get("people")
    found = {}
    for person in (people if isinstance(people, list) else [])[
            :MEMBER_ACTIVITY_PEOPLE_LIMIT]:
        if not isinstance(person, dict):
            continue
        key = " ".join(str(person.get("key") or "").split()).lower()
        if not key or key in found:
            continue
        name = key if MEMBER_NAME_RE.fullmatch(key) else ""
        if name and is_automated_member(name, rules):
            continue
        emails = _clean_emails(person.get("emails"))
        if not name and not emails:
            continue
        mask = str(person.get("dayMask") or "").lower()
        last_at = _count(person.get("lastAt"))
        last_day = ""
        if last_at:
            try:
                last_day = datetime.datetime.fromtimestamp(
                    last_at / 1000, tz=datetime.timezone.utc).date().isoformat()
            except (OverflowError, OSError, ValueError):
                last_day = ""
        row = {kind: _count(person.get(kind)) for kind in MEMBER_STATS_KINDS}
        if not any(row.values()):
            continue
        row.update({
            "name": name,
            "emails": emails,
            "day_start": day_start,
            "day_mask": mask if MEMBER_ACTIVITY_MASK_RE.fullmatch(mask) else "",
            "last_day": last_day,
        })
        found[key] = row
    return sorted(found.items())


def _merge_masks(first, second):
    if not first:
        return second
    if not second:
        return first
    left, right = bytes.fromhex(first), bytes.fromhex(second)
    size = max(len(left), len(right))
    left, right = left.ljust(size, b"\0"), right.ljust(size, b"\0")
    return bytes(a | b for a, b in zip(left, right)).hex()


def _verified_email_owner(rec, email):
    """True when ``rec`` is an active person whose verified email is it."""
    if not isinstance(rec, dict):
        return False
    kind = globals().get("_account_kind")
    account_kind = kind(rec) if callable(kind) else str(
        rec.get("kind") or "user")
    return (rec.get("email_verified") is True
            and str(rec.get("email") or "").strip().lower() == email
            and account_kind == "user"
            and rec.get("status") == "active")


async def member_activity_accounts(env, people, rules=()):
    """Map Pulse identities to accounts and total them per account.

    An identity belongs to the account whose verified email is one of its
    commit addresses (exactly one such account; several distinct ones are
    ambiguous and ignored). Without that, it belongs to the account spelled
    exactly like the identity, Pulse's own rule. Several identities of one
    person (say a full-name commit identity and the account-name ledger
    identity) are disjoint activity, so their figures add up and their days
    merge. One users query covers both kinds of match.
    """
    name_bis = {}
    email_bis = {}
    for key, row in people:
        if row["name"]:
            name_bis[await _member_bi(env, row["name"])] = row["name"]
        for email in row["emails"]:
            if email not in email_bis and len(
                    email_bis) < MEMBER_ACTIVITY_EMAILS_PER_REPO:
                email_bis[email] = await blind_index(env, email)
    if not name_bis and not email_bis:
        return {}
    by_email_bi = {value: email for email, value in email_bis.items()}
    rows = await d1_all(
        env,
        "SELECT user_bi, email_bi, data FROM users "
        "WHERE user_bi IN (SELECT CAST(value AS TEXT) FROM json_each(?)) "
        "OR email_bi IN (SELECT CAST(value AS TEXT) FROM json_each(?))",
        json.dumps(sorted(name_bis)), json.dumps(sorted(by_email_bi))) or []
    names = {}
    email_owner = {}
    for row in rows:
        user_bi = str(row.get("user_bi") or "")
        if user_bi in name_bis:
            names[name_bis[user_bi]] = user_bi
        email = by_email_bi.get(str(row.get("email_bi") or ""))
        if not email:
            continue
        rec = await decrypt_row(env, row.get("data", ""))
        account = str((rec or {}).get("name") or "").strip().lower()
        if (_verified_email_owner(rec, email) and account
                and not is_automated_member(account, rules)):
            email_owner[email] = user_bi
    totals = {}
    for key, row in people:
        owners = {email_owner[email] for email in row["emails"]
                  if email in email_owner}
        user_bi = next(iter(owners)) if len(owners) == 1 else ""
        if not user_bi and row["name"]:
            user_bi = names.get(row["name"], "")
        if not user_bi:
            continue
        total = totals.get(user_bi)
        if total is None:
            totals[user_bi] = {
                **{kind: row[kind] for kind in MEMBER_STATS_KINDS},
                "day_start": row["day_start"], "day_mask": row["day_mask"],
                "last_day": row["last_day"]}
            continue
        for kind in MEMBER_STATS_KINDS:
            total[kind] += row[kind]
        total["day_mask"] = _merge_masks(total["day_mask"], row["day_mask"])
        total["last_day"] = max(total["last_day"], row["last_day"])
    return totals


async def _fetch_repo_people(env, owner, name):
    """The repository's people view, with commit addresses when possible.

    A core-hosted repository is read through a capability-signed core read
    that may carry addresses; otherwise the public read (names only).
    """
    signed = globals().get("hosted_people_with_emails")
    if callable(signed):
        try:
            data = await signed(env, owner, name)
        except Exception:
            data = None
        if isinstance(data, dict):
            return data
    try:
        origin = str(_public_base_url(env) or "").rstrip("/")
        response = await js_fetch_with_timeout(
            "%s/api/repo/%s/%s/%s" % (
                origin, quote(owner, safe=""), quote(name, safe=""),
                MEMBER_ACTIVITY_PULSE_QUERY),
            {"method": "GET"}, MEMBER_ACTIVITY_FETCH_SECONDS)
        if int(getattr(response, "status", 0) or 0) != 200:
            return None
        data = await response.json()
        if hasattr(data, "to_py"):
            data = data.to_py()
        return data if isinstance(data, dict) else None
    except Exception:
        return None


async def _refresh_repo(env, source, rules, now_ms):
    key_bi = source["key_bi"]
    data = await _fetch_repo_people(env, source["owner"], source["name"])
    if data is None:
        await d1_run(
            env,
            "INSERT INTO member_activity_repos (repo_bi, refreshed_at, ok) "
            "VALUES (?,?,0) ON CONFLICT(repo_bi) DO UPDATE SET "
            "refreshed_at=excluded.refreshed_at, ok=0",
            key_bi, now_ms)
        return False
    totals = await member_activity_accounts(
        env, member_activity_people(data, rules), rules)
    statements = [(
        "DELETE FROM member_repo_activity WHERE repo_bi=?", (key_bi,))]
    for user_bi in sorted(totals):
        row = totals[user_bi]
        statements.append((
            "INSERT INTO member_repo_activity (repo_bi, user_bi, commits, "
            "pulls, issues, reviews, day_start, day_mask, last_day, "
            "updated_at) VALUES (?,?,?,?,?,?,?,?,?,?)",
            (key_bi, user_bi, row["commits"], row["pulls"], row["issues"],
             row["reviews"], row["day_start"], row["day_mask"],
             row["last_day"], now_ms)))
    statements.append((
        "INSERT INTO member_activity_repos (repo_bi, refreshed_at, people, ok) "
        "VALUES (?,?,?,1) ON CONFLICT(repo_bi) DO UPDATE SET "
        "refreshed_at=excluded.refreshed_at, people=excluded.people, ok=1",
        (key_bi, now_ms, len(totals))))
    await d1_run_batch(env, statements)
    return True


async def refresh_member_repo_activity(env, now_ms=None):
    """Refresh the repository source a few repositories at a time.

    Each run reads the decrypted public catalog the catalog pages already
    memoise, drops rows of repositories that stopped counting (made private,
    deleted, or now a fork of a counted one), then asks the oldest few due
    repositories for their ``pulse?view=people`` answer: one gateway read
    each, which the gateway itself caches for the hour.
    """
    await ensure_schema(env)
    now_ms = int(Date.now()) if now_ms is None else int(now_ms)
    sources = member_activity_sources(
        await _decrypted_public_catalog(env, now_ms))
    wanted = {source["key_bi"]: source for source in sources}
    state = {
        str(row.get("repo_bi") or ""): _count(row.get("refreshed_at"))
        for row in await d1_all(
            env, "SELECT repo_bi, refreshed_at FROM member_activity_repos")
        or []}
    dropped = sorted(key for key in state if key not in wanted)[
        :MEMBER_ACTIVITY_DROPS_PER_RUN]
    for key_bi in dropped:
        await d1_run_batch(env, [
            ("DELETE FROM member_repo_activity WHERE repo_bi=?", (key_bi,)),
            ("DELETE FROM member_activity_repos WHERE repo_bi=?", (key_bi,)),
        ])
    due = sorted(
        (state.get(key_bi, 0), key_bi) for key_bi in wanted
        if state.get(key_bi, 0) <= now_ms - MEMBER_ACTIVITY_REFRESH_MS)
    rules = member_exclusion_rules(env)
    refreshed = []
    for _, key_bi in due[:MEMBER_ACTIVITY_REPOS_PER_RUN]:
        source = wanted[key_bi]
        if await _refresh_repo(env, source, rules, now_ms):
            refreshed.append(source["owner"] + "/" + source["name"])
    return {"refreshed": refreshed, "dropped": len(dropped),
            "due": max(0, len(due) - MEMBER_ACTIVITY_REPOS_PER_RUN)}
