#!/usr/bin/env python3
"""Searchable public repository index (issue #478): ranking, pagination,
deduplication, manipulation resistance, clone actions, and budgets."""

import ast
import asyncio
import json
import sys
import time
from pathlib import Path
from types import SimpleNamespace

import pytest


APP = Path(__file__).resolve().parents[1]
ROOT = APP.parent
sys.path.insert(0, str(APP / "src"))
import repository_index as index  # noqa: E402


NOW = 1_800_000_000_000
DAY = 24 * 60 * 60 * 1000


def repo(owner, name, **overrides):
    record = {
        "owner": owner,
        "name": name,
        "visibility": "public",
        "isPrivate": False,
        "sharedWithMe": False,
        "maintainer": "key-" + owner,
        "source": "local-node",
        "sourceAuthority": True,
        "rootCommit": "root-" + owner + "-" + name,
        "commit": "",
        "updatedAt": str(NOW - DAY),
        "hostedSince": str(NOW - 30 * DAY),
        "description": "",
        "logoMetadata": {"languages": {}, "topics": []},
        "activityWeeks": [0] * 52,
        "cloneOnline": True,
        "liveHost": True,
        "termsFlagged": False,
    }
    record.update(overrides)
    return record


def catalog(*records):
    return {"ok": True, "repositories": list(records)}


def run(records, now=NOW, **params):
    return index.search(catalog(*records), params, now)


def names(result):
    return [item["fullName"] for item in result["results"]]


def weeks(*recent):
    return [0] * (52 - len(recent)) + list(recent)


# ------------------------------------------------------------ determinism


def test_identical_inputs_give_identical_output_regardless_of_row_order():
    records = [repo("o%d" % i, "r%d" % i, activityWeeks=weeks(i % 3))
               for i in range(30)]
    first = run(records, view="trending", limit=50)
    second = run(list(reversed(records)), view="trending", limit=50)
    assert first == second
    # Ties on score and activity fall back to owner/name ascending.
    tied = run([repo("b", "x"), repo("a", "x"), repo("a", "w")])
    assert names(tied) == ["a/w", "a/x", "b/x"]


def test_cursor_walk_visits_every_match_exactly_once():
    records = [repo("owner%02d" % i, "repo", activityWeeks=weeks(i % 4 + 1))
               for i in range(47)]
    seen, cursor, pages = [], None, 0
    while True:
        params = {"view": "trending", "limit": "10"}
        if cursor:
            params["cursor"] = cursor
        page = index.search(catalog(*records), params, NOW)
        assert page["total"] == 47
        seen.extend(names(page))
        cursor = page["nextCursor"]
        pages += 1
        if cursor is None:
            break
    assert pages == 5
    assert len(seen) == len(set(seen)) == 47


def test_cursor_is_bound_to_its_query_and_validated():
    records = [repo("o%d" % i, "mesh%d" % i) for i in range(5)]
    page = run(records, q="mesh", limit="2")
    with pytest.raises(index.IndexQueryError) as caught:
        run(records, q="mesh", view="updated", limit="2",
            cursor=page["nextCursor"])
    assert caught.value.code == "cursor_query_mismatch"
    for bad in ("!!!", "e30", "eyJmIjoxfQ"):
        with pytest.raises(index.IndexQueryError) as caught:
            run(records, q="mesh", cursor=bad)
        assert caught.value.code == "invalid_cursor"


@pytest.mark.parametrize("params,code", [
    ({"view": "popular"}, "unknown_view"),
    ({"view": "relevance"}, "relevance_requires_query"),
    ({"limit": "0"}, "invalid_limit"),
    ({"limit": "51"}, "invalid_limit"),
    ({"limit": "ten"}, "invalid_limit"),
    ({"cloneable": "maybe"}, "invalid_cloneable"),
])
def test_malformed_parameters_are_rejected_not_coerced(params, code):
    with pytest.raises(index.IndexQueryError) as caught:
        run([repo("a", "b")], **params)
    assert caught.value.code == code


# ------------------------------------------------------------ search + facets


