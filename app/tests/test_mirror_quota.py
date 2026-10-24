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

import mirror_quota as mq  # noqa: E402
import schema  # noqa: E402

OURS = "a" * 40
OLDER = "b" * 40
OTHER = "c" * 40
GITHUB = "d" * 40
HOUR = 3600 * 1000


def bi(text):
    return hashlib.sha256(str(text).strip().lower().encode()).hexdigest()


def pkt(text):
    data = text.encode()
    return b"%04x" % (len(data) + 4) + data


def advertisement(head, branch="main", extra=()):
    lines = [pkt("# service=git-upload-pack\n"), b"0000",
             pkt("%s HEAD\0multi_ack symref=HEAD:refs/heads/%s agent=git/2\n" % (head, branch)),
             pkt("%s refs/heads/%s\n" % (head, branch))]
    lines += [pkt("%s %s\n" % (commit, ref)) for commit, ref in extra]
    return b"".join(lines) + b"0000"


class FakeD1:
    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        for statement in schema.SCHEMA_STATEMENTS:
            if "repo_mirror_" in statement:
                self.db.execute(statement)
        self.db.execute("CREATE TABLE repositories (key_bi TEXT PRIMARY KEY, "
                        "is_private INTEGER, data TEXT)")

    async def all(self, env, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]

    async def first(self, env, sql, *args):
        rows = await self.all(env, sql, *args)
        return rows[0] if rows else None

    async def run(self, env, sql, *args):
        self.db.execute(sql, args)

    def rows(self, table):
        return [dict(row) for row in self.db.execute("SELECT * FROM " + table)]


class Probes:
    def __init__(self):
        self.answers = {}
        self.calls = []

    async def __call__(self, url, timeout):
        self.calls.append(url)
        answer = self.answers.get(url)
        if answer is None:
            raise TypeError("fetch failed: connection refused")
        if isinstance(answer, Exception):
            raise answer
        return answer


@pytest.fixture
def rt(monkeypatch):
    d1 = FakeD1()
    clock = SimpleNamespace(now=1_800_000_000_000)
    roles = {"maintainers": {"alice"}, "admins": set()}
    repos = {"example/forum": {"visibility": "public", "commit": OURS, "branch": "main"},
             "example/secret": {"visibility": "private", "commit": OURS, "branch": "main"}}
    probes = Probes()
    source = {"doc": {"schema": "forkmesh-forge-source-v1", "provider": "github",
                      "repository": "example/forum"}}

    def sync_repos():
        for key, info in repos.items():
            owner, name = key.split("/")
            record = {"owner": owner, "name": name, "visibility": info["visibility"],
                      "commit": info["commit"], "branch": info["branch"]}
            d1.db.execute("INSERT OR REPLACE INTO repositories VALUES (?,?,?)",
                          (bi(key), 0 if info["visibility"] == "public" else 1,
                           "enc:" + json.dumps(record)))

    async def blind_index(env, text):
        return bi(text)

    async def encrypt_row(env, obj):
        return "enc:" + json.dumps(obj)

    async def decrypt_row(env, stored):
        assert stored.startswith("enc:")
        return json.loads(stored[4:])

    async def access(env, request, owner, repo, viewer=""):
        key = (owner + "/" + repo).lower()
        info = repos.get(key)
        if info is None:
            return None
        if info["visibility"] != "public":
            return None
        return {"repoBi": bi(key), "visibility": info["visibility"],
                "record": {"owner": owner, "name": repo, "commit": info["commit"],
                           "branch": info["branch"], "visibility": "public"},
                "viewer": viewer}

    async def session(env, request, data=None):
        if not request.account:
            return "", None
        return bi(request.account), {"name": request.account}

    async def authed(env, request, data=None):
        return request.account

    async def owns(env, actor, owner):
        return actor in roles["maintainers"]

    async def org_permission(env, owner, repo, actor):
        return "maintain" if actor == "mia" else ""

    async def is_admin(env, name):
        return name in roles["admins"]

    async def nothing(env):
        return None

    async def body(request):
        if isinstance(request.body, Exception):
            raise request.body
        return request.body

    class FakeResponse:
        def __init__(self, status, data):
            self.status = status
            self._data = data

        async def text(self):
            return self._data

    async def fetch(url, init, timeout):
        if "/raw?path=" in url and source["doc"] is not None:
            return FakeResponse(200, json.dumps(source["doc"]))
        return FakeResponse(404, "")

    def json_response(data, status=200, cache_control=None, extra_headers=None, **_):
        return SimpleNamespace(status=status, data=data, cache_control=cache_control,
                               headers=extra_headers or {})

    runtime = {
        "d1_all": d1.all, "d1_first": d1.first, "d1_run": d1.run,
        "blind_index": blind_index, "encrypt_row": encrypt_row,
        "decrypt_row": decrypt_row, "_repository_access_context": access,
        "_account_session_record": session, "_authed_account_name": authed,
        "_account_owns_node": owns, "_org_linked_repo_permission": org_permission,
        "_is_admin": is_admin,
        "team_permission_rank": lambda p: ("read", "write", "maintain", "admin").index(p)
        if p in ("read", "write", "maintain", "admin") else -1,
        "ensure_schema": nothing, "bounded_json_request": body,
        "json_response": json_response,
        "method_name": lambda request: request.method,
        "clean_string": lambda value, n=240: value.strip()[:n] if isinstance(value, str) else "",
        "safe_segment": lambda value: value if re.fullmatch(r"[A-Za-z0-9._-]+", value or "") else "",
        "_public_base_url": lambda env, request=None: "https://forum.example.org",
        "js_fetch_with_timeout": fetch,
        "MAX_NODE_NAME": 63,
        "Date": SimpleNamespace(now=lambda: clock.now),
        "probe": probes,
    }
    for name, value in runtime.items():
        monkeypatch.setattr(mq, name, value, raising=False)
    monkeypatch.setattr(mq.random, "randint", lambda a, b: 0)
    sync_repos()
    return SimpleNamespace(d1=d1, clock=clock, roles=roles, repos=repos, probes=probes,
                           source=source, sync=sync_repos)


