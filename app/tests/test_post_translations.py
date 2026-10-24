import asyncio
import base64
import hashlib
import json
import re
import sqlite3
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest
from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

import instance_features  # noqa: E402
import post_translations  # noqa: E402
import schema  # noqa: E402

REPOS = {
    "alice/app": {"visibility": "public", "readers": set()},
    "alice/secret": {"visibility": "private", "readers": {"alice", "carol"}},
}
MANAGERS = {"alice"}
KEY = Ed25519PrivateKey.generate()


def b64url(raw):
    return base64.urlsafe_b64encode(raw).decode().rstrip("=")


def b64url_decode(text):
    return base64.urlsafe_b64decode(text + "=" * (-len(text) % 4))


PUB = b64url(KEY.public_key().public_bytes_raw())


class FakeD1:
    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        for statement in schema.SCHEMA_STATEMENTS:
            if "post_translations" in statement:
                self.db.execute(statement)

    async def all(self, env, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]

    async def first(self, env, sql, *args):
        rows = await self.all(env, sql, *args)
        return rows[0] if rows else None

    async def run(self, env, sql, *args):
        self.db.execute(sql, args)

    def count(self):
        return self.db.execute("SELECT COUNT(*) FROM post_translations").fetchone()[0]


def bi(text):
    return hashlib.sha256(str(text).strip().lower().encode()).hexdigest()


@pytest.fixture
def rt(monkeypatch):
    d1 = FakeD1()
    clock = SimpleNamespace(now=10_000_000_000)
    features = {}

    async def blind_index(env, text):
        return bi(text)

    async def encrypt_row(env, obj):
        return "enc:" + json.dumps(obj)

    async def decrypt_row(env, stored):
        assert stored.startswith("enc:")
        return json.loads(stored[4:])

    async def access(env, request, owner, repo, viewer=""):
        key = (owner + "/" + repo).lower()
        info = REPOS.get(key)
        if info is None:
            return None
        actor = viewer or getattr(request, "account", "")
        if info["visibility"] != "public" and actor not in info["readers"]:
            return None
        return {"repoBi": bi(key), "visibility": info["visibility"],
                "record": {}, "viewer": actor}

    async def session(env, request, data=None):
        if not request.account:
            return "", None
        return bi(request.account), {"name": request.account}

    def token(request, payload=None):
        return request.token, request.token == "cookie"

    async def feature_off(env, owner, repo, feature):
        if features.get(feature) is False:
            return SimpleNamespace(status=403, data={"error": "feature_disabled"},
                                   cache_control=None, headers={})
        return None

    async def manager(env, owner, data, request=None):
        return (request.account in MANAGERS), None

    async def nothing(env):
        return None

    async def body(request, max_bytes=None):
        if isinstance(request.body, Exception):
            raise request.body
        return request.body

    async def sha256_hex(text):
        return hashlib.sha256(text.encode()).hexdigest()

    async def verify(pub, sig, data):
        try:
            Ed25519PublicKey.from_public_bytes(b64url_decode(pub)).verify(b64url_decode(sig), data)
            return True
        except (InvalidSignature, ValueError):
            return False

    def json_response(data, status=200, cache_control=None, extra_headers=None, **_):
        return SimpleNamespace(status=status, data=data, cache_control=cache_control,
                               headers=extra_headers or {})

    def response(text, status=200, headers=None):
        return SimpleNamespace(status=status, text=text, headers=headers or {})

    runtime = {
        "d1_all": d1.all, "d1_first": d1.first, "d1_run": d1.run,
        "blind_index": blind_index, "encrypt_row": encrypt_row,
        "decrypt_row": decrypt_row, "_repository_access_context": access,
        "_account_session_record": session,
        "_request_account_session_token": token,
        "_repo_feature_off": feature_off, "_authorize_issue_manager": manager,
        "ensure_schema": nothing, "bounded_json_request": body,
        "json_response": json_response, "Response": response,
        "sha256_hex": sha256_hex, "ed25519_verify": verify,
        "method_name": lambda request: request.method,
        "clean_string": lambda value, n=240: value.strip()[:n] if isinstance(value, str) else "",
        "MAX_NODE_NAME": 63,
        "Date": SimpleNamespace(now=lambda: clock.now),
    }
    for name, value in runtime.items():
        monkeypatch.setattr(post_translations, name, value, raising=False)
    return SimpleNamespace(d1=d1, clock=clock, features=features)


