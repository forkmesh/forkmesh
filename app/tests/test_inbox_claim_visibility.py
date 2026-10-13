#!/usr/bin/env python3
"""Inbox drain hazards: precise discussion acks and claim visibility.

Two Worker-side hazards surfaced by the mobile-node work
(www/docs/mobile/mobile-node-contract.md), both about an account that now has
MORE THAN ONE node holding `owner_sign` — a desktop and a phone:

1. The per-repo discussion ack used to be a blanket
   ``DELETE FROM discussion_inbox WHERE repo_bi=?`` behind a call to an
   undefined ``_authorize_repo_inbox_signing_key``. It is now the exact
   leased, id-scoped shape issues and pulls already use, so a submission that
   lands mid-drain survives the acknowledgement of the rows before it.

2. ``/api/sync`` leases every inbox row to the polling key for
   ``ISSUE_INBOX_CLAIM_TTL_MS``. With two nodes on one account the loser used
   to read an empty inbox with nothing to explain it; the response now carries
   ``claimedElsewhere`` counts, and a per-repo ack releases the leftover lease.

Unlike tests/test_sync_endpoint.py — which drives the handlers over a Python
re-implementation of the SQL — everything here runs the real handler code
over an in-memory SQLite copy of the real D1 tables. The claim-visibility
read is a three-block ``UNION ALL`` whose parameters must bind in exactly the
order the blocks appear; a hand-written stub cannot catch a mis-bound
parameter, so it gets a real database.
"""

import ast
import asyncio
import hashlib
import importlib.util
import json
import re
import sqlite3
from pathlib import Path
from urllib.parse import parse_qs, unquote, urlparse

import pytest


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
SCHEMA = ROOT / "src" / "schema.py"
ENTRY_TEXT = (
    # entry.py + its lazily-split domain modules
    (ENTRY.parent / "federation.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "ssh_gateway_routes.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "repo_sync.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "agent_routes.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
)
TREE = ast.parse(ENTRY_TEXT, filename=str(ENTRY))

# The real owner-E2EE control plane, so the sealed/legacy prompt split is
# decided by production code rather than by a stub.
_SPEC = importlib.util.spec_from_file_location(
    "inbox_claim_security_controls", ROOT / "src" / "security_controls.py")
SECURITY_CONTROL = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(SECURITY_CONTROL)

NOW = 1_800_000_000_000
CLAIM_TTL_MS = 5 * 60 * 1000

# Every function below is the real thing, lifted out of entry.py by name.
REAL_FUNCTIONS = (
    "sync_handler",
    "discussions_handler",
    # The drain reads one bounded page at a time; load the real pager so the
    # claim scoping under test is the scoping the handler actually applies.
    "_leased_inbox_page",
    "_claim_collaboration_inbox_on_mirror",
    "_drain_collaboration_inbox_on_mirror",
    "_release_inbox_claim",
    "_drain_ids_from_request",
    "_log_inbox_drain",
    "method_name",
)

# Bounds the pager reads straight from entry.py, so a budget change here
# cannot silently drift away from the runtime it is meant to describe.
REAL_CONSTANTS = ("INBOX_DRAIN_MAX_ROWS", "INBOX_DRAIN_MAX_BYTES")

# Tables the two handlers actually touch.
TABLES = (
    "repositories", "issue_inbox", "pull_inbox", "discussion_inbox",
    "agent_prompts", "about_inbox", "branch_op_inbox", "repo_merge_jobs",
    "inbox_drain_log",
)


def _literal_assignment(path, name):
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(
                isinstance(t, ast.Name) and t.id == name
                for t in node.targets):
            return ast.literal_eval(node.value)
    raise AssertionError(name)


def _load(names, extra_globals, constants=REAL_CONSTANTS):
    selected = [
        node for node in TREE.body
        if (isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
            and node.name in names)
        or (isinstance(node, ast.Assign) and any(
            isinstance(t, ast.Name) and t.id in constants
            for t in node.targets))
    ]
    found = set()
    for node in selected:
        if isinstance(node, ast.Assign):
            found.update(
                t.id for t in node.targets if isinstance(t, ast.Name))
        else:
            found.add(node.name)
    missing = (set(names) | set(constants)) - found
    assert not missing, "missing from entry.py: %s" % sorted(missing)
    module = ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[]))
    namespace = {"json": json, "re": re, **extra_globals}
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


