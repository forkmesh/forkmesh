#!/usr/bin/env python3
"""Organization-alias repository social metadata regressions.

The browser document is not part of ``org_alias_rewrite``: its public
organization identity must stay in the address bar and social tags while
privacy/catalog reads use the linked node's signed repository row.  Card
requests *are* internally rewritten, so the renderer recovers the public
identity from the untouched Request URL.
"""

import ast
import asyncio
import html
import re
import sys
import time
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import parse_qs, quote, unquote, urlparse


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
import instance_features  # noqa: E402
import og_card as real_card  # noqa: E402
ENTRY = ROOT / "src" / "entry.py"
ENTRY_TEXT = (
    # entry.py + its lazily-split domain modules
    (ENTRY.parent / "forkbot.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "fediverse_routes.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "ssh_gateway_routes.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "app_runtime.py").read_text(encoding="utf-8")
    + "\n\n\n"
)


def _module(name):
    import importlib.util

    spec = importlib.util.spec_from_file_location(
        name, ROOT / "src" / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


catalog = _module("catalog")
urls = _module("urls")


def _run(coro):
    return asyncio.run(coro)


def _load_function(name, namespace):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    function = next(
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name == name
    )
    scope = dict(namespace)
    scope.setdefault("instance_features", instance_features)
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=[function], type_ignores=[])),
        str(ENTRY), "exec"), scope)
    return scope[name]


def _load_default_method(name, namespace):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    method = next(
        child for node in tree.body
        if isinstance(node, ast.ClassDef) and node.name == "Default"
        for child in node.body
        if isinstance(child, (ast.FunctionDef, ast.AsyncFunctionDef))
        and child.name == name)
    subject = ast.ClassDef(
        name="_Subject",
        bases=[],
        keywords=[],
        body=[method],
        decorator_list=[],
    )
    scope = dict(namespace)
    scope.setdefault("instance_features", instance_features)
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=[subject], type_ignores=[])),
        str(ENTRY), "exec"), scope)
    return scope["_Subject"]


class _Response:
    def __init__(self, body, status=200, headers=None):
        self.body = body
        self.status = status
        self.headers = headers or {}


class _AssetResponse:
    def __init__(self, body):
        self.body = body

    async def text(self):
        return self.body


class _Assets:
    async def fetch(self, _url):
        return _AssetResponse(
            "<!doctype html><html><head>"
            "<title>ForkMesh Dashboard</title></head><body></body></html>")


def test_crawler_gets_alias_canonical_card_and_backing_catalog_metadata():
    """A crawler sees forkmesh/forkmesh, never its mirror2 storage identity."""
    reads = []
    cached = []

    async def org_repo_node(_env, owner, repo):
        reads.append(("alias", owner, repo))
        return "mirror2" if (owner, repo) == ("forkmesh", "forkmesh") else ""

    async def repo_is_private(_env, owner, repo):
        reads.append(("privacy", owner, repo))
        return False

    async def ensure_schema(_env):
        return None

    async def blind_index(_env, value):
        reads.append(("catalog", value))
        return "repo-bi"

    async def d1_first(_env, sql, *params):
        assert "FROM repositories" in sql
        assert params == ("repo-bi",)
        return {"data": "encrypted"}

    async def decrypt_row(_env, value):
        assert value == "encrypted"
        return {
            "owner": "mirror2",
            "name": "forkmesh",
            "visibility": "public",
            "description": "Current organization repository",
            "stateHash": "a" * 64,
            "commit": "b" * 40,
        }

    async def edge_cache_match(_key):
        return None

    async def edge_cache_put(key, response):
        cached.append((key, response))

    page_class = _load_default_method("_serve_repo_page", {
        "safe_segment": catalog.safe_segment,
        "unquote": unquote,
        "quote": quote,
        "_org_repo_node": org_repo_node,
        "_repo_is_private": repo_is_private,
        "edge_cache_match": edge_cache_match,
        "edge_cache_put": edge_cache_put,
        "DASHBOARD_REPO_ASSET": "repo.html",
        "Response": _Response,
        "ensure_schema": ensure_schema,
        "blind_index": blind_index,
        "d1_first": d1_first,
            "decrypt_row": decrypt_row,
            "_ap_origin": lambda _env, _request=None: "https://forkmesh.com",
            "_catalog_record_matches_identity": (
            lambda rec, owner, repo:
            rec.get("owner") == owner and rec.get("name") == repo
        ),
        "_repository_social_version": (
            lambda rec: rec.get("stateHash", "")
        ),
        "_html_escape": html.escape,
        "og_card": SimpleNamespace(CARD_W=1200, CARD_H=630, CARD_REVISION="3"),
    })
    subject = page_class()
    subject.env = SimpleNamespace(ASSETS=_Assets())
    request = SimpleNamespace(headers={
        "user-agent": "Twitterbot/1.0"})
    response = _run(subject._serve_repo_page(
        request, urlparse("https://forkmesh.com/forkmesh/forkmesh")))

    assert response.status == 200
    assert response.headers["cache-control"] == "public, max-age=300"
    assert (
        '<link rel="canonical" href="https://forkmesh.com/forkmesh/forkmesh">'
        in response.body)
    assert (
        '<meta property="og:url" '
        'content="https://forkmesh.com/forkmesh/forkmesh">'
        in response.body)
    assert (
        '<meta property="og:title" content="forkmesh/forkmesh - ForkMesh">'
        in response.body)
    assert (
        '<meta property="og:image" '
        'content="https://forkmesh.com/api/repo/forkmesh/forkmesh/card.png'
        '?v=' + ("a" * 64) + '.3">'
        in response.body)
    assert (
        '<meta name="twitter:image" '
        'content="https://forkmesh.com/api/repo/forkmesh/forkmesh/card.png'
        '?v=' + ("a" * 64) + '.3">'
        in response.body)
    assert "Current organization repository" in response.body
    assert "mirror2" not in response.body
    assert ("privacy", "mirror2", "forkmesh") in reads
    assert ("catalog", "mirror2/forkmesh") in reads
    assert cached[0][0] == (
        "https://forkmesh.com/forkmesh/forkmesh?repo-state=" + ("a" * 64)
    )


