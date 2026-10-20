import asyncio
import base64
import hashlib
import json
import re
import sqlite3
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

import advisory_cvss  # noqa: E402
import schema  # noqa: E402
import security_advisories as sa  # noqa: E402
import urls  # noqa: E402

ed25519 = pytest.importorskip(
    "cryptography.hazmat.primitives.asymmetric.ed25519")
serialization = pytest.importorskip(
    "cryptography.hazmat.primitives.serialization")

ORIGIN = "https://code.example.org"
OSV_REFERENCE_TYPES = {
    "ADVISORY", "ARTICLE", "DETECTION", "DISCUSSION", "REPORT", "FIX",
    "INTRODUCED", "GIT", "PACKAGE", "EVIDENCE", "WEB"}
OSV_CREDIT_TYPES = {
    "FINDER", "REPORTER", "ANALYST", "COORDINATOR", "REMEDIATION_DEVELOPER",
    "REMEDIATION_REVIEWER", "REMEDIATION_VERIFIER", "TOOL", "SPONSOR",
    "OTHER"}
RFC3339 = re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$")


def b64url(raw):
    return base64.urlsafe_b64encode(raw).rstrip(b"=").decode()


class FakeD1:
    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        for statement in schema.SCHEMA_STATEMENTS:
            if "security_advis" in statement:
                self.db.execute(statement)

    async def all(self, env, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]

    async def first(self, env, sql, *args):
        rows = await self.all(env, sql, *args)
        return rows[0] if rows else None

    async def run(self, env, sql, *args):
        self.db.execute(sql, args)


class Key:
    def __init__(self):
        self.private = ed25519.Ed25519PrivateKey.generate()
        self.pub = b64url(self.private.public_key().public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw))

    def sign(self, message):
        return b64url(self.private.sign(message))


