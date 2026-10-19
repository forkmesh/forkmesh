#!/usr/bin/env python3
"""Sync owner-reviewed issue and pull reward estimates into production D1.

The repository metadata is the review record. D1 is the public payment queue
and receipt index used by the ledger and Qt app. This command is a dry run
unless ``--apply`` is supplied; it never rewrites an already submitted or paid
receipt.
"""

from __future__ import annotations

import argparse
import hashlib
import hmac
import json
import os
import sys
import re
import subprocess
import tempfile
from pathlib import Path


APP_ROOT = Path(__file__).resolve().parents[1]
REPOSITORY_ROOT = APP_ROOT.parent
ISSUES_ROOT = REPOSITORY_ROOT / ".forkmesh" / "issues"
NAME_RE = re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")
REWARD_ID_RE = re.compile(r"^reward-[a-z0-9-]{8,88}$")
VALID_STATUSES = {
    "pending_merge", "pending_wallet", "ready", "submitted", "paid",
    "cancelled",
}
SUBJECT_SUMMARY_MAX = 360
MINIMUM_REWARD_LAMPORTS = 1_000_000
MAXIMUM_ISSUE_REWARD_LAMPORTS = 5_000_000
MAXIMUM_PULL_REWARD_LAMPORTS = 25_000_000
# The desktop's served mirror holds every forkmesh/pull/<n> branch; the primary
# checkout holds only the ones it fetched or wrote itself.
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import forkmesh_paths  # noqa: E402  (shared resolver lives with the tools)

DEFAULT_MIRROR_GIT_DIR = (
    forkmesh_paths.resolve(forkmesh_paths.HOME_PROJECT).data_path("mirrors")
    / "forkmesh-forkmesh.git")
PULL_REF_PREFIX = "refs/heads/forkmesh/pull/"
PULL_REF_RE = re.compile(r"^refs/heads/forkmesh/pull/([1-9][0-9]{0,8})$")
PULL_METADATA_REF_PREFIX = "refs/pr/"
PULL_METADATA_REF_RE = re.compile(r"^refs/pr/([1-9][0-9]{0,8})/metadata$")
PULL_RECORD_RE = re.compile(r"^\.forkmesh/pulls/([1-9][0-9]{0,8})/pull\.md$")
UNSETTLED_STATUSES = ("pending_merge", "pending_wallet", "ready", "submitted")


def _load_env(path: Path) -> dict[str, str]:
    environment = dict(os.environ)
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        environment.setdefault(
            key.strip(), value.strip().strip('"').strip("'"))
    return environment


def _blind_key(data_key: str) -> bytes:
    return hashlib.sha256(
        (data_key.strip() + ":blind-index").encode("utf-8")).digest()


def _blind(key: bytes, value: str) -> str:
    normalized = str(value or "").strip().lower().encode("utf-8")
    return hmac.new(key, normalized, hashlib.sha256).hexdigest()


def _frontmatter(text: str) -> dict[str, str]:
    if not text.startswith("---\n"):
        return {}
    header = text.split("\n---\n", 1)[0].splitlines()[1:]
    values = {}
    for line in header:
        if ":" not in line:
            continue
        key, value = line.split(":", 1)
        values[key.strip()] = value.strip()
    return values


def _document_body(text: str) -> str:
    if not text.startswith("---\n") or "\n---\n" not in text:
        return text
    return text.split("\n---\n", 1)[1]


def _compact_subject_summary(text: str) -> str:
    """Keep a small readable synopsis, never a source record's full body."""
    value = str(text or "")
    value = re.sub(r"```.*?```", " ", value, flags=re.DOTALL)
    value = re.sub(r"!\[[^]]*\]\([^)]*\)", " ", value)
    value = re.sub(r"\[([^]]+)\]\([^)]*\)", r"\1", value)
    value = re.sub(r"<[^>]+>", " ", value)
    value = re.sub(r"(?m)^\s{0,3}(?:#{1,6}|[-*+]\s|>\s?)", " ", value)
    value = re.sub(r"[`*_~]", "", value)
    value = re.sub(r"\s+", " ", value).strip()
    if len(value) <= SUBJECT_SUMMARY_MAX:
        return value
    clipped = value[:SUBJECT_SUMMARY_MAX - 1].rsplit(" ", 1)[0].rstrip()
    return (clipped or value[:SUBJECT_SUMMARY_MAX - 1]).rstrip() + "…"