def test_repository_social_version_tracks_refs_then_commit_without_build_rev():
    version = _load_function("_repository_social_version", {
        "clean_string": catalog.clean_string,
        "re": re,
    })
    assert version({
        "stateHash": "a" * 64,
        "commit": "b" * 40,
    }) == "a" * 64
    assert version({"commit": "b" * 40}) == "b" * 40
    assert version({"stateHash": "not-a-pin", "commit": "also-bad"}) == ""
    page_source = ast.get_source_segment(
        ENTRY_TEXT,
        next(
            child
            for node in ast.parse(ENTRY_TEXT).body
            if isinstance(node, ast.ClassDef) and node.name == "Default"
            for child in node.body
            if isinstance(child, ast.AsyncFunctionDef)
            and child.name == "_serve_repo_page"
        ),
    )
    assert "_repository_social_version(repo_record)" in page_source
    assert "_build_rev" not in page_source


def test_org_alias_remote_clone_is_still_an_official_social_actor():
    """The org link, not the backing mirror's source tag, owns the actor."""

    async def blind_index(_env, value):
        return {
            "org:forkmesh": "org-bi",
            "mirror2/forkmesh": "repo-bi",
        }[value]

    async def d1_first(_env, sql, *params):
        if "FROM org_repos" in sql:
            assert params == ("org-bi", "forkmesh")
            return {"node_owner": "mirror2"}
        assert "FROM repositories" in sql
        assert params == ("repo-bi",)
        return {"data": "encrypted"}

    async def decrypt_row(_env, value):
        assert value == "encrypted"
        return {
            "owner": "mirror2",
            "name": "forkmesh",
            "source": "remote-clone",
        }

    official = _load_function("_ap_repo_is_official_actor", {
        "blind_index": blind_index,
        "d1_first": d1_first,
        "decrypt_row": decrypt_row,
        "valid_node_name": lambda value: bool(value),
        "_catalog_record_matches_identity": (
            lambda rec, owner, repo:
            rec.get("owner") == owner and rec.get("name") == repo
        ),
    })

    assert _run(official(
        None, "forkmesh", "forkmesh")) is True