@pytest.fixture
def world(monkeypatch):
    d1 = FakeD1()
    state = SimpleNamespace(
        d1=d1, notes=[], emails=[], commits=[], fail_commit=False,
        repos={("acme", "widget"): "public", ("acme", "secret"): "private"},
        owners={"acme": "alice"},
        org={("acme", "widget", "mia"): "maintain",
             ("acme", "widget", "wes"): "write"},
        accounts={"alice", "bob", "carol", "dave", "mia", "wes"},
        verified={"alice", "bob", "carol", "dave", "mia", "wes"},
        env=SimpleNamespace(SECURITY_EMAIL="security@example.org"),
    )

    async def blind_index(env, value):
        return hashlib.sha256(str(value).strip().lower().encode()).hexdigest()

    async def encrypt_row(env, obj):
        return "enc:" + base64.b64encode(
            json.dumps(obj).encode()[::-1]).decode()

    async def decrypt_row(env, text):
        if not str(text or "").startswith("enc:"):
            return None
        return json.loads(base64.b64decode(text[4:])[::-1])

    def json_response(data, status=200, cache_control=None, **_):
        return SimpleNamespace(status=status, data=data, cache=cache_control)

    def response(body, status=200, headers=None):
        return SimpleNamespace(status=status, body=body, headers=headers or {})

    async def session_record(env, request, data=None, **_):
        actor = getattr(request, "actor", "")
        if not actor:
            return "", None
        return "bi", {"name": actor,
                      "email_verified": actor in state.verified}

    async def access(env, request, owner, repo, viewer=""):
        visibility = state.repos.get((owner, repo))
        if not visibility:
            return None
        if visibility == "private" and viewer != state.owners.get(owner):
            return None
        return {"repoBi": await blind_index(env, owner + "/" + repo),
                "visibility": visibility, "record": {}, "viewer": viewer}

    async def public_owner(env, request, owner, repo, pattern):
        return owner

    async def owns(env, actor, node):
        return state.owners.get(node) == actor

    async def org_permission(env, node, repo, actor):
        return state.org.get((node, repo, actor), "")

    def rank(permission):
        order = ("read", "write", "maintain", "admin")
        return order.index(permission) if permission in order else -1

    async def fanout(env, host, repo):
        return [state.owners[host]] + sorted(
            name for (node, name_repo, name), perm in state.org.items()
            if node == host and name_repo == repo and rank(perm) >= 2)

    async def notify(env, recipient, kind, title, **kwargs):
        state.notes.append((recipient, kind, title))
        return True

    async def send_email(env, to, subject, text, **kwargs):
        state.emails.append((to, subject, text))
        return True

    async def account_row(env, name):
        return ("bi", {"name": name}) if name in state.accounts else (
            "bi", None)

    async def verify(pub, sig, message):
        try:
            raw = base64.urlsafe_b64decode(pub + "=" * (-len(pub) % 4))
            signature = base64.urlsafe_b64decode(sig + "=" * (-len(sig) % 4))
            ed25519.Ed25519PublicKey.from_public_bytes(raw).verify(
                signature, message)
            return True
        except Exception:
            return False

    def signing_keys(request, owner, repo):
        return (owner.lower() + "/" + repo.lower(),)

    async def bind_author(env, request, data, record, session=None):
        record["authorName"] = session
        return None

    async def body(request, max_bytes=0):
        return request.body

    async def nothing(env):
        return None

    async def ledger_commit(env, request, ctx, path, content, message):
        if state.fail_commit:
            raise RuntimeError("push_unavailable")
        state.commits.append((path, json.loads(content), message))
        return "c" * 40

    async def repo_is_private(env, owner, repo):
        return state.repos.get((owner, repo)) != "public"

    runtime = {
        "_repo_is_private": repo_is_private,
        "d1_all": d1.all, "d1_first": d1.first, "d1_run": d1.run,
        "blind_index": blind_index, "encrypt_row": encrypt_row,
        "decrypt_row": decrypt_row, "json_response": json_response,
        "Response": response, "_account_session_record": session_record,
        "_repository_access_context": access,
        "_public_repo_owner": public_owner, "_account_owns_node": owns,
        "_org_linked_repo_permission": org_permission,
        "team_permission_rank": rank, "_org_fanout_recipients": fanout,
        "enqueue_notification": notify, "_send_email": send_email,
        "_account_row": account_row, "ed25519_verify": verify,
        "_event_signing_repo_keys": signing_keys,
        "_bind_session_author": bind_author, "bounded_json_request": body,
        "ensure_schema": nothing, "method_name": lambda request: request.method,
        "_public_base_url": lambda env, request=None: ORIGIN,
        "REPO_API_PREFIX_RE": urls.REPO_API_PREFIX_RE,
        "instance_features": SimpleNamespace(display_name=lambda env: "Code"),
    }
    for name, value in runtime.items():
        monkeypatch.setattr(sa, name, value, raising=False)
    monkeypatch.setattr(sa, "ledger_commit", ledger_commit)
    sa._hall_cache["value"] = None
    return state


def api(world, method, path, actor="", body=None, repo="widget"):
    url = "https://code.example.org/api/repo/acme/%s/security%s" % (repo, path)
    request = SimpleNamespace(method=method, url=url, actor=actor,
                              body=body or {})
    return asyncio.run(sa.repo_route(world.env, request, "acme", repo))


def public(world, method, path, actor="", body=None):
    request = SimpleNamespace(method=method, url=ORIGIN + path, actor=actor,
                              body=body or {})
    return asyncio.run(sa.public_route(world.env, request, path))


REPORT = {
    "title": "SQL injection in search",
    "description": "The `q` parameter reaches the query unescaped.",
    "severity": {"vector": "CVSS:3.1/AV:N/AC:L/PR:N/UI:N/S:U/C:H/I:H/A:H"},
    "cwes": ["CWE-89"],
    "affected": [{"ecosystem": "PyPI", "package": "widget",
                  "vulnerable": ">= 1.0, < 1.4.2", "patched": "1.4.2"}],
    "references": ["https://example.org/writeup"],
}


