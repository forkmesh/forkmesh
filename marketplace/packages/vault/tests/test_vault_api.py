"""Password Vault Worker API: shapes, limits, and who may read what.

The module cannot tell a real ciphertext from noise, so these tests file
well-shaped noise and check the rules around it. The cryptography itself is
exercised end to end in test_vault_crypto.py.
"""

import asyncio
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from vault_runtime import Runtime, bi, vault  # noqa: E402


def b64(length, fill="A"):
    return fill * length


def bundle(name):
    return {"v": 1, "suite": "fmv1", "account": name, "x25519": b64(43),
            "mlkem768": b64(1579), "ed25519": b64(43), "signature": b64(86)}


def grant(fill="G"):
    return {"v": 1, "suite": "fmv1", "x25519": b64(43, fill), "mlkem768": b64(1451, fill),
            "wrapped": b64(80, fill), "signature": b64(86, fill)}


KDF = {"algorithm": "argon2id", "version": 19, "memoryKiB": 65536, "passes": 3,
       "parallelism": 1, "salt": b64(22)}


def install_body(name):
    return {"kdf": KDF, "wrappedKeyset": b64(400), "wrappedRecovery": b64(400, "R"),
            "bundle": bundle(name)}


def call(runtime, method, path, actor="alice", body=None):
    return asyncio.run(vault.handle(runtime.use(method, actor, body), path))


def ready(*names):
    runtime = Runtime()
    for name in names:
        assert call(runtime, "POST", "/api/vault/install", name, install_body(name))["status"] == 201
    return runtime


ITEM = "a" * 32


def add_item(runtime, actor="alice", item_id=ITEM):
    response = call(runtime, "POST", "/api/vault/items", actor,
                    {"id": item_id, "body": b64(344, "B"), "wrappedKey": b64(80, "W")})
    assert response["status"] == 201, response
    return response["data"]["item"]


def share_user(runtime, recipient="bob", permission="read", item_id=ITEM):
    target = {"kind": "user", "name": recipient}
    return call(runtime, "POST", f"/api/vault/items/{item_id}/shares", "alice", {
        "target": target, "permission": permission,
        "grants": [{"recipient": recipient, "via": target, "grant": grant()}]})


def test_install_is_per_account_and_shape_checked():
    runtime = Runtime()
    before = call(runtime, "GET", "/api/vault")
    assert before["data"] == {"ok": True, "installed": False, "actor": "alice", "suite": "fmv1"}
    assert call(runtime, "GET", "/api/vault/items")["status"] == 409
    for broken, code in (
        ({"kdf": dict(KDF, memoryKiB=1024)}, "invalid_kdf"),
        ({"kdf": dict(KDF, passes=1)}, "invalid_kdf"),
        ({"kdf": dict(KDF, algorithm="pbkdf2")}, "invalid_kdf"),
        ({"bundle": bundle("bob")}, "invalid_public_keys"),
        ({"bundle": dict(bundle("alice"), mlkem768=b64(1578))}, "invalid_public_keys"),
        ({"bundle": dict(bundle("alice"), x25519="not base64!" + b64(32))}, "invalid_public_keys"),
        ({"wrappedKeyset": ""}, "invalid_keyset"),
        ({"wrappedRecovery": b64(5000)}, "invalid_keyset"),
    ):
        response = call(runtime, "POST", "/api/vault/install", "alice", dict(install_body("alice"), **broken))
        assert (response["status"], response["data"]["error"]) == (400, code), broken
    created = call(runtime, "POST", "/api/vault/install", "alice", install_body("alice"))
    assert created["status"] == 201 and created["data"]["account"]["hasRecovery"] is True
    # The recovery blob is handed out only by its own endpoint, and audited.
    assert "wrappedRecovery" not in created["data"]["account"]
    assert call(runtime, "POST", "/api/vault/install", "alice", install_body("alice"))["status"] == 409
    assert call(runtime, "GET", "/api/vault/recovery")["data"]["wrappedRecovery"] == b64(400, "R")
    assert any(entry[1] == "vault.recovery.read" for entry in runtime.audits)


def test_every_blob_is_sealed_with_the_host_key_and_never_cached():
    runtime = ready("alice")
    add_item(runtime)
    for table in ("vault_accounts", "vault_items"):
        for row in runtime.db.execute("SELECT data FROM " + table).fetchall():
            assert row["data"].startswith("sealed:")
    response = call(runtime, "GET", "/api/vault/items")
    assert response["cache"] == "no-store, max-age=0, must-revalidate"
    assert response["headers"]["x-content-type-options"] == "nosniff"


