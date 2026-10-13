"""Password Vault: an end-to-end encrypted store the server cannot read.

Every account installs the vault for itself. The browser (or the ForkMesh
browser extension) does all of the cryptography; this module only files
opaque blobs and decides who may fetch which one:

* the account's keyset, wrapped by a key stretched from a master password the
  server never sees (Argon2id), and a second copy wrapped by a recovery code;
* the account's public keys (X25519, ML-KEM-768, Ed25519), self-signed;
* items, each encrypted under its own random key, that key wrapped for the
  owner;
* shares: an item key sealed to one recipient's public keys with a hybrid
  X25519 + ML-KEM-768 exchange and signed by the sender. A share names either
  one ForkMesh user or an organization; an organization share is a set of
  per-member grants, and a grant made through an organization is served only
  while its reader is still a member.

Nothing here can decrypt an item, a title, a URL, or a username: there is no
plaintext column for them. What the server does know is who owns an item, who
it is shared with, and when it changed. Every blob is additionally sealed with
the host's row key, so a database copy alone does not even yield the wrapped
keyset to guess a master password against.

The formats are those of content/crypto/vault-crypto.js (suite "fmv1"); this
module checks their shape and size, never their meaning.
"""

import json
import re


PREFIX = "/api/vault"
PLUGIN_ID = "vault"
REQUEST_PROOF = "forkmesh-vault-request-v1"
SUITE = "fmv1"
MAX_BODY_BYTES = 768 * 1024
MAX_ITEMS = 2000
MAX_ITEM_BLOB = 32 * 1024
MAX_PRIVATE_BLOB = 64 * 1024
MAX_SHARES_PER_ITEM = 50
MAX_GRANTS_PER_REQUEST = 300
MAX_ORG_MEMBERS = 300
PERMISSIONS = ("read", "write")
KINDS = ("user", "organization")
ID_RE = re.compile(r"^[0-9a-f]{32}$")
NAME_RE = re.compile(r"^[a-z0-9](?:[a-z0-9-]{0,62})$")
ORG_RE = re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")
B64_RE = re.compile(r"^[A-Za-z0-9_-]+$")
# base64url lengths of the fixed-size fields: 32-byte keys, a 64-byte
# signature, an ML-KEM-768 public key (1184) and ciphertext (1088), and a
# wrapped 32-byte item key (12 iv + 32 + 16 tag).
LEN_KEY32 = 43
LEN_SIGNATURE = 86
LEN_MLKEM_PUBLIC = 1579
LEN_MLKEM_CIPHERTEXT = 1451
LEN_WRAPPED_KEY = 80
# The same floor the client enforces: stored parameters below it would only
# ever be refused at unlock, so refuse them at the door.
KDF_MIN_MEMORY_KIB = 19456
KDF_MIN_PASSES = 2

