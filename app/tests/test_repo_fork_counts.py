import ast
import asyncio
import json
import sys
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import parse_qs, urlparse

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

import edge_routing  # noqa: E402
import fork_counts  # noqa: E402
import urls  # noqa: E402

PARENT_TIP = "a" * 40
FORK_TIP = "b" * 40
OTHER_TIP = "c" * 40


class FakeResponse:
    def __init__(self, status, value):
        self.status = status
        self._text = json.dumps(value)

    async def text(self):
        return self._text


@pytest.fixture
def runtime(monkeypatch):
    state = SimpleNamespace(
        records={
            ("alice", "widget"): {"visibility": "public"},
            ("bob", "widget"): {"visibility": "public",
                                "forkedFrom": "alice/widget"},
            ("carol", "widget"): {"visibility": "public",
                                  "forkedFrom": "alice/widget"},
            ("dave", "widget"): {"visibility": "public",
                                 "forkedFrom": "secret/widget"},
            ("secret", "widget"): {"visibility": "private"},
            ("erin", "widget"): {"visibility": "public"},
            ("node7", "widget"): {"visibility": "public",
                                  "forkedFrom": "alice/widget"},
        },
        private={("secret", "widget"), ("frank", "widget")},
        orgs={("acme", "widget"): "node7"},
        tips={("alice", "widget"): PARENT_TIP, ("bob", "widget"): FORK_TIP,
              ("carol", "widget"): OTHER_TIP, ("acme", "widget"): FORK_TIP},
        counts={FORK_TIP: (3, 12), OTHER_TIP: (0, 0)},
        reads=[],
    )

    async def catalog(env, owner, repo):
        return state.records.get((owner, repo))

    async def private(env, owner, repo):
        return (owner, repo) in state.private

    async def org_node(env, owner, repo):
        return state.orgs.get((owner, repo), "")

    async def body(request):
        if request.body is None:
            raise ValueError("bad json")
        return request.body

    async def routed(env, url, operation, bypass_cache=False):
        parsed = urlparse(url)
        parts = parsed.path.split("/")
        owner, repo = parts[3], parts[4]
        query = {key: values[0] for key, values in parse_qs(parsed.query).items()}
        state.reads.append((owner, repo, operation, query, bypass_cache))
        if operation == "branches":
            tip = state.tips.get((owner, repo))
            if not tip:
                return FakeResponse(503, {"error": "mirror_unavailable"})
            return FakeResponse(200, {"defaultBranch": "main", "branches": [
                {"name": "feature", "commit": "d" * 40},
                {"name": "main", "commit": tip}]})
        assert operation == "compare"
        ahead, behind = state.counts[query["head"]]
        return FakeResponse(200, {"ok": True, "baseOid": query["base"],
                                  "headOid": query["head"],
                                  "ahead": ahead, "behind": behind})

    def json_response(data, status=200, cache_control=None, extra_headers=None):
        return SimpleNamespace(status=status, data=data,
                               cache_control=cache_control)

    shared = {
        "_repo_catalog_record": catalog, "_repo_is_private": private,
        "_org_repo_node": org_node, "bounded_json_request": body,
        "_routed_repository_read": routed, "json_response": json_response,
        "method_name": lambda request: request.method,
    }
    for name, value in shared.items():
        monkeypatch.setattr(fork_counts, name, value, raising=False)
    fork_counts._counts_cache.clear()
    return state


def call(body, method="POST"):
    request = SimpleNamespace(method=method, body=body)
    return asyncio.run(fork_counts.handler(SimpleNamespace(), request))


def test_the_route_is_registered():
    assert urls.REPO_FORK_COUNTS_RE.match("/api/repo-fork-counts")
    assert not urls.REPO_FORK_COUNTS_RE.match("/api/repo-fork-counts/x")
    runtime_source = (ROOT / "src" / "app_runtime.py").read_text("utf-8")
    assert "REPO_FORK_COUNTS_RE.match(url.path)" in runtime_source
    assert "fork_counts.handler(self.env, request)" in runtime_source
    entry_source = (ROOT / "src" / "entry.py").read_text("utf-8")
    assert '"REPO_FORK_COUNTS_RE"' in entry_source


