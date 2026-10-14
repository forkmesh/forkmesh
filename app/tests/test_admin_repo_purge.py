"""An administrator can erase a repository they do not host (adhoc #196).

The desktop's Network Repos catalog shows every repository the relay
advertises, but its Delete button only ever removed the copy on this
machine — a row for a repository this device never cloned had nothing to
delete, so the button sat disabled with "This repository is not on this
machine". An
administrator now signs an admin-specific canonical and DELETE
/api/repositories purges the repository everywhere: the catalog row, every
repo-scoped table, and a tombstone so the owner's node cannot republish it.
"""

import ast
import json
import re
from pathlib import Path
from types import SimpleNamespace

import pytest

from test_repo_delete_tombstone import (  # noqa: E402
    NOW,
    _Relay,
    _handlers,
    _run,
)
from _maintenance_stubs import with_maintenance_off

ROOT = Path(__file__).resolve().parents[2]
ENTRY = ROOT / "app" / "src" / "entry.py"
SCHEMA = ROOT / "app" / "src" / "schema.py"
CATALOG_ROUTES = ROOT / "app" / "src" / "catalog_routes.py"

ADMIN_CANONICAL = (
    "forkmesh-admin-repo-delete-v1\nroot\nalice\ndemo\n%d" % NOW
)


def _admin_delete_request(user="root", ts=NOW, sig="admin-signature"):
    url = "https://forkmesh.com/api/repositories?owner=alice&name=demo"
    if user:
        url += "&user=" + user
    return SimpleNamespace(
        method="DELETE",
        url=url + "&ts=%d&sig=%s" % (ts, sig),
        headers={},
    )


def _admin_relay(**kwargs):
    # No web session at all: the desktop has none, so the request stands or
    # falls on the administrator's signature.
    return _Relay(session_owner="", admin_account="root", **kwargs)


def test_admin_purge_deletes_the_row_and_every_scoped_table():
    relay = _admin_relay()
    handler = _handlers(relay)["catalog_handler"]
    response = _run(handler(None, _admin_delete_request()))
    assert response.payload == {"ok": True, "deleted": True}
    assert relay.repos == {}
    assert relay.deleted_scoped == ["bi:alice/demo"]
    assert relay.deleted_named == ["alice/demo"]


def test_admin_purge_tombstones_so_the_owner_node_cannot_republish():
    # The owner's node still holds the mirror and republishes on every
    # heartbeat; without the tombstone the purge would undo itself.
    relay = _admin_relay()
    _run(_handlers(relay)["catalog_handler"](None, _admin_delete_request()))
    assert "bi:alice/demo" in relay.tombstones
    handler = _handlers(
        relay,
        {"owner": "alice", "name": "demo", "maintainer": "pk",
         "catalogSigVersion": 2, "catalogSig": "sig", "publishIntent": "auto"},
    )["catalog_handler"]
    response = _run(handler(None, SimpleNamespace(
        method="POST", url="https://forkmesh.com/api/repositories",
        headers={})))
    assert response.status == 410
    assert relay.repos == {}


def test_admin_purge_signs_an_admin_specific_canonical():
    relay = _admin_relay()
    _run(_handlers(relay)["catalog_handler"](None, _admin_delete_request()))
    assert relay.admin_canonicals == [ADMIN_CANONICAL]
    # Naming the administrator and the target inside the signed text is what
    # stops an owner's own delete signature being replayed as an admin purge,
    # and one admin purge being replayed against a different repository.
    assert "forkmesh-catalog-delete-v1" not in relay.admin_canonicals[0]


def test_a_signer_who_is_not_an_admin_changes_nothing():
    relay = _Relay(session_owner="", admin_account="root")
    request = _admin_delete_request(user="mallory")
    response = _run(_handlers(relay)["catalog_handler"](None, request))
    assert response.status == 401
    assert response.payload == {"error": "unauthorized"}
    assert relay.repos != {}
    assert relay.tombstones == {}
    assert relay.deleted_scoped == []


def test_admin_purge_without_a_signature_is_refused():
    relay = _admin_relay()
    request = _admin_delete_request(sig="")
    response = _run(_handlers(relay)["catalog_handler"](None, request))
    assert response.status == 400
    assert relay.repos != {}


def test_admin_purge_cleans_up_when_the_catalog_row_is_already_gone():
    # A partially deleted repository, or one this relay only ever saw through
    # an inbox, leaves scoped rows with no catalog row to find them from.
    relay = _admin_relay(repo_exists=False)
    handler = _handlers(relay)["catalog_handler"]
    response = _run(handler(None, _admin_delete_request()))
    assert response.payload == {"ok": True, "deleted": False}
    assert relay.deleted_scoped == ["bi:alice/demo"]
    assert relay.deleted_named == ["alice/demo"]
    assert "bi:alice/demo" in relay.tombstones


