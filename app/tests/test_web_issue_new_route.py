#!/usr/bin/env python3

import json
import shutil
import subprocess
from pathlib import Path

import pytest


PUBLIC = Path(__file__).resolve().parents[1] / "public"
COMPOSE = (PUBLIC / "dashboard" / "js" / "07-repo-compose-branch.js").read_text(encoding="utf-8")
DETAIL = (PUBLIC / "dashboard" / "js" / "08-repo-detail-network.js").read_text(encoding="utf-8")
MICROJOBS = (PUBLIC / "dashboard" / "js" / "09-microjobs.js").read_text(encoding="utf-8")
LOGIN = (PUBLIC / "login.js").read_text(encoding="utf-8")


def _region(source, start, end):
    offset = source.index(start)
    return source[offset:source.index(end, offset)]


ROUTE_HELPERS = _region(COMPOSE, "  function issueComposeRouteActive(repo)", "  function openIssueCompose(")
FEATURE_PANELS = _region(COMPOSE, "  function loadRepoFeaturePanels(", "  function updateRepoLiveCounts(")
LEAVE_IF_SIGNED_IN = _region(LOGIN, "  async function leaveIfSignedIn()", "  leaveIfSignedIn().catch")


DASHBOARD_HARNESS = r'''
const calls = [];
let path = "";
let formOpen = false;
const state = { session: null, issuesView: { items: [] } };
const repoRouteParts = () => path.split("/").filter(Boolean);
const repoLinkUrl = (repo) => `/${repo.owner}/${repo.name}`;
const form = { remove: () => { formOpen = false; calls.push("removeForm"); } };
const $ = (selector) => selector === "[data-repo-issues] [data-repo-issue-form]" && formOpen ? form : null;
const record = (name) => (...args) => calls.push(name);
const loadRepoMirrors = () => {};
const loadRepoRecordDetail = record("loadRepoRecordDetail");
const loadRepoIssues = record("loadRepoIssues");
const renderRepoIssues = record("renderRepoIssues");
const openIssueCompose = () => { formOpen = true; calls.push("openIssueCompose"); };

__HELPERS__
__PANELS__

const repo = { owner: "forkmesh", name: "forkmesh" };
function run(route, session, open = false) {
  path = route;
  state.session = session;
  state.activeRepoTab = "issues";
  state.issuesView = { items: [] };
  formOpen = open;
  calls.length = 0;
  loadRepoFeaturePanels(repo);
  return [...calls];
}
const signedIn = { nodeName: "alice" };
const out = {
  url: issueComposeUrl(repo),
  newSignedIn: run("/forkmesh/forkmesh/issues/new", signedIn),
  newSignedOut: run("/forkmesh/forkmesh/issues/new", null),
  list: run("/forkmesh/forkmesh/issues", signedIn),
  newWithDraft: run("/forkmesh/forkmesh/issues/new", signedIn, true),
};
formOpen = true;
state.issuesView = { items: [] };
calls.length = 0;
showRepoIssueList(repo);
out.closeUnloaded = [...calls];
formOpen = true;
state.issuesView = { items: [], repo };
calls.length = 0;
showRepoIssueList(repo);
out.closeLoaded = [...calls];
process.stdout.write(JSON.stringify(out));
'''


LOGIN_HARNESS = r'''
const results = {};
async function scenario(name, search, stored, status) {
  const replaced = [];
  const fetched = [];
  globalThis.location = { search, replace: (url) => replaced.push(url) };
  globalThis.localStorage = { getItem: () => stored === undefined ? null : JSON.stringify(stored) };
  globalThis.fetch = async (url, init) => { fetched.push({ url, auth: init.headers.authorization || "" }); return { ok: status === 200, status }; };
  __LEAVE__
  await leaveIfSignedIn();
  results[name] = { replaced, fetched };
}
await scenario("signedIn", "", { nodeName: "alice", sessionToken: "cookie" }, 200);
await scenario("bearer", "", { nodeName: "alice", sessionToken: "tok" }, 200);
await scenario("stale", "", { nodeName: "alice", sessionToken: "cookie" }, 401);
await scenario("guest", "", undefined, 200);
await scenario("withNext", "?next=%2Fadmin", { nodeName: "alice", sessionToken: "cookie" }, 200);
process.stdout.write(JSON.stringify(results));
'''


pytestmark = pytest.mark.skipif(shutil.which("node") is None, reason="Node.js is unavailable")


def _node(source):
    completed = subprocess.run(
        ["node", "--input-type=module", "-"],
        input=source,
        text=True,
        capture_output=True,
        check=True,
    )
    return json.loads(completed.stdout)


def test_microjobs_issue_link_targets_the_new_issue_route():
    assert '"/forkmesh/forkmesh/issues/new"' in MICROJOBS


def test_issues_new_route_opens_the_compose_form_for_a_signed_in_visitor():
    result = _node(DASHBOARD_HARNESS.replace("__HELPERS__", ROUTE_HELPERS).replace("__PANELS__", FEATURE_PANELS))
    assert result["url"] == "/forkmesh/forkmesh/issues/new"
    assert result["newSignedIn"] == ["openIssueCompose"]
    assert result["newSignedOut"] == ["loadRepoIssues"]
    assert result["list"] == ["loadRepoIssues"]
    assert result["newWithDraft"] == ["loadRepoIssues"]
    assert result["closeUnloaded"] == ["removeForm", "loadRepoIssues"]
    assert result["closeLoaded"] == ["renderRepoIssues"]


def test_repo_route_keeps_issues_new_in_the_address_bar():
    assert '''routeKind === "issues" && routeParts[3] === "new" && state.session?.nodeName
            ? issueComposeUrl(repo)''' in DETAIL
    assert 'kind === "issues" && issueComposeRouteActive(repo)' in DETAIL


def test_signed_in_login_page_goes_to_the_dashboard():
    result = _node(LOGIN_HARNESS.replace("__LEAVE__", LEAVE_IF_SIGNED_IN))
    assert result["signedIn"]["replaced"] == ["/dashboard"]
    assert result["signedIn"]["fetched"] == [{"url": "/api/accounts/sessions", "auth": ""}]
    assert result["bearer"]["fetched"][0]["auth"] == "Bearer tok"
    assert result["stale"]["replaced"] == []
    assert result["guest"] == {"replaced": [], "fetched": []}
    assert result["withNext"] == {"replaced": [], "fetched": []}
