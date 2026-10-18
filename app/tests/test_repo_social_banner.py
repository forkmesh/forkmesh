#!/usr/bin/env python3
"""Repository social-header contracts.

The ForkMesh brand banner (`/assets/fediverse-banner.png`) is *this instance's*
identity. It used to be the default header for every repository, so the About
card, the repo's fediverse profile page and its ActivityPub actor document all
flew ForkMesh colours over unrelated projects (a repo you had never heard of
looked like it was ours). A repository's header now comes from the repository:
the owner's uploaded banner, else the `.forkmesh/banner.png` it committed, else
nothing at all.

Run: python3 -m pytest app/tests/test_repo_social_banner.py
"""

import ast
import asyncio
import json
import re
from pathlib import Path
from types import SimpleNamespace
import sys
from urllib.parse import parse_qs, quote, urlparse

ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = ROOT.parent
sys.path.insert(0, str(ROOT / "src"))
import instance_features  # noqa: E402
ENTRY = ROOT / "src" / "entry.py"
FEDIVERSE_ROUTES = ROOT / "src" / "fediverse_routes.py"
DASHBOARD_JS = ROOT / "public" / "dashboard.js"
REPO_CONTENT_JS = ROOT / "public" / "dashboard" / "js" / "06-repo-content.js"
REPO_DETAIL_JS = (
    ROOT / "public" / "dashboard" / "js" / "08-repo-detail-network.js")
BRAND_BANNER = REPO_ROOT / "www" / "public" / "assets" / "fediverse-banner.png"
COMMITTED_BANNER = REPO_ROOT / ".forkmesh" / "banner.png"

ENTRY_TEXT = (
    # entry.py + its lazily-split domain modules
    (ENTRY.parent / "app_runtime.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
)
# entry.py plus the lazily-split fediverse domain module, which reads the
# banner resolver out of the runtime namespace entry.py binds into it.
SOURCE = (
    FEDIVERSE_ROUTES.read_text(encoding="utf-8") + "\n\n\n" + ENTRY_TEXT)

BANNER_RAW_PATH = ".forkmesh%2Fbanner.png"


# Module-level constants the loaded helpers close over, pulled from the real
# source so the tests pin the shipped path rather than a copy of it.
CONSTANTS = ("COMMITTED_BANNER_DIR", "COMMITTED_BANNER_NAME")


def _constant_nodes(tree):
    found = [
        node for node in tree.body
        if isinstance(node, ast.Assign)
        and len(node.targets) == 1
        and isinstance(node.targets[0], ast.Name)
        and node.targets[0].id in CONSTANTS
    ]
    assert {node.targets[0].id for node in found} == set(CONSTANTS)
    return found


def _load(*names, extra_globals=None):
    tree = ast.parse(SOURCE, filename=str(ENTRY))
    selected = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in names
    ]
    missing = set(names) - {node.name for node in selected}
    assert not missing, "missing functions: %s" % sorted(missing)
    namespace = dict(extra_globals or {})
    namespace.setdefault("instance_features", instance_features)
    exec(compile(ast.fix_missing_locations(ast.Module(
        body=_constant_nodes(tree) + selected, type_ignores=[])),
        str(ENTRY), "exec"), namespace)
    return namespace


def _load_method(class_name, name, extra_globals=None):
    tree = ast.parse(SOURCE, filename=str(ENTRY))
    owner = next(
        node for node in tree.body
        if isinstance(node, ast.ClassDef) and node.name == class_name)
    method = next(
        node for node in owner.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name == name)
    subject = ast.ClassDef(
        name="_Subject", bases=[], keywords=[], body=[method],
        decorator_list=[])
    namespace = dict(extra_globals or {})
    namespace.setdefault("instance_features", instance_features)
    exec(compile(ast.fix_missing_locations(ast.Module(
        body=_constant_nodes(tree) + [subject], type_ignores=[])),
        str(ENTRY), "exec"), namespace)
    return namespace["_Subject"]