def _run(coro):
    return asyncio.run(coro)


def _bi(value):
    return hashlib.sha256(str(value).encode()).hexdigest()


class _Date:
    now_ms = NOW

    @classmethod
    def now(cls):
        return cls.now_ms


class _OrgAliasUnresolved(Exception):
    pass


class _Request:
    def __init__(self, url, method="GET", signing_key="desktop-key"):
        self.url = url
        self.method = method
        self.signing_key = signing_key


def _json_response(data, status=200, **_kwargs):
    return {"status": status, "data": data}


class _Relay:
    """Real handler code over an in-memory SQLite copy of the D1 tables."""

    def __init__(self, owner="alice", owner_e2ee=False):
        self.owner = owner
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        for statement in _literal_assignment(SCHEMA, "SCHEMA_STATEMENTS"):
            if not isinstance(statement, str):
                continue
            if any(("EXISTS " + table) in statement for table in TABLES):
                # Skip the repo_merge_jobs guard triggers: they reference the
                # bound-check tables, not the drain paths under test.
                if statement.strip().upper().startswith("CREATE TRIGGER"):
                    continue
                self.db.execute(statement)
        _Date.now_ms = NOW
        relay = self

        async def blind_index(_env, value):
            return _bi(value)

        async def d1_all(_env, sql, *args):
            return [dict(r) for r in relay.db.execute(sql, args)]

        async def d1_first(_env, sql, *args):
            row = relay.db.execute(sql, args).fetchone()
            return dict(row) if row else None

        async def d1_run(_env, sql, *args):
            relay.db.execute(sql, args)
            relay.db.commit()

        async def decrypt_row(_env, data):
            return json.loads(data) if data else None

        async def ensure_schema(_env):
            return None

        async def authorized_owner_signing_key(_env, request, _owner):
            return getattr(request, "signing_key", "")

        async def authorized_repo_inbox_signing_key(_env, request, _o, _r):
            return getattr(request, "signing_key", "")

        async def authorized_mirror_issue_signing_key(_env, request, _o, _r):
            key = getattr(request, "mirror_key", "")
            return (key, "mirror-node") if key else ("", "")

        async def inbox_repo_key(_env, _request, owner, repo):
            return _bi(owner + "/" + repo)

        async def deferred_mirror_intake(*_args, **_kwargs):
            return None

        async def record_mirror_attested_state(*_args, **_kwargs):
            return True

        globals_for_handlers = {
            "Date": _Date,
            "json_response": _json_response,
            "ensure_schema": ensure_schema,
            "blind_index": blind_index,
            "d1_all": d1_all,
            "d1_first": d1_first,
            "d1_run": d1_run,
            "decrypt_row": decrypt_row,
            "_authorized_owner_signing_key": authorized_owner_signing_key,
            "_authorized_repo_inbox_signing_key": (
                authorized_repo_inbox_signing_key),
            "_authorized_mirror_issue_signing_key": (
                authorized_mirror_issue_signing_key),
            "_inbox_repo_key": inbox_repo_key,
            "_OrgAliasUnresolved": _OrgAliasUnresolved,
            "_deferred_mirror_intake": deferred_mirror_intake,
            "_record_mirror_attested_state": record_mirror_attested_state,
            "safe_segment": lambda v, *_a: str(v or "").strip().lower(),
            # catalog.py's sanitizer; pass-through is enough here.
            "clean_string": lambda v, limit=240: str(v or "")[:limit],
            "parse_qs": parse_qs,
            "unquote": unquote,
            "urlparse": urlparse,
            "ISSUE_INBOX_CLAIM_TTL_MS": CLAIM_TTL_MS,
            "LOGIN_MAX_SKEW_MS": 5 * 60 * 1000,
            "HTTPS_MIRROR_MERGE_REQUEST_RE": re.compile(
                r"^[A-Za-z0-9_-]{12,80}$"),
        }
        if owner_e2ee:
            globals_for_handlers["security_control"] = SECURITY_CONTROL
        self.ns = _load(REAL_FUNCTIONS, globals_for_handlers)
        self.env = object()

    # -- fixture helpers ---------------------------------------------------

    def add_repo(self, name):
        repo_bi = _bi(self.owner + "/" + name)
        self.db.execute(
            "INSERT INTO repositories (key_bi, owner_bi, data) VALUES (?,?,?)",
            (repo_bi, _bi(self.owner),
             json.dumps({"name": name, "owner": self.owner,
                         "stateHash": "pin", "updatedAt": "t",
                         "source": "local-node"})))
        self.db.commit()
        return repo_bi

    def add_inbox(self, table, repo_bi, body, claimed_by="", expires=0):
        cur = self.db.execute(
            "INSERT INTO %s (repo_bi, data, claimed_by_bi, claim_expires_at) "
            "VALUES (?,?,?,?)" % table,
            (repo_bi, json.dumps(body), claimed_by, expires))
        self.db.commit()
        return cur.lastrowid

    def add_about(self, repo_bi, body):
        self.db.execute(
            "INSERT INTO about_inbox (repo_bi, data, queued_at) "
            "VALUES (?,?,?)", (repo_bi, json.dumps(body), NOW))
        self.db.commit()

    def add_prompt(self, repo_bi, body):
        cur = self.db.execute(
            "INSERT INTO agent_prompts (repo_bi, agent_id, data, queued_at) "
            "VALUES (?,?,?,?)", (repo_bi, "agent-1", json.dumps(body), NOW))
        self.db.commit()
        return cur.lastrowid

    def rows(self, table, repo_bi=None):
        sql = "SELECT * FROM %s" % table
        args = ()
        if repo_bi is not None:
            sql += " WHERE repo_bi=?"
            args = (repo_bi,)
        return [dict(r) for r in
                self.db.execute(sql + " ORDER BY rowid", args)]

    def ids(self, table, repo_bi=None):
        return [r["id"] for r in self.rows(table, repo_bi)]

    # -- request drivers ---------------------------------------------------

    def sync(self, signing_key="desktop-key"):
        request = _Request(
            "https://forkmesh.test/api/sync?owner=%s&ts=%d&sig=s"
            % (self.owner, NOW), "GET", signing_key)
        return _run(self.ns["sync_handler"](self.env, request))

    def discussions(self, repo, method="GET", ids=None,
                    signing_key="desktop-key", mirror_key=""):
        url = ("https://forkmesh.test/api/repo/%s/%s/discussions"
               "?owner=%s&ts=%d&sig=s" % (self.owner, repo, self.owner, NOW))
        if ids is not None:
            url += "&ids=" + ",".join(str(i) for i in ids)
        if mirror_key:
            url += "&mirror=1"
        request = _Request(url, method, signing_key)
        request.mirror_key = mirror_key
        return _run(self.ns["discussions_handler"](
            self.env, request, self.owner, repo))