def test_sessions_origins_and_methods_are_enforced():
    runtime = ready("alice")
    assert call(runtime, "GET", "/api/vault", "stranger")["status"] == 401
    assert call(runtime, "DELETE", "/api/vault/items")["status"] == 405
    assert call(runtime, "GET", "/api/vault/nothing")["status"] == 404
    assert call(runtime, "GET", "/api/vaulted")["status"] == 404
    runtime.origin_ok = False
    assert call(runtime, "POST", "/api/vault/items", "alice", {})["status"] == 403
    assert call(runtime, "GET", "/api/vault")["status"] == 200


def test_items_are_private_to_their_owner():
    runtime = ready("alice", "bob")
    add_item(runtime)
    assert [item["id"] for item in call(runtime, "GET", "/api/vault/items")["data"]["items"]] == [ITEM]
    theirs = call(runtime, "GET", "/api/vault/items", "bob")["data"]
    assert theirs["items"] == [] and theirs["shared"] == []
    # To anyone else the item does not exist: same answer as a random id.
    for method, suffix, body in (
        ("PUT", "", {"revision": 1, "body": b64(344)}), ("DELETE", "", None),
        ("POST", "/shares", {}), ("POST", "/unshare", {}), ("POST", "/grants", {}),
        ("GET", "/recipients", None),
    ):
        response = call(runtime, method, f"/api/vault/items/{ITEM}{suffix}", "bob", body)
        assert (response["status"], response["data"]["error"]) == (404, "item_not_found"), suffix
    assert call(runtime, "POST", "/api/vault/items", "bob",
                {"id": ITEM, "body": b64(344), "wrappedKey": b64(80)})["status"] == 409
    for broken in ({"id": "short"}, {"body": ""}, {"body": b64(40000)}, {"wrappedKey": b64(79)}):
        body = dict({"id": "b" * 32, "body": b64(344), "wrappedKey": b64(80)}, **broken)
        assert call(runtime, "POST", "/api/vault/items", "alice", body)["status"] == 400


def test_updates_need_the_current_revision():
    runtime = ready("alice")
    add_item(runtime)
    stale = call(runtime, "PUT", f"/api/vault/items/{ITEM}", "alice", {"revision": 7, "body": b64(344, "C")})
    assert (stale["status"], stale["data"]["error"]) == (409, "revision_conflict")
    saved = call(runtime, "PUT", f"/api/vault/items/{ITEM}", "alice", {"revision": 1, "body": b64(344, "C")})
    assert saved["data"]["item"]["revision"] == 2 and saved["data"]["item"]["body"] == b64(344, "C")
    # The owner's wrapped key is untouched by a plain save.
    assert saved["data"]["item"]["wrappedKey"] == b64(80, "W")


def test_a_user_share_needs_exactly_one_grant_for_that_user():
    runtime = ready("alice", "bob", "dave")
    add_item(runtime)
    target = {"kind": "user", "name": "bob"}
    path = f"/api/vault/items/{ITEM}/shares"
    for grants in (None, [], [{"recipient": "dave", "via": target, "grant": grant()}],
                   [{"recipient": "bob", "via": target, "grant": dict(grant(), wrapped="x")}],
                   [{"recipient": "bob", "via": {"kind": "user", "name": "dave"}, "grant": grant()}]):
        response = call(runtime, "POST", path, "alice", {"target": target, "permission": "read", "grants": grants})
        assert (response["status"], response["data"]["error"]) == (400, "invalid_grants"), grants
        # A refused share leaves nothing behind.
        assert runtime.db.execute("SELECT COUNT(*) FROM vault_shares").fetchone()[0] == 0
    assert call(runtime, "POST", path, "alice", {"target": {"kind": "user", "name": "alice"},
                                                   "permission": "read", "grants": []})["status"] == 409
    assert call(runtime, "POST", path, "alice", {"target": {"kind": "user", "name": "carol"},
                                                   "permission": "read", "grants": []})["data"]["error"] == "recipient_has_no_vault"
    assert call(runtime, "POST", path, "alice", {"target": target, "permission": "admin", "grants": []})["status"] == 400
    assert share_user(runtime)["status"] == 200
    shared = call(runtime, "GET", "/api/vault/items", "bob")["data"]
    assert [(i["id"], i["owner"], i["permission"], i["via"]) for i in shared["shared"]] == [
        (ITEM, "alice", "read", {"kind": "user", "name": "bob"})]
    assert shared["shared"][0]["grant"] == grant() and shared["bundles"]["alice"] == bundle("alice")
    # The reader never receives the owner's wrapped key.
    assert "wrappedKey" not in shared["shared"][0]
    assert runtime.pings == [{
        "recipient": "bob", "kind": "plugin_activity", "title": "alice shared a vault item with you",
        "body": "Unlock your Password Vault to open it.", "href": "/dashboard/vault",
        "dedupe": f"vault-share:{ITEM}:user:bob:bob"}]


