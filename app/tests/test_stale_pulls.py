import ast
import asyncio
import hashlib
import json
import re
import sqlite3
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import parse_qs, urlparse

import pytest

import schema
import stale_pulls as sp

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
HOUR = 60 * 60 * 1000
DAY = 24 * HOUR
T0 = 1_800_000_000_000
LEDGER = "a" * 40
HEAD = "b" * 40
HEAD2 = "c" * 40
ENV = SimpleNamespace()


def run(coro):
    return asyncio.run(coro)


def pull_md(number=7, status="open", author="bob-key", name="bob",
            head="bob/fix", derive="branch", draft=""):
    lines = ["---", "number: %d" % number, "title: Fix", "base: main",
             "head: %s" % head, "status: %s" % status, "author: %s" % author,
             "authorName: %s" % name]
    if derive == "branch":
        lines.append("derive: branch")
    else:
        lines.append("headOid: %s" % HEAD)
    if draft:
        lines.append("draft: %s" % draft)
    return "\n".join(lines + ["---", "", "Body", ""])


def event_md(kind, author, name, ts, state="", ident=""):
    lines = ["---", "type: %s" % kind, "id: %s" % (ident or "%s-%s-%s" % (
        kind, name or author, ts)), "author: %s" % author,
        "authorName: %s" % name, "ts: %s" % ts]
    if state:
        lines.append("state: %s" % state)
    return "\n".join(lines + ["sig: s", "---", "", "text", ""])


class FakeD1:
    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        for statement in schema.SCHEMA_STATEMENTS:
            if "stale_pull" in statement:
                self.db.execute(statement)
        self.db.execute(
            "CREATE TABLE pull_inbox (id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "repo_bi TEXT NOT NULL, data TEXT NOT NULL, submitter_bi TEXT)")
        self.db.execute(
            "CREATE TABLE branch_op_inbox (id INTEGER PRIMARY KEY "
            "AUTOINCREMENT, repo_bi TEXT NOT NULL, data TEXT NOT NULL, "
            "queued_at INTEGER NOT NULL)")

    async def all(self, env, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]

    async def first(self, env, sql, *args):
        rows = await self.all(env, sql, *args)
        return rows[0] if rows else None

    async def run(self, env, sql, *args):
        cursor = self.db.execute(sql, args)
        return SimpleNamespace(meta=SimpleNamespace(changes=cursor.rowcount))

    def inbox(self):
        return [json.loads(row[0]) for row in self.db.execute(
            "SELECT data FROM pull_inbox ORDER BY id")]

    def branch_ops(self):
        return [json.loads(row[0]) for row in self.db.execute(
            "SELECT data FROM branch_op_inbox ORDER BY id")]

    def count(self, table):
        return self.db.execute("SELECT COUNT(*) FROM " + table).fetchone()[0]


class Response:
    def __init__(self, status, body):
        self.status = status
        self._body = body

    async def text(self):
        return json.dumps(self._body)


