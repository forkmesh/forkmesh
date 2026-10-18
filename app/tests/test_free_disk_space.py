import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools/free_disk_space.py"
SHORTCUT = ROOT / ".forkmesh/shortcuts/free-disk-space.sh"


def _env(home: Path) -> dict[str, str]:
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("FORKMESH_", "GIT_"))}
    env.update(HOME=str(home), XDG_DATA_HOME=str(home / ".local/share"),
               XDG_CONFIG_HOME=str(home / ".config"), XDG_CACHE_HOME=str(home / ".cache"),
               GIT_CONFIG_NOSYSTEM="1")
    return env


def _old(path: Path) -> Path:
    stamp = time.time() - 3 * 3600
    for root, dirs, files in os.walk(path):
        for name in dirs + files:
            os.utime(os.path.join(root, name), (stamp, stamp), follow_symlinks=False)
    os.utime(path, (stamp, stamp))
    return path


def _layout(tmp_path: Path) -> dict[str, Path]:
    home = tmp_path / "home"
    repo = home / "projects/forkmesh/src"
    tmp = tmp_path / "tmp"
    env = _env(home)
    repo.mkdir(parents=True)
    tmp.mkdir()
    git = ["git", "-C", str(repo), "-c", "user.name=T", "-c", "user.email=t@example.com"]
    subprocess.run(["git", "init", "-q", "-b", "main", str(repo)], check=True, env=env)
    subprocess.run(git + ["commit", "-q", "--allow-empty", "-m", "init"], check=True, env=env)
    subprocess.run(git + ["worktree", "add", "-q", "-b", "live", str(repo / ".worktrees/1500-live")],
                   check=True, env=env)
    paths = {
        "home": home, "repo": repo, "tmp": tmp,
        "orphan_scratch": repo / ".worktrees/.scratch/1400-gone",
        "live_scratch": repo / ".worktrees/.scratch/1500-live",
        "recent_scratch": repo / ".worktrees/.scratch/1401-recent",
        "orphan_build": home / "projects/forkmesh/builds/agent-1400",
        "live_build": home / "projects/forkmesh/builds/agent-1500-tmp",
        "named_build": home / "projects/forkmesh/builds/agent-salvage",
        "orphan_tmp": tmp / "fm1400-swift",
        "sandbox": home / ".local/share/ForkMesh/ForkMesh/actions/sandbox/forkmesh-run-7-1",
        "actions_cache": home / ".local/share/ForkMesh/ForkMesh/actions/cache",
        "old_claude": home / ".local/share/claude/versions/2.1.1",
        "new_claude": home / ".local/share/claude/versions/2.1.10",
        "unrelated": home / ".cache/other",
    }
    for key in ("orphan_scratch", "live_scratch", "orphan_build", "live_build", "named_build",
                "orphan_tmp", "sandbox", "actions_cache", "unrelated"):
        paths[key].mkdir(parents=True)
        (paths[key] / "file.o").write_bytes(b"x" * 4096)
        _old(paths[key])
    paths["recent_scratch"].mkdir(parents=True)
    (paths["recent_scratch"] / "file.o").write_bytes(b"x")
    for key in ("old_claude", "new_claude"):
        paths[key].parent.mkdir(parents=True, exist_ok=True)
        paths[key].write_bytes(b"bin")
        _old(paths[key])
    backups = home / "Backups/forkmesh"
    backups.mkdir(parents=True)
    for hour in range(5):
        (backups / f"forkmesh-fleet-20261015-0{hour}0000.tar.gz.age").write_bytes(b"a")
        (backups / f"forkmesh-minimal-20261015-0{hour}0000.tar.gz").write_bytes(b"m")
    (backups / "history-rewrite-2026-10-13").mkdir()
    _old(backups)
    paths["backups"] = backups
    return paths


