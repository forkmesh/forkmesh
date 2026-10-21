"""Custom reactions: sanitiser, image caps, management permissions, the merged
catalog, the asset endpoint and comment reactions that use them."""

import asyncio
import base64
import hashlib
import json
import re
import sqlite3
import struct
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

import comment_reactions  # noqa: E402
import custom_reactions as cr  # noqa: E402
import schema  # noqa: E402

DOT_SVG = ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 10 10">'
           '<circle cx="5" cy="5" r="4" fill="#e74c3c"/></svg>')


def png(width, height, extra=b""):
    body = struct.pack(">II", width, height) + b"\x08\x06\x00\x00\x00"
    return b"\x89PNG\r\n\x1a\n" + struct.pack(">I", 13) + b"IHDR" + body + b"\0\0\0\0" + extra


def b64(data):
    return base64.b64encode(data).decode()


def run(coro):
    return asyncio.run(coro)


def bi(text):
    return hashlib.sha256(str(text).strip().lower().encode()).hexdigest()


# --------------------------------------------------------------------------
# The SVG sanitiser


@pytest.mark.parametrize("hostile, gone", [
    ('<script>alert(1)</script>', "script"),
    ('<circle onload="alert(1)" cx="1" cy="1" r="1"/>', "onload"),
    ('<foreignObject><div xmlns="http://www.w3.org/1999/xhtml">x</div></foreignObject>',
     "foreignObject"),
    ('<use href="https://evil.example/x.svg#a"/>', "evil.example"),
    ('<use xlink:href="data:image/svg+xml;base64,PHN2Zy8+" '
     'xmlns:xlink="http://www.w3.org/1999/xlink"/>', "data:"),
    ('<image href="data:image/png;base64,AAAA" width="1" height="1"/>', "image"),
    ('<a href="javascript:alert(1)"><circle r="1"/></a>', "javascript"),
    ('<rect width="1" height="1" fill="url(https://evil.example/p)"/>', "evil.example"),
    ('<rect width="1" height="1" style="fill:red;background:url(//evil.example)"/>',
     "evil.example"),
    ('<style>@import url(https://evil.example/x.css);</style>', "@import"),
    ('<set attributeName="href" to="javascript:alert(1)"/>', "javascript"),
])
def test_the_sanitiser_keeps_drawings_and_drops_everything_active(hostile, gone):
    text = ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 10 10">%s'
            '<rect x="1" y="1" width="8" height="8" fill="#0af"/></svg>' % hostile)
    clean = cr.sanitize_svg(text)
    assert gone not in clean
    assert "<rect" in clean and 'fill="#0af"' in clean
    assert clean.startswith('<svg xmlns="http://www.w3.org/2000/svg"')
    assert "ns0:" not in clean


def test_the_sanitiser_keeps_local_references_and_safe_animation():
    text = ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 10 10"><defs>'
            '<linearGradient id="g"><stop offset="0" stop-color="#f00"/></linearGradient>'
            '</defs><circle cx="5" cy="5" r="4" fill="url(#g)"><animateTransform '
            'attributeName="transform" type="rotate" from="0 5 5" to="360 5 5" dur="2s" '
            'repeatCount="indefinite"/></circle><use href="#g"/></svg>')
    clean = cr.sanitize_svg(text)
    assert 'fill="url(#g)"' in clean and "animateTransform" in clean
    assert 'href="#g"' in clean


@pytest.mark.parametrize("text, code", [
    ('<!DOCTYPE svg [<!ENTITY x "y">]><svg xmlns="http://www.w3.org/2000/svg" '
     'viewBox="0 0 1 1">&x;</svg>', "svg_doctype"),
    ("<svg", "invalid_svg"),
    ('<html xmlns="http://www.w3.org/1999/xhtml"/>', "invalid_svg"),
    ('<svg xmlns="http://www.w3.org/2000/svg"><rect/></svg>', "svg_size"),
    ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1 1"><animate '
     'attributeName="href" values="x"/></svg>', "animate_target"),
    ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1 1">%s</svg>'
     % ("<g/>" * 10000), "image_too_large"),
])
def test_the_sanitiser_refuses_what_it_cannot_make_safe(text, code):
    with pytest.raises(cr.ImageError) as raised:
        cr.sanitize_svg(text)
    assert str(raised.value) == code


