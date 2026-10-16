"""Exact-OID, hook-free pull merge plumbing for independently run nodes."""

import json
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace

import pytest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import headless_mirror_refresh as refresh  # noqa: E402


def run(arguments, cwd):
    return subprocess.run(
        arguments, cwd=cwd, check=True, capture_output=True, text=True
    ).stdout.strip()


def repository(
    tmp_path, *, conflict=False, divergent=False, reward="",
    required_checks=False, pull_rules=False,
):
    bare = tmp_path / "mirror.git"
    work = tmp_path / "work"
    run(["git", "init", "--bare", str(bare)], tmp_path)
    run(["git", "init", "-b", "main", str(work)], tmp_path)
    run(["git", "config", "user.name", "Test"], work)
    run(["git", "config", "user.email", "test@example.invalid"], work)
    (work / "value.txt").write_text("base\n", encoding="utf-8")
    run(["git", "add", "value.txt"], work)
    run(["git", "commit", "-m", "base"], work)
    opening_base = run(["git", "rev-parse", "HEAD"], work)

    run(["git", "switch", "-c", "feature"], work)
    (work / "value.txt").write_text(
        "feature\n" if conflict else "base\nfeature\n", encoding="utf-8")
    run(["git", "commit", "-am", "feature"], work)
    head = run(["git", "rev-parse", "HEAD"], work)
    run(["git", "switch", "main"], work)
    if conflict:
        (work / "value.txt").write_text("main\n", encoding="utf-8")
        run(["git", "commit", "-am", "main"], work)
    elif divergent:
        (work / "main-only.txt").write_text("main\n", encoding="utf-8")
        run(["git", "add", "main-only.txt"], work)
        run(["git", "commit", "-m", "divergent main"], work)
    if required_checks:
        (work / ".forkmesh").mkdir()
        (work / ".forkmesh/required-checks.json").write_text(
            '{"schema":"forkmesh-required-checks-v1"}', encoding="utf-8")
        run(["git", "add", ".forkmesh/required-checks.json"], work)
        run(["git", "commit", "-m", "require checks"], work)
    if pull_rules:
        (work / ".forkmesh").mkdir(exist_ok=True)
        (work / ".forkmesh/pull-rules.json").write_text(
            '{"schema":"forkmesh-pull-rules-v1","rules":[]}', encoding="utf-8")
        run(["git", "add", ".forkmesh/pull-rules.json"], work)
        run(["git", "commit", "-m", "pull request rules"], work)
    base = run(["git", "rev-parse", "HEAD"], work)

    run(["git", "switch", "--orphan", "forkmesh/pulls"], work)
    run(["git", "rm", "-rf", "--ignore-unmatch", "value.txt"], work)
    path = work / ".forkmesh" / "pulls" / "7"
    path.mkdir(parents=True)
    (path / "pull.md").write_text(
        "---\n"
        "schema: forkmesh-pull-v1\n"
        "number: 7\n"
        "title: Safe merge\n"
        "base: main\n"
        "head: feature\n"
        "status: open\n"
        "derive: branch\n"
        f"creationBaseOid: {base}\n"
        f"creationHeadOid: {head}\n"
        "ts: 1\n"
        "author: alice\n"
        "authorName: Alice\n"
        "sig: test\n"
        # The owner's reward estimate, authored straight into pull.md outside
        # the contributor's signature, exactly like the live records.
        + (
            "rewardEstimateLamports: 4000000\n"
            "rewardRecipient: alice\n"
            "rewardId: reward-forkmesh-forkmesh-pull-7\n"
            "rewardCondition: merged\n"
            f"rewardStatus: {reward}\n"
            if reward else ""
        )
        + "---\n\nReview me.\n",
        encoding="utf-8",
    )
    (path / "0001-review.md").write_text(
        "---\n"
        "type: review\n"
        "id: review-test-1\n"
        "author: bob\n"
        "authorName: Bob\n"
        "ts: 2\n"
        "state: approved\n"
        "sig: owner-validated-test-signature\n"
        "---\n\nReviewed.\n",
        encoding="utf-8",
    )
    run([
        "git", "add", ".forkmesh/pulls/7/pull.md",
        ".forkmesh/pulls/7/0001-review.md",
    ], work)
    run(["git", "commit", "-m", "pull metadata"], work)
    pulls = run(["git", "rev-parse", "HEAD"], work)
    run(["git", "remote", "add", "origin", str(bare)], work)
    run(["git", "push", "origin", "main", "feature", "forkmesh/pulls"], work)
    run(["git", "symbolic-ref", "HEAD", "refs/heads/main"], bare)
    return bare, base, head, pulls, opening_base


