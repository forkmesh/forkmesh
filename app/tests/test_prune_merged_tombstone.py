from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import prune_merged  # noqa: E402


def git(repo, *args):
    return subprocess.run(["git", "-C", str(repo), *args], capture_output=True,
                          text=True, check=True).stdout.strip()


def make_repo(tmp_path):
    repo = tmp_path / "repo"
    repo.mkdir()
    git(repo, "init", "-q", "-b", "main")
    git(repo, "config", "user.email", "t@example.com")
    git(repo, "config", "user.name", "t")
    (repo / "a.txt").write_text("a\n")
    git(repo, "add", "a.txt")
    git(repo, "commit", "-q", "-m", "a")
    git(repo, "branch", "feature")
    return repo, git(repo, "rev-parse", "feature")


def test_retire_branch_leaves_tombstone_and_drops_mesh_copy(tmp_path):
    repo, sha = make_repo(tmp_path)
    git(repo, "update-ref", "refs/remotes/forkmesh-mesh/feature", sha)
    assert prune_merged.retire_branch(repo, "feature", sha) == ""
    assert git(repo, "rev-parse", "refs/forkmesh/trash/deleted/feature") == sha
    assert git(repo, "for-each-ref", "refs/heads/feature") == ""
    assert git(repo, "for-each-ref", "refs/remotes/forkmesh-mesh/feature") == ""


def test_retire_branch_keeps_a_mesh_copy_with_newer_commits(tmp_path):
    repo, sha = make_repo(tmp_path)
    (repo / "b.txt").write_text("b\n")
    git(repo, "add", "b.txt")
    git(repo, "commit", "-q", "-m", "b")
    newer = git(repo, "rev-parse", "HEAD")
    git(repo, "update-ref", "refs/remotes/forkmesh-mesh/feature", newer)
    assert prune_merged.retire_branch(repo, "feature", sha) == ""
    assert git(repo, "rev-parse", "refs/remotes/forkmesh-mesh/feature") == newer


def test_retire_branch_refuses_a_moved_branch(tmp_path):
    repo, sha = make_repo(tmp_path)
    problem = prune_merged.retire_branch(repo, "feature", "f" * 40)
    assert problem
    assert git(repo, "rev-parse", "refs/heads/feature") == sha


def test_plan_local_keeps_a_standing_agents_branch(tmp_path):
    import json
    repo, sha = make_repo(tmp_path)
    git(repo, "checkout", "-q", "-b", "side")
    (repo / "c.txt").write_text("c\n")
    git(repo, "add", "c.txt")
    git(repo, "commit", "-q", "-m", "c")
    git(repo, "checkout", "-q", "main")
    git(repo, "merge", "-q", "--no-ff", "-m", "merge side", "side")
    git(repo, "branch", "ceo", "side")
    sessions = tmp_path / "sessions"
    (sessions / "7").mkdir(parents=True)
    (sessions / "7" / "meta.json").write_text(json.dumps({
        "id": 7, "status": "success", "branchName": "ceo",
        "teammateId": "ceo", "teammateName": "CEO"}))
    plan = prune_merged.plan_local(repo, sessions, [tmp_path], {})
    deleting = {item["branch"] for item in plan["delete"]}
    assert "ceo" not in deleting
    assert "side" in deleting
    assert "standing agent" in plan["kept_all"]["ceo"]
    assert plan["recycle"] == []