def _run(coro):
    return asyncio.new_event_loop().run_until_complete(coro)


def _resolver():
    return _load("_committed_repository_banner_url", extra_globals={
        "re": re,
        "quote": quote,
        "clean_string": lambda value, cap: str(value or "")[:cap],
    })["_committed_repository_banner_url"]


# --- the resolver -------------------------------------------------------------

def test_committed_banner_is_read_from_the_repos_own_forkmesh_directory():
    helper = _resolver()
    assert helper(
        {"visibility": "public", "commit": "a" * 40,
         "metadata": {"fileStructure": ["README.md", ".forkmesh", "src"]}},
        "alice", "project",
    ) == (
        "/api/repo/alice/project/raw?path=" + BANNER_RAW_PATH
        + "&ref=" + "a" * 40
    )


def test_committed_banner_is_pinned_to_the_published_commit():
    # The immutable ref doubles as the cache-buster: pushing new artwork
    # changes the URL, so Mastodon and the browser both refetch.
    helper = _resolver()
    first = helper(
        {"visibility": "public", "commit": "a" * 40, "metadata": {}},
        "alice", "project")
    second = helper(
        {"visibility": "public", "commit": "b" * 64, "metadata": {}},
        "alice", "project")
    assert first != second
    assert first.endswith("&ref=" + "a" * 40)
    assert second.endswith("&ref=" + "b" * 64)


def test_repo_without_a_forkmesh_directory_has_no_banner():
    # Catalog metadata lists top-level names only. It can rule the banner out
    # (no .forkmesh at all) but never confirm the file inside it.
    helper = _resolver()
    assert helper(
        {"visibility": "public", "commit": "a" * 40,
         "metadata": {"fileStructure": ["README.md", "src"]}},
        "alice", "project") == ""


def test_unlisted_structure_still_offers_the_conventional_banner():
    # Native publishers ship no fileStructure (the catalog fills in an empty
    # list), so gating on it would hide every ForkMesh-hosted repo's own
    # banner. Offer the candidate; a repo without one 404s and shows none.
    helper = _resolver()
    expected = (
        "/api/repo/alice/project/raw?path=" + BANNER_RAW_PATH
        + "&ref=" + "c" * 40)
    for metadata in ({"fileStructure": []}, {}, None):
        assert helper(
            {"visibility": "public", "commit": "c" * 40,
             "metadata": metadata},
            "alice", "project") == expected


def test_committed_banner_requires_a_public_repo_and_an_exact_commit():
    helper = _resolver()
    base = {"visibility": "public", "commit": "d" * 40, "metadata": {}}
    assert helper(base, "alice", "project")
    # Fail closed: private rows, unknown visibility, and anything that is not
    # an exact object id (a branch name is mutable) publish no banner.
    assert helper({**base, "isPrivate": True}, "alice", "project") == ""
    assert helper({**base, "visibility": "private"}, "alice", "project") == ""
    assert helper({"commit": "d" * 40}, "alice", "project") == ""
    assert helper({**base, "commit": "main"}, "alice", "project") == ""
    assert helper({**base, "commit": ""}, "alice", "project") == ""
    assert helper(None, "alice", "project") == ""


def test_banner_path_is_query_encoded_for_the_raw_endpoint():
    # `path` rides in the query string, where the dashboard's URLSearchParams
    # readers percent-encode the separator. Emitting a bare "/" here would not
    # round-trip through the same raw route.
    helper = _resolver()
    url = helper(
        {"visibility": "public", "commit": "e" * 40, "metadata": {}},
        "alice", "project")
    assert "path=.forkmesh/banner.png" not in url
    assert parse_qs(urlparse(url).query)["path"] == [".forkmesh/banner.png"]


# --- GET /api/repo/{o}/{r}/about ---------------------------------------------