def _discussion(number, text):
    return {"number": number, "event": {"type": "comment", "body": text},
            "titleIfNew": ""}


# ---------------------------------------------------------------------------
# Hazard 1 — the discussion ack is id-scoped
# ---------------------------------------------------------------------------


def test_discussion_ack_removes_exactly_the_listed_rows():
    relay = _Relay()
    repo_bi = relay.add_repo("forkmesh")
    first = relay.add_inbox("discussion_inbox", repo_bi, _discussion(1, "a"))
    second = relay.add_inbox("discussion_inbox", repo_bi, _discussion(2, "b"))
    third = relay.add_inbox("discussion_inbox", repo_bi, _discussion(3, "c"))

    read = relay.discussions("forkmesh")
    assert read["status"] == 200
    # The GET hands back the row id so the node can name what it merged.
    assert [item["id"] for item in read["data"]["pending"]] == [
        first, second, third]

    acked = relay.discussions("forkmesh", "DELETE", ids=[first, third])
    assert acked["status"] == 200
    assert acked["data"] == {"ok": True, "drained": 2}
    assert relay.ids("discussion_inbox", repo_bi) == [second]


def test_discussion_ack_never_destroys_a_row_that_landed_mid_drain():
    """The exact silent-data-loss shape the blanket DELETE had."""
    relay = _Relay()
    repo_bi = relay.add_repo("forkmesh")
    first = relay.add_inbox("discussion_inbox", repo_bi, _discussion(1, "a"))
    second = relay.add_inbox("discussion_inbox", repo_bi, _discussion(2, "b"))

    read = relay.discussions("forkmesh")
    drained_ids = [item["id"] for item in read["data"]["pending"]]
    assert drained_ids == [first, second]

    # A visitor files a discussion while the node is still merging.
    late = relay.add_inbox("discussion_inbox", repo_bi, _discussion(3, "late"))

    relay.discussions("forkmesh", "DELETE", ids=drained_ids)
    # The late row is still queued: it was never delivered, so it must not be
    # acknowledged. A blanket delete would have destroyed it undelivered.
    assert relay.ids("discussion_inbox", repo_bi) == [late]
    assert json.loads(
        relay.rows("discussion_inbox", repo_bi)[0]["data"]
    )["event"]["body"] == "late"


