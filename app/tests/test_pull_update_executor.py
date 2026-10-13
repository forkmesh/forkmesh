"""Exact-OID update-from-base plumbing: one branch moves, nothing else does."""

import json
from pathlib import Path
import subprocess
import sys

import pytest

from test_pull_merge_executor import (  # noqa: E402
    assert_fsck_clean,
    config,
    git_ref,
    optional_git_ref,
    repository,
    run,
)


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import headless_mirror_refresh as refresh  # noqa: E402


def request(cfg, *, base, head, pulls, request_id="update_request_0001"):
    value = {
        "schemaVersion": 1,
        "type": refresh.UPDATE_EXECUTOR_TYPE,
        "action": "execute",
        "owner": "forkmesh",
        "repository": "forkmesh",
        "pullNumber": 7,
        "requestId": request_id,
        "expectedBaseOid": base,
        "expectedHeadOid": head,
        "expectedPullsOid": pulls,
    }
    parsed = refresh._merge_request(
        cfg, value, operation=refresh.UPDATE_OPERATION)
    parsed.pop("action")
    return parsed


def test_divergent_base_is_merged_into_the_head_branch_only(tmp_path):
    bare, base, head, pulls, _opening = repository(tmp_path, divergent=True)
    cfg = config(tmp_path, bare)
    req = request(cfg, base=base, head=head, pulls=pulls)
    marker = tmp_path / "hook-ran"
    hook = bare / "hooks" / "reference-transaction"
    hook.write_text(
        "#!/bin/sh\n/usr/bin/touch " + str(marker) + "\n", encoding="utf-8")
    hook.chmod(0o755)

    result = refresh._update_execute_locked(cfg, req)

    assert result["status"] == "updated"
    assert result["baseBefore"] == base
    assert result["head"] == head
    assert result["pullsBefore"] == pulls
    updated = result["headAfter"]
    assert updated not in {base, head}
    # The base branch and the pull-metadata branch are untouched: this mutation
    # publishes exactly one new commit, on the pull request's own branch.
    assert git_ref(bare, "refs/heads/main") == base
    assert git_ref(bare, "refs/heads/forkmesh/pulls") == pulls
    assert git_ref(bare, "refs/heads/feature") == updated
    parents = run([
        "git", "--git-dir", str(bare), "show", "-s", "--format=%P", updated,
    ], bare).split()
    # First parent stays the contributor's own history, as a local merge would.
    assert parents == [head, base]
    metadata = run([
        "git", "--git-dir", str(bare), "show",
        pulls + ":.forkmesh/pulls/7/pull.md",
    ], bare)
    assert "status: open" in metadata
    assert "mergeBase" not in metadata
    assert not marker.exists()
    assert_fsck_clean(bare)

    # A retry is idempotent even though the branch has already moved.
    assert refresh._update_execute_locked(cfg, req) == result


def test_head_already_containing_the_base_is_terminal_and_moves_nothing(
    tmp_path,
):
    bare, base, head, pulls, _opening = repository(tmp_path)
    cfg = config(tmp_path, bare)
    req = request(cfg, base=base, head=head, pulls=pulls)

    result = refresh._update_execute_locked(cfg, req)

    assert result["status"] == "failed"
    assert result["error"] == "already_current"
    assert git_ref(bare, "refs/heads/feature") == head
    assert git_ref(bare, "refs/heads/main") == base
    assert refresh._update_execute_locked(cfg, req) == result
    assert_fsck_clean(bare)


def test_conflict_is_terminal_and_never_moves_the_head_branch(tmp_path):
    bare, base, head, pulls, _opening = repository(tmp_path, conflict=True)
    cfg = config(tmp_path, bare)
    req = request(cfg, base=base, head=head, pulls=pulls)

    result = refresh._update_execute_locked(cfg, req)

    assert result["status"] == "failed"
    assert result["error"] == "merge_conflict"
    assert git_ref(bare, "refs/heads/feature") == head
    assert git_ref(bare, "refs/heads/main") == base
    assert git_ref(bare, "refs/heads/forkmesh/pulls") == pulls
    assert refresh._update_execute_locked(cfg, req) == result
    assert_fsck_clean(bare)


def test_update_needs_no_peer_approval_but_still_pins_every_exact_oid(
    tmp_path,
):
    bare, base, head, pulls, _opening = repository(tmp_path, divergent=True)
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
    unreviewed = git_ref(bare, "refs/heads/forkmesh/pulls")

    # Updating a contributor's own branch is not a merge into the base branch,
    # so it is deliberately not gated on review; the exact OIDs still are.
    ready = refresh._update_execute_locked(
        cfg,
        request(
            cfg, base=base, head=head, pulls=unreviewed,
            request_id="update_request_unreviewed"),
    )
    assert ready["status"] == "updated"

    stale = refresh._update_execute_locked(
        cfg,
        request(
            cfg, base=base, head=head, pulls=pulls,
            request_id="update_request_stale_pulls"),
    )
    assert stale["status"] == "failed"
    assert stale["error"] == "stale_pull_metadata"
    assert_fsck_clean(bare)