def test_alias_ssh_url_checks_public_alias_allowlist_and_publishes_org_path():
    functions = {}
    for name in (
        "_ssh_gateway_settings",
        "_ssh_alias_repository_url",
    ):
        functions[name] = _load_function(name, {
            **functions,
            "ssh_auth": _module("ssh_keys"),
        })
    env = SimpleNamespace(
        SSH_GATEWAY_HOST="ssh.forkmesh.com",
        SSH_GATEWAY_PORT="22",
        SSH_GATEWAY_TOKEN="t" * 32,
        SSH_GATEWAY_REPOSITORIES="forkmesh/forkmesh=read-only",
        SSH_GATEWAY_NODE_HOSTS="forkmesh=ssh-mirror2.forkmesh.com",
    )
    assert functions["_ssh_alias_repository_url"](
        env, "forkmesh", "mirror2", "forkmesh",
    ) == "ssh://git@ssh-mirror2.forkmesh.com/forkmesh/forkmesh.git"
    assert functions["_ssh_alias_repository_url"](
        env, "forkmesh", "mirror3", "forkmesh",
    ) == "ssh://git@ssh-mirror2.forkmesh.com/forkmesh/forkmesh.git"

    org_handler = ast.get_source_segment(
        ENTRY_TEXT,
        next(
            node for node in ast.parse(ENTRY_TEXT).body
            if isinstance(node, ast.AsyncFunctionDef)
            and node.name == "org_repos_handler"
        ),
    )
    assert "_ssh_alias_repository_url(" in org_handler
    detail_js = (
        ROOT / "public" / "dashboard" / "js" / "08-repo-detail-network.js"
    ).read_text(encoding="utf-8")
    assert "sshUrl: String(linked.sshUrl || origin.sshUrl || \"\").trim()" in detail_js


class _BinaryResponse:
    @classmethod
    def new(cls, body, init):
        return SimpleNamespace(
            body=body,
            status=init["status"],
            headers=init["headers"])


class _Uint8Array:
    @staticmethod
    def new(value):
        return value


def _render_alias_card(counts):
    """Run repo_card_handler for the forkmesh/forkmesh alias of mirror2 with
    the given live counts; (response, rendered infos, cache puts, reads)."""
    rendered = []
    cache_puts = []
    count_reads = []

    async def repo_card_counts(_env, owner, repo):
        count_reads.append((owner, repo))
        return dict(counts)

    async def repo_is_private(_env, owner, repo):
        assert (owner, repo) == ("mirror2", "forkmesh")
        return False

    async def d1_first(_env, sql, *params):
        if "FROM repositories" in sql:
            assert params == ("repo-bi",)
            return {"data": "encrypted"}
        if "FROM repo_stars" in sql:
            return {"n": 7}
        raise AssertionError(sql)

    async def org_repo_node(_env, owner, repo):
        assert (owner, repo) == ("forkmesh", "forkmesh")
        return "mirror2"

    class Card:
        counts_token = staticmethod(real_card.counts_token)

        @staticmethod
        def render_repo_card(info, _now, logo=None):
            rendered.append(dict(info, logo=logo))
            return b"png"

    async def public_path(_env, owner, repo):
        return "/%s/%s" % (owner, repo)

    handler = _load_function("repo_card_handler", {
        "method_name": lambda request: request.method,
        "json_response": lambda *args, **kwargs: (args, kwargs),
        "ensure_schema": lambda _env: _async_none(),
        "_repo_is_private": repo_is_private,
        "urlparse": urlparse,
        "parse_qs": parse_qs,
        "re": re,
        "quote": quote,
        "REPO_CARD_RE": urls.REPO_CARD_RE,
        "safe_segment": catalog.safe_segment,
        "_org_repo_node": org_repo_node,
        "_ap_origin": lambda _env, _request=None: "https://forkmesh.com",
        "edge_cache_match_media": lambda *_args: _async_value(None),
        "blind_index": lambda _env, value: _async_value(
            "repo-bi" if value == "mirror2/forkmesh" else "wrong-bi"),
        "d1_first": d1_first,
        "decrypt_row": lambda _env, value: _async_value({
            "description": "Catalog from mirror2",
            "branch": "main",
            "issueCount": "0",
            "branchCount": "45",
        } if value == "encrypted" else {}),
        "_repo_card_counts": repo_card_counts,
        "_repo_card_accent": lambda _env: _async_value(""),
        "_ap_enabled": lambda _env: _async_value(False),
        "_decrypted_public_catalog": lambda *_args: _async_value([]),
        "repo_mirror_same_group": lambda *_args: False,
        "time": time,
        "og_card": Card,
        "unquote": unquote,
        "_repo_public_path": public_path,
        "_repo_card_logo": lambda *_args: _async_value(None),
        "instance_name": lambda _env: "ForkMesh",
        "JsResponse": _BinaryResponse,
        "Uint8Array": _Uint8Array,
        "_to_js": lambda value: value,
        "to_js": lambda value: value,
        "edge_cache_put": lambda key, response: _record_async(
            cache_puts, (key, response)),
    })
    request = SimpleNamespace(
        method="GET",
        url=(
            "https://forkmesh.com/api/repo/forkmesh/forkmesh/card.png"
            "?v=test-rev"),
        headers={"user-agent": "facebookexternalhit/1.1"},
    )
    response = _run(handler(None, request, "mirror2", "forkmesh"))
    return response, rendered, cache_puts, count_reads


