from __future__ import annotations

import json
import subprocess
import sys

from forkmesh_host import paths

PROGRAM = r"""
import asyncio
import json
import os
import sys
import time

from forkmesh_host.env import HostConfig
from forkmesh_host.runtime import HostRuntime
from forkmesh_host.serve import _migrate

contributors, repositories = int(sys.argv[2]), int(sys.argv[3])
host = HostRuntime(HostConfig(data_dir=sys.argv[1], environ={
    "FORKMESH_VAR_DATA_KEY": "unit-" + os.urandom(8).hex(),
    "FORKMESH_HOST_OFFLINE": "1",
}))
_migrate(host)
entry, env = host.entry, host.env


async def seed():
    now = int(time.time() * 1000)
    for i in range(repositories):
        owner = "owner%d" % i
        owner_bi = await entry.blind_index(env, owner)
        await entry.d1_run(
            env, "INSERT INTO repositories (key_bi, owner_bi, data, is_private)"
            " VALUES (?, ?, ?, 0)",
            await entry.blind_index(env, owner + "/repo"), owner_bi,
            await entry.encrypt_row(env, {
                "owner": owner, "name": "repo", "visibility": "public",
                "sizeBytes": 1000 + i, "updatedAt": now}))
        await entry.d1_run(
            env, "INSERT INTO nodes (node_bi, user_bi, pubkey, data, name,"
            " last_seen) VALUES (?, ?, ?, ?, ?, ?)",
            owner_bi, owner_bi, "pk%d" % i,
            await entry.encrypt_row(env, {"name": owner, "status": "active"}),
            owner, now)
        org_bi = await entry.blind_index(env, "org:org%d" % i)
        await entry.d1_run(
            env, "INSERT INTO orgs (org_bi, name, data, created_at)"
            " VALUES (?, ?, '{}', 0)", org_bi, "org%d" % i)
        await entry.d1_run(
            env, "INSERT INTO org_repos (org_bi, repo, node_owner, created_at)"
            " VALUES (?, 'repo', ?, 0)", org_bi, owner)
    for i in range(contributors):
        name = "contrib%d" % i
        await entry.d1_run(
            env, "INSERT INTO contributor_activity (author_bi, name, issues,"
            " pulls, commits, total) VALUES (?, ?, 1, 1, 1, ?)",
            "author%d" % i, name, 100 - i)
    for name, table, private in (
            ("contrib0", "users", True),
            ("contrib1", "nodes", True),
            ("contrib2", "users", False)):
        name_bi = await entry.blind_index(env, name)
        data = await entry.encrypt_row(
            env, {"name": name, "status": "active", "profile_private": private})
        if table == "users":
            await entry.d1_run(
                env, "INSERT INTO users (user_bi, data) VALUES (?, ?)",
                name_bi, data)
        else:
            await entry.d1_run(
                env, "INSERT INTO nodes (node_bi, data, name) VALUES (?, ?, ?)",
                name_bi, data, name)


async def main():
    await seed()
    await entry.ensure_schema(env)
    variable, token, budget = entry._cron_d1_budget_start()
    error = ""
    try:
        await entry._publish_public_snapshot_group(
            env, ("/api/network/overview",), int(time.time() * 1000))
    except BaseException as caught:
        error = str(caught)
    finally:
        entry._cron_d1_budget_stop(variable, token)
    row = await entry.d1_first(
        env, "SELECT payload FROM public_api_snapshots WHERE path=?",
        "/api/network/overview")
    payload = json.loads(row["payload"]) if row else {}
    print(json.dumps({
        "used": budget.get("used"),
        "exhausted": bool(budget.get("exhausted")),
        "error": error,
        "cap": entry.CRON_BATCH_SUBREQUEST_CAP
        - entry.CRON_BATCH_RESERVE_HEADROOM,
        "contributors": [
            row["name"] for row in
            payload.get("leaderboards", {}).get("contributors", [])],
        "largest": [
            row["name"] for row in
            payload.get("leaderboards", {}).get("largest", [])],
    }))


asyncio.run(main())
"""


def _build(tmp_path, sandbox_env, contributors, repositories):
    data = tmp_path / ("data-%d-%d" % (contributors, repositories))
    result = subprocess.run(
        [sys.executable, "-c", PROGRAM, str(data), str(contributors),
         str(repositories)],
        cwd=str(paths.HOST_DIR), env=sandbox_env(tmp_path),
        capture_output=True, text=True, timeout=240)
    assert result.returncode == 0, result.stderr[-4000:]
    return json.loads(result.stdout.strip().splitlines()[-1])


def test_network_overview_build_fits_one_cron_budget(
        staged_src, tmp_path, sandbox_env):
    del staged_src
    small = _build(tmp_path, sandbox_env, 2, 2)
    full = _build(tmp_path, sandbox_env, 12, 12)
    assert full["error"] == "" and not full["exhausted"], full
    assert full["used"] <= full["cap"], full
    assert full["used"] == small["used"], (small, full)
    assert full["contributors"][:3] == [
        "Private contributor", "Private contributor", "contrib2"]
    assert full["contributors"][3:] == ["contrib%d" % i for i in range(3, 10)]
    assert full["largest"] == ["owner%d/repo" % i for i in range(11, 1, -1)]

