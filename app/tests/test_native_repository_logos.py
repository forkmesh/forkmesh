#!/usr/bin/env python3
"""Native catalog logo privacy and authorization contracts."""

import ast
import asyncio
import hashlib
import re
from pathlib import Path
from urllib.parse import parse_qs, quote, unquote, urlparse


SRC = Path(__file__).resolve().parents[1] / "src"
ENTRY = SRC / "entry.py"
CATALOG = SRC / "catalog.py"
SOURCE = (
    ENTRY.read_text(encoding="utf-8")
    + "\n"
    + CATALOG.read_text(encoding="utf-8")
)

FUNCTIONS = {
    "clean_string",
    "safe_segment",
    "_repo_identity_from_clone_url",
    "_catalog_record_matches_identity",
    "_repository_access_context",
    "_native_repository_logo_id",
    "_native_repository_logo_record",
    "_committed_repository_logo_url",
    "_uploaded_repository_logo_url",
    "_native_repository_logo_context",
}


def _load(extra):
    tree = ast.parse(SOURCE, filename=str(ENTRY))
    nodes = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in FUNCTIONS
    ]
    assert {node.name for node in nodes} == FUNCTIONS
    namespace = {
        "hashlib": hashlib,
        "re": re,
        "quote": quote,
        "unquote": unquote,
        "urlparse": urlparse,
        "ROOM_NAME_RE": re.compile(r"^[A-Za-z0-9._:-]+$"),
        "MAX_NODE_NAME": 100,
        "MAX_REPO_SEGMENT": 80,
        **extra,
    }
    module = ast.fix_missing_locations(ast.Module(body=nodes, type_ignores=[]))
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


class Request:
    def __init__(self, actor="", method="GET"):
        self.actor = actor
        self.method = method


def _harness(row):
    decryptions = []

    async def blind_index(_env, value):
        return "bi:" + str(value or "").strip().lower()

    async def d1_first(_env, sql, *_args):
        assert (
            "SELECT owner_bi, is_private, data FROM repositories" in sql
        )
        return dict(row) if row else None

    async def ensure_schema(_env):
        return None

    async def decrypt_row(_env, data):
        decryptions.append(data)
        return dict(data) if isinstance(data, dict) else None

    async def authed(_env, request):
        return request.actor

    async def owns(_env, actor, owner):
        return actor == owner == "alice"

    async def shared(_env, owner, repo, actor):
        return owner == "alice" and repo == "secret" and actor == "bob"

    async def basic_auth(_env, _owner, _repo, _request):
        return False

    namespace = _load({
        "blind_index": blind_index,
        "d1_first": d1_first,
        "ensure_schema": ensure_schema,
        "decrypt_row": decrypt_row,
        "_authed_account_name": authed,
        "_account_owns_node": owns,
        "_repo_shared_with": shared,
        "_basic_auth_view_ok": basic_auth,
    })
    return namespace["_native_repository_logo_context"], decryptions


def run(awaitable):
    return asyncio.run(awaitable)


def _record(visibility):
    return {
        "owner": "alice",
        "name": "secret",
        "visibility": visibility,
        "description": "bounded metadata",
        "logoMetadata": {
            "languages": {"C++": 12},
            "fileStructure": ["src/"],
        },
    }


def test_plaintext_private_gate_rejects_before_catalog_decryption():
    context, decryptions = _harness({
        "owner_bi": "bi:alice",
        "is_private": 1,
        "data": _record("private"),
    })

    assert run(context(None, Request(), "alice", "secret")) == (
        None, "", "")
    assert run(context(None, Request("mallory"), "alice", "secret")) == (
        None, "", "")
    assert decryptions == []

    record, repository_id, actor = run(
        context(None, Request("alice"), "alice", "secret"))
    assert record["isPrivate"] is True
    assert repository_id.startswith("native_")
    assert actor == "alice"
    assert len(decryptions) == 1


def test_private_logo_context_allows_explicitly_shared_collaborator():
    context, decryptions = _harness({
        "owner_bi": "bi:alice",
        "is_private": 1,
        "data": _record("private"),
    })

    record, repository_id, actor = run(
        context(None, Request("bob"), "alice", "secret"))
    assert record["isPrivate"] is True
    assert repository_id.startswith("native_")
    assert actor == "bob"
    assert len(decryptions) == 1


def test_missing_plaintext_visibility_fails_closed_before_decryption():
    context, decryptions = _harness({
        "owner_bi": "bi:alice",
        "is_private": None,
        "data": _record("public"),
    })

    assert run(context(None, Request(), "alice", "secret")) == (
        None, "", "")
    assert decryptions == []


