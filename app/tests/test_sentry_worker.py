#!/usr/bin/env python3
"""Sentry reporting contracts for the Cloudflare Python worker."""

import ast
import asyncio
import traceback
import tomllib
from pathlib import Path
from urllib.parse import urlparse


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
WRANGLER = ROOT / "wrangler.toml"
CI_WORKFLOW = ROOT.parent / ".forkmesh" / "ci.yml"
DESKTOP_CI_WORKFLOW = ROOT.parent / ".forkmesh" / "desktop-ci.yml"
DEPLOY_WORKFLOW = ROOT.parent / ".forkmesh" / "deploy.yml"
DEPLOY_SH = ROOT / "deploy.sh"
ENV_EXAMPLE = ROOT / ".env.production.example"
ACTION_RUNNER_CPP = ROOT.parent / "desktop" / "src" / "ActionRunner.cpp"
ACTION_RUNNER_H = ROOT.parent / "desktop" / "src" / "ActionRunner.h"
CRASH_HANDLER_CPP = ROOT.parent / "desktop" / "src" / "CrashHandler.cpp"
CRASH_HANDLER_H = ROOT.parent / "desktop" / "src" / "CrashHandler.h"
MAIN_CPP = ROOT.parent / "desktop" / "src" / "main.cpp"
MAIN_WINDOW_INTERNAL_H = ROOT.parent / "desktop" / "src" / "MainWindowInternal.h"
MAIN_WINDOW_CPP = ROOT.parent / "desktop" / "src" / "MainWindow.cpp"
MAIN_WINDOW_H = ROOT.parent / "desktop" / "src" / "MainWindow.h"
MAIN_WINDOW_ACTIONS_CPP = ROOT.parent / "desktop" / "src" / "MainWindowActions.cpp"
MAIN_WINDOW_PULLS_CPP = ROOT.parent / "desktop" / "src" / "MainWindowPulls.cpp"
MAIN_WINDOW_SETTINGS_CPP = ROOT.parent / "desktop" / "src" / "MainWindowSettings.cpp"
CRASH_LOG_SCAN_CPP = ROOT.parent / "desktop" / "src" / "CrashLogScan.cpp"
CRASH_LOG_SCAN_H = ROOT.parent / "desktop" / "src" / "CrashLogScan.h"
TEAMMATE_STORE_CPP = ROOT.parent / "desktop" / "src" / "TeammateStore.cpp"
ENTRY_TEXT = (
    # entry.py + its lazily-split domain modules
    ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "app_runtime.py").read_text(encoding="utf-8")
    + "\n\n\n"
)
WRANGLER_DATA = tomllib.loads(WRANGLER.read_text(encoding="utf-8"))
CI_WORKFLOW_TEXT = CI_WORKFLOW.read_text(encoding="utf-8")
DESKTOP_CI_WORKFLOW_TEXT = DESKTOP_CI_WORKFLOW.read_text(encoding="utf-8")
DEPLOY_WORKFLOW_TEXT = DEPLOY_WORKFLOW.read_text(encoding="utf-8")
DEPLOY_SH_TEXT = DEPLOY_SH.read_text(encoding="utf-8")
ENV_EXAMPLE_TEXT = ENV_EXAMPLE.read_text(encoding="utf-8")
ACTION_RUNNER_CPP_TEXT = ACTION_RUNNER_CPP.read_text(encoding="utf-8")
ACTION_RUNNER_H_TEXT = ACTION_RUNNER_H.read_text(encoding="utf-8")
CRASH_HANDLER_CPP_TEXT = CRASH_HANDLER_CPP.read_text(encoding="utf-8")
CRASH_HANDLER_H_TEXT = CRASH_HANDLER_H.read_text(encoding="utf-8")
MAIN_CPP_TEXT = MAIN_CPP.read_text(encoding="utf-8")
APP_PATHS_CPP_TEXT = (MAIN_CPP.parent / "AppPaths.cpp").read_text(encoding="utf-8")
MAIN_WINDOW_INTERNAL_H_TEXT = MAIN_WINDOW_INTERNAL_H.read_text(encoding="utf-8")
MAIN_WINDOW_CPP_TEXT = MAIN_WINDOW_CPP.read_text(encoding="utf-8")
MAIN_WINDOW_H_TEXT = MAIN_WINDOW_H.read_text(encoding="utf-8")
MAIN_WINDOW_ACTIONS_CPP_TEXT = MAIN_WINDOW_ACTIONS_CPP.read_text(encoding="utf-8")
MAIN_WINDOW_PULLS_CPP_TEXT = MAIN_WINDOW_PULLS_CPP.read_text(encoding="utf-8")
MAIN_WINDOW_SETTINGS_CPP_TEXT = MAIN_WINDOW_SETTINGS_CPP.read_text(encoding="utf-8")
CRASH_LOG_SCAN_CPP_TEXT = CRASH_LOG_SCAN_CPP.read_text(encoding="utf-8")
CRASH_LOG_SCAN_H_TEXT = CRASH_LOG_SCAN_H.read_text(encoding="utf-8")
TEAMMATE_STORE_CPP_TEXT = TEAMMATE_STORE_CPP.read_text(encoding="utf-8")


def _load_sentry_dsn_parts():
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    selected = []
    for node in tree.body:
        if isinstance(node, ast.Assign):
            targets = {t.id for t in node.targets if isinstance(t, ast.Name)}
            if "SENTRY_CLIENT" in targets:
                selected.append(node)
        elif isinstance(node, ast.FunctionDef) and node.name == "_sentry_dsn_parts":
            selected.append(node)
    module = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))
    ns = {"urlparse": urlparse}
    exec(compile(module, str(ENTRY), "exec"), ns)
    return ns["_sentry_dsn_parts"]