def report(world, actor="bob", **overrides):
    response = api(world, "POST", "/advisories", actor, dict(REPORT, **overrides))
    assert response.status == 201, response.data
    return response.data["id"]


def publish(world, advisory_id, key, actor="alice", tamper=False, kind="publish",
            fields=None):
    prepared = api(world, "POST", "/advisories/" + advisory_id, actor,
                   {"action": "prepare", "kind": kind, "fields": fields})
    assert prepared.status == 200, prepared.data
    data = prepared.data
    ts = data["preparedAt"]
    digest = data["contentHash"]
    message = sa.signing_message(data["repoKey"], kind, advisory_id, key.pub,
                                 ts, digest)
    event = {"author": key.pub, "ts": ts, "contentHash": digest,
             "sig": key.sign(message)}
    if tamper:
        event["sig"] = key.sign(message + b"x")
    return api(world, "POST", "/advisories/" + advisory_id, actor, {
        "action": "sign", "kind": kind, "preparedAt": ts, "event": event,
        "fields": fields, "revision": data["revision"]})


def test_report_is_private_triage_and_notifies_maintainers(world):
    advisory_id = report(world)
    assert sa.valid_id(advisory_id) and advisory_id.startswith("FMSA-")
    shown = api(world, "GET", "/advisories/" + advisory_id, "bob")
    assert shown.status == 200
    assert shown.data["advisory"]["state"] == "triage"
    assert shown.data["role"] == "reporter"
    assert shown.data["advisory"]["severity"]["score"] == 9.8
    assert shown.data["advisory"]["severity"]["level"] == "critical"
    recipients = {name for name, kind, _ in world.notes}
    assert recipients == {"alice", "mia"}
    assert {kind for _, kind, _ in world.notes} == {"security_advisory"}
    assert world.emails and world.emails[0][0] == "security@example.org"
    assert advisory_id in world.emails[0][2]
    assert REPORT["description"] not in world.emails[0][2]
    assert REPORT["title"] not in world.emails[0][2]
    row = world.d1.db.execute(
        "SELECT data, public_data FROM security_advisories").fetchone()
    assert "SQL injection" not in row["data"] and row["public_data"] == ""
    for table in ("security_advisory_members", "security_advisory_credits"):
        raw = world.d1.db.execute("SELECT * FROM %s" % table).fetchone()
        assert "bob" not in json.dumps(dict(raw))


def test_drafts_answer_one_uniform_404(world):
    advisory_id = report(world)
    missing = "FMSA-2222-3333-4444"
    expected = api(world, "GET", "/advisories/" + missing, "dave")
    assert expected.status == 404
    probes = [
        api(world, "GET", "/advisories/" + advisory_id),
        api(world, "GET", "/advisories/" + advisory_id, "dave"),
        api(world, "GET", "/advisories/" + advisory_id, "wes"),
        api(world, "GET", "/advisories/" + advisory_id + "/osv.json", "dave"),
        api(world, "POST", "/advisories/" + advisory_id, "dave",
            {"action": "comment", "body": "hi"}),
        api(world, "POST", "/advisories/" + advisory_id, "dave",
            {"action": "prepare", "kind": "publish"}),
        api(world, "GET", "/advisories/" + advisory_id, "alice", repo="secret"),
        api(world, "GET", "", "dave", repo="nothing"),
        api(world, "GET", "", "dave", repo="secret"),
        public(world, "GET", "/advisories/%s.json" % advisory_id),
        public(world, "GET", "/advisories/" + advisory_id),
    ]
    for probe in probes:
        assert probe.status == 404
        assert probe.data == expected.data == sa.NOT_FOUND


def test_reporters_see_only_their_own_reports(world):
    bobs = report(world, "bob")
    carols = report(world, "carol", title="Path traversal")
    bob_view = api(world, "GET", "", "bob").data
    assert [item["id"] for item in bob_view["private"]] == [bobs]
    assert api(world, "GET", "/advisories/" + carols, "bob").status == 404
    assert [item["id"] for item in api(world, "GET", "", "carol").data[
        "private"]] == [carols]
    assert api(world, "GET", "", "dave").data["private"] == []
    assert api(world, "GET", "").data["private"] == []
    maintainer = api(world, "GET", "", "mia").data
    assert {item["id"] for item in maintainer["private"]} == {bobs, carols}
    assert maintainer["maintainer"] is True


