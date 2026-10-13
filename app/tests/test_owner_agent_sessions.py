"""Owner-account web agent launch and owned-node routing regressions."""

import ast
import asyncio
from pathlib import Path


# MainWindowChat.cpp was split into feature files; read them in the original
# source order so section slices that span functions still line up.
MAIN_WINDOW_CHAT_SOURCES = (
    "MainWindowChat.cpp",
    "MainWindowNetworkInternal.h",
    "MainWindowDiagnostics.cpp",
    "MainWindowLogs.cpp",
    "MainWindowNavigation.cpp",
    "MainWindowNetworkRepos.cpp",
    "MainWindowHosts.cpp",
    "MainWindowNodes.cpp",
    "MainWindowNetworkDiagnostics.cpp",
    "MainWindowUsers.cpp",
    "MainWindowVultrMirrors.cpp",
    "MainWindowHostInstall.cpp",
)

# MainWindowAgents.cpp was split into feature files; read them in the original
# source order so section slices that span functions still line up.
MAIN_WINDOW_AGENT_SOURCES = (
    "MainWindowAgents.cpp",
    "MainWindowAgentsInternal.h",
    "MainWindowAgentSync.cpp",
    "MainWindowAgentUsage.cpp",
    "MainWindowAgentList.cpp",
    "MainWindowAgentDetail.cpp",
    "MainWindowAgentLaunch.cpp",
    "MainWindowAgentCliSession.cpp",
    "MainWindowAgentEvents.cpp",
    "MainWindowAgentTranscript.cpp",
    "MainWindowAgentLanding.cpp",
    "MainWindowAgentActions.cpp",
)

ROOT = Path(__file__).resolve().parents[1]
ENTRY_PATH = ROOT / "src" / "entry.py"
ENTRY = ENTRY_PATH.read_text(encoding="utf-8")
ROUTES = (ROOT / "src" / "route_handlers.py").read_text(encoding="utf-8")
URLS = (ROOT / "src" / "urls.py").read_text(encoding="utf-8")
RUNTIME = (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
DASHBOARD = (
    ROOT / "public" / "dashboard" / "js" / "06-repo-content.js"
).read_text(encoding="utf-8")
HEADER = (
    ROOT / "public" / "dashboard" / "partials" / "header.html"
).read_text(encoding="utf-8")
DESKTOP = "".join(
    (ROOT.parent / "desktop" / "src" / name).read_text(encoding="utf-8")
    for name in MAIN_WINDOW_AGENT_SOURCES
)
DESKTOP_MAIN = (
    ROOT.parent / "desktop" / "src" / "main.cpp"
).read_text(encoding="utf-8")
DESKTOP_CHAT = "".join(
    (ROOT.parent / "desktop" / "src" / name).read_text(encoding="utf-8")
    for name in MAIN_WINDOW_CHAT_SOURCES
)
SERVER_CATALOG = (
    ROOT.parent / "server" / "catalog.go"
).read_text(encoding="utf-8")


def _load_functions(*names, extra=None):
    tree = ast.parse(ENTRY, filename=str(ENTRY_PATH))
    selected = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in names
    ]
    assert {node.name for node in selected} == set(names)
    namespace = dict(extra or {})
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[])), str(ENTRY_PATH), "exec"),
         namespace)
    return namespace


