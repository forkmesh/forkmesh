#!/usr/bin/env python3
"""Edge-cache contracts for the unauthenticated ActivityPub read endpoints.

Fediverse servers and crawlers hit webfinger/nodeinfo/actor/collection/object
GETs in bursts, and every hit used to reach D1 — the pressure behind the
"Too many requests" failures on the federated repo (issue #416, following
adhoc #9). These tests load the real handlers out of src/entry.py and pin
the collapse behaviour:

  * a cold hit computes the document and stores it under its canonical public
    URL; positive warm hits revalidate the privacy/federation gate before using
    cache so an opt-out or private-repo transition revokes immediately;
  * not-found responses (a non-federating actor) are parked briefly under the
    same key, so probe storms for dead handles collapse too; transient
    errors (503) are never stored;
  * webfinger keys on the parsed acct resource, so different spellings of
    the same handle share one entry;
  * the Update(actor) broadcast drops the cached actor doc so follower
    refetches see the new profile.

Run: python3 -m pytest cloudflare_worker/tests/test_ap_edge_cache.py
"""

import ast
import asyncio
import json
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import parse_qs, quote, urlparse

ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
ENTRY_TEXT = (
    # entry.py + its lazily-split domain modules
    (ENTRY.parent / "forkbot.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "fediverse_routes.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
)

# The repo-actor `url` test exercises the real document builders.
import importlib.util as _ilu  # noqa: E402
_ap_spec = _ilu.spec_from_file_location(
    "activitypub", ROOT / "src" / "activitypub.py")
_ap = _ilu.module_from_spec(_ap_spec)
_ap_spec.loader.exec_module(_ap)


def _load(*names, extra_globals=None):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    selected = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in names
    ]
    found = {n.name for n in selected}
    missing = set(names) - found
    assert not missing, "missing functions: %s" % sorted(missing)
    module = ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[]))
    namespace = dict(extra_globals or {})
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


def _run(coro):
    return asyncio.new_event_loop().run_until_complete(coro)


class FakeResponse:
    def __init__(self, data, status=200, headers=None):
        self.data = data
        self.status = status
        self.headers = headers or {}


class FakeEdgeCache:
    """In-memory stand-in for the edge_cache_* helpers."""

    def __init__(self):
        self.store = {}
        self.puts = []

    async def match(self, key):
        return self.store.get(key)

    async def put(self, key, response):
        self.puts.append(key)
        self.store[key] = response

    async def delete(self, key):
        self.store.pop(key, None)


def _json_response(data, status=200, cache_seconds=None, cache_control=None,
                   extra_headers=None):
    return FakeResponse(data, status=status, headers=dict(extra_headers or {}))


def _base_globals(edge, d1_log):
    async def ensure_schema(env):
        d1_log.append("ensure_schema")

    async def _ap_enabled(env):
        return True

    async def d1_first(env, sql, *args):
        d1_log.append(sql)
        return {"c": 3}

    def _ap_actor_url(origin, kind, handle):
        if kind == "instance":
            return origin + "/ap/actor"
        if kind == "user":
            return origin + "/ap/users/" + handle
        owner, _, repo = handle.partition(".")
        return origin + "/ap/repos/%s/%s" % (owner, repo)

    return {
        "asyncio": asyncio,
        "json": json,
        "parse_qs": parse_qs,
        "urlparse": urlparse,
        "method_name": lambda request: request.method,
        "json_response": _json_response,
        "ensure_schema": ensure_schema,
        "_ap_enabled": _ap_enabled,
        "_ap_disabled_response": lambda: FakeResponse(
            {"error": "not_found"}, status=404),
        "_ap_origin": lambda env, request=None: "https://forkmesh.com",
        "_ap_domain_of": lambda origin: "forkmesh.com",
        "_ap_actor_url": _ap_actor_url,
        "d1_first": d1_first,
        "edge_cache_match": edge.match,
        "edge_cache_put": edge.put,
        "edge_cache_delete": edge.delete,
        "AP_ACTOR_INSTANCE": "instance",
        "AP_ACTOR_USER": "user",
        "AP_ACTOR_REPO": "repo",
        "AP_INSTANCE_HANDLE": "forkmesh",
    }


def _fake_request(url, method="GET"):
    return SimpleNamespace(url=url, method=method)


