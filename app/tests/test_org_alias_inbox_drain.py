#!/usr/bin/env python3
"""Organization-aliased repos must drain their inbox queues.

A repo fronted by an organization is submitted to as /<org>/<repo> but every
drain reads the BACKING NODE's blind index. Two independent defects broke that
end to end, so a source-of-truth node could sit online, sync 200 OK, and never
receive a single web-filed issue or pull request:

  * relay: org_alias_rewrite resolves the alias through _org_repo_node, which
    fails CLOSED. A D1 hiccup returned "" -> the request stayed unrewritten ->
    the insert was keyed on the ORGANIZATION, a queue no drain ever reads.
  * desktop: performRelaySync matched /api/sync entries by RepositoryRecord
    owner (the public alias) against the account owner the relay names, so the
    whole slice - issues, pulls, discussions, prompts, About edits - was
    silently discarded and therefore never acked either.
"""

import ast
import asyncio
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
ENTRY_TEXT = (
    # entry.py + its lazily-split domain modules
    (ENTRY.parent / "forkbot.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "fediverse_routes.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
)
QT_PULLS = ROOT.parent / "desktop" / "src" / "MainWindowPulls.cpp"

FUNCS = {
    "_inbox_repo_key",
    "_recover_alias_inbox_once",
    "_rekey_alias_inbox",
    "_org_repo_node_strict",
    "_org_repo_node",
}


def _load(extra_globals):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    selected = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef))
        and node.name in FUNCS | {"_OrgAliasUnresolved"}
    ]
    found = {node.name for node in selected}
    missing = (FUNCS | {"_OrgAliasUnresolved"}) - found
    assert not missing, "missing from entry.py: %s" % sorted(missing)
    module = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))
    namespace = dict(extra_globals)
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


class _Request:
    def __init__(self, path):
        self.url = "https://forkmesh.test" + path


def _runtime(alias_rows, updates, resolve_error=None):
    """alias_rows maps "org:<name>" blind indexes to the linked node owner."""

    class _Date:
        @staticmethod
        def now():
            return 1_000_000

    async def blind_index(_env, value):
        return "bi:" + str(value)

    async def ensure_schema(_env):
        return None

    async def d1_first(_env, sql, *args):
        if resolve_error is not None:
            raise resolve_error
        assert "FROM org_repos" in sql, sql
        org_bi, repo = args
        node = alias_rows.get((org_bi, repo))
        return {"node_owner": node} if node else None

    async def d1_run(_env, sql, *args):
        updates.append((sql, args))
        return None

    return {
        "Date": _Date,
        "blind_index": blind_index,
        "ensure_schema": ensure_schema,
        "d1_first": d1_first,
        "d1_run": d1_run,
        "valid_node_name": lambda name: bool(
            re.fullmatch(r"[a-z0-9][a-z0-9_-]{1,62}", str(name or ""))),
        "safe_segment": lambda value: str(value or "").strip().lower(),
        "urlparse": __import__("urllib.parse", fromlist=["urlparse"]).urlparse,
        "REPO_API_PREFIX_RE": re.compile(
            r"^/api/repo/([^/]+)/([^/]+)(?:/.*)?$"),
        "_ORG_ALIAS_MEMO": {},
        "ORG_ALIAS_MEMO_TTL_MS": 30 * 1000,
        "ORG_ALIAS_MEMO_MAX": 512,
        "_ALIAS_INBOX_REKEYED": {},
        "ALIAS_INBOX_REKEY_MAX": 512,
    }


def test_a_rewritten_alias_request_keys_the_inbox_on_the_backing_node():
    # org_alias_rewrite already swapped the org for the node, so the handler is
    # called with owner="jett" while the URL still reads /forkmesh/forkmesh.
    updates = []
    ns = _load(_runtime({}, updates))
    key = asyncio.run(ns["_inbox_repo_key"](
        object(), _Request("/api/repo/forkmesh/forkmesh/pulls"),
        "jett", "forkmesh"))
    assert key == "bi:jett/forkmesh"


def test_an_unrewritten_alias_is_resolved_instead_of_keyed_on_the_org():
    # The regression: _org_repo_node failed closed upstream, so the request
    # arrives still naming the organization. Keying it on "forkmesh/forkmesh"
    # dead-letters the submission - no drain ever reads that index.
    updates = []
    ns = _load(_runtime({("bi:org:forkmesh", "forkmesh"): "jett"}, updates))
    key = asyncio.run(ns["_inbox_repo_key"](
        object(), _Request("/api/repo/forkmesh/forkmesh/issues"),
        "forkmesh", "forkmesh"))
    assert key == "bi:jett/forkmesh"


def test_a_plain_node_url_keys_on_the_owner_and_rekeys_nothing():
    updates = []
    ns = _load(_runtime({}, updates))
    key = asyncio.run(ns["_inbox_repo_key"](
        object(), _Request("/api/repo/jett/forkmesh/issues"),
        "jett", "forkmesh"))
    assert key == "bi:jett/forkmesh"
    assert updates == []


