#!/usr/bin/env python3
"""Validate and report the deployed Worker/source footprint.

A deploy may emit the validated snapshot into its ignored static-asset stage
so operators can inspect the exact measurements that accompanied it.
"""

import importlib.util
import json
import re
import sys
from functools import lru_cache
from pathlib import Path


WORLD = Path(__file__).resolve().parents[1]
REPOSITORY = WORLD.parent
APP = REPOSITORY / "app"
WORLD_PUBLIC = WORLD / "public"
# App, WWW, and World Static Assets now deploy as separate Workers. Only the
# World-owned tree consumes this Worker's asset quotas.
PUBLIC_ROOTS = (WORLD_PUBLIC,)
VENDORED = APP / "python_modules"
WORKER_BUILD_TOOL = APP / "tools" / "build_worker_python.py"
STAGED_PYTHON = APP / "src_build"
UNATTACHED_VENDORED_MODULES = {"asgi.py", "workers/wsgi.py"}
# Python Workers parse and execute global scope during deployment validation.
# Keep optional route/build domains out of that path. entry.py reaches them
# through a _LazyModule proxy, a request-local import, or another deferred
# module only after the route which needs them runs.
LAZY_MODULES = {
    # Signed trust-list verification ships for the desktop, mobile and
    # conformance clients; no Worker module imports it, so startup validation
    # never parses it. Billing its 25 KB to startup failed every deploy after
    # issue #514 (runs 9493-9517, 2026-09-14).
    "trust_lists.py",
    "account_status.py",
    "account_activity.py",
    "account_users.py",
    "admin_console.py",
    # The desktop Database tab's route; entry.py reaches it only through the
    # _admin_database_handler proxy.
    "admin_database.py",
    # Shared chrome for every admin page, reached through entry.py's
    # _admin_shell proxy by admin routes and /admin/theme only.
    "admin_shell.py",
    "agent_guestbook.py",
    "agent_notes.py",
    "api_metrics.py",
    "activitypub.py",
    "activitypub_threads.py",
    # entry.py imports apns only inside _apns_module(), on the account-push
    # path, so its source is never parsed during startup validation.
    "apns.py",
    # Acquisition capture and its reports: entry.py holds it behind a
    # _LazyModule proxy that resolves on the first served HTML navigation,
    # so startup validation never parses this domain.
    "attribution.py",
    "badges.py",
    # Ping settings (#347): entry.py exports its helpers through the
    # _ping_settings_module proxy, first touched by a ping or a settings read.
    "ping_settings.py",
    # Mirror payload annotations: entry.py exports these through _LazyModule
    # proxies that resolve on the first mirrors payload build.
    "availability_proof.py",
    "sync_status.py",
    # Crisis preservation plans: entry.py's crisis_preservation proxy resolves
    # on the first GET /api/preservation/crisis-plan.
    "crisis_preservation.py",
    # Imported only by tools/forkmesh_availability_contract.py, the contract
    # fixture generator, and their tests; no Worker code path imports it.
    "availability_contract.py",
    # entry.py holds the role matrix behind its _org_rbac proxy; the only
    # direct importers (organization_discord, organization_succession) are
    # themselves deferred.
    "organization_rbac.py",
    # Imported inside a function of repository_imports.py, itself deferred.
    "repository_migration_report.py",
    # Outbound repository webhooks: entry.py holds them behind a _LazyModule
    # proxy that resolves on the owner's management route, on the
    # webhook-outbox cron phase, or on the first event that queues a delivery.
    "repository_webhooks.py",
    "repository_provider_mirror.py",
    # Mirror Actions control and summary v2: entry.py's _repository_actions
    # proxy resolves on the first POST .../actions/control or the first
    # version 2 summary an owner's Actions tab reads.
    "repository_actions.py",
    # The org-bot Remote MCP endpoint: entry.py's remote_mcp_handler proxy
    # resolves on the first /mcp request. Moved out of entry.py when the
    # repository webhooks merge left startup 1,132 bytes over budget
    # (deploy run 10413, 2026-09-17).
    "remote_mcp.py",
    # Desktop timer pings: entry.py's timer_ping_handler proxy resolves on the
    # first POST /api/notifications/timer.
    "timer_pings.py",
    "desktop_pings.py",
    "mesh_push.py",
    "operator_alerts.py",
    # The iOS UDID profile service: entry.py's ios_udid_handler proxy resolves
    # on the first /api/ios-udid request.
    "ios_udid.py",
    # Imported only by tools/audit_private_surfaces.py and its tests; no
    # Worker code path imports it.
    "private_surface_audit.py",
    # Scoped trust-list resolver: imported only by tools/forkmesh_trust_lists.py,
    # the conformance vector generator, and their tests; no Worker code path
    # imports it.
    "trust_lists.py",
    "blog_feed.py",
    # Reached only through entry.py's bug_reports_handler and
    # bug_reports_inbox_handler proxies, on the /api/bug-reports paths.
    "bug_reports.py",
    # The console's Support sections, reached only through entry.py's
    # admin_support_handler proxy on ?view=bugs and ?view=feedback.
    "admin_support.py",
    "ecosystem.py",
    "calendar_api.py",
    "forkbot.py",
    "fediverse_routes.py",
    "guestbook_runtime.py",
    # Hackathon policy: pure rules imported only by hackathon_runtime, which
    # entry.py reaches through a _LazyModule proxy, so startup validation
    # never parses either.
    "hackathon.py",
    # Web-created empty repositories and their plain-git push
    # credential: entry.py holds hosted_git behind a _LazyModule proxy
    # that resolves on the hosted-create route or on a receive-pack
    # request carrying a Basic credential.
    "hosted_git.py",
    # Core-hosted repository replication and quotas (docs/HOSTED_REPOS.md);
    # imported only by hosted_git.
    "hosted_repos.py",
    # Campaign routes, hooks, personal pages and the admin page: entry.py holds
    # it behind a _LazyModule proxy resolved on the first hackathon route or
    # hook, so startup validation never parses it.
    "hackathon_runtime.py",
    "challenges.py",
    "challenges_runtime.py",
    "workflows.py",
    "workflow_actions.py",
    "workflow_script.py",
    "workflow_integrations.py",
    "workflows_runtime.py",
    "workflow_files.py",
    "workflow_formats.py",
    "workflow_templates.py",
    # The investor deck a contact opens and its view beacon: entry.py reaches
    # it through a _LazyModule proxy on the first /pitch-deck route.
    "pitch_deck_runtime.py",
    "catalog.py",
    "catalog_routes.py",
    # Imported inside catalog_routes.repository_index_search_handler, itself
    # deferred; stable_repository is imported only by repository_index.
    "repository_index.py",
    "stable_repository.py",
    "channel_reminders.py",
    "chat_format.py",
    "chat_channels_api.py",
    "chat_direct_messages_api.py",
    "chat_direct_media_api.py",
    "chat_reads_api.py",
    "direct_reminders.py",
    "community_ads_api.py",
    "community_ads.py",
    "contributions.py",
    "dashboard_bundle.py",
    "dashboard_shell.py",
    "discord_bugs.py",
    "discord_rate.py",
    "edge_routing.py",
    "events.py",
    "fediverse_digest.py",
    "fediverse_mentions_api.py",
    "git_http.py",
    "ledger.py",
    "microjobs.py",
    "microjobs_runtime.py",
    "marketplace_governance.py",
    "marketplace_runtime.py",
    "notes.py",
    "node_events_runtime.py",
    # entry.py reaches the node terminal relay through a _LazyModule proxy on
    # the terminal route only, so startup validation never parses it.
    "node_terminal.py",
    "og_card.py",
    "organization_discord.py",
    "organization_succession.py",
    "organization_succession_api.py",
    # Attached for the Patreon route family, but never imported at Worker
    # global scope; its provider/runtime bindings are resolved on demand.
    "patreon_handler.py",
    # Reached only through patreon_handler.py, which is itself deferred.
    "patreon.py",
    "password_reset.py",
    # Marketplace data/API modules are reached through entry.py's plugin_data
    # proxy only after a plugin route, lifecycle request, or webhook arrives.
    # The generated registry is imported by that same deferred module (and by
    # static_routes.py, which is itself deferred).
    "plugin_data.py",
    "plugin_registry.py",
    # Self-serve plan usage/trial policy, reached through entry.py's
    # plan_services proxy on the /api/plans/* routes only.
    "plan_services.py",
    # ForkMesh Ads policy, reached through entry.py's ad_network proxy on the
    # /api/ads/* routes and the daily cleanup only.
    "ad_network.py",
    "provider_runtimes.py",
    # The token codec is imported through entry.py's _LazyModule proxy only
    # when a portable account session is issued.
    "portable_sessions.py",
    # The founder-only product dashboard, reached through entry.py's
    # _LazyModule proxy on the first /product-analysis request.
    "product_analysis.py",
    "pull_badge.py",
    "public_snapshots.py",
    "request_bodies.py",
    "releases.py",
    "mirrors.py",
    "microjobs.py",
    "repository_imports.py",
    "repository_migration.py",
    "route_handlers.py",
    "reward_policy.py",
    "security_controls.py",
    "security_scan_ingest.py",
    "solana.py",
    "ssh_keys.py",
    "instance_launch.py",
    "schema.py",
    "static_routes.py",
    "status_monitoring.py",
    "usage_reporting.py",
    # Reached through entry.py's _d1_read_guard proxy on the first heavy
    # D1 read or D1 tab request.
    "d1_read_guard.py",
    "urls.py",
    "world.py",
    "world_build_board.py",
    "world_community.py",
    "world_community_api.py",
    "world_element_store.py",
    "world_events_api.py",
    "world_events.py",
    "world_infrastructure.py",
    "world_link_kiosk.py",
    "world_office_tasks.py",
    "world_office_routes.py",
    "world_task_email.py",
    "org_office_routes.py",
    "org_office_chat.py",
    "world_qa.py",
    "world_routes.py",
    "world_satellites.py",
    "world_social_feeds.py",
    "world_visitors.py",
    # Scheduled activity reporting remains outside ordinary request startup.
    "cron_activity.py",
    # Split out to bring the Python startup source back under budget after
    # it crept past 1,730,000 bytes (deploy run 10969, 2026-09-18). Each is
    # reached through entry.py's proxy on first route/cron use.
    "federation.py",
    "ssh_gateway_routes.py",
    "repo_sync.py",
    "agent_routes.py",
    "desktop_commands.py",
    "apps_runtime.py",
    "core_push.py",
    "mail_budget.py",
    "mail_activity.py",
    "mail_log.py",
    "mail_tracking.py",
    "marketplace_reviews.py",
    "node_control.py",
    "smtp_mail.py",
    "mail_providers.py",
    "instance_features.py",
    "instance_pages.py",
    "instance_theme.py",
    "remote_forks.py",
    "turnstile.py",
    "apps.py",
    "apps_api.py",
    "apps_commands.py",
    "apps_core.py",
    "apps_directory.py",
    "apps_events.py",
    "deploy_approvals.py",
    "runner_actions.py",
    "runner_leases.py",
    "runner_logins.py",
    "runner_setup.py",
    "runner_ssh_setup.py",
    "agent_branches.py",
}