def test_owner_context_requires_the_session_account_to_own_repo_node():
    async def account_session(_env, _request, _data):
        return "bi:alice", {"name": "alice"}

    async def owns(_env, actor, node):
        return actor == "alice" and node == "desktop1"

    def error(code, message, status, **details):
        return {"code": code, "message": message, "status": status, **details}

    ns = _load_functions(
        "_org_agent_member_context",
        extra={
            "clean_string": lambda value, maximum: str(value or "")[:maximum],
            "MAX_NODE_NAME": 63,
            "MAX_REPO_SEGMENT": 100,
            "valid_node_name": lambda value: bool(value) and "/" not in value,
            "safe_segment": lambda value: bool(value) and "/" not in value,
            "_account_session_record": account_session,
            "_account_owns_node": owns,
            "_org_owner_of_linked_repo": lambda _env, _node, _repo, _actor: (
                asyncio.sleep(0, result=False)),
            "_profile_catalog_owner_names": lambda _env, _actor: asyncio.sleep(
                0, result=["alice", "desktop1", "laptop"]),
            "_org_agent_error": error,
        },
    )
    context, error_response = asyncio.run(ns["_org_agent_member_context"](
        object(), object(), "desktop1", "project", {}, owner_scope=True))
    assert error_response is None
    assert context["scope"] == "owner"
    assert context["actor"] == "alice"
    assert context["nodeOwner"] == "desktop1"
    assert context["accessBasis"] == "owner_nodes"
    assert context["ownedNodes"] == ["alice", "desktop1", "laptop"]

    context, error_response = asyncio.run(ns["_org_agent_member_context"](
        object(), object(), "stranger", "project", {}, owner_scope=True))
    assert context is None
    assert error_response["code"] == "forbidden"


def test_a_session_on_a_node_no_longer_owned_stays_readable_and_deletable():
    async def account_session(_env, _request, _data):
        return "bi:jett", {"name": "jett"}

    def error(code, message, status, **details):
        return {"code": code, "message": message, "status": status, **details}

    ns = _load_functions(
        "_org_agent_member_context",
        extra={
            "clean_string": lambda value, maximum: str(value or "")[:maximum],
            "MAX_NODE_NAME": 63,
            "MAX_REPO_SEGMENT": 100,
            "valid_node_name": lambda value: bool(value) and "/" not in value,
            "safe_segment": lambda value: bool(value) and "/" not in value,
            "_account_session_record": account_session,
            "_account_owns_node": lambda _env, _actor, _node: asyncio.sleep(
                0, result=False),
            "_org_owner_of_linked_repo": lambda _env, _node, _repo, _actor: (
                asyncio.sleep(0, result=False)),
            "_profile_catalog_owner_names": lambda _env, _actor: asyncio.sleep(
                0, result=["jett", "laptop"]),
            "_org_agent_error": error,
        },
    )
    context, error_response = asyncio.run(ns["_org_agent_member_context"](
        object(), object(), "mirror35", "forkmesh", {}, owner_scope=True))
    assert context is None
    assert error_response["code"] == "forbidden"

    context, error_response = asyncio.run(ns["_org_agent_member_context"](
        object(), object(), "mirror35", "forkmesh", {}, owner_scope=True,
        own_session=True))
    assert error_response is None
    assert context["orgBi"] == "bi:jett"
    assert context["org"] == "mirror35"
    assert context["accessBasis"] == "own_session"
    assert context["ownedNodes"] == ["jett", "laptop"]


def test_only_reads_stops_and_deletes_skip_the_node_ownership_check():
    handler = ROUTES[ROUTES.index("async def org_agent_bot_handler("):]
    handler = handler[:handler.index("\nasync def ", 1)]
    assert 'own_session=method in ("GET", "DELETE") or stopping' in handler
    attachment = ROUTES[
        ROUTES.index("async def org_agent_session_attachment_handler("):]
    attachment = attachment[:attachment.index("\nasync def ", 1)]
    assert "own_session=True" in attachment
    queue = ROUTES[ROUTES.index("async def org_agent_bots_handler("):]
    queue = queue[:queue.index("\nasync def ", 1)]
    assert "own_session" not in queue


