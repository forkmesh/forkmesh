import itertools
import json
import subprocess

import pytest

import test_mirror_gateway as base
from test_mirror_gateway import gateway


TARGET = "/v1/repositories/alice/project/branch-trash"
_ids = itertools.count(1)


def git(bare, *args):
    return subprocess.run(
        ["git", "--git-dir", str(bare), *args],
        check=True, capture_output=True, text=True).stdout.strip()


def trashed_repository(tmp_path):
    bare, _commit = base.make_bare_repository(tmp_path)
    main = git(bare, "rev-parse", "refs/heads/main")
    older = git(bare, "rev-parse", "refs/heads/main~1")
    for ref, oid in (
        ("refs/forkmesh/trash/branch/feature/x/0123456789abcdef", main),
        ("refs/forkmesh/trash/branch/feature/x/1111111111111111", main),
        ("refs/forkmesh/trash/branch/old/2222222222222222", older),
        ("refs/forkmesh/trash/agent-branch/agent/7-fix/fedcba9876543210", main),
        ("refs/forkmesh/trash/branch/bad/not-a-token", main),
        ("refs/forkmesh/trash/branch/0123456789abcdef", main),
        ("refs/forkmesh/trash/deleted/elsewhere", main),
        ("refs/forkmesh/trash/agent/workspace/kept", main),
    ):
        git(bare, "update-ref", ref, oid)
    return bare, main, older


def make_app(tmp_path, operations):
    bare, main, older = trashed_repository(tmp_path)
    (tmp_path / "releases").mkdir(exist_ok=True)
    config_path = base.write_config(tmp_path, bare)
    value = json.loads(config_path.read_text(encoding="utf-8"))
    value["repositories"][0]["operations"] = operations
    value["mergeExecutorCommand"] = ["/usr/bin/true"]
    config_path.write_text(json.dumps(value), encoding="utf-8")
    app = gateway.GatewayApplication(
        gateway.load_config(config_path),
        verifier=base.AcceptVerifier(),
        health_signer=base.HealthSigner(),
        materializer=base.FakeArchiveMaterializer(bare),
        clock_ms=lambda: base.NOW,
        log=[].append,
    )
    return app, main, older


@pytest.fixture
def trash_app(tmp_path):
    app, main, older = make_app(
        tmp_path,
        sorted(gateway.DEFAULT_PUBLIC_OPERATIONS) + ["delete-agent-branch"])
    yield app, main, older
    app.close()


def request(app, target=TARGET, method="GET", body=b"", headers=None):
    supplied = base.capability_headers(
        target, method, body, request_id="trash_request_%06d" % next(_ids))
    supplied.update(headers or {})
    return app.dispatch(method, target, supplied, body)


def test_branch_trash_is_never_public_and_rides_on_branch_deletes():
    operation = gateway.BRANCH_TRASH_OPERATION
    assert operation not in gateway.PUBLIC_OPERATIONS
    assert operation not in gateway.DEFAULT_PUBLIC_OPERATIONS
    assert operation not in gateway.DIRECT_PUBLIC_READ_OPERATIONS
    assert operation not in gateway.WEBSITE_COUNTER_OPERATIONS
    assert operation in gateway.LOG_OPERATIONS
    assert gateway.OPERATION_QUERY_FIELDS[operation] == frozenset()
    assert gateway.BRANCH_TRASH_CARRIER_OPERATION == "delete-agent-branch"


def test_a_router_capability_lists_every_deliberately_deleted_branch(
        trash_app):
    app, main, older = trash_app
    response = request(app)
    assert response.status == 200
    assert response.headers.get("Cache-Control") == "no-store"
    assert json.loads(response.body) == {
        "ok": True,
        "node": "mirror-a",
        "truncated": False,
        "entries": [
            {"branch": "agent/7-fix", "oid": main},
            {"branch": "feature/x", "oid": main},
            {"branch": "old", "oid": older},
        ],
    }


def test_branch_trash_refuses_every_other_spelling(trash_app):
    app, _main, _older = trash_app
    unsigned = request(app, headers={"X-ForkMesh-Signature": "A" * 86})
    assert unsigned.status == 404
    public = request(
        app, target="/v1/public/repositories/alice/project/branch-trash")
    assert public.status == 404
    alias = app.dispatch("GET", "/api/repo/alice/project/branch-trash", {}, b"")
    assert alias.status == 404
    assert request(app, target=TARGET + "?ref=main").status == 404
    assert request(app, method="POST", body=b"{}").status == 404
    assert request(
        app, target="/v1/repositories/alice/missing/branch-trash").status == 404


def test_a_repository_without_branch_deletes_has_no_trash_route(tmp_path):
    app, _main, _older = make_app(
        tmp_path, sorted(gateway.DEFAULT_PUBLIC_OPERATIONS))
    try:
        assert request(app).status == 404
    finally:
        app.close()


def test_the_listing_is_bounded(tmp_path, monkeypatch):
    monkeypatch.setattr(gateway, "MAX_BRANCH_TRASH_ENTRIES", 2)
    app, _main, _older = make_app(
        tmp_path,
        sorted(gateway.DEFAULT_PUBLIC_OPERATIONS) + ["delete-agent-branch"])
    try:
        listing = json.loads(request(app).body)
        assert listing["truncated"] is True
        assert len(listing["entries"]) <= 2
    finally:
        app.close()
