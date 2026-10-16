#!/usr/bin/env python3
"""Regression tests for migrations on a fresh local D1 database."""

import ast
import hashlib
import re
import sqlite3
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]


def test_world_build_board_title_migration_is_safe_after_table_creation():
    create = (ROOT / "migrations" / "0091_world_build_board.sql").read_text(
        encoding="utf-8"
    )
    upgrade = (
        ROOT / "migrations" / "0092_world_build_board_issue_title.sql"
    ).read_text(encoding="utf-8")
    connection = sqlite3.connect(":memory:")
    try:
        connection.executescript(create)
        connection.executescript(upgrade)
        title_columns = [
            row for row in connection.execute(
                "PRAGMA table_info(world_build_board_items)"
            ) if row[1] == "title"
        ]
    finally:
        connection.close()

    assert len(title_columns) == 1


def _literal_assignment(path, name):
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    for node in tree.body:
        if not isinstance(node, ast.Assign):
            continue
        if any(isinstance(target, ast.Name) and target.id == name
               for target in node.targets):
            return ast.literal_eval(node.value)
    raise AssertionError(f"missing {name}")


def _runtime_schema_fingerprint():
    entry_path = ROOT / "src" / "entry.py"
    statements = (
        _literal_assignment(entry_path, "SCHEMA_PRE_CREATE_ALTER_STATEMENTS")
        + _literal_assignment(ROOT / "src" / "schema.py", "SCHEMA_STATEMENTS")
        + _literal_assignment(entry_path, "SCHEMA_ALTER_STATEMENTS")
    )
    return hashlib.sha256("\n".join(statements).encode("utf-8")).hexdigest()


def _migration_schema_fingerprint_stamps():
    pattern = re.compile(
        r"VALUES\s*\(\s*'schema_fingerprint:([0-9a-f]{64})'\s*,"
        r"\s*'applied'\s*\)",
        re.IGNORECASE | re.DOTALL,
    )
    stamps = []
    for migration in sorted((ROOT / "migrations").glob("*.sql")):
        for fingerprint in pattern.findall(migration.read_text(encoding="utf-8")):
            stamps.append((migration.name, fingerprint.lower()))
    return stamps


def test_latest_native_migration_stamps_exact_runtime_schema_fingerprint():
    stamps = _migration_schema_fingerprint_stamps()
    assert stamps
    migration_name, fingerprint = stamps[-1]
    assert migration_name == "0266_badge_pull_facts.sql"
    assert fingerprint == _runtime_schema_fingerprint()
    # Production must not import/hash the complete schema merely to discover
    # the marker the deploy migration just wrote. Pinning the literal keeps a
    # cold ActivityPub inbox request inside its CPU slice; this assertion makes
    # a schema edit update all three sides of that contract together.
    assert _literal_assignment(
        ROOT / "src" / "entry.py", "_SCHEMA_FINGERPRINT") == fingerprint


def test_request_block_rules_migration_is_generic_and_reversible():
    up = (ROOT / "migrations" / "0171_request_block_rules.sql").read_text(
        encoding="utf-8")
    down = (
        ROOT / "migrations" / "down" / "0171_request_block_rules.sql"
    ).read_text(encoding="utf-8")
    connection = sqlite3.connect(":memory:")
    try:
        connection.execute(
            "CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT NOT NULL)")
        connection.executescript(up)
        assert connection.execute(
            "SELECT COUNT(*) FROM request_block_rules").fetchone()[0] == 0
        for rule_id, match_type, pattern in (
            ("agent", "user_agent_keyword", "samplecrawler"),
            ("address", "ip", "203.0.113.7"),
            ("network", "cidr", "198.51.100.0/24"),
            ("range", "ip_range", "192.0.2.10-192.0.2.20"),
        ):
            connection.execute(
                "INSERT INTO request_block_rules "
                "(rule_id,match_type,pattern,created_at,updated_at) "
                "VALUES (?,?,?,?,?)",
                (rule_id, match_type, pattern, 1, 1),
            )
        assert connection.execute(
            "SELECT COUNT(*) FROM request_block_rules WHERE enabled=1"
        ).fetchone()[0] == 4
        connection.executescript(down)
        assert connection.execute(
            "SELECT name FROM sqlite_master "
            "WHERE type='table' AND name='request_block_rules'"
        ).fetchone() is None
    finally:
        connection.close()


def test_pull_submission_receipts_migration_dedupes_and_reverses():
    up = (ROOT / "migrations" / "0228_pull_submission_receipts.sql").read_text(
        encoding="utf-8")
    down = (
        ROOT / "migrations" / "down" / "0228_pull_submission_receipts.sql"
    ).read_text(encoding="utf-8")
    connection = sqlite3.connect(":memory:")
    try:
        connection.execute(
            "CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT NOT NULL)")
        connection.execute(
            "CREATE TABLE pull_inbox (id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "repo_bi TEXT NOT NULL, data TEXT NOT NULL, submitter_bi TEXT)")
        connection.execute(
            "INSERT INTO pull_inbox (repo_bi, data) VALUES ('r', 'legacy')")
        connection.executescript(up)
        for _ in range(2):
            connection.execute(
                "INSERT OR IGNORE INTO pull_inbox "
                "(repo_bi, data, submission_bi) VALUES ('r', 'x', 's')")
            connection.execute(
                "INSERT OR IGNORE INTO pull_submission_receipts "
                "(repo_bi, submission_bi, data) VALUES ('r', 's', 'x')")
        connection.execute(
            "INSERT INTO pull_inbox (repo_bi, data) VALUES ('r', 'comment')")
        assert connection.execute(
            "SELECT COUNT(*) FROM pull_inbox WHERE submission_bi='s'"
        ).fetchone()[0] == 1
        assert connection.execute(
            "SELECT COUNT(*) FROM pull_inbox WHERE submission_bi=''"
        ).fetchone()[0] == 2
        assert connection.execute(
            "SELECT COUNT(*) FROM pull_submission_receipts").fetchone()[0] == 1
        connection.executescript(down)
        assert connection.execute(
            "SELECT name FROM sqlite_master "
            "WHERE type='table' AND name='pull_submission_receipts'"
        ).fetchone() is None
        columns = [row[1] for row in connection.execute(
            "PRAGMA table_info(pull_inbox)")]
        assert "submission_bi" not in columns
        assert connection.execute(
            "SELECT COUNT(*) FROM schema_meta").fetchone()[0] == 0
    finally:
        connection.close()


def test_d1_read_efficiency_migration_indexes_hot_global_scans():
    up = (ROOT / "migrations" / "0172_d1_read_efficiency.sql").read_text(
        encoding="utf-8")
    down = (
        ROOT / "migrations" / "down" / "0172_d1_read_efficiency.sql"
    ).read_text(encoding="utf-8")
    connection = sqlite3.connect(":memory:")
    try:
        connection.executescript(
            """
            CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT NOT NULL);
            CREATE TABLE users (
                user_bi TEXT PRIMARY KEY, data TEXT NOT NULL,
                username TEXT, is_admin INTEGER NOT NULL DEFAULT 0);
            CREATE TABLE org_bot_token_usage (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                used_at INTEGER NOT NULL);
            """
        )
        connection.executescript(up)
        cleanup_plan = " ".join(
            row[3] for row in connection.execute(
                "EXPLAIN QUERY PLAN DELETE FROM org_bot_token_usage "
                "WHERE used_at < ?", (100,)))
        roster_plan = " ".join(
            row[3] for row in connection.execute(
                "EXPLAIN QUERY PLAN SELECT data FROM users "
                "ORDER BY username LIMIT 20"))
        admin_plan = " ".join(
            row[3] for row in connection.execute(
                "EXPLAIN QUERY PLAN SELECT data FROM users "
                "WHERE is_admin = 1 LIMIT 20"))
        assert "idx_org_bot_token_usage_used_at" in cleanup_plan
        assert "idx_users_username" in roster_plan
        assert "idx_users_admin" in admin_plan
        connection.executescript(down)
        remaining = {
            row[0] for row in connection.execute(
                "SELECT name FROM sqlite_master WHERE type = 'index' "
                "AND name LIKE 'idx_%'")
        }
        assert not remaining
    finally:
        connection.close()