def run(coro):
    return asyncio.run(coro)


def signed(owner="alice", repo="app", kind="issue", number=7, comment="c1", lang="en",
           body="Hello world", ts=10_000_000_000, key=KEY):
    pub = b64url(key.public_key().public_bytes_raw())
    text = post_translations.signing_text(owner, repo, kind, number, comment, lang, pub, ts,
                                          hashlib.sha256(body.encode()).hexdigest())
    return {"kind": kind, "number": number, "comment": comment, "lang": lang, "body": body,
            "ts": ts, "pub": pub, "sig": b64url(key.sign(text.encode()))}


def get(owner, repo, query, account=""):
    request = SimpleNamespace(
        method="GET", account=account, token="", body=None,
        url="https://forge.test/api/repo/%s/%s/translations?%s" % (owner, repo, query))
    return run(post_translations.handler(SimpleNamespace(), request, owner, repo))


def post(owner, repo, account, body, token="tok"):
    request = SimpleNamespace(method="POST", account=account, token=token, body=body,
                              url="https://forge.test/api/repo/%s/%s/translations" % (owner, repo))
    return run(post_translations.handler(SimpleNamespace(), request, owner, repo))


def test_a_signed_translation_is_listed_without_its_body_and_read_with_it(rt):
    payload = signed(body="Hello \u200cworld")
    payload["body"] = "Hello \u200cworld\r\n"
    saved = post("alice", "app", "bob", payload)
    assert saved.status == 200, saved.data
    assert saved.data["translations"][0]["body"] == "Hello \u200cworld"
    assert saved.data["translations"][0]["mine"] is True
    listing = get("alice", "app", "kind=issue&number=7")
    assert listing.status == 200
    entry = listing.data["translations"]["c1"][0]
    assert entry["lang"] == "en" and entry["name"] == "bob" and entry["mine"] is False
    assert "body" not in entry and "sig" not in entry
    assert listing.cache_control.startswith("public, max-age=")
    assert "cookie" in listing.headers["vary"]
    detail = get("alice", "app", "kind=issue&number=7&comment=c1&lang=en", account="bob")
    full = detail.data["translations"]["c1"][0]
    assert full["body"] == "Hello \u200cworld" and full["pub"] == PUB and full["mine"] is True
    assert detail.cache_control == "private, no-store"


def test_resubmitting_replaces_the_authors_translation(rt):
    post("alice", "app", "bob", signed(body="first"))
    rt.clock.now += 5000
    again = post("alice", "app", "bob", signed(body="second", ts=rt.clock.now))
    assert again.status == 200
    assert rt.d1.count() == 1
    post("alice", "app", "carol", signed(body="third", ts=rt.clock.now))
    post("alice", "app", "carol", signed(lang="zh", body="你好", ts=rt.clock.now))
    langs = sorted((e["name"], e["lang"]) for e in
                   get("alice", "app", "kind=issue&number=7").data["translations"]["c1"])
    assert langs == [("bob", "en"), ("carol", "en"), ("carol", "zh")]
    row = get("alice", "app", "kind=issue&number=7&comment=c1&lang=en").data["translations"]["c1"]
    bob = [e for e in row if e["name"] == "bob"][0]
    assert bob["body"] == "second" and bob["createdAt"] < bob["updatedAt"]


@pytest.mark.parametrize("change,error", [
    ({"body": "tampered"}, "bad_signature"),
    ({"lang": "fr"}, "bad_signature"),
    ({"comment": "c2"}, "bad_signature"),
    ({"sig": "A" * 86}, "bad_signature"),
    ({"pub": "short"}, "bad_signature"),
    ({"ts": 1}, "bad_signature"),
    ({"body": ""}, "invalid_body"),
    ({"body": "x" * 20001}, "invalid_body"),
    ({"body": ["x"]}, "invalid_body"),
    ({"lang": "<b>"}, "invalid_language"),
    ({"lang": ""}, "invalid_language"),
    ({"comment": "../x"}, "invalid_target"),
    ({"kind": "commit"}, "invalid_target"),
    ({"number": 0}, "invalid_target"),
    ({"source": "nothex"}, "invalid_source"),
])
def test_bad_writes_are_refused(rt, change, error):
    body = signed()
    body.update(change)
    refused = post("alice", "app", "bob", body)
    assert refused.status in (400,), refused.data
    assert refused.data["error"] == error
    assert rt.d1.count() == 0