@pytest.fixture
def rt(monkeypatch):
    d1 = FakeD1()
    world = SimpleNamespace(
        now=T0, records={7: pull_md()}, events={7: []},
        tips={"bob/fix": HEAD, "main": "d" * 40},
        features={"stalePulls": True, "stalePullsDeleteBranch": False},
        pings=[], accounts={"bob-key": "bob", "alice-key": "alice"},
        writers={"carol"}, reads=[], default="main", protected=[],
        catalog={"source": "node"}, budget=False)

    async def blind_index(env, text):
        return hashlib.sha256(str(text).encode()).hexdigest()

    async def encrypt_row(env, obj):
        return json.dumps(obj)

    async def decrypt_row(env, stored):
        return json.loads(stored)

    def blob(content):
        return {"ok": True, "content": content} if content is not None else None

    async def routed(env, url, operation, bypass_cache=False):
        query = parse_qs(urlparse(url).query)
        world.reads.append((operation, query))
        ref = query.get("ref", [""])[0]
        path = query.get("path", [""])[0]
        if operation == "tree" and path == ".forkmesh/pulls":
            return Response(200, {"commit": LEDGER, "entries": [
                {"name": str(n), "type": "tree"} for n in world.records]})
        if operation == "tree" and path.startswith(".forkmesh/pulls/"):
            number = int(path.rsplit("/", 1)[1])
            return Response(200, {"entries": [
                {"name": "%04d-%s.md" % (i + 2, kind), "type": "blob"}
                for i, (kind, _text) in enumerate(world.events.get(number, []))]})
        if operation == "tree":
            return Response(200, {"commit": world.tips.get(ref, "")})
        if operation == "blobs":
            found = {}
            for wanted in query.get("path", []):
                match = re.match(r"\.forkmesh/pulls/(\d+)/(.+)$", wanted)
                number, name = int(match.group(1)), match.group(2)
                if name == "pull.md":
                    found[wanted] = blob(world.records.get(number))
                    continue
                index = int(name.split("-", 1)[0]) - 2
                found[wanted] = blob(world.events[number][index][1])
            return Response(200, {"blobs": found})
        return Response(404, {})

    async def enqueue(env, recipient, kind, title, body="", repo="", href="",
                      actor="", source="", dedupe="", meta=None, **_):
        world.pings.append({"recipient": recipient, "kind": kind,
                            "title": title, "href": href, "dedupe": dedupe,
                            "meta": meta})
        return True

    async def account_for_key(env, key):
        return world.accounts.get(key, "")

    async def owns(env, account, node):
        return account == node

    async def writer(env, node, repo, account):
        return account in world.writers

    async def features(env, owner, repo):
        return dict(world.features)

    async def nothing(*args, **kwargs):
        return None

    async def no(*args, **kwargs):
        return False

    async def node(env, owner, repo):
        return ""

    async def catalog(env, node, repo):
        return world.catalog

    async def default_branch(env, node, repo):
        return world.default

    async def push_protection(env, owner, repo):
        return list(world.protected), None

    protection = SimpleNamespace(
        default_branch=default_branch, push_protection=push_protection,
        branch_is_protected=lambda branch, refs: (
            "refs/heads/" + branch) in refs)
    runtime = {
        "d1_all": d1.all, "d1_first": d1.first, "d1_run": d1.run,
        "blind_index": blind_index, "encrypt_row": encrypt_row,
        "decrypt_row": decrypt_row, "_routed_repository_read": routed,
        "enqueue_notification": enqueue,
        "_git_push_account_for_pubkey": account_for_key,
        "_account_owns_node": owns, "_org_write_allowed": writer,
        "_org_repo_node": node, "_repo_features_get": features,
        "_repo_catalog_record": catalog, "_branch_protection": protection,
        "_inbox_author_over_quota": no, "notify_repo_host": nothing,
        "notify_repo_mirrors": nothing, "ensure_schema": nothing,
        "valid_node_name": lambda name: bool(re.fullmatch(
            r"[a-z][a-z0-9-]{0,62}", str(name or ""))),
        "valid_node_pubkey": lambda key: str(key or "").endswith("-key"),
        "repo_web_href": lambda owner, repo: "/%s/%s" % (owner, repo),
        "NOTIFICATION_ITEM_URL_SEGMENTS": {"pull": "pull"},
        "FORKBOT_AUTHOR": "forkbot", "MAX_PENDING_PULLS": 200,
        "_cron_budget_exhausted": lambda: world.budget,
        "Date": SimpleNamespace(now=lambda: world.now),
    }
    for name, value in runtime.items():
        monkeypatch.setattr(sp, name, value, raising=False)
    sp._prune_memo["at"] = 0
    return SimpleNamespace(d1=d1, world=world)


def enable(rt, at=T0):
    rt.world.now = at
    assert run(sp.enroll(ENV, "alice", "tools", rt.world.features,
                         now=at)) is True


def cron(rt, at):
    rt.world.now = at
    rt.d1.db.execute("UPDATE stale_pull_repos SET next_at=0")
    return run(sp.cron(ENV, now=at))


def say(rt, kind, who, at, state="", number=7, seconds=False):
    key = {"alice": "alice-key", "bob": "bob-key"}.get(who, who + "-key")
    ts = at // 1000 if seconds else at
    rt.world.events.setdefault(number, []).append(
        (kind, event_md(kind, key, who, ts, state)))


def posted(rt):
    return [(item["number"], item["event"]["type"], item["event"]["id"],
             item["event"]["author"]) for item in rt.d1.inbox()]