def test_product_analysis_migration_backfills_referrals_and_reverses():
    up = (ROOT / "migrations" / "0175_product_analysis.sql").read_text(
        encoding="utf-8")
    down = (
        ROOT / "migrations" / "down" / "0175_product_analysis.sql"
    ).read_text(encoding="utf-8")
    connection = sqlite3.connect(":memory:")
    try:
        connection.executescript(
            """
            CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT NOT NULL);
            CREATE TABLE users (
                user_bi TEXT PRIMARY KEY, data TEXT NOT NULL,
                username TEXT, is_admin INTEGER NOT NULL DEFAULT 0);
            CREATE TABLE referral_attributions (
                referred_bi TEXT PRIMARY KEY, referrer_bi TEXT NOT NULL,
                decision TEXT NOT NULL DEFAULT 'pending');
            INSERT INTO users (user_bi, data, username) VALUES
                ('direct', '{}', 'direct'),
                ('credited', '{}', 'credited'),
                ('rejected', '{}', 'rejected');
            INSERT INTO referral_attributions VALUES
                ('credited', 'someone', 'credited'),
                ('rejected', 'someone', 'rejected');
            INSERT INTO schema_meta VALUES
                ('product_analysis_signup_origin_backfill:v1', 'applied');
            """
        )
        connection.executescript(up)
        origins = dict(connection.execute(
            "SELECT user_bi, signup_origin FROM users"))
        # Every referral arrival counts as the referral channel, whatever the
        # later credit decision; a plain signup stays unknown for the
        # Worker-side pass that can read the encrypted marker.
        assert origins == {
            "direct": "unknown", "credited": "referral",
            "rejected": "referral"}
        with pytest.raises(sqlite3.IntegrityError):
            connection.execute(
                "UPDATE users SET signup_origin='guess' WHERE user_bi='direct'")
        connection.execute(
            "INSERT INTO product_clone_daily (day, owner, repo, clones) "
            "VALUES ('2026-09-09', 'forkmesh', 'forkmesh', 1)")
        connection.execute(
            "INSERT INTO product_clone_daily (day, owner, repo, clones) "
            "VALUES ('2026-09-09', 'forkmesh', 'forkmesh', 1) "
            "ON CONFLICT(day, owner, repo) "
            "DO UPDATE SET clones = product_clone_daily.clones + 1")
        assert connection.execute(
            "SELECT clones FROM product_clone_daily").fetchone()[0] == 2
        connection.executescript(down)
        columns = {
            row[1] for row in connection.execute("PRAGMA table_info(users)")}
        assert "signup_origin" not in columns
        assert connection.execute(
            "SELECT name FROM sqlite_master "
            "WHERE type='table' AND name='product_clone_daily'"
        ).fetchone() is None
        assert connection.execute(
            "SELECT COUNT(*) FROM schema_meta").fetchone()[0] == 0
    finally:
        connection.close()


def test_direct_mirror_url_migration_preserves_tunnel_only_rows():
    migration = (
        ROOT / "migrations" / "0162_direct_mirror_urls.sql"
    ).read_text(encoding="utf-8")
    connection = sqlite3.connect(":memory:")
    try:
        connection.executescript(
            """
            CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT NOT NULL);
            CREATE TABLE mirror_https_endpoints (
                node_bi TEXT PRIMARY KEY,
                node_name TEXT NOT NULL,
                base_url TEXT NOT NULL
            );
            INSERT INTO mirror_https_endpoints
            VALUES ('node-bi', 'mirror9', 'https://mirror9.example');
            """
        )
        connection.executescript(migration)
        columns = {
            row[1]: row
            for row in connection.execute(
                "PRAGMA table_info(mirror_https_endpoints)")
        }
        mirror_url = connection.execute(
            "SELECT mirror_url FROM mirror_https_endpoints"
        ).fetchone()[0]
    finally:
        connection.close()

    assert columns["mirror_url"][3] == 1
    assert columns["mirror_url"][4] == "''"
    assert mirror_url == ""


def test_mirror_repository_integrity_migration_is_repository_scoped():
    migration = (
        ROOT / "migrations" / "0163_mirror_repository_integrity.sql"
    ).read_text(encoding="utf-8")
    connection = sqlite3.connect(":memory:")
    try:
        connection.executescript(
            """
            PRAGMA foreign_keys=ON;
            CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT NOT NULL);
            CREATE TABLE mirror_https_endpoints (
                node_bi TEXT PRIMARY KEY,
                node_name TEXT NOT NULL
            );
            INSERT INTO mirror_https_endpoints
            VALUES ('node-bi', 'mirror9');
            """
        )
        connection.executescript(migration)
        connection.execute(
            """INSERT INTO mirror_repository_integrity
               (repo_bi,node_bi,mirror_url,reachable,integrity,
                refs_sha256,checked_at,latency_ms,status_code,reason)
               VALUES (?,?,?,?,?,?,?,?,?,?)""",
            (
                "repo-a", "node-bi", "https://git.example", 1, "ok",
                "a" * 64, 1000, 12, 200, "verified",
            ),
        )
        connection.execute(
            """INSERT INTO mirror_repository_integrity
               (repo_bi,node_bi,mirror_url,reachable,integrity,
                refs_sha256,checked_at,latency_ms,status_code,reason)
               VALUES (?,?,?,?,?,?,?,?,?,?)""",
            (
                "repo-b", "node-bi", "https://git.example", 0, "failed",
                "", 1001, 14, 0, "repository_proof_failed",
            ),
        )
        rows = connection.execute(
            "SELECT repo_bi,integrity FROM mirror_repository_integrity "
            "ORDER BY repo_bi"
        ).fetchall()
        columns = {
            row[1]: row
            for row in connection.execute(
                "PRAGMA table_info(mirror_repository_integrity)")
        }
        connection.execute(
            "DELETE FROM mirror_https_endpoints WHERE node_bi='node-bi'"
        )
        remaining = connection.execute(
            "SELECT COUNT(*) FROM mirror_repository_integrity"
        ).fetchone()[0]
    finally:
        connection.close()

    assert rows == [("repo-a", "ok"), ("repo-b", "failed")]
    assert columns["registration_issued_at"][3] == 1
    assert remaining == 0


def test_tip_work_payment_migration_is_reversible():
    up = (ROOT / "migrations" / "0158_tip_work_payments.sql").read_text(
        encoding="utf-8")
    down = (
        ROOT / "migrations" / "down" / "0158_tip_work_payments.sql"
    ).read_text(encoding="utf-8")
    connection = sqlite3.connect(":memory:")
    try:
        connection.executescript(
            """
            CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT NOT NULL);
            CREATE TABLE tip_notice (
              signature TEXT PRIMARY KEY, from_bi TEXT NOT NULL,
              to_bi TEXT NOT NULL, lamports INTEGER NOT NULL,
              network TEXT NOT NULL, ts INTEGER NOT NULL,
              emailed INTEGER NOT NULL DEFAULT 0
            );
            """
        )
        connection.executescript(up)
        columns = {
            row[1] for row in connection.execute("PRAGMA table_info(tip_notice)")
        }
        assert {
            "task_description", "output_url", "entered_currency",
            "entered_usd_micros", "sol_usd_price_micros",
            "price_observed_at",
        } <= columns
        connection.executescript(down)
        columns = {
            row[1] for row in connection.execute("PRAGMA table_info(tip_notice)")
        }
        assert "task_description" not in columns
        assert "output_url" not in columns
    finally:
        connection.close()