def test_owner_targets_include_only_owned_agent_nodes_for_same_repo():
    records = [
        {"owner": "desktop1", "name": "project", "rootCommit": "abc",
         "runtimeMode": "desktop", "agentProviders": ["claude-code"],
         "updatedAt": 100},
        {"owner": "laptop", "name": "project", "rootCommit": "abc",
         "runtimeMode": "desktop", "agentProviders": ["codex"],
         "updatedAt": 699950},
        {"owner": "foreign", "name": "project", "rootCommit": "abc",
         "runtimeMode": "desktop", "agentProviders": ["codex"],
         "updatedAt": 699990},
        {"owner": "desktop1", "name": "other", "rootCommit": "different",
         "runtimeMode": "desktop", "agentProviders": ["claude-code"],
         "updatedAt": 699990},
        {"owner": "headless", "name": "project", "rootCommit": "abc",
         "runtimeMode": "headless", "agentProviders": ["claude-code", "codex"],
         "updatedAt": 699990},
    ]

    class Clock:
        @staticmethod
        def now():
            return 700000

    async def catalog(_env, _now):
        return [{"data": record, "is_private": False} for record in records]

    ns = _load_functions(
        "_org_agent_owner_targets",
        extra={
            "Date": Clock,
            "_decrypted_public_catalog": catalog,
            "repo_mirror_same_group": lambda target, record: (
                target.get("rootCommit") == record.get("rootCommit")),
            "valid_node_name": lambda value: bool(value) and "/" not in value,
            "ORG_AGENT_PROVIDERS": ("claude-code", "codex"),
            "_org_agent_pool_target": lambda _env, _context, _now: (
                asyncio.sleep(0, result=None)),
            "_org_agent_personal_targets": lambda _env, _context, _now: (
                asyncio.sleep(0, result=[])),
        },
    )
    targets = asyncio.run(ns["_org_agent_owner_targets"](object(), {
        "scope": "owner",
        "nodeOwner": "desktop1",
        "repo": "project",
        "actor": "alice",
        "ownedNodes": ["desktop1", "laptop", "headless"],
    }))
    assert [target["node"] for target in targets] == [
        "headless", "laptop", "desktop1"]
    assert targets[0]["providers"] == ["claude-code", "codex"]
    assert targets[0]["runtimeMode"] == "headless"
    assert targets[0]["online"] is True
    assert targets[1]["providers"] == ["codex"]
    assert targets[2]["source"] is True
    assert targets[2]["online"] is False


def test_owner_may_queue_to_a_selected_offline_owned_desktop():
    targets = [
        {"node": "laptop", "providers": ["codex"], "online": True},
        {"node": "desktop1", "providers": ["claude-code"], "online": False},
    ]

    async def owner_targets(_env, _context):
        return targets

    ns = _load_functions(
        "_org_agent_target_mirror",
        extra={
            "_org_agent_owner_targets": owner_targets,
            "clean_string": lambda value, maximum: str(value or "")[:maximum],
            "MAX_NODE_NAME": 63,
        },
    )
    decision = asyncio.run(ns["_org_agent_target_mirror"](
        object(), {"scope": "owner"}, "claude-code", "desktop1"))
    assert decision == {
        "ok": True,
        "targetNode": "desktop1",
        "targetOnline": False,
        "queueState": "waiting_for_desktop",
        "route": "owner_node",
    }

    rejected = asyncio.run(ns["_org_agent_target_mirror"](
        object(), {"scope": "owner"}, "codex", "desktop1"))
    assert rejected["ok"] is False
    assert rejected["error"] == "preferred_target_ineligible"


def test_organization_target_picker_merges_provider_capabilities_by_node():
    records = [{"owner": "source", "name": "project"}]

    class Clock:
        @staticmethod
        def now():
            return 700000

    async def catalog(_env, _now):
        return [{"data": record, "is_private": False} for record in records]

    def candidates(_records, _target, _source, provider, _now, _fresh_ms):
        return {
            "claude-code": ["shared", "claude-only"],
            "codex": ["shared", "codex-only"],
        }[provider]

    ns = _load_functions(
        "_org_agent_session_targets",
        extra={
            "Date": Clock,
            "_decrypted_public_catalog": catalog,
            "_account_owns_node": lambda *_args: asyncio.sleep(
                0, result=False),
            "_is_admin": lambda *_args: asyncio.sleep(0, result=False),
            "agent_provider_target_decision": lambda *_args, **_kwargs: {
                "ok": False,
            },
            "agent_provider_mirror_candidates": candidates,
            "valid_node_name": lambda value: bool(value) and "/" not in value,
            "ORG_AGENT_PROVIDERS": ("claude-code", "codex"),
        },
    )
    targets = asyncio.run(ns["_org_agent_session_targets"](object(), {
        "scope": "organization",
        "nodeOwner": "source",
        "repo": "project",
        "actor": "engineer",
        "role": "member",
    }))
    assert targets == [
        {"node": "claude-only", "providers": ["claude-code"],
         "online": True, "source": False},
        {"node": "codex-only", "providers": ["codex"],
         "online": True, "source": False},
        {"node": "shared", "providers": ["claude-code", "codex"],
         "online": True, "source": False},
    ]


