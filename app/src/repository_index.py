"""Searchable public repository index (issue #478).

Pure, builtin-only ranking over the anonymous public catalog projection (the
validated ``/api/repositories`` snapshot). No ``js``/``env``/D1 access, so the
whole methodology is deterministic and unit-testable, and the published rules
in ``www/docs/repository-index.md`` describe exactly this code.

Trust inputs are deliberately narrow. Every candidate row already passed the
catalog's account-key signature check, the private/Terms/blocked-identity
filters, and the signed HTTPS mirror liveness derivation before it reached the
snapshot. Ranking then uses only signed repository facts (activity series,
counts, head commit, hosted-since) with per-week caps, and never the
self-reported serving counters (``clonesServed``/``websiteServed``), which a
publisher can inflate by cloning itself.
"""

import base64
import hashlib
import json
import re

from mirrors import repo_mirror_same_group
from stable_repository import record_identity


INDEX_SCHEMA = "forkmesh-repository-index-v1"
VIEWS = ("relevance", "updated", "trending", "new", "discussions")
DEFAULT_LIMIT = 20
MAX_LIMIT = 50
MAX_QUERY_TOKENS = 8
MAX_TOKEN_LENGTH = 40
MAX_FACET_VALUES = 12
# Published budgets. Freshness matches the catalog snapshot's fresh window
# (public_snapshots.SNAPSHOT_PATH_FRESH_MS["/api/repositories"]); latency is
# the server-side ranking budget over a full MAX_CATALOG_REPOS catalog.
FRESHNESS_BUDGET_MS = 6 * 60 * 1000
LATENCY_BUDGET_MS = 250
# A record whose signed state is older than this is still searchable but never
# ranks in trending or discussions: a quiet project cannot hold a slot by
# having once been busy.
ACTIVE_WINDOW_MS = 14 * 24 * 60 * 60 * 1000
TRENDING_WEEKS = 4
# Per-week ceiling on counted commits/merges. Bulk-generated or replayed
# history beyond this in any one week adds nothing to the score.
WEEKLY_ACTIVITY_CAP = 50
MIRROR_POINTS = 5
MAX_COUNTED_MIRRORS = 8
DISCUSSION_COUNT_CAP = 500
# Publisher clocks may be slightly ahead; anything further is not trusted as
# a hosted-since or commit time.
CLOCK_SKEW_MS = 5 * 60 * 1000
OFFICIAL_HOSTS = frozenset((
    "forkmesh.com", "www.forkmesh.com", "app.forkmesh.com"))
PUBLIC_ORIGIN = "https://forkmesh.com"

_HEX_COMMIT = re.compile(r"[0-9a-f]{40,64}")
_TOKEN_SPLIT = re.compile(r"[^0-9a-z._+#-]+")


class IndexQueryError(ValueError):
    """A malformed query parameter; the route answers 400 with ``code``."""

    def __init__(self, code):
        super().__init__(code)
        self.code = code


def _text(value, limit=240):
    return str(value if value is not None else "").strip()[:limit]


def _ms(value, now=None):
    try:
        number = int(str(value if value is not None else "").strip() or "0")
    except (TypeError, ValueError):
        return 0
    if number <= 0:
        return 0
    if now is not None and number > now + CLOCK_SKEW_MS:
        return 0
    return number


def _count(value, cap):
    try:
        number = int(str(value if value is not None else "").strip() or "0")
    except (TypeError, ValueError):
        return 0
    return max(0, min(number, cap))


def _recent_capped(series, weeks=TRENDING_WEEKS):
    values = list(series)[-weeks:] if isinstance(series, list) else []
    total = 0
    for value in values:
        try:
            total += max(0, min(int(value), WEEKLY_ACTIVITY_CAP))
        except (TypeError, ValueError):
            continue
    return total


def _clone_target(value):
    """``owner/name`` a clone URL addresses, lowercased; "" when it names none.

    Same parsing as the nested clone_target in mirrors.repo_mirror_same_group,
    which this module's candidate buckets must agree with exactly.
    """
    path = _text(value, 2048)
    if not path:
        return ""
    if "://" in path:
        path = path.split("://", 1)[1]
        path = path.split("/", 1)[1] if "/" in path else ""
    elif path.startswith("git@") and ":" in path:
        path = path.split(":", 1)[1]
    path = path.split("?", 1)[0].split("#", 1)[0].strip("/")
    parts = [part for part in path.split("/") if part]
    if len(parts) < 2:
        return ""
    owner = parts[-2].strip().lower()
    name = parts[-1].strip().lower()
    if name.endswith(".git"):
        name = name[:-4]
    return owner + "/" + name if owner and name else ""