def test_relevance_requires_every_word_and_ranks_name_matches_first():
    records = [
        repo("alice", "mesh", description="a tool"),
        repo("bob", "meshkit", description="kit"),
        repo("carol", "notes", description="mesh notes"),
        repo("dave", "other", description="nothing"),
    ]
    result = run(records, q="mesh")
    assert result["view"] == "relevance"
    assert names(result) == ["alice/mesh", "bob/meshkit", "carol/notes"]
    assert names(run(records, q="mesh notes")) == ["carol/notes"]


def test_facets_and_filters():
    records = [
        repo("a", "one", logoMetadata={"languages": {"Python": 9, "C": 1},
                                       "topics": ["cli"]}),
        repo("b", "two", logoMetadata={"languages": {"Go": 5},
                                       "topics": ["cli", "mesh"]},
             cloneOnline=False, liveHost=False),
        repo("c", "three", logoMetadata={"languages": {"python": 3},
                                         "topics": []}),
    ]
    result = run(records)
    assert result["facets"]["languages"][0] == {"value": "python", "count": 2}
    assert result["facets"]["topics"][0] == {"value": "cli", "count": 2}
    assert result["facets"]["cloneable"] == 2
    assert names(run(records, language="PYTHON")) == ["a/one", "c/three"]
    assert names(run(records, topic="mesh")) == ["b/two"]
    assert "b/two" not in names(run(records, cloneable="1"))
    assert names(run(records, owner="c")) == ["c/three"]


# ------------------------------------------------------------ moderation


def test_private_flagged_and_unsigned_rows_are_withheld():
    records = [
        repo("ok", "visible"),
        repo("p", "private", visibility="private", isPrivate=True),
        repo("s", "shared", sharedWithMe=True),
        repo("t", "flagged", termsFlagged=True),
        repo("u", "unsigned", maintainer=""),
    ]
    result = run(records)
    assert names(result) == ["ok/visible"]
    assert result["withheld"]["ineligible"] == 4


def test_mirrors_are_not_separate_results_and_self_mirroring_does_not_count():
    source = repo("alice", "tool", rootCommit="r1",
                  activityWeeks=weeks(3), cloneOnline=False, liveHost=False)
    own_device = repo("alice", "tool", rootCommit="r1", source="remote-clone",
                      sourceAuthority=False)
    independent = repo("mirror8", "tool", rootCommit="r1",
                       source="remote-clone", cloneOnline=True)
    result = run([source, own_device, independent], view="trending")
    assert names(result) == ["alice/tool"]
    item = result["results"][0]
    assert item["availability"]["independentMirrors"] == 1
    assert item["availability"]["mirrorRows"] == 2
    # A healthy independent mirror makes the repository cloneable.
    assert item["availability"]["cloneOnline"] is True
    assert item["ranking"]["components"]["score"] == 3 + index.MIRROR_POINTS

    sockpuppets = [
        repo("alice", "tool", rootCommit="r1", source="remote-clone",
             sourceAuthority=False, nodeId="device%d" % i)
        for i in range(20)
    ]
    inflated = run([source] + sockpuppets, view="trending")
    assert inflated["results"][0]["ranking"]["components"]["score"] == 3


def test_bucketed_mirror_grouping_matches_the_brute_force_rule():
    from mirrors import repo_mirror_same_group

    canonical = [
        repo("alice", "tool", rootCommit="r1"),
        repo("bob", "tool", rootCommit=""),
        repo("carol", "site", rootCommit="r2",
             cloneUrl="https://forkmesh.com/mirror9/site-copy.git"),
        repo("dave", "lib", rootCommit="r3"),
    ]
    mirrors = [
        repo("m1", "tool", rootCommit="r1", source="remote-clone"),
        repo("m2", "tool", rootCommit="", source="remote-clone"),
        repo("m3", "renamed", rootCommit="wrong", source="remote-clone",
             cloneUrl="git@forkmesh.com:carol/site.git"),
        repo("mirror9", "site-copy", rootCommit="other", source="remote-clone"),
        repo("m5", "lib", rootCommit="r4", source="remote-clone",
             cloneUrl="https://forkmesh.com/dave/lib?x#y"),
        repo("m6", "unrelated", rootCommit="r9", source="remote-clone"),
    ]
    entries, _ = index.build_index(canonical + mirrors, NOW)
    for entry in entries:
        expected = [
            mirror for mirror in mirrors
            if repo_mirror_same_group(entry["record"], mirror)
        ]
        assert sorted(map(id, entry["group"][1:])) == sorted(map(id, expected))
    groups = {entry["owner"]: {row["owner"] for row in entry["group"][1:]}
              for entry in entries}
    assert groups["carol"] == {"m3", "mirror9"}
    assert groups["dave"] == {"m5"}
    assert "m6" not in set().union(*groups.values())