def test_owner_launch_route_ui_and_desktop_pr_contract_are_connected():
    assert "REPO_OWNER_AGENT_SESSIONS_RE" in URLS
    assert "REPO_OWNER_AGENT_SESSION_RE" in URLS
    assert "owner_scope=True" in RUNTIME
    assert '"targets": targets' in ROUTES
    assert 'create_pr = data.get("createPr") is True' in ROUTES
    assert '"createPr": create_pr' in ROUTES
    assert "${repoApiBase(repo)}/agent-sessions" in DASHBOARD
    assert "data-agent-modal-target" in HEADER
    assert 'name="createPr" type="checkbox" class=' in HEADER
    assert 'name="createPr" type="checkbox" checked' not in HEADER
    assert 'job.value(QStringLiteral("createPr")).toBool(false)' in DESKTOP
    assert "provider, createPr, requestedModel" in DESKTOP
    assert "repoDisplayKey(repo)" in DASHBOARD
    assert "forkmesh://agent-login/" in DASHBOARD
    assert "data-agent-modal-login" in HEADER
    assert "agentLoginLinkParts" in DESKTOP_MAIN
    assert "openAgentLoginFromSystemLink" in DESKTOP_MAIN
    assert "openHostAgentLoginTerminal" in DESKTOP_CHAT
    assert 'record["agentProviders"] = providers' in SERVER_CATALOG


def test_web_permission_mode_and_speed_reach_the_desktop_launch():
    queue = ROUTES[ROUTES.index("async def org_agent_bots_handler("):]
    queue = queue[:queue.index("session_data = await encrypt_row(env, record)")]
    assert 'data.get("mode")' in queue
    assert "if not owner_scope:\n        run_mode = \"\"" in queue
    assert 'job["mode"] = run_mode' in queue
    assert 'job["effort"] = run_effort' in queue
    assert "run_effort not in _ORG_AGENT_RUN_EFFORTS" in queue
    assert 'job.value(QStringLiteral("mode"))' in DESKTOP
    assert 'job.value(QStringLiteral("effort"))' in DESKTOP
    assert "kWebModes.contains(requestedMode) ? requestedMode : QString()" in DESKTOP
    assert "strengthOverride.trimmed().isEmpty()" in DESKTOP


def test_org_owner_of_the_alias_may_start_owner_agents_on_its_backing_node():
    async def account_session(_env, _request, _data):
        return "bi:alice", {"name": "alice"}

    async def owns(_env, _actor, _node):
        return False

    async def org_owner(_env, node, repo, actor):
        return (node, repo, actor) == ("core1", "project", "alice")

    def error(code, message, status, **details):
        return {"code": code, "message": message, "status": status, **details}

    ns = _load_functions(
        "_org_agent_member_context",
        extra={
            "clean_string": lambda value, maximum: str(value or "")[:maximum],
            "MAX_NODE_NAME": 63,
            "MAX_REPO_SEGMENT": 100,
            "valid_node_name": lambda value: bool(value) and "/" not in value,
            "safe_segment": lambda value: bool(value) and "/" not in value,
            "_account_session_record": account_session,
            "_account_owns_node": owns,
            "_org_owner_of_linked_repo": org_owner,
            "_profile_catalog_owner_names": lambda _env, _actor: asyncio.sleep(
                0, result=["alice"]),
            "_org_agent_error": error,
        },
    )
    context, error_response = asyncio.run(ns["_org_agent_member_context"](
        object(), object(), "core1", "project", {}, owner_scope=True))
    assert error_response is None
    assert context["nodeOwner"] == "core1"
    assert "core1" in context["ownedNodes"]

    context, error_response = asyncio.run(ns["_org_agent_member_context"](
        object(), object(), "core1", "other", {}, owner_scope=True))
    assert context is None
    assert error_response["code"] == "forbidden"


