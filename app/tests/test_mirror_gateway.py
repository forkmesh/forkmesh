"""Local read-only mirror gateway and capability boundary contracts."""

import base64
import gzip
import hashlib
import http.client
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import itertools
import json
from pathlib import Path
import random
import re
import shutil
import os
import subprocess
import sys
import threading
import time
from types import SimpleNamespace
from urllib.parse import urlencode

import pytest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import mirror_gateway as gateway  # noqa: E402


PUBLIC_KEY = base64.urlsafe_b64encode(b"N" * 32).decode().rstrip("=")
ROUTER_KEY = base64.urlsafe_b64encode(b"R" * 32).decode().rstrip("=")
SIGNATURE = base64.urlsafe_b64encode(b"S" * 64).decode().rstrip("=")
NOW = 2_000_000_000_000


def run(command, cwd):
    return subprocess.run(
        command,
        cwd=cwd,
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()


def make_bare_repository(tmp_path):
    bare = tmp_path / "mirror.git"
    source = tmp_path / "source"
    run(["git", "init", "--bare", str(bare)], tmp_path)
    run(["git", "init", "-b", "main", str(source)], tmp_path)
    run(["git", "config", "user.email", "alice@example.test"], source)
    run(["git", "config", "user.name", "Alice"], source)
    (source / "README.md").write_text(
        "# Example\n\nsearchable mirror gateway\n", encoding="utf-8"
    )
    (source / "src").mkdir()
    (source / "src" / "main.py").write_text(
        "def greet():\n    return 'hello'\n", encoding="utf-8"
    )
    (source / "src" / "util.js").write_text(
        "export const answer = 42;\n", encoding="utf-8"
    )
    (source / "src" / "main.js").write_text(
        "import { answer } from './util.js';\n"
        "export function value() { return answer; }\n",
        encoding="utf-8",
    )
    (source / "coverage").mkdir()
    (source / "coverage" / "lcov.info").write_text(
        "SF:src/main.js\nLF:4\nLH:3\nend_of_record\n"
        "SF:src/util.js\nDA:1,1\nend_of_record\n"
        "SF:../../outside.py\nLF:1\nLH:1\nend_of_record\n",
        encoding="utf-8",
    )
    (source / "image.bin").write_bytes(b"\x00\x01\x02")
    (source / "logo.png").write_bytes(
        b"\x89PNG\r\n\x1a\n" + b"repository-owned-logo"
    )
    run(["git", "add", "."], source)
    run(["git", "commit", "-m", "Initial files"], source)
    (source / "src" / "main.py").write_text(
        "def greet(name='world'):\n    return f'hello {name}'\n",
        encoding="utf-8",
    )
    run(["git", "add", "."], source)
    run(["git", "commit", "-m", "Improve greeting"], source)
    # One commit behind main (and no commit of its own, so the fixture's
    # commit set is unchanged) so the branch listing has a real behind count.
    run(["git", "branch", "release-preview", "HEAD~1"], source)
    run(["git", "switch", "-c", "forkmesh/pulls"], source)
    (source / ".forkmesh" / "pulls" / "42").mkdir(parents=True)
    (source / ".forkmesh" / "pulls" / "42" / "pull.md").write_text(
        "---\nnumber: 42\nstatus: open\n"
        "title: Metadata branch truth\n---\n\nPinned review.\n",
        encoding="utf-8",
    )
    run(["git", "add", ".forkmesh/pulls/42/pull.md"], source)
    run(["git", "commit", "-m", "Add pull metadata branch"], source)
    run(["git", "switch", "-c", "calendar-merge-leaf"], source)
    run(["git", "commit", "--allow-empty", "-m", "Calendar side commit"], source)
    run(["git", "switch", "forkmesh/pulls"], source)
    run([
        "git", "merge", "--no-ff", "calendar-merge-leaf", "-m",
        "Calendar side merge",
    ], source)
    run(["git", "switch", "-c", "calendar-hidden-operation"], source)
    run(["git", "commit", "--allow-empty", "-m", "Owner-only merge job"], source)
    run(["git", "switch", "main"], source)
    run(["git", "remote", "add", "origin", str(bare)], source)
    run([
        "git", "push", "origin",
        "main", "release-preview", "forkmesh/pulls",
    ], source)
    run([
        "git", "push", "origin",
        "calendar-hidden-operation:refs/forkmesh/merge-jobs/calendar-hidden-operation",
    ], source)
    run(["git", "symbolic-ref", "HEAD", "refs/heads/main"], bare)
    commit = run(["git", "rev-parse", "HEAD"], source)
    return bare, commit


def test_routing_generation_ignores_auxiliary_heads_but_tracks_default_and_tags(
    tmp_path,
):
    bare, main_commit = make_bare_repository(tmp_path)
    routing_before = gateway.routing_refs_sha256(bare)
    full_before = gateway.refs_sha256(bare)

    run([
        "git", "update-ref", "refs/heads/agent/in-flight", main_commit,
    ], bare)
    assert gateway.refs_sha256(bare) != full_before
    assert gateway.routing_refs_sha256(bare) == routing_before

    run(["git", "tag", "v1-routing-test", main_commit], bare)
    assert gateway.routing_refs_sha256(bare) != routing_before


def test_runtime_cleanup_removes_only_gateway_materializations(tmp_path):
    runtime = tmp_path / "runtime"
    runtime.mkdir(mode=0o700)
    stale_one = runtime / "forkmesh-mirror-runtime-a1_b2"
    stale_two = runtime / "forkmesh-mirror-runtime-z9"
    unrelated = runtime / "operator-data"
    stale_one.mkdir()
    stale_two.mkdir()
    unrelated.mkdir()
    (stale_one / "pack").write_bytes(b"stale")
    (unrelated / "keep").write_bytes(b"owned elsewhere")

    assert gateway.cleanup_stale_runtime(runtime) == 2
    assert not stale_one.exists()
    assert not stale_two.exists()
    assert (unrelated / "keep").read_bytes() == b"owned elsewhere"


def signed_manifest(origin="https://mirror.example.test", node="mirror-a"):
    manifest = {
        "schemaVersion": 1,
        "type": "forkmesh.mirror-endpoint",
        "generatedAt": "2026-07-23T10:00:00Z",
        "node": {"name": node, "publicKey": PUBLIC_KEY},
        "endpoint": {
            "origin": origin,
            "healthUrl": origin + "/health",
            "manifestUrl": origin + "/forkmesh-mirror.json",
            "repositoryUrlTemplate": origin + "/{owner}/{repository}",
            "transport": "direct-https",
            "mainProxyMode": "masked",
        },
        "dns": {
            "recordName": "mirror.example.test",
            "proxied": True,
            "workerRoute": "mirror.example.test/*",
        },
        "storage": {
            "repositoryBytesInD1": False,
            "repositoryByteOwner": "independent-mirror-host",
            "d1Purpose": ["service-discovery"],
        },
    }
    payload = gateway._canonical_json(manifest).encode()
    manifest["signature"] = {
        "algorithm": "Ed25519",
        "encoding": "base64url-no-padding",
        "canonicalization": "forkmesh-json-sort-v1",
        "payloadSha256": hashlib.sha256(payload).hexdigest(),
        "value": SIGNATURE,
    }
    return manifest


def write_config(
    tmp_path,
    bare,
    *,
    repositories=None,
    listen_host="127.0.0.1",
):
    manifest_path = tmp_path / "manifest.json"
    manifest_path.write_text(
        json.dumps(signed_manifest()), encoding="utf-8"
    )
    if repositories is None:
        repositories = [
            {
                "owner": "alice",
                "name": "project",
                "visibility": "public",
                "enabled": True,
                "gitDir": str(bare),
                "releaseStore": str(tmp_path / "releases"),
                "integrity": {
                    "expectedRefsSha256": gateway.refs_sha256(bare)
                },
            },
            {
                "owner": "alice",
                "name": "private-project",
                "visibility": "private",
                "enabled": False,
            },
        ]
    config = {
        "schemaVersion": 1,
        "node": {"name": "mirror-a", "publicKey": PUBLIC_KEY},
        "routerPublicKey": ROUTER_KEY,
        "publicOrigin": "https://mirror.example.test",
        "listen": {"host": listen_host, "port": 8790},
        "manifestPath": str(manifest_path),
        "requestVerifierCommand": ["verify-capability"],
        "healthSignerCommand": ["sign-health"],
        "repositories": repositories,
    }
    config_path = tmp_path / "gateway.json"
    config_path.write_text(json.dumps(config), encoding="utf-8")
    return config_path


def write_alias_config(tmp_path, bare):
    config_path = write_config(tmp_path, bare)
    value = json.loads(config_path.read_text(encoding="utf-8"))
    source = value["repositories"][0]
    value["repositories"] = []
    for owner in ("mirror-two", "forkmesh"):
        repository = json.loads(json.dumps(source))
        repository["owner"] = owner
        value["repositories"].append(repository)
    config_path.write_text(json.dumps(value), encoding="utf-8")
    return config_path


class AcceptVerifier:
    def __init__(self):
        self.messages = []

    def verify(self, message, signature):
        self.messages.append((message, signature))
        return signature == SIGNATURE


class HealthSigner:
    def __init__(self):
        self.messages = []

    def sign(self, message):
        self.messages.append(message)
        return SIGNATURE


class FakeArchiveMaterializer:
    def __init__(self, repository):
        self.repository = repository
        self.archives = []
        self.destinations = []

    def materialize(self, archive, destination):
        self.archives.append(archive)
        self.destinations.append(destination)
        return self.repository


def capability_headers(target, method="GET", body=b"", request_id="request_123456"):
    digest = hashlib.sha256(body).hexdigest()
    return {
        "X-ForkMesh-Node": "mirror-a",
        "X-ForkMesh-Request-Id": request_id,
        "X-ForkMesh-Issued-At": str(NOW),
        "X-ForkMesh-Body-Sha256": digest,
        "X-ForkMesh-Signature": SIGNATURE,
    }


@pytest.fixture
def application(tmp_path):
    bare, commit = make_bare_repository(tmp_path)
    releases = tmp_path / "releases"
    release_bytes = b"release-content"
    release_hash = hashlib.sha256(release_bytes).hexdigest()
    release_path = releases / "sha256" / release_hash[:2] / release_hash
    release_path.mkdir(parents=True)
    (release_path / "data").write_bytes(release_bytes)
    config = gateway.load_config(write_config(tmp_path, bare))
    logs = []
    app = gateway.GatewayApplication(
        config,
        verifier=AcceptVerifier(),
        health_signer=HealthSigner(),
        materializer=FakeArchiveMaterializer(bare),
        clock_ms=lambda: NOW,
        log=logs.append,
    )
    yield app, commit, release_hash, logs
    app.close()


def dispatch(
    app,
    operation,
    query=None,
    *,
    request_id,
    method="GET",
    body=b"",
    extra_headers=None,
):
    target = f"/v1/repositories/alice/project/{operation}"
    if query:
        target += "?" + urlencode(query)
    headers = capability_headers(
        target, method=method, body=body, request_id=request_id
    )
    if method == "POST":
        headers["Content-Type"] = "application/x-git-upload-pack-request"
    headers.update(extra_headers or {})
    return app.dispatch(method, target, headers, body)


def test_repository_metadata_hot_read_avoids_second_git_backend(
    application, monkeypatch
):
    app, _commit, _release_hash, _logs = application
    target = "/v1/public/repositories/alice/project/tree?ref=main"
    cold = app.dispatch("GET", target, {}, b"")
    assert cold.status == 200
    assert cold.headers["X-ForkMesh-Memory-Cache"] == "miss"

    def unexpected_git(*_args, **_kwargs):
        raise AssertionError("hot repository metadata read reached Git")

    monkeypatch.setattr(gateway, "_run_git", unexpected_git)
    app.repositories[("alice", "project")]._integrity_checked_at = 0.0
    hot = app.dispatch("GET", target, {}, b"")
    assert hot.status == 200
    assert hot.body == cold.body
    assert hot.headers["X-ForkMesh-Memory-Cache"] == "hit"
    cache = decode_json(app.dispatch("GET", "/health", {}, b""))["cache"]
    repository_cache = cache["repositoryResponses"]
    assert repository_cache["hits"] == 1
    assert repository_cache["backendReads"] == 1
    assert repository_cache["diskReadsAvoided"] == 1
    assert repository_cache["entries"] == 1


def test_startup_prewarm_primes_predictable_repository_metadata(
    application, monkeypatch
):
    app, _commit, _release_hash, _logs = application
    assert app.response_cache.diagnostics()["entries"] == 0
    app.prewarm_memory_caches()
    diagnostics = app.response_cache.diagnostics()
    assert diagnostics["entries"] >= 5
    assert diagnostics["warmups"] >= 5

    def unexpected_git(*_args, **_kwargs):
        raise AssertionError("prewarmed repository metadata reached Git")

    monkeypatch.setattr(gateway, "_run_git", unexpected_git)
    branches = app.dispatch(
        "GET", "/v1/public/repositories/alice/project/branches", {}, b""
    )
    assert branches.status == 200
    assert branches.headers["X-ForkMesh-Memory-Cache"] == "hit"


def test_repository_generation_reload_invalidates_memory_responses(application):
    app, _commit, _release_hash, _logs = application
    target = "/v1/public/repositories/alice/project/branches"
    assert app.dispatch("GET", target, {}, b"").status == 200
    before = app.response_cache.diagnostics()
    assert before["entries"] == 1

    app.reload_from_disk()
    after = app.response_cache.diagnostics()
    assert after["entries"] >= 5
    assert after["invalidations"] == 1
    refreshed = app.dispatch("GET", target, {}, b"")
    assert refreshed.status == 200
    assert refreshed.headers["X-ForkMesh-Memory-Cache"] == "hit"


def test_memory_cache_enforces_entry_and_byte_bounds():
    cache = gateway.BoundedMemoryCache(
        "test", max_entries=2, max_bytes=5)
    assert cache.put("one", b"11", size=2)
    assert cache.put("two", b"22", size=2)
    assert cache.get("one") == (True, b"11")  # make two least-recently used
    assert cache.put("three", b"333", size=3)
    assert cache.get("two") == (False, None)
    assert cache.get("one") == (True, b"11")
    assert cache.get("three") == (True, b"333")
    diagnostics = cache.diagnostics()
    assert diagnostics["entries"] == 2
    assert diagnostics["bytes"] == 5
    assert diagnostics["evictions"] == 1


def test_repository_cache_restart_recovers_from_durable_git(tmp_path):
    bare, _commit = make_bare_repository(tmp_path)
    config = gateway.load_config(write_config(tmp_path, bare))
    target = "/v1/public/repositories/alice/project/tree?ref=main"

    first_app = gateway.GatewayApplication(
        config,
        verifier=AcceptVerifier(),
        health_signer=HealthSigner(),
        clock_ms=lambda: NOW,
        log=lambda _event: None,
    )
    try:
        before_restart = first_app.dispatch("GET", target, {}, b"")
        assert before_restart.status == 200
        assert before_restart.headers["X-ForkMesh-Memory-Cache"] == "miss"
    finally:
        first_app.close()

    restarted = gateway.GatewayApplication(
        gateway.load_config(config.config_path),
        verifier=AcceptVerifier(),
        health_signer=HealthSigner(),
        clock_ms=lambda: NOW,
        log=lambda _event: None,
    )
    try:
        recovered = restarted.dispatch("GET", target, {}, b"")
        assert recovered.status == 200
        assert recovered.body == before_restart.body
        assert recovered.headers["X-ForkMesh-Memory-Cache"] == "miss"
        hot = restarted.dispatch("GET", target, {}, b"")
        assert hot.headers["X-ForkMesh-Memory-Cache"] == "hit"
    finally:
        restarted.close()


def decode_json(response):
    return json.loads(response.body)


def private_replica_bytes(opaque_id):
    envelope = {
        "kind": "forkmesh.mirror",
        "v": 1,
        "alg": "x25519+mlkem768/aes256gcm",
        "nonce": base64.b64encode(b"N" * 12).decode(),
        "tag": base64.b64encode(b"T" * 16).decode(),
        "body": base64.b64encode(b"opaque ciphertext bytes").decode(),
        "recipients": [{
            "kid": hashlib.sha256(b"recipient-public-bundle").hexdigest(),
            "x25519": base64.b64encode(b"X" * 32).decode(),
            "mlkem768": base64.b64encode(b"M" * 64).decode(),
            "nonce": base64.b64encode(b"R" * 12).decode(),
            "tag": base64.b64encode(b"G" * 16).decode(),
            "key": base64.b64encode(b"K" * 48).decode(),
        }],
    }
    stored = {
        "schemaVersion": 1,
        "kind": "forkmesh.private-replica",
        "opaqueId": opaque_id,
        "keyEpoch": 1,
        "createdAt": 1_900_000_000_000,
        "updatedAt": 1_900_000_000_000,
        "ciphertextSha256": hashlib.sha256(
            gateway._canonical_json(envelope).encode()).hexdigest(),
        "envelope": envelope,
    }
    return gateway._canonical_json(stored).encode()


def test_config_requires_loopback_public_integrity_and_no_secret_fields(tmp_path):
    bare, _commit = make_bare_repository(tmp_path)
    with pytest.raises(gateway.GatewayError, match="loopback"):
        gateway.load_config(
            write_config(tmp_path, bare, listen_host="0.0.0.0")
        )

    path = write_config(tmp_path, bare)
    value = json.loads(path.read_text())
    value["privateKey"] = "must-never-be-here"
    path.write_text(json.dumps(value))
    with pytest.raises(gateway.GatewayError, match="secret field"):
        gateway.load_config(path)

    private_enabled = [
        {
            "owner": "alice",
            "name": "private-project",
            "visibility": "private",
            "enabled": True,
            "gitDir": str(bare),
        }
    ]
    with pytest.raises(gateway.GatewayError, match="cannot be enabled"):
        gateway.load_config(
            write_config(tmp_path, bare, repositories=private_enabled)
        )

    plaintext_public = [
        {
            "owner": "alice",
            "name": "project",
            "forkedFrom": "alice/upstream",
            "visibility": "public",
            "enabled": True,
            "gitDir": str(bare),
            "integrity": {
                "expectedRefsSha256": gateway.refs_sha256(bare),
            },
        }
    ]
    loaded = gateway.load_config(
        write_config(tmp_path, bare, repositories=plaintext_public)
    )
    assert loaded.repositories[0].git_dir == bare
    assert loaded.repositories[0].encrypted_archive is None

    invalid_fork_path = write_config(
        tmp_path, bare, repositories=plaintext_public)
    invalid_fork = json.loads(invalid_fork_path.read_text())
    invalid_fork["repositories"][0]["forkedFrom"] = "../../upstream"
    invalid_fork_path.write_text(json.dumps(invalid_fork))
    with pytest.raises(gateway.GatewayError, match="forkedFrom"):
        gateway.load_config(invalid_fork_path)

    ambiguous_path = write_config(tmp_path, bare)
    ambiguous = json.loads(ambiguous_path.read_text())
    ambiguous["repositories"][0]["encryptedArchive"] = {
        "scheme": "age-encrypted-tar-v1"
    }
    ambiguous_path.write_text(json.dumps(ambiguous))
    with pytest.raises(gateway.GatewayError, match="unknown field"):
        gateway.load_config(ambiguous_path)


def test_config_allows_read_only_git_dir_only_in_hosted_import_root(
    tmp_path, monkeypatch
):
    hosted_root = tmp_path / "imports"
    node_root = hosted_root / "mirror-a"
    node_root.mkdir(parents=True)
    bare, _commit = make_bare_repository(node_root)
    repository = node_root / "project.git"
    bare.rename(repository)
    monkeypatch.setattr(gateway, "HOSTED_PUBLIC_GIT_ROOT", hosted_root)
    config = gateway.load_config(
        write_config(
            tmp_path,
            repository,
            repositories=[
                {
                    "owner": "mirror-a",
                    "name": "project",
                    "visibility": "public",
                    "enabled": True,
                    "gitDir": str(repository),
                    "integrity": {
                        "expectedRefsSha256": gateway.refs_sha256(repository),
                    },
                    "operations": ["git-info-refs", "git-upload-pack", "sizes"],
                }
            ],
        )
    )
    assert config.repositories[0].git_dir == repository
    assert config.repositories[0].encrypted_archive is None


def test_identical_alias_plaintext_mirrors_share_git_dir_with_distinct_configs(
    tmp_path,
):
    bare, _commit = make_bare_repository(tmp_path)
    config = gateway.load_config(write_alias_config(tmp_path, bare))
    materializer = FakeArchiveMaterializer(bare)
    app = gateway.GatewayApplication(
        config,
        verifier=AcceptVerifier(),
        health_signer=HealthSigner(),
        materializer=materializer,
    )
    try:
        mirror = app.repositories[("mirror-two", "project")]
        organization = app.repositories[("forkmesh", "project")]
        assert materializer.archives == []
        assert materializer.destinations == []
        assert mirror is not organization
        assert mirror.git_dir == organization.git_dir == bare
        assert mirror.config is config.repositories[0]
        assert organization.config is config.repositories[1]
        assert app.quarantined_count == 0
        health = decode_json(app.dispatch("GET", "/health", {}, b""))
        assert health["publicRepositoryCount"] == 1
    finally:
        app.close()


def test_aliases_can_use_distinct_plaintext_mirrors(tmp_path):
    bare, _commit = make_bare_repository(tmp_path)
    config_path = write_alias_config(tmp_path, bare)
    value = json.loads(config_path.read_text(encoding="utf-8"))
    second = tmp_path / "second.git"
    shutil.copytree(bare, second)
    value["repositories"][1]["gitDir"] = str(second)
    config_path.write_text(json.dumps(value), encoding="utf-8")
    materializer = FakeArchiveMaterializer(bare)
    app = gateway.GatewayApplication(
        gateway.load_config(config_path),
        verifier=AcceptVerifier(),
        health_signer=HealthSigner(),
        materializer=materializer,
    )
    try:
        assert materializer.archives == []
        assert materializer.destinations == []
        assert len(app.repositories) == 2
        assert app.repositories[("mirror-two", "project")].git_dir == bare
        assert app.repositories[("forkmesh", "project")].git_dir == second
        assert app.quarantined_count == 0
    finally:
        app.close()


def test_bad_alias_integrity_is_quarantined_without_poisoning_shared_archive(
    tmp_path,
):
    bare, _commit = make_bare_repository(tmp_path)
    config_path = write_alias_config(tmp_path, bare)
    value = json.loads(config_path.read_text(encoding="utf-8"))
    value["repositories"][0]["integrity"]["expectedRefsSha256"] = "0" * 64
    config_path.write_text(json.dumps(value), encoding="utf-8")
    materializer = FakeArchiveMaterializer(bare)
    app = gateway.GatewayApplication(
        gateway.load_config(config_path),
        verifier=AcceptVerifier(),
        health_signer=HealthSigner(),
        materializer=materializer,
        clock_ms=lambda: NOW,
    )
    try:
        assert materializer.archives == []
        assert ("mirror-two", "project") not in app.repositories
        assert ("forkmesh", "project") in app.repositories
        assert app.quarantined_count == 1

        responses = []
        for index, name in enumerate(("project", "missing")):
            target = f"/v1/repositories/mirror-two/{name}/tree"
            response = app.dispatch(
                "GET",
                target,
                capability_headers(
                    target, request_id=f"quarantined_req_{index:02d}"
                ),
                b"",
            )
            responses.append((response.status, response.body))
        assert responses[0] == responses[1]
        assert responses[0][0] == 404
    finally:
        app.close()


def test_public_plaintext_repositories_do_not_invoke_archive_materializer(
    tmp_path,
):
    bare, _commit = make_bare_repository(tmp_path)
    config = gateway.load_config(write_alias_config(tmp_path, bare))

    class FailingMaterializer:
        def __init__(self):
            self.calls = []

        def materialize(self, archive, destination):
            self.calls.append((archive, destination))
            raise gateway.GatewayError("test materialization failure")

    materializer = FailingMaterializer()
    app = gateway.GatewayApplication(
        config,
        verifier=AcceptVerifier(),
        health_signer=HealthSigner(),
        materializer=materializer,
    )
    try:
        assert materializer.calls == []
        assert len(app.repositories) == 2
        assert app.quarantined_count == 0
    finally:
        app.close()


def test_manifest_digest_identity_and_origin_are_validated(tmp_path):
    bare, _commit = make_bare_repository(tmp_path)
    config_path = write_config(tmp_path, bare)
    config = json.loads(config_path.read_text())
    manifest_path = Path(config["manifestPath"])
    manifest = json.loads(manifest_path.read_text())
    manifest["endpoint"]["origin"] = "https://attacker.example"
    manifest_path.write_text(json.dumps(manifest))
    with pytest.raises(gateway.GatewayError, match="identity/origin"):
        gateway.load_config(config_path)


def test_capability_message_is_byte_compatible_with_edge_router():
    sys.path.insert(0, str(ROOT / "app" / "src"))
    import edge_routing

    arguments = (
        "mirror-a",
        "GET",
        "/v1/repositories/alice/project/tree?path=src",
        hashlib.sha256(b"").hexdigest(),
        "request_123456",
        NOW,
    )
    assert gateway.request_message(*arguments) == edge_routing.request_message(
        *arguments
    )


def test_capability_binds_exact_target_body_time_and_is_single_use(application):
    app, _commit, _release_hash, _logs = application
    target = "/v1/repositories/alice/project/tree?path=src"
    headers = capability_headers(target, request_id="request_123456")
    first = app.dispatch("GET", target, headers, b"")
    assert first.status == 200
    replay = app.dispatch("GET", target, headers, b"")
    assert replay.status == 404

    wrong_target = target + "&ref=main"
    bad = app.dispatch("GET", wrong_target, headers, b"")
    assert bad.status == 404

    stale_headers = capability_headers(
        target, request_id="request_654321"
    )
    stale_headers["X-ForkMesh-Issued-At"] = str(NOW - 60_001)
    assert app.dispatch("GET", target, stale_headers, b"").status == 404


def test_opaque_private_replica_stream_requires_capability_and_exact_ciphertext(
    tmp_path,
):
    bare, _commit = make_bare_repository(tmp_path)
    store = tmp_path / "private-replicas"
    store.mkdir(mode=0o700)
    opaque_id = "a" * 64
    raw = private_replica_bytes(opaque_id)
    replica = store / (opaque_id + ".fm-private")
    replica.write_bytes(raw)
    replica.chmod(0o600)
    config_path = write_config(tmp_path, bare)
    value = json.loads(config_path.read_text())
    value["privateReplicaStore"] = str(store)
    config_path.write_text(json.dumps(value))
    logs = []
    app = gateway.GatewayApplication(
        gateway.load_config(config_path),
        verifier=AcceptVerifier(),
        health_signer=HealthSigner(),
        materializer=FakeArchiveMaterializer(bare),
        clock_ms=lambda: NOW,
        log=logs.append,
    )
    try:
        target = "/v1/private-replicas/" + opaque_id
        unauthorized = app.dispatch("GET", target, {}, b"")
        assert unauthorized.status == 404
        response = app.dispatch(
            "GET",
            target,
            capability_headers(target, request_id="private_cipher_01"),
            b"",
        )
        assert response.status == 200
        assert response.content_type == (
            "application/vnd.forkmesh.private-replica+json")
        assert response.stream.path == replica
        assert response.headers["ETag"] == (
            '"sha256-' + hashlib.sha256(raw).hexdigest() + '"')

        missing_target = "/v1/private-replicas/" + ("b" * 64)
        missing = app.dispatch(
            "GET",
            missing_target,
            capability_headers(
                missing_target, request_id="private_cipher_02"),
            b"",
        )
        assert missing.status == unauthorized.status
        assert missing.body == unauthorized.body
        serialized = json.dumps(logs)
        assert opaque_id not in serialized
        assert "private-project" not in serialized
    finally:
        app.close()


def test_private_replica_rejects_tamper_and_path_traversal(tmp_path):
    store = tmp_path / "private-replicas"
    store.mkdir(mode=0o700)
    opaque_id = "c" * 64
    value = json.loads(private_replica_bytes(opaque_id))
    value["envelope"]["body"] = base64.b64encode(
        b"tampered ciphertext").decode()
    raw = gateway._canonical_json(value).encode()
    (store / (opaque_id + ".fm-private")).write_bytes(raw)
    with pytest.raises(gateway.GatewayError, match="not found"):
        gateway.private_replica_file(store, opaque_id, len(raw) + 1)
    with pytest.raises(gateway.GatewayError, match="not found"):
        gateway.private_replica_file(store, "../secret", 1024)


def test_health_challenge_is_domain_separated_and_externally_signed(application):
    app, _commit, _release_hash, _logs = application
    nonce = "0123456789abcdef"
    response = app.dispatch(
        "GET", f"/health?nonce={nonce}&issuedAt={NOW}", {}, b""
    )
    payload = decode_json(response)
    assert response.status == 200
    assert payload["integrity"] == "ok"
    assert payload["publicRepositoryCount"] == 1
    assert "repositories" not in payload
    assert payload["challenge"]["signature"] == SIGNATURE
    assert app.health_signer.messages == [
        f"forkmesh-https-health-v1\nmirror-a\n{nonce}\n{NOW}"
    ]


def test_plain_health_is_public_cors_for_peer_diagnostics(application):
    app, _commit, _release_hash, _logs = application
    for origin in (
        "https://forkmesh.com",
        "https://app.forkmesh.com",
        "https://mirror37.forkmesh.com",
        "https://diagnostics.example",
    ):
        plain = app.dispatch("GET", "/health", {"Origin": origin}, b"")
        assert plain.headers["Access-Control-Allow-Origin"] == "*"
    plain = app.dispatch("GET", "/health", {}, b"")
    assert "Access-Control-Allow-Origin" not in plain.headers
    challenge = app.dispatch(
        "GET",
        "/health?nonce=cors-signed-check&issuedAt=%d" % NOW,
        {"Origin": "https://forkmesh.com"},
        b"",
    )
    assert "Access-Control-Allow-Origin" not in challenge.headers
    repository = app.dispatch(
        "GET", "/v1/public/repositories/alice/project/branches", {}, b""
    )
    assert "Access-Control-Allow-Origin" not in repository.headers


def test_signed_repository_health_proof_binds_forkmesh_identity_and_refs(
    application,
):
    app, _commit, _release_hash, _logs = application
    nonce = "repository-proof-01"
    response = app.dispatch(
        "GET",
        (
            f"/health?nonce={nonce}&issuedAt={NOW}"
            "&owner=alice&repo=project"
        ),
        {},
        b"",
    )
    payload = decode_json(response)
    proof = payload["repositoryProof"]
    repository = app.repositories[("alice", "project")]
    assert proof["available"] is True
    assert proof["integrity"] == "ok"
    assert proof["refsSha256"] == gateway.routing_refs_sha256(
        repository.git_dir)
    assert {"git-info-refs", "git-upload-pack", "tree", "raw"} <= set(
        proof["operations"]
    )
    expected = gateway.repository_health_challenge(
        "mirror-a",
        nonce,
        NOW,
        "alice",
        "project",
        available=True,
        integrity="ok",
        refs_digest=proof["refsSha256"],
        operations_digest=proof["operationsSha256"],
    )
    assert app.health_signer.messages[-1] == expected
    assert payload["challenge"]["messageType"] == (
        "forkmesh-https-health-repository-v1"
    )


def test_health_tolerates_auxiliary_head_churn_but_not_main_changes(application):
    app, _commit, _release_hash, _logs = application
    repository = app.repositories[("alice", "project")]
    trusted_routing = gateway.routing_refs_sha256(repository.git_dir)
    main_commit = run(
        ["git", "rev-parse", "refs/heads/main"], repository.git_dir)

    run([
        "git", "update-ref", "refs/heads/agent/in-flight", main_commit,
    ], repository.git_dir)
    response = app.dispatch(
        "GET",
        (
            f"/health?nonce=auxiliary-proof-01&issuedAt={NOW}"
            "&owner=alice&repo=project"
        ),
        {},
        b"",
    )
    proof = decode_json(response)["repositoryProof"]
    assert proof["available"] is True
    assert proof["refsSha256"] == trusted_routing

    divergent = run(
        ["git", "rev-parse", "refs/heads/forkmesh/pulls"],
        repository.git_dir,
    )
    assert divergent != main_commit
    run([
        "git", "update-ref", "refs/heads/main", divergent,
    ], repository.git_dir)
    response = app.dispatch(
        "GET",
        (
            f"/health?nonce=default-change-01&issuedAt={NOW}"
            "&owner=alice&repo=project"
        ),
        {},
        b"",
    )
    proof = decode_json(response)["repositoryProof"]
    assert proof["available"] is False
    assert proof["integrity"] == "unavailable"


def test_unknown_and_private_health_proofs_are_uniform_signed_unavailable(
    application,
):
    app, _commit, _release_hash, _logs = application
    proofs = []
    messages = []
    for index, name in enumerate(("private-project", "missing-project")):
        nonce = f"unavailable-proof-{index}"
        response = app.dispatch(
            "GET",
            (
                f"/health?nonce={nonce}&issuedAt={NOW}"
                f"&owner=alice&repo={name}"
            ),
            {},
            b"",
        )
        payload = decode_json(response)
        proof = dict(payload["repositoryProof"])
        proof.pop("repository")
        proofs.append(proof)
        messages.append(app.health_signer.messages[-1])
    assert proofs[0] == proofs[1]
    assert proofs[0]["available"] is False
    assert proofs[0]["integrity"] == "unavailable"
    assert all("private-project" not in json.dumps(proof) for proof in proofs)
    assert all(message.startswith("forkmesh-https-health-repository-v1") for message in messages)


def test_private_unknown_and_quarantined_repositories_are_indistinguishable(
    application,
):
    app, _commit, _release_hash, logs = application
    results = []
    for index, name in enumerate(("private-project", "missing-project")):
        target = f"/v1/repositories/alice/{name}/tree"
        response = app.dispatch(
            "GET",
            target,
            capability_headers(target, request_id=f"private_req_{index:04d}"),
            b"",
        )
        results.append((response.status, response.body))
    assert results[0] == results[1]
    assert b"private-project" not in results[0][1]
    assert all("owner" not in row and "repository" not in row for row in logs)


def test_search_for_a_common_word_stops_at_the_result_cap(tmp_path):
    source = tmp_path / "common"
    run(["git", "init", "-b", "main", str(source)], tmp_path)
    run(["git", "config", "user.email", "alice@example.test"], source)
    run(["git", "config", "user.name", "Alice"], source)
    (source / "many.txt").write_text(
        "".join(f"ForkMesh line {index} {'x' * 80}\n" for index in range(60_000)),
        encoding="utf-8",
    )
    run(["git", "add", "."], source)
    run(["git", "commit", "-m", "Common word everywhere"], source)
    commit = run(["git", "rev-parse", "HEAD"], source)
    output = subprocess.run(
        ["git", "grep", "-n", "-I", "-i", "-F", "-e", "forkmesh", commit, "--"],
        cwd=source, check=True, capture_output=True,
    ).stdout
    assert len(output) > gateway.MAX_SEARCH_OUTPUT_BYTES
    bare = tmp_path / "common.git"
    run(["git", "clone", "--bare", str(source), str(bare)], tmp_path)
    repository = object.__new__(gateway.GitRepository)
    repository.git_dir = bare
    repository.resolve_commit = lambda _ref: commit
    started = time.monotonic()
    payload = repository.search({"path": "forkmesh"})
    assert time.monotonic() - started < 10
    assert payload["ok"] is True
    assert payload["truncated"] is True
    assert len(payload["code"]) == gateway.MAX_SEARCH_RESULTS
    assert payload["code"][0] == {
        "path": "many.txt",
        "line": 1,
        "snippet": "ForkMesh line 0 " + "x" * 80,
    }
    assert repository.search({"path": "absent-needle"})["code"] == []


def test_tree_blob_history_commit_branches_search_stats_and_sizes(application):
    app, commit, _release_hash, _logs = application
    cases = [
        ("tree", {"path": "src"}, "entries"),
        ("blob", {"path": "README.md"}, "content"),
        ("history", {}, "commits"),
        ("commit", {"path": commit}, "files"),
        ("branches", {}, "branches"),
        ("search", {"path": "searchable"}, "code"),
        ("stats", {}, "extensions"),
        ("sizes", {}, "fileCount"),
    ]
    for index, (operation, query, field) in enumerate(cases):
        response = dispatch(
            app,
            operation,
            query,
            request_id=f"operation_{index:04d}",
        )
        assert response.status == 200, (operation, response.body)
        payload = decode_json(response)
        assert payload["ok"] is True
        assert field in payload
    tree = decode_json(
        dispatch(
            app,
            "tree",
            {},
            request_id="tree_exact_commit_time",
        )
    )
    assert re.fullmatch(
        r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}[+-]\d{2}:\d{2}",
        tree["latestCommit"]["date"],
    )
    assert all(
        re.fullmatch(
            r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}[+-]\d{2}:\d{2}",
            commit["date"],
        )
        for commit in decode_json(
            dispatch(
                app,
                "history",
                {},
                request_id="history_exact_commit_times",
            )
        )["commits"]
    )
    history = decode_json(
        dispatch(
            app,
            "history",
            {"ref": commit},
            request_id="history_activity_windows",
        )
    )
    branch_history = decode_json(
        dispatch(
            app,
            "history",
            {"ref": "forkmesh/pulls"},
            request_id="history_named_branch",
        )
    )
    assert branch_history["commit"] != commit
    assert branch_history["commits"][0]["subject"] == "Calendar side merge"
    assert {item["subject"] for item in branch_history["commits"]} >= {
        "Calendar side commit", "Add pull metadata branch", "Improve greeting",
    }
    default_history = decode_json(
        dispatch(
            app,
            "history",
            {},
            request_id="history_default_branch",
        )
    )
    assert default_history["commits"][0]["hash"] == commit
    assert "Calendar side merge" not in {
        item["subject"] for item in default_history["commits"]
    }
    assert history["commit"] == commit
    assert history["activity"]["timezone"] == "UTC"
    assert history["activity"]["weekStartsOn"] == "monday"
    assert set(history["activity"]["windows"]) == {"today", "week", "month"}
    assert all(
        window["commits"] == 2
        for window in history["activity"]["windows"].values()
    )
    assert all(
        window["start"].endswith("+00:00")
        for window in history["activity"]["windows"].values()
    )
    stats = decode_json(
        dispatch(
            app,
            "stats",
            {},
            request_id="stats_complete_insights",
        )
    )
    assert stats["commitCount"] == 2
    assert stats["contributorCount"] == 1
    assert stats["contributorsTruncated"] is False
    assert stats["contributors"][0]["name"] == "Alice"
    assert stats["contributors"][0]["commits"] == 2
    assert len(stats["contributors"][0]["activityWeeks"]) == 52
    assert sum(stats["contributors"][0]["activityWeeks"]) == 2
    assert stats["fileCount"] == 7
    assert stats["totalBytes"] > 0
    assert stats["counts"]["openPulls"] == 1
    now = int(time.time())
    all_ref_history = decode_json(
        dispatch(
            app,
            "history",
            {"all": "1", "since": str(now - 86400),
             "until": str(now + 86400)},
            request_id="history_all_refs_for_calendar",
        )
    )
    assert len(all_ref_history["commits"]) == 5
    assert {item["subject"] for item in all_ref_history["commits"]} >= {
        "Calendar side commit", "Calendar side merge",
    }
    assert "Owner-only merge job" not in {
        item["subject"] for item in all_ref_history["commits"]
    }
    assert sum(
        len(item["parents"].split()) > 1
        for item in all_ref_history["commits"]
    ) == 1
    assert all(
        window["commits"] == 5
        for window in all_ref_history["activity"]["windows"].values()
    )
    raw = dispatch(
        app,
        "raw",
        {"path": "logo.png"},
        request_id="raw_repository_logo",
    )
    assert raw.status == 200
    assert raw.content_type == "image/png"
    assert raw.stream is not None
    assert raw.stream.content_type == "image/png"
    blob = decode_json(
        dispatch(
            app,
            "blob",
            {"path": "README.md"},
            request_id="blob_contents_01",
        )
    )
    assert "searchable mirror gateway" in blob["content"]