def test_read_and_write_permissions():
    runtime = ready("alice", "bob")
    add_item(runtime)
    share_user(runtime, permission="read")
    denied = call(runtime, "PUT", f"/api/vault/items/{ITEM}", "bob", {"revision": 1, "body": b64(344, "X")})
    assert (denied["status"], denied["data"]["error"]) == (403, "read_only")
    share_user(runtime, permission="write")
    assert len(runtime.pings) == 1          # changing a permission is not a new share
    saved = call(runtime, "PUT", f"/api/vault/items/{ITEM}", "bob", {"revision": 1, "body": b64(344, "X")})
    assert saved["status"] == 200 and saved["data"]["revision"] == 2
    owner_view = call(runtime, "GET", "/api/vault/items")["data"]["items"][0]
    assert owner_view["body"] == b64(344, "X") and owner_view["updatedBy"] == "bob"
    # A writer can change the body, never the key or who holds it.
    rotate = call(runtime, "PUT", f"/api/vault/items/{ITEM}", "bob",
                  {"revision": 2, "body": b64(344), "wrappedKey": b64(80), "grants": []})
    assert (rotate["status"], rotate["data"]["error"]) == (403, "owner_required")
    # Walking away from a share made to you by name.
    left = call(runtime, "DELETE", f"/api/vault/items/{ITEM}", "bob")
    assert left["data"] == {"ok": True, "left": True}
    assert call(runtime, "GET", "/api/vault/items", "bob")["data"]["shared"] == []
    assert call(runtime, "GET", "/api/vault/items")["data"]["items"][0]["shares"] == []


def test_organization_shares_follow_membership():
    runtime = ready("alice", "bob", "dave")
    add_item(runtime)
    target = {"kind": "organization", "name": "acme"}
    path = f"/api/vault/items/{ITEM}/shares"
    outsider = call(runtime, "POST", path, "alice", {
        "target": target, "permission": "read",
        "grants": [{"recipient": "dave", "via": target, "grant": grant()}]})
    assert outsider["data"]["error"] == "invalid_grants"       # dave is not in acme
    assert call(runtime, "POST", path, "alice", {"target": {"kind": "organization", "name": "globex"},
                                                   "permission": "read", "grants": []})["status"] == 404
    members = call(runtime, "GET", "/api/vault/organizations/acme/members")["data"]["members"]
    assert [(m["name"], bool(m["bundle"])) for m in members] == [("bob", True), ("carol", False)]
    assert call(runtime, "GET", "/api/vault/organizations/globex/members")["status"] == 404
    shared = call(runtime, "POST", path, "alice", {
        "target": target, "permission": "read",
        "grants": [{"recipient": "bob", "via": target, "grant": grant()}]})
    assert shared["status"] == 200
    assert [i["id"] for i in call(runtime, "GET", "/api/vault/items", "bob")["data"]["shared"]] == [ITEM]
    # A member cannot walk an organization's share out of everyone's vault.
    assert call(runtime, "DELETE", f"/api/vault/items/{ITEM}", "bob")["status"] == 404

    # carol creates a vault later: she shows up as pending for the owner only.
    call(runtime, "POST", "/api/vault/install", "carol", install_body("carol"))
    pending = call(runtime, "GET", "/api/vault/pending")["data"]["pending"]
    assert [(p["itemId"], p["organization"], [m["name"] for m in p["members"]]) for p in pending] == [
        (ITEM, "acme", ["carol"])]
    assert call(runtime, "GET", "/api/vault/pending", "bob")["data"]["pending"] == []
    stored = call(runtime, "POST", f"/api/vault/items/{ITEM}/grants", "alice", {
        "grants": [{"recipient": "carol", "via": target, "grant": grant("C")}]})
    assert stored["data"] == {"ok": True, "stored": 1}
    assert call(runtime, "GET", "/api/vault/pending")["data"]["pending"] == []

    # Changing the permission retires grants signed at the old one: carol,
    # not re-sealed to this time, turns pending again rather than holding a
    # grant whose signature no longer matches.
    changed = call(runtime, "POST", path, "alice", {
        "target": target, "permission": "write",
        "grants": [{"recipient": "bob", "via": target, "grant": grant("W")}]})
    assert changed["status"] == 200
    assert [m["name"] for p in call(runtime, "GET", "/api/vault/pending")["data"]["pending"]
            for m in p["members"]] == ["carol"]
    assert call(runtime, "GET", "/api/vault/items", "carol")["data"]["shared"] == []
    assert call(runtime, "GET", "/api/vault/items", "bob")["data"]["shared"][0]["permission"] == "write"

    # Leaving the organization ends access without anyone touching the grant.
    runtime.leave("acme", "bob")
    assert call(runtime, "GET", "/api/vault/items", "bob")["data"]["shared"] == []
    assert runtime.db.execute("SELECT COUNT(*) FROM vault_grants WHERE recipient_bi=?", (bi("bob"),)).fetchone()[0] == 1
    runtime.join("acme", "bob")
    assert len(call(runtime, "GET", "/api/vault/items", "bob")["data"]["shared"]) == 1


