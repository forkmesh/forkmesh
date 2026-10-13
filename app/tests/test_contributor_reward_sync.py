"""The reward sync reads a merged pull's estimate as no longer pending.

Regression for adhoc #86: pull #32 merged while its ``rewardStatus`` stayed
``pending_merge``, so the public ledger kept printing "PENDING MERGE" and the
payout queue kept treating landed work as not yet payable. Merging records the
transition in the pull itself now; the sync derives the same thing so already
written records heal on the next run instead of needing a hand-edit each.
"""

import importlib.util
from pathlib import Path

import pytest


REPOSITORY = Path(__file__).resolve().parents[2]
MODULE = REPOSITORY / "app/tools/sync_contributor_rewards.py"
SPEC = importlib.util.spec_from_file_location("sync_contributor_rewards", MODULE)
sync_contributor_rewards = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(sync_contributor_rewards)


def record(**overrides):
    fields = {
        "number": "32",
        "status": "merged",
        "rewardId": "reward-forkmesh-forkmesh-pull-32",
        "rewardRecipient": "atomic-gateway-7974",
        "rewardEstimateLamports": "4000000",
        "rewardCondition": "merged",
        "rewardStatus": "pending_merge",
    }
    fields.update(overrides)
    return fields


def test_merged_pull_leaves_pending_merge():
    assert sync_contributor_rewards._merge_settled_status(
        record()) == "pending_wallet"


def test_open_pull_keeps_waiting_on_the_merge():
    assert sync_contributor_rewards._merge_settled_status(
        record(status="open")) == "pending_merge"
    assert sync_contributor_rewards._merge_settled_status(
        record(status="closed")) == "pending_merge"


def test_a_settled_estimate_is_never_walked_backwards():
    for status in ("pending_wallet", "ready", "submitted", "paid", "cancelled"):
        assert sync_contributor_rewards._merge_settled_status(
            record(rewardStatus=status)) == status


def test_an_estimate_conditioned_on_something_else_is_left_alone():
    assert sync_contributor_rewards._merge_settled_status(
        record(rewardCondition="accepted_report")) == "pending_merge"
    assert sync_contributor_rewards._merge_settled_status(
        record(rewardCondition="")) == "pending_merge"


def test_the_frontmatter_the_desktop_writes_parses_into_that_check():
    pull_md = (
        "---\n"
        "schema: forkmesh-pull-v1\n"
        "number: 32\n"
        "title: Fix Qt 6.4 compatibility\n"
        "base: main\n"
        "head: atomic-gateway-7974:fix/qt6.4-compat\n"
        "status: merged\n"
        "ts: 1786624615723\n"
        "rewardEstimateLamports: 4000000\n"
        "rewardRecipient: atomic-gateway-7974\n"
        "rewardId: reward-forkmesh-forkmesh-pull-32\n"
        "rewardCondition: merged\n"
        "rewardStatus: pending_merge\n"
        "---\n"
        "\n## Change\n"
    )

    fields = sync_contributor_rewards._frontmatter(pull_md)

    assert fields["status"] == "merged"
    assert fields["rewardStatus"] == "pending_merge"
    assert sync_contributor_rewards._merge_settled_status(
        fields) == "pending_wallet"


def test_the_derived_status_still_passes_validation():
    rewards = sync_contributor_rewards._validated(
        [{
            "reward_id": "reward-forkmesh-forkmesh-pull-32",
            "recipient_name": "atomic-gateway-7974",
            "repo_owner": "forkmesh",
            "repo_name": "forkmesh",
            "source_kind": "pull",
            "subject_number": 32,
            "subject_title": "Fix Qt 6.4 compatibility",
            "subject_summary": (
                "## Why\nKeep enough context to understand the work without "
                "copying every comment."),
            "amount_lamports": 4000000,
            "condition": "merged",
            "status": sync_contributor_rewards._merge_settled_status(record()),
            "created_at": 1786624615723,
            "updated_at": 1786624615723,
        }],
        "test-data-key",
    )

    assert rewards[0]["status"] == "pending_wallet"
    assert rewards[0]["subject_summary"] == (
        "Why Keep enough context to understand the work without copying "
        "every comment.")
    # An already-paid receipt is never rewritten by an estimate upsert.
    upsert = sync_contributor_rewards._upsert_sql(rewards)
    assert "'pending_wallet'" in upsert
    assert "THEN contributor_reward_estimates.status" in upsert
    assert "THEN contributor_reward_estimates.subject_summary" in upsert
    assert "THEN contributor_reward_estimates.amount_lamports" in upsert
    assert "THEN contributor_reward_estimates.recipient_name" in upsert