def public_origin(request_origin):
    """The clone/browse origin a result advertises.

    Official hosts always advertise forkmesh.com (the repository's durable
    public address, same rule as the dashboard's publicSiteOrigin); a
    self-hosted App advertises its own origin.
    """
    origin = _text(request_origin, 200).rstrip("/")
    match = re.fullmatch(r"https?://([^/:]+)(?::\d+)?", origin)
    if not match or match.group(1).lower() in OFFICIAL_HOSTS:
        return PUBLIC_ORIGIN
    return origin


def _public_owner(record):
    for owner in record.get("logicalOwners") or []:
        if isinstance(owner, dict) and owner.get("kind") == "organization":
            name = _text(owner.get("owner"), 80)
            if name:
                return name
    return _text(record.get("owner"), 80)


def _is_mirror(record):
    return (
        record.get("source") == "remote-clone"
        or record.get("sourceAuthority") is False
    )


def _eligible(record):
    return (
        isinstance(record, dict)
        and _text(record.get("owner"), 80)
        and _text(record.get("name"), 80)
        and _text(record.get("maintainer"), 120)
        and record.get("visibility") == "public"
        and record.get("isPrivate") is not True
        and record.get("sharedWithMe") is not True
        and not record.get("termsFlagged")
    )


def _languages(record):
    languages = (record.get("logoMetadata") or {}).get("languages")
    if not isinstance(languages, dict):
        return []
    ranked = []
    for name, weight in languages.items():
        label = _text(name, 40)
        try:
            amount = float(weight)
        except (TypeError, ValueError):
            amount = 0.0
        if label:
            ranked.append((-amount, label.lower(), label))
    return [label for _neg, _low, label in sorted(ranked)[:5]]


def _topics(record):
    topics = (record.get("logoMetadata") or {}).get("topics")
    if not isinstance(topics, list):
        return []
    seen = []
    for topic in topics:
        label = _text(topic, 40).lower()
        if label and label not in seen:
            seen.append(label)
    return seen[:10]


def _tokens(query):
    raw = _text(query, 200).lower()
    tokens = []
    for token in _TOKEN_SPLIT.split(raw):
        token = token.strip(".-")[:MAX_TOKEN_LENGTH]
        if token and token not in tokens:
            tokens.append(token)
    return tokens[:MAX_QUERY_TOKENS]


def _relevance(entry, tokens):
    """Sum of per-token field matches; 0 when any token matches nothing."""
    name = entry["name"].lower()
    owners = {entry["owner"].lower(), entry["publicOwner"].lower()}
    topics = set(entry["topics"])
    languages = {value.lower() for value in entry["languages"]}
    description = entry["description"].lower()
    total = 0
    for token in tokens:
        score = 0
        if name == token:
            score += 100
        elif name.startswith(token):
            score += 40
        elif token in name:
            score += 20
        if token in owners:
            score += 30
        elif any(token in owner for owner in owners):
            score += 10
        if token in topics:
            score += 15
        if token in languages:
            score += 15
        if token in description:
            score += 5
        if not score:
            return 0
        total += score
    return total


