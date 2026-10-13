"""Account-based Market star ratings with required improvement reviews.

One current rating (1-5 stars) per marketplace package and account. A rating
below five stars must carry a structured review that says what did not work and
how the package could earn five stars; the server enforces that, not only the
page. Five stars may be left without a review.

Every write appends to ``marketplace_review_history`` (so an edited rating
keeps what was said before), then refreshes the cached per-package aggregate in
``marketplace_package_rating_stats``. The package's maintainers receive a ping
(the ordinary Pings rail: inbox, socket, phone banner, email) for every review
under five stars, may reply publicly, and may mark a review "addressed in
vX.Y". Anyone signed in may report abuse; administrators hide reviews.

This module is provider-neutral: every Worker/D1 primitive arrives through the
narrow runtime adapter in ``marketplace_runtime._MarketplaceReviewsRuntime``,
so the whole route is testable against sqlite3. Wire format and rules:
``docs/MARKETPLACE_RATINGS.md``.
"""

from __future__ import annotations

import hashlib
import re


# -- Vocabulary ---------------------------------------------------------------

FIRST_PARTY_MARKET = "forkmesh"
COMMUNITY_MARKET = "forkmesh-community"
APPS_MARKET = "forkmesh-apps"
MARKETS = (FIRST_PARTY_MARKET, COMMUNITY_MARKET, APPS_MARKET)
# The first-party catalog (marketplace/catalog.json), id -> display name.
# test_marketplace_reviews pins this map to the catalog manifests, so a new
# package fails the suite until it is added here.
FIRST_PARTY_PACKAGES = {
    "actions": "Actions",
    "activity": "Activity",
    "agent-watch": "Watch",
    "agents": "Agents",
    "analytics": "Analytics",
    "aquarium": "Aquarium",
    "backups": "Backups",
    "branches": "Branches",
    "browser-extension": "Browser Extension",
    "bug-reports": "Report a bug",
    "calendar": "Calendar",
    "calories": "Calorie Counter",
    "capture": "Capture",
    "chat": "Chat",
    "cleanup": "Disk Cleanup",
    "code": "Code",
    "commits": "Commits",
    "competitor": "Competitor Watch",
    "computer-security": "Computer Security Checkup",
    "coves": "Coves",
    "crm": "CRM",
    "design-system": "Design System",
    "desktops": "Desktops",
    "discord": "Discord",
    "discussions": "Discussions",
    "docs": "Docs",
    "food-guide": "Food Guide",
    "forkmesh-ide": "ForkMesh for VS Code",
    "forkmesh-sky-office": "ForkMesh Aquarium Sky Office",
    "gemma-4": "Gemma 4",
    "glasses-alert": "Glasses Alert",
    "hosts": "Hosts & fleet",
    "inbox": "Inbox",
    "issues": "Issues",
    "ledger": "Ledger",
    "marketing": "Marketing",
    "muse-spark-1.3-contributor": "Muse Spark 1.3 Contributor",
    "nemotron-3-ultra": "Nemotron 3 Ultra",
    "nemotron-3.5-lightning": "Nemotron 3.5 Lightning",
    "network": "Network & nodes",
    "notes": "Notes",
    "on-device-ai": "On-device AI",
    "organizations": "Organizations",
    "pull-requests": "Pull requests",
    "reddit-banner-board": "Reddit Banner Board",
    "releases": "Releases",
    "sentry": "Sentry",
    "shortcuts": "Shortcuts",
    "sign-in": "Sign in",
    "sleep": "Sleep",
    "special-agents": "Special Agents",
    "speech": "Speech Playground",
    "stats": "Stats",
    "status-check-api": "Status: API",
    "status-check-cores": "Status: Core Nodes",
    "status-check-database": "Status: Database",
    "status-check-durable-objects": "Status: Durable Objects",
    "status-check-email": "Status: Email Delivery",
    "status-check-errors": "Status: Worker Errors",
    "status-check-git-hosting": "Status: Git Hosting",
    "status-check-mirrors": "Status: Mirrors",
    "status-check-realtime": "Status: Realtime Sync",
    "status-check-release-android": "Status: Android Release",
    "status-check-release-linux": "Status: Linux Release",
    "status-check-release-macos": "Status: macOS Release",
    "status-check-release-windows": "Status: Windows Release",
    "status-check-repository": "Status: Repository Page",
    "status-check-runners": "Status: Runners",
    "status-check-status-page": "Status: Status Page API",
    "status-check-website": "Status: Website",
    "status-check-world-mobile": "Status: World Mobile Startup",
    "status-page": "Status Page",
    "steps": "Pedometer",
    "stretch": "Stretch Break",
    "sugar-break": "Sugar Break",
    "tasks": "Tasks",
    "timers": "Timers",
    "tour": "Tour",
    "vault": "Password Vault",
    "voice-changer": "Voice Changer",
    "vscodium": "VSCodium",
    "water": "Water Intake",
    "winamp": "Winamp Classic",
    "workflows": "Workflows",
    "workflows-examples": "Workflows: Examples",
    "workflows-pack-ai": "Workflows: AI",
    "workflows-pack-analytics": "Workflows: Analytics",
    "workflows-pack-commerce-and-finance": "Workflows: Commerce and finance",
    "workflows-pack-communication": "Workflows: Communication",
    "workflows-pack-crm-and-support": "Workflows: CRM and support",
    "workflows-pack-data-and-storage": "Workflows: Data and storage",
    "workflows-pack-developer-tools": "Workflows: Developer tools",
    "workflows-pack-email": "Workflows: Email",
    "workflows-pack-forms-and-meetings": "Workflows: Forms and meetings",
    "workflows-pack-productivity": "Workflows: Productivity",
    "workflows-pack-social-and-content": "Workflows: Social and content",
    "workflows-pack-utilities": "Workflows: Utilities",
    "workflows-repository-files": "Workflows: Repository Files",
    "world": "World",
    "x-banner-board": "X Banner Board",
}
# First-party packages are maintained by the forkmesh organization.
FIRST_PARTY_ORG = "forkmesh"
MAINTAINER_ORG_ROLES = ("owner", "admin", "maintainer")
MAX_NOTIFIED_MAINTAINERS = 25

CATEGORIES = ("bug", "missing-feature", "docs", "performance", "ux", "other")
CATEGORY_LABELS = {
    "bug": "Bug",
    "missing-feature": "Missing feature",
    "docs": "Docs",
    "performance": "Performance",
    "ux": "UX",
    "other": "Other",
}
# Required improvement fields of a rating below five stars.
MIN_FIELD_CHARS = 40
MIN_FIELD_WORDS = 8
MIN_DISTINCT_WORDS = 5
MAX_FIELD_CHARS = 4000
MAX_REPLY_CHARS = 2000
MAX_REPORT_REASON_CHARS = 500
MAX_HIDE_REASON_CHARS = 300
MAX_BODY_BYTES = 32 * 1024

# Write budgets per account. Rating writes count every history row.
RATING_WRITES_PER_HOUR = 10
RATING_WRITES_PER_DAY = 30
REPORTS_PER_DAY = 20
HELPFUL_VOTES_PER_DAY = 200
HOUR_MS = 60 * 60 * 1000
DAY_MS = 24 * HOUR_MS

DEFAULT_PAGE_SIZE = 10
MAX_PAGE_SIZE = 50
MAX_FEEDBACK_PAGE_SIZE = 200
SORTS = ("newest", "helpful")
FEEDBACK_SORTS = ("newest", "oldest", "lowest", "highest", "helpful")
FEEDBACK_STATUSES = ("all", "open", "addressed")

PACKAGE_RE = re.compile(r"^[a-z][a-z0-9]*(?:[.-][a-z0-9]+)*$")
REVIEW_ID_RE = re.compile(r"^[0-9a-f]{32}$")
VERSION_RE = re.compile(
    r"^v?(\d{1,6}(?:\.\d{1,6}){0,3})([-+][0-9A-Za-z.-]{1,40})?$")
