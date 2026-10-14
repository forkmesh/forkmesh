import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = ROOT.parent
PUBLIC = ROOT / "public"
COMPOSER = PUBLIC / "dashboard-composer.js"
COMPOSER_CSS = PUBLIC / "dashboard-composer.css"
HAND_PAGES = ("dashboard/tasks/index.html", "dashboard/notes/index.html", "dashboard/calendar/index.html")

if str(ROOT / "src") not in sys.path:
    sys.path.insert(0, str(ROOT / "src"))
if str(REPOSITORY / "world" / "src") not in sys.path:
    sys.path.insert(0, str(REPOSITORY / "world" / "src"))

import dashboard_shell  # noqa: E402


HARNESS = r'''
const vm = require("node:vm");
const fs = require("node:fs");
const noop = () => {};
const storage = new Map();
const dispatched = [];
const window = {
  addEventListener: noop,
  dispatchEvent: (event) => dispatched.push(event.type),
  matchMedia: () => ({ matches: true }),
  location: { origin: "https://app.forkmesh.com", pathname: "/" },
};
const context = {
  window,
  document: { readyState: "loading", addEventListener: noop, documentElement: { dataset: {} } },
  navigator: { platform: "Linux x86_64", language: "en-US" },
  localStorage: { getItem: (key) => storage.get(key) ?? null, setItem: (key, value) => storage.set(key, String(value)) },
  location: window.location,
  console,
  Event: class { constructor(type) { this.type = type; } },
};
window.window = window;
vm.createContext(context);
vm.runInContext(fs.readFileSync(process.argv[2], "utf8"), context);
const api = window.ForkMeshComposer._internals;
const session = { nodeName: "alice", nodes: ["laptop"] };
const out = {
  constants: {
    MAX_PROMPT: api.MAX_PROMPT,
    TASK_TITLE_MAX: api.TASK_TITLE_MAX,
    TASK_DETAILS_MAX: api.TASK_DETAILS_MAX,
    AGENT_ATTACHMENT_MAX_COUNT: api.AGENT_ATTACHMENT_MAX_COUNT,
    AGENT_ATTACHMENT_MAX_BYTES: api.AGENT_ATTACHMENT_MAX_BYTES,
    AGENT_ATTACHMENT_MAX_TOTAL_BYTES: api.AGENT_ATTACHMENT_MAX_TOTAL_BYTES,
    TASK_IMAGE_MAX_COUNT: api.TASK_IMAGE_MAX_COUNT,
    TASK_IMAGE_MAX_BYTES: api.TASK_IMAGE_MAX_BYTES,
    TASK_IMAGE_MAX_TOTAL_BYTES: api.TASK_IMAGE_MAX_TOTAL_BYTES,
  },
  catalog: api.MODEL_CATALOG,
  defaults: api.DEFAULT_MODEL,
  runModes: api.RUN_MODES,
  efforts: api.EFFORTS,
  paths: [
    "/alice/engine",
    "/alice/engine/issues/12",
    "/alice/engine/pulls/3",
    "/alice/engine/pull/4/files",
    "/alice/engine/issues/new",
    "/alice/engine/blob/main/README.md",
    "/@alice/profile",
    "/dashboard",
  ].map((path) => [path, api.parseRepoPath(path)]),
  tasks: [
    api.splitTaskText("Ship the composer\nWith tests\nand docs"),
    api.splitTaskText("x".repeat(200)),
    api.splitTaskText("  single line  "),
  ],
  slash: ["/apps", "/standup today", "/ nope", "text /apps", "/Clear"].map((text) => [text, api.slashCommandName(text)]),
  models: {
    auto: api.modelOptions("auto", null).map((option) => option.value),
    codex: api.modelOptions("codex", null).map((option) => option.value),
    gemma: api.modelOptions("local-llama", null).map((option) => option.value),
    restricted: api.modelOptions("auto", { "claude-code": ["sonnet", "opus"] }).map((option) => option.value),
  },
  targets: {
    auto: api.pickTarget([
      { node: "offline", providers: ["claude-code"], online: false },
      { node: "codex-only", providers: ["codex"], online: true },
    ], "auto")?.node,
    claude: api.pickTarget([
      { node: "offline", providers: ["claude-code"], online: false },
      { node: "codex-only", providers: ["codex"], online: true },
    ], "claude-code")?.node,
    none: api.pickTarget([{ node: "x", providers: [] }], "codex"),
    gemma: api.pickTarget([
      { node: "codex-only", providers: ["codex"], online: true },
      { node: "ollama", providers: ["codex", "local-llama"], online: false },
    ], "local-llama")?.node,
  },
  placement: (() => {
    const list = [
      { node: "laptop", providers: ["codex"], online: true, runtimeMode: "desktop" },
      { node: "fmnext-n1", providers: ["claude-code", "codex"], online: true, source: true, runtimeMode: "runner-pool" },
      { node: "box", providers: ["codex"], online: false, runtimeMode: "headless" },
      { node: "mirror", providers: ["codex"], online: true },
    ];
    return {
      desktop: api.eligibleTargets(list, "codex", false).map((target) => target.node),
      cloud: api.eligibleTargets(list, "codex", true).map((target) => target.node),
      any: api.eligibleTargets(list, "codex").map((target) => target.node),
      pickCloud: api.pickTarget(list, "auto", true)?.node,
      pickDesktop: api.pickTarget(list, "claude-code", false),
    };
  })(),
  ownerRoute: (() => {
    const endpoint = "/api/repo/forkmesh/forkmesh/agent-sessions";
    const org = {
      targets: [{ node: "laptop", providers: ["codex"], online: true }],
      sessions: [{ id: "a", status: "running" }],
      models: null,
      error: "",
    };
    const owner = {
      targets: [
        { node: "laptop", providers: ["codex"], online: true, runtimeMode: "desktop" },
        { node: "fmnext-r1", providers: ["codex"], online: true, runtimeMode: "runner-pool" },
      ],
      sessions: [{ id: "a", status: "running" }, { id: "b", status: "queued" }],
      models: { codex: ["sol"] },
      error: "",
    };
    const merged = api.withOwnerRoute(org, owner, endpoint);
    const failed = api.withOwnerRoute({ targets: [], sessions: [], models: null, error: "Forbidden." }, owner, endpoint);
    return {
      merged,
      cloud: api.pickTarget(merged.targets, "codex", true),
      desktop: api.pickTarget(merged.targets, "codex", false),
      failed: failed.targets.map((target) => target.node),
      failedError: failed.error,
      unchanged: api.withOwnerRoute(org, null, endpoint) === org,
    };
  })(),
  availability: (() => {
    const desktop = { node: "laptop", providers: ["claude-code"], online: true, runtimeMode: "desktop" };
    const runner = { node: "fmnext-r1", providers: ["claude-code"], online: true, runtimeMode: "runner-pool" };
    const repo = { owner: "alice", display: "alice/engine" };
    const place = (targets, mode, cloud, extra = {}) => api.agentPlacement({ targets, ...extra }, mode, cloud);
    return {
      pending: api.agentPlacement({ pending: true }, "auto", true),
      both: place([desktop, runner], "auto", false),
      bothCloud: place([desktop, runner], "auto", true),
      onlyRunner: place([runner], "auto", false),
      onlyDesktop: place([desktop], "auto", true),
      none: place([], "auto", true),
      wrongAgent: place([desktop], "codex", false),
      denied: place([], "auto", false, { error: "Not found.", errorStatus: 404 }),
      failed: place([], "auto", false, { error: "Request failed (HTTP 502).", errorStatus: 502 }),
      noticeReady: api.placementNotice(place([desktop], "auto", false), repo, "auto"),
      noticeNone: api.placementNotice(place([], "auto", false), repo, "auto"),
      noticeAgent: api.placementNotice(place([desktop], "codex", false), repo, "codex"),
      noticeDenied: api.placementNotice(place([], "auto", false, { error: "x", errorStatus: 403 }), repo, "auto"),
      noticeFailed: api.placementNotice(place([], "auto", false, { error: "x", errorStatus: 500 }), repo, "auto"),
    };
  })(),
  sessions: api.promptableSessions([
    { id: "old", status: "completed", localAgentId: 1, updatedAt: 1 },
    { id: "new", status: "running", localAgentId: 2, updatedAt: 5 },
    { id: "unclaimed", status: "queued", localAgentId: 0, updatedAt: 9 },
    { id: "rejected", status: "rejected", localAgentId: 3, updatedAt: 9 },
  ]).map((entry) => entry.id),
  usage: [
    api.contextUsage({ agentInfo: { contextTokens: 190000, contextWindow: 200000 } }),
    api.contextUsage({ agentInfo: { contextTokens: 0, contextWindow: 200000 } }),
  ],
  suffix: api.contextSuffix({ kind: "issue", number: 7, url: "https://app.forkmesh.com/alice/engine/issues/7", repo: { display: "alice/engine" } }),
  repoSuffix: api.contextSuffix({ kind: "repo" }),
  textLike: ["notes.md", "a.py", "photo.heic", "blob.bin", "Dockerfile"].map((name) => [name, api.isTextLike({ name, type: "" })]),
  owned: api.repoFromDisplay("laptop/engine", session),
  org: api.repoFromDisplay("acme/shared", session),
  ready: dispatched,
  pageHandler: (() => {
    let rejected = "";
    try { window.ForkMeshComposer.registerPageHandler({ id: "x" }); } catch (error) { rejected = error.message; }
    const unregister = window.ForkMeshComposer.registerPageHandler({ id: "calories", isActive: () => true, submit: async () => ({}) });
    return { rejected, unregister: typeof unregister };
  })(),
};
process.stdout.write(JSON.stringify(out));
'''


