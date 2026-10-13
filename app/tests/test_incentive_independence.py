#!/usr/bin/env python3
"""Tokens and experimental incentives stay strictly optional (issue #516).

Every requirement in ``conformance/incentives/v1/requirements.json`` names a
test here; the last test keeps that file, the invariant documentation, the
commercial contract, and the protocol specification in agreement.
"""

import ast
import json
from pathlib import Path
import re
import sqlite3
import sys


ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "app" / "src"
sys.path.insert(0, str(SRC))

from schema import SCHEMA_STATEMENTS  # noqa: E402


ADAPTER_MODULES = frozenset((
    "solana", "reward_policy", "ledger", "microjobs", "microjobs_runtime",
    "hackathon", "hackathon_runtime", "patreon", "patreon_handler",
))
CANONICAL_MODULES = (
    "collaboration.py", "catalog.py", "catalog_routes.py", "git_http.py",
    "hosted_git.py", "mirrors.py", "mirror_replica.py", "edge_routing.py",
    "ssh_keys.py", "portable_sessions.py", "security_controls.py",
    "organization_succession.py", "marketplace_governance.py",
    "repository_imports.py", "repository_migration.py", "public_snapshots.py",
)
# Modules whose output is an identity, authorization, moderation, or
# governance decision. They may not mention an asset signal at all.
DECISION_MODULES = (
    "collaboration.py", "marketplace_governance.py", "ssh_keys.py",
    "portable_sessions.py", "edge_routing.py",
)
ASSET_SIGNAL = re.compile(
    r"solana|lamport|wallet|balance|stake|bount|payout|tip_|_tip\b|reward",
    re.IGNORECASE,
)
# The only asset-shaped text a decision module may contain: the retired
# marketplace approval columns, which it writes as blanks and never reads.
RETIRED_ASSET_SQL = (
    "wallet_address,",
    "value_lamports,value_checked_at,",
    "wallet_address='',value_lamports=0,value_checked_at=0,",
)

ADAPTER_TABLE_PREFIXES = (
    "reward_", "hackathon_", "patreon_", "ledger_backfill_",
    "legacy_custody_",
)
ADAPTER_TABLES = frozenset((
    "hackathons", "issue_bounty", "bounty_wallet", "central_fund",
    "funds_received", "wallet_balances", "chain_intents", "pending_rewards",
    "world_element_purchases", "tip_notice", "microjob_claims",
    "contributor_reward_estimates",
))
CANONICAL_TABLES = (
    "repositories", "repo_source_authorities", "repo_device_mirrors",
    "repo_shares", "issue_inbox", "pull_inbox", "discussion_inbox", "users",
    "nodes", "account_devices", "account_ssh_keys", "account_sessions", "orgs",
    "org_members", "org_teams", "org_team_members", "org_repos",
    "org_succession_policies", "org_succession_cases",
    "org_succession_approvals", "org_succession_events",
    "marketplace_submissions", "marketplace_member_approvals",
    "mirror_https_endpoints", "mirror_repository_integrity",
    "private_mirror_routes", "owner_encryption_keys", "repo_privacy_policy",
    "quarantined_users", "quarantined_nodes", "catalog_rev",
    "public_catalog_snapshot", "repo_stars", "repo_forks", "profile_follows",
    "notifications", "community_ad_proposals", "community_ad_votes",
    "community_ad_moderation", "ap_blocked_domains",
)
RETIRED_ASSET_COLUMNS = frozenset((
    ("marketplace_member_approvals", "wallet_address"),
    ("marketplace_member_approvals", "value_lamports"),
    ("marketplace_member_approvals", "value_checked_at"),
))
ASSET_COLUMN = re.compile(
    r"wallet|solana|lamport|balance|stake|bount|tip|reward|payout|amount|price",
    re.IGNORECASE,
)


def _is_adapter_table(name):
    return name in ADAPTER_TABLES or name.startswith(ADAPTER_TABLE_PREFIXES)


def _schema_db():
    db = sqlite3.connect(":memory:")
    for statement in SCHEMA_STATEMENTS:
        db.execute(statement)
    return db


def _tables(db):
    return {
        row[0] for row in db.execute(
            "SELECT name FROM sqlite_master WHERE type='table'")
    }