# --- Collections ---------------------------------------------------------------

def _collection_env(edge, d1_log, federates=True, followers_public=False):
    globs = _base_globals(edge, d1_log)

    async def _ap_user_federates(env, handle):
        return federates

    async def _ap_repo_federates(env, owner, repo):
        return federates

    async def _ap_actor_bi(env, kind, handle):
        return "bi:" + handle

    async def _account_row(env, name):
        return "bi:" + name, {"profile_followers_public": followers_public}

    async def d1_all(env, sql, *args):
        d1_log.append(sql)
        return []

    globs.update({
        "_ap_user_federates": _ap_user_federates,
        "_ap_repo_federates": _ap_repo_federates,
        "_ap_actor_bi": _ap_actor_bi,
        "_account_row": _account_row,
        "d1_all": d1_all,
        "AP_COLLECTION_PAGE_SIZE": 200,
        "ap": SimpleNamespace(
            collection_doc=lambda url, total, items=None: {
                "id": url, "totalItems": total, "items": items},
            ACTIVITY_CONTENT_TYPE="application/activity+json"),
    })
    return _load("ap_collection_handler", "_ap_negative_response",
                 extra_globals=globs)


def test_collection_cold_hit_computes_and_stores_by_canonical_url():
    edge, d1_log = FakeEdgeCache(), []
    ns = _collection_env(edge, d1_log)
    resp = _run(ns["ap_collection_handler"](
        None, _fake_request("https://forkmesh.com/ap/users/alice/followers"),
        "user", "alice", "followers"))
    assert resp.status == 200
    assert resp.data["totalItems"] == 3
    assert edge.puts == ["https://forkmesh.com/ap/users/alice/followers"]
    assert any("COUNT(*)" in q for q in d1_log)


def test_collection_followers_hidden_by_default_for_users():
    edge, d1_log = FakeEdgeCache(), []
    ns = _collection_env(edge, d1_log, followers_public=False)
    resp = _run(ns["ap_collection_handler"](
        None, _fake_request("https://forkmesh.com/ap/users/alice/followers"),
        "user", "alice", "followers"))
    assert resp.data["items"] is None  # opted out: bare collection
    assert not any("ORDER BY" in q for q in d1_log)  # no items row fetch


def test_collection_followers_enumerate_when_user_opts_in():
    edge, d1_log = FakeEdgeCache(), []
    ns = _collection_env(edge, d1_log, followers_public=True)
    resp = _run(ns["ap_collection_handler"](
        None, _fake_request("https://forkmesh.com/ap/users/alice/followers"),
        "user", "alice", "followers"))
    assert resp.data["items"] == []  # opted in, fake d1_all returns none
    assert any("ORDER BY" in q for q in d1_log)  # items row fetch ran


def test_collection_repo_followers_always_enumerate():
    edge, d1_log = FakeEdgeCache(), []
    ns = _collection_env(edge, d1_log)
    resp = _run(ns["ap_collection_handler"](
        None, _fake_request("https://forkmesh.com/ap/repos/acme/widgets/followers"),
        "repo", "acme.widgets", "followers"))
    assert resp.data["items"] == []  # repo watchers are already public


def test_collection_warm_hit_revalidates_visibility_before_using_cache():
    edge, d1_log = FakeEdgeCache(), []
    ns = _collection_env(edge, d1_log)
    request = _fake_request("https://forkmesh.com/ap/users/alice/outbox")
    first = _run(ns["ap_collection_handler"](
        None, request, "user", "alice", "outbox"))
    d1_log.clear()
    second = _run(ns["ap_collection_handler"](
        None, request, "user", "alice", "outbox"))
    assert second is first
    assert d1_log == ["ensure_schema"]  # gate only; no COUNT(*)


def test_collection_not_found_is_negative_cached():
    edge, d1_log = FakeEdgeCache(), []
    ns = _collection_env(edge, d1_log, federates=False)
    url = "https://forkmesh.com/ap/users/ghost/followers"
    resp = _run(ns["ap_collection_handler"](
        None, _fake_request(url), "user", "ghost", "followers"))
    assert resp.status == 404
    assert edge.puts == [url]  # parked at the edge under the canonical key…
    d1_log.clear()
    second = _run(ns["ap_collection_handler"](
        None, _fake_request(url), "user", "ghost", "followers"))
    assert second is resp  # …and the probe storm replays from it
    assert d1_log == []


