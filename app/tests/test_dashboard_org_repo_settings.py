import json
import subprocess
from pathlib import Path

from _dashboard_bundle import assembled_dashboard_js


ROOT = Path(__file__).resolve().parents[1]
JS = ROOT / "public" / "dashboard" / "js"
HELPERS = (JS / "02-helpers.js").read_text(encoding="utf-8")
CONTENT = (JS / "06-repo-content.js").read_text(encoding="utf-8")
DETAIL = (JS / "08-repo-detail-network.js").read_text(encoding="utf-8")
DASHBOARD_JS = assembled_dashboard_js()
BOOT = "  void bootDashboard();"
BROWSER_STUBS = """
const assert = require("assert");
const vm = require("vm");
global.location = {
  origin: "https://code.example.org",
  hostname: "code.example.org",
  pathname: "/dashboard",
  search: "",
};
global.localStorage = { getItem() { return null; }, setItem() {}, removeItem() {} };
const emptyClassList = { add() {}, remove() {}, toggle() {}, contains() { return false; } };
global.document = {
  body: { classList: emptyClassList },
  documentElement: { dataset: {}, style: {} },
  cookie: "",
  querySelector() { return null; },
  querySelectorAll() { return []; },
  addEventListener() {},
};
global.window = {
  matchMedia() { return { addEventListener() {}, addListener() {} }; },
  addEventListener() {},
  history: { pushState() {} },
  lucide: { createIcons() {} },
  setTimeout() {},
  setInterval() {},
  clearInterval() {},
};
global.navigator = {};
global.fetchCalls = [];
global.fetchAnswers = {};
global.fetch = async (url, init = {}) => {
  global.fetchCalls.push({ url, headers: init.headers || {}, method: init.method || "GET" });
  const answer = global.fetchAnswers[url];
  if (answer === "fail") throw new Error("offline");
  return { ok: Boolean(answer), status: answer ? 200 : 404, json: async () => answer || {} };
};
vm.runInThisContext(SOURCE);
"""
EXPORTS = (
    "sessionAdministersRepo, sessionOwnsRepo, sessionCanAssignAgent, "
    "loadRepoViewerPermission, repoTabRoutesFor, repoLinkUrl, repoApiBase, "
    "normalizeRepoAddresses, state"
)
ORG_REPO = """
const orgRepo = {
  owner: "acme", name: "site", ownerKind: "organization",
  logicalOwner: "acme",
  logicalOwners: [{ kind: "organization", owner: "acme" }, { kind: "user", owner: "alice" }],
  canonicalOwner: "acme", canonicalName: "site",
  servingOwner: "alice", servingName: "site",
};
"""


def _run(body):
    transformed = DASHBOARD_JS.replace(
        BOOT, "  globalThis.__dashboardExports = { " + EXPORTS + " };")
    assert transformed != DASHBOARD_JS
    script = ("const SOURCE = " + json.dumps(transformed) + ";\n"
              + BROWSER_STUBS
              + "const {" + EXPORTS + "} = global.__dashboardExports;\n"
              + ORG_REPO + "(async () => {\n" + body
              + "\n})().catch((error) => { console.error(error); process.exit(1); });\n")
    result = subprocess.run(["node"], input=script, capture_output=True,
                            text=True)
    assert result.returncode == 0, result.stderr
    assert result.stderr == ""
    return json.loads(result.stdout)


def test_an_org_owner_gets_settings_from_the_server_permission():
    output = _run("""
state.session = { nodeName: "carol", sessionToken: "tok-carol" };
global.fetchAnswers["/api/repo/acme/site/viewer-permission"] = {
  ok: true, viewer: "carol", permission: "admin", viewerCanAdmin: true,
};
const before = {
  owns: sessionOwnsRepo(orgRepo),
  admin: sessionAdministersRepo(orgRepo),
  settings: repoTabRoutesFor(orgRepo).includes("settings"),
};
const answer = await loadRepoViewerPermission("acme/site");
await loadRepoViewerPermission(orgRepo);
console.log(JSON.stringify({
  before,
  answer,
  admin: sessionAdministersRepo(orgRepo),
  settings: repoTabRoutesFor(orgRepo).includes("settings"),
  agents: sessionCanAssignAgent(orgRepo),
  calls: global.fetchCalls,
}));
""")
    assert output["before"] == {"owns": False, "admin": False,
                                "settings": False}
    assert output["answer"] == {"permission": "admin", "viewerCanAdmin": True}
    assert output["admin"] is True
    assert output["settings"] is True
    assert output["agents"] is True
    assert output["calls"] == [{
        "url": "/api/repo/acme/site/viewer-permission",
        "headers": {"accept": "application/json",
                    "authorization": "Bearer tok-carol"},
        "method": "GET",
    }]


