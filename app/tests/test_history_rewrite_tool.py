import json
import os
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "rewrite_commit_messages.py"


def git(repo, *args, env=None):
    result = subprocess.run(
        ["git", *args],
        cwd=repo,
        check=True,
        capture_output=True,
        text=True,
        env=env,
    )
    return result.stdout.strip()


def commit(repo, filename, value, message, timestamp):
    (repo / filename).write_text(value, encoding="utf-8")
    git(repo, "add", filename)
    env = {
        **os.environ,
        "GIT_AUTHOR_DATE": timestamp,
        "GIT_COMMITTER_DATE": timestamp,
    }
    git(repo, "commit", "-m", message, env=env)
    return git(repo, "rev-parse", "HEAD")


def test_rewrite_is_guarded_recoverable_and_preserves_tree_graph_and_dates(
    tmp_path,
):
    repo = tmp_path / "repository"
    repo.mkdir()
    git(repo, "init", "-b", "main")
    git(repo, "config", "user.name", "Alice Example")
    git(repo, "config", "user.email", "alice@example.org")
    first = commit(
        repo,
        "one.txt",
        "one\n",
        "Alice Example adds the first file\n\nReviewed-by: Bob Person",
        "2025-01-01T12:00:00+00:00",
    )
    second = commit(
        repo,
        "two.txt",
        "two\n",
        "Fix parser with thanks to @bob\n\nLong explanation.",
        "2025-01-02T12:00:00+00:00",
    )
    git(repo, "tag", "-a", "v1", "-m", "signed-looking release", second)
    old_tree = git(repo, "show", "-s", "--format=%T", second)
    old_author = git(repo, "show", "-s", "--format=%an|%ae|%aI|%cI", second)

    refused = subprocess.run(
        [
            sys.executable,
            str(TOOL),
            "--repo",
            str(repo),
            "rewrite",
            "--confirm",
            "NO",
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    assert refused.returncode == 2
    assert git(repo, "rev-parse", "HEAD") == second

    result = subprocess.run(
        [
            sys.executable,
            str(TOOL),
            "--repo",
            str(repo),
            "rewrite",
            "--confirm",
            "REWRITE-COMMIT-HISTORY",
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    artifact = Path(result.stdout.strip())
    new_tip = git(repo, "rev-parse", "HEAD")
    assert new_tip != second
    assert git(repo, "show", "-s", "--format=%T", new_tip) == old_tree
    assert git(repo, "show", "-s", "--format=%an|%ae|%aI|%cI", new_tip) == (
        old_author)
    assert len(git(repo, "rev-list", "--parents", "HEAD").splitlines()) == 2
    messages = git(repo, "log", "--format=%s").splitlines()
    assert all(message.endswith((".", "!", "?")) for message in messages)
    assert all("alice" not in message.lower() for message in messages)
    assert all("@bob" not in message.lower() for message in messages)
    assert git(repo, "rev-parse", "v1^{}") == new_tip

    original = artifact / "original-commit-messages.jsonl"
    assert stat_mode(original) == 0o600
    records = [
        json.loads(line)
        for line in original.read_text(encoding="utf-8").splitlines()
    ]
    assert {item["commit"] for item in records} == {first, second}
    assert any("Long explanation." in item["description"] for item in records)
    assert stat_mode(artifact / "pre-rewrite.bundle") == 0o600
    git(repo, "bundle", "verify", str(artifact / "pre-rewrite.bundle"))

    subprocess.run(
        [
            sys.executable,
            str(TOOL),
            "--repo",
            str(repo),
            "restore",
            "--artifact-dir",
            str(artifact),
            "--confirm",
            "RESTORE-COMMIT-HISTORY",
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    assert git(repo, "rev-parse", "HEAD") == second
    assert git(repo, "rev-parse", "v1^{}") == second


def stat_mode(path):
    return os.stat(path).st_mode & 0o777


def test_plan_is_read_only_and_lists_only_local_heads_and_tags(tmp_path):
    repo = tmp_path / "repository"
    repo.mkdir()
    git(repo, "init", "-b", "main")
    git(repo, "config", "user.name", "Example Contributor")
    git(repo, "config", "user.email", "contributor@example.org")
    tip = commit(
        repo,
        "readme.md",
        "hello\n",
        "Initial project state",
        "2025-01-01T12:00:00+00:00",
    )
    before = git(repo, "show-ref")
    result = subprocess.run(
        [sys.executable, str(TOOL), "--repo", str(repo), "plan"],
        check=True,
        capture_output=True,
        text=True,
    )
    payload = json.loads(result.stdout)
    assert payload["commitCount"] == 1
    assert payload["refs"] == [{
        "backupRef": "",
        "new": "",
        "old": tip,
        "ref": "refs/heads/main",
    }]
    assert git(repo, "show-ref") == before


def test_redaction_placeholder_does_not_collide_with_contributor_identity(
    tmp_path,
):
    repo = tmp_path / "repository"
    repo.mkdir()
    git(repo, "init", "-b", "main")
    git(repo, "config", "user.name", "ForkMesh Contributor")
    git(repo, "config", "user.email", "contributor@example.org")
    commit(
        repo,
        "readme.md",
        "hello\n",
        "Add ability to run @example",
        "2025-01-01T12:00:00+00:00",
    )

    subprocess.run(
        [
            sys.executable,
            str(TOOL),
            "--repo",
            str(repo),
            "rewrite",
            "--confirm",
            "REWRITE-COMMIT-HISTORY",
        ],
        check=True,
        capture_output=True,
        text=True,
    )

    assert git(repo, "log", "-1", "--format=%s") == (
        "Add ability to run [contributor]."
    )
