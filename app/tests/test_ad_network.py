"""ForkMesh Ads: policy, review state machine, allowances, rotation, wiring.

The fake runtime runs the module's real SQL against an in-memory SQLite built
from the shipped migration, so a column or placeholder mistake fails here
rather than in production.
"""

import asyncio
import json
import sqlite3
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
sys.path.insert(0, str(ROOT / "src"))

import ad_network as ads  # noqa: E402
import schema  # noqa: E402
import urls  # noqa: E402


NOW = 1_800_000_000_000
AD = {
    "headline": "Self-host your CI in ten minutes",
    "body": "Open-source runners with reproducible builds.",
    "cta": "Read the docs",
    "sponsor": "Example Forge",
    "url": "https://forge.example.org/docs",
    "category": "developer-tools",
}


def _db():
    db = sqlite3.connect(":memory:")
    db.row_factory = sqlite3.Row
    db.execute("CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT)")
    db.executescript(
        (ROOT / "migrations" / "0187_ad_network.sql").read_text("utf-8"))
    db.execute(
        "CREATE TABLE role_grants (account_bi TEXT, role TEXT, "
        "scope_type TEXT, scope_bi TEXT, granted_by_bi TEXT, "
        "granted_at INTEGER, expires_at INTEGER, revoked_at INTEGER, "
        "PRIMARY KEY (account_bi, role, scope_type, scope_bi))")
    return db


