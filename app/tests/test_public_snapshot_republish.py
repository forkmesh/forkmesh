#!/usr/bin/env python3
"""Event-driven public snapshot republish (Design A).

A mutation that feeds an anonymous catalog projection rings the
snapshot-republish-v1 doorbell object; its alarm rebuilds the affected paths
through the same publisher the Cron phases use. These tests pin the sender
(best-effort, awaited, never a detached task), the row verdict that keeps the
tombstone guarantee, the budgeted publisher, the doorbell call sites, and the
trimmed browser max-age table. The Durable Object state machine itself is
covered in test_cron_runner.py.
"""

import ast
import asyncio
import re
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest


APP = Path(__file__).resolve().parents[1]
ENTRY = APP / "src" / "entry.py"
CATALOG_ROUTES = APP / "src" / "catalog_routes.py"
ENTRY_TEXT = (
    # entry.py + its lazily-split domain modules
    (ENTRY.parent / "app_runtime.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
)
CATALOG_TEXT = CATALOG_ROUTES.read_text(encoding="utf-8")
sys.path.insert(0, str(APP / "src"))
import public_snapshots as snapshots  # noqa: E402


WANTED = {
    "SNAPSHOT_REPUBLISH_NAME",
    "SNAPSHOT_REPUBLISH_PATH",
    "SNAPSHOT_REPUBLISH_LEAD_MS",
    "SNAPSHOT_REPUBLISH_MIN_INTERVAL_MS",
    "SNAPSHOT_REPUBLISH_DOORBELL_TIMEOUT_MS",
    "SNAPSHOT_REPUBLISH_STATUS_PATHS",
    "SNAPSHOT_REPUBLISH_PATHS",
    "SNAPSHOT_REPUBLISH_DOORBELL_PATHS",
    "MAX_REPO_SEGMENT",
    "schedule_public_catalog_republish",
    "schedule_public_snapshot_republish",
    "_public_catalog_snapshot_paths",
    "_is_flagship_snapshot_target",
    "_republish_ready_at",
    "_republish_group",
    "_republish_row_verdict",
    "_republish_publish_path",
    "_exact_public_snapshot_integer",
    "js_nullish",
}


def _load(extra):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    selected = [
        node for node in tree.body
        if (
            isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
            and node.name in WANTED
        ) or (
            isinstance(node, ast.Assign)
            and any(
                isinstance(target, ast.Name) and target.id in WANTED
                for target in node.targets
            )
        )
    ]
    found = {getattr(node, "name", None) for node in selected} | {
        target.id for node in selected
        for target in getattr(node, "targets", [])
        if isinstance(target, ast.Name)
    }
    assert WANTED <= found, WANTED - found
    namespace = {
        "clean_string": lambda value, limit=0: str(value or "")[:limit or None],
        "quote": __import__("urllib.parse", fromlist=["quote"]).quote,
        "to_js": lambda value: value,
        "public_snapshots": snapshots,
        "hmac": __import__("hmac"),
        "re": re,
    }
    namespace.update(extra)
    module = ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[]))
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


class _FakeRequest:
    def __init__(self, url, init):
        self.url = url
        self.init = init


class _FakeSignal:
    timeouts = []

    @classmethod
    def timeout(cls, value):
        cls.timeouts.append(value)
        return ("abort-signal", value)


def _sender_ns(*, response_status=202, fail=None, binding=True):
    calls = {"fetch": [], "charges": 0}

    class Stub:
        def __init__(self, name):
            self.name = name

        async def fetch(self, request):
            calls["fetch"].append((self.name, request))
            if fail is not None:
                raise fail
            return SimpleNamespace(status=response_status)

    class Binding:
        def idFromName(self, name):
            return "id:" + name

        def get(self, object_id):
            return Stub(object_id)

    def charge(count=1):
        calls["charges"] += count

    env = SimpleNamespace(
        FORKMESH_CRON_RUNNER=Binding() if binding else None,
    )
    ns = _load({
        "JsRequest": SimpleNamespace(new=_FakeRequest),
        "JsAbortSignal": _FakeSignal,
        "_cron_d1_query_charge": charge,
    })
    return ns, env, calls