def test_stale_and_unsupported_inputs_fail_closed(tmp_path):
    bare, base, head, pulls, _opening = repository(tmp_path, divergent=True)
    cfg = config(tmp_path, bare)

    stale_base = refresh._update_execute_locked(
        cfg,
        request(
            cfg, base="f" * len(base), head=head, pulls=pulls,
            request_id="update_request_stale_base"),
    )
    assert stale_base["error"] == "stale_base"

    stale_head = refresh._update_execute_locked(
        cfg,
        request(
            cfg, base=base, head="f" * len(head), pulls=pulls,
            request_id="update_request_stale_head"),
    )
    assert stale_head["error"] == "stale_head"
    assert git_ref(bare, "refs/heads/feature") == head
    assert git_ref(bare, "refs/heads/main") == base
    assert_fsck_clean(bare)


def test_recycled_head_branch_off_the_recorded_lineage_is_refused(tmp_path):
    bare, base, head, pulls, opening = repository(tmp_path, divergent=True)
    cfg = config(tmp_path, bare)
    work = tmp_path / "recycle"
    run(["git", "clone", str(bare), str(work)], tmp_path)
    run(["git", "config", "user.name", "Test"], work)
    run(["git", "config", "user.email", "test@example.invalid"], work)
    # A different change set pushed over the same branch name: the recorded
    # creationHeadOid is no longer an ancestor, so this is not the pull request
    # the pinned metadata describes.
    run(["git", "switch", "-c", "recycled", opening], work)
    (work / "unrelated.txt").write_text("other\n", encoding="utf-8")
    run(["git", "add", "unrelated.txt"], work)
    run(["git", "commit", "-m", "unrelated work"], work)
    run(["git", "push", "--force", "origin", "recycled:feature"], work)
    recycled = git_ref(bare, "refs/heads/feature")
    assert recycled != head

    result = refresh._update_execute_locked(
        cfg,
        request(
            cfg, base=base, head=recycled, pulls=pulls,
            request_id="update_request_recycled"),
    )

    assert result["status"] == "failed"
    assert result["error"] == "unsupported_pull"
    # The force-push in this setup deliberately orphans the recorded head, so
    # fsck is not asserted here; what matters is that no ref moved.
    assert git_ref(bare, "refs/heads/feature") == recycled
    assert git_ref(bare, "refs/heads/main") == base
    assert git_ref(bare, "refs/heads/forkmesh/pulls") == pulls


def test_custom_merge_driver_is_rejected_without_execution(tmp_path):
    bare, base, head, pulls, _opening = repository(tmp_path, divergent=True)
    cfg = config(tmp_path, bare)
    marker = tmp_path / "driver-ran"
    (bare / "info" / "attributes").write_text(
        "*.txt merge=evil\n", encoding="utf-8")
    run([
        "git", "--git-dir", str(bare), "config", "merge.evil.driver",
        "/usr/bin/touch " + str(marker),
    ], bare)

    result = refresh._update_execute_locked(
        cfg,
        request(
            cfg, base=base, head=head, pulls=pulls,
            request_id="update_request_driver"),
    )

    assert result["status"] == "failed"
    assert result["error"] == "unsupported_pull"
    assert not marker.exists()
    assert git_ref(bare, "refs/heads/feature") == head
    assert_fsck_clean(bare)


def test_job_records_are_namespaced_away_from_merge_and_carry_no_paths(
    tmp_path,
):
    bare, base, head, pulls, _opening = repository(tmp_path, divergent=True)
    cfg = config(tmp_path, bare)
    req = request(cfg, base=base, head=head, pulls=pulls)
    result = refresh._update_execute_locked(cfg, req)

    path = refresh._merge_job_path(
        cfg, req, operation=refresh.UPDATE_OPERATION)
    raw = path.read_text(encoding="utf-8")
    record = json.loads(raw)
    assert record == result
    assert set(record) == {
        "schemaVersion", "type", "requestDigest", "requestId", "status",
        "error", "baseBefore", "head", "pullsBefore", "headAfter",
    }
    assert record["type"] == refresh.UPDATE_JOB_TYPE
    assert path.parent.name == "update-jobs"
    lowered = raw.lower()
    for word in ("path", "command", "secret", "token", "password"):
        assert word not in lowered
    # A merge with the same idempotency id must not be able to read this state.
    merge_request = dict(req)
    assert refresh._merge_job_record(
        cfg, merge_request, operation=refresh.MERGE_OPERATION) is None
    completed = refresh._merge_completed_refs(
        req, operation=refresh.UPDATE_OPERATION)
    assert len(completed) == 1
    assert git_ref(bare, completed[0]) == result["headAfter"]
    assert not optional_git_ref(
        bare,
        refresh._merge_completed_refs(
            req, operation=refresh.MERGE_OPERATION)[0],
    )
    assert_fsck_clean(bare)