def test_non_maintainers_cannot_publish_or_triage(world):
    key = Key()
    advisory_id = report(world)
    api(world, "POST", "/advisories/" + advisory_id, "alice",
        {"action": "collaborator", "name": "dave"})
    for actor in ("bob", "dave", "wes"):
        for body in ({"action": "prepare", "kind": "publish"},
                     {"action": "state", "state": "draft"},
                     {"action": "credit", "user": actor, "type": "finder"},
                     {"action": "collaborator", "name": "carol"}):
            response = api(world, "POST", "/advisories/" + advisory_id,
                           actor, body)
            assert response.status in (403, 404), (actor, body)
    def forged_sign(actor):
        ts = int(sa._now())
        message = sa.signing_message("acme/widget", "publish", advisory_id,
                                     key.pub, ts, "0" * 64)
        return api(world, "POST", "/advisories/" + advisory_id, actor, {
            "action": "sign", "kind": "publish", "preparedAt": ts,
            "event": {"author": key.pub, "ts": ts, "contentHash": "0" * 64,
                      "sig": key.sign(message)}})

    assert forged_sign("bob").status == 403
    assert api(world, "POST", "/advisories/" + advisory_id, "mia",
               {"action": "state", "state": "draft"}).data["state"] == "draft"
    assert forged_sign("dave").status == 403
    assert forged_sign("wes").status == 404
    assert world.commits == []
    row = world.d1.db.execute(
        "SELECT state, public_data FROM security_advisories").fetchone()
    assert row["state"] == "draft" and row["public_data"] == ""


def test_collaborators_join_the_private_thread(world):
    advisory_id = report(world)
    assert api(world, "POST", "/advisories/" + advisory_id, "alice",
               {"action": "collaborator", "name": "dave"}).status == 200
    assert api(world, "POST", "/advisories/" + advisory_id, "dave",
               {"action": "comment", "body": "Patch in @bob's branch"}
               ).status == 201
    thread = api(world, "GET", "/advisories/" + advisory_id, "bob").data
    assert [c["author"] for c in thread["comments"]] == ["dave"]
    assert "Patch" not in json.dumps([
        dict(row) for row in world.d1.db.execute(
            "SELECT data FROM security_advisory_comments")])
    api(world, "POST", "/advisories/" + advisory_id, "alice",
        {"action": "collaborator", "name": "dave", "remove": True})
    assert api(world, "GET", "/advisories/" + advisory_id, "dave").status == 404