@pytest.fixture(scope="module")
def internals(tmp_path_factory):
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    harness = tmp_path_factory.mktemp("composer") / "harness.js"
    harness.write_text(HARNESS, encoding="utf-8")
    result = subprocess.run(
        [node, str(harness), str(COMPOSER)],
        check=True, capture_output=True, text=True, timeout=60)
    return json.loads(result.stdout)


def _built_pages():
    return [meta["asset"] for meta in dashboard_shell.PAGES.values()] + list(HAND_PAGES)


def test_every_dashboard_page_loads_the_composer_after_the_bundle():
    for relative in _built_pages():
        text = (PUBLIC / relative).read_text(encoding="utf-8")
        assert '<link rel="stylesheet" href="/dashboard-composer.css?v=${version}" />' in text, relative
        bundle = text.index('<script src="/dashboard.js?v=${version}" defer></script>')
        composer = text.index('<script src="/dashboard-composer.js?v=${version}" defer></script>')
        assert composer > bundle, relative


def test_composer_assets_are_cache_busted_and_served_by_mirrors():
    assert "dashboard-composer.js" in dashboard_shell.CACHE_BUSTED_ASSETS
    assert "dashboard-composer.css" in dashboard_shell.CACHE_BUSTED_ASSETS
    gateway = (REPOSITORY / "tools" / "mirror_gateway.py").read_text(encoding="utf-8")
    assert '"/dashboard-composer.js": "app/public/dashboard-composer.js"' in gateway
    assert '"/dashboard-composer.css": "app/public/dashboard-composer.css"' in gateway


