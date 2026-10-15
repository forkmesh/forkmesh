"""Contracts for the network-isolated ForkMesh CI workflow."""

import ast
import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / ".forkmesh" / "ci.yml"
DESKTOP_WORKFLOW = ROOT / ".forkmesh" / "desktop-ci.yml"
DEPLOY_WORKFLOW = ROOT / ".forkmesh" / "deploy.yml"
RELEASE_WORKFLOW = ROOT / ".forkmesh" / "release.yml"
CRITICAL_RUNNER = ROOT / "tools" / "run_critical_tests.py"
QUALITY_GATES = ROOT / "www" / "docs" / "engineering" / "quality-gates.json"
BROWSER_PACKAGE = ROOT / "world" / "browser_tests" / "package.json"
BROWSER_CONFIG = ROOT / "world" / "browser_tests" / "playwright.config.js"
BROWSER_SERVER = ROOT / "world" / "browser_tests" / "run-browser-worker.sh"
BROWSER_BATCH_RUNNER = (
    ROOT / "world" / "browser_tests" / "run-browser-test-batches.js"
)
WINDOW_TEST = ROOT / "desktop" / "tests" / "test_window_resize.cpp"


def test_critical_suites_use_runner_packages_without_network_install():
    source = WORKFLOW.read_text(encoding="utf-8")

    assert "python3 tools/run_critical_tests.py worker" in source
    assert "python3 tools/run_critical_tests.py world" in source
    assert "python3 tools/run_critical_tests.py security" in source
    assert "python3 tools/run_critical_tests.py go" in source
    assert "python3 tools/run_critical_tests.py qt" in DESKTOP_WORKFLOW.read_text(
        encoding="utf-8"
    )
    assert "python3 tools/git_conformance.py run" in source
    assert "python3 tools/git_conformance.py render --check" in source
    assert source.count('--transport auto') == 2
    assert "unshare -rn sh -c 'ip link set lo up && exec \"$@\"' sh \"$@\"" in source
    assert source.count("private_loopback python3 tools/git_conformance.py run") == 2
    assert source.count("private_loopback python3 tools/forkmesh_integration.py contract") == 1
    assert "private_loopback python3 -m pytest -q app/tests/test_integration_contract.py" in source
    assert '--transport file' not in source
    assert '--protocol-version 0' in source
    assert '--protocol-version 2' in source
    assert '("pytest", "cryptography")' in source
    assert "FORKMESH_CI_KNOWN_FAILURES=1" not in source
    assert 'export TMPDIR="$ci_tmp"' in source
    assert 'ci_tmp="$PWD/.forkmesh-ci-tmp"' in source
    assert " -m pip install" not in source
    assert "python3 -m venv" not in source


def test_deploy_reuses_ci_instead_of_installing_pytest():
    source = DEPLOY_WORKFLOW.read_text(encoding="utf-8")

    assert "needs: [.forkmesh/ci.yml]" in source
    assert "- name: Verify deploy runtime address space" in source
    assert 'address_kib="$(ulimit -v)"' in source
    assert '"$address_kib" -lt 33554432' in source
    assert source.index("Verify deploy runtime address space") < source.index(
        "Write production env"
    )
    assert "Run Cloudflare Worker tests" not in source
    assert " -m pip install" not in source
    assert "python3 -m venv" not in source


def test_the_deploy_gate_does_not_wait_for_the_desktop_build():
    """A deploy publishes Workers; none of the Qt client reaches Cloudflare.
    While both suites lived in "CI tests" the deploy's `needs:` held every
    push behind a desktop compile — a quarter of an hour cold — before it
    could ship a one-line Worker change. The Qt suite is its own workflow now,
    so the node's runner pool builds it alongside the deploy instead of in
    front of it. Both still run on every push and pull request."""
    deploy = DEPLOY_WORKFLOW.read_text(encoding="utf-8")
    ci = WORKFLOW.read_text(encoding="utf-8")
    desktop = DESKTOP_WORKFLOW.read_text(encoding="utf-8")

    # The gate names the Worker contracts and nothing else. Read the directive
    # itself: the file also mentions the desktop workflow in prose.
    needs = [line for line in deploy.splitlines() if line.startswith("needs:")]
    assert needs == ["needs: [.forkmesh/ci.yml]"]

    # ...which only holds the deploy back if the Qt build has left that file.
    assert "run_critical_tests.py qt" not in ci
    assert "cmake" not in ci
    assert "- name: Critical Qt client contracts" in desktop
    assert desktop.startswith(
        "name: Desktop client CI tests\non: [push, pull_request]\n"
    )
    # A `needs:` here would rebuild the same queue chain one level down.
    assert not [
        line for line in desktop.splitlines() if line.startswith("needs:")
    ]
    assert "needs:" not in desktop