def _load_sentry_request_payload():
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    want = {
        "method_name",
        "_sentry_header",
        "_sentry_safe_url",
        "_sentry_host",
        "_sentry_request_payload",
    }
    selected = [
        node for node in tree.body
        if isinstance(node, ast.FunctionDef) and node.name in want
    ]
    module = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))
    namespace = {"urlparse": urlparse}
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace["_sentry_request_payload"]


def _load_capture_worker_exception():
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    want = {
        "method_name",
        "_sentry_header",
        "_sentry_safe_url",
        "_safe_error_text",
        "capture_worker_exception",
    }
    selected = []
    for node in tree.body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and node.name in want:
            selected.append(node)
    module = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))
    calls = []

    def _boom(*args, **kwargs):
        calls.append(("dsn", args, kwargs))
        raise RuntimeError("dsn read failed")

    async def _capture(*args, **kwargs):
        calls.append(("sentry", args, kwargs))
        raise RuntimeError("sentry failed")

    async def _write(*args, **kwargs):
        calls.append(("write", args, kwargs))
        raise RuntimeError("d1 failed")

    def _console(message):
        calls.append(("console", str(message), {}))

    ns = {
        "traceback": traceback,
        "_sentry_dsn_parts": _boom,
        "capture_sentry_error": _capture,
        "_write_error_log": _write,
        "_console_error": _console,
        "calls": calls,
    }
    exec(compile(module, str(ENTRY), "exec"), ns)
    return ns


def test_sentry_uses_vanilla_worker_fetch_not_sdk_plugin():
    assert "sentry_sdk" not in ENTRY_TEXT
    assert "capture_sentry_error" in ENTRY_TEXT
    assert "application/x-sentry-envelope" in ENTRY_TEXT
    assert "x-sentry-auth" in ENTRY_TEXT
    assert "from js import fetch as js_fetch" in ENTRY_TEXT


def test_sentry_dsn_parser_builds_envelope_endpoint():
    parse = _load_sentry_dsn_parts()
    parts = parse("https://public@example.ingest.sentry.io/12345")
    assert parts["endpoint"] == "https://example.ingest.sentry.io/api/12345/envelope/"
    assert "sentry_key=public" in parts["auth"]
    assert "sentry_version=7" in parts["auth"]

    self_hosted = parse("https://pub@sentry.example.com/prefix/987")
    assert self_hosted["endpoint"] == "https://sentry.example.com/prefix/api/987/envelope/"


def test_worker_error_log_reports_to_sentry_and_preserves_d1_log():
    assert "await capture_sentry_error(" in ENTRY_TEXT
    assert "INSERT INTO error_log" in ENTRY_TEXT
    assert "request=request" in ENTRY_TEXT
    assert "error=error" in ENTRY_TEXT
    assert '"cloudflare": cf_context' in ENTRY_TEXT


def test_sentry_cron_monitor_checkins_are_disabled():
    # The helper machinery remains available, but no scheduled phase sends a
    # Sentry monitor check-in while this integration is disabled.
    assert 'SENTRY_CRON_MONITOR_SLUG = "forkmesh-relay"' in ENTRY_TEXT
    assert "async def capture_sentry_cron_check_in" in ENTRY_TEXT
    assert '"type": "check_in"' in ENTRY_TEXT
    assert '"monitor_slug": _sentry_cron_monitor_slug(env)' in ENTRY_TEXT
    assert '"status": status' in ENTRY_TEXT
    assert '"monitor_config": _sentry_cron_monitor_config(env, cron)' in ENTRY_TEXT
    assert '"schedule": {"type": "crontab", "value": cron[:120]}' in ENTRY_TEXT
    assert '"checkin_margin": _sentry_int_env(' in ENTRY_TEXT
    assert '"max_runtime": _sentry_int_env(' in ENTRY_TEXT

    alarm = ENTRY_TEXT.split("    async def _coordinator_alarm(self):", 1)[1] \
        .split("    async def _best_effort_storage_put", 1)[0]
    # No live (uncommented) check-in call remains in the coordinator alarm.
    for line in alarm.splitlines():
        stripped = line.strip()
        if stripped.startswith("#"):
            continue
        assert "capture_sentry_cron_check_in(" not in stripped
    # The alarm still records bounded phase failure details in its own
    # storage; disabling Sentry check-ins must not hide them.
    assert '"cron phase failures: "' in alarm
    # The tick builds those bounded lines one level down, phase by phase.
    assert "await self._run_tick_slice(state)" in alarm
    tick = ENTRY_TEXT.split("    async def _run_tick_slice(self, state):", 1)[1] \
        .split("    async def _run_claimed_phase", 1)[0]
    assert "_safe_error_text(error)[:240]" in tick
    for line in tick.splitlines():
        if not line.strip().startswith("#"):
            assert "capture_sentry_cron_check_in(" not in line

    assert "SENTRY_CRON_MONITOR_SLUG=forkmesh-relay" in ENV_EXAMPLE_TEXT
    assert "SENTRY_CRON_CHECKIN_MARGIN_MINUTES=1" in ENV_EXAMPLE_TEXT
    assert "SENTRY_CRON_MAX_RUNTIME_MINUTES=5" in ENV_EXAMPLE_TEXT


def test_worker_1101_exceptions_are_captured_then_reraised():
    assert "async def capture_worker_exception" in ENTRY_TEXT
    assert 'cloudflare_error_code="1101"' in ENTRY_TEXT
    assert '"cloudflare.error_code"] = str(cloudflare_error_code)' in ENTRY_TEXT
    assert '"cloudflare.ray_id"] = str(ray)' in ENTRY_TEXT
    assert "await capture_worker_exception(self.env, request, url, error)" in ENTRY_TEXT
    assert "raise\n" in ENTRY_TEXT
    assert 'return json_response({"error": "internal_error"}, status=500)' not in ENTRY_TEXT
    failure_path = ENTRY_TEXT[
        ENTRY_TEXT.index("        except Exception as error:", ENTRY_TEXT.index(
            "    async def fetch(self, request):"))
        :ENTRY_TEXT.index("        response = _api_cors_response(", ENTRY_TEXT.index(
            "    async def fetch(self, request):"))
    ]
    assert failure_path.index("await capture_worker_exception(") < (
        failure_path.rindex("raise"))


