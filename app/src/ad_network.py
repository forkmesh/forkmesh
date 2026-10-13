"""ForkMesh Ads: the first-party, human-reviewed, contextual ad network.

Any account may write an ad and submit it for review.  An ad is shown only
while three things hold together: a platform administrator approved the exact
creative that is stored now, the owner's schedule is open, and the owner holds
an active membership level that funds a live slot.  Payment processing does
not exist yet, so the membership ladder (Patreon today; any lane that writes
the same ``role_grants`` rows) is the only thing that buys reach.

The Workers adapter in ``entry.py`` owns network and D1 primitives.  This
module stays importable under ordinary CPython so the policy, the review
state machine and the rotation can be tested without a Cloudflare runtime.

Boundaries this module must keep:
- Contextual only.  The serve route takes a product surface and nothing else:
  no session, no cookie, no account, no repository, no history.  An ad cannot
  be aimed at a person, and the network never learns who saw one.
- Text creatives only.  A remote image or script would let an advertiser log
  every viewer's address, so neither is accepted; the destination opens with
  ``rel="sponsored noopener noreferrer"`` and no referrer.
- Measurement is an aggregate daily click count per ad and surface.  No
  impression beacon, no identifier, no address is stored.
- Review is by a human.  The automated screen only raises flags for the
  reviewer (and refuses a few unambiguous things such as link shorteners); it
  never approves.  Every decision lands in ``ad_network_reviews`` and the
  sensitive audit log, and an edited creative always returns to review because
  approval is pinned to the creative's content hash.
- The per-level allowances below are published in business-model.json and
  pinned by test_ad_network.py; change them together or not at all.
"""

from __future__ import annotations

import hashlib
import ipaddress
import json
import re
import time
from urllib.parse import urlparse


NETWORK = "forkmesh-ads-v1"
BODY_MAX_BYTES = 16 * 1024
DAY_MS = 24 * 60 * 60 * 1000

# One approval covers one creative for at most this long; after that the ad
# stops serving until it is reviewed again.
APPROVAL_TTL_MS = 180 * DAY_MS
MAX_SCHEDULE_MS = 366 * DAY_MS
SERVE_CACHE_SECONDS = 300
POLICY_CACHE_SECONDS = 3600
SERVE_CANDIDATE_LIMIT = 400
STATS_WINDOW_DAYS = 30
STATS_RETENTION_DAYS = 400

MAX_ADS_PER_ACCOUNT = 25
MAX_PENDING_PER_ACCOUNT = 5
REVIEW_QUEUE_LIMIT = 100
REVIEW_HISTORY_LIMIT = 10
# Distinct signed-in reporters that pull a running ad back into review without
# waiting for an administrator to notice.
REPORTS_TO_AUTO_SUSPEND = 5

HEADLINE_MAX = 60
BODY_MAX = 160
CTA_MAX = 24
SPONSOR_MAX = 48
NOTE_MAX = 500
REASON_MAX = 500
URL_MAX = 600

EDGE_CACHE_PREFIX = "https://forkmesh.internal/ad-network/v1/"

# Product surfaces an ad can run on.  Clients send exactly these ids.
PLACEMENTS = {
    "web": "Web app and public repository and profile pages",
    "www": "forkmesh.com site",
    "desktop": "Desktop app",
    "mobile": "Mobile apps",
    "world": "ForkMesh World",
}

# What an ad may be about.  An allowlist, so a new kind of ad is a reviewed
# policy change rather than something a submitter can simply type.
CATEGORIES = {
    "developer-tools": "Developer tools and libraries",
    "open-source": "Open-source projects and foundations",
    "infrastructure": "Hosting, infrastructure and self-hosting",
    "security": "Defensive security and privacy tools",
    "education": "Education, books and courses",
    "events": "Conferences, meetups and hackathons",
    "jobs": "Hiring and job boards",
    "accessibility": "Accessibility products and services",
    "nonprofit": "Nonprofits and public-interest causes",
    "sustainability": "Climate and sustainability work",
    "hardware": "Repairable and open hardware",
    "creative": "Independent creative and indie software",
    "community": "Community projects and member shout-outs",
}

# What is never accepted, whatever category is claimed.  The reviewer cites
# these codes on a rejection or suspension so the owner sees the exact rule.
PROHIBITED = {
    "gambling": "Gambling, betting, lotteries and loot-box mechanics",
    "tobacco-alcohol-drugs": "Tobacco, vaping, alcohol and recreational drugs",
    "weapons": "Weapons, ammunition and explosives",
    "adult": "Adult or sexually explicit content",
    "financial-returns": (
        "Promised returns: speculative tokens, trading signals, "
        "get-rich-quick and multi-level marketing"),
    "predatory-lending": "Payday loans, debt traps and predatory credit",
    "unproven-health": "Miracle cures and unproven health or diet claims",
    "surveillance": (
        "Behavioral tracking, data brokers, stalkerware and "
        "people-search services"),
    "malware-exploits": (
        "Malware, exploit or credential sales and offensive hacking "
        "services"),
    "political-persuasion": "Election, candidate and ballot persuasion",
    "hate-harassment": "Hate, harassment, discrimination or violence",
    "deceptive": (
        "Misleading claims, fake urgency, impersonation, clickbait and "
        "dark patterns"),
    "illegal": "Illegal goods or services, counterfeits and piracy",
    "exploitative-labor": "Exploitative labor and unpaid-work schemes",
    "targets-children": "Anything aimed at children",
    "tracking-destination": (
        "Destinations that fingerprint visitors, force a download or "
        "redirect through a tracker"),
}

REPORT_REASONS = {
    "prohibited": "It advertises something the policy prohibits",
    "misleading": "It is misleading or deceptive",
    "offensive": "It is offensive or hateful",
    "broken": "The link is broken or goes somewhere else",
    "other": "Something else",
}

# Membership ladder -> what it funds.  ``liveAds`` is how many approved ads
# can run at once; ``weight`` is the share of the rotation each running ad
# receives.  Roles are the inherited role_grants names Patreon writes
# (app/src/patreon.py); the highest active role wins.
TIERS = (
    {"id": "studio", "label": "Studio", "role": "studio_member",
     "priceUsdMonthly": 100, "liveAds": 10, "weight": 10},
    {"id": "builder", "label": "Builder", "role": "builder_member",
     "priceUsdMonthly": 50, "liveAds": 5, "weight": 5},
    {"id": "pro", "label": "Pro", "role": "pro_member",
     "priceUsdMonthly": 10, "liveAds": 2, "weight": 2},
    {"id": "supporter", "label": "Supporter", "role": "supporting_member",
     "priceUsdMonthly": 5, "liveAds": 1, "weight": 1},
)
FREE_TIER = {
    "id": "free", "label": "Free", "role": "",
    "priceUsdMonthly": 0, "liveAds": 0, "weight": 0,
}
MEMBER_ROLES = tuple(tier["role"] for tier in TIERS)
_ROLE_SQL = ",".join("'" + role + "'" for role in MEMBER_ROLES)

