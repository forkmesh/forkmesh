import os
import pwd
import subprocess
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from forkmesh_host import instance_repo as repo  # noqa: E402


def args(tmp_path, **changes):
    values = {"owner": "Alice", "organization": "acme", "name": "Bolt",
              "description": "  An   import ", "visibility": "public",
              "source": str(tmp_path), "hosted_root": "/var/lib/x/hosted",
              "force": False, "no_hooks": False, "origin": "",
              "app_user": None, "git_user": None}
    values.update(changes)
    return SimpleNamespace(**values)


def test_spec_is_normalized_and_bad_values_are_refused(tmp_path):
    spec = repo.read_spec(args(tmp_path))
    assert spec["owner"] == "alice" and spec["name"] == "Bolt"
    assert spec["description"] == "An import" and spec["hooks"] is True
    assert repo.read_spec(args(tmp_path, organization=""))["organization"] == ""
    for bad in ({"owner": "bad name"}, {"organization": "-x"},
                {"name": ".hidden"}, {"name": "x.git"}, {"name": "a/b"},
                {"source": str(tmp_path / "missing")},
                {"hosted_root": "relative"}):
        with pytest.raises(repo.RepoError):
            repo.read_spec(args(tmp_path, **bad))


def test_fork_and_switch_options_are_normalized(tmp_path):
    spec = repo.read_spec(args(tmp_path))
    assert spec["forkOf"] == "" and spec["protect"] is None
    assert spec["actions"] is None
    spec = repo.read_spec(args(tmp_path, fork_of="Acme/ForkMesh",
                               protect_default_branch=True, actions="off"))
    assert spec["forkOf"] == "acme/forkmesh" and spec["protect"] is True
    assert spec["actions"] is False
    assert repo.read_spec(args(tmp_path, actions="on"))["actions"] is True
    assert repo.read_spec(args(
        tmp_path, protect_default_branch=False))["protect"] is False
    for bad in ("acme", "acme/x/y", "-a/b", "acme/Platform", "alice/platform"):
        with pytest.raises(repo.RepoError):
            repo.read_spec(args(tmp_path, name="Platform", fork_of=bad))


def test_shared_history_is_found_by_commit_or_root(tmp_path):
    work = tmp_path / "work"
    work.mkdir()
    git(work, "init", "-q", "-b", "main")
    first = commit(work, "a", 10)
    second = commit(work, "b", 10)
    local = repo.Git()
    assert repo.shares_history(local, None, {"commits": [second]}, cwd=work)
    assert repo.shares_history(local, None, {"commits": ["f" * 40, first]},
                               cwd=work)
    assert not repo.shares_history(local, None, {"commits": ["f" * 40]},
                                   cwd=work)


@pytest.mark.skipif(os.geteuid() == 0, reason="checks the non-root path")
def test_switching_users_needs_root(tmp_path):
    me = pwd.getpwuid(os.geteuid()).pw_name
    assert repo._users(args(tmp_path)) == ["", ""]
    assert repo._users(args(tmp_path, app_user=me, git_user="")) == ["", ""]
    with pytest.raises(repo.RepoError):
        repo._users(args(tmp_path, app_user="root"))


def test_forked_phase_returns_its_value_or_its_error():
    me = pwd.getpwuid(os.geteuid()).pw_name
    assert repo._forked(me, lambda: {"pid": os.getpid()})["pid"] != os.getpid()

    def fail():
        raise repo.RepoError("broken")
    with pytest.raises(repo.RepoError, match="broken"):
        repo._forked(me, fail)


REPO_ID = "hr" + "0123456789abcdef01234567"


def git(cwd, *argv):
    return subprocess.run(
        ["git", *argv], cwd=str(cwd), check=True, capture_output=True,
        text=True, env=dict(os.environ, GIT_AUTHOR_NAME="T",
                            GIT_AUTHOR_EMAIL="t@example.test",
                            GIT_COMMITTER_NAME="T",
                            GIT_COMMITTER_EMAIL="t@example.test"),
    ).stdout.strip()


def commit(work, name, size):
    (work / name).write_bytes(os.urandom(size))
    git(work, "add", ".")
    git(work, "commit", "-q", "-m", name)
    return git(work, "rev-parse", "HEAD")


@pytest.fixture
def hosted(tmp_path):
    work = tmp_path / "source"
    work.mkdir()
    git(work, "init", "-q", "-b", "main")
    commit(work, "big.bin", 2 << 20)
    git(work, "tag", "-a", "v1", "-m", "v1")
    root = tmp_path / "hosted"
    root.mkdir()
    return work, root


def spec_for(source, root, force=False):
    return {"root": str(root), "source": str(source), "force": force}


def pack_files(git_dir):
    return {path.name: path.stat().st_size
            for path in (git_dir / "objects" / "pack").glob("pack-*.pack")}