def test_worker_exception_capture_does_not_raise_from_reporting_failures():
    ns = _load_capture_worker_exception()

    class HostileEnv:
        @property
        def SENTRY_DSN(self):
            raise RuntimeError("env unavailable")

    class HostileRequest:
        @property
        def headers(self):
            raise RuntimeError("headers unavailable")

        @property
        def method(self):
            raise RuntimeError("method unavailable")

        @property
        def url(self):
            raise RuntimeError("url unavailable")

    class HostileUrl:
        @property
        def path(self):
            raise RuntimeError("path unavailable")

    class HostileError(Exception):
        def __repr__(self):
            raise RuntimeError("repr unavailable")

        def __str__(self):
            raise RuntimeError("str unavailable")

    asyncio.run(ns["capture_worker_exception"](
        HostileEnv(), HostileRequest(), HostileUrl(), HostileError()))

    call_names = [item[0] for item in ns["calls"]]
    assert "sentry" in call_names
    assert "write" in call_names


def test_telemetry_never_runs_on_a_detached_task():
    # There is no fire-and-forget helper any more: a detached task is one more
    # PyodideTask the runtime can re-enter, and a wedged isolate answers 1101
    # for every later request (see tests/test_worker_task_concurrency.py).
    # Telemetry is awaited inline inside its own try/except instead.
    assert "def _fire_and_forget" not in ENTRY_TEXT
    assert "asyncio.ensure_future(" not in ENTRY_TEXT
    assert "asyncio.create_task(" not in ENTRY_TEXT
    # Repository transfers are awaited direct-HTTPS fetches, not detached
    # socket watchdog tasks.
    proxy = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _https_mirror_proxy"):
        ENTRY_TEXT.index("\n\nclass Default")
    ]
    assert "await js_fetch_with_timeout(" in proxy
    assert "await asyncio.wait_for(" not in proxy
    assert "JsAbortSignal.timeout(" in ENTRY_TEXT
    assert "_stream_watchdog" not in ENTRY_TEXT


def test_sentry_request_metadata_is_allowlisted_and_sanitized():
    assert 'for name in ("host", "cf-ray")' in ENTRY_TEXT
    assert '"url": safe_url' in ENTRY_TEXT
    assert "return parsed.scheme + \"://\" + parsed.netloc + (parsed.path or \"/\")" in ENTRY_TEXT
    assert "req = _sentry_request_payload(request, path)" in ENTRY_TEXT
    request_payload = ENTRY_TEXT.split("def _sentry_request_payload", 1)[1] \
        .split("def _sentry_cloudflare_context", 1)[0]
    assert '"authorization"' not in request_payload.lower()
    assert '"cookie"' not in request_payload.lower()
    assert '"user-agent"' not in request_payload.lower()
    assert '"accept"' not in request_payload.lower()
    cloudflare_context = ENTRY_TEXT.split(
        "def _sentry_cloudflare_context", 1)[1].split(
        "def _sentry_environment", 1)[0]
    assert '"country"' not in cloudflare_context
    assert '"timezone"' not in cloudflare_context
    assert '"clientTcpRtt"' not in cloudflare_context


def test_private_route_error_text_is_redacted_in_sentry_and_d1():
    assert "def _privacy_redacted_log_path(path):" in ENTRY_TEXT
    assert "def _privacy_safe_error_text(path, message):" in ENTRY_TEXT
    assert "text = _privacy_safe_error_text(path, message)" in ENTRY_TEXT
    assert "message = _privacy_safe_error_text(path, message)" in ENTRY_TEXT
    assert "Repository-route exception details redacted." in ENTRY_TEXT
    assert "Repository-route error details redacted." in ENTRY_TEXT

    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    selected = [
        node for node in tree.body
        if isinstance(node, ast.FunctionDef)
        and node.name in {
            "_privacy_redacted_log_path",
            "_privacy_safe_error_text",
        }
    ]
    namespace = {}
    exec(
        compile(
            ast.fix_missing_locations(
                ast.Module(body=selected, type_ignores=[])),
            str(ENTRY),
            "exec",
        ),
        namespace,
    )
    safe = namespace["_privacy_safe_error_text"]
    secret = "alice/top-secret?token=do-not-log"
    for path in (
        "/private-or-unpublished-repository",
        "/repository-route-redacted",
        "/api/private-replicas/[opaque]",
    ):
        rendered = safe(path, secret)
        assert rendered == "Repository-route error details redacted."
        assert "alice" not in rendered
        assert "token" not in rendered
    assert safe("/health", "probe failed") == "probe failed"
    assert safe("/private-or-unpublished-repository/[git-info-refs]",
                "response status 503") == "response status 503"
    for raw, kind in (
            ("D1_ERROR: D1 DB is overloaded. alice/top-secret", "overload"),
            ("D1_ERROR: Network connection lost. alice", "network connection lost"),
            ("D1_ERROR: something new about alice/top-secret", "other")):
        rendered = safe("/private-or-unpublished-repository/[git-info-refs]",
                        "d1 unavailable: " + raw)
        assert rendered == ("d1 unavailable (%s): Repository-route error "
                            "details redacted." % kind)
        assert "alice" not in rendered


def test_sentry_request_url_reuses_private_repository_path_redaction():
    request_payload = _load_sentry_request_payload()

    class Request:
        method = "GET"
        url = (
            "https://forkmesh.test/api/repo/alice/top-secret/"
            "private-replica?credential=never-log"
        )
        headers = {
            "host": "forkmesh.test",
            "user-agent": "test-browser",
        }

    payload = request_payload(
        Request(), "/private-or-unpublished-repository")

    assert payload["url"] == (
        "https://forkmesh.test/private-or-unpublished-repository")
    assert "alice" not in repr(payload)
    assert "top-secret" not in repr(payload)
    assert "credential" not in repr(payload)


