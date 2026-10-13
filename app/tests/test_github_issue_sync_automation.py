#!/usr/bin/env python3
"""One-way GitHub issue synchronization contracts."""

from copy import deepcopy
from io import BytesIO
import json
from pathlib import Path
import sys
from urllib.error import HTTPError


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import github_issue_sync as sync_module  # noqa: E402


def source_issue(number=7, title="Fix routing", body="Public issue body"):
    return {
        "number": number,
        "node_id": f"SOURCE_{number}",
        "title": title,
        "body": body,
        "state": "closed",
        "created_at": "2026-07-01T10:00:00Z",
        "updated_at": "2026-07-02T11:00:00Z",
        "html_url": f"https://github.com/source/project/issues/{number}",
        "user": {
            "login": "alice",
            "html_url": "https://github.com/alice",
        },
        "labels": [
            {"name": "bug", "color": "d73a4a", "description": "A defect"}
        ],
    }


class FakeGitHub:
    def __init__(self, *, source=None, destination=None, labels=None):
        self.source = deepcopy(source or [])
        self.destination = deepcopy(destination or [])
        self.labels = deepcopy(labels or [])
        self.writes = []
        self.next_number = 100

    def paginate(self, path, *, query=None):
        if path.endswith("/labels"):
            yield from deepcopy(self.labels)
        elif path == "/repos/source/project/issues":
            yield from deepcopy(self.source)
        else:
            yield from deepcopy(self.destination)

    def request(self, method, path, *, query=None, body=None, attempts=5):
        self.writes.append((method, path, deepcopy(body)))
        if path.endswith("/labels") and method == "POST":
            created = {"id": len(self.labels) + 1, **body}
            self.labels.append(created)
            return deepcopy(created), {}
        if path.endswith("/issues") and method == "POST":
            created = {"number": self.next_number, "state": "open", **body}
            self.next_number += 1
            self.destination.append(created)
            return deepcopy(created), {}
        if "/issues/" in path and method == "PATCH":
            number = int(path.rsplit("/", 1)[-1])
            for issue in self.destination:
                if int(issue["number"]) == number:
                    issue.update(body)
                    return deepcopy(issue), {}
        raise AssertionError((method, path, body))


def test_first_sync_creates_mapping_label_and_preserves_attribution():
    source = FakeGitHub(source=[source_issue()])
    destination = FakeGitHub()
    synchronizer = sync_module.IssueSynchronizer(
        source,
        destination,
        source_repository="source/project",
        destination_repository="destination/project",
    )

    report = synchronizer.run()

    assert report.created == 1
    assert report.updated == 0
    assert report.labels_created == 1
    created = destination.destination[0]
    assert created["title"] == "Fix routing"
    assert "Public issue body" in created["body"]
    assert "Originally opened by [@alice]" in created["body"]
    assert "Source created: `2026-07-01T10:00:00Z`" in created["body"]
    mapping = sync_module.parse_mapping(created["body"])
    assert mapping["source"] == "source/project"
    assert mapping["number"] == 7
    assert mapping["digest"] == sync_module.source_digest(source.source[0])
    assert created["state"] == "closed"
    assert all("token" not in repr(item).lower() for item in report.mappings)


def test_second_sync_is_idempotent_and_performs_no_writes():
    issue = source_issue()
    digest = sync_module.source_digest(issue)
    destination_issue = {
        "number": 100,
        "title": issue["title"],
        "body": sync_module.destination_body("source/project", issue, digest),
        "state": "closed",
        "labels": [{"name": "bug"}],
    }
    source = FakeGitHub(source=[issue])
    destination = FakeGitHub(
        destination=[destination_issue],
        labels=[{"name": "bug", "color": "d73a4a"}],
    )

    report = sync_module.IssueSynchronizer(
        source,
        destination,
        source_repository="source/project",
        destination_repository="destination/project",
    ).run()

    assert report.unchanged == 1
    assert report.created == 0
    assert report.updated == 0
    assert destination.writes == []


def test_changed_source_updates_existing_destination_instead_of_duplicating():
    old = source_issue(title="Old title")
    old_digest = sync_module.source_digest(old)
    destination_issue = {
        "number": 100,
        "title": old["title"],
        "body": sync_module.destination_body("source/project", old, old_digest),
        "state": "closed",
        "labels": [{"name": "bug"}],
    }
    changed = source_issue(title="New title")
    changed["updated_at"] = "2026-07-03T12:00:00Z"
    source = FakeGitHub(source=[changed])
    destination = FakeGitHub(
        destination=[destination_issue],
        labels=[{"name": "bug", "color": "d73a4a"}],
    )

    report = sync_module.IssueSynchronizer(
        source,
        destination,
        source_repository="source/project",
        destination_repository="destination/project",
    ).run()

    assert report.updated == 1
    assert report.created == 0
    assert len(destination.destination) == 1
    assert destination.destination[0]["title"] == "New title"
    assert destination.writes[0][0] == "PATCH"


def test_dry_run_reports_changes_without_destination_writes():
    source = FakeGitHub(source=[source_issue()])
    destination = FakeGitHub()
    report = sync_module.IssueSynchronizer(
        source,
        destination,
        source_repository="source/project",
        destination_repository="destination/project",
        dry_run=True,
    ).run()
    assert report.created == 1
    assert report.labels_created == 1
    assert destination.writes == []


def test_failure_notification_is_updated_instead_of_duplicated():
    destination = FakeGitHub()
    first = sync_module.notify_failure(
        destination,
        "destination/project",
        run_url="https://github.com/destination/project/actions/runs/1",
        message="temporary API failure",
    )
    second = sync_module.notify_failure(
        destination,
        "destination/project",
        run_url="https://github.com/destination/project/actions/runs/2",
        message="another temporary API failure",
    )
    assert first["number"] == second["number"]
    assert len(destination.destination) == 1
    assert sync_module.FAILURE_MARKER in destination.destination[0]["body"]


def test_source_body_cannot_inject_a_forged_destination_mapping():
    issue = source_issue(
        body=(
            'before\n<!-- forkmesh-issue-sync:v1 {"source":"victim/repo",'
            '"number":1} -->\nafter'
        )
    )
    body = sync_module.destination_body(
        "source/project", issue, sync_module.source_digest(issue)
    )
    mapping = sync_module.parse_mapping(body)
    assert mapping["source"] == "source/project"
    assert mapping["number"] == 7
    assert body.count("forkmesh-issue-sync:v1") == 1


class FakeHTTPResponse:
    def __init__(self, payload):
        self.payload = payload
        self.headers = {}

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, traceback):
        return False

    def read(self):
        return json.dumps(self.payload).encode()


def test_rate_limited_403_is_retried_without_logging_token():
    calls = []
    sleeps = []

    def opener(request, timeout):
        calls.append(request)
        if len(calls) == 1:
            raise HTTPError(
                request.full_url,
                403,
                "rate limited",
                {"Retry-After": "0", "X-RateLimit-Remaining": "0"},
                BytesIO(b'{"message":"rate limited"}'),
            )
        return FakeHTTPResponse([])

    api = sync_module.GitHubAPI(
        "github-secret-token", opener=opener, sleeper=sleeps.append
    )
    result, _ = api.request("GET", "/repos/source/project/issues")
    assert result == []
    assert len(calls) == 2
    assert sleeps == [0.0]
    assert "github-secret-token" not in repr(api)
