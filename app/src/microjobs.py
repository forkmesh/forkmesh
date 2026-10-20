"""Paid microjobs: tiny, independently verified community contributions.

A microjob is a small unit of work anybody with a ForkMesh account can pick up
and be paid a fixed, deliberately tiny amount of SOL for. Three jobs ship:

``issue_report``
    File a genuinely new issue. Before the submission is allowed anywhere near
    the repository's issue inbox, a Workers AI model reads the candidate
    alongside the repository's existing open/closed issues and a code search
    for the words the reporter used, and answers one question: is this new, or
    is it a duplicate of / already addressed by work that is already in the
    tree? Only a claim the model calls new files an issue.

``merged_pull``
    Land a pull request that was opened with the ForkMesh MCP agent tool. The
    tool stamps ``client: forkmesh-mcp-v1`` plus a node-signed ``clientSig``
    into the pull's own record, and this module re-reads that record from the
    repository and re-verifies the marker. A pull opened by hand, by the
    desktop UI, or by any other client is a fine pull — it is simply not a
    paid microjob, and the issue/pull pages say so.

``referral_post``
    Post your referral link somewhere public. Payment requires both halves:
    the page must still contain the link when the Worker fetches it, and the
    hostname must already have sent real, counted visitors (``site_referrers``)
    while the referrer's own counters moved (``referral_stats``). A screenshot,
    a promise, or a page nobody visited is not a payout.

Custody is unchanged from the community reward pool: this module only ever
authors a *public, unsigned* ``chain_intents`` plan. The repository owner's
desktop client is the only thing holding a key, and it re-validates the whole
plan locally before it signs anything. Nothing here can move funds.

The module is pure — no ``js``/``workers`` imports and no D1 handle — so both
the Worker and the test suite import it directly. Every side effect goes
through the narrow runtime adapter the Worker hands in.
"""

import re
from urllib.parse import parse_qsl, urlencode, urlsplit, urlunsplit


PREFIX = "/api/microjobs"
CLAIMS_PART = "claims"
BODY_MAX_BYTES = 32 * 1024

LAMPORTS_PER_SOL = 1_000_000_000

# --- Job catalog -------------------------------------------------------------
# Amounts are deliberately tiny: a microjob is a thank-you for a small piece of
# real work, not a wage and not an investment return. They are fixed here (not
# operator-configurable per claim) so the number a contributor reads on the
# page is exactly the number that reaches the unsigned payout plan.
JOBS = {
    "issue_report": {
        "title": "Report a valid issue",
        "lamports": 5_000_000,
        "summary": (
            "Find something broken or missing and write it up. A Cloudflare "
            "Workers AI reviewer checks it against every existing issue and "
            "the code already in the tree first, so only genuinely new "
            "reports are opened."
        ),
        "requirement": (
            "The reviewer must find no existing issue covering it and no code "
            "already addressing it."
        ),
    },
    "merged_pull": {
        "title": "Land a pull request opened with the MCP agent tool",
        "lamports": 50_000_000,
        "summary": (
            "Fix something and open the pull request with the ForkMesh MCP "
            "agent tool. Only MCP-opened pull requests are payable, because "
            "the tool is what stamps the signed marker this check reads."
        ),
        "requirement": (
            "The pull must be merged and its record must carry the MCP "
            "client marker signed by the node key that opened it."
        ),
    },
    "referral_post": {
        "title": "Post your referral link where people will click it",
        "lamports": 10_000_000,
        "summary": (
            "Share your forkmesh.com/r/<you> link on a public page. Payment "
            "needs the link to still be on the page and the site to have "
            "actually sent counted visitors."
        ),
        "requirement": (
            "The public page must contain your referral link and its hostname "
            "must already appear in the referring-site counters."
        ),
    },
}
JOB_IDS = tuple(JOBS)

# --- Claim lifecycle ---------------------------------------------------------
# pending_review  a posted referral's counters are not there yet; the hourly
#                 payout phase re-checks it and nothing is paid meanwhile
# approved        verified; waiting to be batched into an unsigned payout plan
# queued          in a public chain_intents plan the owner's desktop may sign
# paid            a finalized on-chain transfer matched the exact public plan
# rejected        verification said no; the reason is returned to the claimant
STATUSES = ("pending_review", "approved", "queued", "paid", "rejected")
OPEN_STATUSES = ("pending_review", "approved", "queued")

# Anti-farming bounds. None of these are claims about personhood: they raise
# the cost of grinding the same job repeatedly and cap the blast radius of a
# verification bug, and the public policy payload says exactly that.
MAX_OPEN_CLAIMS_PER_ACCOUNT = 12
MAX_CLAIMS_PER_ACCOUNT_PER_DAY = 8
MAX_DAILY_LAMPORTS_PER_ACCOUNT = 120_000_000
DAY_MS = 24 * 60 * 60 * 1000

# One unsigned plan carries at most this many transfers, matching the legacy
# Solana message limit the reward pool already batches against.
MAX_TRANSFERS_PER_INTENT = 8
MAX_PAYOUT_INTENT_LAMPORTS = 500_000_000
PAYOUT_INTENT_KIND = "microjob_payout"
PAYOUT_POLICY = "microjob-payout-v1"
PAYOUT_INTENT_TTL_MS = 4 * 60 * 60 * 1000

# The referral job needs both signals before it is payable.
MIN_REFERRAL_SITE_VISITS = 3
MIN_REFERRAL_CLICKS = 3

MAX_URL = 1200
MAX_TITLE = 240
MAX_ISSUE_BODY = 8000
MAX_REASON = 300
MAX_AI_ISSUES = 40
MAX_AI_CODE_HITS = 12
MAX_FETCH_BYTES = 512 * 1024
MAX_PENDING_REFERRAL_REVIEWS = 4

