#!/usr/bin/env python3
"""GitHub <-> ForkMesh bridge contracts (tools/github_forkmesh_bridge.py)."""

import base64
import hashlib
import json
from pathlib import Path
import subprocess
import sys

import pytest

cryptography = pytest.importorskip("cryptography")
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import github_forkmesh_bridge as bridge  # noqa: E402


REPO = "source/project"


def _b64(text):
    return base64.urlsafe_b64decode(text + "=" * (-len(text) % 4))


def verified(author, sig, message):
    try:
        Ed25519PublicKey.from_public_bytes(_b64(author)).verify(_b64(sig), message)
        return True
    except Exception:
        return False


def canonical(domain, kind, number, author, ts, content):
    digest = hashlib.sha256(content.encode()).hexdigest()
    return f"{domain}\n{kind}\n{number}\n{author}\n{ts}\n{digest}".encode()


def git(repo, *args):
    return subprocess.run(["git", "-C", str(repo), *args], check=True,
                          capture_output=True, text=True).stdout.strip()


@pytest.fixture(autouse=True)
def git_identity(monkeypatch, tmp_path):
    monkeypatch.setenv("GIT_CONFIG_GLOBAL", str(tmp_path / "gitconfig"))
    monkeypatch.setenv("GIT_CONFIG_NOSYSTEM", "1")
    for key in ("AUTHOR", "COMMITTER"):
        monkeypatch.setenv(f"GIT_{key}_NAME", "Test")
        monkeypatch.setenv(f"GIT_{key}_EMAIL", "test@example.invalid")


@pytest.fixture
def signer():
    return bridge.BridgeSigner.generate()


def gh_issue(number=7, **overrides):
    issue = {
        "id": 9000 + number, "number": number, "title": "Fix routing",
        "body": "Line one\r\nLine two\r\n", "state": "open",
        "created_at": "2026-07-01T10:00:00Z", "updated_at": "2026-07-02T11:00:00Z",
        "closed_at": None, "user": {"login": "alice"},
        "labels": [{"name": "bug", "color": "d73a4a"}],
        "assignees": [{"login": "bob"}], "milestone": {"title": "Beta", "state": "open"},
        "comments": 1,
    }
    issue.update(overrides)
    return issue


class FakeGitHub:
    def __init__(self, *, issues=(), comments=None, pulls=(), reviews=None,
                 review_comments=None, discussions=(), default_branch="main"):
        self.issues = list(issues)
        self.comments = comments or {}
        self.pulls = list(pulls)
        self.reviews = reviews or {}
        self.review_comments = review_comments or {}
        self.discussions = list(discussions)
        self.default_branch = default_branch
        self.calls = []

    def paginate(self, path, query=None):
        self.calls.append(path)
        prefix = bridge.repo_path(REPO)
        rest = path[len(prefix):]
        if rest == "/issues":
            return iter(self.issues)
        if rest == "/pulls":
            return iter(self.pulls)
        parts = rest.strip("/").split("/")
        number = int(parts[1])
        if parts[0] == "issues":
            return iter(self.comments.get(number, []))
        if parts[2] == "reviews":
            return iter(self.reviews.get(number, []))
        return iter(self.review_comments.get(number, []))

    def get(self, path, query=None):
        return {"default_branch": self.default_branch}

    def graphql(self, query, variables):
        return {"repository": {"hasDiscussionsEnabled": True, "discussions": {
            "pageInfo": {"hasNextPage": False}, "nodes": self.discussions}}}


def issue_path(root, side, number):
    return root / ".forkmesh" / "issues" / side / str(number) / f"issue-{number}.json"


# ---- signing parity ---------------------------------------------------------


def test_canonical_strings_verify_records_signed_by_the_desktop():
    """The bridge's content functions reproduce desktop-made signatures."""
    discussion = ROOT / ".forkmesh" / "discussions" / "2"
    for name in ("discussion.md", "0002-comment.md"):
        values, body = bridge.parse_front_matter(
            (discussion / name).read_text(encoding="utf-8"))
        values = dict(values)
        event = {"type": values["type"], "body": body,
                 "title": values.get("title", ""), "category": values.get("category", "")}
        assert verified(values["author"], values["sig"], canonical(
            "forkmesh-discussion-event-v1", values["type"], 2, values["author"],
            values["ts"], bridge.discussion_event_content(event)))

    record = json.loads(
        (ROOT / ".forkmesh/issues/closed/126/issue-126.json").read_text(encoding="utf-8"))
    opener = record["events"][0]
    assert verified(opener["author"], opener["sig"], canonical(
        "forkmesh-issue-event-v1", "open", 126, opener["author"], opener["ts"],
        bridge.issue_event_content(opener)))


