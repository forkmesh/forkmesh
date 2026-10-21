import json
import os
import subprocess
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
    run,
    write_config,
)

NOW_S = NOW // 1000
DAY = 86400
MS = 1000


def _commit(source, when, name, message, files=None, email=None):
    for relative, text in (files or {}).items():
        path = source / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        run(["git", "add", relative], source)
    env = dict(
        os.environ,
        GIT_AUTHOR_NAME=name,
        GIT_AUTHOR_EMAIL=email or "%s@example.test" % name.split("[")[0],
        GIT_COMMITTER_NAME=name,
        GIT_COMMITTER_EMAIL=email or "%s@example.test" % name.split("[")[0],
        GIT_AUTHOR_DATE="@%d +0000" % when,
        GIT_COMMITTER_DATE="@%d +0000" % when,
    )
    subprocess.run(
        ["git", "commit", "--allow-empty", "-q", "-m", message],
        cwd=source, check=True, capture_output=True, env=env)


def _append(source, name, when, author, email=None):
    path = source / ("%s.txt" % name)
    previous = path.read_text(encoding="utf-8") if path.exists() else ""
    _commit(source, when, author, "work by " + author,
            {"%s.txt" % name: previous + "line %d\n" % when}, email=email)


def _pull(number, author, status, opened, extra=""):
    return (
        "---\nschema: forkmesh-pull-v1\nnumber: %d\ntitle: Pull %d\n"
        "status: %s\nts: %d\nauthorName: %s\n%s---\n\nBody.\n"
        % (number, number, status, opened, author, extra))


def _event(kind, author, at, body, extra=""):
    return "---\ntype: %s\nauthorName: %s\nts: %d\n%s---\n\n%s\n" % (
        kind, author, at, extra, body)


def _issue(number, author, created, status, events, labels=()):
    return json.dumps({
        "schema": "forkmesh-issue-v1",
        "number": number,
        "title": "Issue %d" % number,
        "authorName": author,
        "createdAt": created,
        "status": status,
        "labels": list(labels),
        "events": events,
    })