SOLANA_ADDRESS_RE = re.compile(r"^[1-9A-HJ-NP-Za-km-z]{32,44}$")
SEGMENT_RE = re.compile(r"^[A-Za-z0-9._-]{1,80}$")
HOST_RE = re.compile(
    r"^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?"
    r"(?:\.[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?)+$"
)
SENSITIVE_QUERY_KEYS = frozenset({
    "access_token", "api_key", "apikey", "auth", "authorization", "code",
    "credential", "email", "key", "password", "secret", "session",
    "sessionid", "sig", "signature", "token",
})

# The exact marker tools/forkmesh_mcp_server.py writes into a pull it opens,
# and the canonical string it signs with the node identity key. Both halves are
# required: the plain field says "the MCP tool wrote this", the signature says
# "and the node that owns this pull agrees", so copying the field alone onto a
# hand-written pull does not make it payable.
MCP_CLIENT_MARKER = "forkmesh-mcp-v1"
MCP_PULL_CANONICAL_PREFIX = "forkmesh-mcp-pull-v1"


def mcp_pull_canonical(number, base, head, author, ts):
    """Canonical string the MCP tool signs when it opens a pull request.

    Reproduced byte-for-byte in ``tools/forkmesh_mcp_server.py``. It binds the
    marker to this pull number, its exact branches, the opening node, and when
    — so one signed marker cannot be lifted onto another pull.
    """
    return "\n".join((
        MCP_PULL_CANONICAL_PREFIX,
        str(int(number or 0)),
        str(base or ""),
        str(head or ""),
        str(author or ""),
        str(int(ts or 0)),
    ))


def public_catalog():
    """The job list exactly as the microjobs page renders it."""
    return [{
        "id": job_id,
        "title": JOBS[job_id]["title"],
        "summary": JOBS[job_id]["summary"],
        "requirement": JOBS[job_id]["requirement"],
        "lamports": JOBS[job_id]["lamports"],
        "sol": lamports_to_sol(JOBS[job_id]["lamports"]),
    } for job_id in JOB_IDS]


def public_policy():
    """Every limit and caveat the page must state before somebody works."""
    return {
        "custody": "external-local-signer",
        "network": "solana",
        "maxOpenClaims": MAX_OPEN_CLAIMS_PER_ACCOUNT,
        "maxClaimsPerDay": MAX_CLAIMS_PER_ACCOUNT_PER_DAY,
        "maxDailySol": lamports_to_sol(MAX_DAILY_LAMPORTS_PER_ACCOUNT),
        "minimumReferralSiteVisits": MIN_REFERRAL_SITE_VISITS,
        "minimumReferralClicks": MIN_REFERRAL_CLICKS,
        "mcpClientMarker": MCP_CLIENT_MARKER,
        "limitations": [
            "A microjob is a fixed thank-you for a small piece of verified "
            "work. It is not employment, an investment, or a promise of "
            "future payment.",
            "ForkMesh never holds a payout key. An approved claim becomes a "
            "public unsigned transfer plan that the repository owner's own "
            "desktop client reviews and signs locally.",
            "Rate limits and de-duplication raise the cost of farming a job. "
            "They are not proof of personhood and are not claimed to be.",
            "Pull requests only count when they were opened with the "
            "ForkMesh MCP agent tool, which stamps the signed marker this "
            "check reads.",
        ],
    }


def lamports_to_sol(lamports):
    """Exact SOL string for a lamport amount (no float rounding surprises)."""
    lamports = max(0, int(lamports or 0))
    whole, fraction = divmod(lamports, LAMPORTS_PER_SOL)
    text = "%d.%09d" % (whole, fraction)
    return text.rstrip("0").rstrip(".") if "." in text else text


def _text(value, maximum, fallback=""):
    clean = "".join(
        character if character.isprintable() or character in "\n\t" else " "
        for character in str(value or "")
    )
    clean = clean.strip()
    return (clean or fallback)[:maximum]


def _one_line(value, maximum, fallback=""):
    return _text(" ".join(str(value or "").split()), maximum, fallback)


def normalize_public_url(value):
    """Return a safe public HTTPS URL and its hostname, or ("", "").

    Fails closed on anything that is not an ordinary public web page: a
    non-HTTPS scheme, embedded credentials, a bare IP literal, a non-443 port,
    or an internal/loopback name. The Worker fetches this URL to check the
    referral link is really posted, so it must never become a probe for
    anything on the inside.
    """
    raw = str(value or "").strip()
    if not raw or len(raw.encode("utf-8")) > MAX_URL * 2:
        return "", ""
    try:
        parsed = urlsplit(raw)
        host = str(parsed.hostname or "").strip().lower().rstrip(".")
        if (
            parsed.scheme != "https"
            or not host
            or not HOST_RE.fullmatch(host)
            or parsed.username
            or parsed.password
            or host.endswith((".local", ".internal", ".localhost"))
            or host in {"localhost", "forkmesh.internal"}
            or host.replace(".", "").isdigit()
            or parsed.port not in (None, 443)
        ):
            return "", ""
        query = urlencode([
            (
                str(key)[:120],
                "[redacted]"
                if str(key).lower() in SENSITIVE_QUERY_KEYS
                else str(item)[:300],
            )
            for key, item in parse_qsl(
                parsed.query, keep_blank_values=True, max_num_fields=40)
        ], doseq=True)
        normalized = urlunsplit((
            "https", host, parsed.path or "/", query, ""))[:MAX_URL]
        return normalized, host
    except Exception:
        return "", ""


def referral_link_variants(origin, account):
    """Every spelling of one account's referral link a post might carry."""
    account = str(account or "").strip().lower()
    if not account:
        return []
    host = ""
    try:
        host = str(urlsplit(str(origin or "")).hostname or "").strip().lower()
    except Exception:
        host = ""
    host = host or "forkmesh.com"
    path = "/r/" + account
    return [
        "https://" + host + path,
        "http://" + host + path,
        "//" + host + path,
        host + path,
    ]