class Runtime:
    """Narrow fake of entry.py's _WorldCommunityRuntime over real SQLite."""

    ids = 0

    def __init__(self, db, method="GET", body=None, user="alice",
                 params=None, admins=("root",), now=NOW):
        self.db = db
        self._method = method
        self._body = body if body is not None else {}
        self._user = user
        self._params = params or {}
        self._admins = admins
        self._now = now
        self.audits = []

    def method(self):
        return self._method

    def now(self):
        return self._now

    def new_id(self):
        Runtime.ids += 1
        return "%08x-0000-4000-8000-%012x" % (Runtime.ids, Runtime.ids)

    def query_params(self):
        return dict(self._params)

    def response(self, data, status=200, cache_control=None,
                 extra_headers=None):
        return {"data": data, "status": status, "cache": cache_control,
                "headers": extra_headers or {}}

    async def ensure_schema(self):
        return None

    async def json_body(self, _limit):
        return dict(self._body), ""

    async def session(self, _data=None):
        if not self._user:
            return "", None
        return "bi:" + self._user, {"name": self._user}

    async def is_admin(self, name):
        return name in self._admins

    async def seal(self, value):
        return "sealed:" + json.dumps(value)

    async def open(self, value):
        return json.loads(value[7:]) if str(value).startswith("sealed:") else None

    async def audit(self, actor, action, target_type="", target="",
                    outcome="success", details=None):
        self.audits.append((actor, action, target, outcome))

    async def d1_all(self, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]

    async def d1_first(self, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row else None

    async def d1_run(self, sql, *args):
        self.db.execute(sql, args)

    async def batch(self, statements):
        with self.db:
            for sql, args in statements:
                self.db.execute(sql, args)


def call(db, method, path, body=None, **kw):
    runtime = Runtime(db, method, body, **kw)
    result = asyncio.run(ads.handle(runtime, path))
    result["runtime"] = runtime
    return result


def grant(db, user, role, expires_at=0, scope="platform"):
    db.execute(
        "INSERT OR REPLACE INTO role_grants VALUES(?,?,?,'','',0,?,0)",
        ("bi:" + user, role, scope, expires_at))


def submit(db, user="alice", **overrides):
    body = dict(AD, submit=True, attestation=True)
    body.update(overrides)
    result = call(db, "POST", "/api/ads", body, user=user)
    assert result["status"] == 201, result["data"]
    return result["data"]["ad"]


def approve(db, ad, reviewer="root"):
    row = db.execute(
        "SELECT content_hash FROM ad_network_ads WHERE ad_id=?",
        (ad["id"],)).fetchone()
    result = call(
        db, "POST", "/api/ads/" + ad["id"] + "/review",
        {"action": "approve", "contentHash": row["content_hash"]},
        user=reviewer)
    assert result["status"] == 200, result["data"]
    return result


def serve(db, placement="www", now=NOW):
    result = call(db, "GET", "/api/ads/serve", user="",
                  params={"placement": placement}, now=now)
    assert result["status"] == 200
    return result["data"]["ad"]


# --- policy -----------------------------------------------------------------

def test_policy_is_public_cacheable_and_states_the_privacy_contract():
    result = call(_db(), "GET", "/api/ads/policy", user="")
    assert result["status"] == 200
    assert result["cache"] == "public, max-age=3600"
    policy = result["data"]
    assert policy["privacy"] == {
        "contextualOnly": True,
        "behavioralTracking": False,
        "personalProfilesUsed": False,
        "remoteImagesOrScripts": False,
        "impressionsMeasured": False,
        "measurement": "aggregate daily clicks per ad and surface",
        "linkRel": "sponsored noopener noreferrer",
    }
    assert policy["review"]["humanReviewed"] is True
    assert policy["review"]["automatedApproval"] is False
    assert policy["payment"]["processor"] is None
    assert {item["id"] for item in policy["placements"]} == {
        "web", "www", "desktop", "mobile", "world"}
    assert [tier["id"] for tier in policy["tiers"]] == [
        "free", "supporter", "pro", "builder", "studio"]
    prohibited = {item["id"] for item in policy["prohibited"]}
    assert {"gambling", "surveillance", "financial-returns", "deceptive",
            "targets-children", "political-persuasion"} <= prohibited
    # Nothing prohibited can be picked as a category.
    assert not prohibited & {item["id"] for item in policy["categories"]}


def test_allowances_follow_the_patreon_ladder_and_the_published_contract():
    import patreon

    assert {tier["role"] for tier in ads.TIERS} == {
        patreon.SUPPORTER_ROLE, patreon.PRO_ROLE, patreon.BUILDER_ROLE,
        patreon.STUDIO_ROLE}
    for tier in ads.TIERS:
        # Each level's role is the top of what Patreon grants for that level,
        # and its price is the pledge that maps to it.
        assert patreon._roles_for_tier(tier["id"])[0] == tier["role"]
        assert patreon._tier_from_amount_cents(
            tier["priceUsdMonthly"] * 100) == tier["id"]
    published = json.loads(
        (REPO / "www" / "public" / "business-model.json").read_text("utf-8")
    )["advertising"]
    assert published["network"] == ads.NETWORK
    assert published["allowances"] == [
        {"level": tier["id"], "liveAds": tier["liveAds"],
         "rotationWeight": tier["weight"]}
        for tier in [ads.FREE_TIER] + list(reversed(ads.TIERS))]
    assert published["paymentProcessor"] is None
    assert published["targeting"] == "product-surface-only"
    assert published["usesPersonalData"] is False
    assert published["humanReviewRequired"] is True
    assert sorted(published["prohibited"]) == sorted(ads.PROHIBITED)
    # Inherited grants resolve to the highest level.
    assert ads.tier_for_roles(
        ["supporting_member", "pro_member", "builder_member"])["id"] == "builder"
    assert ads.tier_for_roles(["mirror_operator"])["id"] == "free"


# --- validation and screening -------------------------------------------------

def test_creative_validation_refuses_unsafe_destinations_and_unknown_fields():
    def error(**overrides):
        body = dict(AD)
        body.update(overrides)
        return ads.normalize_ad(body, NOW)[1]

    assert error() == ""
    assert error(url="http://forge.example.org") == "invalid_url"
    assert error(url="https://127.0.0.1/x") == "invalid_url"
    assert error(url="https://user:pw@forge.example.org") == "invalid_url"
    assert error(url="https://printer.local/x") == "invalid_url"
    assert error(url="https://bit.ly/abc") == "url_shortener_not_allowed"
    assert error(url="https://forge.example.org/?gclid=1") == (
        "tracking_parameter_not_allowed")
    assert error(category="gambling") == "invalid_category"
    assert error(headline="Hi") == "headline_required"
    assert error(sponsor="") == "sponsor_required"
    assert error(placements=["billboard"]) == "invalid_placements"
    assert error(endsAt=NOW - 1) == "invalid_schedule"
    assert error(image="https://forge.example.org/a.png") == (
        "unsupported_ad_field")
    fields, _ = ads.normalize_ad(
        dict(AD, headline="<b>Hello</b>  there friends"), NOW)
    assert fields["headline"] == "b Hello /b there friends"
    assert fields["placements"] == sorted(ads.PLACEMENTS)


def test_screen_flags_prompt_the_reviewer_but_never_decide():
    fields, error = ads.normalize_ad(dict(
        AD, headline="GUARANTEED RETURNS ACT NOW!!!",
        body="Casino bonus, 200% return"), NOW)
    assert error == ""
    assert {"term:financial-returns", "term:gambling", "term:deceptive",
            "style:shouting", "style:punctuation"} <= set(fields["screenFlags"])
    assert ads.screen_flags(AD) == []
    # Whole words only: "scurel" must not match "cure".
    assert ads.screen_flags(dict(AD, body="Secure by default")) == []


# --- submission ----------------------------------------------------------------

def test_any_account_can_submit_but_must_attest_and_sign_in():
    db = _db()
    assert call(db, "POST", "/api/ads", dict(AD), user="")["status"] == 401
    missing = call(db, "POST", "/api/ads", dict(AD, submit=True))
    assert (missing["status"], missing["data"]["error"]) == (
        400, "attestation_required")
    draft = call(db, "POST", "/api/ads", dict(AD))["data"]["ad"]
    assert (draft["status"], draft["delivery"]) == ("draft", "draft")
    # No membership is needed to write or submit; it is needed to run.
    pending = submit(db, user="freeloader")
    assert pending["status"] == "pending"
    stored = db.execute(
        "SELECT owner_sealed,account_bi FROM ad_network_ads WHERE ad_id=?",
        (pending["id"],)).fetchone()
    assert stored["account_bi"] == "bi:freeloader"
    assert "freeloader" in stored["owner_sealed"]
    assert stored["owner_sealed"].startswith("sealed:")


def test_pending_and_total_limits_bound_one_account():
    db = _db()
    for _ in range(ads.MAX_PENDING_PER_ACCOUNT):
        submit(db)
    blocked = call(db, "POST", "/api/ads",
                   dict(AD, submit=True, attestation=True))
    assert (blocked["status"], blocked["data"]["error"]) == (
        409, "pending_limit_reached")
    for _ in range(ads.MAX_ADS_PER_ACCOUNT - ads.MAX_PENDING_PER_ACCOUNT):
        assert call(db, "POST", "/api/ads", dict(AD))["status"] == 201
    full = call(db, "POST", "/api/ads", dict(AD))
    assert (full["status"], full["data"]["error"]) == (409, "ad_limit_reached")
    # Another account is unaffected.
    assert submit(db, user="bob")["status"] == "pending"


# --- review ----------------------------------------------------------------------

def test_only_administrators_review_and_every_decision_is_audited():
    db = _db()
    ad = submit(db)
    denied = call(db, "GET", "/api/ads/review", user="alice")
    assert denied["status"] == 403
    assert denied["runtime"].audits == [
        ("alice", "ad_network.review", "", "denied")]
    assert call(db, "POST", "/api/ads/" + ad["id"] + "/review",
                {"action": "approve"}, user="alice")["status"] == 403
    assert call(db, "GET", "/api/ads/review", user="")["status"] == 401

    queue = call(db, "GET", "/api/ads/review", user="root")["data"]
    assert [item["id"] for item in queue["ads"]] == [ad["id"]]
    assert queue["ads"][0]["owner"] == "alice"
    assert queue["ads"][0]["ownerTier"]["id"] == "free"
    assert queue["counts"] == {"pending": 1}

    result = approve(db, ad)
    assert result["runtime"].audits == [
        ("root", "ad_network.approve", ad["id"], "success")]
    trail = db.execute("SELECT * FROM ad_network_reviews").fetchall()
    assert [(row["action"], row["reviewer_bi"], row["self_review"])
            for row in trail] == [("approve", "bi:root", 0)]
    row = db.execute("SELECT * FROM ad_network_ads").fetchone()
    assert row["status"] == "approved"
    assert row["approved_hash"] == row["content_hash"]
    assert row["approved_until"] == NOW + ads.APPROVAL_TTL_MS


def test_approval_names_the_creative_and_rejection_owes_a_reason():
    db = _db()
    ad = submit(db)
    path = "/api/ads/" + ad["id"] + "/review"
    stale = call(db, "POST", path,
                 {"action": "approve", "contentHash": "0" * 64}, user="root")
    assert (stale["status"], stale["data"]["error"]) == (
        409, "creative_changed")
    bare = call(db, "POST", path, {"action": "reject"}, user="root")
    assert (bare["status"], bare["data"]["error"]) == (400, "reason_required")
    bad = call(db, "POST", path,
               {"action": "reject", "codes": ["not-a-rule"]}, user="root")
    assert bad["data"]["error"] == "invalid_codes"
    done = call(db, "POST", path,
                {"action": "reject", "codes": ["deceptive"],
                 "reason": "Fake urgency in the headline."}, user="root")
    assert done["data"]["status"] == "rejected"
    mine = call(db, "GET", "/api/ads/mine")["data"]["ads"][0]
    assert mine["reviewCodes"] == ["deceptive"]
    assert mine["reviewReason"] == "Fake urgency in the headline."
    # A decided ad cannot be decided again without a new submission.
    again = call(db, "POST", path,
                 {"action": "reject", "reason": "x"}, user="root")
    assert (again["status"], again["data"]["error"]) == (
        409, "invalid_transition")


def test_a_reviewer_deciding_their_own_ad_is_recorded_as_a_self_review():
    db = _db()
    ad = submit(db, user="root")
    assert approve(db, ad)["data"]["selfReview"] is True
    assert db.execute(
        "SELECT self_review FROM ad_network_reviews").fetchone()[0] == 1


def test_editing_an_approved_creative_returns_it_to_review():
    db = _db()
    grant(db, "alice", "supporting_member")
    ad = submit(db)
    approve(db, ad)
    assert serve(db)["id"] == ad["id"]
    path = "/api/ads/" + ad["id"]
    # A schedule change is not a creative change.
    kept = call(db, "PUT", path, {"endsAt": NOW + 5 * ads.DAY_MS})
    assert kept["data"]["ad"]["status"] == "approved"
    assert serve(db)["id"] == ad["id"]
    # New words need the attestation again, then a new review.
    refused = call(db, "PUT", path, {"headline": "Totally different words"})
    assert refused["data"]["error"] == "attestation_required"
    edited = call(db, "PUT", path, {
        "headline": "Totally different words", "attestation": True})
    assert edited["data"]["ad"]["status"] == "pending"
    assert serve(db) is None


def test_suspension_is_a_hold_the_owner_cannot_edit_or_resume_around():
    db = _db()
    grant(db, "alice", "supporting_member")
    ad = submit(db)
    approve(db, ad)
    path = "/api/ads/" + ad["id"]
    held = call(db, "POST", path + "/review", {
        "action": "suspend", "codes": ["tracking-destination"]}, user="root")
    assert held["data"]["status"] == "suspended"
    assert serve(db) is None
    assert call(db, "PUT", path, {"body": "new"})["data"]["error"] == (
        "ad_suspended")
    assert call(db, "POST", path + "/resume")["data"]["error"] == (
        "ad_suspended")
    back = call(db, "POST", path + "/review",
                {"action": "reinstate"}, user="root")
    # Reinstating reopens review; it does not skip it.
    assert back["data"]["status"] == "pending"
    assert serve(db) is None


def test_owner_actions_and_ownership_boundaries():
    db = _db()
    grant(db, "alice", "supporting_member")
    ad = submit(db)
    approve(db, ad)
    path = "/api/ads/" + ad["id"]
    for verb, body in (("PUT", {"body": "x"}), ("DELETE", None)):
        assert call(db, verb, path, body, user="mallory")["status"] == 404
    assert call(db, "POST", path + "/pause", user="mallory")["status"] == 404
    paused = call(db, "POST", path + "/pause")["data"]["ad"]
    assert (paused["paused"], paused["delivery"]) == (True, "paused")
    assert serve(db) is None
    assert call(db, "POST", path + "/resume")["data"]["ad"]["delivery"] == (
        "running")
    gone = call(db, "POST", path + "/withdraw")["data"]["ad"]
    assert gone["status"] == "withdrawn"
    assert serve(db) is None
    # Withdrawn ads re-enter through review, never straight back to running.
    again = call(db, "POST", path + "/submit", {"attestation": True})
    assert again["data"]["ad"]["status"] == "pending"
    assert call(db, "DELETE", path)["data"] == {
        "ok": True, "deleted": ad["id"]}
    assert db.execute("SELECT COUNT(*) FROM ad_network_ads").fetchone()[0] == 0
    # The decision trail is evidence and outlives the creative.
    assert db.execute(
        "SELECT COUNT(*) FROM ad_network_reviews").fetchone()[0] == 1


# --- serving -------------------------------------------------------------------------

def test_serving_needs_approval_membership_schedule_and_a_live_slot():
    db = _db()
    ad = submit(db)
    assert serve(db) is None  # pending
    approve(db, ad)
    assert serve(db) is None  # approved, but the owner has no membership
    mine = call(db, "GET", "/api/ads/mine")["data"]
    assert mine["ads"][0]["delivery"] == "needs-membership"
    assert mine["tier"]["id"] == "free"

    grant(db, "alice", "supporting_member", expires_at=NOW + 1000)
    shown = serve(db)
    assert shown == {
        "id": ad["id"], "label": "Sponsored",
        "headline": AD["headline"], "body": AD["body"], "cta": AD["cta"],
        "sponsor": AD["sponsor"], "url": AD["url"],
        "category": "developer-tools",
        "categoryLabel": "Developer tools and libraries",
    }
    # A lapsed membership pauses the ad by itself; nothing is deleted.
    assert serve(db, now=NOW + 2000) is None
    # An approval is good for APPROVAL_TTL_MS, then needs a fresh look.
    grant(db, "alice", "supporting_member")
    assert serve(db, now=NOW + ads.APPROVAL_TTL_MS - 1) is not None
    assert serve(db, now=NOW + ads.APPROVAL_TTL_MS) is None
    # A live approval has nothing to submit; an expired one is resubmitted.
    path = "/api/ads/" + ad["id"] + "/submit"
    live = call(db, "POST", path, {"attestation": True})
    assert (live["status"], live["data"]["error"]) == (
        409, "invalid_transition")
    later = NOW + ads.APPROVAL_TTL_MS
    expired = call(db, "GET", "/api/ads/mine", now=later)["data"]["ads"][0]
    assert expired["delivery"] == "review-expired"
    again = call(db, "POST", path, {"attestation": True}, now=later)
    assert again["data"]["ad"]["status"] == "pending"


def test_serve_is_viewer_blind_cacheable_and_placement_scoped():
    db = _db()
    grant(db, "alice", "supporting_member")
    ad = submit(db, placements=["desktop"])
    approve(db, ad)
    assert serve(db, "desktop")["id"] == ad["id"]
    assert serve(db, "www") is None
    result = call(db, "GET", "/api/ads/serve", user="",
                  params={"placement": "desktop"})
    assert result["cache"] == "public, max-age=300"
    # The same answer whoever asks: the session is never consulted.
    signed_in = call(db, "GET", "/api/ads/serve", user="alice",
                     params={"placement": "desktop"})
    assert signed_in["data"] == result["data"]
    bad = call(db, "GET", "/api/ads/serve", user="", params={"placement": "x"})
    assert (bad["status"], bad["data"]["error"]) == (400, "invalid_placement")
    assert bad["cache"].startswith("no-store")


def test_level_caps_live_ads_oldest_first_across_surfaces():
    db = _db()
    grant(db, "alice", "supporting_member")  # one live slot
    first = submit(db, headline="First approved ad", placements=["desktop"])
    second = submit(db, headline="Second approved ad", placements=["www"])
    approve(db, first)
    db.execute("UPDATE ad_network_ads SET reviewed_at=? WHERE ad_id=?",
               (NOW - 10, first["id"]))
    approve(db, second)
    # The slot is spent on the desktop ad, so www stays empty.
    assert serve(db, "desktop")["id"] == first["id"]
    assert serve(db, "www") is None
    states = {ad["id"]: ad["delivery"]
              for ad in call(db, "GET", "/api/ads/mine")["data"]["ads"]}
    assert states == {first["id"]: "running", second["id"]: "over-allowance"}
    grant(db, "alice", "pro_member")  # two live slots
    assert serve(db, "www")["id"] == second["id"]


def test_rotation_is_weighted_by_level_and_deterministic_per_bucket():
    def row(ad_id, owner, roles):
        return {
            "ad_id": ad_id, "account_bi": owner, "status": "approved",
            "paused": 0, "content_hash": "h", "approved_hash": "h",
            "approved_until": NOW * 2, "starts_at": 0, "ends_at": 0,
            "placements": "www", "roles": roles}

    rows = [row("studio-ad", "bi:s", "studio_member,supporting_member"),
            row("supporter-ad", "bi:p", "supporting_member")]
    bucket_ms = ads.SERVE_CACHE_SECONDS * 1000
    picks = [ads.select_ad(rows, "www", NOW + n * bucket_ms)["ad_id"]
             for n in range(1100)]
    share = picks.count("studio-ad") / len(picks)
    assert 0.86 < share < 0.96  # 10 : 1
    assert "supporter-ad" in picks
    # Inside one cache bucket every request agrees.
    assert len({ads.select_ad(rows, "www", NOW + n)["ad_id"]
                for n in range(0, bucket_ms - NOW % bucket_ms, 9973)}) == 1
    assert ads.select_ad([], "www", NOW) is None


# --- measurement and reports ------------------------------------------------------------

def test_clicks_are_aggregate_counters_for_approved_ads_only():
    db = _db()
    grant(db, "alice", "supporting_member")
    ad = submit(db)
    path = "/api/ads/" + ad["id"] + "/click"
    call(db, "POST", path, {"placement": "www"}, user="")
    assert db.execute(
        "SELECT COUNT(*) FROM ad_network_daily_stats").fetchone()[0] == 0
    approve(db, ad)
    for _ in range(3):
        assert call(db, "POST", path, {"placement": "www"},
                    user="")["status"] == 200
    call(db, "POST", path, {"placement": "desktop"}, user="")
    assert call(db, "POST", path, {"placement": "tv"},
                user="")["data"]["error"] == "invalid_placement"
    columns = [row[1] for row in db.execute(
        "PRAGMA table_info(ad_network_daily_stats)")]
    # No viewer, address, agent or session column can exist here.
    assert columns == ["ad_id", "day", "placement", "clicks"]
    mine = call(db, "GET", "/api/ads/mine")["data"]["ads"][0]
    assert mine["clicks"] == {
        "windowDays": 30, "total": 4,
        "byPlacement": {"www": 3, "desktop": 1}}
    asyncio.run(ads.cleanup(Runtime(
        db, now=NOW + (ads.STATS_RETENTION_DAYS + 2) * ads.DAY_MS)))
    assert db.execute(
        "SELECT COUNT(*) FROM ad_network_daily_stats").fetchone()[0] == 0


def test_member_reports_pull_a_running_ad_back_into_review():
    db = _db()
    grant(db, "alice", "supporting_member")
    ad = submit(db)
    approve(db, ad)
    path = "/api/ads/" + ad["id"] + "/report"
    assert call(db, "POST", path, {"reason": "misleading"},
                user="")["status"] == 401
    assert call(db, "POST", path, {"reason": "misleading"},
                user="alice")["data"]["error"] == "cannot_report_own_ad"
    assert call(db, "POST", path, {"reason": "spite"},
                user="bob")["data"]["error"] == "invalid_reason"
    for n in range(ads.REPORTS_TO_AUTO_SUSPEND - 1):
        # The same member reporting twice is still one report.
        for _ in range(2):
            result = call(db, "POST", path, {"reason": "misleading"},
                          user="member%d" % n)
        assert result["data"]["suspended"] is False
    assert serve(db)["id"] == ad["id"]
    last = call(db, "POST", path, {"reason": "prohibited"}, user="zed")
    assert last["data"] == {
        "ok": True, "openReports": ads.REPORTS_TO_AUTO_SUSPEND,
        "suspended": True}
    assert serve(db) is None
    reported = call(db, "GET", "/api/ads/review", user="root",
                    params={"status": "reported"})["data"]["ads"]
    assert reported[0]["openReports"] == ads.REPORTS_TO_AUTO_SUSPEND
    assert call(db, "GET", "/api/ads/review", user="root")["data"][
        "counts"] == {"suspended": 1, "reported": 1}
    detail = call(db, "GET", "/api/ads/" + ad["id"] + "/review",
                  user="root")["data"]
    assert detail["history"][0]["action"] == "auto-suspend"
    assert len(detail["reports"]) == ads.REPORTS_TO_AUTO_SUSPEND
    # The reviewer's ruling closes the reports it answers.
    call(db, "POST", "/api/ads/" + ad["id"] + "/review",
         {"action": "reinstate"}, user="root")
    approve(db, ad)
    assert db.execute(
        "SELECT COUNT(*) FROM ad_network_reports WHERE resolved_at=0"
    ).fetchone()[0] == 0
    assert serve(db)["id"] == ad["id"]


# --- routing and wiring --------------------------------------------------------------------

def test_router_rejects_wrong_methods_and_unknown_paths():
    db = _db()
    assert call(db, "POST", "/api/ads/serve")["headers"]["allow"] == "GET"
    assert call(db, "GET", "/api/ads")["status"] == 405
    assert call(db, "GET", "/api/ads/not-an-id")["status"] == 404
    assert call(db, "POST", "/api/ads/" + "a" * 32 + "/explode")["status"] == 404
    assert call(db, "GET", "/api/other")["status"] == 404


def test_edge_cache_keys_cover_only_the_viewer_blind_reads():
    key = ads.edge_cache_key
    assert key("GET", "/api/ads/serve", {"placement": "www"}).endswith(
        "/serve/www")
    assert key("GET", "/api/ads/policy", {}).endswith("/policy")
    assert key("GET", "/api/ads/serve", {"placement": "nope"}) == ""
    assert key("GET", "/api/ads/mine", {}) == ""
    assert key("GET", "/api/ads/review", {}) == ""
    assert key("POST", "/api/ads/serve", {"placement": "www"}) == ""
    purge = ads.edge_cache_purge_keys
    assert len(purge("POST", "/api/ads/" + "a" * 32 + "/review")) == len(
        ads.PLACEMENTS)
    assert purge("POST", "/api/ads/" + "a" * 32 + "/click") == []
    assert purge("GET", "/api/ads/mine") == []


def test_worker_route_schema_account_lifecycle_and_footprint_are_wired():
    entry = (ROOT / "src" / "entry.py").read_text("utf-8")
    app_runtime = (ROOT / "src" / "app_runtime.py").read_text("utf-8")
    footprint = (
        REPO / "world" / "tools" / "build_worker_footprint.py"
    ).read_text("utf-8")
    statements = "\n".join(schema.SCHEMA_STATEMENTS)
    for table in ("ad_network_ads", "ad_network_reviews",
                  "ad_network_reports", "ad_network_daily_stats"):
        assert "CREATE TABLE IF NOT EXISTS " + table in statements
    assert urls.AD_NETWORK_RE.match("/api/ads")
    assert urls.AD_NETWORK_RE.match("/api/ads/serve")
    assert urls.AD_NETWORK_RE.match("/api/ads/" + "a" * 32 + "/review")
    assert not urls.AD_NETWORK_RE.match("/api/adsense")
    assert not urls.AD_NETWORK_RE.match("/api/ads/../accounts")
    assert 'ad_network = _LazyModule("ad_network")' in entry
    assert '"AD_NETWORK_RE",' in entry
    assert "async def ad_network_handler" in entry
    assert "AD_NETWORK_RE.match" in app_runtime
    assert "ad_network_handler" in app_runtime
    assert '"ad_network.py",' in footprint
    # Account rename moves the ads (and re-seals the owner name); account
    # deletion removes them with their counters and reports.
    assert "UPDATE ad_network_ads SET account_bi=?,owner_sealed=?" in entry
    assert "UPDATE ad_network_reports SET account_bi=?" in entry
    assert '"ad_network_ads",' in entry
    assert '"ad_network_reports",' in entry
    assert '("ad_network_daily_stats", "ad_network_reports")' in entry
    assert "await ad_network.cleanup(runtime)" in entry


def test_migration_matches_the_runtime_schema_and_is_reversible():
    db = _db()
    runtime_db = sqlite3.connect(":memory:")
    for statement in schema.SCHEMA_STATEMENTS:
        if "ad_network_" in statement:
            runtime_db.execute(statement)

    def shape(conn):
        return sorted(
            (row[0], row[1], " ".join(str(row[2]).split()))
            for row in conn.execute(
                "SELECT type,name,sql FROM sqlite_master "
                "WHERE name LIKE '%ad_network%' AND sql IS NOT NULL"))

    assert shape(db) == shape(runtime_db)
    db.executescript(
        (ROOT / "migrations" / "down" / "0187_ad_network.sql").read_text(
            "utf-8"))
    assert shape(db) == []