def test_sentry_redacts_opaque_private_access_locator_and_query():
    opaque_id = "a" * 64
    privacy_filter = ENTRY_TEXT.split(
        "async def _privacy_safe_log_path", 1)[1].split(
            "async def capture_worker_exception", 1)[0]
    assert '"/api/private-replicas/[opaque]"' in privacy_filter
    assert 'path.startswith(\n            "/api/private-replicas/")' in (
        privacy_filter)

    request_payload = _load_sentry_request_payload()

    class Request:
        method = "GET"
        url = (
            "https://forkmesh.test/api/private-replicas/" + opaque_id
            + "?credential=never-log"
        )
        headers = {
            "host": "forkmesh.test",
            "user-agent": "test-browser",
        }

    payload = request_payload(
        Request(), "/api/private-replicas/[opaque]")
    rendered = repr(payload)
    assert payload["url"] == (
        "https://forkmesh.test/api/private-replicas/[opaque]")
    assert opaque_id not in rendered
    assert "credential" not in rendered


def test_sentry_redacts_chat_room_identifiers_from_error_paths():
    privacy_filter = ENTRY_TEXT.split(
        "async def _privacy_safe_log_path", 1)[1].split(
            "async def capture_worker_exception", 1)[0]
    assert '"/api/chat/channels/[opaque]"' in privacy_filter
    assert '"/api/chat/direct-messages/[opaque]"' in privacy_filter
    assert 'path.startswith("/api/chat/channels/")' in privacy_filter
    assert 'path.startswith("/api/chat/direct-messages/")' in privacy_filter


def test_simulate_sentry_error_route_is_worker_owned_and_raises():
    run_worker_first = WRANGLER_DATA["assets"]["run_worker_first"]
    assert "/simulate-sentry-error" in run_worker_first
    assert 'url.path in ("/simulate-sentry-error", "/simulate-sentry-error/")' in ENTRY_TEXT
    assert "division_by_zero = 1 / 0" in ENTRY_TEXT


def test_forkmesh_deploy_pushes_sentry_dsn_secret():
    assert "SENTRY_DSN=${{ vars.SENTRY_DSN }}" in DEPLOY_WORKFLOW_TEXT
    assert "\n        push_secrets\n" in DEPLOY_SH_TEXT


def test_forkmesh_actions_run_bounded_critical_suites():
    # Steps run with egress denied, so the suite uses the interpreter the runner
    # exposes rather than building an environment it cannot download into.
    assert "python3 tools/run_critical_tests.py worker" in CI_WORKFLOW_TEXT
    assert "python3 tools/run_critical_tests.py world" in CI_WORKFLOW_TEXT
    assert "FORKMESH_CI_KNOWN_FAILURES=1" not in CI_WORKFLOW_TEXT
    assert "python3 -m venv" not in CI_WORKFLOW_TEXT
    assert [
        line
        for line in CI_WORKFLOW_TEXT.splitlines()
        if "pip install" in line and not line.lstrip().startswith("#")
    ] == []
    assert "needs: [.forkmesh/ci.yml]" in DEPLOY_WORKFLOW_TEXT
    assert "already ran for this commit" in DEPLOY_WORKFLOW_TEXT
    assert "pip install --user" not in CI_WORKFLOW_TEXT
    assert "pip install --user" not in DEPLOY_WORKFLOW_TEXT
    assert "python3 cloudflare_worker/tests/test_crypto.py" not in CI_WORKFLOW_TEXT
    assert "python3 cloudflare_worker/tests/test_crypto.py" not in DEPLOY_WORKFLOW_TEXT
    assert 'cmake -S desktop -B "$build"' in DESKTOP_CI_WORKFLOW_TEXT
    assert "--target critical-tests" in DESKTOP_CI_WORKFLOW_TEXT
    assert "python3 tools/run_critical_tests.py qt" in DESKTOP_CI_WORKFLOW_TEXT


def test_action_sandbox_lives_on_disk_not_the_shared_tmpfs():
    # /tmp is a size-capped tmpfs shared with every other checkout on the host,
    # and the sandbox holds the source, the writable clone, the step's home and
    # the step's /tmp. Filling it is silent — CMake dropped a generated Makefile
    # and still reported success — so the trees belong beside the run records.
    assert "QString sandboxBaseDir(const ActionStore *store)" in ACTION_RUNNER_CPP_TEXT
    assert "store->sandboxDir()" in ACTION_RUNNER_CPP_TEXT
    assert "m_sandboxRoot = sandboxBase + QStringLiteral(\"/forkmesh-run-\")" in (
        ACTION_RUNNER_CPP_TEXT
    )
    assert "m_sandboxRoot =\n        QDir::tempPath()" not in ACTION_RUNNER_CPP_TEXT
    # An app-data directory does not self-clean the way /tmp did, so a run whose
    # node was killed outright must not leak its tree forever.
    assert "removeStaleSandboxes(sandboxBase, sandboxStampMs" in ACTION_RUNNER_CPP_TEXT


def test_action_sandbox_exposes_node_python_packages_for_offline_steps():
    # Egress is denied during a run, so a step can only import what the runner
    # mounts. System packages come with /usr; pip's per-user site directory is
    # in the host home, which is otherwise kept out of the namespace entirely.
    assert 'QStringLiteral("/.local/lib")' in ACTION_RUNNER_CPP_TEXT
    assert 'QStringLiteral("/site-packages")' in ACTION_RUNNER_CPP_TEXT
    assert (
        '<< QStringLiteral("/home/forkmesh/.local/lib/%1/site-packages")'
        in ACTION_RUNNER_CPP_TEXT
    )
    # Read-only, and only site-packages: ~/.local/share holds this node's keys.
    assert (
        'args << QStringLiteral("--ro-bind") << site' in ACTION_RUNNER_CPP_TEXT
    )
    assert 'QStringLiteral("/.local/share")' not in ACTION_RUNNER_CPP_TEXT
    assert 'QStringLiteral("/.local/share")' not in ACTION_RUNNER_H_TEXT