def _code_without_docstrings_and_comments(path):
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    for node in ast.walk(tree):
        body = getattr(node, "body", None)
        if (isinstance(body, list) and body
                and isinstance(body[0], ast.Expr)
                and isinstance(body[0].value, ast.Constant)
                and isinstance(body[0].value.value, str)):
            body[0] = ast.Pass()
    return ast.unparse(tree)


def test_canonical_modules_never_import_incentive_adapters():
    for name in CANONICAL_MODULES:
        path = SRC / name
        tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
        for node in ast.walk(tree):
            imported = set()
            if isinstance(node, ast.Import):
                imported = {alias.name.split(".")[0] for alias in node.names}
            elif isinstance(node, ast.ImportFrom):
                module = (node.module or "").split(".")[0]
                imported = {module} if module else {
                    alias.name for alias in node.names}
            coupled = imported & ADAPTER_MODULES
            assert not coupled, "%s:%d imports %s" % (
                name, node.lineno, sorted(coupled))


def test_decision_modules_read_no_asset_signal():
    for name in DECISION_MODULES:
        code = _code_without_docstrings_and_comments(SRC / name)
        for fragment in RETIRED_ASSET_SQL:
            code = code.replace(fragment, "")
        match = ASSET_SIGNAL.search(code)
        assert match is None, "%s reads an asset signal near %r" % (
            name, code[max(0, match.start() - 60):match.end() + 60])


def _hook_calls(tree):
    """Yield (call, enclosing try handlers) for every awaited hackathon hook."""
    def visit(node, handlers):
        if isinstance(node, ast.Try):
            caught = tuple(
                ast.unparse(handler.type) if handler.type else "BaseException"
                for handler in node.handlers)
            for child in node.body:
                yield from visit(child, handlers + caught)
            for child in node.handlers + node.orelse + node.finalbody:
                yield from visit(child, handlers)
            return
        if (isinstance(node, ast.Await) and isinstance(node.value, ast.Call)
                and isinstance(node.value.func, ast.Attribute)
                and isinstance(node.value.func.value, ast.Name)
                and node.value.func.value.id in ("hackathon", "_hackathon")):
            yield node, handlers
        for child in ast.iter_child_nodes(node):
            yield from visit(child, handlers)
    yield from visit(tree, ())


def test_incentive_hooks_are_best_effort_and_cannot_fail_canonical_writes():
    seen = {}
    for name in ("entry.py", "account_users.py", "catalog_routes.py"):
        path = SRC / name
        tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
        for call, handlers in _hook_calls(tree):
            hook = call.value.func.attr
            seen.setdefault(name, set()).add(hook)
            assert set(handlers) & {"Exception", "BaseException"}, (
                "%s:%d awaits hackathon.%s outside a best-effort try" % (
                    name, call.lineno, hook))
    assert "on_signup" in seen.get("entry.py", set())
    assert "on_referral_credited" in seen.get("account_users.py", set())
    assert "on_first_mirror" in seen.get("catalog_routes.py", set())


def test_removing_adapter_tables_leaves_canonical_schema_unchanged():
    db = _schema_db()
    tables = _tables(db)
    adapters = {name for name in tables if _is_adapter_table(name)}
    assert {"issue_bounty", "wallet_balances", "reward_rounds",
            "microjob_claims", "patreon_account_links"} <= adapters
    canonical = tables - adapters

    for name in canonical:
        for fk in db.execute("PRAGMA foreign_key_list(%s)" % name):
            assert not _is_adapter_table(fk[2]), (
                "%s references adapter table %s" % (name, fk[2]))

    def snapshot():
        return {
            name: (
                db.execute(
                    "SELECT sql FROM sqlite_master WHERE name=?", (name,)
                ).fetchone()[0],
                tuple(db.execute("PRAGMA foreign_key_list(%s)" % name)),
            )
            for name in sorted(canonical)
        }

    before = snapshot()
    db.execute("PRAGMA foreign_keys=ON")
    for name in sorted(adapters):
        db.execute("DROP TABLE %s" % name)
    assert _tables(db) == canonical
    assert snapshot() == before
    assert db.execute("PRAGMA foreign_key_check").fetchall() == []