def config(tmp_path, bare):
    state = tmp_path / "state"
    state.mkdir(mode=0o700)
    return SimpleNamespace(
        source_repository=bare,
        git_program=Path(run(["which", "git"], tmp_path)),
        identity_state_directory=state,
        catalog=SimpleNamespace(branch="main"),
        owner_aliases=("mirror2", "forkmesh"),
        repository_name="forkmesh",
    )


def request(
    config, *, base, head, pulls, request_id="merge_request_0001",
    override_review=False, delete_head=False,
):
    value = {
        "schemaVersion": 1,
        "type": refresh.MERGE_EXECUTOR_TYPE,
        "action": "execute",
        "owner": "forkmesh",
        "repository": "forkmesh",
        "pullNumber": 7,
        "requestId": request_id,
        "expectedBaseOid": base,
        "expectedHeadOid": head,
        "expectedPullsOid": pulls,
        **({"overrideReview": True} if override_review else {}),
        **({"deleteHead": True} if delete_head else {}),
    }
    parsed = refresh._merge_request(config, value)
    parsed.pop("action")
    return parsed


def git_ref(bare, ref):
    return run(["git", "--git-dir", str(bare), "rev-parse", ref], bare)


def optional_git_ref(bare, ref):
    completed = subprocess.run(
        ["git", "--git-dir", str(bare), "rev-parse", "--verify", ref],
        cwd=bare,
        capture_output=True,
        text=True,
        check=False,
    )
    return completed.stdout.strip() if completed.returncode == 0 else ""


def assert_fsck_clean(bare):
    # Exercise the production invocation exactly, then use ordinary fsck output
    # to prove the test did not merely suppress dangling-object diagnostics.
    subprocess.run(
        [
            "git", "--git-dir", str(bare), "fsck", "--full", "--strict",
            "--no-progress", "--no-dangling",
        ],
        cwd=bare,
        check=True,
        capture_output=True,
        text=True,
    )
    visible = subprocess.run(
        [
            "git", "--git-dir", str(bare), "fsck", "--full", "--strict",
            "--no-progress",
        ],
        cwd=bare,
        check=True,
        capture_output=True,
        text=True,
    )
    diagnostics = (visible.stdout + visible.stderr).lower()
    assert "dangling " not in diagnostics
    assert "unreachable " not in diagnostics


def pull_front_matter(text):
    lines = text.splitlines()
    close = lines.index("---", 1)
    return dict(line.split(": ", 1) for line in lines[1:close])