def test_private_blind_owner_mismatch_is_not_a_decryption_oracle():
    context, decryptions = _harness({
        "owner_bi": "bi:other",
        "is_private": 1,
        "data": _record("private"),
    })

    assert run(context(
        None, Request("alice"), "alice", "secret")) == (None, "", "")
    assert decryptions == []


def test_encrypted_visibility_remains_authoritative_and_fails_closed():
    context, decryptions = _harness({
        # Historical migrations could leave this stale/public.
        "owner_bi": "bi:alice",
        "is_private": 0,
        "data": _record("private"),
    })

    assert run(context(None, Request(), "alice", "secret")) == (
        None, "", "")
    assert len(decryptions) == 1


def test_public_native_logo_context_uses_sanitized_catalog_factors():
    context, decryptions = _harness({
        "owner_bi": "bi:alice",
        "is_private": 0,
        "data": _record("public"),
    })

    record, repository_id, actor = run(
        context(None, Request(), "alice", "secret"))
    assert record["isPrivate"] is False
    assert record["metadata"]["languages"] == {"C++": 12}
    assert record["metadata"]["fileStructure"] == ["src/"]
    assert repository_id.startswith("native_")
    assert actor == ""
    assert len(decryptions) == 1


def test_committed_root_logo_is_the_repository_logo():
    namespace = _load({})
    record = namespace["_native_repository_logo_record"]({
        "owner": "alice",
        "name": "project",
        "visibility": "public",
        "commit": "a" * 40,
        "logoMetadata": {
            "fileStructure": ["README.md", "logo.png", "src/"],
        },
    }, "alice", "project")

    assert namespace["_committed_repository_logo_url"](
        record, "alice", "project"
    ) == (
        "/api/repo/alice/project/raw?path=logo.png&ref=" + "a" * 40
    )


def test_committed_logo_requires_public_repo_exact_commit_and_root_image():
    namespace = _load({})
    helper = namespace["_committed_repository_logo_url"]
    base = {
        "isPrivate": False,
        "commit": "b" * 40,
        "metadata": {"fileStructure": ["assets/logo.png"]},
    }
    assert helper(base, "alice", "project") == ""
    assert helper({**base, "metadata": {"fileStructure": ["logo.svg"]}},
                  "alice", "project") == ""
    assert helper({**base, "isPrivate": True,
                   "metadata": {"fileStructure": ["logo.png"]}},
                  "alice", "project") == ""
    assert helper({**base, "commit": "main",
                   "metadata": {"fileStructure": ["logo.png"]}},
                  "alice", "project") == ""


def test_unlisted_root_falls_back_to_the_conventional_logo_png():
    # Native publishers ship no fileStructure at all (the catalog fills in an
    # empty list), so gating on it hid every project's own committed logo.
    # With nothing listed, point at the conventional root logo.png; a repo
    # without one simply fails the raw read and keeps the repository icon.
    namespace = _load({})
    helper = namespace["_committed_repository_logo_url"]
    expected = "/api/repo/alice/project/raw?path=logo.png&ref=" + "c" * 40
    for metadata in ({"fileStructure": []}, {}, None):
        assert helper(
            {"isPrivate": False, "commit": "c" * 40, "metadata": metadata},
            "alice", "project") == expected
    # The gates that protect private rows still come first.
    assert helper(
        {"isPrivate": True, "commit": "c" * 40, "metadata": {}},
        "alice", "project") == ""
    assert helper(
        {"isPrivate": False, "commit": "", "metadata": {}},
        "alice", "project") == ""


def test_committed_root_logo_response_ships_approved_fallback_artwork():
    # The committed URL is streamed by a mirror, so it can fail after the card
    # rendered. The same response carries the owner-approved artwork so a
    # client swaps in place instead of showing a broken image.
    handler = SOURCE[
        SOURCE.index("async def native_repository_logo_handler")
        : SOURCE.index("async def _repo_about_public")
    ]
    committed = handler[handler.index('"source": "repository"') - 600:]
    assert '"fallbackDataUrl"' in committed
    assert "await service._official_logo(env, repository_id)" in committed


def test_logo_handler_never_falls_back_to_generated_artwork():
    # adhoc #23: repository cards showed a machine-generated abstract mark for
    # every project, including projects with a real logo.png at their root.
    handler = SOURCE[
        SOURCE.index("async def native_repository_logo_handler")
        : SOURCE.index("async def _repo_about_public")
    ]
    assert "deterministic_logo" not in handler
    assert "deterministic_logo" not in SOURCE
    # An image projection with no real logo hands back the ForkMesh avatar
    # rather than inventing one, so fediverse actor icons stay valid.
    assert '"location": AP_AVATAR_PATH' in handler


