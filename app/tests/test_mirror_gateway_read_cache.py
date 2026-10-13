"""Gateway read performance contracts (docs/PERF_REPO_PAGE.md).

* the cache key names the commit a ref resolves to, so the warmup's ref-less
  root tree is the one the web UI's ``tree?path=&ref=main`` reads;
* concurrent misses of one key compute it once;
* large directories take their last-commit column from one history walk;
* the warmup keeps a changed-path commit-graph next to the repository.
"""

import json
import subprocess
import threading
import time

import pytest

from test_mirror_gateway import (  # noqa: F401  (fixtures and helpers)
    AcceptVerifier,
    FakeArchiveMaterializer,
    HealthSigner,
    NOW,
    gateway,
    make_bare_repository,
    run,
    write_config,
)


def make_app(tmp_path, bare):
    config = gateway.load_config(write_config(tmp_path, bare))
    return gateway.GatewayApplication(
        config,
        verifier=AcceptVerifier(),
        health_signer=HealthSigner(),
        materializer=FakeArchiveMaterializer(bare),
        clock_ms=lambda: NOW,
        log=lambda _event: None,
    )


def read(app, operation, query=""):
    target = "/v1/public/repositories/alice/project/" + operation
    if query:
        target += "?" + query
    return app.dispatch("GET", target, {}, b"")


@pytest.fixture
def bare_app(tmp_path):
    bare, commit = make_bare_repository(tmp_path)
    app = make_app(tmp_path, bare)
    yield app, bare, commit
    app.close()


def test_warm_root_tree_serves_the_web_ui_spelling(bare_app):
    app, _bare, commit = bare_app
    app.prewarm_memory_caches()
    for query in ("path=&ref=main", "ref=main", "path=", "",
                  "path=&ref=" + commit):
        response = read(app, "tree", query)
        assert response.status == 200, query
        assert response.headers["X-ForkMesh-Memory-Cache"] == "hit", query
    for operation in ("stats", "sizes", "history"):
        response = read(app, operation, "ref=main")
        assert response.headers["X-ForkMesh-Memory-Cache"] == "hit", operation


def test_other_refs_and_paths_keep_their_own_answers(bare_app):
    app, _bare, commit = bare_app
    main = json.loads(read(app, "tree", "path=&ref=main").body)
    older = json.loads(read(app, "tree", "path=&ref=release-preview").body)
    assert main["commit"] == commit
    assert older["commit"] != commit
    src = json.loads(read(app, "tree", "path=src&ref=main").body)
    assert {e["name"] for e in src["entries"]} >= {"main.py", "util.js"}
    assert read(app, "tree", "path=&ref=no-such-branch").status == 404


def test_concurrent_misses_compute_once(bare_app, monkeypatch):
    app, _bare, _commit = bare_app
    repository = app.repositories[("alice", "project")]
    calls = []
    original = gateway.GitRepository.tree

    def slow_tree(self, query):
        calls.append(query)
        time.sleep(0.3)
        return original(self, query)

    monkeypatch.setattr(gateway.GitRepository, "tree", slow_tree)
    repository.cache_ref("main")  # resolve outside the timed section
    results = []

    def worker(query):
        results.append(read(app, "tree", query))

    threads = [threading.Thread(target=worker, args=(q,))
               for q in ("path=&ref=main", "ref=main", "path=&ref=main")]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    assert len(calls) == 1
    assert {r.status for r in results} == {200}
    assert len({r.body for r in results}) == 1


def test_large_directory_uses_one_history_walk(tmp_path, monkeypatch):
    bare, _commit = make_bare_repository(tmp_path)
    work = tmp_path / "work"
    run(["git", "clone", "-q", str(bare), str(work)], tmp_path)
    run(["git", "config", "user.email", "bob@example.test"], work)
    run(["git", "config", "user.name", "Bob"], work)
    many = work / "many"
    many.mkdir()
    count = gateway.TREE_BATCH_SUMMARY_MIN_ENTRIES + 5
    for index in range(count):
        (many / ("f%03d.txt" % index)).write_text("a\n", encoding="utf-8")
    (many / "sub").mkdir()
    (many / "sub" / "deep.txt").write_text("a\n", encoding="utf-8")
    run(["git", "add", "."], work)
    run(["git", "commit", "-q", "-m", "Add many files"], work)
    (many / "f003.txt").write_text("b\n", encoding="utf-8")
    (many / "sub" / "deep.txt").write_text("b\n", encoding="utf-8")
    run(["git", "commit", "-q", "-am", "Touch two"], work)
    run(["git", "push", "-q", "origin", "HEAD:main"], work)
    app = make_app(tmp_path, bare)
    try:
        spawned = []
        original = gateway.GitRepository._commit_summary

        def per_entry(self, commit, path=""):
            spawned.append(path)
            return original(self, commit, path)

        monkeypatch.setattr(gateway.GitRepository, "_commit_summary",
                            per_entry)
        listing = json.loads(read(app, "tree", "path=many&ref=main").body)
        entries = {e["name"]: e for e in listing["entries"]}
        assert len(entries) == count + 1
        # Only the directory's own latestCommit summary spawns a git log.
        assert spawned == [""]
        assert entries["f003.txt"]["subject"] == "Touch two"
        assert entries["f003.txt"]["changeCount"] == 2
        assert entries["sub"]["subject"] == "Touch two"
        assert entries["f000.txt"]["subject"] == "Add many files"
        assert entries["f000.txt"]["changeCount"] == 1
        repository = app.repositories[("alice", "project")]
        for name in ("f003.txt", "sub", "f000.txt"):
            expected = original(repository, listing["commit"],
                                "many/" + name)
            for field in ("commit", "author", "date", "subject",
                          "changeCount", "changeCountCapped"):
                assert entries[name][field] == expected[field], (name, field)
    finally:
        app.close()


def test_warmup_writes_a_changed_path_commit_graph(bare_app, monkeypatch):
    app, bare, _commit = bare_app
    monkeypatch.delenv("FORKMESH_GATEWAY_COMMIT_GRAPH", raising=False)
    app.prewarm_memory_caches()
    chain = bare / "objects" / "info" / "commit-graphs" / "commit-graph-chain"
    assert chain.is_file()
    verify = subprocess.run(
        ["git", "--git-dir", str(bare), "commit-graph", "verify"],
        capture_output=True, text=True, check=False)
    assert verify.returncode == 0, verify.stderr


def test_commit_graph_can_be_turned_off(bare_app, monkeypatch):
    app, bare, _commit = bare_app
    monkeypatch.setenv("FORKMESH_GATEWAY_COMMIT_GRAPH", "0")
    app.prewarm_memory_caches()
    assert not (bare / "objects" / "info" / "commit-graphs").exists()