def test_clean_merge_updates_code_metadata_and_owner_job_atomically(tmp_path):
    bare, base, head, pulls, _opening = repository(tmp_path)
    cfg = config(tmp_path, bare)
    req = request(cfg, base=base, head=head, pulls=pulls)
    marker = tmp_path / "hook-ran"
    hook = bare / "hooks" / "reference-transaction"
    hook.write_text(
        "#!/bin/sh\n/usr/bin/touch " + str(marker) + "\n",
        encoding="utf-8",
    )
    hook.chmod(0o755)

    result = refresh._merge_execute_locked(cfg, req)

    assert result["status"] == "merged"
    assert result["baseBefore"] == base
    assert result["head"] == head
    assert git_ref(bare, "refs/heads/main") == result["baseAfter"] == head
    assert git_ref(bare, "refs/heads/forkmesh/pulls") == result["pullsAfter"]
    assert optional_git_ref(bare, "refs/heads/forkmesh/pull/7") == ""
    isolated = git_ref(bare, "refs/pr/7/metadata")
    assert isolated
    assert run([
        "git", "--git-dir", str(bare), "ls-tree", "-r", "--name-only",
        isolated,
    ], bare).splitlines() == [
        ".forkmesh/pulls/7/0001-review.md",
        ".forkmesh/pulls/7/pull.md",
    ]
    original_metadata = run([
        "git", "--git-dir", str(bare), "show",
        pulls + ":.forkmesh/pulls/7/pull.md",
    ], bare)
    metadata = run([
        "git", "--git-dir", str(bare), "show",
        result["pullsAfter"] + ":.forkmesh/pulls/7/pull.md",
    ], bare)
    assert "status: merged" in metadata
    assert "mergeBase: " + base in metadata
    assert "mergeHead: " + head in metadata
    before = pull_front_matter(original_metadata)
    after = pull_front_matter(metadata)
    # PullStore intentionally leaves owner-applied lifecycle fields outside the
    # author's signature. Every signature-covered input remains byte-identical.
    for field in (
        "title", "base", "head", "creationBaseOid", "creationHeadOid",
        "ts", "author", "authorName", "sig",
    ):
        assert after[field] == before[field]
    assert original_metadata.split("---\n", 2)[2] == metadata.split("---\n", 2)[2]
    job_path = refresh._merge_job_path(cfg, req)
    assert json.loads(job_path.read_text()) == result
    completed = refresh._merge_completed_refs(req)
    assert git_ref(bare, completed[0]) == result["baseAfter"]
    assert git_ref(bare, completed[1]) == result["pullsAfter"]
    assert not marker.exists()
    assert_fsck_clean(bare)

    assert refresh._merge_execute_locked(cfg, req) == result


def test_clean_merge_can_delete_the_head_branch_atomically(tmp_path):
    bare, base, head, pulls, _opening = repository(tmp_path)
    cfg = config(tmp_path, bare)
    req = request(cfg, base=base, head=head, pulls=pulls, delete_head=True)

    result = refresh._merge_execute_locked(cfg, req)

    assert result["status"] == "merged", result
    assert optional_git_ref(bare, "refs/heads/feature") == ""
    assert git_ref(bare, "refs/heads/main") == result["baseAfter"]
    assert git_ref(bare, "refs/heads/forkmesh/pulls") == result["pullsAfter"]
    assert_fsck_clean(bare)


def test_merging_settles_the_pulls_reward_estimate(tmp_path):
    # Regression for adhoc #86: pull #32 merged with its estimate still at
    # pending_merge, which holds the payment out of the payout queue and keeps
    # the public ledger showing "PENDING MERGE" for landed work.
    bare, base, head, pulls, _opening = repository(
        tmp_path, reward="pending_merge")
    cfg = config(tmp_path, bare)
    req = request(cfg, base=base, head=head, pulls=pulls)

    result = refresh._merge_execute_locked(cfg, req)

    assert result["status"] == "merged"
    after = pull_front_matter(run([
        "git", "--git-dir", str(bare), "show",
        result["pullsAfter"] + ":.forkmesh/pulls/7/pull.md",
    ], bare))
    assert after["status"] == "merged"
    assert after["rewardStatus"] == "pending_wallet"
    # Nothing else about the estimate is the merge's business.
    assert after["rewardId"] == "reward-forkmesh-forkmesh-pull-7"
    assert after["rewardEstimateLamports"] == "4000000"
    assert after["rewardRecipient"] == "alice"
    assert after["rewardCondition"] == "merged"