def _marketplace_worker_modules():
    """Every package Worker module a Marketplace manifest declares.

    The build stages them next to entry.py, but only plugin_data's deferred
    importlib dispatch loads them, after a package route, lifecycle request,
    webhook, or reminder cron arrives. Deriving the names keeps a new package
    from being billed to startup: calories.py was missing from a hand-kept
    list and failed every deploy after it merged (run 10260, 2026-09-17).
    """
    names = set()
    for manifest in sorted(REPOSITORY.glob("marketplace/packages/*/manifest.json")):
        worker = json.loads(manifest.read_text(encoding="utf-8")).get("worker")
        module = str((worker or {}).get("module") or "") if isinstance(worker, dict) else ""
        if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", module):
            names.add(module + ".py")
    return names


LAZY_MODULES |= _marketplace_worker_modules()
STATIC_LIMITS = {
    "assetCount": 20_000,
    "maxAssetBytes": 25 * 1024 * 1024,
    # This is an intentionally stricter project budget, not a Cloudflare cap.
    # It leaves headroom for future districts without letting the first visit
    # silently inherit every optional feature module.
    "initialWorldModuleBytesSoft": 2_650_000,
}
STATIC_IMPORT_FROM_RE = re.compile(
    r"""\bfrom\s+["'](?P<path>\.{1,2}/[^"']+)["']"""
)
STATIC_IMPORT_SIDE_EFFECT_RE = re.compile(
    r"""^\s*import\s+["'](?P<path>\.{1,2}/[^"']+)["']""",
    re.MULTILINE,
)