def test_root_tree_reports_exact_open_issue_and_pull_counts(tmp_path):
    bare, _commit = make_bare_repository(tmp_path)
    source = tmp_path / "source"

    run(["git", "switch", "main"], source)
    issue_records = {
        "open/7/issue-7.json": {"number": 7, "status": "open"},
        "closed/8/issue-8.json": {"number": 8, "status": "closed"},
        "9/issue-9.json": {"number": 9, "status": "closed"},
        "10/issue-10.json": {"number": 10, "status": "reopened"},
    }
    for relative, record in issue_records.items():
        path = source / ".forkmesh" / "issues" / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(record), encoding="utf-8")
    discussion = source / ".forkmesh" / "discussions" / "3" / "discussion.md"
    discussion.parent.mkdir(parents=True)
    discussion.write_text("---\nnumber: 3\n---\n", encoding="utf-8")
    run(["git", "add", ".forkmesh"], source)
    run(["git", "commit", "-m", "Add collaboration records"], source)
    run(["git", "push", "origin", "main"], source)

    run(["git", "switch", "forkmesh/pulls"], source)
    closed_pull = source / ".forkmesh" / "pulls" / "43" / "pull.md"
    closed_pull.parent.mkdir(parents=True)
    closed_pull.write_text(
        "---\nnumber: 43\nstatus: closed\ntitle: Finished\n---\n",
        encoding="utf-8",
    )
    merged_pull = source / ".forkmesh" / "pulls" / "44" / "pull.md"
    merged_pull.parent.mkdir(parents=True)
    merged_pull.write_text(
        "---\nnumber: 44\nstatus: merged\ntitle: Shipped\n---\n",
        encoding="utf-8",
    )
    run(["git", "add", ".forkmesh/pulls/43/pull.md",
         ".forkmesh/pulls/44/pull.md"], source)
    run(["git", "commit", "-m", "Finish pulls"], source)
    run(["git", "push", "origin", "forkmesh/pulls"], source)

    config = gateway.load_config(write_config(tmp_path, bare))
    app = gateway.GatewayApplication(
        config,
        verifier=AcceptVerifier(),
        health_signer=HealthSigner(),
        materializer=FakeArchiveMaterializer(bare),
        clock_ms=lambda: NOW,
        log=lambda _record: None,
    )
    try:
        payload = decode_json(dispatch(
            app,
            "tree",
            {},
            request_id="root_collaboration_counts_01",
        ))
    finally:
        app.close()

    assert payload["counts"] == {
        "issues": 4,
        "openIssues": 2,
        "closedIssues": 2,
        "pulls": 3,
        "openPulls": 1,
        "mergedPulls": 1,
        "closedPulls": 2,
        "closedUnmergedPulls": 1,
        "discussions": 1,
    }