def run(coro):
    return asyncio.run(coro)


def call(method, account="", body=None, query="", owner="example", repo="forum"):
    request = SimpleNamespace(
        method=method, account=account, body=body if body is not None else {},
        url="https://forum.example.org/api/repo/%s/%s/mirror-quota%s" % (
            owner, repo, query))
    return run(mq.handler(SimpleNamespace(), request, owner, repo))


def rows_by_id(response):
    return {row["id"]: row for row in response.data["mirrors"]}


def test_url_rules():
    ok = mq.parse_mirror_url("https://Mirror.Example.org/example/forum.git/")
    assert ok["url"] == "https://mirror.example.org/example/forum.git"
    assert ok["checkable"] and ok["scheme"] == "https"
    assert mq.parse_mirror_url("https://mirror.example.org/example/forum")["id"] == ok["id"]
    assert mq.parse_mirror_url("git://mirror.example.org/example/forum")["id"] == ok["id"]
    assert not mq.parse_mirror_url("git://mirror.example.org/bbs")["checkable"]
    assert not mq.parse_mirror_url("git@codeberg.org:example/forum.git")["checkable"]
    assert mq.parse_mirror_url("ssh://git@host.example.net:2222/bbs")["host"] == \
        "host.example.net:2222"
    assert not mq.parse_mirror_url("https://abcdefg.onion/bbs")["checkable"]
    for bad in ("http://mirror.example.org/bbs", "https://user:pw@mirror.example.org/bbs",
                "https://user@mirror.example.org/bbs", "https://10.0.0.1/bbs",
                "https://localhost/bbs", "https://box.local/bbs", "https://mirror.example.org",
                "https://mirror.example.org/bbs?x=1", "https://mirror.example.org/b bs",
                "file:///etc/passwd", "ftp://mirror.example.org/bbs", "",
                "https://mirror.example.org/" + "a" * 400, "https://[::1]/bbs",
                "https://mirror.example.org/café"):
        assert mq.parse_mirror_url(bad) is None, bad