def leftovers(root):
    return sorted(path.name for path in root.iterdir()
                  if path.name != REPO_ID + ".git")


def test_fill_transfers_only_the_missing_objects(hosted):
    work, root = hosted
    git_dir = root / (REPO_ID + ".git")
    first = repo.fill(spec_for(work, root), {"id": REPO_ID}, 64 << 20)
    assert first["branches"] == ["main"] and first["tags"] == 1
    assert git(git_dir, "rev-parse", "main") == git(work, "rev-parse", "main")
    packs = pack_files(git_dir)
    assert sum(packs.values()) > 2 << 20
    assert (git_dir / repo.MAINTENANCE_MARKER).is_file()

    (git_dir / repo.MAINTENANCE_MARKER).unlink()
    again = repo.fill(spec_for(work, root), {"id": REPO_ID}, 64 << 20)
    assert again["updates"] == [] and pack_files(git_dir) == packs
    assert not (git_dir / repo.MAINTENANCE_MARKER).exists()

    new = commit(work, "small.txt", 64)
    moved = repo.fill(spec_for(work, root), {"id": REPO_ID}, 64 << 20)
    assert [u["ref"] for u in moved["updates"]] == ["refs/heads/main"]
    assert git(git_dir, "rev-parse", "main") == new
    added = {name: size for name, size in pack_files(git_dir).items()
             if name not in packs}
    assert len(added) == 1 and sum(added.values()) < 64 << 10
    assert moved["sizeBytes"] < sum(pack_files(git_dir).values()) + (1 << 20)
    git(git_dir, "fsck", "--no-dangling")
    assert leftovers(root) == []


def test_a_full_bundle_adds_only_what_the_repository_lacks(hosted, tmp_path):
    work, root = hosted
    git_dir = root / (REPO_ID + ".git")
    repo.fill(spec_for(work, root), {"id": REPO_ID}, 64 << 20)
    packs = pack_files(git_dir)
    new = commit(work, "next.txt", 128)
    bundle = tmp_path / "all.bundle"
    git(work, "bundle", "create", "-q", str(bundle), "--branches", "--tags")
    repo.fill(spec_for(bundle, root), {"id": REPO_ID}, 64 << 20)
    assert git(git_dir, "rev-parse", "main") == new
    added = {name: size for name, size in pack_files(git_dir).items()
             if name not in packs}
    assert len(added) == 1 and sum(added.values()) < 64 << 10
    git(git_dir, "fsck", "--no-dangling")


def test_a_refused_fill_leaves_the_repository_as_it_was(hosted):
    work, root = hosted
    git_dir = root / (REPO_ID + ".git")
    repo.fill(spec_for(work, root), {"id": REPO_ID}, 4 << 20)
    packs = pack_files(git_dir)
    before = git(git_dir, "for-each-ref")
    commit(work, "huge.bin", 3 << 20)
    with pytest.raises(repo.RepoError, match="HOSTED_REPO_MAX_BYTES"):
        repo.fill(spec_for(work, root), {"id": REPO_ID}, 4 << 20)
    assert pack_files(git_dir) == packs
    assert git(git_dir, "for-each-ref") == before
    assert leftovers(root) == []
    assert not list((git_dir / "objects").glob("[0-9a-f][0-9a-f]"))

    git(work, "reset", "-q", "--hard", "HEAD~1")
    commit(work, "small.txt", 32)
    repo.fill(spec_for(work, root), {"id": REPO_ID}, 4 << 20)
    before = git(git_dir, "for-each-ref")
    git(work, "reset", "-q", "--hard", "HEAD~1")
    commit(work, "other.txt", 32)
    with pytest.raises(repo.RepoError, match="--force"):
        repo.fill(spec_for(work, root), {"id": REPO_ID}, 64 << 20)
    assert git(git_dir, "for-each-ref") == before
    assert leftovers(root) == []
    forced = repo.fill(spec_for(work, root, force=True), {"id": REPO_ID},
                       64 << 20)
    assert forced["commit"] == git(work, "rev-parse", "main")


def test_stored_bytes_count_each_reachable_object_once(hosted, tmp_path):
    work, root = hosted
    git_dir = root / (REPO_ID + ".git")
    repo.fill(spec_for(work, root), {"id": REPO_ID}, 64 << 20)
    once = repo._stored_bytes(repo.Git(), git_dir)
    subprocess.run(
        ["git", "--git-dir=" + str(git_dir), "pack-objects", "-q", "--all",
         "--revs", "--no-reuse-object", "--compression=0",
         str(git_dir / "objects" / "pack" / "pack")],
        input=b"", check=True, capture_output=True)
    assert len(pack_files(git_dir)) == 2
    assert repo._stored_bytes(repo.Git(), git_dir) <= once * 1.2