def test_raster_images_are_sniffed_and_capped():
    assert cr.sniff_raster(png(64, 64)) == ("png", 64, 64)
    assert cr.sniff_raster(b"GIF89a" + struct.pack("<HH", 32, 16) + b"\0" * 8)[0] == "gif"
    webp = b"RIFF\0\0\0\0WEBPVP8X" + b"\0" * 8 + (99).to_bytes(3, "little") + (49).to_bytes(3, "little")
    assert cr.sniff_raster(webp) == ("webp", 100, 50)
    for bad, code in ((png(257, 10), "image_dimensions"), (b"\xff\xd8\xff", "unsupported_image")):
        with pytest.raises(cr.ImageError) as raised:
            cr.sniff_raster(bad)
        assert str(raised.value) == code
    with pytest.raises(cr.ImageError) as raised:
        cr.clean_image({"data": b64(png(10, 10, b"\0" * (70 * 1024)))})
    assert str(raised.value) == "image_too_large"
    kind, raw = cr.clean_image({"data": "data:image/png;base64," + b64(png(8, 8))})
    assert kind == "png" and raw.startswith(b"\x89PNG")
    kind, raw = cr.clean_image({"data": b64(DOT_SVG.encode())})
    assert kind == "svg" and b"<circle" in raw


# --------------------------------------------------------------------------
# Management, catalog, assets


class FakeD1:
    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        for statement in schema.SCHEMA_STATEMENTS:
            if re.search(r"comment_reaction|custom_reactions|comment_extras_rate|"
                         r"comment_tips|instance_settings", statement):
                self.db.execute(statement)
        self.db.execute("CREATE TABLE orgs (org_bi TEXT PRIMARY KEY, name TEXT, data TEXT, created_at INTEGER)")
        self.db.execute("CREATE TABLE org_members (org_bi TEXT, member_bi TEXT, role TEXT)")

    async def all(self, env, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]

    async def first(self, env, sql, *args):
        rows = await self.all(env, sql, *args)
        return rows[0] if rows else None

    async def run(self, env, sql, *args):
        self.db.execute(sql, args)


ADMINS = {"alice"}


@pytest.fixture
def rt(monkeypatch):
    d1 = FakeD1()
    clock = SimpleNamespace(now=10_000_000_000)
    d1.db.execute("INSERT INTO orgs VALUES (?,?,?,?)", (bi("org:acme"), "acme", "{}", 1))
    d1.db.execute("INSERT INTO org_members VALUES (?,?,?)", (bi("org:acme"), bi("bob"), "owner"))
    d1.db.execute("INSERT INTO org_members VALUES (?,?,?)", (bi("org:acme"), bi("carol"), "member"))

    async def blind_index(env, text):
        return bi(text)

    async def org_row(env, org):
        org_bi = bi("org:" + org.strip().lower())
        row = await d1.first(env, "SELECT name FROM orgs WHERE org_bi=?", org_bi)
        return org_bi, row

    async def org_role(env, org_bi, account):
        row = await d1.first(env, "SELECT role FROM org_members WHERE org_bi=? AND member_bi=?",
                             org_bi, bi(account))
        return (row or {}).get("role", "")

    async def session(env, request, data=None):
        if not request.account:
            return "", None
        return bi(request.account), {"name": request.account}

    async def is_admin(env, name):
        return name in ADMINS

    async def encrypt_row(env, obj):
        return "enc:" + json.dumps(obj)

    async def body(request, max_bytes=None):
        return request.body

    def json_response(data, status=200, cache_control=None, extra_headers=None, **_):
        return SimpleNamespace(status=status, data=data, cache_control=cache_control,
                               headers=extra_headers or {})

    async def nothing(env):
        return None

    runtime = {
        "d1_all": d1.all, "d1_first": d1.first, "d1_run": d1.run,
        "blind_index": blind_index, "_org_row": org_row, "_org_role": org_role,
        "_account_session_record": session, "_is_admin": is_admin,
        "encrypt_row": encrypt_row, "bounded_json_request": body,
        "json_response": json_response, "ensure_schema": nothing,
        "method_name": lambda request: request.method,
        "clean_string": lambda value, n=240: value.strip()[:n] if isinstance(value, str) else "",
        "MAX_NODE_NAME": 63, "Date": SimpleNamespace(now=lambda: clock.now),
    }
    for name, value in runtime.items():
        monkeypatch.setattr(cr, name, value, raising=False)
    cr.forget()
    return SimpleNamespace(d1=d1, clock=clock)


def request(method, path, account="", body=None):
    return SimpleNamespace(method=method, account=account, body=body,
                           url="https://forge.test" + path)


def save(account, scope, slug, label="Wasp", image=None, **extra):
    data = {"scope": scope, "action": "save", "slug": slug, "label": label}
    if image is not None:
        data["image"] = image
    data.update(extra)
    return run(cr.handler(SimpleNamespace(), request(
        "POST", "/api/reactions/custom/manage", account, data), "manage"))