def test_merging_deletes_the_pull_branch_and_keeps_its_history(tmp_path):
    bare, base, head, pulls, _opening = repository(
        tmp_path, reward="pending_merge")
    run(["git", "--git-dir", str(bare), "update-ref",
         "refs/heads/forkmesh/pull/7", pulls], bare)
    cfg = config(tmp_path, bare)
    req = request(cfg, base=base, head=head, pulls=pulls)

    result = refresh._merge_execute_locked(cfg, req)

    assert result["status"] == "merged"
    assert optional_git_ref(bare, "refs/heads/forkmesh/pull/7") == ""
    kept = git_ref(bare, "refs/pr/7/metadata")
    assert run(["git", "--git-dir", str(bare), "show", "-s", "--format=%P",
                kept], bare) == pulls
    record = pull_front_matter(run([
        "git", "--git-dir", str(bare), "show",
        kept + ":.forkmesh/pulls/7/pull.md",
    ], bare))
    assert record["status"] == "merged"
    assert record["rewardId"] == "reward-forkmesh-forkmesh-pull-7"
    assert_fsck_clean(bare)


def test_merging_never_walks_a_settled_reward_estimate_backwards(tmp_path):
    bare, base, head, pulls, _opening = repository(tmp_path, reward="paid")
    cfg = config(tmp_path, bare)
    req = request(cfg, base=base, head=head, pulls=pulls)

    result = refresh._merge_execute_locked(cfg, req)

    assert result["status"] == "merged"
    after = pull_front_matter(run([
        "git", "--git-dir", str(bare), "show",
        result["pullsAfter"] + ":.forkmesh/pulls/7/pull.md",
    ], bare))
    assert after["rewardStatus"] == "paid"


def test_conflict_is_terminal_and_never_moves_public_refs(tmp_path):
    bare, base, head, pulls, _opening = repository(tmp_path, conflict=True)
    cfg = config(tmp_path, bare)
    req = request(cfg, base=base, head=head, pulls=pulls)

    result = refresh._merge_execute_locked(cfg, req)

    assert result["status"] == "failed"
    assert result["error"] == "merge_conflict"
    assert git_ref(bare, "refs/heads/main") == base
    assert git_ref(bare, "refs/heads/forkmesh/pulls") == pulls
    assert refresh._merge_execute_locked(cfg, req) == result
    assert_fsck_clean(bare)


def test_merge_fails_closed_when_the_base_requires_signed_checks(tmp_path):
    # This executor cannot verify forkmesh-check-v1 attestations, so a base
    # branch carrying a required-checks policy must not merge through it even
    # when the owner waives the review gate; the Go mirror node enforces it.
    bare, base, head, pulls, _opening = repository(
        tmp_path, required_checks=True)
    cfg = config(tmp_path, bare)
    for request_id, waive in (
        ("merge_request_checks", False),
        ("merge_request_checks_waived", True),
    ):
        result = refresh._merge_execute_locked(cfg, request(
            cfg, base=base, head=head, pulls=pulls,
            request_id=request_id, override_review=waive))
        assert result["status"] == "failed"
        assert result["error"] == "checks_required"
    assert git_ref(bare, "refs/heads/main") == base
    assert git_ref(bare, "refs/heads/forkmesh/pulls") == pulls


def test_merge_fails_closed_when_the_base_sets_pull_rules(tmp_path):
    bare, base, head, pulls, _opening = repository(tmp_path, pull_rules=True)
    cfg = config(tmp_path, bare)
    result = refresh._merge_execute_locked(cfg, request(
        cfg, base=base, head=head, pulls=pulls,
        request_id="merge_request_rules", override_review=True))
    assert result["status"] == "failed"
    assert result["error"] == "rules_required"
    assert git_ref(bare, "refs/heads/main") == base


