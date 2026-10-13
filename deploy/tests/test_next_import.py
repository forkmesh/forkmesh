import os
import subprocess

import pytest

import next_import

IDENT = {
    "GIT_AUTHOR_NAME": "Llama",
    "GIT_AUTHOR_EMAIL": "llama@example.com",
    "GIT_COMMITTER_NAME": "ForkMesh Deploy",
    "GIT_COMMITTER_EMAIL": "deploy@example.com",
}


def sh(repo, *args, when="2026-09-23T12:00:00-0400"):
    env = dict(os.environ, **IDENT, GIT_AUTHOR_DATE=when, GIT_COMMITTER_DATE=when)
    return subprocess.run(
        ["git", "-C", str(repo), *args], check=True, capture_output=True, text=True, env=env,
    ).stdout.strip()


def write(repo, rel, text):
    path = repo / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def commit(repo, message, when):
    sh(repo, "add", "-A")
    sh(repo, "commit", "-q", "--no-gpg-sign", "-m", message, when=when)
    return sh(repo, "rev-parse", "HEAD")


def init(path):
    path.mkdir()
    sh(path, "init", "-q", "-b", "main")
    return path


@pytest.fixture
def repos(tmp_path):
    prod = init(tmp_path / "prod")
    write(prod, "app/entry.py", "one\n")
    write(prod, "mobile/app.dart", "flutter\n")
    write(prod, "native/ios.swift", "old swift\n")
    write(prod, ".github/ci.yml", "ci\n")
    base = commit(prod, "production base", "2026-09-23T10:00:00-0400")

    nxt = init(tmp_path / "next")
    write(nxt, "app/entry.py", "one\n")
    write(nxt, "UPSTREAM.md", "seeded\n")
    seed = commit(nxt, "Seed from production", "2026-09-23T11:00:00-0400")
    write(nxt, "app/entry.py", "one\ntwo\n")
    commit(nxt, "app: two", "2026-09-23T12:00:00-0400")
    sh(nxt, "checkout", "-q", "-b", "side")
    write(nxt, "deploy/fleet.toml", "fleet\n")
    commit(nxt, "deploy: fleet", "2026-09-23T13:00:00-0400")
    sh(nxt, "checkout", "-q", "main")
    write(nxt, "native/ios.swift", "new swift\n")
    commit(nxt, "native: import", "2026-09-23T14:00:00-0400")
    sh(nxt, "merge", "-q", "--no-ff", "--no-gpg-sign", "-m", "Merge side", "side",
       when="2026-09-23T15:00:00-0400")
    return prod, nxt, base, seed


def run(prod, nxt, base, seed, **kw):
    return next_import.run(str(prod), str(nxt), "refs/heads/main", seed, base, "next-import", **kw)


def show(repo, rev, path):
    return sh(repo, "show", f"{rev}:{path}")


def test_replays_history_onto_the_base_with_omitted_paths_restored(repos):
    prod, nxt, base, seed = repos
    result = run(prod, nxt, base, seed)
    tip = result["tip"]
    assert result["updated"] and result["commits"] == 4
    assert sh(prod, "rev-parse", "next-import") == tip
    assert sh(prod, "merge-base", "--is-ancestor", base, tip) == ""
    assert show(prod, tip, "mobile/app.dart") == "flutter"
    assert show(prod, tip, ".github/ci.yml") == "ci"
    assert show(prod, tip, "native/ios.swift") == "new swift"
    assert show(prod, tip, "deploy/fleet.toml") == "fleet"
    assert show(prod, tip, "app/entry.py") == "one\ntwo"
    mapped_seed = sh(prod, "rev-list", "--reverse", "--ancestry-path", f"{base}..{tip}").split()[0]
    assert sh(prod, "diff", "--name-only", base, mapped_seed) == "UPSTREAM.md"
    assert sh(prod, "log", "-1", "--format=%s", mapped_seed) == "Seed from production"


def test_keeps_authors_dates_messages_and_merges(repos):
    prod, nxt, base, seed = repos
    tip = run(prod, nxt, base, seed)["tip"]
    fmt = "%an|%ae|%ad|%cn|%cd|%s"
    original = sh(nxt, "log", "--date=raw", f"--format={fmt}", "main").splitlines()
    replayed = sh(prod, "log", "--date=raw", f"--format={fmt}", f"{base}..{tip}").splitlines()
    assert replayed == original
    assert len(sh(prod, "log", "-1", "--format=%P", tip).split()) == 2


def test_rerun_is_deterministic_and_leaves_the_branch_alone(repos):
    prod, nxt, base, seed = repos
    first = run(prod, nxt, base, seed)
    second = run(prod, nxt, base, seed)
    assert second["tip"] == first["tip"]
    assert second["updated"] is False


def test_new_next_commits_fast_forward_the_branch(repos):
    prod, nxt, base, seed = repos
    first = run(prod, nxt, base, seed)["tip"]
    write(nxt, "app/entry.py", "one\ntwo\nthree\n")
    commit(nxt, "app: three", "2026-09-24T09:00:00-0400")
    second = run(prod, nxt, base, seed)
    assert second["previous"] == first and second["updated"]
    assert sh(prod, "rev-list", "--count", f"{first}..{second['tip']}") == "1"
    assert show(prod, second["tip"], "mobile/app.dart") == "flutter"


def test_refuses_to_rewind_a_diverged_branch(repos):
    prod, nxt, base, seed = repos
    sh(prod, "branch", "next-import", base)
    sh(prod, "checkout", "-q", "-b", "scratch")
    write(prod, "x.txt", "x\n")
    diverged = commit(prod, "local work", "2026-09-24T09:00:00-0400")
    sh(prod, "branch", "-f", "next-import", diverged)
    sh(prod, "checkout", "-q", "main")
    with pytest.raises(next_import.ImportFailure, match="not an ancestor"):
        run(prod, nxt, base, seed)
    assert sh(prod, "rev-parse", "next-import") == diverged
    assert run(prod, nxt, base, seed, force=True)["updated"]


def test_refuses_a_checked_out_branch(repos):
    prod, nxt, base, seed = repos
    sh(prod, "checkout", "-q", "-b", "next-import")
    with pytest.raises(next_import.ImportFailure, match="checked out"):
        run(prod, nxt, base, seed)


def test_dry_run_does_not_touch_the_branch(repos):
    prod, nxt, base, seed = repos
    result = run(prod, nxt, base, seed, dry_run=True)
    assert result["updated"] is False
    assert next_import.resolve(prod, "refs/heads/next-import") is None


def test_rejects_a_seed_that_dropped_part_of_a_kept_directory(repos):
    prod, nxt, base, seed = repos
    write(prod, "app/extra.py", "extra\n")
    base2 = commit(prod, "extra", "2026-09-23T10:30:00-0400")
    with pytest.raises(next_import.ImportFailure, match="app/extra.py"):
        run(prod, nxt, base2, seed)


def test_cli_reports_the_tip(repos, capsys):
    prod, nxt, base, seed = repos
    code = next_import.main([
        "--repo", str(prod), "--next-repo", str(nxt), "--seed", seed, "--base", base,
    ])
    out = capsys.readouterr().out
    assert code == 0
    assert "tip      " + sh(prod, "rev-parse", "next-import") in out
    assert "updated" in out