def test_the_signature_covers_the_repository(rt):
    body = signed(repo="other")
    assert post("alice", "app", "bob", body).data["error"] == "bad_signature"
    assert post("Alice", "App", "bob", signed()).status == 200


def test_signed_out_and_unreadable_writes_are_refused(rt):
    assert post("alice", "app", "", signed()).status == 401
    assert post("alice", "secret", "bob", signed(repo="secret")).status == 404
    assert get("alice", "secret", "kind=issue&number=7", account="bob").status == 404
    assert post("alice", "secret", "carol", signed(repo="secret")).status == 200
    assert get("alice", "secret", "kind=issue&number=7", account="carol").cache_control == "private, no-store"
    rt.features["issues"] = False
    assert post("alice", "app", "bob", signed()).status == 403


def test_authors_remove_their_own_and_managers_remove_anyones(rt):
    post("alice", "app", "bob", signed())
    post("alice", "app", "carol", signed())
    removal = {"kind": "issue", "number": 7, "comment": "c1", "lang": "en", "remove": True}
    assert post("alice", "app", "bob", removal).status == 200
    assert rt.d1.count() == 1
    refused = post("alice", "app", "dave", dict(removal, account="carol"))
    assert refused.status == 403 and refused.data["error"] == "not_authorized"
    assert post("alice", "app", "alice", dict(removal, account="carol")).status == 200
    assert rt.d1.count() == 0


def test_writes_are_rate_limited_and_bounded(rt, monkeypatch):
    for index in range(post_translations.MAX_WRITES_PER_MINUTE):
        assert post("alice", "app", "bob", signed(comment="c%d" % index)).status == 200
    limited = post("alice", "app", "bob", signed(comment="cx"))
    assert limited.status == 429 and limited.data["error"] == "rate_limited"
    rt.clock.now += post_translations.MINUTE_MS
    monkeypatch.setattr(post_translations, "MAX_ROWS_PER_ACCOUNT_PER_RECORD", 10)
    full = post("alice", "app", "bob", signed(comment="cy", ts=rt.clock.now))
    assert full.status == 409 and full.data["error"] == "too_many_translations"
    assert post("alice", "app", "bob", signed(comment="c1", body="edit", ts=rt.clock.now)).status == 200


def test_the_no_script_page_escapes_and_marks_the_language(rt):
    post("alice", "app", "bob", signed(lang="fa", body="<script>alert(1)</script> سلام"))
    page = get("alice", "app", "kind=issue&number=7&format=html")
    assert page.status == 200
    assert page.headers["content-type"].startswith("text/html")
    assert "default-src 'none'" in page.headers["content-security-policy"]
    assert "<script>" not in page.text and "&lt;script&gt;" in page.text
    assert 'lang="fa" dir="auto"' in page.text
    assert "Community translation by bob" in page.text
    assert 'href="/alice/app/issues/7"' in page.text
    empty = get("alice", "app", "kind=issue&number=8&format=html")
    assert "No community translations yet." in empty.text
    assert get("alice", "app", "kind=issue&number=7&lang=%3Cb%3E").status == 400


def test_the_table_is_registered_everywhere_a_repository_table_must_be():
    entry = (ROOT / "src" / "entry.py").read_text("utf-8")
    assert '("post_translations", "repo_bi")' in entry
    assert "DELETE FROM post_translations WHERE account_bi=?" in entry
    runtime = (ROOT / "src" / "app_runtime.py").read_text("utf-8")
    assert "post_translations_handler(" in runtime
    rekey = (ROOT.parent / "tools" / "next_rekey.py").read_text("utf-8")
    assert '"post_translations": ("repo_bi", "account_bi")' in rekey
    assert instance_features.feature_for("/api/repo/a/b/translations") == "community-translations"
    assert "community-translations" not in instance_features.DEFAULT_ON
    assert re.search(r"post_translations", (ROOT / "migrations" / "0293_post_translations.sql").read_text())
