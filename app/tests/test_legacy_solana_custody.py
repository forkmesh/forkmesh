"""Fail-closed contracts for the offline historical-wallet migration."""

import base64
import hashlib
import importlib.util
import json
from pathlib import Path
import sqlite3
import sys
import ast
from _maintenance_stubs import with_maintenance_off


ROOT = Path(__file__).resolve().parents[2]
WORKER_ROOT = ROOT / "app"
ENTRY = WORKER_ROOT / "src" / "entry.py"
APP_RUNTIME = WORKER_ROOT / "src" / "app_runtime.py"
SOLANA = WORKER_ROOT / "src" / "solana.py"
SCHEMA = WORKER_ROOT / "src" / "schema.py"
TOOL_PATH = ROOT / "tools" / "legacy_solana_custody.py"
DOC = ROOT / "www" / "docs" / "operations" / "legacy-solana-custody-migration.md"

SPEC = importlib.util.spec_from_file_location("legacy_solana_custody", TOOL_PATH)
custody = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = custody
SPEC.loader.exec_module(custody)

DATA_KEY = "historical-data-key-that-is-not-an-artifact-passphrase"
PASSPHRASE = "new-owner-artifact-passphrase"


def _seed_and_address(byte=7):
    seed = bytes([byte]) * 32
    encoded = base64.urlsafe_b64encode(seed).decode().rstrip("=")
    return encoded, custody._derive_solana_address(encoded)


def _database(tmp_path):
    database = tmp_path / "snapshot.sqlite"
    connection = sqlite3.connect(database)
    connection.executescript(
        """
        CREATE TABLE users (
          user_bi TEXT PRIMARY KEY,
          data TEXT NOT NULL
        );
        CREATE TABLE issue_bounty (
          bounty_bi TEXT PRIMARY KEY,
          data TEXT NOT NULL
        );
        CREATE TABLE central_fund (
          id INTEGER PRIMARY KEY,
          data TEXT NOT NULL
        );
        """
    )
    donation_seed, donation_address = _seed_and_address(7)
    bounty_seed, bounty_address = _seed_and_address(9)
    user = {
        "name": "legacy-node",
        "donation_address": donation_address,
        "donation_secret": donation_seed,
        "donation_required_lamports": 100,
        "donation_confirmed": True,
    }
    bounty = {
        "owner": "legacy-node",
        "repo": "project",
        "number": 4,
        "address": bounty_address,
        "secret": bounty_seed,
        "status": "funded",
        "payout_tx": "obsolete-public-signed-bytes",
    }
    connection.execute(
        "INSERT INTO users (user_bi, data) VALUES (?,?)",
        ("blind-user", custody.encrypt_d1_record(user, DATA_KEY)),
    )
    connection.execute(
        "INSERT INTO issue_bounty (bounty_bi, data) VALUES (?,?)",
        ("blind-bounty", custody.encrypt_d1_record(bounty, DATA_KEY)),
    )
    connection.commit()
    connection.close()
    return database, (donation_seed, bounty_seed)


def test_inventory_verifies_keys_but_exposes_only_public_metadata(tmp_path):
    database, seeds = _database(tmp_path)
    records, scanned = custody.scan_legacy_records(database, DATA_KEY)
    assert len(records) == 2
    assert scanned == {"central_fund": 0, "issue_bounty": 1, "users": 1}
    assert all("seed=<redacted>" in repr(record) for record in records)
    audit = custody.build_public_audit(
        records,
        scanned,
        network="mainnet-beta",
        rpc_url="https://rpc.example.invalid/with-private-routing-token",
        balance_reader=lambda _url, _address: (0, 12345),
        now=1000,
    )
    rendered = json.dumps(audit)
    assert audit["keyBearingRecordCount"] == 2
    assert audit["allBalancesReconciled"] is True
    assert audit["rpcOrigin"] == "https://rpc.example.invalid"
    for seed in seeds:
        assert seed not in rendered
    assert "with-private-routing-token" not in rendered