def test_an_unresolvable_alias_refuses_the_write_instead_of_mis_keying_it():
    # A durable write whose routing key cannot be determined must fail
    # retryably. Silently accepting it is what stranded rows in the first place.
    updates = []
    ns = _load(_runtime({}, updates, resolve_error=RuntimeError("D1 overloaded")))
    try:
        asyncio.run(ns["_inbox_repo_key"](
            object(), _Request("/api/repo/forkmesh/forkmesh/pulls"),
            "forkmesh", "forkmesh"))
    except ns["_OrgAliasUnresolved"]:
        pass
    else:
        raise AssertionError("a failed alias lookup must not pick a storage key")
    assert updates == [], "nothing may be written when routing is unknown"


def test_every_inbox_handler_routes_its_key_through_the_resolver():
    # A handler that goes back to a bare blind_index re-opens the dead-letter.
    for handler in ("issues_handler", "pulls_handler", "discussions_handler"):
        body = ENTRY_TEXT.split("async def %s(" % handler, 1)[1].split(
            "\nasync def ", 1)[0]
        assert "_inbox_repo_key(env, request, owner, repo)" in body, handler
        assert 'except _OrgAliasUnresolved:' in body, handler
        assert 'status=503' in body, handler


def test_the_recovery_sweep_covers_pulls_and_discussions_not_just_issues():
    # _forkbot_rekey_alias_inbox only ever recovered issue_inbox, so aliased
    # pull and discussion submissions stayed stranded.
    updates = []
    ns = _load(_runtime({("bi:org:forkmesh", "forkmesh"): "jett"}, updates))
    asyncio.run(ns["_inbox_repo_key"](
        object(), _Request("/api/repo/forkmesh/forkmesh/pulls"),
        "forkmesh", "forkmesh"))
    swept = {
        re.search(r"UPDATE (\w+) SET", sql).group(1)
        for sql, _args in updates if sql.startswith("UPDATE ")
    }
    assert swept == {"issue_inbox", "pull_inbox", "discussion_inbox"}
    for sql, args in updates:
        assert args == ("bi:jett/forkmesh", "bi:forkmesh/forkmesh"), sql


def test_a_rewritten_request_still_sweeps_rows_stranded_on_the_alias():
    # Once the relay is healthy every alias request IS rewritten, so recovery
    # has to happen on that path too or already-stranded rows never come back.
    updates = []
    ns = _load(_runtime({}, updates))
    asyncio.run(ns["_inbox_repo_key"](
        object(), _Request("/api/repo/forkmesh/forkmesh/issues"),
        "jett", "forkmesh"))
    assert [args for _sql, args in updates] == [
        ("bi:jett/forkmesh", "bi:forkmesh/forkmesh")] * 3


def test_the_recovery_sweep_runs_once_per_repo_per_isolate():
    updates = []
    ns = _load(_runtime({}, updates))
    for _ in range(4):
        asyncio.run(ns["_inbox_repo_key"](
            object(), _Request("/api/repo/forkmesh/forkmesh/issues"),
            "jett", "forkmesh"))
    assert len(updates) == 3, "one sweep of three tables, not one per request"


def test_org_repo_node_still_fails_closed_for_routing():
    # Routing may treat "cannot resolve" as "not an org URL"; only the storage
    # key must refuse to guess.
    ns = _load(_runtime({}, [], resolve_error=RuntimeError("D1 overloaded")))
    assert asyncio.run(
        ns["_org_repo_node"](object(), "forkmesh", "forkmesh")) == ""


def test_pending_badges_count_the_key_the_drains_read():
    body = ENTRY_TEXT.split("async def repo_pending_counts_handler(", 1)[1] \
        .split("\nasync def ", 1)[0]
    assert "_ap_org_alias_owner(env, owner, repo)" in body, (
        "an aliased repo's badge must not count a queue the owner node is "
        "never shown")


def test_relay_sync_matches_an_aliased_repo_by_its_catalog_owner():
    # /api/sync names each repo by the ACCOUNT owning its catalog row, while an
    # aliased repo keeps the public org in RepositoryRecord::owner. Matching on
    # r.owner alone dropped - and never acked - the entire slice.
    text = QT_PULLS.read_text(encoding="utf-8")
    body = text.split("void MainWindow::performRelaySync()", 1)[1]
    # Exact owner+name stays the preferred match so a plain node-owned repo is
    # never shadowed by an identically named aliased one; the account's catalog
    # identity is the fallback that rescues the aliased slice.
    assert "r.owner.compare(entryOwner, Qt::CaseInsensitive) == 0" in body
    assert "catalogOwner(r).compare(entryOwner," in body
    assert "idx = aliasIdx;" in body
    # An unmatched slice drains nothing; that must not be silent.
    unmatched = body.split("if (idx < 0)", 1)[1][:900]
    assert "logSystem(" in unmatched