def test_pull_rewards_can_be_larger_than_issue_report_rewards():
    base = {
        "reward_id": "reward-forkmesh-forkmesh-pull-36",
        "recipient_name": "gaurav2631",
        "repo_owner": "forkmesh",
        "repo_name": "forkmesh",
        "source_kind": "pull",
        "subject_number": 36,
        "subject_title": "Report a pull delivery failure",
        "subject_summary": "A reviewed pull reward.",
        "amount_lamports": 20_000_000,
        "condition": "merged",
        "status": "pending_merge",
        "created_at": 1786636663064,
        "updated_at": 1786636663064,
    }

    rewards = sync_contributor_rewards._validated([base], "test-data-key")
    assert rewards[0]["amount_lamports"] == 20_000_000

    issue = dict(base, reward_id="reward-forkmesh-forkmesh-issue-36",
                 source_kind="issue", condition="accepted_report")
    with pytest.raises(ValueError, match="allowed range for issue"):
        sync_contributor_rewards._validated([issue], "test-data-key")

    oversized_pull = dict(base, amount_lamports=25_000_001)
    with pytest.raises(ValueError, match="allowed range for pull"):
        sync_contributor_rewards._validated(
            [oversized_pull], "test-data-key")


@pytest.mark.parametrize(
    ("source_kind", "condition"),
    (("issue", "merged"), ("pull", "accepted_report")),
)
def test_validation_rejects_a_condition_for_the_wrong_source(
        source_kind, condition):
    with pytest.raises(
            ValueError, match="condition does not match source"):
        sync_contributor_rewards._validated(
            [{
                "reward_id": "reward-forkmesh-forkmesh-invalid-source",
                "recipient_name": "atomic-gateway-7974",
                "repo_owner": "forkmesh",
                "repo_name": "forkmesh",
                "source_kind": source_kind,
                "subject_number": 344,
                "subject_title": "An owner-reviewed reward",
                "subject_summary": "The condition must match the source.",
                "amount_lamports": 4_000_000,
                "condition": condition,
                "status": "pending_wallet",
                "created_at": 1783093191964,
                "updated_at": 1783093191964,
            }],
            "test-data-key",
        )


def test_subject_summary_is_plain_text_and_strictly_bounded():
    summary = sync_contributor_rewards._compact_subject_summary(
        "# Context\n[A useful link](https://example.com) " + "detail " * 100)

    assert "https://" not in summary
    assert "[" not in summary and "#" not in summary
    assert len(summary) <= sync_contributor_rewards.SUBJECT_SUMMARY_MAX
    assert summary.endswith("…")


# --- where pull records are read from ----------------------------------------
#
# Pull records moved onto one forkmesh/pull/<n> branch per pull, and the
# aggregate forkmesh/pulls tip stopped carrying them. A sync that read only the
# aggregate tip never saw another merge, so landed pulls stayed "pays on merge".

import os
import subprocess


def _pull_md(number, status, reward=True, reward_status="pending_merge",
             title="Work"):
    lines = [
        "---",
        "schema: forkmesh-pull-v1",
        "number: %d" % number,
        "title: %s" % title,
        "status: %s" % status,
        "ts: 1786624615723",
    ]
    if reward:
        lines += [
            "rewardEstimateLamports: 4000000",
            "rewardRecipient: gaurav2631",
            "rewardId: reward-forkmesh-forkmesh-pull-%d" % number,
            "rewardCondition: merged",
            "rewardStatus: %s" % reward_status,
        ]
    return "\n".join(lines + ["---", "", "## Change", ""])


def _git(repository, *arguments, when="1786600000"):
    environment = dict(
        os.environ,
        GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@example.com",
        GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@example.com",
        GIT_AUTHOR_DATE=when + " +0000", GIT_COMMITTER_DATE=when + " +0000",
    )
    return subprocess.run(
        ["git", "-C", str(repository), *arguments], check=True,
        capture_output=True, text=True, env=environment).stdout


def _write(repository, number, text):
    path = repository / ".forkmesh" / "pulls" / str(number) / "pull.md"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def _pull_branch(repository, number, text, when="1786700000"):
    _git(repository, "checkout", "-q", "--orphan", "forkmesh/pull/%d" % number)
    _git(repository, "rm", "-rfq", "--ignore-unmatch", ".")
    _write(repository, number, text)
    _git(repository, "add", "-A")
    _git(repository, "commit", "-qm", "pull #%d" % number, when=when)