def test_quarantine_discord_identity_migration_is_reversible():
    up = (ROOT / "migrations" / "0155_quarantine_discord_identity.sql").read_text(
        encoding="utf-8")
    down = (
        ROOT / "migrations" / "down" /
        "0155_quarantine_discord_identity.sql"
    ).read_text(encoding="utf-8")
    connection = sqlite3.connect(":memory:")
    try:
        connection.execute(
            "CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT NOT NULL)")
        connection.execute(
            "CREATE TABLE quarantined_users ("
            "user_bi TEXT PRIMARY KEY,data TEXT NOT NULL,email_bi TEXT,"
            "username TEXT NOT NULL,is_admin INTEGER NOT NULL DEFAULT 0,"
            "ip_bi TEXT,enable_outreach INTEGER NOT NULL DEFAULT 0,"
            "referrer_bi TEXT NOT NULL DEFAULT '',reason_codes TEXT NOT NULL,"
            "status TEXT NOT NULL,quarantined_at INTEGER NOT NULL,"
            "review_after INTEGER NOT NULL DEFAULT 0,"
            "reviewed_at INTEGER NOT NULL DEFAULT 0,"
            "reviewed_by TEXT NOT NULL DEFAULT '')")
        connection.executescript(up)
        columns = {
            row[1] for row in connection.execute(
                "PRAGMA table_info(quarantined_users)")
        }
        assert "discord_bi" in columns
        assert connection.execute(
            "SELECT name FROM sqlite_master "
            "WHERE name='idx_quarantined_users_discord'"
        ).fetchone()
        connection.executescript(down)
        columns = {
            row[1] for row in connection.execute(
                "PRAGMA table_info(quarantined_users)")
        }
        assert "discord_bi" not in columns
    finally:
        connection.close()


def test_quarantine_discord_lazy_upgrade_precedes_its_runtime_index():
    entry = ROOT / "src" / "entry.py"
    pre_create = _literal_assignment(
        entry, "SCHEMA_PRE_CREATE_ALTER_STATEMENTS")
    post_create = _literal_assignment(entry, "SCHEMA_ALTER_STATEMENTS")
    alter = "ALTER TABLE quarantined_users ADD COLUMN discord_bi TEXT"
    assert alter in pre_create
    assert alter not in post_create
    assert "idx_quarantined_users_discord" in "\n".join(
        _literal_assignment(ROOT / "src" / "schema.py", "SCHEMA_STATEMENTS"))


def test_no_runtime_index_depends_on_a_post_create_column():
    """Every column an index needs is added before that index is created.

    _apply_schema runs SCHEMA_STATEMENTS strictly and SCHEMA_ALTER_STATEMENTS
    only afterwards. A column added there but indexed in SCHEMA_STATEMENTS
    fails "no such column" on any database that predates it, which turns every
    request that ensures the schema into a 500. users.discord_bi did exactly
    that on an older local database.
    """
    entry = ROOT / "src" / "entry.py"
    post_create = _literal_assignment(entry, "SCHEMA_ALTER_STATEMENTS")
    statements = _literal_assignment(ROOT / "src" / "schema.py", "SCHEMA_STATEMENTS")
    index_re = re.compile(r"\s*CREATE\s+(?:UNIQUE\s+)?INDEX", re.I)
    offenders = []
    for alter in post_create:
        match = re.match(
            r"\s*ALTER\s+TABLE\s+(\w+)\s+ADD\s+COLUMN\s+(\w+)", alter, re.I)
        if not match:
            continue
        table, column = match.groups()
        on_table = re.compile(r"\bON\s+%s\s*\(" % re.escape(table), re.I)
        uses_column = re.compile(r"\b%s\b" % re.escape(column))
        for statement in statements:
            if (index_re.match(statement) and on_table.search(statement)
                    and uses_column.search(statement.split("(", 1)[1])):
                offenders.append((table, column))
    assert offenders == []


def test_users_discord_lazy_upgrade_runs_on_a_pre_0148_database():
    entry = ROOT / "src" / "entry.py"
    pre_create = _literal_assignment(entry, "SCHEMA_PRE_CREATE_ALTER_STATEMENTS")
    statements = _literal_assignment(ROOT / "src" / "schema.py", "SCHEMA_STATEMENTS")
    connection = sqlite3.connect(":memory:")
    try:
        connection.execute(
            "CREATE TABLE users (user_bi TEXT PRIMARY KEY, data TEXT NOT NULL, "
            "email_bi TEXT, username TEXT, is_admin INTEGER NOT NULL DEFAULT 0)")
        for statement in pre_create:
            if statement.startswith("ALTER TABLE users "):
                connection.execute(statement)
        for statement in statements:
            if "idx_users_discord" in statement:
                connection.execute(statement)
        indexes = {row[1] for row in connection.execute("PRAGMA index_list(users)")}
    finally:
        connection.close()
    assert "idx_users_discord" in indexes


def test_contributor_payment_snapshot_migration_and_down_are_additive():
    base = (ROOT / "migrations" / "0145_contributor_reward_estimates.sql").read_text(
        encoding="utf-8")
    up = (ROOT / "migrations" / "0154_contributor_payment_snapshots.sql").read_text(
        encoding="utf-8")
    down = (
        ROOT / "migrations" / "down" /
        "0154_contributor_payment_snapshots.sql"
    ).read_text(encoding="utf-8")
    connection = sqlite3.connect(":memory:")
    try:
        connection.execute(
            "CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT NOT NULL)")
        connection.executescript(base)
        connection.executescript(up)
        columns = {
            row[1] for row in connection.execute(
                "PRAGMA table_info(contributor_reward_estimates)")
        }
        assert {
            "subject_summary", "payment_usd_micros",
            "payment_sol_usd_micros", "payment_valued_at",
        } <= columns
        connection.executescript(down)
        columns = {
            row[1] for row in connection.execute(
                "PRAGMA table_info(contributor_reward_estimates)")
        }
        assert not ({
            "subject_summary", "payment_usd_micros",
            "payment_sol_usd_micros", "payment_valued_at",
        } & columns)
    finally:
        connection.close()


