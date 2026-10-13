#!/usr/bin/env python3
"""The desktop mints its own genie credential (adhoc #49).

Pressing "genie" used to require generating a bearer token in Organization
Admin and pasting it into the desktop's Settings. The desktop already proves it
owns its account's Ed25519 key for every organization task it opens, so the same
signature now mints the same revocable, task-scoped credential. These checks pin
that the new proof is as narrow as the task ones — POST only, one path, no
authority for an inactive or non-user account — and that the endpoint it guards
mints nothing broader than the two task scopes.

AST-extraction harness in the style of test_org_task_signed_writes.py.
"""

import ast
import asyncio
import hashlib
import json
import re
from pathlib import Path
from urllib.parse import parse_qs, urlparse


ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"
APP_RUNTIME = ENTRY.parent / "app_runtime.py"
CATALOG = ENTRY.parent / "catalog.py"
ENTRY_TEXT = ENTRY.read_text(encoding="utf-8")
APP_RUNTIME_TEXT = APP_RUNTIME.read_text(encoding="utf-8")
SOURCE_TEXT = ENTRY_TEXT + "\n" + CATALOG.read_text(encoding="utf-8")
QT_INTERNAL = (
    ENTRY.parents[2] / "desktop" / "src" / "MainWindowInternal.h"
).read_text(encoding="utf-8")
QT_MCP = (
    ENTRY.parents[2] / "desktop" / "src" / "MainWindowMcp.cpp"
).read_text(encoding="utf-8")

FUNCS = {
    "_genie_credential_signed_session",
    "_signed_account_session",
    "_account_signing_pubkeys",
    "_owner_signing_pubkeys",
    "_verify_owner_signature",
    "clean_string",
}
CONSTANTS = {"GENIE_CREDENTIAL_PROOF", "ORG_TASK_OPEN_PROOF"}

HANDLER = ENTRY_TEXT[
    ENTRY_TEXT.index("async def genie_credential_handler("):
    ENTRY_TEXT.index("\n\nasync def bot_session_handler(", ENTRY_TEXT.index(
        "async def genie_credential_handler("))
]


def _load(extra):
    tree = ast.parse(SOURCE_TEXT, filename=str(ENTRY))
    selected = []
    for node in tree.body:
        if (
            isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
            and node.name in FUNCS
        ):
            selected.append(node)
        elif isinstance(node, ast.Assign) and any(
            isinstance(target, ast.Name) and target.id in CONSTANTS
            for target in node.targets
        ):
            selected.append(node)
    found = {
        node.name if hasattr(node, "name") else node.targets[0].id
        for node in selected
    }
    assert found == FUNCS | CONSTANTS, \
        "missing: %s" % sorted((FUNCS | CONSTANTS) - found)
    module = ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[]))
    namespace = dict(extra)
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


class _Headers:
    def __init__(self, mapping=None):
        self._m = {str(k).lower(): v for k, v in (mapping or {}).items()}

    def get(self, name, default=None):
        return self._m.get(str(name).lower(), default)


class _Request:
    def __init__(self, method="POST", url="", headers=None):
        self.method = method
        self.url = url
        self.headers = _Headers(headers)


def _fake_sig(pubkey, canonical):
    digest = hashlib.sha256(canonical).hexdigest()[:32]
    return "sig-" + pubkey + "-" + digest


def _harness(accounts, ts_ok=True, devices=None):
    devices = devices or {}

    async def _account_row(_env, name):
        key = str(name or "").strip().lower()
        record = accounts.get(key)
        if record is None:
            return "bi:" + key, None
        record = dict(record)
        record.setdefault("name", key)
        return "bi:" + key, record

    async def _owner_pubkey(_env, owner):
        _bi, record = await _account_row(_env, owner)
        return record.get("pubkey", "") if record else ""

    async def ed25519_verify(pubkey, sig, canonical):
        return bool(pubkey) and sig == _fake_sig(pubkey, canonical)

    async def blind_index(_env, value):
        return "bi:" + str(value or "").strip().lower()

    async def _account_devices_list(_env, account_bi):
        return [dict(device) for device in devices.get(account_bi, [])]

    namespace = _load({
        "re": re,
        "parse_qs": parse_qs,
        "urlparse": urlparse,
        "method_name": lambda request: getattr(request, "method", "GET"),
        "_ts_ok": lambda _ts: ts_ok,
        "_owner_pubkey": _owner_pubkey,
        "_account_row": _account_row,
        "_account_devices_list": _account_devices_list,
        "blind_index": blind_index,
        "_account_kind": lambda record: record.get("kind", "user"),
        "ed25519_verify": ed25519_verify,
        "MAX_NODE_NAME": 63,
    })
    return namespace, object()


