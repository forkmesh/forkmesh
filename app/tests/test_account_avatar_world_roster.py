import ast
import asyncio
import base64
import hashlib
import io
import json
import re
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import parse_qs, quote, urlparse

import pytest


SRC = Path(__file__).resolve().parents[1] / "src"
ACCOUNT_USERS = SRC / "account_users.py"
ACCOUNT_USERS_TEXT = ACCOUNT_USERS.read_text(encoding="utf-8")
ENTRY_TEXT = (SRC / "entry.py").read_text(encoding="utf-8")
URLS_TEXT = (SRC / "urls.py").read_text(encoding="utf-8")

WORLD_CACHE_CONTROL = "public, max-age=30, stale-while-revalidate=300"
DEFAULT_CACHE_CONTROL = "no-store, max-age=0, must-revalidate"


def _entry_defs(*names):
    namespace = {"re": re, "clean_string": lambda v, n: str(v or "")[:n]}
    wanted = set(names)
    for node in ast.parse(ENTRY_TEXT).body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            if node.name in wanted:
                exec(ast.get_source_segment(ENTRY_TEXT, node), namespace)
        elif isinstance(node, ast.Assign) and any(
                isinstance(t, ast.Name) and t.id in wanted
                for t in node.targets):
            exec(ast.get_source_segment(ENTRY_TEXT, node), namespace)
    return namespace


ENTRY_DEFS = _entry_defs(
    "_account_kind", "_is_public_roster_member", "valid_node_name",
    "NODE_NAME_RE", "MAX_NODE_NAME", "AVATAR_B64_RE", "PNG_HEADER",
    "_users_directory_cache_forget", "USERS_DIRECTORY_CACHE_KEY",
    "USERS_WORLD_CACHE_KEY", "USERS_WORLD_CACHE_CONTROL",
    "USERS_WORLD_FRESH_MS", "USERS_WORLD_STALE_SECONDS",
    "USERS_WORLD_BUILT_HEADER", "USERS_DIRECTORY_DATA_BATCH",
    "_host_background")


LOADED_PREFIXES = (
    "_avatar", "_account_avatar", "_users_world", "_account_users_world",
    "_users_directory_public_payload", "_users_directory_records",
    "_users_directory_tag", "_account_users_directory")


def _load_account_users(namespace):
    tree = ast.parse(ACCOUNT_USERS_TEXT, filename=str(ACCOUNT_USERS))
    body = [
        node for node in tree.body
        if (isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
            and node.name.startswith(LOADED_PREFIXES))
        or (isinstance(node, ast.Assign) and all(
            isinstance(t, ast.Name) and (
                t.id.startswith("AVATAR_") or t.id in (
                    "_AVATAR_THUMBS", "_USERS_DIRECTORY_RECORDS"))
            for t in node.targets))
    ]
    scope = dict(namespace)
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=body, type_ignores=[])),
        str(ACCOUNT_USERS), "exec"), scope)
    return scope