def comments(rt):
    return [item["event"]["body"] for item in rt.d1.inbox()
            if item["event"]["type"] == "comment"]


def test_schedule_is_24h_3d_7d_then_close_a_day_after_the_final_warning():
    ns = sp.next_stage
    assert ns(0, 0, 0, DAY - 1) == 0
    assert ns(0, 0, 0, DAY) == 1
    assert ns(0, 1, DAY, 3 * DAY - 1) == 0
    assert ns(0, 1, DAY, 3 * DAY) == 2
    assert ns(0, 2, 3 * DAY, 7 * DAY) == 3
    assert ns(0, 3, 7 * DAY, 8 * DAY) == 4
    assert ns(0, 4, 8 * DAY, 30 * DAY) == 0
    assert ns(0, 0, 0, 5 * DAY) == 2
    assert ns(0, 0, 0, 20 * DAY) == 3
    assert ns(0, 3, 9 * DAY, 9 * DAY + DAY - 1) == 0
    assert ns(0, 3, 9 * DAY, 10 * DAY) == 4
    assert sp.STAGE_DELAYS == (DAY, 3 * DAY, 7 * DAY, 8 * DAY)


def test_waiting_anchor_follows_maintainers_authors_and_pushes():
    def e(role, kind, ts, state=""):
        return {"role": role, "type": kind, "ts": ts, "state": state,
                "id": "%s-%d" % (kind, ts)}

    assert sp.waiting_anchor([e("maintainer", "comment", 1)])["ts"] == 1
    assert sp.waiting_anchor([e("maintainer", "review", 1,
                                "changes_requested")])["ts"] == 1
    assert sp.waiting_anchor([e("maintainer", "review", 1, "approved")]) \
        is None
    assert sp.waiting_anchor([e("maintainer", "comment", 1),
                              e("author", "comment", 2)]) is None
    assert sp.waiting_anchor([e("maintainer", "comment", 1),
                              e("other", "synchronize", 2)]) is None
    assert sp.waiting_anchor([e("maintainer", "comment", 1),
                              e("author", "review", 2, "commented")]) is None
    assert sp.waiting_anchor([e("maintainer", "comment", 1),
                              e("author", "comment", 2),
                              e("maintainer", "comment", 3)])["ts"] == 3
    assert sp.waiting_anchor([e("maintainer", "comment", 1),
                              e("other", "comment", 2),
                              e("bot", "comment", 3)])["ts"] == 1
    assert sp.waiting_anchor([e("other", "comment", 1)]) is None
    assert sp.waiting_anchor([e("author", "comment", 5),
                              e("maintainer", "comment", 1)]) is None


def test_reminders_at_one_three_seven_days_then_close_once_each(rt):
    enable(rt)
    start = T0 + HOUR
    say(rt, "review", "carol", start, "changes_requested")
    cron(rt, start + DAY - HOUR)
    assert posted(rt) == [] and rt.world.pings == []
    cron(rt, start + DAY)
    assert [p[1] for p in posted(rt)] == ["comment"]
    assert rt.world.pings[0]["recipient"] == "bob"
    assert rt.world.pings[0]["kind"] == "pull_attention"
    assert rt.world.pings[0]["href"] == "/alice/tools/pull/7"
    assert "waiting on your reply" in rt.world.pings[0]["title"]
    assert "@bob, carol left feedback" in comments(rt)[0]
    for later in (start + DAY + HOUR, start + 2 * DAY):
        cron(rt, later)
    assert len(posted(rt)) == 1
    cron(rt, start + 3 * DAY)
    assert len(posted(rt)) == 2 and "3 days" in comments(rt)[1]
    cron(rt, start + 7 * DAY)
    assert len(posted(rt)) == 3
    assert "closes in 1 day" in comments(rt)[2]
    assert rt.world.pings[-1]["title"] == "Pull request #7 closes in 1 day"
    cron(rt, start + 8 * DAY - HOUR)
    assert len(posted(rt)) == 3
    cron(rt, start + 8 * DAY)
    events = posted(rt)
    assert [e[1] for e in events] == [
        "comment", "comment", "comment", "comment", "status"]
    close = rt.d1.inbox()[-1]["event"]
    assert close["state"] == "closed" and close["author"] == "forkbot"
    assert "open a new pull request" in comments(rt)[3]
    assert all(e[3] == "forkbot" for e in events)
    assert len({e[2] for e in events}) == 5
    assert rt.world.pings[-1]["meta"]["stage"] == 4
    for later in (start + 8 * DAY + HOUR, start + 20 * DAY):
        cron(rt, later)
    assert len(posted(rt)) == 5 and len(rt.world.pings) == 4
    assert rt.d1.branch_ops() == []


