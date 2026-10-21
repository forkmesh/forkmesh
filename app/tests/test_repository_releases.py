"""Releases and tags from the web (repository_releases.py), against real git."""

import asyncio
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT / "src") not in sys.path:
    sys.path.insert(0, str(ROOT / "src"))

import repository_file_edits  # noqa: E402
import repository_releases  # noqa: E402
from test_repository_file_edits import (  # noqa: E402,F401
    FakeRequest, bare_repo, git, git_out, harness)

SHARED = ("json_response", "method_name", "bounded_json_request",
          "_authed_account_name", "_audit_sensitive_action",
          "_public_base_url")


def releases_harness(bare, **kwargs):
    calls = harness(bare, **kwargs)
    for name in SHARED:
        setattr(repository_releases, name, getattr(repository_file_edits, name))
    notified = []

    async def notify(env, owner, repo, release):
        notified.append(release)
    repository_releases.notify_release_published = notify
    calls["notified"] = notified
    return calls


def release(body):
    return asyncio.run(repository_releases.route(
        object(), FakeRequest(body=body), "alice", "project", "release-edit"))


def manifest(bare, tag):
    return json.loads(git_out(
        bare, "show", "main:.forkmesh/releases/%s/release.json" % tag))


def test_a_release_creates_its_tag_and_notes(bare_repo):
    calls = releases_harness(bare_repo)
    head = git_out(bare_repo, "rev-parse", "main").strip()
    result = release({"op": "save", "tag": "v1.0.0", "branch": "main",
                      "title": "First release", "notes": "## Changes\n- all",
                      "prerelease": True})
    assert result["status"] == 200, result
    assert result["data"]["createdTag"] is True
    assert git_out(bare_repo, "rev-parse", "refs/tags/v1.0.0").strip() == head
    record = manifest(bare_repo, "v1.0.0")
    assert record["name"] == "First release" and record["prerelease"] is True
    assert record["tag_commit"] == head and record["body"] == "## Changes\n- all"
    assert record["source"] == "web" and record["assets"] == []
    assert calls["notified"][0]["tag"] == "v1.0.0"
    git(bare_repo, "fsck", "--strict", "--no-dangling")


def test_editing_keeps_the_tag_and_creation_time(bare_repo):
    releases_harness(bare_repo)
    release({"op": "save", "tag": "v1.0.0", "branch": "main", "title": "A"})
    first = manifest(bare_repo, "v1.0.0")
    tag = git_out(bare_repo, "rev-parse", "refs/tags/v1.0.0").strip()
    edited = release({"op": "save", "tag": "v1.0.0", "branch": "main",
                      "title": "B", "notes": "fixed"})
    assert edited["status"] == 200, edited
    assert "createdTag" not in edited["data"]
    record = manifest(bare_repo, "v1.0.0")
    assert record["name"] == "B" and record["body"] == "fixed"
    assert record["created_at"] == first["created_at"] and record["updated_at"]
    assert git_out(bare_repo, "rev-parse", "refs/tags/v1.0.0").strip() == tag


def test_a_tag_on_its_own_and_at_a_named_commit(bare_repo):
    releases_harness(bare_repo)
    first = git_out(bare_repo, "rev-list", "--max-parents=0", "main").strip()
    tagged = release({"op": "tag", "tag": "seed", "branch": "main",
                      "target": first})
    assert tagged["status"] == 200, tagged
    assert git_out(bare_repo, "rev-parse", "refs/tags/seed").strip() == first
    again = release({"op": "tag", "tag": "seed", "branch": "main"})
    assert again["status"] == 409


def test_deleting_a_release_removes_its_notes_but_keeps_the_tag(bare_repo):
    releases_harness(bare_repo)
    release({"op": "save", "tag": "v2", "branch": "main", "title": "Two"})
    gone = release({"op": "delete", "tag": "v2", "branch": "main"})
    assert gone["status"] == 200, gone
    assert git(bare_repo, "cat-file", "-e",
               "main:.forkmesh/releases/v2/release.json",
               check=False).returncode != 0
    assert git_out(bare_repo, "rev-parse", "refs/tags/v2").strip()
    assert release({"op": "delete", "tag": "v2", "branch": "main"})[
        "status"] == 404


def test_signed_and_reserved_channels_are_never_written(bare_repo, tmp_path):
    releases_harness(bare_repo)
    work = tmp_path / "signed"
    git(tmp_path, "clone", "-q", str(bare_repo), str(work))
    folder = work / ".forkmesh" / "releases" / "v9"
    folder.mkdir(parents=True)
    (folder / "release.json").write_text('{"tag": "v9"}\n')
    (folder / "release.json.sig").write_text("signature\n")
    git(work, "add", "-A")
    git(work, "commit", "-q", "-m", "signed")
    git(work, "tag", "v9")
    git(work, "push", "-q", "origin", "main", "v9")
    signed = release({"op": "save", "tag": "v9", "branch": "main", "title": "x"})
    assert signed["status"] == 409 and signed["data"]["error"] == "signed_release"
    for name in ("latest", "candidate-1.2.3"):
        refused = release({"op": "save", "tag": name, "branch": "main"})
        assert refused["data"]["error"] == "reserved_tag"


def test_only_writers_manage_releases(bare_repo):
    releases_harness(bare_repo, owner_write=False)
    refused = release({"op": "save", "tag": "v1", "branch": "main"})
    assert refused["status"] == 403
    releases_harness(bare_repo, access=False, owner_write=False)
    assert release({"op": "save", "tag": "v1", "branch": "main"})[
        "status"] == 404
    releases_harness(bare_repo)
    for body in ({"op": "save", "tag": "../x", "branch": "main"},
                 {"op": "rename", "tag": "v1", "branch": "main"},
                 {"op": "save", "tag": "v1", "branch": "main",
                  "notes": "x" * (128 * 1024 + 1)}):
        assert release(body)["status"] in (400, 413)
