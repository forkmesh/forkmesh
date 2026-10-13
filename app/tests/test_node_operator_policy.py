"""Operator policy fixtures: refused content never corrupts permitted repos."""

import json
import os
from pathlib import Path
import subprocess
import sys

import pytest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import node_operator_policy as policy_mod  # noqa: E402


NOW = 2_000_000_000.0


def git(*arguments, cwd=None, check=True, env=None):
    completed = subprocess.run(
        ["git", *arguments], cwd=cwd, capture_output=True, text=True,
        check=False, env=env, timeout=60,
    )
    if check and completed.returncode != 0:
        raise AssertionError(completed.stderr)
    return completed


def write_policy(path, **overrides):
    value = {"schemaVersion": 1, "profile": "home", **overrides}
    path.write_text(json.dumps(value), encoding="utf-8")
    path.chmod(0o600)
    return path


@pytest.fixture
def node(tmp_path):
    root = tmp_path / "repos"
    root.mkdir(mode=0o700)
    state = tmp_path / "state"
    state.mkdir(mode=0o700)
    policy_path = write_policy(
        tmp_path / "policy.json",
        limits={"maxBlobBytes": 4096, "maxRepositoryBytes": 10 * 1024 * 1024},
        content={
            "blockedExtensions": [".exe"],
            "blockedPathPatterns": ["secrets/*.pem", "*.iso"],
        },
        repositories={"block": ["banned-owner", "alice/leak"]},
    )
    hooks = policy_mod.install_hook(
        tmp_path / "hooks", policy_path, state, root)
    return {
        "tmp": tmp_path, "root": root, "state": state,
        "policy": policy_path, "hooks": hooks,
    }


def make_bare(root, owner, name):
    git_dir = root / owner / (name + ".git")
    git_dir.parent.mkdir(mode=0o700, exist_ok=True)
    git("init", "--bare", "-b", "main", str(git_dir))
    return git_dir


def make_client(tmp_path, label):
    client = tmp_path / ("client-" + label)
    git("init", "-b", "main", str(client))
    git("config", "user.email", "dev@example.test", cwd=client)
    git("config", "user.name", "Dev", cwd=client)
    (client / "README.md").write_text("# permitted\n", encoding="utf-8")
    git("add", ".", cwd=client)
    git("commit", "-m", "permitted", cwd=client)
    return client


def push(node, client, git_dir, *extra):
    receive = "git -c core.hooksPath=%s receive-pack" % node["hooks"]
    return git(
        "push", "--receive-pack=" + receive, str(git_dir), *extra,
        "main:main", cwd=client, check=False,
    )


def commit_file(client, relative, data):
    target = client / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    git("add", ".", cwd=client)
    git("commit", "-m", "add " + relative, cwd=client)


def tip(git_dir):
    return git("--git-dir", str(git_dir), "rev-parse", "refs/heads/main").stdout.strip()


def has_object(git_dir, oid):
    return git("--git-dir", str(git_dir), "cat-file", "-e", oid,
               check=False).returncode == 0


def assert_intact(git_dir, expected_tip):
    assert tip(git_dir) == expected_tip
    git("--git-dir", str(git_dir), "fsck", "--strict", "--no-dangling")


def test_profiles_are_finite_and_home_refuses_private_replicas(tmp_path):
    for profile in policy_mod.PROFILES:
        loaded = policy_mod.load_policy(
            write_policy(tmp_path / (profile + ".json"), profile=profile))
        assert all(loaded["limits"][field] > 0
                   for field in policy_mod.LIMIT_FIELDS)
        assert loaded["violationAction"] == "quarantine"
        assert loaded["jurisdiction"] == "other"
    home = policy_mod.load_policy(tmp_path / "home.json")
    community = policy_mod.load_policy(tmp_path / "community.json")
    commercial = policy_mod.load_policy(tmp_path / "commercial.json")
    assert home["seeding"]["privateReplicas"] is False
    assert community["seeding"]["privateReplicas"] is False
    assert (home["limits"]["storageBudgetBytes"]
            < community["limits"]["storageBudgetBytes"]
            < commercial["limits"]["storageBudgetBytes"])


