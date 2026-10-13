"""Public, non-custodial dashboard replica contract."""

import json
from pathlib import Path
import sys

import pytest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "app" / "src"))

import mirror_replica  # noqa: E402


NETWORK = {
    "ok": True,
    "stats": {"ok": True},
    "leaderboards": {"ok": True},
    "history": {"ok": True},
}

ABOUT = {
    "ok": True,
    "description": "Independent repository",
    "website": "https://forkmesh.com",
    "logoUrl": "/api/repo/forkmesh/forkmesh/raw?path=logo.png",
    "logo": {"internalFutureField": "must not be retained"},
    "bannerUrl": "/api/repo/forkmesh/forkmesh/raw?path=.forkmesh/banner.png",
    "defaultLogoUrl": "/assets/fediverse-avatar.png",
    "fediverse": {
        "enabled": True,
        "handle": "@forkmesh.forkmesh@forkmesh.com",
        "actorUrl": "https://forkmesh.com/ap/repos/forkmesh/forkmesh",
        "followers": 4,
        "followersList": [{
            "handle": "alice@example.test",
            "url": "https://example.test/users/alice",
            "name": "Alice",
            "avatarUrl": "https://example.test/alice.png",
            "about": "not needed by the mirror action card",
            "instance": "example.test",
            "profileUrl": "https://example.test/@alice",
            "followedAt": 1_999_999_999_000,
        }],
        "settings": {
            "federate": True,
            "broadcastEvents": True,
            "acceptComments": True,
        },
    },
}


def test_replica_accepts_only_shared_public_snapshot_schemas():
    document = mirror_replica.build_document(
        "/api/network/overview", NETWORK, 2_000_000_000_000)
    assert document["payload"] == NETWORK
    assert len(document["sha256"]) == 64

    private = dict(NETWORK, sessionToken="secret")
    with pytest.raises(mirror_replica.ReplicaError, match="schema"):
        mirror_replica.build_document(
            "/api/network/overview", private, 2_000_000_000_000)
    with pytest.raises(mirror_replica.ReplicaError, match="path"):
        mirror_replica.build_document(
            "/api/accounts/sessions", {"ok": True}, 2_000_000_000_000)


def test_replica_projects_repository_action_stats_without_viewer_or_future_fields():
    star = mirror_replica.build_document(
        "/api/repo/forkmesh/forkmesh/star",
        {"ok": True, "count": 6, "starred": True, "sessionToken": "secret"},
        2_000_000_000_000,
    )["payload"]
    assert star == {"ok": True, "count": 6, "starred": False}

    forks = mirror_replica.build_document(
        "/api/repo/forkmesh/forkmesh/forks",
        {
            "ok": True,
            "count": 1,
            "forks": [{
                "owner": "alice",
                "name": "forkmesh",
                "url": "/alice/forkmesh",
                "createdAt": 1_999_999_999_000,
                "avatarPng": "private-to-the-controller-projection",
            }],
        },
        2_000_000_000_000,
    )["payload"]
    assert forks["count"] == 1
    assert set(forks["forks"][0]) == {"owner", "name", "url", "createdAt"}

    about = mirror_replica.build_document(
        "/api/repo/forkmesh/forkmesh/about", ABOUT, 2_000_000_000_000
    )["payload"]
    assert about["fediverse"]["followers"] == 4
    assert about["fediverse"]["followersList"][0]["name"] == "Alice"
    assert "logo" not in about
    assert "about" not in about["fediverse"]["followersList"][0]

    with pytest.raises(mirror_replica.ReplicaError, match="validation"):
        mirror_replica.build_document(
            "/api/repo/forkmesh/forkmesh/star",
            {"ok": True, "count": -1, "starred": False},
            2_000_000_000_000,
        )


def test_refresh_retains_last_good_documents_during_controller_outage(tmp_path):
    replica = tmp_path / "dashboard-replica.json"
    now = iter((2_000_000_000_000, 2_000_000_100_000))

    def first_fetch(url):
        if url.endswith(mirror_replica.REPLICA_SOURCE_PATH):
            return 404, None
        if url.endswith("/api/network/overview"):
            return 200, NETWORK
        return 503, None

    first = mirror_replica.refresh_bundle(
        replica, "https://app.forkmesh.example",
        fetch=first_fetch, now_ms=lambda: next(now))
    assert first == {
        "ok": True,
        "refreshed": 1,
        "retained": 0,
        "failed": len(mirror_replica.REPLICA_PATHS) - 1,
    }
    assert replica.stat().st_mode & 0o777 == 0o600
    first_raw = replica.read_bytes()

    second = mirror_replica.refresh_bundle(
        replica, "https://app.forkmesh.example",
        fetch=lambda _url: (503, None), now_ms=lambda: next(now))
    assert second["refreshed"] == 0
    assert second["retained"] == 1
    assert replica.read_bytes() == first_raw
    _bundle, documents = mirror_replica.load_bundle(replica)
    assert documents["/api/network/overview"]["payload"] == NETWORK