def make_pulse_repository(tmp_path):
    bare = tmp_path / "pulse.git"
    source = tmp_path / "pulse-source"
    run(["git", "init", "--bare", "-q", str(bare)], tmp_path)
    run(["git", "init", "-q", "-b", "main", str(source)], tmp_path)
    _commit(source, NOW_S - 40 * DAY, "alice", "Initial files", {
        "alice.txt": "start\n",
        ".forkmesh/issues/closed/7/issue-7.json": _issue(
            7, "dave", (NOW_S - 5 * DAY) * MS, "closed", [
                {"type": "status", "status": "closed", "authorName": "carol",
                 "ts": (NOW_S - DAY) * MS},
                {"type": "comment", "authorName": "alice", "body": "/assign",
                 "ts": (NOW_S - 2 * DAY) * MS},
                {"type": "comment", "authorName": "alice",
                 "body": "Looks good", "ts": (NOW_S - 2 * DAY) * MS},
            ], labels=("bug",)),
        ".forkmesh/issues/open/8/issue-8.json": _issue(
            8, "frank", (NOW_S - 2 * DAY) * MS, "open", [
                {"type": "comment", "authorName": "bob", "body": "Reply",
                 "ts": (NOW_S - 2 * DAY + index) * MS}
                for index in range(4)
            ]),
        ".forkmesh/issues/9/issue-9.json": _issue(
            9, "grace", (NOW_S - 100 * DAY) * MS, "open", []),
    })
    _append(source, "carol", NOW_S - 10 * DAY, "carol")
    _append(source, "bob", NOW_S - 3 * DAY, "bob")
    for minute in range(7):
        _append(source, "alice", NOW_S - 2 * DAY + minute * 60, "alice")
    _append(source, "alice", NOW_S - DAY, "alice")
    _append(source, "deps", NOW_S - DAY + 60, "dependabot[bot]",
            email="dependabot[bot]@users.noreply.github.com")
    _append(source, "agent", NOW_S - DAY + 70, "Claude Opus 5.5",
            email="noreply@anthropic.com")
    _append(source, "agent", NOW_S - DAY + 80, "Release Helper",
            email="llama@cloudflare.com")
    _append(source, "node", NOW_S - DAY + 90, "mirror7",
            email="mirror7@users.noreply.example.test")
    run(["git", "switch", "-q", "-c", "topic"], source)
    _append(source, "carol", NOW_S - 2 * DAY + 3600, "carol")
    run(["git", "switch", "-q", "main"], source)
    env = dict(os.environ, GIT_AUTHOR_NAME="alice",
               GIT_AUTHOR_EMAIL="alice@example.test",
               GIT_COMMITTER_NAME="alice",
               GIT_COMMITTER_EMAIL="alice@example.test",
               GIT_AUTHOR_DATE="@%d +0000" % (NOW_S - DAY + 120),
               GIT_COMMITTER_DATE="@%d +0000" % (NOW_S - DAY + 120))
    subprocess.run(["git", "merge", "-q", "--no-ff", "topic", "-m",
                    "Merge topic"], cwd=source, check=True,
                   capture_output=True, env=env)
    _append(source, "alice", NOW_S - 3 * 3600, "alice")
    run(["git", "switch", "-q", "-c", "feature/x"], source)
    _append(source, "erin", NOW_S - DAY, "erin")
    run(["git", "switch", "-q", "--orphan", "forkmesh/pulls"], source)
    run(["git", "rm", "-rq", "--cached", "--ignore-unmatch", "."], source)
    for leftover in ("alice.txt", "bob.txt", "carol.txt", "deps.txt",
                     "erin.txt"):
        (source / leftover).unlink(missing_ok=True)
    pulls = ".forkmesh/pulls/"
    _commit(source, NOW_S - 3600, "ledger", "pull ledger", {
        pulls + "42/pull.md": _pull(
            42, "alice", "merged", (NOW_S - 4 * DAY) * MS,
            "mergedAt: %d\n" % ((NOW_S - DAY) * MS)),
        pulls + "42/0002-review.md": _event(
            "review", "bob", (NOW_S - 2 * DAY) * MS, "Ship it",
            "state: approved\n"),
        pulls + "42/0003-review.md": _event(
            "review", "alice", (NOW_S - 2 * DAY) * MS, "Self review",
            "state: approved\n"),
        pulls + "42/0004-comment.md": _event(
            "comment", "carol", (NOW_S - 2 * DAY) * MS, "Nice"),
        pulls + "42/0005-comment.md": _event(
            "comment", "carol", (NOW_S - 2 * DAY) * MS, "/label bug"),
        pulls + "42/changes.patch": "diff --git a/x b/x\n",
        pulls + "43/pull.md": _pull(43, "erin", "open", (NOW_S - DAY) * MS),
        pulls + "43/0002-review.md": _event(
            "review", "alice", (NOW_S - 12 * 3600) * MS, "Needs work",
            "state: changes_requested\n"),
        pulls + "44/pull.md": _pull(44, "mallory", "deleted",
                                    (NOW_S - DAY) * MS),
        pulls + "45/pull.md": _pull(45, "bob", "closed",
                                    (NOW_S - 50 * DAY) * MS),
        pulls + "45/0002-status.md": _event(
            "status", "bob", (NOW_S - 45 * DAY) * MS, "Closing",
            "state: closed\n"),
    })
    run(["git", "switch", "-q", "--orphan", "forkmesh/pull/46"], source)
    run(["git", "rm", "-rq", "--cached", "--ignore-unmatch", "."], source)
    _commit(source, NOW_S - 3600, "ledger", "pull #46", {
        pulls + "46/pull.md": _pull(46, "frank", "open",
                                    (NOW_S - 6 * DAY) * MS),
    })
    run(["git", "switch", "-q", "main"], source)
    run(["git", "remote", "add", "origin", str(bare)], source)
    run(["git", "push", "-q", "origin", "main", "feature/x", "topic",
         "forkmesh/pulls", "forkmesh/pull/46"], source)
    run(["git", "symbolic-ref", "HEAD", "refs/heads/main"], bare)
    return bare


def _application(tmp_path, bare, clock, repositories=None):
    config = gateway.load_config(
        write_config(tmp_path, bare, repositories=repositories))
    return gateway.GatewayApplication(
        config,
        verifier=AcceptVerifier(),
        health_signer=HealthSigner(),
        materializer=FakeArchiveMaterializer(bare),
        clock_ms=lambda: clock[0],
        log=lambda _record: None,
    )


@pytest.fixture
def pulse_app(tmp_path):
    bare = make_pulse_repository(tmp_path)
    clock = [NOW]
    app = _application(tmp_path, bare, clock)
    yield app, clock, bare
    app.close()


