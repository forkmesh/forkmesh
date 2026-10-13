#!/usr/bin/env python3

import asyncio
import importlib.util
from pathlib import Path

SRC = Path(__file__).resolve().parents[1] / "src" / "microjobs_runtime.py"


def _runtime(nodes, orgs, aliases):
    spec = importlib.util.spec_from_file_location("microjobs_runtime_own", SRC)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    async def alias_owner(_env, owner, repo):
        return aliases.get((owner, repo), owner)

    async def owns_node(_env, actor, name):
        return name in nodes.get(actor, ())

    async def org_row(_env, name):
        return ("org:" + name, {"name": name}) if name in orgs else ("", None)

    async def org_role(_env, org_bi, account):
        return orgs.get(org_bi[4:], {}).get(account, "")

    module._bind_runtime({
        "clean_string": lambda value, maximum=240: str(value or "")[:maximum],
        "MAX_NODE_NAME": 64,
        "_ap_org_alias_owner": alias_owner,
        "_account_owns_node": owns_node,
        "_org_row": org_row,
        "_org_role": org_role,
    })
    return module._MicrojobsRuntime(object())


def _controls(runtime, account, owner, repo):
    return asyncio.run(
        runtime.account_controls_repository(account, owner, repo))


def test_owner_node_and_org_members_control_the_repository():
    runtime = _runtime(
        nodes={"alice": ("alice-box",)},
        orgs={"acme": {"bob": "member"}},
        aliases={("acme", "site"): "alice-box"},
    )
    assert _controls(runtime, "alice", "alice", "r")
    assert _controls(runtime, "alice", "alice-box", "r")
    assert _controls(runtime, "alice", "acme", "site")
    assert _controls(runtime, "bob", "acme", "other")
    assert _controls(runtime, "", "anyone", "r")


def test_an_unrelated_account_does_not_control_the_repository():
    runtime = _runtime(
        nodes={"alice": ("alice-box",)},
        orgs={"acme": {"bob": "member"}},
        aliases={},
    )
    assert not _controls(runtime, "carol", "alice", "r")
    assert not _controls(runtime, "carol", "alice-box", "r")
    assert not _controls(runtime, "carol", "acme", "site")