def _component_for(name):
    """Group source modules into stable architectural concepts."""
    if name in {"entry.py", "worker_runtime.py"}:
        return "Worker routing + runtime"
    if name.startswith("world"):
        return "World + Office"
    if name.startswith(("activitypub", "fediverse")):
        return "Fediverse"
    if name.startswith(("organization", "chat_")):
        return "Organizations + chat"
    if name in {
        "catalog.py",
        "edge_routing.py",
        "git_http.py",
        "mirrors.py",
        "pull_badge.py",
        "releases.py",
        "repository_imports.py",
    }:
        return "Repositories + mirrors"
    if name in {
        "badges.py",
        "reward_policy.py",
        "security_controls.py",
        "security_scan_ingest.py",
        "solana.py",
        "ssh_keys.py",
    }:
        return "Identity + security"
    if name.startswith("community_") or name in {
        "blog_feed.py",
    "forkbot.py",
    "fediverse_routes.py",
        "challenges.py",
        "challenges_runtime.py",
        "contributions.py",
        "og_card.py",
    }:
        return "Community + publishing"
    return "Platform + build"


def _files(root, pattern="*"):
    return [path for path in root.rglob(pattern) if path.is_file()]


@lru_cache(maxsize=2)
def _deployed_python_files(from_stage=False):
    """Return the compact Python payload the App Worker actually attaches."""
    if from_stage:
        entrypoint = STAGED_PYTHON / "entry.py"
        if not entrypoint.is_file():
            raise RuntimeError(
                "staged App Worker is missing; run "
                "app/tools/build_worker_python.py first"
            )
        return [
            (path.relative_to(STAGED_PYTHON), path.read_bytes())
            for path in sorted(STAGED_PYTHON.rglob("*.py"))
            if path.is_file() and "__pycache__" not in path.parts
        ]
    spec = importlib.util.spec_from_file_location(
        "forkmesh_build_worker_python", WORKER_BUILD_TOOL
    )
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load Worker build tool: {WORKER_BUILD_TOOL}")
    builder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(builder)
    return [
        (relative, payload)
        for relative, _, payload in builder.worker_python_payloads()
        if relative.suffix == ".py"
    ]