def test_republished_identical_content_collapses_to_the_earliest_host():
    head = "a" * 40
    original = repo("alice", "tool", commit=head,
                    hostedSince=str(NOW - 90 * DAY), activityWeeks=weeks(2))
    copy = repo("spammer", "tool-copy", commit=head,
                hostedSince=str(NOW - DAY), activityWeeks=weeks(50))
    result = run([copy, original], view="trending")
    assert names(result) == ["alice/tool"]
    assert result["results"][0]["duplicates"] == ["spammer/tool-copy"]
    assert result["withheld"]["duplicateContent"] == 1


def test_weekly_cap_bounds_bulk_history_and_stale_projects_leave_trending():
    bulk = repo("bulk", "gen", activityWeeks=weeks(10_000, 10_000, 10_000, 10_000))
    steady = repo("steady", "lib", activityWeeks=weeks(40, 40, 40, 40))
    result = run([bulk, steady], view="trending")
    scores = {item["fullName"]: item["ranking"]["components"]["score"]
              for item in result["results"]}
    assert scores["bulk/gen"] == 4 * index.WEEKLY_ACTIVITY_CAP
    assert scores["steady/lib"] == 160
    # Old history outside the trending window adds nothing.
    old = repo("old", "busy", activityWeeks=[500] * 48 + [0, 0, 0, 0])
    assert names(run([old], view="trending")) == []
    stale = repo("stale", "x", updatedAt=str(NOW - 30 * DAY),
                 activityWeeks=weeks(9), discussionCount="4")
    assert names(run([stale], view="trending")) == []
    assert names(run([stale], view="discussions")) == []
    assert names(run([stale], q="stale")) == ["stale/x"]


def test_heartbeat_republish_does_not_change_rank_and_future_clocks_are_ignored():
    first = repo("a", "one", commitAt=str(NOW - 3 * DAY))
    second = repo("b", "two", commitAt=str(NOW - 2 * DAY))
    before = names(run([first, second]))
    first["updatedAt"] = str(NOW)  # heartbeat republish, no new commit
    assert names(run([first, second])) == before == ["b/two", "a/one"]
    future = repo("f", "future", hostedSince=str(NOW + DAY))
    assert names(run([future, repo("n", "normal")], view="new")) == ["n/normal"]


def test_discussions_view_and_new_view():
    records = [
        repo("a", "quiet", hostedSince=str(NOW - 10 * DAY)),
        repo("b", "talky", discussionCount="3", pullCount="2", issueCount="1",
             hostedSince=str(NOW - 20 * DAY)),
        repo("c", "spam", discussionCount="999999", hostedSince=""),
    ]
    discussions = run(records, view="discussions")
    assert names(discussions) == ["c/spam", "b/talky"]
    assert discussions["results"][0]["ranking"]["components"]["score"] == 500
    assert names(run(records, view="new")) == ["a/quiet", "b/talky"]


# ------------------------------------------------------------ clone + provenance


def test_every_result_has_account_free_clone_and_maintainer_provenance():
    org = repo("mirror3", "site", logicalOwners=[
        {"kind": "user", "owner": "mirror3"},
        {"kind": "organization", "owner": "acme"}],
        sshUrl="ssh://git@ssh.forkmesh.com/acme/site.git")
    result = index.search(catalog(org, repo("alice", "tool")), {}, NOW,
                          origin=index.public_origin("https://app.forkmesh.com"))
    for item in result["results"]:
        clone = item["clone"]
        assert clone["requiresAccount"] is False
        assert clone["https"].startswith("https://forkmesh.com/")
        assert clone["command"] == "git clone " + clone["https"]
        assert item["maintainer"]["key"]
        assert item["maintainer"]["verification"] == (
            "account-key-signed-catalog-record")
    by_name = {item["name"]: item for item in result["results"]}
    assert by_name["site"]["clone"]["https"] == "https://forkmesh.com/acme/site"
    assert by_name["site"]["clone"]["ssh"].endswith("/acme/site.git")
    assert "ssh" not in by_name["tool"]["clone"]
    assert index.public_origin("https://node.example:8443") == (
        "https://node.example:8443")
    assert index.public_origin("javascript:alert(1)") == index.PUBLIC_ORIGIN