def test_encrypted_artifact_round_trip_and_scrub_sql_never_leak_seed(tmp_path):
    database, seeds = _database(tmp_path)
    records, scanned = custody.scan_legacy_records(database, DATA_KEY)
    audit = custody.build_public_audit(
        records,
        scanned,
        network="devnet",
        rpc_url="https://api.devnet.solana.com",
        balance_reader=lambda _url, _address: (0, 777),
        now=2000,
    )
    artifact = custody.create_artifact(
        records,
        audit,
        PASSPHRASE,
        now=2000,
        scrypt_n=1 << 15,
    )
    rendered_artifact = json.dumps(artifact)
    for seed in seeds:
        assert seed not in rendered_artifact
    payload = custody.open_artifact(artifact, PASSPHRASE)
    custody.verify_artifact_matches_records(payload, records)

    artifact_digest = custody.artifact_sha256(artifact)
    audit_digest = hashlib.sha256(custody._canonical_json(audit)).hexdigest()
    sql = custody.build_scrub_sql(
        records,
        DATA_KEY,
        artifact_digest=artifact_digest,
        audit_digest=audit_digest,
        confirmed_at=3000,
        prepared_at=4000,
        reconciliation_records=audit["records"],
        network="devnet",
    )
    for seed in seeds:
        assert seed not in sql
    assert "do not add BEGIN/COMMIT" in sql
    assert "BEGIN IMMEDIATE" not in sql
    assert "WHERE \"user_bi\"='blind-user' AND data=" in sql
    assert "CASE WHEN changes()=1" in sql
    assert "legacy_custody_migration_audit" in sql

    connection = sqlite3.connect(database)
    connection.executescript(sql)
    connection.close()
    remaining, _ = custody.scan_legacy_records(database, DATA_KEY)
    assert remaining == []
    connection = sqlite3.connect(database)
    stored = connection.execute(
        "SELECT data FROM issue_bounty WHERE bounty_bi='blind-bounty'"
    ).fetchone()
    user_stored = connection.execute(
        "SELECT data FROM users WHERE user_bi='blind-user'"
    ).fetchone()[0]
    cleaned = custody.decrypt_d1_record(user_stored, DATA_KEY)
    receipt = connection.execute(
        "SELECT status, artifact_sha256, record_count "
        "FROM legacy_custody_migration_audit"
    ).fetchone()
    connection.close()
    assert stored is None
    assert "donation_secret" not in cleaned
    assert "payout_tx" not in cleaned
    assert cleaned["legacy_custody_migrated_at"] == 4000
    assert receipt == ("scrubbed", artifact_digest, 2)


def test_scan_fails_closed_on_wrong_key_or_seed_address_mismatch(tmp_path):
    database, _ = _database(tmp_path)
    try:
        custody.scan_legacy_records(database, "wrong-historical-key")
    except custody.CustodyMigrationError as error:
        assert str(error).startswith("legacy_scan_failed_for_")
    else:
        raise AssertionError("wrong DATA_KEY must fail closed")

    connection = sqlite3.connect(database)
    bad_seed, _ = _seed_and_address(13)
    _, other_address = _seed_and_address(14)
    record = {"address": other_address, "secret": bad_seed}
    connection.execute(
        "UPDATE issue_bounty SET data=? WHERE bounty_bi='blind-bounty'",
        (custody.encrypt_d1_record(record, DATA_KEY),),
    )
    connection.commit()
    connection.close()
    try:
        custody.scan_legacy_records(database, DATA_KEY)
    except custody.CustodyMigrationError as error:
        assert str(error).startswith("legacy_scan_failed_for_")
    else:
        raise AssertionError("seed/address mismatch must fail closed")


def test_scrub_requires_zero_balances_and_explicit_separate_confirmation():
    source = TOOL_PATH.read_text(encoding="utf-8")
    assert custody.SCRUB_CONFIRMATION in source
    assert "confirmation.get(\"artifactSha256\")" in source
    assert "confirmation.get(\"auditSha256\")" in source
    assert "if nonzero:" in source
    assert "refusing_to_scrub_" in source
    assert "D1 was not contacted or changed" in source
    assert '"method": "getBalance"' in source
    assert "sendTransaction" not in source
    assert ".sign(" not in source