def test_the_root_path_bundles_only_commits_the_repository_lacks(
        hosted, monkeypatch):
    class OwnedGit(repo.Git):
        def __init__(self, user=""):
            super().__init__("")
            self.ids = (os.getuid(), os.getgid())

    work, root = hosted
    git_dir = root / (REPO_ID + ".git")
    monkeypatch.setattr(repo, "Git", OwnedGit)
    bundled = []
    original = repo._stage_source

    def watch(git_, spec, scratch, current):
        source, known = original(git_, spec, scratch, current)
        bundled.append(Path(source).stat().st_size if source else 0)
        return source, known

    monkeypatch.setattr(repo, "_stage_source", watch)
    repo.fill(spec_for(work, root), {"id": REPO_ID}, 64 << 20)
    assert bundled[-1] > 2 << 20
    packs = pack_files(git_dir)

    new = commit(work, "small.txt", 64)
    repo.fill(spec_for(work, root), {"id": REPO_ID}, 64 << 20)
    assert 0 < bundled[-1] < 64 << 10
    assert git(git_dir, "rev-parse", "main") == new
    assert len(pack_files(git_dir)) == len(packs) + 1

    git(work, "branch", "copy", "main")
    moved = repo.fill(spec_for(work, root), {"id": REPO_ID}, 64 << 20)
    assert bundled[-1] == 0
    assert [u["ref"] for u in moved["updates"]] == ["refs/heads/copy"]
    assert git(git_dir, "rev-parse", "copy") == new
    assert leftovers(root) == []
    git(git_dir, "fsck", "--no-dangling")


def dated_commit(work, name, when, *extra):
    (work / name).write_text(name)
    git(work, "add", ".")
    env = dict(os.environ, GIT_AUTHOR_NAME="T", GIT_AUTHOR_EMAIL="t@e.test",
               GIT_COMMITTER_NAME="T", GIT_COMMITTER_EMAIL="t@e.test",
               GIT_AUTHOR_DATE="@%d +0000" % when,
               GIT_COMMITTER_DATE="@%d +0000" % when)
    subprocess.run(["git", "commit", "-q", "-m", name, *extra],
                   cwd=str(work), check=True, capture_output=True, env=env)


def test_history_stats_count_the_whole_history_and_bucket_weeks(tmp_path):
    work = tmp_path / "source"
    work.mkdir()
    git(work, "init", "-q", "-b", "main")
    now = 1_800_000_000
    week = repo.WEEK_SECONDS
    dated_commit(work, "ancient", now - 400 * week)
    dated_commit(work, "early", now - 51 * week - 60)
    git(work, "checkout", "-q", "-b", "side")
    dated_commit(work, "side", now - 2 * 86400)
    git(work, "checkout", "-q", "main")
    dated_commit(work, "late", now - 86400)
    env = dict(os.environ, GIT_AUTHOR_NAME="T", GIT_AUTHOR_EMAIL="t@e.test",
               GIT_COMMITTER_NAME="T", GIT_COMMITTER_EMAIL="t@e.test",
               GIT_AUTHOR_DATE="@%d +0000" % (now - 60),
               GIT_COMMITTER_DATE="@%d +0000" % (now - 60))
    subprocess.run(["git", "merge", "-q", "--no-ff", "-m", "merge", "side"],
                   cwd=str(work), check=True, capture_output=True, env=env)
    head = git(work, "rev-parse", "HEAD")
    stats = repo.history_stats(repo.Git(), work / ".git", head, now=now)
    assert stats["commitCount"] == 5
    assert stats["mergeCount"] == 1
    assert len(stats["activityWeeks"]) == 52
    assert stats["activityWeeks"][0] == 1
    assert stats["activityWeeks"][-1] == 3
    assert sum(stats["activityWeeks"]) == 4
    assert stats["mergeActivityWeeks"][-1] == 1
    empty = repo.history_stats(repo.Git(), work / ".git", "", now=now)
    assert empty["commitCount"] == 0 and sum(empty["activityWeeks"]) == 0


def test_fill_reports_the_whole_history_and_its_activity(hosted):
    work, root = hosted
    filled = repo.fill(spec_for(work, root), {"id": REPO_ID}, 64 << 20)
    assert filled["commitCount"] == 1
    assert filled["mergeCount"] == 0
    assert sum(filled["activityWeeks"]) == 1
    assert len(filled["mergeActivityWeeks"]) == 52


def test_the_total_commit_count_is_never_capped():
    calls = []

    class Recorder:
        def run(self, _git_dir, *argv, check=True):
            calls.append(argv)
            return SimpleNamespace(stdout=b"45204\n")

    assert repo._commit_count(Recorder(), "x", "", "a" * 40, limit=0) == 45204
    assert not any(arg.startswith("--max-count") for arg in calls[-1])
    repo._commit_count(Recorder(), "x", "b" * 40, "a" * 40)
    assert "--max-count=%d" % repo.COMMIT_COUNT_LIMIT in calls[-1]