def test_pull_listing_advertises_branch_only_pulls_the_badge_counts(tmp_path):
    # Issue #683: the root openPulls badge unions the aggregate forkmesh/pulls
    # index with every per-pull forkmesh/pull/<n> branch, but the web list only
    # read the index, so a pull the index lagged was counted and never listed.
    bare, _commit = make_bare_repository(tmp_path)
    source = tmp_path / "source"

    run(["git", "switch", "forkmesh/pulls"], source)
    indexed = source / ".forkmesh" / "pulls" / "43" / "pull.md"
    indexed.parent.mkdir(parents=True)
    indexed.write_text(
        "---\nnumber: 43\nstatus: open\ntitle: Indexed\n---\n",
        encoding="utf-8",
    )
    run(["git", "add", ".forkmesh/pulls/43/pull.md"], source)
    run(["git", "commit", "-m", "pull #43"], source)
    run(["git", "push", "origin", "forkmesh/pulls"], source)

    run(["git", "switch", "--orphan", "forkmesh/pull/45"], source)
    branch_only = source / ".forkmesh" / "pulls" / "45" / "pull.md"
    branch_only.parent.mkdir(parents=True, exist_ok=True)
    branch_only.write_text(
        "---\nnumber: 45\nstatus: open\ntitle: Branch only\n---\n",
        encoding="utf-8",
    )
    run(["git", "add", ".forkmesh/pulls/45/pull.md"], source)
    run(["git", "commit", "-m", "pull #45: metadata snapshot"], source)
    run(["git", "push", "origin", "forkmesh/pull/45"], source)
    branch_commit = run(["git", "rev-parse", "HEAD"], source)

    config = gateway.load_config(write_config(tmp_path, bare))
    app = gateway.GatewayApplication(
        config,
        verifier=AcceptVerifier(),
        health_signer=HealthSigner(),
        materializer=FakeArchiveMaterializer(bare),
        clock_ms=lambda: NOW,
        log=lambda _record: None,
    )
    try:
        root = decode_json(dispatch(
            app, "tree", {}, request_id="branch_only_pull_counts_01"))
        listing = decode_json(dispatch(
            app,
            "tree",
            {"path": ".forkmesh/pulls", "ref": "forkmesh/pulls"},
            request_id="branch_only_pull_listing_01",
        ))
        record = decode_json(dispatch(
            app,
            "blob",
            {"path": ".forkmesh/pulls/45/pull.md", "ref": branch_commit},
            request_id="branch_only_pull_record_01",
        ))
    finally:
        app.close()

    # #42 (fixture) and #43 are indexed; #45 exists only on its own branch.
    assert root["counts"]["openPulls"] == 3
    assert "pullRefs" not in root
    assert [entry["name"] for entry in listing["entries"]] == ["42", "43"]
    assert listing["pullRefs"] == {"45": branch_commit}
    assert "status: open" in record["content"]



def test_pull_listing_carries_each_pulls_reviews_in_file_order(tmp_path):
    # The web pull list shows each pull's review state. The listing carries
    # every committed review's (reviewer, state) so the list reads no per-pull
    # folder; the dashboard applies the merge box's latest-review rule.
    bare, _commit = make_bare_repository(tmp_path)
    source = tmp_path / "source"
    run(["git", "switch", "forkmesh/pulls"], source)
    folder = source / ".forkmesh" / "pulls" / "43"
    folder.mkdir(parents=True)
    (folder / "pull.md").write_text(
        "---\nnumber: 43\nstatus: open\nauthor: bob-key\n---\n",
        encoding="utf-8")
    review = "---\ntype: review\nauthor: %s\nstate: %s\nsig: s\n---\n\nstate: approved\n"
    (folder / "0001-review.md").write_text(
        review % ("carol-key", "changes_requested"), encoding="utf-8")
    (folder / "0002-comment.md").write_text(
        "---\ntype: comment\nauthor: dave-key\n---\n\nHi\n", encoding="utf-8")
    (folder / "0003-review.md").write_text(
        review % ("carol-key", "approved"), encoding="utf-8")
    (folder / "0004-review.md").write_text(
        "---\ntype: review\nauthor: \nstate: approved\n---\n", encoding="utf-8")
    run(["git", "add", ".forkmesh/pulls/43"], source)
    run(["git", "commit", "-m", "pull #43 reviews"], source)
    run(["git", "push", "origin", "forkmesh/pulls"], source)
    ledger = run(["git", "rev-parse", "HEAD"], source)

    config = gateway.load_config(write_config(tmp_path, bare))
    app = gateway.GatewayApplication(
        config,
        verifier=AcceptVerifier(),
        health_signer=HealthSigner(),
        materializer=FakeArchiveMaterializer(bare),
        clock_ms=lambda: NOW,
        log=lambda _record: None,
    )
    try:
        listing = decode_json(dispatch(
            app, "tree", {"path": ".forkmesh/pulls", "ref": ledger},
            request_id="pull_listing_reviews_01"))
        folder_listing = decode_json(dispatch(
            app, "tree", {"path": ".forkmesh/pulls/43", "ref": ledger},
            request_id="pull_listing_reviews_02"))
    finally:
        app.close()

    assert listing["pullReviews"]["43"] == [
        ["carol-key", "changes_requested"], ["carol-key", "approved"]]
    assert set(listing["pullReviews"]) == {"43"}
    assert "pullReviews" not in folder_listing

def _commit_at(source, when, message):
    env = dict(os.environ, GIT_AUTHOR_DATE="@%d +0000" % when,
               GIT_COMMITTER_DATE="@%d +0000" % when)
    subprocess.run(["git", "commit", "-m", message], cwd=source, check=True,
                   capture_output=True, env=env)


def _pull_branch(source, number, when):
    run(["git", "switch", "--orphan", "forkmesh/pull/%d" % number], source)
    record = source / ".forkmesh" / "pulls" / str(number) / "pull.md"
    record.parent.mkdir(parents=True, exist_ok=True)
    record.write_text("---\nnumber: %d\nstatus: open\ntitle: T\n---\n" % number,
                      encoding="utf-8")
    run(["git", "add", str(record.relative_to(source))], source)
    _commit_at(source, when, "pull #%d: metadata snapshot" % number)
    run(["git", "push", "origin", "forkmesh/pull/%d" % number], source)
    return run(["git", "rev-parse", "HEAD"], source)


def test_pulls_deleted_from_the_index_stop_counting_their_leftover_branch(tmp_path):
    bare, _commit = make_bare_repository(tmp_path)
    source = tmp_path / "source"
    _pull_branch(source, 45, 1_790_000_000)
    reused = _pull_branch(source, 46, 1_790_000_900)

    run(["git", "switch", "forkmesh/pulls"], source)
    for number in (45, 46):
        record = source / ".forkmesh" / "pulls" / str(number) / "pull.md"
        record.parent.mkdir(parents=True, exist_ok=True)
        record.write_text("---\nnumber: %d\nstatus: open\n---\n" % number,
                          encoding="utf-8")
    run(["git", "add", ".forkmesh/pulls"], source)
    _commit_at(source, 1_790_000_100, "pulls #45, #46")
    run(["git", "rm", "-q", "-r", ".forkmesh/pulls/45", ".forkmesh/pulls/46"], source)
    _commit_at(source, 1_790_000_500, "pulls: deleted #45, #46")
    run(["git", "push", "origin", "forkmesh/pulls"], source)

    config = gateway.load_config(write_config(tmp_path, bare))
    app = gateway.GatewayApplication(
        config,
        verifier=AcceptVerifier(),
        health_signer=HealthSigner(),
        materializer=FakeArchiveMaterializer(bare),
        clock_ms=lambda: NOW,
        log=lambda _record: None,
    )
    try:
        root = decode_json(dispatch(
            app, "tree", {}, request_id="deleted_pull_counts_01"))
        listing = decode_json(dispatch(
            app,
            "tree",
            {"path": ".forkmesh/pulls", "ref": "forkmesh/pulls"},
            request_id="deleted_pull_listing_01",
        ))
    finally:
        app.close()

    # #42 (fixture) is indexed; #45 was deleted after its branch last moved;
    # #46's branch moved again after the deletion, so it is a live pull.
    assert root["counts"]["openPulls"] == 2
    assert root["counts"]["pulls"] == 2
    assert listing["pullRefs"] == {"46": reused}


def test_branches_returns_main_and_additional_heads(application):
    app, _commit, _release_hash, _logs = application
    payload = decode_json(
        dispatch(
            app,
            "branches",
            {},
            request_id="branches_records_01",
        )
    )
    assert [branch["name"] for branch in payload["branches"]] == [
        "main",
        "release-preview",
    ]
    assert all(branch["commit"] for branch in payload["branches"])
    assert all(branch["updatedAt"] for branch in payload["branches"])
    # Divergence from the default branch travels with the listing, so a client
    # can show "behind / ahead" per branch without one request per branch.
    assert payload["defaultBranch"] == "main"
    rows = {branch["name"]: branch for branch in payload["branches"]}
    assert (rows["main"]["ahead"], rows["main"]["behind"]) == (0, 0)
    assert (rows["release-preview"]["ahead"],
            rows["release-preview"]["behind"]) == (0, 1)


def test_branch_listing_counts_divergence_and_degrades_without_the_atom(
    application, monkeypatch,
):
    app, _commit, _release_hash, _logs = application
    repository = app.repositories[("alice", "project")]
    # A head that is both ahead of and behind main: the pull metadata branch
    # carries its own merge commits and misses main's newest one.
    run([
        "git", "update-ref", "refs/heads/feature-ahead",
        "refs/heads/forkmesh/pulls",
    ], repository.git_dir)

    rows = {
        branch["name"]: branch
        for branch in repository.branches({})["branches"]
    }
    assert rows["feature-ahead"]["ahead"] >= 1
    assert rows["feature-ahead"]["behind"] == 0
    assert (rows["release-preview"]["ahead"],
            rows["release-preview"]["behind"]) == (0, 1)

    # A Git too old for the ahead-behind atom rejects the whole format. The
    # listing still has to arrive, just without the counts.
    real_run_git = gateway._run_git

    def refuse_ahead_behind(git_dir, arguments, **kwargs):
        arguments = list(arguments)
        if any("ahead-behind" in argument for argument in arguments):
            raise gateway.GitError("unknown field name")
        return real_run_git(git_dir, arguments, **kwargs)

    monkeypatch.setattr(gateway, "_run_git", refuse_ahead_behind)
    degraded = repository.branches({})
    assert degraded["defaultBranch"] == "main"
    assert [branch["name"] for branch in degraded["branches"]] == [
        "feature-ahead", "main", "release-preview",
    ]
    assert all(
        "ahead" not in branch and "behind" not in branch
        for branch in degraded["branches"]
    )


def test_public_repository_reads_can_bypass_router_without_exposing_writes(
    application,
):
    app, _commit, _release_hash, _logs = application
    target = "/v1/public/repositories/alice/project/branches"

    response = app.dispatch("GET", target, {}, b"")
    assert response.status == 200
    payload = decode_json(response)
    assert any(branch["name"] == "main" for branch in payload["branches"])

    protected = app.dispatch(
        "GET",
        "/v1/public/repositories/alice/project/actions-status",
        {},
        b"",
    )
    mutation = app.dispatch(
        "POST",
        "/v1/public/repositories/alice/project/merge-pull",
        {},
        b"{}",
    )
    private_replica = app.dispatch(
        "GET", "/v1/private-replicas/" + "a" * 64, {}, b""
    )
    assert protected.status == 404
    assert mutation.status == 404
    assert private_replica.status == 404


def test_public_smart_http_clone_routes_need_no_router_capability(application):
    app, _commit, _release_hash, _logs = application
    advert = app.dispatch(
        "GET",
        "/alice/project.git/info/refs?service=git-upload-pack",
        {},
        b"",
    )
    assert advert.status == 200
    assert advert.content_type == (
        "application/x-git-upload-pack-advertisement")
    assert advert.body.startswith(b"001e# service=git-upload-pack\n0000")

    upload = app.dispatch(
        "POST",
        "/alice/project.git/git-upload-pack",
        {"Content-Type": "application/x-git-upload-pack-request"},
        b"0000",
    )
    assert upload.status == 200
    assert upload.stream.kind == "process"
    assert app.dispatch(
        "POST", "/alice/project.git/git-receive-pack", {}, b"0000"
    ).status == 404


def test_public_info_refs_errors_are_plain_text_git_prints(application):
    app, _commit, _release_hash, _logs = application
    missing = app.dispatch(
        "GET", "/alice/nothing.git/info/refs?service=git-upload-pack", {}, b"")
    assert missing.status == 404
    assert missing.content_type == "text/plain; charset=utf-8"
    assert missing.body.decode().startswith(
        "This ForkMesh mirror has no public repository at this address.")
    assert b"https://forkmesh.com/docs/git-errors/" in missing.body
    upload = app.dispatch(
        "POST", "/alice/nothing.git/git-upload-pack", {}, b"0000")
    assert upload.status == 404
    assert decode_json(upload)["error"] == "not_found"
    capability = app.dispatch(
        "GET", "/v1/repositories/alice/nothing/git-info-refs"
        "?service=git-upload-pack", {}, b"")
    assert decode_json(capability)["error"] == "not_found"


def test_legacy_direct_branch_probe_is_public_read_only_compatibility(
    application,
):
    app, _commit, _release_hash, _logs = application
    legacy = "/v1/repositories/alice/project/branches"

    response = app.dispatch("GET", legacy, {}, b"")
    assert response.status == 200
    payload = decode_json(response)
    assert any(branch["name"] == "main" for branch in payload["branches"])

    assert app.dispatch("HEAD", legacy, {}, b"").status == 200
    assert app.dispatch("GET", legacy + "?ref=main", {}, b"").status == 404
    assert app.dispatch("POST", legacy, {}, b"{}").status == 404
    assert app.dispatch(
        "GET", "/v1/repositories/alice/project/tree", {}, b""
    ).status == 404
    assert app.dispatch(
        "GET", "/v1/repositories/alice/private-project/branches", {}, b""
    ).status == 404


def test_pull_metadata_branch_is_read_through_its_resolved_commit(application):
    app, _commit, _release_hash, _logs = application
    repository = app.repositories[("alice", "project")]
    branches = decode_json(
        dispatch(
            app,
            "branches",
            {},
            request_id="pull_metadata_branches_01",
        )
    )["branches"]
    assert "forkmesh/pulls" not in {branch["name"] for branch in branches}
    metadata_commit = repository.resolve_commit("forkmesh/pulls")
    assert len(metadata_commit) == 40

    tree = decode_json(
        dispatch(
            app,
            "tree",
            {"path": ".forkmesh/pulls", "ref": metadata_commit},
            request_id="pull_metadata_tree_01",
        )
    )
    assert tree["commit"] == metadata_commit
    assert [(entry["name"], entry["type"]) for entry in tree["entries"]] == [
        ("42", "tree")
    ]
    blob = decode_json(
        dispatch(
            app,
            "blob",
            {
                "path": ".forkmesh/pulls/42/pull.md",
                "ref": metadata_commit,
            },
            request_id="pull_metadata_blob_01",
        )
    )
    assert "Metadata branch truth" in blob["content"]


def test_missing_tree_path_or_ref_is_not_found_instead_of_mirror_unavailable(
    application,
):
    app, _commit, _release_hash, _logs = application
    repository = app.repositories[("alice", "project")]
    metadata_commit = repository.resolve_commit("forkmesh/pulls")

    missing = dispatch(
        app,
        "tree",
        {
            "path": ".forkmesh/does-not-exist",
            "ref": metadata_commit,
        },
        request_id="missing_tree_path_01",
    )

    assert missing.status == 404
    assert decode_json(missing) == {"ok": False, "error": "not_found"}

    missing_raw = dispatch(
        app,
        "raw",
        {
            "path": ".forkmesh/banner.png",
            "ref": metadata_commit,
        },
        request_id="missing_raw_path_01",
    )
    assert missing_raw.status == 404
    assert decode_json(missing_raw) == {"ok": False, "error": "not_found"}

    missing_ref = dispatch(
        app,
        "tree",
        {"path": ".forkmesh", "ref": "does-not-exist"},
        request_id="missing_tree_ref_01",
    )
    assert missing_ref.status == 404
    assert decode_json(missing_ref) == {"ok": False, "error": "not_found"}


@pytest.mark.parametrize(
    ("path", "ref"),
    [
        (".forkmesh/issues/open/304/issue-304.json", "main"),
        (".forkmesh/issues/open/677/issue-677.json", ""),
        ("src", "main"),
        ("src/main.py/extra", "main"),
    ],
)
def test_missing_blob_path_is_not_found_instead_of_mirror_unavailable(
    application, path, ref,
):
    app, _commit, _release_hash, logs = application
    query = {"path": path}
    if ref:
        query["ref"] = ref
    missing = dispatch(app, "blob", query, request_id="missing_blob_01")
    assert missing.status == 404
    assert decode_json(missing) == {"ok": False, "error": "not_found"}

    public = app.dispatch(
        "GET",
        "/v1/public/repositories/alice/project/blob?" + urlencode(query),
        {},
        b"",
    )
    assert public.status == 404


def test_blob_reads_through_the_tree_entry(application):
    app, _commit, _release_hash, _logs = application
    found = decode_json(
        dispatch(
            app, "blob", {"path": "src/main.py", "ref": "main"},
            request_id="found_blob_01",
        )
    )
    assert found["ok"] is True
    assert found["encoding"] == "utf8"
    assert found["size"] == len(found["content"].encode("utf-8"))


def test_batched_blobs_resolve_every_member_with_one_tree_listing(
    application, monkeypatch,
):
    app, commit, _release_hash, _logs = application
    repository = app.repositories[("alice", "project")]
    listings = []
    original = gateway._run_git

    def counted(git_dir, arguments, **kwargs):
        arguments = list(arguments)
        if arguments[:1] == ["ls-tree"]:
            listings.append(arguments)
        return original(git_dir, arguments, **kwargs)

    monkeypatch.setattr(gateway, "_run_git", counted)
    payload = repository.blobs(
        ["README.md", "src/main.py", "src", "../escape",
         ".forkmesh/issues/open/304/issue-304.json", "README.md"],
        "main",
    )
    assert payload["commit"] == commit
    assert payload["blobs"]["README.md"]["ok"] is True
    assert payload["blobs"]["src/main.py"]["ok"] is True
    assert payload["blobs"]["src"] is None
    assert payload["blobs"]["../escape"] is None
    assert payload["blobs"][".forkmesh/issues/open/304/issue-304.json"] is None
    assert len(listings) == 1