def test_admins_manage_the_instance_and_owners_their_organization(rt):
    assert save("", "instance", "wasp", image={"svg": DOT_SVG}).status == 401
    assert save("carol", "instance", "wasp", image={"svg": DOT_SVG}).status == 403
    assert save("carol", "org:acme", "wasp", image={"svg": DOT_SVG}).status == 403
    assert save("bob", "instance", "wasp", image={"svg": DOT_SVG}).status == 403
    assert save("bob", "org:nope", "wasp", image={"svg": DOT_SVG}).status == 403
    made = save("alice", "instance", "wasp", image={"svg": DOT_SVG})
    assert made.status == 200 and made.data["reaction"]["emoji"] == "custom:wasp"
    assert made.data["reaction"]["src"].startswith("/api/reactions/custom/asset/")
    assert save("bob", "org:acme", "rocket-acme", "Acme rocket",
                image={"data": b64(png(32, 32))}).status == 200
    listing = run(cr.handler(SimpleNamespace(), request(
        "GET", "/api/reactions/custom/manage", "bob"), "manage"))
    assert listing.data["scopes"] == [{"scope": "org:acme", "label": "acme"}]
    admin = run(cr.handler(SimpleNamespace(), request(
        "GET", "/api/reactions/custom/manage?scope=instance", "alice"), "manage"))
    assert [item["slug"] for item in admin.data["reactions"]] == ["wasp"]
    assert run(cr.handler(SimpleNamespace(), request(
        "GET", "/api/reactions/custom/manage?scope=instance", "bob"), "manage")).status == 403


@pytest.mark.parametrize("fields, code", [
    ({"slug": "Bad Slug"}, "invalid_slug"),
    ({"slug": "-x"}, "invalid_slug"),
    ({"label": "   "}, "invalid_label"),
    ({"image": None}, "image_required"),
    ({"image": {"svg": "<svg"}}, "invalid_svg"),
    ({"image": {"data": b64(png(300, 300))}}, "image_dimensions"),
    ({"enabled": "yes"}, "invalid_state"),
])
def test_bad_saves_are_refused(rt, fields, code):
    data = {"slug": "wasp", "label": "Wasp", "image": {"svg": DOT_SVG}}
    data.update(fields)
    image = data.pop("image")
    reply = save("alice", "instance", data.pop("slug"), data.pop("label"), image, **data)
    assert reply.status == 400 and reply.data["error"] == code


def test_edits_keep_the_image_and_deletes_remove_the_row(rt):
    first = save("alice", "instance", "wasp", image={"svg": DOT_SVG}).data["reaction"]
    edited = save("alice", "instance", "wasp", "Wasp!", position=3, enabled=False).data["reaction"]
    assert edited["src"] == first["src"] and edited["label"] == "Wasp!"
    assert edited["position"] == 3 and edited["enabled"] is False
    gone = run(cr.handler(SimpleNamespace(), request(
        "POST", "/api/reactions/custom/manage", "alice",
        {"scope": "instance", "action": "delete", "slug": "wasp"}), "manage"))
    assert gone.status == 200
    assert rt.d1.db.execute("SELECT COUNT(*) FROM custom_reactions").fetchone()[0] == 0


def test_scopes_are_capped_and_writes_rate_limited(rt, monkeypatch):
    monkeypatch.setattr(cr, "MAX_PER_SCOPE", 2)
    assert save("alice", "instance", "a", image={"svg": DOT_SVG}).status == 200
    assert save("alice", "instance", "b", image={"svg": DOT_SVG}).status == 200
    assert save("alice", "instance", "c", image={"svg": DOT_SVG}).data["error"] == "scope_full"
    monkeypatch.setattr(cr, "RATE_MAX_PER_WINDOW", 3)
    assert save("alice", "instance", "a", "A").data["error"] == "rate_limited"


def test_the_public_list_merges_the_owner_organization_and_is_cached(rt):
    save("alice", "instance", "wasp", "Instance wasp", image={"svg": DOT_SVG}, position=2)
    save("alice", "instance", "hidden", image={"svg": DOT_SVG}, enabled=False)
    save("bob", "org:acme", "wasp", "Acme wasp", image={"data": b64(png(16, 16))})
    save("bob", "org:acme", "acme", "Acme", image={"svg": DOT_SVG}, position=1)
    plain = run(cr.handler(SimpleNamespace(), request("GET", "/api/reactions/custom"), ""))
    assert plain.cache_control.startswith("public, max-age=60")
    assert [(i["slug"], i["label"]) for i in plain.data["reactions"]] == [("wasp", "Instance wasp")]
    acme = run(cr.handler(SimpleNamespace(), request("GET", "/api/reactions/custom?owner=acme"), ""))
    # The organization's wasp (order 0) replaces the instance's; then order.
    assert [(i["slug"], i["label"], i["scope"]) for i in acme.data["reactions"]] == [
        ("wasp", "Acme wasp", "org"), ("acme", "Acme", "org")]
    # Remembered per owner for 30 seconds, forgotten on every write.
    rt.d1.db.execute("DELETE FROM custom_reactions")
    again = run(cr.catalog(SimpleNamespace(), "acme"))
    assert set(again) == {"acme", "wasp"}
    rt.clock.now += 31_000
    assert run(cr.catalog(SimpleNamespace(), "acme")) == {}


