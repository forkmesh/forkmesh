"""Restricted repositories: ForkMesh-hosted private repos on a core gateway.

A ``visibility: "restricted"`` entry is configured like a public repository
(plaintext ``gitDir``, integrity pin, operations) but the gateway serves it
only to a verified router capability (``/v1/repositories/...``) or a runner
grant (``/v1/runner/...``, which core replicas use to fetch each other). The
unauthenticated spellings (``/v1/public/...``, ``/<owner>/<repo>.git``, the
legacy branches read) answer exactly as for an unknown repository.
docs/HOSTED_REPOS.md.
"""

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from test_mirror_gateway import (  # noqa: E402
    NOW,
    AcceptVerifier,
    FakeArchiveMaterializer,
    HealthSigner,
    capability_headers,
    gateway,
    make_bare_repository,
    write_config,
)
from test_mirror_gateway_runner_grants import grant  # noqa: E402


def restricted_entry(bare, owner="alice", name="secret"):
    return {
        "owner": owner,
        "name": name,
        "visibility": "restricted",
        "enabled": True,
        "gitDir": str(bare),
        "operations": sorted(
            gateway.DEFAULT_PUBLIC_OPERATIONS | {"git-receive-pack"}),
        "integrity": {"expectedRefsSha256": gateway.refs_sha256(bare)},
    }


@pytest.fixture
def served(tmp_path):
    bare, commit = make_bare_repository(tmp_path)
    repositories = [
        {
            "owner": "alice",
            "name": "project",
            "visibility": "public",
            "enabled": True,
            "gitDir": str(bare),
            "integrity": {"expectedRefsSha256": gateway.refs_sha256(bare)},
        },
        restricted_entry(bare),
    ]
    config = gateway.load_config(
        write_config(tmp_path, bare, repositories=repositories))
    app = gateway.GatewayApplication(
        config,
        verifier=AcceptVerifier(),
        health_signer=HealthSigner(),
        materializer=FakeArchiveMaterializer(bare),
        clock_ms=lambda: NOW,
        log=lambda _entry: None,
    )
    yield app, commit
    app.close()


def test_restricted_entry_parses_like_a_public_one(served):
    app, _commit = served
    assert ("alice", "secret") in app.restricted_repositories
    assert ("alice", "secret") not in app.repositories
    assert app.quarantined_count == 0


def test_unknown_visibility_is_refused(tmp_path):
    bare, _commit = make_bare_repository(tmp_path)
    entry = restricted_entry(bare)
    entry["visibility"] = "internal"
    with pytest.raises(gateway.GatewayError):
        gateway.load_config(write_config(tmp_path, bare, repositories=[entry]))


@pytest.mark.parametrize("path", [
    "/v1/public/repositories/alice/secret/tree",
    "/alice/secret.git/info/refs?service=git-upload-pack",
    "/v1/repositories/alice/secret/branches",
])
def test_public_spellings_never_reach_a_restricted_repository(served, path):
    app, _commit = served
    response = app.dispatch("GET", path, {}, b"")
    assert response.status == 404
    # The public twin of the same bytes still answers on the same route.
    public = app.dispatch("GET", path.replace("secret", "project"), {}, b"")
    assert public.status == 200


def test_capability_reads_a_restricted_repository(served):
    app, commit = served
    target = "/v1/repositories/alice/secret/tree"
    response = app.dispatch("GET", target, capability_headers(target), b"")
    assert response.status == 200
    assert json.loads(response.body)["commit"] == commit
    target = "/v1/repositories/alice/secret/git-info-refs?service=git-upload-pack"
    response = app.dispatch(
        "GET", target, capability_headers(target, request_id="request_654321"),
        b"")
    assert response.status == 200
    assert commit.encode() in response.body


def test_runner_grant_reads_a_restricted_repository(served):
    app, commit = served
    headers = {"X-ForkMesh-Runner-Grant": grant(
        "read", repository="secret", runner="fmnext-n2")}
    response = app.dispatch(
        "GET", "/v1/runner/alice/secret.git/info/refs?service=git-upload-pack",
        headers, b"")
    assert response.status == 200
    assert commit.encode() in response.body
    # Without a grant the route is closed.
    response = app.dispatch(
        "GET", "/v1/runner/alice/secret.git/info/refs?service=git-upload-pack",
        {}, b"")
    assert response.status == 404


def test_health_proof_covers_a_restricted_repository(served):
    app, _commit = served
    response = app.dispatch(
        "GET",
        "/health?nonce=nonce_0123456789abcdef&issuedAt=%d&owner=alice"
        "&repo=secret" % NOW,
        {}, b"")
    assert response.status == 200
    proof = json.loads(response.body)["repositoryProof"]
    assert proof["available"] is True
    assert "git-receive-pack" in proof["operations"]
    # The node-wide public count is unchanged by the restricted entry.
    assert json.loads(response.body)["publicRepositoryCount"] == 1