def _issue_rewards() -> list[dict]:
    rewards = []
    for path in sorted(ISSUES_ROOT.glob("*/*/issue-*.json")):
        record = json.loads(path.read_text(encoding="utf-8"))
        if not record.get("rewardId"):
            continue
        opening = next(
            (event for event in record.get("events", [])
             if isinstance(event, dict) and event.get("type") == "open"),
            {},
        )
        rewards.append({
            "reward_id": record.get("rewardId"),
            "recipient_name": record.get("rewardRecipient"),
            "repo_owner": "forkmesh",
            "repo_name": "forkmesh",
            "source_kind": "issue",
            "subject_number": record.get("number"),
            "subject_title": record.get("title"),
            "subject_summary": _compact_subject_summary(
                opening.get("body")),
            "amount_lamports": record.get("rewardEstimateLamports"),
            "condition": record.get("rewardCondition"),
            "status": record.get("rewardStatus"),
            "created_at": record.get("createdAt"),
            "updated_at": record.get("updatedAt"),
        })
    return rewards


def _merge_settled_status(record: dict[str, str]) -> str:
    """The reward status a pull's own record implies.

    A merge-conditional estimate stops waiting the moment the pull lands, so a
    merged pull must never still read ``pending_merge`` — that status is what
    holds the reward out of the payout queue and shows "pending merge" on the
    public ledger. Merging records this itself now; deriving it here as well
    heals the records written before it did, without a hand-edit per pull.
    """
    status = str(record.get("rewardStatus") or "").strip()
    if status != "pending_merge":
        return status
    if str(record.get("rewardCondition") or "").strip() != "merged":
        return status
    if str(record.get("status") or "").strip().lower() != "merged":
        return status
    return "pending_wallet"


def _git(repository: Path, *arguments: str) -> str:
    """One read-only git command's output, or "" when it fails."""
    result = subprocess.run(
        ["git", "-C", str(repository), *arguments],
        capture_output=True, text=True,
    )
    return result.stdout if result.returncode == 0 else ""


def _pull_record_texts(branch: str,
                       repositories: list[Path]) -> dict[int, str]:
    """The authoritative ``pull.md`` text for every pull that has one.

    Pull records moved off the aggregate ``forkmesh/pulls`` branch onto one
    ``forkmesh/pull/<n>`` branch per pull, and the aggregate tip no longer
    carries them. Reading only that branch silently dropped every merge made
    since, which is how landed pulls stayed "pays on merge" on the ledger. In
    order of authority:

    1. the pull's own branch, the newest commit across ``repositories``;
    2. the aggregate branch tip, for a record that was never moved;
    3. the last copy before the aggregate branch deleted the record, only when
       that copy is a merged pull carrying a reward. The work landed and its
       estimate was already on the ledger; deleting the pull's metadata later
       must not strand the payment.
    """
    newest: dict[int, tuple[int, str]] = {}
    for repository in repositories:
        listing = _git(repository, "for-each-ref",
                       "--format=%(refname) %(committerdate:unix)",
                       PULL_REF_PREFIX, PULL_METADATA_REF_PREFIX)
        for line in listing.splitlines():
            ref, _, stamp = line.strip().partition(" ")
            match = (PULL_REF_RE.fullmatch(ref)
                     or PULL_METADATA_REF_RE.fullmatch(ref))
            if not match:
                continue
            number = int(match.group(1))
            text = _git(repository, "show",
                        "%s:.forkmesh/pulls/%d/pull.md" % (ref, number))
            if not text:
                continue
            committed = int(stamp) if stamp.strip().isdigit() else 0
            if number not in newest or committed > newest[number][0]:
                newest[number] = (committed, text)
    texts = {number: text for number, (_, text) in newest.items()}

    for repository in repositories:
        if not _git(repository, "rev-parse", "--verify", "-q",
                    branch + "^{commit}"):
            continue
        for path in _git(repository, "ls-tree", "-r", "--name-only", branch,
                         ".forkmesh/pulls").splitlines():
            match = PULL_RECORD_RE.fullmatch(path.strip())
            if not match or int(match.group(1)) in texts:
                continue
            text = _git(repository, "show", branch + ":" + path.strip())
            if text:
                texts[int(match.group(1))] = text
        deletions = _git(repository, "log", "--format=commit %H",
                         "--name-only", "--diff-filter=D", branch, "--",
                         ".forkmesh/pulls")
        commit = ""
        seen = set()
        for line in deletions.splitlines():
            line = line.strip()
            if line.startswith("commit "):
                commit = line[len("commit "):].strip()
                continue
            match = PULL_RECORD_RE.fullmatch(line)
            if not match or not commit:
                continue
            number = int(match.group(1))
            # Newest deletion first: only the copy it removed is considered.
            if number in seen or number in texts:
                continue
            seen.add(number)
            text = _git(repository, "show", "%s^:%s" % (commit, line))
            record = _frontmatter(text)
            if (str(record.get("status") or "").strip().lower() == "merged"
                    and record.get("rewardId")):
                texts[number] = text
        # One aggregate history is the record; a second copy of the same
        # branch elsewhere adds nothing but disagreement.
        break
    return texts