DATA_SCHEMA_UP = [
    """CREATE TABLE IF NOT EXISTS vault_accounts (
        account_bi TEXT PRIMARY KEY,
        account_name TEXT NOT NULL,
        revision INTEGER NOT NULL DEFAULT 1 CHECK(revision >= 1),
        created_at INTEGER NOT NULL CHECK(created_at >= 0),
        updated_at INTEGER NOT NULL CHECK(updated_at >= 0),
        data TEXT NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_vault_accounts_name ON vault_accounts(account_name)",
    """CREATE TABLE IF NOT EXISTS vault_items (
        item_id TEXT PRIMARY KEY,
        owner_bi TEXT NOT NULL,
        owner_name TEXT NOT NULL,
        revision INTEGER NOT NULL DEFAULT 1 CHECK(revision >= 1),
        created_at INTEGER NOT NULL CHECK(created_at >= 0),
        updated_at INTEGER NOT NULL CHECK(updated_at >= 0),
        updated_by TEXT NOT NULL,
        data TEXT NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_vault_items_owner ON vault_items(owner_bi,updated_at)",
    """CREATE TABLE IF NOT EXISTS vault_shares (
        item_id TEXT NOT NULL,
        target_kind TEXT NOT NULL CHECK(target_kind IN ('user','organization')),
        target_bi TEXT NOT NULL,
        target_name TEXT NOT NULL,
        permission TEXT NOT NULL CHECK(permission IN ('read','write')),
        created_at INTEGER NOT NULL CHECK(created_at >= 0),
        PRIMARY KEY(item_id,target_kind,target_bi))""",
    "CREATE INDEX IF NOT EXISTS idx_vault_shares_target ON vault_shares(target_kind,target_bi)",
    """CREATE TABLE IF NOT EXISTS vault_grants (
        item_id TEXT NOT NULL,
        recipient_bi TEXT NOT NULL,
        recipient_name TEXT NOT NULL,
        via_kind TEXT NOT NULL CHECK(via_kind IN ('user','organization')),
        via_bi TEXT NOT NULL,
        created_at INTEGER NOT NULL CHECK(created_at >= 0),
        data TEXT NOT NULL,
        PRIMARY KEY(item_id,recipient_bi,via_kind,via_bi))""",
    "CREATE INDEX IF NOT EXISTS idx_vault_grants_recipient ON vault_grants(recipient_bi)",
]
DATA_SCHEMA_DOWN = [
    "DROP TABLE IF EXISTS vault_grants",
    "DROP TABLE IF EXISTS vault_shares",
    "DROP TABLE IF EXISTS vault_items",
    "DROP TABLE IF EXISTS vault_accounts",
]

_SCHEMA_READY = False


def _response(runtime, payload, status=200, allow=""):
    headers = {"x-content-type-options": "nosniff"}
    if allow:
        headers["allow"] = allow
    return runtime.response(
        payload,
        status=status,
        cache_control="no-store, max-age=0, must-revalidate",
        extra_headers=headers,
    )


def _error(runtime, code, status=400):
    return _response(runtime, {"error": code}, status)


def _clean(value, limit):
    return " ".join(str(value or "").replace("\x00", "").split())[:limit]


def _route(path):
    clean = str(path or "").rstrip("/")
    if clean == PREFIX:
        return ()
    if not clean.startswith(PREFIX + "/"):
        return None
    parts = tuple(part for part in clean[len(PREFIX) + 1:].split("/") if part)
    if any(len(part) > 64 for part in parts):
        return None
    return parts


async def _actor(runtime, data=None):
    account_bi, record = await runtime.session(data or {})
    return str(account_bi or ""), _clean((record or {}).get("name"), 64).lower()


async def _ensure_tables(runtime):
    global _SCHEMA_READY
    if _SCHEMA_READY:
        return
    for statement in DATA_SCHEMA_UP:
        await runtime.d1_run(statement)
    _SCHEMA_READY = True


# --- shape checks --------------------------------------------------------------
# The server cannot tell a good ciphertext from noise, so it holds every field
# to its exact alphabet and size instead. A client bug or a hostile client can
# then store garbage only in its own rows, never oversize or malformed ones.


def _b64(value, low, high=None):
    text = value if isinstance(value, str) else ""
    top = low if high is None else high
    return text if low <= len(text) <= top and B64_RE.fullmatch(text) else ""


def _integer(value, low, high):
    if isinstance(value, bool) or not isinstance(value, int):
        return None
    return value if low <= value <= high else None


def _kdf(value):
    raw = value if isinstance(value, dict) else {}
    memory = _integer(raw.get("memoryKiB"), KDF_MIN_MEMORY_KIB, 1048576)
    passes = _integer(raw.get("passes"), KDF_MIN_PASSES, 16)
    lanes = _integer(raw.get("parallelism"), 1, 16)
    salt = _b64(raw.get("salt"), 22, 86)
    if (raw.get("algorithm") != "argon2id" or raw.get("version") != 19
            or memory is None or passes is None or lanes is None or not salt):
        return None
    return {"algorithm": "argon2id", "version": 19, "memoryKiB": memory,
            "passes": passes, "parallelism": lanes, "salt": salt}


def _bundle(value, account):
    raw = value if isinstance(value, dict) else {}
    fields = {
        "x25519": _b64(raw.get("x25519"), LEN_KEY32),
        "mlkem768": _b64(raw.get("mlkem768"), LEN_MLKEM_PUBLIC),
        "ed25519": _b64(raw.get("ed25519"), LEN_KEY32),
        "signature": _b64(raw.get("signature"), LEN_SIGNATURE),
    }
    # The name inside the bundle is part of what its signature covers; one
    # that names somebody else could never verify, so it is refused unread.
    if (raw.get("v") != 1 or raw.get("suite") != SUITE
            or raw.get("account") != account or not all(fields.values())):
        return None
    return dict(fields, v=1, suite=SUITE, account=account)


def _grant(value):
    raw = value if isinstance(value, dict) else {}
    fields = {
        "x25519": _b64(raw.get("x25519"), LEN_KEY32),
        "mlkem768": _b64(raw.get("mlkem768"), LEN_MLKEM_CIPHERTEXT),
        "wrapped": _b64(raw.get("wrapped"), LEN_WRAPPED_KEY),
        "signature": _b64(raw.get("signature"), LEN_SIGNATURE),
    }
    if raw.get("v") != 1 or raw.get("suite") != SUITE or not all(fields.values()):
        return None
    return dict(fields, v=1, suite=SUITE)


def _keyset_blob(value):
    return _b64(value, 200, 2048)


def _item_blobs(data):
    body = _b64(data.get("body"), 40, MAX_ITEM_BLOB)
    wrapped = _b64(data.get("wrappedKey"), LEN_WRAPPED_KEY)
    return body, wrapped


def _target(data):
    raw = data.get("target") if isinstance(data.get("target"), dict) else data
    kind = str(raw.get("kind") or "")
    name = str(raw.get("name") or "").strip().lower()
    if kind == "user" and NAME_RE.fullmatch(name):
        return kind, name
    if kind == "organization" and ORG_RE.fullmatch(name):
        return kind, name
    return "", ""


# --- accounts ------------------------------------------------------------------


async def _account_row(runtime, account_bi):
    return await runtime.d1_first(
        "SELECT account_bi,account_name,revision,created_at,updated_at,data "
        "FROM vault_accounts WHERE account_bi=?", account_bi)


async def _account_data(runtime, row):
    data = await runtime.open((row or {}).get("data"))
    return data if isinstance(data, dict) else {}


async def _project_account(runtime, row):
    data = await _account_data(runtime, row)
    return {
        "kdf": data.get("kdf"),
        "wrappedKeyset": str(data.get("wrappedKeyset") or ""),
        "hasRecovery": bool(data.get("wrappedRecovery")),
        "bundle": data.get("bundle"),
        "trust": str(data.get("trust") or ""),
        "revision": int(row.get("revision") or 1),
        "createdAt": int(row.get("created_at") or 0),
        "updatedAt": int(row.get("updated_at") or 0),
    }


async def _bundle_of(runtime, account_bi):
    row = await _account_row(runtime, account_bi)
    if not row:
        return None
    bundle = (await _account_data(runtime, row)).get("bundle")
    return bundle if isinstance(bundle, dict) else None


async def _install(runtime, account_bi, actor, data):
    if await _account_row(runtime, account_bi):
        return _error(runtime, "vault_exists", 409)
    kdf = _kdf(data.get("kdf"))
    bundle = _bundle(data.get("bundle"), actor)
    wrapped = _keyset_blob(data.get("wrappedKeyset"))
    recovery = _keyset_blob(data.get("wrappedRecovery"))
    if kdf is None:
        return _error(runtime, "invalid_kdf")
    if bundle is None:
        return _error(runtime, "invalid_public_keys")
    if not wrapped or not recovery:
        return _error(runtime, "invalid_keyset")
    now = runtime.now()
    await runtime.d1_run(
        "INSERT INTO vault_accounts(account_bi,account_name,revision,created_at,"
        "updated_at,data) VALUES(?,?,1,?,?,?)",
        account_bi, actor, now, now, await runtime.seal({
            "kdf": kdf, "wrappedKeyset": wrapped, "wrappedRecovery": recovery,
            "bundle": bundle, "trust": ""}))
    await runtime.audit(actor, "plugin.install", "account_plugin", actor + ":" + PLUGIN_ID)
    row = await _account_row(runtime, account_bi)
    return _response(runtime, {
        "ok": True, "installed": True, "plugin": PLUGIN_ID,
        "account": await _project_account(runtime, row)}, 201)


async def _rewrap(runtime, row, actor, data):
    """A master password or recovery code change: new wrappings of the same
    keyset. The public keys cannot change here, so existing shares stay valid."""
    current = await _account_data(runtime, row)
    if _integer(data.get("revision"), 1, 2 ** 53) != int(row.get("revision") or 1):
        return _error(runtime, "revision_conflict", 409)
    updated = dict(current)
    if "wrappedKeyset" in data or "kdf" in data:
        kdf = _kdf(data.get("kdf"))
        wrapped = _keyset_blob(data.get("wrappedKeyset"))
        if kdf is None:
            return _error(runtime, "invalid_kdf")
        if not wrapped:
            return _error(runtime, "invalid_keyset")
        updated["kdf"], updated["wrappedKeyset"] = kdf, wrapped
    if "wrappedRecovery" in data:
        recovery = _keyset_blob(data.get("wrappedRecovery"))
        if not recovery:
            return _error(runtime, "invalid_keyset")
        updated["wrappedRecovery"] = recovery
    if updated == current:
        return _error(runtime, "nothing_to_change")
    await runtime.d1_run(
        "UPDATE vault_accounts SET revision=revision+1,updated_at=?,data=? "
        "WHERE account_bi=? AND revision=?",
        runtime.now(), await runtime.seal(updated), row["account_bi"],
        int(row.get("revision") or 1))
    await runtime.audit(actor, "vault.rewrap", "vault_account", actor)
    fresh = await _account_row(runtime, row["account_bi"])
    return _response(runtime, {"ok": True, "account": await _project_account(runtime, fresh)})


async def _save_trust(runtime, row, data):
    trust = _b64(data.get("trust"), 40, MAX_PRIVATE_BLOB)
    if not trust:
        return _error(runtime, "invalid_trust")
    current = await _account_data(runtime, row)
    current["trust"] = trust
    await runtime.d1_run(
        "UPDATE vault_accounts SET updated_at=?,data=? WHERE account_bi=?",
        runtime.now(), await runtime.seal(current), row["account_bi"])
    return _response(runtime, {"ok": True})


# --- organizations ---------------------------------------------------------------


async def _organizations(runtime, account_bi):
    rows = await runtime.d1_all(
        "SELECT o.name,m.role FROM org_members m JOIN orgs o ON o.org_bi=m.org_bi "
        "WHERE m.member_bi=? ORDER BY o.name COLLATE NOCASE LIMIT 200", account_bi)
    return [{"name": str(row.get("name") or ""), "role": str(row.get("role") or "member")}
            for row in rows or []]


async def _org_members(runtime, org_bi):
    """Every member of an organization with the bundle of those who have a
    vault. A member without one simply cannot be sealed to yet."""
    rows = await runtime.d1_all(
        "SELECT m.member_bi,m.name,a.data FROM org_members m "
        "LEFT JOIN vault_accounts a ON a.account_bi=m.member_bi "
        "WHERE m.org_bi=? ORDER BY m.name COLLATE NOCASE LIMIT ?",
        org_bi, MAX_ORG_MEMBERS + 1)
    members = []
    for row in rows or []:
        bundle = None
        if row.get("data"):
            bundle = (await _account_data(runtime, row)).get("bundle")
        members.append({
            "bi": str(row.get("member_bi") or ""),
            "name": str(row.get("name") or "").lower(),
            "bundle": bundle if isinstance(bundle, dict) else None,
        })
    return members


def _public_member(member):
    return {"name": member["name"], "bundle": member["bundle"]}


# --- items -------------------------------------------------------------------------


async def _item_row(runtime, item_id):
    return await runtime.d1_first(
        "SELECT item_id,owner_bi,owner_name,revision,created_at,updated_at,"
        "updated_by,data FROM vault_items WHERE item_id=?", item_id)


async def _shares(runtime, item_id):
    rows = await runtime.d1_all(
        "SELECT target_kind,target_bi,target_name,permission,created_at "
        "FROM vault_shares WHERE item_id=? ORDER BY target_kind,target_name", item_id)
    return [dict(row) for row in rows or []]


async def _readable_grant(runtime, item_id, account_bi):
    """The best live grant ``account_bi`` holds on an item, or None.

    A grant lives only as long as the share it was made under, and a grant
    made through an organization only while its reader is still a member.
    Write beats read when a person is reached twice.
    """
    rows = await runtime.d1_all(
        "SELECT g.via_kind,g.via_bi,g.data,s.permission,s.target_name "
        "FROM vault_grants g JOIN vault_shares s ON s.item_id=g.item_id "
        "AND s.target_kind=g.via_kind AND s.target_bi=g.via_bi "
        "WHERE g.item_id=? AND g.recipient_bi=? AND (g.via_kind='user' OR EXISTS("
        "SELECT 1 FROM org_members m WHERE m.org_bi=g.via_bi AND m.member_bi=g.recipient_bi))",
        item_id, account_bi)
    best = None
    for row in rows or []:
        if best is None or (row.get("permission") == "write"
                            and best.get("permission") != "write"):
            best = row
    return best


async def _project_owned(runtime, row):
    data = await runtime.open(row.get("data")) or {}
    shares = []
    for share in await _shares(runtime, row["item_id"]):
        shares.append({
            "kind": share["target_kind"], "name": share["target_name"],
            "permission": share["permission"], "createdAt": int(share["created_at"] or 0),
        })
    return {
        "id": row["item_id"], "owner": row["owner_name"], "owned": True,
        "permission": "owner", "body": str(data.get("body") or ""),
        "wrappedKey": str(data.get("wrappedKey") or ""),
        "revision": int(row.get("revision") or 1),
        "createdAt": int(row.get("created_at") or 0),
        "updatedAt": int(row.get("updated_at") or 0),
        "updatedBy": str(row.get("updated_by") or ""),
        "shares": shares,
    }


async def _list_items(runtime, account_bi):
    owned_rows = await runtime.d1_all(
        "SELECT item_id,owner_bi,owner_name,revision,created_at,updated_at,"
        "updated_by,data FROM vault_items WHERE owner_bi=? "
        "ORDER BY updated_at DESC LIMIT ?", account_bi, MAX_ITEMS)
    owned = [await _project_owned(runtime, row) for row in owned_rows or []]
    rows = await runtime.d1_all(
        "SELECT DISTINCT g.item_id FROM vault_grants g WHERE g.recipient_bi=? LIMIT ?",
        account_bi, MAX_ITEMS)
    shared, bundles = [], {}
    for entry in rows or []:
        item_id = str(entry.get("item_id") or "")
        grant = await _readable_grant(runtime, item_id, account_bi)
        row = await _item_row(runtime, item_id) if grant else None
        if not row or row["owner_bi"] == account_bi:
            continue
        data = await runtime.open(row.get("data")) or {}
        sealed = await runtime.open(grant.get("data")) or {}
        owner = str(row["owner_name"])
        if owner not in bundles:
            bundles[owner] = await _bundle_of(runtime, row["owner_bi"])
        shared.append({
            "id": item_id, "owner": owner, "owned": False,
            "permission": str(grant.get("permission") or "read"),
            "via": {"kind": str(grant.get("via_kind")), "name": str(grant.get("target_name"))},
            "body": str(data.get("body") or ""), "grant": sealed.get("grant"),
            "revision": int(row.get("revision") or 1),
            "createdAt": int(row.get("created_at") or 0),
            "updatedAt": int(row.get("updated_at") or 0),
            "updatedBy": str(row.get("updated_by") or ""),
        })
    return owned, shared, {name: bundle for name, bundle in bundles.items() if bundle}


async def _create_item(runtime, account_bi, actor, data):
    item_id = str(data.get("id") or "")
    body, wrapped = _item_blobs(data)
    if not ID_RE.fullmatch(item_id) or not body or not wrapped:
        return _error(runtime, "invalid_item")
    count = await runtime.d1_first(
        "SELECT COUNT(*) AS items FROM vault_items WHERE owner_bi=?", account_bi)
    if int((count or {}).get("items") or 0) >= MAX_ITEMS:
        return _error(runtime, "vault_full", 409)
    if await _item_row(runtime, item_id):
        return _error(runtime, "item_exists", 409)
    now = runtime.now()
    await runtime.d1_run(
        "INSERT INTO vault_items(item_id,owner_bi,owner_name,revision,created_at,"
        "updated_at,updated_by,data) VALUES(?,?,?,1,?,?,?,?)",
        item_id, account_bi, actor, now, now, actor,
        await runtime.seal({"body": body, "wrappedKey": wrapped}))
    row = await _item_row(runtime, item_id)
    return _response(runtime, {"ok": True, "item": await _project_owned(runtime, row)}, 201)


async def _grant_rows(runtime, item_id, owner_bi, grants):
    """Validate a list of {recipient, via:{kind,name}, grant} against the
    item's shares; returns (rows, error). A grant is accepted only for a
    recipient its share actually covers, so a client cannot widen access by
    filing a grant the share list does not show."""
    if not isinstance(grants, list) or len(grants) > MAX_GRANTS_PER_REQUEST:
        return None, "invalid_grants"
    shares = {(s["target_kind"], s["target_name"]): s for s in await _shares(runtime, item_id)}
    rows, seen = [], set()
    for entry in grants:
        raw = entry if isinstance(entry, dict) else {}
        kind, name = _target(raw.get("via") if isinstance(raw.get("via"), dict) else {})
        recipient = str(raw.get("recipient") or "").strip().lower()
        grant = _grant(raw.get("grant"))
        share = shares.get((kind, name))
        if not share or grant is None or not NAME_RE.fullmatch(recipient):
            return None, "invalid_grants"
        recipient_bi, recipient_name = await runtime.account(recipient)
        if not recipient_bi or recipient_bi == owner_bi:
            return None, "invalid_grants"
        if kind == "user":
            covered = recipient_bi == share["target_bi"]
        else:
            covered = bool(await runtime.d1_first(
                "SELECT 1 AS one FROM org_members WHERE org_bi=? AND member_bi=?",
                share["target_bi"], recipient_bi))
        key = (recipient_bi, kind, share["target_bi"])
        if not covered or key in seen:
            return None, "invalid_grants"
        if not await _account_row(runtime, recipient_bi):
            return None, "recipient_has_no_vault"
        seen.add(key)
        rows.append((recipient_bi, recipient_name, kind, share["target_bi"], grant))
    return rows, ""


async def _store_grants(runtime, item_id, rows):
    now = runtime.now()
    for recipient_bi, recipient_name, kind, via_bi, grant in rows:
        await runtime.d1_run(
            "INSERT INTO vault_grants(item_id,recipient_bi,recipient_name,via_kind,"
            "via_bi,created_at,data) VALUES(?,?,?,?,?,?,?) "
            "ON CONFLICT(item_id,recipient_bi,via_kind,via_bi) DO UPDATE SET "
            "recipient_name=excluded.recipient_name,created_at=excluded.created_at,"
            "data=excluded.data",
            item_id, recipient_bi, recipient_name, kind, via_bi, now,
            await runtime.seal({"grant": grant}))


async def _update_item(runtime, row, account_bi, actor, data):
    owner = row["owner_bi"] == account_bi
    if not owner:
        grant = await _readable_grant(runtime, row["item_id"], account_bi)
        if not grant:
            return _error(runtime, "item_not_found", 404)
        if grant.get("permission") != "write":
            return _error(runtime, "read_only", 403)
    if _integer(data.get("revision"), 1, 2 ** 53) != int(row.get("revision") or 1):
        return _error(runtime, "revision_conflict", 409)
    current = await runtime.open(row.get("data")) or {}
    body = _b64(data.get("body"), 40, MAX_ITEM_BLOB)
    if not body:
        return _error(runtime, "invalid_item")
    updated = {"body": body, "wrappedKey": str(current.get("wrappedKey") or "")}
    grant_rows = None
    if "wrappedKey" in data or "grants" in data:
        # A new item key: only the owner can rotate, and a rotation replaces
        # every grant at once, because every old grant wraps the old key.
        if not owner:
            return _error(runtime, "owner_required", 403)
        wrapped = _b64(data.get("wrappedKey"), LEN_WRAPPED_KEY)
        if not wrapped:
            return _error(runtime, "invalid_item")
        grant_rows, error = await _grant_rows(
            runtime, row["item_id"], account_bi, data.get("grants"))
        if error:
            return _error(runtime, error)
        updated["wrappedKey"] = wrapped
    result = await runtime.d1_run(
        "UPDATE vault_items SET revision=revision+1,updated_at=?,updated_by=?,data=? "
        "WHERE item_id=? AND revision=?",
        runtime.now(), actor, await runtime.seal(updated), row["item_id"],
        int(row.get("revision") or 1))
    if getattr(result, "rowcount", 1) == 0:
        return _error(runtime, "revision_conflict", 409)
    if grant_rows is not None:
        await runtime.d1_run("DELETE FROM vault_grants WHERE item_id=?", row["item_id"])
        await _store_grants(runtime, row["item_id"], grant_rows)
    fresh = await _item_row(runtime, row["item_id"])
    if owner:
        return _response(runtime, {"ok": True, "item": await _project_owned(runtime, fresh)})
    return _response(runtime, {"ok": True, "revision": int(fresh.get("revision") or 1),
                               "updatedAt": int(fresh.get("updated_at") or 0)})


async def _delete_item_rows(runtime, item_id):
    await runtime.d1_run("DELETE FROM vault_grants WHERE item_id=?", item_id)
    await runtime.d1_run("DELETE FROM vault_shares WHERE item_id=?", item_id)
    await runtime.d1_run("DELETE FROM vault_items WHERE item_id=?", item_id)


async def _delete_item(runtime, row, account_bi, actor):
    if row["owner_bi"] == account_bi:
        await _delete_item_rows(runtime, row["item_id"])
        return _response(runtime, {"ok": True, "deleted": True})
    # A reader can only walk away from a share made to them by name.
    share = await runtime.d1_first(
        "SELECT 1 AS one FROM vault_shares WHERE item_id=? AND target_kind='user' "
        "AND target_bi=?", row["item_id"], account_bi)
    if not share:
        return _error(runtime, "item_not_found", 404)
    await runtime.d1_run(
        "DELETE FROM vault_grants WHERE item_id=? AND via_kind='user' AND via_bi=?",
        row["item_id"], account_bi)
    await runtime.d1_run(
        "DELETE FROM vault_shares WHERE item_id=? AND target_kind='user' AND target_bi=?",
        row["item_id"], account_bi)
    await runtime.audit(actor, "vault.share.leave", "vault_item", row["item_id"])
    return _response(runtime, {"ok": True, "left": True})


# --- sharing -------------------------------------------------------------------------


async def _resolve_target(runtime, account_bi, kind, name):
    """(target_bi, canonical name, error). The sharer must belong to an
    organization to share with it; any active user can be shared with."""
    if kind == "user":
        target_bi, target_name = await runtime.account(name)
        if not target_bi:
            return "", "", "recipient_not_found"
        if target_bi == account_bi:
            return "", "", "cannot_share_with_yourself"
        if not await _account_row(runtime, target_bi):
            return "", "", "recipient_has_no_vault"
        return target_bi, target_name, ""
    org_bi, role = await runtime.organization(name, account_bi)
    if not org_bi or not role:
        return "", "", "organization_not_found"
    return org_bi, name, ""


async def _share(runtime, row, account_bi, actor, data):
    kind, name = _target(data)
    permission = str(data.get("permission") or "read")
    if not kind or permission not in PERMISSIONS:
        return _error(runtime, "invalid_share")
    target_bi, target_name, error = await _resolve_target(runtime, account_bi, kind, name)
    if error:
        return _error(runtime, error, 404 if error.endswith("not_found") else 409)
    existing = await _shares(runtime, row["item_id"])
    previous = next((s for s in existing
                     if s["target_kind"] == kind and s["target_bi"] == target_bi), None)
    known = previous is not None
    if not known and len(existing) >= MAX_SHARES_PER_ITEM:
        return _error(runtime, "too_many_shares", 409)
    # The share row comes first so its grants have something to belong to;
    # if the grants turn out malformed the row is taken back.
    await runtime.d1_run(
        "INSERT INTO vault_shares(item_id,target_kind,target_bi,target_name,permission,"
        "created_at) VALUES(?,?,?,?,?,?) ON CONFLICT(item_id,target_kind,target_bi) "
        "DO UPDATE SET permission=excluded.permission,target_name=excluded.target_name",
        row["item_id"], kind, target_bi, target_name, permission, runtime.now())
    rows, error = await _grant_rows(runtime, row["item_id"], account_bi, data.get("grants"))
    if not error and any(r[2] != kind or r[3] != target_bi for r in rows):
        error = "invalid_grants"
    if not error and kind == "user" and len(rows) != 1:
        error = "invalid_grants"
    if error:
        if not known:
            await runtime.d1_run(
                "DELETE FROM vault_shares WHERE item_id=? AND target_kind=? AND target_bi=?",
                row["item_id"], kind, target_bi)
        return _error(runtime, error)
    if known and previous["permission"] != permission:
        # A grant's signature covers its permission, so every grant made at
        # the old one is dead weight now: a member the client did not re-seal
        # to this time is picked up again as pending instead of failing to
        # verify a stale grant.
        await runtime.d1_run(
            "DELETE FROM vault_grants WHERE item_id=? AND via_kind=? AND via_bi=?",
            row["item_id"], kind, target_bi)
    await _store_grants(runtime, row["item_id"], rows)
    await runtime.audit(actor, "vault.share", "vault_item",
                        row["item_id"] + ":" + kind + ":" + target_name)
    if not known:
        # The ping says that something was shared, never what: the server
        # does not know the item's title either.
        recipients = [target_name] if kind == "user" else [r[1] for r in rows]
        for recipient in recipients[:MAX_ORG_MEMBERS]:
            await runtime.notify(
                recipient, "plugin_activity",
                actor + " shared a vault item with " + (
                    "you" if kind == "user" else target_name),
                body="Unlock your Password Vault to open it.",
                href="/dashboard/vault",
                dedupe="vault-share:" + row["item_id"] + ":" + kind + ":" + target_name
                + ":" + recipient)
    fresh = await _item_row(runtime, row["item_id"])
    return _response(runtime, {"ok": True, "item": await _project_owned(runtime, fresh)})


async def _unshare(runtime, row, account_bi, actor, data):
    kind, name = _target(data)
    share = next((s for s in await _shares(runtime, row["item_id"])
                  if s["target_kind"] == kind and s["target_name"] == name), None)
    if not share:
        return _error(runtime, "share_not_found", 404)
    await runtime.d1_run(
        "DELETE FROM vault_grants WHERE item_id=? AND via_kind=? AND via_bi=?",
        row["item_id"], kind, share["target_bi"])
    await runtime.d1_run(
        "DELETE FROM vault_shares WHERE item_id=? AND target_kind=? AND target_bi=?",
        row["item_id"], kind, share["target_bi"])
    await runtime.audit(actor, "vault.unshare", "vault_item",
                        row["item_id"] + ":" + kind + ":" + name)
    fresh = await _item_row(runtime, row["item_id"])
    # The client follows this with a key rotation: whoever lost access may
    # still hold the old item key, so the item is re-encrypted under a new one.
    return _response(runtime, {"ok": True, "rotate": True,
                               "item": await _project_owned(runtime, fresh)})


async def _add_grants(runtime, row, account_bi, data):
    rows, error = await _grant_rows(runtime, row["item_id"], account_bi, data.get("grants"))
    if error:
        return _error(runtime, error)
    await _store_grants(runtime, row["item_id"], rows)
    return _response(runtime, {"ok": True, "stored": len(rows)})


async def _recipients(runtime, row):
    """Everyone an item's shares cover right now, with their bundle, grouped
    by share: what the owner's client seals a (new) item key to."""
    out = []
    for share in await _shares(runtime, row["item_id"]):
        if share["target_kind"] == "user":
            bundle = await _bundle_of(runtime, share["target_bi"])
            members = [{"name": share["target_name"], "bundle": bundle}]
        else:
            members = [_public_member(m) for m in await _org_members(runtime, share["target_bi"])
                       if m["bi"] != row["owner_bi"]]
        out.append({"kind": share["target_kind"], "name": share["target_name"],
                    "permission": share["permission"], "members": members})
    return out


async def _pending(runtime, account_bi):
    """Organization members who joined, or made a vault, after an item was
    shared with their organization: the owner's client seals to them on its
    next unlock, because only a holder of the item key can."""
    rows = await runtime.d1_all(
        "SELECT s.item_id,s.target_bi,s.target_name,s.permission FROM vault_shares s "
        "JOIN vault_items i ON i.item_id=s.item_id "
        "WHERE i.owner_bi=? AND s.target_kind='organization' LIMIT 500", account_bi)
    pending, rosters = [], {}
    for row in rows or []:
        org_bi = str(row["target_bi"])
        if org_bi not in rosters:
            rosters[org_bi] = await _org_members(runtime, org_bi)
        held = await runtime.d1_all(
            "SELECT recipient_bi FROM vault_grants WHERE item_id=? AND via_kind='organization' "
            "AND via_bi=?", row["item_id"], org_bi)
        have = {str(entry.get("recipient_bi")) for entry in held or []}
        missing = [_public_member(m) for m in rosters[org_bi]
                   if m["bundle"] and m["bi"] != account_bi and m["bi"] not in have]
        if missing:
            pending.append({"itemId": row["item_id"], "organization": row["target_name"],
                            "permission": row["permission"], "members": missing})
    return pending


# --- lifecycle --------------------------------------------------------------------------


async def account_has_data(runtime, account_bi):
    return bool(await _account_row(runtime, account_bi))


async def account_installed(runtime, account_bi):
    return bool(await _account_row(runtime, account_bi))


async def export_account_data(runtime, account_bi, actor):
    """Everything the account stored, still encrypted: an export is only
    useful together with the master password or the recovery code."""
    row = await _account_row(runtime, account_bi)
    account = await _account_data(runtime, row) if row else {}
    owned, _shared, _bundles = await _list_items(runtime, account_bi)
    return {
        "format": "forkmesh-vault-export-v1", "suite": SUITE, "account": actor,
        "exportedAt": runtime.now(), "kdf": account.get("kdf"),
        "wrappedKeyset": account.get("wrappedKeyset"),
        "wrappedRecovery": account.get("wrappedRecovery"),
        "bundle": account.get("bundle"),
        "items": [{"id": item["id"], "body": item["body"], "wrappedKey": item["wrappedKey"],
                   "revision": item["revision"], "updatedAt": item["updatedAt"]}
                  for item in owned],
    }


async def purge_account_data(runtime, account_bi, account_name=""):
    """Remove an account's vault, the items it owned, and every share to it."""
    del account_name
    rows = await runtime.d1_all(
        "SELECT item_id FROM vault_items WHERE owner_bi=?", account_bi)
    for row in rows or []:
        await _delete_item_rows(runtime, str(row["item_id"]))
    await runtime.d1_run("DELETE FROM vault_grants WHERE recipient_bi=?", account_bi)
    await runtime.d1_run(
        "DELETE FROM vault_shares WHERE target_kind='user' AND target_bi=?", account_bi)
    await runtime.d1_run("DELETE FROM vault_accounts WHERE account_bi=?", account_bi)


async def _uninstall(runtime, account_bi, actor):
    export = await export_account_data(runtime, account_bi, actor)
    await purge_account_data(runtime, account_bi, actor)
    await runtime.audit(actor, "plugin.uninstall", "account_plugin", actor + ":" + PLUGIN_ID)
    return _response(runtime, {
        "ok": True, "uninstalled": True, "plugin": PLUGIN_ID,
        "filename": "forkmesh-vault-" + actor + ".json", "export": export})


def _allowed_methods(parts):
    if not parts:
        return ("GET",)
    head = parts[0]
    if len(parts) == 1:
        return {
            "install": ("POST",), "uninstall": ("POST",), "keys": ("PUT",),
            "recovery": ("GET",), "trust": ("PUT",), "items": ("GET", "POST"),
            "organizations": ("GET",), "pending": ("GET",), "export": ("GET",),
        }.get(head, ())
    if head == "users" and len(parts) == 2:
        return ("GET",)
    if head == "organizations" and len(parts) == 3 and parts[2] == "members":
        return ("GET",)
    if head == "items" and len(parts) == 2:
        return ("PUT", "DELETE")
    if head == "items" and len(parts) == 3:
        return {"shares": ("POST",), "unshare": ("POST",), "grants": ("POST",),
                "recipients": ("GET",)}.get(parts[2], ())
    return ()


async def handle(runtime, path):
    parts = _route(path)
    if parts is None:
        return _error(runtime, "not_found", 404)
    method = runtime.method()
    allowed = _allowed_methods(parts)
    if not allowed:
        return _error(runtime, "not_found", 404)
    if method not in allowed:
        return _response(runtime, {"error": "method_not_allowed"}, 405, ", ".join(allowed))
    if method not in ("GET", "HEAD") and not runtime.same_origin():
        return _error(runtime, "origin_not_allowed", 403)
    data = {}
    if method not in ("GET", "HEAD"):
        data, error = await runtime.json_body(MAX_BODY_BYTES)
        if error:
            return _error(runtime, error, 413 if error == "payload_too_large" else 400)
    await runtime.ensure_schema()
    account_bi, actor = await _actor(runtime, data)
    if not account_bi or not actor or not NAME_RE.fullmatch(actor):
        return _error(runtime, "invalid_session", 401)
    await _ensure_tables(runtime)
    head = parts[0] if parts else ""
    if head == "install":
        return await _install(runtime, account_bi, actor, data)
    account = await _account_row(runtime, account_bi)
    if not account:
        if not parts:
            return _response(runtime, {"ok": True, "installed": False, "actor": actor,
                                       "suite": SUITE})
        return _response(runtime, {"error": "plugin_not_installed", "canInstall": True}, 409)
    if not parts:
        return _response(runtime, {
            "ok": True, "installed": True, "actor": actor, "suite": SUITE,
            "account": await _project_account(runtime, account),
            "limits": {"items": MAX_ITEMS, "sharesPerItem": MAX_SHARES_PER_ITEM}})
    if head == "uninstall":
        return await _uninstall(runtime, account_bi, actor)
    if head == "keys":
        return await _rewrap(runtime, account, actor, data)
    if head == "recovery":
        stored = await _account_data(runtime, account)
        await runtime.audit(actor, "vault.recovery.read", "vault_account", actor)
        return _response(runtime, {
            "ok": True, "wrappedRecovery": str(stored.get("wrappedRecovery") or "")})
    if head == "trust":
        return await _save_trust(runtime, account, data)
    if head == "export":
        return _response(runtime, {
            "ok": True, "filename": "forkmesh-vault-" + actor + ".json",
            "export": await export_account_data(runtime, account_bi, actor)})
    if head == "organizations":
        if len(parts) == 1:
            return _response(runtime, {
                "ok": True, "organizations": await _organizations(runtime, account_bi)})
        if not ORG_RE.fullmatch(parts[1]):
            return _error(runtime, "organization_not_found", 404)
        org_bi, role = await runtime.organization(parts[1], account_bi)
        if not org_bi or not role:
            return _error(runtime, "organization_not_found", 404)
        members = await _org_members(runtime, org_bi)
        if len(members) > MAX_ORG_MEMBERS:
            return _error(runtime, "organization_too_large", 409)
        return _response(runtime, {
            "ok": True, "organization": parts[1],
            "members": [_public_member(m) for m in members if m["bi"] != account_bi]})
    if head == "users":
        name = parts[1].lower()
        if not NAME_RE.fullmatch(name):
            return _error(runtime, "recipient_not_found", 404)
        target_bi, target_name = await runtime.account(name)
        if not target_bi:
            return _error(runtime, "recipient_not_found", 404)
        bundle = await _bundle_of(runtime, target_bi)
        if not bundle:
            return _error(runtime, "recipient_has_no_vault", 409)
        return _response(runtime, {"ok": True, "name": target_name, "bundle": bundle})
    if head == "pending":
        return _response(runtime, {"ok": True, "pending": await _pending(runtime, account_bi)})
    # head == "items"
    if len(parts) == 1:
        if method == "POST":
            return await _create_item(runtime, account_bi, actor, data)
        owned, shared, bundles = await _list_items(runtime, account_bi)
        return _response(runtime, {"ok": True, "items": owned, "shared": shared,
                                   "bundles": bundles})
    if not ID_RE.fullmatch(parts[1]):
        return _error(runtime, "item_not_found", 404)
    row = await _item_row(runtime, parts[1])
    if not row:
        return _error(runtime, "item_not_found", 404)
    if len(parts) == 2:
        if method == "DELETE":
            return await _delete_item(runtime, row, account_bi, actor)
        return await _update_item(runtime, row, account_bi, actor, data)
    # Shares, grants, and the recipient roster are the owner's alone; to
    # anyone else the item does not exist.
    if row["owner_bi"] != account_bi:
        return _error(runtime, "item_not_found", 404)
    if parts[2] == "shares":
        return await _share(runtime, row, account_bi, actor, data)
    if parts[2] == "unshare":
        return await _unshare(runtime, row, account_bi, actor, data)
    if parts[2] == "grants":
        return await _add_grants(runtime, row, account_bi, data)
    return _response(runtime, {"ok": True, "recipients": await _recipients(runtime, row)})
