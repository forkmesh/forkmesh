#!/usr/bin/env python3
"""Run the bounded critical suites used by local development and CI."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TEST_BUDGET_SECONDS = 55
SUITE_BUDGET_SECONDS = {"security": 300, "go": 900}

PYTEST_SUITES = {
    "worker": (
        "app/tests/test_entry_call_arity.py",
        "app/tests/test_bounded_json_requests.py",
        "app/tests/test_browser_security_headers.py",
        "app/tests/test_crypto.py",
        "app/tests/test_edge_routing.py",
        "app/tests/test_static_route_canonicalization.py",
        "app/tests/test_offline_ci_workflow.py",
        "app/tests/test_discord_accounts.py",
        "app/tests/test_codemagic_native_build.py",
    ),
    "world": (
        "world/tests/test_world_backend.py::test_context_returns_only_country_and_shared_clock_fields",
        "world/tests/test_world_backend.py::test_presence_message_discards_sensitive_and_unknown_fields",
        "world/tests/test_world_backend.py::test_movement_is_bounded_quantized_and_profile_fields_cannot_change",
        "world/tests/test_world_backend.py::test_world_durable_object_is_transient_and_hibernating",
        "world/tests/test_world_backend.py::test_browser_world_socket_is_same_origin_only",
        "world/tests/test_world_backend.py::test_world_static_route_is_reserved_and_asset_first",
        "world/tests/test_world_frontend.py::test_world_first_paint_has_one_loader_and_a_load_error_watchdog",
        "world/tests/test_world_frontend.py::test_world_entry_modules_have_valid_ecmascript_module_syntax",
        "world/tests/test_world_frontend.py::test_presence_client_uses_only_coarse_ephemeral_world_protocol",
        "world/tests/test_world_frontend.py::test_world_has_responsive_and_reduced_motion_fallbacks",
    ),
    "security": (
        "app/tests/test_users_nodes_claim_link.py",
        "app/tests/test_account_totp_gate.py",
        "app/tests/test_login_key_binding.py",
        "app/tests/test_no_unguarded_destructive_deletes.py",
        "app/tests/test_forkbot.py::test_forkbot_does_not_rekey_when_the_owner_is_a_plain_node",
        "app/tests/test_forkbot.py::test_forkbot_agent_requests_ignore_the_claimed_chat_sender",
        "app/tests/test_forkbot.py::test_forkbot_starts_agent_on_most_recent_issue_for_owner",
        "app/tests/test_forkbot.py::test_forkbot_starts_agent_on_named_issue_for_admin",
        "app/tests/test_forkbot.py::test_forkbot_resolves_only_allowlisted_model_picks",
        "app/tests/test_forkbot.py::test_ai_workspace_name_rejects_unsigned_and_falls_back_on_unusable_results",
        "app/tests/test_forkbot.py::test_ai_ask_refuses_unsigned_stale_and_unverified_callers",
        "app/tests/test_forkbot.py::test_ai_agent_caps_the_output_tokens_a_client_may_request",
        "app/tests/test_forkbot.py::test_forkbot_denies_agent_start_for_non_owner",
        "app/tests/test_forkbot.py::test_forkbot_rejects_agent_start_on_unknown_issue",
    ),
}


def go_binary() -> str:
    found = shutil.which("go")
    if found:
        return found
    fallback = Path("/usr/local/go/bin/go")
    if fallback.is_file():
        return str(fallback)
    raise RuntimeError("go is required for the Go critical suite")


def go_environment() -> dict[str, str]:
    env = dict(os.environ)
    for leaked in ("FORKMESH_HOME", "FORKMESH_PROJECTS_DIR"):
        env.pop(leaked, None)
    env.update(GOTOOLCHAIN="local", GOPROXY="off", GOFLAGS="-mod=readonly", CGO_ENABLED="0")
    env.setdefault("GOCACHE", str(Path(tempfile.gettempdir()) / "forkmesh-go-cache"))
    env.setdefault("GOPATH", str(Path(tempfile.gettempdir()) / "forkmesh-go-path"))
    return env


def command_for(suite: str, build_dir: Path) -> list[str]:
    if suite in PYTEST_SUITES:
        return [sys.executable, "-m", "pytest", "-q", *PYTEST_SUITES[suite]]
    if suite == "go":
        return [go_binary(), "test", "-C", "server", "./..."]

    ctest = shutil.which("ctest")
    if ctest is None:
        raise RuntimeError("ctest is required for the Qt critical suite")
    return [
        ctest,
        "--test-dir",
        str(build_dir),
        "--output-on-failure",
        "--label-regex",
        "^critical$",
        "--timeout",
        "20",
        "--parallel",
        "3",
    ]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("suite", choices=(*PYTEST_SUITES, "go", "qt"))
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=ROOT / "desktop" / "build",
        help="configured Qt build directory (qt suite only)",
    )
    args = parser.parse_args()

    try:
        command = command_for(args.suite, args.build_dir.resolve())
    except RuntimeError as exc:
        print(f"[critical-tests] {args.suite}: {exc}", file=sys.stderr)
        return 2
    budget = SUITE_BUDGET_SECONDS.get(args.suite, TEST_BUDGET_SECONDS)
    started = time.monotonic()
    print(
        f"[critical-tests] {args.suite}: {budget}s budget",
        flush=True,
    )
    try:
        result = subprocess.run(
            command,
            cwd=ROOT,
            check=False,
            timeout=budget,
            env=go_environment() if args.suite == "go" else None,
        )
    except subprocess.TimeoutExpired:
        print(
            f"[critical-tests] {args.suite} exceeded "
            f"{budget}s",
            file=sys.stderr,
        )
        return 124

    elapsed = time.monotonic() - started
    print(f"[critical-tests] {args.suite}: {elapsed:.2f}s", flush=True)
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