def post_contains_referral_link(page_text, origin, account):
    """True when the fetched page still carries this account's share link.

    Compared case-insensitively against the raw response text: the link may sit
    in an anchor href, in a data attribute, or in plain prose, and every one of
    those counts as "you posted it".
    """
    haystack = str(page_text or "").lower()
    if not haystack:
        return False
    return any(
        variant.lower() in haystack
        for variant in referral_link_variants(origin, account)
    )


def parse_pull_frontmatter(text):
    """Reduce a ``.forkmesh/pulls/<n>/pull.md`` record to its header fields.

    The desktop's PullStore, the MCP tool, and the website all write the same
    ``---``-delimited ``key: value`` header. Unknown keys and the body are
    ignored; a record without a header yields ``{}``.
    """
    raw = str(text or "")
    if not raw.lstrip().startswith("---"):
        return {}
    body = raw.lstrip()[3:]
    end = body.find("\n---")
    if end < 0:
        return {}
    fields = {}
    for line in body[:end].splitlines():
        if ":" not in line:
            continue
        key, _, value = line.partition(":")
        key = key.strip()
        if key and key not in fields:
            fields[key] = value.strip()
    return fields


def pull_is_merged(fields):
    status = str((fields or {}).get("status") or "").strip().lower()
    return status in ("merged", "closed-merged")


def mcp_marker_fields(fields):
    """The (author, marker, signature, canonical) an MCP pull must present."""
    fields = fields or {}
    try:
        number = int(str(fields.get("number") or "0").strip() or 0)
    except (TypeError, ValueError):
        number = 0
    try:
        ts = int(str(fields.get("ts") or "0").strip() or 0)
    except (TypeError, ValueError):
        ts = 0
    return {
        "number": number,
        "ts": ts,
        "base": str(fields.get("base") or "").strip(),
        "head": str(fields.get("head") or "").strip(),
        "author": str(fields.get("author") or "").strip(),
        "client": str(fields.get("client") or "").strip(),
        "clientSig": str(fields.get("clientSig") or "").strip(),
    }


# --- Duplicate review --------------------------------------------------------

AI_SYSTEM_PROMPT = (
    "You are a strict issue triager for a software repository. You decide "
    "whether a newly reported issue is genuinely new, or whether it is a "
    "duplicate of an existing issue or is already addressed by code that is "
    "already in the repository. Judge only what the supplied context shows. "
    "When the context is thin, say so rather than guessing. Answer with JSON "
    "only."
)

AI_VERDICT_SCHEMA = {
    "name": "microjob_issue_review",
    "schema": {
        "type": "object",
        "properties": {
            "verdict": {
                "type": "string",
                "enum": ["new", "duplicate", "already_addressed", "unclear"],
            },
            "duplicateOf": {"type": "integer"},
            "reason": {"type": "string"},
        },
        "required": ["verdict", "reason"],
    },
}


def issue_review_prompt(title, body, issues, code_hits):
    """The exact user prompt the duplicate reviewer sees.

    Existing issues arrive newest-first and are truncated hard: the model gets
    enough of each to recognise the same complaint, and the prompt stays inside
    a small-model context even for a busy repository.
    """
    lines = ["NEW REPORT", "title: " + _one_line(title, MAX_TITLE),
             "body:", _text(body, 1200), ""]
    lines.append("EXISTING ISSUES (newest first)")
    if not issues:
        lines.append("(none)")
    for record in list(issues or [])[:MAX_AI_ISSUES]:
        if not isinstance(record, dict):
            continue
        lines.append("#%d [%s] %s :: %s" % (
            int(record.get("number") or 0),
            _one_line(record.get("status"), 20, "open"),
            _one_line(record.get("title"), 160),
            _one_line(record.get("body"), 240),
        ))
    lines.append("")
    lines.append("CODE ALREADY IN THE REPOSITORY MATCHING THE REPORT")
    if not code_hits:
        lines.append("(no matching code found)")
    for hit in list(code_hits or [])[:MAX_AI_CODE_HITS]:
        if not isinstance(hit, dict):
            continue
        lines.append("%s:%d: %s" % (
            _one_line(hit.get("path"), 160),
            int(hit.get("line") or 0),
            _one_line(hit.get("snippet"), 160),
        ))
    lines.append("")
    lines.append(
        'Answer with JSON: {"verdict":"new|duplicate|already_addressed|'
        'unclear","duplicateOf":<issue number or 0>,"reason":"<one short '
        'sentence>"}'
    )
    return "\n".join(lines)


def search_terms(title, body):
    """Distinctive words from a report, for the repository code search.

    Deliberately crude: the gateway search is a fixed-string ``git grep``, so
    the longest few non-generic words from the title (falling back to the body)
    are what actually produce useful hits.
    """
    stop = {
        "about", "after", "again", "also", "because", "before", "being",
        "could", "doesn", "does", "error", "every", "from", "have", "issue",
        "into", "just", "like", "make", "more", "must", "never", "only",
        "other", "over", "should", "some", "still", "than", "that", "them",
        "then", "there", "these", "they", "this", "used", "using", "when",
        "where", "which", "while", "will", "with", "would", "your",
    }
    words = []
    for source in (title, body):
        for word in re.findall(r"[A-Za-z_][A-Za-z0-9_]{3,39}", str(source or "")):
            lowered = word.lower()
            if lowered in stop or lowered in words:
                continue
            words.append(lowered)
        if len(words) >= 3:
            break
    words.sort(key=len, reverse=True)
    return words[:3]