def test_cas_race_keeps_the_speculative_commit_anchored(tmp_path, monkeypatch):
    bare, base, head, pulls, _opening = repository(tmp_path, divergent=True)
    cfg = config(tmp_path, bare)
    req = request(
        cfg, base=base, head=head, pulls=pulls,
        request_id="update_request_cas_race")
    original = refresh._update_atomic_update

    def race(config_value, request_value, journal):
        run([
            "git", "--git-dir", str(bare), "update-ref",
            "refs/heads/feature", base, head,
        ], bare)
        return original(config_value, request_value, journal)

    monkeypatch.setattr(refresh, "_update_atomic_update", race)
    result = refresh._update_execute_locked(cfg, req)

    assert result["status"] == "failed"
    assert result["error"] == "stale_head"
    assert git_ref(bare, "refs/heads/feature") == base
    staging = refresh._merge_ref_pair(
        cfg,
        refresh._merge_staging_refs(req, operation=refresh.UPDATE_OPERATION),
    )
    assert staging is not None
    assert refresh._merge_ref_pair(
        cfg,
        refresh._merge_completed_refs(
            req, operation=refresh.UPDATE_OPERATION),
    ) is None
    assert_fsck_clean(bare)


def test_executor_entry_point_requires_the_advertised_capability(tmp_path):
    bare, base, head, pulls, _opening = repository(tmp_path, divergent=True)
    cfg = config(tmp_path, bare)
    value = {
        "schemaVersion": 1,
        "type": refresh.UPDATE_EXECUTOR_TYPE,
        "action": "status",
        "owner": "forkmesh",
        "repository": "forkmesh",
        "pullNumber": 7,
        "requestId": "update_request_capability",
        "expectedBaseOid": base,
        "expectedHeadOid": head,
        "expectedPullsOid": pulls,
    }
    cfg.operations = ("tree", "blob", "raw")
    with pytest.raises(refresh.RefreshError, match="capability is disabled"):
        refresh.update_executor(cfg, value)
    # The shared stdin entry point routes strictly by declared type, so
    # advertising one mutation never enables the other.
    cfg.operations = ("tree", "blob", "raw", "update-pull-branch")
    with pytest.raises(refresh.RefreshError, match="capability is disabled"):
        refresh.pull_executor(
            cfg, dict(value, type=refresh.MERGE_EXECUTOR_TYPE))
    cfg.operations = ("tree", "blob", "raw", "merge-pull")
    with pytest.raises(refresh.RefreshError, match="capability is disabled"):
        refresh.pull_executor(cfg, value)
    # A merge-typed body can never satisfy the update request validator.
    cfg.operations = ("tree", "blob", "raw", "update-pull-branch")
    with pytest.raises(refresh.RefreshError, match="protocol is unsupported"):
        refresh.update_executor(
            cfg, dict(value, type=refresh.MERGE_EXECUTOR_TYPE))


def test_gateway_config_advertises_one_executor_for_both_mutations():
    source = (ROOT / "tools" / "mirror_gateway.py").read_text(encoding="utf-8")
    assert "update-pull-branch requires an operator-owned" in source
    assert "update-pull-branch" in refresh.PUBLIC_OPERATIONS
    schema = json.loads(
        (ROOT / "www" / "docs" / "headless-mirror-refresh.schema.json")
        .read_text(encoding="utf-8"))
    operations = schema["properties"]["operations"]
    assert "update-pull-branch" in operations["items"]["enum"]
    assert "merge-preview" in operations["items"]["enum"]
    assert operations["maxItems"] == len(refresh.PUBLIC_OPERATIONS)


def test_update_and_merge_transactions_use_disjoint_ref_namespaces():
    request_value = {"requestId": "update_request_namespace"}
    update_staging = refresh._merge_staging_refs(
        request_value, operation=refresh.UPDATE_OPERATION)
    merge_staging = refresh._merge_staging_refs(
        request_value, operation=refresh.MERGE_OPERATION)
    assert not set(update_staging) & set(merge_staging)
    assert all(
        ref.startswith("refs/forkmesh/update-staging/")
        for ref in update_staging
    )
    assert subprocess.run  # imported for the helpers above