def test_restart_and_racing_runs_never_post_twice(rt):
    enable(rt)
    say(rt, "comment", "alice", T0 + HOUR)
    cron(rt, T0 + HOUR + DAY)
    sp._prune_memo["at"] = 0
    cron(rt, T0 + HOUR + DAY)
    assert len(posted(rt)) == 1
    row = rt.d1.db.execute(
        "SELECT version, stage FROM stale_pulls").fetchone()
    assert row["stage"] == 1
    stale = {"anchor_at": T0 + HOUR, "stage": 0, "stage_at": 0,
             "version": row["version"] - 1}
    assert run(sp._swap(ENV, rt.d1.db.execute(
        "SELECT repo_bi FROM stale_pulls").fetchone()[0], 7,
        stale["version"], stale["anchor_at"], 1, T0, {}, T0)) is False


def test_author_reply_resets_and_a_new_maintainer_comment_restarts(rt):
    enable(rt)
    start = T0 + HOUR
    say(rt, "comment", "carol", start)
    cron(rt, start + DAY)
    assert len(posted(rt)) == 1
    say(rt, "comment", "bob", start + DAY + HOUR, seconds=True)
    cron(rt, start + 3 * DAY)
    cron(rt, start + 9 * DAY)
    assert len(posted(rt)) == 1
    assert rt.d1.count("stale_pulls") == 0
    again = start + 10 * DAY
    say(rt, "comment", "alice", again)
    cron(rt, again + DAY - HOUR)
    assert len(posted(rt)) == 1
    cron(rt, again + DAY)
    assert len(posted(rt)) == 2
    assert "alice left feedback" in comments(rt)[1]
    assert posted(rt)[0][2] != posted(rt)[1][2]


def test_a_push_resets_the_clock_by_event_or_by_head(rt):
    enable(rt)
    start = T0 + HOUR
    say(rt, "comment", "carol", start)
    cron(rt, start + DAY)
    say(rt, "synchronize", "bob", start + DAY + HOUR)
    cron(rt, start + 3 * DAY)
    assert len(posted(rt)) == 1

    rt.world.events[7] = []
    say(rt, "comment", "carol", start + 4 * DAY)
    cron(rt, start + 5 * DAY)
    assert len(posted(rt)) == 2
    rt.world.tips["bob/fix"] = HEAD2
    cron(rt, start + 7 * DAY)
    cron(rt, start + 13 * DAY)
    assert len(posted(rt)) == 2


def test_drafts_closed_and_maintainer_pulls_are_skipped(rt):
    rt.world.records = {
        7: pull_md(7, draft="true"), 8: pull_md(8, status="closed"),
        9: pull_md(9, status="merged"),
        10: pull_md(10, author="alice-key", name="alice"),
        11: pull_md(11, author="carol-key", name="carol"),
        12: pull_md(12, status="draft")}
    enable(rt)
    for number in rt.world.records:
        say(rt, "comment", "alice", T0 + HOUR, number=number)
    for day in range(1, 10):
        cron(rt, T0 + HOUR + day * DAY)
    assert posted(rt) == [] and rt.world.pings == []


def test_comments_by_others_and_bots_start_nothing(rt):
    enable(rt)
    say(rt, "comment", "dave", T0 + HOUR)
    say(rt, "comment", "forkbot", T0 + HOUR)
    cron(rt, T0 + 3 * DAY)
    assert posted(rt) == []


