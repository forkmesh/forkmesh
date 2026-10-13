"""The /dashboard/agents page and GET /api/account/desktop-agents.

See docs/app/agents-page.md.

Run: python3 -m pytest app/tests/test_agents_dashboard.py
"""

import ast
import asyncio
import json
import re
import shutil
import subprocess
import sys
import tomllib
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
if str(ROOT / "src") not in sys.path:
    sys.path.insert(0, str(ROOT / "src"))

import dashboard_bundle  # noqa: E402
import dashboard_shell  # noqa: E402
import static_routes  # noqa: E402
import urls  # noqa: E402


PUBLIC = ROOT / "public"
DASHBOARD = PUBLIC / "dashboard"
HANDLERS = ROOT / "src" / "route_handlers.py"
AGENTS_JS = (DASHBOARD / "js" / "09-agents.js").read_text(encoding="utf-8")
REPO_JS = (DASHBOARD / "js" / "06-repo-content.js").read_text(encoding="utf-8")
HELPERS_JS = (DASHBOARD / "js" / "02-helpers.js").read_text(encoding="utf-8")
BOOT_JS = (DASHBOARD / "js" / "10-boot.js").read_text(encoding="utf-8")
SIDEBAR = (DASHBOARD / "partials" / "sidebar.html").read_text(encoding="utf-8")
BUNDLE = (PUBLIC / "dashboard.js").read_text(encoding="utf-8")
CHUNK = (PUBLIC / "dashboard-page-agents.js").read_text(encoding="utf-8")
WRANGLER = tomllib.loads((ROOT / "wrangler.toml").read_text(encoding="utf-8"))
APP_RUNTIME = (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
ENTRY = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")


def test_agents_page_is_a_composed_dashboard_page():
    page = dashboard_shell.PAGES["agents"]
    assert page["route"] == "/dashboard/agents"
    assert page["view"] == "agents" and page["nav"] == "agents"
    assert page["asset"] == "dashboard/agents/index.html"
    assert static_routes.DASHBOARD_PAGE_ASSETS["/dashboard/agents"] == page["asset"]
    assert "/dashboard/agents/index.html" in static_routes.BLOCKED_STATIC_HTML_PATHS
    first = WRANGLER["assets"]["run_worker_first"]
    assert "!/dashboard/agents" in first and "!/dashboard/agents/" in first
    built = (PUBLIC / page["asset"]).read_text(encoding="utf-8")
    assert built.count("<section data-view=") == 1
    assert 'data-page="agents"' in built
    assert "/dashboard-page-agents.js?v=" in built
    for marker in ('data-agents-filter="status"', 'data-agents-filter="repo"',
                   'data-agents-filter="node"', "data-agents-list",
                   "data-agents-refresh", "data-agents-count"):
        assert marker in built, marker


def test_page_module_is_a_lazy_chunk_the_boot_dispatches():
    assert dashboard_bundle.LAZY_CHUNKS["agents"] == "09-agents.js"
    assert "09-agents.js" in dashboard_bundle.FRAGMENTS
    assert dashboard_shell.CHUNK_PAGES["agents"] == "agents"
    assert "dashboard-page-agents.js" in dashboard_shell.CACHE_BUSTED_ASSETS
    assert '"agents": initAgentsPage,' in BOOT_JS
    assert "function initAgentsPage()" in AGENTS_JS
    assert "function initAgentsPage(" in BUNDLE
    assert "function renderAgentsPage()" in CHUNK


def test_nav_link_is_visible_to_every_signed_in_account_on_every_page():
    tag = SIDEBAR[SIDEBAR.index('<a data-nav="agents"'):]
    link = tag[:tag.index("</a>")]
    tag = tag[:tag.index(">")]
    assert 'href="/dashboard/agents"' in tag
    assert "data-admin-only" not in tag and 'class="hidden' not in tag
    assert link.rstrip().endswith("Agents")
    assert SIDEBAR.index('data-nav="agents"') < SIDEBAR.index('data-nav="apps"')
    for meta in dashboard_shell.PAGES.values():
        built = (PUBLIC / meta["asset"]).read_text(encoding="utf-8")
        if 'data-nav="repos"' in built:
            assert 'data-nav="agents"' in built, meta["asset"]


def test_page_reads_both_account_lists_and_follows_the_socket():
    assert '"/api/account/agent-sessions"' in AGENTS_JS
    assert '"/api/account/desktop-agents"' in AGENTS_JS
    assert 'addEventListener("forkmesh:cloud-agents"' in AGENTS_JS
    assert 'addEventListener("forkmesh:desktop-agents"' in AGENTS_JS
    assert 'if (topic === "agents")' in HELPERS_JS
    assert "setInterval" not in AGENTS_JS and "setTimeout" not in AGENTS_JS


def test_repo_agents_tab_opens_a_linked_session():
    take = REPO_JS[REPO_JS.index("function takeLinkedAgentSession()"):]
    take = take[:take.index("\n  }\n")]
    assert 'params.get("session")' in take
    assert "/agents" in take and 'params.delete("session")' in take
    assert "  let linkedAgentSession = takeLinkedAgentSession();\n" in REPO_JS
    load = REPO_JS[REPO_JS.index("async function loadRepoAgents("):]
    load = load[:load.index("orgAgentRequest(")]
    assert "state.agentsView.selectedAgentId = linkedAgentSession;" in load
    assert 'linkedAgentSession = "";' in load


def test_desktop_agents_route_is_dispatched():
    assert urls.ACCOUNT_DESKTOP_AGENTS_RE.match("/api/account/desktop-agents")
    assert not urls.ACCOUNT_DESKTOP_AGENTS_RE.match("/api/account/desktop-agents/x")
    assert "ACCOUNT_DESKTOP_AGENTS_RE.match(url.path)" in APP_RUNTIME
    assert "account_desktop_agents_handler(self.env, request)" in APP_RUNTIME
    assert '"ACCOUNT_DESKTOP_AGENTS_RE",' in ENTRY
    assert '_route_handlers.export(\n    "account_desktop_agents_handler")' in ENTRY


class Recorder:
    def __init__(self, token=None, owners=(), repositories=None, agents=()):
        self.token = token
        self.owners = list(owners)
        self.repositories = dict(repositories or {})
        self.agents = list(agents)
        self.queries = []

    async def d1_all(self, _env, sql, *args):
        self.queries.append((sql, args))
        if "FROM repositories" in sql:
            return self.repositories.get(args[0], [])[:args[-1]]
        if "FROM repo_agents" in sql:
            keys = set(args[:-1])
            rows = [row for row in self.agents if row["repo_bi"] in keys]
            rows.sort(key=lambda row: -row["updated_at"])
            return rows[:args[-1]]
        raise AssertionError(sql)

    async def account_session(self, _env, _request, _data=None):
        return self.token or ("", None)

    async def owner_names(self, _env, name):
        assert name == "jett"
        return list(self.owners)


def _load(recorder, **constants):
    tree = ast.parse(HANDLERS.read_text(encoding="utf-8"), filename=str(HANDLERS))
    wanted = {"account_desktop_agents_handler"}
    names = {"ACCOUNT_DESKTOP_AGENT_MAX_REPOS", "ACCOUNT_DESKTOP_AGENT_MAX_ROWS",
             "ACCOUNT_DESKTOP_AGENT_CHUNK"}
    selected = [
        node for node in tree.body
        if (isinstance(node, (ast.AsyncFunctionDef, ast.FunctionDef))
            and node.name in wanted)
        or (isinstance(node, ast.Assign)
            and any(isinstance(target, ast.Name) and target.id in names
                    for target in node.targets))
    ]
    assert len(selected) == len(wanted) + len(names)
    module = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))

    async def noop(*_args, **_kwargs):
        return None

    namespace = {
        "MAX_NODE_NAME": 64,
        "MAX_REPO_SEGMENT": 80,
        "ensure_schema": noop,
        "method_name": lambda request: request["method"],
        "clean_string": lambda value, maximum: str(
            "" if value is None else value)[:maximum],
        "d1_all": recorder.d1_all,
        "blind_index": lambda _env, value: asyncio.sleep(0, result="bi:" + value),
        "decrypt_row": lambda _env, value: asyncio.sleep(0, result=value),
        "_account_session_record": recorder.account_session,
        "_profile_catalog_owner_names": recorder.owner_names,
        "_org_agent_error": lambda code, message, status, **kwargs: {
            "status": status,
            "data": {"ok": False, "error": code, "message": message, **kwargs},
        },
        "json_response": lambda data, status=200, **kwargs: {
            "status": status, "data": data, **kwargs},
    }
    exec(compile(module, str(HANDLERS), "exec"), namespace)
    namespace.update(constants)
    return namespace