def test_dashboard_bundle_exposes_the_composer_host_after_boot():
    bundle = (PUBLIC / "dashboard.js").read_text(encoding="utf-8")
    assert "window.ForkMeshComposerHost = Object.freeze({" in bundle
    boot = bundle[bundle.index("async function bootDashboard()"):]
    assert boot.index("initPageHistory();") < boot.index('new CustomEvent("forkmesh:composer-host")')
    for name in ("repoChoices", "loadRepoChoices", "selectedAgentId", "agentsChanged", "rememberRepo"):
        assert re.search(r"\b%s: " % name, bundle), name


def test_model_catalog_matches_the_server_allowlist(internals):
    entry = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
    start = entry.index("ORG_AGENT_MODEL_ALIASES = {")
    block = entry[start:entry.index("\n}\n", start)]
    server = {}
    provider = None
    for line in block.splitlines():
        head = re.match(r'\s+"([a-z-]+)": \{$', line)
        pair = re.match(r'\s+"([a-z0-9.:-]+)": "([a-z0-9.:-]+)",$', line)
        if head:
            provider = head.group(1)
            server[provider] = {}
        elif pair and provider:
            server[provider][pair.group(1)] = pair.group(2)
    catalog = internals["catalog"]
    assert set(catalog) == set(server)
    for provider, aliases in server.items():
        assert {option["value"]: option["id"] for option in catalog[provider]} == aliases
    assert catalog["claude-code"][0] == {
        "value": "opus", "label": "Opus 5.5", "id": "claude-opus-5-5",
        "icon": "model-opus-face",
    }
    assert internals["defaults"] == {"claude-code": "opus", "codex": "sol", "local-llama": "gemma4:e4b"}
    assert 'ORG_AGENT_GENERAL_PROVIDERS = ("agent", "bot", "auto")' in entry


