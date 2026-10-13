import os
import subprocess
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
SHORTCUT = ROOT / ".forkmesh/shortcuts/scrub-term.sh"
TERM = "Zorblax"


def _env(tmp_path: Path, **extra: str) -> dict[str, str]:
    env = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith(("FORKMESH_", "GIT_", "SCRUB_"))
    }
    home = tmp_path / "home"
    home.mkdir(exist_ok=True)
    env.update(
        HOME=str(home),
        XDG_DATA_HOME=str(home / ".local/share"),
        XDG_CONFIG_HOME=str(home / ".config"),
        XDG_CACHE_HOME=str(home / ".cache"),
        GIT_CONFIG_NOSYSTEM="1",
        GIT_AUTHOR_NAME="Tester",
        GIT_AUTHOR_EMAIL="tester@example.com",
        GIT_COMMITTER_NAME="Tester",
        GIT_COMMITTER_EMAIL="tester@example.com",
    )
    env.update(extra)
    return env


def _git(repo: Path, env: dict[str, str], *args: str) -> str:
    return subprocess.run(
        ["git", "-C", str(repo), *args], check=True, capture_output=True, text=True, env=env
    ).stdout


def _repo(tmp_path: Path) -> tuple[Path, dict[str, str]]:
    repo = tmp_path / "repo"
    repo.mkdir()
    env = _env(tmp_path)
    _git(repo, env, "init", "-q", "-b", "main")
    (repo / "notes.txt").write_text(f"hello {TERM} world\n", encoding="utf-8")
    (repo / "clean.txt").write_text("nothing here\n", encoding="utf-8")
    (repo / "blob.bin").write_bytes(b"\0zorblax\0")
    _git(repo, env, "add", "-A")
    _git(repo, env, "commit", "-q", "-m", f"add notes about {TERM.lower()}")
    author_env = dict(env, GIT_AUTHOR_NAME=f"{TERM} Person", GIT_AUTHOR_EMAIL="zorblax@example.com")
    (repo / "notes.txt").write_text("hello world\n", encoding="utf-8")
    _git(repo, author_env, "commit", "-q", "-am", "drop it")
    _git(repo, env, "branch", "side", "HEAD~1")
    _git(repo, env, "tag", "-a", "v1", "-m", f"tag {TERM}", "HEAD~1")
    _git(repo, env, "update-ref", "refs/forkmesh/trash/old", "HEAD~1")
    (repo / "later.txt").write_text(f"{TERM.upper()} again\n", encoding="utf-8")
    _git(repo, env, "add", "later.txt")
    _git(repo, env, "commit", "-q", "-m", "later")
    return repo, env


def _objects_with_term(repo: Path, env: dict[str, str]) -> int:
    listed = _git(repo, env, "cat-file", "--batch-all-objects", "--batch-check=%(objectname) %(objecttype)")
    ids = "".join(
        line.split()[0] + "\n" for line in listed.splitlines() if line.split()[1] != "tree"
    )
    dump = subprocess.run(
        ["git", "-C", str(repo), "cat-file", "--batch"],
        input=ids.encode(),
        check=True,
        capture_output=True,
        env=env,
    ).stdout
    return sum(1 for line in dump.split(b"\n") if TERM.lower().encode() in line.lower())


def _run(repo: Path, env: dict[str, str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["bash", str(SHORTCUT)], cwd=repo, capture_output=True, text=True, env=env
    )


def test_scrubs_source_history_messages_authors_and_tags(tmp_path: Path) -> None:
    repo, env = _repo(tmp_path)
    assert _objects_with_term(repo, env) > 0
    backups = tmp_path / "backups"

    result = _run(repo, dict(env, SCRUB_TERM=TERM, SCRUB_BACKUP_DIR=str(backups)))

    assert result.returncode == 0, result.stdout + result.stderr
    assert (repo / "later.txt").read_text(encoding="utf-8") == "[redacted] again\n"
    assert _git(repo, env, "status", "--porcelain") == ""
    assert _objects_with_term(repo, env) == 1
    assert (repo / "blob.bin").read_bytes() == b"\0zorblax\0"
    assert "binary file still holds it" in result.stdout
    assert "[redacted] Person" in _git(repo, env, "log", "--all", "--format=%an <%ae>")
    assert "[redacted]@example.com" in _git(repo, env, "log", "--all", "--format=%ae")
    assert "about [redacted]" in _git(repo, env, "log", "side", "--format=%s")
    assert "tag [redacted]" in _git(repo, env, "tag", "-n", "v1")
    assert "hello [redacted] world" in _git(repo, env, "show", "refs/forkmesh/trash/old:notes.txt")
    assert len(list(backups.glob("repo-*.bundle"))) == 1
    assert _git(repo, env, "log", "-1", "--format=%s") == "Scrub a term from the source\n"


def test_source_only_leaves_history(tmp_path: Path) -> None:
    repo, env = _repo(tmp_path)
    before = _git(repo, env, "rev-parse", "side")

    result = _run(repo, dict(env, SCRUB_TERM=TERM, SCRUB_HISTORY="0"))

    assert result.returncode == 0, result.stdout + result.stderr
    assert "History: left alone" in result.stdout
    assert _git(repo, env, "rev-parse", "side") == before
    assert (repo / "later.txt").read_text(encoding="utf-8") == "[redacted] again\n"


def test_keeps_stash_untouched(tmp_path: Path) -> None:
    repo, env = _repo(tmp_path)
    (repo / "clean.txt").write_text(f"stashed {TERM}\n", encoding="utf-8")
    _git(repo, env, "stash", "push", "-q", "-m", "keep me")
    stash = _git(repo, env, "rev-parse", "refs/stash")

    result = _run(repo, dict(env, SCRUB_TERM=TERM, SCRUB_BACKUP_DIR=str(tmp_path / "b")))

    assert result.returncode == 0, result.stdout + result.stderr
    assert _git(repo, env, "rev-parse", "refs/stash") == stash
    assert "refs/stash was left alone" in result.stdout


@pytest.mark.parametrize(
    ("extra", "message"),
    [
        ({}, "SCRUB_TERM is not set"),
        ({"SCRUB_TERM": "ab"}, "shorter than 3"),
        ({"SCRUB_TERM": TERM, "SCRUB_REPLACEMENT": f"x{TERM}"}, "contains the term"),
        ({"SCRUB_TERM": "a<b"}, "cannot hold"),
    ],
)
def test_refuses_bad_input(tmp_path: Path, extra: dict[str, str], message: str) -> None:
    repo, env = _repo(tmp_path)
    head = _git(repo, env, "rev-parse", "HEAD")

    result = _run(repo, dict(env, **extra))

    assert result.returncode == 2
    assert message in result.stderr
    assert _git(repo, env, "rev-parse", "HEAD") == head


def test_refuses_uncommitted_changes(tmp_path: Path) -> None:
    repo, env = _repo(tmp_path)
    (repo / "clean.txt").write_text("edited\n", encoding="utf-8")

    result = _run(repo, dict(env, SCRUB_TERM=TERM))

    assert result.returncode == 2
    assert "uncommitted changes" in result.stderr
    assert (repo / "clean.txt").read_text(encoding="utf-8") == "edited\n"
