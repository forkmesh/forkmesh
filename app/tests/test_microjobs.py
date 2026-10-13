"""End-to-end contracts for paid microjobs.

Exercises the pure policy module against a sqlite-backed fake runtime: the
Workers AI duplicate review, the MCP agent-tool pull marker, referral-post
validation, the anti-farming bounds, and the unsigned payout plan. Also pins
the Worker wiring (route, lazy module, cron phase, chain-intent hooks) and the
byte-for-byte MCP canonical string the relay re-verifies.
"""

from __future__ import annotations

import asyncio
import importlib.util
import json
import sqlite3
import sys
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
MIGRATIONS = ROOT / "migrations"
ENTRY = (SRC / "entry.py").read_text(encoding="utf-8")
APP_RUNTIME = (SRC / "app_runtime.py").read_text(encoding="utf-8")
MICROJOBS_RUNTIME = (SRC / "microjobs_runtime.py").read_text(encoding="utf-8")
SCHEMA_TEXT = (SRC / "schema.py").read_text(encoding="utf-8")
MCP_SERVER = (ROOT.parent / "tools" / "forkmesh_mcp_server.py").read_text(
    encoding="utf-8")
PULL_STORE = (ROOT.parent / "desktop" / "src" / "PullStore.cpp").read_text(
    encoding="utf-8")
REWARD_SIGNER = (
    ROOT.parent / "desktop" / "src" / "RewardPoolSigner.cpp"
).read_text(encoding="utf-8")
DOC = ROOT.parent / "www" / "docs" / "operations" / "microjobs.md"

sys.path.insert(0, str(SRC))
import microjobs  # noqa: E402