# --- Actor documents -------------------------------------------------------------

def _actor_env(edge, d1_log, resolves=True):
    globs = _base_globals(edge, d1_log)

    async def _ap_resolve_local_target(env, origin, url):
        return (
            "repo", "acme.widgets", "acme/widgets", "@acme.widgets"
        ) if resolves else None

    async def _ap_local_actor(env, kind, handle, create=False):
        d1_log.append("local_actor")
        return {"pubkeyPem": "PEM", "privkey": "K", "actorBi": "bi"}

    async def _ap_build_actor_doc(env, origin, kind, handle, rec):
        return {"id": globs["_ap_actor_url"](origin, kind, handle),
                "type": "Person"}

    globs.update({
        "_ap_resolve_local_target": _ap_resolve_local_target,
        "_ap_local_actor": _ap_local_actor,
        "_ap_build_actor_doc": _ap_build_actor_doc,
        "ap": SimpleNamespace(
            ACTIVITY_CONTENT_TYPE="application/activity+json"),
    })
    return _load("_ap_actor_doc_response", "_ap_negative_response",
                 extra_globals=globs)


def test_actor_doc_warm_hit_revalidates_visibility_before_using_cache():
    edge, d1_log = FakeEdgeCache(), []
    ns = _actor_env(edge, d1_log)
    request = _fake_request("https://forkmesh.com/ap/repos/acme/widgets")
    first = _run(ns["_ap_actor_doc_response"](
        None, request, "repo", "acme.widgets"))
    assert first.status == 200
    assert edge.puts == ["https://forkmesh.com/ap/repos/acme/widgets"]
    d1_log.clear()
    second = _run(ns["_ap_actor_doc_response"](
        None, request, "repo", "acme.widgets"))
    assert second is first
    assert d1_log == ["ensure_schema"]


def test_actor_doc_404_is_negative_cached():
    edge, d1_log = FakeEdgeCache(), []
    ns = _actor_env(edge, d1_log, resolves=False)
    url = "https://forkmesh.com/ap/repos/ghost/missing"
    resp = _run(ns["_ap_actor_doc_response"](
        None, _fake_request(url), "repo", "ghost.missing"))
    assert resp.status == 404
    assert edge.puts == [url]
    d1_log.clear()
    second = _run(ns["_ap_actor_doc_response"](
        None, _fake_request(url), "repo", "ghost.missing"))
    assert second is resp  # Mastodon's re-resolve on follow costs no D1
    assert d1_log == []


# --- WebFinger --------------------------------------------------------------------

def _webfinger_env(edge, d1_log):
    globs = _base_globals(edge, d1_log)

    async def _ap_user_federates(env, name):
        d1_log.append("user_federates")
        return True

    async def _ap_repo_federates(env, owner, repo):
        d1_log.append("repo_federates")
        return True

    def parse_acct_resource(raw):
        raw = (raw or "").strip().lower()
        if raw.startswith("acct:"):
            raw = raw[5:]
        if "@" not in raw:
            return (None, None)
        handle, _, domain = raw.partition("@")
        return (handle or None, domain or None)

    globs.update({
        "_ap_user_federates": _ap_user_federates,
        "_ap_repo_federates": _ap_repo_federates,
        "ap": SimpleNamespace(
            parse_acct_resource=parse_acct_resource,
            split_handle=lambda handle: (
                ("repo", *handle.split(".", 1))
                if "." in handle else ("user", handle)
            ),
            repo_handle=lambda owner, repo: owner + "." + repo,
            webfinger_doc=lambda acct, actor_url: {
                "subject": "acct:" + acct, "actor": actor_url},
            JRD_CONTENT_TYPE="application/jrd+json"),
    })
    return _load(
        "ap_webfinger_handler", "_ap_negative_response",
        extra_globals=globs)