def test_deploy_fails_before_secrets_when_the_sandbox_has_no_egress():
    """Steps run with the network namespace unshared unless the node opts in,
    and the runner never exposes FORKMESH_ACTIONS_ALLOW_NETWORK to a step. Run
    1908 therefore spent ten retries inside deploy.sh and failed the whole run
    with "the live mirror-router identity could not be read safely" — after
    writing every production secret into the sandbox. Probe first and fail: a
    deployment that did not reach Cloudflare must never report green."""
    source = DEPLOY_WORKFLOW.read_text(encoding="utf-8")

    assert "- name: Check deploy egress" in source
    assert "https://app.forkmesh.com/api/mirrors/https" in source
    # Only "cannot resolve" (6) and "cannot connect" (7) are the sandbox's own
    # policy; every other curl failure has to reach deploy.sh and stay red.
    assert 'case "$probe" in' in source
    assert "\n            6|7)\n" in source
    # The probe has to run before the secrets are materialized and must fail the
    # job. deploy.sh compares live fingerprints, so a single-commit diff must
    # never skip Worker changes stranded behind an earlier failed CI run.
    assert source.index("Check deploy egress") < source.index(
        "Write production env"
    )
    no_egress = source.split('            6|7)', 1)[1].split(
        '            *)', 1
    )[0]
    assert "exit 1" in no_egress
    assert "forkmesh-skip-deploy" not in source
    assert "cd app && ./deploy.sh changed" in source
    assert "FORKMESH_ACTIONS_ALLOW_NETWORK" in source


def test_critical_suites_have_isolated_workflow_steps():
    source = WORKFLOW.read_text(encoding="utf-8")

    assert "- name: Critical Cloudflare Worker contracts" in source
    assert "- name: Critical virtual World contracts" in source
    assert "- name: Critical Qt client contracts" in DESKTOP_WORKFLOW.read_text(
        encoding="utf-8"
    )
    assert "Test suites (in parallel)" not in source
    assert "heartbeat()" not in source


def test_qt_step_builds_every_binary_the_critical_label_selects():
    """ctest reports an unbuilt suite as "***Not Run", so a suite promoted to
    the "critical" label without a matching build target fails CI silently.
    The workflow builds one aggregate target that CMake derives from the same
    place it applies the label, so the two lists cannot drift apart."""
    source = DESKTOP_WORKFLOW.read_text(encoding="utf-8")
    cmake = (ROOT / "desktop" / "CMakeLists.txt").read_text(encoding="utf-8")

    assert "--target critical-tests" in source
    # No enumerating suites in the workflow: that is what drifted before.
    assert "--target forkmesh-" not in source
    assert "add_custom_target(critical-tests" in cmake

    labelled = _cmake_list(cmake, "FORKMESH_CRITICAL_TESTS")
    built = _cmake_list(cmake, "FORKMESH_CRITICAL_TEST_TARGETS")
    assert labelled and built
    for test in labelled:
        # A test name is not a target name: the flag-selected suites (e.g.
        # --mirror-fleet-only) are extra runs of one binary. Read the binary
        # out of add_test's COMMAND rather than guessing from the name.
        binary = _test_binary(cmake, test)
        assert binary, f"{test} has no add_test() command"
        assert binary in built, f"{test} has no binary in critical-tests"


def test_focused_mirror_fleet_suite_dispatches_before_generic_window_checks():
    """The focused binary has a 20-second CTest deadline. Its selector must
    run immediately after MainWindow construction rather than after unrelated
    Calendar, Ledger, marketplace, and media fixtures."""
    source = WINDOW_TEST.read_text(encoding="utf-8")
    constructed = source.index("    MainWindow window;")
    dispatched = source.index("    if (mirrorFleetOnly)", constructed)
    generic = source.index(
        "    // The persisted Actions history can contain", constructed
    )

    assert constructed < dispatched < generic
    assert "return runMirrorFleetOnly(window);" in source[dispatched:generic]


