import os
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))

import forkmesh_safe_delete as safe  # noqa: E402

SHELL_HELPER = REPO / "tools" / "forkmesh-safe-rm.sh"
LAYOUT = ("src", "data", "config", "ssh", "state", "logs")


@pytest.fixture
def box(tmp_path, monkeypatch):
    home = tmp_path / "home"
    work = tmp_path / "work"
    home.mkdir(exist_ok=True)
    work.mkdir(exist_ok=True)
    monkeypatch.setenv("HOME", str(home))
    for name in ("FORKMESH_HOME", "FORKMESH_PROJECTS_DIR", "FORKMESH_DATA_DIR", "FORKMESH_CONFIG_DIR",
                 "FORKMESH_PROJECT_DIR", "XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME"):
        monkeypatch.delenv(name, raising=False)
    project = home / "projects" / "forkmesh"
    for sub in LAYOUT:
        (project / sub).mkdir(parents=True, exist_ok=True)
    (home / "projects" / "forkmesh-backup" / "data").mkdir(parents=True, exist_ok=True)
    (home / "projects" / "other" / "data").mkdir(parents=True, exist_ok=True)
    return tmp_path.resolve(), home.resolve(), work.resolve(), project.resolve()


def refused(target, parent, **kwargs):
    return bool(safe.refusal(target, parent, **kwargs))


def test_refuses_empty_root_home_and_its_ancestors(box):
    root, home, work, _ = box
    assert refused("", work)
    assert refused("   ", work)
    assert refused("/", "/")
    assert refused(home, root)
    assert refused(root, root.parent)
    assert refused(home.parent, home.parent.parent)


def test_refuses_without_or_outside_the_expected_parent(box):
    _, _, work, _ = box
    (work / "scratch").mkdir()
    assert refused(work / "scratch", "")
    assert refused(work / "scratch", "/")
    assert refused(work / "scratch", work / "elsewhere")
    assert refused(work, work)
    assert refused(str(work / "scratch" / ".." / ".." / "work"), work)
    assert not refused(work / "scratch", work)


def test_refuses_checkouts_and_git_history(box):
    _, _, work, _ = box
    checkout = work / "checkout"
    (checkout / ".git" / "objects").mkdir(parents=True)
    assert refused(checkout, work)
    assert refused(checkout, work, worktree=True)
    assert refused(checkout / ".git", checkout)
    assert refused(checkout / ".git" / "objects", checkout / ".git")
    assert refused(checkout / ".git" / "refs", checkout / ".git")


def test_linked_worktree_needs_the_worktree_flag(box):
    _, _, work, _ = box
    linked = work / "linked"
    linked.mkdir()
    (linked / ".git").write_text("gitdir: /elsewhere/.git/worktrees/linked\n")
    assert refused(linked, work)
    assert not refused(linked, work, worktree=True)
    assert safe.remove_tree(linked, work, worktree=True)
    assert not linked.exists()


def test_refuses_the_projects_layout(box):
    _, home, _, project = box
    projects = home / "projects"
    assert refused(projects, home)
    assert refused(project, projects)
    assert refused(projects / "other", projects)
    for sub in LAYOUT:
        assert refused(project / sub, project), sub
    assert refused(project / "src" / "desktop", project / "src")
    assert refused(project / "ssh" / "fleet", project / "ssh")
    assert refused(project / "data" / "identity", project / "data")
    assert refused(project / "notes", project)
    assert refused(projects / "forkmesh-backup" / "data" / "x", projects / "forkmesh-backup" / "data")


def test_allows_caches_builds_and_nested_data(box):
    _, _, _, project = box
    assert not refused(project / "src" / "desktop" / "build", project / "src" / "desktop")
    assert not refused(project / "src" / ".claude" / "worktrees" / "agent-x", project / "src" / ".claude" / "worktrees")
    assert not refused(project / "data" / "mirrors" / "repo", project / "data" / "mirrors")
    assert not refused(project / "builds" / "x", project / "builds")
    assert not refused(project / "worktrees" / "x", project / "worktrees")
    assert not refused(project / "toolchain-linux" / "x", project / "toolchain-linux")
    issues = project / "src" / ".forkmesh" / "issues"
    assert not refused(issues / "7", issues)
    assert refused(project / "src" / ".forkmesh", project / "src")