def test_action_runner_does_not_override_installer_source_in_ci_jobs():
    # Repository identity is only injected for release jobs. Keep the owner/name
    # guard nested in that release-only block so ordinary CI runs retain the
    # installer source chosen by their workflow environment.
    assert "if (isReleaseRun()) {" in ACTION_RUNNER_CPP_TEXT
    assert (
        "if (!m_run.owner.isEmpty() && !m_run.name.isEmpty())\n"
        "                env.insert(QStringLiteral(\"FORKMESH_REPO\"),"
    ) in ACTION_RUNNER_CPP_TEXT
    assert "env.insert(QStringLiteral(\"FORKMESH_REPO\")," in ACTION_RUNNER_CPP_TEXT
    assert "if (!m_run.owner.isEmpty() && !m_run.name.isEmpty())\n        env.insert(QStringLiteral(\"FORKMESH_REPO\")," not in ACTION_RUNNER_CPP_TEXT


def test_action_runner_caps_process_output_so_pipeline_logs_do_not_crash_app():
    assert "kActionProcessLogMaxBytes" in ACTION_RUNNER_CPP_TEXT
    assert "kActionProcessLogChunkBytes" in ACTION_RUNNER_CPP_TEXT
    assert "kActionProcessLogTailBytes" in ACTION_RUNNER_CPP_TEXT
    assert "kActionProcessLogMaxLineChars" in ACTION_RUNNER_CPP_TEXT
    assert "emitProcessOutput(const QByteArray &bytes)" in ACTION_RUNNER_CPP_TEXT
    assert "emitProcessOutputTruncationNotice()" in ACTION_RUNNER_CPP_TEXT
    assert "normalizeProcessOutputForLog(const QString &text)" in ACTION_RUNNER_CPP_TEXT
    assert "rememberSuppressedProcessOutput(const QByteArray &bytes)" in ACTION_RUNNER_CPP_TEXT
    assert "emitSuppressedProcessOutputTail()" in ACTION_RUNNER_CPP_TEXT
    assert "rememberCrashProcessOutput(const QByteArray &bytes)" in ACTION_RUNNER_CPP_TEXT
    assert "updateCrashContext()" in ACTION_RUNNER_CPP_TEXT
    # Artifact validation can replace the caller's original finalMessage, so
    # diagnostics must capture the effective result shown to the user.
    assert "logFailureDiagnostic(resultMessage)" in ACTION_RUNNER_CPP_TEXT
    assert "m_processOutputBytes = 0;" in ACTION_RUNNER_CPP_TEXT
    assert "m_processOutputSuppressedBytes = 0;" in ACTION_RUNNER_CPP_TEXT
    assert "m_processOutputTail.clear();" in ACTION_RUNNER_CPP_TEXT
    assert "m_processOutputLineChars = 0;" in ACTION_RUNNER_CPP_TEXT
    assert "m_processOutputTruncated = false;" in ACTION_RUNNER_CPP_TEXT
    assert "Action output exceeded %1 KiB" in ACTION_RUNNER_CPP_TEXT
    assert "Showing the final %1 KiB of suppressed process output" in ACTION_RUNNER_CPP_TEXT
    assert "recent process output tail:" in ACTION_RUNNER_CPP_TEXT
    assert "forkmesh::setCrashContext(forkmesh::fitCrashContext(head, tail))" in ACTION_RUNNER_CPP_TEXT
    assert "forkmesh::setTerminationSignalSurvivalEnabled(true)" in ACTION_RUNNER_CPP_TEXT
    assert "forkmesh::setTerminationSignalSurvivalEnabled(false)" in ACTION_RUNNER_CPP_TEXT
    assert "emitProcessOutput(process->readAllStandardOutput())" in ACTION_RUNNER_CPP_TEXT
    assert "emitProcessOutput(tail)" in ACTION_RUNNER_CPP_TEXT
    assert "emitSuppressedProcessOutputTail();" in ACTION_RUNNER_CPP_TEXT
    assert "emitLog(QString::fromUtf8(m_process->readAllStandardOutput()))" not in ACTION_RUNNER_CPP_TEXT
    assert "emitLog(QString::fromUtf8(tail))" not in ACTION_RUNNER_CPP_TEXT
    assert "void emitProcessOutput(const QByteArray &bytes);" in ACTION_RUNNER_H_TEXT
    assert "QByteArray m_processOutputTail;" in ACTION_RUNNER_H_TEXT
    assert "QByteArray m_crashOutputTail;" in ACTION_RUNNER_H_TEXT
    assert "bool m_processOutputTruncated = false;" in ACTION_RUNNER_H_TEXT
    assert "g_crashContext" in CRASH_HANDLER_CPP_TEXT
    assert "addCrashContext(g_record);" in CRASH_HANDLER_CPP_TEXT
    assert "record.add(g_crashContext);" in CRASH_HANDLER_CPP_TEXT
    assert "SIGTERM (terminated)" in CRASH_HANDLER_CPP_TEXT
    assert "SIGINT (interrupt)" in CRASH_HANDLER_CPP_TEXT
    assert "SIGHUP (hangup)" in CRASH_HANDLER_CPP_TEXT
    assert "SIGQUIT (quit)" in CRASH_HANDLER_CPP_TEXT
    assert "SIGXCPU (CPU time limit exceeded)" in CRASH_HANDLER_CPP_TEXT
    assert "SIGXFSZ (file size limit exceeded)" in CRASH_HANDLER_CPP_TEXT
    assert "SIGTERM, SIGINT" in CRASH_HANDLER_CPP_TEXT
    signal_handlers = CRASH_HANDLER_CPP_TEXT[
        CRASH_HANDLER_CPP_TEXT.index("void installSignalHandlers()")
        :CRASH_HANDLER_CPP_TEXT.index("} // namespace", CRASH_HANDLER_CPP_TEXT.index(
            "void installSignalHandlers()"))
    ]
    assert "SIGKILL" not in signal_handlers
    assert "SIGSTOP" not in signal_handlers
    assert "std::atomic_int g_surviveTerminationSignals" in CRASH_HANDLER_CPP_TEXT
    assert "bool isTerminationSignal(int sig)" in CRASH_HANDLER_CPP_TEXT
    assert "g_surviveTerminationSignals.load(std::memory_order_relaxed)" in CRASH_HANDLER_CPP_TEXT
    assert "termination signal survived" in CRASH_HANDLER_CPP_TEXT
    assert "int g_mainLogFd = -1;" in CRASH_HANDLER_CPP_TEXT
    assert "safeWriteMainLogSignalRecord" in CRASH_HANDLER_CPP_TEXT
    assert "ForkMesh signal: " in CRASH_HANDLER_CPP_TEXT
    assert "action workflow survived and will finish/fail normally" in CRASH_HANDLER_CPP_TEXT
    # An external stop request is not a crash: it gets its own heading, no
    # event-loop backtrace, and is handed to the event loop so closeEvent()
    # still saves settings/chat/log before the process exits.
    assert "===== ForkMesh shutdown signal =====" in CRASH_HANDLER_CPP_TEXT
    assert "bool requestGracefulShutdown()" in CRASH_HANDLER_CPP_TEXT
    assert "std::atomic_flag g_gracefulShutdownRequested" in CRASH_HANDLER_CPP_TEXT
    assert "kGracefulShutdownDeadlineSeconds" in CRASH_HANDLER_CPP_TEXT
    assert "::alarm(kGracefulShutdownDeadlineSeconds);" in CRASH_HANDLER_CPP_TEXT
    assert "; shutting down cleanly" in CRASH_HANDLER_CPP_TEXT
    assert "void enableGracefulTerminationShutdown(QObject *context," in CRASH_HANDLER_H_TEXT
    assert "forkmesh::enableGracefulTerminationShutdown(&app," in MAIN_CPP_TEXT
    assert "void addSignalInfo(SignalSafeRecord &record, const siginfo_t *info)" in CRASH_HANDLER_CPP_TEXT
    assert "signal code: " in CRASH_HANDLER_CPP_TEXT
    assert "sender pid: " in CRASH_HANDLER_CPP_TEXT
    assert "sender uid: " in CRASH_HANDLER_CPP_TEXT
    assert "sa.sa_flags = SA_ONSTACK | SA_SIGINFO;" in CRASH_HANDLER_CPP_TEXT
    assert "sa.sa_flags = SA_ONSTACK | SA_RESETHAND" not in CRASH_HANDLER_CPP_TEXT
    # The obsolete desktop headless/server runtime was removed entirely; the
    # GUI process has one crash/shutdown signal owner now.
    assert not (ROOT.parent / "desktop" / "src" / "HeadlessConsole.cpp").exists()
    assert "HeadlessConsole" not in MAIN_CPP_TEXT
    assert "appendDiagnosticToMainLog" in CRASH_HANDLER_CPP_TEXT
    assert "network_log.txt" in CRASH_HANDLER_CPP_TEXT
    assert "void installCrashHandler(const QString &crashLogPath = QString()," in CRASH_HANDLER_H_TEXT
    assert "const QString &mainLogPath = QString())" in CRASH_HANDLER_H_TEXT
    assert "void setCrashContext(const QString &context)" in CRASH_HANDLER_H_TEXT
    assert "void setTerminationSignalSurvivalEnabled(bool enabled)" in CRASH_HANDLER_H_TEXT
    assert "void logDiagnosticEvent(const QString &context, const QString &details)" in CRASH_HANDLER_H_TEXT
    # The pre-QApplication log path comes from the layout resolver, so a crash
    # before startup lands in the same project's log as everything after it.
    assert "forkmesh::paths::earlyMainLogPath(argc, argv));" in MAIN_CPP_TEXT
    assert "QString earlyMainLogPath(int argc, char **argv)" in APP_PATHS_CPP_TEXT
    assert 'QStringLiteral("network_log.txt")' in APP_PATHS_CPP_TEXT
    assert "displaySafePlainLog" in MAIN_WINDOW_INTERNAL_H_TEXT
    assert "text.size() > 8192" in MAIN_WINDOW_INTERNAL_H_TEXT
    assert "m_actionLog->setLineWrapMode(QPlainTextEdit::NoWrap)" in MAIN_WINDOW_ACTIONS_CPP_TEXT
    assert "view->insertPlainText(displaySafePlainLog(text));" in MAIN_WINDOW_ACTIONS_CPP_TEXT
    assert "displaySafePlainLog(m_actionStore->readLog(*run))" in MAIN_WINDOW_ACTIONS_CPP_TEXT
    assert "displaySafePlainLog(" in MAIN_WINDOW_PULLS_CPP_TEXT
    assert "~MainWindow() override;" in MAIN_WINDOW_H_TEXT
    assert "MainWindow::~MainWindow()" in MAIN_WINDOW_CPP_TEXT
    assert "MainWindow teardown" in MAIN_WINDOW_CPP_TEXT
    assert "QObject::disconnect(view, nullptr, this, nullptr)" in MAIN_WINDOW_CPP_TEXT
    assert "m_diffViews.clear();" in MAIN_WINDOW_CPP_TEXT
    assert "RESTARTING: ForkMesh exited while this " in MAIN_WINDOW_ACTIONS_CPP_TEXT
    assert "run was active" in MAIN_WINDOW_ACTIONS_CPP_TEXT
    assert "actionRunLogPath(const ActionRun &run)" in MAIN_WINDOW_ACTIONS_CPP_TEXT
    assert "Run log: %6" in MAIN_WINDOW_ACTIONS_CPP_TEXT
    assert "void MainWindow::refreshCommitBarStatusGlyph()" in MAIN_WINDOW_ACTIONS_CPP_TEXT
    assert "void MainWindow::refreshCommitTableStatusGlyphs()" in MAIN_WINDOW_ACTIONS_CPP_TEXT
    assert "refreshCommitBarStatusGlyph();" in MAIN_WINDOW_ACTIONS_CPP_TEXT
    assert "refreshCommitTableStatusGlyphs();" in MAIN_WINDOW_ACTIONS_CPP_TEXT
    assert "m_commitBarStatusHash" in MAIN_WINDOW_H_TEXT
    assert "m_commitBarBodyHtml" in MAIN_WINDOW_H_TEXT
    assert "m_overviewLoadedKey.clear();\n        loadRepoOverview(m_overviewPath);" not in MAIN_WINDOW_ACTIONS_CPP_TEXT
    assert "case 1: // Commits list\n        loadCommits();" not in MAIN_WINDOW_ACTIONS_CPP_TEXT