def test_owner_uploaded_logo_is_addressed_through_the_public_media_route():
    reads = []

    async def blind_index(_env, value):
        return "bi:" + str(value or "").lower()

    async def d1_first(_env, sql, *args):
        reads.append((sql, args))
        assert "FROM repo_media" in sql and "kind='logo'" in sql
        return {"updated_at": 77}

    helper = _load({
        "blind_index": blind_index, "d1_first": d1_first,
    })["_uploaded_repository_logo_url"]

    assert run(helper(None, {"isPrivate": False}, "alice", "project")) == (
        "/api/repo/alice/project/media/logo.png?v=77")
    assert reads[-1][1] == ("bi:alice/project",)
    # repo_media 404s for a private repository, so it is never even queried.
    assert run(helper(None, {"isPrivate": True}, "alice", "project")) == ""
    assert run(helper(None, {}, "alice", "project")) == ""
    assert len(reads) == 1


def _logo_handler(uploaded="", committed="", approved=None, aliases=None):
    """Run the GET side of the logo handler over one resolution outcome."""
    wanted = {"native_repository_logo_handler", "_public_repository_owner"}
    nodes = [
        node for node in ast.parse(SOURCE, filename=str(ENTRY)).body
        if isinstance(node, ast.AsyncFunctionDef) and node.name in wanted
    ]
    assert {node.name for node in nodes} == wanted
    nodes += [
        node for node in ast.parse(SOURCE, filename=str(ENTRY)).body
        if isinstance(node, ast.FunctionDef) and node.name == "safe_segment"
    ]
    committed_owners = []

    async def ensure_schema(_env):
        return None

    async def context(_env, _request, _owner, _repo):
        return {"isPrivate": False}, "native_opaque", ""

    async def uploaded_url(_env, _record, _owner, _repo):
        return uploaded

    async def org_repo_node(_env, org, repo):
        return (aliases or {}).get((org, repo), "")

    def committed_url(_record, url_owner, _repo):
        committed_owners.append(url_owner)
        return committed

    class Service:
        async def _official_logo(self, _env, _repository_id):
            return approved

        async def logo_for_record(self, _env, repository_id):
            return {"data": {"repositoryId": repository_id,
                             "logo": approved}}

    namespace = {
        "ensure_schema": ensure_schema,
        "_native_repository_logo_context": context,
        "_uploaded_repository_logo_url": uploaded_url,
        "_committed_repository_logo_url": committed_url,
        "_org_repo_node": org_repo_node,
        "REPO_API_PREFIX_RE": re.compile(
            r"^/api/repo/([^/]+)/([^/]+)(?:/.*)?$"),
        "ROOM_NAME_RE": re.compile(r"^[A-Za-z0-9._:-]+$"),
        "MAX_REPO_SEGMENT": 80,
        "unquote": unquote,
        "_repository_import_service": Service,
        "_repository_logo_image_response": lambda logo, public=True: {
            "image": logo, "public": public},
        "json_response": lambda data, status=200, **_kw: {
            "status": status, "data": data},
        "method_name": lambda request: request.method,
        "parse_qs": parse_qs,
        "urlparse": urlparse,
        "Response": lambda body, status=200, headers=None: {
            "status": status, "headers": dict(headers or {})},
        "AP_AVATAR_PATH": "/assets/fediverse-avatar.png",
    }
    module = ast.fix_missing_locations(ast.Module(body=nodes, type_ignores=[]))
    exec(compile(module, str(ENTRY), "exec"), namespace)

    def call(url, owner="alice", repo="project"):
        request = Request()
        request.url = url
        return run(namespace["native_repository_logo_handler"](
            None, request, owner, repo))

    call.committed_owners = committed_owners
    return call


def test_logo_resolution_prefers_owner_upload_then_committed_root_logo():
    approved = {"dataUrl": "data:image/png;base64,AAAA", "approved": True}
    url = "https://forkmesh.test/api/repo/alice/project/logo"

    uploaded = _logo_handler(
        uploaded="/api/repo/alice/project/media/logo.png?v=9",
        committed="/api/repo/alice/project/raw?path=logo.png&ref=" + "a" * 40,
        approved=approved)
    assert uploaded(url)["data"]["logo"]["dataUrl"] == (
        "/api/repo/alice/project/media/logo.png?v=9")

    committed = _logo_handler(
        committed="/api/repo/alice/project/raw?path=logo.png&ref=" + "a" * 40,
        approved=approved)
    logo = committed(url)["data"]["logo"]
    assert logo["source"] == "repository"
    # The root logo streams through a mirror that can be offline, so the
    # approved artwork rides along for the client to swap in.
    assert logo["fallbackDataUrl"] == approved["dataUrl"]

    assert _logo_handler(approved=approved)(url)["data"]["logo"] == approved
    assert _logo_handler()(url)["data"]["logo"] is None