def test_doorbell_targets_the_republish_object_with_the_snapshot_paths():
    ns, env, calls = _sender_ns()
    ring = ns["schedule_public_catalog_republish"]
    assert asyncio.run(ring(env, "alice", "demo")) is True
    assert len(calls["fetch"]) == 1
    name, request = calls["fetch"][0]
    assert name == "id:snapshot-republish-v1"
    assert request.url == (
        "https://forkmesh.internal/cron-runner/republish?paths="
        "%2Fapi%2Frepositories%2C%2Fapi%2Fnetwork%2Foverview")
    assert request.init["method"] == "POST"
    # Native AbortSignal timeout, never a Python wait_for: no nested task.
    assert request.init["signal"] == (
        "abort-signal", ns["SNAPSHOT_REPUBLISH_DOORBELL_TIMEOUT_MS"])
    # One DO call is one subrequest against a ringing Cron phase's budget.
    assert calls["charges"] == 1

    # A status transition uses the same awaited doorbell. Both public status
    # projections travel together so the full and World views cannot disagree.
    assert asyncio.run(ns["schedule_public_snapshot_republish"](
        env, snapshots.SNAPSHOT_STATUS_PATHS)) is True
    status_request = calls["fetch"][-1][1]
    assert status_request.url == (
        "https://forkmesh.internal/cron-runner/republish?paths="
        "%2Fapi%2Fstatus%2C%2Fapi%2Fstatus%2Fworld")

    # A forkmesh repository (canonical or any mirror-node copy) also refreshes
    # the flagship mirror roster; so does an explicit include_flagship.
    asyncio.run(ring(env, "mirror9", "ForkMesh"))
    asyncio.run(ring(env, include_flagship=True))
    for _name, request in calls["fetch"][-2:]:
        assert "%2Fapi%2Frepo%2Fforkmesh%2Fforkmesh%2Fmirrors" in request.url
    assert asyncio.run(ns["_public_catalog_snapshot_paths"](
        env, "alice", "demo")) == [
        "/api/repositories", "/api/network/overview"]
    assert set(asyncio.run(ns["_public_catalog_snapshot_paths"](
        env, "alice", "forkmesh"))) == set(ns["SNAPSHOT_REPUBLISH_PATHS"])


def test_doorbell_is_best_effort_and_never_fails_the_mutation():
    # No binding (tests, stripped self-hosts): quietly skipped.
    ns, env, calls = _sender_ns(binding=False)
    assert asyncio.run(
        ns["schedule_public_catalog_republish"](env, "alice", "demo")) is False
    assert calls["fetch"] == []
    # A rejected doorbell (mixed-version rollout, old object code) or a
    # thrown DO error only means the change waits for the Cron cadence.
    ns, env, _calls = _sender_ns(response_status=404)
    assert asyncio.run(
        ns["schedule_public_catalog_republish"](env, "alice", "demo")) is False
    ns, env, _calls = _sender_ns(fail=RuntimeError("durable object reset"))
    assert asyncio.run(
        ns["schedule_public_catalog_republish"](env, "alice", "demo")) is False


def _verdict_ns(row):
    async def snapshot_row(_env, path):
        if isinstance(row, BaseException):
            raise row
        return row

    return _load({"_public_snapshot_row": snapshot_row})


def _row(path, kind, generated_at, stale_until):
    return {
        "path": path,
        "kind": kind,
        "generated_at": generated_at,
        "fresh_until": stale_until,
        "stale_until": stale_until,
    }


