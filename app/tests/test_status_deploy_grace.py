"""Deployment handoff grace for flagship repository health alerts."""

import ast
from pathlib import Path
from types import SimpleNamespace


ROOT = Path(__file__).resolve().parents[1]
ENTRY_PATH = ROOT / "src/entry.py"
# The /status sampler + history projection live in the on-demand
# status_monitoring module (kept out of Worker startup).
ENTRY = (
    ENTRY_PATH.read_text(encoding="utf-8") + "\n"
    + ENTRY_PATH.with_name("status_monitoring.py").read_text(encoding="utf-8")
)
DEPLOY = (ROOT / "deploy.sh").read_text(encoding="utf-8")


def _node(name):
    for node in ast.parse(ENTRY, filename=str(ENTRY_PATH)).body:
        if isinstance(node, ast.Assign):
            if any(
                isinstance(target, ast.Name) and target.id == name
                for target in node.targets
            ):
                return node
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            if node.name == name:
                return node
    raise AssertionError(name)


def test_deploy_stamps_an_epoch_and_grace_is_bounded_to_five_minutes():
    assert 'DEPLOYED_AT_MS="$(date -u +%s)000"' in DEPLOY
    assert '--var "DEPLOYED_AT_MS:${DEPLOYED_AT_MS}"' in DEPLOY
    namespace = {}
    module = ast.fix_missing_locations(ast.Module(
        body=[
            _node("STATUS_DEPLOY_GRACE_MS"),
            _node("_deployment_status_grace_active"),
        ],
        type_ignores=[],
    ))
    exec(compile(module, str(ENTRY_PATH), "exec"), namespace)
    grace = namespace["STATUS_DEPLOY_GRACE_MS"]
    active = namespace["_deployment_status_grace_active"]
    assert grace == 5 * 60 * 1000
    env = SimpleNamespace(DEPLOYED_AT_MS=1_000_000)
    assert active(env, 1_000_000)
    assert active(env, 1_000_000 + grace - 1)
    assert not active(env, 1_000_000 + grace)
    assert not active(SimpleNamespace(), 1_000_000)


def test_flagship_probe_applies_deploy_grace_without_delaying_real_outage_pings():
    sample = ENTRY[
        ENTRY.index("async def record_status_sample"):
        ENTRY.index("\n\nasync def status_history")
    ]
    assert sample.count("_deployment_status_grace_active(env, now)") == 2
    # The handoff grace forgives only the probe's own routed reads, and only
    # while no visitor is failing: people getting 5xx from the repository API
    # is an outage whatever the deploy clock says (adhoc #68).
    assert sample.count("not observed_failures") == 2
    assert (
        "not repository_ok\n"
        "            and not observed_failures\n"
        "            and _deployment_status_grace_active(env, now)"
    ) in sample
    assert "_flagship_public_read_failures(env, now)" in sample
    transition = ENTRY[
        ENTRY.index("async def _record_status_monitor_transitions"):
        ENTRY.index("\n\nasync def record_status_sample")
    ]
    # Deployment handoff is suppressed while sampling, not by holding every
    # real repository failure for five minutes in the alert path.
    assert "STATUS_DEPLOY_GRACE_MS" not in transition
    assert 'system_id == "flagship_repository" and is_up' in transition
    assert 'prior_notified != "down"' in transition
    assert 'notified = "up"' in transition


def test_shared_deploy_semaphore_covers_rollout_and_post_ready_grace():
    namespace = {}
    module = ast.fix_missing_locations(ast.Module(
        body=[
            _node("STATUS_DEPLOY_GRACE_MS"),
            _node("STATUS_DEPLOY_MAX_MS"),
        ],
        type_ignores=[],
    ))
    exec(compile(module, str(ENTRY_PATH), "exec"), namespace)
    assert namespace["STATUS_DEPLOY_GRACE_MS"] == 5 * 60 * 1000
    assert namespace["STATUS_DEPLOY_MAX_MS"] == 30 * 60 * 1000

    semaphore = ENTRY[
        ENTRY.index("async def _status_deploy_semaphore_active"):
        ENTRY.index("\n\nasync def _claim_status_sample_minute")
    ]
    assert "SELECT state,started_at,finished_at" in semaphore
    assert 'state == "deploying"' in semaphore
    assert "< STATUS_DEPLOY_MAX_MS" in semaphore
    assert 'state == "ready"' in semaphore
    assert "< STATUS_DEPLOY_GRACE_MS" in semaphore

    sample = ENTRY[
        ENTRY.index("async def record_status_sample"):
        ENTRY.index("\n\nasync def status_history")
    ]
    assert "_status_deploy_semaphore_active(env, now)" in sample
    # The deploy-window minute is neutral for new failures only: rows that
    # were already failing are carried through the rollout (event buckets
    # excepted) and visitor flagship evidence is still read.
    deploy_minute = ENTRY[
        ENTRY.index("async def _record_status_deploy_sample"):
        ENTRY.index("\n\nasync def record_status_sample")
    ]
    assert "_status_carried_failures(env, minute_ts)" in deploy_minute
    assert "_status_deploy_recovered_systems(env, now)" in deploy_minute
    assert "_flagship_public_read_failures(env, now)" in deploy_minute
    assert "_record_status_monitor_transitions" not in deploy_minute


def test_deploy_announces_before_migrations_and_retries_after_schema_setup():
    start = DEPLOY.index("deploy_app_target() {")
    production = DEPLOY[
        start:DEPLOY.index("\n}\n\ninitial_official_cutover_needed", start)
    ]
    first_signal = production.index(
        'if signal_deploy_status deploying "$BUILD_REV"; then')
    migration = production.index("./migrate.sh", first_signal)
    retry_signal = production.index(
        'if ! signal_deploy_status deploying "$BUILD_REV"; then', migration
    )
    assert first_signal < migration < retry_signal
    assert "Deploy alert semaphore: state=$state" in DEPLOY
    assert "deploy semaphore is unavailable after migrations" in production
    assert 'signal_deploy_status ready "$BUILD_REV"' in production

    static_start = DEPLOY.index("deploy_static_target() {")
    static = DEPLOY[static_start:DEPLOY.index("\n}\n\ndeploy_app_version", static_start)]
    assert 'signal_deploy_status deploying "$BUILD_REV"' in static
    assert 'signal_deploy_status ready "$BUILD_REV"' in static