def build_index(repositories, now):
    """Deduplicate the catalog into one ranked entry per discoverable repo.

    Returns ``(entries, withheld)`` where ``withheld`` counts rows removed by
    each moderation or deduplication rule, so the response can report them.
    """
    withheld = {"ineligible": 0, "duplicateContent": 0, "mirrorRows": 0}
    canonical = []
    mirrors = []
    for record in repositories or []:
        if not _eligible(record):
            withheld["ineligible"] += 1
            continue
        (mirrors if _is_mirror(record) else canonical).append(record)
    withheld["mirrorRows"] = len(mirrors)

    # Same head commit means byte-identical history: republishing someone
    # else's repository under a second account is one discovery, credited to
    # the earliest hosted (then alphabetically first) publisher.
    def seniority(record):
        hosted = _ms(record.get("hostedSince"), now)
        return (
            hosted if hosted else float("inf"),
            _text(record.get("owner")).lower(),
            _text(record.get("name")).lower(),
        )

    by_commit = {}
    kept = []
    for record in sorted(canonical, key=seniority):
        commit = _text(record.get("commit"), 64).lower()
        if _HEX_COMMIT.fullmatch(commit):
            primary = by_commit.get(commit)
            if primary is not None:
                primary["duplicates"].append(
                    _text(record.get("owner")) + "/" + _text(record.get("name")))
                withheld["duplicateContent"] += 1
                continue
        entry = {"record": record, "duplicates": [], "group": [record]}
        if _HEX_COMMIT.fullmatch(commit):
            by_commit[commit] = entry
        kept.append(entry)

    # repo_mirror_same_group matches on an equal root commit, an equal name,
    # or a clone URL naming the other row. Bucket mirrors by those keys so each
    # repository confirms only its real candidates instead of every mirror.
    by_key = {}
    for position, mirror in enumerate(mirrors):
        target = _clone_target(mirror.get("cloneUrl"))
        for key in (
                ("root", _text(mirror.get("rootCommit"), 64).lower()),
                ("name", _text(mirror.get("name"), 80).lower()),
                ("slug", _text(mirror.get("owner"), 80).lower() + "/"
                 + _text(mirror.get("name"), 80).lower()),
                ("target", target)):
            if key[1] and key[1] != "/":
                by_key.setdefault(key, []).append(position)

    entries = []
    for item in kept:
        record = item["record"]
        owner = _text(record.get("owner"), 80)
        name = _text(record.get("name"), 80)
        candidates = set()
        for key in (
                ("root", _text(record.get("rootCommit"), 64).lower()),
                ("name", name.lower()),
                ("target", owner.lower() + "/" + name.lower()),
                ("slug", _clone_target(record.get("cloneUrl")))):
            candidates.update(by_key.get(key, ()))
        group = [record] + [
            mirrors[position] for position in sorted(candidates)
            if repo_mirror_same_group(record, mirrors[position])
        ]
        # Only mirrors run by a different account count. Same-account device
        # mirrors and duplicate republishers never raise a project's standing.
        duplicate_owners = {
            value.split("/", 1)[0].lower() for value in item["duplicates"]}
        independent = sorted({
            _text(mirror.get("owner"), 80).lower()
            for mirror in group[1:]
            if _text(mirror.get("owner"), 80).lower() not in (
                {owner.lower()} | duplicate_owners)
        })
        updated_at = _ms(record.get("updatedAt"), now)
        commit_at = _ms(record.get("commitAt"), now)
        last_activity = commit_at or updated_at
        stale = not updated_at or now - updated_at > ACTIVE_WINDOW_MS
        recent_commits = _recent_capped(record.get("activityWeeks"))
        recent_merges = _recent_capped(record.get("mergeActivityWeeks"))
        counted_mirrors = min(len(independent), MAX_COUNTED_MIRRORS)
        discussions = _count(record.get("discussionCount"), DISCUSSION_COUNT_CAP)
        pulls = _count(record.get("pullCount"), DISCUSSION_COUNT_CAP)
        issues = _count(record.get("issueCount"), DISCUSSION_COUNT_CAP)
        trending = 0 if stale else (
            recent_commits + recent_merges + MIRROR_POINTS * counted_mirrors)
        conversation = 0 if stale else discussions + pulls + issues
        entries.append({
            "owner": owner,
            "name": name,
            "publicOwner": _public_owner(record),
            "description": _text(record.get("description"), 240),
            "languages": _languages(record),
            "topics": _topics(record),
            "category": _text(
                (record.get("logoMetadata") or {}).get("projectCategory"), 40),
            "record": record,
            "group": group,
            "duplicates": sorted(item["duplicates"]),
            "independentMirrors": independent,
            "updatedAt": updated_at,
            "commitAt": commit_at,
            "lastActivity": last_activity,
            "hostedSince": _ms(record.get("hostedSince"), now),
            "stale": stale,
            "recentCommits": recent_commits,
            "recentMerges": recent_merges,
            "discussions": discussions,
            "pulls": pulls,
            "issues": issues,
            "trending": trending,
            "conversation": conversation,
            "cloneOnline": any(bool(row.get("cloneOnline")) for row in group),
            "liveHost": bool(record.get("liveHost")),
        })
    return entries, withheld


def normalize_query(params):
    """Validate query parameters into a canonical, hashable dict."""
    def one(name, limit=200):
        value = params.get(name)
        if isinstance(value, list):
            value = value[0] if value else ""
        return _text(value, limit)

    query = one("q")
    tokens = _tokens(query)
    view = one("view", 20).lower()
    if not view:
        view = "relevance" if tokens else "updated"
    if view not in VIEWS:
        raise IndexQueryError("unknown_view")
    if view == "relevance" and not tokens:
        raise IndexQueryError("relevance_requires_query")
    raw_limit = one("limit", 6)
    if raw_limit:
        if not raw_limit.isdigit() or not 1 <= int(raw_limit) <= MAX_LIMIT:
            raise IndexQueryError("invalid_limit")
        limit = int(raw_limit)
    else:
        limit = DEFAULT_LIMIT
    cloneable = one("cloneable", 5).lower()
    if cloneable not in ("", "0", "1", "true", "false"):
        raise IndexQueryError("invalid_cloneable")
    return {
        "tokens": tokens,
        "view": view,
        "limit": limit,
        "language": one("language", 40).lower(),
        "topic": one("topic", 40).lower(),
        "owner": one("owner", 80).lower(),
        "cloneable": cloneable in ("1", "true"),
        "cursor": one("cursor", 400),
    }