def test_publish_records_a_signed_ledger_entry_and_valid_osv(world):
    key = Key()
    advisory_id = report(world)
    api(world, "POST", "/advisories/" + advisory_id, "alice",
        {"action": "state", "state": "draft"})
    api(world, "POST", "/advisories/" + advisory_id, "alice", {
        "action": "edit", "fields": {"cve": "CVE-2026-12345",
                                     "fix": {"pull": 42}}})
    assert publish(world, advisory_id, key, tamper=True).status == 401
    response = publish(world, advisory_id, key)
    assert response.status == 200, response.data
    assert response.data["state"] == "published"
    assert response.data["ledger"]["state"] == "recorded"
    path, document, _ = world.commits[-1]
    assert path == ".forkmesh/advisories/%s.json" % advisory_id
    assert document["format"] == "forkmesh-advisory-v1"
    event = document["events"][-1]
    assert event["authorName"] == "alice" and event["type"] == "publish"
    assert sa.content_hash(event["osv"]) == event["contentHash"]
    message = sa.signing_message(event["repoKey"], "publish", advisory_id,
                                 event["author"], event["ts"],
                                 event["contentHash"])
    assert message.startswith(b"forkmesh-advisory-event-v2\nacme/widget\n")
    ed25519.Ed25519PublicKey.from_public_bytes(base64.urlsafe_b64decode(
        event["author"] + "=")).verify(
        base64.urlsafe_b64decode(event["sig"] + "=="), message)
    anonymous = api(world, "GET", "/advisories/" + advisory_id)
    assert anonymous.status == 200 and anonymous.data["comments"] == []
    assert anonymous.data["ledger"] == {
        "state": "recorded", "path": path, "commit": "c" * 40}
    assert "history" not in anonymous.data["advisory"]
    osv = api(world, "GET", "/advisories/%s/osv.json" % advisory_id).data
    assert_valid_osv(osv)
    assert osv["aliases"] == ["CVE-2026-12345"]
    assert osv["affected"][0]["ranges"][0]["events"] == [
        {"introduced": "1.0"}, {"fixed": "1.4.2"}]
    assert {"type": "FIX", "url": ORIGIN + "/acme/widget/pulls/42"} in \
        osv["references"]
    assert osv == public(world, "GET", "/advisories/%s.json" % advisory_id).data
    assert osv["credits"] == [{"name": "bob", "contact": [ORIGIN + "/bob"],
                               "type": "REPORTER"}]
    feed = public(world, "GET", "/advisories.json").data
    assert [item["id"] for item in feed["advisories"]] == [advisory_id]
    for item in feed["advisories"]:
        assert_valid_osv(item)
    page = public(world, "GET", "/advisories")
    assert advisory_id in page.body and "SQL injection" in page.body
    world.repos[("acme", "widget")] = "private"
    assert public(world, "GET", "/advisories.json").data["advisories"] == []
    assert advisory_id not in public(world, "GET", "/advisories").body
    assert public(world, "GET", "/advisories/%s.json" % advisory_id).status == 404


def test_signed_kinds_refuse_stale_content_and_withdraw_is_recorded(world):
    key = Key()
    advisory_id = report(world, "alice")
    prepared = api(world, "POST", "/advisories/" + advisory_id, "alice",
                   {"action": "prepare", "kind": "publish"}).data
    api(world, "POST", "/advisories/" + advisory_id, "alice",
        {"action": "credit", "user": "carol", "type": "analyst"})
    message = sa.signing_message("acme/widget", "publish", advisory_id,
                                 key.pub, prepared["preparedAt"],
                                 prepared["contentHash"])
    stale = api(world, "POST", "/advisories/" + advisory_id, "alice", {
        "action": "sign", "kind": "publish",
        "preparedAt": prepared["preparedAt"], "revision": prepared["revision"],
        "event": {"author": key.pub, "ts": prepared["preparedAt"],
                  "contentHash": prepared["contentHash"],
                  "sig": key.sign(message)}})
    assert stale.status == 409
    assert publish(world, advisory_id, key).status == 200
    edit = api(world, "POST", "/advisories/" + advisory_id, "alice",
               {"action": "edit", "fields": {"title": "Changed"}})
    assert edit.status == 409 and edit.data["error"] == "signature_required"
    updated = publish(world, advisory_id, key, kind="update",
                      fields={"title": "SQL injection in search API"})
    assert updated.status == 200, updated.data
    withdrawn = publish(world, advisory_id, key, kind="withdraw")
    assert withdrawn.data["state"] == "withdrawn"
    events = world.commits[-1][1]["events"]
    assert [event["type"] for event in events] == [
        "publish", "update", "withdraw"]
    osv = public(world, "GET", "/advisories/%s.json" % advisory_id).data
    assert_valid_osv(osv)
    assert osv["summary"] == "SQL injection in search API"
    assert RFC3339.match(osv["withdrawn"])


def test_ledger_failure_keeps_publish_and_can_retry(world):
    key = Key()
    advisory_id = report(world, "alice", affected=[])
    world.fail_commit = True
    response = publish(world, advisory_id, key)
    assert response.data["state"] == "published"
    assert response.data["ledger"]["state"] == "pending"
    assert response.data["warnings"] == ["no_patched_version"]
    world.fail_commit = False
    retried = api(world, "POST", "/advisories/" + advisory_id, "alice",
                  {"action": "ledger-retry"})
    assert retried.data["ledger"]["state"] == "recorded"
    assert retried.data["ledger"]["commit"] == "c" * 40