def _cmake_list(cmake: str, name: str) -> list[str]:
    _, _, rest = cmake.partition(f"set({name}\n")
    body, _, _ = rest.partition(")")
    return body.split()


def _test_binary(cmake: str, test: str) -> str | None:
    match = re.search(
        rf"add_test\(\s*NAME\s+{re.escape(test)}\s+COMMAND\s+([^\s)]+)", cmake
    )
    return match.group(1) if match else None


def test_qt_builds_pin_autogen_pools_to_the_sandbox_budget():
    """CMake's AUTOGEN_PARALLEL default (AUTO) sizes each AUTOMOC driver's
    thread pool from the host's core count, which no step can see from inside
    the Actions cgroup. One such pool per -j slot exhausted the scope's task
    ceiling and the build died on pthread_create's "Resource temporarily
    unavailable" (adhoc #1586). Both Qt builds pin the pools to the same job
    count the compiles are sized from."""
    for workflow in (DESKTOP_WORKFLOW, RELEASE_WORKFLOW):
        source = workflow.read_text(encoding="utf-8")
        assert '-DCMAKE_AUTOGEN_PARALLEL="$jobs"' in source, workflow.name
        max_jobs = 2 if workflow == DESKTOP_WORKFLOW else 4
        assert (f'[ "$jobs" -gt {max_jobs} ] && jobs={max_jobs}' in source or
                f'if [ "$jobs" -gt {max_jobs} ]; then jobs={max_jobs}; fi' in source), workflow.name
        # Sizing the pools from $jobs only works if $jobs is already computed.
        assert source.index("jobs=") < source.index("-DCMAKE_AUTOGEN_PARALLEL")


def test_qt_step_keeps_its_objects_between_runs():
    """Every directory the Actions sandbox creates is discarded when the run
    ends, so the client recompiled from zero — about fifteen minutes — on every
    push. The runner keeps one writable mount per repository and publishes it
    as FORKMESH_ACTIONS_CACHE; the ccache and the configured CMake tree both
    have to live there, and the step still has to work when the mount is
    absent (disabled on the node, or held by another run of the repository)."""
    source = DESKTOP_WORKFLOW.read_text(encoding="utf-8")
    runner = (ROOT / "desktop/src/ActionRunner.cpp").read_text(encoding="utf-8")

    assert 'cache="${FORKMESH_ACTIONS_CACHE:-$ci_tmp}"' in source
    assert 'export CCACHE_DIR="$cache/ccache"' in source
    assert 'build="$cache/desktop-build-ci"' in source
    assert 'cmake -S desktop -B "$build"' in source
    assert 'cmake --build "$build" -j "$jobs" --target critical-tests' in source
    assert 'run_critical_tests.py qt --build-dir "$build"' in source
    # A kept tree that names another source path is dropped, not configured on.
    assert 'CMAKE_HOME_DIRECTORY:INTERNAL=$PWD/desktop' in source
    assert source.count("rm -rf \"$build\"") == 2

    # The mount, the environment it publishes, and the one-run-at-a-time lock
    # that makes sharing a build tree safe.
    assert 'QStringLiteral("/cache")' in runner
    assert 'env.insert(QStringLiteral("FORKMESH_ACTIONS_CACHE")' in runner
    assert 'env.insert(QStringLiteral("CCACHE_DIR")' in runner
    assert "::flock(fd, LOCK_EX | LOCK_NB)" in runner
    assert "enforceCacheBudget" in runner


def test_critical_suites_enforce_sub_minute_deadlines():
    runner = CRITICAL_RUNNER.read_text(encoding="utf-8")
    package = BROWSER_PACKAGE.read_text(encoding="utf-8")
    config = BROWSER_CONFIG.read_text(encoding="utf-8")
    server = BROWSER_SERVER.read_text(encoding="utf-8")
    batches = BROWSER_BATCH_RUNNER.read_text(encoding="utf-8")

    assert "TEST_BUDGET_SECONDS = 55" in runner
    assert 'SUITE_BUDGET_SECONDS = {"security": 300, "go": 900}' in runner
    assert 'playwright test --grep @critical' in package
    assert "globalTimeout: fullSuite ? 0 : 55_000" in config
    assert '"test:all": "node run-browser-test-batches.js"' in package
    assert 'FORKMESH_BROWSER_FULL: "1"' in batches
    assert "timeout: fullSuite ? 180_000 : 45_000" in config
    assert "python3 -m http.server" in server
    full_suite_branch = 'if [ "${FORKMESH_BROWSER_FULL:-0}" = "1" ]; then'
    assert full_suite_branch in server
    assert "pywrangler dev" in server
    assert server.index(full_suite_branch) < server.index("pywrangler dev")
    assert server.index("exit $?") < server.index("python3 -m http.server")