def _signed_url(namespace, proof_name="GENIE_CREDENTIAL_PROOF",
                path="/api/genie/credential", node="alice",
                pubkey="PK-alice", ts="1700000000000"):
    canonical = namespace[proof_name] + "\n" + node + "\n" + ts
    sig = _fake_sig(pubkey, canonical.encode())
    return (
        "https://forkmesh.test" + path
        + "?node=" + node + "&ts=" + ts + "&sig=" + sig
    )


def _user(**overrides):
    record = {"pubkey": "PK-alice", "status": "active", "kind": "user"}
    record.update(overrides)
    return record


def test_key_signed_desktop_resolves_its_own_account():
    namespace, env = _harness({"alice": _user()})
    account_bi, record = asyncio.run(
        namespace["_genie_credential_signed_session"](
            env, _Request(url=_signed_url(namespace))))
    assert account_bi == "bi:alice"
    assert record["name"] == "alice"


def test_registered_device_key_mints_the_credential():
    """A restarted/reinstalled desktop signs with its account_devices key.

    The primary pubkey still names the install that created the account, so
    the gate has to accept every enabled owner_sign device — the same rule
    the task board follows (adhoc #63).
    """
    namespace, env = _harness(
        {"alice": _user()},
        devices={"bi:alice": [
            {"pubkey": "PK-alice-laptop", "capabilities": ["owner_sign"],
             "enabled": True},
            {"pubkey": "PK-alice-retired", "capabilities": ["owner_sign"],
             "enabled": False},
        ]},
    )
    resolve = namespace["_genie_credential_signed_session"]
    account_bi, record = asyncio.run(resolve(env, _Request(
        url=_signed_url(namespace, pubkey="PK-alice-laptop"))))
    assert (account_bi, record["name"]) == ("bi:alice", "alice")
    assert asyncio.run(resolve(env, _Request(
        url=_signed_url(namespace, pubkey="PK-alice-retired")))) == ("", None)


def test_genie_proof_is_post_only_and_not_interchangeable_with_task_proofs():
    namespace, env = _harness({"alice": _user()})
    resolve = namespace["_genie_credential_signed_session"]

    for method in ("GET", "PATCH", "DELETE"):
        assert asyncio.run(resolve(env, _Request(
            method=method, url=_signed_url(namespace)))) == ("", None), method

    # A proof signed to open a task must not mint a credential.
    crossed = _signed_url(namespace, proof_name="ORG_TASK_OPEN_PROOF")
    assert asyncio.run(resolve(env, _Request(url=crossed))) == ("", None)


def test_forged_stale_and_ineligible_accounts_are_refused():
    namespace, env = _harness({
        "alice": _user(),
        "suspended": _user(status="suspended"),
        "orgaccount": _user(kind="org"),
    })
    resolve = namespace["_genie_credential_signed_session"]

    forged = (
        "https://forkmesh.test/api/genie/credential"
        "?node=alice&ts=1700000000000&sig=not-a-real-signature"
    )
    assert asyncio.run(resolve(env, _Request(url=forged))) == ("", None)
    assert asyncio.run(resolve(env, _Request(url=_signed_url(
        namespace, pubkey="PK-mallory")))) == ("", None)
    assert asyncio.run(resolve(env, _Request(
        url="https://forkmesh.test/api/genie/credential"))) == ("", None)
    for node in ("nobody", "suspended", "orgaccount"):
        assert asyncio.run(resolve(env, _Request(url=_signed_url(
            namespace, node=node, pubkey="PK-" + node
            if node == "nobody" else "PK-alice")))) == ("", None), node

    stale, stale_env = _harness({"alice": _user()}, ts_ok=False)
    assert asyncio.run(stale["_genie_credential_signed_session"](
        stale_env, _Request(url=_signed_url(stale)))) == ("", None)