def _pull_rewards(branch: str,
                  repositories: list[Path] | None = None) -> list[dict]:
    texts = _pull_record_texts(branch, repositories or [REPOSITORY_ROOT])
    rewards = []
    for number in sorted(texts):
        text = texts[number]
        record = _frontmatter(text)
        if not record.get("rewardId"):
            continue
        rewards.append({
            "reward_id": record.get("rewardId"),
            "recipient_name": record.get("rewardRecipient"),
            "repo_owner": "forkmesh",
            "repo_name": "forkmesh",
            "source_kind": "pull",
            "subject_number": record.get("number"),
            "subject_title": record.get("title"),
            "subject_summary": _compact_subject_summary(
                _document_body(text)),
            "amount_lamports": record.get("rewardEstimateLamports"),
            "condition": record.get("rewardCondition"),
            "status": _merge_settled_status(record),
            "created_at": record.get("ts"),
            "updated_at": record.get("ts"),
        })
    return rewards


def _validated(rewards: list[dict], data_key: str) -> list[dict]:
    if not data_key.strip():
        raise RuntimeError("DATA_KEY is required")
    key = _blind_key(data_key)
    reward_ids = set()
    subjects = set()
    output = []
    for item in rewards:
        reward = dict(item)
        reward_id = str(reward.get("reward_id") or "").strip().lower()
        recipient = str(reward.get("recipient_name") or "").strip().lower()
        source_kind = str(reward.get("source_kind") or "")
        condition = str(reward.get("condition") or "")
        status = str(reward.get("status") or "")
        number = int(reward.get("subject_number") or 0)
        amount = int(reward.get("amount_lamports") or 0)
        created_at = max(0, int(reward.get("created_at") or 0))
        updated_at = max(created_at, int(reward.get("updated_at") or 0))
        subject = (reward["repo_owner"], reward["repo_name"], source_kind,
                   number)
        if not REWARD_ID_RE.fullmatch(reward_id):
            raise ValueError("invalid reward id: " + reward_id)
        if not NAME_RE.fullmatch(recipient):
            raise ValueError("invalid recipient for " + reward_id)
        if source_kind not in ("issue", "pull") or number <= 0:
            raise ValueError("invalid subject for " + reward_id)
        expected_condition = (
            "accepted_report" if source_kind == "issue" else "merged")
        if condition != expected_condition:
            raise ValueError(
                "condition does not match source for " + reward_id)
        if status not in VALID_STATUSES:
            raise ValueError("invalid status for " + reward_id)
        maximum_amount = (
            MAXIMUM_PULL_REWARD_LAMPORTS
            if source_kind == "pull" else MAXIMUM_ISSUE_REWARD_LAMPORTS)
        if not MINIMUM_REWARD_LAMPORTS <= amount <= maximum_amount:
            raise ValueError(
                "estimate outside the allowed range for " + source_kind
                + ": " + reward_id)
        if reward_id in reward_ids or subject in subjects:
            raise ValueError("duplicate reward id or subject: " + reward_id)
        reward_ids.add(reward_id)
        subjects.add(subject)
        reward.update({
            "reward_id": reward_id,
            "recipient_name": recipient,
            "recipient_bi": _blind(key, recipient),
            "subject_number": number,
            "subject_title": str(reward.get("subject_title") or "")[:500],
            "subject_summary": _compact_subject_summary(
                reward.get("subject_summary")),
            "amount_lamports": amount,
            "created_at": created_at,
            "updated_at": updated_at,
        })
        output.append(reward)
    return sorted(output, key=lambda row: row["reward_id"])


def _sql_text(value) -> str:
    return "'" + str(value or "").replace("'", "''") + "'"