def test_alias_card_renders_public_identity_after_internal_route_rewrite():
    """Card data comes from mirror2, but its visible title remains ForkMesh."""
    response, rendered, cache_puts, count_reads = _render_alias_card(
        {"issues": None, "branches": None, "contributors": None})

    assert response.status == 200
    # The counts are read from the backing repository, keyed by card version.
    assert count_reads == [("mirror2", "forkmesh")]
    assert rendered[0]["owner"] == "forkmesh"
    assert rendered[0]["repo"] == "forkmesh"
    assert rendered[0]["description"] == "Catalog from mirror2"
    assert rendered[0]["path"] == "/forkmesh/forkmesh"
    assert cache_puts[0][0] == (
        "https://forkmesh.com/api/repo/forkmesh/forkmesh/card.png"
        "?v=test-rev&n=i-.b-.c-")
    # Unreadable live counts fall back to the catalog record.
    assert rendered[0]["issues"] == "0" and rendered[0]["branches"] == "45"
    assert rendered[0]["contributors"] == 0


def test_card_counts_come_from_the_repository_not_the_catalog():
    """The ledger's 39 open issues and the 2 current branches win over the
    catalog's stale issueCount 0 / branchCount 45, and the cache key moves
    with them."""
    _, rendered, cache_puts, _ = _render_alias_card(
        {"issues": 39, "branches": 2, "contributors": 273})
    assert rendered[0]["issues"] == 39
    assert rendered[0]["branches"] == 2
    assert rendered[0]["contributors"] == 273
    assert cache_puts[0][0].endswith("?v=test-rev&n=i39.b2.c273")
    _, _, other_puts, _ = _render_alias_card(
        {"issues": 40, "branches": 2, "contributors": 273})
    assert other_puts[0][0] != cache_puts[0][0]


def _alias_repo_profile(uploaded_logo=True):
    reads = []
    rendered = []

    async def repo_federates(_env, owner, repo):
        reads.append(("federates", owner, repo))
        return True

    async def alias_owner(_env, owner, repo):
        reads.append(("alias", owner, repo))
        return "mirror2"

    async def blind_index(_env, value):
        reads.append(("catalog", value))
        return "repo-bi"

    async def d1_first(_env, sql, *params):
        if "FROM repositories" in sql:
            assert params == ("repo-bi",)
            return {"is_private": 0, "data": "repo-record"}
        if "FROM ap_followers" in sql:
            return {"c": 4}
        raise AssertionError(sql)

    async def d1_all(_env, sql, *params):
        if "FROM repo_media" in sql:
            return (
                [{"kind": "logo", "updated_at": 77}]
                if uploaded_logo else []
            )
        if "FROM ap_objects" in sql:
            return []
        raise AssertionError(sql)

    async def decrypt_row(_env, value):
        assert value == "repo-record"
        return {
            "owner": "mirror2",
            "name": "forkmesh",
            "visibility": "public",
            "description": "Canonical organization repository",
        }

    def profile_html(**kwargs):
        rendered.append(kwargs)
        return (
            "<html><body>"
            + kwargs["title"]
            + "|"
            + kwargs["icon_url"]
            + "|"
            + kwargs["code_url"]
            + "</body></html>"
        )

    page_class = _load_default_method("_serve_repo_profile_page", {
        "ap": SimpleNamespace(
            repo_handle=lambda owner, repo: owner + "." + repo,
            iso_utc=lambda value: str(value),
        ),
        "ensure_schema": lambda _env: _async_none(),
        "_ap_enabled": lambda _env: _async_value(True),
        "_ap_repo_federates": repo_federates,
        "_ap_org_alias_owner": alias_owner,
        "edge_cache_match": lambda _key: _async_value(None),
        "blind_index": blind_index,
        "d1_first": d1_first,
        "d1_all": d1_all,
        "decrypt_row": decrypt_row,
        "_catalog_record_matches_identity": (
            lambda record, owner, repo:
            record.get("owner") == owner and record.get("name") == repo
        ),
        "_repository_social_version": (
            lambda record: record.get("stateHash", "")
        ),
        "_ap_origin": lambda _env, _request=None: "https://forkmesh.com",
        "clean_string": catalog.clean_string,
        "MAX_NODE_NAME": 63,
        "quote": quote,
        "_ap_actor_bi": lambda *_args: _async_value("actor-bi"),
        "AP_ACTOR_REPO": "repo",
        "AP_AVATAR_PATH": "/assets/fediverse-avatar.png",
        "AP_BANNER_PATH": "/assets/fediverse-banner.png",
        "_html_escape": html.escape,
        "_ap_domain_of": lambda origin: urlparse(origin).netloc,
        "repo_web_href": lambda owner, repo: "/" + owner + "/" + repo,
        "_repo_fedi_profile_html": profile_html,
        "Response": _Response,
        "edge_cache_put": lambda *_args: _async_none(),
    })
    subject = page_class()
    subject.env = SimpleNamespace()
    response = _run(subject._serve_repo_profile_page(
        urlparse("https://forkmesh.com/@forkmesh.forkmesh"),
        "forkmesh",
        "forkmesh",
    ))
    return response, rendered[0], reads