def _get(recorder, method="GET", **constants):
    namespace = _load(recorder, **constants)
    return asyncio.run(namespace["account_desktop_agents_handler"](
        object(), {"method": method}))


ACCOUNT = ("bi:jett", {"name": "Jett"})


def _repo(owner, name):
    return {"key_bi": "bi:%s/%s" % (owner, name), "data": {"name": name}}


def _agent(owner, name, agent_id, updated):
    return {"repo_bi": "bi:%s/%s" % (owner, name), "agent_id": str(agent_id),
            "updated_at": updated, "data": "owner-sealed-v1:secret"}


def test_desktop_agents_list_only_routing_metadata_for_owned_nodes():
    recorder = Recorder(
        token=ACCOUNT,
        owners=["jett", "jett-laptop"],
        repositories={
            "bi:jett": [_repo("jett", "site")],
            "bi:jett-laptop": [_repo("jett-laptop", "engine"),
                               _repo("jett-laptop", "quiet")],
        },
        agents=[_agent("jett-laptop", "engine", 7, 300),
                _agent("jett", "site", 2, 100),
                _agent("someone", "else", 9, 900)],
    )
    response = _get(recorder)
    assert response["status"] == 200
    assert response["cache_control"] == "no-store"
    data = response["data"]
    assert data["ok"] is True and data["account"] == "jett"
    assert data["serverCanDecrypt"] is False and data["truncated"] is False
    assert data["agents"] == [
        {"id": "7", "owner": "jett-laptop", "repo": "engine",
         "node": "jett-laptop", "reportedAt": 300},
        {"id": "2", "owner": "jett", "repo": "site", "node": "jett",
         "reportedAt": 100},
    ]
    agent_sql = [sql for sql, _ in recorder.queries if "repo_agents" in sql]
    assert agent_sql and all(
        sql.startswith("SELECT repo_bi, agent_id, updated_at FROM repo_agents")
        for sql in agent_sql)
    assert "secret" not in json.dumps(data)