def test_discussion_ack_without_ids_removes_nothing():
    """The blanket form is gone: an ack that names no rows is a no-op.

    The old handler answered this request by deleting the repo's entire
    discussion queue (behind a NameError, so in practice it 500'd instead).
    It now behaves exactly like the issue and pull acks, which have required
    `?ids=` since adhoc #97.
    """
    relay = _Relay()
    repo_bi = relay.add_repo("forkmesh")
    relay.add_inbox("discussion_inbox", repo_bi, _discussion(1, "a"))
    relay.add_inbox("discussion_inbox", repo_bi, _discussion(2, "b"))
    relay.discussions("forkmesh")

    acked = relay.discussions("forkmesh", "DELETE")
    assert acked["status"] == 200
    assert acked["data"]["drained"] == 0
    assert len(relay.rows("discussion_inbox", repo_bi)) == 2


def test_discussion_ack_is_claim_scoped_to_the_acking_key():
    """A node can only ack rows leased to its own signing key."""
    relay = _Relay()
    repo_bi = relay.add_repo("forkmesh")
    row = relay.add_inbox("discussion_inbox", repo_bi, _discussion(1, "a"))

    # The desktop reads (and therefore leases) the row.
    relay.discussions("forkmesh", signing_key="desktop-key")
    held = relay.rows("discussion_inbox", repo_bi)[0]["claimed_by_bi"]
    assert held == _bi(
        "collaboration-inbox-claim:discussion_inbox:desktop-key")

    # The phone, authorized on the same account, acks the id it never held.
    stolen = relay.discussions(
        "forkmesh", "DELETE", ids=[row], signing_key="phone-key")
    assert stolen["data"]["drained"] == 0
    assert relay.ids("discussion_inbox", repo_bi) == [row]


def test_discussion_ack_rejects_an_unauthorized_caller():
    relay = _Relay()
    repo_bi = relay.add_repo("forkmesh")
    row = relay.add_inbox("discussion_inbox", repo_bi, _discussion(1, "a"))
    denied = relay.discussions(
        "forkmesh", "DELETE", ids=[row], signing_key="")
    assert denied["status"] == 401
    assert denied["data"] == {"error": "unauthorized"}
    assert relay.ids("discussion_inbox", repo_bi) == [row]


def test_discussion_ack_logs_the_drain_like_issues_and_pulls():
    relay = _Relay()
    repo_bi = relay.add_repo("forkmesh")
    row = relay.add_inbox("discussion_inbox", repo_bi, _discussion(1, "a"))
    relay.discussions("forkmesh")
    relay.discussions("forkmesh", "DELETE", ids=[row])
    logged = [dict(r) for r in relay.db.execute(
        "SELECT repo_bi, kind, count FROM inbox_drain_log")]
    assert logged == [{"repo_bi": repo_bi, "kind": "discussions", "count": 1}]