def test_kind_inference():
    assert mq.infer_kind("github.com") == "github"
    assert mq.infer_kind("gitlab.com") == "gitlab"
    assert mq.infer_kind("gitlab.example.org") == "gitlab"
    assert mq.infer_kind("codeberg.org") == "codeberg"
    assert mq.infer_kind("forum.forkmesh.com") == "forkmesh-instance"
    assert mq.infer_kind("mirror.example.org", "git") == "git"
    assert mq.infer_kind("mirror.example.org") == "other"
    assert mq.choose_kind("mirror.example.org", "https", "forkmesh-instance") == \
        "forkmesh-instance"
    assert mq.choose_kind("github.com", "https", "gitlab") == "github"
    assert mq.choose_kind("mirror.example.org", "https", "bogus") == "other"


def test_ref_advertisement_parsing():
    refs, symref = mq.parse_refs(advertisement(OURS, "trunk"))
    assert symref == "refs/heads/trunk"
    assert mq.head_of(refs, "trunk") == OURS
    assert mq.head_of(refs, "main") == OURS
    dumb = ("%s\trefs/heads/main\n%s\trefs/tags/v1\n" % (OTHER, OURS)).encode()
    refs, _ = mq.parse_refs(dumb)
    assert mq.head_of(refs, "main") == OTHER
    assert mq.parse_refs(b"<html>not git</html>")[0] == {}
    assert mq.parse_refs(b"00zz")[0] == {}


def test_health_classification():
    now = 10 * 24 * HOUR
    heads = {OURS, OLDER}
    assert mq.classify({}, heads, now) == ("unknown", "Not checked yet")
    assert mq.classify({}, heads, now, checkable=False)[0] == "unknown"
    state = mq.apply_check({}, {"at": now, "ok": True, "headCommit": OURS})
    assert mq.classify(state, heads, now)[0] == "green"
    state = mq.apply_check({}, {"at": now, "ok": True, "headCommit": OLDER})
    assert mq.classify(state, heads, now)[0] == "green"
    state = mq.apply_check({}, {"at": now, "ok": True, "headCommit": OTHER})
    assert mq.classify(state, heads, now) == ("yellow", "Behind or different")
    state = mq.apply_check({}, {"at": now, "ok": True, "headCommit": ""})
    assert mq.classify(state, heads, now)[0] == "yellow"
    state = mq.apply_check({}, {"at": now, "ok": False, "error": "unreachable"})
    assert mq.classify(state, heads, now) == ("red", "Never reached")
    state = mq.apply_check({}, {"at": now - 2 * HOUR, "ok": True, "headCommit": OURS})
    state = mq.apply_check(state, {"at": now, "ok": False})
    assert mq.classify(state, heads, now)[0] == "green"
    state = mq.apply_check(state, {"at": now, "ok": False})
    assert mq.classify(state, heads, now) == ("red", "Unreachable")
    state = mq.apply_check({}, {"at": now - 25 * HOUR, "ok": True, "headCommit": OURS})
    state = mq.apply_check(state, {"at": now, "ok": False})
    assert mq.classify(state, heads, now) == ("yellow", "Not reached for a day")


def test_head_history_keeps_a_day():
    now = 5 * 24 * HOUR
    heads = mq.remember_head([], OLDER, now - 30 * HOUR)
    heads = mq.remember_head(heads, OTHER, now - 2 * HOUR)
    heads = mq.remember_head(heads, OURS, now)
    assert {row["c"] for row in heads} == {OTHER, OURS}
    assert mq.recent_heads(heads, OURS, now) == {OTHER, OURS}
    assert mq.recent_heads(heads, OURS, now + 23 * HOUR) == {OURS}


def test_source_classification_and_document():
    now = 5 * 24 * HOUR
    source = mq.source_from_document({"schema": "forkmesh-forge-source-v1",
                                      "provider": "github", "repository": "example/forum"})
    assert source["url"] == "https://github.com/example/forum"
    assert source["label"] == "GitHub (original)" and source["kind"] == "github"
    gitea = mq.source_from_document({"schema": "forkmesh-forge-source-v1",
                                     "provider": "gitea", "repository": "a/b",
                                     "baseUrl": "https://codeberg.org"})
    assert gitea["url"] == "https://codeberg.org/a/b" and gitea["kind"] == "codeberg"
    assert mq.source_from_document({"schema": "x", "provider": "github",
                                    "repository": "a/b"}) is None
    assert mq.source_from_document({"schema": "forkmesh-forge-source-v1",
                                    "provider": "github", "repository": "../x"}) is None
    state = mq.apply_check({}, {"at": now - 30 * HOUR, "ok": True, "headCommit": GITHUB},
                           OURS)
    state = mq.apply_check(state, {"at": now, "ok": True, "headCommit": GITHUB}, OURS)
    assert mq.classify_source(state, OURS, now) == ("yellow", "Our copy is behind")
    assert mq.classify_source(state, "", now)[0] == "green"
    state = mq.apply_check(state, {"at": now, "ok": True, "headCommit": OURS}, OURS)
    assert mq.classify_source(state, OURS, now)[0] == "green"


