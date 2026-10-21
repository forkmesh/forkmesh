import datetime
import fnmatch
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
    entry["lastActive"] = last if re.fullmatch(
        r"\d{4}-\d{2}-\d{2}", last) else ""
    return entry


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
             SELECT days.subject_user_bi AS user_bi,
                    SUM(days.pulls) AS pulls,
                    SUM(days.commits) AS commits,
                    SUM(days.issues) AS issues,
                    SUM(days.reviews) AS reviews,
                    COUNT(DISTINCT days.day) AS active_days,
                    MAX(days.day) AS last_day
               FROM selected_sources AS sources
               JOIN profile_contribution_days AS days
                 ON days.source_repo_bi=sources.source_repo_bi
                AND days.generation_bi=sources.active_generation_bi
              WHERE days.subject_user_bi IN (%s)
                AND days.day>=? AND days.day<=?
                AND sources.verified_from<=days.day
              GROUP BY days.subject_user_bi"""
        % ",".join("?" * len(user_bis)),
        *user_bis, since, through,
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
    by_bi = {str(row.get("user_bi") or ""): row for row in rows or []}
    for user_bi, name in pending.items():
        entry = None
        if user_bi in members:
            entry = member_stats_entry(name, by_bi.get(user_bi))
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