def test_alias_repo_actor_profile_reads_backing_repo_but_keeps_public_branding():
    response, rendered, reads = _alias_repo_profile(uploaded_logo=True)
    assert response.status == 200
    assert rendered["title"] == "forkmesh/forkmesh"
    assert rendered["code_url"] == "https://forkmesh.com/forkmesh/forkmesh"
    assert rendered["icon_url"] == (
        "/api/repo/forkmesh/forkmesh/media/logo.png?v=77"
    )
    assert ("federates", "forkmesh", "forkmesh") in reads
    assert ("alias", "forkmesh", "forkmesh") in reads
    assert ("catalog", "mirror2/forkmesh") in reads
    assert "mirror2" not in response.body


def test_alias_repo_actor_profile_uses_valid_default_when_no_logo_is_uploaded():
    response, rendered, _reads = _alias_repo_profile(uploaded_logo=False)
    assert response.status == 200
    assert rendered["icon_url"] == (
        "/api/repo/forkmesh/forkmesh/logo?image=1"
    )


def test_native_repository_logo_image_projection_serves_approved_artwork():
    image_response = _load_function("_repository_logo_image_response", {
        "base64": __import__("base64"),
        "unquote": unquote,
        "json_response": lambda data, status=200, **_kwargs: SimpleNamespace(
            status=status, data=data),
        "JsResponse": _BinaryResponse,
        "Uint8Array": _Uint8Array,
        "_to_js": lambda value: value,
        "to_js": lambda value: value,
    })
    svg = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 8 8"></svg>'
    response = image_response({
        "contentType": "image/svg+xml",
        "dataUrl": "data:image/svg+xml;charset=utf-8," + quote(svg, safe=""),
    })
    assert response.status == 200
    assert response.headers["content-type"] == "image/svg+xml"
    assert bytes(response.body).startswith(b"<svg ")
    missing = image_response({"dataUrl": "data:text/plain,not-an-image"})
    assert missing.status == 404


async def _async_none():
    return None


async def _async_value(value):
    return value


async def _record_async(collection, value):
    collection.append(value)


def test_alias_html_stays_out_of_internal_path_rewrite():
    """The bare page identity is intentionally resolved inside page serving."""
    rewrite = ENTRY_TEXT[
        ENTRY_TEXT.index("async def org_alias_rewrite"):
        ENTRY_TEXT.index("\n\n\nclass _OrganizationSuccessionRuntime")
    ]
    assert "REPO_API_PREFIX_RE.match" in rewrite
    assert not re.search(
        r"(?:REPO_PAGE|DASHBOARD_REPO).*\\.match", rewrite)


def test_alias_stars_remain_scoped_to_public_repository_identity():
    handler = ENTRY_TEXT[
        ENTRY_TEXT.index("async def repo_star_handler"):
        ENTRY_TEXT.index("\n\nasync def fedi_comments_handler")
    ]
    assert "_public_repo_owner(\n        env, request, owner, repo, REPO_STAR_RE)" in handler
    assert 'blind_index(env, public_owner + "/" + repo)' in handler
    # The alias resolution itself is shared with the fork list, so both social
    # surfaces stay on the organization identity rather than its backing node.
    resolver = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _public_repo_owner"):
        ENTRY_TEXT.index("\n\nasync def _index_repo_fork_edge")
    ]
    assert "pattern.match(" in resolver
    assert "await _org_repo_node(" in resolver