def test_a_crash_starts_the_repair_teammate_on_the_next_launch():
    # The crash handler writes its block straight to the log fd from the dying
    # process, so nothing in the running app can react to it. The next launch
    # reads it back and hands it to the teammate that owns error-log repair -
    # immediately, without waiting for that teammate's ten-error batch.
    assert '"===== ForkMesh crash ====="' in CRASH_LOG_SCAN_H_TEXT
    assert "QList<Record> parseCrashRecords(const QStringList &lines);" in CRASH_LOG_SCAN_H_TEXT
    # A stop request keeps its own heading and is never repair work.
    assert '"===== ForkMesh shutdown signal ====="' in CRASH_LOG_SCAN_CPP_TEXT
    assert "newCrashes = unrepairedCrashRecords();" in MAIN_WINDOW_SETTINGS_CPP_TEXT
    assert "startCrashRepairTeammate(repoIndex, teammate, taskType, stateKey);" in (
        MAIN_WINDOW_SETTINGS_CPP_TEXT
    )
    # One crash is enough, and it does not go through a badge count.
    assert "trigger.threshold = 1;" in MAIN_WINDOW_SETTINGS_CPP_TEXT
    assert "trigger.inclusive = true;" in MAIN_WINDOW_SETTINGS_CPP_TEXT
    # Ownership is the task route, which survives a monitor being edited away.
    assert "QString errorRepairTaskType(" in MAIN_WINDOW_SETTINGS_CPP_TEXT
    assert "route.taskType == forkmesh::teammates::kTaskTypeErrorLogs" in (
        MAIN_WINDOW_SETTINGS_CPP_TEXT
    )
    # Same operator switch as the teammate's other automatic runs, and the
    # crash takes the teammate's one slot ahead of the error batch.
    assert (
        "const bool automaticRunsEnabled =\n"
        "        QSettings().value(kAutoAgentOnErrorLogSetting, true).toBool();"
    ) in MAIN_WINDOW_SETTINGS_CPP_TEXT
    assert "!repairingCrash.contains(teammate.id)" in MAIN_WINDOW_SETTINGS_CPP_TEXT
    # Queued once per crash, and repaired until a run actually succeeds.
    assert "kCrashRepairObservedEpochSetting" in MAIN_WINDOW_INTERNAL_H_TEXT
    assert "crashRuntime.finishRun(true, completedAtMs);" in MAIN_WINDOW_SETTINGS_CPP_TEXT
    assert "latest.finishRun(false, 0);" in MAIN_WINDOW_SETTINGS_CPP_TEXT
    # The repair may not simply stop recording crashes.
    assert "do not remove or narrow the crash handler" in MAIN_WINDOW_SETTINGS_CPP_TEXT
    assert "or weaken logging - or " in TEAMMATE_STORE_CPP_TEXT
    assert "the crash handler - to make the count fall" in TEAMMATE_STORE_CPP_TEXT


