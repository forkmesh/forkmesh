"""A second same-account checkout cannot replace the canonical repo device."""

import ast
from pathlib import Path


# MainWindowRepos.cpp was split into feature files; read them in the original
# source order so section slices that span functions still line up.
MAIN_WINDOW_REPOS_SOURCES = (
    "MainWindowRepos.cpp",
    "MainWindowReposInternal.h",
    "MainWindowRepoSettings.cpp",
    "MainWindowRepoSync.cpp",
    "MainWindowMirrorPush.cpp",
    "MainWindowRepoFetch.cpp",
    "MainWindowAccounts.cpp",
)

ROOT = Path(__file__).resolve().parents[2]
ENTRY = (
    (ROOT / "app/src/entry.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ROOT / "app/src/catalog_routes.py").read_text(encoding="utf-8")
)
SCHEMA = (ROOT / "app/src/schema.py").read_text(encoding="utf-8")
QT_REPOS = "".join(
    (ROOT / "desktop/src" / name).read_text(encoding="utf-8")
    for name in MAIN_WINDOW_REPOS_SOURCES
)


def test_schema_tracks_one_authority_and_many_device_mirrors():
    assert "repo_source_authorities" in SCHEMA
    assert "repo_bi TEXT PRIMARY KEY, node_id TEXT NOT NULL" in SCHEMA
    assert "is_private INTEGER NOT NULL DEFAULT 0" in SCHEMA
    assert "data TEXT NOT NULL DEFAULT ''" in SCHEMA
    assert "PRIMARY KEY (repo_bi, node_id)" in SCHEMA
    migration = ROOT / "app/migrations/0117_repo_source_authorities.sql"
    assert migration.exists()


def test_catalog_demotes_other_devices_before_canonical_write():
    authority = ENTRY.index("SELECT node_id,machine_name,is_private,data ")
    different_device = ENTRY.index("and secondary_device")
    mirror_write = ENTRY.index("INSERT INTO repo_device_mirrors")
    canonical_write = ENTRY.index("_contribution_write_catalog_state", authority)
    assert authority < different_device < mirror_write < canonical_write
    assert 'mirror_record["source"] = "remote-clone"' in ENTRY
    assert '"mirror": True' in ENTRY
    assert "status=202" in ENTRY[different_device:canonical_write]


def test_private_authority_and_secondary_device_metadata_are_not_plaintext():
    assert '"" if private_authority else authority_node_id' in ENTRY
    assert '"" if private_authority else authority_machine' in ENTRY
    assert '"private-repo-device:" + incoming_node_id' in ENTRY
    assert '"DELETE FROM repo_state_history WHERE key_bi=?"' in ENTRY


def test_authority_decision_is_lazy_and_never_transfers_implicitly():
    tree = ast.parse(ENTRY)
    function = next(
        node for node in tree.body
        if isinstance(node, ast.FunctionDef)
        and node.name == "_repo_publication_authority")
    namespace = {}
    exec(compile(ast.fix_missing_locations(ast.Module(
        body=[function], type_ignores=[])), "entry.py", "exec"), namespace)
    decide = namespace["_repo_publication_authority"]

    assert decide("node-main", "", "node-main") == ("node-main", False)
    assert decide("node-main", "", "node-clone") == ("node-main", True)
    # Backward-compatible lazy migration adopts an existing record's device.
    assert decide("", "legacy-main", "node-clone") == ("legacy-main", True)
    # A legacy row with no device identity lets the first modern signed device
    # establish the durable authority once.
    assert decide("", "", "first-modern") == ("first-modern", False)


def test_public_device_mirror_requires_an_explicitly_public_canonical_row():
    tree = ast.parse(ENTRY)
    function = next(
        node for node in tree.body
        if isinstance(node, ast.FunctionDef)
        and node.name == "_device_mirror_has_public_canonical")
    namespace = {}
    exec(compile(ast.fix_missing_locations(ast.Module(
        body=[function], type_ignores=[])), "entry.py", "exec"), namespace)
    allowed = namespace["_device_mirror_has_public_canonical"]

    public_device = {"visibility": "public"}
    assert allowed(public_device, "repo-1", {"repo-1"}) is True
    # Canonical private, deleted, unreadable, or moderation-filtered rows never
    # enter this explicit set, so an older public device row is excluded.
    assert allowed(public_device, "repo-1", set()) is False
    assert allowed(public_device, "repo-1", {"another-repo"}) is False


def test_secondary_public_to_private_transition_barriers_before_overwrite():
    start = ENTRY.index("and secondary_device")
    end = ENTRY.index("if not exists:", start)
    branch = ENTRY[start:end]
    prior_read = branch.index(
        "SELECT data FROM repo_device_mirrors")
    transition = branch.index(
        'prior_device.get("visibility") != "private"')
    barrier = branch.index("invalidate_public_catalog_snapshots")
    encrypt = branch.index("mirror_enc = await encrypt_row")
    write = branch.index("INSERT INTO repo_device_mirrors")
    assert prior_read < transition < barrier < encrypt < write
    # The unchanged-republish fast return may only fire when the device
    # record is NOT a meaningful change — a visibility flip must always fall
    # through to the transition barrier above.
    unchanged = branch.index("if not device_meaningful:")
    assert prior_read < unchanged < transition
    assert "catalog_meaningful_change(prior_device, mirror_record)" in branch


def test_catalog_returns_canonical_and_secondary_nodes_explicitly():
    assert 'rec["canonicalNodeId"] = authority_node_id' in ENTRY
    assert 'rec["sourceAuthority"] = (' in ENTRY
    assert 'mirror["source"] = "remote-clone"' in ENTRY
    assert 'mirror["sourceAuthority"] = False' in ENTRY
    assert "SELECT node_id,data FROM repo_device_mirrors WHERE repo_bi=?" in ENTRY
    assert 'device_record["source"] = "remote-clone"' in ENTRY
    assert 'record.get("machineName") or record.get("owner", "")' in ENTRY


def test_native_server_publication_binds_node_identity_and_source_kind():
    catalog = (ROOT / "server/catalog.go").read_text(encoding="utf-8")
    assert '"machineName": p.Node' in catalog
    assert '"source": "remote-clone"' in catalog
    assert '"runtimeMode": "headless"' in catalog
    assert "catalogSigVersion" not in QT_REPOS


def test_repo_deletion_removes_authority_and_secondary_records():
    delete_start = ENTRY.index("async def _delete_repo_scoped_state")
    delete_end = ENTRY.index("async def _delete_repo_namespace", delete_start)
    deletion = ENTRY[delete_start:delete_end]
    assert "DELETE FROM repo_device_mirrors WHERE repo_bi=?" in deletion
    assert "DELETE FROM repo_source_authorities WHERE repo_bi=?" in deletion


def test_private_publication_keeps_blind_indexed_device_mirrors():
    """Only the public->private flip discards device rows.

    Rows written while the repository was public carry a plaintext node id and
    must go. Rows written as private are already encrypted under a blind device
    index, and the catalog plus HTTPS routing still read them for owner-only
    failover, so an ordinary private republish must not wipe them.
    """
    catalog = (ROOT / "app/src/catalog_routes.py").read_text(encoding="utf-8")
    start = catalog.index('if record.get("visibility") == "private":')
    branch = catalog[start:catalog.index("elif state_hash", start)]
    assert "DELETE FROM repo_state_history WHERE key_bi=?" in branch
    purge = branch.index("DELETE FROM repo_device_mirrors WHERE repo_bi=?")
    guard = branch.index('if exists and prior.get("visibility") == "public":')
    assert guard < purge, "device purge must be gated on the visibility flip"


def test_private_device_mirrors_are_stored_under_a_blind_index():
    assert 'device_storage_id = await blind_index(' in ENTRY
    assert '"private-repo-device:" + incoming_node_id' in ENTRY
    write = ENTRY.index("INSERT INTO repo_device_mirrors")
    derive = ENTRY.index('device_storage_id = await blind_index(')
    assert derive < write
    assert "key_bi, device_storage_id, mirror_enc" in ENTRY