def test_counts_compare_the_default_tips_against_the_parent(runtime):
    out = call({"forks": ["Bob/widget", "carol/widget", "bob/widget"]})
    assert out.status == 200 and out.cache_control.startswith("no-store")
    assert out.data == {"ok": True, "counts": {
        "bob/widget": {"ahead": 3, "behind": 12, "parent": "alice/widget",
                       "branch": "main"},
        "carol/widget": {"ahead": 0, "behind": 0, "parent": "alice/widget",
                         "branch": "main"},
    }}
    compares = [read for read in runtime.reads if read[2] == "compare"]
    assert [(owner, query) for owner, _repo, _op, query, _fresh in compares] == [
        ("bob", {"base": PARENT_TIP, "head": FORK_TIP,
                 "cherry": "alice/widget", "counts": "1"}),
        ("carol", {"base": PARENT_TIP, "head": OTHER_TIP,
                   "cherry": "alice/widget", "counts": "1"}),
    ]
    parent_reads = [read for read in runtime.reads
                    if read[:3] == ("alice", "widget", "branches")]
    assert len(parent_reads) == 1
    assert all(read[4] is False for read in runtime.reads)


def test_counts_are_cached_until_a_tip_moves(runtime):
    call({"forks": ["bob/widget"]})
    runtime.reads.clear()
    call({"forks": ["bob/widget"]})
    assert not [read for read in runtime.reads if read[2] == "compare"]
    runtime.tips[("alice", "widget")] = OTHER_TIP
    call({"forks": ["bob/widget"]})
    assert [read[3]["base"] for read in runtime.reads
            if read[2] == "compare"] == [OTHER_TIP]


def test_private_unknown_and_unforked_repositories_get_no_counts(runtime):
    out = call({"forks": ["dave/widget", "frank/widget", "erin/widget",
                          "alice/widget", "nobody/here", "x/y/z", "../a", 7]})
    assert out.data == {"ok": True, "counts": {}}
    assert not [read for read in runtime.reads if read[2] == "compare"]


def test_an_unreadable_tip_or_failed_compare_is_left_out(runtime):
    del runtime.tips[("bob", "widget")]
    assert call({"forks": ["bob/widget"]}).data["counts"] == {}

    async def failing(env, url, operation, bypass_cache=False):
        return FakeResponse(404, {"error": "not_found"})

    fork_counts._routed_repository_read = failing
    assert call({"forks": ["carol/widget"]}).data["counts"] == {}


def test_an_organization_address_reads_the_backing_catalog_row(runtime):
    out = call({"forks": ["acme/widget"]})
    assert out.data["counts"] == {"acme/widget": {
        "ahead": 3, "behind": 12, "parent": "alice/widget", "branch": "main"}}


def test_requests_are_bounded_and_post_only(runtime):
    assert call({"forks": []}, method="GET").status == 405
    assert call(None).status == 400
    keys = fork_counts.fork_keys(["o/r%d" % index for index in range(100)])
    assert len(keys) == fork_counts.MAX_FORKS
    assert fork_counts.fork_keys("bob/widget") == []


def _function_source(name):
    source = (ROOT / "src" / "entry.py").read_text("utf-8")
    tree = ast.parse(source)
    node = next(item for item in tree.body
                if isinstance(item, ast.FunctionDef) and item.name == name)
    return ast.get_source_segment(source, node)


def test_the_worker_forwards_counts_for_compare_only():
    namespace = {"parse_qs": parse_qs, "MAX_BLOB_BATCH": 60,
                 "HTTPS_MIRROR_REF_OPERATIONS": frozenset({"tree"}),
                 "_repository_read_path": lambda value: value}
    exec(_function_source("_https_mirror_request_query"), namespace)
    request_query = namespace["_https_mirror_request_query"]
    raw = "base=%s&head=%s&cherry=alice/widget&counts=1" % (PARENT_TIP, FORK_TIP)
    assert request_query(urlparse("https://x.invalid/?" + raw), "compare") == {
        "base": PARENT_TIP, "head": FORK_TIP, "cherry": "alice/widget",
        "counts": "1"}
    assert "counts" not in request_query(
        urlparse("https://x.invalid/?" + raw), "merge-preview")
    assert edge_routing.masked_target_url(
        "https://core.example.test", "bob", "widget", "compare",
        {"base": PARENT_TIP, "head": FORK_TIP, "cherry": "alice/widget",
         "counts": "1"},
    ).endswith("&counts=1")