def test_assets_are_served_by_digest_with_an_immutable_cache(rt, monkeypatch):
    item = save("alice", "instance", "wasp", image={"svg": DOT_SVG},
                animated={"data": b64(png(8, 8))}).data["reaction"]
    sent = []

    def asset_response(data, content_type):
        sent.append((data, content_type))
        return SimpleNamespace(status=200)

    monkeypatch.setattr(cr, "_asset_response", asset_response)
    digest, ext = re.search(r"asset/([0-9a-f]{40})\.(\w+)$", item["src"]).groups()
    assert run(cr.asset_handler(SimpleNamespace(), request("GET", item["src"]), digest, ext)).status == 200
    assert sent[-1][1] == "image/svg+xml" and b"<circle" in sent[-1][0]
    anim, anim_ext = re.search(r"asset/([0-9a-f]{40})\.(\w+)$", item["animatedSrc"]).groups()
    run(cr.asset_handler(SimpleNamespace(), request("GET", item["animatedSrc"]), anim, anim_ext))
    assert sent[-1][1] == "image/png"
    assert run(cr.asset_handler(SimpleNamespace(), request("GET", "/x"), digest, "png")).status == 404
    assert run(cr.asset_handler(SimpleNamespace(), request("GET", "/x"), "0" * 40, "svg")).status == 404
    assert "sandbox" in cr.ASSET_CSP and cr.ASSET_CACHE.endswith("immutable")


# --------------------------------------------------------------------------
# Comment reactions with a custom slug


@pytest.fixture
def reactions(rt, monkeypatch):
    async def catalog(env, owner):
        return await cr.catalog(env, owner)

    async def access(env, request, owner, repo, viewer=""):
        return {"repoBi": bi(owner + "/" + repo), "visibility": "public",
                "record": {}, "viewer": viewer or request.account}

    async def decrypt_row(env, stored):
        return json.loads(stored[4:])

    async def feature_off(*_args):
        return None

    async def enqueue(*_args, **_kwargs):
        return True

    for name, value in {
        "d1_all": rt.d1.all, "d1_first": rt.d1.first, "d1_run": rt.d1.run,
        "blind_index": cr.blind_index, "encrypt_row": cr.encrypt_row,
        "decrypt_row": decrypt_row, "_repository_access_context": access,
        "_account_session_record": cr._account_session_record,
        "_request_account_session_token": lambda request, data=None: ("tok", False),
        "_repo_feature_off": feature_off, "enqueue_notification": enqueue,
        "ensure_schema": cr.ensure_schema, "bounded_json_request": cr.bounded_json_request,
        "json_response": cr.json_response, "method_name": cr.method_name,
        "clean_string": cr.clean_string, "valid_node_name": lambda v: True,
        "repo_web_href": lambda o, r: "/%s/%s" % (o, r), "MAX_NODE_NAME": 63,
        "Date": cr.Date, "custom_reactions_catalog": catalog,
    }.items():
        monkeypatch.setattr(comment_reactions, name, value, raising=False)
    return rt


def react(account, emoji, on=True, owner="acme"):
    req = SimpleNamespace(method="POST", account=account, token="tok", url="https://forge.test/",
                          body={"kind": "issue", "number": 1, "comment": "c1",
                                "emoji": emoji, "on": on})
    return run(comment_reactions.handler(SimpleNamespace(), req, owner, "app"))


def read(owner="acme"):
    req = SimpleNamespace(method="GET", account="", token="", body=None,
                          url="https://forge.test/api/repo/%s/app/reactions?kind=issue&number=1" % owner)
    return run(comment_reactions.handler(SimpleNamespace(), req, owner, "app"))


