import json
from pathlib import Path
import sys

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import test_mirror_gateway as fixtures  # noqa: E402
from test_mirror_gateway import gateway, run  # noqa: E402


def _commit_file(work, name, text, message):
    (work / name).write_text(text, encoding="utf-8")
    run(["git", "add", name], work)
    run(["git", "commit", "-m", message], work)
    return run(["git", "rev-parse", "HEAD"], work)


@pytest.fixture
def repositories(tmp_path):
    upstream = tmp_path / "upstream.git"
    instance = tmp_path / "instance.git"
    work = tmp_path / "work"
    run(["git", "init", "--bare", str(upstream)], tmp_path)
    run(["git", "init", "--bare", str(instance)], tmp_path)
    run(["git", "init", "-b", "main", str(work)], tmp_path)
    run(["git", "config", "user.email", "dev@example.test"], work)
    run(["git", "config", "user.name", "Dev"], work)
    root = _commit_file(work, "README.md", "root\n", "Root")
    run(["git", "switch", "-c", "fm/merged"], work)
    merged = _commit_file(work, "a.txt", "a\n", "Add a")
    run(["git", "switch", "-c", "fm/pending", root], work)
    pending = _commit_file(work, "b.txt", "b\n", "Add b")
    run(["git", "switch", "-c", "fm/stacked"], work)
    stacked = _commit_file(work, "c.txt", "c\n", "Add c")
    run(["git", "switch", "main"], work)
    run(["git", "push", str(instance), "main", "fm/merged", "fm/pending",
         "fm/stacked"], work)
    _commit_file(work, "upstream.txt", "elsewhere\n", "Upstream work")
    run(["git", "cherry-pick", merged], work)
    upstream_main = run(["git", "rev-parse", "HEAD"], work)
    run(["git", "push", str(upstream), "main"], work)
    for bare in (upstream, instance):
        run(["git", "symbolic-ref", "HEAD", "refs/heads/main"], bare)
    config_path = fixtures.write_config(tmp_path, instance, repositories=[
        {
            "owner": "alice", "name": "project", "visibility": "public",
            "enabled": True, "gitDir": str(instance),
            "integrity": {"expectedRefsSha256": gateway.refs_sha256(instance)},
        },
        {
            "owner": "up", "name": "stream", "visibility": "public",
            "enabled": True, "gitDir": str(upstream),
            "integrity": {"expectedRefsSha256": gateway.refs_sha256(upstream)},
        },
    ])
    app = gateway.GatewayApplication(
        gateway.load_config(config_path),
        verifier=fixtures.AcceptVerifier(),
        health_signer=fixtures.HealthSigner(),
        materializer=fixtures.FakeArchiveMaterializer(instance),
        clock_ms=lambda: fixtures.NOW,
        log=lambda _entry: None,
    )
    yield app, {"root": root, "merged": merged, "pending": pending,
                "stacked": stacked, "upstream": upstream_main}
    app.close()


def _cherry(app, query, request_id):
    response = fixtures.dispatch(app, "compare", query, request_id=request_id)
    return response.status, (json.loads(response.body) if response.body else {})


def test_a_branch_whose_patch_landed_upstream_reads_as_merged(repositories):
    app, oids = repositories
    status, merged = _cherry(app, {
        "base": oids["upstream"], "head": oids["merged"], "cherry": "up/stream",
        "limit": oids["root"]}, "cherry_merged_0001")
    assert status == 200
    assert merged["pending"] == 0 and merged["merged"] == 1
    assert merged["commits"] == [{"commit": oids["merged"], "upstream": True}]
    status, pending = _cherry(app, {
        "base": oids["upstream"], "head": oids["pending"], "cherry": "up/stream"},
        "cherry_pending_01")
    assert status == 200
    assert pending["pending"] == 1 and pending["merged"] == 0


def test_a_limit_keeps_only_the_stacked_rows_own_commits(repositories):
    app, oids = repositories
    status, stacked = _cherry(app, {
        "base": oids["upstream"], "head": oids["stacked"], "cherry": "up/stream",
        "limit": oids["pending"]}, "cherry_stacked_01")
    assert status == 200
    assert [item["commit"] for item in stacked["commits"]] == [oids["stacked"]]


def test_the_cherry_read_needs_commit_ids_and_a_served_upstream(repositories):
    app, oids = repositories
    for index, query in enumerate((
        {"base": "main", "head": oids["merged"], "cherry": "up/stream"},
        {"base": oids["upstream"], "head": oids["merged"], "cherry": "nobody/here"},
        {"base": oids["upstream"], "head": oids["merged"], "cherry": "../x"},
        {"base": "f" * 40, "head": oids["merged"], "cherry": "up/stream"},
    )):
        status, _ = _cherry(app, query, "cherry_refused_%02d" % index)
        assert status == 404
    status, plain = _cherry(app, {"base": oids["root"], "head": oids["pending"]},
                            "plain_compare_001")
    assert status == 200 and "patch" in plain


def test_an_upstream_too_far_ahead_is_refused_before_git_cherry(
        repositories, monkeypatch):
    app, oids = repositories
    monkeypatch.setattr(gateway, "MAX_CHERRY_UPSTREAM_COMMITS", 1)
    status, _ = _cherry(app, {
        "base": oids["upstream"], "head": oids["pending"], "cherry": "up/stream"},
        "cherry_toolarge_01")
    assert status != 200
    monkeypatch.setattr(gateway, "MAX_CHERRY_UPSTREAM_COMMITS", 2)
    status, _ = _cherry(app, {
        "base": oids["upstream"], "head": oids["pending"], "cherry": "up/stream"},
        "cherry_fits_00001")
    assert status == 200