def test_collaboration_tree_avoids_whole_repo_analysis_and_per_entry_logs(
    application, monkeypatch,
):
    app, _commit, _release_hash, _logs = application
    repository = app.repositories[("alice", "project")]
    summary_paths = []
    original_summary = repository._commit_summary

    def unavailable_analysis(_commit):
        raise AssertionError(
            "collaboration metadata must not occupy the analysis lock"
        )

    def counted_summary(commit, path=""):
        summary_paths.append(path)
        return original_summary(commit, path)

    monkeypatch.setattr(repository, "_analysis", unavailable_analysis)
    monkeypatch.setattr(repository, "_commit_summary", counted_summary)
    metadata_commit = repository.resolve_commit("forkmesh/pulls")
    payload = repository.tree({
        "path": ".forkmesh/pulls",
        "ref": metadata_commit,
    })

    assert payload["ok"] is True
    assert payload["commit"] == metadata_commit
    assert payload["analysis"]["commit"] == metadata_commit
    assert payload["analysis"]["dependency"]["status"] == "not-requested"
    assert payload["analysis"]["coverage"]["status"] == "not-requested"
    assert summary_paths == [""]
    assert [(item["name"], item["type"]) for item in payload["entries"]] == [
        ("42", "tree")
    ]
    assert payload["entries"][0]["analysisCommit"] == metadata_commit
    assert payload["entries"][0]["dependencies"] == []


def test_collaboration_tree_lists_past_the_code_tree_entry_cap(application, monkeypatch):
    app, _commit, _release_hash, _logs = application
    repository = app.repositories[("alice", "project")]
    monkeypatch.setattr(gateway, "MAX_TREE_ENTRIES", 0)
    metadata_commit = repository.resolve_commit("forkmesh/pulls")

    code = repository.tree({"path": "", "ref": "main"})
    metadata = repository.tree({"path": ".forkmesh/pulls", "ref": metadata_commit})

    assert code["truncated"] is True and code["entries"] == []
    assert metadata["truncated"] is False
    assert [item["name"] for item in metadata["entries"]] == ["42"]
    monkeypatch.setattr(gateway, "MAX_COLLABORATION_TREE_ENTRIES", 0)
    assert repository.tree({"path": ".forkmesh/pulls", "ref": metadata_commit})["truncated"] is True


def test_compare_returns_bounded_portable_pull_change_set(application):
    app, commit, _release_hash, _logs = application
    repository = app.repositories[("alice", "project")]
    base = gateway._run_git(
        repository.git_dir,
        ["rev-parse", commit + "^"],
        max_output=128,
    ).decode().strip()
    payload = decode_json(
        dispatch(
            app,
            "compare",
            {"base": base, "head": "main"},
            request_id="compare_change_set_01",
        )
    )
    assert payload["ok"] is True
    assert payload["baseOid"] == base
    assert payload["headOid"] == commit
    assert payload["mergeBaseOid"] == base
    assert payload["commitCount"] == 1
    assert "src/main.py" in payload["patch"]
    assert "Improve greeting" in payload["commits"]
    assert payload["commits"].startswith("From ")


def test_compare_requires_both_refs_and_enforces_byte_cap(
    application, monkeypatch
):
    app, commit, _release_hash, _logs = application
    missing = dispatch(
        app,
        "compare",
        {"head": commit},
        request_id="compare_missing_ref_01",
    )
    assert missing.status == 404

    repository = app.repositories[("alice", "project")]
    base = gateway._run_git(
        repository.git_dir,
        ["rev-parse", commit + "^"],
        max_output=128,
    ).decode().strip()
    monkeypatch.setattr(gateway, "MAX_COMPARE_BYTES", 8)
    oversized = dispatch(
        app,
        "compare",
        {"base": base, "head": commit},
        request_id="compare_byte_cap_01",
    )
    assert oversized.status == 413
    assert decode_json(oversized)["error"] == "comparison_too_large"


def divergent_conflicting_branch(tmp_path, bare, name="contributor/rename"):
    """Push a branch that edits the same line the default branch changed."""
    work = tmp_path / ("diverge-" + name.replace("/", "-"))
    run(["git", "clone", str(bare), str(work)], tmp_path)
    run(["git", "config", "user.email", "bob@example.test"], work)
    run(["git", "config", "user.name", "Bob"], work)
    run(["git", "switch", "-c", name, "main~1"], work)
    (work / "src" / "main.py").write_text(
        "def greet(who):\n    return 'hi ' + who\n", encoding="utf-8")
    run(["git", "commit", "-am", "Rename the greeting argument"], work)
    run(["git", "push", "origin", name], work)
    return run(["git", "rev-parse", "HEAD"], work)


def test_merge_preview_reports_ahead_behind_and_a_clean_merge(application):
    app, commit, _release_hash, _logs = application
    repository = app.repositories[("alice", "project")]
    base = gateway._run_git(
        repository.git_dir,
        ["rev-parse", commit + "^"],
        max_output=128,
    ).decode().strip()

    stale = decode_json(
        dispatch(
            app,
            "merge-preview",
            {"base": "main", "head": base},
            request_id="merge_preview_behind_01",
        )
    )
    assert stale["ok"] is True
    assert stale["baseOid"] == commit
    assert stale["headOid"] == base
    assert stale["mergeBaseOid"] == base
    assert stale["ahead"] == 0
    assert stale["behind"] == 1
    assert stale["clean"] is True
    assert stale["conflicts"] == []

    current = decode_json(
        dispatch(
            app,
            "merge-preview",
            {"base": base, "head": "main"},
            request_id="merge_preview_current_01",
        )
    )
    assert current["ahead"] == 1
    assert current["behind"] == 0
    assert current["clean"] is True
    assert current["conflicts"] == []


def test_merge_preview_names_conflicts_without_writing_any_ref(
    application, tmp_path
):
    app, _commit, _release_hash, _logs = application
    repository = app.repositories[("alice", "project")]
    head = divergent_conflicting_branch(tmp_path, repository.git_dir)
    refs_before = gateway.refs_sha256(repository.git_dir)

    payload = decode_json(
        dispatch(
            app,
            "merge-preview",
            {"base": "main", "head": head},
            request_id="merge_preview_conflict_01",
        )
    )

    assert payload["ok"] is True
    assert payload["ahead"] == 1
    assert payload["behind"] == 1
    assert payload["clean"] is False
    assert payload["conflicts"] == ["src/main.py"]
    # The merge trial happens entirely in the object database: a readiness read
    # must never publish a ref or otherwise change the serving generation.
    assert gateway.refs_sha256(repository.git_dir) == refs_before


def test_merge_preview_requires_both_refs_and_a_shared_history(
    application, tmp_path
):
    app, commit, _release_hash, _logs = application
    assert dispatch(
        app,
        "merge-preview",
        {"head": commit},
        request_id="merge_preview_missing_01",
    ).status == 404

    repository = app.repositories[("alice", "project")]
    unrelated = tmp_path / "unrelated"
    run(["git", "init", "-b", "orphan", str(unrelated)], tmp_path)
    run(["git", "config", "user.email", "eve@example.test"], unrelated)
    run(["git", "config", "user.name", "Eve"], unrelated)
    (unrelated / "only.txt").write_text("nothing shared\n", encoding="utf-8")
    run(["git", "add", "only.txt"], unrelated)
    run(["git", "commit", "-m", "unrelated root"], unrelated)
    run(["git", "remote", "add", "mirror", str(repository.git_dir)], unrelated)
    run(["git", "push", "mirror", "orphan"], unrelated)

    assert dispatch(
        app,
        "merge-preview",
        {"base": "main", "head": "orphan"},
        request_id="merge_preview_unrelated_01",
    ).status == 404


def test_public_merge_preview_answers_the_fields_the_phones_parse(
    application, tmp_path
):
    app, commit, _release_hash, _logs = application
    repository = app.repositories[("alice", "project")]
    head = divergent_conflicting_branch(
        tmp_path, repository.git_dir, name="phone/preview")
    response = app.dispatch(
        "GET",
        "/v1/public/repositories/alice/project/merge-preview?"
        + urlencode({"base": commit, "head": "phone/preview"}),
        {},
        b"",
    )
    assert response.status == 200
    payload = decode_json(response)
    assert payload["baseOid"] == commit
    assert payload["headOid"] == head
    assert payload["clean"] is False
    assert payload["conflicts"] == ["src/main.py"]
    assert all(isinstance(path, str) for path in payload["conflicts"])


def test_merge_preview_asks_the_executor_to_evaluate_pull_rules(
    application, tmp_path
):
    app, commit, _release_hash, _logs = application
    repository = app.repositories[("alice", "project")]
    head = divergent_conflicting_branch(
        tmp_path, repository.git_dir, name="rules/preview")
    calls = []

    class RulesExecutor:
        def call(self, request, *, timeout=20, max_bytes=16 * 1024):
            calls.append((request, max_bytes))
            return {"ok": True, "rules": {
                "available": True, "branch": "main", "passed": False,
                "blocked": True, "rules": [{"id": "tests"}]}}

    def preview(query):
        return app.dispatch(
            "GET",
            "/v1/public/repositories/alice/project/merge-preview?"
            + urlencode(query), {}, b"")

    app.merge_executor = None
    plain = decode_json(preview({"base": commit, "head": "rules/preview"}))
    assert "rules" not in plain
    offline = decode_json(preview({
        "base": commit, "head": "rules/preview", "rules": "1", "pull": "7"}))
    assert offline["rules"] == {"available": False, "rules": []}

    app.merge_executor = RulesExecutor()
    org_rules = json.dumps({"schema": "forkmesh-pull-rules-v1", "rules": [
        {"id": "tests", "kind": "tests"}]})
    encoded = base64.urlsafe_b64encode(
        org_rules.encode()).decode().rstrip("=")
    payload = decode_json(preview({
        "base": commit, "head": "rules/preview", "rules": "1", "pull": "8",
        "orgRules": encoded}))
    assert payload["rules"]["blocked"] is True
    request, max_bytes = calls[-1]
    assert request == {
        "schemaVersion": 1, "type": "forkmesh.pull-rules-evaluate-v1",
        "owner": "alice", "repository": "project", "base": commit,
        "head": head, "pullNumber": 8, "orgRules": org_rules,
    }
    assert max_bytes == gateway.MAX_PULL_RULES_RESPONSE_BYTES
    for bad in ({"pull": "0"}, {"pull": "7", "orgRules": "!!"}):
        response = preview({
            "base": commit, "head": "rules/preview", "rules": "1", **bad})
        assert response.status == 404


def test_merge_preview_is_a_read_operation_enabled_by_default():
    assert "merge-preview" in gateway.PUBLIC_OPERATIONS
    assert "merge-preview" in gateway.DEFAULT_PUBLIC_OPERATIONS
    assert "merge-preview" in gateway.WEBSITE_COUNTER_OPERATIONS
    assert "merge-preview" not in gateway.PULL_MUTATIONS
    assert "update-pull-branch" in gateway.PULL_MUTATIONS
    assert "update-pull-branch" not in gateway.DEFAULT_PUBLIC_OPERATIONS


def test_sizes_exposes_bounded_file_leaves_with_full_paths(application):
    app, _commit, _release_hash, _logs = application
    payload = decode_json(
        dispatch(
            app,
            "sizes",
            {},
            request_id="sizes_file_leaves_01",
        )
    )
    assert payload["type"] == "directory"
    root_children = payload["children"]
    readme = next(item for item in root_children if item["name"] == "README.md")
    assert readme == {
        "name": "README.md",
        "path": "README.md",
        "size": len("# Example\n\nsearchable mirror gateway\n".encode()),
        "type": "file",
    }
    src = next(item for item in root_children if item["name"] == "src")
    assert src["type"] == "directory"
    assert {
        item["path"] for item in src["children"] if item["type"] == "file"
    } == {"src/main.js", "src/main.py", "src/util.js"}
    assert all(
        len(node.get("children", ())) <= 41
        for node in [payload, *root_children]
    )


def test_tree_analysis_is_commit_matched_and_uses_real_edges_and_coverage(
    application,
):
    app, commit, _release_hash, _logs = application
    payload = decode_json(
        dispatch(
            app,
            "tree",
            {"path": "src", "ref": commit},
            request_id="tree_analysis_01",
        )
    )
    assert payload["commit"] == commit
    assert payload["analysis"]["commit"] == commit
    assert payload["analysis"]["dependency"]["filesParsed"] >= 3
    assert payload["analysis"]["dependency"]["edgeCount"] == 1
    assert payload["analysis"]["coverage"]["status"] == "available"
    assert payload["analysis"]["coverage"]["artifacts"] == [
        "coverage/lcov.info"
    ]
    entries = {entry["path"]: entry for entry in payload["entries"]}
    assert entries["src/main.js"]["dependencies"] == ["src/util.js"]
    assert entries["src/main.js"]["dependencyDepth"] == 1
    assert entries["src/main.js"]["coverage"] == 75.0
    assert entries["src/util.js"]["dependencies"] == []
    assert entries["src/util.js"]["dependencyDepth"] == 0
    assert entries["src/util.js"]["coverage"] == 100.0
    assert all(
        entry["analysisCommit"] == commit for entry in entries.values()
    )
    assert "../../outside.py" not in json.dumps(payload)


def test_bounded_analysis_parsers_reject_escape_and_parse_common_artifacts():
    paths = {"src/main.ts", "src/util.ts", "pkg/model.py"}
    istanbul = json.dumps({
        "/checkout/src/main.ts": {"s": {"0": 1, "1": 0, "2": 2}},
        "../../etc/passwd": {"s": {"0": 1}},
    }).encode()
    assert gateway._parse_json_coverage(istanbul, paths) == {
        "src/main.ts": 66.7
    }
    coverage_py = json.dumps({
        "files": {
            "pkg/model.py": {
                "summary": {
                    "covered_lines": 7,
                    "num_statements": 10,
                }
            }
        }
    }).encode()
    assert gateway._parse_json_coverage(coverage_py, paths) == {
        "pkg/model.py": 70.0
    }
    cobertura = (
        b'<coverage><class filename="src/util.ts" line-rate="0.5">'
        b"</class></coverage>"
    )
    assert gateway._parse_xml_coverage(cobertura, paths) == {
        "src/util.ts": 50.0
    }
    deeply_nested = b"[" * 2_000 + b"]" * 2_000
    assert gateway._parse_json_coverage(deeply_nested, paths) == {}
    suffix_index = {
        "src/util.ts": ["src/util.ts"],
        "util.ts": ["src/util.ts"],
    }
    assert gateway._dependency_target(
        "src/main.ts", "./util", paths, suffix_index
    ) == "src/util.ts"
    assert gateway._dependency_target(
        "src/main.ts", "../../../etc/passwd", paths, suffix_index
    ) == ""
    assert gateway._dependency_target(
        "src/main.ts", "https://example.test/a.js", paths, suffix_index
    ) == ""
    assert gateway._dependency_target(
        "src/main.ts",
        "//example.test/a.js",
        paths | {"example.test/a.js"},
        {**suffix_index, "example.test/a.js": ["example.test/a.js"]},
    ) == ""
    assert gateway._dependency_target(
        "src/main.ts",
        "react",
        paths | {"react.ts"},
        {**suffix_index, "react.ts": ["react.ts"]},
    ) == ""


def test_batched_blobs_preserve_repeated_paths_and_bound_missing_files(application):
    app, commit, _release_hash, _logs = application
    target = (
        "/v1/repositories/alice/project/blobs"
        "?path=README.md&path=src%2Fmain.py"
        "&path=does-not-exist&ref=HEAD"
    )
    response = app.dispatch(
        "GET",
        target,
        capability_headers(target, request_id="batched_blobs_01"),
        b"",
    )
    assert response.status == 200
    payload = decode_json(response)
    assert payload["commit"] == commit
    assert "searchable mirror gateway" in payload["blobs"]["README.md"]["content"]
    assert payload["blobs"]["src/main.py"]["ok"] is True
    assert payload["blobs"]["does-not-exist"] is None


def test_raw_active_media_types_are_attachments(application, monkeypatch):
    app, _commit, _release_hash, _logs = application
    for index, (media, expected, disposition) in enumerate((
        ("application/atom+xml", "application/octet-stream", "attachment"),
        ("application/rdf+xml", "application/octet-stream", "attachment"),
        ("application/mathml+xml", "application/octet-stream", "attachment"),
        ("application/xslt+xml", "application/octet-stream", "attachment"),
        ("application/xhtml+xml", "application/octet-stream", "attachment"),
        ("image/svg+xml", "application/octet-stream", "attachment"),
        ("text/html", "application/octet-stream", "attachment"),
        ("application/ecmascript", "application/octet-stream", "attachment"),
        ("multipart/related", "application/octet-stream", "attachment"),
        (None, "application/octet-stream", "attachment"),
        ("text/x-python", "text/plain; charset=utf-8", "inline"),
        ("image/png", "image/png", "inline"),
    )):
        monkeypatch.setattr(gateway.mimetypes, "guess_type",
                            lambda _path, media=media: (media, None))
        raw = dispatch(app, "raw", {"path": "README.md"},
                       request_id="raw_active_%04d" % index)
        assert raw.status == 200
        assert raw.stream.content_type == expected, media
        assert raw.stream.disposition == disposition, media
    monkeypatch.undo()
    for name in ("feed.atom", "x.rdf", "x.mml", "x.xht", "x.xsl", "x.svg",
                 "x.html", "x.js"):
        assert gateway._active_media_type(
            gateway.mimetypes.guess_type(name)[0] or ""), name


def test_raw_release_and_git_upload_pack_are_stream_specs(application):
    app, commit, release_hash, _logs = application
    raw = dispatch(
        app,
        "raw",
        {"path": "README.md"},
        request_id="raw_stream_0001",
    )
    assert raw.status == 200
    assert raw.stream.kind == "process"
    assert raw.stream.disposition == "inline"
    raw_bytes = subprocess.run(
        raw.stream.command,
        check=True,
        capture_output=True,
        env=gateway._git_environment(),
    ).stdout
    assert b"searchable mirror gateway" in raw_bytes

    release = dispatch(
        app,
        "release-blob",
        {"sha256": release_hash},
        request_id="release_stream_1",
    )
    assert release.stream.kind == "file"
    assert release.stream.disposition == "attachment"
    assert release.stream.path.read_bytes() == b"release-content"
    assert release.headers["Cache-Control"] == (
        "public, max-age=31536000, immutable"
    )
    assert release.headers["ETag"] == f'"sha256-{release_hash}"'

    advert = dispatch(
        app,
        "git-info-refs",
        {"service": "git-upload-pack"},
        request_id="git_advert_0001",
    )
    assert advert.status == 200
    assert advert.body.startswith(b"001e# service=git-upload-pack\n0000")
    assert b" filter " in advert.body

    v2_advert = dispatch(
        app,
        "git-info-refs",
        {"service": "git-upload-pack"},
        request_id="git_advert_v2_0001",
        extra_headers={"Git-Protocol": "version=2"},
    )
    assert v2_advert.status == 200
    assert v2_advert.body.startswith(b"000eversion 2\n")
    assert b"filter" in v2_advert.body
    assert b"# service=git-upload-pack" not in v2_advert.body

    invalid_protocol = dispatch(
        app,
        "git-info-refs",
        {"service": "git-upload-pack"},
        request_id="git_advert_invalid_0001",
        extra_headers={"Git-Protocol": "version=99"},
    )
    assert invalid_protocol.status == 404

    def pkt_line(value):
        payload = value.encode("ascii")
        return f"{len(payload) + 4:04x}".encode("ascii") + payload

    upload_request = (
        pkt_line(
            f"want {commit} multi_ack_detailed no-done side-band-64k "
            "thin-pack no-progress include-tag ofs-delta deepen-since "
            "deepen-not agent=git/test"
        )
        + pkt_line("deepen 1")
        + b"0000"
        + pkt_line("done\n")
        + b"0000"
    )
    upload = dispatch(
        app,
        "git-upload-pack",
        request_id="git_upload_0001",
        method="POST",
        body=upload_request,
    )
    assert upload.status == 200
    assert upload.stream.kind == "process"
    assert "upload-pack" in upload.stream.command
    assert "receive-pack" not in upload.stream.command
    assert not any(
        item.startswith("uploadpack.packObjectsHook")
        for item in upload.stream.command
    )
    assert "core.alternateRefsCommand=/usr/bin/true" in upload.stream.command
    assert "uploadpack.hideRefs=refs/forkmesh/" in upload.stream.command
    assert "uploadpack.allowTipSHA1InWant=false" in upload.stream.command
    assert "uploadpack.allowReachableSHA1InWant=false" in upload.stream.command
    assert "uploadpack.allowAnySHA1InWant=false" in upload.stream.command
    assert "uploadpack.allowFilter=true" in upload.stream.command
    assert upload.stream.git_protocol == ""
    completed = subprocess.run(
        upload.stream.command,
        input=upload.stream.input_bytes,
        capture_output=True,
        env=gateway._git_environment(),
        timeout=5,
        check=False,
    )
    assert completed.returncode == 0
    assert b"PACK" in completed.stdout

    v2_upload = dispatch(
        app,
        "git-upload-pack",
        request_id="git_upload_v2_0001",
        method="POST",
        body=b"0000",
        extra_headers={"Git-Protocol": "version=2"},
    )
    assert v2_upload.status == 200
    assert v2_upload.stream.git_protocol == "version=2"
    assert gateway._git_environment(v2_upload.stream.git_protocol)[
        "GIT_PROTOCOL"
    ] == "version=2"