def _module(name):
    spec = importlib.util.spec_from_file_location(name, SRC / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


urls = _module("urls")
static_routes = _module("static_routes")
dashboard_shell = _module("dashboard_shell")
dashboard_bundle = _module("dashboard_bundle")


def _run(coro):
    return asyncio.run(coro)


class Response:
    def __init__(self, data, status):
        self.data = data
        self.status = status


class Runtime:
    """Everything the Worker adapter supplies, backed by sqlite and stubs."""

    def __init__(self, **overrides):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        # The real migration, verbatim — including its schema_meta fingerprint
        # stamp, so a broken migration fails here rather than on a deploy.
        self.db.executescript(
            "CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT NOT NULL);")
        self.db.executescript(
            (MIGRATIONS / "0131_microjobs.sql").read_text(encoding="utf-8"))
        self.db.executescript(
            """
            CREATE TABLE site_referrers (host TEXT PRIMARY KEY, visits INT);
            CREATE TABLE referral_stats (
                referrer_bi TEXT PRIMARY KEY, clicks INT, signups INT);
            CREATE TABLE chain_intents (
                intent_id TEXT PRIMARY KEY, kind TEXT, source_address TEXT,
                signer_account TEXT, status TEXT, data TEXT,
                created_at INT, expires_at INT);
            """
        )
        self._method = overrides.get("method", "POST")
        self._now = 1_700_000_000_000
        self._ids = 0
        self.account = overrides.get("account", ("acct-bi", "reporter"))
        self.wallets = overrides.get(
            "wallets", {"acct-bi": "3n1Xd9K7bTFVJdWXsZbLuiPGgCwXsBAmyYq5tE2rvqPz"})
        self.pool = overrides.get("pool", {
            "address": "9WzDXwBbmkg8ZTbNMqUxvQRAyrZzDsGYdLVL9zYtAWWM",
            "network": "mainnet-beta",
            "signerAccount": "owner",
        })
        self.ai_answer = overrides.get("ai_answer", {
            "verdict": "new", "duplicateOf": 0, "reason": "Not seen before.",
        })
        self.issue_numbers = overrides.get("issue_numbers", [3, 2, 1])
        self.issue_records = overrides.get("issue_records", [
            {"number": 3, "title": "Sidebar overlaps", "status": "open",
             "body": "the sidebar covers the content"},
        ])
        self.code_hits = overrides.get("code_hits", [])
        self.issue_matches = overrides.get("issue_matches", [])
        self.pull_record = overrides.get("pull_record")
        self.page_text = overrides.get("page_text", "")
        self.signature_ok = overrides.get("signature_ok", True)
        self.owns_key = overrides.get("owns_key", True)
        self.public_repo = overrides.get("public_repo", True)
        self.controls_repo = overrides.get("controls_repo", False)
        self.controls_checks = []
        self.filed = []
        self.file_issue_result = overrides.get("file_issue_result", True)
        self.audits = []
        self.prompts = []

    # --- request plumbing -------------------------------------------------
    def method(self):
        return self._method

    def same_origin(self):
        return True

    def now(self):
        return self._now

    def new_id(self):
        self._ids += 1
        return "%032x" % self._ids

    def public_origin(self):
        return "https://forkmesh.com"

    def response(self, data, status=200, cache_control=None,
                 extra_headers=None):
        return Response(data, status)

    def encode_plan(self, plan):
        return json.dumps(plan, sort_keys=True, separators=(",", ":"))

    async def ensure_schema(self):
        return None

    async def json_body(self, _limit):
        return dict(self.body), ""

    async def session(self, _data=None):
        account_bi, name = self.account
        return account_bi, ({"name": name} if name else None)

    async def blind(self, value):
        return "bi:" + str(value)

    async def seal(self, value):
        return json.dumps(value)

    async def open(self, value):
        return json.loads(value) if value else None

    # --- storage ----------------------------------------------------------
    async def d1_all(self, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]

    async def d1_first(self, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row else None

    async def d1_run(self, sql, *args):
        self.db.execute(sql, args)
        self.db.commit()

    async def batch(self, statements):
        for sql, params in statements:
            self.db.execute(sql, params)
        self.db.commit()

    async def audit(self, actor, action, kind, resource, details):
        self.audits.append((actor, action, kind, resource, details))

    # --- verification surfaces -------------------------------------------
    async def payout_wallet(self, account_bi):
        return self.wallets.get(str(account_bi), "")

    async def payout_source(self):
        return dict(self.pool)

    async def repository_is_public(self, _owner, _repo):
        return self.public_repo

    async def recent_issue_numbers(self, _owner, _repo):
        return self.issue_numbers

    async def load_issue_records(self, _owner, _repo, _numbers):
        return self.issue_records

    async def search_issues(self, _owner, _repo, _term):
        return self.issue_matches

    async def search_code(self, _owner, _repo, _term):
        return self.code_hits

    async def read_pull_record(self, _owner, _repo, _number):
        return self.pull_record

    async def verify_node_signature(self, _key, _sig, canonical):
        self.prompts.append(canonical)
        return self.signature_ok

    async def account_owns_node_key(self, _account_bi, _key):
        return self.owns_key

    async def account_controls_repository(self, account, owner, repo):
        self.controls_checks.append((account, owner, repo))
        return self.controls_repo

    async def review(self, _system, user_prompt, _schema):
        self.prompts.append(user_prompt)
        return self.ai_answer

    async def read_public_page(self, _url, _limit):
        return self.page_text

    async def file_issue(self, owner, repo, submission):
        self.filed.append((owner, repo, submission))
        return self.file_issue_result


SIGNED_EVENT = {"type": "open", "author": "pub", "body": "b", "sig": "s"}


def _claim(runtime, body):
    runtime.body = body
    return _run(microjobs.handle(runtime, "/api/microjobs/claims"))


def _issue_body(**overrides):
    body = {
        "job": "issue_report",
        "owner": "forkmesh",
        "repo": "forkmesh",
        "title": "Release downloads 404 on Windows",
        "body": "Clicking the Windows installer on the releases tab returns "
                "a 404 from the mirror.",
        "event": dict(SIGNED_EVENT),
    }
    body.update(overrides)
    return body


# --- catalog and policy ------------------------------------------------------

def test_every_job_publishes_a_fixed_tiny_amount_and_its_requirement():
    catalog = microjobs.public_catalog()
    assert [job["id"] for job in catalog] == [
        "issue_report", "merged_pull", "referral_post"]
    for job in catalog:
        assert job["title"] and job["summary"] and job["requirement"]
        # Tiny on purpose: no job may pay a whole SOL.
        assert 0 < job["lamports"] < microjobs.LAMPORTS_PER_SOL
        assert job["sol"] == microjobs.lamports_to_sol(job["lamports"])


def test_lamports_render_as_exact_sol_without_float_drift():
    assert microjobs.lamports_to_sol(5_000_000) == "0.005"
    assert microjobs.lamports_to_sol(50_000_000) == "0.05"
    assert microjobs.lamports_to_sol(1_000_000_000) == "1"
    assert microjobs.lamports_to_sol(1) == "0.000000001"
    assert microjobs.lamports_to_sol(0) == "0"


def test_public_policy_states_the_limits_and_refuses_to_promise_returns():
    policy = microjobs.public_policy()
    assert policy["custody"] == "external-local-signer"
    assert policy["mcpClientMarker"] == microjobs.MCP_CLIENT_MARKER
    joined = " ".join(policy["limitations"]).lower()
    assert "not employment" in joined
    assert "investment" in joined
    assert "never holds a payout key" in joined
    assert "not proof of personhood" in joined


def test_the_catalog_is_public_and_claiming_is_not():
    runtime = Runtime(method="GET")
    payload = _run(microjobs.handle(runtime, "/api/microjobs")).data
    assert payload["ok"] is True
    assert len(payload["jobs"]) == 3

    signed_out = Runtime(account=("", ""))
    response = _claim(signed_out, _issue_body())
    assert response.status == 401
    assert response.data["error"] == "invalid_session"


def test_only_the_two_documented_paths_are_served():
    runtime = Runtime(method="GET")
    assert _run(microjobs.handle(runtime, "/api/microjobs/other")).status == 404
    runtime._method = "DELETE"
    assert _run(microjobs.handle(runtime, "/api/microjobs")).status == 405


# --- issue reports -----------------------------------------------------------

def test_a_new_report_is_filed_and_approved():
    runtime = Runtime()
    response = _claim(runtime, _issue_body())
    assert response.status == 201
    assert response.data["claim"]["status"] == "approved"
    assert response.data["issueFiled"] is True
    assert len(runtime.filed) == 1
    # The reviewer saw the existing issues and was asked for a JSON verdict.
    assert "Sidebar overlaps" in runtime.prompts[0]
    assert "EXISTING ISSUES" in runtime.prompts[0]


@pytest.mark.parametrize("verdict,reason", [
    ({"verdict": "duplicate", "duplicateOf": 3, "reason": "Same as #3."},
     "Already reported as #3"),
    ({"verdict": "already_addressed", "duplicateOf": 0,
      "reason": "Fixed in releases.py."},
     "Already addressed by code"),
])
def test_a_duplicate_or_already_fixed_report_files_nothing_and_pays_nothing(
        verdict, reason):
    runtime = Runtime(ai_answer=verdict)
    response = _claim(runtime, _issue_body())
    assert response.status == 201
    assert response.data["claim"]["status"] == "rejected"
    assert reason in response.data["claim"]["detail"]
    assert runtime.filed == []


def test_an_unusable_reviewer_answer_files_and_records_nothing():
    # Only an affirmative `new` verdict may reach either the issue inbox or the
    # claims table. An unavailable model is retryable, never implicit approval.
    for answer in (None, "not json at all", {"verdict": "banana"}, {}):
        runtime = Runtime(ai_answer=answer)
        response = _claim(runtime, _issue_body())
        assert response.status == 503, answer
        assert response.data["error"] == "issue_review_inconclusive", answer
        assert runtime.filed == [], answer
        assert runtime.db.execute(
            "SELECT COUNT(*) FROM microjob_claims").fetchone()[0] == 0


def test_an_offline_repository_files_and_records_nothing_without_guessing():
    runtime = Runtime(issue_numbers=None)
    response = _claim(runtime, _issue_body())
    assert response.status == 503
    assert response.data["error"] == "issue_review_inconclusive"
    assert runtime.prompts == []  # no reviewer call without the comparison set
    assert runtime.filed == []


def test_a_report_the_inbox_refuses_opens_no_claim():
    runtime = Runtime(file_issue_result=False)
    response = _claim(runtime, _issue_body())
    assert response.status == 400
    assert response.data["error"] == "issue_not_accepted"
    assert runtime.db.execute(
        "SELECT COUNT(*) FROM microjob_claims").fetchone()[0] == 0


def test_a_report_needs_a_signed_open_event_and_a_public_repository():
    runtime = Runtime()
    assert _claim(runtime, _issue_body(event=None)).status == 400
    assert _claim(runtime, _issue_body(
        event={"type": "comment"})).status == 400
    assert _claim(runtime, _issue_body(title="short")).status == 400
    assert _claim(runtime, _issue_body(owner="../etc")).status == 400
    private = Runtime(public_repo=False)
    assert _claim(private, _issue_body()).status == 404


def test_the_same_report_cannot_be_claimed_twice():
    runtime = Runtime()
    assert _claim(runtime, _issue_body()).status == 201
    duplicate = _claim(runtime, _issue_body())
    assert duplicate.status == 409
    assert duplicate.data["error"] == "already_claimed"


def test_the_reviewer_prompt_carries_matching_code_so_fixed_work_is_visible():
    runtime = Runtime(code_hits=[
        {"path": "app/src/releases.py", "line": 42, "snippet": "def blob()"},
    ])
    _claim(runtime, _issue_body())
    assert "app/src/releases.py:42" in runtime.prompts[0]


def test_matching_old_issues_are_prioritized_ahead_of_the_recent_page():
    runtime = Runtime(
        issue_numbers=list(range(100, 80, -1)),
        issue_matches=[{"number": 7, "title": "Old matching report"}],
        issue_records=[
            {"number": 7, "title": "Old matching report", "status": "closed",
             "body": "same release downloads failure"},
        ],
    )
    _claim(runtime, _issue_body())
    assert "Old matching report" in runtime.prompts[0]


def test_search_terms_prefer_distinctive_words():
    terms = microjobs.search_terms(
        "Release downloads 404 on Windows", "This will not be used")
    assert "this" not in terms and "will" not in terms
    assert "downloads" in terms


# --- merged pull requests ----------------------------------------------------

MCP_PULL = "\n".join([
    "---", "schema: forkmesh-pull-v1", "number: 128", "title: Fix it",
    "base: main", "head: fix/thing", "status: merged", "derive: branch",
    "ts: 1700000000000", "author: nodepubkey", "authorName: reporter",
    "sig: abc", "client: forkmesh-mcp-v1", "clientSig: marker-signature",
    "---", "", "body",
])


def _pull_body(**overrides):
    body = {"job": "merged_pull", "owner": "forkmesh", "repo": "forkmesh",
            "number": 128}
    body.update(overrides)
    return body


def test_a_merged_mcp_pull_is_approved_against_its_exact_canonical():
    runtime = Runtime(pull_record=MCP_PULL)
    response = _claim(runtime, _pull_body())
    assert response.status == 201
    assert response.data["claim"]["status"] == "approved"
    assert runtime.prompts[0] == microjobs.mcp_pull_canonical(
        128, "main", "fix/thing", "nodepubkey", 1700000000000)


def test_a_pull_without_the_mcp_marker_is_refused_with_the_reason():
    without = MCP_PULL.replace("client: forkmesh-mcp-v1\n", "").replace(
        "clientSig: marker-signature\n", "")
    runtime = Runtime(pull_record=without)
    response = _claim(runtime, _pull_body())
    assert response.status == 409
    assert response.data["error"] == "pull_not_opened_with_mcp"
    assert "MCP" in response.data["detail"]


def test_a_forged_marker_field_without_a_valid_signature_is_refused():
    runtime = Runtime(pull_record=MCP_PULL, signature_ok=False)
    response = _claim(runtime, _pull_body())
    assert response.status == 409
    assert response.data["error"] == "mcp_marker_signature_invalid"


def test_a_signed_marker_cannot_be_copied_to_a_different_pull():
    copied = MCP_PULL.replace("number: 128", "number: 127")
    runtime = Runtime(pull_record=copied)
    response = _claim(runtime, _pull_body())
    assert response.status == 409
    assert response.data["error"] == "mcp_marker_pull_mismatch"
    assert runtime.prompts == []


def test_somebody_elses_mcp_pull_cannot_be_claimed():
    runtime = Runtime(pull_record=MCP_PULL, owns_key=False)
    response = _claim(runtime, _pull_body())
    assert response.status == 403
    assert response.data["error"] == "pull_not_yours"


def test_a_claim_on_the_claimants_own_repository_is_refused():
    for body, record in ((_pull_body(), MCP_PULL), (_issue_body(), None)):
        runtime = Runtime(pull_record=record, controls_repo=True)
        response = _claim(runtime, body)
        assert response.status == 403
        assert response.data["error"] == "own_repository"
        assert runtime.controls_checks == [("reporter", "forkmesh", "forkmesh")]
        assert runtime.filed == []
        assert runtime.db.execute(
            "SELECT COUNT(*) FROM microjob_claims").fetchone()[0] == 0


def test_an_open_pull_is_not_yet_payable():
    runtime = Runtime(pull_record=MCP_PULL.replace(
        "status: merged", "status: open"))
    response = _claim(runtime, _pull_body())
    assert response.status == 409
    assert response.data["error"] == "pull_not_merged"


def test_an_unreadable_pull_is_a_retry_not_a_rejection():
    runtime = Runtime(pull_record=None)
    assert _claim(runtime, _pull_body()).status == 503
    missing = Runtime(pull_record="no frontmatter here")
    assert _claim(missing, _pull_body()).status == 404


def test_pull_frontmatter_parsing_ignores_the_body_and_unknown_keys():
    fields = microjobs.parse_pull_frontmatter(MCP_PULL)
    assert fields["head"] == "fix/thing"
    assert fields["client"] == microjobs.MCP_CLIENT_MARKER
    assert "body" not in fields
    assert microjobs.parse_pull_frontmatter("no header") == {}
    assert microjobs.pull_is_merged({"status": "merged"}) is True
    assert microjobs.pull_is_merged({"status": "open"}) is False


# --- referral posts ----------------------------------------------------------

PAGE = '<a href="https://forkmesh.com/r/reporter">try ForkMesh</a>'


def _referral_body(url="https://example.com/post"):
    return {"job": "referral_post", "url": url}


def _seed_referral(runtime, visits, clicks, host="example.com"):
    runtime.db.execute(
        "INSERT INTO site_referrers (host,visits) VALUES (?,?)", (host, visits))
    runtime.db.execute(
        "INSERT INTO referral_stats (referrer_bi,clicks,signups) "
        "VALUES (?,?,?)", ("acct-bi", clicks, 1))
    runtime.db.commit()


def test_a_posted_link_with_real_traffic_is_approved():
    runtime = Runtime(page_text=PAGE)
    _seed_referral(runtime, 25, 9)
    response = _claim(runtime, _referral_body())
    assert response.status == 201
    assert response.data["claim"]["status"] == "approved"
    assert "25 visits" in response.data["claim"]["detail"]


def test_a_posted_link_nobody_clicked_waits_and_says_what_is_missing():
    runtime = Runtime(page_text=PAGE)
    _seed_referral(runtime, 1, 0)
    response = _claim(runtime, _referral_body())
    assert response.data["claim"]["status"] == "pending_review"
    detail = response.data["claim"]["detail"]
    assert "now 1" in detail and "now 0" in detail


def test_a_pending_referral_is_rechecked_and_queued_after_traffic_arrives():
    runtime = Runtime(page_text=PAGE)
    _seed_referral(runtime, 1, 0)
    response = _claim(runtime, _referral_body())
    claim_id = response.data["claim"]["id"]
    runtime.db.execute(
        "UPDATE site_referrers SET visits=25 WHERE host='example.com'")
    runtime.db.execute(
        "UPDATE referral_stats SET clicks=9 WHERE referrer_bi='acct-bi'")
    runtime.db.commit()
    result = _run(microjobs.queue_payouts(runtime))
    assert result["queued"] == 1
    row = runtime.db.execute(
        "SELECT status,intent_id,data FROM microjob_claims WHERE claim_id=?",
        (claim_id,),
    ).fetchone()
    assert row["status"] == "queued" and row["intent_id"]
    assert "25 visits" in json.loads(row["data"])["detail"]


def test_a_page_without_the_link_is_refused():
    runtime = Runtime(page_text="<p>nothing here</p>")
    _seed_referral(runtime, 99, 99)
    response = _claim(runtime, _referral_body())
    assert response.status == 409
    assert response.data["error"] == "referral_link_not_found"


def test_an_unreachable_page_is_refused():
    runtime = Runtime(page_text=None)
    response = _claim(runtime, _referral_body())
    assert response.status == 400
    assert response.data["error"] == "page_unreachable"


def test_the_link_is_found_in_any_spelling_a_post_might_use():
    for page in (
        'href="https://forkmesh.com/r/reporter"',
        "visit HTTP://NEXT.FORKMESH.COM/R/REPORTER today",
        'src="//forkmesh.com/r/reporter"',
        "forkmesh.com/r/reporter",
    ):
        assert microjobs.post_contains_referral_link(
            page, "https://forkmesh.com", "reporter"), page
    assert not microjobs.post_contains_referral_link(
        'href="https://forkmesh.com/r/someoneelse"',
        "https://forkmesh.com", "reporter")


@pytest.mark.parametrize("url", [
    "http://example.com/post",            # not https
    "https://user:pw@example.com/post",   # embedded credentials
    "https://127.0.0.1/post",             # bare IP literal
    "https://localhost/post",
    "https://box.internal/post",
    "https://example.com:8080/post",      # non-443 port
    "ftp://example.com/post",
    "",
])
def test_only_ordinary_public_pages_may_be_fetched(url):
    assert microjobs.normalize_public_url(url) == ("", "")
    runtime = Runtime(page_text=PAGE)
    assert _claim(runtime, _referral_body(url)).status == 400


def test_a_submitted_url_is_normalized_and_its_secrets_redacted():
    url, host = microjobs.normalize_public_url(
        "https://Example.COM./post?utm=x&token=shhh#frag")
    assert host == "example.com"
    assert url.startswith("https://example.com/post?")
    assert "utm=x" in url and "shhh" not in url and "frag" not in url


def test_the_same_page_cannot_be_claimed_twice():
    runtime = Runtime(page_text=PAGE)
    _seed_referral(runtime, 25, 9)
    assert _claim(runtime, _referral_body()).status == 201
    assert _claim(runtime, _referral_body()).status == 409


# --- anti-farming ------------------------------------------------------------

def test_open_claims_daily_claims_and_daily_amount_are_all_bounded():
    runtime = Runtime(page_text=PAGE)
    _seed_referral(runtime, 25, 9)
    accepted = 0
    for index in range(microjobs.MAX_CLAIMS_PER_ACCOUNT_PER_DAY + 3):
        response = _claim(
            runtime, _referral_body("https://example.com/post-%d" % index))
        if response.status == 201:
            accepted += 1
            continue
        assert response.status == 429
        assert response.data["error"] in (
            "open_claim_limit", "daily_claim_limit", "daily_amount_limit")
        # The refusal repeats the policy so the page can explain itself.
        assert response.data["policy"]["maxClaimsPerDay"] == (
            microjobs.MAX_CLAIMS_PER_ACCOUNT_PER_DAY)
        break
    assert accepted <= microjobs.MAX_CLAIMS_PER_ACCOUNT_PER_DAY
    assert accepted * microjobs.JOBS["referral_post"]["lamports"] <= (
        microjobs.MAX_DAILY_LAMPORTS_PER_ACCOUNT)


def test_yesterdays_claims_do_not_count_against_todays_limit():
    runtime = Runtime(page_text=PAGE)
    _seed_referral(runtime, 25, 9)
    _claim(runtime, _referral_body("https://example.com/old"))
    runtime.db.execute(
        "UPDATE microjob_claims SET created_at=?, status='paid'",
        (runtime._now - 2 * microjobs.DAY_MS,))
    runtime.db.commit()
    assert _claim(runtime, _referral_body("https://example.com/new")).status == 201


# --- payouts -----------------------------------------------------------------

def _approved(runtime, count, account_bi="acct-bi", job="referral_post"):
    for index in range(count):
        runtime.db.execute(
            "INSERT INTO microjob_claims (claim_id,account_bi,job,subject_bi,"
            "status,amount_lamports,data,created_at,updated_at) "
            "VALUES (?,?,?,?,'approved',?,?,?,?)",
            ("c%s%d" % (account_bi, index), account_bi, job,
             "s%s%d" % (account_bi, index),
             microjobs.JOBS[job]["lamports"], "{}", index, index),
        )
    runtime.db.commit()


def test_approved_claims_become_one_public_unsigned_plan():
    runtime = Runtime()
    _approved(runtime, 2)
    result = _run(microjobs.queue_payouts(runtime))
    assert result["queued"] == 2
    row = runtime.db.execute("SELECT * FROM chain_intents").fetchone()
    plan = json.loads(row["data"])
    assert row["kind"] == microjobs.PAYOUT_INTENT_KIND
    assert row["status"] == "pending_signature"
    assert row["source_address"] == runtime.pool["address"]
    # Custody: unsigned, funds untouched, and the desktop must sign locally.
    assert plan["custody"]["forkMeshHoldsUserKeys"] is False
    assert plan["custody"]["fundsRemainInSourceWalletUntilSigned"] is True
    assert plan["signing"] == {
        "required": True, "performed": False, "method": "external-local-qt"}
    assert plan["policy"] == microjobs.PAYOUT_POLICY
    # Two claims by one person collapse into one transfer: a legacy Solana
    # message may not repeat a destination.
    assert len(plan["transfers"]) == 1
    assert plan["transfers"][0]["lamports"] == 2 * (
        microjobs.JOBS["referral_post"]["lamports"])
    assert plan["distributedLamports"] == plan["transfers"][0]["lamports"]
    assert [r["status"] for r in runtime.db.execute(
        "SELECT status FROM microjob_claims")] == ["queued", "queued"]


def test_payout_batch_never_exceeds_the_desktop_signers_local_cap():
    wallet = "3n1Xd9K7bTFVJdWXsZbLuiPGgCwXsBAmyYq5tE2rvqPz"
    claims = [{"claimId": "c%d" % index, "wallet": wallet,
               "lamports": 50_000_000} for index in range(20)]
    transfers = microjobs.batch_transfers(claims)
    assert sum(item["lamports"] for item in transfers) == 500_000_000
    assert len(transfers[0]["claimIds"]) == 10


def test_every_claim_a_plan_pays_for_is_marked_queued():
    """No claim may be paid for by a plan and still left approved.

    Claim ids are 32 hex characters, so a wallet with five approved claims
    overflows the truncated per-transfer label. Recovering the ids from that
    label used to drop the claims past the cut: the plan paid their lamports
    while they stayed approved, and the next hourly batch paid them again.
    """
    runtime = Runtime()
    claim_ids = ["%032x" % (index + 1) for index in range(6)]
    for index, claim_id in enumerate(claim_ids):
        runtime.db.execute(
            "INSERT INTO microjob_claims (claim_id,account_bi,job,subject_bi,"
            "status,amount_lamports,data,created_at,updated_at) "
            "VALUES (?,?,?,?,'approved',?,?,?,?)",
            (claim_id, "acct-bi", "referral_post", "subject-%d" % index,
             microjobs.JOBS["referral_post"]["lamports"], "{}", index, index),
        )
    runtime.db.commit()

    result = _run(microjobs.queue_payouts(runtime))

    assert result["queued"] == len(claim_ids)
    assert [r["status"] for r in runtime.db.execute(
        "SELECT status FROM microjob_claims")] == ["queued"] * len(claim_ids)
    plan = json.loads(
        runtime.db.execute("SELECT data FROM chain_intents").fetchone()["data"])
    assert plan["distributedLamports"] == len(claim_ids) * (
        microjobs.JOBS["referral_post"]["lamports"])
    # The private bookkeeping never reaches the plan the desktop signs.
    assert all("claimIds" not in transfer for transfer in plan["transfers"])
    # A second batch has nothing left to pay, so nobody is paid twice.
    assert _run(microjobs.queue_payouts(runtime))["queued"] == 0


def test_a_claimant_without_a_payout_address_stays_approved_and_unpaid():
    runtime = Runtime(wallets={})
    _approved(runtime, 1)
    assert _run(microjobs.queue_payouts(runtime))["queued"] == 0
    assert runtime.db.execute(
        "SELECT status FROM microjob_claims").fetchone()[0] == "approved"


def test_no_plan_is_authored_without_a_configured_pool():
    runtime = Runtime(pool={})
    _approved(runtime, 1)
    assert _run(microjobs.queue_payouts(runtime)) == {
        "queued": 0, "reason": "pool_not_configured"}
    assert runtime.db.execute(
        "SELECT COUNT(*) FROM chain_intents").fetchone()[0] == 0


def test_a_payout_never_sends_the_pool_its_own_money():
    runtime = Runtime(wallets={"acct-bi": None})
    runtime.wallets["acct-bi"] = runtime.pool["address"]
    _approved(runtime, 1)
    assert _run(microjobs.queue_payouts(runtime))["queued"] == 0


def test_one_plan_never_exceeds_the_legacy_transfer_limit():
    runtime = Runtime()
    for index in range(microjobs.MAX_TRANSFERS_PER_INTENT + 4):
        runtime.wallets["a%d" % index] = "wallet%d" % index
    transfers = microjobs.batch_transfers([
        {"claimId": "c%d" % index, "wallet": "3n1Xd9K7bTFVJdWXsZbLuiPGgCwXsBAmyYq5tE2rvqP%s"
         % "zyxwvutsrqponml"[index], "lamports": 1000}
        for index in range(microjobs.MAX_TRANSFERS_PER_INTENT + 4)
    ])
    assert len(transfers) == microjobs.MAX_TRANSFERS_PER_INTENT
    assert len({t["address"] for t in transfers}) == len(transfers)


def test_batching_drops_unusable_wallets_and_zero_amounts():
    assert microjobs.batch_transfers([
        {"claimId": "a", "wallet": "not-a-solana-address", "lamports": 10},
        {"claimId": "b", "wallet": "3n1Xd9K7bTFVJdWXsZbLuiPGgCwXsBAmyYq5tE2rvqPz",
         "lamports": 0},
    ]) == []


# --- Worker wiring -----------------------------------------------------------

def test_the_route_matches_only_the_two_documented_paths():
    for path in ("/api/microjobs", "/api/microjobs/", "/api/microjobs/claims",
                 "/api/microjobs/claims/"):
        assert urls.MICROJOBS_RE.match(path), path
    for path in ("/api/microjobsx", "/api/microjobs/claims/1",
                 "/api/microjob"):
        assert not urls.MICROJOBS_RE.match(path), path


def test_the_worker_lazily_loads_the_module_and_routes_to_it():
    assert 'microjobs = _LazyModule("microjobs")' in ENTRY
    assert '"MICROJOBS_RE"' in ENTRY
    assert "if MICROJOBS_RE.match(url.path):" in APP_RUNTIME
    assert "return await microjobs_handler(self.env, request, url.path)" in APP_RUNTIME
    assert 'microjobs_runtime = _LazyModule("microjobs_runtime")' in ENTRY
    assert "class _MicrojobsRuntime:" in MICROJOBS_RUNTIME


def test_the_payout_cron_phase_is_scheduled_and_dispatched():
    assert 'if hourly == 7:\n        phases.append("microjob-payouts")' in ENTRY
    assert 'if phase == "microjob-payouts":' in ENTRY
    assert "return await queue_microjob_payouts(env)" in ENTRY


def test_a_finalized_transfer_pays_the_claims_and_a_failure_returns_them():
    assert (
        "UPDATE microjob_claims SET status='paid', tx_signature=?, "
        '"\n            "updated_at=? WHERE intent_id=? AND status=\'queued\''
    ) in ENTRY or "SET status='paid', tx_signature=?" in ENTRY
    assert "UPDATE microjob_claims SET status='approved',intent_id=''," in ENTRY


def test_claims_are_stored_encrypted_and_deduplicated_by_subject():
    assert "CREATE TABLE IF NOT EXISTS microjob_claims" in SCHEMA_TEXT
    assert (
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_microjob_claims_subject "
    ) in SCHEMA_TEXT
    # The verdict and the readable subject never sit in plaintext columns.
    assert "verdict" not in SCHEMA_TEXT.split(
        "microjob_claims")[1].split(")\"\"\"")[0]


def test_claims_are_outside_the_platform_admin_table_editor():
    admin = (SRC / "admin_console.py").read_text(encoding="utf-8")
    assert '"microjob_claims",' in admin


# --- the MCP marker contract -------------------------------------------------

def test_the_mcp_tool_and_the_worker_agree_on_the_canonical_string():
    assert 'MCP_CLIENT_MARKER = "forkmesh-mcp-v1"' in MCP_SERVER
    assert '"forkmesh-mcp-pull-v1", str(int(number)), base, head, author' in (
        MCP_SERVER)
    assert 'f"client: {MCP_CLIENT_MARKER}"' in MCP_SERVER
    assert 'f"clientSig: {client_sig}"' in MCP_SERVER
    assert microjobs.mcp_pull_canonical(7, "main", "topic", "pub", 5) == (
        "forkmesh-mcp-pull-v1\n7\nmain\ntopic\npub\n5")


def test_the_desktop_carries_the_marker_through_a_merge_rewrite():
    # A merge rewrites pull.md. Dropping these two fields there would silently
    # make an MCP-opened pull unpayable long after the work was done.
    assert 'lines << "client: " + pr.client;' in PULL_STORE
    assert 'lines << "clientSig: " + pr.clientSig;' in PULL_STORE
    assert 'out.client = fm.get("client");' in PULL_STORE
    assert 'pr.clientSig = fm.get("clientSig");' in PULL_STORE
    assert '{"client", client}, {"clientSig", clientSig}' in PULL_STORE


def test_the_desktop_signer_accepts_the_payout_kind_under_its_own_cap():
    assert 'QLatin1String("microjob_payout")' in REWARD_SIGNER
    assert 'QLatin1String("microjob-payout-v1")' in REWARD_SIGNER


# --- documentation -----------------------------------------------------------

def test_the_operations_doc_states_the_custody_and_verification_rules():
    text = DOC.read_text(encoding="utf-8")
    for phrase in (
        "never holds a key",
        "microjob-payout-v1",
        "forkmesh-mcp-v1",
        "pending_review",
        "site_referrers",
        "not proof of personhood",
    ):
        assert phrase in text, phrase


def test_the_page_is_routed_bundled_and_linked():
    assert dashboard_shell.PAGES["microjobs"]["route"] == "/microjobs"
    assert static_routes.DASHBOARD_PAGE_ASSETS["/microjobs"] == (
        "microjobs/index.html")
    assert "microjobs" in static_routes.RESERVED_ROUTE_PREFIXES
    assert "/microjobs/index.html" in static_routes.BLOCKED_STATIC_HTML_PATHS
    assert "09-microjobs.js" in dashboard_bundle.FRAGMENTS
    sidebar = (ROOT / "public" / "dashboard" / "partials" / "sidebar.html"
               ).read_text(encoding="utf-8")
    assert 'data-nav="microjobs"' in sidebar and 'href="/microjobs"' in sidebar
    www_worker = (ROOT.parent / "www" / "worker.js").read_text(
        encoding="utf-8")
    www_wrangler = (ROOT.parent / "www" / "wrangler.toml").read_text(
        encoding="utf-8")
    www_redirects = (ROOT.parent / "www" / "public" / "_redirects").read_text(
        encoding="utf-8")
    assert '"/microjobs"' in www_worker
    assert '\n  "/microjobs",' not in www_wrangler
    assert "/microjobs https://app.forkmesh.com/microjobs 308" in www_redirects


def test_issue_and_pull_composers_use_the_normal_reward_submission_flow():
    fragment = (ROOT / "public" / "dashboard" / "js" / "09-microjobs.js"
                ).read_text(encoding="utf-8")
    compose = (ROOT / "public" / "dashboard" / "js"
               / "07-repo-compose-branch.js").read_text(encoding="utf-8")
    assert "microjobPullNoticeHtml()" in compose
    assert "microjobIssueNoticeHtml()" in compose
    assert "No second claim is needed" in fragment
    assert "Submit it here normally" in fragment
    assert "Open the pull request normally" in fragment
    assert "/api/microjobs/claims" not in fragment