def test_off_by_default_and_turning_it_off_stops_everything(rt):
    tree = ast.parse((ROOT / "src" / "entry.py").read_text("utf-8"))
    defaults = next(
        ast.literal_eval(node.value) for node in tree.body
        if isinstance(node, ast.Assign)
        and getattr(node.targets[0], "id", "") == "REPO_FEATURE_DEFAULTS")
    assert defaults["stalePulls"] is False
    assert defaults["stalePullsDeleteBranch"] is False
    assert run(sp.enroll(ENV, "alice", "tools", {}, now=T0)) is False
    assert rt.d1.count("stale_pull_repos") == 0
    enable(rt)
    say(rt, "comment", "carol", T0 + HOUR)
    rt.world.features["stalePulls"] = False
    cron(rt, T0 + 2 * DAY)
    assert posted(rt) == [] and rt.d1.count("stale_pull_repos") == 0


def test_enabling_never_counts_feedback_older_than_the_switch(rt):
    say(rt, "comment", "carol", T0 - 30 * DAY)
    enable(rt)
    cron(rt, T0 + DAY - HOUR)
    assert posted(rt) == []
    cron(rt, T0 + DAY)
    assert len(posted(rt)) == 1 and "closes on" in comments(rt)[0]


def test_close_after_an_outage_still_gives_a_days_notice(rt):
    enable(rt)
    say(rt, "comment", "carol", T0 + HOUR)
    cron(rt, T0 + 20 * DAY)
    assert len(posted(rt)) == 1 and "closes in 1 day" in comments(rt)[0]
    cron(rt, T0 + 20 * DAY + 12 * HOUR)
    assert len(posted(rt)) == 1
    cron(rt, T0 + 21 * DAY)
    assert [p[1] for p in posted(rt)] == ["comment", "comment", "status"]


def test_failed_delivery_is_retried_not_lost(rt, monkeypatch):
    enable(rt)
    say(rt, "comment", "carol", T0 + HOUR)

    async def full(*args):
        return True

    monkeypatch.setattr(sp, "_inbox_author_over_quota", full)
    cron(rt, T0 + HOUR + DAY)
    assert posted(rt) == []
    assert rt.d1.db.execute("SELECT stage FROM stale_pulls").fetchone()[0] == 0

    async def room(*args):
        return False

    monkeypatch.setattr(sp, "_inbox_author_over_quota", room)
    cron(rt, T0 + 2 * HOUR + DAY)
    assert len(posted(rt)) == 1


def test_head_branch_is_deleted_only_when_enabled_and_safe(rt):
    rt.world.features["stalePullsDeleteBranch"] = True
    enable(rt)
    say(rt, "comment", "carol", T0 + HOUR)
    for day in (1, 3, 7, 8):
        cron(rt, T0 + HOUR + day * DAY)
    assert rt.d1.branch_ops() == [{
        "op": "delete", "branch": "bob/fix", "expectedOid": HEAD,
        "requestedBy": "forkbot", "queuedAt": T0 + HOUR + 8 * DAY,
        "source": "stale-pull"}]
    assert "`bob/fix` is being deleted" in comments(rt)[-1]


@pytest.mark.parametrize("change", [
    {"features": {"stalePulls": True, "stalePullsDeleteBranch": False}},
    {"record": pull_md(head="bob:fix")},
    {"record": pull_md(derive="patch")},
    {"record": pull_md(head="forkmesh/pull/7")},
    {"default": "bob/fix"},
    {"protected": ["refs/heads/bob/fix"]},
    {"catalog": {"source": "hosted-web"}},
])
def test_head_branch_is_kept_in_every_unsafe_case(rt, change):
    rt.world.features["stalePullsDeleteBranch"] = True
    rt.world.features.update(change.get("features", {}))
    if "record" in change:
        rt.world.records[7] = change["record"]
    for key in ("default", "protected", "catalog"):
        if key in change:
            setattr(rt.world, key, change[key])
    enable(rt)
    say(rt, "comment", "carol", T0 + HOUR)
    for day in (1, 3, 7, 8):
        cron(rt, T0 + HOUR + day * DAY)
    assert posted(rt)[-1][1] == "status"
    assert rt.d1.branch_ops() == []
    assert "being deleted" not in comments(rt)[-1]