def test_signing_key_round_trips_through_pem_and_seed(signer):
    assert bridge.BridgeSigner.from_text(signer.pem()).public_key == signer.public_key
    with pytest.raises(bridge.BridgeError):
        bridge.BridgeSigner.from_text("not a key")


# ---- issues -----------------------------------------------------------------


def test_issue_export_writes_strict_signed_records(tmp_path, signer):
    client = FakeGitHub(issues=[gh_issue(), {"number": 8, "pull_request": {}}],
                        comments={7: [{"id": 55, "body": "Seen it", "created_at":
                                       "2026-07-01T12:00:00Z", "user": {"login": "carol"}}]})
    report = bridge.Exporter(client, signer, github_repository=REPO, checkout=tmp_path,
                             kinds=["issues"]).run()

    assert report.counts["issues"]["created"] == 1
    record = json.loads(issue_path(tmp_path, "open", 7).read_text(encoding="utf-8"))
    assert record["schema"] == "forkmesh-issue-v1" and record["number"] == 7
    assert record["labels"] == ["bug"] and record["milestone"] == "Beta"
    assert record["assignees"] == ["github:bob"]
    assert record["authorName"] == "github:alice"
    assert [ev["type"] for ev in record["events"]] == [
        "open", "labels", "milestone", "assignees", "comment"]
    assert record["events"][0]["body"] == "Line one\nLine two"
    for ev in record["events"]:
        # Every field the desktop's strictIssueObjectValid requires.
        assert ev["author"] == signer.public_key and ev["sig"] and isinstance(ev["ts"], int)
        assert verified(ev["author"], ev["sig"], canonical(
            "forkmesh-issue-event-v1", ev["type"], 7, ev["author"], ev["ts"],
            bridge.issue_event_content(ev)))
    labels = json.loads((tmp_path / ".forkmesh/issues/labels.json").read_text())
    assert labels == [{"name": "bug", "color": "#d73a4a"}]
    config = json.loads((tmp_path / bridge.CONFIG_REL).read_text())
    assert config["issues"]["7"] == {"number": 7, "updatedAt": "2026-07-02T11:00:00Z"}
    assert config["signer"] == signer.public_key


def test_unchanged_issue_is_skipped_and_regeneration_is_byte_identical(tmp_path, signer):
    client = FakeGitHub(issues=[gh_issue(comments=0)])
    bridge.Exporter(client, signer, github_repository=REPO, checkout=tmp_path,
                    kinds=["issues"]).run()
    path = issue_path(tmp_path, "open", 7)
    first = path.read_bytes()

    client.calls.clear()
    report = bridge.Exporter(client, signer, github_repository=REPO, checkout=tmp_path,
                             kinds=["issues"]).run()
    assert report.counts["issues"]["unchanged"] == 1
    assert client.calls == [bridge.repo_path(REPO) + "/issues"]

    # Forget the cache: the rebuilt record is still byte-identical.
    config = json.loads((tmp_path / bridge.CONFIG_REL).read_text())
    config["issues"]["7"].pop("updatedAt")
    (tmp_path / bridge.CONFIG_REL).write_text(json.dumps(config))
    report = bridge.Exporter(client, signer, github_repository=REPO, checkout=tmp_path,
                             kinds=["issues"]).run()
    assert report.counts["issues"]["unchanged"] == 1
    assert path.read_bytes() == first


def test_closing_moves_the_folder_and_native_events_survive(tmp_path, signer):
    client = FakeGitHub(issues=[gh_issue(comments=0)])
    bridge.Exporter(client, signer, github_repository=REPO, checkout=tmp_path,
                    kinds=["issues"]).run()
    path = issue_path(tmp_path, "open", 7)
    record = json.loads(path.read_text())
    native = {"type": "comment", "id": "3c2d-native", "author": "nativeKey",
              "authorName": "jett", "ts": 1782950000000, "body": "from ForkMesh",
              "attachments": [], "sig": "nativeSig"}
    record["events"].append(native)
    record["priority"] = 42
    path.write_text(json.dumps(record))

    client.issues = [gh_issue(comments=0, state="closed", closed_at="2026-07-05T00:00:00Z",
                              updated_at="2026-07-05T00:00:00Z")]
    bridge.Exporter(client, signer, github_repository=REPO, checkout=tmp_path,
                    kinds=["issues"]).run()

    assert not path.parent.exists()
    closed = json.loads(issue_path(tmp_path, "closed", 7).read_text())
    assert closed["status"] == "closed" and closed["priority"] == 42
    assert native in closed["events"]
    assert closed["events"][-1]["type"] == "status"