# Five or more of the same letter in a row ("aaaaa"), or ten or more of any
# other visible character ("!!!!!!!!!!"): filler, never a real sentence.
REPEATED_LETTER_RE = re.compile(r"([^\W\d_])\1{4,}")
REPEATED_ANY_RE = re.compile(r"(\S)\1{9,}")
WORD_RE = re.compile(r"[^\W\d_]+(?:['\u2019-][^\W\d_]+)*")
CJK_RE = re.compile(r"[\u3040-\u30ff\u3400-\u4dbf\u4e00-\u9fff\uf900-\ufaff]")
CONTROL_RE = re.compile(r"[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]")

REVIEW_COLUMNS = (
    "review_id,market,package_id,reviewer_bi,reviewer_name,reviewer_display,"
    "stars,problem,improvement,context,comment,category,package_version,"
    "verified_use,helpful_count,report_count,hidden,hidden_by,hidden_reason,"
    "hidden_at,reply_text,reply_by,reply_at,addressed_version,addressed_by,"
    "addressed_at,revision,created_at,updated_at"
)

# The tables this module owns (migration 0198_marketplace_reviews.sql holds
# the same statements; the test keeps them in step). Applied once per isolate
# so a database that missed the migration still serves the route.
SCHEMA = (
    """CREATE TABLE IF NOT EXISTS marketplace_reviews (
    review_id TEXT PRIMARY KEY,
    market TEXT NOT NULL,
    package_id TEXT NOT NULL,
    reviewer_bi TEXT NOT NULL,
    reviewer_name TEXT NOT NULL,
    reviewer_display TEXT NOT NULL DEFAULT '',
    stars INTEGER NOT NULL CHECK (stars BETWEEN 1 AND 5),
    problem TEXT NOT NULL DEFAULT '',
    improvement TEXT NOT NULL DEFAULT '',
    context TEXT NOT NULL DEFAULT '',
    comment TEXT NOT NULL DEFAULT '',
    category TEXT NOT NULL DEFAULT '',
    package_version TEXT NOT NULL DEFAULT '',
    verified_use INTEGER NOT NULL DEFAULT 0,
    helpful_count INTEGER NOT NULL DEFAULT 0,
    report_count INTEGER NOT NULL DEFAULT 0,
    hidden INTEGER NOT NULL DEFAULT 0,
    hidden_by TEXT NOT NULL DEFAULT '',
    hidden_reason TEXT NOT NULL DEFAULT '',
    hidden_at INTEGER NOT NULL DEFAULT 0,
    reply_text TEXT NOT NULL DEFAULT '',
    reply_by TEXT NOT NULL DEFAULT '',
    reply_at INTEGER NOT NULL DEFAULT 0,
    addressed_version TEXT NOT NULL DEFAULT '',
    addressed_by TEXT NOT NULL DEFAULT '',
    addressed_at INTEGER NOT NULL DEFAULT 0,
    revision INTEGER NOT NULL DEFAULT 1,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    UNIQUE (market, package_id, reviewer_bi)
)""",
    """CREATE INDEX IF NOT EXISTS idx_marketplace_reviews_newest
    ON marketplace_reviews(market, package_id, hidden, updated_at)""",
    """CREATE INDEX IF NOT EXISTS idx_marketplace_reviews_helpful
    ON marketplace_reviews(market, package_id, hidden, helpful_count, updated_at)""",
    """CREATE INDEX IF NOT EXISTS idx_marketplace_reviews_reported
    ON marketplace_reviews(report_count, updated_at)""",
    """CREATE TABLE IF NOT EXISTS marketplace_review_history (
    review_id TEXT NOT NULL,
    revision INTEGER NOT NULL,
    reviewer_bi TEXT NOT NULL,
    stars INTEGER NOT NULL CHECK (stars BETWEEN 1 AND 5),
    problem TEXT NOT NULL DEFAULT '',
    improvement TEXT NOT NULL DEFAULT '',
    context TEXT NOT NULL DEFAULT '',
    comment TEXT NOT NULL DEFAULT '',
    category TEXT NOT NULL DEFAULT '',
    package_version TEXT NOT NULL DEFAULT '',
    created_at INTEGER NOT NULL,
    PRIMARY KEY (review_id, revision)
)""",
    """CREATE INDEX IF NOT EXISTS idx_marketplace_review_history_reviewer
    ON marketplace_review_history(reviewer_bi, created_at)""",
    """CREATE TABLE IF NOT EXISTS marketplace_review_votes (
    review_id TEXT NOT NULL,
    voter_bi TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    PRIMARY KEY (review_id, voter_bi)
)""",
    """CREATE INDEX IF NOT EXISTS idx_marketplace_review_votes_voter
    ON marketplace_review_votes(voter_bi, created_at)""",
    """CREATE TABLE IF NOT EXISTS marketplace_review_reports (
    review_id TEXT NOT NULL,
    reporter_bi TEXT NOT NULL,
    reporter_name TEXT NOT NULL,
    reason TEXT NOT NULL DEFAULT '',
    created_at INTEGER NOT NULL,
    resolved_at INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (review_id, reporter_bi)
)""",
    """CREATE INDEX IF NOT EXISTS idx_marketplace_review_reports_reporter
    ON marketplace_review_reports(reporter_bi, created_at)""",
    """CREATE TABLE IF NOT EXISTS marketplace_package_rating_stats (
    market TEXT NOT NULL,
    package_id TEXT NOT NULL,
    rating_count INTEGER NOT NULL DEFAULT 0,
    rating_total INTEGER NOT NULL DEFAULT 0,
    stars_1 INTEGER NOT NULL DEFAULT 0,
    stars_2 INTEGER NOT NULL DEFAULT 0,
    stars_3 INTEGER NOT NULL DEFAULT 0,
    stars_4 INTEGER NOT NULL DEFAULT 0,
    stars_5 INTEGER NOT NULL DEFAULT 0,
    updated_at INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (market, package_id)
)""",
    """CREATE TABLE IF NOT EXISTS marketplace_package_uses (
    market TEXT NOT NULL,
    package_id TEXT NOT NULL,
    account_bi TEXT NOT NULL,
    source TEXT NOT NULL DEFAULT 'client',
    first_at INTEGER NOT NULL,
    last_at INTEGER NOT NULL,
    PRIMARY KEY (market, package_id, account_bi)
)""",
)
ACTIVITY_SCHEMA = (
    """CREATE TABLE IF NOT EXISTS marketplace_package_activity (
    market TEXT NOT NULL,
    package_id TEXT NOT NULL,
    account_bi TEXT NOT NULL,
    account_name TEXT NOT NULL DEFAULT '',
    views INTEGER NOT NULL DEFAULT 0,
    opens INTEGER NOT NULL DEFAULT 0,
    first_at INTEGER NOT NULL,
    last_at INTEGER NOT NULL,
    PRIMARY KEY (market, package_id, account_bi)
)""",
    """CREATE INDEX IF NOT EXISTS idx_marketplace_package_activity_package
    ON marketplace_package_activity(market, package_id, opens, views)""",
)
ACTIVITY_KINDS = ("view", "open")
MAX_ACTIVITY_PAGE_SIZE = 200
_SCHEMA_READY = False


# -- Pure validation ------------------------------------------------------------

def clean_text(value, maximum=MAX_FIELD_CHARS + 1):
    """Normalize free text: keep line breaks, drop control characters."""
    if not isinstance(value, str):
        return ""
    text = CONTROL_RE.sub("", value.replace("\r\n", "\n").replace("\r", "\n"))
    lines = [" ".join(line.split()) for line in text.split("\n")]
    text = "\n".join(lines)
    text = re.sub(r"\n{3,}", "\n\n", text).strip()
    return text[:maximum]