def test_refuses_legacy_and_environment_roots(box, monkeypatch):
    root, home, _, _ = box
    assert refused(home / ".forkmesh", home)
    assert refused(home / ".config" / "ForkMesh", home / ".config")
    assert refused(home / ".local" / "share" / "ForkMesh", home / ".local" / "share")
    assert refused(home / ".ssh", home)
    custom = root / "custom"
    (custom / "data" / "x").mkdir(parents=True)
    monkeypatch.setenv("FORKMESH_HOME", str(custom))
    assert refused(custom, root)
    assert refused(custom / "data", custom)
    monkeypatch.setenv("FORKMESH_DATA_DIR", str(root / "elsewhere-data"))
    assert refused(root / "elsewhere-data", root)


def test_remove_tree_raises_and_keeps_refused_targets(box):
    _, _, work, _ = box
    checkout = work / "checkout"
    (checkout / ".git").mkdir(parents=True)
    with pytest.raises(safe.RefusedDelete):
        safe.remove_tree(checkout, work)
    with pytest.raises(safe.RefusedDelete):
        safe.remove_tree(checkout, work, ignore_errors=True)
    assert (checkout / ".git").is_dir()
    scratch = work / "scratch"
    (scratch / "a").mkdir(parents=True)
    (scratch / "a" / "f").write_text("x")
    assert safe.remove_tree(scratch, work)
    assert not scratch.exists()
    assert safe.remove_tree(work / "missing", work) is False


def test_remove_trees_checks_every_target_before_deleting(box):
    _, _, work, _ = box
    first = work / "first"
    first.mkdir()
    checkout = work / "checkout"
    (checkout / ".git").mkdir(parents=True)
    with pytest.raises(safe.RefusedDelete):
        safe.remove_trees([first, checkout], work)
    assert first.exists()


def test_symlink_is_unlinked_not_followed(box):
    _, _, work, _ = box
    precious = work / "precious"
    precious.mkdir()
    (precious / "keep").write_text("x")
    link = work / "cache-link"
    link.symlink_to(precious)
    assert safe.remove_tree(link, work)
    assert (precious / "keep").exists()


def test_move_aside_keeps_everything(box):
    _, _, work, _ = box
    checkout = work / "checkout"
    (checkout / ".git").mkdir(parents=True)
    moved = safe.move_aside(checkout)
    assert not checkout.exists()
    assert (moved / ".git").is_dir()
    assert moved.name.startswith("checkout.replaced-")
    with pytest.raises(safe.RefusedDelete):
        safe.move_aside("")


def _store(home):
    store = home / "projects" / "forkmesh_data"
    for slug in ("forkmesh", "side"):
        for sub in ("data", "config", "ssh", "state", "logs", "cache"):
            (store / slug / sub).mkdir(parents=True, exist_ok=True)
    return store.resolve()


def test_refuses_the_forkmesh_data_store(box, monkeypatch):
    root, home, _, _ = box
    store = _store(home)
    assert refused(store, home / "projects")
    assert refused(store / "forkmesh", store)
    assert refused(store / "side", store)
    for sub in ("data", "config", "ssh", "state", "logs"):
        assert refused(store / "forkmesh" / sub, store / "forkmesh"), sub
    assert refused(store / "side" / "data" / "identity", store / "side" / "data")
    assert refused(store / "forkmesh" / "ssh" / "fleet", store / "forkmesh" / "ssh")
    (store / "forkmesh" / "data" / "mirrors" / "repo").mkdir(parents=True)
    assert not refused(store / "forkmesh" / "data" / "mirrors" / "repo",
                       store / "forkmesh" / "data" / "mirrors")
    assert not refused(store / "forkmesh" / "cache", store / "forkmesh")
    custom = root / "sandbox" / "forkmesh"
    custom.mkdir(parents=True)
    (root / "sandbox" / "forkmesh_data" / "forkmesh" / "data").mkdir(parents=True)
    monkeypatch.setenv("FORKMESH_HOME", str(custom))
    assert refused(root / "sandbox" / "forkmesh_data" / "forkmesh" / "data",
                   root / "sandbox" / "forkmesh_data" / "forkmesh")