def _about(uploaded_banner=False, record=None, catalog_records=()):
    stored = dict(record or {
        "description": "A federated repo",
        "website": "",
        "visibility": "public",
        "commit": "f" * 40,
        "metadata": {},
    })

    async def _repo_is_private(_env, _owner, _repo):
        return False

    async def blind_index(_env, value):
        return "bi:" + str(value or "")

    async def d1_first(_env, sql, *_args):
        if "FROM repositories" in sql:
            return {"data": stored}
        if "COUNT(*)" in sql:
            return {"c": 0}
        return None

    async def d1_all(_env, sql, *_args):
        if "FROM repo_media" in sql:
            return ([{"kind": "banner", "updated_at": 42}]
                    if uploaded_banner else [])
        return []

    async def decrypt_row(_env, data):
        return dict(data)

    async def _decrypted_public_catalog(_env, _now):
        return [
            {"is_private": 0, "data": dict(candidate)}
            for candidate in catalog_records
        ]

    async def _ap_enabled(_env):
        return False

    async def _ap_repo_settings_get(_env, _owner, _repo):
        return {"federate": True, "broadcastEvents": True,
                "acceptComments": True}

    namespace = _load(
        "_repo_about_public", "_committed_repository_banner_url",
        "_freshest_public_repository_group_record",
        extra_globals={
            "re": re,
            "json": json,
            "quote": quote,
            "urlparse": urlparse,
            "clean_string": lambda value, cap: str(value or "")[:cap],
            "safe_segment": lambda value: value,
            "json_response": lambda data, status=200, **_kwargs: {
                "status": status, "data": data},
            "_repo_is_private": _repo_is_private,
            "blind_index": blind_index,
            "d1_first": d1_first,
            "d1_all": d1_all,
            "decrypt_row": decrypt_row,
            "_decrypted_public_catalog": _decrypted_public_catalog,
            "repo_mirror_same_group": lambda left, right: (
                left.get("groupKey") == right.get("groupKey")
            ),
            "Date": SimpleNamespace(now=lambda: 2_000_000_000_000),
            "_ap_enabled": _ap_enabled,
            "_ap_repo_settings_get": _ap_repo_settings_get,
            "_ap_origin": lambda _env, _request=None: "https://forkmesh.com",
            "_ap_domain_of": lambda _origin: "forkmesh.com",
            "_ap_actor_url": lambda origin, _kind, handle: (
                origin + "/ap/repos/" + handle),
            "ap": SimpleNamespace(
                repo_handle=lambda owner, repo: owner + "." + repo),
            "REPO_ABOUT_RE": re.compile(
                r"^/api/repo/([^/]+)/([^/]+)/about/?$"),
            "AP_ACTOR_REPO": "repo",
            "AP_AVATAR_PATH": "/assets/fediverse-avatar.png",
        })
    request = SimpleNamespace(
        url="https://forkmesh.com/api/repo/alice/project/about")
    return _run(namespace["_repo_about_public"](
        None, request, "alice", "project"))


def test_about_serves_the_repos_committed_banner():
    body = _about()["data"]
    assert body["bannerUrl"] == (
        "/api/repo/alice/project/raw?path=" + BANNER_RAW_PATH
        + "&ref=" + "f" * 40)


def test_about_prefers_owner_uploaded_artwork_over_the_commit():
    body = _about(uploaded_banner=True)["data"]
    assert body["bannerUrl"] == (
        "/api/repo/alice/project/media/banner.png?v=42")


def test_about_committed_media_tracks_the_freshest_group_commit():
    body = _about(
        record={
            "description": "", "website": "", "visibility": "public",
            "commit": "a" * 40, "updatedAt": 100, "groupKey": "g",
            "metadata": {"fileStructure": [".forkmesh"]},
        },
        catalog_records=[{
            "visibility": "public", "commit": "b" * 40,
            "updatedAt": 200, "groupKey": "g",
            "metadata": {"fileStructure": [".forkmesh"]},
        }],
    )["data"]
    assert body["bannerUrl"].endswith("&ref=" + "b" * 40)