def test_row_verdict_keeps_the_tombstone_barrier_and_skips_reflected_rows():
    path = "/api/repositories"
    now = 1_784_000_000_000
    requested = now - 3_000
    verdict = lambda row: asyncio.run(_verdict_ns(row)[
        "_republish_row_verdict"](None, path, requested, now))

    # An active privacy-transition barrier defers the rebuild to its deadline
    # instead of spending a scan the UPSERT guard would reject.
    active = _row(path, snapshots.SNAPSHOT_TOMBSTONE_KIND, now - 60_000,
                  now + 90_000)
    assert verdict(active) == ("tombstone", now + 90_000)
    # An expired barrier is exactly what the rebuild heals.
    expired = _row(path, snapshots.SNAPSHOT_TOMBSTONE_KIND, now - 300_000,
                   now - 1)
    assert verdict(expired) == ("build", 0)
    # A generation that started after the mutation committed already carries
    # it (a Cron phase got there first): no second scan.
    assert verdict(_row(path, snapshots.SNAPSHOT_KIND, requested + 1,
                        now + 300_000)) == ("reflected", requested + 1)
    # Same-millisecond or older generations may have missed it: build.
    assert verdict(_row(path, snapshots.SNAPSHOT_KIND, requested,
                        now + 300_000)) == ("build", 0)
    assert verdict(_row(path, snapshots.SNAPSHOT_KIND, requested - 5_000,
                        now + 300_000)) == ("build", 0)
    # Missing, foreign, corrupt or unreadable rows all fall through to a
    # normal build; the publisher's own validation still guards the write.
    assert verdict(None) == ("build", 0)
    assert verdict(_row("/api/status", snapshots.SNAPSHOT_KIND, now,
                        now + 1)) == ("build", 0)
    assert verdict(_row(path, snapshots.SNAPSHOT_KIND, "soon",
                        now + 1)) == ("build", 0)
    assert verdict(_row(path, "unknown-kind", now, now + 1)) == ("build", 0)
    assert verdict(RuntimeError("D1 unavailable")) == ("build", 0)


def _publisher_ns(*, fail=None, exhaust=False):
    calls = {"publish": [], "logs": [], "budget": []}
    variable = {"state": None}

    def budget_start():
        state = {"used": 0, "exhausted": False}
        variable["state"] = state
        calls["budget"].append("start")
        return variable, "token", state

    def budget_stop(_variable, token):
        assert token == "token"
        variable["state"] = None
        calls["budget"].append("stop")

    async def publish(_env, paths, generated_at):
        calls["publish"].append((tuple(paths), generated_at))
        assert variable["state"] is not None, "publish must run under budget"
        if exhaust:
            variable["state"]["exhausted"] = True
        if fail is not None:
            raise fail

    async def log_cron_error(_env, path, message, error=None, failures=None):
        # The failure is logged after the budget is released, so the log's
        # own D1 write can never be what exhausts it.
        assert variable["state"] is None
        calls["logs"].append((path, message, error))

    ns = _load({
        "_cron_d1_budget_start": budget_start,
        "_cron_d1_budget_stop": budget_stop,
        "_publish_public_snapshot_group": publish,
        "log_cron_error": log_cron_error,
        "_safe_error_text": str,
    })
    return ns, calls


def test_republish_publisher_is_budgeted_and_logs_failures_once():
    ns, calls = _publisher_ns()
    ok = asyncio.run(ns["_republish_publish_path"](
        None, "/api/repositories", 1_000))
    assert ok is True
    assert calls["publish"] == [(("/api/repositories",), 1_000)]
    assert calls["budget"] == ["start", "stop"]
    assert calls["logs"] == []

    ns, calls = _publisher_ns()
    ok = asyncio.run(ns["_republish_publish_path"](
        None, "/api/status", 1_500))
    assert ok is True
    assert calls["publish"] == [(
        ("/api/status", "/api/status/world"), 1_500)]

    ns, calls = _publisher_ns(fail=RuntimeError("handler returned HTTP 500"))
    ok = asyncio.run(ns["_republish_publish_path"](
        None, "/api/network/overview", 2_000))
    assert ok is False
    assert calls["budget"] == ["start", "stop"]
    assert calls["logs"] == [(
        "/cron/public-api-snapshots/api-network-overview",
        "public API snapshot api-network-overview republish failed: "
        "handler returned HTTP 500",
        calls["logs"][0][2],
    )]
    # error_log groups on the exact message: volatile byte counts and platform
    # reference ids collapse so a persistent failure stays one group.
    for raw, stable in (
        ("handler response exceeded the snapshot cap (1638401 bytes)",
         "handler response exceeded the snapshot cap (# bytes)"),
        ("D1_ERROR: internal error; reference = 7f3a1c9e2b4d",
         "D1_ERROR: internal error; reference = #"),
    ):
        ns, calls = _publisher_ns(fail=RuntimeError(raw))
        asyncio.run(ns["_republish_publish_path"](
            None, "/api/repositories", 2_500))
        assert calls["logs"][0][1].endswith("republish failed: " + stable)

    # A swallowed 41st subrequest still fails the publication closed.
    ns, calls = _publisher_ns(exhaust=True)
    assert asyncio.run(ns["_republish_publish_path"](
        None, "/api/repo/forkmesh/forkmesh/mirrors", 3_000)) is False
    assert "budget exhausted" in calls["logs"][0][1]


