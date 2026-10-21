import asyncio
import hashlib
import json
import re
import sqlite3
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

import comment_reactions  # noqa: E402
import schema  # noqa: E402

REPOS = {
    "alice/app": {"visibility": "public", "readers": set()},
    "alice/secret": {"visibility": "private", "readers": {"alice", "carol"}},
}


class FakeD1:
    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        for statement in schema.SCHEMA_STATEMENTS:
            if "comment_reaction" in statement:
                self.db.execute(statement)

    async def all(self, env, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]

    async def first(self, env, sql, *args):
        rows = await self.all(env, sql, *args)
        return rows[0] if rows else None

    async def run(self, env, sql, *args):
        self.db.execute(sql, args)

    def count(self):
        return self.db.execute("SELECT COUNT(*) FROM comment_reactions").fetchone()[0]


def bi(text):
    return hashlib.sha256(str(text).strip().lower().encode()).hexdigest()


@pytest.fixture
def rt(monkeypatch):
    d1 = FakeD1()
    clock = SimpleNamespace(now=10_000_000_000)
    pings = []
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

    async def enqueue(env, recipient, kind, title, **kwargs):
        pings.append(dict(kwargs, recipient=recipient, kind=kind, title=title))
        return True

    async def nothing(env):
        return None

    async def body(request):
        if isinstance(request.body, Exception):
            raise request.body
        return request.body

    def json_response(data, status=200, cache_control=None, extra_headers=None, **_):
        return SimpleNamespace(status=status, data=data, cache_control=cache_control,
                               headers=extra_headers or {})

    runtime = {
        "d1_all": d1.all, "d1_first": d1.first, "d1_run": d1.run,
        "blind_index": blind_index, "encrypt_row": encrypt_row,
        "decrypt_row": decrypt_row, "_repository_access_context": access,
        "_account_session_record": session,
        "_request_account_session_token": token,
        "_repo_feature_off": feature_off, "enqueue_notification": enqueue,
        "ensure_schema": nothing, "bounded_json_request": body,
        "json_response": json_response,
        "method_name": lambda request: request.method,
        "clean_string": lambda value, n=240: value.strip()[:n] if isinstance(value, str) else "",
        "valid_node_name": lambda value: bool(re.fullmatch(r"[a-z0-9][a-z0-9-]{0,62}", value or "")),
        "repo_web_href": lambda owner, repo: "/%s/%s" % (owner, repo),
        "MAX_NODE_NAME": 63,
        "Date": SimpleNamespace(now=lambda: clock.now),
    }
    for name, value in runtime.items():
        monkeypatch.setattr(comment_reactions, name, value, raising=False)
    return SimpleNamespace(d1=d1, clock=clock, pings=pings, features=features)


def run(coro):
    return asyncio.run(coro)


def get(owner, repo, query, account=""):
    request = SimpleNamespace(
        method="GET", account=account, token="", body=None,
        url="https://forge.test/api/repo/%s/%s/reactions?%s" % (owner, repo, query))
    return run(comment_reactions.handler(SimpleNamespace(), request, owner, repo))


def post(owner, repo, account, token="tok", **fields):
    body = {"kind": "issue", "number": 7, "comment": "c1", "emoji": "like", "on": True}
    body.update(fields)
    request = SimpleNamespace(method="POST", account=account, token=token, body=body,
                              url="https://forge.test/api/repo/%s/%s/reactions" % (owner, repo))
    return run(comment_reactions.handler(SimpleNamespace(), request, owner, repo))


def test_signed_out_readers_see_counts_but_cannot_react(rt):
    assert post("alice", "app", "bob").status == 200
    response = get("alice", "app", "kind=issue&number=7")
    assert response.status == 200
    assert response.data["signedIn"] is False
    tally = response.data["reactions"]["c1"][0]
    assert tally == {"emoji": "like", "count": 1, "mine": False,
                     "people": [{"name": "bob", "agent": False}]}
    assert response.cache_control.startswith("public, max-age=")
    assert "authorization" in response.headers["vary"]
    refused = post("alice", "app", "")
    assert refused.status == 401 and refused.data["error"] == "unauthorized"
    assert rt.d1.count() == 1