def test_pull_reward_range_migration_preserves_receipts_and_is_source_specific():
    base = (ROOT / "migrations" / "0145_contributor_reward_estimates.sql").read_text(
        encoding="utf-8")
    snapshots = (
        ROOT / "migrations" / "0154_contributor_payment_snapshots.sql"
    ).read_text(encoding="utf-8")
    upgrade = (ROOT / "migrations" / "0161_pull_reward_range.sql").read_text(
        encoding="utf-8")
    connection = sqlite3.connect(":memory:")
    try:
        connection.execute(
            "CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT NOT NULL)")
        connection.executescript(base)
        connection.executescript(snapshots)
        connection.execute(
            "INSERT INTO contributor_reward_estimates ("
            "reward_id,recipient_bi,recipient_name,repo_owner,repo_name,"
            "source_kind,subject_number,subject_title,subject_summary,"
            "amount_lamports,condition,status,created_at,updated_at,"
            "submitted_at,paid_at,payment_usd_micros,"
            "payment_sol_usd_micros,payment_valued_at,source_address,"
            "destination_address,tx_signature) VALUES ("
            "'reward-preserved-pull','recipient-bi','gaurav2631','forkmesh',"
            "'forkmesh','pull',14,'Recycle bin','Preserved summary',5000000,"
            "'merged','paid',10,20,30,40,1500000,96000000,50,"
            "'source-wallet','destination-wallet','signature')")

        connection.executescript(upgrade)

        preserved = connection.execute(
            "SELECT recipient_name,subject_summary,amount_lamports,status,"
            "submitted_at,paid_at,payment_usd_micros,"
            "payment_sol_usd_micros,payment_valued_at,tx_signature "
            "FROM contributor_reward_estimates WHERE reward_id="
            "'reward-preserved-pull'").fetchone()
        assert preserved == (
            "gaurav2631", "Preserved summary", 5_000_000, "paid",
            30, 40, 1_500_000, 96_000_000, 50, "signature")

        connection.execute(
            "INSERT INTO contributor_reward_estimates ("
            "reward_id,recipient_bi,recipient_name,repo_owner,repo_name,"
            "source_kind,subject_number,amount_lamports,condition,status,"
            "created_at,updated_at) VALUES ("
            "'reward-pull-twenty','recipient-bi','gaurav2631','forkmesh',"
            "'forkmesh','pull',36,20000000,'merged','pending_merge',60,60)")
        with pytest.raises(sqlite3.IntegrityError):
            connection.execute(
                "INSERT INTO contributor_reward_estimates ("
                "reward_id,recipient_bi,recipient_name,repo_owner,repo_name,"
                "source_kind,subject_number,amount_lamports,condition,status,"
                "created_at,updated_at) VALUES ("
                "'reward-issue-twenty','recipient-bi','gaurav2631','forkmesh',"
                "'forkmesh','issue',36,20000000,'accepted_report',"
                "'pending_wallet',60,60)")
        with pytest.raises(sqlite3.IntegrityError):
            connection.execute(
                "INSERT INTO contributor_reward_estimates ("
                "reward_id,recipient_bi,recipient_name,repo_owner,repo_name,"
                "source_kind,subject_number,amount_lamports,condition,status,"
                "created_at,updated_at) VALUES ("
                "'reward-pull-too-large','recipient-bi','gaurav2631','forkmesh',"
                "'forkmesh','pull',37,25000001,'merged','pending_merge',60,60)")

        indexes = {
            row[1] for row in connection.execute(
                "PRAGMA index_list(contributor_reward_estimates)")
        }
        assert {
            "idx_contributor_reward_subject", "idx_contributor_reward_status",
            "idx_contributor_reward_recipient", "idx_contributor_reward_tx",
        } <= indexes
    finally:
        connection.close()


def test_modular_marketplace_migration_and_its_down_run_on_fresh_sqlite():
    up = (ROOT / "migrations" / "0153_modular_marketplace_governance.sql").read_text(
        encoding="utf-8"
    )
    down = (
        ROOT / "migrations" / "down" /
        "0153_modular_marketplace_governance.sql"
    ).read_text(encoding="utf-8")
    connection = sqlite3.connect(":memory:")
    try:
        connection.execute(
            "CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT NOT NULL)"
        )
        connection.executescript(up)
        package_columns = {
            row[1] for row in connection.execute("PRAGMA table_info(package_installs)")
        }
        assert "market" in package_columns
        assert connection.execute(
            "SELECT name FROM sqlite_master WHERE name='marketplace_submissions'"
        ).fetchone()
        connection.executescript(down)
        package_columns = {
            row[1] for row in connection.execute("PRAGMA table_info(package_installs)")
        }
        assert "market" not in package_columns
        assert connection.execute(
            "SELECT name FROM sqlite_master WHERE name='marketplace_submissions'"
        ).fetchone() is None
    finally:
        connection.close()


def test_market_ratings_migration_is_reversible():
    up = (ROOT / "migrations" / "0165_market_package_ratings.sql").read_text(
        encoding="utf-8"
    )
    down = (
        ROOT / "migrations" / "down" / "0165_market_package_ratings.sql"
    ).read_text(encoding="utf-8")
    connection = sqlite3.connect(":memory:")
    try:
        connection.execute(
            "CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT NOT NULL)"
        )
        connection.executescript(up)
        assert connection.execute(
            "SELECT name FROM sqlite_master WHERE name='package_ratings'"
        ).fetchone()
        connection.executescript(down)
        assert connection.execute(
            "SELECT name FROM sqlite_master WHERE name='package_ratings'"
        ).fetchone() is None
    finally:
        connection.close()


def test_operating_expenses_migration_seeds_linked_cursor_reimbursement():
    up = (ROOT / "migrations" / "0166_operating_expenses.sql").read_text(
        encoding="utf-8"
    )
    down = (
        ROOT / "migrations" / "down" / "0166_operating_expenses.sql"
    ).read_text(encoding="utf-8")
    connection = sqlite3.connect(":memory:")
    try:
        connection.execute(
            "CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT NOT NULL)"
        )
        connection.executescript(up)
        row = connection.execute(
            "SELECT * FROM operating_expenses WHERE expense_id=?",
            ("cursor-e45e60f2-0007",),
        ).fetchone()
        assert row is not None
        columns = [
            value[1] for value in connection.execute(
                "PRAGMA table_info(operating_expenses)")
        ]
        seeded = dict(zip(columns, row))
        assert seeded["vendor"] == "Cursor"
        assert seeded["paid_by_name"] == "mdkaifansari04"
        assert seeded["amount_minor"] == 2360
        assert seeded["original_amount_minor"] == 234359
        assert seeded["original_currency"] == "INR"
        assert seeded["external_reference"] == "E45E60F2-0007"
        assert seeded["payment_method_summary"] == ""
        assert seeded["incurred_at"] == 1785427200000
        assert seeded["reimbursement_status"] == "outstanding"
        connection.executescript(down)
        assert connection.execute(
            "SELECT name FROM sqlite_master WHERE name='operating_expenses'"
        ).fetchone() is None
    finally:
        connection.close()


def test_jett_ai_expenses_are_exact_matched_debts_and_reversible():
    schema = (ROOT / "migrations" / "0166_operating_expenses.sql").read_text(
        encoding="utf-8"
    )
    up = (
        ROOT / "migrations" / "0168_jett_ai_operating_expenses.sql"
    ).read_text(encoding="utf-8")
    down = (
        ROOT / "migrations" / "down" /
        "0168_jett_ai_operating_expenses.sql"
    ).read_text(encoding="utf-8")
    connection = sqlite3.connect(":memory:")
    try:
        connection.execute(
            "CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT NOT NULL)"
        )
        connection.executescript(schema)
        connection.executescript(up)
        totals = connection.execute(
            "SELECT COUNT(*),SUM(amount_minor),"
            "SUM(CASE WHEN vendor='OpenAI' THEN amount_minor ELSE 0 END),"
            "SUM(CASE WHEN vendor='Anthropic' THEN amount_minor ELSE 0 END) "
            "FROM operating_expenses WHERE paid_by_name='jett' "
            "AND reimbursement_status='outstanding'"
        ).fetchone()
        invoices = dict(connection.execute(
            "SELECT external_reference,amount_minor FROM operating_expenses "
            "WHERE paid_by_name='jett'"
        ))

        assert totals == (11, 105_891, 59_999, 45_892)
        assert invoices["9RUQ8RRU-0005"] == 17_527
        assert invoices["9RUQ8RRU-0004"] == 9_165
        assert invoices["9RUQ8RRU-0003"] == 2_178
        assert invoices["8RYATVG1-0010"] == 21_775
        assert invoices["8RYATVG1-0009"] == 2_178
        assert invoices["8RYATVG1-0008"] == 12_264
        assert invoices["8RYATVG1-0006"] == 9_675
        assert "authoritative\n-- payment-time USD value" in up

        connection.executescript(down)
        assert connection.execute(
            "SELECT COUNT(*) FROM operating_expenses "
            "WHERE paid_by_name='jett'"
        ).fetchone()[0] == 0
        assert connection.execute(
            "SELECT COUNT(*) FROM operating_expenses"
        ).fetchone()[0] == 1
    finally:
        connection.close()