@pytest.mark.parametrize("document, message", [
    ({"schemaVersion": 1, "profile": "home", "bandwidth": 1}, "unknown"),
    ({"schemaVersion": 2, "profile": "home"}, "schemaVersion"),
    ({"schemaVersion": 1, "profile": "cloud"}, "profile"),
    ({"schemaVersion": 1, "profile": "home",
      "limits": {"maxBlobBytes": 0}}, "positive"),
    ({"schemaVersion": 1, "profile": "home",
      "content": {"blockedObjectIds": ["abc"]}}, "object ids"),
    ({"schemaVersion": 1, "profile": "home",
      "content": {"blockedExtensions": ["exe"]}}, "start with"),
    ({"schemaVersion": 1, "profile": "home",
      "repositories": {"block": ["Not An Owner"]}}, "owner"),
])
def test_invalid_policies_are_refused(document, message):
    with pytest.raises(policy_mod.PolicyError, match=message):
        policy_mod.normalize_policy(document)


def test_group_writable_policy_file_is_refused(tmp_path):
    path = write_policy(tmp_path / "policy.json")
    path.chmod(0o664)
    with pytest.raises(policy_mod.PolicyError, match="writable only"):
        policy_mod.load_policy(path)


def test_permitted_push_is_accepted(node):
    git_dir = make_bare(node["root"], "alice", "app")
    client = make_client(node["tmp"], "ok")
    pushed = push(node, client, git_dir)
    assert pushed.returncode == 0, pushed.stderr
    assert_intact(git_dir, git("rev-parse", "HEAD", cwd=client).stdout.strip())
    assert not (node["state"] / "audit.jsonl").exists()


@pytest.mark.parametrize("relative, data, rule", [
    ("big.bin", b"x" * 8192, "blob-size"),
    ("tools/setup.EXE", b"MZ", "blocked-extension"),
    ("disc.iso", b"iso", "blocked-path"),
    ("secrets/server.pem", b"key", "blocked-path"),
])
def test_disallowed_push_is_rejected_without_touching_repositories(
    node, relative, data, rule
):
    git_dir = make_bare(node["root"], "alice", "app")
    neighbour = make_bare(node["root"], "bob", "lib")
    client = make_client(node["tmp"], "bad")
    assert push(node, client, git_dir).returncode == 0
    assert push(node, client, neighbour).returncode == 0
    permitted_tip = git("rev-parse", "HEAD", cwd=client).stdout.strip()

    commit_file(client, relative, data)
    refused_tip = git("rev-parse", "HEAD", cwd=client).stdout.strip()
    blob = git("rev-parse", "HEAD:" + relative, cwd=client).stdout.strip()
    pushed = push(node, client, git_dir)

    assert pushed.returncode != 0
    assert "operator policy refused the push" in pushed.stderr
    assert rule in pushed.stderr
    assert_intact(git_dir, permitted_tip)
    assert_intact(neighbour, permitted_tip)
    # Git discarded its receive quarantine: no refused object was stored.
    assert not has_object(git_dir, refused_tip)
    assert not has_object(git_dir, blob)
    assert not [
        item for item in (git_dir / "objects").iterdir()
        if item.name.startswith("tmp_objdir")
    ]

    (evidence,) = (node["state"] / "evidence").glob("evidence-*.json")
    record = json.loads(evidence.read_text())
    assert record["contentRetained"] is False
    assert record["owner"] == "alice" and record["source"] == "push"
    assert rule in {item["rule"] for item in record["findings"]}
    assert policy_mod.verify_audit(node["state"])["ok"] is True

    # A permitted follow-up from a clean history still lands.
    git("reset", "--hard", permitted_tip, cwd=client)
    commit_file(client, "docs/notes.md", b"fine\n")
    assert push(node, client, git_dir).returncode == 0


def test_renaming_an_accepted_blob_to_a_refused_name_is_caught(node):
    git_dir = make_bare(node["root"], "alice", "app")
    client = make_client(node["tmp"], "rename")
    commit_file(client, "payload.txt", b"same bytes\n")
    assert push(node, client, git_dir).returncode == 0
    accepted = tip(git_dir)
    git("mv", "payload.txt", "payload.exe", cwd=client)
    git("commit", "-m", "rename", cwd=client)
    pushed = push(node, client, git_dir)
    assert pushed.returncode != 0 and "blocked-extension" in pushed.stderr
    assert_intact(git_dir, accepted)