STATUSES = (
    "draft", "pending", "approved", "rejected", "suspended", "withdrawn")
REVIEW_ACTIONS = {
    # action -> (statuses it applies to, resulting status)
    "approve": (("pending",), "approved"),
    "reject": (("pending",), "rejected"),
    "suspend": (("approved",), "suspended"),
    "reinstate": (("suspended",), "pending"),
}
OWNER_ACTIONS = ("submit", "pause", "resume", "withdraw")

CREATIVE_FIELDS = (
    "headline", "body", "cta", "sponsor", "url", "category", "placements")
_WRITE_FIELDS = frozenset(CREATIVE_FIELDS) | {
    "startsAt", "endsAt", "note", "attestation", "submit", "sessionToken"}

_ID_RE = re.compile(r"^[a-f0-9]{32}$")
_NONPUBLIC_SUFFIXES = (
    ".internal", ".invalid", ".local", ".localhost", ".test", ".example",
    ".onion",
)
# A shortener hides the destination the reviewer approved.
_SHORTENER_HOSTS = frozenset({
    "bit.ly", "t.co", "tinyurl.com", "goo.gl", "ow.ly", "is.gd", "buff.ly",
    "rebrand.ly", "cutt.ly", "shorturl.at", "rb.gy", "lnkd.in", "t.ly",
    "tiny.cc", "v.gd", "s.id",
})
# Query keys that exist only to follow a visitor across sites.
_TRACKING_PARAMS = frozenset({
    "fbclid", "gclid", "gbraid", "wbraid", "msclkid", "dclid", "yclid",
    "twclid", "ttclid", "li_fat_id", "mc_eid", "igshid", "_hsenc", "_hsmi",
    "vero_id", "oly_enc_id", "oly_anon_id",
})

# Screening vocabulary: prohibited code -> words that raise a reviewer flag.
# A flag is a prompt to look, never a verdict: "exploit" is ordinary in a
# security tool's copy, which is exactly why a human decides.
_SCREEN_TERMS = {
    "gambling": (
        "casino", "betting", "sportsbook", "poker", "lottery", "jackpot",
        "slots", "wager"),
    "tobacco-alcohol-drugs": (
        "vape", "vaping", "cigarette", "tobacco", "nicotine", "whiskey",
        "vodka", "cannabis", "thc"),
    "weapons": ("firearm", "rifle", "ammo", "ammunition", "handgun"),
    "adult": ("porn", "xxx", "escort", "nsfw", "onlyfans"),
    "financial-returns": (
        "guaranteed returns", "passive income", "get rich", "100x",
        "presale", "airdrop", "trading signals", "forex", "memecoin",
        "double your", "mlm", "downline"),
    "predatory-lending": ("payday loan", "cash advance", "no credit check"),
    "unproven-health": (
        "miracle", "cure", "weight loss", "detox", "anti-aging",
        "testosterone"),
    "surveillance": (
        "spy on", "track anyone", "stalkerware", "people search",
        "data broker", "background check", "keylogger"),
    "malware-exploits": (
        "0day", "zero-day for sale", "botnet", "ddos service", "stresser",
        "cracked accounts", "combo list", "ransomware"),
    "political-persuasion": (
        "vote for", "re-elect", "ballot measure", "campaign donation"),
    "deceptive": (
        "act now", "last chance", "you won", "you have been selected",
        "doctors hate", "one weird trick", "limited time only", "risk-free"),
    "targets-children": ("for kids", "for children", "toys"),
}


def valid_id(value):
    return bool(_ID_RE.fullmatch(str(value or "").strip().lower()))


def clean_text(value, limit):
    raw = str(value or "")
    clean = "".join(
        char if char.isprintable() and char not in "<>" else " "
        for char in raw
    )
    return " ".join(clean.split())[:limit].strip()


def destination_url(value):
    """Return (normalized https URL, "") or ("", error code)."""

    raw = str(value or "").strip()
    if not raw or len(raw) > URL_MAX or "\\" in raw:
        return "", "invalid_url"
    try:
        parsed = urlparse(raw)
        host = str(parsed.hostname or "").strip().lower().rstrip(".")
        port = parsed.port
    except (TypeError, ValueError):
        return "", "invalid_url"
    if (
        parsed.scheme.lower() != "https"
        or not host
        or parsed.username is not None
        or parsed.password is not None
        or port not in (None, 443)
        or "." not in host
        or any(host.endswith(suffix) for suffix in _NONPUBLIC_SUFFIXES)
    ):
        return "", "invalid_url"
    try:
        ipaddress.ip_address(host)
        return "", "invalid_url"
    except ValueError:
        pass
    bare = host[4:] if host.startswith("www.") else host
    if bare in _SHORTENER_HOSTS:
        return "", "url_shortener_not_allowed"
    for pair in (parsed.query or "").split("&"):
        key = pair.split("=", 1)[0].strip().lower()
        if key in _TRACKING_PARAMS:
            return "", "tracking_parameter_not_allowed"
    return parsed._replace(
        scheme="https", netloc=host, fragment="").geturl(), ""


def _placements(value):
    if value in (None, "", []):
        return sorted(PLACEMENTS)
    if isinstance(value, str):
        value = [part for part in value.split(",") if part]
    if not isinstance(value, list) or len(value) > len(PLACEMENTS):
        return None
    normalized = []
    for item in value:
        clean = str(item or "").strip().lower()
        if clean not in PLACEMENTS:
            return None
        if clean not in normalized:
            normalized.append(clean)
    return sorted(normalized) or None


def _timestamp(value):
    try:
        number = int(value or 0)
    except (TypeError, ValueError):
        return None
    return number if 0 <= number < 10 ** 14 else None


def content_hash(creative):
    """Digest of exactly what a reviewer approves (not the schedule)."""

    material = {
        field: creative.get(field) for field in CREATIVE_FIELDS
    }
    return hashlib.sha256(json.dumps(
        material, sort_keys=True, separators=(",", ":"),
        ensure_ascii=True).encode()).hexdigest()