def test_github_number_owned_by_a_native_issue_is_remapped(tmp_path, signer):
    native = issue_path(tmp_path, "open", 7)
    native.parent.mkdir(parents=True)
    native.write_text('{"schema": "forkmesh-issue-v1", "number": 7, "events": []}')
    client = FakeGitHub(issues=[gh_issue(comments=0), gh_issue(number=9, comments=0)])
    report = bridge.Exporter(client, signer, github_repository=REPO, checkout=tmp_path,
                             kinds=["issues"]).run()

    assert json.loads(native.read_text())["events"] == []
    remapped = json.loads(issue_path(tmp_path, "open", 10).read_text())
    assert remapped["number"] == 10 and remapped["title"] == "Fix routing"
    assert issue_path(tmp_path, "open", 9).is_file()
    assert any("stored as #10" in warning for warning in report.warnings)


# ---- discussions ------------------------------------------------------------


def test_discussions_become_signed_markdown_with_flat_comments(tmp_path, signer):
    discussion = {
        "id": "D_1", "number": 3, "title": "Roadmap\nthoughts", "body": "What next?",
        "createdAt": "2026-07-01T10:00:00Z", "updatedAt": "2026-07-01T10:00:00Z",
        "closed": True, "author": {"login": "alice"}, "category": {"name": "Ideas"},
        "comments": {"pageInfo": {"hasNextPage": False}, "nodes": [{
            "databaseId": 11, "body": "More mirrors", "createdAt": "2026-07-01T11:00:00Z",
            "author": {"login": "bob"}, "replies": {"nodes": [{
                "databaseId": 12, "body": "Agreed", "createdAt": "2026-07-01T12:00:00Z",
                "author": None}]}}]},
    }
    client = FakeGitHub(discussions=[discussion])
    directory = tmp_path / ".forkmesh/discussions/3"
    bridge.Exporter(client, signer, github_repository=REPO, checkout=tmp_path,
                    kinds=["discussions"]).run()
    # A ForkMesh user replies on the imported discussion, newest of all.
    (directory / "0009-comment.md").write_text(
        "---\ntype: comment\nid: native-1\nauthor: k\nauthorName: jett\n"
        "ts: 1783000000000\nsig: s\n---\n\nnative\n")
    report = bridge.Exporter(client, signer, github_repository=REPO, checkout=tmp_path,
                             kinds=["discussions"]).run()
    assert report.counts["discussions"]["updated"] == 1  # renumbered only

    values, body = bridge.parse_front_matter((directory / "discussion.md").read_text())
    values = dict(values)
    assert values["title"] == "Roadmap thoughts" and values["status"] == "closed"
    assert verified(values["author"], values["sig"], canonical(
        "forkmesh-discussion-event-v1", "open", 3, values["author"], values["ts"],
        f"Roadmap thoughts\x00{body}\x00Ideas"))
    names = sorted(p.name for p in directory.iterdir() if p.name != "discussion.md")
    assert names == ["0002-comment.md", "0003-comment.md", "0004-comment.md"]
    texts = [(directory / name).read_text() for name in names]
    assert "native" in texts[2]  # ordered by ts; the native reply is newest
    for text in texts[:2]:
        pairs, comment_body = bridge.parse_front_matter(text)
        v = dict(pairs)
        assert verified(v["author"], v["sig"], canonical(
            "forkmesh-discussion-event-v1", "comment", 3, v["author"], v["ts"],
            comment_body))
    assert "authorName: github:ghost" in texts[1]


# ---- git fixtures -----------------------------------------------------------


def make_remotes(tmp_path):
    github = tmp_path / "github.git"
    forkmesh = tmp_path / "forkmesh.git"
    for bare in (github, forkmesh):
        subprocess.run(["git", "init", "-q", "--bare", "-b", "main", str(bare)], check=True)
    seed = tmp_path / "seed"
    subprocess.run(["git", "init", "-q", "-b", "main", str(seed)], check=True)
    (seed / "README.md").write_text("hello\n")
    git(seed, "add", ".")
    git(seed, "commit", "-qm", "init")
    for bare in (github, forkmesh):
        git(seed, "push", "-q", str(bare), "main")
    return github, forkmesh, seed


