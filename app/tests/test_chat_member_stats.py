import asyncio
import importlib.util
import json
import re
import sqlite3
from pathlib import Path
from types import SimpleNamespace

import pytest

from src.schema import SCHEMA_STATEMENTS


ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = ROOT.parent
ENTRY = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
APP_RUNTIME = (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
FOOTPRINT = (
    REPOSITORY / "world" / "tools" / "build_worker_footprint.py"
).read_text(encoding="utf-8")
CHAT = (ROOT / "public" / "dashboard-chat.js").read_text(encoding="utf-8")
CHAT_VIEW = (
    ROOT / "public" / "dashboard" / "partials" / "views" / "chat.html"
).read_text(encoding="utf-8")
CHAT_PAGE = (
    ROOT / "public" / "dashboard" / "chat" / "index.html"
).read_text(encoding="utf-8")
NOW = 1_800_000_000_000
TODAY = "2027-01-15"
YESTERDAY = "2027-01-14"


def _contribution_tables():
    wanted = (
        "profile_contribution_projects (", "profile_contribution_days (")
    return [
        statement for statement in SCHEMA_STATEMENTS
        if any(name in statement for name in wanted)
    ]


def _load():
    spec = importlib.util.spec_from_file_location(
        "chat_member_stats_under_test",
        ROOT / "src" / "chat_member_stats.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    db = sqlite3.connect(":memory:")
    db.row_factory = sqlite3.Row
    db.execute("CREATE TABLE users (user_bi TEXT PRIMARY KEY, data TEXT)")
    db.execute(
        "CREATE TABLE repositories (key_bi TEXT PRIMARY KEY, "
        "is_private INTEGER NOT NULL DEFAULT 0)")
    for statement in _contribution_tables():
        db.execute(statement)
    state = {
        "now": NOW, "queries": [], "cache": {}, "generation": {
            "contributions": 0}}

    class _Date:
        @staticmethod
        def now():
            return state["now"]

    async def d1_all(env, sql, *params):
        state["queries"].append(sql)
        return [dict(row) for row in db.execute(sql, params).fetchall()]

    async def decrypt_row(env, data):
        return json.loads(data) if data else None

    async def blind_index(env, value):
        return "bi:" + str(value).strip().lower()

    async def ensure_schema(env):
        return None

    async def edge_cache_match(key):
        return state["cache"].get(key)

    async def edge_cache_put(key, response):
        state["cache"][key] = response

    def json_response(payload, status=200, cache_control=None,
                      extra_headers=None, cache_seconds=None):
        return SimpleNamespace(
            status=status, payload=payload, cache_control=cache_control,
            headers=extra_headers or {})

    def method_name(request):
        return str(getattr(request, "method", "GET")).upper()

    def is_public_roster_member(rec):
        return bool(rec) and (
            rec.get("kind", "user") == "user"
            and rec.get("status") == "active"
            and not rec.get("profile_private"))

    module._bind_runtime({
        "Date": _Date,
        "d1_all": d1_all,
        "decrypt_row": decrypt_row,
        "blind_index": blind_index,
        "ensure_schema": ensure_schema,
        "edge_cache_match": edge_cache_match,
        "edge_cache_put": edge_cache_put,
        "json_response": json_response,
        "method_name": method_name,
        "_is_public_roster_member": is_public_roster_member,
        "_D1_WATCH_GENERATIONS": state["generation"],
    })
    return module, db, state


def _user(db, name, **fields):
    record = {"name": name, "kind": "user", "status": "active", **fields}
    db.execute(
        "INSERT INTO users (user_bi, data) VALUES (?, ?)",
        ("bi:" + name, json.dumps(record)))


def _repo(db, key, private=False, public_project=True,
          verified_from="2020-01-01", project=None):
    db.execute(
        "INSERT INTO repositories (key_bi, is_private) VALUES (?, ?)",
        (key, 1 if private else 0))
    db.execute(
        "INSERT INTO profile_contribution_projects (source_repo_bi, "
        "source_account_bi, owner_user_bi, project_bi, first_public_day, "
        "is_public, active_generation_bi, captured_at, verified_from, data) "
        "VALUES (?,?,?,?,?,?,?,?,?,?)",
        (key, "acct", "owner", project or ("project:" + key), "2020-01-01",
         1 if public_project else 0, "gen:" + key, 1, verified_from, "{}"))


def _day(db, repo, name, day, commits=0, issues=0, pulls=0, reviews=0):
    db.execute(
        "INSERT INTO profile_contribution_days (generation_bi, "
        "subject_user_bi, source_account_bi, source_repo_bi, project_bi, day, "
        "commits, issues, pulls, reviews, captured_at, data) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
        ("gen:" + repo, "bi:" + name, "acct", repo, "project:" + repo, day,
         commits, issues, pulls, reviews, 1, "{}"))


def _payload(module, names):
    return asyncio.run(module.member_stats_payload(
        SimpleNamespace(), names, NOW))


def _get(module, query, method="GET"):
    request = SimpleNamespace(
        method=method,
        url="https://example.test/api/chat/member-stats?" + query)
    return asyncio.run(module.member_stats_route(SimpleNamespace(), request))


def _members(payload):
    return {entry["name"]: entry for entry in payload["members"]}


def test_route_is_wired_lazily_and_kept_off_worker_startup():
    assert '_chat_member_stats = _LazyModule("chat_member_stats")' in ENTRY
    assert '"/api/chat/member-stats", "/api/chat/member-stats/"' in APP_RUNTIME
    assert "_chat_member_stats.member_stats_route(" in APP_RUNTIME
    assert '"chat_member_stats.py",' in FOOTPRINT


def test_names_are_normalized_bounded_and_strict():
    module, _, _ = _load()
    assert module.parse_member_names(
        "/x?names=Alice,bob,,alice,not_valid,-bad") == ["alice", "bob"]
    assert module.parse_member_names("/x") == []
    with pytest.raises(ValueError, match="too_many_names"):
        module.parse_member_names(
            "/x?names=" + ",".join("user%d" % i for i in range(51)))
    with pytest.raises(ValueError, match="invalid_query"):
        module.parse_member_names("/x?names=alice&channel=1")
    with pytest.raises(ValueError, match="invalid_query"):
        module.parse_member_names("/x?names=alice&names=bob")


def test_payload_shape_counts_the_past_year_of_public_activity():
    module, db, _ = _load()
    _user(db, "alice")
    _user(db, "bob")
    _repo(db, "repo-a")
    _repo(db, "repo-b")
    _day(db, "repo-a", "alice", YESTERDAY, commits=5, pulls=2, reviews=1)
    _day(db, "repo-b", "alice", "2026-12-01", commits=1, issues=3)
    _day(db, "repo-a", "alice", "2025-12-01", commits=100, pulls=100)
    payload = _payload(module, ["alice", "bob"])
    assert payload["ok"] is True
    assert payload["window"] == {
        "from": "2026-01-16", "to": TODAY, "days": 365}
    assert payload["members"] == [
        {"name": "alice", "pulls": 2, "commits": 6, "issues": 3,
         "reviews": 1, "activeDays": 2, "lastActive": YESTERDAY},
        {"name": "bob", "pulls": 0, "commits": 0, "issues": 0,
         "reviews": 0, "activeDays": 0, "lastActive": ""},
    ]


def test_private_repository_activity_is_never_counted():
    module, db, _ = _load()
    _user(db, "alice")
    _repo(db, "public-repo")
    _repo(db, "private-repo", private=True)
    _repo(db, "hidden-project", public_project=False)
    _repo(db, "unverified", verified_from="2027-01-01")
    _day(db, "public-repo", "alice", YESTERDAY, commits=1)
    _day(db, "private-repo", "alice", YESTERDAY, commits=50, pulls=9)
    _day(db, "hidden-project", "alice", TODAY, issues=7, reviews=4)
    _day(db, "unverified", "alice", "2026-12-31", pulls=3)
    alice = _members(_payload(module, ["alice"]))["alice"]
    assert (alice["commits"], alice["pulls"], alice["issues"],
            alice["reviews"]) == (1, 0, 0, 0)
    assert alice["lastActive"] == YESTERDAY


def test_one_project_mirrored_twice_is_counted_once():
    module, db, _ = _load()
    _user(db, "alice")
    _repo(db, "mirror-1", project="shared")
    _repo(db, "mirror-2", project="shared")
    db.execute(
        "UPDATE profile_contribution_projects SET captured_at=2 "
        "WHERE source_repo_bi='mirror-2'")
    db.execute(
        "UPDATE profile_contribution_days SET project_bi='shared'")
    _day(db, "mirror-1", "alice", YESTERDAY, commits=4)
    _day(db, "mirror-2", "alice", YESTERDAY, commits=4)
    assert _members(_payload(module, ["alice"]))["alice"]["commits"] == 4


def test_only_public_human_members_are_returned():
    module, db, state = _load()
    _user(db, "alice")
    _user(db, "dave", profile_private=True)
    _user(db, "erin", status="suspended")
    _user(db, "node-one", kind="node")
    _user(db, "release-bot")
    _user(db, "claude-code")
    _repo(db, "repo-a")
    for name in ("dave", "erin", "node-one", "release-bot", "claude-code"):
        _day(db, "repo-a", name, YESTERDAY, commits=9)
    payload = _payload(module, [
        "alice", "carol", "dave", "erin", "node-one", "release-bot",
        "claude-code", "forkbot", "agent"])
    assert [entry["name"] for entry in payload["members"]] == ["alice"]
    users_query = next(sql for sql in state["queries"] if "FROM users" in sql)
    assert "release-bot" not in users_query


def test_instance_exclusion_list_is_honored():
    module, db, _ = _load()
    _user(db, "alice")
    _user(db, "deploy-helper")
    env = SimpleNamespace(ACTIVITY_EXCLUDE_AUTHORS="deploy-*, ")
    payload = asyncio.run(module.member_stats_payload(
        env, ["alice", "deploy-helper"], NOW))
    assert [entry["name"] for entry in payload["members"]] == ["alice"]


def test_people_whose_names_look_like_tools_stay_counted():
    module, _, _ = _load()
    for name in ("jules", "devin", "cursor", "aider", "llama", "talbot",
                 "claudette", "abbot"):
        assert not module.is_automated_member(name)
    for name in ("forkbot", "release-bot", "claude", "codex", "gpt-4o",
                 "copilot", "fmnext-core-1", "mirror2", "eu-mirror"):
        assert module.is_automated_member(name)


def test_a_batch_is_two_queries_however_many_names():
    module, db, state = _load()
    names = ["user%d" % index for index in range(50)]
    for name in names:
        _user(db, name)
    _repo(db, "repo-a")
    for name in names:
        _day(db, "repo-a", name, YESTERDAY, commits=1)
    payload = _payload(module, names)
    assert len(payload["members"]) == 50
    assert len(state["queries"]) == 2


def test_results_are_memoized_until_ttl_or_a_contribution_write():
    module, db, state = _load()
    _user(db, "alice")
    _repo(db, "repo-a")
    _day(db, "repo-a", "alice", YESTERDAY, commits=1)
    _payload(module, ["alice", "carol"])
    assert len(state["queries"]) == 2
    _payload(module, ["alice", "carol"])
    assert len(state["queries"]) == 2
    _day(db, "repo-a", "alice", TODAY, commits=2)
    state["generation"]["contributions"] += 1
    fresh = _members(_payload(module, ["alice"]))["alice"]
    assert fresh["commits"] == 3
    assert len(state["queries"]) == 4
    state["queries"].clear()
    asyncio.run(module.member_stats_payload(
        SimpleNamespace(), ["alice"], NOW + module.MEMBER_STATS_TTL_MS))
    assert len(state["queries"]) == 2


def test_memo_is_bounded():
    module, _, _ = _load()
    for index in range(module.MEMBER_STATS_MEMO_LIMIT + 25):
        module._memo_put("bi:%d" % index, NOW, 0, TODAY, None)
    assert len(module._MEMBER_STATS_MEMO) == module.MEMBER_STATS_MEMO_LIMIT
    assert "bi:0" not in module._MEMBER_STATS_MEMO


def test_route_caches_the_answer_and_rejects_bad_requests():
    module, db, state = _load()
    _user(db, "alice")
    first = _get(module, "names=alice")
    assert first.status == 200
    assert first.cache_control == "public, max-age=300"
    assert len(state["cache"]) == 1
    key = next(iter(state["cache"]))
    assert key.endswith("?names=alice&to=" + TODAY)
    queries = len(state["queries"])
    assert _get(module, "names=alice") is first
    assert len(state["queries"]) == queries
    assert _get(module, "names=" + ",".join(
        "user%d" % i for i in range(51))).status == 400
    assert _get(module, "names=alice", method="POST").status == 405


def test_member_list_requests_one_batch_for_visible_rows():
    assert CHAT.count("MEMBER_STATS_ENDPOINT") == 2
    assert 'const MEMBER_STATS_ENDPOINT = "/api/chat/member-stats";' in CHAT
    assert "const MEMBER_STATS_BATCH = 50;" in CHAT
    flush = CHAT[CHAT.index("async function flushMemberStats("):
                 CHAT.index("function watchMemberStats(")]
    assert flush.count("fetch(") == 1
    assert ".slice(0, MEMBER_STATS_BATCH)" in flush
    entry = CHAT[CHAT.index("function memberListEntry("):
                 CHAT.index("function isAutomatedMemberName(")]
    assert "fetch(" not in entry
    assert "freshMemberStats(person.name)" in entry
    assert "memberStatsObserver.observe(row)" in entry
    assert "!isAutomatedMemberName(person.name)" in entry
    assert "new IntersectionObserver(" in CHAT
    assert "root: memberPane" in CHAT


def test_member_stats_row_has_icons_tooltip_and_screen_reader_text():
    fill = CHAT[CHAT.index("function fillMemberStats("):
                CHAT.index("function applyMemberStats(")]
    for kind in ("pulls", "commits", "issues", "reviews", "recent"):
        assert 'data-member-stat="' in fill
        assert kind in CHAT[CHAT.index("const MEMBER_STATS_ICONS"):
                            CHAT.index("const MEMBER_STATS_LABELS")]
    assert "stats.title = summary;" in fill
    assert "data-member-stats-label" in fill
    assert 'aria-hidden="true"' in fill
    assert "escapeHtml(" in fill


def test_member_rows_stay_two_lines_and_open_as_a_drawer_on_phones():
    for source in (CHAT_VIEW, CHAT_PAGE):
        stats = source[source.index("[data-member-stats-visible] {"):]
        stats = stats[:stats.index("}")]
        assert "white-space: nowrap;" in stats
        assert "overflow: hidden;" in stats
        assert re.search(
            r"@media \(max-width: 1199px\) \{.*?\[data-members-open\] "
            r"\[data-dashboard-chat-members\] \{[^}]*position: absolute;",
            source, re.S)
        row = source[source.index(
            'html:not([data-world-embed="1"]) [data-dashboard-chat-member] {'):]
        row = row[:row.index("}")]
        assert "min-height: 40px;" in row
        assert "height: 40px;" not in row.replace("min-height: 40px;", "")
