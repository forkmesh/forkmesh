#!/usr/bin/env python3
"""Platform-admin-only permanent World node removal."""

import ast
import asyncio
from pathlib import Path

from worker_test_helpers import json_from_request_double


# MainWindowChat.cpp was split into feature files; read them in the original
# source order so section slices that span functions still line up.
MAIN_WINDOW_CHAT_SOURCES = (
    "MainWindowChat.cpp",
    "MainWindowNetworkInternal.h",
    "MainWindowDiagnostics.cpp",
    "MainWindowLogs.cpp",
    "MainWindowNavigation.cpp",
    "MainWindowNetworkRepos.cpp",
    "MainWindowHosts.cpp",
    "MainWindowNodes.cpp",
    "MainWindowNetworkDiagnostics.cpp",
    "MainWindowUsers.cpp",
    "MainWindowVultrMirrors.cpp",
    "MainWindowHostInstall.cpp",
)

ROOT = Path(__file__).resolve().parents[2] / "app"
ENTRY = ROOT / "src" / "entry.py"
# The World and Office route bodies moved into sibling modules so Cloudflare's
# Python startup validation never parses them. This contract still covers the
# whole route family, so their source is read alongside entry.py.
WORLD_ROUTE_SOURCE = "".join(
    (Path(__file__).resolve().parents[1] / "src" / name).read_text(
        encoding="utf-8")
    for name in ("world_routes.py", "world_office_routes.py")
)
ENTRY_TEXT = (
    ENTRY.read_text(encoding="utf-8")
    + WORLD_ROUTE_SOURCE
    + (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
)
WORLD = ROOT.parent / "world" / "public" / "world" / "world.js"


class Request:
    method = "POST"

    def __init__(self, payload):
        self._payload = payload

    async def json(self):
        return self._payload


def response(payload, status=200, **kwargs):
    return {"data": payload, "status": status, **kwargs}


def runtime(admin=True, alias=False, missing=False, endpoint_alias=False,
            session=True, signed_actor=""):
    node_record = {"name": "mirror6", "owner": "jett", "pubkey": "p" * 64}
    owner_record = {"name": "jett", "nodes": ["mirror6", "mirror7"]}
    writes = []
    audits = []
    saved = []
    deleted = []

    async def account_row(_env, name):
        if name == "mirror6" and not alias and not missing:
            return "bi:mirror6", dict(node_record)
        if name == "mirror6" and alias and not missing:
            return "bi:mirror6", dict(node_record)
        if name == "jett":
            return "bi:jett", dict(owner_record)
        return f"bi:{name}", None

    async def d1_first(_env, sql, *params):
        if "FROM nodes" in sql and params == ("bi:mirror6",):
            return {"node_bi": "bi:mirror6"}
        if "lower(name)" in sql and params == (
            "threaded-lantern-2584",
            "mirror6-host",
            "p" * 64,
        ):
            return {"node_bi": "bi:mirror6", "name": "mirror6"}
        if "FROM mirror_https_endpoints" in sql and endpoint_alias:
            return {
                "node_bi": "bi:meshed-mirror-1271",
                "node_name": "meshed-mirror-1271",
            }
        return None

    async def d1_run(_env, sql, *params):
        writes.append((sql, params))

    async def audit(_env, actor, action, target_type, target, outcome,
                    details=None):
        audits.append((actor, action, target_type, target, outcome, details))

    async def save(_env, key, record):
        saved.append((key, record))

    async def delete(_env, key, record):
        deleted.append((key, record))

    async def rehome_aliases(_env, _target, _owner):
        return [], ""

    function = next(
        node for node in ast.parse(ENTRY_TEXT, filename=str(ENTRY)).body
        if isinstance(node, ast.AsyncFunctionDef)
        and node.name == "world_admin_delete_node_handler"
    )
    namespace = {
        "method_name": lambda request: request.method,
        "bounded_json_request": json_from_request_double,
        "json_response": response,
        "_authed_account_name": lambda *_args: asyncio.sleep(
            0, result="jett" if session else ""),
        "_world_node_delete_signed_actor": lambda *_args: asyncio.sleep(
            0, result=signed_actor),
        "_is_admin": lambda _env, name: asyncio.sleep(
            0, result=admin and name == "jett"),
        "clean_string": lambda value, maximum: str(value or "")[:maximum],
        "MAX_NODE_NAME": 64,
        "valid_node_name": lambda value: bool(value)
        and value.replace("-", "").isalnum(),
        "_audit_sensitive_action": audit,
        "_account_row": account_row,
        "d1_first": d1_first,
        "_owned_nodes": lambda record: list(record.get("nodes") or []),
        "_save_account": save,
        "d1_run": d1_run,
        "_delete_account_namespace": delete,
        "rehome_organization_aliases_before_node_delete": rehome_aliases,
        "STATUS_MIRROR_PREFIX": "mirror:",
    }
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=[function], type_ignores=[])), str(ENTRY), "exec"),
         namespace)
    return namespace["world_admin_delete_node_handler"], writes, audits, saved, deleted


