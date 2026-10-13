#!/usr/bin/env python3
"""Hosted provider imports stop depending on the provider they came from.

A mirror node clones a public import and publishes a signed catalog record for
it; the relay then links that record back to the import entry and routes the
importer's own logical name to it. Both halves used to be Codeberg- and
mirror2/mirror3-shaped, so a GitHub import stayed a provider link forever
(issue #679).
"""

import ast
import asyncio
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
ENTRY_TEXT = ENTRY.read_text(encoding="utf-8")
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import catalog as catalog_policy  # noqa: E402
import repository_imports as repository_import  # noqa: E402

NOW = 1_800_000_000_000
FUNCTIONS = {
    "_promote_hosted_repository_import",
    "_hosted_repository_import_route",
}


class _Date:
    @staticmethod
    def now():
        return NOW


def _load(rows, notifications, updates):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    selected = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in FUNCTIONS
    ]
    assert {node.name for node in selected} == FUNCTIONS
    module = ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[]))

    async def d1_all(_env, sql, *args):
        assert "FROM repository_imports" in sql
        assert "is_private=0" in sql
        selected_rows = []
        for row in rows:
            if "status='actively_mirrored'" in sql:
                if row["status"] != "actively_mirrored":
                    continue
            elif "status NOT IN ('actively_mirrored','archived')" in sql:
                if row["status"] in ("actively_mirrored", "archived"):
                    continue
            providers = re.search(r"provider IN \(([^)]*)\)", sql)
            if providers:
                allowed = {
                    value.strip().strip("'")
                    for value in providers.group(1).split(",")
                }
                if row["data"]["provider"] not in allowed:
                    continue
            selected_rows.append({"id": row["id"], "data": row["data"]})
        return selected_rows[: args[-1]]

    async def d1_run(_env, sql, *args):
        assert "UPDATE repository_imports" in sql
        updates.append(args)
        return None

    async def decrypt_row(_env, data):
        return data

    async def encrypt_row(_env, record):
        return record

    async def enqueue_notification(_env, recipient, kind, title, **rest):
        notifications.append(
            {"recipient": recipient, "kind": kind, "title": title, **rest})
        return True

    namespace = {
        "Date": _Date,
        "MAX_NODE_NAME": 63,
        "MAX_REPO_SEGMENT": 80,
        "clean_string": catalog_policy.clean_string,
        "safe_segment": catalog_policy.safe_segment,
        "valid_node_name": lambda value: bool(
            re.fullmatch(r"[a-z0-9][a-z0-9-]{0,62}", (value or "").strip())),
        "_repository_import_module": lambda: repository_import,
        "d1_all": d1_all,
        "d1_run": d1_run,
        "decrypt_row": decrypt_row,
        "encrypt_row": encrypt_row,
        "enqueue_notification": enqueue_notification,
    }
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


def _row(import_id, provider, name, *, branch="main",
         status="external_repository", mirror=None):
    return {
        "id": import_id,
        "status": status,
        "data": {
            "id": import_id,
            "provider": provider,
            "name": name,
            "targetOwner": "spidertest",
            "status": status,
            "metadata": {"defaultBranch": branch},
            **({"mirror": mirror} if mirror else {}),
        },
    }


def test_a_github_import_is_linked_to_its_hosted_mirror():
    rows = [_row("ext_" + "a" * 24, "github", "spider-demo")]
    notifications, updates = [], []
    namespace = _load(rows, notifications, updates)

    promoted = asyncio.run(namespace["_promote_hosted_repository_import"](
        None, {"owner": "mirror37", "name": "spider-demo", "branch": "main"}))

    assert promoted is True
    record = rows[0]["data"]
    assert record["status"] == "actively_mirrored"
    assert record["mirrored"] is True
    assert record["sourceCodeFetched"] is True
    assert record["mirror"] == {
        "owner": "mirror37", "name": "spider-demo", "live": True}
    assert updates and updates[0][0] == "actively_mirrored"
    assert notifications[0]["recipient"] == "spidertest"
    assert "GitHub import is synced" in notifications[0]["body"]


def test_promotion_still_covers_gitlab_and_codeberg_imports():
    for provider, label in (("gitlab", "GitLab"), ("codeberg", "Codeberg")):
        rows = [_row("ext_" + "b" * 24, provider, "demo")]
        notifications, updates = [], []
        namespace = _load(rows, notifications, updates)

        promoted = asyncio.run(namespace["_promote_hosted_repository_import"](
            None, {"owner": "mirror37", "name": "demo", "branch": "main"}))

        assert promoted is True
        assert label + " import is synced" in notifications[0]["body"]


def test_promotion_prefers_the_import_whose_branch_the_mirror_published():
    """One repository name can arrive from two providers.

    A catalog record carries no import id, so the published default branch is
    the only signal that separates two same-named public imports. The wrong
    entry must not be told its code is hosted.
    """
    rows = [
        _row("ext_" + "c" * 24, "github", "app", branch="trunk"),
        _row("ext_" + "d" * 24, "codeberg", "app", branch="main"),
    ]
    notifications, updates = [], []
    namespace = _load(rows, notifications, updates)

    promoted = asyncio.run(namespace["_promote_hosted_repository_import"](
        None, {"owner": "mirror37", "name": "app", "branch": "main"}))

    assert promoted is True
    assert rows[1]["data"]["status"] == "actively_mirrored"
    assert rows[0]["data"]["status"] == "external_repository"
    assert notifications[0]["meta"] == {"importId": "ext_" + "d" * 24}


def test_an_unsupported_provider_row_is_never_promoted():
    rows = [_row("ext_" + "e" * 24, "bitbucket", "demo")]
    notifications, updates = [], []
    namespace = _load(rows, notifications, updates)

    promoted = asyncio.run(namespace["_promote_hosted_repository_import"](
        None, {"owner": "mirror37", "name": "demo", "branch": "main"}))

    assert promoted is False
    assert (notifications, updates) == ([], [])


def test_hosted_route_resolves_a_currently_serving_mirror_node():
    """Routing may not be pinned to the two nodes of the original burst.

    mirror2 and mirror3 are retired, so pinning them sent every newly hosted
    import back to its provider instead of to ForkMesh.
    """
    rows = [
        _row(
            "ext_" + "f" * 24, "github", "spider-demo",
            status="actively_mirrored",
            mirror={"owner": "mirror37", "name": "spider-demo", "live": True},
        ),
    ]
    namespace = _load(rows, [], [])

    resolved = asyncio.run(namespace["_hosted_repository_import_route"](
        None, "spidertest", "spider-demo"))

    assert resolved == ("mirror37", "spider-demo")


def test_hosted_route_refuses_an_unusable_mirror_pair():
    rows = [
        _row(
            "ext_" + "0" * 24, "github", "spider-demo",
            status="actively_mirrored",
            mirror={"owner": "", "name": "spider-demo", "live": True},
        ),
    ]
    namespace = _load(rows, [], [])

    assert asyncio.run(namespace["_hosted_repository_import_route"](
        None, "spidertest", "spider-demo")) is None