def test_ci_has_no_failure_quarantine():
    conftests = [
        (ROOT / unit / "tests" / "conftest.py").read_text(encoding="utf-8")
        for unit in ("app", "world")
    ]

    for source in conftests:
        assert "KNOWN_MAIN_FAILURES" not in source
        assert "FORKMESH_CI_KNOWN_FAILURES" not in source
        assert "pytest_collection_modifyitems" not in source
        assert "xfail" not in source


def test_every_pinned_critical_node_id_still_resolves():
    """The world suite pins individual test node ids. Renaming one of those
    tests leaves pytest with an unmatched id, and pytest answers a bad id with
    usage error 4 *before running anything* -- so the whole suite silently
    stops covering CI and the log says only "no match in any of". Run 4325 lost
    all ten world contracts that way, to a rename in 0e8500a7c. Check the ids
    against the files here, in the worker suite that runs first."""
    suites = _pinned_suites()
    assert suites, "run_critical_tests.py exposes no pytest suites"

    for suite, node_ids in suites.items():
        assert node_ids, f"{suite} pins no tests"
        for node_id in node_ids:
            path, _, name = node_id.partition("::")
            target = ROOT / path
            assert target.exists(), f"{suite}: {path} does not exist"
            if not name:
                continue
            defined = {
                node.name
                for node in ast.parse(
                    target.read_text(encoding="utf-8")
                ).body
                if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
            }
            assert name in defined, (
                f"{suite} pins {node_id}, but {path} defines no such test. "
                "Update PYTEST_SUITES in tools/run_critical_tests.py to the "
                "test's new name (pytest would exit 4 and run nothing)."
            )


def _pinned_suites() -> dict[str, tuple[str, ...]]:
    """Read PYTEST_SUITES without importing the runner, which parses argv."""
    module = ast.parse(CRITICAL_RUNNER.read_text(encoding="utf-8"))
    for node in module.body:
        if not isinstance(node, ast.Assign):
            continue
        targets = [t.id for t in node.targets if isinstance(t, ast.Name)]
        if "PYTEST_SUITES" in targets:
            return ast.literal_eval(node.value)
    raise AssertionError("run_critical_tests.py defines no PYTEST_SUITES")


def test_security_guards_and_go_tests_run_in_ci_and_the_quality_gate():
    source = WORKFLOW.read_text(encoding="utf-8")
    suites = _pinned_suites()
    gates = json.loads(QUALITY_GATES.read_text(encoding="utf-8"))
    commands = {gate["id"]: gate["command"] for gate in gates["gates"]}

    assert "- name: Security guard contracts" in source
    assert "- name: Mirror server Go tests" in source
    for path in ("app/tests/test_users_nodes_claim_link.py", "app/tests/test_account_totp_gate.py",
                 "app/tests/test_login_key_binding.py",
                 "app/tests/test_no_unguarded_destructive_deletes.py"):
        assert path in suites["security"]
    assert any(item.startswith("app/tests/test_forkbot.py::") for item in suites["security"])
    assert commands["security-guard-contracts"][1:] == ["tools/run_critical_tests.py", "security"]
    assert commands["mirror-server-go-tests"][1:] == ["tools/run_critical_tests.py", "go"]
    for profile in ("core", "release"):
        assert "security-guard-contracts" in gates["profiles"][profile]
        assert "mirror-server-go-tests" in gates["profiles"][profile]
    runner = CRITICAL_RUNNER.read_text(encoding="utf-8")
    assert '"test", "-C", "server", "./..."' in runner
    assert 'GOPROXY="off"' in runner and 'GOTOOLCHAIN="local"' in runner