def test_endpoint_is_routed_and_prefers_the_signature_over_ambient_authority():
    assert 'url.path in ("/api/genie/credential", "/api/genie/credential/")' \
        in APP_RUNTIME_TEXT
    assert "return await genie_credential_handler(self.env, request)" \
        in APP_RUNTIME_TEXT
    signed = HANDLER.index("_genie_credential_signed_session(env, request)")
    session = HANDLER.index("_account_session_record(env, request, data)")
    # The signature is consulted first, and an account session only as a
    # fallback — never a cookie on a cross-origin write, which
    # _account_session_record itself refuses.
    assert signed < session
    assert '"method_not_allowed"' in HANDLER
    assert 'cache_control="no-store"' in HANDLER


def test_minted_credential_is_task_only_and_admin_gated():
    assert 'GENIE_CREDENTIAL_SCOPES = ("organization.tasks.read", ' \
        '"organization.tasks.write")' in ENTRY_TEXT
    assert "scopes = list(GENIE_CREDENTIAL_SCOPES)" in HANDLER
    assert 'await _org_role(env, org_bi, actor) not in ("owner", "admin")' \
        in HANDLER
    assert '"forbidden"' in HANDLER
    # Same store, same cap, and an audit record either way.
    assert "INSERT INTO org_bot_tokens " in HANDLER
    assert "ORG_BOT_TOKEN_MAX_ACTIVE" in HANDLER
    assert '"too_many_bot_tokens"' in HANDLER
    assert HANDLER.count('"organization.bot_token_create"') == 2
    # Re-minting retires this device's previous credential instead of stacking
    # new ones up against the cap.
    assert "UPDATE org_bot_tokens SET revoked_at=?" in HANDLER


def test_genie_prompt_cannot_collide_with_a_project_scoped_mcp_server():
    """The remote task server must not be named after a checkout's own server.

    This repository's .mcp.json already points `forkmesh` at the local stdio
    mesh server, so a genie run told to configure a remote `forkmesh` would end
    up without list_org_tasks at all.
    """
    project = json.loads(
        (ENTRY.parents[2] / ".mcp.json").read_text(encoding="utf-8"))
    assert "forkmesh" in project["mcpServers"]
    prompt = QT_MCP[
        QT_MCP.index("QString MainWindow::genieSetupPrompt("):
        QT_MCP.index("void MainWindow::startGenieAgent()")
    ]
    assert 'QStringLiteral("forkmesh-tasks")' in prompt
    assert 'QStringLiteral("forkmesh")' not in prompt
    assert "list_org_tasks on forkmesh-tasks" in prompt


def test_desktop_signs_the_same_proof_and_shows_the_run_as_a_session():
    assert '"forkmesh-genie-credential-v1"' in ENTRY_TEXT
    assert '"forkmesh-genie-credential-v1"' in QT_INTERNAL
    assert '"/api/genie/credential"' in QT_MCP
    assert "kGenieCredentialProof" in QT_MCP
    # Pressing genie starts the run; it no longer bounces to Settings for a
    # hand-pasted credential.
    start = QT_MCP[
        QT_MCP.index("void MainWindow::startGenieAgent()"):
        QT_MCP.index("void MainWindow::requestGenieCredential(")
    ]
    assert "requestGenieCredential(repoIndex, typed)" in start
    assert "showSection(1)" not in start
    # The session list names it, so the run is visible as a running agent
    # session rather than only in the log.
    launch = QT_MCP[
        QT_MCP.index("void MainWindow::launchGenieRun("):
        QT_MCP.index("void MainWindow::applyGenieTaskTitle(")
    ]
    assert "startAdHocAgentForRepo(" in launch
    assert "Genie" in launch
    assert "agent session #%1" in launch