def test_worker_observability_stays_within_the_free_event_budget():
    # Retain a useful diagnostic sample without letting routine invocations
    # consume Cloudflare's 200k/day observability-event allowance. Sentry's
    # explicit error path remains independent from this dashboard sample.
    observability = WRANGLER_DATA["observability"]
    assert observability["enabled"] is True
    assert 0 < observability["head_sampling_rate"] <= 0.05
    assert (
        observability["logs"]["head_sampling_rate"]
        == observability["head_sampling_rate"]
    )
    assert "destinations" not in observability["logs"]
    assert observability["logs"]["invocation_logs"] is False
    assert observability["traces"]["enabled"] is False
    assert "destinations" not in observability["traces"]


def test_expected_degraded_responses_skip_the_generic_5xx_logger():
    # Deliberate degraded answers — DO-abort 503s already counted by
    # log_durable_object_abort, the fail-closed git push
    # 501, and central-fund/office "upstream unavailable" 503s — used to be
    # re-logged by the outer fetch as anonymous "response status N" Sentry
    # events (one DO abort could produce multiple error rows). They now carry the
    # expected-degraded marker and the generic logger skips them.
    assert 'EXPECTED_DEGRADED_HEADER = "x-forkmesh-expected-degraded"' in (
        ENTRY_TEXT)
    assert "def _response_is_expected_degraded(response):" in ENTRY_TEXT

    logger_block = ENTRY_TEXT.split(
        "_is_tunnel_content_path(url.path):", 1)[1][:700]
    assert "_response_is_expected_degraded(response)" in logger_block
    assert logger_block.index("_response_is_expected_degraded") < (
        logger_block.index("await log_error("))

    # Every DO-abort 503 fallback is marked, so the aggregate stays the only
    # record of the event.
    for site_start in [
        match for match in range(len(ENTRY_TEXT))
        if ENTRY_TEXT.startswith("await log_durable_object_abort(", match)
    ]:
        tail = ENTRY_TEXT[site_start:site_start + 700]
        assert "EXPECTED_DEGRADED_HEADERS" in tail, ENTRY_TEXT[
            site_start:site_start + 120]

    # The deliberate no-write-endpoint push answer and the central-fund
    # unavailable answers are marked too.
    push_blocks = ENTRY_TEXT.split('{"error": "push_unavailable"}')[1:]
    assert push_blocks
    for push_block in push_blocks:
        assert "EXPECTED_DEGRADED_HEADERS" in push_block[:300]
    fund_block = ENTRY_TEXT.split(
        "async def _account_central_fund", 1)[1][:1600]
    assert fund_block.count("EXPECTED_DEGRADED_HEADERS") == 2