def test_custom_reactions_are_stored_by_slug_and_render_until_deleted(reactions):
    assert react("dave", "custom:wasp").data["error"] == "invalid_reaction"
    save("alice", "instance", "wasp", "Wasp", image={"svg": DOT_SVG})
    save("bob", "org:acme", "acme", "Acme", image={"svg": DOT_SVG})
    assert react("dave", "custom:wasp").status == 200
    reactions.clock.now += 1000
    assert react("dave", "custom:acme").status == 200
    # An organization's reaction is not offered on someone else's repository.
    assert react("dave", "custom:acme", owner="zed").data["error"] == "invalid_reaction"
    assert react("dave", "custom:Not Valid").data["error"] == "invalid_reaction"
    tallies = read().data["reactions"]["c1"]
    assert [(t["emoji"], t["custom"]["label"]) for t in tallies] == [
        ("custom:wasp", "Wasp"), ("custom:acme", "Acme")]
    assert tallies[0]["custom"]["src"].startswith("/api/reactions/custom/asset/")
    run(cr.handler(SimpleNamespace(), request(
        "POST", "/api/reactions/custom/manage", "alice",
        {"scope": "instance", "action": "delete", "slug": "wasp"}), "manage"))
    after = read().data["reactions"]["c1"]
    assert after[0] == {"emoji": "custom:wasp", "count": 1, "mine": False,
                        "people": [{"name": "dave", "agent": False}], "custom": None}
    # A deleted reaction can still be taken back, never added again.
    assert react("erin", "custom:wasp").data["error"] == "invalid_reaction"
    assert react("dave", "custom:wasp", on=False).status == 200


def test_reads_carry_tip_counts(reactions):
    rt = reactions
    for index, status in enumerate(("confirmed", "confirmed", "pending")):
        rt.d1.db.execute(
            "INSERT INTO comment_tips (reference, repo_bi, subject, comment, tipper_bi, "
            "recipient_bi, token, amount, status, created_at, data) VALUES (?,?,?,?,?,?,?,?,?,?,?)",
            ("ref%d" % index, bi("acme/app"), "issue:1", "c1", "t", "r", "sol", "1", status, 1, "x"))
    assert read().data["tips"] == {"c1": 2}


def test_migration_creates_the_tables_and_its_down_file_drops_them():
    [path] = sorted((ROOT / "migrations").glob("*_comment_gratitude_gifs_tips.sql"))
    migration = path.read_text("utf-8")
    down = (ROOT / "migrations" / "down" / path.name).read_text("utf-8")
    fingerprint = re.search(r"schema_fingerprint:([0-9a-f]{64})", migration).group(1)
    assert fingerprint in down
    db = sqlite3.connect(":memory:")
    db.execute("CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT)")
    db.executescript(migration)
    db.execute("INSERT INTO comment_tips (reference, repo_bi, subject, comment, tipper_bi, "
               "recipient_bi, token, amount, signature, created_at, data) VALUES "
               "('a','r','issue:1','c','t','u','sol','1','sig',1,'x')")
    with pytest.raises(sqlite3.IntegrityError):
        db.execute("INSERT INTO comment_tips (reference, repo_bi, subject, comment, tipper_bi, "
                   "recipient_bi, token, amount, signature, created_at, data) VALUES "
                   "('b','r','issue:1','c','t','u','sol','1','sig',1,'x')")
    db.executescript(down)
    assert db.execute("SELECT name FROM sqlite_master WHERE name IN "
                      "('custom_reactions','comment_tips','comment_extras_rate')").fetchall() == []
    entry = (ROOT / "src/entry.py").read_text("utf-8")
    assert '("comment_tips", "repo_bi"),' in entry
    assert "DELETE FROM comment_tips WHERE tipper_bi=? OR recipient_bi=?" in entry


def test_routes_are_wired():
    import urls
    assert urls.CUSTOM_REACTIONS_RE.match("/api/reactions/custom").group(1) is None
    assert urls.CUSTOM_REACTIONS_RE.match("/api/reactions/custom/manage").group(1) == "manage"
    assert urls.CUSTOM_REACTION_ASSET_RE.match("/api/reactions/custom/asset/%s.svg" % ("a" * 40))
    assert not urls.CUSTOM_REACTION_ASSET_RE.match("/api/reactions/custom/asset/../x.svg")
    assert urls.COMMENT_GIFS_RE.match("/api/gifs/search")
    assert urls.COMMENT_TIPS_RE.match("/api/tips/verify")
    assert urls.REPO_TIPS_RE.match("/api/repo/alice/app/tips")
    runtime = (ROOT / "src/app_runtime.py").read_text("utf-8")
    for name in ("custom_reactions_handler(", "custom_reactions_asset_handler(",
                 "comment_gifs_handler(", "comment_tips_handler(",
                 "comment_tips_intent_handler("):
        assert name in runtime
