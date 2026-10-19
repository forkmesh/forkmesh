import asyncio
import json
import sys
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import quote, unquote, urlparse

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

import app_runtime  # noqa: E402
import repo_short_urls  # noqa: E402
import static_routes  # noqa: E402

RUNTIME = (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
ENTRY = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
TERMS = json.loads((ROOT.parent / "www" / "docs" / "terminology.json")
                   .read_text(encoding="utf-8"))


@pytest.fixture(autouse=True)
def fresh_cache():
    repo_short_urls.forget()
    yield
    repo_short_urls.forget()


@pytest.mark.parametrize("path,query,target", [
    ("/dashboard/profile/alice", "", "/@alice"),
    ("/dashboard/profile/Alice/", "", "/@alice"),
    ("/profile/alice", "tab=x", "/@alice?tab=x"),
    ("/profile/%40alice", "", "/@alice"),
    ("/profile/alice/repositories", "", "/@alice/repositories"),
    ("/dashboard/profile/acme-org/repositories", "", "/@acme-org/repositories"),
])
def test_profile_paths_redirect_to_the_at_profile(path, query, target):
    assert static_routes.profile_page_redirect(path, query) == target


@pytest.mark.parametrize("path", [
    "/dashboard/profile",
    "/dashboard/profile/repositories",
    "/profile",
    "/profile/alice/extra",
    "/profile/-bad",
    "/profile/a.b",
    "/acme/profile/alice",
])
def test_other_profile_shaped_paths_are_left_alone(path):
    assert static_routes.profile_page_redirect(path, "") is None


def test_dashboard_pages_never_bounce_to_a_repository_path():
    redirect = static_routes.dashboard_repo_redirect
    assert redirect("/dashboard/alice/widgets") == "/alice/widgets"
    assert redirect("/dashboard/alice/widgets/issues") == "/alice/widgets/issues"
    for path in ("/dashboard/profile/alice", "/dashboard/repos/widgets",
                 "/dashboard/settings/alice", "/dashboard/agents/x",
                 "/dashboard/runners/x"):
        assert redirect(path) is None, path
    assert "profile" in static_routes.RESERVED_ROUTE_PREFIXES
    assert not static_routes.looks_like_repo_route("/profile/alice")


def test_forge_page_names_redirect_and_keep_the_query():
    page = static_routes.legacy_page_redirect
    assert page("/explore", "") == "/dashboard/repos"
    assert page("/explore/", "q=x") == "/dashboard/repos?q=x"
    assert page("/forgot", "") == "/forgot-password"
    assert page("/forgot/", "") == "/forgot-password"
    aliases = {(a["alias"], a["canonical"]) for a in TERMS["compatibilityAliases"]}
    assert ("/explore", "/dashboard/repos") in aliases
    assert ("/forgot", "/forgot-password") in aliases
    assert ("/OWNER/REPOSITORY/commit/NUMBER",
            "/OWNER/REPOSITORY/commits/NUMBER") in aliases


def test_commit_urls_are_an_alias_of_the_commits_route():
    redirect = static_routes.repo_alias_redirect
    sha = "0123456789abcdef0123456789abcdef01234567"
    assert redirect("/acme/widgets/commit/" + sha, "") == (
        "/acme/widgets/commits/" + sha)
    assert static_routes.looks_like_repo_route("/acme/widgets/commits/" + sha)
    assert repo_short_urls.alias_redirect(
        "/widgets/commit/" + sha, "x=1", "acme") == (
        "/widgets/commits/" + sha + "?x=1")
    assert repo_short_urls.alias_redirect(
        "/widgets/commits/" + sha, "", "acme") is None
    assert repo_short_urls.alias_redirect(
        "/widgets/tree/commit", "", "acme") is None
    assert repo_short_urls.alias_redirect("/api/commit/x", "", "acme") is None
    assert repo_short_urls.rewrite("/widgets/commits/" + sha, "acme") == (
        "/acme/widgets/commits/" + sha, "widgets")


@pytest.mark.parametrize("name", [
    "profile", "explore", "forgot", "repos", "settings", "agents", "runners",
    "dashboard", "api", "login",
])
def test_dashboard_and_forge_names_are_never_captured_as_short_repositories(name):
    assert repo_short_urls.rewrite("/" + name, "acme") is None
    assert repo_short_urls.rewrite("/" + name + "/alice", "acme") is None
    assert not repo_short_urls.name_can_be_short(name, "acme")
    doc = repo_short_urls.client_doc(
        {"short": True, "owner": "acme", "canonical": True})
    assert name in doc["reserved"]


def test_single_segment_account_page_names():
    name = static_routes.account_page_name
    assert name("/alice") == "alice"
    assert name("/Acme-Org/") == "acme-org"
    for path in ("/", "/alice/widgets", "/dashboard", "/explore", "/a.b",
                 "/profile", "/-x"):
        assert name(path) == "", path


def _runtime(monkeypatch, orgs=(), accounts=None):
    accounts = accounts or {}

    async def org_row(_env, name):
        return "bi", ({"name": name} if name in orgs else None)

    async def account_row(_env, name):
        return "bi", accounts.get(name)

    for attr, value in {
            "_org_row": org_row, "_account_row": account_row,
            "_account_kind": lambda rec: rec.get("kind", "user"),
            "account_page_name": static_routes.account_page_name,
            "quote": quote}.items():
        monkeypatch.setattr(app_runtime, attr, value, raising=False)
    return SimpleNamespace(env=None)


def test_plain_account_and_organization_paths_redirect_to_their_profile(
        monkeypatch):
    me = _runtime(monkeypatch, orgs={"acme"}, accounts={
        "alice": {"status": "active"},
        "gone": {"status": "deleted"},
        "acme-node": {"status": "active", "kind": "node"}})
    redirect = app_runtime.Default._account_page_redirect

    def run(path):
        return asyncio.run(redirect(me, urlparse("https://code.example.org"
                                                 + path)))

    assert run("/acme") == "/@acme"
    assert run("/alice") == "/@alice"
    assert run("/Alice?tab=repositories") == "/@alice?tab=repositories"
    assert run("/gone") is None
    assert run("/acme-node") is None
    assert run("/nobody") is None
    assert run("/alice/widgets") is None


def test_unknown_browser_navigations_get_the_styled_404_page():
    route = RUNTIME.split("    async def _route(self, request, url):", 1)[1]
    route = route.split("\n    async def _account_page_redirect", 1)[0]
    tail = route.rsplit("return await self._serve_not_found_page(url)", 2)
    assert "account_target = await self._account_page_redirect(url)" in tail[1]
    assert '_is_page_navigation(request)' in tail[1]
    assert route.rstrip().endswith(
        'return json_response({"error": "not_found"}, status=404)')
    assert route.index("profile_page_redirect(url.path, url.query)") < \
        route.index("short_route = await self._repo_short_route(url)")
    assert 'profile_page_redirect = _static_routes.export(' in ENTRY
    assert 'account_page_name = _static_routes.export(' in ENTRY


class _Assets:
    async def fetch(self, _url):
        class _Resp:
            async def text(self):
                return "<html><head></head><body>shell</body></html>"
        return _Resp()


def _repo_page(monkeypatch, visible):
    calls = []

    async def access(env, request, owner, repo):
        calls.append((owner, repo))
        return {"owner": owner} if visible else None

    async def false(*_args):
        return False

    async def private(*_args):
        return True

    async def node(_env, owner, repo):
        return "alice"

    async def setting():
        return {"short": True, "owner": "acme", "canonical": True}

    class _Response:
        def __init__(self, body, status=200, headers=None):
            self.body, self.status, self.headers = body, status, headers

    for attr, value in {
            "_repository_access_context": access,
            "_repo_is_private": private, "_org_repo_node": node,
            "_ap_origin": lambda env: "https://code.example.org",
            "Response": _Response, "quote": quote, "unquote": unquote,
            "safe_segment": lambda value: value,
            "_html_escape": lambda value: value,
            "repo_short_urls": repo_short_urls,
            "DASHBOARD_REPO_ASSET": "dashboard/repo.html",
            "edge_cache_put": false}.items():
        monkeypatch.setattr(app_runtime, attr, value, raising=False)
    me = SimpleNamespace(env=SimpleNamespace(ASSETS=_Assets()),
                         _repo_short_setting=setting)
    url = urlparse("https://code.example.org/acme/nothing")
    page = asyncio.run(app_runtime.Default._serve_repo_page(
        me, SimpleNamespace(headers={}), url, "nothing"))
    return page, calls


def test_a_short_address_without_a_visible_repository_answers_404(monkeypatch):
    page, calls = _repo_page(monkeypatch, visible=False)
    assert page.status == 404
    assert "shell" in page.body
    assert calls == [("alice", "nothing")]


def test_a_private_repository_its_reader_may_see_keeps_200(monkeypatch):
    page, calls = _repo_page(monkeypatch, visible=True)
    assert page.status == 200
    assert calls == [("alice", "nothing")]


def test_the_repository_page_opens_and_addresses_a_commit_by_its_url():
    js = ROOT / "public" / "dashboard" / "js"
    helpers = (js / "02-helpers.js").read_text(encoding="utf-8")
    compose = (js / "07-repo-compose-branch.js").read_text(encoding="utf-8")
    detail = (js / "08-repo-detail-network.js").read_text(encoding="utf-8")
    bundle = (ROOT / "public" / "dashboard.js").read_text(encoding="utf-8")
    assert "function repoCommitUrl(repo, hash)" in helpers
    assert '/commits/${encodeURIComponent(String(hash || ""))}' in helpers
    assert 'routeKind === "commits" && /^[0-9a-f]{7,64}$/i.test(recordNumber)' in detail
    assert "loadRepoFeaturePanels(repo, recordRoute, compareRoute, commitHash)" in detail
    assert "navigateHistory(repoCommitUrl(state.selectedRepo, commitHash))" in detail
    assert "if (commitHash) loadRepoCommitDetail(repo, commitHash);" in compose
    assert "function repoCommitUrl(repo, hash)" in bundle