def _repository(tmp_path, name="repo"):
    repository = tmp_path / name
    repository.mkdir()
    _git(repository, "init", "-q", "-b", "forkmesh/pulls")
    # The aggregate ledger: 40 merged then deleted, 41 closed then deleted,
    # 45 merged without a reward then deleted, and 50 still at the tip (open).
    for number, status, reward in ((40, "merged", True), (41, "closed", True),
                                   (45, "merged", False), (50, "open", True)):
        _write(repository, number, _pull_md(number, status, reward))
        _git(repository, "add", "-A")
        _git(repository, "commit", "-qm", "pull #%d" % number)
    _git(repository, "rm", "-rq", ".forkmesh/pulls/40", ".forkmesh/pulls/41",
         ".forkmesh/pulls/45")
    _git(repository, "commit", "-qm", "pulls: deleted #40, #41, #45")
    _pull_branch(repository, 14, _pull_md(14, "merged"))
    # The pull's own branch is authoritative over the aggregate copy.
    _pull_branch(repository, 50, _pull_md(50, "merged"))
    return repository


def test_pull_rewards_come_from_per_pull_branches_and_the_aggregate_history(
        tmp_path):
    repository = _repository(tmp_path)

    rewards = {
        int(row["subject_number"]): row
        for row in sync_contributor_rewards._pull_rewards(
            "forkmesh/pulls", [repository])
    }

    assert sorted(rewards) == [14, 40, 50]
    assert rewards[14]["status"] == "pending_wallet"
    assert rewards[50]["status"] == "pending_wallet"
    # Deleted after it merged: its estimate still leaves pending_merge.
    assert rewards[40]["status"] == "pending_wallet"
    assert rewards[40]["reward_id"] == "reward-forkmesh-forkmesh-pull-40"


def test_a_deleted_pull_that_never_merged_is_not_resurrected(tmp_path):
    repository = _repository(tmp_path)

    numbers = {
        int(row["subject_number"])
        for row in sync_contributor_rewards._pull_rewards(
            "forkmesh/pulls", [repository])
    }

    assert 41 not in numbers  # closed, then deleted
    assert 45 not in numbers  # merged, but it never carried a reward


def test_a_reused_pull_number_does_not_inherit_the_old_pulls_reward(tmp_path):
    """Pull #40 on production: a rewarded pull closed and was deleted, and
    the number was reused by an unrelated pull that merged without a reward.
    Only the newest deleted copy counts, and it carries no reward."""
    repository = tmp_path / "reused"
    repository.mkdir()
    _git(repository, "init", "-q", "-b", "forkmesh/pulls")
    _write(repository, 40, _pull_md(40, "closed", title="Feed filter"))
    _git(repository, "add", "-A")
    _git(repository, "commit", "-qm", "pull #40: closed")
    _git(repository, "rm", "-rq", ".forkmesh/pulls/40")
    _git(repository, "commit", "-qm", "pull #40: deleted")
    _write(repository, 40, _pull_md(40, "merged", reward=False,
                                    title="Mobile type scale"))
    _git(repository, "add", "-A")
    _git(repository, "commit", "-qm", "pull #40: merged")
    _git(repository, "rm", "-rq", ".forkmesh/pulls/40")
    _git(repository, "commit", "-qm", "pull #40: deleted")

    assert sync_contributor_rewards._pull_rewards(
        "forkmesh/pulls", [repository]) == []


def test_the_newest_per_pull_branch_wins_across_repositories(tmp_path):
    older = _repository(tmp_path, "checkout")
    newer = tmp_path / "mirror"
    newer.mkdir()
    _git(newer, "init", "-q", "-b", "scratch")
    _pull_branch(newer, 14, _pull_md(14, "merged", reward_status="paid"),
                 when="1786900000")

    for order in ([older, newer], [newer, older]):
        rewards = {
            int(row["subject_number"]): row
            for row in sync_contributor_rewards._pull_rewards(
                "forkmesh/pulls", order)
        }
        assert rewards[14]["status"] == "paid"
        assert rewards[40]["status"] == "pending_wallet"


def test_a_merged_pull_whose_branch_was_retired_reads_its_metadata_ref(
        tmp_path):
    repository = _repository(tmp_path)
    _pull_branch(repository, 36, _pull_md(36, "merged"), when="1786800000")
    tip = _git(repository, "rev-parse", "forkmesh/pull/36").strip()
    _git(repository, "update-ref", "refs/pr/36/metadata", tip)
    _git(repository, "checkout", "-q", "forkmesh/pulls")
    _git(repository, "update-ref", "-d", "refs/heads/forkmesh/pull/36", tip)

    rewards = {
        int(row["subject_number"]): row
        for row in sync_contributor_rewards._pull_rewards(
            "forkmesh/pulls", [repository])
    }

    assert rewards[36]["status"] == "pending_wallet"
    assert rewards[36]["reward_id"] == "reward-forkmesh-forkmesh-pull-36"
