import asyncio
import importlib
import json
import re
import sqlite3
import sys
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
sys.path.insert(0, str(ROOT / "src"))
sys.path.insert(0, str(REPO / "tools"))

import mirror_gateway  # noqa: E402
import pytest  # noqa: E402


def run(coroutine):
    return asyncio.run(coroutine)


class World:
    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.executescript(
            "CREATE TABLE repo_feature_settings (repo_bi TEXT PRIMARY KEY,"
            " data TEXT, updated_at INTEGER);"
            "CREATE TABLE pull_review_ledger (repo_bi TEXT, pull_number "
            "INTEGER, account TEXT, state TEXT, updated_at INTEGER, "
            "PRIMARY KEY (repo_bi, pull_number, account));"
            "CREATE TABLE org_repos (org_bi TEXT, repo TEXT, node_owner TEXT);"
            "INSERT INTO org_repos VALUES ('org:acme', 'platform', 'alice');")
        self.roles = {"alice": "owner", "bob": "admin", "carol": "maintainer"}
        self.pull = {"number": "3", "base": "main", "author": "K" * 43,
                     "authorName": "dave"}
        self.keys = {"K" * 43: "dave"}
        self.catalog = {"branch": "main", "source": "hosted-web"}
        self.now = 1000
        self.fail_settings = False

    async def d1_first(self, env, sql, *args):
        if self.fail_settings and "repo_feature_settings" in sql:
            raise RuntimeError("database down")
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row else None

    async def d1_all(self, env, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]

    async def d1_run(self, env, sql, *args):
        self.db.execute(sql, args)

    def bind(self, module):
        async def blind_index(env, value):
            return value

        async def node(env, owner, repo):
            return "alice" if (owner, repo) == ("acme", "platform") else ""

        async def catalog(env, owner, repo):
            return dict(self.catalog, owner=owner, name=repo) if (
                owner, repo) == ("alice", "platform") else None

        async def pull(env, owner, repo, number):
            return dict(self.pull)

        async def account(env, key):
            return self.keys.get(key, "")

        async def role(env, org_bi, account):
            return self.roles.get(account, "") if org_bi == "org:acme" else ""

        module._bind_runtime({
            "d1_first": self.d1_first, "d1_all": self.d1_all,
            "d1_run": self.d1_run, "blind_index": blind_index,
            "_org_repo_node_strict": node, "_repo_catalog_record": catalog,
            "_pull_lifecycle_record": pull,
            "_git_push_account_for_pubkey": account, "_org_role": role,
            "valid_node_name": lambda name: bool(
                re.fullmatch(r"[a-z][a-z0-9-]{0,38}", name or "")),
            "Date": SimpleNamespace(now=lambda: self.now),
            "json_response": lambda data, status=200, **_: (status, data),
        })

    def protect(self, on=True, actions=None):
        doc = {"protectDefaultBranch": on}
        if actions is not None:
            doc["actions"] = actions
        self.db.execute(
            "INSERT OR REPLACE INTO repo_feature_settings VALUES (?,?,?)",
            ("repo-feature-settings:alice/platform", json.dumps(doc), 1))


@pytest.fixture
def world():
    sys.modules.pop("branch_protection", None)
    module = importlib.import_module("branch_protection")
    state = World()
    state.bind(module)
    return module, state


def review(module, state, account, verdict):
    state.now += 1
    return run(module.record_review(None, "acme", "platform", 3, account,
                                    verdict))


def test_protected_refs_follow_the_setting_and_the_catalog_branch(world):
    module, state = world
    assert run(module.protected_refs(None, "acme", "platform")) == []
    state.protect()
    refs = run(module.protected_refs(None, "acme", "platform"))
    assert refs == ["refs/heads/main", "refs/heads/forkmesh/"]
    assert run(module.protected_refs(None, "alice", "platform")) == refs
    assert module.branch_is_protected("main", refs)
    assert module.branch_is_protected("forkmesh/pulls", refs)
    assert not module.branch_is_protected("main-2", refs)
    assert not module.branch_is_protected("topic", refs)
    state.catalog["branch"] = ""
    with pytest.raises(module.Unavailable):
        run(module.protected_refs(None, "acme", "platform"))
    state.fail_settings = True
    assert run(module.is_protected(None, "acme", "platform")) is True
    assert run(module.push_protection(None, "acme", "platform")) == (
        None, "protection_unavailable")
    assert run(module.actions_off(None, "acme", "platform")) is True


def test_actions_switch_defaults_on(world):
    module, state = world
    assert run(module.actions_off(None, "acme", "platform")) is False
    state.protect(False, actions=False)
    assert run(module.actions_off(None, "acme", "platform")) is True


def test_branch_refusal_answers_with_the_branch(world):
    module, state = world
    state.protect()
    assert run(module.branch_refusal(None, "acme", "platform",
                                     ["topic"])) is None
    status, body = run(module.branch_refusal(None, "acme", "platform",
                                             ["topic", "main"]))
    assert status == 403 and body["error"] == "branch_protected"
    assert body["branch"] == "main"


def test_a_merge_into_a_protected_branch_needs_an_owner_or_admin_approval(
        world):
    module, state = world
    merge = lambda waive=False: run(module.merge_refusal(  # noqa: E731
        None, "acme", "platform", 3, waive))
    assert merge() is None
    state.protect()
    assert merge()[1]["error"] == "approval_required"
    review(module, state, "carol", "approved")
    assert merge()[1]["error"] == "approval_required"
    review(module, state, "dave", "approved")
    assert merge()[1]["error"] == "approval_required"
    review(module, state, "bob", "approved")
    assert merge() is None
    assert merge(waive=True)[1]["error"] == "review_waiver_not_allowed"
    review(module, state, "alice", "changes_requested")
    assert merge()[1]["error"] == "changes_requested"
    review(module, state, "alice", "dismissed")
    assert merge() is None
    review(module, state, "bob", "dismissed")
    assert merge()[1]["error"] == "approval_required"