def test_real_git_clone_cannot_advertise_or_fetch_internal_merge_objects(
    application, tmp_path
):
    app, _commit, _release_hash, _logs = application
    repository = next(iter(app.repositories.values()))
    secret = subprocess.run(
        [
            "git", "--git-dir", str(repository.git_dir),
            "hash-object", "-w", "--stdin",
        ],
        input=b"owner-only merge job",
        check=True,
        capture_output=True,
    ).stdout.decode("ascii").strip()
    run([
        "git", "--git-dir", str(repository.git_dir), "update-ref",
        "refs/forkmesh/merge-jobs/test-job", secret,
    ], tmp_path)
    run([
        "git", "--git-dir", str(repository.git_dir), "update-ref",
        "refs/forkmesh/merge-staging/test-job/base", secret,
    ], tmp_path)
    # Repository-local settings are hostile input at this boundary. Explicit
    # command-line policy must override attempts to expose arbitrary objects.
    run([
        "git", "--git-dir", str(repository.git_dir), "config",
        "uploadpack.allowAnySHA1InWant", "true",
    ], tmp_path)
    run([
        "git", "--git-dir", str(repository.git_dir), "config", "--add",
        "uploadpack.hideRefs", "!refs/forkmesh/",
    ], tmp_path)

    advertisement = repository.git_advertisement()
    assert b"refs/forkmesh/" not in advertisement
    assert secret.encode("ascii") not in advertisement

    class GitHandler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *_args):
            pass

        def do_GET(self):
            if self.path != "/repo/info/refs?service=git-upload-pack":
                self.send_error(404)
                return
            body = repository.git_advertisement(
                self.headers.get("Git-Protocol", ""))
            self.send_response(200)
            self.send_header(
                "Content-Type",
                "application/x-git-upload-pack-advertisement",
            )
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_POST(self):
            if self.path != "/repo/git-upload-pack":
                self.send_error(404)
                return
            length = int(self.headers.get("Content-Length") or 0)
            request_body = self.rfile.read(length)
            stream = repository.upload_pack_spec(
                request_body, self.headers.get("Git-Protocol", ""))
            completed = subprocess.run(
                stream.command,
                input=stream.input_bytes,
                capture_output=True,
                env=gateway._git_environment(stream.git_protocol),
                timeout=10,
                check=False,
            )
            body = completed.stdout
            self.send_response(200 if completed.returncode == 0 else 500)
            self.send_header(
                "Content-Type", "application/x-git-upload-pack-result")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    server = ThreadingHTTPServer(("127.0.0.1", 0), GitHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    origin = "http://127.0.0.1:%d/repo" % server.server_address[1]
    clone = tmp_path / "clone-with-hidden-refs"
    try:
        cloned = subprocess.run(
            [
                "git", "-c", "protocol.version=2", "clone", "--no-tags",
                origin, str(clone),
            ],
            capture_output=True,
            text=True,
            timeout=20,
            check=False,
            env=gateway._git_environment(),
        )
        assert cloned.returncode == 0, cloned.stderr
        refs = run(["git", "-C", str(clone), "show-ref"], tmp_path)
        assert "refs/forkmesh/" not in refs
        assert (clone / "README.md").exists()
        assert subprocess.run(
            ["git", "-C", str(clone), "cat-file", "-e", secret],
            capture_output=True,
            check=False,
        ).returncode != 0
        exact_fetch = subprocess.run(
            [
                "git", "-c", "protocol.version=2", "-C", str(clone),
                "fetch", origin, secret,
            ],
            capture_output=True,
            text=True,
            timeout=20,
            check=False,
            env=gateway._git_environment(),
        )
        assert exact_fetch.returncode != 0

        partial = tmp_path / "partial-clone-with-hidden-refs"
        partial_clone = subprocess.run(
            [
                "git", "-c", "protocol.version=2", "clone",
                "--filter=blob:none", "--no-checkout", origin, str(partial),
            ],
            capture_output=True,
            text=True,
            timeout=20,
            check=False,
            env=gateway._git_environment(),
        )
        assert partial_clone.returncode == 0, partial_clone.stderr
        assert "filtering not recognized by server" not in partial_clone.stderr
        assert run(
            ["git", "-C", str(partial), "config", "--bool",
             "remote.origin.promisor"],
            tmp_path,
        ) == "true"
        missing = run(
            ["git", "-C", str(partial), "rev-list", "--objects",
             "--missing=print", "HEAD"],
            tmp_path,
        )
        assert "?" in missing
        checkout = subprocess.run(
            [
                "git", "-c", "protocol.version=2", "-C", str(partial),
                "checkout", "-q",
            ],
            capture_output=True,
            text=True,
            timeout=20,
            check=False,
            env=gateway._git_environment(),
        )
        assert checkout.returncode == 0, checkout.stderr
        assert (partial / "README.md").read_text(encoding="utf-8") == (
            "# Example\n\nsearchable mirror gateway\n"
        )
        assert subprocess.run(
            ["git", "-C", str(partial), "cat-file", "-e", secret],
            capture_output=True,
            check=False,
        ).returncode != 0
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)


@pytest.mark.parametrize("downstream_bytes_per_second", [2 * 1024 * 1024, 512 * 1024])
def test_large_git_clone_survives_downstream_backpressure_at_multiple_rates(
    application,
    tmp_path,
    monkeypatch,
    downstream_bytes_per_second,
):
    """A slow receiver must not consume the established host-idle window."""
    app, _commit, _release_hash, _logs = application
    repository = next(iter(app.repositories.values()))

    # Add enough incompressible history to make the response last well beyond
    # the deliberately tiny regression watchdog below. This keeps the test
    # quick while reproducing the old total-transfer-deadline failure mode.
    producer = tmp_path / "large-clone-producer"
    run(["git", "clone", str(repository.git_dir), str(producer)], tmp_path)
    run(["git", "config", "user.email", "alice@example.test"], producer)
    run(["git", "config", "user.name", "Alice"], producer)
    payload = random.Random(608).randbytes(2 * 1024 * 1024)
    (producer / "large.bin").write_bytes(payload)
    run(["git", "add", "large.bin"], producer)
    run(["git", "commit", "-m", "Add large clone fixture"], producer)
    run(["git", "push", "origin", "HEAD:main"], producer)
    commit = run(["git", "rev-parse", "HEAD"], producer)

    monkeypatch.setattr(
        gateway, "STREAM_INITIAL_RESPONSE_TIMEOUT_SECONDS", 5)
    monkeypatch.setattr(gateway, "STREAM_INACTIVITY_TIMEOUT_SECONDS", 0.2)

    class ThrottledWriter:
        def __init__(self, stream):
            self.stream = stream

        def write(self, data):
            # Sleeping inside write models the gateway blocked on a full HTTP
            # send buffer because the receiving side is draining slowly.
            time.sleep(len(data) / downstream_bytes_per_second)
            return self.stream.write(data)

        def __getattr__(self, name):
            return getattr(self.stream, name)

    def pkt_line(value):
        value = value.encode("ascii")
        return ("%04x" % (len(value) + 4)).encode("ascii") + value

    request_body = (
        pkt_line(
            "want %s side-band-64k thin-pack ofs-delta agent=git/test\n"
            % commit
        )
        + b"0000"
        + pkt_line("done\n")
    )
    stream = repository.upload_pack_spec(request_body)
    sink = io.BytesIO()
    handler = SimpleNamespace(wfile=ThrottledWriter(sink))
    byte_count = gateway.MirrorGatewayHandler._stream(handler, stream)
    wire = sink.getvalue()
    assert byte_count == len(wire)

    # Demultiplex Git's sideband exactly as fetch-pack does. A watchdog cut
    # appears here as a truncated pkt-line or an invalid index-pack input.
    pack = bytearray()
    cursor = 0
    while cursor < len(wire):
        assert cursor + 4 <= len(wire), "truncated sideband length"
        packet_length = int(wire[cursor:cursor + 4], 16)
        cursor += 4
        if packet_length == 0:
            continue
        payload_length = packet_length - 4
        packet = wire[cursor:cursor + payload_length]
        assert len(packet) == payload_length, "truncated sideband packet"
        cursor += payload_length
        if packet[:1] == b"\x01":
            pack.extend(packet[1:])
        elif packet[:1] == b"\x03":
            raise AssertionError(packet[1:].decode("utf-8", errors="replace"))

    clone_git = tmp_path / ("throttled-clone-%d.git" % downstream_bytes_per_second)
    run(["git", "init", "--bare", str(clone_git)], tmp_path)
    indexed = subprocess.run(
        ["git", "--git-dir", str(clone_git), "index-pack", "--stdin", "--fix-thin"],
        input=bytes(pack),
        capture_output=True,
        timeout=20,
        check=False,
    )
    assert indexed.returncode == 0, indexed.stderr.decode(errors="replace")
    cloned_blob = subprocess.run(
        ["git", "--git-dir", str(clone_git), "show", commit + ":large.bin"],
        capture_output=True,
        timeout=20,
        check=True,
    ).stdout
    assert cloned_blob == payload


@pytest.mark.parametrize(
    ("script", "expected_phase"),
    [
        ("import time; time.sleep(1)", "initial response"),
        (
            "import sys,time; sys.stdout.buffer.write(b'x'); "
            "sys.stdout.buffer.flush(); time.sleep(1)",
            "mid-stream",
        ),
    ],
)
def test_git_stream_has_separate_initial_and_established_inactivity_timeouts(
    monkeypatch, script, expected_phase
):
    assert (
        gateway.STREAM_INACTIVITY_TIMEOUT_SECONDS
        > gateway.STREAM_INITIAL_RESPONSE_TIMEOUT_SECONDS
    )
    monkeypatch.setattr(
        gateway, "STREAM_INITIAL_RESPONSE_TIMEOUT_SECONDS", 0.05)
    monkeypatch.setattr(gateway, "STREAM_INACTIVITY_TIMEOUT_SECONDS", 0.05)
    stream = gateway.StreamSpec(
        kind="process",
        content_type="application/x-git-upload-pack-result",
        content_length=None,
        command=(sys.executable, "-c", script),
    )
    handler = SimpleNamespace(wfile=io.BytesIO())
    with pytest.raises(TimeoutError, match=expected_phase):
        gateway.MirrorGatewayHandler._stream(handler, stream)


def test_git_upload_pack_decodes_gzip_after_verifying_transport_digest(application):
    app, _commit, _release_hash, _logs = application
    target = "/v1/repositories/alice/project/git-upload-pack"
    compressed = gzip.compress(b"0000")
    headers = capability_headers(
        target,
        method="POST",
        body=compressed,
        request_id="git_gzip_req_01",
    )
    headers["Content-Type"] = "application/x-git-upload-pack-request"
    headers["Content-Encoding"] = "gzip"
    response = app.dispatch("POST", target, headers, compressed)
    assert response.status == 200
    assert response.stream.input_bytes == b"0000"


def test_http_adapter_streams_ordinary_https_origin_contract_without_redirect(
    application,
):
    app, _commit, _release_hash, logs = application
    server = gateway.MirrorGatewayServer(("127.0.0.1", 0), app)
    # The server owns application cleanup after this point.
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        target = "/v1/repositories/alice/project/raw?path=README.md"
        connection = http.client.HTTPConnection(
            "127.0.0.1", server.server_address[1], timeout=5
        )
        connection.request(
            "GET",
            target,
            headers=capability_headers(
                target, request_id="http_raw_req_01"
            ),
        )
        response = connection.getresponse()
        body = response.read()
        assert response.status == 200
        assert b"searchable mirror gateway" in body
        assert response.getheader("Location") is None
        assert response.getheader("Set-Cookie") is None
        assert response.getheader("X-Content-Type-Options") == "nosniff"
        connection.request(
            "GET", "/health", headers={"Origin": "https://forkmesh.com"}
        )
        health = connection.getresponse()
        health.read()
        assert health.status == 200
        assert health.getheader("Access-Control-Allow-Origin") == "*"
        connection.close()
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)
    assert logs
    serialized = json.dumps(logs)
    assert "README.md" not in serialized
    assert "127.0.0.1" not in serialized


def test_http_adapter_emits_gateway_portable_session_cookie():
    """The response allowlist must not drop a verified handoff cookie."""
    emitted = []
    handler = object.__new__(gateway.MirrorGatewayHandler)
    handler.send_response = lambda status: emitted.append((":status", status))
    handler.send_header = lambda key, value: emitted.append((key, value))
    handler.end_headers = lambda: emitted.append((":end", True))
    response = gateway.GatewayResponse(
        303,
        "text/plain; charset=utf-8",
        b"",
        {
            "Location": "/forkmesh/forkmesh",
            "Set-Cookie": (
                "forkmesh_portable=verified; Path=/; Domain=.forkmesh.com; "
                "Max-Age=120; HttpOnly; Secure; SameSite=Strict"
            ),
            "Cache-Control": "no-store",
        },
    )

    gateway.MirrorGatewayHandler._send_headers(
        handler, response, content_length=0
    )

    assert ("Set-Cookie", response.headers["Set-Cookie"]) in emitted
    assert ("Location", "/forkmesh/forkmesh") in emitted


def test_http_adapter_accepts_only_strict_empty_chunking_for_get(application):
    """A tunnel may translate a bodyless HTTP/2 GET into empty chunking."""
    app, _commit, _release_hash, _logs = application
    target = "/v1/repositories/alice/project/blob?path=README.md"

    def decode(chunks, *, content_length=None):
        handler = object.__new__(gateway.MirrorGatewayHandler)
        handler.headers = {"Transfer-Encoding": "chunked"}
        if content_length is not None:
            handler.headers["Content-Length"] = str(content_length)
        handler.rfile = io.BytesIO(chunks)
        return handler._read_body()

    body = decode(b"0\r\n\r\n")
    response = app.dispatch(
        "GET", target,
        capability_headers(target, request_id="empty_chunked_get_01"), body)
    assert response.status == 200
    assert json.loads(response.body)["content"].startswith("# Example")

    # Supporting the proxy's canonical empty body must not admit request
    # smuggling or weaken the application's no-body GET contract.
    body = decode(b"1\r\nx\r\n0\r\n\r\n")
    response = app.dispatch(
        "GET", target,
        capability_headers(
            target, body=body, request_id="chunked_get_body_01"), body)
    assert response.status == 404
    with pytest.raises(gateway.GatewayError):
        decode(b"0\r\n\r\n", content_length=0)
    with pytest.raises(gateway.GatewayError):
        decode(b"0\r\nX-Trailer: rejected\r\n\r\n")
    with pytest.raises(gateway.GatewayError):
        decode(b"1\r\nx\r\n" * (gateway.MAX_REQUEST_CHUNKS + 1))


def test_gateway_hot_reload_keeps_listener_and_adopts_validated_generation(
    application,
):
    app, commit, _release_hash, logs = application
    repository = app.repositories[("alice", "project")]
    bare = repository.git_dir
    run(["git", "update-ref", "refs/heads/hot-reload", commit], bare)
    new_digest = gateway.refs_sha256(bare)
    config_path = app.config.config_path
    config = json.loads(config_path.read_text(encoding="utf-8"))
    config["repositories"][0]["integrity"][
        "expectedRefsSha256"] = new_digest
    config_path.write_text(json.dumps(config), encoding="utf-8")

    # Exercise the same serve_forever control hook directly. Keeping this a
    # unit test makes the application identity and generation swap explicit;
    # the daemon integration test covers the long-lived child process.
    server = object.__new__(gateway.MirrorGatewayServer)
    server.application = app
    server._reload_requested = threading.Event()
    server._cache_invalidation_requested = threading.Event()
    server._reload_in_progress = threading.Event()
    server._reload_control_lock = threading.Lock()
    server._prewarm_state_lock = threading.Lock()
    server._prewarm_thread = None
    server._prewarm_again = False
    application_identity = id(server.application)
    # The daemon waits on the acknowledgement; the cache warmup (seconds on
    # a large repository) must follow it, off the accept loop, not precede it.
    generation_marker = gateway.gateway_generation_marker_path(config_path)
    warmed_after_acknowledgement = []
    original_prewarm = app.prewarm_memory_caches

    def prewarm_after_acknowledgement():
        warmed_after_acknowledgement.append(
            generation_marker.is_file()
            and generation_marker.read_text(encoding="utf-8").strip()
            == hashlib.sha256(config_path.read_bytes()).hexdigest()
            and threading.current_thread() is not threading.main_thread()
        )
        original_prewarm()

    app.prewarm_memory_caches = prewarm_after_acknowledgement
    server.request_reload()
    server.reload_if_requested()
    server.wait_for_prewarm(timeout=30)
    assert warmed_after_acknowledgement == [True]
    assert app.response_cache.diagnostics()["entries"] >= 5

    expected_marker = hashlib.sha256(config_path.read_bytes()).hexdigest()
    marker = gateway.gateway_generation_marker_path(config_path)
    assert marker.read_text(encoding="utf-8").strip() == expected_marker
    assert server.application.config.generation_sha256 == expected_marker
    assert id(server.application) == application_identity
    assert server.application.repositories[(
        "alice", "project")].config.expected_refs_sha256 == new_digest
    target = "/v1/repositories/alice/project/branches"
    response = app.dispatch(
        "GET", target,
        capability_headers(target, request_id="after_hot_reload_01"), b"")
    payload = json.loads(response.body)
    assert response.status == 200
    assert "hot-reload" in {
        branch["name"] for branch in payload["branches"]}
    assert any(
        record.get("operation") == "reload"
        and record.get("status") == 200
        for record in logs)


def test_gateway_projection_invalidation_event_is_applied_without_reload():
    class ProjectionApplication:
        invalidations = 0

        def invalidate_projection_cache(self):
            self.invalidations += 1

    server = object.__new__(gateway.MirrorGatewayServer)
    server.application = ProjectionApplication()
    server._reload_requested = threading.Event()
    server._cache_invalidation_requested = threading.Event()
    server._reload_in_progress = threading.Event()
    server._reload_control_lock = threading.Lock()

    server.request_cache_invalidation()
    server.reload_if_requested()
    assert server.application.invalidations == 1
    assert not server._cache_invalidation_requested.is_set()


def test_release_hash_is_verified_before_any_bytes_are_served(application):
    app, _commit, release_hash, _logs = application
    repository = next(iter(app.repositories.values()))
    path = (
        repository.config.release_store
        / "sha256"
        / release_hash[:2]
        / release_hash
        / "data"
    )
    path.write_bytes(b"tampered")
    response = dispatch(
        app,
        "release-blob",
        {"sha256": release_hash},
        request_id="release_bad_001",
    )
    assert response.status == 404
    assert response.stream is None


def _release_store_application(tmp_path, bare):
    """One gateway over a config that names no releaseStore."""
    config = gateway.load_config(
        write_config(
            tmp_path,
            bare,
            repositories=[
                {
                    "owner": "alice",
                    "name": "project",
                    "visibility": "public",
                    "enabled": True,
                    "gitDir": str(bare),
                    "integrity": {
                        "expectedRefsSha256": gateway.refs_sha256(bare)
                    },
                }
            ],
        )
    )
    return gateway.GatewayApplication(
        config,
        verifier=AcceptVerifier(),
        health_signer=HealthSigner(),
        materializer=FakeArchiveMaterializer(bare),
        clock_ms=lambda: NOW,
        log=lambda _entry: None,
    )


