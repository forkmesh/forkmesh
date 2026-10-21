import json
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from test_issue_timeline_people import _const, _function  # noqa: E402
from test_mirror_gateway import (  # noqa: E402
    NOW,
    AcceptVerifier,
    FakeArchiveMaterializer,
    HealthSigner,
    gateway,
    run,
    write_config,
)

ROOT = Path(__file__).resolve().parents[1]
CONTENT = (ROOT / "public" / "dashboard" / "js" / "06-repo-content.js").read_text(
    encoding="utf-8")
T = 1_760_000_000_000


def _event(kind, eid, author, ts, **extra):
    return dict({"type": kind, "id": eid, "author": "key-" + author,
                 "authorName": author, "ts": ts}, **extra)


RECORDS = {
    ".forkmesh/issues/open/12/issue-12.json": {
        "number": 12, "title": "Edited and labelled", "status": "open",
        "labels": ["bug", "help wanted"], "milestone": "v1", "priority": 2,
        "progress": "40", "assignees": ["carol", 7], "createdAt": T,
        "authorName": "alice", "wantsAgent": True,
        "events": [
            _event("open", "o12", "alice", T, body="Parent issue: #3\nsee #9"),
            _event("edit", "e12", "alice", T + 5, target="o12",
                   body="Depends on: #3, #10\nping @Bob and @carol-x, `#4`\n"
                   "Fixed by #77"),
            _event("comment", "c1", "bob", T + 9,
                   body="mentions #10 and @dave\nLinked pull request #78"),
            _event("comment", "c2", "erin", T + 10, body="#10 again"),
            _event("delete", "d1", "erin", T + 11, target="c2"),
            _event("comment", "c3", "frank", T + 12,
                   body="Unlinked pull request #77"),
            _event("vote", "v1", "frank", T + 13),
            None,
            "junk",
        ],
    },
    ".forkmesh/issues/open/10/issue-10.json": {
        "number": 10, "title": "Commented", "labels": "[ ops , ,infra ]",
        "createdAt": str(T // 1000), "updatedAt": "",
        "events": [
            _event("open", "o10", "gina", T, body="Sub of #12 nope\n"
                   "```\n#12 in code\n```\n- **Depends on:** #12"),
            _event("comment", "c10", "hal", "bad", body="see #12"),
            _event("status", "s10", "hal", T + 50, status="reopened"),
        ],
    },
    ".forkmesh/issues/closed/3/issue-3.json": {
        "number": 3, "title": "Closed parent", "status": "open",
        "createdAt": T - 100, "votes": 4,
        "events": [
            _event("open", "o3", "ivy", T - 100, body="Root (sub-issue #12)"),
            _event("status", "s3", "ivy", T - 50, status="closed"),
            _event("delete", "x3", "mallory", T - 40, target="self"),
        ],
    },
    ".forkmesh/issues/closed/4/issue-4.json": {
        "title": "Deleted by its creator",
        "events": [
            _event("open", "o4", "jay", T - 10, body="gone #12"),
            _event("delete", "x4", "jay", T - 5, target="self"),
            _event("status", "s4", "jay", T - 4, status="closed"),
        ],
    },
    ".forkmesh/issues/9/issue-9.json": {
        "number": 9, "state": "closed", "createdAt": 1.5e12 + 0.25,
        "events": []},
    ".forkmesh/issues/open/8/issue-8.json": "{not json",
    ".forkmesh/issues/open/7/notes.txt": "folder without its record",
    ".forkmesh/issues/closed/12/issue-12.json": {
        "number": 12, "title": "stale closed copy", "events": []},
}


def _make_repository(tmp_path, records=RECORDS, bare=None):
    source = tmp_path / "issues-source"
    first = bare is None
    if first:
        bare = tmp_path / "issues.git"
        run(["git", "init", "--bare", "-q", str(bare)], tmp_path)
        run(["git", "init", "-q", "-b", "main", str(source)], tmp_path)
        run(["git", "config", "user.email", "t@example.test"], source)
        run(["git", "config", "user.name", "t"], source)
        (source / "README.md").write_text("hi\n", encoding="utf-8")
    for relative, value in records.items():
        path = source / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        text = value if isinstance(value, str) else json.dumps(value)
        path.write_text(text, encoding="utf-8")
    run(["git", "add", "-A"], source)
    run(["git", "commit", "-q", "-m", "issues"], source)
    if first:
        run(["git", "remote", "add", "origin", str(bare)], source)
    run(["git", "push", "-q", "-f", "origin", "main"], source)
    if first:
        run(["git", "symbolic-ref", "HEAD", "refs/heads/main"], bare)
    return bare


def _application(tmp_path, bare, repositories=None):
    config = gateway.load_config(
        write_config(tmp_path, bare, repositories=repositories))
    return gateway.GatewayApplication(
        config,
        verifier=AcceptVerifier(),
        health_signer=HealthSigner(),
        materializer=FakeArchiveMaterializer(bare),
        clock_ms=lambda: NOW,
        log=lambda _record: None,
    )


@pytest.fixture
def issue_app(tmp_path):
    bare = _make_repository(tmp_path)
    app = _application(tmp_path, bare)
    yield app, bare, tmp_path
    app.close()


def _read(app, query="", repository="project"):
    target = "/v1/public/repositories/alice/%s/issue-index" % repository
    if query:
        target += "?" + query
    return app.dispatch("GET", target, {}, b"")


def _payload(app, query=""):
    response = _read(app, query)
    assert response.status == 200, response.body
    return json.loads(response.body)


def test_index_lists_every_record_once_with_its_folder(issue_app):
    app, bare, _tmp = issue_app
    value = _payload(app, "ref=main")
    assert value["ok"] is True and value["truncated"] is False
    assert value["commit"] == run(["git", "rev-parse", "main"], bare)
    rows = {item["number"]: item for item in value["issues"]}
    assert [item["number"] for item in value["issues"]] == [12, 10, 9, 8, 4, 3]
    assert rows[12]["location"] == "open" and rows[12]["title"] == (
        "Edited and labelled")
    assert rows[3]["location"] == "closed" and rows[9]["location"] == ""
    assert rows[8]["title"] == "issue #8"
    assert value["missing"] == [7]
    assert value["openFolders"] == 4 and value["closedFolders"] == 3
    assert all(event["type"] == "status" for event in rows[10]["events"])


SEARCHES = ["#77", "ping", "help wanted", "v1", "alice", " PING ", "dave",
            "again", "12", "Commented"]
COMMENT_ONLY = {"dave", "12"}


def _node_compare(summaries, raw):
    names = (
        "parseFrontMatterList", "parseIssueJson", "issueFromIndex",
        "issueDependencyNumbers", "issueReferenceNumbers",
        "issueSubIssueNumbers", "issueMentionsOf", "developmentProse",
        "issueDevelopmentPullStates", "issueRecordPullStates",
        "pullLinkedIssueNumbers", "issueMentionsAccount",
        "issueInvolvesAccount", "issueMatchesQuery", "issueSortItems",
        "formatRecordDate",
    )
    script = """
const state = { session: { nodeName: "me" }, issuesView: { filters: { sort: "recently_updated" } } };
function formatDate(value) { return "date:" + String(value); }
function issueFilterState() { return state.issuesView.filters; }
%s
%s
%s
const raw = %s;
const summaries = %s;
const full = Object.entries(raw).map(([number, text]) => parseIssueJson(text, Number(number)));
const indexed = summaries.map(issueFromIndex);
const byNumber = (a, b) => Number(b.number) - Number(a.number);
full.sort(byNumber);
indexed.sort(byNumber);
const skip = new Set(["events", "openAttachments", "body"]);
const out = { rows: {}, relations: {} };
full.forEach((issue) => {
  const other = indexed.find((item) => item.number === issue.number);
  out.rows[issue.number] = Object.keys(issue).filter((key) => !skip.has(key))
    .filter((key) => JSON.stringify(issue[key]) !== JSON.stringify(other?.[key]))
    .map((key) => [key, issue[key], other?.[key]]);
});
const numbers = full.map((issue) => issue.number);
for (const target of numbers.concat([77, 78])) {
  out.relations[target] = [
    [JSON.stringify(issueMentionsOf(full, target)), JSON.stringify(issueMentionsOf(indexed, target))],
    [JSON.stringify(issueSubIssueNumbers(full, target, "")), JSON.stringify(issueSubIssueNumbers(indexed, target, ""))],
    [JSON.stringify(pullLinkedIssueNumbers(target, [], full)), JSON.stringify(pullLinkedIssueNumbers(target, [], indexed))],
  ];
}
out.people = {};
for (const name of ["bob", "BOB", "carol-x", "carol", "dave", "hal", "alice", "frank", "nobody"]) {
  out.people[name] = [
    full.map((issue) => [issueMentionsAccount(issue, name), issueInvolvesAccount(issue, name)]),
    indexed.map((issue) => [issueMentionsAccount(issue, name), issueInvolvesAccount(issue, name)]),
  ];
}
out.sorts = {};
for (const sort of ["recently_updated", "newest", "oldest", "priority", "comments"]) {
  out.sorts[sort] = [issueSortItems(full.slice(), sort).map((i) => i.number),
    issueSortItems(indexed.slice(), sort).map((i) => i.number)];
}
out.search = Object.fromEntries(SEARCHES.map((q) =>
  [q, full.filter((i) => issueMatchesQuery(i, q.trim().toLowerCase())).map((i) => i.number)]));
out.dependsOn = full.map((issue) => [issueDependencyNumbers(issue.body),
  indexed.find((item) => item.number === issue.number)?.dependsOn]);
out.childRefs = full.map((issue) => [issueSubIssueNumbers([], issue.number, issue.body),
  (indexed.find((item) => item.number === issue.number)?.childRefs || []).slice().sort((a, b) => a - b)]);
console.log(JSON.stringify(out));
""" % (
        _const(CONTENT, "ISSUE_PULL_LINK"),
        _const(CONTENT, "PULL_CLOSING_REFERENCE"),
        "\n".join(_function(CONTENT, name) for name in names),
        json.dumps(raw), json.dumps(summaries))
    script = "const SEARCHES = %s;\n" % json.dumps(SEARCHES) + script
    result = subprocess.run(["node", "-e", script], capture_output=True,
                            text=True, check=True)
    return json.loads(result.stdout)


def test_folding_matches_parse_issue_json_in_the_dashboard(issue_app):
    app, _bare, _tmp = issue_app
    value = _payload(app)
    raw = {}
    for item in value["issues"]:
        place = item["location"]
        path = ".forkmesh/issues/%s%d/issue-%d.json" % (
            place + "/" if place else "", item["number"], item["number"])
        record = RECORDS[path]
        raw[item["number"]] = record if isinstance(record, str) else json.dumps(
            record)
    assert not any("mentionRefs" in item for item in value["issues"])
    repository = app.repositories[("alice", "project")]
    out = _node_compare(repository._issue_index_tree[1]["issues"], raw)
    assert out["rows"] == {str(number): [] for number in raw}
    for target, pairs in out["relations"].items():
        for full, indexed in pairs:
            assert full == indexed, target
    for name, (full, indexed) in out["people"].items():
        assert full == indexed, name
    for sort, (full, indexed) in out["sorts"].items():
        assert full == indexed, sort
    for full, indexed in out["dependsOn"] + out["childRefs"]:
        assert full == indexed
    assert "body" not in value["issues"][0]
    for query, full in out["search"].items():
        found = repository.issue_search({"q": query})["numbers"]
        assert set(full) <= set(found), query
        if query not in COMMENT_ONLY:
            assert sorted(found) == sorted(full), query
    assert json.loads(out["relations"]["10"][0][0])
    assert json.loads(out["relations"]["77"][2][0]) == []
    assert json.loads(out["relations"]["78"][2][0]) == [12]


def test_fold_of_edge_values_matches_the_dashboard():
    records = {
        1: json.dumps({"number": "0x10", "title": 0, "labels": [None, 1.5, True],
                       "priority": "1e-7", "votes": [3],
                       "events": [{"type": "open", "id": [], "ts": "12"},
                                  {"type": "edit", "target": [], "body": "x"}]}),
        2: json.dumps([1, 2]),
        3: "null",
        4: json.dumps({"createdAt": 1e21, "updatedAt": 0.000001,
                       "milestone": 0, "status": "", "state": "triage",
                       "assignees": [12345678901234567890, -0.0]}),
        5: "\ufeff{}",
        6: json.dumps({"events": [{"type": "status", "status": "closed",
                                   "ts": "Infinity"},
                                  {"type": "comment", "id": 4},
                                  {"type": "delete", "target": 4}]}),
    }
    summaries = [dict(gateway._issue_index_summary(number, text.encode()),
                      location="open") for number, text in records.items()]
    out = _node_compare(summaries, records)
    assert out["rows"] == {str(json.loads(json.dumps(s["number"]))): []
                           for s in summaries}, out["rows"]


def test_js_number_text_matches_javascript():
    values = [1.5e-7, 0.000001, 1e21, 123456789012345680000.0, 0.1, 100.0,
              -2.5, 1e-7, 5e-324, 1.7976931348623157e308, 12345.678]
    script = "console.log(JSON.stringify(%s.map(String)))" % json.dumps(values)
    expected = json.loads(subprocess.run(
        ["node", "-e", script], capture_output=True, text=True,
        check=True).stdout)
    assert [gateway._js_number_text(value) for value in values] == expected


def test_records_are_refolded_only_when_their_file_changes(
        issue_app, monkeypatch):
    app, bare, tmp_path = issue_app
    first = _payload(app, "ref=main")
    calls = []
    original = gateway._issue_index_summary
    monkeypatch.setattr(gateway, "_issue_index_summary",
                        lambda number, raw: calls.append(number)
                        or original(number, raw))
    changed = dict(RECORDS[".forkmesh/issues/open/10/issue-10.json"],
                   title="Retitled")
    _make_repository(tmp_path, {
        ".forkmesh/issues/open/10/issue-10.json": changed}, bare=bare)
    repository = app.repositories[("alice", "project")]
    head = subprocess.run(["git", "--git-dir", str(bare), "rev-parse", "main"],
                          capture_output=True, text=True, check=True).stdout.strip()
    second = repository.issue_index({"ref": head})
    assert calls == [10]
    titles = {item["number"]: item["title"] for item in second["issues"]}
    assert titles[10] == "Retitled" and titles[12] == "Edited and labelled"
    assert second["commit"] == head != first["commit"]
    calls.clear()
    assert repository.issue_index({"ref": head})["issues"] == second["issues"]
    assert repository.issue_index({"ref": first["commit"]})["issues"] == (
        first["issues"])
    assert calls == []


def test_repeated_reads_are_served_from_memory(issue_app):
    app, _bare, _tmp = issue_app
    _read(app, "ref=main")
    again = _read(app, "ref=main")
    assert again.status == 200
    assert again.headers.get("X-ForkMesh-Memory-Cache") == "hit"


def test_bad_ref_and_unknown_repository_answer_not_found(issue_app):
    app, _bare, _tmp = issue_app
    assert _read(app, "ref=no-such-branch").status == 404
    assert _read(app, "ref=main", repository="missing").status == 404
    assert _read(app, "ref=main&path=x").status == 404


def test_issue_index_rides_on_the_blobs_operation(tmp_path):
    bare = _make_repository(tmp_path)
    entry = {
        "owner": "alice", "name": "project", "visibility": "public",
        "enabled": True, "gitDir": str(bare),
        "integrity": {"expectedRefsSha256": gateway.refs_sha256(bare)},
    }
    without = dict(entry, name="noblobs", operations=sorted(
        gateway.DEFAULT_PUBLIC_OPERATIONS - {"blobs"}))
    app = _application(tmp_path, bare, repositories=[entry, without])
    try:
        assert _read(app).status == 200
        refused = _read(app, repository="noblobs")
        unknown = _read(app, repository="missing")
        assert refused.status == unknown.status == 404
        assert refused.body == unknown.body
        assert gateway.ISSUE_INDEX_OPERATION not in gateway.PUBLIC_OPERATIONS
    finally:
        app.close()


def test_related_answer_holds_only_the_issues_that_concern_one_issue(issue_app):
    app, _bare, _tmp = issue_app
    full = _payload(app, "ref=main")
    related = _payload(app, "ref=main&related=12")
    assert related["related"] == 12 and related["commit"] == full["commit"]
    rows = {item["number"]: item for item in related["issues"]}
    assert sorted(rows) == [3, 4, 10]
    assert rows[10]["dependsOn"] == [12]
    assert all(ref[0] == 12 for item in rows.values()
               for ref in item["mentionRefs"])
    assert "participants" not in rows[10] and "mentionHandles" not in rows[10]
    assert len(json.dumps(related)) < len(json.dumps(full)) / 2
    for number in (3, 10, 12):
        answer = _payload(app, "ref=main&related=%d" % number)
        expected = [ref for item in app.repositories[
            ("alice", "project")]._issue_index_tree[1]["issues"]
            if item["number"] != number
            for ref in item["mentionRefs"] if ref[0] == number]
        assert [ref for item in answer["issues"]
                for ref in item["mentionRefs"]] == expected
    assert _read(app, "ref=main&related=0").status == 404
    assert _read(app, "ref=main&related=x").status == 404


def _search(app, query):
    return app.dispatch(
        "GET", "/v1/public/repositories/alice/project/issue-search?" + query,
        {}, b"")


def test_search_reads_titles_bodies_and_live_comments(issue_app):
    app, _bare, _tmp = issue_app
    found = lambda q: json.loads(_search(app, "ref=main&q=" + q).body)["numbers"]
    assert found("dave") == [12]
    assert found("again") == []
    assert found("COMMENTED") == [10]
    assert found("%23") == [12, 10, 9, 8, 4, 3]
    assert _search(app, "ref=main&q=%20").status == 404
    assert _search(app, "ref=main&q=" + "x" * 201).status == 404
    assert _search(app, "ref=main&q=ping").headers.get(
        "X-ForkMesh-Memory-Cache") in (None, "miss")
    assert _search(app, "ref=main&q=ping").headers.get(
        "X-ForkMesh-Memory-Cache") == "hit"


def test_index_is_sent_as_utf8_not_escapes(tmp_path):
    bare = _make_repository(tmp_path, {
        ".forkmesh/issues/open/1/issue-1.json": {
            "number": 1, "title": "Блокировка в Иране", "events": []}})
    app = _application(tmp_path, bare)
    try:
        body = _read(app, "ref=main").body
        assert "Блокировка".encode() in body and b"\\u" not in body
    finally:
        app.close()
