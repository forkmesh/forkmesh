"""Catalog writes accept only account-bound owner-signing device keys."""

import ast
import asyncio
from pathlib import Path

import pytest


ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"
SOURCE = ENTRY.read_text(encoding="utf-8")
CATALOG_ROUTES = ENTRY.parent / "catalog_routes.py"
CATALOG_SOURCE = CATALOG_ROUTES.read_text(encoding="utf-8")
FUNCTIONS = {
    "_account_devices_list",
    "_owner_signing_pubkeys",
    "_account_signing_pubkeys",
    "_catalog_publication_key",
}

PRIMARY_KEY = "P" * 43
DEVICE_KEY = "D" * 43
UNBOUND_KEY = "U" * 43


def _load_functions(device_rows):
    tree = ast.parse(SOURCE, filename=str(ENTRY))
    selected = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in FUNCTIONS
    ]
    assert {node.name for node in selected} == FUNCTIONS

    async def _account_row(_env, owner):
        return "bi:" + str(owner), {"name": owner, "pubkey": PRIMARY_KEY}

    async def blind_index(_env, value):
        return "bi:" + str(value)

    async def d1_all(_env, sql, *_args):
        assert "FROM account_devices WHERE account_bi=?" in sql
        return list(device_rows)

    def clean_string(value, maximum):
        return str(value or "")[:maximum]

    def valid_node_pubkey(value):
        value = str(value or "")
        return len(value) == 43 and value.isalnum()

    namespace = {
        "_account_row": _account_row,
        "blind_index": blind_index,
        "d1_all": d1_all,
        "clean_string": clean_string,
        "valid_node_pubkey": valid_node_pubkey,
    }
    module = ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[]))
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


def _device(*, enabled=True, revoked_at=0, capabilities=None):
    return {
        "device_bi": "device-bi",
        "pubkey": DEVICE_KEY,
        "kind": "desktop_node",
        "label": "headless mirror",
        "enabled": 1 if enabled else 0,
        "last_seen": 1,
        "revoked_at": revoked_at,
        "capabilities": (
            "browse,owner_sign"
            if capabilities is None else capabilities
        ),
    }


def _selected(devices, key):
    functions = _load_functions(devices)
    return asyncio.run(functions["_catalog_publication_key"](
        object(), "mirror2", key))


def test_primary_and_enabled_owner_sign_device_can_publish_catalog():
    assert _selected([], PRIMARY_KEY) == PRIMARY_KEY
    assert _selected([_device()], DEVICE_KEY) == DEVICE_KEY


@pytest.mark.parametrize(
    "device",
    [
        _device(enabled=False),  # disabled
        _device(revoked_at=1),
        _device(capabilities="browse,publish_repo"),
    ],
    ids=["disabled", "revoked", "missing-owner-sign"],
)
def test_inactive_or_unprivileged_device_cannot_publish_catalog(device):
    assert _selected([device], DEVICE_KEY) == ""


def test_unbound_or_malformed_key_cannot_publish_catalog():
    assert _selected([_device()], UNBOUND_KEY) == ""
    assert _selected([_device()], "not-an-ed25519-key") == ""


def test_catalog_and_state_signatures_use_selected_maintainer_key():
    tree = ast.parse(CATALOG_SOURCE, filename=str(CATALOG_ROUTES))
    handler = next(
        node for node in tree.body
        if isinstance(node, ast.AsyncFunctionDef)
        and node.name == "catalog_handler"
    )

    selected_assignment = [
        node for node in ast.walk(handler)
        if isinstance(node, ast.Assign)
        and any(
            isinstance(target, ast.Name) and target.id == "owner_pub"
            for target in node.targets
        )
        and isinstance(node.value, ast.Await)
        and isinstance(node.value.value, ast.Call)
        and isinstance(node.value.value.func, ast.Name)
        and node.value.value.func.id == "_catalog_publication_key"
    ]
    assert len(selected_assignment) == 1

    verification_pairs = set()
    for node in ast.walk(handler):
        if (
            isinstance(node, ast.Call)
            and isinstance(node.func, ast.Name)
            and node.func.id == "ed25519_verify"
            and len(node.args) >= 2
            and isinstance(node.args[0], ast.Name)
            and isinstance(node.args[1], ast.Name)
        ):
            verification_pairs.add((node.args[0].id, node.args[1].id))

    assert ("owner_pub", "catalog_sig") in verification_pairs
    assert ("owner_pub", "state_sig") in verification_pairs