def test_jett_anthropic_invoice_0012_is_an_exact_outstanding_debt():
    schema = (ROOT / "migrations" / "0166_operating_expenses.sql").read_text(
        encoding="utf-8"
    )
    up = (
        ROOT / "migrations" / "0185_jett_anthropic_expense_2026_09_12.sql"
    ).read_text(encoding="utf-8")
    down = (
        ROOT / "migrations" / "down" /
        "0185_jett_anthropic_expense_2026_09_12.sql"
    ).read_text(encoding="utf-8")
    connection = sqlite3.connect(":memory:")
    try:
        connection.execute(
            "CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT NOT NULL)"
        )
        connection.executescript(schema)
        before = connection.execute(
            "SELECT COUNT(*) FROM operating_expenses").fetchone()[0]
        connection.executescript(up)
        connection.executescript(up)  # re-applying never duplicates it
        assert connection.execute(
            "SELECT vendor,paid_by_name,amount_minor,currency,incurred_at,"
            "reimbursement_status,reimbursed_at "
            "FROM operating_expenses WHERE external_reference='8RYATVG1-0012'"
        ).fetchall() == [
            ("Anthropic", "jett", 21_775, "USD", 1_789_171_200_000,
             "outstanding", 0),
        ]
        connection.executescript(down)
        assert connection.execute(
            "SELECT COUNT(*) FROM operating_expenses").fetchone()[0] == before
    finally:
        connection.close()


def test_payment_card_identifier_scrub_is_irreversible():
    schema = (ROOT / "migrations" / "0166_operating_expenses.sql").read_text(
        encoding="utf-8"
    )
    up = (
        ROOT / "migrations" / "0169_scrub_payment_card_identifiers.sql"
    ).read_text(encoding="utf-8")
    down = (
        ROOT / "migrations" / "down" /
        "0169_scrub_payment_card_identifiers.sql"
    ).read_text(encoding="utf-8")
    connection = sqlite3.connect(":memory:")
    try:
        connection.execute(
            "CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT NOT NULL)"
        )
        connection.executescript(schema)
        connection.execute(
            "UPDATE operating_expenses SET payment_method_summary=? "
            "WHERE expense_id=?",
            ("private payment-card detail", "cursor-e45e60f2-0007"),
        )
        connection.executescript(up)
        assert connection.execute(
            "SELECT payment_method_summary FROM operating_expenses "
            "WHERE expense_id=?",
            ("cursor-e45e60f2-0007",),
        ).fetchone()[0] == ""

        # A rollback must not recreate privacy-sensitive data.
        connection.executescript(down)
        assert connection.execute(
            "SELECT payment_method_summary FROM operating_expenses "
            "WHERE expense_id=?",
            ("cursor-e45e60f2-0007",),
        ).fetchone()[0] == ""
    finally:
        connection.close()


def test_jett_business_expenses_are_exact_outstanding_debts():
    schema = (ROOT / "migrations" / "0166_operating_expenses.sql").read_text(
        encoding="utf-8"
    )
    up = (
        ROOT / "migrations" / "0170_jett_business_expenses.sql"
    ).read_text(encoding="utf-8")
    down = (
        ROOT / "migrations" / "down" /
        "0170_jett_business_expenses.sql"
    ).read_text(encoding="utf-8")
    connection = sqlite3.connect(":memory:")
    try:
        connection.execute(
            "CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT NOT NULL)"
        )
        connection.executescript(schema)
        connection.executescript(up)
        rows = connection.execute(
            "SELECT vendor,amount_minor,currency,incurred_at,"
            "reimbursement_status,payment_method_summary "
            "FROM operating_expenses WHERE paid_by_name='jett' "
            "ORDER BY incurred_at DESC"
        ).fetchall()
        assert rows == [
            ("Business registration", 20_300, "USD", 1_787_702_400_000,
             "outstanding", ""),
            ("Apple", 10_000, "USD", 1_785_974_400_000,
             "outstanding", ""),
            ("Cloudflare", 1_046, "USD", 1_781_049_600_000,
             "outstanding", ""),
        ]
        assert sum(row[1] for row in rows) == 31_346
        connection.executescript(down)
        assert connection.execute(
            "SELECT COUNT(*) FROM operating_expenses "
            "WHERE paid_by_name='jett'"
        ).fetchone()[0] == 0
    finally:
        connection.close()


def test_contributor_payment_historical_quotes_are_exact_and_reversible():
    up = (
        ROOT / "migrations" /
        "0167_contributor_payment_historical_quotes.sql"
    ).read_text(encoding="utf-8")
    down = (
        ROOT / "migrations" / "down" /
        "0167_contributor_payment_historical_quotes.sql"
    ).read_text(encoding="utf-8")
    rewards = {
        "issue-655": (5_000_000, 1_787_690_247_356),
        "issue-656": (4_000_000, 1_787_690_212_452),
        "issue-623": (3_000_000, 1_787_673_981_676),
        "issue-648": (3_000_000, 1_787_609_401_002),
        "pull-39": (3_000_000, 1_787_609_374_573),
        "issue-639": (2_000_000, 1_787_609_345_485),
        "pull-38": (2_000_000, 1_787_609_318_788),
        "pull-37": (4_000_000, 1_787_609_290_981),
        "pull-32": (4_000_000, 1_787_609_259_936),
        "issue-652": (3_000_000, 1_787_609_167_400),
        "issue-651": (2_000_000, 1_787_609_165_407),
        "issue-649": (2_000_000, 0),
        "issue-586": (2_000_000, 0),
        "issue-587": (1_000_000, 0),
        "issue-588": (1_000_000, 0),
        "issue-598": (2_000_000, 0),
        "issue-599": (1_000_000, 0),
        "issue-600": (2_000_000, 0),
        "issue-603": (3_000_000, 0),
        "issue-622": (3_000_000, 0),
        "issue-624": (4_000_000, 0),
        "issue-633": (3_000_000, 0),
        "issue-634": (2_000_000, 0),
        "issue-635": (3_000_000, 0),
        "issue-636": (1_000_000, 0),
        "issue-637": (1_000_000, 0),
        "issue-605": (2_000_000, 0),
        "issue-641": (3_000_000, 0),
        "issue-642": (3_000_000, 0),
        "issue-640": (2_000_000, 0),
        "issue-601": (2_000_000, 0),
        "issue-589": (3_000_000, 0),
        "issue-590": (3_000_000, 0),
        "issue-602": (4_000_000, 0),
        "issue-628": (4_000_000, 0),
        "issue-643": (5_000_000, 0),
        "issue-627": (2_000_000, 0),
    }
    connection = sqlite3.connect(":memory:")
    try:
        connection.executescript(
            """
            CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT NOT NULL);
            CREATE TABLE contributor_reward_estimates (
              reward_id TEXT PRIMARY KEY,
              amount_lamports INTEGER NOT NULL,
              status TEXT NOT NULL,
              tx_signature TEXT NOT NULL,
              payment_usd_micros INTEGER NOT NULL DEFAULT 0,
              payment_sol_usd_micros INTEGER NOT NULL DEFAULT 0,
              payment_valued_at INTEGER NOT NULL DEFAULT 0
            );
            """
        )
        connection.executemany(
            "INSERT INTO contributor_reward_estimates "
            "(reward_id,amount_lamports,status,tx_signature,"
            "payment_valued_at) VALUES (?,?,?,?,?)",
            [
                ("reward-forkmesh-forkmesh-" + suffix, lamports,
                 "paid", "signature-" + suffix, valued_at)
                for suffix, (lamports, valued_at) in rewards.items()
            ],
        )
        connection.executescript(up)
        aggregate = connection.execute(
            "SELECT COUNT(*),SUM(amount_lamports),SUM(payment_usd_micros),"
            "COUNT(DISTINCT payment_sol_usd_micros) "
            "FROM contributor_reward_estimates "
            "WHERE payment_usd_micros>0"
        ).fetchone()
        issue_655 = connection.execute(
            "SELECT payment_usd_micros,payment_sol_usd_micros,"
            "payment_valued_at FROM contributor_reward_estimates "
            "WHERE reward_id='reward-forkmesh-forkmesh-issue-655'"
        ).fetchone()

        assert aggregate == (37, 99_000_000, 9_752_359, 6)
        assert issue_655 == (489_626, 97_925_273, 1_787_688_000_000)
        assert "api.coingecko.com/api/v3/coins/solana/market_chart/range" in up
        assert "observation at or before paid_at" in up

        connection.executescript(down)
        restored = dict(connection.execute(
            "SELECT reward_id,payment_valued_at "
            "FROM contributor_reward_estimates"
        ))
        remaining = connection.execute(
            "SELECT COUNT(*) FROM contributor_reward_estimates "
            "WHERE payment_usd_micros<>0 OR payment_sol_usd_micros<>0"
        ).fetchone()[0]
        assert remaining == 0
        for suffix, (_, valued_at) in rewards.items():
            assert restored["reward-forkmesh-forkmesh-" + suffix] == valued_at
    finally:
        connection.close()


