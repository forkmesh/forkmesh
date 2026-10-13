import json
import subprocess

import pytest

import prune_merged


def run(cwd, *args):
    subprocess.run(args, cwd=cwd, check=True, capture_output=True)


def sha(repo, ref):
    return subprocess.run(["git", "-C", str(repo), "rev-parse", ref], check=True,
                          capture_output=True, text=True).stdout.strip()


@pytest.fixture
def world(tmp_path, monkeypatch):
    for name, value in (("GIT_AUTHOR_NAME", "t"), ("GIT_AUTHOR_EMAIL", "t@t"),
                        ("GIT_COMMITTER_NAME", "t"), ("GIT_COMMITTER_EMAIL", "t@t")):
        monkeypatch.setenv(name, value)
    repo = tmp_path / "src"
    repo.mkdir()
    run(repo, "git", "init", "-q", "-b", "main")
    run(repo, "git", "commit", "-q", "--allow-empty", "-m", "base")
    for branch in ("done", "dirty", "tasked", "running", "unmerged"):
        run(repo, "git", "worktree", "add", "-q", "-b", branch,
            str(repo / ".worktrees" / branch))
        run(repo / ".worktrees" / branch, "git", "commit", "-q", "--allow-empty",
            "-m", branch)
    for branch in ("done", "dirty", "tasked", "running"):
        run(repo, "git", "merge", "-q", "--no-ff", "-m",
            "Merge %s into main" % branch, branch)
    (repo / ".worktrees" / "dirty" / "wip.txt").write_text("x")
    run(repo, "git", "branch", "stale", "HEAD^2")
    run(repo, "git", "branch", "fresh", "HEAD~1")
    agents = tmp_path / "agents"
    sessions = agents / "sessions" / "forkmesh-forkmesh"
    for number, branch, status in ((1, "done", "success"), (2, "running", "running"),
                                   (3, "gone", "stopped"), (4, "unmerged", "stopped")):
        (sessions / str(number)).mkdir(parents=True)
        (sessions / str(number) / "meta.json").write_text(json.dumps({
            "id": number, "branchName": branch, "status": status,
            "provider": "claude-code", "model": "m", "merged": branch == "done"}))
    tasks = tmp_path / "tasks.json"
    tasks.write_text(json.dumps({"tasks": [
        {"id": "a" * 32, "status": "idle", "title": "t",
         "workItems": [{"repository": "forkmesh/forkmesh", "branch": "tasked"}]},
        {"id": "b" * 32, "status": "done", "title": "old",
         "workItems": [{"repository": "forkmesh/forkmesh", "branch": "stale"}]}]}))
    return repo, agents, tasks


def branches(repo):
    return set(subprocess.run(["git", "-C", str(repo), "for-each-ref",
                               "--format=%(refname:short)", "refs/heads"],
                              check=True, capture_output=True, text=True).stdout.split())


def test_dry_run_changes_nothing(world, capsys):
    repo, agents, tasks = world
    before = branches(repo)
    assert prune_merged.main(["--repo", str(repo), "--agents", str(agents),
                              "--tasks", str(tasks)]) == 0
    assert branches(repo) == before
    assert (agents / "sessions" / "forkmesh-forkmesh" / "1").is_dir()
    assert "dry run" in capsys.readouterr().out


def test_execute_prunes_only_finished_merged_work(world):
    repo, agents, tasks = world
    assert prune_merged.main(["--repo", str(repo), "--agents", str(agents),
                              "--tasks", str(tasks), "--execute"]) == 0
    assert branches(repo) == {"main", "dirty", "tasked", "running", "unmerged", "fresh"}
    assert not (repo / ".worktrees" / "done").exists()
    assert (repo / ".worktrees" / "dirty" / "wip.txt").exists()
    sessions = agents / "sessions" / "forkmesh-forkmesh"
    recycled = agents / "recycle-bin" / "forkmesh-forkmesh"
    assert sorted(p.name for p in sessions.iterdir()) == ["2", "3", "4"]
    assert (recycled / "1" / "deleted-at.json").exists()
    outcomes = json.loads((agents / "model-outcomes.json").read_text())
    assert outcomes["models"] == [{"merged": 1, "model": "m",
                                   "provider": "claude-code", "runs": 1}]


def test_keep_and_missing_tasks_file(world):
    repo, agents, _ = world
    prune_merged.main(["--repo", str(repo), "--agents", str(agents),
                       "--keep", "stale", "--execute"])
    assert {"stale", "running", "dirty", "unmerged"} <= branches(repo)
    assert "tasked" not in branches(repo)


def test_delete_remote_hands_every_pair_to_the_fleet_tool(monkeypatch):
    calls = []
    import next_fleet
    import next_common
    monkeypatch.setattr(next_common, "load_fleet", lambda: {"nodes": []})
    monkeypatch.setattr(next_common, "Ssh", lambda: "ssh")
    monkeypatch.setattr(next_fleet, "delete_refs",
                        lambda fleet, names, ssh, runner, repository, pairs:
                        calls.append((names, repository, pairs, runner.execute)))
    prune_merged.delete_remote([("refs/heads/x", "a" * 40)], "forkmesh/forkmesh", False)
    assert calls == [([], "forkmesh/forkmesh", [("refs/heads/x", "a" * 40)], False)]