def test_anonymous_read_shows_this_instance_only_until_configured(rt):
    response = call("GET")
    assert response.status == 200
    assert response.cache_control == mq.PUBLIC_CACHE
    data = response.data
    assert data["target"] == 0 and data["registered"] == 0 and "viewer" not in data
    assert [row["id"] for row in data["mirrors"]] == ["self"]
    assert data["mirrors"][0]["url"] == "https://forum.example.org/example/forum"
    assert data["cloneUrl"] == "https://forum.example.org/example/forum.git"
    assert data["healthy"] == 1


def test_private_and_unknown_repositories_are_not_found(rt):
    assert call("GET", owner="example", repo="secret").status == 404
    assert call("GET", owner="example", repo="nope").status == 404
    assert call("POST", "bob", {"url": "https://m.example.org/bbs"},
                repo="secret").status == 404


def test_only_maintainers_set_the_target(rt):
    assert call("PUT", "", {"target": 10}).status == 401
    assert call("PUT", "bob", {"target": 10}).status == 403
    assert call("PUT", "alice", {"target": 101}).status == 400
    assert call("PUT", "alice", {"target": True}).status == 400
    response = call("PUT", "alice", {"target": 10})
    assert response.status == 200 and response.data["target"] == 10
    assert response.data["viewer"]["canSetTarget"] is True
    assert call("PUT", "mia", {"target": 12}).data["target"] == 12
    rt.roles["admins"].add("root")
    assert call("PUT", "root", {"target": 9}).data["target"] == 9
    assert call("GET").data["target"] == 9
    viewer = call("GET", "bob").data["viewer"]
    assert viewer == {"signedIn": True, "name": "bob", "canSetTarget": False, "canAdd": True}


def test_signed_in_users_add_mirrors_with_validation_and_dedupe(rt):
    assert call("POST", "", {"url": "https://m.example.org/bbs"}).status == 401
    assert call("POST", "bob", {"url": "http://m.example.org/bbs"}).status == 400
    assert call("POST", "bob", {"url": "https://forum.example.org/x/bbs"}
                ).data["error"] == "same_instance"
    added = call("POST", "bob", {"url": "https://m.example.org/example/forum.git",
                                 "label": "  Bob's\n mirror "})
    assert added.status == 201
    row = rows_by_id(added)[added.data["added"]]
    assert row["label"] == "Bob's mirror" and row["kind"] == "other"
    assert row["addedBy"] == "bob" and row["status"] == "unknown" and row["canRemove"]
    assert call("POST", "carol", {"url": "git://m.example.org/example/forum"}
                ).data["error"] == "duplicate"
    stored = rt.d1.rows("repo_mirror_registry")
    assert len(stored) == 1 and stored[0]["data"].startswith("enc:")
    assert stored[0]["next_check_at"] == 0
    quota = rt.d1.rows("repo_mirror_quota")
    assert len(quota) == 1 and quota[0]["target"] == 0


def test_display_only_mirrors_are_listed_but_not_probed(rt):
    added = call("POST", "bob", {"url": "git@codeberg.org:example/forum.git"})
    row = rows_by_id(added)[added.data["added"]]
    assert row["checkable"] is False and row["kind"] == "codeberg"
    assert row["status"] == "unknown"
    run(mq.run_checks(SimpleNamespace(), rt.clock.now + 1))
    assert not [url for url in rt.probes.calls if "codeberg" in url]