def test_bucket_and_lead_delay_are_the_tombstone_bucket_and_seconds():
    ns = _load({})
    assert ns["SNAPSHOT_REPUBLISH_MIN_INTERVAL_MS"] == (
        snapshots.SNAPSHOT_TOMBSTONE_MS)
    assert ns["SNAPSHOT_REPUBLISH_LEAD_MS"] == 3_000
    assert set(ns["SNAPSHOT_REPUBLISH_PATHS"]) == set(
        snapshots.SNAPSHOT_PATHS) - set(snapshots.SNAPSHOT_STATUS_PATHS)
    assert set(ns["SNAPSHOT_REPUBLISH_STATUS_PATHS"]) == set(
        snapshots.SNAPSHOT_STATUS_PATHS)
    assert set(ns["SNAPSHOT_REPUBLISH_DOORBELL_PATHS"]) == set(
        snapshots.SNAPSHOT_PATHS)
    ready_at = ns["_republish_ready_at"]
    # Worst case per path: one leading build plus one build per bucket, so
    # even a catalog that never stops changing adds far fewer scheduled rows
    # than the Free-plan write allowance the cadence test pins.
    per_day = 24 * 60 * 60 * 1000 // ns["SNAPSHOT_REPUBLISH_MIN_INTERVAL_MS"]
    assert per_day * len(ns["SNAPSHOT_REPUBLISH_PATHS"]) == 2160 < 100_000
    now = 1_784_000_000_000
    assert ready_at(now, 0) == now + 3_000
    assert ready_at(now, now - 1_000) == now - 1_000 + 120_000
    assert ready_at(now, now - 1_000, "/api/status") == now + 3_000


def test_no_detached_task_or_wait_until_carries_the_republish():
    # ctx.waitUntil on a Python coroutine would be a second Pyodide task —
    # the isolate-wedging shape the module-level rule forbids. The doorbell
    # is awaited and the rebuild runs in the object's own alarm invocation.
    assert ".waitUntil(" not in ENTRY_TEXT
    assert ".waitUntil(" not in CATALOG_TEXT
    start = ENTRY_TEXT.index("async def schedule_public_snapshot_republish")
    sender = ENTRY_TEXT[start:ENTRY_TEXT.index("\n\n\n", start)]
    assert "await doorbell.fetch(request)" in sender
    assert "asyncio" not in sender
    assert "JsAbortSignal.timeout(" in sender
    assert "except BaseException:\n        return False" in sender