def test_webfinger_spelling_variants_share_one_edge_entry():
    edge, d1_log = FakeEdgeCache(), []
    ns = _webfinger_env(edge, d1_log)
    base = "https://forkmesh.com/.well-known/webfinger?resource="
    first = _run(ns["ap_webfinger_handler"](
        None, _fake_request(base + "acct%3Aacme.widgets%40forkmesh.com")))
    assert first.status == 200
    d1_log.clear()
    # Same resource, different spelling (no acct: prefix, upper case) — the
    # normalized key must hit the entry stored by the first request.
    second = _run(ns["ap_webfinger_handler"](
        None, _fake_request(base + "Acme.Widgets%40ForkMesh.com")))
    assert second is first
    assert d1_log == ["ensure_schema", "repo_federates"]
    assert list(edge.store) == [
        "https://forkmesh.com/.well-known/webfinger"
        "?resource=acct:acme.widgets@forkmesh.com"]


def test_webfinger_invalid_resource_is_rejected_without_caching():
    edge, d1_log = FakeEdgeCache(), []
    ns = _webfinger_env(edge, d1_log)
    resp = _run(ns["ap_webfinger_handler"](
        None,
        _fake_request("https://forkmesh.com/.well-known/webfinger?resource=x")))
    assert resp.status == 400
    assert edge.store == {}
    assert d1_log == []


# --- Update broadcast invalidation --------------------------------------------------

def test_broadcast_actor_update_drops_cached_actor_doc_and_page():
    edge, d1_log = FakeEdgeCache(), []
    globs = _base_globals(edge, d1_log)
    actor_url = "https://forkmesh.com/ap/users/alice"
    page_url = "https://forkmesh.com/@alice"
    edge.store[actor_url] = FakeResponse({"stale": True})
    edge.store[page_url] = FakeResponse({"stale": True})
    edge.store[page_url + "/repositories"] = FakeResponse({"stale": True})

    async def _ap_actor_bi(env, kind, handle):
        return "bi"

    async def _ap_local_actor(env, kind, handle, create=False):
        return {"pubkeyPem": "PEM", "privkey": "K"}

    async def d1_all(env, sql, *args):
        return []  # no followers — bail after the invalidation

    async def _ap_build_actor_doc(env, origin, kind, handle, rec):
        return {"id": actor_url}

    globs.update({
        "_ap_actor_bi": _ap_actor_bi,
        "_ap_local_actor": _ap_local_actor,
        "_ap_build_actor_doc": _ap_build_actor_doc,
        "d1_all": d1_all,
        "quote": quote,
        "ACCOUNT_LOOKUP_CACHE_PREFIX": "https://forkmesh.internal/api/accounts/",
    })
    ns = _load("_ap_broadcast_actor_update", extra_globals=globs)
    _run(ns["_ap_broadcast_actor_update"](
        None, _fake_request(actor_url), "user", "alice"))
    # Both the actor doc and its rel=me verification page leave the cache.
    assert edge.store == {}


# --- Media endpoints (issue #416 round 3) ------------------------------------------
# Every remote server that resolves the repo actor fetches its avatar/banner,
# and every server receiving a Note fetches its image attachments — the last
# unauthenticated fetch surfaces that still hit D1 on every request.

class FakeJsResponse:
    @staticmethod
    def new(body, init):
        return FakeResponse(body, status=200)


def _media_globals(edge, d1_log):
    globs = _base_globals(edge, d1_log)

    async def edge_cache_match_media(key, fallback_type):
        return edge.store.get(key)

    async def decrypt_row(env, data):
        return data

    globs.update({
        "edge_cache_match_media": edge_cache_match_media,
        "decrypt_row": decrypt_row,
        "JsResponse": FakeJsResponse,
        # Uint8Array.new(_to_js(bytes)) copies the WASM view into a JS-owned
        # buffer so the body survives being read off the GIL; the stub just
        # returns the bytes back (bytes() copies) to mirror that.
        "Uint8Array": SimpleNamespace(new=lambda v: bytes(v)),
        "to_js": lambda v: v,
        "_to_js": lambda v: v,
        "base64": __import__("base64"),
    })
    return globs


def _object_media_env(edge, d1_log):
    import base64 as b64
    globs = _media_globals(edge, d1_log)

    async def d1_first(env, sql, *args):
        d1_log.append(sql)
        return {"data": {"media": [
            {"mediaType": "image/png",
             "data": b64.b64encode(b"png-bytes").decode()}]}}

    globs["d1_first"] = d1_first
    return _load("ap_object_media_handler", extra_globals=globs)