# ------------------------------------------------------------ budgets


def test_freshness_is_reported_against_the_published_budget():
    fresh = index.search(catalog(repo("a", "b")), {}, NOW,
                         snapshot_generated_at=NOW - 60_000)
    assert fresh["freshness"]["withinBudget"] is True
    assert fresh["budgets"]["freshnessMs"] == index.FRESHNESS_BUDGET_MS
    old = index.search(catalog(repo("a", "b")), {}, NOW,
                       snapshot_generated_at=NOW - index.FRESHNESS_BUDGET_MS - 1)
    assert old["freshness"]["withinBudget"] is False
    # The published freshness budget is the snapshot's own fresh window.
    snapshots = (APP / "src" / "public_snapshots.py").read_text(encoding="utf-8")
    assert '"/api/repositories": 6 * 60 * 1000,' in snapshots
    assert index.FRESHNESS_BUDGET_MS == 6 * 60 * 1000


def test_ranking_a_full_catalog_stays_inside_the_latency_budget():
    entry = (APP / "src" / "entry.py").read_text(encoding="utf-8")
    maximum = int(entry.split("\nMAX_CATALOG_REPOS = ", 1)[1].split("\n", 1)[0])
    records = []
    for i in range(maximum):
        records.append(repo(
            "owner%03d" % i, "repo%03d" % i,
            description="mesh tool number %d" % i,
            logoMetadata={"languages": {"Python": i, "Go": 1},
                          "topics": ["mesh", "t%d" % (i % 7)]},
            activityWeeks=[i % 11] * 52, rootCommit="root%d" % (i % 150)))
        records.append(repo(
            "mirror%03d" % i, "repo%03d" % i, source="remote-clone",
            rootCommit="root%d" % (i % 150)))
    payload = catalog(*records)
    views = ({"q": "mesh tool"}, {"view": "trending"}, {"view": "new"},
             {"view": "discussions"}, {})
    worst = 0.0
    for params in views:
        started = time.perf_counter()
        index.search(payload, dict(params, limit="50"), NOW)
        worst = max(worst, (time.perf_counter() - started) * 1000)
    assert worst < index.LATENCY_BUDGET_MS, worst


# ------------------------------------------------------------ route wiring


def _load_handler(extra):
    source = (APP / "src" / "catalog_routes.py").read_text(encoding="utf-8")
    tree = ast.parse(source)
    node = next(item for item in tree.body
                if isinstance(item, ast.AsyncFunctionDef)
                and item.name == "repository_index_search_handler")
    namespace = dict(extra)
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=[node], type_ignores=[])), "catalog_routes", "exec"),
        namespace)
    return namespace["repository_index_search_handler"]


def _handler_runtime(snapshot_payload=None, live_payload=None):
    from urllib.parse import parse_qs, urlparse
    calls = {"live": 0}

    class Response:
        def __init__(self, body, status=200, headers=None):
            self.body, self.status, self.headers = body, status, headers or {}

    def json_response(data, status=200, cache_seconds=None,
                      cache_control=None, extra_headers=None):
        headers = dict(extra_headers or {})
        if cache_control:
            headers["cache-control"] = cache_control
        elif cache_seconds is not None:
            headers["cache-control"] = "public, max-age=%d" % cache_seconds
        return Response(json.loads(json.dumps(data)), status, headers)

    async def _public_snapshot_row(_env, path):
        assert path == "/api/repositories"
        return {"generated_at": NOW - 1000, "payload": snapshot_payload}

    def _validated_public_snapshot_row(row, path, now, fingerprint,
                                       allow_authoritative_fallback=False):
        assert allow_authoritative_fallback is True
        if row["payload"] is None:
            raise RuntimeError("snapshot row is missing")
        return row["payload"]

    async def catalog_handler(_env, _request):
        calls["live"] += 1
        return live_payload

    async def _response_json(response):
        return response or {}

    runtime = {
        "method_name": lambda request: request.method,
        "json_response": json_response,
        "urlparse": urlparse, "parse_qs": parse_qs,
        "Date": SimpleNamespace(now=lambda: NOW),
        "_public_snapshot_row": _public_snapshot_row,
        "_validated_public_snapshot_row": _validated_public_snapshot_row,
        "catalog_handler": catalog_handler,
        "_response_json": _response_json,
        "JsRequest": SimpleNamespace(new=lambda url, init: (url, init)),
        "to_js": lambda value: value,
        "CATALOG_TTL": 30,
    }
    return _load_handler(runtime), calls


