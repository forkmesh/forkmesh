#!/usr/bin/env python3
"""The "not publicly served" memo in front of _repo_is_private (adhoc #1642).

Repository API paths for a name this relay has never held are the open web's
most-repeated question: crawlers, stale links and polling clients walk
/api/repo/<owner>/<repo>/... forever, and each probe used to pay a blind index
plus a D1 read before its 404. The CLOSED answer is memoized on a short TTL; a
public verdict is reused only while catalog_rev and this isolate's repositories
writes stand still, a publish must be visible to its own isolate immediately,
and a database failure must not stick.

Same AST-extraction harness as the other entry.py unit tests: pull the two
functions out and stub the D1/crypto/JS primitives they call.
"""

import ast
import asyncio
from pathlib import Path
from types import SimpleNamespace


ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"
FUNCS = {"_repo_is_private", "_forget_repo_closed"}


class _Clock:
    def __init__(self):
        self.ms = 1_750_000_000_000

    def now(self):
        return self.ms


class _Harness:
    def __init__(self, rows=None, fail=False, rev=None):
        self.rows = rows or {}
        self.fail = fail
        self.reads = 0
        self.clock = _Clock()
        self.memo = {}
        self.open_memo = {}
        self.rev = rev
        self.generations = {"repositories": 0}

        async def catalog_rev_current(env, now=None):
            if isinstance(self.rev, Exception):
                raise self.rev
            return self.rev

        async def ensure_schema(env):
            return None

        async def blind_index(env, value):
            return "bi:" + value

        async def d1_first(env, sql, *args):
            self.reads += 1
            if self.fail:
                raise RuntimeError("d1 unavailable")
            return self.rows.get(args[0])

        async def decrypt_row(env, blob):
            return blob

        tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
        selected = [
            node for node in tree.body
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
            and node.name in FUNCS
        ]
        found = {node.name for node in selected}
        assert found == FUNCS, "missing functions: %s" % sorted(FUNCS - found)
        module = ast.fix_missing_locations(
            ast.Module(body=selected, type_ignores=[]))
        self.ns = {
            "Date": SimpleNamespace(now=self.clock.now),
            "ensure_schema": ensure_schema,
            "blind_index": blind_index,
            "d1_first": d1_first,
            "decrypt_row": decrypt_row,
            "_REPO_CLOSED_MEMO": self.memo,
            "REPO_CLOSED_MEMO_TTL_MS": 5000,
            "REPO_CLOSED_MEMO_MAX": 4,
        }
        if rev is not None:
            self.ns.update({
                "_REPO_OPEN_MEMO": self.open_memo,
                "REPO_OPEN_MEMO_TTL_MS": 30000,
                "_catalog_rev_current": catalog_rev_current,
                "_D1_WATCH_GENERATIONS": self.generations,
            })
        exec(compile(module, str(ENTRY), "exec"), self.ns)

    def is_private(self, owner="alice", repo="ghost"):
        return asyncio.run(self.ns["_repo_is_private"](object(), owner, repo))

    def forget(self, owner, repo):
        self.ns["_forget_repo_closed"](owner, repo)


def _public_row(owner, repo):
    return {"bi:%s/%s" % (owner, repo):
            {"is_private": 0, "data": {"visibility": "public"}}}


def test_unknown_repo_is_read_once_per_ttl():
    h = _Harness()
    assert h.is_private() is True
    assert h.reads == 1
    for _ in range(20):
        assert h.is_private() is True
    assert h.reads == 1, "a probe burst must not re-read D1 per request"


def test_memo_expires_with_the_ttl():
    h = _Harness()
    assert h.is_private() is True
    h.clock.ms += 4999
    assert h.is_private() is True
    assert h.reads == 1
    h.clock.ms += 2
    assert h.is_private() is True
    assert h.reads == 2, "the verdict is re-read once the TTL lapses"


def test_public_repositories_are_never_memoized():
    h = _Harness(rows=_public_row("alice", "proj"))
    for expected_reads in (1, 2, 3):
        assert h.is_private("alice", "proj") is False
        assert h.reads == expected_reads


def test_publishing_clears_the_verdict_for_its_own_isolate():
    h = _Harness()
    assert h.is_private("alice", "proj") is True
    assert h.reads == 1
    # The publish lands in the same isolate that just answered 404.
    h.rows.update(_public_row("alice", "proj"))
    h.forget("Alice", "PROJ")  # names arrive in whatever case the caller used
    assert h.is_private("alice", "proj") is False
    assert h.reads == 2


def test_a_database_failure_is_not_memoized():
    h = _Harness(fail=True)
    assert h.is_private() is True
    assert h.is_private() is True
    assert h.reads == 2, "an outage must not pin a repository closed"


def test_the_memo_is_bounded():
    h = _Harness()
    for index in range(6):
        h.is_private("alice", "ghost%d" % index)
    assert len(h.memo) <= 4


def test_a_public_verdict_is_reused_while_the_catalog_stands_still():
    h = _Harness(rows=_public_row("alice", "proj"), rev=7)
    for _ in range(5):
        assert h.is_private("alice", "proj") is False
    assert h.reads == 1
    h.clock.ms += 30001
    assert h.is_private("alice", "proj") is False
    assert h.reads == 2, "the verdict is re-read once the TTL lapses"


def test_a_catalog_change_elsewhere_re_reads_the_public_verdict():
    h = _Harness(rows=_public_row("alice", "proj"), rev=7)
    assert h.is_private("alice", "proj") is False
    h.rows["bi:alice/proj"] = {"is_private": 1, "data": {}}
    h.rev = 8
    assert h.is_private("alice", "proj") is True
    assert h.reads == 2


def test_a_local_repositories_write_re_reads_the_public_verdict():
    h = _Harness(rows=_public_row("alice", "proj"), rev=7)
    assert h.is_private("alice", "proj") is False
    h.rows["bi:alice/proj"] = {"is_private": 1, "data": {}}
    h.generations["repositories"] += 1
    assert h.is_private("alice", "proj") is True
    assert h.reads == 2


def test_forgetting_drops_a_public_verdict_too():
    h = _Harness(rows=_public_row("alice", "proj"), rev=7)
    assert h.is_private("alice", "proj") is False
    h.rows["bi:alice/proj"] = {"is_private": 1, "data": {}}
    h.forget("alice", "proj")
    assert h.is_private("alice", "proj") is True


def test_an_unknown_catalog_rev_never_memoizes_a_public_verdict():
    h = _Harness(rows=_public_row("alice", "proj"), rev=RuntimeError("down"))
    for expected_reads in (1, 2):
        assert h.is_private("alice", "proj") is False
        assert h.reads == expected_reads