def test_status_batch_migration_stamps_schema_without_changing_custody_marker():
    migration = (
        ROOT / "migrations" / "0124_system_status_batches.sql"
    ).read_text(encoding="utf-8")
    connection = sqlite3.connect(":memory:")
    try:
        connection.executescript(
            """
            CREATE TABLE schema_meta (
                k TEXT PRIMARY KEY,
                v TEXT NOT NULL
            );
            CREATE TABLE mailtrap_email_sends (sent_at INTEGER NOT NULL);
            CREATE TABLE chat_history (ts INTEGER NOT NULL);
            CREATE TABLE nodes (name TEXT NOT NULL);
            INSERT INTO schema_meta (k, v)
            VALUES ('legacy_wallet_custody_v2', 'clean-v2:prior');
            INSERT INTO schema_meta (k, v)
            VALUES ('fingerprint', 'prior-worker-fingerprint');
            """
        )
        connection.executescript(migration)
        metadata = dict(connection.execute("SELECT k, v FROM schema_meta"))
        history_indexes = {
            row[1] for row in connection.execute(
                "PRAGMA index_list(repo_state_history)"
            )
        }
    finally:
        connection.close()

    fingerprint = next(
        value for name, value in _migration_schema_fingerprint_stamps()
        if name == "0124_system_status_batches.sql"
    )
    assert metadata["fingerprint"] == "prior-worker-fingerprint"
    assert metadata["schema_fingerprint:" + fingerprint] == "applied"
    assert metadata["legacy_wallet_custody_v2"] == "clean-v2:prior"
    assert "idx_repo_state_history_key_ts" in history_indexes


def test_public_snapshot_migration_is_append_only_and_matches_lazy_schema():
    migration = (
        ROOT / "migrations" / "0125_public_api_snapshots.sql"
    ).read_text(encoding="utf-8")
    connection = sqlite3.connect(":memory:")
    try:
        connection.executescript(
            """
            CREATE TABLE schema_meta (
                k TEXT PRIMARY KEY,
                v TEXT NOT NULL
            );
            INSERT INTO schema_meta (k, v)
            VALUES ('legacy_wallet_custody_v2', 'clean-v2:prior');
            INSERT INTO schema_meta (k, v)
            VALUES ('fingerprint', 'prior-worker-fingerprint');
            INSERT INTO schema_meta (k, v)
            VALUES ('schema_fingerprint:prior', 'applied');
            """
        )
        connection.executescript(migration)
        metadata = dict(connection.execute("SELECT k, v FROM schema_meta"))
        columns = {
            row[1]: row
            for row in connection.execute(
                "PRAGMA table_info(public_api_snapshots)"
            )
        }
        table_sql = connection.execute(
            "SELECT sql FROM sqlite_schema "
            "WHERE type='table' AND name='public_api_snapshots'"
        ).fetchone()[0]
        counter_indexes = {
            row[1] for row in connection.execute(
                "PRAGMA index_list(mirror_serve_counters)"
            )
        }
        counter_sql = (
            "SELECT node_name,SUM(clones) AS clones,SUM(website) AS website "
            "FROM mirror_serve_counters "
            "WHERE repo=? AND owner IN (?,?) GROUP BY node_name"
        )
        counter_plan = [
            row[3] for row in connection.execute(
                "EXPLAIN QUERY PLAN " + counter_sql,
                ("forkmesh", "forkmesh", "mirror1"),
            )
        ]
    finally:
        connection.close()

    # Its own stamp, not the current runtime one: later migrations append the
    # head fingerprint, which
    # test_latest_native_migration_stamps_exact_runtime_schema_fingerprint
    # pins to the runtime DDL.
    fingerprint = next(
        value for name, value in _migration_schema_fingerprint_stamps()
        if name == "0125_public_api_snapshots.sql"
    )
    assert metadata["fingerprint"] == "prior-worker-fingerprint"
    assert metadata["schema_fingerprint:prior"] == "applied"
    assert metadata["schema_fingerprint:" + fingerprint] == "applied"
    assert metadata["legacy_wallet_custody_v2"] == "clean-v2:prior"
    assert set(columns) == {
        "path", "schema_version", "kind", "generated_at", "fresh_until",
        "stale_until", "deploy_fingerprint", "content_length", "payload",
    }
    assert columns["path"][5] == 1
    assert "WITHOUT ROWID" in table_sql.upper()
    assert "idx_mirror_serve_counters_repo_owner_node" in counter_indexes
    assert any(
        "SEARCH mirror_serve_counters USING INDEX "
        "idx_mirror_serve_counters_repo_owner_node" in step
        for step in counter_plan
    )
    assert not any("SCAN mirror_serve_counters" in step for step in counter_plan)


def test_blog_post_links_migration_stores_public_permalinks_only():
    migration = (
        ROOT / "migrations" / "0126_blog_post_links.sql"
    ).read_text(encoding="utf-8")
    for forbidden in ("ip_address", "user_agent", "session_id", "visitor_id"):
        assert forbidden not in migration
    connection = sqlite3.connect(":memory:")
    try:
        connection.executescript(
            """
            CREATE TABLE schema_meta (
                k TEXT PRIMARY KEY,
                v TEXT NOT NULL
            );
            INSERT INTO schema_meta (k, v)
            VALUES ('legacy_wallet_custody_v2', 'clean-v2:prior');
            """
        )
        connection.executescript(migration)
        columns = {
            row[1] for row in connection.execute(
                "PRAGMA table_info(blog_post_links)"
            )
        }
        connection.execute(
            "INSERT INTO blog_post_links "
            "(slug,network,url,updated_at,updated_by_bi) VALUES (?,?,?,?,?)",
            ("actions", "mastodon", "https://mastodon.social/@forkmesh/1",
             1, "actor-bi"),
        )
        with pytest.raises(sqlite3.IntegrityError):
            connection.execute(
                "INSERT INTO blog_post_links (slug,network,url) "
                "VALUES (?,?,?)",
                ("actions", "bluesky", "https://example.test/1"),
            )
        metadata = dict(connection.execute("SELECT k, v FROM schema_meta"))
    finally:
        connection.close()

    assert columns == {
        "slug", "network", "url", "updated_at", "updated_by_bi",
    }
    assert metadata["legacy_wallet_custody_v2"] == "clean-v2:prior"
    # Its own stamp: the head fingerprint moved on with the later migration.
    fingerprint = next(
        value for name, value in _migration_schema_fingerprint_stamps()
        if name == "0126_blog_post_links.sql"
    )
    assert metadata["schema_fingerprint:" + fingerprint] == "applied"


