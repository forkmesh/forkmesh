"""The generic admin browser must not bypass domain authorization."""

import ast
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
ADMIN_CONSOLE = ROOT / "src" / "admin_console.py"
TEXT = (
    ENTRY.read_text(encoding="utf-8") + "\n"
    + ENTRY.with_name("app_runtime.py").read_text(encoding="utf-8") + "\n"
    + ADMIN_CONSOLE.read_text(encoding="utf-8")
)


def _function_source(name):
    tree = ast.parse(TEXT)
    for node in tree.body:
        if (
            isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
            and node.name == name
        ):
            return ast.get_source_segment(TEXT, node)
    raise AssertionError(name)


def test_admin_inventory_lists_all_application_tables_but_keeps_sensitive_rows_restricted():
    assert "ADMIN_VISIBLE_TABLES" not in TEXT
    assert "def _admin_purge_allowed(table):" in TEXT
    table_list = _function_source("_admin_list_tables")
    assert "sqlite_%" in table_list
    assert "_cf_%" in table_list
    assert "ADMIN_HIDDEN_TABLES" not in table_list

    hidden = TEXT.split("ADMIN_HIDDEN_TABLES = (", 1)[1].split(")", 1)[0]
    for table in (
        "account_devices",
        "account_ssh_keys",
        "nodes",
        "org_members",
        "mirror_https_endpoints",
        "mirror_repository_integrity",
        "private_mirror_routes",
        "reward_node_observations",
        "reward_rounds",
        "funds_received",
        "clone_rr",
    ):
        assert f'"{table}"' in hidden

    table_view = _function_source("_render_table_view")
    assert "if table in ADMIN_HIDDEN_TABLES" in table_view
    assert "This table is restricted." in table_view


def test_generic_insert_and_update_fail_closed_without_database_writes():
    for function_name in ("_admin_update_row", "_admin_insert_row"):
        source = _function_source(function_name)
        assert "blocked" in source.lower()
        assert "d1_run" not in source
        assert "UPDATE " not in source
        assert "INSERT INTO " not in source

    row_form = _function_source("_render_row_form")
    assert "Generic row editing is disabled" in row_form
    assert "textarea" not in row_form
    assert "decrypt_row" not in row_form


def test_operational_purges_are_allowlisted_and_content_digested():
    admin = _function_source("_admin_selected_rows_digest")
    assert "_admin_purge_allowed(table)" in admin
    assert "hashlib.sha256(canonical).hexdigest()" in admin

    start = TEXT.index("    async def _admin(self, request):")
    relay_admin = TEXT[
        start:TEXT.index("\n    async def _route(", start)
    ]
    assert "_admin_purge_allowed(table)" in relay_admin
    assert '"rowDigestBefore"' in relay_admin
    assert "generic database mutation is " in relay_admin
    assert "purpose-built audited action" in relay_admin


def test_bulk_delete_is_blocked_only_for_hidden_tables():
    # Row selection + "Delete selected" is the default for every table whose
    # rows actually render; only ADMIN_HIDDEN_TABLES stays fully read-only.
    purge_fn = _function_source("_admin_purge_allowed")
    assert "table not in ADMIN_HIDDEN_TABLES" in purge_fn

    table_view = _function_source("_render_table_view")
    assert "purge_allowed = _admin_purge_allowed(table)" in table_view


def test_table_view_discloses_which_columns_are_encrypted():
    # An admin looking at a decrypted `data` blob must be told it was
    # ciphertext at rest, not silently shown plaintext-looking values.
    table_view = _function_source("_render_table_view")
    assert '"data" in columns' in table_view
    assert "AES-GCM encrypted and decrypted" in table_view
    assert "are stored as plaintext" in table_view
    assert "No encrypted columns in this table" in table_view


def test_local_reward_snapshot_reverifies_signed_evidence_and_operations():
    source = _function_source("_eligible_reward_snapshot")
    assert "_verified_federated_reward_attestation" in source
    # Local signing keys are fetched in one bounded query now; keep the same
    # owner-signing authorization gates the former per-owner helper enforced.
    assert "allowed_identity_keys = {}" in source
    assert "FROM account_devices" in source
    assert "enabled=1 AND revoked_at=0" in source
    assert 'if "owner_sign" not in capabilities:' in source
    assert "public_key in allowed_identity_keys.get(node_bi, set())" in source
    assert "https_routing.operations_sha256(operations)" in source
    assert "HTTPS_MIRROR_REQUIRED_FORKMESH_OPERATIONS.issubset" in source
    assert '"identitySignatureVerified": endpoint_ok' in source
    assert "forkmesh_operations_json" in source


def test_operations_evidence_upgrade_is_durable():
    migration = (
        ROOT / "migrations" / "0072_reward_attestation_operations.sql"
    ).read_text(encoding="utf-8")
    schema = (ROOT / "src" / "schema.py").read_text(encoding="utf-8")
    assert "ADD COLUMN forkmesh_operations_json" in migration
    assert "forkmesh_operations_json TEXT NOT NULL DEFAULT '[]'" in schema
    assert (
        "ADD COLUMN forkmesh_operations_json TEXT NOT NULL DEFAULT '[]'"
        in TEXT
    )
