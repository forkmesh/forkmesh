"""Projects from the web (repository_projects.py), against real git."""

import asyncio
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT / "src") not in sys.path:
    sys.path.insert(0, str(ROOT / "src"))

import repository_file_edits  # noqa: E402
import repository_projects  # noqa: E402
from test_event_repo_binding import _Signer, _ed25519_verify  # noqa: E402
from test_repository_file_edits import (  # noqa: E402,F401
    FakeRequest, bare_repo, git, git_out, harness)

SHARED = ("json_response", "method_name", "bounded_json_request",
          "_authed_account_name", "_audit_sensitive_action")
SIGNER = _Signer()


def projects_harness(bare, **kwargs):
    calls = harness(bare, **kwargs)
    for name in SHARED:
        setattr(repository_projects, name, getattr(repository_file_edits, name))
    repository_projects.ed25519_verify = _ed25519_verify
    return calls


def signed(number, ev_type, ts, **fields):
    ev = {"type": ev_type, "id": "%s-web-%d" % (ev_type, ts),
          "author": SIGNER.pub, "authorName": "alice", "ts": ts, **fields}
    ev["sig"] = SIGNER.sign(
        repository_projects.canonical_string(number, ev).decode("utf-8"))
    return ev


def save(number, *events):
    return asyncio.run(repository_projects.route(
        object(), FakeRequest(body={"branch": "main", "number": number,
                                    "events": list(events)}),
        "alice", "project", "project-edit"))


def stored(bare, number):
    return json.loads(git_out(
        bare, "show", "main:" + repository_projects.project_path(number)))


def test_the_canonical_string_matches_project_store():
    ev = {"type": "dates", "author": "k", "ts": 5, "startDate": 1, "endDate": 2}
    digest = hashlib.sha256(b"1\x002").hexdigest()
    assert repository_projects.canonical_string(3, ev) == (
        "forkmesh-project-event-v1\ndates\n3\nk\n5\n" + digest).encode()
    issues = {"type": "issues", "issues": [5, 2, 5]}
    assert repository_projects.content_for_signing(issues) == "2,5"


def test_a_web_project_is_created_with_its_details(bare_repo):
    projects_harness(bare_repo)
    result = save(1,
                  signed(1, "open", 1790000000000, title="Roadmap", body="Q4"),
                  signed(1, "dates", 1790000000001, startDate=1790000000000,
                         endDate=1795000000000),
                  signed(1, "milestone", 1790000000002, milestone="v2"),
                  signed(1, "issues", 1790000000003, issues=[7, 3, 7]))
    assert result["status"] == 200, result
    project = stored(bare_repo, 1)
    assert project["schema"] == "forkmesh-project-v1"
    assert (project["title"], project["body"], project["status"]) == (
        "Roadmap", "Q4", "open")
    assert project["issues"] == [3, 7] and project["milestone"] == "v2"
    assert project["endDate"] == 1795000000000
    assert project["createdAt"] == 1790000000000
    assert project["updatedAt"] == 1790000000003
    assert len(project["events"]) == 4 and project["author"] == SIGNER.pub
    git(bare_repo, "fsck", "--strict", "--no-dangling")


def test_editing_closing_and_deleting_append_signed_events(bare_repo):
    projects_harness(bare_repo)
    save(1, signed(1, "open", 1790000000000, title="Roadmap", body=""))
    assert save(1, signed(1, "title", 1790000000100, title="Roadmap 2"),
                signed(1, "status", 1790000000200, status="closed"))[
        "status"] == 200
    project = stored(bare_repo, 1)
    assert project["title"] == "Roadmap 2" and project["status"] == "closed"
    replay = save(1, signed(1, "title", 1790000000100, title="Roadmap 2"))
    assert replay["status"] == 200
    assert len(stored(bare_repo, 1)["events"]) == 3      # same id: kept once
    assert save(1, signed(1, "delete", 1790000000300, target="self"))[
        "status"] == 200
    assert stored(bare_repo, 1)["events"][-1]["target"] == "self"


def test_new_projects_take_the_next_free_number(bare_repo):
    projects_harness(bare_repo)
    save(1, signed(1, "open", 1790000000000, title="One"))
    taken = save(1, signed(1, "open", 1790000000001, title="Again"))
    assert taken["status"] == 409 and taken["data"]["next"] == 2
    assert save(2, signed(2, "open", 1790000000002, title="Two"))[
        "status"] == 200
    assert stored(bare_repo, 2)["title"] == "Two"


def test_signatures_events_and_permissions_are_checked(bare_repo):
    projects_harness(bare_repo)
    forged = signed(1, "open", 1790000000000, title="Roadmap")
    forged["title"] = "Something else"
    assert save(1, forged)["status"] == 401
    wrong_number = signed(5, "open", 1790000000000, title="Roadmap")
    assert save(1, wrong_number)["status"] == 401
    assert save(1, signed(1, "status", 1, status="frozen"))["status"] == 400
    assert save(1, signed(1, "delete", 1, target="other"))["status"] == 400
    assert save(1, signed(1, "title", 1, title="x"))["status"] == 404
    projects_harness(bare_repo, owner_write=False)
    assert save(1, signed(1, "open", 1, title="x"))["status"] == 403