def _static_javascript_graph(roots):
    """Return local modules fetched before any dynamic import boundary."""
    pending = [path.resolve() for path in roots]
    found = {}
    while pending:
        path = pending.pop()
        if path in found or not path.is_file():
            continue
        source = path.read_text(encoding="utf-8")
        found[path] = path.stat().st_size
        imports = {
            match.group("path")
            for pattern in (STATIC_IMPORT_FROM_RE, STATIC_IMPORT_SIDE_EFFECT_RE)
            for match in pattern.finditer(source)
        }
        for imported in imports:
            target = (path.parent / imported).resolve()
            if target.suffix == "":
                target = target.with_suffix(".js")
            if any(target.is_relative_to(root.resolve()) for root in PUBLIC_ROOTS):
                pending.append(target)
    return found


def footprint(from_stage=False):
    source_files = _deployed_python_files(from_stage)
    vendored_files = [
        path for path in _files(VENDORED)
        if path.relative_to(VENDORED).as_posix()
        not in UNATTACHED_VENDORED_MODULES
    ]
    public_files = [
        path
        for root in PUBLIC_ROOTS
        for path in _files(root)
    ]
    initial_world_graph = _static_javascript_graph(
        [
            # stage_world deploys this first-paint shell at /world/world.js and
            # the authored implementation as dynamically imported
            # /world/world-runtime.js.
            WORLD_PUBLIC / "world" / "world-loader.js",
            WORLD_PUBLIC / "world" / "world-discord.js",
        ]
    )
    largest_asset = max(public_files, key=lambda path: path.stat().st_size)
    modules = [
        {
            "name": relative.name,
            "bytes": len(payload),
            "phase": (
                "on-demand" if relative.name in LAZY_MODULES else "startup"
            ),
        }
        for relative, payload in source_files
    ]
    modules.sort(key=lambda item: (-item["bytes"], item["name"]))
    source_bytes = sum(item["bytes"] for item in modules)
    lazy_bytes = sum(
        item["bytes"] for item in modules if item["phase"] == "on-demand"
    )
    vendored_bytes = sum(path.stat().st_size for path in vendored_files)
    component_totals = {}
    for item in modules:
        component = _component_for(item["name"])
        component_totals[component] = (
            component_totals.get(component, 0) + item["bytes"]
        )
    components = [
        {"name": name, "bytes": size}
        for name, size in component_totals.items()
    ]
    components.append({"name": "Vendored Python runtime", "bytes": vendored_bytes})
    components.sort(key=lambda item: (-item["bytes"], item["name"]))
    return {
        "measurement": "uncompressed deployed source bytes",
        "attachedPythonBytes": source_bytes + vendored_bytes,
        "estimatedStartupSourceBytes": source_bytes - lazy_bytes + vendored_bytes,
        "onDemandSourceBytes": lazy_bytes,
        "vendoredBytes": vendored_bytes,
        "moduleCount": len(modules) + len(vendored_files),
        "staticAssetBytes": sum(path.stat().st_size for path in public_files),
        "staticAssetCount": len(public_files),
        "largestStaticAsset": {
            "name": largest_asset.relative_to(REPOSITORY).as_posix(),
            "bytes": largest_asset.stat().st_size,
        },
        "initialWorldModuleBytes": sum(initial_world_graph.values()),
        "initialWorldModuleCount": len(initial_world_graph),
        "initialWorldModules": sorted(
            (
                "world/public/world/world.js"
                if path == (WORLD_PUBLIC / "world" / "world-loader.js").resolve()
                else path.relative_to(REPOSITORY).as_posix()
            )
            for path in initial_world_graph
        ),
        # Keep the complete generated inventory in the asset; the Three.js
        # chart chooses its largest visible rows, while tests and future views
        # can still inspect every clearly named Python module.
        "modules": modules,
        "components": components,
        "staticLimits": STATIC_LIMITS,
        "note": (
            "Deployed source bytes are a reproducible footprint measurement, "
            "not heap."
        ),
    }