def test_discussion_get_leases_rows_to_the_reading_key_only():
    """Two nodes cannot both be handed the same discussion row."""
    relay = _Relay()
    repo_bi = relay.add_repo("forkmesh")
    relay.add_inbox("discussion_inbox", repo_bi, _discussion(1, "a"))

    desktop = relay.discussions("forkmesh", signing_key="desktop-key")
    assert len(desktop["data"]["pending"]) == 1

    phone = relay.discussions("forkmesh", signing_key="phone-key")
    assert phone["data"]["pending"] == []


def test_discussion_ack_releases_the_leftover_lease():
    """After a node's cycle ends, what it did not ack goes back in the pool."""
    relay = _Relay()
    repo_bi = relay.add_repo("forkmesh")
    first = relay.add_inbox("discussion_inbox", repo_bi, _discussion(1, "a"))
    second = relay.add_inbox("discussion_inbox", repo_bi, _discussion(2, "b"))

    relay.discussions("forkmesh", signing_key="desktop-key")
    # The desktop could only apply the first row.
    relay.discussions(
        "forkmesh", "DELETE", ids=[first], signing_key="desktop-key")

    leftover = relay.rows("discussion_inbox", repo_bi)[0]
    assert leftover["id"] == second
    assert leftover["claimed_by_bi"] == ""
    assert leftover["claim_expires_at"] == 0

    # The phone gets it immediately instead of waiting out the full TTL.
    phone = relay.discussions("forkmesh", signing_key="phone-key")
    assert [i["id"] for i in phone["data"]["pending"]] == [second]


# ---------------------------------------------------------------------------
# Hazard 2 — claim starvation is visible, and never leaks the other key
# ---------------------------------------------------------------------------


def test_sync_reports_zero_claimed_elsewhere_for_a_single_node_account():
    relay = _Relay()
    repo_bi = relay.add_repo("forkmesh")
    relay.add_inbox("issue_inbox", repo_bi, {"title": "one"})

    answer = relay.sync("desktop-key")
    assert answer["status"] == 200
    assert answer["data"]["claimedElsewhere"] == {
        "issues": 0, "pulls": 0, "discussions": 0, "total": 0}
    # Absent per-repo, so a one-node account keeps the body it has today.
    assert "claimedElsewhere" not in answer["data"]["repos"][0]


def test_sync_tells_the_losing_node_rows_are_claimed_elsewhere():
    """A desktop and a phone on one account: the loser learns why it
    reads empty."""
    relay = _Relay()
    repo_bi = relay.add_repo("forkmesh")
    relay.add_inbox("issue_inbox", repo_bi, {"title": "one"})
    relay.add_inbox("issue_inbox", repo_bi, {"title": "two"})
    relay.add_inbox("pull_inbox", repo_bi, {"number": 4})
    relay.add_inbox("discussion_inbox", repo_bi, _discussion(1, "a"))

    first = relay.sync("desktop-key")
    assert len(first["data"]["repos"][0]["issues"]) == 2
    assert first["data"]["claimedElsewhere"]["total"] == 0

    second = relay.sync("phone-key")
    # Nothing readable...
    assert second["data"]["repos"][0]["issues"] == []
    assert second["data"]["repos"][0]["pulls"] == []
    assert second["data"]["repos"][0]["discussions"] == []
    # ...but the reason is now on the wire, per kind and account-wide.
    assert second["data"]["claimedElsewhere"] == {
        "issues": 2, "pulls": 1, "discussions": 1, "total": 4}
    assert second["data"]["repos"][0]["claimedElsewhere"] == {
        "issues": 2, "pulls": 1, "discussions": 1}


def test_claimed_elsewhere_never_leaks_the_holding_key():
    relay = _Relay()
    repo_bi = relay.add_repo("forkmesh")
    relay.add_inbox("issue_inbox", repo_bi, {"title": "one"})
    relay.sync("desktop-key")

    body = json.dumps(relay.sync("phone-key")["data"])
    holder = _bi("issue-inbox-claim:desktop-key")
    assert holder not in body
    assert "desktop-key" not in body
    # Only integers describe the other node's hold.
    assert all(isinstance(v, int) for v in
               json.loads(body)["claimedElsewhere"].values())