def _upsert_sql(rewards: list[dict]) -> str:
    # Wrangler's remote D1 file importer owns rollback for the uploaded batch
    # and rejects explicit SQL BEGIN/SAVEPOINT statements.
    statements = []
    columns = (
        "reward_id", "recipient_bi", "recipient_name", "repo_owner",
        "repo_name", "source_kind", "subject_number", "subject_title",
        "subject_summary", "amount_lamports", "condition", "status",
        "created_at", "updated_at",
    )
    for reward in rewards:
        values = []
        for column in columns:
            value = reward[column]
            values.append(
                str(value) if isinstance(value, int) else _sql_text(value))
        statements.append(
            "INSERT INTO contributor_reward_estimates ("
            + ",".join(columns) + ") VALUES (" + ",".join(values) + ") "
            "ON CONFLICT(reward_id) DO UPDATE SET "
            # The relay records a Discord report's estimate under the number
            # it proposed; mirror intake can file the issue under the next
            # free one. The issue record is authoritative, so follow it.
            "subject_number=CASE WHEN contributor_reward_estimates.status IN "
            "('submitted','paid') THEN contributor_reward_estimates.subject_number "
            "ELSE excluded.subject_number END,"
            "recipient_bi=CASE WHEN contributor_reward_estimates.status IN "
            "('submitted','paid') THEN contributor_reward_estimates.recipient_bi "
            "ELSE excluded.recipient_bi END,"
            "recipient_name=CASE WHEN contributor_reward_estimates.status IN "
            "('submitted','paid') THEN contributor_reward_estimates.recipient_name "
            "ELSE excluded.recipient_name END,"
            "subject_title=CASE WHEN contributor_reward_estimates.status IN "
            "('submitted','paid') THEN contributor_reward_estimates.subject_title "
            "ELSE excluded.subject_title END,"
            "subject_summary=CASE WHEN contributor_reward_estimates.status IN "
            "('submitted','paid') THEN contributor_reward_estimates.subject_summary "
            "ELSE excluded.subject_summary END,"
            "amount_lamports=CASE WHEN contributor_reward_estimates.status IN "
            "('submitted','paid') THEN contributor_reward_estimates.amount_lamports "
            "ELSE excluded.amount_lamports END,"
            "condition=CASE WHEN contributor_reward_estimates.status IN "
            "('submitted','paid') THEN contributor_reward_estimates.condition "
            "ELSE excluded.condition END,"
            "status=CASE WHEN contributor_reward_estimates.status IN "
            "('submitted','paid') THEN contributor_reward_estimates.status "
            "ELSE excluded.status END,"
            "created_at=MIN(contributor_reward_estimates.created_at,"
            "excluded.created_at),"
            "updated_at=CASE WHEN contributor_reward_estimates.status IN "
            "('submitted','paid') THEN contributor_reward_estimates.updated_at "
            "ELSE MAX(contributor_reward_estimates.updated_at,"
            "excluded.updated_at) END;"
        )
    return "\n".join(statements) + "\n"


def _apply(database: str, sql: str, environment: dict[str, str]) -> None:
    path = ""
    try:
        with tempfile.NamedTemporaryFile(
                mode="w", encoding="utf-8", suffix=".sql",
                prefix="forkmesh-contributor-rewards-", delete=False) as handle:
            handle.write(sql)
            path = handle.name
        subprocess.run(
            [
                "bash", "-c",
                '. ./pywrangler.sh; pywrangler d1 execute "$1" --remote '
                '--file "$2"',
                "sync-contributor-rewards", database, path,
            ],
            cwd=APP_ROOT, env=environment, check=True,
        )
    finally:
        if path:
            Path(path).unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--database", default="forkmesh")
    parser.add_argument("--env-file", type=Path,
                        default=APP_ROOT / ".env.production")
    parser.add_argument("--pull-branch", default="forkmesh/pulls")
    parser.add_argument(
        "--pull-repository", action="append", type=Path, default=None,
        help=("git directory holding forkmesh/pull/<n> branches; repeatable "
              "(default: the served mirror, then this checkout)"))
    arguments = parser.parse_args()

    repositories = arguments.pull_repository or [
        path for path in (DEFAULT_MIRROR_GIT_DIR, REPOSITORY_ROOT)
        if path.exists()
    ]
    environment = _load_env(arguments.env_file)
    rewards = _validated(
        _issue_rewards() + _pull_rewards(arguments.pull_branch, repositories),
        environment.get("DATA_KEY", ""),
    )
    kinds = {kind: sum(row["source_kind"] == kind for row in rewards)
             for kind in ("issue", "pull")}
    statuses = {
        status: sum(row["status"] == status for row in rewards)
        for status in sorted({row["status"] for row in rewards})
    }
    amount = sum(row["amount_lamports"] for row in rewards)
    print(json.dumps({
        "ok": True,
        "mode": "apply" if arguments.apply else "dry-run",
        "rewards": len(rewards),
        "kinds": kinds,
        "statuses": statuses,
        "totalLamports": amount,
        # What a dry run would leave waiting, so a reviewer can check the
        # rows that matter before --apply.
        "unsettled": [
            {"rewardId": row["reward_id"], "status": row["status"],
             "recipient": row["recipient_name"],
             "amountLamports": row["amount_lamports"]}
            for row in rewards if row["status"] in UNSETTLED_STATUSES
        ],
    }, sort_keys=True))
    if arguments.apply:
        _apply(arguments.database, _upsert_sql(rewards), environment)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
