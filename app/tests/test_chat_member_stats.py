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
        "profile_contribution_projects (", "profile_contribution_days (",
        "member_repo_activity (", "member_activity_repos (",
        "idx_member_repo_activity_user ")
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
            "contributions": 0}, "catalog": [], "people": {}, "fetches": [],
        "writes": []}

    class _Date:
        @staticmethod
        def now():
            return state["now"]

    async def d1_all(env, sql, *params):
        state["queries"].append(sql)
        return [dict(row) for row in db.execute(sql, params).fetchall()]

    async def d1_run(env, sql, *params):
        state["writes"].append(sql)
        db.execute(sql, params)

    async def d1_run_batch(env, statements):
        state["writes"].append("batch")
        for sql, params in statements:
            db.execute(sql, params)

    async def decrypted_public_catalog(env, now):
        return state["catalog"]

    def public_base_url(env, request=None):
        return "https://forge.example.test"

    class _Response:
        def __init__(self, data):
            self.status = 200 if data is not None else 503
            self._data = data

        async def json(self):
            return self._data

    async def js_fetch_with_timeout(url, init, timeout):
        state["fetches"].append(url)
        path = url.split("/api/repo/", 1)[1].rsplit("/pulse", 1)[0]
        return _Response(state["people"].get(path))

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
        "d1_run": d1_run,
        "d1_run_batch": d1_run_batch,
        "_decrypted_public_catalog": decrypted_public_catalog,
        "_public_base_url": public_base_url,
        "js_fetch_with_timeout": js_fetch_with_timeout,
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


WINDOW_START = "2026-01-16"