def test_credits_need_the_researchers_acceptance(world):
    key = Key()
    advisory_id = report(world)
    api(world, "POST", "/advisories/" + advisory_id, "alice",
        {"action": "state", "state": "draft"})
    api(world, "POST", "/advisories/" + advisory_id, "alice",
        {"action": "credit", "user": "carol", "type": "finder"})
    api(world, "POST", "/advisories/" + advisory_id, "alice",
        {"action": "credit", "user": "dave", "type": "remediation_developer"})
    early = public(world, "POST", "/api/security-advisories/credits/carol",
                   "carol", {"advisoryId": advisory_id, "decision": "accept"})
    assert early.status == 404
    world.notes.clear()
    assert publish(world, advisory_id, key).status == 200
    assert ("carol", "security_advisory",
            "You were credited on security advisory " + advisory_id) in \
        world.notes
    view = api(world, "GET", "/advisories/" + advisory_id, "carol").data
    assert view["viewerCredit"] == {"user": "carol", "type": "finder",
                                    "state": "pending"}
    assert [c["user"] for c in view["credits"]] == ["bob"]
    forged = public(world, "POST", "/api/security-advisories/credits/carol",
                    "dave", {"advisoryId": advisory_id, "decision": "accept"})
    assert forged.status == 403
    assert public(world, "POST", "/api/security-advisories/credits/carol",
                  "carol", {"advisoryId": advisory_id,
                            "decision": "accept"}).data["state"] == "accepted"
    assert public(world, "POST", "/api/security-advisories/credits/dave",
                  "dave", {"advisoryId": advisory_id,
                           "decision": "decline"}).data["state"] == "declined"
    osv = public(world, "GET", "/advisories/%s.json" % advisory_id).data
    assert {c["name"]: c["type"] for c in osv["credits"]} == {
        "bob": "REPORTER", "carol": "FINDER"}
    profile = public(world, "GET", "/api/security-advisories/credits/carol").data
    assert profile["count"] == 1
    assert profile["credits"][0]["severity"] == "critical"
    assert public(world, "GET", "/api/security-advisories/credits/dave"
                  ).data["count"] == 0
    own = public(world, "GET", "/api/security-advisories/credits/dave",
                 "dave").data
    assert [c["state"] for c in own["credits"]] == ["declined"]
    hall = public(world, "GET", "/api/security-advisories/hall-of-fame").data
    assert [(r["user"], r["points"]) for r in hall["researchers"]] == [
        ("bob", 10), ("carol", 10)]
    page = public(world, "GET", "/security-researchers")
    assert "carol" in page.body and "dave" not in page.body


def test_hall_of_fame_weights_by_severity(world):
    key = Key()
    low = report(world, "alice", severity={"level": "low"})
    high = report(world, "alice", title="Second",
                  severity={"vector": "CVSS:4.0/AV:N/AC:L/AT:N/PR:L/UI:N/"
                                      "VC:H/VI:L/VA:N/SC:N/SI:N/SA:N"})
    for advisory_id in (low, high):
        api(world, "POST", "/advisories/" + advisory_id, "alice",
            {"action": "credit", "user": "alice", "type": "finder"})
        assert publish(world, advisory_id, key).status == 200
    ranked = asyncio.run(sa.hall_of_fame(world.env))
    assert ranked[0]["user"] == "alice"
    assert ranked[0]["points"] == sa.SEVERITY_POINTS["low"] + \
        sa.SEVERITY_POINTS["high"]