def test_replica_store_hot_reads_are_disk_free_and_restart_recovers(
    tmp_path, monkeypatch
):
    replica = tmp_path / "dashboard-replica.json"
    mirror_replica.refresh_bundle(
        replica,
        "https://app.forkmesh.example",
        fetch=lambda url: (
            (404, None)
            if url.endswith(mirror_replica.REPLICA_SOURCE_PATH)
            else (200, NETWORK)
            if url.endswith("/api/network/overview")
            else (503, None)
        ),
        now_ms=lambda: 2_000_000_000_000,
    )
    real_load = mirror_replica.load_bundle
    reads = 0

    def counted_load(path):
        nonlocal reads
        reads += 1
        return real_load(path)

    monkeypatch.setattr(mirror_replica, "load_bundle", counted_load)
    store = mirror_replica.ReplicaStore(replica)
    assert reads == 1  # one startup warmup
    for _ in range(5):
        assert store.document("/api/network/overview")["payload"] == NETWORK
    assert reads == 1
    diagnostics = store.diagnostics()
    assert diagnostics["hits"] == 5
    assert diagnostics["diskReads"] == 1
    assert diagnostics["diskReadsAvoided"] == 5

    # Explicit revision invalidation performs exactly one durable reload, then
    # returns to disk-free serving. A process restart independently rebuilds
    # the same last-good state from the atomic bundle.
    store.invalidate(preload=True)
    assert reads == 2
    assert store.document("/api/network/overview") is not None
    assert reads == 2
    restarted = mirror_replica.ReplicaStore(replica)
    assert reads == 3
    assert restarted.document("/api/network/overview")["payload"] == NETWORK


def test_replica_digest_and_permissions_fail_closed(tmp_path):
    replica = tmp_path / "dashboard-replica.json"
    mirror_replica.refresh_bundle(
        replica,
        "https://app.forkmesh.example",
        fetch=lambda url: (
            (404, None)
            if url.endswith(mirror_replica.REPLICA_SOURCE_PATH)
            else (200, NETWORK)
            if url.endswith("/api/network/overview") else (503, None)
        ),
        now_ms=lambda: 2_000_000_000_000,
    )
    value = json.loads(replica.read_text(encoding="utf-8"))
    value["documents"]["/api/network/overview"]["sha256"] = "0" * 64
    replica.write_text(json.dumps(value), encoding="utf-8")
    replica.chmod(0o600)
    with pytest.raises(mirror_replica.ReplicaError, match="digest"):
        mirror_replica.load_bundle(replica)

    replica.chmod(0o644)
    with pytest.raises(mirror_replica.ReplicaError, match="permissions"):
        mirror_replica.load_bundle(replica)


def test_replica_metadata_types_fail_closed(tmp_path):
    replica = tmp_path / "dashboard-replica.json"
    mirror_replica.refresh_bundle(
        replica,
        "https://app.forkmesh.example",
        fetch=lambda url: (
            (404, None)
            if url.endswith(mirror_replica.REPLICA_SOURCE_PATH)
            else (200, NETWORK)
            if url.endswith("/api/network/overview") else (503, None)
        ),
        now_ms=lambda: 2_000_000_000_000,
    )
    value = json.loads(replica.read_text(encoding="utf-8"))
    value["documents"]["/api/network/overview"]["fetchedAt"] = "later"
    replica.write_text(json.dumps(value), encoding="utf-8")
    replica.chmod(0o600)
    with pytest.raises(mirror_replica.ReplicaError, match="metadata"):
        mirror_replica.load_bundle(replica)


@pytest.mark.parametrize(
    "source",
    (
        "http://app.forkmesh.example",
        "https://user@app.forkmesh.example",
        "https://app.forkmesh.example/path",
        "https://app.forkmesh.example?query=1",
        "https://app.forkmesh.example:8443",
    ),
)
def test_refresh_requires_an_exact_https_origin(tmp_path, source):
    with pytest.raises(mirror_replica.ReplicaError, match="HTTPS origin"):
        mirror_replica.refresh_bundle(
            tmp_path / "replica.json", source, fetch=lambda _url: (503, None)
        )


def test_replica_reader_does_not_follow_symlinks(tmp_path):
    target = tmp_path / "target.json"
    target.write_text("{}", encoding="utf-8")
    target.chmod(0o600)
    link = tmp_path / "replica.json"
    link.symlink_to(target)
    with pytest.raises(mirror_replica.ReplicaError, match="readable valid JSON"):
        mirror_replica.load_bundle(link)


def test_replica_source_is_canonicalized():
    assert mirror_replica.normalize_source(
        "https://APP.ForkMesh.Example:443/"
    ) == "https://app.forkmesh.example"