def _png(width, height, mode="RGBA", transparent=True):
    Image = pytest.importorskip("PIL.Image")
    image = Image.new(mode, (width, height), (200, 40, 40, 255)[:len(mode)])
    if transparent and mode == "RGBA":
        for x in range(width):
            for y in range(height // 3):
                image.putpixel((x, y), (0, 0, 0, 0))
    out = io.BytesIO()
    image.save(out, "PNG")
    return base64.b64encode(out.getvalue()).decode("ascii")


TINY_PNG = base64.b64encode(
    b"\x89PNG\r\n\x1a\n" + b"\x00" * 24).decode("ascii")


class FakeResponse:
    def __init__(self, body, status=200, headers=None):
        self.body = body
        self.status = status
        self.headers = dict(headers or {})

    async def text(self):
        return self.body


class JsResponseStub:
    @staticmethod
    def new(body, init):
        return SimpleNamespace(
            body=bytes(body), status=init["status"],
            headers=dict(init["headers"]))


def _json_response(data, status=200, cache_seconds=None, cache_control=None,
                   extra_headers=None):
    headers = {"content-type": "application/json; charset=utf-8"}
    if cache_control is not None:
        headers["cache-control"] = cache_control
    elif cache_seconds is not None:
        headers["cache-control"] = "public, max-age=%d" % cache_seconds
    headers.update(extra_headers or {})
    return FakeResponse(
        json.dumps(data, separators=(",", ":")), status, headers)


class Harness:
    def __init__(self, records, pil=True):
        self.records = records
        self.cache = {}
        self.deleted = []
        self.scans = 0
        self.admin_checks = 0
        self.renders = 0
        self.decrypts = []
        self.versions = {}
        self.epochs = {}
        self.clock = {"now": 1_000_000}
        self.background = []
        harness = self

        def tag(key):
            return "%s#%d" % (key, harness.versions.get(key, 0))

        async def d1_all(_env, query, *args):
            if "FROM users u" in query:
                harness.scans += 1
                return [{"user_bi": "bi-" + key} for key in harness.records]
            if "WHERE user_bi IN" in query:
                return [
                    {"user_bi": bi, "data": bi[3:], "data_tag": tag(bi[3:]),
                     "data_len": 10}
                    for bi in args if bi[3:] in harness.records]
            assert "FROM users ORDER BY" in query
            return [
                {"user_bi": "bi-" + key, "email_bi": "e-" + key,
                 "is_admin": 0, "enable_outreach": 0,
                 "data_tag": tag(key), "data_len": 10}
                for key in harness.records
            ]

        async def decrypt_row(_env, value):
            harness.decrypts.append(value)
            return dict(harness.records[value])

        def edge_cache_epoch(key):
            return harness.epochs.get(key, 0)

        def host_background(_env, key, factory):
            if harness.background is None:
                return False
            harness.background.append((key, factory))
            return True

        async def admin_actor(_env, request):
            harness.admin_checks += 1
            return "root" if getattr(request, "admin", False) else ""

        async def users_cache_get():
            hit = harness.cache.get("users")
            if hit is None:
                return None
            return FakeResponse(hit.body, 200, {
                "content-type": "application/json; charset=utf-8",
                "cache-control": DEFAULT_CACHE_CONTROL})

        async def edge_cache_match(key):
            return harness.cache.get(key)

        async def edge_cache_put(key, response):
            harness.cache[key] = response

        async def edge_cache_delete(key):
            harness.epochs[key] = harness.epochs.get(key, 0) + 1
            harness.deleted.append(key)
            harness.cache.pop(key, None)

        async def identity_rec(_env, name_bi):
            for rec in harness.records.values():
                if "bi:" + rec.get("name", "") == name_bi:
                    return dict(rec)
            return None

        async def blind_index(_env, value):
            return "bi:" + value

        def chat_payload(rec, *_args, **_kwargs):
            return {
                "name": rec["name"], "status": rec["status"],
                "avatarPng": rec.get("avatar_png", ""),
                "avatarUpdatedAt": rec.get("avatar_updated_at", 0),
                "createdAt": rec.get("created_at", 0), "kind": "user",
                "lastEmailAt": 0, "lastEmailStatus": "",
                "userStatus": {"away": False, "emoji": "", "text": "",
                               "expiresAt": 0},
            }

        async def value(result):
            return result

        self.edge_cache_delete = edge_cache_delete
        self.scope = _load_account_users({
            **ENTRY_DEFS,
            "base64": base64, "hashlib": hashlib, "io": io, "json": json,
            "quote": quote, "parse_qs": parse_qs, "urlparse": urlparse,
            "clean_string": lambda v, n: str(v or "")[:n],
            "_account_users_admin_actor": admin_actor,
            "_users_directory_cache_get": users_cache_get,
            "_credited_referrers_by_user": lambda _env: value({}),
            "_account_admin_signup_fields": lambda _env: value({}),
            "_account_admin_quarantined_users": lambda _env: value([]),
            "_admin_signup_referral_code_state": lambda _env: value({}),
            "_world_user_activity_buckets": lambda _env: value({}),
            "_contribution_commit_totals": lambda _env: value({}),
            "_account_chat_user_payload": chat_payload,
            "_clean_discord_id": lambda v: str(v or ""),
            "d1_all": d1_all, "decrypt_row": decrypt_row,
            "edge_cache_epoch": edge_cache_epoch,
            "_host_background": host_background,
            "Date": SimpleNamespace(now=lambda: harness.clock["now"]),
            "edge_cache_match": edge_cache_match,
            "edge_cache_put": edge_cache_put,
            "_account_identity_rec_by_bi": identity_rec,
            "blind_index": blind_index,
            "json_response": _json_response,
            "Response": FakeResponse,
            "JsResponse": JsResponseStub,
            "Uint8Array": SimpleNamespace(new=bytes),
            "_to_js": lambda v: v, "to_js": lambda v: v,
            "USERS_DIRECTORY_CACHE_KEY": "users",
            "USERS_WORLD_CACHE_KEY": "users-world",
            "USERS_WORLD_CACHE_CONTROL": WORLD_CACHE_CONTROL,
            "USERS_WORLD_FRESH_MS": 30_000,
            "USERS_WORLD_STALE_SECONDS": 90,
            "USERS_WORLD_BUILT_HEADER": "x-forkmesh-built",
            "USERS_DIRECTORY_DATA_BATCH": 2,
            "USERS_DIRECTORY_TTL": 120,
            "USERS_DIRECTORY_CLIENT_CACHE_CONTROL": DEFAULT_CACHE_CONTROL,
        })
        render = self.scope["_avatar_render_thumbnail"]

        def counting_render(*args):
            harness.renders += 1
            return render(*args)

        self.scope["_avatar_render_thumbnail"] = counting_render
        if not pil:
            self.scope["_avatar_image_module"] = lambda: None

    def run(self, name, *args):
        return asyncio.run(self.scope[name](object(), *args))

    def avatar(self, name, query=""):
        request = SimpleNamespace(
            url="https://app.example/api/accounts/%s/avatar%s" % (
                name, ("?" + query) if query else ""))
        return self.run("_account_avatar", request, name)

    def digest(self, name):
        return self.scope["_avatar_digest"](
            self.scope["_avatar_b64"](self.records[name]["avatar_png"]))


def _records(avatar):
    return {
        "zed": {"name": "zed", "kind": "user", "status": "active",
                "created_at": 5, "avatar_png": avatar},
        "amy": {"name": "amy", "kind": "user", "status": "active",
                "created_at": 1},
        "bob": {"name": "bob", "kind": "user", "status": "active",
                "created_at": 3, "avatar_png": "data:image/png;base64," + avatar},
        "hid": {"name": "hid", "kind": "user", "status": "active",
                "created_at": 2, "profile_private": True,
                "avatar_png": avatar},
        "new": {"name": "new", "kind": "user", "status": "pending",
                "created_at": 4, "avatar_png": avatar},
        "box": {"name": "box", "kind": "node", "status": "active",
                "created_at": 6, "avatar_png": avatar},
    }


def test_full_avatar_versioned_and_stale_cache_headers():
    harness = Harness(_records(TINY_PNG))
    digest = harness.digest("zed")
    assert re.fullmatch(r"[0-9a-f]{16}", digest)
    assert digest == hashlib.sha256(TINY_PNG.encode()).hexdigest()[:16]
    fresh = harness.avatar("zed", "v=" + digest)
    assert fresh.status == 200
    assert fresh.body == base64.b64decode(TINY_PNG)
    assert fresh.headers["content-type"] == "image/png"
    assert fresh.headers["x-content-type-options"] == "nosniff"
    assert fresh.headers["cache-control"] == (
        "public, max-age=31536000, immutable")
    assert fresh.headers["cdn-cache-control"] == "no-store"
    for query in ("", "v=0000000000000000"):
        stale = harness.avatar("zed", query)
        assert stale.status == 200
        assert stale.body == fresh.body
        assert stale.headers["cache-control"] == "public, max-age=60"
    prefixed = harness.avatar("bob", "v=" + harness.digest("bob"))
    assert prefixed.status == 200 and prefixed.body == fresh.body


def test_avatar_is_404_outside_the_public_roster():
    harness = Harness(_records(TINY_PNG))
    for name in ("amy", "hid", "new", "box", "ghost", "Bad_Name", "..", ""):
        response = harness.avatar(name)
        assert response.status == 404, name
        assert response.headers["cache-control"] == "no-store"
    harness.records["zed"]["avatar_png"] = "bm90IGEgcG5n"
    assert harness.avatar("zed").status == 404
    assert harness.avatar("zed", "size=64").status == 400


def test_thumbnail_is_a_128_webp_with_alpha_cached_by_hash():
    Image = pytest.importorskip("PIL.Image")
    harness = Harness(_records(_png(300, 200)))
    digest = harness.digest("zed")
    response = harness.avatar("zed", "size=128&v=" + digest)
    assert response.status == 200
    assert response.headers["content-type"] == "image/webp"
    assert response.headers["x-content-type-options"] == "nosniff"
    assert response.headers["cache-control"] == (
        "public, max-age=31536000, immutable")
    assert len(response.body) < 8192
    with Image.open(io.BytesIO(response.body)) as thumb:
        assert thumb.format == "WEBP"
        assert thumb.size == (128, 128)
        rgba = thumb.convert("RGBA")
        assert rgba.getpixel((64, 2))[3] < 16
        assert rgba.getpixel((64, 120))[3] > 240
    assert harness.renders == 1
    again = harness.avatar("zed", "size=128")
    assert again.body == response.body
    assert again.headers["cache-control"] == "public, max-age=60"
    assert harness.renders == 1
    assert digest in harness.scope["_AVATAR_THUMBS"]


def test_thumbnail_cache_is_bounded():
    pytest.importorskip("PIL.Image")
    harness = Harness(_records(TINY_PNG))
    harness.scope["AVATAR_THUMB_CACHE_MAX"] = 3
    thumbs = harness.scope["_AVATAR_THUMBS"]
    thumbs.clear()
    for index in range(5):
        png = _png(10 + index, 10 + index)
        harness.scope["_avatar_thumbnail_webp"](png, "d%d" % index)
    assert list(thumbs) == ["d2", "d3", "d4"]
    harness.scope["_avatar_thumbnail_webp"]("", "d2")
    assert list(thumbs) == ["d3", "d4", "d2"]


def test_oversized_or_non_png_sources_get_no_thumbnail():
    Image = pytest.importorskip("PIL.Image")
    harness = Harness(_records(TINY_PNG))
    harness.scope["AVATAR_THUMB_MAX_SOURCE_PIXELS"] = 100
    raw = base64.b64decode(_png(20, 20))
    assert harness.scope["_avatar_render_thumbnail"](Image, raw) == b""
    assert harness.scope["_avatar_render_thumbnail"](Image, b"junk") == b""


def test_world_view_without_pillow_keeps_avatar_url_only():
    harness = Harness(_records(TINY_PNG), pil=False)
    world = json.loads(harness.run(
        "_account_users_world_directory", SimpleNamespace()).body)
    zed = next(u for u in world["users"] if u["name"] == "zed")
    assert "avatarThumb" not in zed
    assert zed["avatarUrl"] == (
        "/api/accounts/zed/avatar?v=" + harness.digest("zed"))
    assert harness.avatar("zed", "size=128").status == 404
    assert harness.avatar("zed").status == 200


def test_world_view_is_the_public_roster_without_inline_pngs():
    pytest.importorskip("PIL.Image")
    harness = Harness(_records(_png(64, 96)))
    default = harness.run("_account_users_directory", SimpleNamespace())
    public = json.loads(default.body)["users"]
    harness.cache.clear()
    world_response = harness.run(
        "_account_users_world_directory", SimpleNamespace(admin=True))
    assert harness.admin_checks == 1
    world = json.loads(world_response.body)
    assert world_response.headers["cache-control"] == WORLD_CACHE_CONTROL
    assert world_response.headers["cdn-cache-control"] == "no-store"
    assert world_response.headers["content-type"].startswith(
        "application/json")
    assert [u["name"] for u in world["users"]] == [
        u["name"] for u in public] == ["amy", "bob", "zed"]
    for before, after in zip(public, world["users"]):
        assert "avatarPng" not in after
        expected = {k: v for k, v in before.items() if k != "avatarPng"}
        extra = {k: after[k] for k in after if k not in expected}
        assert {k: after[k] for k in expected} == expected
        if before["avatarPng"]:
            assert set(extra) == {"avatarThumb", "avatarUrl"}
            assert extra["avatarThumb"].startswith("data:image/webp;base64,")
            assert extra["avatarUrl"].startswith(
                "/api/accounts/%s/avatar?v=" % before["name"])
        else:
            assert extra == {}
    assert world["ok"] is True
    assert set(world) == {"ok", "users"}
    for key in ("publicUsers", "quarantinedUsers", "signupReferralCode"):
        assert key not in world


def test_world_view_reuses_the_roster_build_and_its_own_cache():
    harness = Harness(_records(TINY_PNG), pil=False)
    harness.run("_account_users_directory", SimpleNamespace())
    assert harness.scans == 1
    first = harness.run("_account_users_world_directory", SimpleNamespace())
    assert harness.scans == 1
    assert harness.cache["users-world"].headers["cache-control"] == (
        "public, max-age=90")
    assert "cdn-cache-control" not in harness.cache["users-world"].headers
    assert first.headers["cache-control"] == WORLD_CACHE_CONTROL
    harness.cache.pop("users")
    second = harness.run("_account_users_world_directory", SimpleNamespace())
    assert harness.scans == 1
    assert second.body == first.body
    assert second.headers["cdn-cache-control"] == "no-store"
    harness.cache.clear()
    harness.run("_account_users_world_directory", SimpleNamespace())
    assert harness.scans == 2
    assert "users" in harness.cache and "users-world" in harness.cache


def test_default_directory_response_is_unchanged():
    harness = Harness(_records(TINY_PNG))
    response = harness.run("_account_users_directory", SimpleNamespace())
    payload = json.loads(response.body)
    assert response.headers["cache-control"] == DEFAULT_CACHE_CONTROL
    assert set(payload) == {"ok", "users"}
    assert [u["name"] for u in payload["users"]] == ["amy", "bob", "zed"]
    zed = payload["users"][2]
    assert zed["avatarPng"] == TINY_PNG
    assert "avatarThumb" not in zed and "avatarUrl" not in zed
    assert json.loads(harness.cache["users"].body) == payload
    admin = json.loads(harness.run(
        "_account_users_directory", SimpleNamespace(admin=True)).body)
    assert [u["name"] for u in admin["publicUsers"]] == ["amy", "bob", "zed"]
    assert admin["publicUsers"][2]["avatarPng"] == TINY_PNG


def test_cache_forget_drops_both_roster_copies():
    deleted = []

    async def edge_cache_delete(key):
        deleted.append(key)

    scope = dict(ENTRY_DEFS)
    scope["edge_cache_delete"] = edge_cache_delete
    exec(compile(ast.Module(body=[
        node for node in ast.parse(ENTRY_TEXT).body
        if getattr(node, "name", "") == "_users_directory_cache_forget"
    ], type_ignores=[]), "entry", "exec"), scope)
    asyncio.run(scope["_users_directory_cache_forget"]())
    assert deleted == [
        ENTRY_DEFS["USERS_DIRECTORY_CACHE_KEY"],
        ENTRY_DEFS["USERS_WORLD_CACHE_KEY"]]
    assert ENTRY_DEFS["USERS_WORLD_CACHE_KEY"] != (
        ENTRY_DEFS["USERS_DIRECTORY_CACHE_KEY"])
    assert ENTRY_DEFS["USERS_WORLD_CACHE_CONTROL"] == WORLD_CACHE_CONTROL


def test_every_direct_roster_purge_also_drops_the_world_copy():
    for path in sorted(SRC.glob("*.py")):
        text = path.read_text(encoding="utf-8")
        for match in re.finditer(r"USERS_DIRECTORY_CACHE_KEY", text):
            window = text[max(0, match.start() - 300):match.end() + 300]
            if "delete" not in window:
                continue
            assert "USERS_WORLD_CACHE_KEY" in window, (
                path.name, text[:match.start()].count("\n") + 1)


def test_avatar_route_is_dispatched_before_generic_account_routes():
    assert ('ACCOUNT_AVATAR_RE = re.compile(r"^/api/accounts/([^/]+)/avatar$")'
            in URLS_TEXT)
    assert '"ACCOUNT_AVATAR_RE"' in ENTRY_TEXT
    start = ENTRY_TEXT.index("async def accounts_handler(env, request):")
    handler = ENTRY_TEXT[start:]
    avatar = handler.index("ACCOUNT_AVATAR_RE.match(url.path)")
    assert avatar < handler.index("ACCOUNT_CONTRIBUTIONS_RE.match(url.path)")
    assert avatar < handler.index("ACCOUNTS_RE.match(url.path)")
    users = handler.index('url.path == "/api/accounts/users"')
    world = handler.index("_account_users_world_directory(env, request)")
    assert users < world < handler.index(
        "_account_users_directory(env, request)")
    assert '_account_avatar = _account_users.export("_account_avatar")' in (
        ENTRY_TEXT)
    pattern = re.compile(r"^/api/accounts/([^/]+)/avatar$")
    assert pattern.match("/api/accounts/jett/avatar").group(1) == "jett"
    assert not pattern.match("/api/accounts/jett/avatar/x")


def _world(harness, headers=None):
    request = SimpleNamespace(headers=dict(headers or {}))
    return harness.run("_account_users_world_directory", request)


def test_world_view_carries_an_etag_and_answers_if_none_match_with_304():
    harness = Harness(_records(TINY_PNG), pil=False)
    first = _world(harness)
    etag = first.headers["etag"]
    assert re.fullmatch(r'W/"[0-9a-f]{32}"', etag)
    assert harness.cache["users-world"].headers["etag"] == etag
    for header in (etag, etag[2:], '"other", ' + etag, "*"):
        again = _world(harness, {"if-none-match": header})
        assert again.status == 304, header
        assert again.body is None
        assert again.headers["etag"] == etag
        assert again.headers["cache-control"] == WORLD_CACHE_CONTROL
        assert again.headers["cdn-cache-control"] == "no-store"
    miss = _world(harness, {"if-none-match": 'W/"stale"'})
    assert miss.status == 200 and miss.body == first.body
    assert harness.scans == 1


def test_a_stale_world_copy_is_served_while_the_host_rebuilds_it():
    harness = Harness(_records(TINY_PNG), pil=False)
    first = _world(harness)
    harness.records["amy"]["created_at"] = 7
    harness.versions["amy"] = 1
    harness.clock["now"] += 31_000
    stale = _world(harness)
    assert stale.body == first.body
    assert harness.scans == 1
    [(key, factory)] = harness.background
    assert key == "users-world"
    harness.cache.pop("users")
    asyncio.run(factory())
    assert harness.scans == 2
    fresh = _world(harness)
    assert fresh.body != first.body
    assert [u["name"] for u in json.loads(fresh.body)["users"]] == [
        "bob", "zed", "amy"]


def test_without_a_host_a_stale_world_copy_is_rebuilt_inline():
    harness = Harness(_records(TINY_PNG), pil=False)
    harness.background = None
    _world(harness)
    harness.cache.pop("users")
    harness.clock["now"] += 31_000
    _world(harness)
    assert harness.scans == 2


def test_a_purge_during_a_build_keeps_the_old_rows_out_of_the_cache():
    harness = Harness(_records(TINY_PNG), pil=False)
    decrypt = harness.scope["decrypt_row"]

    async def purge_midway(env, value):
        rec = await decrypt(env, value)
        await harness.edge_cache_delete("users")
        await harness.edge_cache_delete("users-world")
        return rec

    harness.scope["decrypt_row"] = purge_midway
    response = _world(harness)
    assert response.status == 200
    assert "users-world" not in harness.cache
    assert "users" not in harness.cache


def test_a_purge_drops_the_stale_copy_so_it_is_never_served():
    harness = Harness(_records(TINY_PNG), pil=False)
    _world(harness)
    harness.records["zed"]["profile_private"] = True
    harness.versions["zed"] = 1
    asyncio.run(harness.edge_cache_delete("users"))
    asyncio.run(harness.edge_cache_delete("users-world"))
    world = json.loads(_world(harness).body)
    assert [u["name"] for u in world["users"]] == ["amy", "bob"]
    assert harness.background == []


def test_directory_rebuilds_decrypt_only_changed_records():
    harness = Harness(_records(TINY_PNG), pil=False)
    harness.run("_account_users_directory", SimpleNamespace())
    assert sorted(harness.decrypts) == sorted(harness.records)
    harness.decrypts.clear()
    harness.cache.clear()
    harness.run("_account_users_directory", SimpleNamespace())
    assert harness.decrypts == []
    harness.versions["bob"] = 1
    harness.records["bob"]["status"] = "pending"
    harness.cache.clear()
    payload = json.loads(harness.run(
        "_account_users_directory", SimpleNamespace()).body)
    assert harness.decrypts == ["bob"]
    assert [u["name"] for u in payload["users"]] == ["amy", "zed"]
    del harness.records["amy"]
    harness.cache.clear()
    payload = json.loads(harness.run(
        "_account_users_directory", SimpleNamespace()).body)
    assert [u["name"] for u in payload["users"]] == ["zed"]
    assert "bi-amy" not in harness.scope["_USERS_DIRECTORY_RECORDS"]


def test_every_roster_purge_goes_through_edge_cache_delete():
    for path in sorted(SRC.glob("*.py")):
        if path.name == "entry.py":
            continue
        text = path.read_text(encoding="utf-8")
        assert not re.search(
            r"js_caches\.default\.delete\((cache_key|USERS_)", text), (
                path.name)
    delete = ENTRY_TEXT[ENTRY_TEXT.index("async def edge_cache_delete("):]
    delete = delete[:delete.index("\n\n\n")]
    assert "EDGE_CACHE_EPOCHS[cache_key]" in delete