def test_toggle_is_explicit_idempotent_and_marks_mine(rt):
    first = post("alice", "app", "bob")
    again = post("alice", "app", "bob")
    assert first.data["reactions"] == again.data["reactions"]
    assert first.data["reactions"][0]["count"] == 1
    assert first.data["reactions"][0]["mine"] is True
    post("alice", "app", "carol", emoji="celebrate")
    mine = get("alice", "app", "kind=issue&number=7", account="bob")
    assert mine.cache_control == "private, no-store"
    tallies = {t["emoji"]: t for t in mine.data["reactions"]["c1"]}
    assert tallies["like"]["mine"] is True and tallies["celebrate"]["mine"] is False
    off = post("alice", "app", "bob", on=False)
    assert [t["emoji"] for t in off.data["reactions"]] == ["celebrate"]
    assert post("alice", "app", "bob", on=False).status == 200
    assert rt.d1.count() == 1


def test_tallies_keep_the_order_of_the_first_reaction(rt):
    post("alice", "app", "bob", emoji="rocket")
    rt.clock.now += 1000
    post("alice", "app", "carol", emoji="like")
    rt.clock.now += 1000
    post("alice", "app", "dave", emoji="rocket")
    tallies = get("alice", "app", "kind=issue&number=7").data["reactions"]["c1"]
    assert [(t["emoji"], t["count"]) for t in tallies] == [("rocket", 2), ("like", 1)]


@pytest.mark.parametrize("fields,error", [
    ({"emoji": "👍"}, "invalid_reaction"),
    ({"emoji": "party_parrot"}, "invalid_reaction"),
    ({"emoji": ["like"]}, "invalid_reaction"),
    ({"kind": "commit"}, "invalid_target"),
    ({"number": 0}, "invalid_target"),
    ({"number": "7; DROP"}, "invalid_target"),
    ({"comment": "../x"}, "invalid_target"),
    ({"comment": "<script>"}, "invalid_target"),
    ({"comment": "x" * 200}, "invalid_target"),
    ({"on": "yes"}, "invalid_state"),
])
def test_only_catalog_reactions_on_well_formed_targets_are_stored(rt, fields, error):
    response = post("alice", "app", "bob", **fields)
    assert response.status == 400 and response.data["error"] == error
    assert rt.d1.count() == 0


def test_bad_reads_are_refused(rt):
    assert get("alice", "app", "kind=issue").status == 400
    assert get("alice", "app", "kind=wiki&number=1").status == 400
    assert get("alice", "missing", "kind=issue&number=1").status == 404


def test_private_repository_reactions_need_read_access(rt):
    assert get("alice", "secret", "kind=pull&number=3").status == 404
    assert get("alice", "secret", "kind=pull&number=3", account="mallory").status == 404
    assert post("alice", "secret", "mallory", kind="pull", number=3).status == 404
    assert rt.d1.count() == 0
    assert post("alice", "secret", "carol", kind="pull", number=3).status == 200
    reader = get("alice", "secret", "kind=pull&number=3", account="alice")
    assert reader.status == 200 and reader.cache_control == "private, no-store"
    assert reader.data["reactions"]["c1"][0]["people"] == [{"name": "carol", "agent": False}]


def test_records_are_separate_per_kind_number_and_repository(rt):
    post("alice", "app", "bob", kind="pull", number=7)
    post("alice", "app", "bob", kind="discussion", number=7, comment="body")
    assert get("alice", "app", "kind=issue&number=7").data["reactions"] == {}
    assert list(get("alice", "app", "kind=pull&number=7").data["reactions"]) == ["c1"]
    assert list(get("alice", "app", "kind=discussion&number=7").data["reactions"]) == ["body"]


def test_reactions_follow_the_record_feature_switch(rt):
    rt.features["discussions"] = False
    assert post("alice", "app", "bob", kind="discussion").status == 403
    assert post("alice", "app", "bob", kind="issue").status == 200


def test_one_account_has_a_reaction_cap_per_comment(rt):
    slugs = [slug for slug, _ in comment_reactions.CATALOG]
    for slug in slugs[:comment_reactions.MAX_PER_ACCOUNT_PER_COMMENT]:
        assert post("alice", "app", "bob", emoji=slug).status == 200
    over = post("alice", "app", "bob", emoji=slugs[-1])
    assert over.status == 409 and over.data["error"] == "too_many_reactions"
    assert post("alice", "app", "bob", emoji=slugs[0], on=False).status == 200
    assert post("alice", "app", "bob", emoji=slugs[-1]).status == 200