def test_worker_bundle_does_not_recompute_repository_stats():
    source = (ROOT / "app" / "src" / "entry.py").read_text(encoding="utf-8")
    bundle = source[
        source.index("async def _mirror_replica_source_payload"):
        source.index("async def mirror_replica_source_handler")
    ]
    assert "repo_star_handler" not in bundle
    assert "repo_forks_handler" not in bundle
    assert "repo_about_handler" not in bundle
    assert "js_fetch_with_timeout" not in bundle


def test_worker_bundle_keeps_each_snapshot_independent_and_avoids_reparsing():
    source = (ROOT / "app" / "src" / "entry.py").read_text(encoding="utf-8")
    bundle = source[
        source.index("async def _mirror_replica_source_payload"):
        source.index("async def mirror_replica_source_handler")
    ]
    assert "json_valid(payload) AS payload_json_valid" in bundle
    assert "_validated_public_snapshot_text(" in bundle
    assert "except RuntimeError:" in bundle
    assert "continue" in bundle
    assert "_validated_public_snapshot_row(" not in bundle
    assert "_strict_public_snapshot_json(" not in bundle


def test_refresh_uses_one_bundled_controller_request(tmp_path):
    replica = tmp_path / "dashboard-replica.json"
    calls = []
    payloads = {
        path: NETWORK
        for path in mirror_replica.REPLICA_PATHS
    }
    # Keep this focused on transport fan-out; build_document is already
    # covered above with every path-specific validator.
    original = mirror_replica.build_document
    mirror_replica.build_document = lambda path, payload, fetched_at: {
        "fetchedAt": fetched_at,
        "generatedAt": fetched_at,
        "sha256": "a" * 64,
        "payload": payload,
    }
    try:
        result = mirror_replica.refresh_bundle(
            replica,
            "https://app.forkmesh.example",
            fetch=lambda url: (
                calls.append(url) or (
                    200,
                    {
                        "ok": True,
                        "schemaVersion": mirror_replica.REPLICA_SCHEMA_VERSION,
                        "type": mirror_replica.REPLICA_SOURCE_KIND,
                        "generatedAt": 2_000_000_000_000,
                        "documents": payloads,
                    },
                )
            ),
            now_ms=lambda: 2_000_000_000_000,
        )
    finally:
        mirror_replica.build_document = original
    assert result == {
        "ok": True,
        "refreshed": len(mirror_replica.REPLICA_PATHS),
        "retained": 0,
        "failed": 0,
    }
    assert calls == [
        "https://app.forkmesh.example" + mirror_replica.REPLICA_SOURCE_PATH
    ]


def test_partial_bundle_fetches_only_missing_repository_stats(tmp_path):
    replica = tmp_path / "dashboard-replica.json"
    calls = []
    source_documents = {
        path: NETWORK for path in mirror_replica.REPLICA_PATHS
        if path not in mirror_replica.REPLICA_REPOSITORY_STATS_PATHS
    }
    original = mirror_replica.build_document
    mirror_replica.build_document = lambda path, payload, fetched_at: {
        "fetchedAt": fetched_at,
        "generatedAt": fetched_at,
        "sha256": "a" * 64,
        "payload": payload,
    }
    try:
        result = mirror_replica.refresh_bundle(
            replica,
            "https://app.forkmesh.example",
            fetch=lambda url: (
                calls.append(url) or (
                    (200, {
                        "ok": True,
                        "schemaVersion": mirror_replica.REPLICA_SCHEMA_VERSION,
                        "type": mirror_replica.REPLICA_SOURCE_KIND,
                        "generatedAt": 2_000_000_000_000,
                        "documents": source_documents,
                    })
                    if url.endswith(mirror_replica.REPLICA_SOURCE_PATH)
                    else (200, NETWORK)
                )
            ),
            now_ms=lambda: 2_000_000_000_000,
        )
    finally:
        mirror_replica.build_document = original
    assert result == {
        "ok": True,
        "refreshed": len(mirror_replica.REPLICA_PATHS),
        "retained": 0,
        "failed": len(mirror_replica.REPLICA_REPOSITORY_STATS_PATHS),
    }
    assert calls[0].endswith(mirror_replica.REPLICA_SOURCE_PATH)
    assert set(calls[1:]) == {
        "https://app.forkmesh.example" + path
        for path in mirror_replica.REPLICA_REPOSITORY_STATS_PATHS
    }


def test_rate_limited_bundle_does_not_fan_out_legacy_retries(tmp_path):
    calls = []
    with pytest.raises(mirror_replica.ReplicaError, match="no valid"):
        mirror_replica.refresh_bundle(
            tmp_path / "replica.json",
            "https://app.forkmesh.example",
            fetch=lambda url: (calls.append(url) or (429, None)),
            now_ms=lambda: 2_000_000_000_000,
        )
    assert calls == [
        "https://app.forkmesh.example" + mirror_replica.REPLICA_SOURCE_PATH
    ]
