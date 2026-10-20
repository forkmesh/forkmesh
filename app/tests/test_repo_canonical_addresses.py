import ast
import asyncio
import json
import re
import subprocess
import sys
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import parse_qs, urlencode, urlparse

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
sys.path.insert(0, str(ROOT / "host"))

import admin_shell  # noqa: E402
import app_runtime  # noqa: E402
import hosted_git  # noqa: E402
import repo_projects  # noqa: E402
import repo_short_urls  # noqa: E402
import server_update_admin as admin  # noqa: E402
from forkmesh_host import request_guard  # noqa: E402

ENTRY_PATH = ROOT / "src" / "entry.py"
ENTRY = ENTRY_PATH.read_text(encoding="utf-8")
RUNTIME = (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
HELPERS = (ROOT / "public" / "dashboard" / "js" / "02-helpers.js").read_text(
    encoding="utf-8")
BOOT = (ROOT / "public" / "dashboard" / "js" / "10-boot.js").read_text(
    encoding="utf-8")
JS_DIR = ROOT / "public" / "dashboard" / "js"
NOW = 1_800_000_000_000
ON = {"short": True, "owner": "acme", "canonical": True}
SHORT_ONLY = {"short": True, "owner": "acme", "canonical": False}


@pytest.fixture(autouse=True)
def fresh_cache():
    repo_short_urls.forget()
    yield
    repo_short_urls.forget()


def test_the_setting_carries_the_canonical_switch_only_with_short_on():
    assert repo_short_urls.parse("") == {
        "short": False, "owner": "", "canonical": False}
    assert repo_short_urls.parse(
        '{"short":true,"owner":"Acme","canonical":true}') == ON
    assert repo_short_urls.parse(
        '{"short":false,"owner":"acme","canonical":true}')["canonical"] is False
    assert repo_short_urls.parse('{"short":true,"owner":"acme"}') == SHORT_ONLY
    assert repo_short_urls.clean(True, "@Acme", True) == ON
    assert repo_short_urls.clean(False, "acme", True)["canonical"] is False
    assert repo_short_urls.dump(ON) == (
        '{"canonical":true,"owner":"acme","short":true}')
    assert repo_short_urls.parse(repo_short_urls.dump(ON)) == ON


@pytest.mark.parametrize("path,query,target", [
    ("/acme/Bolt", "", "/Bolt"),
    ("/Acme/Bolt/", "", "/Bolt"),
    ("/acme/Bolt/issues", "", "/Bolt/issues"),
    ("/acme/Bolt/issues/4", "label=bug", "/Bolt/issues/4?label=bug"),
    ("/acme/Bolt/tree/bolt-c", "", "/Bolt/tree/bolt-c"),
    ("/acme/Bolt/blob/main/README.md", "ref=main",
     "/Bolt/blob/main/README.md?ref=main"),
    ("/acme/Bolt/compare/main...dev", "", "/Bolt/compare/main...dev"),
    ("/acme/Bolt/pulls/3/files", "", "/Bolt/pulls/3/files"),
])
def test_full_page_addresses_redirect_to_the_short_form(path, query, target):
    assert repo_short_urls.canonical_redirect(path, query, ON)[0] == target


@pytest.mark.parametrize("path", [
    "/acme/Bolt/info/refs",
    "/acme/Bolt.git/info/refs",
    "/acme/Bolt/git-upload-pack",
    "/acme/Bolt/git-receive-pack",
    "/acme/Bolt.git",
    "/acme/docs",
    "/acme/api",
    "/acme/dashboard/repos",
    "/acme/acme",
    "/acme/robots.txt",
    "/acme/Bolt/src",
    "/other/Bolt",
    "/acme",
    "/api/repo/acme/Bolt/tree",
    "/acme/%5Cevil.com",
])
def test_git_api_reserved_and_other_owner_paths_are_never_redirected(path):
    assert repo_short_urls.canonical_redirect(path, "", ON) is None


def test_nothing_redirects_unless_both_switches_are_on():
    assert repo_short_urls.canonical_redirect("/acme/Bolt", "", SHORT_ONLY) is None
    assert repo_short_urls.canonical_redirect(
        "/acme/Bolt", "", dict(ON, short=False)) is None
    assert repo_short_urls.canonical_redirect("/acme/Bolt", "", {}) is None


def test_public_paths_keep_the_owner_for_names_that_cannot_be_short():
    assert repo_short_urls.public_path("acme", "Bolt", ON) == "/Bolt"
    assert repo_short_urls.public_path("Acme", "Bolt", ON) == "/Bolt"
    assert repo_short_urls.public_path("acme", "Bolt", SHORT_ONLY) == "/acme/Bolt"
    assert repo_short_urls.public_path("other", "Bolt", ON) == "/other/Bolt"
    for name in ("docs", "acme", "notes.txt", "x.git", ".hidden"):
        assert repo_short_urls.public_path("acme", name, ON) == "/acme/" + name
    assert repo_short_urls.public_path("acme", "ada", ON, keep=("Ada",)) == (
        "/acme/ada")
    names = {"Bolt", "docs", "ada", "Leaf", "a.json"}
    assert repo_short_urls.owner_candidates(names, "acme") == [
        "ada", "bolt", "leaf"]
    assert repo_short_urls.keep_names(names, "acme", ["ada"]) == [
        "a.json", "ada", "docs"]


def test_the_browser_document_names_the_owner_and_the_exceptions():
    assert repo_short_urls.client_doc(SHORT_ONLY) == {
        "owner": "acme", "short": True, "canonical": False}
    doc = repo_short_urls.client_doc(ON, {"keep": ["Ada", "docs"],
                                          "aliases": {"bolt": "drew"}})
    assert doc["owner"] == "acme" and doc["canonical"] is True
    assert doc["keep"] == ["ada", "docs"]
    assert doc["aliases"] == {"bolt": "drew"}
    assert "dashboard" in doc["reserved"] and "api" in doc["reserved"]
    repo_short_urls.remember_keep("acme", ["ada"], NOW)
    assert repo_short_urls.cached_keep("acme", NOW + 1) == ["ada"]
    assert repo_short_urls.cached_keep("other", NOW + 1) is None
    assert repo_short_urls.cached_keep(
        "acme", NOW + repo_short_urls.KEEP_CACHE_MS) is None
    repo_short_urls.remember_keep("acme", ["ada"], NOW)
    repo_short_urls.forget()
    assert repo_short_urls.cached_keep("acme", NOW) is None


def test_the_runtime_redirects_only_page_reads(monkeypatch):
    calls = []

    async def canonical_path(env, path, query="", public_only=True):
        calls.append((path, query, public_only))
        return "/Bolt" + ("?" + query if query else "")

    monkeypatch.setattr(app_runtime, "_repo_canonical_path", canonical_path,
                        raising=False)
    monkeypatch.setattr(app_runtime, "method_name",
                        lambda request: request.method, raising=False)
    redirect = app_runtime.Default._repo_canonical_redirect
    me = SimpleNamespace(env=None)

    def run(method, query=""):
        url = urlparse("https://git.example.org/drew/Bolt"
                       + ("?" + query if query else ""))
        return asyncio.run(redirect(me, SimpleNamespace(method=method), url))

    assert run("GET", "q=1") == "/Bolt?q=1"
    assert run("HEAD") == "/Bolt"
    assert run("POST") is None
    assert calls == [("/drew/Bolt", "q=1", True), ("/drew/Bolt", "", True)]


def test_the_route_redirects_before_serving_the_full_address_page():
    route = RUNTIME.split("    async def _route(self, request, url):", 1)[1]
    block = route.split("        if looks_like_repo_route(url.path):", 1)[1]
    block = block.split("        if url.path == \"/dashboard\"", 1)[0]
    assert block.index("_repo_canonical_redirect(") < block.index(
        "return await self._serve_repo_page(request, url)")
    assert '"location": canonical_target' in block and "status=308" in block
    assert route.index("project_target = await self._repo_projects_redirect(url)") < \
        route.index("if looks_like_repo_route(url.path):")
    assert route.index("git_info = GIT_INFO_RE.match(url.path)") < \
        route.index("_repo_canonical_redirect(")
    assert '"repoAddresses": await self._repo_addresses_doc()' in RUNTIME
    page = RUNTIME.split("    async def _serve_repo_page(", 1)[1].split(
        "\n    async def ", 1)[0]
    assert 'canonical = "%s/%s" % (origin, quote(short_name))' in page
    assert 'meta name="forkmesh-repo-canonical"' in page
    assert "canonical, title" in page


ALIASES = {("acme-bolt", "bolt"): "drew", ("acme-bolt", "secret"): "drew"}
PRIVATE = {("drew", "secret")}
CANON = {"short": True, "owner": "acme-bolt", "canonical": True}


def entry_namespace(setting_doc, accounts=(), rows=None, records=(),
                    nodes=("forkmesh", "acme-bolt-actions")):
    tree = ast.parse(ENTRY, filename=str(ENTRY_PATH))
    names = {"_git_request_path", "_repo_short_setting_row",
             "_repo_name_is_account", "_repo_canonical_path",
             "_repo_public_path", "_repo_public_href", "_repo_short_links",
             "_repo_clone_url", "_short_repo_board"}
    selected = [node for node in tree.body
                if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
                and node.name in names]
    assert {node.name for node in selected} == names
    queries = []

    async def d1_first(_env, sql, *args):
        assert args == ("repo_paths",)
        return {"doc": setting_doc}

    async def d1_all(_env, sql, *args):
        queries.append(sql)
        if "org_repos" in sql:
            return [{"repo": repo, "node_owner": node}
                    for (org, repo), node in (rows or {}).items()
                    if "bi:org:" + org == args[0]]
        return [{"data": record} for record in records]

    async def decrypt_row(_env, data):
        return data

    async def blind_index(_env, value):
        return "bi:" + value

    async def account_row(_env, name):
        if name in nodes:
            return "bi", {"name": name, "kind": "node"}
        return "bi", ({"name": name} if name in accounts else None)

    async def org_row(_env, name):
        return "bi", ({"name": name} if name == "acme-bolt" else None)

    async def org_repo_node(_env, org, repo):
        return (rows if rows is not None else ALIASES).get(
            (org.lower(), repo.lower()), "")

    async def repo_is_private(_env, owner, repo):
        return (owner.lower(), repo.lower()) in PRIVATE

    from urllib.parse import quote, unquote
    namespace = {"urlparse": urlparse, "quote": quote, "unquote": unquote,
                 "repo_short_urls": repo_short_urls,
                 "Date": SimpleNamespace(now=lambda: NOW),
                 "d1_first": d1_first, "d1_all": d1_all,
                 "decrypt_row": decrypt_row, "blind_index": blind_index,
                 "_account_row": account_row, "_org_row": org_row,
                 "_org_repo_node": org_repo_node,
                 "_repo_is_private": repo_is_private, "queries": queries,
                 "_account_kind": lambda rec: rec.get("kind", "user")}
    exec(compile(ast.Module(body=selected, type_ignores=[]),
                 str(ENTRY_PATH), "exec"), namespace)
    return namespace


def test_creator_and_organization_page_paths_redirect_to_the_short_form():
    ns = entry_namespace(repo_short_urls.dump(CANON), accounts={"drew"})
    path = ns["_repo_canonical_path"]

    def run(value, query="", public_only=True):
        return asyncio.run(path(None, value, query, public_only))

    assert run("/drew/Bolt") == "/Bolt"
    assert run("/Drew/Bolt/issues/4", "label=bug") == "/Bolt/issues/4?label=bug"
    assert run("/drew/Bolt/tree/bolt-c") == "/Bolt/tree/bolt-c"
    assert run("/acme-bolt/Bolt/pulls") == "/Bolt/pulls"
    assert run("/drew/other") is None
    assert run("/someone/Bolt") is None
    assert run("/drew/Bolt/info/refs") is None
    assert run("/drew/Bolt.git") is None
    assert run("/drew/secret") is None
    assert run("/drew/secret", public_only=False) == "/secret"
    assert run("/acme-bolt/secret") == "/secret"
    assert run("/drew") is None
    repo_short_urls.forget()
    ns = entry_namespace(repo_short_urls.dump(
        dict(CANON, canonical=False)), accounts={"drew"})
    assert asyncio.run(ns["_repo_canonical_path"](
        None, "/drew/Bolt")) is None


def test_the_platform_mirror_owned_by_the_node_account_moves_to_the_root():
    ns = entry_namespace(repo_short_urls.dump(CANON),
                         rows={("acme-bolt", "forkmesh"): "forkmesh"})
    path = ns["_repo_canonical_path"]
    assert asyncio.run(path(None, "/forkmesh/forkmesh")) == "/forkmesh"
    assert asyncio.run(path(None, "/acme-bolt/forkmesh/issues")) == (
        "/forkmesh/issues")
    assert asyncio.run(ns["_repo_clone_url"](
        None, "https://code.example.org", "acme-bolt", "forkmesh")) == (
        "https://code.example.org/forkmesh.git")


def test_the_largest_repositories_board_names_short_repositories_only():
    ns = entry_namespace(
        repo_short_urls.dump({"short": True, "owner": "acme", "canonical": True}),
        rows={("acme", "repo"): "alice", ("acme", "forkmesh"): "forkmesh"})
    rows = [{"name": "forkmesh/forkmesh", "bytes": 3},
            {"name": "alice/Repo", "bytes": 2},
            {"name": "bob/tools", "bytes": 1},
            {"name": "node", "bytes": 0}]
    shown = asyncio.run(ns["_short_repo_board"](None, rows))
    assert [row["name"] for row in shown] == [
        "forkmesh", "Repo", "bob/tools", "node"]
    assert [row["bytes"] for row in shown] == [3, 2, 1, 0]
    repo_short_urls.forget()
    ns = entry_namespace(
        repo_short_urls.dump({"short": True, "owner": "acme", "canonical": False}),
        rows={("acme", "repo"): "alice"})
    assert [row["name"] for row in asyncio.run(ns["_short_repo_board"](
        None, rows))] == ["forkmesh/forkmesh", "alice/Repo", "bob/tools", "node"]
    assert 'short_board = globals().get("_short_repo_board")' in ENTRY
    assert '"largest": await short_board(env, largest_board[:LEADERBOARD_LIMIT])' in ENTRY


def test_names_of_accounts_keep_the_owner():
    ns = entry_namespace(repo_short_urls.dump(CANON),
                         accounts={"drew", "ada"},
                         rows={("acme-bolt", "ada"): "drew"})
    assert asyncio.run(ns["_repo_canonical_path"](None, "/acme-bolt/ada")) is None
    assert asyncio.run(ns["_repo_canonical_path"](None, "/drew/ada")) is None


def test_notification_links_and_clone_urls_drop_the_creator():
    ns = entry_namespace(repo_short_urls.dump(CANON), accounts={"drew"})
    href = ns["_repo_public_href"]
    assert asyncio.run(href(None, "/drew/Bolt/issues/3")) == "/Bolt/issues/3"
    assert asyncio.run(href(None, "/drew/secret/pulls/2?x=1")) == (
        "/secret/pulls/2?x=1")
    assert asyncio.run(href(None, "/dashboard/settings")) == "/dashboard/settings"
    assert asyncio.run(href(None, "https://elsewhere.org/drew/Bolt")) == (
        "https://elsewhere.org/drew/Bolt")
    clone = ns["_repo_clone_url"]
    origin = "https://code.example.org"
    assert asyncio.run(clone(None, origin, "acme-bolt", "Bolt")) == origin + "/Bolt.git"
    assert asyncio.run(clone(None, origin, "drew", "Bolt")) == origin + "/Bolt.git"
    assert asyncio.run(clone(None, origin, "bob", "Bolt")) == origin + "/bob/Bolt"
    enqueue = ENTRY.split("async def enqueue_notification(", 1)[1].split(
        "\nasync def ", 1)[0]
    assert 'globals().get("_repo_public_href")' in enqueue
    assert enqueue.index("href = await public_href(env, href)") < \
        enqueue.index("payload = notification_payload(")


def test_clone_urls_stay_long_without_the_canonical_switch():
    ns = entry_namespace(repo_short_urls.dump(
        {"short": True, "owner": "acme-bolt", "canonical": False}))
    assert asyncio.run(ns["_repo_clone_url"](
        None, "https://git.example.org", "acme-bolt", "Bolt")) == (
        "https://git.example.org/acme-bolt/Bolt")


def test_the_link_table_hides_private_repositories_from_the_browser():
    ns = entry_namespace(
        repo_short_urls.dump(CANON), accounts={"drew", "bolt-team"},
        rows={("acme-bolt", "bolt"): "drew",
              ("acme-bolt", "secret"): "drew",
              ("acme-bolt", "docs"): "drew",
              ("acme-bolt", "forkmesh"): "forkmesh",
              ("acme-bolt", "bolt-team"): "drew"},
        records=[{"name": "Own", "visibility": "public"}])
    links = asyncio.run(ns["_repo_short_links"](None, "acme-bolt"))
    assert links["aliases"] == {"bolt": "drew", "docs": "drew",
                                "forkmesh": "forkmesh",
                                "bolt-team": "drew"}
    assert links["links"]["secret"] == "drew"
    assert links["keep"] == ["bolt-team", "docs"]
    assert links["allKeep"] == ["bolt-team", "docs"]
    count = len(ns["queries"])
    assert asyncio.run(ns["_repo_short_links"](None, "acme-bolt")) == links
    assert len(ns["queries"]) == count
    doc = repo_short_urls.client_doc(CANON, links)
    assert doc["aliases"] == links["aliases"] and "secret" not in json.dumps(doc)


def test_short_git_requests_recover_the_full_route_for_alias_lookups():
    ns = entry_namespace(repo_short_urls.dump(ON))
    path = ns["_git_request_path"]
    request = SimpleNamespace(url="https://git.example.org/Bolt.git/info/refs")
    assert path(request) == "/Bolt.git/info/refs"
    repo_short_urls.remember(ON, NOW)
    assert path(request) == "/acme/Bolt.git/info/refs"
    assert path(SimpleNamespace(
        url="https://git.example.org/Bolt/git-receive-pack")) == (
        "/acme/Bolt/git-receive-pack")
    assert path(SimpleNamespace(
        url="https://git.example.org/bob/Bolt/git-receive-pack")) == (
        "/bob/Bolt/git-receive-pack")
    assert path(SimpleNamespace(url="https://git.example.org/Bolt/issues")) == (
        "/Bolt/issues")
    alias = ENTRY.split("async def _git_push_alias_owner(", 1)[1].split(
        "\nasync def ", 1)[0]
    assert "_git_request_path(request)" in alias
    proxy = ENTRY.split("async def _https_mirror_proxy(", 1)[1].split(
        "\nasync def ", 1)[0]
    assert "original_path = _git_request_path(request)" in proxy


LINKS = {"keep": ["docs"], "allKeep": ["docs", "hidden"],
         "aliases": {"bolt": "drew"},
         "links": {"bolt": "drew", "hidden": "drew",
                   "secret": "drew"}}


def test_catalog_and_search_urls_name_only_the_repository():
    origin = "https://code.example.org"
    payload = {"ok": True, "repositories": [
        {"owner": "drew", "name": "Bolt",
         "cloneUrl": origin + "/acme-bolt/Bolt",
         "mirrors": [{"cloneUrl": "https://n1.example.org/drew/Bolt"}]},
        {"owner": "drew", "name": "secret",
         "cloneUrl": origin + "/drew/secret.git"},
        {"owner": "drew", "name": "docs", "cloneUrl": origin + "/drew/docs"},
        {"owner": "bob", "name": "Bolt", "cloneUrl": origin + "/bob/Bolt"},
    ], "results": [{"url": origin + "/acme-bolt/Bolt",
                    "clone": {"https": origin + "/drew/Bolt",
                              "command": "git clone " + origin + "/drew/Bolt"},
                    "href": origin + "/drew/Bolt/issues/2",
                    "other": origin + "/drew/Bolt/src/x"}]}
    out = repo_short_urls.short_urls(payload, origin, CANON, LINKS)
    records = out["repositories"]
    assert records[0]["cloneUrl"] == origin + "/Bolt.git"
    assert records[0]["mirrors"][0]["cloneUrl"] == "https://n1.example.org/drew/Bolt"
    assert records[1]["cloneUrl"] == origin + "/secret.git"
    assert records[2]["cloneUrl"] == origin + "/drew/docs"
    assert records[3]["cloneUrl"] == origin + "/bob/Bolt"
    result = out["results"][0]
    assert result["url"] == origin + "/Bolt"
    assert result["clone"] == {"https": origin + "/Bolt.git",
                               "command": "git clone " + origin + "/Bolt.git"}
    assert result["href"] == origin + "/Bolt/issues/2"
    assert result["other"] == origin + "/drew/Bolt/src/x"
    assert repo_short_urls.short_urls(payload, origin, SHORT_ONLY, LINKS) is payload


def test_the_catalog_response_is_rewritten_for_get_only(monkeypatch):
    origin = "https://code.example.org"
    body = json.dumps({"ok": True, "repositories": [
        {"owner": "drew", "name": "Bolt", "cloneUrl": origin + "/drew/Bolt"}]})

    class Headers:
        def items(self):
            return [("content-type", "application/json"), ("etag", "x"),
                    ("content-length", "9"), ("cache-control", "public")]

    class FakeResponse:
        status = 200
        headers = Headers()

        async def text(self):
            return body

    built = []

    def response(text, status=200, headers=None):
        built.append((text, status, headers))
        return SimpleNamespace(text=text, status=status, headers=headers)

    async def setting():
        return CANON

    async def keep(owner):
        return LINKS

    monkeypatch.setattr(app_runtime, "method_name",
                        lambda request: request.method, raising=False)
    monkeypatch.setattr(app_runtime, "Response", response, raising=False)
    monkeypatch.setattr(app_runtime, "json", json, raising=False)
    monkeypatch.setattr(app_runtime, "repo_short_urls", repo_short_urls,
                        raising=False)
    me = SimpleNamespace(_repo_short_setting=setting, _repo_short_keep=keep)
    url = urlparse(origin + "/api/repositories")
    rewrite = app_runtime.Default._repo_canonical_json
    original = FakeResponse()
    assert asyncio.run(rewrite(me, SimpleNamespace(method="HEAD"), url,
                               original)) is original
    answer = asyncio.run(rewrite(me, SimpleNamespace(method="GET"), url,
                                 FakeResponse()))
    assert json.loads(answer.text)["repositories"][0]["cloneUrl"] == origin + "/Bolt.git"
    assert answer.headers == [("content-type", "application/json"),
                              ("cache-control", "public")]
    assert '"/api/repositories", "/api/repositories/search"' in RUNTIME


def test_a_short_push_gets_the_git_push_body_limit():
    big = request_guard.DEFAULT_MAX_GIT_PUSH_BODY
    assert request_guard.body_limit("POST", "/Bolt.git/git-receive-pack") == big
    assert request_guard.body_limit("POST", "/acme/Bolt/git-receive-pack") == big
    assert request_guard.body_limit("POST", "/a/b/c/git-receive-pack") != big


def test_hosted_push_instructions_follow_the_public_address():
    remote = hosted_git._push_instructions(
        "https://git.example.org/Bolt.git", "ada", "pw", 5, "n1")
    assert remote["remoteUrl"] == "https://git.example.org/Bolt.git"
    assert "git remote add forkmesh https://git.example.org/Bolt.git" in (
        remote["commands"])
    namespace = vars(hosted_git)
    saved = namespace.pop("_repo_clone_url", None)
    try:
        assert asyncio.run(hosted_git._remote_url(
            None, "https://x.org", "acme", "Bolt")) == "https://x.org/acme/Bolt"

        async def clone(_env, origin, owner, name):
            return origin + "/" + name + ".git"
        namespace["_repo_clone_url"] = clone
        assert asyncio.run(hosted_git._remote_url(
            None, "https://x.org", "acme", "Bolt")) == "https://x.org/Bolt.git"
    finally:
        namespace.pop("_repo_clone_url", None)
        if saved is not None:
            namespace["_repo_clone_url"] = saved
    source = (ROOT / "src" / "hosted_git.py").read_text(encoding="utf-8")
    assert 'grant["remoteUrl"] = await _remote_url(env, origin, public_owner, name)' in source
    assert "line.replace(old_remote, grant[\"remoteUrl\"])" in source


def test_project_addresses_still_target_the_short_repository():
    setting = repo_projects.parse(json.dumps({
        "repo": "Bolt", "table": "projects/leaf\tLeaf\tbolt-c",
        "legacy": ["acme-bolt"]}))
    assert repo_projects.redirect("/Bolt/Leaf", "", setting, "acme") == (
        "/Bolt/tree/projects/leaf")
    assert repo_projects.redirect("/acme-bolt/bolt-c/issues", "", setting,
                                  "acme") == "/Bolt/Leaf/issues"
    assert repo_short_urls.canonical_redirect(
        "/acme/Bolt/tree/projects/leaf", "", ON)[0] == "/Bolt/tree/projects/leaf"


class FakeRequest:
    def __init__(self, url, method="GET", form=None):
        self.url = url
        self.method = method
        self._form = form or {}

    async def text(self):
        return urlencode(self._form)


def bind_admin(monkeypatch, values):
    for name, value in values.items():
        monkeypatch.setattr(admin, name, value, raising=False)


def test_the_admin_form_saves_and_shows_the_second_switch(monkeypatch):
    saved = []
    docs = {}

    async def d1_run(env, sql, *args):
        saved.append(args)
        docs[args[0]] = args[1]

    async def d1_first(env, sql, *args):
        return {"doc": docs[args[0]]} if args[0] in docs else None

    async def org_row(env, name):
        return "bi", ({"name": name} if name == "acme" else None)

    async def account_row(env, name):
        return "bi", None

    async def audit(*args):
        pass

    bind_admin(monkeypatch, {
        "d1_run": d1_run, "d1_first": d1_first, "_org_row": org_row,
        "_account_row": account_row, "_audit_sensitive_action": audit,
        "Date": SimpleNamespace(now=lambda: NOW),
        "_admin_csrf_ok": lambda env, form: form.get("csrf") == ["ok"],
        "_admin_href": lambda query, **kw: "/admin?" + "&amp;".join(
            "%s=%s" % item for item in kw.items()),
        "method_name": lambda request: request.method,
        "_html_escape": admin_shell.esc,
        "_admin_shell": admin_shell,
        "Response": lambda body, status=200, headers=None: SimpleNamespace(
            body=body, status=status, headers=headers or {}),
    })
    env = SimpleNamespace()
    request = FakeRequest("https://git.example.org/x", "POST", {
        "csrf": "ok", "action": "save_repo_paths", "short": "1",
        "canonical": "1", "owner": "Acme"})
    answer = asyncio.run(admin.admin_handler(
        env, request, {"view": ["repo-addresses"]}, "", "", "ada"))
    assert "notice=addresses_saved" in answer.headers["location"]
    assert saved[-1][:2] == (
        "repo_paths", '{"canonical":true,"owner":"acme","short":true}')
    page = asyncio.run(admin.render_addresses(
        env, FakeRequest("https://git.example.org/x"), "", ""))
    assert 'name="canonical" value="1" checked' in page
    assert "single-organization instance" in page
    assert "redirects to https://git.example.org/&lt;repository&gt;" in page
    request = FakeRequest("https://git.example.org/x", "POST", {
        "csrf": "ok", "action": "save_repo_paths", "canonical": "1",
        "owner": "acme"})
    asyncio.run(admin.admin_handler(
        env, request, {"view": ["repo-addresses"]}, "", "", "ada"))
    assert saved[-1][1] == '{"canonical":false,"owner":"acme","short":false}'
    page = asyncio.run(admin.render_addresses(
        env, FakeRequest("https://git.example.org/x"), "", ""))
    assert 'name="canonical" value="1">' in page
    assert "keeps working" in page


def js_function(source, name):
    start = source.index("function %s(" % name)
    if source[max(0, start - 6):start] == "async ":
        start -= 6
    cursor = source.index("{", source.index(")", start))
    depth = 0
    while cursor < len(source):
        if source[cursor] == "{":
            depth += 1
        elif source[cursor] == "}":
            depth -= 1
            if depth == 0:
                return source[start:cursor + 1]
        cursor += 1
    raise AssertionError(name)


def js_const(source, name):
    match = re.search(r"^\s*const %s = .*;$" % name, source, re.M)
    assert match, name
    return match.group(0).strip()


HARNESS = """
const metas = %(metas)s;
const store = new Map(%(store)s);
const document = { querySelector: (sel) => {
  const key = (sel.match(/name="([^"]+)"/) || [])[1];
  return key in metas ? { content: metas[key] } : null;
}, body: { dataset: { page: "repo" } } };
const localStorage = {
  getItem: (k) => (store.has(k) ? store.get(k) : null),
  setItem: (k, v) => store.set(k, String(v)),
  removeItem: (k) => store.delete(k),
};
const location = { pathname: %(path)s, hostname: "git.example.org", search: "", origin: "https://git.example.org" };
const state = {};
const publicSiteOrigin = () => "https://git.example.org";
const repoPublicOwner = (repo) => repo.logicalOwner || "";
"""


def run_helpers(script, metas=None, store=None, path="/"):
    names = ["safeDecodeURIComponent", "normalizeRepoAddresses",
             "repoAddressesSignature",
             "repoAddresses", "applyRepoAddresses", "repoAddressesKnown",
             "repoShortName", "repoPathUrl", "repoLinkUrl", "cloneUrl",
             "repoShortRoute", "repoRouteParts", "repoPublicHref",
             "repoPublicKey", "normalizeRepoSegment", "isThisSiteUrl",
             "parseCloneUrlIdentity"]
    consts = ["REPO_ADDRESSES_STORE", "REPO_ADDRESSES_SAVED_STORE",
              "REPO_ADDRESSES_TTL_MS", "REPO_CORE_HOSTS", "REPO_SHORT_NAME_RE",
              "REPO_SHORT_FILE_RE", "REPO_TAB_ROUTES"]
    source = (HARNESS % {"metas": json.dumps(metas or {}),
                         "store": json.dumps(list((store or {}).items())),
                         "path": json.dumps(path)}
              + "\n".join(js_const(HELPERS, name) for name in consts) + "\n"
              + "\n".join(js_function(HELPERS, name) for name in names)
              + "\n" + script)
    result = subprocess.run(["node", "--input-type=module", "-e", source],
                            check=True, text=True, capture_output=True)
    return json.loads(result.stdout)


DOC = {"owner": "acme", "short": True, "canonical": True,
       "keep": ["ada", "forkmesh"], "reserved": ["api", "dashboard", "docs"],
       "aliases": {"bolt": "drew"}}


def test_browser_links_drop_the_owner_for_the_canonical_owner_only():
    out = run_helpers("""
applyRepoAddresses(%s);
console.log(JSON.stringify({
  plain: repoPathUrl({ owner: "acme", name: "Bolt" }),
  tree: repoPathUrl({ owner: "acme", name: "Bolt" }, "tree", "bolt-c/src"),
  upper: repoPathUrl({ owner: "Acme", name: "Bolt" }, "blob", "README.md"),
  other: repoPathUrl({ owner: "bob", name: "Bolt" }),
  keep: repoPathUrl({ owner: "acme", name: "Ada" }),
  node: repoPathUrl({ owner: "acme", name: "forkmesh" }),
  reserved: repoPathUrl({ owner: "acme", name: "docs" }),
  file: repoPathUrl({ owner: "acme", name: "notes.txt" }),
  alias: repoLinkUrl({ owner: "forkmesh", logicalOwner: "acme", name: "Bolt" }),
  clone: cloneUrl({ owner: "forkmesh", logicalOwner: "acme", name: "Bolt" }),
  longClone: cloneUrl({ owner: "bob", name: "Bolt" }),
  stored: localStorage.getItem(REPO_ADDRESSES_STORE),
  known: repoAddressesKnown(),
}));
""" % json.dumps(DOC))
    assert out["plain"] == "/Bolt"
    assert out["tree"] == "/Bolt/tree/bolt-c/src"
    assert out["upper"] == "/Bolt/blob/README.md"
    assert out["other"] == "/bob/Bolt"
    assert out["keep"] == "/acme/Ada"
    assert out["node"] == "/acme/forkmesh"
    assert out["reserved"] == "/acme/docs"
    assert out["file"] == "/acme/notes.txt"
    assert out["alias"] == "/Bolt"
    assert out["clone"] == "https://git.example.org/Bolt.git"
    assert out["longClone"] == "https://git.example.org/bob/Bolt"
    assert json.loads(out["stored"]) == DOC and out["known"] is True


def test_creator_links_labels_and_clone_identities_use_the_short_form():
    out = run_helpers("""
applyRepoAddresses(%s);
console.log(JSON.stringify({
  creator: repoPathUrl({ owner: "drew", name: "Bolt" }, "tree", "x"),
  creatorOther: repoPathUrl({ owner: "drew", name: "Other" }),
  logical: repoPathUrl({ owner: "drew", logicalOwner: "acme", name: "Private" }),
  href: repoPublicHref("/drew/Bolt/issues/3?x=1#c"),
  orgHref: repoPublicHref("/acme/Bolt/pulls/2"),
  deepHref: repoPublicHref("/drew/Bolt/src/x"),
  otherHref: repoPublicHref("/bob/Bolt/issues/1"),
  dashHref: repoPublicHref("/dashboard/settings"),
  key: repoPublicKey("drew/Bolt"),
  orgKey: repoPublicKey("acme/Bolt"),
  otherKey: repoPublicKey("bob/Bolt"),
  identity: parseCloneUrlIdentity("https://git.example.org/Bolt.git"),
  elsewhere: parseCloneUrlIdentity("https://other.example.org/Bolt.git"),
  long: parseCloneUrlIdentity("https://git.example.org/bob/Bolt"),
}));
""" % json.dumps(DOC))
    assert out["creator"] == "/Bolt/tree/x"
    assert out["creatorOther"] == "/drew/Other"
    assert out["logical"] == "/Private"
    assert out["href"] == "/Bolt/issues/3?x=1#c"
    assert out["orgHref"] == "/Bolt/pulls/2"
    assert out["deepHref"] == "/drew/Bolt/src/x"
    assert out["otherHref"] == "/bob/Bolt/issues/1"
    assert out["dashHref"] == "/dashboard/settings"
    assert (out["key"], out["orgKey"], out["otherKey"]) == ("Bolt", "Bolt", "bob/Bolt")
    assert out["identity"] == {"owner": "acme", "name": "Bolt"}
    assert out["elsewhere"] is None
    assert out["long"] == {"owner": "bob", "name": "Bolt"}


def test_browser_links_stay_long_without_the_setting():
    out = run_helpers("""
const first = repoPathUrl({ owner: "acme", name: "Bolt" });
applyRepoAddresses({ owner: "acme", short: true, canonical: false });
console.log(JSON.stringify({ first, after: repoPathUrl({ owner: "acme", name: "Bolt" }),
  stored: localStorage.getItem(REPO_ADDRESSES_STORE), known: repoAddressesKnown() }));
""")
    assert out["first"] == "/acme/Bolt" and out["after"] == "/acme/Bolt"
    assert out["known"] is True


def test_the_short_page_meta_turns_canonical_links_on_before_the_fetch():
    out = run_helpers("""
console.log(JSON.stringify({ url: repoPathUrl({ owner: "acme", name: "Bolt" }, "tree", "x"),
  parts: repoRouteParts() }));
""", metas={"forkmesh-repo-short": "Bolt", "forkmesh-repo-owner": "acme",
            "forkmesh-repo-canonical": "acme"}, path="/Bolt/bolt-c")
    assert out["url"] == "/Bolt/tree/x"
    assert out["parts"] == ["acme", "Bolt", "tree", "bolt-c"]


def test_short_addresses_made_in_the_page_are_parsed_back_after_navigation():
    out = run_helpers("""
applyRepoAddresses(%s);
const before = repoRouteParts();
repoPathUrl({ owner: "acme", name: "Leaf" });
const after = repoRouteParts();
location.pathname = "/dashboard/Leaf";
const prefixed = repoRouteParts();
location.pathname = "/bob/tools";
const other = repoRouteParts();
console.log(JSON.stringify({ before, after, prefixed, other }));
""" % json.dumps(DOC), metas={"forkmesh-repo-short": "Bolt",
                               "forkmesh-repo-owner": "acme"},
        path="/Leaf/issues/4")
    assert out["before"] == ["Leaf", "issues", "4"]
    assert out["after"] == ["acme", "Leaf", "issues", "4"]
    assert out["prefixed"] == ["Leaf"]
    assert out["other"] == ["bob", "tools"]


def test_the_dashboard_loads_the_setting_before_the_first_render():
    loader = js_function(HELPERS, "loadRepoAddresses")
    # The page's one shared /api/instance read (test_dashboard_shared_reads).
    assert "await sharedInstanceDocument()" in loader
    assert 'fetch("/api/instance"' in js_function(HELPERS, "sharedInstanceDocument")
    assert "REPO_CORE_HOSTS.includes(location.hostname.toLowerCase())" in loader
    assert BOOT.index("const repoAddressesLoaded = loadRepoAddresses();") < \
        BOOT.index("(PAGE_INITS[currentPage()] || initHomePage)();")
    assert BOOT.index("if (!repoAddressesKnown())") < \
        BOOT.index("(PAGE_INITS[currentPage()] || initHomePage)();")


def test_no_dashboard_link_builds_the_owner_form_by_hand():
    pattern = re.compile(
        r"`/\$\{encodeURIComponent\([^)]*\)\}/\$\{encodeURIComponent")
    offenders = []
    for path in sorted(JS_DIR.glob("*.js")):
        for number, line in enumerate(
                path.read_text(encoding="utf-8").splitlines(), 1):
            if pattern.search(line) and "/api/" not in line \
                    and "url.pathname" not in line:
                offenders.append("%s:%d" % (path.name, number))
    assert offenders == []


@pytest.mark.parametrize("path", [
    "/admin", "/admin/", "/admin/theme", "/health", "/readyz", "/oauth/x",
    "/developers", "/mcp", "/nodeinfo/2.0", "/ap/actor", "/v1/runner/acme/Repo.git",
])
def test_server_pages_are_never_read_as_a_short_repository(path):
    assert repo_short_urls.rewrite(path, "acme") is None