def test_desktop_agents_need_a_signed_in_get():
    assert _get(Recorder(token=ACCOUNT), method="POST")["status"] == 405
    refused = _get(Recorder())
    assert refused["status"] == 401
    assert refused["data"]["required_action"] == "sign_in"


def test_desktop_agents_read_is_chunked_and_bounded():
    repositories = {"bi:jett": [_repo("jett", "r%d" % n) for n in range(7)]}
    agents = [_agent("jett", "r%d" % n, n, 100 + n) for n in range(7)]
    recorder = Recorder(token=ACCOUNT, owners=["jett"],
                        repositories=repositories, agents=agents)
    response = _get(recorder, ACCOUNT_DESKTOP_AGENT_CHUNK=3,
                    ACCOUNT_DESKTOP_AGENT_MAX_ROWS=4)
    data = response["data"]
    assert data["truncated"] is True
    assert [item["id"] for item in data["agents"]] == ["6", "5", "4", "3"]
    agent_queries = [args for sql, args in recorder.queries if "repo_agents" in sql]
    assert [len(args) - 1 for args in agent_queries] == [3, 3, 1]
    assert all(args[-1] == 5 for args in agent_queries)


def test_desktop_agents_skip_rows_without_a_catalog_name():
    recorder = Recorder(
        token=ACCOUNT, owners=["jett"],
        repositories={"bi:jett": [{"key_bi": "bi:jett/gone", "data": None}]},
        agents=[_agent("jett", "gone", 1, 5)],
    )
    assert _get(recorder)["data"]["agents"] == []