def test_merge_requires_one_independent_approval(tmp_path):
    bare, base, head, pulls, _opening = repository(tmp_path)
    cfg = config(tmp_path, bare)
    work = tmp_path / "edit-pulls"
    run(["git", "clone", str(bare), str(work)], tmp_path)
    run(["git", "config", "user.name", "Test"], work)
    run(["git", "config", "user.email", "test@example.invalid"], work)
    run(["git", "switch", "forkmesh/pulls"], work)
    (work / ".forkmesh/pulls/7/0001-review.md").unlink()
    run(["git", "add", "-u"], work)
    run(["git", "commit", "-m", "remove peer review"], work)
    run(["git", "push", "origin", "forkmesh/pulls"], work)
    no_review_pulls = git_ref(bare, "refs/heads/forkmesh/pulls")
    req = request(
        cfg,
        base=base,
        head=head,
        pulls=no_review_pulls,
        request_id="merge_request_peer_review",
    )

    result = refresh._merge_execute_locked(cfg, req)

    assert result["status"] == "failed"
    assert result["error"] == "review_required"
    assert git_ref(bare, "refs/heads/main") == base


def test_owner_waiver_merges_without_an_independent_approval(tmp_path):
    # The peer gate is the repository's default, not a lock on its owner: the
    # relay authorizes and audits the waiver before a mirror ever sees it.
    bare, base, head, pulls, _opening = repository(tmp_path)
    cfg = config(tmp_path, bare)
    work = tmp_path / "waive-pulls"
    run(["git", "clone", str(bare), str(work)], tmp_path)
    run(["git", "config", "user.name", "Test"], work)
    run(["git", "config", "user.email", "test@example.invalid"], work)
    run(["git", "switch", "forkmesh/pulls"], work)
    (work / ".forkmesh/pulls/7/0001-review.md").unlink()
    run(["git", "add", "-u"], work)
    run(["git", "commit", "-m", "remove peer review"], work)
    run(["git", "push", "origin", "forkmesh/pulls"], work)
    no_review_pulls = git_ref(bare, "refs/heads/forkmesh/pulls")
    locked = refresh._merge_execute_locked(cfg, request(
        cfg, base=base, head=head, pulls=no_review_pulls,
        request_id="merge_request_locked"))
    assert locked["error"] == "review_required"

    waived_request = request(
        cfg,
        base=base,
        head=head,
        pulls=no_review_pulls,
        request_id="merge_request_waived",
        override_review=True,
    )
    result = refresh._merge_execute_locked(cfg, waived_request)

    assert result["status"] == "merged"
    assert git_ref(bare, "refs/heads/main") != base
    # The waiver is legible in the ledger commit the merge published.
    assert "review gate waived by owner" in run(
        ["git", "--git-dir", str(bare), "log", "-1", "--format=%s",
         "refs/heads/forkmesh/pulls"], bare)
    assert_fsck_clean(bare)


def test_waived_and_reviewed_requests_never_share_an_idempotency_digest(
        tmp_path):
    bare, base, head, pulls, _opening = repository(tmp_path)
    cfg = config(tmp_path, bare)
    reviewed = request(cfg, base=base, head=head, pulls=pulls)
    waived = request(
        cfg, base=base, head=head, pulls=pulls, override_review=True)

    assert reviewed["requestId"] == waived["requestId"]
    assert reviewed["requestDigest"] != waived["requestDigest"]
    assert "overrideReview" not in reviewed
    # Updating a branch from its base has no review gate, so it cannot waive
    # one; an executor that took the field there would accept a meaningless
    # request instead of refusing an unknown one.
    with pytest.raises(refresh.RefreshError):
        refresh._merge_request(
            cfg,
            {
                "schemaVersion": 1,
                "type": refresh.UPDATE_EXECUTOR_TYPE,
                "action": "execute",
                "owner": "forkmesh",
                "repository": "forkmesh",
                "pullNumber": 7,
                "requestId": "merge_request_0001",
                "expectedBaseOid": base,
                "expectedHeadOid": head,
                "expectedPullsOid": pulls,
                "overrideReview": True,
            },
            operation=refresh.UPDATE_OPERATION,
        )