def test_every_catalog_mutation_rings_the_doorbell_after_its_purge():
    ring = "await schedule_public_catalog_republish("
    # catalog_routes: canonical publish, device-mirror publish, both delete
    # branches and the organization alias handoff.
    post_body = CATALOG_TEXT.split('if method == "POST":', 1)[1]
    delete_body = post_body.split('if method == "DELETE":', 1)[1]
    post_body = post_body.split('if method == "DELETE":', 1)[0]
    canonical_purge = post_body.index("await purge_catalog_related_caches()")
    canonical_ring = post_body.index(ring, canonical_purge)
    assert canonical_ring < post_body.index(
        'notify_repo_host(env, owner, record["name"], "catalog")')
    assert "include_flagship=republish_flagship" in post_body[
        canonical_ring:canonical_ring + 200]
    device_bump = post_body.index("await _bump_catalog_rev(env)")
    assert post_body.index(ring, device_bump) < canonical_purge
    assert delete_body.count(ring) == 2
    assert delete_body.index("await _record_repo_delete_tombstone(") < \
        delete_body.index(ring)
    assert delete_body.index("await purge_catalog_related_caches()") < \
        delete_body.rindex(ring)
    rehome = CATALOG_TEXT[
        CATALOG_TEXT.index(
            "async def rehome_organization_aliases_before_node_delete"):
        CATALOG_TEXT.index("\n\n\n", CATALOG_TEXT.index(
            "async def rehome_organization_aliases_before_node_delete"))
    ]
    assert rehome.index("await purge_catalog_related_caches()") < \
        rehome.index(ring)

    # entry.py: every purge site that describes a catalog change now rings.
    def body(name):
        start = ENTRY_TEXT.index("async def " + name)
        return ENTRY_TEXT[start:ENTRY_TEXT.index("\n\n\n", start)]

    for name in (
        "purge_blocked_catalog",
        "purge_stale_registered_nodes",
        "_delete_account_namespace",
    ):
        text = body(name)
        assert text.index("await purge_catalog_related_caches()") < \
            text.index(ring), name
    # The rename and account-deletion tombstones share their flagship answer
    # with the doorbell that heals them.
    for name in ("_delete_account_namespace",):
        assert "include_flagship=include_flagship" in body(name)
    rename = ENTRY_TEXT[
        ENTRY_TEXT.index("    move_error = await _move_repo_namespace(\n"
                         "        env, name_bi, old_name, new_name_bi, "
                         "new_name, apply_changes=False)"):
    ]
    rename = rename[:rename.index("\n\n\n")]
    assert rename.index("await purge_catalog_related_caches()") < \
        rename.index(ring)
    assert "include_flagship=include_flagship" in rename
    # About edits, admin Terms flags and organization alias changes.
    about = ENTRY_TEXT[ENTRY_TEXT.index(
        'notify_repo_host(env, owner, repo, "about"))'):]
    about = about[:about.index("if text_changed or media_changed:")]
    assert about.index("await purge_catalog_related_caches()") < \
        about.index(ring)
    terms = ENTRY_TEXT[ENTRY_TEXT.index("INSERT INTO repo_terms_flags"):]
    terms = terms[:terms.index("Repository Terms flag cleared.")]
    assert terms.index("await purge_catalog_related_caches()") < \
        terms.index("await schedule_public_catalog_republish(self.env)")
    assert ENTRY_TEXT.count(
        "# The alias feeds the anonymous catalog's logicalOwners.") == 2
    org_delete = ENTRY_TEXT[ENTRY_TEXT.index(
        '{"linkedRepositories": len(linked or [])}') - 700:]
    org_delete = org_delete[:org_delete.index("linkedRepositories")]
    assert "if linked:" in org_delete
    assert org_delete.index("await purge_catalog_related_caches()") < \
        org_delete.index(ring)


def test_browser_max_age_keeps_catalog_hot_without_caching_status():
    default = next(
        node for node in ast.parse(ENTRY_TEXT).body
        if isinstance(node, ast.ClassDef) and node.name == "Default")
    table = next(
        ast.literal_eval(node.value) for node in default.body
        if isinstance(node, ast.Assign)
        and any(getattr(target, "id", "") == "_PUBLIC_SNAPSHOT_MAX_AGE"
                for target in node.targets))
    assert table == {
        "/api/repositories": 300,
        "/api/network/overview": 20,
        "/api/repo/forkmesh/forkmesh/mirrors": 3,
        "/api/status": 10,
        "/api/status/world": 10,
    }
    # Status bodies ride the /api/status/ws doorbell and stay uncached.
    response = ENTRY_TEXT[ENTRY_TEXT.index(
        "    async def _public_snapshot_response"):]
    response = response[:response.index("\n    def ")]
    assert "if path in public_snapshots.SNAPSHOT_STATUS_PATHS" in response


def test_cron_cadence_is_unchanged_and_documents_the_doorbell():
    # The doorbell is additive: mirrors/status keep the three-minute
    # residues and the slow projections their five-minute stagger, because
    # liveness drift (endpoint health, behind counts, online history) never
    # rings a doorbell and the rows must not expire into live fallback.
    assert "PUBLIC_SNAPSHOT_FREQUENT_CADENCE_MINUTES = 3" in ENTRY_TEXT
    assert "PUBLIC_SNAPSHOT_SLOW_CADENCE_MINUTES = 5" in ENTRY_TEXT
    due = ENTRY_TEXT[ENTRY_TEXT.index("def _cron_due_phases"):]
    due = due[:due.index("\n\n\n")]
    for expected in (
        'if frequent_residue == 0:\n        phases.append("snapshot-status")',
        'elif frequent_residue == 1:\n'
        '        phases.append("snapshot-mirrors")',
        'if slow_residue == 2:\n        phases.append("snapshot-catalog")',
        'if slow_residue == 4:\n'
        '        phases.extend(("snapshot-network", "ledger-backfill"))',
        "schedule_public_catalog_republish",
    ):
        assert expected in due, expected