def test_blocked_object_id_and_blocked_repository(node):
    client = make_client(node["tmp"], "ids")
    commit_file(client, "notice.txt", b"taken down\n")
    blob = git("rev-parse", "HEAD:notice.txt", cwd=client).stdout.strip()
    write_policy(node["policy"], content={"blockedObjectIds": [blob]})
    git_dir = make_bare(node["root"], "alice", "app")
    pushed = push(node, client, git_dir)
    assert pushed.returncode != 0 and "blocked-object" in pushed.stderr

    write_policy(node["policy"], repositories={"block": ["alice/leak"]})
    blocked = make_bare(node["root"], "alice", "leak")
    pushed = push(node, client, blocked)
    assert pushed.returncode != 0 and "repository-blocked" in pushed.stderr
    assert git("--git-dir", str(blocked), "for-each-ref").stdout == ""
    assert push(node, client, git_dir).returncode == 0


def test_quarantined_repository_refuses_pushes(node):
    git_dir = make_bare(node["root"], "alice", "app")
    client = make_client(node["tmp"], "quarantined")
    policy_mod.quarantine_repository(
        node["state"], owner="alice", name="app", reason="legal hold",
        actor="op", now=NOW)
    pushed = push(node, client, git_dir)
    assert pushed.returncode != 0
    assert "repository-quarantined" in pushed.stderr


def test_unreadable_policy_fails_closed(node):
    git_dir = make_bare(node["root"], "alice", "app")
    client = make_client(node["tmp"], "closed")
    node["policy"].unlink()
    pushed = push(node, client, git_dir)
    assert pushed.returncode != 0
    assert "policy is unavailable" in pushed.stderr
    assert git("--git-dir", str(git_dir), "for-each-ref").stdout == ""


def test_storage_budget_and_repository_count(node):
    write_policy(node["policy"], limits={
        "storageBudgetBytes": 1, "maxRepositories": 1})
    git_dir = make_bare(node["root"], "alice", "app")
    client = make_client(node["tmp"], "budget")
    pushed = push(node, client, git_dir)
    assert pushed.returncode != 0 and "storage-budget" in pushed.stderr
    policy = policy_mod.load_policy(node["policy"])
    assert policy_mod.new_repository_admission(
        policy, node["root"], "alice", "app") == []
    refused = policy_mod.new_repository_admission(
        policy, node["root"], "carol", "other")
    assert [item["rule"] for item in refused] == ["repository-count"]


def seed_stored(node, owner, name, files):
    git_dir = make_bare(node["root"], owner, name)
    client = make_client(node["tmp"], owner + "-" + name)
    for relative, data in files.items():
        commit_file(client, relative, data)
    # Upstream fetches do not run the hook; simulate one with a plain push.
    git("push", str(git_dir), "main:main", cwd=client)
    return git_dir


def test_enforce_previews_then_quarantines_only_violators(node):
    bad = seed_stored(node, "alice", "app", {"dump.iso": b"iso"})
    good = seed_stored(node, "bob", "lib", {"src/lib.py": b"x = 1\n"})
    good_tip, bad_tip = tip(good), tip(bad)
    policy = policy_mod.load_policy(node["policy"])

    dry = policy_mod.enforce(
        policy, node["root"], node["state"], apply=False, actor="op", now=NOW)
    assert [(item["owner"], item["action"]) for item in dry["violations"]] == [
        ("alice", "none")]
    assert not policy_mod.is_quarantined(node["state"], "alice", "app")

    applied = policy_mod.enforce(
        policy, node["root"], node["state"], apply=True, actor="op", now=NOW)
    assert applied["violations"][0]["action"] == "quarantined"
    assert policy_mod.is_quarantined(node["state"], "alice", "app")
    assert not policy_mod.is_quarantined(node["state"], "bob", "lib")
    # Quarantine is reversible: nothing was deleted or rewritten.
    assert_intact(bad, bad_tip)
    assert_intact(good, good_tip)
    assert policy_mod.release_repository(
        node["state"], owner="alice", name="app", reason="appeal", actor="op",
        now=NOW)
    assert not policy_mod.is_quarantined(node["state"], "alice", "app")
    actions = [
        json.loads(line)["action"]
        for line in (node["state"] / "audit.jsonl").read_text().splitlines()
    ]
    assert actions == ["quarantine", "release"]