def test_org_owner_of_linked_repo_requires_the_owner_role_on_a_linking_org():
    roles = {("org-a", "alice"): "owner", ("org-a", "bob"): "admin"}

    async def d1_all(_env, sql, node, repo):
        assert "org_repos" in sql
        return [{"org_bi": "org-a"}] if (node, repo) == ("core1", "project") else []

    async def org_role(_env, org_bi, actor):
        return roles.get((org_bi, actor), "")

    ns = _load_functions(
        "_org_owner_of_linked_repo",
        extra={"d1_all": d1_all, "_org_role": org_role},
    )
    check = ns["_org_owner_of_linked_repo"]
    assert asyncio.run(check(object(), "core1", "project", "alice")) is True
    assert asyncio.run(check(object(), "Core1", "Project", "Alice")) is True
    assert asyncio.run(check(object(), "core1", "project", "bob")) is False
    assert asyncio.run(check(object(), "core1", "other", "alice")) is False
    assert asyncio.run(check(object(), "", "project", "alice")) is False


def _pool_ns(rows, personal=None):
    async def d1_all(_env, sql, *args):
        assert "runner_nodes" in sql
        if "scope='personal'" in sql:
            return [row for row in personal or []
                    if row.get("registered_by") == args[0]]
        assert "scope='pool'" in sql
        return rows

    import json as _json
    return _load_functions(
        "_org_agent_pool_target",
        "_org_agent_personal_targets",
        "_org_agent_runner_status",
        "_org_agent_pool_node",
        extra={
            "d1_all": d1_all,
            "json": _json,
            "valid_node_name": lambda value: bool(value) and "/" not in value,
            "ORG_AGENT_PROVIDERS": ("claude-code", "codex"),
        },
    )


class _PoolEnv:
    RUNNER_POOL_TARGET_NODES = "core1, core2"


def test_pool_target_routes_owner_jobs_on_a_pool_core_to_an_opted_in_runner():
    ready = '{"claude-code": {"loginState": "available"}, "codex": {"loginState": "missing"}}'
    ns = _pool_ns([
        {"name": "runner2", "allowed_owners": '["alice"]',
         "last_seen_at": 680000, "data": ready},
        {"name": "runner1", "allowed_owners": '["alice"]',
         "last_seen_at": 690000, "data": ready},
    ])
    target = asyncio.run(ns["_org_agent_pool_target"](
        _PoolEnv(), {"nodeOwner": "core1", "org": "core1", "actor": "alice"},
        700000))
    runners = target.pop("runners")
    assert target == {
        "node": "runner1", "queueNode": "core1", "runner": "runner1",
        "providers": ["claude-code"], "needsLogin": {}, "online": True,
        "lastSeenAt": 690000, "source": True, "runtimeMode": "runner-pool",
    }
    assert [item["name"] for item in runners] == ["runner1", "runner2"]
    assert runners[0] == {
        "name": "runner1", "scope": "pool", "online": True,
        "lastSeenAt": 690000, "ready": ["claude-code"], "needsLogin": [],
        "signIn": ["codex"],
    }