def _run(paths: dict[str, Path], *extra: str, command: list[str] | None = None) -> str:
    base = command or [sys.executable, str(TOOL)]
    result = subprocess.run(
        base + ["--repo", str(paths["repo"]), "--home", str(paths["home"]), "--tmp", str(paths["tmp"]),
                "--no-caches", *extra],
        check=True, capture_output=True, text=True, env=_env(paths["home"]))
    return result.stdout


def test_dry_run_deletes_nothing(tmp_path: Path) -> None:
    paths = _layout(tmp_path)
    output = _run(paths)
    assert "Dry run" in output
    assert str(paths["orphan_scratch"]) in output
    assert paths["orphan_scratch"].exists()
    assert paths["orphan_build"].exists()


def test_execute_removes_only_orphaned_idle_debris(tmp_path: Path) -> None:
    paths = _layout(tmp_path)
    _run(paths, "--execute")
    for key in ("orphan_scratch", "orphan_build", "orphan_tmp", "sandbox", "old_claude"):
        assert not paths[key].exists(), key
    for key in ("live_scratch", "recent_scratch", "live_build", "named_build", "actions_cache",
                "new_claude", "unrelated", "repo"):
        assert paths[key].exists(), key
    assert (paths["repo"] / ".worktrees/1500-live/.git").exists()
    remaining = sorted(path.name for path in paths["backups"].iterdir())
    assert remaining == [
        "forkmesh-fleet-20261015-020000.tar.gz.age",
        "forkmesh-fleet-20261015-030000.tar.gz.age",
        "forkmesh-fleet-20261015-040000.tar.gz.age",
        "forkmesh-minimal-20261015-020000.tar.gz",
        "forkmesh-minimal-20261015-030000.tar.gz",
        "forkmesh-minimal-20261015-040000.tar.gz",
        "history-rewrite-2026-10-13",
    ]


def test_shortcut_executes_by_default(tmp_path: Path) -> None:
    paths = _layout(tmp_path)
    output = _run(paths, command=["bash", str(SHORTCUT)])
    assert "Freed" in output
    assert not paths["orphan_scratch"].exists()
    assert paths["live_scratch"].exists()


def test_step_removes_only_its_group(tmp_path: Path) -> None:
    paths = _layout(tmp_path)
    output = _run(paths, "--execute", "--step", "agents")
    for key in ("orphan_scratch", "orphan_build", "orphan_tmp"):
        assert not paths[key].exists(), key
    for key in ("sandbox", "old_claude", "live_scratch"):
        assert paths[key].exists(), key
    assert len(list(paths["backups"].iterdir())) == 11
    assert "Before:" not in output and "After:" not in output
    _run(paths, "--execute", "--step", "tests")
    assert not paths["sandbox"].exists()
    assert paths["old_claude"].exists()
    _run(paths, "--execute", "--step", "versions")
    assert not paths["old_claude"].exists()
    assert paths["new_claude"].exists()
    assert len(list(paths["backups"].iterdir())) == 7


def test_usage_and_report_steps_delete_nothing(tmp_path: Path) -> None:
    paths = _layout(tmp_path)
    usage = _run(paths, "--execute", "--step", "usage")
    report = _run(paths, "--execute", "--step", "report")
    assert usage.startswith("Before: /")
    assert "Keeping 2 registered worktrees" in usage
    assert report.startswith("After: /")
    assert paths["orphan_scratch"].exists()
    assert paths["old_claude"].exists()


def test_shortcut_announces_every_step_it_declares(tmp_path: Path) -> None:
    paths = _layout(tmp_path)
    declared = [line.split(":", 1)[1].strip() for line in SHORTCUT.read_text().splitlines()
                if line.startswith("# step:")]
    output = _run(paths, "--dry-run", command=["bash", str(SHORTCUT)])
    announced = [line[len("::step::"):] for line in output.splitlines() if line.startswith("::step::")]
    assert len(declared) == 6
    assert announced == declared
    assert paths["orphan_scratch"].exists()