def test_reporting_switch_and_private_repository_default(world):
    assert api(world, "GET", "", "dave").data["canReport"] is True
    assert api(world, "POST", "/settings", "mia",
               {"reporting": False}).status == 403
    assert api(world, "POST", "/settings", "alice",
               {"reporting": False}).data["reportingEnabled"] is False
    refused = api(world, "POST", "/advisories", "dave", REPORT)
    assert refused.status == 403 and refused.data["error"] == "reporting_disabled"
    assert api(world, "POST", "/advisories", "mia", REPORT).status == 201
    world.verified.discard("carol")
    api(world, "POST", "/settings", "alice", {"reporting": True})
    assert api(world, "POST", "/advisories", "carol", REPORT).status == 403
    assert api(world, "POST", "/advisories", "", REPORT).status == 401
    secret = api(world, "GET", "", "alice", repo="secret").data
    assert secret["reportingEnabled"] is False


def test_fields_are_validated(world):
    for field, value in (
            ("severity", {"vector": "CVSS:3.1/AV:N"}),
            ("severity", {"level": "apocalyptic"}),
            ("cwes", ["XSS"]),
            ("cve", "CVE-12"),
            ("references", ["javascript:alert(1)"]),
            ("affected", [{"ecosystem": "Nope", "package": "x"}])):
        response = api(world, "POST", "/advisories", "bob",
                       dict(REPORT, **{field: value}))
        assert response.status == 400, field
    assert api(world, "POST", "/advisories", "bob",
               dict(REPORT, title=" ")).data["error"] == "title_required"


def test_cvss_scores_match_the_reference_calculator():
    cvss = pytest.importorskip("cvss")
    vectors = [
        "CVSS:3.1/AV:N/AC:L/PR:N/UI:N/S:U/C:H/I:H/A:H",
        "CVSS:3.1/AV:N/AC:L/PR:N/UI:R/S:C/C:L/I:L/A:N",
        "CVSS:3.1/AV:P/AC:H/PR:H/UI:R/S:U/C:N/I:N/A:L",
    ]
    for vector in vectors:
        assert advisory_cvss.score_v3(vector) == float(
            cvss.CVSS3(vector).base_score)
    vectors4 = [
        "CVSS:4.0/AV:N/AC:L/AT:N/PR:N/UI:N/VC:H/VI:H/VA:H/SC:N/SI:N/SA:N",
        "CVSS:4.0/AV:L/AC:H/AT:P/PR:H/UI:A/VC:L/VI:N/VA:N/SC:L/SI:L/SA:N",
        "CVSS:4.0/AV:A/AC:L/AT:N/PR:L/UI:P/VC:H/VI:L/VA:H/SC:H/SI:H/SA:H"
        "/E:P/CR:M/MSI:S",
    ]
    for vector in vectors4:
        assert advisory_cvss.score_v4(vector) == float(
            cvss.CVSS4(vector).base_score)


def test_cvss_known_scores_and_rejects_malformed_vectors():
    assert advisory_cvss.score(
        "CVSS:3.1/AV:N/AC:L/PR:N/UI:N/S:U/C:H/I:H/A:H") == (9.8, "CVSS_V3")
    assert advisory_cvss.score(
        "CVSS:4.0/AV:N/AC:L/AT:N/PR:N/UI:N/VC:H/VI:H/VA:H/SC:N/SI:N/SA:N"
    ) == (9.3, "CVSS_V4")
    for bad in ("CVSS:3.1/AV:N/AC:L", "CVSS:3.1/AC:L/AV:N/PR:N/UI:N/S:U/C:H/"
                "I:H/A:H", "CVSS:4.0/AV:N/AC:L/AT:N/PR:N/UI:N/VC:H/VI:H/VA:H/"
                "SC:N/SI:S/SA:N", "CVSS:2.0/AV:N", ""):
        assert advisory_cvss.score(bad)[0] is None
    assert advisory_cvss.level_for_score(6.9) == "moderate"
    base = "CVSS:3.1/AV:N/AC:L/PR:N/UI:N/S:U/C:H/I:H/A:H"
    assert advisory_cvss.score(base + "/E:P/RL:O/RC:C") == (9.8, "CVSS_V3")
    assert advisory_cvss.score(base + "/CR:H/MAV:L") == (9.8, "CVSS_V3")
    for bad in (base + "/E:Q", base + "/XX:H",
                "CVSS:3.1/AV:NA/AC:L/PR:N/UI:N/S:U/C:H/I:H/A:H"):
        assert advisory_cvss.score(bad)[0] is None