def test_pool_target_names_a_runner_whose_login_expired():
    expired = ('{"claude-code": {"binaryFound": true, "loginState": "expired", '
               '"auth": {"state": "expired"}}, "codex": {"binaryFound": true, '
               '"loginState": "available"}}')
    ns = _pool_ns([
        {"name": "fmnext-r1", "allowed_owners": '["alice"]',
         "last_seen_at": 690000, "data": expired},
    ])
    context = {"nodeOwner": "core1", "org": "core1", "actor": "alice"}
    target = asyncio.run(ns["_org_agent_pool_target"](
        _PoolEnv(), context, 700000))
    assert target["providers"] == ["codex"]
    assert target["needsLogin"] == {"claude-code": ["fmnext-r1"]}
    only_claude = expired.replace('"loginState": "available"', '"loginState": "missing"')
    ns = _pool_ns([
        {"name": "fmnext-r1", "allowed_owners": '["alice"]',
         "last_seen_at": 690000, "data": only_claude},
    ])
    target = asyncio.run(ns["_org_agent_pool_target"](
        _PoolEnv(), context, 700000))
    assert target["providers"] == []
    assert target["needsLogin"] == {"claude-code": ["fmnext-r1"]}


def test_owner_submit_says_which_runner_needs_a_reconnect():
    async def owner_targets(_env, _context):
        return [{"node": "core1", "queueNode": "core1", "providers": [],
                 "needsLogin": {"claude-code": ["fmnext-r1"]},
                 "online": False, "runtimeMode": "runner-pool"}]

    ns = _load_functions(
        "_org_agent_target_mirror",
        extra={
            "_org_agent_owner_targets": owner_targets,
            "clean_string": lambda value, maximum: str(value or "")[:maximum],
            "MAX_NODE_NAME": 63,
        },
    )
    for preferred in ("", "core1"):
        decision = asyncio.run(ns["_org_agent_target_mirror"](
            object(), {"scope": "owner", "actor": "alice"}, "claude-code",
            preferred))
        assert decision["ok"] is False
        assert decision["error"] == "runner_login_expired"
        assert decision["message"].startswith(
            "Runner fmnext-r1 needs Claude reconnected")
        assert decision["targetNode"] == "fmnext-r1"
    other = asyncio.run(ns["_org_agent_target_mirror"](
        object(), {"scope": "owner", "actor": "alice"}, "codex"))
    assert other["error"] == "provider_not_advertised"


def test_pool_target_without_a_runner_name_falls_back_to_the_core():
    ready = '{"claude-code": {"loginState": "available"}}'
    ns = _pool_ns([
        {"allowed_owners": '["alice"]', "last_seen_at": 690000, "data": ready},
    ])
    target = asyncio.run(ns["_org_agent_pool_target"](
        _PoolEnv(), {"nodeOwner": "core1", "org": "core1", "actor": "alice"},
        700000))
    assert target["node"] == "core1" and target["queueNode"] == "core1"


def test_pool_target_needs_a_pool_node_an_opted_in_requester_and_a_signed_in_cli():
    ready = '{"claude-code": {"loginState": "available"}}'
    context = {"nodeOwner": "core1", "org": "core1", "actor": "alice"}
    ns = _pool_ns([
        {"allowed_owners": '["core1"]', "last_seen_at": 690000, "data": ready},
    ])
    assert asyncio.run(ns["_org_agent_pool_target"](
        _PoolEnv(), context, 700000)) is None
    ns = _pool_ns([
        {"allowed_owners": '["alice"]', "last_seen_at": 690000,
         "data": '{"claude-code": {"loginState": "missing"}}'},
    ])
    assert asyncio.run(ns["_org_agent_pool_target"](
        _PoolEnv(), context, 700000)) is None
    ns = _pool_ns([
        {"name": "fresh-r1", "allowed_owners": '["alice"]',
         "last_seen_at": 690000,
         "data": '{"claude-code": {"binaryFound": true, '
                 '"loginState": "missing"}}'},
    ])
    unsigned = asyncio.run(ns["_org_agent_pool_target"](
        _PoolEnv(), context, 700000))
    assert unsigned["providers"] == [] and unsigned["runner"] == "fresh-r1"
    assert unsigned["node"] == "core1"
    assert unsigned["runners"][0]["signIn"] == ["claude-code"]
    assert unsigned["online"] is True
    ns = _pool_ns([
        {"allowed_owners": '["alice"]', "last_seen_at": 690000, "data": ready},
    ])
    assert asyncio.run(ns["_org_agent_pool_target"](
        _PoolEnv(), {"nodeOwner": "desktop1", "org": "desktop1",
                     "actor": "alice"}, 700000)) is None
    assert asyncio.run(ns["_org_agent_pool_target"](
        _PoolEnv(), {"nodeOwner": "core1", "org": "core1"}, 700000)) is None
    ns = _pool_ns([
        {"allowed_owners": '["alice"]', "last_seen_at": 1000, "data": ready},
    ])
    stale = asyncio.run(ns["_org_agent_pool_target"](
        _PoolEnv(), context, 700000))
    assert stale["online"] is False