def test_non_admin_cannot_delete_a_node():
    handler, writes, audits, saved, deleted = runtime(admin=False)
    result = asyncio.run(handler(None, Request({
        "nodeName": "mirror6",
        "confirmation": "DELETE mirror6",
    })))
    assert result["status"] == 403
    assert not writes and not audits and not saved and not deleted


def test_exact_confirmation_is_required_before_any_mutation():
    handler, writes, audits, saved, deleted = runtime()
    result = asyncio.run(handler(None, Request({
        "nodeName": "mirror6",
        "confirmation": "mirror6",
    })))
    assert result["status"] == 400
    assert result["data"]["requiredConfirmation"] == "DELETE mirror6"
    assert not writes and not audits and not saved and not deleted


def test_admin_delete_unlinks_owner_purges_node_state_and_audits():
    handler, writes, audits, saved, deleted = runtime()
    result = asyncio.run(handler(None, Request({
        "nodeName": "mirror6",
        "confirmation": "DELETE mirror6",
    })))
    assert result["status"] == 200
    assert result["data"]["nodeDeleted"] == "mirror6"
    assert saved == [("bi:jett", {"name": "jett", "nodes": ["mirror7"]})]
    assert any("reward_node_observations" in sql for sql, _ in writes)
    assert any("mirror_https_endpoints" in sql for sql, _ in writes)
    assert deleted == [("bi:mirror6", {
        "name": "mirror6", "owner": "jett", "pubkey": "p" * 64,
    })]
    assert audits[-1][1:5] == (
        "world.node_delete", "node", "mirror6", "success")


def test_admin_delete_resolves_visible_machine_and_node_ids_to_canonical_node():
    handler, writes, audits, saved, deleted = runtime(alias=True)
    result = asyncio.run(handler(None, Request({
        "nodeName": "threaded-lantern-2584",
        "machineName": "mirror6-host",
        "nodeId": "p" * 64,
        "confirmation": "DELETE threaded-lantern-2584",
    })))
    assert result["status"] == 200
    assert result["data"]["nodeDeleted"] == "mirror6"
    assert saved == [("bi:jett", {"name": "jett", "nodes": ["mirror7"]})]
    assert any(params == ("bi:mirror6",) for _, params in writes)
    assert {key for key, _ in deleted} == {
        "bi:threaded-lantern-2584",
        "bi:mirror6",
    }
    assert audits[-1][3:5] == ("mirror6", "success")


def test_admin_delete_finishes_partially_missing_node_and_all_related_state():
    handler, writes, audits, saved, deleted = runtime(
        missing=True, endpoint_alias=True)
    result = asyncio.run(handler(None, Request({
        "nodeName": "meshed-mirror-1271",
        "machineName": "meshed-mirror-1271",
        "confirmation": "DELETE meshed-mirror-1271",
    })))
    assert result["status"] == 200
    assert result["data"]["nodeDeleted"] == "meshed-mirror-1271"
    assert "meshed-mirror-1271" in result["data"]["identifiers"]
    for table in (
        "reward_node_observations",
        "private_mirror_routes",
        "mirror_https_endpoints",
        "org_agent_jobs",
        "org_agent_sessions",
    ):
        assert any(table in sql for sql, _ in writes)
    assert deleted == [(
        "bi:meshed-mirror-1271",
        {"name": "meshed-mirror-1271", "kind": "node"},
    )]
    assert audits[-1][4] == "success"


def test_admin_node_delete_is_idempotent_after_every_primary_row_is_gone():
    handler, writes, audits, saved, deleted = runtime(missing=True)
    result = asyncio.run(handler(None, Request({
        "nodeName": "old-mirror",
        "confirmation": "DELETE old-mirror",
    })))
    assert result["status"] == 200
    assert result["data"]["alreadyAbsent"] is True
    assert deleted == [
        ("bi:old-mirror", {"name": "old-mirror", "kind": "node"})
    ]
    assert audits[-1][4] == "success"


def test_admin_delete_erases_the_nodes_status_page_history():
    handler, writes, _audits, _saved, _deleted = runtime()
    result = asyncio.run(handler(None, Request({
        "nodeName": "mirror6",
        "confirmation": "DELETE mirror6",
    })))
    assert result["status"] == 200
    for table in (
        "system_status_daily",
        "system_status_hourly",
        "system_status_minute",
    ):
        assert ("DELETE FROM " + table + " WHERE system=?",
                ("mirror:mirror6",)) in writes


def test_key_signed_desktop_admin_can_delete_without_a_session_token():
    handler, _writes, audits, saved, deleted = runtime(
        session=False, signed_actor="jett")
    result = asyncio.run(handler(None, Request({
        "nodeName": "mirror6",
        "confirmation": "DELETE mirror6",
    })))
    assert result["status"] == 200
    assert saved == [("bi:jett", {"name": "jett", "nodes": ["mirror7"]})]
    assert deleted and audits[-1][0] == "jett"