def test_release_store_defaults_to_the_cas_beside_the_mirror(tmp_path):
    # Publishers write the release CAS to <gitDir>/forkmesh-releases, and both
    # the desktop host and a headless refresh can hand this loader a config
    # that never names it. Defaulting to that path is what stops the one node
    # holding a freshly published artifact from answering 404 for it — the
    # Worker reports 404 to the visitor as soon as every mirror does.
    bare, _commit = make_bare_repository(tmp_path)
    payload = b"artifact published beside the mirror"
    digest = hashlib.sha256(payload).hexdigest()
    blob = bare / "forkmesh-releases" / "sha256" / digest[:2] / digest
    blob.mkdir(parents=True)
    (blob / "data").write_bytes(payload)
    app = _release_store_application(tmp_path, bare)
    try:
        assert (
            next(iter(app.repositories.values())).config.release_store
            == (bare / "forkmesh-releases").resolve()
        )
        response = dispatch(
            app,
            "release-blob",
            {"sha256": digest},
            request_id="release_default_01",
        )
        assert response.status == 200
        assert response.stream.kind == "file"
        assert response.stream.path.read_bytes() == payload
    finally:
        app.close()


def test_default_release_store_is_never_a_symlink(tmp_path):
    # The default is only adopted for a real directory inside the verified
    # owner-controlled gitDir: a symlink there would aim the store, and the
    # traversal check that rides on it, at bytes the operator never published.
    bare, _commit = make_bare_repository(tmp_path)
    elsewhere = tmp_path / "elsewhere"
    payload = b"bytes outside the mirror"
    digest = hashlib.sha256(payload).hexdigest()
    blob = elsewhere / "sha256" / digest[:2] / digest
    blob.mkdir(parents=True)
    (blob / "data").write_bytes(payload)
    (bare / "forkmesh-releases").symlink_to(elsewhere, target_is_directory=True)
    app = _release_store_application(tmp_path, bare)
    try:
        assert (
            next(iter(app.repositories.values())).config.release_store is None
        )
        response = dispatch(
            app,
            "release-blob",
            {"sha256": digest},
            request_id="release_symlink_1",
        )
        assert response.status == 404
        assert response.stream is None
    finally:
        app.close()


def test_external_helpers_receive_public_json_and_no_ambient_credentials(monkeypatch):
    monkeypatch.setenv("CLOUDFLARE_API_TOKEN", "api-secret")
    monkeypatch.setenv("FORKMESH_PRIVATE_KEY", "private-secret")
    calls = []

    def run_helper(command, **kwargs):
        calls.append((command, kwargs))
        request = json.loads(kwargs["input"])
        if request["type"].endswith("verification"):
            return SimpleNamespace(
                stdout=json.dumps({"valid": True, "publicKey": ROUTER_KEY})
            )
        return SimpleNamespace(
            stdout=json.dumps({"publicKey": PUBLIC_KEY, "signature": SIGNATURE})
        )

    verifier = gateway.ExternalCapabilityVerifier(
        ["verify"], ROUTER_KEY, runner=run_helper
    )
    assert verifier.verify("public-message", SIGNATURE)
    signer = gateway.ExternalHealthSigner(
        ["sign"], PUBLIC_KEY, runner=run_helper
    )
    assert signer.sign("public-health-message") == SIGNATURE
    assert all("CLOUDFLARE_API_TOKEN" not in call[1]["env"] for call in calls)
    assert all("FORKMESH_PRIVATE_KEY" not in call[1]["env"] for call in calls)
    assert all("api-secret" not in call[1]["input"] for call in calls)
    assert all("private-secret" not in call[1]["input"] for call in calls)


def test_encrypted_archive_materializer_uses_ciphertext_digest_and_key_reference(
    tmp_path,
):
    bare, _commit = make_bare_repository(tmp_path)
    ciphertext = tmp_path / "repository.tar.age"
    ciphertext.write_bytes(
        gateway.AGE_NATIVE_HEADER + b"opaque-age-ciphertext")
    archive = gateway.EncryptedArchive(
        scheme="age-encrypted-tar-v1",
        ciphertext_path=ciphertext,
        ciphertext_sha256=hashlib.sha256(ciphertext.read_bytes()).hexdigest(),
        key_reference="keychain:forkmesh/alice-project",
        materialize_command=("decrypt-archive",),
    )
    captured = {}

    def materialize(command, **kwargs):
        captured["helperTimeout"] = kwargs["timeout"]
        request = json.loads(kwargs["input"])
        captured.update(request)
        destination = Path(request["destination"])
        shutil.copytree(bare, destination / "repo.git")
        return SimpleNamespace(
            stdout=json.dumps({"ok": True, "repositoryPath": "repo.git"})
        )

    destination = tmp_path / "runtime"
    destination.mkdir()
    result = gateway.ArchiveMaterializer(runner=materialize).materialize(
        archive, destination
    )
    assert result == destination / "repo.git"
    assert captured["helperTimeout"] == 5 * 60
    assert captured["keyReference"] == "keychain:forkmesh/alice-project"
    assert "key" not in {
        key.lower()
        for key in captured
        if key.lower() not in {"keyreference"}
    }
    assert not any(
        field in captured for field in ("privateKey", "secret", "seed", "token")
    )


def test_archive_materializer_rejects_plaintext_before_external_helper(tmp_path):
    ciphertext = tmp_path / "not-encrypted.tar.age"
    ciphertext.write_bytes(b"plaintext tar bytes")
    archive = gateway.EncryptedArchive(
        scheme="age-encrypted-tar-v1",
        ciphertext_path=ciphertext,
        ciphertext_sha256=hashlib.sha256(ciphertext.read_bytes()).hexdigest(),
        key_reference="keychain:forkmesh/alice-project",
        materialize_command=("decrypt-archive",),
    )
    calls = []

    def must_not_run(*args, **kwargs):
        calls.append((args, kwargs))
        raise AssertionError("plaintext reached external materializer")

    with pytest.raises(gateway.GatewayError, match="not an age file"):
        gateway.ArchiveMaterializer(runner=must_not_run).materialize(
            archive, tmp_path / "runtime")
    assert calls == []


def test_source_never_enables_push_or_default_request_logging():
    source = (ROOT / "tools" / "mirror_gateway.py").read_text(encoding="utf-8")
    schema = json.loads(
        (ROOT / "www" / "docs" / "mirror-gateway-config.schema.json").read_text(
            encoding="utf-8"))
    # Push exists but is never enabled implicitly: it requires an explicit
    # per-repository operations entry or the writable-owner allowlist, and
    # always a router capability whose signed target is the receive-pack path.
    assert "git-receive-pack" not in gateway.DEFAULT_PUBLIC_OPERATIONS
    assert "git-receive-pack" in gateway.PUBLIC_OPERATIONS
    assert "receive.denyDeletes=true" in source
    assert "receive.denyNonFastForwards=true" in source
    assert "receive.fsckObjects=true" in source
    # Repository hooks never run: the hooks path is the null device unless
    # the gateway-owned operator-policy directory replaces it.
    assert "hooks_path: str = os.devnull,\n        allow_rewrite: bool = False" in source
    assert "self.rewrite_hooks_path = \"\"" in source
    assert 'core.hooksPath=" + hooks_path' in source
    assert "self.receive_hooks_path = os.devnull" in source
    assert "writableOwners requires writableRepositoryRoot" in source
    assert "def log_message" in source
    assert "BaseHTTPRequestHandler logs client IP and raw path" in source
    assert "gateway must listen on loopback" in source
    enabled = schema["$defs"]["publicRepository"]["allOf"][1]
    assert "gitDir" in enabled["required"]
    assert "encryptedArchive" not in json.dumps(enabled)
    assert "enabled public repository requires plaintext gitDir storage" in source
    assert "operator-envelope-v1" not in source


def test_log_rejects_attacker_controlled_request_ids(application):
    app, _commit, _release_hash, logs = application
    app._record(
        request_id="private-form-value\nnext",
        operation="tree",
        status=404,
        started=0,
        byte_count=0,
    )
    assert logs[-1]["requestId"] == ""
    assert "private-form-value" not in json.dumps(logs[-1])

# ---------------------------------------------------------------------------
# git-receive-pack (mobile-node-contract.md section 7, gateway side)
# ---------------------------------------------------------------------------


_PUSH_REQUEST_IDS = itertools.count(1)


def push_capability_headers(body=b"", issued_at=NOW):
    headers = capability_headers(
        "", body=body, request_id="push_req_%08d" % next(_PUSH_REQUEST_IDS))
    headers["X-ForkMesh-Issued-At"] = str(issued_at)
    return headers


def write_writable_config(tmp_path, *, owners=("mobile-owner",)):
    root = tmp_path / "writable"
    root.mkdir(mode=0o700)
    config_path = write_config(tmp_path, None, repositories=[])
    value = json.loads(config_path.read_text(encoding="utf-8"))
    value["writableRepositoryRoot"] = str(root)
    value["writableOwners"] = list(owners)
    config_path.write_text(json.dumps(value), encoding="utf-8")
    return config_path, root


def make_push_application(tmp_path, *, owners=("mobile-owner",), clock=None):
    config_path, root = write_writable_config(tmp_path, owners=owners)
    config = gateway.load_config(config_path)
    logs = []
    app = gateway.GatewayApplication(
        config,
        verifier=AcceptVerifier(),
        health_signer=HealthSigner(),
        clock_ms=(clock or (lambda: NOW)),
        log=logs.append,
    )
    return app, root, config_path, logs


class _PushGitHandler(BaseHTTPRequestHandler):
    """Bridge a real git client onto GatewayApplication.dispatch."""

    application = None
    protocol_version = "HTTP/1.1"

    def log_message(self, *_arguments):
        pass

    def _reply(self, status, content_type, payload):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self):
        path, _, query = self.path.partition("?")
        parts = path.split("/")
        service = ""
        for pair in query.split("&"):
            if pair.startswith("service="):
                service = pair.split("=", 1)[1]
        if service == "git-receive-pack":
            target = (
                "/v1/repositories/%s/%s/git-receive-pack"
                "?service=git-receive-pack" % (parts[1], parts[2]))
        else:
            target = (
                "/v1/repositories/%s/%s/git-info-refs"
                "?service=git-upload-pack" % (parts[1], parts[2]))
        response = self.application.dispatch(
            "GET", target, push_capability_headers(), b"")
        self._reply(response.status, response.content_type, response.body)

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length)
        parts = self.path.split("/")
        target = "/v1/repositories/%s/%s/%s" % (parts[1], parts[2], parts[3])
        headers = push_capability_headers(body)
        headers["Content-Type"] = self.headers.get("Content-Type", "")
        if self.headers.get("Content-Encoding"):
            headers["Content-Encoding"] = self.headers["Content-Encoding"]
        response = self.application.dispatch("POST", target, headers, body)
        if response.stream is not None:
            completed = subprocess.run(
                response.stream.command,
                input=response.stream.input_bytes,
                capture_output=True,
                env=gateway._git_environment(),
                timeout=30,
                check=False,
            )
            self._reply(
                response.status if completed.returncode == 0 else 500,
                response.stream.content_type,
                completed.stdout,
            )
            return
        self._reply(response.status, response.content_type, response.body)


def make_push_client(tmp_path, origin):
    source = tmp_path / "push-client"
    run(["git", "init", "-b", "main", str(source)], tmp_path)
    run(["git", "config", "user.email", "phone@example.test"], source)
    run(["git", "config", "user.name", "Phone"], source)
    (source / "README.md").write_text("# pushed from device\n", encoding="utf-8")
    run(["git", "add", "."], source)
    run(["git", "commit", "-m", "First device commit"], source)
    run(["git", "remote", "add", "origin", origin], source)
    return source


def _git_push(source, *arguments):
    return subprocess.run(
        ["git", "push", *arguments],
        cwd=source,
        capture_output=True,
        text=True,
        timeout=30,
        check=False,
        env=gateway._git_environment(),
    )


def test_real_git_push_creates_repo_sidecar_and_refresh_marker(
    tmp_path, monkeypatch
):
    clock = {"now": NOW}
    app, root, config_path, _logs = make_push_application(
        tmp_path, clock=lambda: clock["now"])
    pending = tmp_path / "run" / "pending"
    pending.parent.mkdir(mode=0o700)
    monkeypatch.setattr(gateway, "REFRESH_PENDING_PATH", pending)
    server = ThreadingHTTPServer(("127.0.0.1", 0), _PushGitHandler)
    server.RequestHandlerClass.application = app
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        origin = "http://127.0.0.1:%d/mobile-owner/app" % (
            server.server_address[1])
        source = make_push_client(tmp_path, origin)
        pushed = _git_push(source, "origin", "main")
        assert pushed.returncode == 0, pushed.stderr

        git_dir = root / "mobile-owner" / "app.git"
        assert (git_dir / "HEAD").is_file()
        client_head = run(["git", "rev-parse", "HEAD"], source)
        assert run(
            ["git", "--git-dir", str(git_dir), "rev-parse",
             "refs/heads/main"], tmp_path) == client_head

        sidecar_path = config_path.parent / "hosted-pushes.json"
        sidecar = json.loads(sidecar_path.read_text(encoding="utf-8"))
        assert sidecar["type"] == "forkmesh.hosted-pushes"
        (entry,) = sidecar["repositories"]
        assert entry["owner"] == "mobile-owner"
        assert entry["name"] == "app"
        assert entry["gitDir"] == str(git_dir)
        assert entry["firstPushedAt"] == NOW
        assert entry["lastPushedAt"] == NOW
        # Atomic write leaves no temporary sibling behind.
        assert not [
            leftover for leftover in config_path.parent.iterdir()
            if leftover.name.startswith(".hosted-pushes.json.")
        ]
        assert pending.read_text(encoding="utf-8") == "forkmesh-refresh-v1\n"

        # Hooks are disabled even when an attacker (or operator) plants one.
        hook_marker = tmp_path / "hook-ran"
        for hook in ("pre-receive", "post-receive", "update"):
            hook_path = git_dir / "hooks" / hook
            hook_path.write_text(
                "#!/bin/sh\ntouch %s\n" % hook_marker, encoding="utf-8")
            hook_path.chmod(0o755)
        (source / "second.txt").write_text("second\n", encoding="utf-8")
        run(["git", "add", "."], source)
        run(["git", "commit", "-m", "Second device commit"], source)
        clock["now"] = NOW + 30_000
        second = _git_push(source, "origin", "main")
        assert second.returncode == 0, second.stderr
        assert not hook_marker.exists()
        sidecar = json.loads(sidecar_path.read_text(encoding="utf-8"))
        (entry,) = sidecar["repositories"]
        assert entry["firstPushedAt"] == NOW
        assert entry["lastPushedAt"] == NOW + 30_000

        # Deletes are refused outright.
        head_before = run(
            ["git", "--git-dir", str(git_dir), "rev-parse",
             "refs/heads/main"], tmp_path)
        deletion = _git_push(source, "origin", "--delete", "main")
        assert deletion.returncode != 0
        assert run(
            ["git", "--git-dir", str(git_dir), "rev-parse",
             "refs/heads/main"], tmp_path) == head_before

        # Non-fast-forward updates are refused even with --force.
        run(["git", "reset", "--hard", "HEAD~1"], source)
        (source / "rewritten.txt").write_text("rewrite\n", encoding="utf-8")
        run(["git", "add", "."], source)
        run(["git", "commit", "-m", "Rewritten history"], source)
        forced = _git_push(source, "--force", "origin", "main")
        assert forced.returncode != 0
        assert run(
            ["git", "--git-dir", str(git_dir), "rev-parse",
             "refs/heads/main"], tmp_path) == head_before
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)
        app.close()

    # The pushed repository is served for reads and proves git-receive-pack
    # in its signed health projection — including after a restart that only
    # has the sidecar to go on.
    restarted = gateway.GatewayApplication(
        gateway.load_config(config_path),
        verifier=AcceptVerifier(),
        health_signer=HealthSigner(),
        clock_ms=lambda: NOW,
        log=lambda _event: None,
    )
    try:
        advertisement = restarted.dispatch(
            "GET",
            "/v1/repositories/mobile-owner/app/git-info-refs"
            "?service=git-upload-pack",
            push_capability_headers(),
            b"",
        )
        assert advertisement.status == 200
        assert b"refs/heads/main" in advertisement.body
        health = restarted.dispatch(
            "GET",
            "/health?nonce=%s&issuedAt=%d&owner=mobile-owner&repo=app"
            % ("N" * 24, NOW),
            {},
            b"",
        )
        proof = json.loads(health.body)["repositoryProof"]
        assert proof["available"] is True
        assert "git-receive-pack" in proof["operations"]
    finally:
        restarted.close()


def test_push_repo_creation_is_refused_for_non_allowlisted_owner(tmp_path):
    app, root, _config_path, _logs = make_push_application(
        tmp_path, owners=("mobile-owner",))
    try:
        for method, target, body in (
            (
                "GET",
                "/v1/repositories/intruder/app/git-receive-pack"
                "?service=git-receive-pack",
                b"",
            ),
            ("POST", "/v1/repositories/intruder/app/git-receive-pack", b"x"),
        ):
            headers = push_capability_headers(body)
            if method == "POST":
                headers["Content-Type"] = (
                    "application/x-git-receive-pack-request")
            response = app.dispatch(method, target, headers, body)
            assert response.status == 404
        assert not (root / "intruder").exists()
        # The allowlisted owner is refused everywhere except the writable
        # root: a configured read-only repository never becomes writable.
        assert app.dispatch(
            "GET",
            "/v1/repositories/alice/project/git-receive-pack"
            "?service=git-receive-pack",
            push_capability_headers(),
            b"",
        ).status == 404
    finally:
        app.close()


def test_wildcard_writable_owners_accepts_any_authorized_owner(tmp_path):
    """A node hosting website-created repositories cannot list its owners.

    The owner of a repository created in a browser (issue #676) has no node
    of their own, and the hosting node never learns which accounts it will
    hold, so ["*"] stands in for the name list. Authorization does not move:
    the request below still has to carry a verified router capability.
    """
    app, root, _config_path, _logs = make_push_application(
        tmp_path, owners=("*",))
    try:
        target = (
            "/v1/repositories/web-newcomer/first-project/git-receive-pack"
            "?service=git-receive-pack")
        advertisement = app.dispatch(
            "GET", target, push_capability_headers(), b"")
        assert advertisement.status == 200
        assert (root / "web-newcomer" / "first-project.git").is_dir()
        assert ("web-newcomer", "first-project") in app.writable_repositories

        # An unauthorized request is still refused under the wildcard.
        unsigned = app.dispatch("GET", target, {}, b"")
        assert unsigned.status in (401, 403, 404)
    finally:
        app.close()


def test_wildcard_writable_owners_must_be_the_only_entry(tmp_path):
    # A mixed list reads as narrower than it is, so it is refused outright
    # rather than silently widened to every owner.
    config_path, _root = write_writable_config(
        tmp_path, owners=("*", "mobile-owner"))
    with pytest.raises(gateway.GatewayError, match="wildcard"):
        gateway.load_config(config_path)


