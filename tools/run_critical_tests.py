#!/usr/bin/env python3
"""Run the bounded critical suites used by local development and CI."""

from __future__ import annotations

import argparse
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
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
        "app/tests/host/test_repo_fastpath.py::test_raw_active_content_is_a_sandboxed_download",
        "app/tests/host/test_repo_fastpath.py::test_every_fast_path_answer_carries_the_sandbox_policy",
        "app/tests/host/test_repo_fastpath.py::test_memory_hit_after_the_repository_went_private_falls_through",
        "app/tests/test_mirror_gateway.py::test_raw_active_media_types_are_attachments",
        "app/tests/test_mirror_gateway.py::test_reads_refuse_commit_reachable_only_from_hidden_refs",
        "app/tests/test_mobile_node_devices.py::test_revoking_the_primary_desktop_takes_its_key_off_the_account",
        "app/tests/test_mobile_node_devices.py::test_a_primary_key_with_a_revoked_device_row_never_signs",
        "app/tests/test_admin_totp_required.py::test_signed_admin_requests_refuse_a_revoked_device_key",
        "app/tests/test_users_nodes_claim_link.py::test_claim_password_checks_spend_the_login_budget_and_honour_the_lock",
        "app/tests/test_users_nodes_claim_link.py::test_claim_endpoints_need_the_code_of_a_totp_account",
        "app/tests/test_mail_budget.py::test_package_smtp_mail_is_capped_per_package_and_organization",
        "app/tests/test_patreon_runtime.py::test_webhook_email_fallback_ignores_an_unconfirmed_email",
        "app/tests/test_microjobs.py::test_a_fork_copy_of_a_paid_pull_is_the_same_claim",
        "app/tests/test_microjobs.py::test_the_daily_caps_hold_even_when_the_precheck_is_raced",
        "world/tests/test_world_office_tasks.py::test_guests_get_no_tasks_and_auditors_cannot_write",
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
    cpus = env.get("FORKMESH_ACTIONS_CPUS", "")
    if cpus.isdigit() and int(cpus) > 0:
        env.setdefault("GOMAXPROCS", cpus)
    return env


GO_RESULT = re.compile(r"^(ok|FAIL|\?)\s+(\S+)")
GO_TIMEOUT_SIGNS = ("i/o timeout", "context deadline exceeded", "Client.Timeout exceeded", "timed out")
GO_RETRY_LIMIT = 3
GO_LOAD_SENSITIVE_TESTS = frozenset({"TestThreePeopleHoldACallThroughImpairedLinks"})


def go_timeout_failures(output: str) -> dict[str, list[str]] | None:
    failures: dict[str, list[str]] = {}
    reasons: dict[str, list[str]] = {}
    current = ""
    panicked = False
    for line in output.splitlines():
        if line.startswith("--- FAIL: "):
            current = line.split()[2]
            reasons[current] = []
            continue
        if current and line.startswith("    "):
            reasons[current].append(line)
            continue
        current = ""
        if line.startswith("panic: "):
            panicked = True
        result = GO_RESULT.match(line)
        if not result:
            continue
        if result.group(1) == "FAIL":
            timed_out = all(name in GO_LOAD_SENSITIVE_TESTS
                            or any(sign in text for text in lines for sign in GO_TIMEOUT_SIGNS)
                            for name, lines in reasons.items())
            if panicked or not reasons or not timed_out or "[" in line:
                return None
            failures[result.group(2)] = list(reasons)
        reasons = {}
        panicked = False
    if reasons or sum(map(len, failures.values())) > GO_RETRY_LIMIT:
        return None
    return failures


def go_retry_command(package: str, tests: list[str]) -> list[str]:
    pattern = "^(" + "|".join(re.escape(name) for name in tests) + ")$"
    return [go_binary(), "test", "-C", "server", "-count=1", "-run", pattern, package]


def run_streaming(command: list[str], env: dict[str, str] | None, timeout: float) -> tuple[int, str]:
    process = subprocess.Popen(command, cwd=ROOT, env=env, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, errors="replace",
                               start_new_session=True)
    expired = threading.Event()

    def stop() -> None:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass

    def expire() -> None:
        expired.set()
        stop()

    timer = threading.Timer(max(timeout, 0.0), expire)
    timer.start()
    lines = []
    try:
        for line in process.stdout:
            sys.stdout.write(line)
            sys.stdout.flush()
            lines.append(line)
        process.wait()
    finally:
        timer.cancel()
        if process.poll() is None:
            stop()
            process.wait()
    if expired.is_set():
        raise subprocess.TimeoutExpired(command, timeout)
    return process.returncode, "".join(lines)


def run_go_suite(command: list[str], budget: float) -> int:
    deadline = time.monotonic() + budget
    env = go_environment()
    returncode, output = run_streaming(command, env, budget)
    if returncode == 0:
        return 0
    failures = go_timeout_failures(output)
    if not failures:
        return returncode
    for package, tests in failures.items():
        print(f"[critical-tests] go: retrying {package} {' '.join(tests)} once: "
              "every failure was a timeout or a load-sensitive call test", flush=True)
        returncode, _ = run_streaming(go_retry_command(package, tests), env,
                                      deadline - time.monotonic())
        if returncode != 0:
            return returncode
    print("[critical-tests] go: passed on retry; the first failures were timeouts "
          "under load (docs/server/README.md)", flush=True)
    return 0


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
        if args.suite == "go":
            returncode = run_go_suite(command, budget)
        else:
            returncode = subprocess.run(
                command,
                cwd=ROOT,
                check=False,
                timeout=budget,
            ).returncode
    except subprocess.TimeoutExpired:
        print(
            f"[critical-tests] {args.suite} exceeded "
            f"{budget}s",
            file=sys.stderr,
        )
        return 124

    elapsed = time.monotonic() - started
    print(f"[critical-tests] {args.suite}: {elapsed:.2f}s", flush=True)
    return returncode


if __name__ == "__main__":
    raise SystemExit(main())