def parse_ai_verdict(raw):
    """Normalize whatever the model returned into a decision this can act on.

    Workers AI answers either a parsed object (JSON mode) or a string that may
    wrap the object in prose or a fenced block. Anything unrecognisable is
    ``unclear`` — never ``new``, so an unparseable answer can never pay.
    """
    data = raw
    if isinstance(raw, str):
        text = raw.strip()
        start, end = text.find("{"), text.rfind("}")
        data = None
        if 0 <= start < end:
            try:
                import json as _json
                data = _json.loads(text[start:end + 1])
            except Exception:
                data = None
    if not isinstance(data, dict):
        return {"verdict": "unclear", "duplicateOf": 0,
                "reason": "The reviewer did not return a usable answer."}
    verdict = str(data.get("verdict") or "").strip().lower()
    if verdict not in ("new", "duplicate", "already_addressed", "unclear"):
        verdict = "unclear"
    try:
        duplicate_of = int(data.get("duplicateOf") or 0)
    except (TypeError, ValueError):
        duplicate_of = 0
    return {
        "verdict": verdict,
        "duplicateOf": max(0, min(999_999_999, duplicate_of)),
        "reason": _one_line(data.get("reason"), MAX_REASON,
                            "No reason was given."),
    }


def verdict_outcome(verdict):
    """(claim status, whether to file the issue, public message)."""
    kind = str((verdict or {}).get("verdict") or "unclear")
    reason = _one_line((verdict or {}).get("reason"), MAX_REASON)
    if kind == "new":
        return "approved", True, (
            "Reviewed as a new report: " + reason if reason
            else "Reviewed as a new report.")
    if kind == "duplicate":
        number = int((verdict or {}).get("duplicateOf") or 0)
        return "rejected", False, (
            "Already reported%s. %s" % (
                " as #%d" % number if number else "", reason)).strip()
    if kind == "already_addressed":
        return "rejected", False, (
            "Already addressed by code in the repository. " + reason).strip()
    return "pending_review", False, (
        "The reviewer could not establish that this report is new, so no "
        "issue or claim was opened. Try again when repository review is "
        "available. " + reason).strip()


# --- Payout planning ---------------------------------------------------------

def payout_plan(source_address, network, transfers, now):
    """The public, unsigned transfer plan for a batch of approved claims.

    Shaped exactly like the community reward pool's plans so the desktop
    signer's existing custody/network/transfer validation applies unchanged.
    Nothing here is signed, and no key is required to build it.
    """
    # Only the three public per-transfer fields reach the plan the desktop
    # signer validates. ``claimIds`` is bookkeeping for queue_payouts and is
    # deliberately not published on-chain or in the signing job.
    transfers = [{
        "address": str((transfer or {}).get("address") or ""),
        "lamports": int((transfer or {}).get("lamports") or 0),
        "nodeId": str((transfer or {}).get("nodeId") or ""),
    } for transfer in list(transfers or [])[:MAX_TRANSFERS_PER_INTENT]]
    return {
        "v": 2,
        "network": str(network or ""),
        "sourceAddress": str(source_address or ""),
        "custody": {
            "forkMeshHoldsUserKeys": False,
            "fundsRemainInSourceWalletUntilSigned": True,
        },
        "signing": {
            "required": True,
            "performed": False,
            "method": "external-local-qt",
        },
        "transfers": transfers,
        "policy": PAYOUT_POLICY,
        "distributedLamports": sum(
            int(transfer.get("lamports") or 0) for transfer in transfers),
        "batch": {
            "claimCount": len(transfers),
            "maximumTransfersPerTransaction": MAX_TRANSFERS_PER_INTENT,
        },
        "generatedAt": int(now or 0),
        "notice": (
            "Fixed thank-you amounts for verified microjobs. Not employment, "
            "not an investment, and no future payment is promised."
        ),
    }


def batch_transfers(claims):
    """Group approved claims into payable transfers, one per wallet.

    Two approved claims by the same person collapse into one transfer, because
    a legacy Solana message may not repeat a destination and the desktop signer
    rejects a duplicate destination outright.
    """
    order = []
    totals = {}
    claim_ids = {}
    distributed = 0
    for claim in claims or []:
        wallet = str((claim or {}).get("wallet") or "")
        lamports = int((claim or {}).get("lamports") or 0)
        if not SOLANA_ADDRESS_RE.fullmatch(wallet) or lamports <= 0:
            continue
        # Keep the authored plan inside the independent half-SOL limit the
        # desktop signer enforces. Stop at the first claim that would cross it
        # so older approved work remains ordered ahead of newer work.
        if distributed + lamports > MAX_PAYOUT_INTENT_LAMPORTS:
            break
        if wallet not in totals:
            if len(order) >= MAX_TRANSFERS_PER_INTENT:
                continue
            order.append(wallet)
            totals[wallet] = 0
            claim_ids[wallet] = []
        totals[wallet] += lamports
        distributed += lamports
        claim_ids[wallet].append(str(claim.get("claimId") or ""))
    # ``nodeId`` is a truncated human-readable label. ``claimIds`` is the whole
    # list and is what marks claims queued: reading the ids back out of the
    # truncated label would silently leave the claims past the cut approved,
    # and the next batch would pay them a second time.
    return [{
        "address": wallet,
        "lamports": totals[wallet],
        "nodeId": ",".join(claim_ids[wallet])[:160],
        "claimIds": list(claim_ids[wallet]),
    } for wallet in order]


# --- HTTP surface ------------------------------------------------------------

def _response(runtime, payload, status=200):
    return runtime.response(
        payload,
        status=status,
        cache_control="no-store, max-age=0, must-revalidate",
        extra_headers={"x-content-type-options": "nosniff"},
    )