def test_about_advertises_no_default_banner():
    # The old `defaultBannerUrl` was the brand banner, and the dashboard's
    # `bannerUrl || defaultBannerUrl` chain is what painted it onto every repo.
    body = _about(record={
        "description": "", "website": "", "visibility": "public",
        "commit": "", "metadata": {},
    })["data"]
    assert "defaultBannerUrl" not in body
    assert body["bannerUrl"] == ""
    # The avatar keeps its brand fallback: an anonymous default avatar claims
    # nothing about a project, a branded header does.
    assert body["defaultLogoUrl"] == "/assets/fediverse-avatar.png"
    assert "AP_BANNER_PATH" not in ENTRY_TEXT[
        ENTRY_TEXT.index("async def _repo_about_public")
        : ENTRY_TEXT.index("async def repo_about_handler")]


# --- the ActivityPub actor document -----------------------------------------

def _actor_doc(kind, handle, media=(), record=None):
    import importlib.util as ilu
    spec = ilu.spec_from_file_location(
        "activitypub", ROOT / "src" / "activitypub.py")
    activitypub = ilu.module_from_spec(spec)
    spec.loader.exec_module(activitypub)

    stored = dict(record or {
        "description": "A federated repo",
        "visibility": "public",
        "commit": "a" * 40,
        "metadata": {},
    })

    async def blind_index(_env, value):
        return "bi:" + str(value or "")

    async def d1_first(_env, sql, *_args):
        if "FROM repositories" in sql:
            return {"is_private": 0, "data": stored}
        return None

    async def d1_all(_env, _sql, *_args):
        return list(media)

    async def decrypt_row(_env, data):
        return dict(data)

    async def _ap_user_federates(_env, _handle):
        return True

    async def _account_row(_env, _handle):
        return None, {"profile_bio": ""}

    async def _ap_org_alias_owner(_env, owner, _repo):
        return owner

    namespace = _load(
        "_ap_build_actor_doc", "_committed_repository_banner_url",
        extra_globals={
            "re": re,
            "quote": quote,
            "ap": activitypub,
            "clean_string": lambda value, cap=0: (
                str(value or "")[:cap] if cap else str(value or "")),
            "blind_index": blind_index,
            "d1_first": d1_first,
            "d1_all": d1_all,
            "decrypt_row": decrypt_row,
            "_ap_user_federates": _ap_user_federates,
            "_account_row": _account_row,
            "_ap_org_alias_owner": _ap_org_alias_owner,
            "_ap_domain_of": lambda origin: "forkmesh.com",
            "_ap_actor_url": lambda origin, kind, handle: (
                origin + "/ap/" + kind + "/" + handle),
            "_repository_social_version": lambda record: "",
            "repo_web_href": lambda owner, repo: "/%s/%s" % (owner, repo),
            "AP_ACTOR_INSTANCE": "instance",
            "AP_ACTOR_USER": "user",
            "AP_ACTOR_REPO": "repo",
            "AP_AVATAR_PATH": "/assets/fediverse-avatar.png",
            "AP_BANNER_PATH": "/assets/fediverse-banner.png",
        })
    return _run(namespace["_ap_build_actor_doc"](
        None, "https://forkmesh.com", kind, handle,
        {"pubkeyPem": "PEM", "createdAt": 123}))


def test_repo_actor_ships_its_own_committed_banner():
    doc = _actor_doc("repo", "alice.project")
    assert doc["image"]["url"] == (
        "https://forkmesh.com/api/repo/alice/project/raw?path="
        + BANNER_RAW_PATH + "&ref=" + "a" * 40)


def test_repo_actor_prefers_uploaded_artwork_over_the_commit():
    doc = _actor_doc(
        "repo", "alice.project",
        media=[{"kind": "banner", "updated_at": 9}])
    assert doc["image"]["url"] == (
        "https://forkmesh.com/api/repo/alice/project/media/banner.png?v=9")