def test_durable_object_aborts_are_aggregated_without_error_rows():
    body = ENTRY_TEXT.split(
        "async def log_durable_object_abort(", 1)[1].split(
            "\n\ndef _is_d1_platform_error", 1)[0]
    assert "INSERT INTO durable_object_abort_minute" in body
    assert "ON CONFLICT(minute_ts) DO UPDATE SET" in body
    assert "duration_aborts" in body
    assert "_write_error_log(" not in body
    assert "capture_sentry_error(" not in body


def test_mirror_gateway_retries_all_5xx_and_marks_unavailability_expected():
    proxy = ENTRY_TEXT.split(
        "async def _https_mirror_proxy(", 1)[1].split(
            "\n\nclass Default(", 1)[0]
    assert "or 500 <= status <= 599" in proxy
    assert proxy.count("extra_headers=EXPECTED_DEGRADED_HEADERS") >= 3
    assert 'status=503' in proxy


def test_redacted_repo_routes_keep_identity_free_route_family_tags():
    # /private-or-unpublished-repository collapsed EVERY failing private or
    # unknown repo route into one bucket; recurring failures could not even
    # be told apart by endpoint. The redacted path now keeps only the fixed
    # route-family name — never owner/repo or user-named path parts.
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    selected = [
        node for node in tree.body
        if isinstance(node, ast.FunctionDef)
        and node.name == "_privacy_redacted_route_kind"
    ]
    assert selected, "_privacy_redacted_route_kind not found"
    import re as re_module
    namespace = {
        "GIT_INFO_RE": re_module.compile(r"^/([^/]+)/([^/]+)/info/refs$"),
        "GIT_PACK_RE": re_module.compile(
            r"^/([^/]+)/([^/]+)/git-upload-pack$"),
        "GIT_RECEIVE_RE": re_module.compile(
            r"^/([^/]+)/([^/]+)/git-receive-pack$"),
        "RELEASE_BLOB_RE": re_module.compile(
            r"^/([^/]+)/([^/]+)/releases/blob/"),
        "REPO_API_PREFIX_RE": re_module.compile(
            r"^/api/repo/([^/]+)/([^/]+)(?:/.*)?$"),
        "safe_segment": lambda value: (
            value if re_module.fullmatch(r"[A-Za-z0-9._-]{1,100}", value or "")
            else ""),
    }
    exec(
        compile(
            ast.fix_missing_locations(
                ast.Module(body=selected, type_ignores=[])),
            str(ENTRY),
            "exec",
        ),
        namespace,
    )
    kind = namespace["_privacy_redacted_route_kind"]
    assert kind("/alice/top-secret/info/refs") == "[git-info-refs]"
    assert kind("/alice/top-secret/git-receive-pack") == "[git-receive-pack]"
    assert kind("/api/repo/alice/top-secret/tree") == "[api:tree]"
    assert kind("/api/repo/alice/top-secret/star") == "[api:star]"
    assert kind("/alice/top-secret/blob/src/keys.pem") == "[page]"
    # User-named data never survives into the tag.
    for rendered in (
        kind("/api/repo/alice/top-secret/" + "x" * 60),
        kind("/api/repo/alice/top-secret/%2e%2e"),
    ):
        assert "top-secret" not in rendered
        assert "alice" not in rendered
        assert rendered in ("[api]", "[api:x]")
    assert "response status " in ENTRY_TEXT.split(
        "def _privacy_safe_error_text", 1)[1][:900]


def test_internal_log_labels_are_not_mistaken_for_repository_routes():
    import asyncio
    import re as _re

    source = ENTRY_TEXT.split("async def _privacy_safe_log_path", 1)[1].split(
        "async def capture_worker_exception", 1)[0]
    namespace = {
        "REPO_API_PREFIX_RE": _re.compile(r"^/api/repo/([^/]+)/([^/]+)"),
        "GIT_INFO_RE": _re.compile(r"^$x"),
        "GIT_PACK_RE": _re.compile(r"^$x"),
        "GIT_RECEIVE_RE": _re.compile(r"^$x"),
        "RELEASE_BLOB_RE": _re.compile(r"^$x"),
        "looks_like_repo_route": lambda path: True,
        "safe_segment": lambda value: value,
        "unquote": lambda value: value,
        "_privacy_redacted_route_kind": lambda path: "[page]",
    }

    async def private(env, owner, repo):
        return True

    namespace["_repo_is_private"] = private
    exec("async def _privacy_safe_log_path" + source, namespace)
    redact = namespace["_privacy_safe_log_path"]
    for label in ("forkbot/ai", "forkbot/ai-agent", "plugin/ai",
                  "username/moderation"):
        assert asyncio.run(redact(None, label)) == label
    assert asyncio.run(redact(None, "/forkbot/ai")) == (
        "/private-or-unpublished-repository/[page]")