def test_organization_guests_are_never_recipients_or_readers():
    runtime = ready("alice", "bob", "erin")
    runtime.join("acme", "erin", "guest")
    add_item(runtime)
    target = {"kind": "organization", "name": "acme"}
    path = f"/api/vault/items/{ITEM}/shares"
    shared = call(runtime, "POST", path, "alice", {
        "target": target, "permission": "read",
        "grants": [{"recipient": "bob", "via": target, "grant": grant()}]})
    assert shared["status"] == 200
    members = call(runtime, "GET", "/api/vault/organizations/acme/members")["data"]["members"]
    assert "erin" not in [m["name"] for m in members]
    assert call(runtime, "GET", "/api/vault/organizations/acme/members", "erin")["status"] == 404
    assert "acme" not in [o["name"] for o in call(
        runtime, "GET", "/api/vault/organizations", "erin")["data"]["organizations"]]
    add_item(runtime, "erin", "b" * 32)
    guest_share = call(runtime, "POST", "/api/vault/items/" + "b" * 32 + "/shares", "erin", {
        "target": target, "permission": "read", "grants": []})
    assert guest_share["data"]["error"] == "organization_not_found"
    pending = call(runtime, "GET", "/api/vault/pending")["data"]["pending"]
    assert "erin" not in [m["name"] for p in pending for m in p["members"]]
    sealed = call(runtime, "POST", f"/api/vault/items/{ITEM}/grants", "alice", {
        "grants": [{"recipient": "erin", "via": target, "grant": grant("E")}]})
    assert sealed["data"]["error"] == "invalid_grants"
    runtime.db.execute(
        "INSERT INTO vault_grants(item_id,recipient_bi,recipient_name,via_kind,via_bi,"
        "created_at,data) SELECT item_id,?,?,via_kind,via_bi,created_at,data "
        "FROM vault_grants WHERE recipient_bi=?", (bi("erin"), "erin", bi("bob")))
    assert call(runtime, "GET", "/api/vault/items", "erin")["data"]["shared"] == []


def test_unshare_removes_grants_and_rotation_replaces_every_grant():
    runtime = ready("alice", "bob", "carol")
    add_item(runtime)
    org = {"kind": "organization", "name": "acme"}
    share_user(runtime)
    call(runtime, "POST", f"/api/vault/items/{ITEM}/shares", "alice", {
        "target": org, "permission": "read",
        "grants": [{"recipient": "carol", "via": org, "grant": grant()}]})
    gone = call(runtime, "POST", f"/api/vault/items/{ITEM}/unshare", "alice", {"kind": "user", "name": "bob"})
    assert gone["data"]["rotate"] is True
    assert call(runtime, "POST", f"/api/vault/items/{ITEM}/unshare", "alice",
                {"kind": "user", "name": "bob"})["status"] == 404
    recipients = call(runtime, "GET", f"/api/vault/items/{ITEM}/recipients")["data"]["recipients"]
    assert [(g["kind"], g["name"], [m["name"] for m in g["members"]]) for g in recipients] == [
        ("organization", "acme", ["bob", "carol"])]
    # A rotation may not smuggle bob's removed user share back in.
    smuggle = call(runtime, "PUT", f"/api/vault/items/{ITEM}", "alice", {
        "revision": 1, "body": b64(344, "N"), "wrappedKey": b64(80, "N"),
        "grants": [{"recipient": "bob", "via": {"kind": "user", "name": "bob"}, "grant": grant("N")}]})
    assert smuggle["data"]["error"] == "invalid_grants"
    rotated = call(runtime, "PUT", f"/api/vault/items/{ITEM}", "alice", {
        "revision": 1, "body": b64(344, "N"), "wrappedKey": b64(80, "N"),
        "grants": [{"recipient": "carol", "via": org, "grant": grant("N")}]})
    assert rotated["status"] == 200 and rotated["data"]["item"]["wrappedKey"] == b64(80, "N")
    rows = runtime.db.execute("SELECT recipient_name,data FROM vault_grants").fetchall()
    assert [(row["recipient_name"], json.loads(row["data"].removeprefix("sealed:"))["grant"]["wrapped"])
            for row in rows] == [("carol", b64(80, "N"))]