def test_repo_actor_without_artwork_ships_no_header_at_all():
    # Not the brand banner: every fediverse client renders a missing `image`
    # as a plain header, which is honest about the repo having none.
    doc = _actor_doc("repo", "alice.project", record={
        "description": "", "visibility": "public", "commit": "",
        "metadata": {"fileStructure": ["README.md"]},
    })
    assert "image" not in doc
    # The avatar still resolves through the repository logo API.
    assert doc["icon"]["url"] == (
        "https://forkmesh.com/api/repo/alice/project/logo?image=1")


def test_instance_and_user_actors_keep_the_brand_banner():
    # There the brand banner IS the subject's identity.
    for kind, handle in (("instance", "instance"), ("user", "alice")):
        doc = _actor_doc(kind, handle)
        assert doc["image"]["url"] == (
            "https://forkmesh.com/assets/fediverse-banner.png")


# --- the repo's fediverse profile page --------------------------------------

def _profile_page(media=(), record=None):
    rendered = {}
    stored = dict(record or {
        "owner": "alice", "name": "project", "description": "A repo",
        "visibility": "public", "commit": "a" * 40, "metadata": {},
    })

    async def _ap_enabled(_env):
        return True

    async def _ap_repo_federates(_env, _owner, _repo):
        return True

    async def _ap_org_alias_owner(_env, owner, _repo):
        return owner

    async def edge_cache_match(_key):
        return None

    async def edge_cache_put(*_args):
        return None

    async def blind_index(_env, value):
        return "bi:" + str(value or "")

    async def d1_first(_env, sql, *_args):
        if "FROM repositories" in sql:
            return {"is_private": 0, "data": stored}
        return {"c": 0}

    async def d1_all(_env, sql, *_args):
        if "FROM repo_media" in sql:
            return list(media)
        return []

    async def decrypt_row(_env, data):
        return dict(data)

    async def _ap_actor_bi(*_args):
        return "actor-bi"

    async def ensure_schema(_env):
        return None

    def profile_html(**kwargs):
        rendered.update(kwargs)
        return "<html></html>"

    subject = _load_method("Default", "_serve_repo_profile_page", {
        "instance_features": instance_features,
        "re": re,
        "quote": quote,
        "clean_string": lambda value, cap=0: (
            str(value or "")[:cap] if cap else str(value or "")),
        "ensure_schema": ensure_schema,
        "blind_index": blind_index,
        "d1_first": d1_first,
        "d1_all": d1_all,
        "decrypt_row": decrypt_row,
        "_ap_enabled": _ap_enabled,
        "_ap_repo_federates": _ap_repo_federates,
        "_ap_org_alias_owner": _ap_org_alias_owner,
        "_ap_origin": lambda _env, _request=None: "https://forkmesh.com",
        "_ap_domain_of": lambda origin: "forkmesh.com",
        "_ap_actor_bi": _ap_actor_bi,
        "_ap_actor_url": lambda origin, kind, handle: (
            origin + "/ap/" + kind + "/" + handle),
        "_catalog_record_matches_identity": lambda record, owner, repo: True,
        "_repository_social_version": lambda record: "",
        "_committed_repository_banner_url": _resolver(),
        "_repo_fedi_profile_html": lambda title, bio, handle_full, code_url,
        actor_json, icon_url, image_url, followers, post_count, posts_html: (
            profile_html(image_url=image_url, icon_url=icon_url)),
        "_html_escape": lambda value: str(value),
        "repo_web_href": lambda owner, repo: "/%s/%s" % (owner, repo),
        "edge_cache_match": edge_cache_match,
        "edge_cache_put": edge_cache_put,
        "Response": lambda body, status=200, headers=None: SimpleNamespace(
            body=body, status=status, headers=headers or {}),
        "ap": SimpleNamespace(
            repo_handle=lambda owner, repo: owner + "." + repo,
            iso_utc=lambda ms: "1970-01-01T00:00:00Z"),
        "AP_ACTOR_REPO": "repo",
        "AP_AVATAR_PATH": "/assets/fediverse-avatar.png",
    })()
    subject.env = SimpleNamespace()
    _run(subject._serve_repo_profile_page(
        urlparse("https://forkmesh.com/@alice.project"), "alice", "project"))
    return rendered