def test_unsigned_sessionless_request_is_still_refused():
    handler, writes, audits, saved, deleted = runtime(session=False)
    result = asyncio.run(handler(None, Request({
        "nodeName": "mirror6",
        "confirmation": "DELETE mirror6",
    })))
    assert result["status"] == 403
    assert not writes and not audits and not saved and not deleted


def test_signed_actor_proof_names_the_node_it_deletes():
    source = ENTRY_TEXT.split(
        "async def _world_node_delete_signed_actor", 1)[1].split(
            "\nasync def ", 1)[0]
    # The signature covers actor + target, so it cannot be replayed against a
    # different node, and _verify_owner_signature accepts the account's stored
    # device keys (a silently authenticated desktop never holds the primary).
    assert 'WORLD_NODE_DELETE_PROOF + "\\n" + actor + "\\n" + target' in source
    assert "_verify_owner_signature(env, actor, sig, canonical)" in source
    assert "_ts_ok(ts)" in source
    assert 'record.get("status") != "active"' in source


def test_qt_nodes_page_deletes_the_node_everywhere():
    chat = "".join(
        (Path(__file__).resolve().parents[2] / "desktop" / "src" / name)
        .read_text(encoding="utf-8")
        for name in MAIN_WINDOW_CHAT_SOURCES
    )
    assert "kNodeColActions" in chat
    assert "void MainWindow::deleteMeshNodeCompletely" in chat
    # Vultr teardown, DNS cleanup and the relay removal, in that order.
    assert "destroyVultrServerForNode(target" in chat
    assert "removeVultrMirrorDns(target" in chat
    assert "sendMeshNodeDeleteRequest(target, nodeId)" in chat
    assert '"1/3 ' in chat
    assert '"2/3 ' in chat
    assert '"3/3 ' in chat
    assert '"/v2/instances/") + instanceId' in chat
    assert '"/zones/%1/dns_records/%2"' in chat
    assert '"/api/world/admin/nodes/delete"' in chat
    assert 'QStringLiteral("DELETE ") + node' in chat
    # Admin-only, and never offered for this machine or a reserved name.
    buttons = chat[chat.index("void MainWindow::refreshNodeActionButtons()"):]
    buttons = buttons[:buttons.index('QStringLiteral("Delete")')]
    assert (
        "setColumnHidden(kNodeColActions, !m_isAdmin && !linkActions)"
        in buttons
    )
    assert "if (!m_isAdmin)\n        return;" in buttons
    assert "isProtectedMeshNode" in chat
    assert "forkmesh-world-node-delete-v1" in chat


def test_world_ui_only_renders_delete_action_for_admins():
    world = WORLD.read_text(encoding="utf-8")
    assert "this.identity?.isAdmin === true" in world
    assert "data-world-admin-delete-node" in world
    assert 'data-world-delete-step="1"' in world
    assert 'data-world-delete-step="2"' in world
    assert 'data-world-delete-step="3"' in world
    assert 'setStep(2, "active")' in world
    assert 'setStep(3, "complete")' in world
    assert "node?.name || node?.machineName" in world
    assert "DELETE ${nodeName}" in world
    assert 'this.postJSON("/api/world/admin/nodes/delete"' in world
    assert "this.world?.deleteNetworkNode?.({" in world
    assert "identifiers: Array.isArray(result?.identifiers)" in world
    assert "await this.loadWorldData({ forceMirrors: true });" in world
    assert 'cache: force ? "no-store" : "default"' in world
    assert "/api/world/admin/nodes/delete" in ENTRY_TEXT


def test_desktop_node_delete_keeps_progress_visible_and_requires_relay_confirmation():
    chat = "".join(
        (Path(__file__).resolve().parents[2] / "desktop" / "src" / name)
        .read_text(encoding="utf-8")
        for name in MAIN_WINDOW_CHAT_SOURCES
    )
    header = (
        Path(__file__).resolve().parents[2]
        / "desktop" / "src" / "MainWindow.h"
    ).read_text(encoding="utf-8")
    assert "bool m_nodeDeleteInProgress = false;" in header
    assert "QSet<QString> m_deletedMeshNodeNames;" in header
    assert (
        "if (!m_nodesTable || !m_nodesStatus || m_nodeDeleteInProgress)\n"
        "        return;" in chat
    )
    assert 'response.value(QStringLiteral("nodeDeleted"))' in chat
    assert 'response.value(QStringLiteral("identifiers"))' in chat
    assert "The relay did not confirm deletion" in chat
    assert "Relay confirmed deletion of" in chat
    assert "m_deletedMeshNodeNames.unite(deletedNames);" in chat
    assert "recordDeletedMeshNode(deleted);" in chat
    assert "recordDecommissionedMirrorNode(deleted);" in chat
    assert "meshNodeWasDeleted(key, catalogPublishedMs(key))" in chat
    assert "Node was not deleted. Click Delete again" in chat