def test_rewrap_keeps_the_public_keys_and_checks_the_revision():
    runtime = ready("alice")
    stale = call(runtime, "PUT", "/api/vault/keys", "alice", {"revision": 9, "kdf": KDF, "wrappedKeyset": b64(400, "Z")})
    assert stale["status"] == 409
    weak = call(runtime, "PUT", "/api/vault/keys", "alice",
                {"revision": 1, "kdf": dict(KDF, memoryKiB=8), "wrappedKeyset": b64(400, "Z")})
    assert weak["data"]["error"] == "invalid_kdf"
    changed = call(runtime, "PUT", "/api/vault/keys", "alice", {"revision": 1, "kdf": KDF, "wrappedKeyset": b64(400, "Z")})
    account = changed["data"]["account"]
    assert account["wrappedKeyset"] == b64(400, "Z") and account["revision"] == 2
    assert account["bundle"] == bundle("alice")
    assert call(runtime, "GET", "/api/vault/recovery")["data"]["wrappedRecovery"] == b64(400, "R")
    renewed = call(runtime, "PUT", "/api/vault/keys", "alice", {"revision": 2, "wrappedRecovery": b64(400, "Q")})
    assert renewed["status"] == 200
    assert call(runtime, "GET", "/api/vault/recovery")["data"]["wrappedRecovery"] == b64(400, "Q")
    # A bundle sent along is ignored: public keys change only with a new vault.
    call(runtime, "PUT", "/api/vault/keys", "alice",
         {"revision": 3, "wrappedRecovery": b64(400, "S"), "bundle": bundle("mallory")})
    assert call(runtime, "GET", "/api/vault")["data"]["account"]["bundle"] == bundle("alice")


def test_uninstall_and_account_deletion_leave_nothing():
    runtime = ready("alice", "bob")
    add_item(runtime)
    add_item(runtime, "bob", "b" * 32)
    share_user(runtime)
    call(runtime, "POST", f"/api/vault/items/{'b' * 32}/shares", "bob", {
        "target": {"kind": "user", "name": "alice"}, "permission": "read",
        "grants": [{"recipient": "alice", "via": {"kind": "user", "name": "alice"}, "grant": grant()}]})
    removed = call(runtime, "POST", "/api/vault/uninstall", "alice", {})
    export = removed["data"]["export"]
    assert export["format"] == "forkmesh-vault-export-v1" and [i["id"] for i in export["items"]] == [ITEM]
    assert export["wrappedRecovery"] == b64(400, "R")
    for table in ("vault_accounts", "vault_items", "vault_shares", "vault_grants"):
        rows = [dict(row) for row in runtime.db.execute("SELECT * FROM " + table).fetchall()]
        assert "bi:alice" not in json.dumps(rows), table
    assert call(runtime, "GET", "/api/vault")["data"]["installed"] is False
    asyncio.run(vault.purge_account_data(runtime, bi("bob"), "bob"))
    for table in ("vault_accounts", "vault_items", "vault_shares", "vault_grants"):
        assert runtime.db.execute("SELECT COUNT(*) FROM " + table).fetchone()[0] == 0


def test_schema_reverses_cleanly():
    runtime = ready("alice")
    for statement in vault.DATA_SCHEMA_DOWN:
        runtime.db.execute(statement)
    left = [row[0] for row in runtime.db.execute(
        "SELECT name FROM sqlite_master WHERE name LIKE 'vault_%'").fetchall()]
    assert left == []