def validate_budgets(data=None, from_stage=False):
    """Fail builds before a new asset crosses a Free-plan growth boundary."""
    snapshot = data or footprint(from_stage)
    errors = []
    if snapshot["staticAssetCount"] > STATIC_LIMITS["assetCount"]:
        errors.append(
            "static asset count "
            f"{snapshot['staticAssetCount']:,} exceeds "
            f"{STATIC_LIMITS['assetCount']:,}"
        )
    largest = snapshot["largestStaticAsset"]
    if largest["bytes"] > STATIC_LIMITS["maxAssetBytes"]:
        errors.append(
            f"{largest['name']} is {largest['bytes']:,} bytes; "
            f"the per-asset limit is {STATIC_LIMITS['maxAssetBytes']:,}"
        )
    if (
        snapshot["initialWorldModuleBytes"]
        > STATIC_LIMITS["initialWorldModuleBytesSoft"]
    ):
        errors.append(
            "initial World module graph is "
            f"{snapshot['initialWorldModuleBytes']:,} bytes; "
            "the project soft budget is "
            f"{STATIC_LIMITS['initialWorldModuleBytesSoft']:,}"
        )
    if errors:
        raise RuntimeError("Free-plan growth budget failed: " + "; ".join(errors))
    return snapshot


def write_deploy_report(path, data=None, from_stage=False):
    """Write the validated snapshot into an ignored deploy-stage asset."""
    snapshot = validate_budgets(data, from_stage)
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(
        json.dumps({"ok": True, **snapshot}, separators=(",", ":")) + "\n",
        encoding="utf-8",
    )
    return snapshot


if __name__ == "__main__":
    arguments = sys.argv[1:]
    from_stage = False
    report_path = None
    if arguments[:1] == ["--staged"]:
        from_stage = True
        arguments = arguments[1:]
    if arguments[:1] == ["--report"] and len(arguments) == 2:
        report_path = Path(arguments[1])
        arguments = []
    if arguments:
        raise SystemExit(
            "usage: build_worker_footprint.py [--staged] [--report PATH]"
        )
    if report_path is not None:
        write_deploy_report(report_path, from_stage=from_stage)
        print(
            "Worker footprint budgets are valid; wrote deploy report to "
            f"{report_path}."
        )
    else:
        validate_budgets(from_stage=from_stage)
        print("Worker footprint budgets are valid.")