NODE_HARNESS = r"""
import { readFileSync } from "node:fs";
const source = readFileSync(process.argv[2], "utf8");
const stubs = {
  $: () => null, $$: () => [], state: { session: null },
  escapeHtml: (v) => String(v ?? "").replace(/[&<>"']/g, (c) => `&#${c.charCodeAt(0)};`),
  formatTimeAgo: () => "now", parseFlexibleDate: (v) => new Date(Number(v)),
  formatCount: (v) => String(v), loadingHtml: (v) => v,
  orgAgentStatusDotClass: () => "bg-yellow-500", repoAgentStatusTone: () => "text-yellow-500",
  orgAgentRequest: async () => ({}),
};
const factory = new Function(...Object.keys(stubs),
  source + "\nreturn { agentsCloudRow, agentsDesktopRow, agentsFilteredRows, agentsRows, renderAgentsRow, agentsPageView };");
const api = factory(...Object.values(stubs));
const cloud = [
  { id: "a1", org: "jett", repo: "engine", scope: "owner", provider: "claude-code", status: "running",
    targetNode: "mirror9", title: "Fix the build", createdAt: 1, updatedAt: 50,
    agentInfo: { model: "claude-opus-5" }, displayStatus: "running" },
  { id: "o2", org: "acme", repo: "web", scope: "organization", provider: "codex", status: "completed",
    targetNode: "acme-runner", title: "", createdAt: 1, updatedAt: 90, displayStatus: "completed" },
  { id: "a3", org: "jett", repo: "engine", scope: "owner", provider: "codex", status: "queued",
    targetNode: "mirror9", title: "Stuck", createdAt: 1, updatedAt: 10, displayStatus: "attention",
    diagnostic: { message: "The selected node has not claimed this job." } },
];
api.agentsPageView.cloud = cloud.map(api.agentsCloudRow);
api.agentsPageView.desktop = [{ id: "4", owner: "jett-laptop", repo: "notes", node: "jett-laptop", reportedAt: 70 }]
  .map(api.agentsDesktopRow);
const ids = (rows) => rows.map((row) => row.key);
const filter = (status, repo, node) => {
  api.agentsPageView.filters = { status, repo, node };
  return ids(api.agentsFilteredRows());
};
const rows = api.agentsRows();
console.log(JSON.stringify({
  order: ids(rows),
  first: rows[0],
  org: api.agentsCloudRow(cloud[1]),
  attention: api.agentsCloudRow(cloud[2]),
  active: filter("active", "", ""),
  failed: filter("failed", "", ""),
  needs: filter("attention", "", ""),
  desktop: filter("desktop", "", ""),
  repo: filter("", "jett/engine", ""),
  node: filter("", "", "acme-runner"),
  both: filter("active", "jett/engine", "mirror9"),
  html: rows.map(api.renderAgentsRow),
}));
"""


@pytest.mark.skipif(shutil.which("node") is None, reason="Node.js is unavailable")
def test_rows_filters_and_links_in_the_page_module():
    completed = subprocess.run(
        ["node", "--input-type=module", "-", str(DASHBOARD / "js" / "09-agents.js")],
        input=NODE_HARNESS, capture_output=True, text=True, timeout=60, check=False)
    assert completed.returncode == 0, completed.stderr
    result = json.loads(completed.stdout)
    assert result["order"] == [
        "cloud:a1", "cloud:a3", "cloud:o2", "desktop:jett-laptop/notes:4"]
    first = result["first"]
    assert first["kind"] == "cloud" and first["live"] is True
    assert first["provider"] == "Claude Code" and first["model"] == "claude-opus-5"
    assert first["node"] == "mirror9"
    assert first["href"] == "/jett/engine/agents?session=a1"
    org = result["org"]
    assert org["kind"] == "organization" and org["title"] == "Codex session"
    assert org["href"] == "/acme/web/agents?session=o2"
    assert result["attention"]["badge"] == "attention"
    assert "not claimed" in result["attention"]["message"]
    assert result["active"] == ["cloud:a1", "cloud:a3"]
    assert result["failed"] == []
    assert result["needs"] == ["cloud:a3"]
    assert result["desktop"] == ["desktop:jett-laptop/notes:4"]
    assert result["repo"] == ["cloud:a1", "cloud:a3"]
    assert result["node"] == ["cloud:o2"]
    assert result["both"] == ["cloud:a1", "cloud:a3"]
    live, attention, done, desktop = result["html"]
    assert live.startswith("<a ") and 'href="/jett/engine/agents?session=a1"' in live
    assert "animate-pulse" in live and "Working" in live and "mirror9" in live
    assert "Needs attention" in attention and "text-amber-500" in attention
    assert "Completed" in done and "Organization run" in done
    assert desktop.startswith("<a ") and "End-to-end encrypted" in desktop
    assert 'href="/jett-laptop/notes/agents"' in desktop and "jett-laptop/notes" in desktop


def test_tailwind_sheet_carries_the_page_classes():
    sheet = (DASHBOARD / "tailwind.css").read_text(encoding="utf-8")
    for cls in ("bg-amber-500", "bg-emerald-500", "animate-pulse", "text-amber-500",
                "text-emerald-500", "bg-muted-foreground", "gap-x-3", "max-w-52"):
        assert re.search(r"\.%s[{:,]" % re.escape(cls), sheet), cls