def test_blog_view_dedupe_migration_keeps_only_blinded_reader_keys():
    migration = (
        ROOT / "migrations" / "0127_blog_post_view_seen.sql"
    ).read_text(encoding="utf-8")
    for forbidden in ("ip_address", "user_agent", "session_id", "visitor_id"):
        assert forbidden not in migration
    connection = sqlite3.connect(":memory:")
    try:
        connection.executescript(
            """
            CREATE TABLE schema_meta (
                k TEXT PRIMARY KEY,
                v TEXT NOT NULL
            );
            INSERT INTO schema_meta (k, v)
            VALUES ('legacy_wallet_custody_v2', 'clean-v2:prior');
            """
        )
        connection.executescript(migration)
        columns = {
            row[1] for row in connection.execute(
                "PRAGMA table_info(blog_post_view_seen)"
            )
        }
        # One row per reader per post: a reload updates the row it already has
        # rather than adding a second read, and `counted_at` stays on the read
        # that moved the counter so the window cannot slide forever.
        for index, counted_at in enumerate((10, 0, 0)):
            connection.execute(
                "INSERT INTO blog_post_view_seen "
                "(slug,viewer_key,first_at,last_at,counted_at,hits,"
                "referrer_host) VALUES (?,?,?,?,?,1,?) "
                "ON CONFLICT(slug,viewer_key) DO UPDATE SET "
                "last_at=excluded.last_at,hits=blog_post_view_seen.hits+1,"
                "counted_at=MAX(blog_post_view_seen.counted_at,"
                "excluded.counted_at)",
                ("actions", "blinded", 10, 10 + index, counted_at,
                 "mastodon.social"),
            )
        rows = list(connection.execute(
            "SELECT hits,counted_at,last_at FROM blog_post_view_seen"))
        indexes = {
            row[1] for row in connection.execute(
                "PRAGMA index_list(blog_post_view_seen)"
            )
        }
        metadata = dict(connection.execute("SELECT k, v FROM schema_meta"))
    finally:
        connection.close()

    assert columns == {
        "slug", "viewer_key", "first_at", "last_at", "counted_at", "hits",
        "referrer_host",
    }
    assert rows == [(3, 10, 12)]
    assert "idx_blog_post_view_seen_age" in indexes
    assert metadata["legacy_wallet_custody_v2"] == "clean-v2:prior"
    # 0127 stamps the fingerprint of the build it shipped in, and the markers
    # are append-only — the prior 'schema_fingerprint:prior' marker survives
    # alongside it. Which fingerprint the *current* build stamps is pinned by
    # test_latest_native_migration_stamps_exact_runtime_schema_fingerprint
    # against the newest migration, not here.
    assert dict(_migration_schema_fingerprint_stamps())[
        "0127_blog_post_view_seen.sql"] in {
            key.split(":", 1)[1] for key in metadata
            if key.startswith("schema_fingerprint:")}


def test_activitypub_thread_lifecycle_migration_upgrades_remote_replies():
    initial = (ROOT / "migrations" / "0028_activitypub.sql").read_text(
        encoding="utf-8"
    )
    upgrade = (
        ROOT / "migrations" / "0065_activitypub_thread_lifecycle.sql"
    ).read_text(encoding="utf-8")
    connection = sqlite3.connect(":memory:")
    try:
        connection.executescript(initial)
        connection.execute(
            "INSERT INTO ap_comments "
            "(context_bi,remote_id_bi,data,ts) VALUES (?,?,?,?)",
            ("context", "remote", "encrypted", 1),
        )
        connection.executescript(upgrade)
        columns = {
            row[1]: row for row in connection.execute(
                "PRAGMA table_info(ap_comments)"
            )
        }
        assert "parent_remote_id_bi" in columns
        assert "lifecycle" in columns
        assert connection.execute(
            "SELECT lifecycle,data FROM ap_comments"
        ).fetchone() == ("active", "encrypted")
        indexes = {
            row[1] for row in connection.execute(
                "PRAGMA index_list(ap_comments)"
            )
        }
        assert "idx_ap_comments_parent" in indexes
        try:
            connection.execute(
                "UPDATE ap_comments SET lifecycle='native-signed'"
            )
            raise AssertionError("invalid lifecycle was accepted")
        except sqlite3.IntegrityError:
            pass
    finally:
        connection.close()


def test_lazy_schema_upgrade_adds_columns_before_dependent_indexes():
    entry_path = ROOT / "src" / "entry.py"
    schema_path = ROOT / "src" / "schema.py"
    initial = (ROOT / "migrations" / "0028_activitypub.sql").read_text(
        encoding="utf-8"
    )
    pre_alters = _literal_assignment(
        entry_path, "SCHEMA_PRE_CREATE_ALTER_STATEMENTS"
    )
    schema_statements = _literal_assignment(schema_path, "SCHEMA_STATEMENTS")
    connection = sqlite3.connect(":memory:")
    try:
        connection.executescript(initial)
        for statement in pre_alters:
            if "ap_comments" not in statement and "ap_outbox" not in statement:
                continue
            connection.execute(statement)
        for statement in schema_statements:
            if "ap_comments" in statement or "ap_outbox" in statement:
                connection.execute(statement)
        columns = {
            row[1] for row in connection.execute("PRAGMA table_info(ap_comments)")
        }
        outbox_columns = {
            row[1] for row in connection.execute("PRAGMA table_info(ap_outbox)")
        }
        indexes = {
            row[1] for row in connection.execute("PRAGMA index_list(ap_comments)")
        }
    finally:
        connection.close()

    assert {"parent_remote_id_bi", "lifecycle"}.issubset(columns)
    assert "dedupe_bi" in outbox_columns
    assert "idx_ap_comments_parent" in indexes
    apply_schema = entry_path.read_text(encoding="utf-8").split(
        "async def _apply_schema(env):", 1
    )[1].split("\ndef js_nullish", 1)[0]
    assert apply_schema.index("SCHEMA_PRE_CREATE_ALTER_STATEMENTS") < (
        apply_schema.index("SCHEMA_STATEMENTS")
    )


def test_lazy_schema_upgrade_adds_encrypted_chat_member_payload_column():
    entry_path = ROOT / "src" / "entry.py"
    schema_path = ROOT / "src" / "schema.py"
    schema_statements = _literal_assignment(schema_path, "SCHEMA_STATEMENTS")
    post_alters = _literal_assignment(entry_path, "SCHEMA_ALTER_STATEMENTS")
    connection = sqlite3.connect(":memory:")
    try:
        connection.executescript(
            """
            CREATE TABLE chat_channels (
                channel_id TEXT PRIMARY KEY,
                name_bi TEXT NOT NULL UNIQUE,
                data TEXT NOT NULL,
                created_by_bi TEXT NOT NULL,
                created_at INTEGER NOT NULL,
                updated_at INTEGER NOT NULL,
                key_version INTEGER NOT NULL DEFAULT 1
            );
            CREATE TABLE chat_channel_members (
                channel_id TEXT NOT NULL,
                member_bi TEXT NOT NULL,
                invited_by_bi TEXT NOT NULL,
                joined_at INTEGER NOT NULL,
                PRIMARY KEY (channel_id, member_bi)
            );
            """
        )
        for statement in schema_statements:
            if "chat_channel" in statement:
                connection.execute(statement)
        for statement in post_alters:
            if "chat_channel_members" in statement:
                connection.execute(statement)
        columns = {
            row[1]
            for row in connection.execute(
                "PRAGMA table_info(chat_channel_members)"
            )
        }
    finally:
        connection.close()

    assert "data" in columns


def test_release_downloads_user_agent_migration_bootstraps_fresh_local_db():
    migration = (ROOT / "migrations" / "0034_release_downloads_ua.sql").read_text(
        encoding="utf-8"
    )
    connection = sqlite3.connect(":memory:")
    try:
        connection.executescript(migration)
        columns = {
            row[1]
            for row in connection.execute("PRAGMA table_info(release_downloads)")
        }
    finally:
        connection.close()

    assert {"repo_bi", "sha256", "ts", "ua"}.issubset(columns)