def test_mode_and_speed_pickers_match_the_server_and_desktop(internals):
    handlers = (ROOT / "src" / "route_handlers.py").read_text(encoding="utf-8")
    modes = [option["value"] for option in internals["runModes"]]
    efforts = [option["value"] for option in internals["efforts"]]
    assert modes == ["auto", "ask", "plan", "edit"]
    assert [option["label"] for option in internals["runModes"]] == ["Auto", "Ask", "Plan", "Edit"]
    assert efforts == ["low", "medium", "high", "xhigh", "max"]
    assert [option["label"] for option in internals["efforts"]] == ["Low", "Medium", "High", "Ultra", "Max"]
    for option in internals["runModes"]:
        assert '"%s": "%s"' % (option["value"], option["label"]) in handlers
    assert "_ORG_AGENT_RUN_EFFORTS = (%s)" % ", ".join('"%s"' % value for value in efforts) in handlers
    icons = {option["icon"] for option in internals["runModes"] + internals["efforts"]}
    icons |= {option["icon"] for options in internals["catalog"].values() for option in options if option.get("icon")}
    for name in icons:
        asset = PUBLIC / "assets" / "agent-ui" / (name + ".webp")
        assert asset.is_file(), asset
        assert (REPOSITORY / "desktop" / "resources" / "agent-ui" / "icons" / (name + ".png")).is_file(), name


def test_limits_match_the_agent_and_task_servers(internals):
    import world_office_tasks

    entry = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
    handlers = (ROOT / "src" / "route_handlers.py").read_text(encoding="utf-8")
    constants = internals["constants"]
    assert "ORG_AGENT_MAX_PROMPT = %d\n" % constants["MAX_PROMPT"] in entry
    assert "ORG_AGENT_ATTACHMENT_MAX_COUNT = %d\n" % constants["AGENT_ATTACHMENT_MAX_COUNT"] in handlers
    assert constants["AGENT_ATTACHMENT_MAX_BYTES"] == 5 * 1024 * 1024
    assert "ORG_AGENT_ATTACHMENT_MAX_BYTES = 5 * 1024 * 1024\n" in handlers
    assert constants["AGENT_ATTACHMENT_MAX_TOTAL_BYTES"] == 10 * 1024 * 1024
    assert "ORG_AGENT_ATTACHMENT_MAX_TOTAL_BYTES = 10 * 1024 * 1024\n" in handlers
    assert constants["TASK_TITLE_MAX"] == world_office_tasks.MAX_TITLE
    assert constants["TASK_DETAILS_MAX"] == world_office_tasks.MAX_DETAILS
    assert constants["TASK_IMAGE_MAX_COUNT"] == world_office_tasks.MAX_TASK_IMAGES
    assert constants["TASK_IMAGE_MAX_BYTES"] == world_office_tasks.MAX_TASK_IMAGE_BYTES
    assert constants["TASK_IMAGE_MAX_TOTAL_BYTES"] == world_office_tasks.MAX_TASK_IMAGE_TOTAL_BYTES


def test_page_context_comes_from_the_repo_route(internals):
    paths = dict((path, value) for path, value in internals["paths"])
    assert paths["/alice/engine"] == {"owner": "alice", "name": "engine", "kind": "repo", "number": 0}
    assert paths["/alice/engine/issues/12"]["kind"] == "issue"
    assert paths["/alice/engine/issues/12"]["number"] == 12
    assert paths["/alice/engine/pulls/3"] == {"owner": "alice", "name": "engine", "kind": "pull", "number": 3}
    assert paths["/alice/engine/pull/4/files"]["number"] == 4
    assert paths["/alice/engine/issues/new"]["kind"] == "repo"
    assert paths["/alice/engine/blob/main/README.md"]["kind"] == "repo"
    assert paths["/@alice/profile"] is None
    assert paths["/dashboard"] is None
    assert internals["suffix"] == (
        "\n\nContext: Issue #7 in alice/engine "
        "(https://app.forkmesh.com/alice/engine/issues/7)")
    assert internals["repoSuffix"] == ""


def test_owned_repositories_use_owner_routes_and_others_org_routes(internals):
    assert internals["owned"]["endpoint"] == "/api/repo/laptop/engine/agent-sessions"
    assert internals["owned"]["owns"] is True
    assert internals["org"]["endpoint"] == "/api/orgs/acme/repos/shared/agent-bots"
    assert internals["org"]["owns"] is False