def commit_file(repo, name, text, message):
    (repo / name).write_text(text)
    git(repo, "add", name)
    git(repo, "commit", "-qm", message)
    return git(repo, "rev-parse", "HEAD")


def test_git_sync_creates_and_fast_forwards_both_ways_and_reports_divergence(tmp_path):
    github, forkmesh, seed = make_remotes(tmp_path)
    ahead = commit_file(seed, "a.txt", "a", "github ahead")
    git(seed, "push", "-q", str(github), "main")
    git(seed, "tag", "v1")
    git(seed, "push", "-q", str(forkmesh), "HEAD:refs/heads/feature", "v1")
    git(seed, "checkout", "-qb", "left", "main~1")
    left = commit_file(seed, "l.txt", "l", "left")
    git(seed, "push", "-q", str(github), "left")
    git(seed, "checkout", "-q", "main~1")
    commit_file(seed, "r.txt", "r", "right")
    git(seed, "push", "-q", str(forkmesh), "HEAD:refs/heads/left")

    checkout = tmp_path / "checkout"
    subprocess.run(["git", "clone", "-q", str(github), str(checkout)], check=True)
    report = bridge.sync_refs(checkout, "origin", str(forkmesh))

    assert {"ref": "refs/heads/main", "oid": ahead} in report.to_forkmesh
    assert {"ref": "refs/heads/feature", "oid": ahead} in report.to_github
    assert any(e["ref"] == "refs/tags/v1" for e in report.to_github)
    assert report.diverged == ["refs/heads/left"]
    assert git(forkmesh, "rev-parse", "main") == ahead
    assert git(github, "rev-parse", "feature") == ahead
    assert git(github, "rev-parse", "left") == left  # never forced

    again = bridge.sync_refs(checkout, "origin", str(forkmesh))
    assert again.to_github == [] and again.to_forkmesh == []


def test_run_imports_records_and_publishes_to_both_remotes(tmp_path, signer):
    github, forkmesh, seed = make_remotes(tmp_path)
    git(seed, "checkout", "-qb", "topic")
    head = commit_file(seed, "feature.txt", "feature\n", "Add feature")
    git(seed, "push", "-q", str(github), "topic", "HEAD:refs/pull/5/head")
    base = git(seed, "rev-parse", "main")
    # A ForkMesh-side commit GitHub has not seen yet.
    git(seed, "checkout", "-q", "main")
    native = commit_file(seed, "native.txt", "n", "native ForkMesh change")
    git(seed, "push", "-q", str(forkmesh), "main")

    checkout = tmp_path / "checkout"
    subprocess.run(["git", "clone", "-q", str(github), str(checkout)], check=True)
    client = FakeGitHub(
        issues=[gh_issue(comments=0)],
        pulls=[{"number": 5, "title": "Add feature", "body": "Please merge", "state": "open",
                "merged_at": None, "created_at": "2026-07-03T00:00:00Z",
                "updated_at": "2026-07-03T00:00:00Z", "user": {"login": "dana"},
                "base": {"ref": "main", "sha": base},
                "head": {"ref": "topic", "sha": head, "label": "source:topic",
                         "repo": {"full_name": REPO}}}],
        reviews={5: [{"id": 1, "state": "APPROVED", "body": "LGTM",
                      "submitted_at": "2026-07-03T01:00:00Z", "user": {"login": "erin"}}]},
        review_comments={5: [{"id": 2, "body": "nit", "path": "feature.txt", "line": 1,
                              "side": "RIGHT", "created_at": "2026-07-03T02:00:00Z",
                              "user": {"login": "erin"}}]})

    result = bridge.run_sync(repo=checkout, github_repository=REPO,
                             forkmesh_remote=str(forkmesh), client=client, signer=signer,
                             logger=lambda _: None)

    assert {"ref": "refs/heads/main", "oid": native} in result["refs"]["toGitHub"]
    main_tip = git(github, "rev-parse", "main")
    assert main_tip == git(forkmesh, "rev-parse", "main")
    assert git(github, "rev-parse", "main~1") == native
    assert "issue-7.json" in git(github, "ls-tree", "-r", "--name-only", "main")

    ledger = git(forkmesh, "rev-parse", "forkmesh/pulls")
    assert ledger == git(github, "rev-parse", "forkmesh/pulls")
    files = git(forkmesh, "ls-tree", "-r", "--name-only", ledger, ".forkmesh/pulls/5").split()
    assert files == [".forkmesh/pulls/5/0002-review.md", ".forkmesh/pulls/5/0003-line-comment.md",
                     ".forkmesh/pulls/5/changes.patch", ".forkmesh/pulls/5/commits.mbox",
                     ".forkmesh/pulls/5/pull.md"]
    show = lambda name: subprocess.run(
        ["git", "-C", str(forkmesh), "show", f"{ledger}:.forkmesh/pulls/5/{name}"],
        check=True, capture_output=True).stdout
    values, _ = bridge.parse_front_matter(show("pull.md").decode())
    values = dict(values)
    assert values["head"] == "topic" and values["status"] == "open"
    patch, commits = show("changes.patch"), show("commits.mbox")
    assert b"+feature" in patch and b"Subject: [PATCH] Add feature" in commits
    content = b"\x00".join([b"Add feature", b"main", b"topic", patch, commits])
    assert verified(values["author"], values["sig"],
                    b"forkmesh-pull-event-v1\n" + values["author"].encode() + b"\n"
                    + values["ts"].encode() + b"\n"
                    + hashlib.sha256(content).hexdigest().encode())
    review = dict(bridge.parse_front_matter(show("0002-review.md").decode())[0])
    assert review["state"] == "approved"
    line = dict(bridge.parse_front_matter(show("0003-line-comment.md").decode())[0])
    assert line["side"] == "new" and line["line"] == "1"

    # A second run with nothing new publishes nothing.
    again = bridge.run_sync(repo=checkout, github_repository=REPO,
                            forkmesh_remote=str(forkmesh), client=client, signer=signer,
                            logger=lambda _: None)
    assert again["published"] == {}
    assert git(github, "rev-parse", "main") == main_tip