def test_claimed_elsewhere_counts_each_kind_against_its_own_claimant():
    """Each block of the UNION must bind its OWN claimant index.

    A mis-bound parameter here (three blocks, three claimant indexes, three
    repo lists) would make a node's own leases look like someone else's. The
    real database is what makes this assertion meaningful.
    """
    relay = _Relay()
    repo_bi = relay.add_repo("forkmesh")
    relay.add_inbox("issue_inbox", repo_bi, {"title": "one"})
    relay.add_inbox("pull_inbox", repo_bi, {"number": 4})
    relay.add_inbox("discussion_inbox", repo_bi, _discussion(1, "a"))

    answer = relay.sync("desktop-key")
    # The caller holds all three leases, under three DIFFERENT claimant
    # indexes; none of them may be reported as held elsewhere.
    assert answer["data"]["claimedElsewhere"]["total"] == 0
    held = {
        table: relay.rows(table, repo_bi)[0]["claimed_by_bi"]
        for table in ("issue_inbox", "pull_inbox", "discussion_inbox")}
    assert held["issue_inbox"] == _bi("issue-inbox-claim:desktop-key")
    assert held["pull_inbox"] == _bi(
        "collaboration-inbox-claim:pull_inbox:desktop-key")
    assert held["discussion_inbox"] == _bi(
        "collaboration-inbox-claim:discussion_inbox:desktop-key")
    assert len(set(held.values())) == 3


def test_an_expired_claim_is_not_reported_as_claimed_elsewhere():
    relay = _Relay()
    repo_bi = relay.add_repo("forkmesh")
    relay.add_inbox("issue_inbox", repo_bi, {"title": "one"})

    relay.sync("desktop-key")
    # Five minutes and a second later the lease has lapsed.
    _Date.now_ms = NOW + CLAIM_TTL_MS + 1000
    answer = relay.sync("phone-key")
    assert answer["data"]["claimedElsewhere"]["total"] == 0
    assert len(answer["data"]["repos"][0]["issues"]) == 1
    _Date.now_ms = NOW


def test_claimed_elsewhere_is_scoped_to_the_callers_own_repos():
    relay = _Relay()
    mine = relay.add_repo("forkmesh")
    relay.add_inbox("issue_inbox", mine, {"title": "mine"})
    # Another account's repo, leased by another key entirely.
    theirs = _bi("bob/other")
    relay.add_inbox("issue_inbox", theirs, {"title": "theirs"},
                    claimed_by=_bi("issue-inbox-claim:bob-key"),
                    expires=NOW + CLAIM_TTL_MS)

    relay.sync("desktop-key")
    answer = relay.sync("phone-key")
    # Exactly one row is claimed elsewhere: the one in the caller's own repo.
    assert answer["data"]["claimedElsewhere"]["issues"] == 1


def test_a_released_claim_clears_the_claimed_elsewhere_signal():
    """The starvation window closes on ack, not only on TTL expiry."""
    relay = _Relay()
    repo_bi = relay.add_repo("forkmesh")
    row = relay.add_inbox("discussion_inbox", repo_bi, _discussion(1, "a"))
    extra = relay.add_inbox("discussion_inbox", repo_bi, _discussion(2, "b"))

    relay.sync("desktop-key")
    assert relay.sync("phone-key")["data"][
        "claimedElsewhere"]["discussions"] == 2

    # The desktop finishes its cycle, acking one row and releasing the rest.
    relay.discussions("forkmesh", "DELETE", ids=[row],
                      signing_key="desktop-key")
    assert relay.ids("discussion_inbox", repo_bi) == [extra]

    phone = relay.sync("phone-key")
    assert phone["data"]["claimedElsewhere"]["discussions"] == 0
    assert [d["id"] for d in phone["data"]["repos"][0]["discussions"]] == [
        extra]


# ---------------------------------------------------------------------------
# Read-destructive kinds: About edits and legacy agent prompts
# ---------------------------------------------------------------------------


def test_sync_names_the_read_destructive_kinds_it_deletes():
    relay = _Relay(owner_e2ee=True)
    relay.add_repo("forkmesh")
    answer = relay.sync("desktop-key")
    # With owner E2EE on (production), prompts wait for an explicit ack; only
    # About edits and web branch operations vanish on read.
    assert answer["data"]["readDestructiveKinds"] == [
        "aboutUpdate", "branchOps"]