def _request(query, method="GET"):
    return SimpleNamespace(
        method=method,
        url="https://app.forkmesh.com/api/repositories/search" + query)


def test_handler_serves_snapshot_rejects_bad_input_and_falls_back():
    handler, calls = _handler_runtime(snapshot_payload=catalog(
        repo("alice", "mesh"), repo("bob", "other")))
    ok = asyncio.run(handler(SimpleNamespace(), _request("?q=mesh")))
    assert ok.status == 200
    assert ok.headers["x-forkmesh-index-source"] == "snapshot"
    assert ok.headers["cache-control"] == "public, max-age=30"
    assert [item["fullName"] for item in ok.body["results"]] == ["alice/mesh"]
    assert ok.body["results"][0]["clone"]["https"] == (
        "https://forkmesh.com/alice/mesh")
    assert ok.body["freshness"]["snapshotAgeMs"] == 1000
    assert calls["live"] == 0

    bad = asyncio.run(handler(SimpleNamespace(), _request("?view=nope")))
    assert (bad.status, bad.body) == (400, {"error": "unknown_view"})
    post = asyncio.run(handler(SimpleNamespace(), _request("", method="POST")))
    assert post.status == 405

    fallback, calls = _handler_runtime(
        snapshot_payload=None, live_payload=catalog(repo("carol", "tool")))
    live = asyncio.run(fallback(SimpleNamespace(), _request("")))
    assert live.status == 200 and calls["live"] == 1
    assert live.headers["x-forkmesh-index-source"] == "live-catalog"
    assert live.body["freshness"]["withinBudget"] is True

    down, _ = _handler_runtime(snapshot_payload=None, live_payload={})
    unavailable = asyncio.run(down(SimpleNamespace(), _request("")))
    assert unavailable.status == 503
    assert unavailable.body == {"error": "catalog_unavailable"}


def test_route_export_contract_and_docs_stay_in_step():
    runtime = (APP / "src" / "app_runtime.py").read_text(encoding="utf-8")
    route = runtime.index('"/api/repositories/search"')
    assert route < runtime.index(
        'if url.path in ("/api/repositories", "/api/repositories/"):')
    entry = (APP / "src" / "entry.py").read_text(encoding="utf-8")
    assert '_catalog_routes.export(\n    "repository_index_search_handler")' in entry
    contract = json.loads((ROOT / "conformance" / "integrations" / "v1" /
                           "contract.json").read_text(encoding="utf-8"))
    resource = next(item for item in contract["interfaces"]["http"]
                    if item["id"] == "repository-search")
    assert resource["path"] == "/api/repositories/search"
    assert resource["auth"] == "anonymous"
    docs = (ROOT / "www" / "docs" / "repository-index.md").read_text(
        encoding="utf-8")
    for fact in ("50", "14 days", "capped at 50", "5 points per independent",
                 "at most 8 mirrors", "capped at 500", "6 minutes", "250 ms"):
        assert fact in docs, fact
    assert index.MAX_LIMIT == 50
    assert index.ACTIVE_WINDOW_MS == 14 * DAY
    assert index.MIRROR_POINTS == 5 and index.MAX_COUNTED_MIRRORS == 8
    assert index.DISCUSSION_COUNT_CAP == 500
    assert index.LATENCY_BUDGET_MS == 250