# ---- connect & workflow -----------------------------------------------------


def test_connect_scaffolds_config_script_workflow_remotes_and_key(tmp_path):
    repo = tmp_path / "repo"
    subprocess.run(["git", "init", "-q", "-b", "main", str(repo)], check=True)
    key = tmp_path / "keys" / "bridge.pem"
    result = bridge.connect(repo=repo, github_repository="octo/app",
                            forkmesh_repository="mesh/app", key_out=key)

    config = json.loads((repo / bridge.CONFIG_REL).read_text())
    assert config["github"] == "octo/app"
    assert config["forkmeshRemote"] == "ssh://git@ssh.forkmesh.com/mesh/app.git"
    assert (repo / bridge.DEFAULT_SCRIPT_REL).read_bytes() == Path(bridge.__file__).read_bytes()
    assert (repo / bridge.WORKFLOW_REL).read_text() == bridge.render_workflow()
    assert git(repo, "remote", "get-url", "github") == "https://github.com/octo/app.git"
    assert oct(key.stat().st_mode & 0o777) == "0o600"
    assert result["bridgePublicKey"] == bridge.BridgeSigner.from_text(key.read_text()).public_key

    # Re-running keeps the key and the mapping state.
    config["issues"] = {"1": {"number": 1}}
    (repo / bridge.CONFIG_REL).write_text(json.dumps(config))
    again = bridge.connect(repo=repo, github_repository="octo/app",
                           forkmesh_repository="mesh/app", key_out=key)
    assert again["bridgePublicKey"] == result["bridgePublicKey"]
    assert json.loads((repo / bridge.CONFIG_REL).read_text())["issues"] == {"1": {"number": 1}}


def test_checked_in_workflow_is_the_rendered_template():
    workflow = ROOT / ".github/workflows/forkmesh-github-sync.yml"
    assert workflow.read_text(encoding="utf-8") == bridge.render_workflow(
        "tools/github_forkmesh_bridge.py")


def test_workflow_passes_untrusted_values_only_through_the_environment():
    text = bridge.render_workflow()
    for line in text.splitlines():
        stripped = line.strip()
        if "${{" in stripped:
            # Expressions only in `with:`/`env:` values or the job condition,
            # never interpolated into a shell script.
            assert stripped.split(":", 1)[0] in {
                "ref", "SSH_KEY", "KNOWN_HOSTS", "GITHUB_TOKEN",
                "FORKMESH_BRIDGE_SIGNING_KEY", "FORKMESH_SYNC_REMOTE",
                "DEFAULT_BRANCH", "DRY_RUN"}, line
    assert "uses: actions/checkout@" in text and "persist-credentials: false" not in text
    assert "--force" not in text