def public_claim(record, row):
    """One claim as its own claimant sees it. No wallet, no reviewer internals."""
    record = record if isinstance(record, dict) else {}
    job = str(row.get("job") or "")
    return {
        "id": str(row.get("claim_id") or ""),
        "job": job,
        "jobTitle": JOBS.get(job, {}).get("title", job),
        "status": str(row.get("status") or ""),
        "lamports": int(row.get("amount_lamports") or 0),
        "sol": lamports_to_sol(row.get("amount_lamports")),
        "subject": _one_line(record.get("subject"), 200),
        "detail": _one_line(record.get("detail"), MAX_REASON),
        "createdAt": int(row.get("created_at") or 0),
        "updatedAt": int(row.get("updated_at") or 0),
        "transactionSignature": str(row.get("tx_signature") or ""),
    }


async def _claims_for(runtime, account_bi):
    rows = await runtime.d1_all(
        "SELECT claim_id,job,status,amount_lamports,data,created_at,"
        "updated_at,tx_signature FROM microjob_claims WHERE account_bi=? "
        "ORDER BY created_at DESC LIMIT 50",
        str(account_bi),
    )
    claims = []
    for row in rows or []:
        try:
            record = await runtime.open(row.get("data"))
        except Exception:
            record = None
        claims.append(public_claim(record, row))
    return claims


async def _earned_lamports(runtime, account_bi):
    row = await runtime.d1_first(
        "SELECT COALESCE(SUM(amount_lamports),0) AS total "
        "FROM microjob_claims WHERE account_bi=? AND status='paid'",
        str(account_bi),
    )
    return max(0, int((row or {}).get("total") or 0))


async def _quota_error(runtime, account_bi, lamports, now):
    """The reason this account may not open another claim right now, or ""."""
    open_row = await runtime.d1_first(
        "SELECT COUNT(*) AS count FROM microjob_claims "
        "WHERE account_bi=? AND status IN ('pending_review','approved',"
        "'queued')",
        str(account_bi),
    )
    if int((open_row or {}).get("count") or 0) >= MAX_OPEN_CLAIMS_PER_ACCOUNT:
        return "open_claim_limit"
    day_row = await runtime.d1_first(
        "SELECT COUNT(*) AS count,COALESCE(SUM(amount_lamports),0) AS total "
        "FROM microjob_claims WHERE account_bi=? AND created_at>=? "
        "AND status<>'rejected'",
        str(account_bi), int(now) - DAY_MS,
    )
    if int((day_row or {}).get("count") or 0) >= MAX_CLAIMS_PER_ACCOUNT_PER_DAY:
        return "daily_claim_limit"
    if (int((day_row or {}).get("total") or 0) + int(lamports)
            > MAX_DAILY_LAMPORTS_PER_ACCOUNT):
        return "daily_amount_limit"
    return ""


def _repo_names(data):
    owner = _one_line((data or {}).get("owner"), 80).lower()
    repo = _one_line((data or {}).get("repo"), 80).lower()
    if not SEGMENT_RE.fullmatch(owner) or not SEGMENT_RE.fullmatch(repo):
        return "", ""
    return owner, repo


async def _review_issue(runtime, owner, repo, title, body):
    """Run the duplicate/already-addressed review for one candidate report."""
    numbers = await runtime.recent_issue_numbers(owner, repo)
    if numbers is None:
        return {
            "verdict": "unclear", "duplicateOf": 0,
            "reason": (
                "No online host could serve the repository's existing issues "
                "for comparison."
            ),
        }, [], []
    # Search the complete issue ledger for the report's distinctive terms,
    # then put those matches ahead of the newest issues. The record loader has
    # a hard batch cap, so this gives the model relevant old issues as well as
    # recent context instead of silently reviewing only the newest page.
    matching_numbers = []
    terms = search_terms(title, body)
    for term in terms:
        for match in await runtime.search_issues(owner, repo, term) or []:
            try:
                match_number = int((match or {}).get("number") or 0)
            except (AttributeError, TypeError, ValueError):
                match_number = 0
            if match_number > 0 and match_number not in matching_numbers:
                matching_numbers.append(match_number)
    issue_numbers = matching_numbers + [
        number for number in numbers if number not in matching_numbers]
    issues = await runtime.load_issue_records(owner, repo, issue_numbers)
    code_hits = []
    for term in terms:
        found = await runtime.search_code(owner, repo, term)
        for hit in found or []:
            if len(code_hits) >= MAX_AI_CODE_HITS:
                break
            code_hits.append(hit)
        if len(code_hits) >= MAX_AI_CODE_HITS:
            break
    raw = await runtime.review(
        AI_SYSTEM_PROMPT,
        issue_review_prompt(title, body, issues, code_hits),
        AI_VERDICT_SCHEMA,
    )
    if raw is None:
        return {
            "verdict": "unclear", "duplicateOf": 0,
            "reason": "The Workers AI reviewer was unavailable.",
        }, issues, code_hits
    return parse_ai_verdict(raw), issues, code_hits