def test_report_only_policy_never_quarantines(node):
    seed_stored(node, "alice", "app", {"dump.iso": b"iso"})
    write_policy(node["policy"], violationAction="report",
                 content={"blockedPathPatterns": ["*.iso"]})
    result = policy_mod.enforce(
        policy_mod.load_policy(node["policy"]), node["root"], node["state"],
        apply=True, actor="op", now=NOW)
    assert result["violations"][0]["action"] == "none"
    assert not policy_mod.is_quarantined(node["state"], "alice", "app")


def test_preview_estimates_capacity_and_apply_requires_confirmation(node):
    seed_stored(node, "alice", "app", {"dump.iso": b"iso"})
    seed_stored(node, "bob", "lib", {"src/lib.py": b"x = 1\n"})
    target = write_policy(node["tmp"] / "active.json")
    candidate = node["policy"]

    report = policy_mod.preview(
        policy_mod.load_policy(candidate), node["root"], node["state"],
        policy_mod.load_policy(target))
    capacity = report["capacity"]
    stored = sum(
        policy_mod.repository_storage_bytes(item[2])
        for item in policy_mod.stored_repositories(node["root"]))
    assert capacity["storedBytes"] == stored > 0
    assert capacity["repositorySlotsUsed"] == 2
    assert capacity["headroomBytes"] == capacity["storageBudgetBytes"] - stored
    assert report["availabilityReducing"] is True
    assert report["newlyUnavailable"] == [
        {"owner": "alice", "repository": "app", "rules": ["blocked-path"]}]
    assert any("Other mirrors" in item for item in report["consequences"])

    before = target.read_bytes()
    refused = policy_mod.apply_policy(
        candidate, target, node["state"], node["root"], confirm=False,
        actor="op", now=NOW)
    assert refused["applied"] is False and target.read_bytes() == before
    assert not (node["state"] / "audit.jsonl").exists()

    applied = policy_mod.apply_policy(
        candidate, target, node["state"], node["root"], confirm=True,
        actor="op", now=NOW)
    assert applied["applied"] is True
    assert target.read_bytes() == candidate.read_bytes()
    (archive,) = policy_mod.policy_history(node["state"])
    assert archive.read_bytes() == before

    # Rollback through the CLI restores the archived policy (loosening needs
    # no confirmation) and records it.
    code = policy_mod.main([
        "rollback", "--state", str(node["state"]), "--root",
        str(node["root"]), "--target", str(target), "--actor", "op"])
    assert code == 0
    assert target.read_bytes() == before
    # The policy rolled away from is archived in turn, so it can be re-applied.
    (forward,) = policy_mod.policy_history(node["state"])
    assert forward.read_bytes() == candidate.read_bytes()
    actions = [
        json.loads(line)["action"]
        for line in (node["state"] / "audit.jsonl").read_text().splitlines()
    ]
    assert actions == ["apply-policy", "rollback-policy"]


def test_loosening_policy_is_not_availability_reducing(node):
    strict = policy_mod.load_policy(node["policy"])
    loose = policy_mod.normalize_policy({
        "schemaVersion": 1, "profile": "commercial"})
    report = policy_mod.preview(loose, node["root"], node["state"], strict)
    assert report["availabilityReducing"] is False
    assert report["consequences"] == []


def test_report_action_appeal_reverse_releases_and_audits(node):
    seed_stored(node, "alice", "app", {"README.md": b"hi\n"})
    write_policy(node["policy"], jurisdiction="eu")
    policy = policy_mod.load_policy(node["policy"])
    case = policy_mod.open_case(
        node["state"], policy, owner="alice", name="app",
        category="copyright", reporter="rights@example.test",
        details="file X copies work Y", now=NOW)
    assert case["runbook"] == policy_mod.RUNBOOKS["eu"]
    policy_mod.action_case(
        node["state"], policy, case["id"], actor="op", note="notice valid",
        now=NOW + 60)
    assert policy_mod.is_quarantined(node["state"], "alice", "app")
    policy_mod.appeal_case(
        node["state"], case["id"], appellant="alice",
        details="licensed under MIT", now=NOW + 120)
    resolved = policy_mod.resolve_case(
        node["state"], case["id"], decision="reverse", actor="op",
        note="licence verified", now=NOW + 180)
    assert resolved["status"] == "reversed"
    assert not policy_mod.is_quarantined(node["state"], "alice", "app")
    assert [item["event"] for item in resolved["history"]] == [
        "reported", "actioned", "appealed", "resolved"]

    summary = policy_mod.status_summary(node["state"])
    assert summary["quarantinedRepositories"] == 0
    assert summary["resolvedCases"] == 1
    assert summary["audit"]["ok"] is True
    assert summary["audit"]["records"] == 6

    log = node["state"] / "audit.jsonl"
    lines = log.read_text().splitlines()
    tampered = json.loads(lines[1])
    tampered["detail"]["actor"] = "someone-else"
    lines[1] = json.dumps(tampered)
    log.write_text("\n".join(lines) + "\n")
    assert policy_mod.verify_audit(node["state"]) == {
        "ok": False, "records": 1, "brokenAtLine": 2}