def test_writes_are_rate_limited_per_account(rt):
    for index in range(comment_reactions.RATE_MAX_PER_WINDOW):
        assert post("alice", "app", "bob", on=index % 2 == 0).status == 200
    limited = post("alice", "app", "bob")
    assert limited.status == 429 and limited.data["error"] == "rate_limited"
    assert int(limited.headers["Retry-After"]) >= 1
    assert post("alice", "app", "carol").status == 200
    rt.clock.now += comment_reactions.RATE_WINDOW_MS
    assert post("alice", "app", "bob").status == 200


def test_daily_and_per_record_caps_bound_the_table(rt, monkeypatch):
    monkeypatch.setattr(comment_reactions, "MAX_ADDS_PER_DAY", 3)
    for index in range(3):
        assert post("alice", "app", "bob", comment="c%d" % index).status == 200
    daily = post("alice", "app", "bob", comment="c9")
    assert daily.status == 429 and daily.data["error"] == "daily_limit"
    monkeypatch.setattr(comment_reactions, "MAX_ROWS_PER_RECORD", 4)
    assert post("alice", "app", "carol", comment="c1").status == 200
    full = post("alice", "app", "dave", comment="c1")
    assert full.status == 409 and full.data["error"] == "record_full"


def test_people_lists_are_bounded_and_names_are_encrypted(rt, monkeypatch):
    monkeypatch.setattr(comment_reactions, "RATE_MAX_PER_WINDOW", 1000)
    for index in range(15):
        post("alice", "app", "user%d" % index)
    tally = get("alice", "app", "kind=issue&number=7").data["reactions"]["c1"][0]
    assert tally["count"] == 15
    assert len(tally["people"]) == comment_reactions.MAX_PEOPLE
    stored = rt.d1.db.execute("SELECT * FROM comment_reactions").fetchall()
    for row in stored:
        assert "user" not in row["account_bi"] and "alice" not in row["repo_bi"]
        assert row["data"].startswith("enc:")


def test_agent_tokens_are_marked(rt):
    post("alice", "app", "helper", token="fmat_abc")
    post("alice", "app", "bob", token="cookie")
    people = get("alice", "app", "kind=issue&number=7").data["reactions"]["c1"][0]["people"]
    assert {p["name"]: p["agent"] for p in people} == {"helper": True, "bob": False}


def test_the_author_hears_once_per_comment_with_a_collapsed_summary(rt):
    post("alice", "app", "bob", author="alice")
    rt.clock.now += 1000
    post("alice", "app", "carol", emoji="celebrate", author="alice")
    rt.clock.now += 1000
    post("alice", "app", "dave", emoji="celebrate", author="alice")
    pings = [p for p in rt.pings if p["recipient"] == "alice"]
    assert len({p["dedupe"] for p in pings}) == 1
    last = pings[-1]
    assert last["kind"] == "reaction" and last["push"] is False
    assert last["title"] == "dave, carol and 1 other reacted to your comment on alice/app#7"
    assert last["body"] == "\U0001F389 2 \U0001F44D 1"
    assert last["href"] == "/alice/app/issues/7"
    assert last["meta"] == {"kind": "issue", "number": 7, "comment": "c1"}


def test_no_ping_for_self_removals_bad_names_or_private_strangers(rt):
    post("alice", "app", "bob", author="bob")
    post("alice", "app", "bob", on=False, author="alice")
    post("alice", "app", "carol", author="<b>x</b>")
    post("alice", "secret", "carol", author="mallory")
    assert rt.pings == []
    post("alice", "secret", "carol", emoji="eyes", author="alice")
    assert [p["recipient"] for p in rt.pings] == ["alice"]


def test_opening_post_pings_name_the_record(rt):
    post("alice", "app", "bob", kind="pull", number=4, comment="body", author="alice")
    assert rt.pings[0]["title"] == "bob reacted to your pull request on alice/app#4"
    assert rt.pings[0]["href"] == "/alice/app/pull/4"


def test_only_get_and_post(rt):
    request = SimpleNamespace(method="DELETE", account="bob", token="", body=None,
                              url="https://forge.test/api/repo/alice/app/reactions")
    assert run(comment_reactions.handler(SimpleNamespace(), request, "alice", "app")).status == 405