async def _submit_issue_report(runtime, context, data):
    owner, repo = _repo_names(data)
    title = _one_line(data.get("title"), MAX_TITLE)
    body = _text(data.get("body"), MAX_ISSUE_BODY)
    event = data.get("event")
    if not owner or not repo:
        return _response(runtime, {"error": "invalid_repository"}, status=400)
    if len(title) < 8 or len(body) < 20:
        return _response(runtime, {"error": "report_too_short"}, status=400)
    if not isinstance(event, dict) or str(event.get("type") or "") != "open":
        return _response(runtime, {"error": "signed_open_event_required"},
                         status=400)
    if not await runtime.repository_is_public(owner, repo):
        return _response(runtime, {"error": "repository_not_available"},
                         status=404)
    if await runtime.account_controls_repository(
            context["account"], owner, repo):
        return _response(runtime, {"error": "own_repository"}, status=403)

    subject = "%s/%s issue: %s" % (owner, repo, title)
    subject_bi = await runtime.blind(
        "microjob:issue_report:%s/%s:%s" % (owner, repo, title.lower()))
    verdict, issues, code_hits = await _review_issue(
        runtime, owner, repo, title, body)
    status, should_file, message = verdict_outcome(verdict)

    # A missing/unusable model answer or unavailable comparison set cannot
    # prove this report is new. Fail closed before either the repository inbox
    # or the claims table is touched, and let the reporter retry later.
    if status == "pending_review":
        return _response(
            runtime,
            {"error": "issue_review_inconclusive", "detail": message,
             "review": verdict},
            status=503,
        )

    filed = False
    filed_number = 0
    if should_file:
        filed_result = await runtime.file_issue(owner, repo, {
            "titleIfNew": title,
            "event": event,
            "meta": data.get("meta"),
            "account": context["account"],
        })
        filed = bool(filed_result)
        # Production returns the collision-safe number reserved for the inbox.
        # Boolean test/legacy adapters still mean "accepted" without inventing
        # an issue number that was never actually allocated.
        if filed and not isinstance(filed_result, bool):
            try:
                filed_number = max(0, int(filed_result))
            except (TypeError, ValueError):
                filed_number = 0
        if not filed:
            return _response(
                runtime,
                {"error": "issue_not_accepted",
                 "detail": (
                     "The repository's issue inbox refused the signed event; "
                     "no claim was opened.")},
                status=400,
            )
    return await _record_claim(runtime, context, {
        "job": "issue_report",
        "status": status,
        "subject": subject,
        "subjectBi": subject_bi,
        "detail": message,
        "record": {
            "owner": owner,
            "repo": repo,
            "title": title,
            "number": filed_number,
            "verdict": verdict,
            "comparedIssues": len(issues or []),
            "comparedCodeHits": len(code_hits or []),
            "issueFiled": filed,
        },
        "extra": {
            "issueFiled": filed,
            "issueNumber": filed_number,
            "review": verdict,
        },
    })


async def _submit_merged_pull(runtime, context, data):
    owner, repo = _repo_names(data)
    try:
        number = int(data.get("number") or 0)
    except (TypeError, ValueError):
        number = 0
    if not owner or not repo:
        return _response(runtime, {"error": "invalid_repository"}, status=400)
    if number <= 0 or number > 999_999_999:
        return _response(runtime, {"error": "invalid_pull_number"}, status=400)
    if not await runtime.repository_is_public(owner, repo):
        return _response(runtime, {"error": "repository_not_available"},
                         status=404)
    if await runtime.account_controls_repository(
            context["account"], owner, repo):
        return _response(runtime, {"error": "own_repository"}, status=403)

    text = await runtime.read_pull_record(owner, repo, number)
    if text is None:
        return _response(
            runtime,
            {"error": "pull_unreadable",
             "detail": (
                 "No online host could serve this pull request's record. "
                 "Try again once a mirror is back.")},
            status=503,
        )
    fields = parse_pull_frontmatter(text)
    if not fields:
        return _response(runtime, {"error": "pull_not_found"}, status=404)
    marker = mcp_marker_fields(fields)
    if not pull_is_merged(fields):
        return _response(
            runtime,
            {"error": "pull_not_merged",
             "detail": "Only a merged pull request is payable."},
            status=409,
        )
    if marker["client"] != MCP_CLIENT_MARKER or not marker["clientSig"]:
        return _response(
            runtime,
            {"error": "pull_not_opened_with_mcp",
             "detail": (
                 "This pull request carries no ForkMesh MCP agent-tool "
                 "marker. Only pull requests opened with the MCP tool are "
                 "payable microjobs.")},
            status=409,
        )
    if marker["number"] != number:
        return _response(
            runtime,
            {"error": "mcp_marker_pull_mismatch",
             "detail": (
                 "The MCP marker belongs to a different pull request and "
                 "cannot be reused for this claim.")},
            status=409,
        )
    verified = await runtime.verify_node_signature(
        marker["author"],
        marker["clientSig"],
        mcp_pull_canonical(
            number, marker["base"], marker["head"],
            marker["author"], marker["ts"]),
    )
    if not verified:
        return _response(
            runtime,
            {"error": "mcp_marker_signature_invalid",
             "detail": (
                 "The MCP marker on this pull request is not signed by the "
                 "node that opened it.")},
            status=409,
        )
    if not await runtime.account_owns_node_key(
            context["accountBi"], marker["author"]):
        return _response(
            runtime,
            {"error": "pull_not_yours",
             "detail": (
                 "The MCP marker was signed by a node key this account does "
                 "not own. Claim from the account whose node opened it.")},
            status=403,
        )

    return await _record_claim(runtime, context, {
        "job": "merged_pull",
        "status": "approved",
        "subject": "%s/%s pull #%d" % (owner, repo, number),
        "subjectBi": await runtime.blind(
            "microjob:merged_pull:%s/%s:%d" % (owner, repo, number)),
        "detail": "Merged pull request opened with the MCP agent tool.",
        "record": {
            "owner": owner,
            "repo": repo,
            "number": number,
            "head": marker["head"],
            "base": marker["base"],
        },
    })