def test_logo_image_projection_never_synthesizes_a_mark():
    url = "https://forkmesh.test/api/repo/alice/project/logo?image=1"
    committed = "/api/repo/alice/project/raw?path=logo.png&ref=" + "a" * 40

    assert _logo_handler(uploaded="/api/repo/alice/project/media/logo.png?v=9")(
        url) == {"status": 302, "headers": {
            "location": "/api/repo/alice/project/media/logo.png?v=9",
            "cache-control": "public, max-age=300"}}
    assert _logo_handler(committed=committed)(url)["headers"]["location"] == (
        committed)

    approved = {"dataUrl": "data:image/png;base64,AAAA"}
    assert _logo_handler(approved=approved)(url)["image"] == approved
    # Nothing real to serve: the ForkMesh avatar, never generated artwork.
    assert _logo_handler()(url)["headers"]["location"] == (
        "/assets/fediverse-avatar.png")


def test_committed_logo_url_keeps_the_organization_the_caller_addressed():
    # adhoc #122: org_alias_rewrite hands the handler the backing node, and the
    # avatar redirect was built from it. /api/repo/<node>/<repo>/raw is pinned
    # to that node's row, so once the node went offline every Mastodon instance
    # that followed the flagship repo's icon got mirror_unavailable and drew
    # the default placeholder. The organization URL routes the whole group.
    handler = _logo_handler(
        committed="/api/repo/forkmesh/forkmesh/raw?path=logo.png&ref="
                  + "a" * 40,
        aliases={("forkmesh", "forkmesh"): "mirror23"})
    result = handler(
        "https://forkmesh.test/api/repo/forkmesh/forkmesh/logo?image=1",
        owner="mirror23", repo="forkmesh")
    assert handler.committed_owners == ["forkmesh"]
    assert result["headers"]["location"] == (
        "/api/repo/forkmesh/forkmesh/raw?path=logo.png&ref=" + "a" * 40)

    # A name the alias table does not map to this node is never substituted,
    # and a plain node URL keeps the node.
    unrelated = _logo_handler(committed="/committed.png")
    unrelated("https://forkmesh.test/api/repo/mallory/forkmesh/logo?image=1",
              owner="mirror23", repo="forkmesh")
    assert unrelated.committed_owners == ["mirror23"]
    direct = _logo_handler(committed="/committed.png")
    direct("https://forkmesh.test/api/repo/mirror23/forkmesh/logo?image=1",
           owner="mirror23", repo="forkmesh")
    assert direct.committed_owners == ["mirror23"]


def test_native_logo_handler_normalizes_missing_and_unauthorized_to_404():
    handler = SOURCE[
        SOURCE.index("async def native_repository_logo_handler")
        : SOURCE.index("async def _repo_about_public")
    ]
    assert '{"error": "not_found"}' in handler
    assert "status=404" in handler
    assert 'cache_control="no-store"' in handler


def test_private_collaborator_can_suggest_but_cannot_replace_or_review():
    tree = ast.parse(SOURCE, filename=str(ENTRY))
    nodes = [
        node for node in tree.body
        if isinstance(node, ast.AsyncFunctionDef)
        and node.name == "native_repository_logo_handler"
    ]
    assert len(nodes) == 1

    async def ensure_schema(_env):
        return None

    async def context(_env, _request, _owner, _repo):
        return {"isPrivate": True}, "native_opaque", "bob"

    async def owns(_env, actor, owner):
        return actor == owner == "alice"

    async def moderator(_env, actor):
        return actor == "mod"

    def json_response(data, status=200, **_kwargs):
        return {"status": status, "data": data}

    class Service:
        async def native_logo_suggestions(
                self, _env, _request, repository_id, record, can_admin):
            return {
                "repositoryId": repository_id,
                "private": record["isPrivate"],
                "collaboratorMayReview": await can_admin("bob"),
                "ownerMayReview": await can_admin("alice"),
                "moderatorMayReview": await can_admin("mod"),
            }

    namespace = {
        "ensure_schema": ensure_schema,
        "_native_repository_logo_context": context,
        "_account_owns_node": owns,
        "_repo_admin_allowed": (
            lambda env, actor, owner, repo: owns(env, actor, owner)),
        "_repository_import_moderator": moderator,
        "_repository_import_service": Service,
        "json_response": json_response,
    }
    module = ast.fix_missing_locations(
        ast.Module(body=nodes, type_ignores=[]))
    exec(compile(module, str(ENTRY), "exec"), namespace)
    result = run(namespace["native_repository_logo_handler"](
        None, Request("bob", method="POST"), "alice", "secret",
        suggestions=True))

    assert result == {
        "repositoryId": "native_opaque",
        "private": True,
        "collaboratorMayReview": False,
        "ownerMayReview": True,
        "moderatorMayReview": True,
    }