def test_evidence_retention_honours_legal_hold(node):
    policy = policy_mod.load_policy(node["policy"])
    held = policy_mod.open_case(
        node["state"], policy, owner="alice", name="app", category="illegal",
        reporter="police", details="preservation request", legal_hold=True,
        now=NOW)
    policy_mod.action_case(
        node["state"], policy, held["id"], actor="op", note="preserve",
        now=NOW)
    policy_mod.resolve_case(
        node["state"], held["id"], decision="uphold", actor="op", note="kept",
        now=NOW)
    stale = policy_mod.record_evidence(
        node["state"], policy, owner="bob", name="lib", source="push",
        findings=[{"rule": "blob-size", "detail": "big"}], now=NOW)
    later = NOW + (policy["retention"]["evidenceDays"] + 1) * 86400
    dry = policy_mod.prune_evidence(
        policy, node["state"], apply=False, now=later)
    assert dry["pruned"] == [stale]
    policy_mod.prune_evidence(policy, node["state"], apply=True, now=later)
    remaining = [
        json.loads(path.read_text())["caseId"]
        for path in (node["state"] / "evidence").glob("*.json")
    ]
    assert remaining == [held["id"]]


def test_remove_requires_quarantine_and_confirmation(node):
    git_dir = seed_stored(node, "alice", "app", {"README.md": b"hi\n"})
    with pytest.raises(policy_mod.PolicyError, match="quarantine"):
        policy_mod.remove_repository(
            node["state"], node["root"], owner="alice", name="app",
            actor="op", reason="court order", confirm=True)
    policy_mod.quarantine_repository(
        node["state"], owner="alice", name="app", reason="court order",
        actor="op", now=NOW)
    preview = policy_mod.remove_repository(
        node["state"], node["root"], owner="alice", name="app", actor="op",
        reason="court order", confirm=False)
    assert preview["removed"] is False and git_dir.is_dir()
    done = policy_mod.remove_repository(
        node["state"], node["root"], owner="alice", name="app", actor="op",
        reason="court order", confirm=True)
    assert done["removed"] is True and not git_dir.exists()
    last = json.loads(
        (node["state"] / "audit.jsonl").read_text().splitlines()[-1])
    assert last["action"] == "remove" and last["detail"]["irreversible"]


def test_sha256_repositories_are_scanned(node):
    git_dir = node["root"] / "alice" / "sha.git"
    git_dir.parent.mkdir(mode=0o700)
    git("init", "--bare", "--object-format=sha256", "-b", "main", str(git_dir))
    client = node["tmp"] / "client-sha"
    git("init", "--object-format=sha256", "-b", "main", str(client))
    git("config", "user.email", "dev@example.test", cwd=client)
    git("config", "user.name", "Dev", cwd=client)
    commit_file(client, "ok.txt", b"ok\n")
    assert push(node, client, git_dir).returncode == 0
    commit_file(client, "bin/tool.exe", b"MZ")
    pushed = push(node, client, git_dir)
    assert pushed.returncode != 0 and "blocked-extension" in pushed.stderr


def test_hook_script_is_owner_only_and_quotes_paths(tmp_path):
    state = tmp_path / "state dir"
    state.mkdir(mode=0o700)
    policy = write_policy(tmp_path / "policy.json")
    hooks = policy_mod.install_hook(tmp_path / "hooks", policy, state, None)
    script = hooks / "pre-receive"
    assert oct(script.stat().st_mode & 0o777) == "0o700"
    assert "'" + str(state) + "'" in script.read_text()
    assert sorted(item.name for item in hooks.iterdir()) == ["pre-receive"]
    (hooks / "post-receive").write_text("#!/bin/sh\n")
    with pytest.raises(policy_mod.PolicyError, match="other files"):
        policy_mod.install_hook(hooks, policy, state, None)
