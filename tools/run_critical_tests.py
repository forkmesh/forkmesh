#!/usr/bin/env python3
"""Run the bounded critical suites used by local development and CI."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TEST_BUDGET_SECONDS = 55

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
}


def command_for(suite: str, build_dir: Path) -> list[str]:
    if suite in PYTEST_SUITES:
        return [sys.executable, "-m", "pytest", "-q", *PYTEST_SUITES[suite]]

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
    parser.add_argument("suite", choices=(*PYTEST_SUITES, "qt"))
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=ROOT / "desktop" / "build",
        help="configured Qt build directory (qt suite only)",
    )
    args = parser.parse_args()

    command = command_for(args.suite, args.build_dir.resolve())
    started = time.monotonic()
    print(
        f"[critical-tests] {args.suite}: {TEST_BUDGET_SECONDS}s budget",
        flush=True,
    )
    try:
        result = subprocess.run(
            command,
            cwd=ROOT,
            check=False,
            timeout=TEST_BUDGET_SECONDS,
        )
    except subprocess.TimeoutExpired:
        print(
            f"[critical-tests] {args.suite} exceeded "
            f"{TEST_BUDGET_SECONDS}s",
            file=sys.stderr,
        )
        return 124

    elapsed = time.monotonic() - started
    print(f"[critical-tests] {args.suite}: {elapsed:.2f}s", flush=True)
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