def test_owner_signed_delete_still_needs_no_tombstone():
    # The admin branch must not change the desktop's own owner-signed delete,
    # which is the publisher moving or retiring its own repository.
    relay = _Relay(session_owner="")
    namespace = _handlers(relay)

    async def owner_pubkey(env, owner):
        return "pk"

    async def verify_owner_signature(env, owner, sig, canonical):
        return True

    namespace["_owner_pubkey"] = owner_pubkey
    namespace["_verify_owner_signature"] = verify_owner_signature
    request = _admin_delete_request(user="")
    response = _run(namespace["catalog_handler"](None, request))
    assert response.payload == {"ok": True, "deleted": True}
    assert relay.tombstones == {}
    assert relay.admin_canonicals == []


def test_the_purge_names_the_acting_administrator_in_the_audit_log():
    source = CATALOG_ROUTES.read_text(encoding="utf-8")
    start = source.index('if method == "DELETE":')
    branch = source[start:source.index(
        '"method_not_allowed"', start)]
    assert 'env, audit_actor, "repository.delete"' in branch
    assert 'env, owner, "repository.delete"' not in branch
    assert '{"reason": "not_admin"}' in branch


# --- the purge has to keep up with the schema -------------------------------

# Repository-scoped tables a delete deliberately leaves alone. Everything
# else keyed on a repository must be purged, or a deleted repository keeps
# living there.
PURGE_EXEMPT = {
    # The tombstone recording the delete itself.
    "repo_deletions",
    # Moderation and payment evidence about people, not repository state.
    "discord_bug_reports",
    # Not a repository table: the comment above it mentions repo_bi.
    "relay_self",
    "org_office_visits",
}


def _schema_tables():
    source = SCHEMA.read_text(encoding="utf-8")
    pattern = re.compile(
        r'CREATE TABLE IF NOT EXISTS\s+([a-z_0-9]+)\s*\((.*?)\)"""'
        r'|CREATE TABLE IF NOT EXISTS\s+([a-z_0-9]+)\s*\((.*?)\)"',
        re.S)
    tables = {}
    for match in pattern.finditer(source):
        name = match.group(1) or match.group(3)
        tables[name] = match.group(2) or match.group(4)
    assert len(tables) > 200, "the schema scan should see the whole schema"
    return tables


def _purged_tables(tables):
    """Every table the two repo purge helpers name, however they name it.

    Some are written inline ("DELETE FROM repo_shares WHERE ..."), the rest
    are entries in a table-driven list, so match any quoted word that is a
    real table name as well as the inline statements.
    """
    source = ENTRY.read_text(encoding="utf-8")
    start = source.index("_REPO_SCOPED_TABLES = (")
    end = source.index("async def _delete_repo_namespace", start)
    body = source[start:end]
    purged = set(re.findall(r'DELETE FROM ([a-z_0-9]+)', body))
    purged |= {word for word in re.findall(r'"([a-z_0-9]+)"', body)
               if word in tables}
    return purged


@pytest.mark.parametrize("column", ["repo_bi", "source_repo_bi"])
def test_every_repo_keyed_table_is_purged_by_a_repository_delete(column):
    tables = _schema_tables()
    purged = _purged_tables(tables)
    missing = sorted(
        name for name, body in tables.items()
        if re.search(r"\b%s\b\s+TEXT" % column, body)
        and name not in purged and name not in PURGE_EXEMPT
    )
    assert not missing, (
        "these repository-scoped tables survive a repository delete; purge "
        "them in _delete_repo_scoped_state or list them in PURGE_EXEMPT: "
        + ", ".join(missing))


# --- the purge has to fit in one request's CPU budget -----------------------

class _BatchDB:
    """A D1 binding that records single-statement runs and batches apart."""

    def __init__(self):
        self.runs = 0
        self.batches = []

    def prepare(self, sql):
        db = self

        class _Stmt:
            args = ()

            def bind(self, *args):
                self.args = args
                return self

            async def run(self):
                db.runs += 1

        stmt = _Stmt()
        stmt.sql = sql
        return stmt

    async def batch(self, statements):
        self.batches.append([(stmt.sql, stmt.args) for stmt in statements])