def test_pull_author_self_approval_does_not_unlock_merge(tmp_path):
    bare, base, head, _pulls, _opening = repository(tmp_path)
    cfg = config(tmp_path, bare)
    work = tmp_path / "self-review"
    run(["git", "clone", str(bare), str(work)], tmp_path)
    run(["git", "config", "user.name", "Test"], work)
    run(["git", "config", "user.email", "test@example.invalid"], work)
    run(["git", "switch", "forkmesh/pulls"], work)
    review = work / ".forkmesh/pulls/7/0001-review.md"
    review.write_text(
        review.read_text(encoding="utf-8").replace(
            "author: bob", "author: alice"
        ),
        encoding="utf-8",
    )
    run(["git", "add", ".forkmesh/pulls/7/0001-review.md"], work)
    run(["git", "commit", "-m", "self review"], work)
    run(["git", "push", "origin", "forkmesh/pulls"], work)
    self_reviewed_pulls = git_ref(bare, "refs/heads/forkmesh/pulls")
    req = request(
        cfg,
        base=base,
        head=head,
        pulls=self_reviewed_pulls,
        request_id="merge_request_self_review",
    )

    result = refresh._merge_execute_locked(cfg, req)

    assert result["status"] == "failed"
    assert result["error"] == "review_required"
    assert git_ref(bare, "refs/heads/main") == base


def test_divergent_clean_merge_quarantines_commit_tree_objects(tmp_path):
    bare, base, head, pulls, _opening = repository(tmp_path, divergent=True)
    cfg = config(tmp_path, bare)
    req = request(
        cfg, base=base, head=head, pulls=pulls,
        request_id="merge_request_divergent",
    )

    result = refresh._merge_execute_locked(cfg, req)

    assert result["status"] == "merged"
    assert result["baseAfter"] not in {base, head}
    parents = run([
        "git", "--git-dir", str(bare), "show", "-s", "--format=%P",
        result["baseAfter"],
    ], bare).split()
    assert parents == [base, head]
    assert git_ref(bare, "refs/heads/main") == result["baseAfter"]
    assert_fsck_clean(bare)


def test_stale_exact_oid_and_reused_request_id_fail_closed(tmp_path):
    bare, base, head, pulls, _opening = repository(tmp_path)
    cfg = config(tmp_path, bare)
    stale = request(
        cfg, base=("f" * len(base)), head=head, pulls=pulls,
        request_id="merge_request_stale_1")

    result = refresh._merge_execute_locked(cfg, stale)
    assert result["status"] == "failed"
    assert result["error"] == "stale_base"
    assert git_ref(bare, "refs/heads/main") == base
    assert_fsck_clean(bare)

    changed = dict(stale)
    changed["expectedBaseOid"] = base
    changed["requestDigest"] = "f" * 64
    with pytest.raises(refresh.RefreshError, match="already used"):
        refresh._merge_execute_locked(cfg, changed)


def test_pull_metadata_must_be_branch_backed_and_creation_oid_pinned(tmp_path):
    bare, base, head, pulls, _opening = repository(tmp_path)
    cfg = config(tmp_path, bare)
    req = request(
        cfg, base=base, head=("f" * len(head)), pulls=pulls,
        request_id="merge_request_bad_meta")

    result = refresh._merge_execute_locked(cfg, req)
    assert result["status"] == "failed"
    assert result["error"] == "unsupported_pull"
    assert git_ref(bare, "refs/heads/main") == base
    assert_fsck_clean(bare)


def test_custom_merge_driver_is_rejected_without_execution(tmp_path):
    bare, base, head, pulls, _opening = repository(tmp_path)
    cfg = config(tmp_path, bare)
    marker = tmp_path / "driver-ran"
    (bare / "info" / "attributes").write_text(
        "*.txt merge=evil\n", encoding="utf-8")
    run([
        "git", "--git-dir", str(bare), "config", "merge.evil.driver",
        "/usr/bin/touch " + str(marker),
    ], bare)
    req = request(
        cfg, base=base, head=head, pulls=pulls,
        request_id="merge_request_driver_1")

    result = refresh._merge_execute_locked(cfg, req)

    assert result["status"] == "failed"
    assert result["error"] == "unsupported_pull"
    assert not marker.exists()
    assert git_ref(bare, "refs/heads/main") == base
    assert_fsck_clean(bare)