def test_the_author_cannot_approve_under_another_name(world):
    module, state = world
    state.protect()
    state.pull["authorName"] = "bob"
    state.keys = {"K" * 43: "alice"}
    review(module, state, "alice", "approved")
    review(module, state, "bob", "approved")
    assert run(module.merge_refusal(None, "acme", "platform", 3, False))[
        1]["error"] == "approval_required"


def test_other_bases_and_unreadable_pulls(world):
    module, state = world
    state.protect()
    state.pull["base"] = "topic"
    assert run(module.merge_refusal(None, "acme", "platform", 3, True)) is None
    state.pull = {}
    status, body = run(module.merge_refusal(None, "acme", "platform", 3, False))
    assert status == 503 and body["error"] == "pull_unavailable"


def test_reviews_are_recorded_only_with_valid_values(world):
    module, state = world
    assert not review(module, state, "", "approved")
    assert not review(module, state, "bob", "commented")
    assert not run(module.record_review(None, "acme", "platform", 0, "bob",
                                        "approved"))
    assert review(module, state, "Bob", "approved")
    assert run(module.ledger(None, "acme", "platform", 3)) == {
        "bob": "approved"}


def test_only_organization_owners_and_admins_configure_protection(world):
    module, state = world
    for actor, allowed in (("alice", True), ("bob", True), ("carol", False),
                           ("dave", False), ("", False)):
        assert run(module.may_configure(None, actor, "acme", "platform")) is \
            allowed, actor


def test_settings_only_saves_skip_the_owner_key_for_hosted_repositories(world):
    module, state = world
    body = {"features": {"actions": False}, "sessionToken": "t"}
    assert run(module.settings_only(None, body, "acme", "platform"))
    assert not run(module.settings_only(
        None, dict(body, description="x"), "acme", "platform"))
    state.catalog["source"] = "remote-clone"
    assert not run(module.settings_only(None, body, "acme", "platform"))


def receive_pack_body(*commands):
    lines = []
    for index, (old, new, ref) in enumerate(commands):
        text = "%s %s %s" % (old, new, ref)
        if index == 0:
            text += "\0report-status"
        text += "\n"
        lines.append("%04x%s" % (len(text) + 4, text))
    return ("".join(lines) + "0000").encode()


def test_the_gateway_refuses_protected_refs():
    protected = mirror_gateway.parse_protected_refs(
        "refs/heads/main refs/heads/forkmesh/")
    zero, one = "0" * 40, "1" * 40
    commands = mirror_gateway.receive_pack_commands(
        receive_pack_body((zero, one, "refs/heads/topic")), 16)
    mirror_gateway.enforce_protected_refs(commands, protected)
    for ref in ("refs/heads/main", "refs/heads/forkmesh/pulls"):
        commands = mirror_gateway.receive_pack_commands(
            receive_pack_body((zero, one, "refs/heads/topic"),
                              (one, zero, ref)), 16)
        with pytest.raises(mirror_gateway.GatewayError, match="protected"):
            mirror_gateway.enforce_protected_refs(commands, protected)
    for bad in ("refs/tags/v1", "main", "refs/heads/a..b",
                " ".join(["refs/heads/x"] * 9)):
        with pytest.raises(mirror_gateway.GatewayError):
            mirror_gateway.parse_protected_refs(bad)
    assert mirror_gateway.parse_protected_refs("") == []
    assert "protect" in mirror_gateway.OPERATION_QUERY_FIELDS["git-receive-pack"]


def test_the_signed_push_target_carries_the_protection():
    import edge_routing
    url = edge_routing.masked_target_url(
        "https://git.example.test", "alice", "platform", "git-receive-pack",
        {"protect": "refs/heads/main refs/heads/forkmesh/"})
    assert url.endswith(
        "?protect=refs%2Fheads%2Fmain+refs%2Fheads%2Fforkmesh%2F")
    entry = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
    send = entry.split("async def _https_mirror_push_send(", 1)[1].split(
        "\nasync def ", 1)[0]
    assert "_branch_protection.push_protection(" in send
    assert 'query = dict(query or {}, protect=" ".join(protected))' in send
    assert send.index("protect=") < send.index("masked_target_url(")


def test_every_write_path_consults_the_protection():
    src = ROOT / "src"
    handlers = (src / "route_handlers.py").read_text(encoding="utf-8")
    ops = handlers.split("async def repository_branch_ops_handler(", 1)[1]
    assert ops.index("_branch_protection.branch_refusal(") < ops.index(
        "_branch_op_on_cores(")
    merge = handlers.split("async def repository_pull_merge_handler(", 1)[1]
    assert merge.index("_branch_protection.merge_refusal(") < merge.index(
        "INSERT OR IGNORE INTO repo_merge_jobs")
    agents = (src / "agent_branches.py").read_text(encoding="utf-8")
    assert agents.count("_branch_protection.") >= 3
    ssh = (src / "ssh_gateway_routes.py").read_text(encoding="utf-8")
    assert "_branch_protection.is_protected(" in ssh
    leases = (src / "runner_leases.py").read_text(encoding="utf-8")
    assert "_branch_protection.actions_off(" in leases
    assert "protection.branch_is_protected(" in leases
    entry = (src / "entry.py").read_text(encoding="utf-8")
    assert "await _record_pull_review(" in entry
    assert '"actions": True, "protectDefaultBranch": False' in entry
