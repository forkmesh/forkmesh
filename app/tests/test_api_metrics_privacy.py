import asyncio
import importlib
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))


def metrics_module():
    sys.modules.pop("api_metrics", None)
    return importlib.import_module("api_metrics")


def test_account_route_groups_never_publish_usernames():
    metrics = metrics_module()
    assert metrics._masked_group("/api/accounts/alice") == "accounts/*"
    assert (
        metrics._masked_group("/api/accounts/alice/followers")
        == "accounts/*"
    )


def test_room_badge_and_unknown_identifiers_are_never_public_groups():
    metrics = metrics_module()
    assert metrics._masked_group("/api/badges/alice") == "badges/*"
    assert metrics._masked_group("/api/room/private-team/ws") == "room/*"
    assert metrics._masked_group("/api/customer-name/private-repo") == "other"
    for group in (
        metrics._masked_group("/api/badges/alice"),
        metrics._masked_group("/api/room/private-team/ws"),
        metrics._masked_group("/api/customer-name/private-repo"),
    ):
        assert "alice" not in group
        assert "private-team" not in group
        assert "customer-name" not in group


def test_traffic_statistics_table_is_dropped_by_a_roll_forward_migration():
    # Per-route traffic is counted by the desktop from the Worker log tail;
    # D1 keeps only the failure-only counter /status reads.
    migration = (ROOT / "migrations/0190_drop_api_metrics_minute.sql").read_text(
        encoding="utf-8"
    )
    assert "DROP TABLE IF EXISTS api_metrics_minute" in migration
    assert "CREATE TABLE IF NOT EXISTS flagship_read_failure_minute" in migration
    down = (
        ROOT / "migrations/down/0190_drop_api_metrics_minute.sql"
    ).read_text(encoding="utf-8")
    assert "CREATE TABLE IF NOT EXISTS api_metrics_minute" in down
    source = (ROOT / "src" / "api_metrics.py").read_text(encoding="utf-8")
    assert "api_metrics_minute" not in source
    assert "metrics_summary_handler" not in source
    runtime = (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
    assert "/api/metrics/summary" not in runtime


def test_flagship_content_reads_have_their_own_bucket_and_nothing_else_does():
    # /status reads this exact group as visitor-side evidence that
    # forkmesh/forkmesh is not loading; it must match the sampler's constant,
    # cover exactly the mirror-routed content reads the repository page
    # renders from, and never leak any other repository's identity.
    metrics = metrics_module()
    assert metrics.FLAGSHIP_ROUTE_GROUP == "repo/forkmesh/forkmesh/content"
    monitoring = (ROOT / "src" / "status_monitoring.py").read_text(
        encoding="utf-8")
    assert (
        'FLAGSHIP_PUBLIC_READ_ROUTE_GROUP = "%s"'
        % metrics.FLAGSHIP_ROUTE_GROUP
    ) in monitoring
    for path in (
        "/api/repo/forkmesh/forkmesh/tree",
        "/api/repo/forkmesh/forkmesh/blob",
        "/api/repo/forkmesh/forkmesh/blobs",
        "/api/repo/forkmesh/forkmesh/raw",
        "/api/repo/forkmesh/forkmesh/history",
        "/api/repo/forkmesh/forkmesh/commit",
        "/api/repo/forkmesh/forkmesh/compare",
        "/api/repo/FORKMESH/ForkMesh/branches",
        "/api/repo/forkmesh/forkmesh/search",
        "/api/repo/forkmesh/forkmesh/stats",
        "/api/repo/forkmesh/forkmesh/sizes",
        "/api/repo/forkmesh/forkmesh/releases/blob/sha256/" + "a" * 64,
    ):
        assert metrics._masked_group(path) == metrics.FLAGSHIP_ROUTE_GROUP, path
    # Not repository content: chat rooms (a Durable Object abort there is the
    # realtime row's business), node/agent endpoints, the Mirrors tab, the
    # retired socket actions, other repositories.
    for path in (
        "/api/repo/forkmesh/forkmesh/rooms/general/ws",
        "/api/repo/forkmesh/forkmesh/agents/list",
        "/api/repo/forkmesh/forkmesh/mirrors",
        "/api/repo/forkmesh/forkmesh/mirrors/mirror23/reachability",
        "/api/repo/forkmesh/forkmesh/host",
        "/api/repo/forkmesh/forkmesh/merge-preview",
        "/api/repo/forkmesh/forkmesh/releases/downloads",
        "/api/repo/forkmesh/forkmesh/tree/extra",
        "/api/repo/forkmesh/forkmesh",
        "/api/repo/alice/forkmesh/tree",
        "/api/repo/forkmesh/other/tree",
    ):
        assert metrics._masked_group(path) == "repo/*", path
    assert metrics._masked_group("/api/repo/forkmesh") == "repo/*"


class _Clock:
    value = 120_000

    @classmethod
    def now(cls):
        return cls.value


def test_only_failed_flagship_reads_reach_d1_at_most_twice_a_minute():
    # The /status sampler reads the failure count at the next minute boundary
    # and calls the repository down at two failed reads. Two writes per
    # isolate-minute (the sampler's threshold), not one per failed request,
    # and nothing at all for served responses or any other route.
    metrics = metrics_module()
    monitoring = (ROOT / "src" / "status_monitoring.py").read_text(
        encoding="utf-8")
    assert metrics.FLAGSHIP_FAILURE_WRITES == 2
    assert "FLAGSHIP_PUBLIC_READ_FAILURE_THRESHOLD = 2" in monitoring
    writes = []

    async def ensure_schema(_env):
        return None

    async def d1_run(_env, sql, *args):
        writes.append((sql, args))

    _Clock.value = 120_000
    metrics.Date = _Clock
    metrics.ensure_schema = ensure_schema
    metrics.d1_run = d1_run

    def inserts():
        return [w for w in writes if w[0].startswith("INSERT")]

    async def scenario():
        await metrics.record_api_request(
            object(), "/api/repo/forkmesh/forkmesh/tree", 200)
        await metrics.record_api_request(object(), "/api/version", 500)
        await metrics.record_api_request(
            object(), "/api/repo/alice/forkmesh/tree", 503)
        assert writes == []                       # nothing but flagship 5xx
        await metrics.record_api_request(
            object(), "/api/repo/forkmesh/forkmesh/tree", 503)
        assert len(inserts()) == 1
        sql, args = inserts()[0]
        assert "INSERT INTO flagship_read_failure_minute" in sql
        assert args == (120_000,)
        # The first write of the minute also prunes; later ones do not.
        prunes = [w for w in writes if w[0].startswith("DELETE")]
        assert len(prunes) == 1
        assert prunes[0][1] == (
            120_000 - metrics.FLAGSHIP_FAILURE_RETENTION_MS,)
        await metrics.record_api_request(
            object(), "/api/repo/forkmesh/forkmesh/blob", 503)
        assert len(inserts()) == 2
        assert len([w for w in writes if w[0].startswith("DELETE")]) == 1
        await metrics.record_api_request(
            object(), "/api/repo/forkmesh/forkmesh/raw", 500)
        assert len(inserts()) == 2                # budget spent this minute
        _Clock.value = 180_000
        await metrics.record_api_request(
            object(), "/api/repo/forkmesh/forkmesh/blob", 503)
        assert len(inserts()) == 3                # next minute: fresh budget
        assert inserts()[-1][1] == (180_000,)

    asyncio.run(scenario())


def test_a_failing_write_never_breaks_serving():
    metrics = metrics_module()

    async def ensure_schema(_env):
        return None

    async def d1_run(_env, sql, *args):
        raise RuntimeError("D1 down")

    metrics.Date = _Clock
    metrics.ensure_schema = ensure_schema
    metrics.d1_run = d1_run
    asyncio.run(metrics.record_api_request(
        object(), "/api/repo/forkmesh/forkmesh/tree", 503))