def test_org_alias_repositories_take_cloud_targets_from_the_owner_route(internals):
    route = internals["ownerRoute"]
    endpoint = "/api/repo/forkmesh/forkmesh/agent-sessions"
    assert route["cloud"]["node"] == "fmnext-r1"
    assert route["cloud"]["endpoint"] == endpoint
    assert route["desktop"] == {"node": "laptop", "providers": ["codex"], "online": True}
    assert [target["node"] for target in route["merged"]["targets"]] == ["laptop", "fmnext-r1"]
    assert route["merged"]["sessions"] == [
        {"id": "a", "status": "running"},
        {"id": "b", "status": "queued", "endpoint": endpoint},
    ]
    assert route["merged"]["models"] == {"codex": ["sol"]}
    assert route["failed"] == ["laptop", "fmnext-r1"]
    assert route["failedError"] == ""
    assert route["unchanged"] is True


def test_task_title_is_the_first_line(internals):
    first, long_line, single = internals["tasks"]
    assert first == {"title": "Ship the composer", "details": "With tests\nand docs"}
    assert len(long_line["title"]) == 160
    assert long_line["title"].endswith("…")
    assert long_line["details"] == "x" * 200
    assert single == {"title": "single line", "details": ""}


def test_slash_commands_models_targets_and_sessions(internals):
    assert dict(internals["slash"]) == {
        "/apps": "apps", "/standup today": "standup", "/ nope": "",
        "text /apps": "", "/Clear": "clear",
    }
    assert internals["models"]["auto"] == ["opus", "opus-5", "sonnet", "haiku", "fable"]
    assert internals["models"]["codex"] == ["sol", "terra", "luna", "spark"]
    assert internals["models"]["gemma"] == ["gemma4:e4b", "gemma4:e2b", "gemma4:12b", "gemma4:26b", "gemma4:31b"]
    assert internals["models"]["restricted"] == ["opus", "sonnet"]
    assert internals["targets"]["auto"] == "codex-only"
    assert internals["targets"]["claude"] == "offline"
    assert internals["targets"]["none"] is None
    assert internals["targets"]["gemma"] == "ollama"
    assert internals["sessions"] == ["new", "old"]
    assert internals["placement"] == {
        "desktop": ["laptop", "mirror"],
        "cloud": ["fmnext-n1", "box"],
        "any": ["laptop", "fmnext-n1", "box", "mirror"],
        "pickCloud": "fmnext-n1",
        "pickDesktop": None,
    }
    high, empty = internals["usage"]
    assert high["bars"] == 5 and round(high["ratio"], 2) == 0.95
    assert empty is None
    assert dict(internals["textLike"]) == {
        "notes.md": True, "a.py": True, "photo.heic": False,
        "blob.bin": False, "Dockerfile": True,
    }


def test_composer_source_has_no_comments():
    for path in (COMPOSER, COMPOSER_CSS):
        text = path.read_text(encoding="utf-8")
        assert "/*" not in text.replace("*/", ""), path
        assert not re.search(r"(^|\s)//\s", text, re.M), path


def test_pages_can_take_over_enter_once_the_composer_says_it_is_ready(internals):
    assert internals["ready"] == ["forkmesh:composer-ready"]
    assert internals["pageHandler"] == {"rejected": "invalid_composer_page_handler", "unregister": "function"}
    source = COMPOSER.read_text(encoding="utf-8")
    assert 'if (action === "page") void runPageAction();' in source


def test_agent_placement_follows_what_can_actually_run(internals):
    cases = internals["availability"]
    assert cases["pending"] is None
    assert cases["both"]["cloud"] is False and cases["both"]["ready"] is True
    assert cases["bothCloud"]["cloud"] is True
    assert cases["onlyRunner"]["cloud"] is True
    assert cases["onlyRunner"]["canDesktop"] is False
    assert cases["onlyDesktop"]["cloud"] is False
    assert cases["onlyDesktop"]["canCloud"] is False
    assert cases["none"]["ready"] is False and cases["none"]["otherAgent"] is False
    assert cases["wrongAgent"]["ready"] is False and cases["wrongAgent"]["otherAgent"] is True
    assert cases["denied"]["denied"] is True
    assert cases["failed"]["denied"] is False and cases["failed"]["failed"] is True


def test_placement_notice_links_to_runner_and_desktop_setup(internals):
    cases = internals["availability"]
    assert cases["noticeReady"] is None
    none = cases["noticeNone"]
    assert "alice/engine" in none["text"]
    assert [link["href"] for link in none["links"]] == ["/dashboard/runners?setup=1", "/desktop"]
    assert "Codex" in cases["noticeAgent"]["text"] and cases["noticeAgent"]["links"] == []
    assert cases["noticeDenied"]["text"].startswith("Only alice can start agents")
    assert "could not be loaded" in cases["noticeFailed"]["text"]