def test_sync_names_legacy_prompts_as_read_destructive_without_e2ee():
    relay = _Relay(owner_e2ee=False)
    relay.add_repo("forkmesh")
    answer = relay.sync("desktop-key")
    assert answer["data"]["readDestructiveKinds"] == [
        "aboutUpdate", "branchOps", "agentPrompts"]


def test_about_update_is_deleted_on_read_and_never_redelivered():
    relay = _Relay()
    repo_bi = relay.add_repo("forkmesh")
    relay.add_about(repo_bi, {"about": "a new description"})

    first = relay.sync("desktop-key")
    assert first["data"]["repos"][0]["aboutUpdate"] == {
        "about": "a new description"}
    assert relay.rows("about_inbox", repo_bi) == []

    # Deliberate: the caller gets exactly one delivery. A client that cannot
    # apply it must persist it, which is why the kind is advertised above.
    second = relay.sync("desktop-key")
    assert "aboutUpdate" not in second["data"]["repos"][0]


def test_about_read_deletes_only_the_row_it_actually_read():
    """Drain-on-read still targets rows by id, never the whole repo key."""
    relay = _Relay()
    first_repo = relay.add_repo("forkmesh")
    second_repo = relay.add_repo("desktop")
    relay.add_about(first_repo, {"about": "one"})
    relay.add_about(second_repo, {"about": "two"})

    answer = relay.sync("desktop-key")
    delivered = {r["name"]: r.get("aboutUpdate") for r in
                 answer["data"]["repos"]}
    assert delivered == {"forkmesh": {"about": "one"},
                         "desktop": {"about": "two"}}
    assert relay.rows("about_inbox") == []


def test_legacy_agent_prompts_drain_on_read_by_exact_id():
    relay = _Relay(owner_e2ee=False)
    repo_bi = relay.add_repo("forkmesh")
    first = relay.add_prompt(repo_bi, {"prompt": "one"})

    answer = relay.sync("desktop-key")
    assert [p["prompt"] for p in
            answer["data"]["repos"][0]["agentPrompts"]] == ["one"]
    assert relay.ids("agent_prompts", repo_bi) == []

    # A prompt queued after the read survives to the next sync.
    relay.add_prompt(repo_bi, {"prompt": "two"})
    again = relay.sync("desktop-key")
    assert [p["prompt"] for p in
            again["data"]["repos"][0]["agentPrompts"]] == ["two"]
    assert first not in relay.ids("agent_prompts", repo_bi)


def test_owner_sealed_prompts_are_retained_until_an_explicit_ack():
    """Under E2EE a prompt is NOT read-destructive: it waits for the ack."""
    relay = _Relay(owner_e2ee=True)
    repo_bi = relay.add_repo("forkmesh")
    queued = relay.add_prompt(repo_bi, {"prompt": "sealed"})

    relay.sync("desktop-key")
    # Undecodable by the relay, so it is neither delivered nor destroyed.
    assert relay.ids("agent_prompts", repo_bi) == [queued]


# ---------------------------------------------------------------------------
# Contract guards
# ---------------------------------------------------------------------------


def test_no_blanket_discussion_delete_survives_in_a_drain_path():
    """The hazard itself, asserted against the source.

    The only remaining unfiltered `DELETE FROM discussion_inbox WHERE
    repo_bi=?` is repository deletion, where dropping the queue is the point.
    """
    blanket = [
        m.start() for m in re.finditer(
            r'"DELETE FROM discussion_inbox WHERE repo_bi=\?"', ENTRY_TEXT)]
    assert len(blanket) == 1
    context = ENTRY_TEXT[blanket[0] - 2000:blanket[0]]
    assert "discussions_handler" not in context


@pytest.mark.parametrize("table", ["issue_inbox", "pull_inbox",
                                   "discussion_inbox"])
def test_every_inbox_carries_the_claim_columns(table):
    relay = _Relay()
    columns = {row["name"] for row in
               relay.db.execute("PRAGMA table_info(%s)" % table)}
    assert {"claimed_by_bi", "claim_expires_at"} <= columns