def screen_flags(creative):
    """Reviewer prompts for one creative; sorted, never a decision."""

    text = " ".join(
        str(creative.get(field) or "")
        for field in ("headline", "body", "cta", "sponsor", "url")
    ).lower()
    words = set(re.findall(r"[a-z0-9][a-z0-9-]*", text))
    flags = set()
    for code, terms in _SCREEN_TERMS.items():
        for term in terms:
            if (" " in term and term in text) or term in words:
                flags.add("term:" + code)
                break
    headline = str(creative.get("headline") or "")
    letters = [char for char in headline if char.isalpha()]
    if len(letters) >= 8 and sum(
            1 for char in letters if char.isupper()) > 0.7 * len(letters):
        flags.add("style:shouting")
    if re.search(r"[!?]{2,}", headline + " " + str(creative.get("body") or "")):
        flags.add("style:punctuation")
    if re.search(r"[$€£]\s?\d|\d\s?%\s*(off|return|apy|apr)", text):
        flags.add("claim:price-or-rate")
    if "?" in str(creative.get("url") or ""):
        flags.add("url:query-string")
    return sorted(flags)


def normalize_ad(data, now, existing=None):
    """Validate one create/update body.

    Returns ``(fields, "")`` or ``(None, error_code)``.  ``existing`` supplies
    the stored values an update leaves out.
    """

    if not isinstance(data, dict):
        return None, "invalid_json"
    if set(data) - _WRITE_FIELDS:
        return None, "unsupported_ad_field"
    base = existing if isinstance(existing, dict) else {}

    def pick(field, fallback=""):
        return data[field] if field in data else base.get(field, fallback)

    headline = clean_text(pick("headline"), HEADLINE_MAX)
    body = clean_text(pick("body"), BODY_MAX)
    cta = clean_text(pick("cta"), CTA_MAX)
    sponsor = clean_text(pick("sponsor"), SPONSOR_MAX)
    category = str(pick("category") or "").strip().lower()
    if len(headline) < 4:
        return None, "headline_required"
    if len(sponsor) < 2:
        return None, "sponsor_required"
    if category not in CATEGORIES:
        return None, "invalid_category"
    url, url_error = destination_url(pick("url"))
    if url_error:
        return None, url_error
    placements = _placements(pick("placements", None))
    if placements is None:
        return None, "invalid_placements"
    starts_at = _timestamp(
        data["startsAt"] if "startsAt" in data else base.get("startsAt", 0))
    ends_at = _timestamp(
        data["endsAt"] if "endsAt" in data else base.get("endsAt", 0))
    if starts_at is None or ends_at is None:
        return None, "invalid_schedule"
    if ends_at and (
            ends_at <= max(starts_at, now)
            or ends_at - max(starts_at, now) > MAX_SCHEDULE_MS):
        return None, "invalid_schedule"
    fields = {
        "headline": headline,
        "body": body,
        "cta": cta or "Learn more",
        "sponsor": sponsor,
        "url": url,
        "category": category,
        "placements": placements,
        "startsAt": starts_at,
        "endsAt": ends_at,
        "note": clean_text(pick("note"), NOTE_MAX),
    }
    fields["contentHash"] = content_hash(fields)
    fields["screenFlags"] = screen_flags(fields)
    return fields, ""


def tier_for_roles(roles):
    """Highest membership level among active role names."""

    held = {str(role or "").strip().lower() for role in (roles or ())}
    for tier in TIERS:
        if tier["role"] in held:
            return tier
    return FREE_TIER


def _tier_public(tier):
    return {
        "id": tier["id"],
        "label": tier["label"],
        "priceUsdMonthly": tier["priceUsdMonthly"],
        "liveAds": tier["liveAds"],
        "weight": tier["weight"],
    }


def policy_document():
    return {
        "ok": True,
        "network": NETWORK,
        "label": "Sponsored",
        "summary": (
            "ForkMesh Ads are short text placements written by members, "
            "read by a person before they run, and shown by product surface "
            "only. They are never aimed at you and never follow you."),
        # Deliberately modest, like community placements: a reviewer checked an
        # ad against a policy; nobody certified a company.
        "claim": (
            "Approval means a ForkMesh reviewer found this ad within the "
            "published policy when they read it. It is not an endorsement "
            "of the advertiser or a claim that a company is ethical."),
        "placements": [
            {"id": key, "label": label} for key, label in PLACEMENTS.items()],
        "categories": [
            {"id": key, "label": label} for key, label in CATEGORIES.items()],
        "prohibited": [
            {"id": key, "label": label} for key, label in PROHIBITED.items()],
        "reportReasons": [
            {"id": key, "label": label}
            for key, label in REPORT_REASONS.items()],
        "tiers": [_tier_public(FREE_TIER)] + [
            _tier_public(tier) for tier in reversed(TIERS)],
        "limits": {
            "headline": HEADLINE_MAX,
            "body": BODY_MAX,
            "cta": CTA_MAX,
            "sponsor": SPONSOR_MAX,
            "note": NOTE_MAX,
            "adsPerAccount": MAX_ADS_PER_ACCOUNT,
            "pendingPerAccount": MAX_PENDING_PER_ACCOUNT,
            "approvalDays": APPROVAL_TTL_MS // DAY_MS,
        },
        "review": {
            "humanReviewed": True,
            "automatedApproval": False,
            "editsReturnToReview": True,
            "decisionsAreAudited": True,
            "reportsToAutoSuspend": REPORTS_TO_AUTO_SUSPEND,
        },
        "privacy": {
            "contextualOnly": True,
            "behavioralTracking": False,
            "personalProfilesUsed": False,
            "remoteImagesOrScripts": False,
            "impressionsMeasured": False,
            "measurement": "aggregate daily clicks per ad and surface",
            "linkRel": "sponsored noopener noreferrer",
        },
        "payment": {
            "processor": None,
            "fundedBy": "membership level",
            "note": (
                "There is no ad billing yet. A membership level funds a "
                "number of live slots and a rotation weight; an approved ad "
                "pauses by itself while its owner has no active membership."),
        },
    }


def edge_cache_key(method, path, params):
    """Synthetic Cache API key for the two public reads, else ''."""

    if str(method or "").upper() != "GET":
        return ""
    tail = _tail(path)
    if tail == ["policy"]:
        return EDGE_CACHE_PREFIX + "policy"
    if tail == ["serve"]:
        placement = str((params or {}).get("placement") or "").strip().lower()
        if placement in PLACEMENTS:
            return EDGE_CACHE_PREFIX + "serve/" + placement
    return ""


def edge_cache_purge_keys(method, path):
    """Serve keys a successful mutation should drop (this colo only)."""

    if str(method or "").upper() in ("GET", "HEAD", "OPTIONS"):
        return []
    if _tail(path)[1:] == ["click"]:
        return []
    return [EDGE_CACHE_PREFIX + "serve/" + key for key in PLACEMENTS]


def _tail(path):
    clean = str(path or "").split("?", 1)[0].strip("/")
    parts = clean.split("/") if clean else []
    if parts[:2] != ["api", "ads"]:
        return ["?"]
    return parts[2:]