def _purge_helpers():
    names = {"d1_run_batch", "_delete_repo_scoped_state",
             "_repo_scoped_state_statements", "_delete_repo_named_state"}
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"))
    selected = [
        node for node in tree.body
        if (isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
            and node.name in names)
        or (isinstance(node, ast.Assign)
            and any(isinstance(target, ast.Name)
                    and target.id == "_REPO_SCOPED_TABLES"
                    for target in node.targets))
    ]

    async def blind_index(env, value):
        return "bi:" + value

    namespace = {
        "_AGENT_PUSH_DIGESTS": {},
        "_cron_d1_query_charge": lambda count=1: None,
        "_log_d1_query": lambda *args, **kwargs: None,
        "d1_bind_args": lambda args: list(args),
        "blind_index": blind_index,
        "clean_string": lambda value, maximum=240: str(value)[:maximum],
        "MAX_NODE_NAME": 64,
        "MAX_REPO_SEGMENT": 100,
    }
    exec(compile(ast.Module(body=selected, type_ignores=[]), str(ENTRY),
                 "exec"), with_maintenance_off(namespace))
    return namespace


def test_the_purge_is_two_d1_batches_not_one_await_per_table():
    # ~60 sequential d1_run awaits pushed an admin purge past the Worker CPU
    # limit (Error 1102) half way through, after the tombstone was written.
    helpers = _purge_helpers()
    env = SimpleNamespace(DB=_BatchDB())
    _run(helpers["_delete_repo_scoped_state"](env, "bi:alice/demo"))
    _run(helpers["_delete_repo_named_state"](env, "alice", "demo"))
    assert env.DB.runs == 0
    assert len(env.DB.batches) == 2
    scoped, named = env.DB.batches
    assert len(scoped) > 30
    assert all(sql.startswith("DELETE FROM ") for sql, _ in scoped + named)
    assert ("DELETE FROM repo_forks WHERE fork_bi=? OR parent_bi=?",
            ("bi:alice/demo", "bi:alice/demo")) in scoped
    assert ("DELETE FROM repo_webhooks WHERE repo_bi=?",
            ("bi:alice/demo",)) in scoped
    assert ("DELETE FROM org_repos WHERE repo=? AND node_owner=?",
            ("demo", "alice")) in named


# --- the desktop side of the same contract ----------------------------------

NETWORK_REPOS = ROOT / "desktop" / "src" / "MainWindowNetworkRepos.cpp"
ADMIN_DELETE_HEADER = ROOT / "desktop" / "src" / "CatalogAdminDelete.h"
ENDPOINT_CATALOG = (
    ROOT / "desktop" / "resources" / "network" / "endpoint-catalog.json")


def test_desktop_signs_the_canonical_the_relay_verifies():
    header = ADMIN_DELETE_HEADER.read_text(encoding="utf-8")
    assert "forkmesh-admin-repo-delete-v1" in header
    # The relay lowercases the administrator and the target before building
    # the canonical, so the desktop has to as well or nothing verifies.
    assert "user.toLower()" in header
    assert "owner.toLower()" in header
    assert 'addQueryItem(QStringLiteral("user")' in header


def test_the_catalog_delete_button_is_live_only_for_an_admin():
    source = NETWORK_REPOS.read_text(encoding="utf-8")
    start = source.index("void MainWindow::addNetworkRepoRowActions(")
    body = source[start:source.index("\nvoid MainWindow::", start + 1)]
    assert "if (canAdminPurgeRepos()) {" in body
    # The old dead button is what a non-admin still gets.
    dead = body.index('"This repository is not on this machine"')
    assert body.index("canAdminPurgeRepos()") < dead
    assert "adminPurgeNetworkRepo(routeOwner, routeName)" in body


def test_the_desktop_purge_needs_an_admin_key_and_a_typed_confirmation():
    source = NETWORK_REPOS.read_text(encoding="utf-8")
    start = source.index("bool MainWindow::canAdminPurgeRepos() const")
    body = source[start:source.index(
        "void MainWindow::discardNetworkRepoPreview(", start)]
    assert "m_isAdmin" in body and "hasOwnerSigningCapability(account)" in body
    assert "QInputDialog::getText(" in body
    assert "catalogAdminDeleteCanonical(admin, owner, name, ts)" in body
    # A real DeleteOperation, so the network log can name the call from the
    # endpoint catalog (a custom verb records as "CUSTOM" and matches nothing).
    assert "m_networkAccess->deleteResource(request)" in body


def test_the_purge_request_is_catalogued_for_the_desktop_firewall():
    catalog = json.loads(ENDPOINT_CATALOG.read_text(encoding="utf-8"))
    entries = [
        entry for entry in catalog["endpoints"]
        if entry["pattern"].endswith("/api/repositories*")
        and entry["method"] == "DELETE"
    ]
    assert len(entries) == 1
    assert entries[0]["source"] == [
        "desktop/src/MainWindowNetworkRepos.cpp: "
        "MainWindow::adminPurgeNetworkRepo"
    ]