def _query_fingerprint(query):
    material = json.dumps({
        key: query[key] for key in (
            "tokens", "view", "language", "topic", "owner", "cloneable")
    }, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(material.encode()).hexdigest()[:16]


def _sort_key(entry, view, relevance):
    tail = (entry["owner"].lower(), entry["name"].lower())
    if view == "relevance":
        head = (-relevance, -entry["lastActivity"])
    elif view == "trending":
        head = (-entry["trending"], -entry["lastActivity"])
    elif view == "discussions":
        head = (-entry["conversation"], -entry["lastActivity"])
    elif view == "new":
        head = (-entry["hostedSince"], 0)
    else:
        head = (-entry["lastActivity"], 0)
    return head + tail


def _encode_cursor(fingerprint, key):
    raw = json.dumps(
        {"f": fingerprint, "k": list(key)}, separators=(",", ":")).encode()
    return base64.urlsafe_b64encode(raw).decode().rstrip("=")


def _decode_cursor(cursor, fingerprint):
    try:
        padded = cursor + "=" * (-len(cursor) % 4)
        data = json.loads(base64.urlsafe_b64decode(padded.encode()).decode())
        key = data["k"]
        if (
            not isinstance(data, dict)
            or not isinstance(key, list)
            or len(key) != 4
            or not all(isinstance(value, int) for value in key[:2])
            or not all(isinstance(value, str) for value in key[2:])
        ):
            raise ValueError("shape")
    except Exception:
        raise IndexQueryError("invalid_cursor")
    if data.get("f") != fingerprint:
        raise IndexQueryError("cursor_query_mismatch")
    return tuple(key)


def _facets(entries, field):
    counts = {}
    for entry in entries:
        values = (
            [value.lower() for value in entry["languages"]]
            if field == "languages" else entry["topics"])
        for value in set(values):
            counts[value] = counts.get(value, 0) + 1
    ranked = sorted(counts.items(), key=lambda item: (-item[1], item[0]))
    return [
        {"value": value, "count": count}
        for value, count in ranked[:MAX_FACET_VALUES]
    ]


def _result(entry, view, relevance, key, origin, now):
    record = entry["record"]
    owner = entry["publicOwner"] or entry["owner"]
    path = "/" + owner + "/" + entry["name"]
    https_url = origin + path
    clone = {
        "https": https_url,
        "command": "git clone " + https_url,
        "requiresAccount": False,
    }
    ssh_url = _text(record.get("sshUrl"), 300)
    if ssh_url:
        clone["ssh"] = ssh_url
    if view == "relevance":
        components = {"queryScore": relevance}
    elif view == "trending":
        components = {
            "score": entry["trending"],
            "recentCommits": entry["recentCommits"],
            "recentMerges": entry["recentMerges"],
            "countedMirrors": min(
                len(entry["independentMirrors"]), MAX_COUNTED_MIRRORS),
        }
    elif view == "discussions":
        components = {
            "score": entry["conversation"],
            "discussions": entry["discussions"],
            "pullRequests": entry["pulls"],
            "issues": entry["issues"],
        }
    elif view == "new":
        components = {"hostedSince": entry["hostedSince"]}
    else:
        components = {"lastActivity": entry["lastActivity"]}
    return {
        "owner": entry["owner"],
        "name": entry["name"],
        "fullName": owner + "/" + entry["name"],
        # Provider-independent identity (www/docs/dependency-workflows.md);
        # "" when the signed record has no root commit to derive it from.
        "identity": record_identity(record),
        "description": entry["description"],
        "languages": entry["languages"],
        "topics": entry["topics"],
        "category": entry["category"],
        "url": https_url,
        "clone": clone,
        "maintainer": {
            "account": entry["owner"],
            "publicOwner": owner,
            "key": _text(record.get("maintainer"), 120),
            "verification": "account-key-signed-catalog-record",
            "sourceAuthority": record.get("sourceAuthority") is not False,
        },
        "activity": {
            "lastActivity": entry["lastActivity"],
            "commitAt": entry["commitAt"],
            "updatedAt": entry["updatedAt"],
            "hostedSince": entry["hostedSince"],
            "recentCommits": entry["recentCommits"],
            "recentMerges": entry["recentMerges"],
            "commitCount": _count(record.get("commitCount"), 10 ** 9),
            "discussionCount": entry["discussions"],
            "pullCount": entry["pulls"],
            "issueCount": entry["issues"],
        },
        "availability": {
            "cloneOnline": entry["cloneOnline"],
            "liveHost": entry["liveHost"],
            "independentMirrors": len(entry["independentMirrors"]),
            "mirrorRows": len(entry["group"]) - 1,
        },
        "freshness": {
            "recordAgeMs": max(0, now - entry["updatedAt"])
            if entry["updatedAt"] else None,
            "stale": entry["stale"],
        },
        "duplicates": entry["duplicates"],
        "ranking": {"view": view, "sortKey": list(key), "components": components},
    }


def search(catalog_payload, params, now, origin=PUBLIC_ORIGIN,
           snapshot_generated_at=0):
    """Run one deterministic, paginated index query over a catalog payload.

    Raises ``IndexQueryError`` for malformed parameters. Identical catalog +
    parameters always produce identical output; pages are keyset-paginated on
    the full sort tuple (which ends in the unique owner/name), so walking
    ``nextCursor`` over a stable snapshot visits every match exactly once.
    """
    query = normalize_query(params)
    fingerprint = _query_fingerprint(query)
    repositories = (catalog_payload or {}).get("repositories") or []
    entries, withheld = build_index(repositories, now)

    matched = []
    for entry in entries:
        if query["owner"] and query["owner"] not in (
                entry["owner"].lower(), entry["publicOwner"].lower()):
            continue
        if query["language"] and query["language"] not in {
                value.lower() for value in entry["languages"]}:
            continue
        if query["topic"] and query["topic"] not in entry["topics"]:
            continue
        if query["cloneable"] and not entry["cloneOnline"]:
            continue
        relevance = _relevance(entry, query["tokens"]) if query["tokens"] else 0
        if query["tokens"] and not relevance:
            continue
        view = query["view"]
        if view in ("trending", "discussions") and entry["stale"]:
            continue
        if view == "trending" and not entry["trending"]:
            continue
        if view == "discussions" and not entry["conversation"]:
            continue
        if view == "new" and not entry["hostedSince"]:
            continue
        matched.append((_sort_key(entry, view, relevance), relevance, entry))
    matched.sort(key=lambda item: item[0])

    start = 0
    if query["cursor"]:
        after = _decode_cursor(query["cursor"], fingerprint)
        while start < len(matched) and matched[start][0] <= after:
            start += 1
    page = matched[start:start + query["limit"]]
    has_more = start + len(page) < len(matched)
    next_cursor = (
        _encode_cursor(fingerprint, page[-1][0]) if page and has_more else None)
    snapshot_age = (
        max(0, now - int(snapshot_generated_at))
        if snapshot_generated_at else None)
    return {
        "ok": True,
        "schema": INDEX_SCHEMA,
        "view": query["view"],
        "query": {
            "q": " ".join(query["tokens"]),
            "language": query["language"],
            "topic": query["topic"],
            "owner": query["owner"],
            "cloneable": query["cloneable"],
            "limit": query["limit"],
        },
        "total": len(matched),
        "results": [
            _result(entry, query["view"], relevance, key, origin, now)
            for key, relevance, entry in page
        ],
        "nextCursor": next_cursor,
        "facets": {
            "languages": _facets([item[2] for item in matched], "languages"),
            "topics": _facets([item[2] for item in matched], "topics"),
            "cloneable": sum(1 for item in matched if item[2]["cloneOnline"]),
        },
        "withheld": withheld,
        "freshness": {
            "snapshotGeneratedAt": int(snapshot_generated_at or 0) or None,
            "snapshotAgeMs": snapshot_age,
            "budgetMs": FRESHNESS_BUDGET_MS,
            "withinBudget": (
                snapshot_age is not None
                and snapshot_age <= FRESHNESS_BUDGET_MS),
        },
        "budgets": {
            "latencyMs": LATENCY_BUDGET_MS,
            "freshnessMs": FRESHNESS_BUDGET_MS,
            "maxLimit": MAX_LIMIT,
        },
        "methodology": PUBLIC_ORIGIN + "/docs/repository-index/",
    }