async def _submit_referral_post(runtime, context, data):
    url, host = normalize_public_url(data.get("url"))
    if not url:
        return _response(
            runtime,
            {"error": "invalid_public_url",
             "detail": "Give the public https:// page your link is posted on."},
            status=400,
        )
    subject_bi = await runtime.blind("microjob:referral_post:" + url)
    account = context["account"]
    origin = runtime.public_origin()

    page = await runtime.read_public_page(url, MAX_FETCH_BYTES)
    if page is None:
        return _response(
            runtime,
            {"error": "page_unreachable",
             "detail": (
                 "The page could not be fetched. It must be publicly "
                 "readable without a login.")},
            status=400,
        )
    if not post_contains_referral_link(page, origin, account):
        return _response(
            runtime,
            {"error": "referral_link_not_found",
             "detail": (
                 "That page does not contain %s/r/%s. Post the link, then "
                 "claim." % (origin.rstrip("/"), account))},
            status=409,
        )
    site = await runtime.d1_first(
        "SELECT visits FROM site_referrers WHERE host=?", host)
    visits = max(0, int((site or {}).get("visits") or 0))
    stats = await runtime.d1_first(
        "SELECT clicks,signups FROM referral_stats WHERE referrer_bi=?",
        context["accountBi"],
    )
    clicks = max(0, int((stats or {}).get("clicks") or 0))
    if visits < MIN_REFERRAL_SITE_VISITS or clicks < MIN_REFERRAL_CLICKS:
        status = "pending_review"
        detail = (
            "The link is posted. It becomes payable once %s has sent at "
            "least %d counted visits (now %d) and your link has at least %d "
            "clicks (now %d)."
            % (host, MIN_REFERRAL_SITE_VISITS, visits,
               MIN_REFERRAL_CLICKS, clicks)
        )
    else:
        status = "approved"
        detail = (
            "Link found on the page, %d visits from %s and %d clicks on your "
            "link." % (visits, host, clicks)
        )
    return await _record_claim(runtime, context, {
        "job": "referral_post",
        "status": status,
        "subject": url,
        "subjectBi": subject_bi,
        "detail": detail,
        "record": {
            "url": url,
            "host": host,
            "visits": visits,
            "clicks": clicks,
        },
    })


async def _record_claim(runtime, context, claim):
    """Insert one verified claim, or report the duplicate/quota that stopped it."""
    job = claim["job"]
    lamports = JOBS[job]["lamports"]
    now = int(runtime.now())
    quota = await _quota_error(runtime, context["accountBi"], lamports, now)
    if quota:
        return _response(runtime, {"error": quota, "policy": public_policy()},
                         status=429)
    claim_id = str(runtime.new_id())
    sealed = await runtime.seal({
        "account": context["account"],
        "job": job,
        "subject": claim["subject"],
        "detail": claim["detail"],
        "record": claim.get("record") or {},
    })
    try:
        await runtime.d1_run(
            "INSERT INTO microjob_claims "
            "(claim_id,account_bi,job,subject_bi,status,amount_lamports,"
            "data,created_at,updated_at) VALUES (?,?,?,?,?,?,?,?,?)",
            claim_id, context["accountBi"], job, claim["subjectBi"],
            claim["status"], lamports, sealed, now, now,
        )
    except Exception:
        return _response(
            runtime,
            {"error": "already_claimed",
             "detail": "This exact work has already been claimed."},
            status=409,
        )
    await runtime.audit(
        context["account"], "microjob.claim", "microjob_claim", claim_id,
        {"job": job, "status": claim["status"], "lamports": lamports},
    )
    payload = {
        "ok": True,
        "claim": public_claim(
            {"subject": claim["subject"], "detail": claim["detail"]},
            {
                "claim_id": claim_id, "job": job, "status": claim["status"],
                "amount_lamports": lamports, "created_at": now,
                "updated_at": now, "tx_signature": "",
            },
        ),
        "payoutWallet": bool(
            await runtime.payout_wallet(context["accountBi"])),
    }
    payload.update(claim.get("extra") or {})
    return _response(runtime, payload, status=201)


async def refresh_pending_referrals(runtime,
                                    limit=MAX_PENDING_REFERRAL_REVIEWS):
    """Re-check early referral claims as their traffic counters catch up.

    A claimant should not have to delete or duplicate a claim merely because
    the first few visitors had not arrived yet. The hourly payout phase reads
    a small bounded slice, verifies that the link is still live, and promotes
    only claims whose live page and both database counters now pass.
    """
    rows = await runtime.d1_all(
        "SELECT claim_id,account_bi,data FROM microjob_claims "
        "WHERE job='referral_post' AND status='pending_review' "
        "ORDER BY created_at ASC LIMIT ?",
        max(1, min(int(limit or 1), MAX_PENDING_REFERRAL_REVIEWS)),
    )
    approved = 0
    for row in rows or []:
        try:
            envelope = await runtime.open(row.get("data"))
        except Exception:
            continue
        envelope = envelope if isinstance(envelope, dict) else {}
        record = envelope.get("record")
        record = record if isinstance(record, dict) else {}
        url, host = normalize_public_url(record.get("url"))
        account = _one_line(envelope.get("account"), 64).lower()
        if not url or not account:
            continue
        page = await runtime.read_public_page(url, MAX_FETCH_BYTES)
        if page is None or not post_contains_referral_link(
                page, runtime.public_origin(), account):
            continue
        site = await runtime.d1_first(
            "SELECT visits FROM site_referrers WHERE host=?", host)
        stats = await runtime.d1_first(
            "SELECT clicks,signups FROM referral_stats WHERE referrer_bi=?",
            str(row.get("account_bi") or ""),
        )
        visits = max(0, int((site or {}).get("visits") or 0))
        clicks = max(0, int((stats or {}).get("clicks") or 0))
        if visits < MIN_REFERRAL_SITE_VISITS or clicks < MIN_REFERRAL_CLICKS:
            continue
        detail = (
            "Link found on the page, %d visits from %s and %d clicks on your "
            "link." % (visits, host, clicks)
        )
        record.update({"url": url, "host": host, "visits": visits,
                       "clicks": clicks})
        envelope.update({"detail": detail, "record": record})
        sealed = await runtime.seal(envelope)
        now = int(runtime.now())
        await runtime.d1_run(
            "UPDATE microjob_claims SET status='approved',data=?,updated_at=? "
            "WHERE claim_id=? AND status='pending_review'",
            sealed, now, str(row.get("claim_id") or ""),
        )
        await runtime.audit(
            account, "microjob.referral_approved", "microjob_claim",
            str(row.get("claim_id") or ""),
            {"host": host, "visits": visits, "clicks": clicks},
        )
        approved += 1
    return approved