def assert_valid_osv(osv):
    allowed = {"schema_version", "id", "modified", "published", "withdrawn",
               "aliases", "related", "summary", "details", "severity",
               "affected", "references", "credits", "database_specific"}
    assert set(osv) <= allowed
    assert re.match(r"^\d+\.\d+\.\d+$", osv["schema_version"])
    assert sa.valid_id(osv["id"])
    for key in ("modified", "published", "withdrawn"):
        if key in osv:
            assert RFC3339.match(osv[key]), key
    assert isinstance(osv["summary"], str) and "\n" not in osv["summary"]
    for item in osv.get("severity", []):
        assert item["type"] in ("CVSS_V3", "CVSS_V4")
        assert advisory_cvss.score(item["score"])[0] is not None
    for affected in osv.get("affected", []):
        if "package" in affected:
            assert affected["package"]["ecosystem"] in sa.OSV_ECOSYSTEMS
            assert affected["package"]["name"]
        for rng in affected.get("ranges", []):
            assert rng["type"] in ("ECOSYSTEM", "SEMVER", "GIT")
            if rng["type"] == "GIT":
                assert rng["repo"].startswith("https://")
            else:
                assert "package" in affected
            events = rng["events"]
            assert events and "introduced" in events[0]
            kinds = {key for event in events for key in event}
            assert kinds <= {"introduced", "fixed", "last_affected", "limit"}
            assert not {"fixed", "last_affected"} <= kinds
            for event in events:
                assert len(event) == 1
    for reference in osv["references"]:
        assert reference["type"] in OSV_REFERENCE_TYPES
        assert reference["url"].startswith("https://")
    for credit in osv.get("credits", []):
        assert credit["type"] in OSV_CREDIT_TYPES and credit["name"]
    json.dumps(osv)


def test_ledger_commit_writes_the_file_through_the_web_edit_path(tmp_path,
                                                                  monkeypatch):
    import repository_file_edits as edits
    import test_repository_file_edits as fe

    work = tmp_path / "work"
    work.mkdir()
    fe.git(work, "init", "-q", "-b", "main")
    (work / "README.md").write_text("# Widget\n")
    fe.git(work, "add", "-A")
    fe.git(work, "commit", "-q", "-m", "seed")
    bare = tmp_path / "repo.git"
    fe.git(tmp_path, "clone", "-q", "--bare", str(work), str(bare))
    calls = fe.harness(bare)
    original = edits._read

    async def read(env, owner, repo, action):
        if action == "branches":
            return {"defaultBranch": "main"}
        return await original(env, owner, repo, action)

    monkeypatch.setattr(edits, "_read", read)
    ctx = {"owner": "alice", "repo": "project", "actor": "alice"}
    path = sa.ledger_path("FMSA-2222-3333-4444")
    for content in ('{"events": [1]}\n', '{"events": [1, 2]}\n'):
        commit = asyncio.run(sa.ledger_commit(
            SimpleNamespace(), fe.FakeRequest(), ctx, path, content,
            "Security advisory FMSA-2222-3333-4444: publish"))
        assert fe.git_out(bare, "rev-parse", "main").strip() == commit
        assert fe.git_out(bare, "show", "main:" + path) == content
    assert fe.git_out(bare, "show", "main:README.md") == "# Widget\n"
    fe.git(bare, "fsck", "--strict", "--no-dangling")
    assert calls["pushes"][0][2] == "alice"


def test_a_lone_surrogate_is_refused_not_a_server_error(world):
    response = api(world, "POST", "/advisories", "bob",
                   dict(REPORT, description="bad \ud800 text"))
    assert response.status == 400 and response.data["error"] == "invalid_json"