def test_owner_decision_for_a_pool_target_names_the_requester_as_runner_owner():
    async def owner_targets(_env, _context):
        return [{"node": "runner1", "queueNode": "core1",
                 "providers": ["claude-code"],
                 "online": True, "runtimeMode": "runner-pool"}]

    ns = _load_functions(
        "_org_agent_target_mirror",
        extra={
            "_org_agent_owner_targets": owner_targets,
            "clean_string": lambda value, maximum: str(value or "")[:maximum],
            "MAX_NODE_NAME": 63,
        },
    )
    decision = asyncio.run(ns["_org_agent_target_mirror"](
        object(), {"scope": "owner", "actor": "Alice"}, "claude-code"))
    assert decision["targetNode"] == "core1"
    assert decision["runnerNode"] == "runner1"
    assert decision["route"] == "runner_pool"
    assert decision["runnerOwner"] == "alice"
    for preferred in ("runner1", "core1"):
        picked = asyncio.run(ns["_org_agent_target_mirror"](
            object(), {"scope": "owner", "actor": "alice"}, "claude-code",
            preferred))
        assert picked["targetNode"] == "core1"
        assert picked["runnerNode"] == "runner1"


def test_pool_sessions_are_shown_under_the_runner_name():
    assert '"runnerNode": shown_node,' in ROUTES
    assert 'f"Sent to {shown_node}.' in ROUTES
    assert 'record.get("runnerNode") or row.get("target_node")' in ROUTES


def test_queued_pool_job_carries_the_runner_owner_as_its_organization():
    assert 'target_decision.get("runnerOwner")' in ROUTES
    assert 'job["organization"] = runner_owner' in ROUTES


def test_pool_target_for_an_expired_runner_keeps_its_name_and_last_seen():
    expired = ('{"claude-code": {"binaryFound": true, "loginState": "expired", '
               '"auth": {"state": "expired"}}, "codex": {"binaryFound": false, '
               '"loginState": "missing"}}')
    ns = _pool_ns([
        {"name": "fmnext-r1", "scope": "pool", "allowed_owners": '["jett"]',
         "last_seen_at": 690000, "data": expired},
    ])
    target = asyncio.run(ns["_org_agent_pool_target"](
        _PoolEnv(), {"nodeOwner": "core1", "org": "core1", "actor": "jett"},
        700000))
    assert target["node"] == "core1" and target["runner"] == "fmnext-r1"
    assert target["providers"] == []
    assert target["needsLogin"] == {"claude-code": ["fmnext-r1"]}
    assert target["online"] is True and target["lastSeenAt"] == 690000
    assert target["runners"] == [{
        "name": "fmnext-r1", "scope": "pool", "online": True,
        "lastSeenAt": 690000, "ready": [], "needsLogin": ["claude-code"],
        "signIn": [],
    }]
    assert asyncio.run(ns["_org_agent_pool_target"](
        _PoolEnv(), {"nodeOwner": "core1", "org": "core1", "actor": "bob"},
        700000)) is None


