import importlib.util
import re
from functools import lru_cache
from pathlib import Path
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2] / "app"
SCENE = world_scene_source()
ENTRY = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")


@lru_cache(maxsize=1)
def _generator():
    path = ROOT.parent / "world" / "tools" / "build_worker_footprint.py"
    spec = importlib.util.spec_from_file_location("build_worker_footprint", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_worker_footprint_budgets_match_current_source_tree():
    generator = _generator()
    data = generator.footprint()
    entry = next(item for item in data["modules"] if item["name"] == "entry.py")
    assert data["measurement"] == "uncompressed deployed source bytes"
    assert entry["bytes"] < (ROOT / "src" / "entry.py").stat().st_size
    assert data["attachedPythonBytes"] > data["estimatedStartupSourceBytes"]
    assert data["onDemandSourceBytes"] > 100_000
    assert data["staticAssetBytes"] > 0
    assert data["staticAssetCount"] < data["staticLimits"]["assetCount"]
    assert (
        data["largestStaticAsset"]["bytes"]
        < data["staticLimits"]["maxAssetBytes"]
    )
    assert (
        data["initialWorldModuleBytes"]
        < data["staticLimits"]["initialWorldModuleBytesSoft"]
    )
    assert data["initialWorldModuleBytes"] < (
        data["staticLimits"]["initialWorldModuleBytesSoft"] // 10
    )
    assert "world/public/world/world.js" in data["initialWorldModules"]
    assert "world/public/world/world-runtime.js" not in data[
        "initialWorldModules"
    ]
    assert "world/world-office.js" not in data["initialWorldModules"]
    assert "world/world-office-meeting.js" not in data["initialWorldModules"]
    assert "world/world-office-tasks.js" not in data["initialWorldModules"]
    assert "world/world-discord-panel.js" not in data["initialWorldModules"]
    assert "qr.js" not in data["initialWorldModules"]
    assert "world/world-scene.js" not in data["initialWorldModules"]
    # Reachable only through the scene builder, so they leave with it.
    assert "world/world-sky.js" not in data["initialWorldModules"]
    assert "world/worker-footprint.js" not in data["initialWorldModules"]
    assert any(
        item["name"] == "repository_imports.py"
        and item["phase"] == "on-demand"
        for item in data["modules"]
    )
    assert any(
        item["name"] == "world_infrastructure.py"
        and item["phase"] == "on-demand"
        for item in data["modules"]
    )
    for name in (
        "account_users.py",
        "calendar_api.py",
        "cron_activity.py",
        "guestbook_runtime.py",
        "marketplace_governance.py",
        "marketplace_runtime.py",
        "microjobs.py",
        "microjobs_runtime.py",
        "provider_runtimes.py",
        "repository_migration.py",
        "instance_launch.py",
        "usage_reporting.py",
        "portable_sessions.py",
        "competitor.py",
    ):
        assert any(
            item["name"] == name and item["phase"] == "on-demand"
            for item in data["modules"]
        ), name
    assert not {
        "collaboration.py",
        "dashboard_bundle.py",
        "dashboard_shell.py",
        "mirror_replica.py",
    }.intersection(item["name"] for item in data["modules"])
    for name in ("app_runtime.py",):
        assert any(
            item["name"] == name and item["phase"] == "startup"
            for item in data["modules"]
        ), name
    assert generator.UNATTACHED_VENDORED_MODULES == {
        "asgi.py",
        "workers/wsgi.py",
    }
    assert sum(item["bytes"] for item in data["components"]) == (
        data["attachedPythonBytes"]
    )
    assert "workerLimits" not in data
    assert not hasattr(generator, "WORKER_LIMITS")
    assert data["staticLimits"] == {
        "assetCount": 20_000,
        "maxAssetBytes": 25 * 1024 * 1024,
        "initialWorldModuleBytesSoft": 2_650_000,
    }
    world_public = ROOT.parent / "world" / "public"
    assert data["staticAssetCount"] == sum(
        path.is_file()
        for path in world_public.rglob("*")
    )


def test_every_lazily_proxied_module_is_budgeted_as_on_demand():
    """A module reached only through a proxy must not be billed to startup.

    The phase split above is a hand-kept list, so a new domain can be held
    behind a _LazyModule proxy and still be counted as source the isolate
    parses during startup validation. attribution.py and bug_reports.py both
    landed that way and spent 61 KB of the budget the Worker never pays,
    failing a deploy that was nowhere near the real ceiling. Derive the
    expectation from the proxies themselves so the next one cannot.
    """
    generator = _generator()
    sources = sorted(
        path
        for root in (ROOT / "src", ROOT.parent / "world" / "src")
        for path in root.glob("*.py")
    )
    local = {path.stem for path in sources}
    proxied = {
        name
        for path in sources
        for name in re.findall(
            r"""_LazyModule\(\s*["']([A-Za-z0-9_]+)["']\s*\)""",
            path.read_text(encoding="utf-8"),
        )
        # Stdlib names go through the same proxy and are not deployed source.
        if name in local
    }
    assert {"attribution", "bug_reports"} <= proxied
    phases = {
        item["name"]: item["phase"]
        for item in generator.footprint()["modules"]
    }
    billed_to_startup = sorted(
        name for name in proxied if phases.get(name + ".py") == "startup"
    )
    assert not billed_to_startup, (
        "add to LAZY_MODULES in build_worker_footprint.py: %s"
        % ", ".join(billed_to_startup)
    )


def test_every_marketplace_worker_module_is_budgeted_as_on_demand():
    """Package Worker modules load only through plugin_data's deferred import."""
    import json

    generator = _generator()
    declared = {
        str(json.loads(path.read_text(encoding="utf-8"))["worker"]["module"]) + ".py"
        for path in (ROOT.parent / "marketplace" / "packages").glob("*/manifest.json")
        if isinstance(json.loads(path.read_text(encoding="utf-8")).get("worker"), dict)
    }
    assert "calories.py" in declared
    assert declared <= generator.LAZY_MODULES


def test_synced_runtime_sdk_is_budgeted_against_deployed_python(
    tmp_path, monkeypatch
):
    """A pywrangler sync must not revive the pre-compaction false positive."""
    generator = _generator()
    raw_startup_bytes = sum(
        path.stat().st_size
        for root in (ROOT / "src", ROOT.parent / "world" / "src")
        for path in root.rglob("*.py")
        if path.name not in generator.LAZY_MODULES
        and path.name not in {"dashboard_bundle.py", "dashboard_shell.py"}
    )
    vendor_bytes = 2_000_000
    vendor = tmp_path / "python_modules" / "workers"
    vendor.mkdir(parents=True)
    (vendor / "runtime.py").write_bytes(b"#" * vendor_bytes)
    monkeypatch.setattr(generator, "VENDORED", vendor.parent)

    data = generator.footprint()

    assert data["vendoredBytes"] == vendor_bytes
    assert data["estimatedStartupSourceBytes"] < raw_startup_bytes + vendor_bytes
    generator.validate_budgets(data)


def test_deploy_report_contains_the_validated_staged_snapshot(tmp_path):
    generator = _generator()
    report = tmp_path / "worker-footprint.json"
    staged = generator.footprint()

    assert generator.write_deploy_report(report, staged) == staged
    payload = __import__("json").loads(report.read_text(encoding="utf-8"))

    assert payload["ok"] is True
    assert payload["estimatedStartupSourceBytes"] == (
        staged["estimatedStartupSourceBytes"]
    )
    assert "workerLimits" not in payload
    assert payload["initialWorldModuleBytes"] < (
        payload["staticLimits"]["initialWorldModuleBytesSoft"]
    )


def test_provider_import_module_is_loaded_only_when_its_routes_need_it():
    eager_imports = ENTRY[: ENTRY.index("def _repository_import_module():")]
    assert "import repository_imports" not in eager_imports
    assert "def _repository_import_module():" in ENTRY
    assert "repository_import = _repository_import_module()" in ENTRY
    assert "import world_infrastructure" not in eager_imports
    assert "def _world_infrastructure_module():" in ENTRY


def test_optional_python_route_modules_are_deferred_from_global_scope():
    """Keep Cloudflare's Python startup validation below its memory ceiling."""
    eager_imports = ENTRY[: ENTRY.index("def _repository_import_module():")]
    deferred = (
        "activitypub",
        "activitypub_threads",
        "badges",
        "blog_feed",
        "catalog",
        "chat_channels_api",
        "chat_direct_messages_api",
        "chat_reads_api",
        "community_ads_api",
        "contributions",
        "edge_routing",
        "events",
        "fediverse_digest",
        "fediverse_mentions_api",
        "git_http",
        "notes",
        "og_card",
        "organization_discord",
        "organization_succession_api",
        "releases",
        "reward_policy",
        "security_controls",
        "security_scan_ingest",
        "solana",
        "static_routes",
        "urls",
        "schema",
        "world",
        "world_build_board",
        "world_community_api",
        "world_element_store",
        "world_events_api",
        "world_link_kiosk",
        "world_office_tasks",
        "world_satellites",
        "world_social_feeds",
        "world_visitors",
    )
    for module in deferred:
        assert f'_LazyModule("{module}")' in eager_imports
        assert f"import {module}" not in eager_imports
    assert '_LazyModule("admin_console")' in ENTRY
    assert "import admin_console" not in ENTRY


def test_heavy_stdlib_modules_are_deferred_from_global_scope():
    """Pyodide must not allocate optional stdlib modules during validation."""
    eager_imports = ENTRY[: ENTRY.index("MAX_ROOM_NAME =")]
    for module in (
        "base64", "gzip", "io", "ipaddress", "math", "struct", "time",
        "asyncio", "hashlib", "hmac", "json", "traceback", "urllib.parse",
    ):
        assert f'_LazyModule("{module}")' in eager_imports
    for statement in (
        "import asyncio", "import base64", "import gzip", "import hashlib",
        "import hmac", "import io", "import ipaddress", "import json",
        "import math", "import struct", "import time", "import traceback",
        "from urllib.parse import",
    ):
        assert statement not in eager_imports


def test_office_runtime_is_outside_the_initial_world_module_graph():
    world = (ROOT.parent / "world" / "public" / "world" / "world.js").read_text(encoding="utf-8")
    assert 'import("./world-office.js")' in world
    assert 'import("./world-office-meeting.js")' in world
    assert 'import("./world-office-tasks.js")' in world
    assert 'from "./world-office.js"' not in world
    assert 'from "./world-office-meeting.js"' not in world
    assert 'from "./world-office-tasks.js"' not in world
    assert 'from "./world-office.js"' not in SCENE


def test_scene_builder_streams_beside_three_instead_of_blocking_the_shell():
    """The renderer cannot run before three.js, so it must not block first paint."""
    world = (ROOT.parent / "world" / "public" / "world" / "world.js").read_text(encoding="utf-8")
    assert 'const WORLD_SCENE_MODULE = import("./world-scene.js");' in world
    assert 'from "./world-scene.js"' not in world
    # Started on the same tick as three.js, and awaited together with it.
    assert world.index("const WORLD_SCENE_MODULE") > world.index(
        "const THREE_MODULE = import(THREE_MODULE_URL);"
    )
    assert "Promise.all([THREE_MODULE, WORLD_SCENE_MODULE])" in world
    assert "this.world = await scene.createWorldScene({" in world
    # The account badge paints behind the loading curtain, so its painter stays
    # in the shell's own graph rather than dragging the scene back in with it.
    assert (
        'import { proceduralAvatarFaceDataURL } from "./world-avatar-face.js";'
        in world
    )


def test_wallet_qr_encoder_is_outside_the_initial_world_module_graph():
    world = (ROOT.parent / "world" / "public" / "world" / "world.js").read_text(encoding="utf-8")
    assert 'import("../../qr.js")' in SCENE
    assert 'import "../../qr.js"' not in SCENE
    assert "QR_MODULE_READY" in world


def test_idle_deploy_watch_does_not_burn_the_free_request_budget():
    world = (ROOT.parent / "world" / "public" / "world" / "world.js").read_text(encoding="utf-8")
    assert "const WORLD_DEPLOY_STATUS_IDLE_MS = 60 * 1000;" in world
    assert 'state === "deploying"' in world
    assert "? WORLD_DEPLOY_STATUS_POLL_MS" in world
    assert ": WORLD_DEPLOY_STATUS_IDLE_MS" in world
    assert "this.deployStatusTimer = window.setTimeout(" in world