def _unique_email_setup(connection):
    # Minimal accounts/users tables + the old non-unique email indexes that
    # migration 0041 replaces, seeded with duplicate and keyless-email rows.
    connection.executescript(
        """
        CREATE TABLE accounts (name_bi TEXT PRIMARY KEY, data TEXT NOT NULL,
            email_bi TEXT, name TEXT, is_admin INTEGER NOT NULL DEFAULT 0,
            ip_bi TEXT, enable_outreach INTEGER NOT NULL DEFAULT 0);
        CREATE INDEX idx_accounts_email ON accounts(email_bi);
        CREATE TABLE users (user_bi TEXT PRIMARY KEY, data TEXT NOT NULL,
            email_bi TEXT, username TEXT, is_admin INTEGER NOT NULL DEFAULT 0,
            ip_bi TEXT, enable_outreach INTEGER NOT NULL DEFAULT 0);
        CREATE INDEX idx_users_email ON users(email_bi);
        """
    )
    for table, key in (("accounts", "name_bi"), ("users", "user_bi")):
        connection.executescript(
            f"""
            INSERT INTO {table} ({key}, data, email_bi) VALUES
                ('first',  '{{}}', 'dup@'),
                ('second', '{{}}', 'dup@'),
                ('other',  '{{}}', 'solo@'),
                ('keyless1', '{{}}', NULL),
                ('keyless2', '{{}}', NULL);
            """
        )


def test_unique_email_migration_dedupes_keeping_first_and_enforces_uniqueness():
    migration = (ROOT / "migrations" / "0041_unique_account_email.sql").read_text(
        encoding="utf-8"
    )
    connection = sqlite3.connect(":memory:")
    try:
        _unique_email_setup(connection)
        connection.executescript(migration)

        for table, key in (("accounts", "name_bi"), ("users", "user_bi")):
            # The first duplicate row is kept, the later one is deleted.
            kept = {
                row[0]
                for row in connection.execute(
                    f"SELECT {key} FROM {table} WHERE email_bi = 'dup@'"
                )
            }
            assert kept == {"first"}, table
            # Non-duplicate and keyless rows are all preserved.
            total = connection.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0]
            assert total == 4, table  # first, other, keyless1, keyless2

            # A fresh duplicate email is now rejected by the unique index...
            try:
                connection.execute(
                    f"INSERT INTO {table} ({key}, data, email_bi) "
                    f"VALUES ('late', '{{}}', 'solo@')"
                )
                raise AssertionError(f"{table} allowed a duplicate email")
            except sqlite3.IntegrityError:
                pass
            # ...but additional keyless (NULL-email) rows are still allowed.
            connection.execute(
                f"INSERT INTO {table} ({key}, data, email_bi) "
                f"VALUES ('keyless3', '{{}}', NULL)"
            )
    finally:
        connection.close()


def test_founders_outreach_migration_drops_only_the_retired_console_tables():
    migration = (ROOT / "migrations" / "0177_founders_outreach_to_crm.sql"
                 ).read_text(encoding="utf-8")
    down = (ROOT / "migrations" / "down" / "0177_founders_outreach_to_crm.sql"
            ).read_text(encoding="utf-8")
    schema_text = (ROOT / "src" / "schema.py").read_text(encoding="utf-8")
    # The CRM package owns founders outreach now; core no longer declares the
    # retired roster/log, but keeps the users.enable_outreach column layout.
    assert "outreach_team" not in schema_text
    assert "outreach_log" not in schema_text
    assert "enable_outreach INTEGER NOT NULL DEFAULT 0" in schema_text
    connection = sqlite3.connect(":memory:")
    try:
        connection.executescript("""
            CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT NOT NULL);
            CREATE TABLE users (user_bi TEXT PRIMARY KEY, username TEXT,
                enable_outreach INTEGER NOT NULL DEFAULT 0);
            INSERT INTO users VALUES ('bi:alice', 'alice', 1);
            CREATE TABLE outreach_team (name_bi TEXT PRIMARY KEY, name TEXT NOT NULL,
                added_by TEXT, added_at INTEGER NOT NULL);
            CREATE TABLE outreach_log (id INTEGER PRIMARY KEY AUTOINCREMENT,
                ts INTEGER NOT NULL, sender TEXT NOT NULL,
                ok INTEGER NOT NULL DEFAULT 1, data TEXT NOT NULL);
            CREATE INDEX idx_outreach_log_sender_ts ON outreach_log(sender, ts);
        """)
        connection.executescript(migration)
        tables = {row[0] for row in connection.execute(
            "SELECT name FROM sqlite_master WHERE type='table'")}
        assert "outreach_team" not in tables and "outreach_log" not in tables
        assert connection.execute(
            "SELECT enable_outreach FROM users").fetchone() == (1,)
        # Re-running is a no-op, and the down restores the (empty) tables.
        connection.executescript(migration)
        connection.executescript(down)
        tables = {row[0] for row in connection.execute(
            "SELECT name FROM sqlite_master WHERE type='table'")}
        assert {"outreach_team", "outreach_log"} <= tables
        assert connection.execute(
            "SELECT COUNT(*) FROM schema_meta").fetchone() == (0,)
    finally:
        connection.close()


def test_drop_accounts_migration_removes_only_the_legacy_table():
    migration = (ROOT / "migrations" / "0042_drop_accounts.sql").read_text(
        encoding="utf-8"
    )
    connection = sqlite3.connect(":memory:")
    try:
        _unique_email_setup(connection)
        connection.executescript(migration)

        tables = {
            row[0]
            for row in connection.execute(
                "SELECT name FROM sqlite_master WHERE type='table'"
            )
        }
        assert "accounts" not in tables
        assert "users" in tables
        # The users rows (the authoritative store) are untouched.
        total = connection.execute("SELECT COUNT(*) FROM users").fetchone()[0]
        assert total == 5

        # Re-running is a no-op (DROP TABLE IF EXISTS), matching how
        # ensure_schema replays the same statement on lazily-created DBs.
        connection.executescript(migration)
    finally:
        connection.close()


def test_social_directory_migration_preserves_rows_and_widens_kinds():
    initial = (
        ROOT / "migrations" / "0052_world_fediverse_media.sql"
    ).read_text(encoding="utf-8")
    upgrade = (
        ROOT / "migrations" / "0061_world_social_directory.sql"
    ).read_text(encoding="utf-8")
    connection = sqlite3.connect(":memory:")
    try:
        connection.executescript(initial)
        connection.execute(
            "INSERT INTO world_fediverse_instances "
            "(instance_id,kind,host,url,data,created_by_bi,created_at,updated_at) "
            "VALUES (?,?,?,?,?,?,?,?)",
            ("a" * 32, "mastodon", "social.example.org",
             "https://social.example.org/", "{}", "admin-bi", 1, 1),
        )
        connection.executescript(upgrade)
        assert connection.execute(
            "SELECT kind,host FROM world_fediverse_instances"
        ).fetchall() == [("mastodon", "social.example.org")]
        connection.execute(
            "INSERT INTO world_fediverse_instances "
            "(instance_id,kind,host,url,data,created_by_bi,created_at,updated_at) "
            "VALUES (?,?,?,?,?,?,?,?)",
            ("b" * 32, "x", "x.com", "https://x.com/", "{}",
             "admin-bi", 2, 2),
        )
        try:
            connection.execute(
                "INSERT INTO world_fediverse_instances "
                "(instance_id,kind,host,url,data,created_by_bi,created_at,"
                "updated_at) VALUES (?,?,?,?,?,?,?,?)",
                ("c" * 32, "unsupported", "bad.example.org",
                 "https://bad.example.org/", "{}", "admin-bi", 3, 3),
            )
            raise AssertionError("unsupported directory kind was accepted")
        except sqlite3.IntegrityError:
            pass
    finally:
        connection.close()