def _mask(days, start=WINDOW_START):
    import datetime
    first = datetime.date.fromisoformat(start).toordinal()
    mask = bytearray(46)
    for day in days:
        offset = datetime.date.fromisoformat(day).toordinal() - first
        mask[offset // 8] |= 1 << (offset % 8)
    return mask.hex()


def _repo_activity(db, repo, name, commits=0, pulls=0, issues=0, reviews=0,
                   days=(), last_day="", day_start=WINDOW_START):
    db.execute(
        "INSERT INTO member_repo_activity (repo_bi, user_bi, commits, pulls, "
        "issues, reviews, day_start, day_mask, last_day, updated_at) "
        "VALUES (?,?,?,?,?,?,?,?,?,?)",
        (repo, "bi:" + name, commits, pulls, issues, reviews, day_start,
         _mask(days, day_start), last_day or (max(days) if days else ""), 1))


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
        _repo_activity(db, "repo-a", name, commits=3)
        _repo_activity(db, "repo-b", name, pulls=1)
    db.execute("INSERT INTO repositories (key_bi) VALUES ('repo-b')")
    payload = _payload(module, names)
    assert len(payload["members"]) == 50
    assert all((entry["commits"], entry["pulls"]) == (3, 1)
               for entry in payload["members"])
    assert len(state["queries"]) == 2
    activity = state["queries"][1]
    assert "profile_contribution_days" in activity
    assert "member_repo_activity" in activity
    # 50 names, the window, and the same names again as one JSON value:
    # inside D1's 100 bound parameters.
    assert activity.count("?") == 50 + 2 + 1


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


# ---------------------------------------------------------------------------
# Repository source


def _catalog_row(key, owner, name, **record):
    return {"key_bi": key, "is_private": 0, "data": {
        "owner": owner, "name": name, "visibility": "public",
        "source": "hosted-web", **record}}


def _people(*rows, day_start=WINDOW_START):
    return {"ok": True, "view": "people", "dayStart": day_start,
            "people": [dict({"commits": 0, "pulls": 0, "issues": 0,
                             "reviews": 0, "dayMask": "", "lastAt": 0},
                            **row) for row in rows]}


def _sweep(module, state, now=NOW):
    return asyncio.run(module.refresh_member_repo_activity(
        SimpleNamespace(), now))


def test_repository_activity_counts_when_the_profile_source_is_empty():
    module, db, _ = _load()
    _user(db, "alice")
    for repo in ("repo-a", "repo-b"):
        db.execute("INSERT INTO repositories (key_bi) VALUES (?)", (repo,))
    _repo_activity(db, "repo-a", "alice", commits=40, pulls=2, reviews=1,
                   days=("2026-12-01", YESTERDAY))
    _repo_activity(db, "repo-b", "alice", commits=2, issues=1,
                   days=("2026-12-01", "2026-12-02"))
    alice = _members(_payload(module, ["alice"]))["alice"]
    assert alice == {
        "name": "alice", "pulls": 2, "commits": 42, "issues": 1,
        "reviews": 1, "activeDays": 3, "lastActive": YESTERDAY}


def test_private_and_deleted_repositories_never_count():
    module, db, _ = _load()
    _user(db, "alice")
    db.execute("INSERT INTO repositories (key_bi) VALUES ('public-repo')")
    db.execute(
        "INSERT INTO repositories (key_bi, is_private) VALUES ('secret', 1)")
    _repo_activity(db, "public-repo", "alice", commits=1, days=(YESTERDAY,))
    _repo_activity(db, "secret", "alice", commits=90, pulls=9,
                   days=(TODAY,))
    _repo_activity(db, "deleted-repo", "alice", issues=5, days=(TODAY,))
    alice = _members(_payload(module, ["alice"]))["alice"]
    assert (alice["commits"], alice["pulls"], alice["issues"],
            alice["activeDays"], alice["lastActive"]) == (
        1, 0, 0, 1, YESTERDAY)


def test_the_two_sources_combine_by_the_larger_figure_not_the_sum():
    module, db, _ = _load()
    _user(db, "alice")
    _repo(db, "repo-a")
    _day(db, "repo-a", "alice", "2026-12-01", commits=6, pulls=2)
    _day(db, "repo-a", "alice", "2026-12-02", reviews=3)
    _repo_activity(db, "repo-a", "alice", commits=4, pulls=5,
                   days=("2026-12-01",), last_day="2026-12-01")
    alice = _members(_payload(module, ["alice"]))["alice"]
    assert (alice["commits"], alice["pulls"], alice["reviews"],
            alice["activeDays"], alice["lastActive"]) == (
        6, 5, 3, 2, "2026-12-02")
    combined = module.combine_member_activity(
        {"commits": 1, "active_days": 1, "last_day": "2026-02-01"},
        [{"commits": 2, "day_start": WINDOW_START,
          "day_mask": _mask(("2026-01-16", "2026-03-01")),
          "last_day": "2026-03-01"}],
        "2026-02-01", TODAY)
    assert combined == {
        "pulls": 0, "commits": 2, "issues": 0, "reviews": 0,
        "active_days": 1, "last_day": "2026-03-01"}


def test_fork_families_count_once_and_private_repositories_never():
    module, _, _ = _load()
    root = "a" * 40
    rows = [
        _catalog_row("k-main", "ann", "app", logicalOwner="team",
                     rootCommit=root, commitCount=90),
        _catalog_row("k-fork", "bob", "app", forkedFrom="team/app",
                     commitCount=95),
        _catalog_row("k-fork2", "cat", "app", forkedFrom="ANN/APP"),
        _catalog_row("k-copy", "dan", "app-copy", rootCommit=root,
                     commitCount=10),
        _catalog_row("k-own", "eve", "tool", forkedFrom="elsewhere/tool"),
        _catalog_row("k-secret", "ann", "secret"),
        dict(_catalog_row("k-flag", "ann", "flag"), is_private=1),
    ]
    rows[5]["data"]["visibility"] = "private"
    sources = module.member_activity_sources(rows)
    assert [item["key_bi"] for item in sources] == ["k-main", "k-own"]
    assert sources[0] == {"key_bi": "k-main", "owner": "ann", "name": "app"}


def test_only_repositories_hosted_on_this_instance_count():
    module, _, _ = _load()
    # A mirror of a project hosted elsewhere never counts, and does not make
    # the hosted fork of it a duplicate: the fork is that family's copy here.
    rows = [
        _catalog_row("k-mirror", "upstream", "tool", source="remote-clone",
                     logicalOwner="team", commitCount=900),
        _catalog_row("k-fork", "ann", "tool", forkedFrom="team/tool"),
        _catalog_row("k-sync", "ann", "lib", source="github"),
        _catalog_row("k-none", "ann", "misc", source=""),
    ]
    sources = module.member_activity_sources(rows)
    assert [item["key_bi"] for item in sources] == ["k-fork"]


def test_people_map_to_accounts_only_by_their_exact_pulse_name():
    module, _, _ = _load()
    rows = dict(module.member_activity_people(_people(
        {"key": "alice", "commits": 3, "dayMask": "01", "lastAt": NOW},
        {"key": "Bob", "pulls": 1, "dayMask": "zz"},
        {"key": "carol jones", "commits": 9},
        {"key": "release-bot", "commits": 9},
        {"key": "claude", "commits": 9},
        {"key": "deploy-helper", "commits": 9},
        {"key": "idle"},
        {"key": "alice", "commits": 100},
    ), ("deploy-*",)))
    assert sorted(rows) == ["alice", "bob"]
    assert rows["alice"]["commits"] == 3
    assert rows["alice"]["last_day"] == TODAY
    assert rows["alice"]["day_mask"] == "01"
    assert rows["bob"]["day_mask"] == ""
    assert module.member_activity_people({"view": "leaderboard"}) == []
    assert module.member_activity_people(
        _people({"key": "alice", "commits": 1}, day_start="bad")) == []


def test_the_sweep_stores_accounts_and_refreshes_on_schedule():
    module, db, state = _load()
    _user(db, "alice")
    _user(db, "bob")
    for repo in ("k-a", "k-b", "k-fork"):
        db.execute("INSERT INTO repositories (key_bi) VALUES (?)", (repo,))
    state["catalog"] = [
        _catalog_row("k-a", "ann", "app"),
        _catalog_row("k-b", "ann", "tool"),
        _catalog_row("k-fork", "bob", "app", forkedFrom="ann/app"),
    ]
    state["people"] = {
        "ann/app": _people(
            {"key": "alice", "commits": 5, "pulls": 1,
             "dayMask": _mask((YESTERDAY,)), "lastAt": NOW - 86400000},
            {"key": "carol", "commits": 7},
            {"key": "dependabot", "commits": 50}),
        "ann/tool": _people({"key": "alice", "reviews": 2}),
        "bob/app": _people({"key": "alice", "commits": 500}),
    }
    result = _sweep(module, state)
    assert sorted(result["refreshed"]) == ["ann/app", "ann/tool"]
    assert sorted(url.split("/api/repo/")[1] for url in state["fetches"]) == [
        "ann/app/pulse?view=people", "ann/tool/pulse?view=people"]
    stored = db.execute(
        "SELECT repo_bi, user_bi, commits, pulls, reviews, last_day "
        "FROM member_repo_activity ORDER BY repo_bi").fetchall()
    assert [tuple(row) for row in stored] == [
        ("k-a", "bi:alice", 5, 1, 0, YESTERDAY),
        ("k-b", "bi:alice", 0, 0, 2, "")]
    alice = _members(_payload(module, ["alice"]))["alice"]
    assert (alice["commits"], alice["pulls"], alice["reviews"],
            alice["activeDays"], alice["lastActive"]) == (
        5, 1, 2, 1, YESTERDAY)
    # Fresh repositories are not read again until the refresh interval.
    state["fetches"].clear()
    assert _sweep(module, state, NOW + 60_000)["refreshed"] == []
    assert state["fetches"] == []
    later = NOW + module.MEMBER_ACTIVITY_REFRESH_MS
    state["people"]["ann/app"] = None
    result = _sweep(module, state, later)
    assert result["refreshed"] == ["ann/tool"]
    assert db.execute(
        "SELECT ok FROM member_activity_repos WHERE repo_bi='k-a'"
    ).fetchone()[0] == 0
    assert db.execute(
        "SELECT commits FROM member_repo_activity WHERE repo_bi='k-a'"
    ).fetchone()[0] == 5
    # A repository that stops counting loses its rows on the next run.
    state["catalog"] = state["catalog"][1:]
    assert _sweep(module, state, later + 1)["dropped"] == 1
    assert db.execute(
        "SELECT COUNT(*) FROM member_repo_activity WHERE repo_bi='k-a'"
    ).fetchone()[0] == 0


def test_the_sweep_reads_a_bounded_number_of_repositories_per_run():
    module, db, state = _load()
    state["catalog"] = [
        _catalog_row("k-%02d" % index, "ann", "repo%d" % index)
        for index in range(module.MEMBER_ACTIVITY_REPOS_PER_RUN + 4)]
    result = _sweep(module, state)
    assert len(state["fetches"]) == module.MEMBER_ACTIVITY_REPOS_PER_RUN
    assert result["due"] == 4
    state["fetches"].clear()
    _sweep(module, state, NOW + 1)
    assert len(state["fetches"]) == 4


def test_the_request_path_never_reads_a_repository():
    module, db, state = _load()
    _user(db, "alice")
    state["catalog"] = [_catalog_row("k-a", "ann", "app")]
    _payload(module, ["alice"])
    _get(module, "names=alice")
    assert state["fetches"] == [] and state["writes"] == []
    assert "refresh_member_repo_activity" not in module.member_stats_payload \
        .__code__.co_names


def test_the_member_activity_phase_is_scheduled_and_dispatched():
    assert 'phases.append("member-activity")' in ENTRY
    assert ("return await _chat_member_stats.refresh_member_repo_activity("
            "env)") in ENTRY
    assert '("member_repo_activity", "repo_bi"),' in ENTRY
    assert "DELETE FROM member_repo_activity WHERE user_bi=?" in ENTRY