def test_profile_page_header_is_the_repos_committed_banner():
    assert _profile_page()["image_url"] == (
        "/api/repo/alice/project/raw?path=" + BANNER_RAW_PATH
        + "&ref=" + "a" * 40)


def test_profile_page_header_is_empty_without_repo_artwork():
    rendered = _profile_page(record={
        "owner": "alice", "name": "project", "description": "",
        "visibility": "public", "commit": "",
        "metadata": {"fileStructure": ["README.md"]},
    })
    assert rendered["image_url"] == ""


def test_profile_page_html_omits_the_background_when_there_is_no_banner():
    namespace = _load("_repo_fedi_profile_html", extra_globals={})
    render = namespace["_repo_fedi_profile_html"]
    args = ("t", "b", "@a.p", "/a/p", "/ap/repos/a/p",
            "/icon.png", "", 0, 0, "")
    assert '<div class="banner"></div>' in render(*args)
    with_banner = render(*args[:6], "/banner.png", *args[7:])
    assert "background-image:url('/banner.png')" in with_banner
    assert "fediverse-banner.png" not in render(*args)


# --- the dashboard About card ------------------------------------------------

def test_dashboard_has_no_brand_banner_fallback():
    for source in (REPO_CONTENT_JS.read_text(encoding="utf-8"),
                   DASHBOARD_JS.read_text(encoding="utf-8")):
        assert "defaultBannerUrl" not in source
        assert "fediverse-banner.png" not in source


def test_dashboard_probes_the_banner_before_painting_it():
    # The relay can only offer the committed banner as a candidate, so the card
    # must confirm the image loads; otherwise a repo with a `.forkmesh`
    # directory but no banner keeps an empty grey strip forever.
    source = REPO_CONTENT_JS.read_text(encoding="utf-8")
    helper = source[
        source.index("function applyRepoSocialBanner")
        : source.index("async function loadRepoFediverse")]
    assert "new Image()" in helper
    assert "probe.onload" in helper
    assert 'banner.classList.toggle("hidden", !shown)' in helper
    # The logo only overlaps a header that is actually there.
    assert 'logo?.classList.toggle("-mt-7", shown)' in helper
    assert 'identity?.classList.toggle("pt-3", !shown)' in helper
    # A probe that resolves after the reader opened another repo must not
    # paint the previous project's artwork onto the card now on screen.
    assert "if (!repoAboutStillCurrent(repo)) return;" in helper
    assert 'applyRepoSocialBanner(repo, String(body.bannerUrl || ""))' in source


def test_about_rail_shows_no_banner_or_logo_header():
    markup = REPO_DETAIL_JS.read_text(encoding="utf-8")
    assert "data-repo-social-badge" not in markup
    assert "data-repo-social-banner" not in markup
    assert "data-repo-social-logo" not in markup


# --- ForkMesh's own repository ----------------------------------------------

def test_forkmesh_ships_its_banner_in_its_own_forkmesh_directory():
    # The header shown on forkmesh/forkmesh is now committed artwork like any
    # other project's, byte-identical to the brand asset it replaced.
    assert COMMITTED_BANNER.is_file()
    assert (COMMITTED_BANNER.read_bytes()
            == BRAND_BANNER.read_bytes())
    header = COMMITTED_BANNER.read_bytes()[:24]
    assert header[:8] == b"\x89PNG\r\n\x1a\n"
    # Mastodon's exact header size, so the same file serves the actor `image`.
    assert int.from_bytes(header[16:20], "big") == 1500
    assert int.from_bytes(header[20:24], "big") == 500