def count_words(text):
    """Words of two or more letters; CJK runs count one word per two glyphs."""
    total = 0
    for token in WORD_RE.findall(text or ""):
        cjk = len(CJK_RE.findall(token))
        if cjk:
            total += max(1, cjk // 2)
        elif len(token) >= 2:
            total += 1
    return total


def field_issue(text, required):
    """The first rule a review field breaks, or "" when it is acceptable."""
    if not text:
        return "required" if required else ""
    if len(text) > MAX_FIELD_CHARS:
        return "too_long"
    if not required:
        return ""
    if REPEATED_LETTER_RE.search(text) or REPEATED_ANY_RE.search(text):
        return "repeated_characters"
    if len(text) < MIN_FIELD_CHARS:
        return "too_short"
    if count_words(text) < MIN_FIELD_WORDS:
        return "too_few_words"
    words = [word.casefold() for word in WORD_RE.findall(text) if len(word) >= 2]
    if not CJK_RE.search(text):
        distinct = set(words)
        most = max((words.count(word) for word in distinct), default=0)
        if len(distinct) < MIN_DISTINCT_WORDS or most * 2 > len(words):
            return "repetitive"
    return ""


def parse_stars(value):
    if isinstance(value, bool):
        return 0
    if isinstance(value, str) and value in ("1", "2", "3", "4", "5"):
        return int(value)
    if isinstance(value, int) and 1 <= value <= 5:
        return value
    return 0


def normalize_version(value):
    """``"1.2"`` / ``"v1.2.3"`` -> ``"v1.2"`` / ``"v1.2.3"``; ``None`` if bad."""
    raw = str(value or "").strip()
    if not raw:
        return ""
    if len(raw) > 64:
        return None
    match = VERSION_RE.fullmatch(raw)
    if not match:
        return None
    return "v" + match.group(1) + (match.group(2) or "")


def review_fields(payload):
    """Validate one rating write.

    Returns ``(fields, None)`` or ``(None, (error, field_errors))``. A rating
    below five stars requires ``problem`` and ``improvement`` (at least
    MIN_FIELD_CHARS characters and MIN_FIELD_WORDS words each, no filler);
    ``context``, ``comment`` and ``category`` stay optional. Five stars needs
    no text at all.
    """
    if not isinstance(payload, dict):
        return None, ("invalid_rating", {})
    stars = parse_stars(payload.get("stars", payload.get("rating")))
    if not stars:
        return None, ("invalid_rating", {"stars": "invalid"})
    problem = clean_text(payload.get("problem"))
    improvement = clean_text(payload.get("improvement"))
    context = clean_text(payload.get("context"))
    comment = clean_text(payload.get("comment"))
    category = str(payload.get("category") or "").strip().lower()
    category = category.replace("_", "-").replace(" ", "-")
    needs_review = stars < 5
    errors = {}
    for name, text in (("problem", problem), ("improvement", improvement)):
        issue = field_issue(text, needs_review)
        if issue:
            errors[name] = issue
    for name, text in (("context", context), ("comment", comment)):
        if len(text) > MAX_FIELD_CHARS:
            errors[name] = "too_long"
    if (needs_review and "problem" not in errors
            and "improvement" not in errors
            and problem.casefold() == improvement.casefold()):
        errors["improvement"] = "same_as_problem"
    if category and category not in CATEGORIES:
        errors["category"] = "invalid"
    version = normalize_version(payload.get("version"))
    if version is None:
        errors["version"] = "invalid"
    if errors:
        missing = needs_review and not problem and not improvement
        return None, ("review_required" if missing else "review_invalid",
                      errors)
    return {
        "stars": stars,
        "problem": problem,
        "improvement": improvement,
        "context": context,
        "comment": comment,
        "category": category,
        "version": version or "",
    }, None


def review_id_for(market, package_id, reviewer_bi):
    canonical = "\n".join((
        "forkmesh-market-review-v1", market, package_id, str(reviewer_bi)))
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()[:32]


def summary_from_row(row):
    row = row or {}
    histogram = {str(star): max(0, int(row.get("stars_%d" % star) or 0))
                 for star in range(1, 6)}
    count = max(0, int(row.get("rating_count") or 0))
    total = max(0, int(row.get("rating_total") or 0))
    average = round(total / count, 1) if count else 0.0
    return {"average": average, "count": count, "histogram": histogram}


def rules_payload():
    return {
        "reviewRequiredBelow": 5,
        "minChars": MIN_FIELD_CHARS,
        "minWords": MIN_FIELD_WORDS,
        "maxChars": MAX_FIELD_CHARS,
        "categories": [
            {"id": key, "label": CATEGORY_LABELS[key]} for key in CATEGORIES],
    }


# -- Routing --------------------------------------------------------------------

def route(path):
    """``(kind, args)`` for a ``/api/market/reviews`` path, or ``None``."""
    base = "/api/market/reviews"
    if path != base and not path.startswith(base + "/"):
        return None
    parts = [part for part in path[len(base):].split("/") if part]
    if not parts:
        return None
    head = parts[0]
    if head == "summary" and len(parts) == 1:
        return "summary", ()
    if head == "maintained" and len(parts) == 1:
        return "maintained", ()
    if head == "reports" and len(parts) == 1:
        return "reports", ()
    if head == "uses" and len(parts) == 1:
        return "uses", ()
    if head == "activity" and len(parts) == 1:
        return "activity", ()
    if head == "items" and len(parts) == 3 and REVIEW_ID_RE.fullmatch(parts[1]):
        if parts[2] in ("history", "reply", "addressed", "helpful", "report",
                        "moderation"):
            return "item", (parts[1], parts[2])
        return None
    if head in MARKETS and len(parts) in (2, 3) and PACKAGE_RE.fullmatch(parts[1]):
        if len(parts) == 2:
            return "package", (head, parts[1])
        if parts[2] == "feedback":
            return "feedback", (head, parts[1])
        if parts[2] == "activity":
            return "people", (head, parts[1])
    return None


def _response(runtime, payload, status=200, allow=None, cache="no-store"):
    headers = {"x-content-type-options": "nosniff"}
    if allow:
        headers["allow"] = allow
    return runtime.response(payload, status=status, cache_control=cache,
                            extra_headers=headers)


def _int_param(query, name, default, low, high):
    try:
        value = int(str(query.get(name) or default))
    except (TypeError, ValueError):
        value = default
    return max(low, min(high, value))


async def _ensure_tables(runtime):
    global _SCHEMA_READY
    if _SCHEMA_READY:
        return
    for statement in SCHEMA + ACTIVITY_SCHEMA:
        await runtime.d1_run(statement)
    _SCHEMA_READY = True


# -- Package, maintainers, and eligibility ------------------------------------

async def _package(runtime, market, package_id):
    """``{"market", "id", "name", "version", "maintainers": {bi: name}}``."""
    if market == FIRST_PARTY_MARKET:
        name = FIRST_PARTY_PACKAGES.get(package_id)
        if not name:
            return None
        org_bi = await runtime.blind_index("org:" + FIRST_PARTY_ORG)
        rows = await runtime.d1_all(
            "SELECT member_bi,name FROM org_members WHERE org_bi=? "
            "AND role IN ('owner','admin','maintainer') ORDER BY created_at "
            "LIMIT ?", org_bi, MAX_NOTIFIED_MAINTAINERS)
        return {
            "market": market, "id": package_id, "name": name, "version": "",
            "maintainers": {
                str(row.get("member_bi") or ""): str(row.get("name") or "").lower()
                for row in rows or [] if row.get("member_bi")},
        }
    if market == APPS_MARKET:
        app = await runtime.d1_first(
            "SELECT owner_bi, org_bi FROM apps WHERE slug=? "
            "AND listing='listed'", package_id)
        if not app:
            return None
        owner = await runtime.d1_first(
            "SELECT username FROM users WHERE user_bi=?", app["owner_bi"])
        maintainers = {str(app["owner_bi"]):
                       str((owner or {}).get("username") or "").lower()}
        if app.get("org_bi"):
            for row in await runtime.d1_all(
                    "SELECT member_bi,name FROM org_members WHERE org_bi=? "
                    "AND role IN ('owner','admin') LIMIT ?", app["org_bi"],
                    MAX_NOTIFIED_MAINTAINERS) or []:
                maintainers[str(row.get("member_bi") or "")] = str(
                    row.get("name") or "").lower()
        return {"market": market, "id": package_id, "name": package_id,
                "version": "", "maintainers": maintainers}
    if market == COMMUNITY_MARKET:
        rows = await runtime.d1_all(
            "SELECT version,submitted_by_bi,submitted_by_name FROM "
            "marketplace_submissions WHERE marketplace_id=? AND package_id=? "
            "AND state='published' ORDER BY published_at DESC LIMIT 20",
            market, package_id)
        if not rows:
            return None
        return {
            "market": market, "id": package_id, "name": package_id,
            "version": "v" + str(rows[0].get("version") or "").lstrip("v"),
            "maintainers": {
                str(row.get("submitted_by_bi") or ""):
                    str(row.get("submitted_by_name") or "").lower()
                for row in rows if row.get("submitted_by_bi")},
        }
    return None


async def _use_state(runtime, package, viewer_bi):
    """``(use_required, viewer_has_use)`` for the install/use gate.

    A package "has use records" once any account reported installing or using
    it, or any organization installed it. From then on a rating needs the
    rater's own record, so a drive-by account cannot rate a package it never
    ran. Packages with no records yet accept ratings, marked unverified.
    """
    market, package_id = package["market"], package["id"]
    if market == APPS_MARKET:
        installed = viewer_bi and await runtime.d1_first(
            "SELECT 1 AS ok FROM app_installs i JOIN apps a "
            "ON a.app_id=i.app_id WHERE a.slug=? AND (i.installed_by_bi=? "
            "OR (i.target_kind='org' AND i.target_bi IN (SELECT org_bi "
            "FROM org_members WHERE member_bi=?))) LIMIT 1",
            package_id, str(viewer_bi), str(viewer_bi))
        return True, bool(installed)
    mine = await runtime.d1_first(
        "SELECT 1 AS ok FROM marketplace_package_uses WHERE market=? "
        "AND package_id=? AND account_bi=?", market, package_id, str(viewer_bi))
    if not mine and market == FIRST_PARTY_MARKET and viewer_bi:
        mine = await runtime.d1_first(
            "SELECT 1 AS ok FROM org_plugin_installs WHERE plugin=? AND "
            "(installed_by_bi=? OR org_bi IN (SELECT org_bi FROM org_members "
            "WHERE member_bi=?)) LIMIT 1", package_id, str(viewer_bi),
            str(viewer_bi))
    if mine:
        return True, True
    any_use = await runtime.d1_first(
        "SELECT 1 AS ok FROM marketplace_package_uses WHERE market=? "
        "AND package_id=? LIMIT 1", market, package_id)
    if not any_use and market == FIRST_PARTY_MARKET:
        any_use = await runtime.d1_first(
            "SELECT 1 AS ok FROM org_plugin_installs WHERE plugin=? LIMIT 1",
            package_id)
    return bool(any_use), False


def _viewer_verified(rec):
    return bool(rec) and bool(rec.get("email_verified")) and str(
        rec.get("status") or "active") == "active"


async def _viewer_context(runtime, package, viewer_bi, viewer, is_admin):
    """What the signed-in viewer may do with this package's ratings."""
    name = str((viewer or {}).get("name") or "").lower()
    maintainer = bool(viewer_bi) and str(viewer_bi) in package["maintainers"]
    context = {
        "signedIn": bool(viewer_bi),
        "name": name,
        "emailVerified": bool((viewer or {}).get("email_verified")),
        "isMaintainer": maintainer,
        "isAdmin": bool(is_admin),
        "canRate": False,
        "reason": "",
        "useRequired": False,
        "verifiedUse": False,
    }
    if not viewer_bi:
        context["reason"] = "sign_in_required"
        return context
    required, has_use = await _use_state(runtime, package, viewer_bi)
    context["useRequired"] = required
    context["verifiedUse"] = has_use
    if not _viewer_verified(viewer):
        context["reason"] = "email_not_verified"
    elif maintainer:
        context["reason"] = "maintainer_cannot_rate"
    elif required and not has_use:
        context["reason"] = "package_use_required"
    else:
        context["canRate"] = True
    return context


# -- Projection -------------------------------------------------------------------

def _project(row, *, full=False, voted=False):
    reply = None
    if row.get("reply_text"):
        reply = {
            "text": str(row.get("reply_text") or ""),
            "by": str(row.get("reply_by") or ""),
            "at": int(row.get("reply_at") or 0),
        }
    revision = int(row.get("revision") or 1)
    updated = int(row.get("updated_at") or 0)
    item = {
        "id": str(row.get("review_id") or ""),
        "market": str(row.get("market") or ""),
        "package": str(row.get("package_id") or ""),
        "stars": int(row.get("stars") or 0),
        "reviewer": {
            "name": str(row.get("reviewer_name") or ""),
            "displayName": str(row.get("reviewer_display") or "")
            or str(row.get("reviewer_name") or ""),
        },
        "problem": str(row.get("problem") or ""),
        "improvement": str(row.get("improvement") or ""),
        "context": str(row.get("context") or ""),
        "comment": str(row.get("comment") or ""),
        "category": str(row.get("category") or ""),
        "version": str(row.get("package_version") or ""),
        "verifiedUse": bool(int(row.get("verified_use") or 0)),
        "helpful": int(row.get("helpful_count") or 0),
        "viewerFoundHelpful": bool(voted),
        "createdAt": int(row.get("created_at") or 0),
        "updatedAt": updated,
        "edited": revision > 1,
        "revision": revision,
        "reply": reply,
        "editedAfterReply": bool(reply) and updated > reply["at"],
        "addressedIn": str(row.get("addressed_version") or ""),
        "addressedAt": int(row.get("addressed_at") or 0),
    }
    if full:
        item["hidden"] = bool(int(row.get("hidden") or 0))
        item["hiddenReason"] = str(row.get("hidden_reason") or "")
        item["reportCount"] = int(row.get("report_count") or 0)
    return item


async def _voted_ids(runtime, viewer_bi, rows):
    ids = [str(row.get("review_id") or "") for row in rows or []]
    if not viewer_bi or not ids:
        return set()
    marks = ",".join("?" for _ in ids)
    voted = await runtime.d1_all(
        "SELECT review_id FROM marketplace_review_votes WHERE voter_bi=? "
        "AND review_id IN (" + marks + ")", str(viewer_bi), *ids)
    return {str(row.get("review_id") or "") for row in voted or []}


async def _summary(runtime, market, package_id):
    row = await runtime.d1_first(
        "SELECT * FROM marketplace_package_rating_stats WHERE market=? "
        "AND package_id=?", market, package_id)
    return summary_from_row(row)


async def _refresh_stats(runtime, market, package_id):
    """Recompute and cache one package's aggregate from its visible ratings."""
    rows = await runtime.d1_all(
        "SELECT stars, COUNT(*) AS n FROM marketplace_reviews WHERE market=? "
        "AND package_id=? AND hidden=0 GROUP BY stars", market, package_id)
    counts = {star: 0 for star in range(1, 6)}
    for row in rows or []:
        star = int(row.get("stars") or 0)
        if star in counts:
            counts[star] = int(row.get("n") or 0)
    total_count = sum(counts.values())
    total = sum(star * n for star, n in counts.items())
    await runtime.d1_run(
        "INSERT INTO marketplace_package_rating_stats (market,package_id,"
        "rating_count,rating_total,stars_1,stars_2,stars_3,stars_4,stars_5,"
        "updated_at) VALUES (?,?,?,?,?,?,?,?,?,?) ON CONFLICT(market,package_id) "
        "DO UPDATE SET rating_count=excluded.rating_count,"
        "rating_total=excluded.rating_total,stars_1=excluded.stars_1,"
        "stars_2=excluded.stars_2,stars_3=excluded.stars_3,"
        "stars_4=excluded.stars_4,stars_5=excluded.stars_5,"
        "updated_at=excluded.updated_at",
        market, package_id, total_count, total, counts[1], counts[2],
        counts[3], counts[4], counts[5], runtime.now())
    return summary_from_row({
        "rating_count": total_count, "rating_total": total,
        **{"stars_%d" % star: n for star, n in counts.items()}})


async def _review(runtime, review_id):
    return await runtime.d1_first(
        "SELECT " + REVIEW_COLUMNS + " FROM marketplace_reviews "
        "WHERE review_id=?", review_id)


def _package_href(market, package_id, review_id=""):
    href = "/market-reviews?market=%s&package=%s" % (market, package_id)
    return href + ("#review-" + review_id if review_id else "")


# -- Handlers -------------------------------------------------------------------

async def _get_summary(runtime):
    rows = await runtime.d1_all(
        "SELECT * FROM marketplace_package_rating_stats WHERE rating_count>0 "
        "ORDER BY market, package_id")
    markets = {}
    for row in rows or []:
        market = str(row.get("market") or "")
        package_id = str(row.get("package_id") or "")
        if market in MARKETS and package_id:
            markets.setdefault(market, {})[package_id] = summary_from_row(row)
    usage_rows = await runtime.d1_all(
        "SELECT market, package_id, COALESCE(SUM(views),0) AS views, "
        "COALESCE(SUM(opens),0) AS opens, COUNT(*) AS people FROM "
        "marketplace_package_activity GROUP BY market, package_id "
        "ORDER BY market, package_id")
    usage = {}
    for row in usage_rows or []:
        market = str(row.get("market") or "")
        package_id = str(row.get("package_id") or "")
        if market in MARKETS and package_id:
            usage.setdefault(market, {})[package_id] = {
                "views": int(row.get("views") or 0),
                "opens": int(row.get("opens") or 0),
                "people": int(row.get("people") or 0),
            }
    return _response(runtime, {
        "ok": True,
        "packages": markets.get(FIRST_PARTY_MARKET, {}),
        "marketplaces": markets,
        "usage": usage.get(FIRST_PARTY_MARKET, {}),
        "usageByMarketplace": usage,
        "observedAt": runtime.now(),
    }, cache="public, max-age=60")


async def _get_package(runtime, package, viewer_bi, viewer, is_admin):
    query = runtime.query()
    sort = str(query.get("sort") or "newest")
    sort = sort if sort in SORTS else "newest"
    limit = _int_param(query, "limit", DEFAULT_PAGE_SIZE, 1, MAX_PAGE_SIZE)
    page = _int_param(query, "page", 1, 1, 10000)
    stars = parse_stars(str(query.get("stars") or ""))
    include_hidden = is_admin and str(query.get("includeHidden") or "") in (
        "1", "true")
    where = ["market=?", "package_id=?"]
    args = [package["market"], package["id"]]
    if not include_hidden:
        where.append("hidden=0")
    if stars:
        where.append("stars=?")
        args.append(stars)
    clause = " AND ".join(where)
    order = ("helpful_count DESC, updated_at DESC, review_id"
             if sort == "helpful" else "updated_at DESC, review_id")
    rows = await runtime.d1_all(
        "SELECT " + REVIEW_COLUMNS + " FROM marketplace_reviews WHERE "
        + clause + " ORDER BY " + order + " LIMIT ? OFFSET ?",
        *args, limit + 1, (page - 1) * limit)
    rows = list(rows or [])
    has_more = len(rows) > limit
    rows = rows[:limit]
    total_row = await runtime.d1_first(
        "SELECT COUNT(*) AS n FROM marketplace_reviews WHERE " + clause, *args)
    voted = await _voted_ids(runtime, viewer_bi, rows)
    viewer_context = await _viewer_context(
        runtime, package, viewer_bi, viewer, is_admin)
    if viewer_bi:
        own = await _review(runtime, review_id_for(
            package["market"], package["id"], viewer_bi))
        viewer_context["review"] = _project(own, full=True) if own else None
    viewer_context.update(await _own_activity(
        runtime, package["market"], package["id"], viewer_bi))
    return _response(runtime, {
        "ok": True,
        "market": package["market"],
        "package": package["id"],
        "packageName": package["name"],
        "latestVersion": package["version"],
        "summary": await _summary(runtime, package["market"], package["id"]),
        "activity": await _activity_totals(
            runtime, package["market"], package["id"]),
        "sort": sort,
        "page": page,
        "limit": limit,
        "total": int((total_row or {}).get("n") or 0),
        "hasMore": has_more,
        "reviews": [
            _project(row, full=bool(is_admin),
                     voted=str(row.get("review_id")) in voted)
            for row in rows],
        "viewer": viewer_context,
        "rules": rules_payload(),
    })


async def _put_rating(runtime, package, viewer_bi, viewer, is_admin, data):
    context = await _viewer_context(
        runtime, package, viewer_bi, viewer, is_admin)
    if not context["canRate"]:
        reason = context["reason"]
        status = 401 if reason == "sign_in_required" else 403
        payload = {"error": reason}
        if reason == "email_not_verified":
            payload["verifyUrl"] = "/verify-pending"
        return _response(runtime, payload, status)
    now = runtime.now()
    recent = await runtime.d1_first(
        "SELECT SUM(CASE WHEN created_at>=? THEN 1 ELSE 0 END) AS hour,"
        "COUNT(*) AS day FROM marketplace_review_history WHERE reviewer_bi=? "
        "AND created_at>=?", now - HOUR_MS, str(viewer_bi), now - DAY_MS) or {}
    if (int(recent.get("hour") or 0) >= RATING_WRITES_PER_HOUR
            or int(recent.get("day") or 0) >= RATING_WRITES_PER_DAY):
        return _response(runtime, {
            "error": "rating_rate_limited",
            "limits": {"perHour": RATING_WRITES_PER_HOUR,
                       "perDay": RATING_WRITES_PER_DAY},
        }, 429)
    fields, problem = review_fields(data)
    if problem:
        error, field_errors = problem
        return _response(runtime, {
            "error": error,
            "fields": field_errors,
            "rules": rules_payload(),
        }, 400)
    version = fields["version"] or package["version"]
    market, package_id = package["market"], package["id"]
    review_id = review_id_for(market, package_id, viewer_bi)
    existing = await _review(runtime, review_id)
    name = str(viewer.get("name") or "").lower()
    display = " ".join(str(
        viewer.get("displayName") or viewer.get("display_name") or ""
    ).split())[:80]
    verified = 1 if context["verifiedUse"] else 0
    if existing and all((
            int(existing.get("stars") or 0) == fields["stars"],
            str(existing.get("problem") or "") == fields["problem"],
            str(existing.get("improvement") or "") == fields["improvement"],
            str(existing.get("context") or "") == fields["context"],
            str(existing.get("comment") or "") == fields["comment"],
            str(existing.get("category") or "") == fields["category"],
            str(existing.get("package_version") or "") == version)):
        return _response(runtime, {
            "ok": True, "unchanged": True,
            "review": _project(existing, full=True),
            "summary": await _summary(runtime, market, package_id),
        })
    revision = int((existing or {}).get("revision") or 0) + 1
    if existing:
        await runtime.d1_run(
            "UPDATE marketplace_reviews SET stars=?,problem=?,improvement=?,"
            "context=?,comment=?,category=?,package_version=?,verified_use=?,"
            "reviewer_name=?,reviewer_display=?,revision=?,updated_at=? "
            "WHERE review_id=?",
            fields["stars"], fields["problem"], fields["improvement"],
            fields["context"], fields["comment"], fields["category"], version,
            verified, name, display, revision, now, review_id)
    else:
        await runtime.d1_run(
            "INSERT INTO marketplace_reviews (review_id,market,package_id,"
            "reviewer_bi,reviewer_name,reviewer_display,stars,problem,"
            "improvement,context,comment,category,package_version,"
            "verified_use,revision,created_at,updated_at) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            review_id, market, package_id, str(viewer_bi), name, display,
            fields["stars"], fields["problem"], fields["improvement"],
            fields["context"], fields["comment"], fields["category"], version,
            verified, revision, now, now)
    await runtime.d1_run(
        "INSERT INTO marketplace_review_history (review_id,revision,"
        "reviewer_bi,stars,problem,improvement,context,comment,category,"
        "package_version,created_at) VALUES (?,?,?,?,?,?,?,?,?,?,?)",
        review_id, revision, str(viewer_bi), fields["stars"],
        fields["problem"], fields["improvement"], fields["context"],
        fields["comment"], fields["category"], version, now)
    summary = await _refresh_stats(runtime, market, package_id)
    notified = []
    if fields["stars"] < 5:
        notified = await _notify_maintainers(
            runtime, package, review_id, revision, fields, name)
    row = await _review(runtime, review_id)
    return _response(runtime, {
        "ok": True,
        "created": not existing,
        "review": _project(row, full=True),
        "summary": summary,
        "notified": len(notified),
    }, 201 if not existing else 200)


async def _notify_maintainers(runtime, package, review_id, revision, fields,
                              reviewer):
    stars = fields["stars"]
    label = CATEGORY_LABELS.get(fields["category"], "")
    title = "%d-star review of %s: how to improve" % (stars, package["name"])
    body = fields["improvement"]
    if label:
        body = "[" + label + "] " + body
    sent = []
    for maintainer_bi, name in sorted(package["maintainers"].items(),
                                      key=lambda item: item[1]):
        if not name or name == reviewer or name in sent:
            continue
        try:
            delivered = await runtime.notify(
                name,
                title,
                body=body,
                href=_package_href(package["market"], package["id"], review_id),
                actor=reviewer,
                dedupe="package-review:%s:%d" % (review_id, revision),
                meta={
                    "market": package["market"], "package": package["id"],
                    "reviewId": review_id, "stars": stars,
                    "category": fields["category"],
                },
            )
        except Exception:
            delivered = False
        if delivered:
            sent.append(name)
        if len(sent) >= MAX_NOTIFIED_MAINTAINERS:
            break
    return sent


async def _get_feedback(runtime, package, viewer_bi, is_admin):
    if not viewer_bi:
        return _response(runtime, {"error": "sign_in_required"}, 401)
    if not is_admin and str(viewer_bi) not in package["maintainers"]:
        return _response(runtime, {"error": "maintainer_required"}, 403)
    query = runtime.query()
    category = str(query.get("category") or "").strip().lower()
    status = str(query.get("status") or "all")
    status = status if status in FEEDBACK_STATUSES else "all"
    sort = str(query.get("sort") or "newest")
    sort = sort if sort in FEEDBACK_SORTS else "newest"
    limit = _int_param(query, "limit", 50, 1, MAX_FEEDBACK_PAGE_SIZE)
    page = _int_param(query, "page", 1, 1, 10000)
    base = ["market=?", "package_id=?", "hidden=0", "stars<5"]
    args = [package["market"], package["id"]]
    counts = await runtime.d1_all(
        "SELECT category, CASE WHEN addressed_version<>'' THEN 1 ELSE 0 END "
        "AS addressed, COUNT(*) AS n FROM marketplace_reviews WHERE "
        + " AND ".join(base) + " GROUP BY category, addressed", *args)
    categories = {key: 0 for key in CATEGORIES}
    categories["uncategorized"] = 0
    statuses = {"open": 0, "addressed": 0}
    for row in counts or []:
        key = str(row.get("category") or "") or "uncategorized"
        n = int(row.get("n") or 0)
        categories[key] = categories.get(key, 0) + n
        statuses["addressed" if int(row.get("addressed") or 0) else "open"] += n
    where = list(base)
    if category == "uncategorized":
        where.append("category=''")
    elif category in CATEGORIES:
        where.append("category=?")
        args.append(category)
    if status == "open":
        where.append("addressed_version=''")
    elif status == "addressed":
        where.append("addressed_version<>''")
    order = {
        "newest": "updated_at DESC",
        "oldest": "updated_at ASC",
        "lowest": "stars ASC, updated_at DESC",
        "highest": "stars DESC, updated_at DESC",
        "helpful": "helpful_count DESC, updated_at DESC",
    }[sort]
    rows = await runtime.d1_all(
        "SELECT " + REVIEW_COLUMNS + " FROM marketplace_reviews WHERE "
        + " AND ".join(where) + " ORDER BY " + order + ", review_id "
        "LIMIT ? OFFSET ?", *args, limit + 1, (page - 1) * limit)
    rows = list(rows or [])
    return _response(runtime, {
        "ok": True,
        "market": package["market"],
        "package": package["id"],
        "packageName": package["name"],
        "summary": await _summary(runtime, package["market"], package["id"]),
        "categories": categories,
        "statuses": statuses,
        "filter": {"category": category, "status": status, "sort": sort},
        "page": page,
        "limit": limit,
        "hasMore": len(rows) > limit,
        "items": [_project(row, full=True) for row in rows[:limit]],
        "rules": rules_payload(),
    })


async def _get_maintained(runtime, viewer_bi, is_admin):
    if not viewer_bi:
        return _response(runtime, {"error": "sign_in_required"}, 401)
    packages = []
    org_bi = await runtime.blind_index("org:" + FIRST_PARTY_ORG)
    role = await runtime.d1_first(
        "SELECT role FROM org_members WHERE org_bi=? AND member_bi=?",
        org_bi, str(viewer_bi))
    if is_admin or str((role or {}).get("role") or "") in MAINTAINER_ORG_ROLES:
        packages.extend((FIRST_PARTY_MARKET, key, name)
                        for key, name in sorted(FIRST_PARTY_PACKAGES.items()))
    rows = await runtime.d1_all(
        "SELECT DISTINCT package_id FROM marketplace_submissions WHERE "
        "marketplace_id=? AND state='published'" + (
            "" if is_admin else " AND submitted_by_bi=?")
        + " ORDER BY package_id LIMIT 200",
        COMMUNITY_MARKET, *(() if is_admin else (str(viewer_bi),)))
    packages.extend((COMMUNITY_MARKET, str(row.get("package_id") or ""),
                     str(row.get("package_id") or "")) for row in rows or [])
    open_rows = await runtime.d1_all(
        "SELECT market, package_id, COUNT(*) AS n FROM marketplace_reviews "
        "WHERE hidden=0 AND stars<5 AND addressed_version='' "
        "GROUP BY market, package_id")
    open_counts = {(str(row.get("market")), str(row.get("package_id"))):
                   int(row.get("n") or 0) for row in open_rows or []}
    stats = await runtime.d1_all(
        "SELECT * FROM marketplace_package_rating_stats")
    by_key = {(str(row.get("market")), str(row.get("package_id"))): row
              for row in stats or []}
    return _response(runtime, {
        "ok": True,
        "packages": [{
            "market": market, "package": key, "packageName": name,
            "summary": summary_from_row(by_key.get((market, key))),
            "openFeedback": open_counts.get((market, key), 0),
        } for market, key, name in packages if key],
    })


async def _get_reports(runtime, is_admin):
    if not is_admin:
        return _response(runtime, {"error": "admin_required"}, 403)
    rows = await runtime.d1_all(
        "SELECT " + REVIEW_COLUMNS + " FROM marketplace_reviews WHERE "
        "report_count>0 ORDER BY hidden ASC, report_count DESC, updated_at "
        "DESC LIMIT 100")
    items = []
    for row in rows or []:
        reports = await runtime.d1_all(
            "SELECT reporter_name, reason, created_at FROM "
            "marketplace_review_reports WHERE review_id=? AND resolved_at=0 "
            "ORDER BY created_at LIMIT 20", str(row.get("review_id")))
        item = _project(row, full=True)
        item["reports"] = [{
            "by": str(report.get("reporter_name") or ""),
            "reason": str(report.get("reason") or ""),
            "at": int(report.get("created_at") or 0),
        } for report in reports or []]
        items.append(item)
    return _response(runtime, {"ok": True, "items": items})


async def _post_use(runtime, viewer_bi, data):
    if not viewer_bi:
        return _response(runtime, {"error": "sign_in_required"}, 401)
    market = str(data.get("market") or FIRST_PARTY_MARKET).strip().lower()
    package_id = str(data.get("package") or "").strip().lower()
    if market not in MARKETS or not PACKAGE_RE.fullmatch(package_id):
        return _response(runtime, {"error": "unknown_package"}, 404)
    package = await _package(runtime, market, package_id)
    if not package:
        return _response(runtime, {"error": "unknown_package"}, 404)
    source = str(data.get("source") or "client").strip().lower()
    if source not in ("client", "desktop", "mobile", "web"):
        source = "client"
    now = runtime.now()
    await runtime.d1_run(
        "INSERT INTO marketplace_package_uses (market,package_id,account_bi,"
        "source,first_at,last_at) VALUES (?,?,?,?,?,?) ON CONFLICT(market,"
        "package_id,account_bi) DO UPDATE SET last_at=excluded.last_at,"
        "source=excluded.source", market, package_id, str(viewer_bi), source,
        now, now)
    return _response(runtime, {"ok": True, "market": market,
                               "package": package_id})


async def _activity_totals(runtime, market, package_id):
    row = await runtime.d1_first(
        "SELECT COALESCE(SUM(views),0) AS views, COALESCE(SUM(opens),0) AS "
        "opens, COUNT(*) AS people FROM marketplace_package_activity WHERE "
        "market=? AND package_id=?", market, package_id)
    row = row or {}
    return {"views": int(row.get("views") or 0),
            "opens": int(row.get("opens") or 0),
            "people": int(row.get("people") or 0)}


async def _own_activity(runtime, market, package_id, viewer_bi):
    if not viewer_bi:
        return {"views": 0, "opens": 0}
    row = await runtime.d1_first(
        "SELECT views, opens FROM marketplace_package_activity WHERE market=? "
        "AND package_id=? AND account_bi=?", market, package_id,
        str(viewer_bi))
    row = row or {}
    return {"views": int(row.get("views") or 0),
            "opens": int(row.get("opens") or 0)}


async def _post_activity(runtime, viewer_bi, viewer, data):
    if not viewer_bi:
        return _response(runtime, {"error": "sign_in_required"}, 401)
    kind = str(data.get("kind") or "").strip().lower()
    if kind not in ACTIVITY_KINDS:
        return _response(runtime, {"error": "invalid_kind"}, 400)
    market = str(data.get("market") or FIRST_PARTY_MARKET).strip().lower()
    package_id = str(data.get("package") or "").strip().lower()
    if market not in MARKETS or not PACKAGE_RE.fullmatch(package_id):
        return _response(runtime, {"error": "unknown_package"}, 404)
    package = await _package(runtime, market, package_id)
    if not package:
        return _response(runtime, {"error": "unknown_package"}, 404)
    name = str((viewer or {}).get("name") or "").lower()
    views, opens = (1, 0) if kind == "view" else (0, 1)
    now = runtime.now()
    await runtime.d1_run(
        "INSERT INTO marketplace_package_activity (market,package_id,"
        "account_bi,account_name,views,opens,first_at,last_at) VALUES "
        "(?,?,?,?,?,?,?,?) ON CONFLICT(market,package_id,account_bi) DO "
        "UPDATE SET views=views+excluded.views,opens=opens+excluded.opens,"
        "account_name=excluded.account_name,last_at=excluded.last_at",
        market, package_id, str(viewer_bi), name, views, opens, now, now)
    if kind == "open":
        source = str(data.get("source") or "client").strip().lower()
        if source not in ("client", "desktop", "mobile", "web"):
            source = "client"
        await runtime.d1_run(
            "INSERT INTO marketplace_package_uses (market,package_id,"
            "account_bi,source,first_at,last_at) VALUES (?,?,?,?,?,?) ON "
            "CONFLICT(market,package_id,account_bi) DO UPDATE SET "
            "last_at=excluded.last_at,source=excluded.source", market,
            package_id, str(viewer_bi), source, now, now)
    own = await _own_activity(runtime, market, package_id, viewer_bi)
    return _response(runtime, {"ok": True, "market": market,
                               "package": package_id, "kind": kind, **own})


async def _get_people(runtime, package, viewer_bi, is_admin):
    if not viewer_bi:
        return _response(runtime, {"error": "sign_in_required"}, 401)
    if not is_admin and str(viewer_bi) not in package["maintainers"]:
        return _response(runtime, {"error": "maintainer_required"}, 403)
    query = runtime.query()
    limit = _int_param(query, "limit", 50, 1, MAX_ACTIVITY_PAGE_SIZE)
    page = _int_param(query, "page", 1, 1, 10000)
    rows = await runtime.d1_all(
        "SELECT account_name,views,opens,first_at,last_at FROM "
        "marketplace_package_activity WHERE market=? AND package_id=? "
        "ORDER BY opens DESC, views DESC, last_at DESC, account_bi "
        "LIMIT ? OFFSET ?", package["market"], package["id"], limit + 1,
        (page - 1) * limit)
    rows = list(rows or [])
    return _response(runtime, {
        "ok": True,
        "market": package["market"],
        "package": package["id"],
        "packageName": package["name"],
        "totals": await _activity_totals(
            runtime, package["market"], package["id"]),
        "page": page,
        "limit": limit,
        "hasMore": len(rows) > limit,
        "people": [{
            "name": str(row.get("account_name") or ""),
            "views": int(row.get("views") or 0),
            "opens": int(row.get("opens") or 0),
            "firstAt": int(row.get("first_at") or 0),
            "lastAt": int(row.get("last_at") or 0),
        } for row in rows[:limit]],
    })


async def _item(runtime, review_id, action, viewer_bi, viewer, is_admin, data):
    method = runtime.method()
    row = await _review(runtime, review_id)
    if not row:
        return _response(runtime, {"error": "not_found"}, 404)
    if not viewer_bi:
        return _response(runtime, {"error": "sign_in_required"}, 401)
    market, package_id = str(row.get("market")), str(row.get("package_id"))
    package = await _package(runtime, market, package_id) or {
        "market": market, "id": package_id, "name": package_id,
        "version": "", "maintainers": {}}
    maintainer = str(viewer_bi) in package["maintainers"]
    author = str(row.get("reviewer_bi") or "") == str(viewer_bi)
    name = str((viewer or {}).get("name") or "").lower()
    now = runtime.now()

    if action == "history":
        if method != "GET":
            return _response(runtime, {"error": "method_not_allowed"}, 405, "GET")
        if not (author or maintainer or is_admin):
            return _response(runtime, {"error": "forbidden"}, 403)
        rows = await runtime.d1_all(
            "SELECT revision,stars,problem,improvement,context,comment,"
            "category,package_version,created_at FROM "
            "marketplace_review_history WHERE review_id=? ORDER BY revision",
            review_id)
        return _response(runtime, {"ok": True, "id": review_id, "revisions": [{
            "revision": int(item.get("revision") or 0),
            "stars": int(item.get("stars") or 0),
            "problem": str(item.get("problem") or ""),
            "improvement": str(item.get("improvement") or ""),
            "context": str(item.get("context") or ""),
            "comment": str(item.get("comment") or ""),
            "category": str(item.get("category") or ""),
            "version": str(item.get("package_version") or ""),
            "at": int(item.get("created_at") or 0),
        } for item in rows or []]})

    if method not in ("POST", "DELETE"):
        return _response(runtime, {"error": "method_not_allowed"}, 405,
                         "POST, DELETE")
    if not _viewer_verified(viewer):
        return _response(runtime, {"error": "email_not_verified",
                                   "verifyUrl": "/verify-pending"}, 403)

    if action in ("reply", "addressed"):
        if not (maintainer or is_admin):
            return _response(runtime, {"error": "maintainer_required"}, 403)
        if action == "reply" and method == "DELETE":
            await runtime.d1_run(
                "UPDATE marketplace_reviews SET reply_text='',reply_by='',"
                "reply_at=0 WHERE review_id=?", review_id)
        elif action == "reply":
            text = clean_text(data.get("text"), MAX_REPLY_CHARS + 1)
            if not text:
                return _response(runtime, {"error": "reply_required"}, 400)
            if len(text) > MAX_REPLY_CHARS:
                return _response(runtime, {"error": "reply_too_long",
                                           "max": MAX_REPLY_CHARS}, 400)
            addressed = normalize_version(data.get("addressedIn"))
            if addressed is None:
                return _response(runtime, {"error": "invalid_version"}, 400)
            await runtime.d1_run(
                "UPDATE marketplace_reviews SET reply_text=?,reply_by=?,"
                "reply_at=? WHERE review_id=?", text, name, now, review_id)
            if addressed:
                await runtime.d1_run(
                    "UPDATE marketplace_reviews SET addressed_version=?,"
                    "addressed_by=?,addressed_at=? WHERE review_id=?",
                    addressed, name, now, review_id)
            reviewer = str(row.get("reviewer_name") or "")
            if reviewer and reviewer != name:
                try:
                    await runtime.notify(
                        reviewer,
                        "@%s replied to your review of %s" % (
                            name, package["name"]),
                        body=text,
                        href=_package_href(market, package_id, review_id),
                        actor=name,
                        dedupe="package-review-reply:%s:%d" % (review_id, now),
                        meta={"market": market, "package": package_id,
                              "reviewId": review_id},
                    )
                except Exception:
                    pass
        elif method == "DELETE":
            await runtime.d1_run(
                "UPDATE marketplace_reviews SET addressed_version='',"
                "addressed_by='',addressed_at=0 WHERE review_id=?", review_id)
        else:
            addressed = normalize_version(data.get("version"))
            if not addressed:
                return _response(runtime, {"error": "invalid_version"}, 400)
            await runtime.d1_run(
                "UPDATE marketplace_reviews SET addressed_version=?,"
                "addressed_by=?,addressed_at=? WHERE review_id=?",
                addressed, name, now, review_id)
        await runtime.audit(name, "marketplace.review_" + action, review_id)
        return _response(runtime, {
            "ok": True, "review": _project(await _review(runtime, review_id),
                                           full=True)})

    if action == "helpful":
        if author:
            return _response(runtime, {"error": "cannot_vote_own_review"}, 403)
        if method == "POST":
            recent = await runtime.d1_first(
                "SELECT COUNT(*) AS n FROM marketplace_review_votes WHERE "
                "voter_bi=? AND created_at>=?", str(viewer_bi), now - DAY_MS)
            if int((recent or {}).get("n") or 0) >= HELPFUL_VOTES_PER_DAY:
                return _response(runtime, {"error": "vote_rate_limited"}, 429)
            await runtime.d1_run(
                "INSERT INTO marketplace_review_votes (review_id,voter_bi,"
                "created_at) VALUES (?,?,?) ON CONFLICT(review_id,voter_bi) "
                "DO NOTHING", review_id, str(viewer_bi), now)
        else:
            await runtime.d1_run(
                "DELETE FROM marketplace_review_votes WHERE review_id=? "
                "AND voter_bi=?", review_id, str(viewer_bi))
        await runtime.d1_run(
            "UPDATE marketplace_reviews SET helpful_count=(SELECT COUNT(*) "
            "FROM marketplace_review_votes WHERE review_id=?) WHERE "
            "review_id=?", review_id, review_id)
        fresh = await _review(runtime, review_id)
        return _response(runtime, {
            "ok": True, "helpful": int(fresh.get("helpful_count") or 0),
            "viewerFoundHelpful": method == "POST"})

    if action == "report":
        if method != "POST":
            return _response(runtime, {"error": "method_not_allowed"}, 405, "POST")
        if author:
            return _response(runtime, {"error": "cannot_report_own_review"}, 403)
        reason = clean_text(data.get("reason"), MAX_REPORT_REASON_CHARS)
        if len(reason) < 5:
            return _response(runtime, {"error": "report_reason_required"}, 400)
        recent = await runtime.d1_first(
            "SELECT COUNT(*) AS n FROM marketplace_review_reports WHERE "
            "reporter_bi=? AND created_at>=?", str(viewer_bi), now - DAY_MS)
        if int((recent or {}).get("n") or 0) >= REPORTS_PER_DAY:
            return _response(runtime, {"error": "report_rate_limited"}, 429)
        await runtime.d1_run(
            "INSERT INTO marketplace_review_reports (review_id,reporter_bi,"
            "reporter_name,reason,created_at) VALUES (?,?,?,?,?) "
            "ON CONFLICT(review_id,reporter_bi) DO UPDATE SET "
            "reason=excluded.reason,created_at=excluded.created_at,"
            "resolved_at=0", review_id, str(viewer_bi), name, reason, now)
        await runtime.d1_run(
            "UPDATE marketplace_reviews SET report_count=(SELECT COUNT(*) "
            "FROM marketplace_review_reports WHERE review_id=? AND "
            "resolved_at=0) WHERE review_id=?", review_id, review_id)
        return _response(runtime, {"ok": True, "reported": True})

    if action == "moderation":
        if not is_admin:
            return _response(runtime, {"error": "admin_required"}, 403)
        if method != "POST":
            return _response(runtime, {"error": "method_not_allowed"}, 405, "POST")
        hidden = data.get("hidden")
        if not isinstance(hidden, bool):
            return _response(runtime, {"error": "hidden_flag_required"}, 400)
        reason = clean_text(data.get("reason"), MAX_HIDE_REASON_CHARS)
        if hidden and not reason:
            return _response(runtime, {"error": "hide_reason_required"}, 400)
        await runtime.d1_run(
            "UPDATE marketplace_reviews SET hidden=?,hidden_by=?,"
            "hidden_reason=?,hidden_at=? WHERE review_id=?",
            1 if hidden else 0, name if hidden else "",
            reason if hidden else "", now if hidden else 0, review_id)
        # A decision closes the open reports either way.
        await runtime.d1_run(
            "UPDATE marketplace_review_reports SET resolved_at=? WHERE "
            "review_id=? AND resolved_at=0", now, review_id)
        await runtime.d1_run(
            "UPDATE marketplace_reviews SET report_count=0 WHERE review_id=?",
            review_id)
        summary = await _refresh_stats(runtime, market, package_id)
        await runtime.audit(
            name, "marketplace.review_hide" if hidden
            else "marketplace.review_unhide", review_id)
        return _response(runtime, {
            "ok": True, "summary": summary,
            "review": _project(await _review(runtime, review_id), full=True)})
    return _response(runtime, {"error": "not_found"}, 404)


async def handle(runtime, path):
    """Serve one ``/api/market/reviews`` request through ``runtime``."""
    matched = route(path)
    if matched is None:
        return _response(runtime, {"error": "not_found"}, 404)
    kind, args = matched
    method = runtime.method()
    if method == "PUT":
        method = "POST"
    allowed = {
        "summary": ("GET",),
        "maintained": ("GET",),
        "reports": ("GET",),
        "uses": ("POST",),
        "activity": ("POST",),
        "package": ("GET", "POST"),
        "feedback": ("GET",),
        "people": ("GET",),
        "item": ("GET", "POST", "DELETE"),
    }[kind]
    if method not in allowed:
        return _response(runtime, {"error": "method_not_allowed"}, 405,
                         ", ".join(allowed))
    data = {}
    if method in ("POST", "DELETE"):
        if not runtime.same_origin():
            return _response(runtime, {"error": "origin_not_allowed"}, 403)
        data, error = await runtime.json_body(MAX_BODY_BYTES)
        if error:
            return _response(
                runtime, {"error": error},
                413 if error == "payload_too_large" else 400)
    await runtime.ensure_schema()
    await _ensure_tables(runtime)
    if kind == "summary":
        return await _get_summary(runtime)
    viewer_bi, viewer = await runtime.session(data)
    viewer_bi = str(viewer_bi or "")
    viewer = viewer if isinstance(viewer, dict) else {}
    viewer_name = str(viewer.get("name") or "").lower()
    is_admin = bool(viewer_bi) and await runtime.is_admin(viewer_name)
    if kind == "maintained":
        return await _get_maintained(runtime, viewer_bi, is_admin)
    if kind == "reports":
        return await _get_reports(runtime, is_admin)
    if kind == "uses":
        return await _post_use(runtime, viewer_bi, data)
    if kind == "activity":
        return await _post_activity(runtime, viewer_bi, viewer, data)
    if kind == "item":
        return await _item(runtime, args[0], args[1], viewer_bi, viewer,
                           is_admin, data)
    package = await _package(runtime, args[0], args[1])
    if not package:
        return _response(runtime, {"error": "unknown_package"}, 404)
    if kind == "feedback":
        return await _get_feedback(runtime, package, viewer_bi, is_admin)
    if kind == "people":
        return await _get_people(runtime, package, viewer_bi, is_admin)
    if method == "GET":
        return await _get_package(runtime, package, viewer_bi, viewer, is_admin)
    return await _put_rating(runtime, package, viewer_bi, viewer, is_admin, data)