def test_object_media_warm_hit_revalidates_context_before_using_cache():
    edge, d1_log = FakeEdgeCache(), []
    ns = _object_media_env(edge, d1_log)
    uuid = "a" * 32
    request = _fake_request(
        "https://forkmesh.com/ap/o/%s/media/0" % uuid)
    first = _run(ns["ap_object_media_handler"](None, request, uuid, "0"))
    assert first.status == 200
    key = "https://forkmesh.com/ap/o/%s/media/0" % uuid
    assert edge.puts == [key]
    d1_log.clear()
    second = _run(ns["ap_object_media_handler"](None, request, uuid, "0"))
    assert second is first
    assert d1_log[0] == "ensure_schema"
    assert any("FROM ap_objects" in query for query in d1_log[1:])


def test_object_media_missing_index_is_not_cached():
    edge, d1_log = FakeEdgeCache(), []
    ns = _object_media_env(edge, d1_log)
    uuid = "b" * 32
    resp = _run(ns["ap_object_media_handler"](
        None, _fake_request("https://forkmesh.com/ap/o/%s/media/5" % uuid),
        uuid, "5"))
    assert resp.status == 404
    assert edge.store == {}


def _repo_media_env(edge, d1_log, has_media=True):
    import base64 as b64
    globs = _media_globals(edge, d1_log)

    async def _repo_is_private(env, owner, repo):
        return False

    async def blind_index(env, value):
        return "bi:" + value

    async def d1_first(env, sql, *args):
        d1_log.append(sql)
        if not has_media:
            return None
        return {"data": {"png": b64.b64encode(b"logo-bytes").decode()}}

    globs.update({
        "_repo_is_private": _repo_is_private,
        "blind_index": blind_index,
        "d1_first": d1_first,
    })
    return _load("repo_media_handler", extra_globals=globs)


def test_repo_media_cold_hit_stores_under_versioned_url():
    edge, d1_log = FakeEdgeCache(), []
    ns = _repo_media_env(edge, d1_log)
    url = "https://forkmesh.com/api/repo/forkmesh/forkmesh/media/logo.png?v=7"
    first = _run(ns["repo_media_handler"](
        None, _fake_request(url), "forkmesh", "forkmesh", "logo"))
    assert first.status == 200
    assert edge.puts == [url]  # the ?v= buster is part of the key
    d1_log.clear()
    second = _run(ns["repo_media_handler"](
        None, _fake_request(url), "forkmesh", "forkmesh", "logo"))
    assert second is first
    # The signed repository visibility is always checked before a cached
    # branding asset can be returned.
    assert d1_log == ["ensure_schema"]


def test_repo_media_not_found_is_not_cached():
    edge, d1_log = FakeEdgeCache(), []
    ns = _repo_media_env(edge, d1_log, has_media=False)
    resp = _run(ns["repo_media_handler"](
        None,
        _fake_request(
            "https://forkmesh.com/api/repo/forkmesh/forkmesh/media/logo.png"),
        "forkmesh", "forkmesh", "logo"))
    assert resp.status == 404
    assert edge.store == {}


# --- Repo actor `url` points at the fediverse profile page (adhoc #50) --------
# Clicking a repo handle on Mastodon (a mention, or the profile external-link)
# sends the user to the actor's `url`. It used to be the raw git page, dropping
# social-timeline visitors onto a code forge; it is now the /@owner.repo
# fediverse profile, with the git page kept as the verified "Repository" row.

