import asyncio
import base64
import json
from pathlib import Path
import sqlite3
import sys
from urllib.parse import parse_qs, urlparse

import pytest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
import organization_rbac  # noqa: E402
import pull_rules  # noqa: E402


class ContextUnavailable(Exception):
    pass


class Response:
    def __init__(self, status, value):
        self.status = status
        self._text = json.dumps(value)

    async def text(self):
        return self._text


class Request:
    def __init__(self, url="https://forkmesh.com/api", method="GET", body=None):
        self.url = url
        self.method = method
        self.body = body


class Harness:
    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.executescript(
            "CREATE TABLE orgs (org_bi TEXT PRIMARY KEY, name TEXT);"
            "CREATE TABLE org_repos (org_bi TEXT, repo TEXT, node_owner TEXT);"
            "CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT NOT NULL);")
        self.db.executescript(
            (ROOT / "migrations" / "0241_org_pull_rules.sql").read_text())
        self.db.execute("INSERT INTO orgs VALUES ('bi-acme','acme')")
        self.db.execute(
            "INSERT INTO org_repos VALUES ('bi-acme','proj','core-a')")
        self.db.commit()
        self.roles = {"olive": "owner", "max": "member"}
        self.actor = "olive"
        self.audits = []
        self.reads = []
        self.report = {"available": True, "passed": True, "blocked": False,
                       "rules": []}
        self.read_status = 200
        pull_rules.__dict__.update({
            "d1_all": self.d1_all, "d1_first": self.d1_first,
            "d1_run": self.d1_run, "ensure_schema": self.noop,
            "_org_row": self.org_row, "_org_role": self.org_role,
            "_org_rbac": organization_rbac,
            "_account_session_record": self.session,
            "_audit_sensitive_action": self.audit,
            "json_response": self.json_response,
            "method_name": lambda request: request.method,
            "bounded_json_request": self.body,
            "Date": type("Date", (), {"now": staticmethod(lambda: 1234)}),
            "_routed_repository_read": self.read,
            "_https_mirror_public_context": self.context,
            "_HttpsMirrorContextUnavailable": ContextUnavailable,
        })

    async def noop(self, *_args):
        return None

    async def d1_all(self, _env, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]

    async def d1_first(self, _env, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row else None

    async def d1_run(self, _env, sql, *args):
        self.db.execute(sql, args)
        self.db.commit()

    async def org_row(self, _env, org):
        row = self.db.execute(
            "SELECT org_bi,name FROM orgs WHERE name=?", (org,)).fetchone()
        return (row["org_bi"], dict(row)) if row else ("", None)

    async def org_role(self, _env, _org_bi, account):
        return self.roles.get(account, "")

    async def session(self, _env, _request, _data):
        return ("bi", {"name": self.actor}) if self.actor else ("", None)

    async def audit(self, _env, actor, action, _kind, target, outcome, meta):
        self.audits.append((actor, action, target, outcome, meta))

    @staticmethod
    def json_response(value, status=200, cache_control=None,
                      extra_headers=None):
        return {"status": status, "body": value}

    @staticmethod
    async def body(request):
        return request.body

    async def read(self, _env, url, operation, bypass_cache=False):
        self.reads.append((url, operation, bypass_cache))
        return Response(self.read_status, {"ok": True, "rules": self.report})

    async def context(self, _env, owner, repo):
        return {"owner": owner, "repo": repo}

    def run(self, coroutine):
        return asyncio.run(coroutine)

    def org(self, method="GET", body=None):
        return self.run(pull_rules.org_pull_rules_handler(
            None, Request(method=method, body=body), "acme"))


def policy(*rules):
    return {"schema": "forkmesh-pull-rules-v1", "rules": list(rules)}


def test_policy_normalization_keeps_only_meaningful_fields():
    normalized = pull_rules.normalize_policy(policy(
        {"id": "tests", "kind": "tests", "title": "", "branches": [],
         "maxFiles": 0, "enforcement": ""},
        {"id": "opus", "kind": "model", "models": ["claude-opus-5-5"],
         "enforcement": "warn", "branches": ["main", "release/*"]},
    ))
    assert normalized["rules"] == [
        {"id": "tests", "kind": "tests"},
        {"id": "opus", "kind": "model", "models": ["claude-opus-5-5"],
         "enforcement": "warn", "branches": ["main", "release/*"]},
    ]


@pytest.mark.parametrize("bad", [
    {"schema": "other", "rules": []},
    {"schema": "forkmesh-pull-rules-v1", "rules": [], "extra": 1},
    policy({"id": "Bad", "kind": "tests"}),
    policy({"id": "a", "kind": "tests"}, {"id": "a", "kind": "docs"}),
    policy({"id": "a", "kind": "vibes"}),
    policy({"id": "a", "kind": "tests", "colour": "red"}),
    policy({"id": "a", "kind": "size"}),
    policy({"id": "a", "kind": "size", "maxFiles": True}),
    policy({"id": "a", "kind": "paths"}),
    policy({"id": "a", "kind": "model", "models": []}),
    policy({"id": "a", "kind": "commits", "subjectPattern": "(?=feat)"}),
    policy({"id": "a", "kind": "secrets", "patterns": ["("]}),
    policy({"id": "a", "kind": "tests", "branches": ["../x y"]}),
    policy(*[{"id": "r%d" % i, "kind": "tests"} for i in range(33)]),
    policy(*[{"id": "r%d" % i, "kind": "paths", "paths": ["x" * 190]}
             for i in range(32)]),
])
def test_policy_normalization_rejects(bad):
    with pytest.raises(pull_rules.RulesError):
        pull_rules.normalize_policy(bad)


def test_combined_org_policies_keep_rule_ids_unique():
    combined = pull_rules.combine_policies([
        ("acme", policy({"id": "tests", "kind": "tests"})),
        ("beta", policy({"id": "tests", "kind": "docs"},
                        {"id": "tests-2", "kind": "size", "maxFiles": 3})),
    ])
    assert [rule["id"] for rule in combined["rules"]] == [
        "tests", "tests-2", "tests-2-2"]
    assert pull_rules.combine_policies([]) is None


def test_org_rules_are_read_by_members_and_written_by_admins():
    harness = Harness()
    harness.actor = "max"
    read = harness.org()
    assert read["status"] == 200 and read["body"]["canManage"] is False
    assert read["body"]["policy"]["rules"] == []
    denied = harness.org("POST", {"policy": policy(
        {"id": "tests", "kind": "tests"})})
    assert denied["status"] == 403
    assert harness.audits[-1][3] == "denied"

    harness.actor = "olive"
    invalid = harness.org("POST", {"policy": policy({"id": "a", "kind": "size"})})
    assert invalid["status"] == 400
    assert "file or line limit" in invalid["body"]["message"]
    saved = harness.org("POST", {"policy": policy(
        {"id": "tests", "kind": "tests", "branches": ["main"]})})
    assert saved["status"] == 200
    assert harness.audits[-1][1:4] == (
        "organization.pull_rules_change", "acme", "success")
    stored = harness.db.execute("SELECT data,updated_by FROM org_pull_rules").fetchone()
    assert json.loads(stored["data"])["rules"][0]["branches"] == ["main"]
    assert stored["updated_by"] == "olive"
    assert harness.org()["body"]["policy"]["rules"][0]["id"] == "tests"

    cleared = harness.org("POST", {"policy": policy()})
    assert cleared["status"] == 200
    assert harness.db.execute("SELECT COUNT(*) FROM org_pull_rules").fetchone()[0] == 0

    harness.actor = "stranger"
    assert harness.org()["status"] == 403
    harness.actor = ""
    assert harness.org()["status"] == 401


CONTEXT = {"owner": "core-a", "repo": "proj"}
BASE = "a" * 40
HEAD = "b" * 40


def test_merge_gate_needs_no_node_call_without_org_rules():
    harness = Harness()
    assert harness.run(pull_rules.merge_gate(None, CONTEXT, 7, BASE, HEAD)) is None
    assert harness.reads == []


def test_merge_gate_checks_org_rules_on_the_pinned_commits():
    harness = Harness()
    harness.org("POST", {"policy": policy({"id": "tests", "kind": "tests"})})
    harness.report = {"available": True, "passed": False, "blocked": True,
                      "rules": [{"id": "tests", "status": "failed"}]}
    blocked = harness.run(pull_rules.merge_gate(None, CONTEXT, 7, BASE, HEAD))
    assert blocked["error"] == "rules_required"
    assert blocked["rules"]["blocked"] is True
    url, operation, bypass = harness.reads[-1]
    assert operation == "merge-preview" and bypass is True
    parsed = urlparse(url)
    assert parsed.path == "/api/repo/core-a/proj/merge-preview"
    query = {key: values[0] for key, values in parse_qs(parsed.query).items()}
    org_rules = query.pop("orgRules")
    assert query == {"base": BASE, "head": HEAD, "rules": "1", "pull": "7"}
    decoded = base64.urlsafe_b64decode(org_rules + "=" * (-len(org_rules) % 4))
    assert json.loads(decoded) == policy({"id": "tests", "kind": "tests"})

    harness.report = {"available": True, "passed": False, "blocked": False,
                      "rules": [{"id": "tests", "status": "failed",
                                 "enforcement": "warn"}]}
    assert harness.run(pull_rules.merge_gate(None, CONTEXT, 7, BASE, HEAD)) is None

    harness.report = {"available": False, "rules": []}
    unavailable = harness.run(pull_rules.merge_gate(None, CONTEXT, 7, BASE, HEAD))
    assert unavailable["error"] == "rules_required"
    harness.read_status = 503
    assert harness.run(pull_rules.merge_gate(
        None, CONTEXT, 7, BASE, HEAD))["error"] == "rules_required"


def test_pull_page_endpoint_reports_rules_and_their_organizations():
    harness = Harness()
    harness.org("POST", {"policy": policy({"id": "tests", "kind": "tests"})})
    request = Request(
        "https://forkmesh.com/api/repo/core-a/proj/pulls/7/rules?base=%s&head=feature" % BASE)
    result = harness.run(pull_rules.repo_pull_rules_handler(
        None, request, "core-a", "proj", 7))
    assert result["status"] == 200
    assert result["body"]["organizations"] == ["acme"]
    assert result["body"]["rules"]["available"] is True
    bad = Request("https://forkmesh.com/api/repo/core-a/proj/pulls/7/rules?base=..&head=x")
    assert harness.run(pull_rules.repo_pull_rules_handler(
        None, bad, "core-a", "proj", 7))["status"] == 400


def test_merge_handler_gates_on_org_rules_before_creating_a_job():
    source = (ROOT / "src" / "route_handlers.py").read_text()
    handler = source[source.index("async def repository_pull_merge_handler"):]
    gate = handler.index("_pull_rules.merge_gate(")
    assert handler.index("if not authorized:") < gate
    assert gate < handler.index("_repo_merge_jobs_make_room(")
    assert '"rules_required"' in handler[gate:gate + 600]