def test_personal_runners_are_their_own_pinned_targets():
    ready = '{"claude-code": {"binaryFound": true, "loginState": "available"}}'
    ns = _pool_ns([], personal=[
        {"name": "alice-box", "scope": "personal", "registered_by": "alice",
         "allowed_owners": '["alice"]', "last_seen_at": 1000, "data": ready},
        {"name": "core2", "scope": "personal", "registered_by": "alice",
         "allowed_owners": '["alice"]', "last_seen_at": 690000, "data": ready},
        {"name": "bob-box", "scope": "personal", "registered_by": "bob",
         "allowed_owners": '["bob"]', "last_seen_at": 690000, "data": ready},
    ])
    context = {"nodeOwner": "core1", "org": "core1", "actor": "alice"}
    targets = asyncio.run(ns["_org_agent_personal_targets"](
        _PoolEnv(), context, 700000))
    assert [target["node"] for target in targets] == ["alice-box"]
    target = targets[0]
    assert target["queueNode"] == "core1"
    assert target["runnerPin"] == "alice-box"
    assert target["providers"] == ["claude-code"]
    assert target["online"] is False and target["lastSeenAt"] == 1000
    assert target["runtimeMode"] == "runner-pool"
    assert asyncio.run(ns["_org_agent_personal_targets"](
        _PoolEnv(), {"nodeOwner": "desktop1", "org": "desktop1",
                     "actor": "alice"}, 700000)) == []


def test_owner_decision_pins_a_personal_runner_target():
    async def owner_targets(_env, _context):
        return [{"node": "alice-box", "queueNode": "core1",
                 "runnerPin": "alice-box", "providers": ["claude-code"],
                 "online": True, "runtimeMode": "runner-pool"}]

    ns = _load_functions(
        "_org_agent_target_mirror",
        extra={
            "_org_agent_owner_targets": owner_targets,
            "clean_string": lambda value, maximum: str(value or "")[:maximum],
            "MAX_NODE_NAME": 63,
        },
    )
    decision = asyncio.run(ns["_org_agent_target_mirror"](
        object(), {"scope": "owner", "actor": "alice"}, "claude-code",
        "alice-box"))
    assert decision["targetNode"] == "core1"
    assert decision["runnerNode"] == "alice-box"
    assert decision["runnerPin"] == "alice-box"
    assert 'job["runner"] = runner_pin' in ROUTES


def test_new_agents_never_request_a_pull_request_by_default():
    repo = ROOT.parent
    composer = (ROOT / "public" / "dashboard-composer.js").read_text(encoding="utf-8")
    assert "createPr: composer.stored.createPr === true," in composer
    assert "createPr !== false" not in composer
    assert "setStored({ createPr: false });" in composer
    assert "createPr: true" not in DASHBOARD
    for page in (ROOT / "public").rglob("*.html"):
        assert 'name="createPr" type="checkbox" checked' not in page.read_text(
            encoding="utf-8"), page
    ios = repo / "native" / "ios" / "ForkMeshCore" / "Sources"
    for source in (
        ios / "FMAgents" / "AgentStartModel.swift",
        ios / "FMApi" / "Services" / "ApiService+Agents.swift",
        ios / "FMApi" / "Services" / "ApiService+RunnerAgents.swift",
    ):
        text = source.read_text(encoding="utf-8")
        assert "createPr: true" not in text, source
        assert "createPr: Bool = true" not in text, source
    sheet = (repo / "native" / "ios" / "ForkMesh" / "Features" / "Code" / "Board"
             / "IssueToolsSheet.swift").read_text(encoding="utf-8")
    assert "createPr: mention?.intent == .work" not in sheet
    android = (repo / "native" / "android" / "core" / "src" / "main" / "kotlin" / "com"
               / "forkmesh" / "nativeapp" / "agents" / "AgentQueueApi.kt").read_text(
        encoding="utf-8")
    assert "createPr: Boolean = true" not in android
    mcp = (repo / "tools" / "forkmesh_mcp_server.py").read_text(encoding="utf-8")
    assert "only when the user explicitly" in mcp