def _response(runtime, data, status=200, allow="", cache_seconds=0):
    headers = {"x-content-type-options": "nosniff"}
    if allow:
        headers["allow"] = allow
    return runtime.response(
        data,
        status=status,
        cache_control=(
            "public, max-age=" + str(int(cache_seconds))
            if cache_seconds and status == 200
            else "no-store, max-age=0, must-revalidate"),
        extra_headers=headers,
    )


def _error(runtime, code, status=400, **extra):
    payload = {"ok": False, "error": code}
    payload.update(extra)
    return _response(runtime, payload, status)


def _split(value):
    return [part for part in str(value or "").split(",") if part]


def _day(now):
    """UTC calendar day as yyyymmdd."""

    parts = time.gmtime(int(now) // 1000)
    return parts.tm_year * 10000 + parts.tm_mon * 100 + parts.tm_mday


def _row_creative(row):
    return {
        "headline": str(row.get("headline") or ""),
        "body": str(row.get("body") or ""),
        "cta": str(row.get("cta") or ""),
        "sponsor": str(row.get("sponsor") or ""),
        "url": str(row.get("url") or ""),
        "category": str(row.get("category") or ""),
        "placements": _split(row.get("placements")),
    }


def delivery_state(row, tier, now, live_rank=0):
    """Why an ad is or is not running right now, as one word for the owner."""

    status = str(row.get("status") or "")
    if status != "approved":
        return status
    if str(row.get("approved_hash") or "") != str(
            row.get("content_hash") or ""):
        return "pending"
    if int(row.get("approved_until") or 0) <= now:
        return "review-expired"
    if int(row.get("paused") or 0):
        return "paused"
    if int(row.get("starts_at") or 0) > now:
        return "scheduled"
    ends_at = int(row.get("ends_at") or 0)
    if ends_at and ends_at <= now:
        return "ended"
    if not tier["liveAds"]:
        return "needs-membership"
    if live_rank >= tier["liveAds"]:
        return "over-allowance"
    return "running"


def _public_ad(row):
    creative = _row_creative(row)
    return {
        "id": str(row.get("ad_id") or ""),
        "label": "Sponsored",
        "headline": creative["headline"],
        "body": creative["body"],
        "cta": creative["cta"],
        "sponsor": creative["sponsor"],
        "url": creative["url"],
        "category": creative["category"],
        "categoryLabel": CATEGORIES.get(creative["category"], ""),
    }


def _owner_ad(row, state, clicks):
    ad = _public_ad(row)
    ad.update({
        "placements": _split(row.get("placements")),
        "status": str(row.get("status") or ""),
        "delivery": state,
        "paused": bool(int(row.get("paused") or 0)),
        "startsAt": int(row.get("starts_at") or 0),
        "endsAt": int(row.get("ends_at") or 0),
        "note": str(row.get("owner_note") or ""),
        # What a reviewer names when approving, so an approval can only ever
        # apply to the words they actually read.
        "contentHash": str(row.get("content_hash") or ""),
        "screenFlags": _split(row.get("screen_flags")),
        "reviewReason": str(row.get("review_reason") or ""),
        "reviewCodes": _split(row.get("review_codes")),
        "submittedAt": int(row.get("submitted_at") or 0),
        "reviewedAt": int(row.get("reviewed_at") or 0),
        "approvedUntil": int(row.get("approved_until") or 0),
        "createdAt": int(row.get("created_at") or 0),
        "updatedAt": int(row.get("updated_at") or 0),
        "clicks": clicks,
    })
    return ad


def select_ad(rows, placement, now):
    """Pick one running ad for a surface.

    ``rows`` are approved candidates, oldest approval first, each carrying a
    ``roles`` column with the owner's active membership roles.  The owner's
    level caps how many of their ads count (oldest first) and weights each;
    the pick rotates on a five-minute bucket so a cached response and a fresh
    one agree, and no randomness or viewer input is involved.
    """

    live_per_owner = {}
    pool = []
    for row in rows or []:
        tier = tier_for_roles(_split(row.get("roles")))
        owner = str(row.get("account_bi") or "")
        rank = live_per_owner.get(owner, 0)
        if delivery_state(row, tier, now, rank) != "running":
            continue
        # The slot is consumed whichever surfaces the ad names, so a level's
        # allowance means "ads running", not "ads per surface".
        live_per_owner[owner] = rank + 1
        if placement in _split(row.get("placements")):
            pool.append((row, tier["weight"]))
    total = sum(weight for _row, weight in pool)
    if not total:
        return None
    bucket = int(now) // (SERVE_CACHE_SECONDS * 1000)
    digest = hashlib.sha256(
        (placement + ":" + str(bucket)).encode()).digest()
    ticket = int.from_bytes(digest[:8], "big") % total
    for row, weight in pool:
        if ticket < weight:
            return row
        ticket -= weight
    return pool[-1][0]


async def _account_tier(runtime, account_bi, now):
    rows = await runtime.d1_all(
        "SELECT role FROM role_grants WHERE account_bi=? "
        "AND role IN (" + _ROLE_SQL + ") AND revoked_at=0 "
        "AND (expires_at=0 OR expires_at>?)",
        account_bi, now,
    )
    return tier_for_roles([row.get("role") for row in rows or []])


async def _serve(runtime):
    placement = str(
        runtime.query_params().get("placement") or "").strip().lower()
    if placement not in PLACEMENTS:
        return _error(runtime, "invalid_placement")
    now = runtime.now()
    rows = await runtime.d1_all(
        "SELECT a.ad_id,a.account_bi,a.status,a.paused,a.headline,a.body,"
        "a.cta,a.sponsor,a.url,a.category,a.placements,a.content_hash,"
        "a.approved_hash,a.approved_until,a.starts_at,a.ends_at,"
        "(SELECT GROUP_CONCAT(g.role) FROM role_grants g "
        "WHERE g.account_bi=a.account_bi AND g.role IN (" + _ROLE_SQL + ") "
        "AND g.revoked_at=0 AND (g.expires_at=0 OR g.expires_at>?)) AS roles "
        "FROM ad_network_ads a WHERE a.status='approved' AND a.paused=0 "
        "AND a.approved_until>? AND a.starts_at<=? "
        "AND (a.ends_at=0 OR a.ends_at>?) "
        "ORDER BY a.reviewed_at,a.ad_id LIMIT ?",
        now, now, now, now, SERVE_CANDIDATE_LIMIT,
    )
    row = select_ad(rows, placement, now)
    return _response(runtime, {
        "ok": True,
        "network": NETWORK,
        "placement": placement,
        "ad": _public_ad(row) if row else None,
        "policyUrl": "/docs/operations/ad-network/",
    }, cache_seconds=SERVE_CACHE_SECONDS)


async def _click(runtime, ad_id):
    data, error = await runtime.json_body(BODY_MAX_BYTES)
    if error:
        return _error(
            runtime, error, 413 if error == "payload_too_large" else 400)
    placement = str(data.get("placement") or "").strip().lower()
    if placement not in PLACEMENTS:
        return _error(runtime, "invalid_placement")
    # The EXISTS guard keeps the counter from becoming a write primitive for
    # arbitrary ids: only an ad that is approved right now can be counted.
    await runtime.d1_run(
        "INSERT INTO ad_network_daily_stats(ad_id,day,placement,clicks) "
        "SELECT ?,?,?,1 WHERE EXISTS(SELECT 1 FROM ad_network_ads "
        "WHERE ad_id=? AND status='approved') "
        "ON CONFLICT(ad_id,day,placement) DO UPDATE SET clicks=clicks+1",
        ad_id, _day(runtime.now()), placement, ad_id,
    )
    return _response(runtime, {"ok": True})


async def _session(runtime, data=None):
    account_bi, account = await runtime.session(data or {})
    name = str((account or {}).get("name") or "").strip().lower()
    if not account_bi or not name:
        return "", ""
    return account_bi, name


async def _report(runtime, ad_id):
    data, error = await runtime.json_body(BODY_MAX_BYTES)
    if error:
        return _error(
            runtime, error, 413 if error == "payload_too_large" else 400)
    if set(data) - {"reason", "note", "sessionToken"}:
        return _error(runtime, "unsupported_report_field")
    reason = str(data.get("reason") or "").strip().lower()
    if reason not in REPORT_REASONS:
        return _error(runtime, "invalid_reason")
    account_bi, name = await _session(runtime, data)
    if not account_bi:
        return _error(runtime, "unauthorized", 401)
    row = await runtime.d1_first(
        "SELECT ad_id,account_bi,status FROM ad_network_ads WHERE ad_id=?",
        ad_id)
    if not row or str(row.get("status") or "") not in (
            "approved", "suspended"):
        return _error(runtime, "not_found", 404)
    if str(row.get("account_bi") or "") == account_bi:
        return _error(runtime, "cannot_report_own_ad", 409)
    now = runtime.now()
    await runtime.d1_run(
        "INSERT INTO ad_network_reports"
        "(ad_id,account_bi,reason,note,created_at,resolved_at) "
        "VALUES(?,?,?,?,?,0) ON CONFLICT(ad_id,account_bi) DO UPDATE SET "
        "reason=excluded.reason,note=excluded.note,"
        "created_at=excluded.created_at,resolved_at=0",
        ad_id, account_bi, reason, clean_text(data.get("note"), NOTE_MAX),
        now,
    )
    count = await runtime.d1_first(
        "SELECT COUNT(*) AS n FROM ad_network_reports "
        "WHERE ad_id=? AND resolved_at=0", ad_id)
    open_reports = int((count or {}).get("n") or 0)
    suspended = False
    if (open_reports >= REPORTS_TO_AUTO_SUSPEND
            and str(row.get("status") or "") == "approved"):
        # Enough independent members objected: stop serving first, let a
        # reviewer decide second.  The status guard makes this idempotent.
        await runtime.batch([
            (
                "UPDATE ad_network_ads SET status='suspended',"
                "review_reason=?,review_codes='',reviewed_at=?,updated_at=? "
                "WHERE ad_id=? AND status='approved'",
                ("Paused automatically after member reports; a reviewer "
                 "will look at it.", now, now, ad_id),
            ),
            (
                "INSERT INTO ad_network_reviews"
                "(review_id,ad_id,action,reviewer_bi,self_review,reason,"
                "codes,content_hash,created_at) "
                "SELECT ?,ad_id,'auto-suspend','',0,?,'',content_hash,? "
                "FROM ad_network_ads WHERE ad_id=?",
                (runtime.new_id(), "member reports: " + str(open_reports),
                 now, ad_id),
            ),
        ])
        suspended = True
        await runtime.audit(
            name, "ad_network.auto_suspend", "ad", ad_id, "success",
            {"openReports": open_reports})
    return _response(runtime, {
        "ok": True, "openReports": open_reports, "suspended": suspended,
    }, 201)


async def _clicks_by_ad(runtime, account_bi, now):
    since = _day(now - STATS_WINDOW_DAYS * DAY_MS)
    rows = await runtime.d1_all(
        "SELECT s.ad_id,s.placement,SUM(s.clicks) AS clicks "
        "FROM ad_network_daily_stats s JOIN ad_network_ads a "
        "ON a.ad_id=s.ad_id WHERE a.account_bi=? AND s.day>=? "
        "GROUP BY s.ad_id,s.placement",
        account_bi, since,
    )
    result = {}
    for row in rows or []:
        entry = result.setdefault(
            str(row.get("ad_id") or ""),
            {"windowDays": STATS_WINDOW_DAYS, "total": 0, "byPlacement": {}})
        clicks = int(row.get("clicks") or 0)
        entry["total"] += clicks
        entry["byPlacement"][str(row.get("placement") or "")] = clicks
    return result


def _no_clicks():
    return {"windowDays": STATS_WINDOW_DAYS, "total": 0, "byPlacement": {}}


async def _owner_rows(runtime, account_bi):
    return await runtime.d1_all(
        "SELECT * FROM ad_network_ads WHERE account_bi=? "
        "ORDER BY CASE status WHEN 'approved' THEN 0 ELSE 1 END,"
        "reviewed_at,ad_id LIMIT ?",
        account_bi, MAX_ADS_PER_ACCOUNT,
    ) or []


def _owner_view(rows, tier, now, clicks):
    """Owner ads with their delivery word; ranks match select_ad's order."""

    rank = 0
    ads = []
    for row in rows:
        state = delivery_state(row, tier, now, rank)
        if state in ("running", "over-allowance"):
            rank += 1
        ads.append(_owner_ad(
            row, state, clicks.get(str(row.get("ad_id") or ""), _no_clicks())))
    ads.sort(key=lambda ad: -ad["updatedAt"])
    return ads


async def _mine(runtime):
    account_bi, name = await _session(runtime)
    if not account_bi:
        return _error(runtime, "unauthorized", 401)
    now = runtime.now()
    tier = await _account_tier(runtime, account_bi, now)
    rows = await _owner_rows(runtime, account_bi)
    ads = _owner_view(
        rows, tier, now, await _clicks_by_ad(runtime, account_bi, now))
    return _response(runtime, {
        "ok": True,
        "tier": _tier_public(tier),
        "usage": {
            "running": sum(1 for ad in ads if ad["delivery"] == "running"),
            "liveAds": tier["liveAds"],
            "pending": sum(1 for ad in ads if ad["status"] == "pending"),
            "pendingLimit": MAX_PENDING_PER_ACCOUNT,
            "total": len(ads),
            "totalLimit": MAX_ADS_PER_ACCOUNT,
        },
        "ads": ads,
        "isReviewer": bool(await runtime.is_admin(name)),
    })


async def _single_owner_ad(runtime, account_bi, ad_id, status=200):
    now = runtime.now()
    tier = await _account_tier(runtime, account_bi, now)
    rows = await _owner_rows(runtime, account_bi)
    ads = _owner_view(rows, tier, now, {})
    for ad in ads:
        if ad["id"] == ad_id:
            return _response(runtime, {"ok": True, "ad": ad}, status)
    return _error(runtime, "not_found", 404)


async def _pending_count(runtime, account_bi, exclude=""):
    row = await runtime.d1_first(
        "SELECT COUNT(*) AS n FROM ad_network_ads "
        "WHERE account_bi=? AND status='pending' AND ad_id<>?",
        account_bi, exclude)
    return int((row or {}).get("n") or 0)


async def _create(runtime):
    data, error = await runtime.json_body(BODY_MAX_BYTES)
    if error:
        return _error(
            runtime, error, 413 if error == "payload_too_large" else 400)
    account_bi, name = await _session(runtime, data)
    if not account_bi:
        return _error(runtime, "unauthorized", 401)
    now = runtime.now()
    fields, error = normalize_ad(data, now)
    if error:
        return _error(runtime, error)
    submit = bool(data.get("submit"))
    if submit and data.get("attestation") is not True:
        return _error(runtime, "attestation_required")
    total = await runtime.d1_first(
        "SELECT COUNT(*) AS n FROM ad_network_ads WHERE account_bi=?",
        account_bi)
    if int((total or {}).get("n") or 0) >= MAX_ADS_PER_ACCOUNT:
        return _error(runtime, "ad_limit_reached", 409,
                      limit=MAX_ADS_PER_ACCOUNT)
    if submit and await _pending_count(
            runtime, account_bi) >= MAX_PENDING_PER_ACCOUNT:
        return _error(runtime, "pending_limit_reached", 409,
                      limit=MAX_PENDING_PER_ACCOUNT)
    ad_id = str(runtime.new_id()).replace("-", "").lower()[:32]
    if not valid_id(ad_id):
        return _error(runtime, "id_unavailable", 503)
    await runtime.d1_run(
        "INSERT INTO ad_network_ads"
        "(ad_id,account_bi,owner_sealed,status,paused,headline,body,cta,"
        "sponsor,url,category,placements,content_hash,approved_hash,"
        "screen_flags,owner_note,review_reason,review_codes,starts_at,"
        "ends_at,submitted_at,reviewed_at,approved_until,created_at,"
        "updated_at) VALUES(?,?,?,?,0,?,?,?,?,?,?,?,?,'',?,?,'','',?,?,?,0,0,"
        "?,?)",
        ad_id, account_bi, await runtime.seal({"name": name}),
        "pending" if submit else "draft",
        fields["headline"], fields["body"], fields["cta"], fields["sponsor"],
        fields["url"], fields["category"], ",".join(fields["placements"]),
        fields["contentHash"], ",".join(fields["screenFlags"]),
        fields["note"], fields["startsAt"], fields["endsAt"],
        now if submit else 0, now, now,
    )
    return await _single_owner_ad(runtime, account_bi, ad_id, 201)


async def _owned_row(runtime, account_bi, ad_id):
    return await runtime.d1_first(
        "SELECT * FROM ad_network_ads WHERE ad_id=? AND account_bi=?",
        ad_id, account_bi)


def _stored_fields(row):
    fields = _row_creative(row)
    fields.update({
        "startsAt": int(row.get("starts_at") or 0),
        "endsAt": int(row.get("ends_at") or 0),
        "note": str(row.get("owner_note") or ""),
    })
    return fields


async def _update(runtime, ad_id):
    data, error = await runtime.json_body(BODY_MAX_BYTES)
    if error:
        return _error(
            runtime, error, 413 if error == "payload_too_large" else 400)
    account_bi, _name = await _session(runtime, data)
    if not account_bi:
        return _error(runtime, "unauthorized", 401)
    row = await _owned_row(runtime, account_bi, ad_id)
    if not row:
        return _error(runtime, "not_found", 404)
    status = str(row.get("status") or "")
    if status == "suspended":
        # A suspension is a reviewer's hold; editing around it would let the
        # owner clear it themselves.
        return _error(runtime, "ad_suspended", 409)
    now = runtime.now()
    fields, error = normalize_ad(data, now, _stored_fields(row))
    if error:
        return _error(runtime, error)
    changed = fields["contentHash"] != str(row.get("content_hash") or "")
    submit = bool(data.get("submit"))
    next_status = status
    if changed and status == "approved":
        # The approval was for the old words.  Straight back to the queue
        # rather than to draft, so a typo fix does not silently end a run.
        next_status = "pending"
    elif changed and status == "rejected":
        next_status = "draft"
    if submit and next_status in ("draft", "rejected", "withdrawn"):
        next_status = "pending"
    if next_status == "pending" and status != "pending":
        if data.get("attestation") is not True:
            return _error(runtime, "attestation_required")
        if await _pending_count(
                runtime, account_bi, ad_id) >= MAX_PENDING_PER_ACCOUNT:
            return _error(runtime, "pending_limit_reached", 409,
                          limit=MAX_PENDING_PER_ACCOUNT)
    await runtime.d1_run(
        "UPDATE ad_network_ads SET status=?,headline=?,body=?,cta=?,"
        "sponsor=?,url=?,category=?,placements=?,content_hash=?,"
        "screen_flags=?,owner_note=?,starts_at=?,ends_at=?,"
        "submitted_at=CASE WHEN ?=1 THEN ? ELSE submitted_at END,"
        "updated_at=? WHERE ad_id=? AND account_bi=? AND status=?",
        next_status, fields["headline"], fields["body"], fields["cta"],
        fields["sponsor"], fields["url"], fields["category"],
        ",".join(fields["placements"]), fields["contentHash"],
        ",".join(fields["screenFlags"]), fields["note"], fields["startsAt"],
        fields["endsAt"],
        1 if next_status == "pending" and status != "pending" else 0, now,
        now, ad_id, account_bi, status,
    )
    return await _single_owner_ad(runtime, account_bi, ad_id)


async def _owner_action(runtime, ad_id, action):
    data, error = await runtime.json_body(BODY_MAX_BYTES)
    if error:
        return _error(
            runtime, error, 413 if error == "payload_too_large" else 400)
    if set(data) - {"attestation", "note", "sessionToken"}:
        return _error(runtime, "unsupported_ad_field")
    account_bi, _name = await _session(runtime, data)
    if not account_bi:
        return _error(runtime, "unauthorized", 401)
    row = await _owned_row(runtime, account_bi, ad_id)
    if not row:
        return _error(runtime, "not_found", 404)
    status = str(row.get("status") or "")
    now = runtime.now()
    if action == "submit":
        # An approval past its 180 days is resubmitted the same way a draft
        # is; a live approval has nothing to submit.
        expired = status == "approved" and int(
            row.get("approved_until") or 0) <= now
        if status not in ("draft", "rejected", "withdrawn") and not expired:
            return _error(runtime, "invalid_transition", 409, current=status)
        if data.get("attestation") is not True:
            return _error(runtime, "attestation_required")
        if await _pending_count(
                runtime, account_bi, ad_id) >= MAX_PENDING_PER_ACCOUNT:
            return _error(runtime, "pending_limit_reached", 409,
                          limit=MAX_PENDING_PER_ACCOUNT)
        note = clean_text(data.get("note"), NOTE_MAX) or str(
            row.get("owner_note") or "")
        await runtime.d1_run(
            "UPDATE ad_network_ads SET status='pending',owner_note=?,"
            "submitted_at=?,updated_at=? "
            "WHERE ad_id=? AND account_bi=? AND status=?",
            note, now, now, ad_id, account_bi, status)
    elif action == "withdraw":
        if status not in ("pending", "approved", "rejected"):
            return _error(runtime, "invalid_transition", 409, current=status)
        await runtime.d1_run(
            "UPDATE ad_network_ads SET status='withdrawn',approved_hash='',"
            "approved_until=0,updated_at=? "
            "WHERE ad_id=? AND account_bi=? AND status=?",
            now, ad_id, account_bi, status)
    else:
        # pause/resume is the owner's own switch and needs no review: it can
        # only ever show less than what was approved.
        if status == "suspended":
            return _error(runtime, "ad_suspended", 409)
        await runtime.d1_run(
            "UPDATE ad_network_ads SET paused=?,updated_at=? "
            "WHERE ad_id=? AND account_bi=?",
            1 if action == "pause" else 0, now, ad_id, account_bi)
    return await _single_owner_ad(runtime, account_bi, ad_id)


async def _delete(runtime, ad_id):
    account_bi, _name = await _session(runtime)
    if not account_bi:
        return _error(runtime, "unauthorized", 401)
    row = await _owned_row(runtime, account_bi, ad_id)
    if not row:
        return _error(runtime, "not_found", 404)
    # The review trail outlives the creative on purpose: it records what a
    # reviewer decided about a content hash, and holds no ad text.
    await runtime.batch([
        ("DELETE FROM ad_network_daily_stats WHERE ad_id=?", (ad_id,)),
        ("DELETE FROM ad_network_reports WHERE ad_id=?", (ad_id,)),
        ("DELETE FROM ad_network_ads WHERE ad_id=? AND account_bi=?",
         (ad_id, account_bi)),
    ])
    return _response(runtime, {"ok": True, "deleted": ad_id})


async def _reviewer(runtime, data=None):
    account_bi, name = await _session(runtime, data)
    if not account_bi:
        return "", "", _error(runtime, "unauthorized", 401)
    if not await runtime.is_admin(name):
        await runtime.audit(
            name, "ad_network.review", "ad", "", "denied",
            {"reason": "insufficient_role"})
        return "", "", _error(runtime, "forbidden", 403)
    return account_bi, name, None


async def _review_queue(runtime):
    _reviewer_bi, _name, denied = await _reviewer(runtime)
    if denied is not None:
        return denied
    wanted = str(runtime.query_params().get("status") or "pending").lower()
    if wanted == "reported":
        where = (
            "EXISTS(SELECT 1 FROM ad_network_reports r "
            "WHERE r.ad_id=a.ad_id AND r.resolved_at=0)")
        args = ()
    elif wanted in STATUSES:
        where = "a.status=?"
        args = (wanted,)
    else:
        return _error(runtime, "invalid_status")
    now = runtime.now()
    rows = await runtime.d1_all(
        "SELECT a.*,"
        "(SELECT COUNT(*) FROM ad_network_reports r "
        "WHERE r.ad_id=a.ad_id AND r.resolved_at=0) AS open_reports,"
        "(SELECT GROUP_CONCAT(g.role) FROM role_grants g "
        "WHERE g.account_bi=a.account_bi AND g.role IN (" + _ROLE_SQL + ") "
        "AND g.revoked_at=0 AND (g.expires_at=0 OR g.expires_at>?)) AS roles "
        "FROM ad_network_ads a WHERE " + where + " "
        "ORDER BY a.submitted_at,a.ad_id LIMIT ?",
        now, *args, REVIEW_QUEUE_LIMIT,
    ) or []
    ads = []
    for row in rows:
        tier = tier_for_roles(_split(row.get("roles")))
        ad = _owner_ad(row, delivery_state(row, tier, now), _no_clicks())
        ad.pop("clicks", None)
        owner = await runtime.open(str(row.get("owner_sealed") or ""))
        ad["owner"] = str((owner or {}).get("name") or "")
        ad["ownerTier"] = _tier_public(tier)
        ad["openReports"] = int(row.get("open_reports") or 0)
        ads.append(ad)
    counts = {
        str(row.get("status") or ""): int(row.get("n") or 0)
        for row in await runtime.d1_all(
            "SELECT status,COUNT(*) AS n FROM ad_network_ads "
            "GROUP BY status") or []
    }
    reported = await runtime.d1_first(
        "SELECT COUNT(DISTINCT ad_id) AS n FROM ad_network_reports "
        "WHERE resolved_at=0")
    if int((reported or {}).get("n") or 0):
        counts["reported"] = int(reported["n"])
    return _response(runtime, {
        "ok": True,
        "status": wanted,
        "ads": ads,
        "counts": counts,
        "prohibited": [
            {"id": key, "label": label} for key, label in PROHIBITED.items()],
    })


async def _review_detail(runtime, ad_id):
    _reviewer_bi, _name, denied = await _reviewer(runtime)
    if denied is not None:
        return denied
    history = await runtime.d1_all(
        "SELECT action,self_review,reason,codes,content_hash,created_at "
        "FROM ad_network_reviews WHERE ad_id=? "
        "ORDER BY created_at DESC LIMIT ?", ad_id, REVIEW_HISTORY_LIMIT)
    reports = await runtime.d1_all(
        "SELECT reason,note,created_at,resolved_at FROM ad_network_reports "
        "WHERE ad_id=? ORDER BY created_at DESC LIMIT ?",
        ad_id, REVIEW_HISTORY_LIMIT)
    return _response(runtime, {
        "ok": True,
        "history": [{
            "action": str(row.get("action") or ""),
            "selfReview": bool(int(row.get("self_review") or 0)),
            "reason": str(row.get("reason") or ""),
            "codes": _split(row.get("codes")),
            "contentHash": str(row.get("content_hash") or ""),
            "createdAt": int(row.get("created_at") or 0),
        } for row in history or []],
        "reports": [{
            "reason": str(row.get("reason") or ""),
            "note": str(row.get("note") or ""),
            "createdAt": int(row.get("created_at") or 0),
            "resolved": bool(int(row.get("resolved_at") or 0)),
        } for row in reports or []],
    })


async def _review(runtime, ad_id):
    data, error = await runtime.json_body(BODY_MAX_BYTES)
    if error:
        return _error(
            runtime, error, 413 if error == "payload_too_large" else 400)
    if set(data) - {
            "action", "reason", "codes", "contentHash", "sessionToken"}:
        return _error(runtime, "unsupported_review_field")
    reviewer_bi, name, denied = await _reviewer(runtime, data)
    if denied is not None:
        return denied
    action = str(data.get("action") or "").strip().lower()
    if action not in REVIEW_ACTIONS:
        return _error(runtime, "invalid_action")
    codes = data.get("codes") or []
    if not isinstance(codes, list) or len(codes) > len(PROHIBITED):
        return _error(runtime, "invalid_codes")
    codes = sorted({str(code or "").strip().lower() for code in codes})
    if any(code not in PROHIBITED for code in codes):
        return _error(runtime, "invalid_codes")
    reason = clean_text(data.get("reason"), REASON_MAX)
    if action in ("reject", "suspend") and not (reason or codes):
        # The owner is owed the rule they broke.
        return _error(runtime, "reason_required")
    row = await runtime.d1_first(
        "SELECT ad_id,account_bi,status,content_hash FROM ad_network_ads "
        "WHERE ad_id=?", ad_id)
    if not row:
        return _error(runtime, "not_found", 404)
    allowed_from, next_status = REVIEW_ACTIONS[action]
    status = str(row.get("status") or "")
    if status not in allowed_from:
        return _error(runtime, "invalid_transition", 409, current=status)
    stored_hash = str(row.get("content_hash") or "")
    if action == "approve" and str(
            data.get("contentHash") or "") != stored_hash:
        # The reviewer must name the creative they read.  If the owner edited
        # it after the queue was loaded, the approval does not carry over.
        return _error(runtime, "creative_changed", 409,
                      contentHash=stored_hash)
    now = runtime.now()
    self_review = 1 if str(row.get("account_bi") or "") == reviewer_bi else 0
    approved = action == "approve"
    statements = [
        (
            "UPDATE ad_network_ads SET status=?,approved_hash=?,"
            "approved_until=?,review_reason=?,review_codes=?,reviewed_at=?,"
            "updated_at=? WHERE ad_id=? AND status=? AND content_hash=?",
            (next_status, stored_hash if approved else "",
             now + APPROVAL_TTL_MS if approved else 0, reason,
             ",".join(codes), now, now, ad_id, status, stored_hash),
        ),
        (
            "INSERT INTO ad_network_reviews"
            "(review_id,ad_id,action,reviewer_bi,self_review,reason,codes,"
            "content_hash,created_at) VALUES(?,?,?,?,?,?,?,?,?)",
            (runtime.new_id(), ad_id, action, reviewer_bi, self_review,
             reason, ",".join(codes), stored_hash, now),
        ),
    ]
    if action != "reinstate":
        # A decision answers the reports that prompted it.
        statements.append((
            "UPDATE ad_network_reports SET resolved_at=? "
            "WHERE ad_id=? AND resolved_at=0", (now, ad_id)))
    await runtime.batch(statements)
    await runtime.audit(
        name, "ad_network." + action, "ad", ad_id, "success",
        {"codes": codes, "selfReview": bool(self_review),
         "contentHash": stored_hash})
    return _response(runtime, {
        "ok": True, "id": ad_id, "status": next_status,
        "selfReview": bool(self_review),
    })


async def cleanup(runtime):
    """Cron hook: drop click counters past the retention window."""

    await runtime.d1_run(
        "DELETE FROM ad_network_daily_stats WHERE day<?",
        _day(runtime.now() - STATS_RETENTION_DAYS * DAY_MS))


def _method_not_allowed(runtime, allow):
    return _response(
        runtime, {"ok": False, "error": "method_not_allowed"}, 405, allow)


async def handle(runtime, path):
    tail = _tail(path)
    method = runtime.method()
    if tail == ["policy"]:
        if method != "GET":
            return _method_not_allowed(runtime, "GET")
        return _response(
            runtime, policy_document(), cache_seconds=POLICY_CACHE_SECONDS)
    if tail == ["serve"]:
        if method != "GET":
            return _method_not_allowed(runtime, "GET")
        await runtime.ensure_schema()
        return await _serve(runtime)
    if tail == ["mine"]:
        if method != "GET":
            return _method_not_allowed(runtime, "GET")
        await runtime.ensure_schema()
        return await _mine(runtime)
    if tail == ["review"]:
        if method != "GET":
            return _method_not_allowed(runtime, "GET")
        await runtime.ensure_schema()
        return await _review_queue(runtime)
    if not tail:
        if method != "POST":
            return _method_not_allowed(runtime, "POST")
        await runtime.ensure_schema()
        return await _create(runtime)
    ad_id = str(tail[0] or "").lower()
    if not valid_id(ad_id) or len(tail) > 2:
        return _error(runtime, "not_found", 404)
    if len(tail) == 1:
        if method not in ("PUT", "PATCH", "DELETE"):
            return _method_not_allowed(runtime, "PUT, PATCH, DELETE")
        await runtime.ensure_schema()
        if method == "DELETE":
            return await _delete(runtime, ad_id)
        return await _update(runtime, ad_id)
    action = str(tail[1] or "").lower()
    if action == "review":
        if method not in ("GET", "POST"):
            return _method_not_allowed(runtime, "GET, POST")
        await runtime.ensure_schema()
        if method == "GET":
            return await _review_detail(runtime, ad_id)
        return await _review(runtime, ad_id)
    if action in ("click", "report") or action in OWNER_ACTIONS:
        if method != "POST":
            return _method_not_allowed(runtime, "POST")
        await runtime.ensure_schema()
        if action == "click":
            return await _click(runtime, ad_id)
        if action == "report":
            return await _report(runtime, ad_id)
        return await _owner_action(runtime, ad_id, action)
    return _error(runtime, "not_found", 404)