def test_adds_are_rate_limited_and_capped(rt, monkeypatch):
    for index in range(3):
        assert call("POST", "bob", {"url": "https://m%d.example.org/bbs" % index}).status == 201
    limited = call("POST", "bob", {"url": "https://m9.example.org/bbs"})
    assert limited.status == 429 and limited.headers["Retry-After"] == "60"
    rt.clock.now += 2 * 60 * 1000
    assert call("POST", "bob", {"url": "https://m9.example.org/bbs"}).status == 201
    monkeypatch.setattr(mq, "MAX_MIRRORS", 4)
    full = call("POST", "carol", {"url": "https://m10.example.org/bbs"})
    assert full.status == 409 and full.data["error"] == "too_many_mirrors"


def test_remove_is_for_the_adder_or_a_maintainer(rt):
    first = call("POST", "bob", {"url": "https://m1.example.org/bbs"}).data["added"]
    second = call("POST", "bob", {"url": "https://m2.example.org/bbs"}).data["added"]
    assert call("DELETE", "carol", {"id": first}).status == 403
    assert rows_by_id(call("GET", "carol"))[first]["canRemove"] is False
    assert call("DELETE", "bob", {"id": "nothex"}).status == 400
    assert call("DELETE", "bob", {}, query="?id=" + first).status == 200
    assert call("DELETE", "alice", {"id": second}).status == 200
    assert call("DELETE", "alice", {"id": second}).status == 404
    assert rt.d1.rows("repo_mirror_registry") == []


def test_cron_checks_mirrors_and_the_forge_source(rt):
    call("PUT", "alice", {"target": 10})
    good = "https://good.example.org/example/forum"
    stale = "https://stale.example.org/example/forum"
    down = "https://down.example.org/example/forum"
    for url in (good, stale, down):
        assert call("POST", "bob" if url != down else "carol", {"url": url}).status == 201
    rt.probes.answers[good] = (200, advertisement(OURS))
    rt.probes.answers[stale] = (200, advertisement(OTHER))
    rt.probes.answers["https://github.com/example/forum"] = (200, advertisement(GITHUB))
    report = run(mq.run_checks(SimpleNamespace()))
    assert report["checked"] == 4
    data = call("GET").data
    rows = {row["host"]: row for row in data["mirrors"]}
    assert rows["github.com"]["label"] == "GitHub (original)"
    assert rows["github.com"]["status"] == "green"
    assert rows["good.example.org"]["status"] == "green"
    assert rows["good.example.org"]["lastCheck"]["headCommit"] == OURS
    assert rows["stale.example.org"]["status"] == "yellow"
    assert rows["down.example.org"]["status"] == "red"
    assert rows["down.example.org"]["lastCheck"]["error"] == "unreachable"
    assert data["healthy"] == 3 and data["total"] == 5 and data["target"] == 10
    calls = len(rt.probes.calls)
    run(mq.run_checks(SimpleNamespace()))
    assert len(rt.probes.calls) == calls
    rt.clock.now += 16 * 60 * 1000
    rt.repos["example/forum"]["commit"] = OLDER
    rt.sync()
    run(mq.run_checks(SimpleNamespace()))
    assert len(rt.probes.calls) == calls + 4
    rows = {row["host"]: row for row in call("GET").data["mirrors"]}
    assert rows["good.example.org"]["status"] == "green"


def test_cron_respects_the_per_run_budget(rt, monkeypatch):
    for index in range(9):
        call("POST", "user%d" % index, {"url": "https://m%d.example.org/bbs" % index})
    rt.source["doc"] = None
    report = run(mq.run_checks(SimpleNamespace()))
    assert report["checked"] == mq.CHECKS_PER_RUN
    assert len(rt.probes.calls) == mq.CHECKS_PER_RUN
    rt.clock.now += mq.CHECK_INTERVAL_MS + 1
    monkeypatch.setattr(mq, "RUN_BUDGET_MS", -1)
    report = run(mq.run_checks(SimpleNamespace(), rt.clock.now))
    assert report["checked"] == 0 and report["skipped"] > 0