def _actor_doc_globals():
    async def _ap_user_federates(env, name):
        return True

    async def blind_index(env, value):
        return "bi:" + value

    async def d1_first(env, sql, *args):
        # The repositories row: public, with an encrypted blob we decrypt below.
        return {"is_private": 0, "data": "BLOB"}

    async def d1_all(env, sql, *args):
        return []  # no uploaded logo/banner media

    async def decrypt_row(env, data):
        return {"description": "A federated repo"}

    async def _ap_org_alias_owner(env, owner, repo):
        return owner

    return {
        "AP_ACTOR_INSTANCE": "instance",
        "AP_ACTOR_USER": "user",
        "AP_ACTOR_REPO": "repo",
        "AP_AVATAR_PATH": "/assets/fediverse-avatar.png",
        "AP_BANNER_PATH": "/assets/fediverse-banner.png",
        "ap": _ap,
        "clean_string": lambda value, n=0: str(value or "")[:n] if n else str(
            value or ""),
        "blind_index": blind_index,
        "d1_first": d1_first,
        "d1_all": d1_all,
        "decrypt_row": decrypt_row,
        "_ap_org_alias_owner": _ap_org_alias_owner,
        "_ap_user_federates": _ap_user_federates,
        "_ap_domain_of": lambda origin: "forkmesh.com",
        "quote": quote,
        "repo_web_href": lambda owner, repo: "/%s/%s" % (owner, repo),
        "_ap_actor_url": lambda origin, kind, handle: (
            origin + "/ap/repos/%s/%s" % tuple(handle.split(".", 1))
            if kind == "repo" else origin + "/ap/users/" + handle),
    }


def test_repo_actor_url_is_fediverse_profile_page_not_git_page():
    ns = _load("_ap_build_actor_doc", extra_globals=_actor_doc_globals())
    doc = _run(ns["_ap_build_actor_doc"](
        None, "https://forkmesh.com", "repo", "owner.repo",
        {"pubkeyPem": "PEM", "createdAt": 123}))
    # `url` (Mastodon's click-through target) is the social profile page...
    assert doc["url"] == "https://forkmesh.com/@owner.repo"
    # ...while the actor id and the verified "Repository" row still point at
    # the git page so the code stays reachable and the green check survives.
    assert doc["id"] == "https://forkmesh.com/ap/repos/owner/repo"
    repo_row = next(a for a in doc["attachment"]
                    if a.get("name") == "Repository")
    assert "https://forkmesh.com/owner/repo" in repo_row["value"]
    assert "/@owner.repo" not in repo_row["value"]
    assert doc["icon"]["url"] == (
        "https://forkmesh.com/api/repo/owner/repo/logo?image=1"
    )


def _org_object_env(edge, d1_log, private_repos):
    globs = _media_globals(edge, d1_log)
    checked = []

    async def d1_first(env, sql, *args):
        d1_log.append(sql)
        return {"data": {
            "note": {"id": "https://forkmesh.com/ap/o/" + args[0],
                     "type": "Note"},
            "context": {"owner": "forkmesh", "repo": "forkmesh"},
            "media": [{"mediaType": "image/png", "data": "cG5n"}],
        }}

    async def _org_repo_node(env, owner, repo):
        return "jett-node" if (owner, repo) == ("forkmesh", "forkmesh") else ""

    async def _repo_is_private(env, owner, repo):
        checked.append((owner, repo))
        return (owner, repo) in private_repos

    globs.update({
        "d1_first": d1_first,
        "_org_repo_node": _org_repo_node,
        "_repo_is_private": _repo_is_private,
        "safe_segment": lambda value: str(value or ""),
        "ap": _ap,
    })
    ns = _load("ap_object_handler", "ap_object_media_handler",
               "_ap_org_alias_owner", extra_globals=globs)
    return ns, checked


def test_org_repo_objects_check_privacy_on_the_backing_node():
    edge, d1_log = FakeEdgeCache(), []
    ns, checked = _org_object_env(edge, d1_log, {("forkmesh", "forkmesh")})
    uuid = "c" * 32
    note = _run(ns["ap_object_handler"](
        None, _fake_request("https://forkmesh.com/ap/o/" + uuid), uuid))
    media = _run(ns["ap_object_media_handler"](
        None, _fake_request("https://forkmesh.com/ap/o/%s/media/0" % uuid),
        uuid, "0"))
    assert note.status == 200
    assert note.data["id"] == "https://forkmesh.com/ap/o/" + uuid
    assert media.status == 200
    assert checked == [("jett-node", "forkmesh"), ("jett-node", "forkmesh")]


def test_org_repo_objects_stay_hidden_when_the_backing_repo_is_private():
    edge, d1_log = FakeEdgeCache(), []
    ns, _checked = _org_object_env(edge, d1_log, {("jett-node", "forkmesh")})
    uuid = "d" * 32
    note = _run(ns["ap_object_handler"](
        None, _fake_request("https://forkmesh.com/ap/o/" + uuid), uuid))
    assert note.status == 404
    assert edge.store == {}