def test_job_records_contain_no_paths_commands_or_secrets(tmp_path):
    bare, base, head, pulls, _opening = repository(tmp_path)
    cfg = config(tmp_path, bare)
    req = request(cfg, base=base, head=head, pulls=pulls)
    result = refresh._merge_execute_locked(cfg, req)
    raw = refresh._merge_job_path(cfg, req).read_text(encoding="utf-8")
    record = json.loads(raw)
    assert record["status"] == result["status"]
    assert set(record) == {
        "schemaVersion", "type", "requestDigest", "requestId", "status",
        "error", "baseBefore", "head", "pullsBefore", "baseAfter",
        "pullsAfter",
    }
    lowered = raw.lower()
    for word in ("path", "command", "secret", "token", "password"):
        assert word not in lowered
    # Result/idempotency state is owner-only and is not a Git ref or object.
    assert not optional_git_ref(
        bare, "refs/forkmesh/merge-jobs/" + refresh._merge_request_token(req))
    assert_fsck_clean(bare)


def test_cas_race_retains_hidden_anchors_without_partial_metadata(
    tmp_path, monkeypatch
):
    bare, base, head, pulls, _opening = repository(tmp_path)
    cfg = config(tmp_path, bare)
    req = request(
        cfg, base=base, head=head, pulls=pulls,
        request_id="merge_request_cas_race",
    )
    original = refresh._merge_atomic_update

    def race(config_value, request_value, journal):
        run([
            "git", "--git-dir", str(bare), "update-ref",
            "refs/heads/main", head, base,
        ], bare)
        return original(config_value, request_value, journal)

    monkeypatch.setattr(refresh, "_merge_atomic_update", race)
    result = refresh._merge_execute_locked(cfg, req)

    assert result["status"] == "failed"
    assert result["error"] == "stale_pull_metadata"
    assert git_ref(bare, "refs/heads/main") == head
    assert git_ref(bare, "refs/heads/forkmesh/pulls") == pulls
    assert (_pair := refresh._merge_ref_pair(
        cfg, refresh._merge_staging_refs(req)))
    assert _pair[0] == head
    assert _pair[1] != pulls
    assert refresh._merge_ref_pair(
        cfg, refresh._merge_completed_refs(req)) is None
    assert_fsck_clean(bare)


def test_recovery_anchors_a_journaled_interrupted_install_before_fsck(
    tmp_path, monkeypatch
):
    bare, base, head, pulls, _opening = repository(tmp_path)
    cfg = config(tmp_path, bare)
    req = request(
        cfg, base=base, head=head, pulls=pulls,
        request_id="merge_request_recovery",
    )
    metadata, mode = refresh._merge_metadata_blob(cfg, req)
    updated, base_branch, head_branch = refresh._merge_pull_metadata(
        metadata, req, "main")
    original = refresh._merge_ensure_staging_refs
    calls = {"count": 0}

    def interrupt_once(*args, **kwargs):
        calls["count"] += 1
        if calls["count"] == 1:
            raise refresh.RefreshError("simulated interruption")
        return original(*args, **kwargs)

    monkeypatch.setattr(refresh, "_merge_ensure_staging_refs", interrupt_once)
    with pytest.raises(refresh.RefreshError, match="simulated interruption"):
        refresh._merge_prepare_quarantine(
            cfg,
            req,
            base_branch=base_branch,
            head_branch=head_branch,
            updated_metadata=updated,
            metadata_mode=mode,
        )
    assert refresh._merge_quarantine_path(cfg, req).exists()

    refresh._fsck_source(cfg)
    assert refresh._merge_ref_pair(
        cfg, refresh._merge_staging_refs(req))
    assert_fsck_clean(bare)

    result = refresh._merge_execute_locked(cfg, req)
    assert result["status"] == "merged"
    assert_fsck_clean(bare)