def test_alias_fork_lists_remain_scoped_to_public_repository_identity():
    handler = ENTRY_TEXT[
        ENTRY_TEXT.index("async def repo_forks_handler"):
        ENTRY_TEXT.index("\n\nasync def repo_star_handler")
    ]
    assert "_public_repo_owner(\n        env, request, owner, repo, REPO_FORKS_RE)" in handler
    assert 'blind_index(env, public_owner + "/" + repo)' in handler


def test_a_canonical_short_page_titles_the_repository_without_its_owner():
    import sys
    sys.path.insert(0, str(ROOT / "src"))
    import repo_short_urls

    async def org_repo_node(_env, owner, repo):
        return "drew" if (owner, repo) == ("acme-bolt", "Bolt") else ""

    async def no(*_args):
        return False

    async def blind_index(_env, value):
        assert value == "drew/Bolt"
        return "repo-bi"

    async def d1_first(_env, sql, *params):
        return {"data": "encrypted"}

    async def decrypt_row(_env, value):
        return {"owner": "drew", "name": "Bolt", "visibility": "public",
                "description": "", "stateHash": "a" * 64}

    async def edge_cache_match(_key):
        raise AssertionError("a short page skips the shared cache")

    page_class = _load_default_method("_serve_repo_page", {
        "safe_segment": catalog.safe_segment,
        "unquote": unquote,
        "quote": quote,
        "_org_repo_node": org_repo_node,
        "_repo_is_private": no,
        "edge_cache_match": edge_cache_match,
        "edge_cache_put": no,
        "DASHBOARD_REPO_ASSET": "repo.html",
        "Response": _Response,
        "ensure_schema": no,
        "blind_index": blind_index,
        "d1_first": d1_first,
        "decrypt_row": decrypt_row,
        "_ap_origin": lambda _env, _request=None: "https://git.example.org",
        "_catalog_record_matches_identity": (
            lambda rec, owner, repo:
            rec.get("owner") == owner and rec.get("name") == repo),
        "_repository_social_version": lambda rec: rec.get("stateHash", ""),
        "_html_escape": html.escape,
        "og_card": SimpleNamespace(CARD_W=1200, CARD_H=630, CARD_REVISION="3"),
        "repo_short_urls": repo_short_urls,
    })

    def page(setting, name=""):
        subject = page_class()
        subject.env = SimpleNamespace(ASSETS=_Assets(), INSTANCE_NAME=name)

        async def short_setting():
            return setting

        subject._repo_short_setting = short_setting
        return _run(subject._serve_repo_page(
            SimpleNamespace(headers={}),
            urlparse("https://git.example.org/acme-bolt/Bolt"),
            "Bolt")).body

    body = page({"short": True, "owner": "acme-bolt", "canonical": True})
    assert '<meta property="og:title" content="Bolt - ForkMesh">' in body
    assert '<meta name="twitter:title" content="Bolt - ForkMesh">' in body
    assert 'content="Bolt on ForkMesh' in body
    assert '<link rel="canonical" href="https://git.example.org/Bolt">' in body
    assert '<meta property="og:url" content="https://git.example.org/Bolt">' in body
    assert ('<meta property="og:image" content="https://git.example.org/api/'
            'repo/acme-bolt/Bolt/card.png?v=' + "a" * 64 + '.3">') in body
    assert '<meta property="og:image:type" content="image/png">' in body
    assert '<meta property="og:image:alt" content="Bolt - ForkMesh">' in body
    assert '<meta name="twitter:image:alt" content="Bolt - ForkMesh">' in body
    branded = page({"short": True, "owner": "acme-bolt", "canonical": True},
                   "Acme <Code>")
    assert ('<meta property="og:site_name" content="Acme &lt;Code&gt;">'
            in branded)
    assert ('<meta property="og:title" content="Bolt - Acme &lt;Code&gt;">'
            in branded)
    assert 'ForkMesh' not in branded.split("</head>", 1)[0].split(
        '<link rel="canonical"', 1)[1]
    body = page({"short": True, "owner": "acme-bolt", "canonical": False})
    assert ('<meta property="og:title" content="acme-bolt/Bolt - ForkMesh">'
            in body)