def test_canonical_tables_carry_no_asset_columns():
    db = _schema_db()
    tables = _tables(db)
    for name in CANONICAL_TABLES:
        assert name in tables, name
        assert not _is_adapter_table(name), name
        for column in db.execute("PRAGMA table_info(%s)" % name):
            if (name, column[1]) in RETIRED_ASSET_COLUMNS:
                continue
            assert not ASSET_COLUMN.search(column[1]), (
                "%s.%s is an asset column" % (name, column[1]))


def test_unconfigured_adapters_answer_explicitly():
    patreon = (SRC / "patreon_handler.py").read_text(encoding="utf-8")
    microjobs = (SRC / "microjobs.py").read_text(encoding="utf-8")
    assert '{"error": "patreon_oauth_not_configured"}, 503' in patreon
    assert '{"error": "patreon_webhook_not_configured"}, status=503' in patreon
    assert '"reason": "pool_not_configured"' in microjobs

    runtime = (SRC / "marketplace_runtime.py").read_text(encoding="utf-8")
    assert "wallet_value" not in runtime
    assert "_display_balance_lamports" not in runtime


def test_commercial_and_protocol_commitments_are_published():
    requirements = json.loads(
        (ROOT / "conformance" / "incentives" / "v1" / "requirements.json")
        .read_text(encoding="utf-8"))
    guide_path = ROOT / requirements["documentation"]
    guide = " ".join(
        re.sub(r"(?m)^> ?", "", guide_path.read_text(encoding="utf-8")).split())
    assert requirements["schema"] == "forkmesh-incentive-independence-conformance-v1"
    assert requirements["invariant"] in guide

    this_file = Path(__file__).read_text(encoding="utf-8")
    ids = []
    for requirement in requirements["requirements"]:
        ids.append(requirement["id"])
        assert requirement["level"] == "MUST"
        assert requirement["evidence"]
        for evidence in requirement["evidence"]:
            path, _, test_name = evidence.partition("::")
            source = (ROOT / path).read_text(encoding="utf-8")
            assert "def %s(" % test_name in source, evidence
        assert requirement["id"].startswith("INC-")
    assert ids == ["INC-%d" % index for index in range(1, len(ids) + 1)]
    assert "def test_commercial_and_protocol_commitments_are_published(" in this_file

    for marker in (
        "## Architecture",
        "## Commercial commitment",
        "## Protocol governance",
        "## Payment and no-payment paths",
        "## Disabled deployment",
        "## Failure, rollback, and recovery",
        "## Telemetry and measures",
        "## Linked automated evidence",
        "Essential no-incentive journey pass rate",
        "Adapter coupling violations",
        "User comprehension",
        "app/tests/test_incentive_independence.py",
        "app/tests/test_marketplace_governance.py",
        "app/tests/test_non_custodial_surfaces.py",
    ):
        assert marker in guide, marker

    contract = json.loads(
        (ROOT / "www" / "public" / "business-model.json").read_text(encoding="utf-8"))
    commitments = contract["permanentCommitments"]
    assert commitments["tokenOrBlockchainRequired"] is False
    assert commitments[
        "incentivesAffectIdentityPermissionsRankingModerationOrGovernance"] is False

    business = " ".join(
        (ROOT / "www" / "docs" / "business-model.md").read_text(encoding="utf-8").split())
    assert "[incentive independence](incentive-independence.md)" in business
    protocol = " ".join(
        (ROOT / "www" / "docs" / "design" / "conflict-safe-collaboration.md")
        .read_text(encoding="utf-8").split())
    assert "MUST NOT infer any role from asset ownership" in protocol

    gates = json.loads(
        (ROOT / "www" / "docs" / "engineering" / "quality-gates.json")
        .read_text(encoding="utf-8"))
    gate = {item["id"]: item for item in gates["gates"]}["incentive-independence"]
    assert "app/tests/test_incentive_independence.py" in gate["command"]
    for profile in ("core", "release"):
        assert "incentive-independence" in gates["profiles"][profile]

    assert "[Incentive independence](incentive-independence.md)" in (
        ROOT / "www" / "docs" / "README.md").read_text(encoding="utf-8")

    market = (ROOT / "www" / "public" / "market.html").read_text(encoding="utf-8")
    assert "wallet-qualified" not in market
    assert "valueLamports" not in market