def test_a_sweep_is_bounded_and_resumes_where_it_stopped(rt):
    rt.world.records = {n: pull_md(n) for n in range(1, 11)}
    enable(rt)
    for number in rt.world.records:
        say(rt, "comment", "carol", T0 + HOUR, number=number)
    rt.world.now = T0 + HOUR + DAY
    run(sp.cron(ENV, now=rt.world.now))
    assert len(posted(rt)) == sp.CHECKS_PER_RUN
    for _ in range(5):
        run(sp.cron(ENV, now=rt.world.now))
    assert sorted(p[0] for p in posted(rt)) == list(range(1, 11))
    row = rt.d1.db.execute("SELECT next_at FROM stale_pull_repos").fetchone()
    assert row[0] == rt.world.now + sp.SWEEP_MS


def test_closed_pulls_drop_their_rows_in_one_statement(rt, monkeypatch):
    rt.world.records = {n: pull_md(n) for n in (7, 8, 9)}
    enable(rt)
    for number in rt.world.records:
        say(rt, "comment", "alice", T0 + HOUR, number=number)
    cron(rt, T0 + HOUR + DAY)
    assert rt.d1.db.execute("SELECT COUNT(*) FROM stale_pulls").fetchone()[0] == 3
    rt.world.records = {n: pull_md(n, status="closed") for n in (7, 8, 9)}
    sent = []
    real = rt.d1.run

    async def spy(env, sql, *args):
        sent.append(sql)
        return await real(env, sql, *args)

    monkeypatch.setattr(sp, "d1_run", spy)
    cron(rt, T0 + HOUR + DAY + sp.SWEEP_MS)
    assert rt.d1.db.execute("SELECT COUNT(*) FROM stale_pulls").fetchone()[0] == 0
    assert len([q for q in sent if q.startswith("DELETE FROM stale_pulls")
                and "updated_at" not in q]) == 1


def test_entry_wires_the_setting_the_cron_phase_and_the_purge():
    entry = (ROOT / "src" / "entry.py").read_text("utf-8")
    assert '_stale_pulls = _LazyModule("stale_pulls")' in entry
    assert 'phases.append("stale-pulls")' in entry
    assert 'if phase == "stale-pulls":\n        return await stale_pulls_cron(env)' in entry
    assert "stale_pulls_enroll(" in entry
    scoped = entry[entry.index("_REPO_SCOPED_TABLES = ("):]
    scoped = scoped[:scoped.index(")\n\n")]
    assert '("stale_pull_repos", "repo_bi")' in scoped
    assert '("stale_pulls", "repo_bi")' in scoped
    activity = (ROOT / "src" / "cron_activity.py").read_text("utf-8")
    assert '("stale-pulls", 5, (1,), "notifications",' in activity
    footprint = (REPO / "world" / "tools" / "build_worker_footprint.py").read_text("utf-8")
    assert '"stale_pulls.py",' in footprint
    rekey = (REPO / "tools" / "next_rekey.py").read_text("utf-8")
    assert '"stale_pulls": ("repo_bi",),' in rekey


def test_migration_creates_and_drops_both_tables():
    up = (ROOT / "migrations" / "0298_stale_pulls.sql").read_text("utf-8")
    down = (ROOT / "migrations" / "down" / "0298_stale_pulls.sql").read_text(
        "utf-8")
    db = sqlite3.connect(":memory:")
    db.execute("CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT NOT NULL)")
    db.executescript(up)
    tables = {row[0] for row in db.execute(
        "SELECT name FROM sqlite_master WHERE type='table'")}
    assert {"stale_pull_repos", "stale_pulls"} <= tables
    db.executescript(down)
    tables = {row[0] for row in db.execute(
        "SELECT name FROM sqlite_master WHERE type='table'")}
    assert not {"stale_pull_repos", "stale_pulls"} & tables
    assert db.execute("SELECT COUNT(*) FROM schema_meta").fetchone()[0] == 0


def test_settings_page_offers_both_switches_unchecked():
    for path in (ROOT / "public" / "dashboard" / "js" / "08-repo-detail-network.js",
                 ROOT / "public" / "dashboard.js"):
        text = path.read_text("utf-8")
        for key in ("stalePulls", "stalePullsDeleteBranch"):
            box = re.search(r'<input data-repo-feature="%s"[^>]*>' % key, text)
            assert box and "checked" not in box.group(0)
    content = (ROOT / "public" / "dashboard" / "js" / "06-repo-content.js").read_text("utf-8")
    assert '"protectDefaultBranch", "stalePulls", "stalePullsDeleteBranch"]' in content