def read(app, query="", repository="project"):
    target = "/v1/public/repositories/alice/%s/pulse" % repository
    if query:
        target += "?" + query
    return app.dispatch("GET", target, {}, b"")


def payload(app, query=""):
    response = read(app, query)
    assert response.status == 200, response.body
    return json.loads(response.body)


def test_period_bounds_follow_the_cache_bucket(pulse_app):
    app, _clock, _bare = pulse_app
    for period, seconds, bucket in (
        ("24h", DAY, 300), ("3d", 3 * DAY, 900), ("1w", 7 * DAY, 1800),
        ("1m", 30 * DAY, 3600),
    ):
        value = payload(app, "period=" + period)
        end = -(-NOW_S // bucket) * bucket
        assert value["period"] == period
        assert value["since"] == gateway._pulse_iso(end - seconds)
        assert value["until"] == gateway._pulse_iso(NOW_S)
        assert value["refreshSeconds"] == bucket
    default = payload(app)
    assert default["period"] == "1w"
    every = payload(app, "period=all")
    assert every["since"] is None
    assert "pulls" not in every and "summary" not in every
    for query in ("period=2w", "period=1W", "period=24h&ref=main",
                  "period=1w&period=1m", "person=" + "x" * 101,
                  "person=%0Aalice"):
        assert read(app, query).status == 404, query


def test_overview_lists_and_commit_summary(pulse_app):
    app, _clock, _bare = pulse_app
    value = payload(app, "period=1w")
    assert value["branch"] == "main"
    pulls, issues = value["pulls"], value["issues"]
    assert [item["number"] for item in pulls["merged"]["items"]] == [42]
    assert pulls["merged"]["items"][0]["at"] == (NOW_S - DAY) * MS
    assert [item["number"] for item in pulls["opened"]["items"]] == [
        43, 42, 46]
    assert pulls["opened"]["count"] == 3 and pulls["opened"]["authors"] == 3
    assert [item["number"] for item in issues["closed"]["items"]] == [7]
    assert [item["number"] for item in issues["opened"]["items"]] == [8, 7]
    assert issues["opened"]["items"][0] == {
        "number": 8, "title": "Issue 8", "author": "frank",
        "status": "open", "at": (NOW_S - 2 * DAY) * MS}
    summary = value["summary"]
    assert summary["main"] == 11
    assert summary["all"] == 12
    assert summary["authors"] == 4
    assert summary["additions"] == 11 and summary["deletions"] == 0
    assert summary["filesChanged"] == 3
    assert summary["partial"] is False and summary["truncated"] is False
    assert [(row["name"], row["commits"]) for row in value["committers"]] == [
        ("alice", 9), ("bob", 1), ("carol", 1)]
    series = value["series"]
    assert series["stepSeconds"] == DAY
    assert len(series["buckets"]) == 7
    assert sum(bucket["commits"] for bucket in series["buckets"]) == 11
    assert sum(bucket["additions"] for bucket in series["buckets"]) == 11


def test_leaderboard_scores_breakdown_and_rank_movement(pulse_app):
    app, _clock, _bare = pulse_app
    value = payload(app, "period=1w")
    weights = value["scoring"]["weights"]
    assert weights == {"merged": 10, "commits": 1, "issuesClosed": 3,
                       "reviews": 4, "comments": 1}
    rows = {row["name"]: row for row in value["leaderboard"]}
    assert set(rows) == {"alice", "bob", "carol"}
    assert rows["alice"]["breakdown"] == {
        "merged": 1, "commits": 7, "issuesClosed": 0, "reviews": 1,
        "comments": 1}
    assert rows["alice"]["score"] == 22
    assert rows["bob"]["breakdown"] == {
        "merged": 0, "commits": 1, "issuesClosed": 0, "reviews": 1,
        "comments": 3}
    assert rows["bob"]["score"] == 8
    assert rows["carol"]["breakdown"]["issuesClosed"] == 1
    assert rows["carol"]["score"] == 5
    assert [row["rank"] for row in value["leaderboard"]] == [1, 2, 3]
    assert rows["alice"]["previousRank"] is None
    assert rows["carol"]["previousRank"] == 1
    assert rows["alice"]["streak"] == 3
    assert rows["alice"]["achievements"] == {
        "mergedPulls": 1, "reviews": 1, "bugsClosed": 0, "longestStreak": 3}
    assert rows["carol"]["achievements"]["bugsClosed"] == 1
    assert value["leaderboardTotal"] == 3
    every = payload(app, "period=all")
    totals = {row["name"]: (row["rank"], row["score"], row["previousRank"])
              for row in every["leaderboard"]}
    assert totals == {"alice": (1, 23, 1), "bob": (2, 8, None),
                      "carol": (3, 6, 1)}


def test_newcomers_and_person_progress(pulse_app):
    app, _clock, _bare = pulse_app
    value = payload(app, "period=1w")
    assert [(row["name"], row["kind"], row["number"])
            for row in value["newcomers"]] == [
        ("erin", "pull", 43), ("dave", "issue", 7), ("frank", "pull", 46)]
    assert value["newcomerCount"] == 3
    mine = payload(app, "period=1w&person=Alice")
    assert "leaderboard" not in mine and "pulls" not in mine
    person = mine["person"]
    assert person["found"] is True and person["rank"] == 1
    assert person["score"] == 22
    assert person["totals"]["commits"] == 10
    assert person["totals"]["mergedPulls"] == 1
    assert person["totals"]["longestStreak"] == 3
    assert person["totals"]["currentStreak"] == 3
    assert person["before"]["mergedPulls"] == 0
    assert person["before"]["commits"] == 1
    stranger = payload(app, "period=1w&person=nobody")["person"]
    assert stranger["found"] is False and stranger["rank"] is None
    assert stranger["totals"]["commits"] == 0


def test_answers_are_cached_per_window_and_partial_ones_are_not(
        pulse_app, monkeypatch):
    app, clock, _bare = pulse_app
    first = read(app, "period=24h")
    assert first.headers["X-ForkMesh-Memory-Cache"] == "miss"
    again = read(app, "period=24h")
    assert again.headers["X-ForkMesh-Memory-Cache"] == "hit"
    assert again.body == first.body
    clock[0] = NOW + 301 * 1000
    later = read(app, "period=24h")
    assert later.headers["X-ForkMesh-Memory-Cache"] == "miss"
    repository = app.repositories[("alice", "project")]
    repository._pulse_numstat_memo.clear()
    monkeypatch.setattr(gateway, "PULSE_NUMSTAT_SECONDS", -1.0)
    partial = read(app, "period=3d")
    assert json.loads(partial.body)["summary"]["partial"] is True
    assert partial.headers["Cache-Control"] == "no-store"
    assert read(app, "period=3d").headers["X-ForkMesh-Memory-Cache"] == "miss"


def test_ledgers_are_reread_only_where_they_changed(pulse_app, monkeypatch):
    app, _clock, _bare = pulse_app
    repository = app.repositories[("alice", "project")]
    payload(app, "period=1w")
    calls = []
    original = gateway._pulse_pull_record
    monkeypatch.setattr(gateway, "_pulse_pull_record",
                        lambda *args: calls.append(args[0]) or original(*args))
    repository._pulse_pulls = (("\0", "\0"), [])
    repository._pulse_ledger = {}
    payload(app, "period=1m")
    assert calls == []


def test_pull_ledger_reads_are_capped(pulse_app, monkeypatch):
    app, _clock, _bare = pulse_app
    repository = app.repositories[("alice", "project")]
    seen = {}
    original = gateway._pulse_pull_record
    monkeypatch.setattr(
        gateway, "_pulse_pull_record",
        lambda number, files: seen.setdefault(number, sorted(files))
        and original(number, files))
    monkeypatch.setattr(gateway, "PULSE_PULL_EVENT_FILES", 1)
    repository._pulse_pull_index()
    assert seen[42] == ["0005-comment.md", "pull.md"]
    assert seen[43] == ["0002-review.md", "pull.md"]

    seen.clear()
    repository._pulse_pulls = (("\0", "\0"), [])
    repository._pulse_pull_records = {}
    monkeypatch.setattr(gateway, "PULSE_PULL_READ_BYTES", 0)
    assert repository._pulse_pull_index()[1] == []
    assert seen == {}
    assert repository._pulse_pull_records == {}


def test_pulse_rides_on_the_stats_operation(tmp_path):
    bare = make_pulse_repository(tmp_path)
    entry = {
        "owner": "alice", "name": "project", "visibility": "public",
        "enabled": True, "gitDir": str(bare),
        "integrity": {"expectedRefsSha256": gateway.refs_sha256(bare)},
    }
    without = dict(entry, name="nostats", operations=sorted(
        gateway.DEFAULT_PUBLIC_OPERATIONS - {"stats"}))
    app = _application(tmp_path, bare, [NOW], repositories=[entry, without])
    try:
        assert read(app, "period=1w").status == 200
        refused = read(app, "period=1w", repository="nostats")
        unknown = read(app, "period=1w", repository="missing")
        assert refused.status == unknown.status == 404
        assert refused.body == unknown.body
        assert gateway.PULSE_OPERATION not in gateway.PUBLIC_OPERATIONS
    finally:
        app.close()


def test_restricted_repositories_answer_pulse_only_to_a_capability(tmp_path):
    bare = make_pulse_repository(tmp_path)
    restricted = {
        "owner": "alice", "name": "secret", "visibility": "restricted",
        "enabled": True, "gitDir": str(bare),
        "operations": sorted(gateway.DEFAULT_PUBLIC_OPERATIONS),
        "integrity": {"expectedRefsSha256": gateway.refs_sha256(bare)},
    }
    app = _application(tmp_path, bare, [NOW], repositories=[restricted])
    try:
        public = read(app, "period=1w", repository="secret")
        unknown = read(app, "period=1w", repository="nothing")
        assert public.status == unknown.status == 404
        assert public.body == unknown.body
        target = "/v1/repositories/alice/secret/pulse?period=1w"
        allowed = app.dispatch("GET", target, capability_headers(target), b"")
        assert allowed.status == 200
        assert json.loads(allowed.body)["pulls"]["merged"]["count"] == 1
    finally:
        app.close()


def test_scoring_helpers_cap_and_rank():
    assert gateway._pulse_streaks([1, 2, 3, 7, 8], 8) == (3, 2)
    assert gateway._pulse_streaks([1, 2, 3], 5) == (3, 0)
    assert gateway._pulse_streaks([4, 5, 9], 5) == (2, 2)
    ranked = gateway._pulse_ranking(
        {"a": (5, {}), "b": (5, {}), "c": (2, {})},
        {"a": "Ann", "b": "Ben", "c": "Cy"})
    assert [(rank, key) for rank, key, _score, _parts in ranked] == [
        (1, "a"), (1, "b"), (3, "c")]
    for name in ("dependabot[bot]", "github-actions", "renovate", "ci-bot",
                 "release-actions", "", "Claude", "Claude Opus 5.5",
                 "claude code", "Codex", "OpenAI Codex", "ChatGPT", "GPT-5",
                 "Gemini", "Gemini Code Assist", "GitHub Copilot",
                 "copilot-swe-agent", "Llama", "Devin", "Devin AI",
                 "Cursor Agent", "fmnext-n1", "fmnext-r1", "mirror35",
                 "eu-mirror", "ForkMesh Deploy", "ForkMesh Issue Reconciler"):
        assert gateway._pulse_is_automated(name), name
    for name, email in (
            ("Release Helper", "llama@cloudflare.com"),
            ("Someone", "noreply@anthropic.com"),
            ("Pat", "198982749+Copilot@users.noreply.github.com"),
            ("Deploy", "deploy@forkmesh.local"),
            ("Helper", "helper[bot]@users.noreply.github.com")):
        assert gateway._pulse_is_automated(name, email), name
    for name in ("Robot Hughes", "Claude Monet", "Claudia", "Gemma Lee",
                 "Mirror Lake", "Devina", "alice"):
        assert not gateway._pulse_is_automated(name, "x@example.test"), name
    for name in ("Jules", "Devin", "Cursor", "Sweep", "Llama", "Aider"):
        assert gateway._pulse_is_automated(name), name
        assert not gateway._pulse_is_automated(
            name, name.lower() + "@example.test"), name
    assert gateway._pulse_is_automated("Devin", "devin@devin.ai")
    assert gateway._pulse_is_automated("Jules", "jules@google.com")
    for email in ("bob@example.test", "pat@example.test", "bot@example.test"):
        assert not gateway._pulse_is_automated("Pat", email), email
    rules = gateway.pulse_exclusion_rules(
        "Node-A", {gateway.PULSE_EXCLUDE_ENV: " build-*, *@ci.example.test,,"})
    assert rules == ("*@ci.example.test", "build-*", "node-a")
    assert gateway._pulse_is_automated("Build-Agent", "", rules)
    assert gateway._pulse_is_automated("Pat", "pat@ci.example.test", rules)
    assert gateway._pulse_is_automated("node-a", "", rules)
    assert not gateway._pulse_is_automated("Pat", "pat@example.test", rules)
    assert gateway.pulse_exclusion_rules("", {}) == ()
    assert gateway._pulse_ms("1700000000") == 1_700_000_000_000
    assert gateway._pulse_ms("bad") == 0


def test_configured_exclusions_and_the_gateway_node_are_never_ranked(
        tmp_path, monkeypatch):
    monkeypatch.setenv(gateway.PULSE_EXCLUDE_ENV, "bob*")
    bare = make_pulse_repository(tmp_path)
    app = _application(tmp_path, bare, [NOW])
    try:
        value = json.loads(read(app, "period=1w").body)
        names = {row["name"] for row in value["leaderboard"]}
        assert names == {"alice", "carol"}
        assert value["summary"]["main"] == 10
        assert value["summary"]["authors"] == 3
        assert value["scoring"]["excludesAutomated"] is True
        assert all(row["name"] != "bob" for row in value["committers"])
        assert value["issues"]["opened"]["authors"] == 2
        mine = json.loads(read(app, "period=1w&person=bob").body)["person"]
        assert mine["found"] is False and mine["rank"] is None
    finally:
        app.close()
    assert "mirror-a" in gateway.pulse_exclusion_rules("mirror-a", {})


def test_people_view_counts_each_human_over_the_past_year(
        pulse_app, monkeypatch):
    app, clock, _bare = pulse_app
    value = payload(app, "view=people")
    end = -(-NOW_S // 3600) * 3600
    assert value["view"] == "people"
    assert value["since"] == gateway._pulse_iso(end - 365 * DAY)
    assert value["refreshSeconds"] == 3600
    assert "leaderboard" not in value and "pulls" not in value
    rows = {row["key"]: row for row in value["people"]}
    figures = {
        key: (row["commits"], row["pulls"], row["issues"], row["reviews"])
        for key, row in rows.items()}
    assert figures == {
        "alice": (10, 1, 0, 1), "bob": (1, 1, 0, 1), "carol": (2, 0, 0, 0),
        "dave": (0, 0, 1, 0), "erin": (0, 1, 0, 0), "frank": (0, 1, 1, 0),
        "grace": (0, 0, 1, 0)}
    assert value["peopleTotal"] == 7 and value["truncated"] is False
    assert [row["key"] for row in value["people"]][:2] == ["alice", "bob"]
    alice = rows["alice"]
    assert alice["lastAt"] == (NOW_S - 3 * 3600) * MS
    first = (end - 365 * DAY) // DAY
    assert value["dayStart"] == gateway._pulse_iso(first * DAY)[:10]
    mask = bytes.fromhex(alice["dayMask"])
    days = {first + index for index in range(len(mask) * 8)
            if mask[index // 8] >> (index % 8) & 1}
    expected = {(NOW_S - offset) // DAY for offset in (
        40 * DAY, 4 * DAY, 2 * DAY, 2 * DAY + 6 * 60, DAY, 12 * 3600,
        3 * 3600)}
    assert days == expected and alice["activeDays"] == len(expected)
    assert all(not gateway._pulse_is_automated(row["name"])
               for row in value["people"])
    assert read(app, "view=people").headers[
        "X-ForkMesh-Memory-Cache"] == "hit"
    clock[0] = NOW + 3601 * 1000
    assert read(app, "view=people").headers[
        "X-ForkMesh-Memory-Cache"] == "miss"
    for query in ("view=leaderboard", "view=people&period=1w",
                  "view=people&person=alice", "view="):
        assert read(app, query).status == 404, query
    monkeypatch.setattr(gateway, "PULSE_PEOPLE_LIMIT", 2)
    clock[0] = NOW + 2 * 3601 * 1000
    limited = payload(app, "view=people")
    assert len(limited["people"]) == 2 and limited["truncated"] is True


def test_the_worker_router_forwards_the_people_view():
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
    import edge_routing
    assert edge_routing.masked_target_url(
        "https://core.example.test", "alice", "project", "pulse",
        {"view": "people"},
    ) == ("https://core.example.test/v1/repositories/alice/project/pulse"
          "?view=people")