def test_signing_code_exists_only_for_the_gated_deposit_sweep():
    """Signing is back, but only for custodial element-store deposits.

    The legacy per-signup deposit funnel stays dead (asserted below and in the
    sibling tests). What is restored is a narrow path: a temporary per-purchase
    address, swept 50/50 to the treasury and online mirrors, whose key is
    destroyed on success. This test pins that narrowness rather than the old
    "no signing code anywhere" rule.
    """
    worker = (
    # entry.py + its lazily-split domain modules
    (ENTRY.parent / "federation.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "ssh_gateway_routes.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "repo_sync.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "agent_routes.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
)
    solana = SOLANA.read_text(encoding="utf-8")
    # The read-only helper must stay incapable of sending, so a future caller
    # cannot reach sendTransaction through it with a dynamic method string.
    assert "_SOLANA_READ_ONLY_METHODS" in solana
    read_only = solana[
        solana.index("_SOLANA_READ_ONLY_METHODS = frozenset({")
        : solana.index("def _solana_endpoints")
    ]
    assert "sendTransaction" not in read_only
    assert '_SOLANA_WRITE_METHODS = frozenset({"sendTransaction"})' in solana
    assert "async def _solana_rpc_write(env, method, params):" in solana
    rpc = solana[
        solana.index("async def _solana_rpc(env, method, params):")
        : solana.index("async def _solana_rpc_call")
    ]
    assert "if method not in _SOLANA_READ_ONLY_METHODS:" in rpc
    assert "sendTransaction" not in rpc

    # Broadcasting is reachable only through the write helper.
    send = worker[
        worker.index("async def _solana_send_transaction(env, tx_bytes):")
        : worker.index("async def _solana_send_transfers")
    ]
    assert "_solana_rpc_write(" in send

    # The sweep is the only caller, and it is gated + destroys the key.
    sweep = worker[
        worker.index("async def _sweep_element_deposit")
        : worker.index("async def _record_sweep_error")
    ]
    assert "_solana_send_transfers(env, from_addr, seed, transfers)" in sweep
    assert "deposit_secret=''" in sweep
    assert 'if row.get("sweep_signature"):' in sweep
    assert "_custodial_deposits_enabled" in worker
    gate = worker[
        worker.index("def _custodial_deposits_enabled(env):")
        : worker.index("async def _online_mirror_payout_addresses")
    ]
    assert 'flag in ("0", "false", "off", "no")' in gate
    assert "return bool(_deposit_treasury_address(env))" in gate

    handler = worker[
        worker.index("async def bounties_handler")
        : worker.index("\n\n# Cap on collaborators")
    ]
    assert 'if action in ("wallet", "create", "payout"):' in handler
    for forbidden in (
        "donation_secret",
        '"secret"',
        "_save_bounty",
        "_new_solana_keypair",
        "_solana_send",
        "_solana_broadcast",
    ):
        assert forbidden not in handler
    assert "_account_has_legacy_wallet_key(rec)" in worker
    assert 'raise RuntimeError("legacy_custody_migration_required")' in worker


def test_historical_status_and_admin_views_cannot_rewrite_or_reveal_keys():
    # The admin console (with its purge-table allowlist) loads on demand from
    # its own module, outside Worker startup.
    worker = (
        ENTRY.read_text(encoding="utf-8") + "\n"
        + ENTRY.with_name("admin_console.py").read_text(encoding="utf-8")
        + "\n" + APP_RUNTIME.read_text(encoding="utf-8")
        + "\n" + ENTRY.with_name("federation.py").read_text(encoding="utf-8")
    )
    account_status = worker[
        worker.index("async def _account_donation_status")
        : worker.index("def _signup_metadata")
    ]
    federation_status = worker[
        worker.index("async def _federation_donation_status")
        : worker.index("async def _federation_nodes")
    ]
    bounty_status = worker[
        worker.index("async def bounties_handler")
        : worker.index("\n\n# Cap on collaborators")
    ]
    for body in (account_status, federation_status, bounty_status):
        assert "donation_secret" not in body
        assert "await encrypt_row" not in body
        assert "_save_account" not in body
        assert "_save_bounty" not in body
    assert '"accounts",' in worker
    assert '"federated_signup",' in worker
    assert '"relay_self",' in worker
    assert "_admin_redact_wallet_keys(decoded)" in worker
    assert "wallet_private_key_fields_are_not_accepted" in worker
    assert "_admin_selected_rows_have_wallet_keys" in worker
    assert "Delete blocked: a selected row contains frozen" in worker


def test_schema_and_runbook_keep_legacy_fields_explicitly_compatibility_only():
    schema = SCHEMA.read_text(encoding="utf-8")
    migration = (
        WORKER_ROOT / "migrations" / "0049_legacy_custody_audit.sql"
    ).read_text(encoding="utf-8")
    docs = DOC.read_text(encoding="utf-8")
    assert "CREATE TABLE IF NOT EXISTS legacy_custody_migration_audit" in schema
    assert "CREATE TABLE IF NOT EXISTS legacy_custody_migration_audit" in migration
    assert "never stores a key" in migration
    assert "The tool intentionally has no “sweep” command." in docs
    assert custody.SCRUB_CONFIRMATION in docs


def test_every_historical_wallet_shape_scrubs_idempotently_to_public_evidence(
        tmp_path):
    database = tmp_path / "all-legacy-shapes.sqlite"
    connection = sqlite3.connect(database)
    connection.executescript(
        """
        CREATE TABLE users (user_bi TEXT PRIMARY KEY, data TEXT NOT NULL);
        CREATE TABLE issue_bounty (
          bounty_bi TEXT PRIMARY KEY, data TEXT NOT NULL);
        CREATE TABLE bounty_wallet (
          wallet_bi TEXT PRIMARY KEY, data TEXT NOT NULL);
        CREATE TABLE federated_signup (
          reference TEXT PRIMARY KEY, relay_bi TEXT NOT NULL,
          data TEXT NOT NULL, created_at INTEGER NOT NULL);
        CREATE TABLE central_fund (
          id INTEGER PRIMARY KEY, data TEXT NOT NULL);
        """
    )
    fixtures = [
        ("users", "user_bi", "user", 21, "donation_secret",
         "donation_address"),
        ("issue_bounty", "bounty_bi", "issue", 22, "secret", "address"),
        ("bounty_wallet", "wallet_bi", "wallet", 23, "secret", "address"),
        ("federated_signup", "reference", "signup", 24,
         "donation_secret", "donation_address"),
        ("central_fund", "id", 1, 25, "secret", "address"),
    ]
    seeds = []
    for table, pk_column, pk_value, seed_byte, key_field, address_field in fixtures:
        seed, address = _seed_and_address(seed_byte)
        seeds.append(seed)
        record = {
            key_field: seed,
            address_field: address,
            "status": "historical",
            "payout_tx": "obsolete-signed-transaction-bytes",
        }
        columns = f"{pk_column},data"
        values = [pk_value, custody.encrypt_d1_record(record, DATA_KEY)]
        if table == "federated_signup":
            columns = "reference,relay_bi,data,created_at"
            values = [
                pk_value, "blind-relay",
                custody.encrypt_d1_record(record, DATA_KEY), 1,
            ]
        connection.execute(
            f"INSERT INTO {table} ({columns}) VALUES "
            f"({','.join('?' for _ in values)})",
            values,
        )
    connection.commit()
    connection.close()

    records, scanned = custody.scan_legacy_records(database, DATA_KEY)
    assert len(records) == 5
    assert scanned == {
        "bounty_wallet": 1,
        "central_fund": 1,
        "federated_signup": 1,
        "issue_bounty": 1,
        "users": 1,
    }
    audit = custody.build_public_audit(
        records,
        scanned,
        network="devnet",
        rpc_url="https://api.devnet.solana.com",
        balance_reader=lambda _url, _address: (0, 9001),
        now=5_000,
    )
    artifact = custody.create_artifact(
        records, audit, PASSPHRASE, now=5_000, scrypt_n=1 << 15)
    sql = custody.build_scrub_sql(
        records,
        DATA_KEY,
        artifact_digest=custody.artifact_sha256(artifact),
        audit_digest=hashlib.sha256(
            custody._canonical_json(audit)).hexdigest(),
        confirmed_at=6_000,
        prepared_at=7_000,
        reconciliation_records=audit["records"],
        network="devnet",
    )
    for seed in seeds:
        assert seed not in sql

    connection = sqlite3.connect(database)
    # Applying the exact reviewed patch twice is a no-op on the second pass.
    connection.executescript(sql)
    connection.executescript(sql)
    for table, pk_column, pk_value, *_rest in fixtures:
        row = connection.execute(
            f"SELECT data FROM {table} WHERE {pk_column}=?",
            (pk_value,),
        ).fetchone()
        if table in custody.RETIRED_ROW_TABLES:
            assert row is None
        else:
            decoded = custody.decrypt_d1_record(row[0], DATA_KEY)
            assert custody._contains_wallet_key(decoded) is False
            assert "payout_tx" not in decoded
    evidence = connection.execute(
        "SELECT source_table,public_address,balance_lamports,rpc_slot "
        "FROM legacy_custody_reconciliation ORDER BY source_table"
    ).fetchall()
    marker = connection.execute(
        "SELECT v FROM schema_meta WHERE k='legacy_wallet_custody_v2'"
    ).fetchone()
    connection.close()
    assert len(evidence) == 5
    assert all(row[1] and row[2:] == (0, 9001) for row in evidence)
    assert marker[0].startswith("clean-v2:")
    remaining, _ = custody.scan_legacy_records(database, DATA_KEY)
    assert remaining == []


def test_unknown_wallet_key_alias_fails_closed_instead_of_being_skipped(
        tmp_path):
    database = tmp_path / "unknown-shape.sqlite"
    connection = sqlite3.connect(database)
    connection.execute(
        "CREATE TABLE users (user_bi TEXT PRIMARY KEY, data TEXT NOT NULL)")
    seed, address = _seed_and_address(31)
    connection.execute(
        "INSERT INTO users (user_bi,data) VALUES (?,?)",
        (
            "unknown",
            custody.encrypt_d1_record(
                {
                    "donation_address": address,
                    "walletPrivateKey": seed,
                },
                DATA_KEY,
            ),
        ),
    )
    connection.commit()
    connection.close()
    try:
        custody.scan_legacy_records(database, DATA_KEY)
    except custody.CustodyMigrationError as error:
        assert str(error) == "legacy_scan_failed_for_1_relevant_row(s)"
    else:
        raise AssertionError("unknown wallet-key spelling must fail closed")


def test_runtime_readiness_gate_blocks_any_key_bearing_shape():
    source = ENTRY.read_text(encoding="utf-8")
    tree = ast.parse(source, filename=str(ENTRY))
    node = next(
        item for item in tree.body
        if isinstance(item, ast.AsyncFunctionDef)
        and item.name == "_assert_legacy_wallet_custody_ready"
    )

    async def run(rows_by_table, marker=None):
        writes = []

        async def d1_first(_env, _sql, *_args):
            return {"v": marker} if marker else None

        async def d1_all(_env, sql, *_args):
            table = sql.rsplit(" ", 1)[-1]
            return [{"data": value} for value in rows_by_table.get(table, [])]

        async def decrypt(_env, value):
            return value

        async def d1_run(_env, sql, *args):
            writes.append((sql, args))

        namespace = {
            "LEGACY_WALLET_CUSTODY_MARKER": "legacy_wallet_custody_v2",
            "LEGACY_WALLET_CUSTODY_TABLES": (
                "users", "issue_bounty", "bounty_wallet",
                "federated_signup", "central_fund"),
            "_SCHEMA_FINGERPRINT": "fingerprint",
            "_admin_contains_wallet_key": custody._contains_wallet_key,
            "d1_first": d1_first,
            "d1_all": d1_all,
            "decrypt_row": decrypt,
            "d1_run": d1_run,
        }
        exec(compile(ast.fix_missing_locations(ast.Module(
            body=[node], type_ignores=[])), str(ENTRY), "exec"), with_maintenance_off(namespace))
        await namespace[node.name](object())
        return writes

    import asyncio

    key_rows = {
        "central_fund": [{
            "address": "11111111111111111111111111111111",
            "secret": "recoverable-wallet-seed",
        }],
    }
    try:
        asyncio.run(run(key_rows))
    except RuntimeError as error:
        assert str(error) == "legacy_custody_migration_required"
    else:
        raise AssertionError("runtime must not claim ready with a retained key")

    writes = asyncio.run(run({"users": [{"name": "clean"}]}))
    assert len(writes) == 1
    assert writes[0][1][0] == "legacy_wallet_custody_v2"
    assert writes[0][1][1].startswith("clean-v2:")
    assert asyncio.run(run(key_rows, marker="clean-v2:reviewed")) == []


def test_readiness_failure_is_public_but_exposes_no_legacy_record_details():
    source = ENTRY.read_text(encoding="utf-8")
    start = source.index("def _legacy_custody_not_ready_response")
    end = source.index("\n\n# --- Edge cache", start)
    body = source[start:end]
    assert '"status": "not_ready"' in body
    assert '"error": "legacy_custody_migration_required"' in body
    assert "offline export" in body
    assert "zero-balance reconciliation" in body
    assert "address" in body
    for forbidden in (
        "donation_secret",
        '"secret"',
        "SELECT ",
        "decrypt_row",
        "legacy_custody_reconciliation",
    ):
        assert forbidden not in body
    runtime = APP_RUNTIME.read_text(encoding="utf-8")
    fetch_start = runtime.index("    async def fetch(self, request):")
    fetch_end = runtime.index("\n    async def _admin", fetch_start)
    fetch = runtime[fetch_start:fetch_end]
    assert (
        'if str(error) == "legacy_custody_migration_required":'
        in fetch
    )
    assert "response = _legacy_custody_not_ready_response()" in fetch
    assert "response = _api_cors_response(" in fetch
    assert "response = _apply_browser_security_headers(" in fetch