def test_revoking_a_writable_owner_stops_serving_its_pushed_repository(
    tmp_path,
):
    """`writableOwners` gates serving, not only first creation.

    Removing an owner is the operator's revocation control. The refresh cycle
    already stops rendering that owner's push-created repositories
    (tools/headless_mirror_refresh._pushed_gateway_repositories), so a running
    gateway that kept answering reads and receive-pack out of its own
    hosted-pushes sidecar would silently disagree with the signed
    configuration and leave push access alive after revocation.
    """
    upstream, _commit = make_bare_repository(tmp_path)
    root = tmp_path / "writable"
    root.mkdir(mode=0o700)
    owner_root = root / "mobile-owner"
    owner_root.mkdir(mode=0o700)
    pushed = owner_root / "app.git"
    run(["git", "clone", "--bare", str(upstream), str(pushed)], tmp_path)
    pushed.chmod(0o700)

    def application(owners, slot):
        # Two gateways over the identical writable root, sidecar and bare
        # repository. The allowlist is the only difference between them.
        base = tmp_path / slot
        base.mkdir()
        config_path = write_config(base, None, repositories=[])
        value = json.loads(config_path.read_text(encoding="utf-8"))
        value["writableRepositoryRoot"] = str(root)
        value["writableOwners"] = list(owners)
        config_path.write_text(json.dumps(value), encoding="utf-8")
        sidecar = base / gateway.HOSTED_PUSHES_FILE
        sidecar.write_text(
            json.dumps(
                {
                    "schemaVersion": 1,
                    "type": gateway.HOSTED_PUSHES_TYPE,
                    "repositories": [
                        {
                            "owner": "mobile-owner",
                            "name": "app",
                            "gitDir": str(pushed),
                            "firstPushedAt": NOW,
                            "lastPushedAt": NOW,
                        }
                    ],
                }
            ),
            encoding="utf-8",
        )
        sidecar.chmod(0o600)
        return gateway.GatewayApplication(
            gateway.load_config(config_path),
            verifier=AcceptVerifier(),
            health_signer=HealthSigner(),
            clock_ms=lambda: NOW,
            log=lambda _event: None,
        )

    read = ("/v1/repositories/mobile-owner/app/git-info-refs"
            "?service=git-upload-pack")
    write = ("/v1/repositories/mobile-owner/app/git-receive-pack"
             "?service=git-receive-pack")
    health = (
        "/health?nonce=%s&issuedAt=%d&owner=mobile-owner&repo=app"
        % ("N" * 24, NOW))

    allowed = application(("mobile-owner",), "allowed")
    try:
        assert ("mobile-owner", "app") in allowed.writable_repositories
        assert allowed.dispatch(
            "GET", read, push_capability_headers(), b"").status == 200
        assert allowed.dispatch(
            "GET", write, push_capability_headers(), b"").status == 200
        proof = json.loads(
            allowed.dispatch("GET", health, {}, b"").body)["repositoryProof"]
        assert proof["available"] is True
        assert "git-receive-pack" in proof["operations"]
    finally:
        allowed.close()

    revoked = application(("someone-else",), "revoked")
    try:
        assert revoked.writable_repositories == {}
        assert revoked.dispatch(
            "GET", read, push_capability_headers(), b"").status == 404
        assert revoked.dispatch(
            "GET", write, push_capability_headers(), b"").status == 404
        post = push_capability_headers(b"payload")
        post["Content-Type"] = "application/x-git-receive-pack-request"
        assert revoked.dispatch(
            "POST",
            "/v1/repositories/mobile-owner/app/git-receive-pack",
            post,
            b"payload",
        ).status == 404
        proof = json.loads(
            revoked.dispatch("GET", health, {}, b"").body)["repositoryProof"]
        assert proof["available"] is False
        assert proof["operations"] == []
        # Revocation stops serving; it never touches the bytes on disk.
        assert (pushed / "HEAD").is_file()
    finally:
        revoked.close()


def test_capability_signed_for_another_target_is_refused(tmp_path):
    class UploadPackOnlyVerifier:
        def verify(self, message, signature):
            return signature == SIGNATURE and "/git-upload-pack" in message

    config_path, _root = write_writable_config(tmp_path)
    app = gateway.GatewayApplication(
        gateway.load_config(config_path),
        verifier=UploadPackOnlyVerifier(),
        health_signer=HealthSigner(),
        clock_ms=lambda: NOW,
        log=lambda _event: None,
    )
    try:
        headers = push_capability_headers(b"payload")
        headers["Content-Type"] = "application/x-git-receive-pack-request"
        response = app.dispatch(
            "POST",
            "/v1/repositories/mobile-owner/app/git-receive-pack",
            headers,
            b"payload",
        )
        assert response.status == 404
    finally:
        app.close()


def _push_limits(config_path, receive_max_input_bytes):
    value = json.loads(config_path.read_text(encoding="utf-8"))
    value["limits"] = {"receiveMaxInputBytes": receive_max_input_bytes}
    config_path.write_text(json.dumps(value), encoding="utf-8")


def test_oversized_push_body_is_refused_before_any_git_work(tmp_path):
    receive_max = 16 * 1024 * 1024
    config_path, root = write_writable_config(tmp_path)
    _push_limits(config_path, receive_max)
    app = gateway.GatewayApplication(
        gateway.load_config(config_path),
        verifier=AcceptVerifier(),
        health_signer=HealthSigner(),
        clock_ms=lambda: NOW,
        log=lambda _event: None,
    )
    try:
        target = "/v1/repositories/mobile-owner/app/git-receive-pack"
        body = b"x" * (receive_max + 1)
        headers = push_capability_headers(body)
        headers["Content-Type"] = "application/x-git-receive-pack-request"
        assert app.dispatch("POST", target, headers, body).status == 413
        # The pack-size limit is the operator's receive.maxInputSize, not the
        # base request cap every other route keeps.
        body = b"x" * (gateway.MAX_REQUEST_BODY + 1)
        headers = push_capability_headers(body)
        headers["Content-Type"] = "application/x-git-receive-pack-request"
        assert app.dispatch("POST", target, headers, body).status != 413
        headers = capability_headers(
            "/v1/repositories/alice/project/blob", body=body,
            request_id="oversized_read_01")
        assert app.dispatch(
            "POST", "/v1/repositories/alice/project/blob", headers, body,
        ).status == 413
    finally:
        app.close()
    assert not list(
        (root / "mobile-owner" / "app.git" / "objects").glob("**/*.pack"))


def test_request_body_limit_is_decided_from_verified_headers_alone(tmp_path):
    """Nothing unsigned may make the gateway hold more than the base cap."""
    receive_max = 32 * 1024 * 1024
    config_path, _root = write_writable_config(tmp_path)
    _push_limits(config_path, receive_max)
    target = "/v1/repositories/mobile-owner/app/git-receive-pack"

    class SignatureVerifier:
        def verify(self, message, signature):
            return signature == SIGNATURE and "\n" + NOW_TEXT in message

    NOW_TEXT = str(NOW)
    app = gateway.GatewayApplication(
        gateway.load_config(config_path),
        verifier=SignatureVerifier(),
        health_signer=HealthSigner(),
        clock_ms=lambda: NOW,
        log=lambda _event: None,
    )
    try:
        headers = push_capability_headers()
        assert app.request_body_limit("POST", target, headers) == receive_max
        # Same headers, any other shape: base cap.
        assert app.request_body_limit("GET", target, headers) == (
            gateway.MAX_REQUEST_BODY)
        assert app.request_body_limit(
            "POST", "/v1/repositories/mobile-owner/app/git-upload-pack",
            headers) == gateway.MAX_REQUEST_BODY
        assert app.request_body_limit("POST", target, {}) == (
            gateway.MAX_REQUEST_BODY)
        forged = dict(headers, **{"X-ForkMesh-Signature": "forged"})
        assert app.request_body_limit("POST", target, forged) == (
            gateway.MAX_REQUEST_BODY)
        stale = push_capability_headers(
            issued_at=NOW - gateway.CAPABILITY_WINDOW_MS - 1)
        assert app.request_body_limit("POST", target, stale) == (
            gateway.MAX_REQUEST_BODY)
        # Deciding the limit claims nothing: the same capability still
        # authorizes its one request afterwards.
        headers = push_capability_headers(b"0000")
        assert app.request_body_limit("POST", target, headers) == receive_max
        headers["Content-Type"] = "application/x-git-receive-pack-request"
        assert app.dispatch("POST", target, headers, b"0000").status == 200
    finally:
        app.close()


def test_streamed_body_capability_is_honoured_only_for_receive_pack_post(
        tmp_path, application):
    """The Worker cannot digest a pack it streams, so it binds the
    capability to the fixed streamed-body digest; that binding is not a
    wildcard for any other route or method."""
    import edge_routing
    assert gateway.STREAMED_REQUEST_BODY_SHA256 == (
        edge_routing.STREAMED_REQUEST_BODY_SHA256)
    assert gateway.SHA256_RE.fullmatch(gateway.STREAMED_REQUEST_BODY_SHA256)
    marker = gateway.STREAMED_REQUEST_BODY_SHA256

    app, _root, _config_path, _logs = make_push_application(tmp_path)
    try:
        target = "/v1/repositories/mobile-owner/app/git-receive-pack"
        headers = push_capability_headers()
        headers["X-ForkMesh-Body-Sha256"] = marker
        headers["Content-Type"] = "application/x-git-receive-pack-request"
        # The body's own digest is not the marker, yet the push is accepted.
        assert app.dispatch("POST", target, headers, b"0000").status == 200
        # The advertisement GET on the same target is not.
        headers = push_capability_headers()
        headers["X-ForkMesh-Body-Sha256"] = marker
        assert app.dispatch(
            "GET", target + "?service=git-receive-pack", headers, b"",
        ).status == 404
    finally:
        app.close()

    read_app, _commit, _release_hash, _logs = application
    for method, target, body in (
        ("GET", "/v1/repositories/alice/project/blob?path=README.md", b""),
        ("POST", "/v1/repositories/alice/project/git-upload-pack", b"0000"),
    ):
        headers = capability_headers(
            target, method=method, body=body,
            request_id="streamed_marker_%s_01" % method.lower())
        headers["X-ForkMesh-Body-Sha256"] = marker
        if method == "POST":
            headers["Content-Type"] = "application/x-git-upload-pack-request"
        assert read_app.dispatch(method, target, headers, body).status == 404


def test_http_adapter_reads_a_large_chunked_push_body_within_its_limit():
    limit = 16 * 1024 * 1024

    def decode(chunks, limit):
        handler = object.__new__(gateway.MirrorGatewayHandler)
        handler.headers = {"Transfer-Encoding": "chunked"}
        handler.rfile = io.BytesIO(chunks)
        return handler._read_body(limit)

    piece = b"p" * 65536
    frame = b"10000\r\n" + piece + b"\r\n"
    count = (9 * 1024 * 1024) // len(piece)
    encoded = frame * count + b"0\r\n\r\n"
    body = decode(encoded, limit)
    assert len(body) == count * len(piece)
    assert body[:8] == b"pppppppp" and body[-8:] == b"pppppppp"
    with pytest.raises(gateway.GatewayError):
        decode(encoded, gateway.MAX_REQUEST_BODY)
    # The chunk-count guard scales with the allowance rather than capping
    # a legitimate stream of small pieces the Worker's fetch may emit.
    small = b"100\r\n" + b"s" * 256 + b"\r\n"
    many = gateway.MAX_REQUEST_CHUNKS * 4
    assert len(decode(small * many + b"0\r\n\r\n", limit)) == many * 256
    with pytest.raises(gateway.GatewayError):
        decode(small * (gateway.MAX_REQUEST_CHUNKS + 1) + b"0\r\n\r\n",
               gateway.MAX_REQUEST_BODY)
    with pytest.raises(gateway.GatewayError):
        decode(b"1\r\nx\r\n" * (limit // 256 + 1) + b"0\r\n\r\n", limit)


def test_receive_pack_deadline_grows_with_the_pack():
    floor = gateway.RECEIVE_PACK_TIMEOUT_SECONDS
    assert gateway.receive_pack_timeout_seconds(0) == floor
    assert gateway.receive_pack_timeout_seconds(4096) == floor
    assert gateway.receive_pack_timeout_seconds(256 * 1024 * 1024) == (
        floor + 256)


class _StreamingPushBridge(BaseHTTPRequestHandler):
    """What the Worker does for a large push: forward git's request to the
    real gateway server chunked, under a streamed-body capability."""

    gateway_port = 0
    protocol_version = "HTTP/1.1"

    def log_message(self, *_arguments):
        pass

    def _read_request_body(self):
        if self.headers.get("Transfer-Encoding", "").lower() == "chunked":
            body = bytearray()
            while True:
                size = int(self.rfile.readline().strip(), 16)
                if size == 0:
                    self.rfile.readline()
                    return bytes(body)
                body.extend(self.rfile.read(size))
                self.rfile.readline()
        return self.rfile.read(int(self.headers.get("Content-Length") or 0))

    def _forward(self, method, target, headers, body):
        connection = http.client.HTTPConnection(
            "127.0.0.1", self.gateway_port, timeout=120)
        connection.putrequest(method, target)
        for key, value in headers.items():
            connection.putheader(key, value)
        if method == "POST":
            connection.putheader("Transfer-Encoding", "chunked")
        connection.endheaders()
        if method == "POST":
            for offset in range(0, len(body), 65536):
                piece = body[offset:offset + 65536]
                connection.send(b"%x\r\n" % len(piece) + piece + b"\r\n")
            connection.send(b"0\r\n\r\n")
        response = connection.getresponse()
        payload = response.read()
        self.send_response(response.status)
        self.send_header("Content-Type", response.getheader("Content-Type"))
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)
        connection.close()

    def do_GET(self):
        path, _, query = self.path.partition("?")
        parts = path.split("/")
        service = dict(
            pair.split("=", 1) for pair in query.split("&") if "=" in pair
        ).get("service", "")
        if service == "git-receive-pack":
            target = (
                "/v1/repositories/%s/%s/git-receive-pack"
                "?service=git-receive-pack" % (parts[1], parts[2]))
        else:
            target = (
                "/v1/repositories/%s/%s/git-info-refs"
                "?service=git-upload-pack" % (parts[1], parts[2]))
        self._forward("GET", target, push_capability_headers(), b"")

    def do_POST(self):
        body = self._read_request_body()
        parts = self.path.split("/")
        target = "/v1/repositories/%s/%s/%s" % (parts[1], parts[2], parts[3])
        headers = push_capability_headers()
        headers["X-ForkMesh-Body-Sha256"] = (
            gateway.STREAMED_REQUEST_BODY_SHA256)
        headers["Content-Type"] = self.headers.get("Content-Type", "")
        if self.headers.get("Content-Encoding"):
            headers["Content-Encoding"] = self.headers["Content-Encoding"]
        self._forward("POST", target, headers, body)


def test_real_git_push_of_a_pack_above_the_base_cap_streams_end_to_end(
        tmp_path, monkeypatch):
    """A repository well past the old 8 MiB ceiling lands through the real
    HTTP adapter: chunked body, spooled read, streamed-body capability,
    size-scaled receive-pack deadline."""
    config_path, root = write_writable_config(tmp_path)
    _push_limits(config_path, 64 * 1024 * 1024)
    app = gateway.GatewayApplication(
        gateway.load_config(config_path),
        verifier=AcceptVerifier(),
        health_signer=HealthSigner(),
        clock_ms=lambda: NOW,
        log=lambda _event: None,
    )
    pending = tmp_path / "run" / "pending"
    pending.parent.mkdir(mode=0o700)
    monkeypatch.setattr(gateway, "REFRESH_PENDING_PATH", pending)
    server = gateway.MirrorGatewayServer(("127.0.0.1", 0), app)
    server_thread = threading.Thread(target=server.serve_forever, daemon=True)
    server_thread.start()
    bridge = ThreadingHTTPServer(("127.0.0.1", 0), _StreamingPushBridge)
    bridge.RequestHandlerClass.gateway_port = server.server_address[1]
    bridge_thread = threading.Thread(target=bridge.serve_forever, daemon=True)
    bridge_thread.start()
    try:
        origin = "http://127.0.0.1:%d/mobile-owner/big" % (
            bridge.server_address[1])
        source = make_push_client(tmp_path, origin)
        # Incompressible, so the pack itself is comfortably above the base
        # request cap and git sends it chunked (over http.postBuffer).
        payload = random.Random(7).randbytes(12 * 1024 * 1024)
        (source / "assets.bin").write_bytes(payload)
        run(["git", "add", "."], source)
        run(["git", "commit", "-m", "Large import"], source)
        pushed = subprocess.run(
            ["git", "push", "origin", "main"],
            cwd=source, capture_output=True, text=True, timeout=120,
            check=False, env=gateway._git_environment())
        assert pushed.returncode == 0, pushed.stderr

        git_dir = root / "mobile-owner" / "big.git"
        assert run(
            ["git", "--git-dir", str(git_dir), "rev-parse",
             "refs/heads/main"], tmp_path) == run(
                ["git", "rev-parse", "HEAD"], source)
        served = subprocess.run(
            ["git", "--git-dir", str(git_dir), "cat-file", "-p",
             "HEAD:assets.bin"],
            capture_output=True, timeout=60, check=True)
        assert served.stdout == payload
        assert pending.read_text(encoding="utf-8") == "forkmesh-refresh-v1\n"
    finally:
        bridge.shutdown()
        bridge.server_close()
        bridge_thread.join(timeout=5)
        server.shutdown()
        server.server_close()
        server_thread.join(timeout=5)


def test_writable_configuration_is_validated_fail_closed(tmp_path):
    config_path, root = write_writable_config(tmp_path)

    def mutate(**changes):
        value = json.loads(config_path.read_text(encoding="utf-8"))
        value.update(changes)
        mutated = tmp_path / "mutated-gateway.json"
        mutated.write_text(json.dumps(value), encoding="utf-8")
        return mutated

    with pytest.raises(gateway.GatewayError, match="writableOwners requires"):
        value = json.loads(config_path.read_text(encoding="utf-8"))
        del value["writableRepositoryRoot"]
        stripped = tmp_path / "stripped-gateway.json"
        stripped.write_text(json.dumps(value), encoding="utf-8")
        gateway.load_config(stripped)
    with pytest.raises(gateway.GatewayError, match="writableOwners"):
        gateway.load_config(mutate(writableOwners=["Bad Owner!"]))
    with pytest.raises(gateway.GatewayError, match="writableRepositoryRoot"):
        gateway.load_config(
            mutate(writableRepositoryRoot=str(tmp_path / "missing")))
    link = tmp_path / "writable-link"
    link.symlink_to(root)
    with pytest.raises(gateway.GatewayError, match="writableRepositoryRoot"):
        gateway.load_config(mutate(writableRepositoryRoot=str(link)))
    with pytest.raises(gateway.GatewayError, match="receiveMaxInputBytes"):
        gateway.load_config(mutate(limits={"receiveMaxInputBytes": 1}))

    loaded = gateway.load_config(config_path)
    assert loaded.writable_repository_root == root
    assert loaded.writable_owners == frozenset({"mobile-owner"})
    # Unset, a push-hosting node accepts the SSH gateway's pack size rather
    # than the base request cap.
    assert loaded.receive_max_input_bytes == (
        gateway.DEFAULT_RECEIVE_MAX_INPUT_BYTES)
    assert gateway.DEFAULT_RECEIVE_MAX_INPUT_BYTES == 256 * 1024 * 1024


def test_refresh_rendered_push_repository_serves_reads_writes_and_moving_refs(
    tmp_path,
):
    """The shape tools/headless_mirror_refresh.py renders for a pushed repo.

    Once the refresh cycle picks a repository up from hosted-pushes.json it
    stops being a sidecar-only entry and becomes a normal signed-configuration
    repository that additionally carries git-receive-pack. It must keep every
    read route, keep accepting pushes, and it must not be quarantined when its
    refs legitimately move behind the rendered pin.
    """
    root = tmp_path / "writable"
    root.mkdir(mode=0o700)
    owner_root = root / "mobile-owner"
    owner_root.mkdir(mode=0o700)
    bare, _commit = make_bare_repository(tmp_path)
    pushed = owner_root / "app.git"
    run(["git", "clone", "--bare", str(bare), str(pushed)], tmp_path)
    rendered_pin = gateway.refs_sha256(pushed)
    operations = [
        "branches",
        "blob",
        "git-info-refs",
        "git-receive-pack",
        "git-upload-pack",
        "raw",
        "tree",
    ]
    config_path = write_config(
        tmp_path,
        bare,
        repositories=[
            {
                "owner": "alice",
                "name": "project",
                "visibility": "public",
                "enabled": True,
                "gitDir": str(bare),
                "integrity": {
                    "expectedRefsSha256": gateway.refs_sha256(bare)},
            },
            {
                "owner": "mobile-owner",
                "name": "app",
                "visibility": "public",
                "enabled": True,
                "gitDir": str(pushed),
                "integrity": {"expectedRefsSha256": rendered_pin},
                "operations": operations,
            },
        ],
    )
    value = json.loads(config_path.read_text(encoding="utf-8"))
    value["writableRepositoryRoot"] = str(root)
    value["writableOwners"] = ["mobile-owner"]
    config_path.write_text(json.dumps(value), encoding="utf-8")

    app = gateway.GatewayApplication(
        gateway.load_config(config_path),
        verifier=AcceptVerifier(),
        health_signer=HealthSigner(),
        clock_ms=lambda: NOW,
        log=lambda _event: None,
    )
    try:
        assert app.quarantined_count == 0
        # The rendered entry is a configured repository, not a sidecar one.
        assert ("mobile-owner", "app") in app.repositories
        assert ("mobile-owner", "app") not in app.writable_repositories

        target = (
            "/v1/repositories/mobile-owner/app/git-info-refs"
            "?service=git-upload-pack")
        advertisement = app.dispatch(
            "GET", target, capability_headers(target), b"")
        assert advertisement.status == 200
        assert b"refs/heads/main" in advertisement.body

        # A further authorized push moves refs away from the rendered pin,
        # including the routing generation the signed proof advertises.
        routing_before = gateway.routing_refs_sha256(pushed)
        run(["git", "--git-dir", str(pushed), "update-ref",
             "refs/tags/v1", "refs/heads/main"], tmp_path)
        assert gateway.refs_sha256(pushed) != rendered_pin
        assert gateway.routing_refs_sha256(pushed) != routing_before
        assert app.repositories[("mobile-owner", "app")].check_integrity(
            force=True) is True
        assert app.repositories[("alice", "project")].check_integrity(
            force=True) is True

        health = app.dispatch(
            "GET",
            "/health?nonce=%s&issuedAt=%d&owner=mobile-owner&repo=app"
            % ("N" * 24, NOW),
            {},
            b"",
        )
        proof = json.loads(health.body)["repositoryProof"]
        assert proof["available"] is True
        assert proof["integrity"] == "ok"
        assert "git-receive-pack" in proof["operations"]
        assert proof["refsSha256"] == gateway.routing_refs_sha256(pushed)

        # The ordinary mirror keeps the strict immutable pin: the same kind
        # of ref motion the pushed repository is allowed quarantines it.
        run(["git", "--git-dir", str(bare), "update-ref",
             "refs/tags/rogue", "refs/heads/main"], tmp_path)
        assert app.repositories[("alice", "project")].check_integrity(
            force=True) is False
        rogue = "/v1/repositories/alice/project/branches"
        assert app.dispatch(
            "GET", rogue, capability_headers(rogue), b"").status == 404
    finally:
        app.close()


def _node_log_request(app, viewer_private, *, request_id="viewer_request_01",
                      issued_at=NOW, node="mirror-a", tamper=False, query=""):
    from cryptography.hazmat.primitives import serialization

    viewer = base64.urlsafe_b64encode(
        viewer_private.public_key().public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw
        )
    ).decode().rstrip("=")
    message = gateway.node_logs_message(node, viewer, request_id, issued_at)
    signature = viewer_private.sign(message.encode())
    if tamper:
        signature = bytes([signature[0] ^ 1]) + signature[1:]
    headers = {
        "X-ForkMesh-Log-Viewer": viewer,
        "X-ForkMesh-Request-Id": request_id,
        "X-ForkMesh-Issued-At": str(issued_at),
        "X-ForkMesh-Signature": base64.urlsafe_b64encode(signature)
        .decode()
        .rstrip("="),
    }
    target = gateway.NODE_LOGS_PATH + (("?" + query) if query else "")
    return viewer, app.dispatch("GET", target, headers, b"")