def _shell(box, parent, *targets, worktree=False):
    root, home, _, _ = box
    env = {"HOME": str(home), "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
           "XDG_DATA_HOME": str(home / ".local" / "share"), "XDG_CONFIG_HOME": str(home / ".config"),
           "XDG_CACHE_HOME": str(home / ".cache")}
    if worktree:
        env["FM_SAFE_RM_WORKTREE"] = "1"
    script = f'. "{SHELL_HELPER}"; fm_safe_rm_tree "$@"'
    return subprocess.run(["bash", "-c", script, "fm", str(parent), *map(str, targets)],
                          env=env, capture_output=True, text=True, timeout=30, cwd=root)


def test_shell_helper_refusals(box):
    root, home, work, project = box
    checkout = work / "checkout"
    (checkout / ".git").mkdir(parents=True)
    linked = work / "linked"
    linked.mkdir()
    (linked / ".git").write_text("gitdir: x\n")
    (project / "data" / "identity").mkdir()
    cases = [
        (work, [""]),
        ("", [work / "x"]),
        ("/", [work]),
        (root, [home]),
        (work / "other", [checkout]),
        (work, [checkout]),
        (checkout, [checkout / ".git"]),
        (work, [linked]),
        (home / "projects", [project]),
        (project, [project / "src"]),
        (project / "src", [project / "src" / "desktop"]),
        (project / "data", [project / "data" / "identity"]),
        (home, [home / "projects"]),
    ]
    for parent, targets in cases:
        for target in targets:
            if target and not Path(target).exists():
                Path(target).mkdir(parents=True, exist_ok=True)
        result = _shell(box, parent, *targets)
        assert result.returncode != 0, (parent, targets, result.stderr)
        assert "Refusing" in result.stderr, (parent, targets)
    assert checkout.joinpath(".git").is_dir()
    assert project.joinpath("src").is_dir()


def test_shell_helper_deletes_allowed_targets(box):
    _, _, work, project = box
    scratch = work / "scratch"
    (scratch / "a").mkdir(parents=True)
    build = project / "src" / "desktop" / "build"
    build.mkdir(parents=True)
    nested = project / "data" / "mirrors" / "repo"
    nested.mkdir(parents=True)
    linked = work / "linked"
    linked.mkdir()
    (linked / ".git").write_text("gitdir: x\n")
    assert _shell(box, work, scratch, work / "missing").returncode == 0
    assert _shell(box, project / "src" / "desktop", build).returncode == 0
    assert _shell(box, project / "data" / "mirrors", nested).returncode == 0
    issue = project / "src" / ".forkmesh" / "issues" / "7"
    issue.mkdir(parents=True)
    assert _shell(box, issue.parent, issue).returncode == 0
    assert _shell(box, project / "src", project / "src" / ".forkmesh").returncode != 0
    assert not issue.exists() and (project / "src" / ".forkmesh").is_dir()
    assert _shell(box, work, linked, worktree=True).returncode == 0
    assert not scratch.exists() and not build.exists() and not nested.exists() and not linked.exists()


def test_shell_helper_guards_the_forkmesh_data_store(box):
    _, home, _, _ = box
    store = _store(home)
    (store / "forkmesh" / "data" / "identity").mkdir()
    for parent, target in [
        (home / "projects", store),
        (store, store / "forkmesh"),
        (store / "forkmesh", store / "forkmesh" / "data"),
        (store / "forkmesh", store / "forkmesh" / "ssh"),
        (store / "side", store / "side" / "config"),
        (store / "forkmesh" / "data", store / "forkmesh" / "data" / "identity"),
    ]:
        result = _shell(box, parent, target)
        assert result.returncode != 0 and "Refusing" in result.stderr, target
        assert target.is_dir()
    mirror = store / "forkmesh" / "data" / "mirrors" / "repo"
    mirror.mkdir(parents=True)
    assert _shell(box, mirror.parent, mirror).returncode == 0
    assert _shell(box, store / "side", store / "side" / "cache").returncode == 0
    assert not mirror.exists() and not (store / "side" / "cache").exists()