SUBMITTERS = {
    "issue_report": _submit_issue_report,
    "merged_pull": _submit_merged_pull,
    "referral_post": _submit_referral_post,
}


async def handle(runtime, path):
    """Serve ``/api/microjobs`` and ``/api/microjobs/claims``."""
    normalized = str(path or "").rstrip("/") or PREFIX
    if normalized not in (PREFIX, PREFIX + "/" + CLAIMS_PART):
        return _response(runtime, {"error": "not_found"}, status=404)
    method = runtime.method()
    if method not in ("GET", "POST"):
        return _response(runtime, {"error": "method_not_allowed"}, status=405)
    await runtime.ensure_schema()

    if method == "GET":
        account_bi, record = await runtime.session({})
        account = _one_line((record or {}).get("name"), 64).lower()
        payload = {
            "ok": True,
            "jobs": public_catalog(),
            "policy": public_policy(),
            "account": account,
            "claims": await _claims_for(runtime, account_bi)
            if account_bi else [],
            "earnedLamports": await _earned_lamports(runtime, account_bi)
            if account_bi else 0,
            "payoutWallet": await runtime.payout_wallet(account_bi)
            if account_bi else "",
            "referralLink": (
                runtime.public_origin().rstrip("/") + "/r/" + account
                if account else ""
            ),
        }
        payload["earnedSol"] = lamports_to_sol(payload["earnedLamports"])
        return _response(runtime, payload)

    if normalized != PREFIX + "/" + CLAIMS_PART:
        return _response(runtime, {"error": "method_not_allowed"}, status=405)
    if not runtime.same_origin():
        return _response(runtime, {"error": "origin_not_allowed"}, status=403)
    data, error = await runtime.json_body(BODY_MAX_BYTES)
    if error:
        return _response(runtime, {"error": error},
                         status=413 if error == "payload_too_large" else 400)
    account_bi, record = await runtime.session(data)
    account = _one_line((record or {}).get("name"), 64).lower()
    if not account_bi or not account:
        return _response(runtime, {"error": "invalid_session"}, status=401)
    job = _one_line(data.get("job"), 40).lower()
    if job not in SUBMITTERS:
        return _response(runtime, {"error": "unknown_job"}, status=400)
    context = {"accountBi": str(account_bi), "account": account}
    return await SUBMITTERS[job](runtime, context, data)


# --- Payout batching ---------------------------------------------------------

async def queue_payouts(runtime, limit=MAX_TRANSFERS_PER_INTENT):
    """Turn approved claims into one public unsigned payout plan.

    Called from cron and from the owner's desktop refresh. Approved claims
    whose claimant has not supplied a self-custodial payout address are simply
    left approved — no wallet is ever created for anybody, and the funds stay
    where they are until the owner signs.
    """
    await runtime.ensure_schema()
    await refresh_pending_referrals(runtime)
    pool = await runtime.payout_source()
    if not pool.get("address") or not pool.get("network"):
        return {"queued": 0, "reason": "pool_not_configured"}
    rows = await runtime.d1_all(
        "SELECT claim_id,account_bi,job,amount_lamports FROM microjob_claims "
        "WHERE status='approved' ORDER BY created_at ASC LIMIT ?",
        max(1, min(int(limit or 1), MAX_TRANSFERS_PER_INTENT)) * 4,
    )
    claims = []
    wallets = {}
    for row in rows or []:
        account_bi = str(row.get("account_bi") or "")
        if account_bi not in wallets:
            wallets[account_bi] = await runtime.payout_wallet(account_bi)
        wallet = wallets[account_bi]
        if not wallet or wallet == pool.get("address"):
            continue
        claims.append({
            "claimId": str(row.get("claim_id") or ""),
            "wallet": wallet,
            "lamports": int(row.get("amount_lamports") or 0),
        })
    transfers = batch_transfers(claims)
    if not transfers:
        return {"queued": 0, "reason": "no_payable_claims"}
    queued_ids = [
        claim_id
        for transfer in transfers
        for claim_id in transfer.get("claimIds") or ()
        if claim_id
    ]
    now = int(runtime.now())
    intent_id = str(runtime.new_id())
    plan = payout_plan(pool["address"], pool["network"], transfers, now)
    statements = [(
        "INSERT INTO chain_intents "
        "(intent_id,kind,source_address,signer_account,status,data,"
        "created_at,expires_at) VALUES (?,?,?,?,'pending_signature',?,?,?)",
        (
            intent_id, PAYOUT_INTENT_KIND, pool["address"],
            pool.get("signerAccount", ""), runtime.encode_plan(plan),
            now, now + PAYOUT_INTENT_TTL_MS,
        ),
    )]
    statements.extend(
        (
            "UPDATE microjob_claims SET status='queued',intent_id=?,"
            "updated_at=? WHERE claim_id=? AND status='approved'",
            (intent_id, now, claim_id),
        )
        for claim_id in queued_ids
    )
    await runtime.batch(statements)
    await runtime.audit(
        "", "microjob.payout_intent", "chain_intent", intent_id,
        {
            "claimCount": len(queued_ids),
            "transferCount": len(transfers),
            "distributedLamports": plan["distributedLamports"],
        },
    )
    return {
        "queued": len(queued_ids),
        "intentId": intent_id,
        "transfers": len(transfers),
        "lamports": plan["distributedLamports"],
    }