def test_node_logs_open_only_for_a_listed_viewer_once(
    application, tmp_path, monkeypatch
):
    from cryptography.hazmat.primitives.asymmetric.ed25519 import (
        Ed25519PrivateKey,
    )

    app = application[0]
    listed = Ed25519PrivateKey.generate()
    stranger = Ed25519PrivateKey.generate()
    keys = tmp_path / "log-viewer-keys"
    monkeypatch.setenv(gateway.NODE_LOG_VIEWER_KEYS_ENV, str(keys))

    # No key file: the host offers no feed at all.
    _viewer, response = _node_log_request(app, listed)
    assert response.status == 404

    viewer, _ = _node_log_request(app, listed, request_id="viewer_request_00")
    keys.write_text("# desktop keys\n" + viewer + "\nnot-a-key\n")
    assert gateway.load_log_viewer_keys() == frozenset({viewer})

    _viewer, response = _node_log_request(app, listed)
    assert response.status == 200
    assert response.content_type.startswith("text/event-stream")
    assert response.stream.kind == "node-logs"
    assert response.stream.content_length is None
    assert response.stream.command[0] == "journalctl"
    assert "short-iso" in response.stream.command

    refusals = [
        _node_log_request(app, listed)[1],  # replayed request id
        _node_log_request(app, stranger, request_id="viewer_request_02")[1],
        _node_log_request(app, listed, request_id="viewer_request_03",
                          tamper=True)[1],
        _node_log_request(app, listed, request_id="viewer_request_04",
                          issued_at=NOW - gateway.CAPABILITY_WINDOW_MS - 1)[1],
        _node_log_request(app, listed, request_id="viewer_request_05",
                          node="mirror-b")[1],
        _node_log_request(app, listed, request_id="viewer_request_06",
                          query="x=1")[1],
    ]
    assert [r.status for r in refusals] == [404] * len(refusals)
    assert all(r.stream is None for r in refusals)


def test_node_logs_stream_relays_journal_lines_as_events():
    handler = gateway.MirrorGatewayHandler.__new__(gateway.MirrorGatewayHandler)
    handler.wfile = io.BytesIO()
    script = (
        "import sys; "
        "sys.stdout.write('2026-09-21T10:00:00+0000 mirror-a sshd[1]: hi\\n"
        "2026-09-21T10:00:01+0000 mirror-a forkmesh-mirror-node[2]: synced\\n"
        "tail-without-newline'); sys.stdout.flush()"
    )
    spec = gateway.StreamSpec(
        kind="node-logs",
        content_type="text/event-stream",
        content_length=None,
        command=(sys.executable, "-c", script),
    )
    total = handler._stream(spec)
    body = handler.wfile.getvalue().decode()
    assert total == len(body.encode())
    events = [chunk[len("data: "):] for chunk in body.split("\n\n") if chunk]
    assert events == [
        "K",
        "J 2026-09-21T10:00:00+0000 mirror-a sshd[1]: hi",
        "J 2026-09-21T10:00:01+0000 mirror-a forkmesh-mirror-node[2]: synced",
        "J tail-without-newline",
        "M host journalctl exited",
    ]


def test_node_logs_message_matches_the_desktop_signer():
    # desktop/src/HostLogSources.h nodeLogsMessage signs these exact bytes.
    key = "A" * 43
    assert gateway.node_logs_message(
        "mirror35", key, "0123456789abcdef", 2_000_000_000_000
    ) == (
        "forkmesh-node-logs-v1\nmirror35\n" + key
        + "\n0123456789abcdef\n2000000000000"
    )
    assert gateway.node_logs_message("Mirror35!", key, "0123456789abcdef", 1) == ""
    assert gateway.node_logs_message("mirror35", "short", "0123456789abcdef", 1) == ""
    assert gateway.node_logs_message("mirror35", key, "short", 1) == ""


def test_broken_served_refs_reports_empty_and_missing_refs(tmp_path):
    source = tmp_path / "source"
    run(["git", "init", "-q", "-b", "main", str(source)], tmp_path)
    run(["git", "-c", "user.email=t@example.test", "-c", "user.name=T",
         "commit", "-q", "--allow-empty", "-m", "one"], source)
    bare = tmp_path / "served.git"
    run(["git", "clone", "-q", "--bare", str(source), str(bare)], tmp_path)
    assert gateway.broken_served_refs(bare) == []
    (bare / "refs" / "heads" / "empty").write_text("")
    (bare / "refs" / "heads" / "missing").write_text("ab" * 20 + "\n")
    (bare / "refs" / "heads" / "held.lock").write_text("")
    internal = bare / "refs" / "forkmesh" / "upstream" / "heads"
    internal.mkdir(parents=True)
    (internal / "empty").write_text("")
    assert gateway.broken_served_refs(bare) == [
        "refs/heads/empty", "refs/heads/missing"]


def test_reads_resolve_only_visible_refs(application):
    app, _commit, _release_hash, _logs = application
    repository = app.repositories[("alice", "project")]
    tip = repository.resolve_commit("main")
    subprocess.run(
        ["git", "--git-dir", str(repository.git_dir), "update-ref",
         "refs/forkmesh/trash/deleted/secret", tip],
        check=True,
    )
    for hidden in ("refs/forkmesh/trash/deleted/secret",
                   "forkmesh/trash/deleted/secret"):
        with pytest.raises(gateway.GatewayError):
            repository.resolve_commit(hidden)
    assert repository.resolve_commit("refs/heads/main") == tip
    assert repository.resolve_commit(tip[:12]) == tip


def _hidden_commit(git_dir, parent, ref, content):
    blob = subprocess.run(
        ["git", "--git-dir", git_dir, "hash-object", "-w", "--stdin"],
        input=content, check=True, capture_output=True,
    ).stdout.decode().strip()
    tree = subprocess.run(
        ["git", "--git-dir", git_dir, "mktree"],
        input=f"100644 blob {blob}\tsecret.txt\n".encode(),
        check=True, capture_output=True,
    ).stdout.decode().strip()
    commit = subprocess.run(
        ["git", "--git-dir", git_dir, "-c", "user.name=t",
         "-c", "user.email=t@example.invalid", "commit-tree", tree,
         "-p", parent, "-m", "hidden"],
        check=True, capture_output=True,
    ).stdout.decode().strip()
    subprocess.run(["git", "--git-dir", git_dir, "update-ref", ref, commit],
                   check=True)
    return commit


def test_reads_refuse_commit_reachable_only_from_hidden_refs(application):
    app, _commit, _release_hash, _logs = application
    repository = app.repositories[("alice", "project")]
    git_dir = str(repository.git_dir)
    tip = repository.resolve_commit("main")

    secret = _hidden_commit(
        git_dir, tip, "refs/forkmesh/trash/branch/secret", b"trashed secret\n")
    for requested in (secret, secret[:7], secret[:12]):
        with pytest.raises(gateway.GatewayError):
            repository.resolve_commit(requested)
        for operation in ("raw", "history", "tree"):
            query = urlencode({"ref": requested, "path": "secret.txt"})
            response = app.dispatch(
                "GET",
                f"/v1/public/repositories/alice/project/{operation}?{query}",
                {}, b"")
            assert response.status == 404, (operation, requested)
            assert response.stream is None
            assert b"trashed secret" not in response.body
    with pytest.raises(gateway.GatewayError):
        repository.resolve_commit(tip[:6])

    kept = _hidden_commit(
        git_dir, tip, "refs/forkmesh/history/main/0000000001", b"kept\n")
    repository.invalidate_memory_generation()
    assert repository.resolve_commit(kept[:12]) == kept
    response = app.dispatch(
        "GET",
        "/v1/public/repositories/alice/project/raw?"
        + urlencode({"ref": kept, "path": "secret.txt"}), {}, b"")
    assert response.status == 200
    assert subprocess.run(
        response.stream.command, check=True, capture_output=True,
        env=gateway._git_environment()).stdout == b"kept\n"


def test_commit_id_visibility_is_cached_per_refs_state_and_bounded(
    application, monkeypatch
):
    app, _commit, _release_hash, _logs = application
    repository = app.repositories[("alice", "project")]
    git_dir = str(repository.git_dir)
    tip = repository.resolve_commit("main")
    parent = subprocess.run(
        ["git", "--git-dir", git_dir, "rev-parse", tip + "^"],
        capture_output=True).stdout.decode().strip()
    secret = _hidden_commit(
        git_dir, tip, "refs/forkmesh/trash/branch/secret", b"secret\n")
    repository.invalidate_memory_generation()
    assert repository.resolve_commit(tip) == tip
    spawns = []
    real_run_git, real_run_git_status = gateway._run_git, gateway._run_git_status

    def counted(real):
        def run(*args, **kwargs):
            spawns.append(args[1])
            return real(*args, **kwargs)
        return run

    monkeypatch.setattr(gateway, "_run_git", counted(real_run_git))
    monkeypatch.setattr(gateway, "_run_git_status", counted(real_run_git_status))
    for _ in range(3):
        assert repository.resolve_commit(tip) == tip
        if parent:
            assert repository.resolve_commit(parent) == parent
        with pytest.raises(gateway.GatewayError):
            repository.resolve_commit(secret)
    assert spawns == []

    monkeypatch.setattr(gateway, "VISIBLE_COMMIT_SET_LIMIT", 0)
    repository.invalidate_memory_generation()
    assert repository.resolve_commit(tip) == tip
    with pytest.raises(gateway.GatewayError):
        repository.resolve_commit(secret)
    walks = [args for args in spawns if "--contains" in list(args)]
    assert walks and all("core.commitGraph=true" in list(args) for args in walks)
    spawns.clear()
    with pytest.raises(gateway.GatewayError):
        repository.resolve_commit(secret)
    assert not [args for args in spawns if "--contains" in list(args)]

    busy = gateway.threading.BoundedSemaphore(1)
    busy.acquire()
    monkeypatch.setattr(gateway, "_visible_commit_walks", busy)
    monkeypatch.setattr(gateway, "VISIBLE_COMMIT_WALK_WAIT_SECONDS", 0.01)
    repository.invalidate_memory_generation()
    other = _hidden_commit(
        git_dir, tip, "refs/forkmesh/trash/branch/other", b"other\n")
    with pytest.raises(gateway.GitError):
        repository.resolve_commit(other)
    response = app.dispatch(
        "GET",
        "/v1/public/repositories/alice/project/history?"
        + urlencode({"ref": other}), {}, b"")
    assert response.status == 503


def test_retired_runtime_roots_are_reaped_after_grace(monkeypatch):
    class Root:
        def __init__(self):
            self.cleaned = False

        def cleanup(self):
            self.cleaned = True

    now = [10_000.0]
    monkeypatch.setattr(gateway.time, "monotonic", lambda: now[0])
    application = object.__new__(gateway.GatewayApplication)
    roots = [Root() for _ in range(6)]
    ages = [5000, 4000, 3000, 100, 50, 10]
    application._retired_roots = [(now[0] - age, root) for age, root in zip(ages, roots)]
    application._reap_retired_roots()
    assert [root.cleaned for root in roots] == [True, True, True, False, False, False]
    assert [root for _, root in application._retired_roots] == roots[3:]
    now[0] += 10_000
    application._reap_retired_roots()
    assert [root.cleaned for root in roots] == [True, True, True, True, False, False]
    assert len(application._retired_roots) == gateway.RETIRED_ROOTS_KEPT


def test_portable_session_handoff_refuses_redirects_browsers_rewrite():
    from types import SimpleNamespace
    from urllib.parse import urlencode

    origin = "https://forkmesh.com"
    service = SimpleNamespace(
        config=SimpleNamespace(dashboard_canonical_origin=origin,
                               router_public_key="", public_origin=origin),
        application=SimpleNamespace(clock_ms=lambda: NOW))
    headers = {"Origin": origin,
               "Content-Type": "application/x-www-form-urlencoded"}

    def status(next_path):
        body = urlencode({"grant": "not-a-grant", "next": next_path}).encode()
        return gateway.DashboardService._accept_portable_session_handoff(
            service, headers, body).status

    assert status("/forkmesh/forkmesh") == 401
    for hostile in ("/\\evil.example", "/\\/evil.example",
                    "/ok\r\nSet-Cookie: x=1", "/ok\nLocation: //evil.example",
                    "/ok\x7f"):
        assert status(hostile) == 400, hostile


def test_release_digest_is_remembered_until_the_file_changes(tmp_path, monkeypatch):
    blob = tmp_path / "data"
    blob.write_bytes(b"release bytes")
    digest = hashlib.sha256(b"release bytes").hexdigest()
    gateway._RELEASE_DIGEST_MEMO.clear()
    hashed = []
    real = gateway._sha256_file

    def counted(path):
        hashed.append(path)
        return real(path)

    monkeypatch.setattr(gateway, "_sha256_file", counted)
    assert gateway._release_digest_matches(blob, digest)
    assert gateway._release_digest_matches(blob, digest)
    assert len(hashed) == 1
    blob.write_bytes(b"tampered bytes")
    os.utime(blob, ns=(1, 1))
    assert not gateway._release_digest_matches(blob, digest)
    assert len(hashed) == 2
    gateway._RELEASE_DIGEST_MEMO.clear()


def test_github_noreply_emails_give_contributor_avatars():
    avatar = gateway._github_noreply_avatar
    assert avatar("12345+alice@users.noreply.github.com") == \
        "https://avatars.githubusercontent.com/u/12345?s=56&v=4"
    assert avatar("49699333+dependabot[bot]@users.noreply.github.com") == \
        "https://avatars.githubusercontent.com/u/49699333?s=56&v=4"
    assert avatar("alice@users.noreply.github.com") == "https://github.com/alice.png?size=56"
    for email in ("github-actions[bot]@users.noreply.github.com", "alice@example.com",
                  "a/b@users.noreply.github.com", "", "x@users.noreply.github.com.evil.test"):
        assert avatar(email) == "", email


def test_a_cut_text_blob_stays_text_at_a_character_boundary():
    data = ("\u0627\u06cc" * 10).encode("utf-8")
    for cut in range(1, len(data)):
        prefix = gateway._utf8_prefix(data[:cut])
        assert data.startswith(prefix) and len(prefix) >= cut - 3
        prefix.decode("utf-8")
    broken = b"ok\xff\xfeok"
    assert gateway._utf8_prefix(broken) == broken


def test_sizes_lists_every_issue_folder_but_still_folds_code(application, monkeypatch):
    app, _commit, _release_hash, _logs = application
    repository = app.repositories[("alice", "project")]
    blobs = [(100 + n, f".forkmesh/issues/open/{n}/issue-{n}.json") for n in range(1, 121)]
    blobs += [(10, f"vendor/pkg{n}.js") for n in range(60)]
    monkeypatch.setattr(repository, "_all_blobs", lambda _commit: blobs)
    payload = repository.sizes({})
    forkmesh = next(item for item in payload["children"] if item["name"] == ".forkmesh")
    open_dir = forkmesh["children"][0]["children"][0]
    assert open_dir["name"] == "open"
    assert len(open_dir["children"]) == 120
    assert all(item["type"] == "directory" and item["children"][0]["type"] == "file"
               for item in open_dir["children"])
    vendor = next(item for item in payload["children"] if item["name"] == "vendor")
    assert len(vendor["children"]) == 41 and vendor["children"][-1]["type"] == "summary"


def _commit_with_file(git_dir, parent, path, content):
    env = dict(os.environ, GIT_INDEX_FILE=str(Path(git_dir) / "analysis-test-index"),
               GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@example.com",
               GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@example.com")

    def git(*args, data=None):
        return subprocess.run(["git", "--git-dir", str(git_dir), *args], env=env, input=data,
                              check=True, capture_output=True).stdout.decode().strip()

    blob = git("hash-object", "-w", "--stdin", data=content.encode())
    git("read-tree", parent)
    git("update-index", "--add", "--cacheinfo", "100644,%s,%s" % (blob, path))
    tree = git("write-tree")
    return git("commit-tree", tree, "-p", parent, "-m", "change " + path)


def test_tree_analysis_is_reused_when_only_forkmesh_metadata_moved(
    application, monkeypatch,
):
    app, commit, _release_hash, _logs = application
    repository = app.repositories[("alice", "project")]
    first = repository._analysis(commit)
    metadata = _commit_with_file(repository.git_dir, commit,
                                 ".forkmesh/pull-batch.json", '{"pulls": []}\n')
    builds = []
    original = repository._build_analysis

    def counted(target):
        builds.append(target)
        return original(target)

    monkeypatch.setattr(repository, "_build_analysis", counted)
    repository.invalidate_memory_generation()
    reused = repository._analysis(metadata)
    assert builds == []
    assert reused["commit"] == reused["summary"]["commit"] == metadata
    assert reused["adjacency"] == first["adjacency"]
    assert original(metadata) == reused
    code = _commit_with_file(repository.git_dir, metadata, "src/extra.js",
                             "export const x = 1;\n")
    assert repository._analysis(code)["commit"] == code
    assert builds == [code]


def test_tree_analysis_window_ignores_forkmesh_metadata(application, monkeypatch):
    app, commit, _release_hash, _logs = application
    repository = app.repositories[("alice", "project")]
    code_files = [path for _size, path in repository._all_blobs(commit)
                  if not path.startswith(".forkmesh/")]
    monkeypatch.setattr(gateway, "MAX_ANALYSIS_REPOSITORY_FILES", len(code_files))
    before = repository._build_analysis(commit)
    metadata = _commit_with_file(repository.git_dir, commit,
                                 ".forkmesh/aaa-pull-batch.json", '{"pulls": []}\n')
    after = repository._build_analysis(metadata)
    assert after["summary"]["dependency"] == before["summary"]["dependency"]
    assert after["adjacency"] == before["adjacency"]