def test_check_url_reports_errors_without_raising(rt):
    rt.probes.answers["https://m.example.org/a"] = (404, b"")
    rt.probes.answers["https://m.example.org/b"] = (200, b"<html></html>")
    rt.probes.answers["https://m.example.org/c"] = TypeError("fetch blocked: private address")
    rt.probes.answers["https://m.example.org/d"] = (200, advertisement(OURS, "trunk"))
    assert run(mq.check_url("https://m.example.org/a", "main"))["error"] == "HTTP 404"
    assert run(mq.check_url("https://m.example.org/b", "main"))["error"] == \
        "no git refs advertised"
    assert "network policy" in run(mq.check_url("https://m.example.org/c", "main"))["error"]
    found = run(mq.check_url("https://m.example.org/d", "main"))
    assert found["ok"] and found["headCommit"] == OURS


def test_bounded_body_stops_reading_past_the_cap():
    class Reader:
        def __init__(self):
            self.cancelled = False
            self.reads = 0

        async def read(self):
            self.reads += 1
            return {"done": False, "value": b"x" * 1024}

        def cancel(self):
            self.cancelled = True

    reader = Reader()
    response = SimpleNamespace(body=SimpleNamespace(getReader=lambda: reader))
    assert run(mq.bounded_body(response, 4096)) is None
    assert reader.cancelled and reader.reads == 5


def test_the_route_cron_phase_and_dashboard_are_wired():
    runtime = (ROOT / "src" / "app_runtime.py").read_text("utf-8")
    assert 'action == "mirror-quota" and len(pieces) == 5' in runtime
    assert "mirror_quota.handler(" in runtime
    entry = (ROOT / "src" / "entry.py").read_text("utf-8")
    assert 'if phase == "mirror-quota":' in entry and "mirror_quota.run_checks(env)" in entry
    assert '("repo_mirror_quota", "repo_bi")' in entry
    assert '("repo_mirror_registry", "repo_bi")' in entry
    js = ROOT / "public" / "dashboard" / "js"
    assert "initRepoMirrorQuota();" in (js / "10-boot.js").read_text("utf-8")
    assert "loadRepoMirrorQuota(repo);" in (js / "06-repo-content.js").read_text("utf-8")
    card = (js / "09-mirror-quota.js").read_text("utf-8")
    for needle in ("git clone --mirror", ".forkmesh/issues", "start your own ForkMesh instance",
                   "data-mirror-quota-add", "Set target", "Set a mirror goal"):
        assert needle in card
    for banned in ("<img", "favicon", "bg-cta", "fm-btn-cta", "text-success"):
        assert banned not in card
    import dashboard_bundle
    assert "09-mirror-quota.js" in dashboard_bundle.FRAGMENTS


def test_a_failed_forge_source_read_keeps_the_original_row(rt, monkeypatch):
    call("PUT", "alice", {"target": 10})
    rt.probes.answers["https://github.com/example/forum"] = (200, advertisement(GITHUB))
    run(mq.run_checks(SimpleNamespace()))
    assert "source" in rows_by_id(call("GET"))
    working = mq.js_fetch_with_timeout

    async def broken(url, init, timeout):
        raise TypeError("fetch failed")
    monkeypatch.setattr(mq, "js_fetch_with_timeout", broken)
    rt.clock.now += mq.SOURCE_READ_INTERVAL_MS + 1
    run(mq.run_checks(SimpleNamespace()))
    assert "source" in rows_by_id(call("GET"))
    monkeypatch.setattr(mq, "js_fetch_with_timeout", working)
    rt.source["doc"] = None
    rt.clock.now += mq.SOURCE_RETRY_MS + 1
    run(mq.run_checks(SimpleNamespace()))
    assert "source" not in rows_by_id(call("GET"))



def test_overall_status_is_red_below_half_the_goal_yellow_below_it_green_at_it():
    import mirror_quota
    assert mirror_quota.overall_status(2, 10) == "red"
    assert mirror_quota.overall_status(4, 10) == "red"
    assert mirror_quota.overall_status(5, 10) == "yellow"
    assert mirror_quota.overall_status(9, 10) == "yellow"
    assert mirror_quota.overall_status(10, 10) == "green"
    assert mirror_quota.overall_status(12, 10) == "green"
    assert mirror_quota.overall_status(0, 0) == "unknown"
    assert mirror_quota.overall_status(2, 0) == "green"