def test_members_and_outsiders_do_not_get_settings():
    output = _run("""
const seen = {};
for (const [viewer, permission] of [["max", "maintain"], ["mia", "read"], ["mallory", ""]]) {
  state.session = { nodeName: viewer, sessionToken: "cookie" };
  global.fetchAnswers["/api/repo/acme/site/viewer-permission"] = {
    ok: true, viewer, permission, viewerCanAdmin: false,
  };
  await loadRepoViewerPermission(orgRepo);
  seen[viewer] = {
    admin: sessionAdministersRepo(orgRepo),
    settings: repoTabRoutesFor(orgRepo).includes("settings"),
  };
}
console.log(JSON.stringify({
  seen,
  cookieHeaders: global.fetchCalls.map((call) => call.headers),
}));
""")
    for viewer in ("max", "mia", "mallory"):
        assert output["seen"][viewer] == {"admin": False, "settings": False}
    assert all("authorization" not in headers
               for headers in output["cookieHeaders"])


def test_a_failed_lookup_grants_nothing_and_is_retried():
    output = _run("""
state.session = { nodeName: "carol", sessionToken: "tok" };
global.fetchAnswers["/api/repo/acme/site/viewer-permission"] = "fail";
const first = await loadRepoViewerPermission(orgRepo);
const adminAfterFailure = sessionAdministersRepo(orgRepo);
global.fetchAnswers["/api/repo/acme/site/viewer-permission"] = {
  ok: true, permission: "admin", viewerCanAdmin: true,
};
await loadRepoViewerPermission(orgRepo);
console.log(JSON.stringify({
  first, adminAfterFailure,
  admin: sessionAdministersRepo(orgRepo),
  calls: global.fetchCalls.length,
}));
""")
    assert output["first"] is None
    assert output["adminAfterFailure"] is False
    assert output["admin"] is True
    assert output["calls"] == 2


def test_the_creator_node_and_a_signed_out_visitor_need_no_lookup():
    output = _run("""
const personal = { owner: "alice", name: "notes" };
state.session = { nodeName: "alice", sessionToken: "tok" };
const creator = {
  personal: sessionAdministersRepo(personal),
  lookup: await loadRepoViewerPermission(personal),
};
state.session = null;
const anonymous = {
  admin: sessionAdministersRepo(orgRepo),
  lookup: await loadRepoViewerPermission(orgRepo),
};
console.log(JSON.stringify({ creator, anonymous, calls: global.fetchCalls.length }));
""")
    assert output["creator"] == {"personal": True, "lookup": None}
    assert output["anonymous"] == {"admin": False, "lookup": None}
    assert output["calls"] == 0


def test_a_permission_is_scoped_to_the_signed_in_account():
    output = _run("""
state.session = { nodeName: "carol", sessionToken: "tok" };
global.fetchAnswers["/api/repo/acme/site/viewer-permission"] = {
  ok: true, permission: "admin", viewerCanAdmin: true,
};
await loadRepoViewerPermission(orgRepo);
state.session = { nodeName: "mallory", sessionToken: "tok2" };
console.log(JSON.stringify({ admin: sessionAdministersRepo(orgRepo) }));
""")
    assert output == {"admin": False}


def test_settings_links_use_the_short_org_address_in_canonical_mode():
    output = _run("""
state.repoAddresses = normalizeRepoAddresses({ owner: "acme", canonical: true });
console.log(JSON.stringify({
  settings: `${repoLinkUrl(orgRepo)}/settings`,
  api: repoApiBase(orgRepo),
}));
""")
    assert output == {"settings": "/site/settings",
                      "api": "/api/repo/acme/site"}


def test_every_settings_surface_uses_the_admin_rule():
    assert "const canEditAbout = sessionAdministersRepo(repo);" in DETAIL
    assert "const canSeeSettingsTab = canEditAbout;" in DETAIL
    assert ('if (repo && sessionAdministersRepo(repo)) routes.push("settings");'
            in HELPERS)
    assert ('(routeParts[2] === "settings" && !sessionAdministersRepo(repo))'
            in DETAIL)
    for gate in (
            "if (!target || !sessionAdministersRepo(repo) || repo.isPrivate) return;",
            "if (!list || !sessionAdministersRepo(repo)) return;",
            "if (!holder || !sessionAdministersRepo(repo)) return;",
            "if (!card || !repo || !sessionAdministersRepo(repo)) return false;",
            "if (!repo || !sessionAdministersRepo(repo)) return true;",
            "editable: sessionAdministersRepo(repo),",
            "const mayReview = sessionAdministersRepo(repo) || Boolean(state.session?.isAdmin);",
    ):
        assert gate in CONTENT, gate
    click = DETAIL[DETAIL.index('const aboutEditButton = event.target.closest("[data-repo-about-edit]");'):]
    click = click[:click.index('const logoRemove')]
    assert "sessionOwnsRepo" not in click
    assert click.count("sessionAdministersRepo(state.selectedRepo)") == 4
    init = DETAIL[DETAIL.index("async function initRepoPage"):]
    init = init[:init.index("renderRepoDetail(repo);")]
    assert init.index("loadRepoViewerPermission(requested)") < init.index(
        "await (repositoriesReady || loadRepositories());")
    assert "await (viewerPermissionReady || Promise.resolve());" in init