def test_invalid_json_is_refused(rt):
    request = SimpleNamespace(method="POST", account="bob", token="", body=ValueError("x"),
                              url="https://forge.test/api/repo/alice/app/reactions")
    assert run(comment_reactions.handler(SimpleNamespace(), request, "alice", "app")).status == 400


def test_names_sentence():
    sentence = comment_reactions.names_sentence
    assert sentence(["alice"], 1) == "alice"
    assert sentence(["alice", "bob"], 2) == "alice and bob"
    assert sentence(["alice", "bob"], 5) == "alice, bob and 3 others"
    assert sentence(["alice"], 2) == "alice and 1 other"


def test_one_catalog_on_every_surface():
    server = {slug: comment_reactions.EMOJI[slug] for slug, _ in comment_reactions.CATALOG}
    repo_ui = (ROOT / "public/dashboard/js/06-repo-content.js").read_text("utf-8")
    block = repo_ui[repo_ui.index("const ISSUE_REACTION_EMOJI"):]
    block = block[:block.index("});")]
    assert set(re.findall(r'(\w+): "', block)) == set(server)
    table = repo_ui[repo_ui.index("const COMMENT_REACTIONS = "):]
    table = table[:table.index("]);")]
    labels = dict(re.findall(r'\["(\w+)", "([^"]+)", "[^"]*"\]', table))
    assert labels == dict(comment_reactions.CATALOG)
    sys.path.insert(0, str(ROOT / "host"))
    from forkmesh_host import repo_ssr_issue
    assert repo_ssr_issue.REACTION_EMOJI == server
    assert repo_ssr_issue.REACTION_LABELS == dict(comment_reactions.CATALOG)
    desktop = (ROOT.parent / "desktop/src/ReactionCatalog.h").read_text("utf-8")
    assert re.findall(r'QStringLiteral\("(\w+)"\), QStringLiteral\("([^"]+)"\)', desktop) \
        == list(comment_reactions.CATALOG)
    chat = (ROOT / "public/dashboard-chat.js").read_text("utf-8")
    quick = re.search(r"REACTION_EMOJI = Object.freeze\(\[([^\]]+)\]\)", chat).group(1)
    quick_slugs = re.search(r"COMMENT_REACTION_QUICK = Object.freeze\(\[([^\]]+)\]\)",
                            repo_ui).group(1)
    slugs = re.findall(r'"(\w+)"', quick_slugs)
    assert [server[slug] for slug in slugs] == re.findall(r'"([^"]+)"', quick)


def test_schema_migration_and_cleanup_are_registered():
    [path] = sorted((ROOT / "migrations").glob("*_comment_reactions.sql"))
    migration = path.read_text("utf-8")
    down = (ROOT / "migrations" / "down" / path.name).read_text("utf-8")
    entry = (ROOT / "src/entry.py").read_text("utf-8")
    fingerprint = re.search(r"schema_fingerprint:([0-9a-f]{64})", migration).group(1)
    # The newest migration's stamp is pinned to entry.py by
    # test_local_migrations; this one only has to match its down file.
    assert fingerprint in down
    assert '("comment_reactions", "repo_bi"),' in entry
    assert 'DELETE FROM comment_reactions WHERE account_bi=?' in entry
    assert 'DELETE FROM comment_reaction_rate WHERE account_bi=?' in entry
    assert '"reaction",\n})' in entry
    db = sqlite3.connect(":memory:")
    db.execute("CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT)")
    db.executescript(migration)
    db.execute("INSERT INTO comment_reactions VALUES ('r','issue:1','c','like','a',1,'x')")
    with pytest.raises(sqlite3.IntegrityError):
        db.execute("INSERT INTO comment_reactions VALUES ('r','issue:1','c','like','a',2,'y')")
    db.executescript(down)
    assert db.execute("SELECT name FROM sqlite_master WHERE name LIKE 'comment_reaction%'").fetchall() == []


def test_route_is_wired():
    import urls
    assert urls.REPO_REACTIONS_RE.match("/api/repo/alice/app/reactions")
    assert not urls.REPO_REACTIONS_RE.match("/api/repo/alice/app/reactions/x")
    runtime = (ROOT / "src/app_runtime.py").read_text("utf-8")
    assert "REPO_REACTIONS_RE.match(url.path)" in runtime
    assert "comment_reactions_handler(" in runtime
