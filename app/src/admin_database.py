"""On-demand D1 database manager for the desktop administrator.

`/api/accounts/admin-database` is the desktop's Database tab: a live table
inventory annotated with the D1 audit (issue #682), paged row reads with sealed
`data` blobs decrypted, and audited insert/update/delete. It is loaded only
when that route runs, so none of it is parsed during Worker startup.

Every request carries an administrator's Ed25519 signature over the exact
request. Writes are bounded by the same restricted-table boundary as the web
console (`admin_console.ADMIN_HIDDEN_TABLES`): identity, custody, mirror,
reward and audit-evidence tables stay behind their purpose-built APIs. Each
write is optimistic (an update or single delete must name the row digest it
was read at), single-use (the request id is recorded in the sensitive audit
log before anything changes) and audited with content-free digests.
"""

import hashlib
import json
import re
from urllib.parse import parse_qs, urlparse


def _bind_runtime(runtime):
    """Supply the entrypoint primitives used by the database manager."""
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


ADMIN_DATABASE_READ_CONTEXT = "forkmesh-admin-database-read-v1"
ADMIN_DATABASE_WRITE_CONTEXT = "forkmesh-admin-database-write-v1"
# The query parameters a read signature covers, in canonical order. The
# desktop builds the same sequence (forkmesh::d1DatabaseReadCanonical).
ADMIN_DATABASE_READ_FIELDS = (
    "op", "table", "cursor", "limit", "filterColumn", "filterMode",
    "filterValue", "order",
)
ADMIN_DATABASE_PAGE_LIMIT = 200
# A sealed `data` blob costs an AES-GCM round-trip per row.
ADMIN_DATABASE_SEALED_PAGE_LIMIT = 50
ADMIN_DATABASE_PHONE_PAGE_LIMIT = 50
ADMIN_DATABASE_DELETE_MAX = 500
ADMIN_DATABASE_VALUE_MAX = 64 * 1024
ADMIN_DATABASE_DATA_MAX = 256 * 1024
ADMIN_DATABASE_COLUMN_CHUNK = 50
ADMIN_DATABASE_INVENTORY_TTL_MS = 5 * 60 * 1000
_REQUEST_ID_RE = re.compile(r"^[A-Za-z0-9_-]{16,64}$")

# Readable in the generic browser, but not writable through it: the account
# blob has its own audited editor, and the rest are migration bookkeeping or
# the only record of the 2026-07-23 custody scrub.
ADMIN_DATABASE_WRITE_BLOCKED = {
    "users": "Accounts are edited through the audited user editor.",
    "d1_migrations": "Cloudflare's migration ledger; change it with a "
                     "migration, never by hand.",
    "schema_meta": "Schema fingerprint bookkeeping; ensure_schema owns it.",
    "legacy_custody_migration_audit": "Audit record of the custody scrub.",
    "legacy_custody_reconciliation": "Audit record of the custody scrub.",
}

ADMIN_DATABASE_DELETE_ONLY = {
    "repositories": "Catalog rows are published by the owner's node; they "
                    "can only be deleted here, which purges the repository.",
}
ADMIN_DATABASE_REPOSITORY_DELETE_MAX = 50

# Findings of the production D1 audit (issue #682, measured 2026-09-10). The
# live inventory joins them onto the tables that still exist, so the desktop
# shows what the cleanup plan says about a table next to its real row count.
ADMIN_DATABASE_AUDIT = "#682 (D1 audit 2026-09-10)"
ADMIN_DATABASE_AUDIT_FINDINGS = {
    "commit_inbox": (
        "drop", "No code references since migration 0009; dead."),
    "forkmesh_d1_migrations": (
        "drop", "Pre-wrangler migration tracker; d1_migrations is live."),
    "system_status_minute": (
        "drop-after-readers",
        "Superseded by *_batch; the dual-read in _status_minute_records "
        "still queries it. Remove the read, then drop."),
    "system_status_hourly": (
        "drop-after-readers",
        "Superseded by system_status_hourly_batch and drained, but "
        "_status_hour_records still reads it. Remove the read, then drop."),
    "system_status_daily": (
        "drop-after-readers",
        "Superseded by system_status_daily_batch and drained, but "
        "_status_day_records still reads it. Remove the read, then drop."),
    "telemetry": (
        "drop-after-export",
        "Plaintext desktop crash summaries with no reader since b417261a1 "
        "(2026-07-25). Export, confirm with the owner, then drop."),
    "host_presence": (
        "drop-after-readers",
        "No writer since repository sockets retired; entry.py readers still "
        "send ~9.9k queries a week against an empty table."),
    "bounty_wallet": (
        "custody", "Legacy custody; drop once the custody record is signed "
                    "off."),
    "central_fund": (
        "custody", "Legacy custody; drop once the custody record is signed "
                    "off."),
    "issue_bounty": (
        "custody", "Legacy custody; bounty routes in entry.py and ledger.py "
                    "still read and write it."),
    "federated_signup": (
        "custody", "Custody-era sign-up path."),
    "legacy_custody_migration_audit": (
        "custody", "Audit record of the 2026-07-23 scrub; export before "
                    "dropping."),
    "legacy_custody_reconciliation": (
        "custody", "Audit record of the 2026-07-23 scrub; export before "
                    "dropping."),
    "funds_received": (
        "custody", "Historical deposit records; export before dropping."),
    "relay_self": (
        "keep", "Live: _relay_identity seals the relay signing key here on "
                "first federated call. Do not drop."),
    "world_element_purchases": (
        "encryption-gap",
        "deposit_secret is sealed with encrypt_row, but a failed sweep "
        "keeps the sealed seed until a later sweep succeeds."),
    "repositories": (
        "hot", "Public catalog full scan was ~15% of all rows read; now "
               "served from public_catalog_snapshot."),
    "users": (
        "hot", "data averages 8.4 KB on the most-run statement in the "
               "database."),
    "sensitive_audit_log": (
        "hot", "5.3 MB, mostly per-ping organization.task_agent_status "
               "rows."),
}
for _runtime_only_table in (
        "agent_prompts", "ai_ask_rate", "catalog_rev", "clone_sticky",
        "issue_seq", "login_attempts", "online_hourly_nodes", "relay_self",
        "repo_agents", "repo_deletions", "repository_monitor_state",
        "signup_rate"):
    ADMIN_DATABASE_AUDIT_FINDINGS.setdefault(_runtime_only_table, (
        "runtime-only",
        "Created only by schema.py at runtime; needs a catch-up migration so "
        "a fresh database built from migrations has it."))
for _plugin_table in (
        "crm_contacts", "crm_email_sends", "crm_mail_messages",
        "crm_settings", "marketing_ads", "marketing_content",
        "marketing_experiments", "marketing_feed_items", "marketing_funnel",
        "marketing_metrics", "marketing_reddit_threads", "marketing_settings"):
    ADMIN_DATABASE_AUDIT_FINDINGS.setdefault(_plugin_table, (
        "plugin", "Owned by a marketplace plugin's lifecycle, not app "
                  "migrations."))
del _runtime_only_table, _plugin_table

# A plaintext column whose name says it holds a credential. Hashes, digests,
# identifiers and timestamps about a secret are not the secret.
_SECRET_COLUMN_RE = re.compile(
    r"(secret|password|passwd|private|seed|mnemonic|token|api_?key)")
_SECRET_COLUMN_SAFE_SUFFIXES = (
    "_bi", "_hash", "_digest", "_sha256", "_id", "_at", "_ts", "_count",
    "_kind", "_hint", "_status", "_expires", "_prefix",
)

_INVENTORY_MEMO = {"at": 0, "value": None}


def _qi(name):
    return '"' + str(name).replace('"', '""') + '"'


def _now():
    return int(Date.now())


def _no_store(payload, status=200):
    return json_response(
        payload, status=status,
        cache_control="no-store, max-age=0, must-revalidate")


def _hidden_tables():
    return set(_admin_console.ADMIN_HIDDEN_TABLES)


def admin_database_read_canonical(node, ts, fields):
    """Signed text for a read: context, node, ts, then each read field."""
    parts = [ADMIN_DATABASE_READ_CONTEXT, node, ts]
    parts.extend(str(fields.get(name, "")) for name in ADMIN_DATABASE_READ_FIELDS)
    return "\n".join(parts).encode("utf-8")


def admin_database_write_canonical(node, ts, request_id, payload_text):
    """Signed text for a write: the payload travels as an opaque string, so
    the signature covers its exact bytes instead of a re-serialization."""
    digest = hashlib.sha256(payload_text.encode("utf-8")).hexdigest()
    return "\n".join((
        ADMIN_DATABASE_WRITE_CONTEXT, node, ts, request_id, digest,
    )).encode("utf-8")


def secret_named_plaintext_columns(columns):
    """Column names that look like stored credentials outside a sealed blob."""
    flagged = []
    for column in columns:
        lowered = str(column).lower()
        if lowered == "data" or lowered.endswith(_SECRET_COLUMN_SAFE_SUFFIXES):
            continue
        if _SECRET_COLUMN_RE.search(lowered):
            flagged.append(column)
    return flagged


def storage_model(columns):
    names = {str(c).lower() for c in columns}
    has_blob = "data" in names
    has_index = any(n.endswith("_bi") for n in names)
    if has_blob and has_index:
        return "sealed+indexed"
    if has_blob:
        return "sealed"
    if has_index:
        return "indexed"
    return "plaintext"


def _looks_plaintext_json(value):
    return isinstance(value, str) and value[:1] in ("{", "[")


def row_digest(row):
    """Content digest of a raw D1 row, as read, for optimistic writes."""
    canonical = json.dumps(
        row, sort_keys=True, separators=(",", ":"), default=str)
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()


def _json_safe(value):
    if value is None or isinstance(value, (bool, int, float, str)):
        return value
    if isinstance(value, (bytes, bytearray)):
        return {"$bytes": len(value)}
    return str(value)


async def _table_catalog(env):
    rows = await d1_all(
        env,
        "SELECT name, sql FROM sqlite_master WHERE type='table' "
        "AND name NOT LIKE 'sqlite_%' AND name NOT LIKE '_cf_%' ORDER BY name",
    )
    catalog = {}
    for row in rows or []:
        name = str(row.get("name") or "")
        if name:
            catalog[name] = str(row.get("sql") or "")
    return catalog


def _without_rowid(create_sql):
    return "without rowid" in " ".join(str(create_sql).lower().split())


async def _table_columns(env, table):
    rows = await d1_all(env, "PRAGMA table_info(" + _qi(table) + ")")
    columns = []
    for row in rows or []:
        name = row.get("name")
        if not name:
            continue
        default = row.get("dflt_value")
        columns.append({
            "name": str(name),
            "type": str(row.get("type") or ""),
            "primaryKey": bool(row.get("pk")),
            "notNull": bool(row.get("notnull")),
            "hasDefault": default is not None,
            "default": None if default is None else str(default),
        })
    return columns


async def _inventory_columns(env):
    """Every table's columns in one statement; empty when D1 refuses the
    table-valued pragma, in which case column-derived findings are skipped."""
    try:
        rows = await d1_all(
            env,
            "SELECT m.name AS t, p.name AS c, p.type AS type "
            "FROM sqlite_master m, pragma_table_info(m.name) p "
            "WHERE m.type='table' AND m.name NOT LIKE 'sqlite_%' "
            "AND m.name NOT LIKE '_cf_%' ORDER BY m.name, p.cid",
        )
    except Exception:
        return None
    columns = {}
    for row in rows or []:
        table = str(row.get("t") or "")
        if table:
            columns.setdefault(table, []).append(
                {"name": str(row.get("c") or ""),
                 "type": str(row.get("type") or "")})
    return columns


async def _plaintext_data_tables(env, tables):
    """Tables whose first stored `data` value is JSON text, not ciphertext.

    One row per table, a chunk of tables per statement. The audit found six
    such tables by hand; this keeps the check live."""
    plaintext = set()
    for start in range(0, len(tables), ADMIN_DATABASE_COLUMN_CHUNK):
        chunk = tables[start:start + ADMIN_DATABASE_COLUMN_CHUNK]
        terms = [
            "SELECT * FROM (SELECT '%s' AS t, substr(data,1,1) AS c FROM %s "
            "WHERE data IS NOT NULL AND data<>'' LIMIT 1)"
            % (table.replace("'", "''"), _qi(table))
            for table in chunk
        ]
        try:
            rows = await d1_all(env, " UNION ALL ".join(terms))
        except Exception:
            continue
        for row in rows or []:
            if str(row.get("c") or "") in ("{", "["):
                plaintext.add(str(row.get("t") or ""))
    return plaintext


def _write_block_reason(table, catalog, hidden, op=""):
    if table in hidden:
        return "Restricted: this table is changed only through its own " \
               "audited API."
    if table in ADMIN_DATABASE_DELETE_ONLY and op != "delete":
        return ADMIN_DATABASE_DELETE_ONLY[table]
    if table in ADMIN_DATABASE_WRITE_BLOCKED:
        return ADMIN_DATABASE_WRITE_BLOCKED[table]
    if _without_rowid(catalog.get(table, "")):
        return "WITHOUT ROWID table: rows have no stable rowid to edit by."
    return ""


async def _inventory(env, refresh=False):
    now = _now()
    memo = _INVENTORY_MEMO
    if (not refresh and memo["value"] is not None
            and now - memo["at"] < ADMIN_DATABASE_INVENTORY_TTL_MS):
        return memo["value"]
    catalog = await _table_catalog(env)
    names = sorted(catalog)
    hidden = _hidden_tables()
    counts = await _admin_table_counts_cached(env, names, refresh=refresh)
    columns = await _inventory_columns(env)
    sealed_tables = [
        name for name in names
        if name not in hidden and columns is not None
        and any(c["name"] == "data" for c in columns.get(name, []))
    ]
    plaintext_data = await _plaintext_data_tables(env, sealed_tables)
    tables = []
    categories = {}
    for name in names:
        table_columns = (columns or {}).get(name, [])
        column_names = [c["name"] for c in table_columns]
        gaps = []
        for column in secret_named_plaintext_columns(column_names):
            gaps.append("plaintext secret-named column: " + column)
        if name in plaintext_data:
            gaps.append("data column holds plaintext JSON, not ciphertext")
        block = _write_block_reason(name, catalog, hidden)
        category, note = ADMIN_DATABASE_AUDIT_FINDINGS.get(name, ("", ""))
        if category:
            categories[category] = categories.get(category, 0) + 1
        tables.append({
            "name": name,
            "rows": int(counts.get(name, 0) or 0),
            "restricted": name in hidden,
            "writable": not block,
            "deletable": not _write_block_reason(
                name, catalog, hidden, "delete"),
            "writeBlockedReason": block,
            "rowid": not _without_rowid(catalog[name]),
            "storage": storage_model(column_names) if columns is not None
            else "",
            "plaintextData": name in plaintext_data,
            "columns": table_columns,
            "gaps": gaps,
            "auditCategory": category,
            "auditNote": note,
        })
    missing = sorted(
        name for name in ADMIN_DATABASE_AUDIT_FINDINGS
        if name not in catalog
        and ADMIN_DATABASE_AUDIT_FINDINGS[name][0].startswith("drop"))
    value = {
        "ok": True,
        "sampledAt": now,
        "audit": ADMIN_DATABASE_AUDIT,
        "columnsAvailable": columns is not None,
        "tables": tables,
        "auditCategories": categories,
        # Drop candidates that no longer exist: the cleanup already landed.
        "droppedSinceAudit": missing,
    }
    memo["at"] = now
    memo["value"] = value
    return value


def _inventory_forget():
    _INVENTORY_MEMO["at"] = 0
    _INVENTORY_MEMO["value"] = None


def _filter_clause(mode, column, value, column_names):
    if not column:
        return "", []
    if column != "rowid" and column not in column_names:
        raise ValueError("unknown_filter_column")
    target = "rowid" if column == "rowid" else _qi(column)
    if mode == "null":
        return target + " IS NULL", []
    if mode == "contains":
        escaped = (value.replace("\\", "\\\\").replace("%", "\\%")
                   .replace("_", "\\_"))
        return ("CAST(" + target + " AS TEXT) LIKE ? ESCAPE '\\'",
                ["%" + escaped + "%"])
    if mode in ("", "equals", "blind"):
        return target + "=?", [value]
    raise ValueError("unknown_filter_mode")


async def _read_rows(env, fields):
    table = fields["table"]
    catalog = await _table_catalog(env)
    if table not in catalog:
        return _no_store({"error": "unknown_table"}, 404)
    if table in _hidden_tables():
        return _no_store({
            "error": "restricted",
            "detail": "This table is restricted to its purpose-built API.",
        }, 403)
    columns = await _table_columns(env, table)
    column_names = [c["name"] for c in columns]
    sealed = "data" in column_names
    has_rowid = not _without_rowid(catalog[table])
    cap = (ADMIN_DATABASE_SEALED_PAGE_LIMIT if sealed
           else ADMIN_DATABASE_PAGE_LIMIT)
    try:
        limit = max(1, min(int(fields.get("limit") or cap), cap))
    except ValueError:
        limit = cap
    mode = fields.get("filterMode", "")
    value = fields.get("filterValue", "")
    if mode == "blind":
        if not fields.get("filterColumn", "").endswith("_bi"):
            return _no_store({"error": "blind_filter_needs_bi_column"}, 400)
        value = await blind_index(env, value)
    try:
        where, args = _filter_clause(
            mode, fields.get("filterColumn", ""), value, column_names)
    except ValueError as error:
        return _no_store({"error": str(error)}, 400)
    ascending = fields.get("order") == "asc"
    clauses = [where] if where else []
    cursor = fields.get("cursor", "")
    offset = 0
    if has_rowid:
        if cursor:
            try:
                cursor_value = int(cursor)
            except ValueError:
                return _no_store({"error": "bad_cursor"}, 400)
            clauses.append("rowid>?" if ascending else "rowid<?")
            args.append(cursor_value)
        select = "SELECT rowid AS _rowid_, * FROM " + _qi(table)
        order = " ORDER BY rowid " + ("ASC" if ascending else "DESC")
    else:
        # No rowid to key on: page by offset, which D1 bills as rows read.
        try:
            offset = max(0, int(cursor or 0))
        except ValueError:
            return _no_store({"error": "bad_cursor"}, 400)
        select = "SELECT * FROM " + _qi(table)
        order = ""
    sql = (select + (" WHERE " + " AND ".join(clauses) if clauses else "")
           + order + " LIMIT ?")
    args.append(limit + 1)
    if not has_rowid:
        sql += " OFFSET ?"
        args.append(offset)
    raw_rows = await d1_all(env, sql, *args)
    more = len(raw_rows) > limit
    raw_rows = raw_rows[:limit]
    rows = []
    for raw in raw_rows:
        rowid = raw.get("_rowid_") if has_rowid else None
        values = {}
        entry = {"rowid": rowid, "digest": row_digest(raw)}
        for key, item in raw.items():
            if key == "_rowid_":
                continue
            if key == "data" and isinstance(item, str) and item:
                entry["dataBytes"] = len(item)
                if _looks_plaintext_json(item):
                    try:
                        decoded = json.loads(item)
                        entry["dataMode"] = "plaintext"
                    except ValueError:
                        decoded = None
                        entry["dataMode"] = "unreadable"
                else:
                    decoded = await decrypt_row(env, item)
                    entry["dataMode"] = (
                        "sealed" if decoded is not None else "unreadable")
                if decoded is not None:
                    entry["walletKeys"] = bool(
                        _admin_contains_wallet_key(decoded))
                    entry["data"] = _admin_redact_wallet_keys(decoded)
                values[key] = None
                continue
            values[key] = _json_safe(item)
        entry["values"] = values
        rows.append(entry)
    next_cursor = ""
    if more and rows:
        next_cursor = (str(rows[-1]["rowid"]) if has_rowid
                       else str(offset + limit))
    return _no_store({
        "ok": True,
        "table": table,
        "columns": columns,
        "rowid": has_rowid,
        "rows": rows,
        "limit": limit,
        "nextCursor": next_cursor,
    })


async def _read_schema(env, table):
    catalog = await _table_catalog(env)
    if table not in catalog:
        return _no_store({"error": "unknown_table"}, 404)
    rows = await d1_all(
        env,
        "SELECT type, name, sql FROM sqlite_master "
        "WHERE tbl_name=? AND sql IS NOT NULL ORDER BY type DESC, name",
        table,
    )
    return _no_store({
        "ok": True,
        "table": table,
        "objects": [
            {"type": str(r.get("type") or ""), "name": str(r.get("name") or ""),
             "sql": str(r.get("sql") or "")}
            for r in rows or []
        ],
    })


async def admin_database_handler(env, request):
    method = str(getattr(request, "method", "GET") or "GET").upper()
    if method == "GET":
        return await _handle_read(env, request)
    if method == "POST":
        return await _handle_write(env, request)
    return _no_store({"error": "method_not_allowed"}, 405)


async def _handle_read(env, request):
    params = parse_qs(urlparse(request.url).query, keep_blank_values=True)

    def param(name, limit=512):
        return clean_string(params.get(name, [""])[0], limit)

    node = param("node", MAX_NODE_NAME).strip().lower()
    ts = param("ts", 20)
    sig = param("sig", 200)
    fields = {name: param(name) for name in ADMIN_DATABASE_READ_FIELDS}
    canonical = admin_database_read_canonical(node, ts, fields)
    if not await _admin_authorized(env, node, ts, sig, canonical):
        return _no_store({"error": "unauthorized"}, 401)
    op = fields["op"] or "inventory"
    try:
        if op == "inventory":
            return _no_store(await _inventory(
                env, refresh=bool(param("refresh", 8))))
        if op == "rows":
            return await _read_rows(env, fields)
        if op == "schema":
            return await _read_schema(env, fields["table"])
    except Exception as error:
        return _no_store(
            {"error": "d1_rejected", "detail": str(error)[:300]}, 400)
    return _no_store({"error": "unknown_op"}, 400)


async def _phone_admin(env, request):
    try:
        authorization = str(request.headers.get("authorization") or "")
    except Exception:
        return ""
    if not authorization.lower().startswith("bearer "):
        return ""
    actor = await _authed_account_name(env, request)
    return actor if actor and await _is_admin(env, actor) else ""


async def admin_database_phone_handler(env, request):
    method = str(getattr(request, "method", "GET") or "GET").upper()
    if method != "GET":
        return _no_store({"error": "method_not_allowed"}, 405)
    if not await _phone_admin(env, request):
        return _no_store({"error": "unauthorized"}, 401)
    params = parse_qs(urlparse(request.url).query, keep_blank_values=True)

    def param(name, limit=512):
        return clean_string(params.get(name, [""])[0], limit)

    fields = {name: param(name) for name in ADMIN_DATABASE_READ_FIELDS}
    op = fields["op"] or "inventory"
    try:
        if op == "inventory":
            return _no_store(await _inventory(
                env, refresh=bool(param("refresh", 8))))
        if op == "rows":
            try:
                limit = int(fields["limit"] or ADMIN_DATABASE_PHONE_PAGE_LIMIT)
            except ValueError:
                limit = ADMIN_DATABASE_PHONE_PAGE_LIMIT
            fields["limit"] = str(
                max(1, min(limit, ADMIN_DATABASE_PHONE_PAGE_LIMIT)))
            return await _read_rows(env, fields)
    except Exception as error:
        return _no_store(
            {"error": "d1_rejected", "detail": str(error)[:300]}, 400)
    return _no_store({"error": "unknown_op"}, 400)


def _scalar(value):
    if isinstance(value, bool):
        return 1 if value else 0
    if value is None or isinstance(value, (int, float)):
        return value
    if isinstance(value, str):
        if len(value) > ADMIN_DATABASE_VALUE_MAX:
            raise ValueError("value_too_large")
        return value
    raise ValueError("unsupported_value")


async def _data_mode(env, table, existing):
    if isinstance(existing, str) and existing:
        return "plaintext" if _looks_plaintext_json(existing) else "sealed"
    sample = await d1_first(
        env,
        "SELECT substr(data,1,1) AS c FROM " + _qi(table)
        + " WHERE data IS NOT NULL AND data<>'' LIMIT 1",
    )
    return ("plaintext" if str((sample or {}).get("c") or "") in ("{", "[")
            else "sealed")


async def _assignments(env, table, payload, column_names, existing_data):
    """Validated (columns, values) for an insert or update."""
    names, values = [], []
    for column, value in dict(payload.get("values") or {}).items():
        if column not in column_names:
            raise ValueError("unknown_column:" + str(column)[:64])
        if column == "data" or column.endswith("_bi"):
            raise ValueError("column_needs_its_own_field:" + column)
        names.append(column)
        values.append(_scalar(value))
    for column, plaintext in dict(payload.get("blind") or {}).items():
        if column not in column_names or not column.endswith("_bi"):
            raise ValueError("blind_index_needs_bi_column:" + str(column)[:64])
        if not isinstance(plaintext, str) or not plaintext:
            raise ValueError("blind_index_needs_plaintext:" + column)
        names.append(column)
        values.append(await blind_index(env, plaintext))
    if "data" in payload:
        if "data" not in column_names:
            raise ValueError("table_has_no_data_column")
        data = payload.get("data")
        if not isinstance(data, (dict, list)):
            raise ValueError("data_must_be_json_object_or_array")
        if _admin_contains_wallet_key(data):
            raise ValueError("wallet_private_key_fields_are_not_accepted")
        serialized = json.dumps(data)
        if len(serialized) > ADMIN_DATABASE_DATA_MAX:
            raise ValueError("data_too_large")
        mode = await _data_mode(env, table, existing_data)
        names.append("data")
        values.append(serialized if mode == "plaintext"
                      else await encrypt_row(env, data))
    return names, values


async def _replayed(env, actor, request_id, now):
    actor_bi = await blind_index(env, actor)
    token = await blind_index(env, "audit:database_request:" + request_id)
    row = await d1_first(
        env,
        "SELECT 1 AS hit FROM sensitive_audit_log "
        "WHERE actor_bi=? AND ts>=? AND target_token=? LIMIT 1",
        actor_bi, now - 2 * LOGIN_MAX_SKEW_MS, token,
    )
    return bool(row)


async def _handle_write(env, request):
    try:
        body = json.loads(await request.text() or "{}")
    except Exception:
        return _no_store({"error": "invalid_json"}, 400)
    if not isinstance(body, dict):
        return _no_store({"error": "invalid_json"}, 400)
    node = clean_string(body.get("node", ""), MAX_NODE_NAME).strip().lower()
    ts = clean_string(str(body.get("ts", "")), 20)
    sig = clean_string(body.get("sig", ""), 200)
    request_id = str(body.get("requestId") or "")
    payload_text = body.get("payload")
    if not isinstance(payload_text, str) or not _REQUEST_ID_RE.match(request_id):
        return _no_store({"error": "invalid_request"}, 400)
    canonical = admin_database_write_canonical(
        node, ts, request_id, payload_text)
    if not await _admin_authorized(env, node, ts, sig, canonical):
        return _no_store({"error": "unauthorized"}, 401)
    try:
        payload = json.loads(payload_text)
    except ValueError:
        return _no_store({"error": "invalid_payload"}, 400)
    if not isinstance(payload, dict):
        return _no_store({"error": "invalid_payload"}, 400)
    op = str(payload.get("op") or "")
    table = str(payload.get("table") or "")
    if op not in ("insert", "update", "delete"):
        return _no_store({"error": "unknown_op"}, 400)

    now = _now()
    if await _replayed(env, node, request_id, now):
        return _no_store({"error": "replayed_request"}, 409)
    catalog = await _table_catalog(env)
    if table not in catalog:
        return _no_store({"error": "unknown_table"}, 404)
    action = "admin.database_" + op
    block = _write_block_reason(table, catalog, _hidden_tables(), op)
    audit_details = {"table": table}
    if block:
        await _audit_sensitive_action(
            env, node, action, "database_request", request_id, "denied",
            audit_details)
        return _no_store({"error": "write_blocked", "detail": block}, 403)
    # The request id is spent before anything changes, so a replay inside the
    # signature window finds it even if this write fails part-way.
    await _audit_sensitive_action(
        env, node, action, "database_request", request_id, "requested",
        audit_details)
    try:
        result, status = await _apply_write(
            env, node, op, table, payload, audit_details)
    except ValueError as error:
        result, status = {"error": str(error)[:200]}, 400
    except Exception as error:
        result, status = {
            "error": "d1_rejected", "detail": str(error)[:300]}, 400
    outcome = ("success" if status == 200
               else "denied" if status in (403, 409) else "failed")
    await _audit_sensitive_action(
        env, node, action, "database_request", request_id, outcome,
        audit_details)
    if status == 200:
        await _admin_table_counts_forget()
        _inventory_forget()
    return _no_store(result, status)


async def _apply_write(env, actor, op, table, payload, audit_details):
    if op == "delete":
        return await _apply_delete(env, actor, table, payload, audit_details)
    column_names = [c["name"] for c in await _table_columns(env, table)]
    if op == "insert":
        names, values = await _assignments(
            env, table, payload, column_names, None)
        audit_details["columnCount"] = len(names)
        if names:
            sql = ("INSERT INTO " + _qi(table) + " ("
                   + ",".join(_qi(n) for n in names) + ") VALUES ("
                   + ",".join(["?"] * len(names)) + ") RETURNING rowid AS _rowid_")
        else:
            sql = ("INSERT INTO " + _qi(table)
                   + " DEFAULT VALUES RETURNING rowid AS _rowid_")
        row = await d1_first(env, sql, *values)
        rowid = (row or {}).get("_rowid_")
        audit_details["rowCount"] = 1
        return {"ok": True, "op": op, "table": table, "rowid": rowid}, 200

    try:
        rowid = int(payload.get("rowid"))
    except (TypeError, ValueError):
        return {"error": "rowid_required"}, 400
    before = await d1_first(
        env,
        "SELECT rowid AS _rowid_, * FROM " + _qi(table) + " WHERE rowid=?",
        rowid,
    )
    if not before:
        return {"error": "row_not_found"}, 404
    digest = row_digest(before)
    audit_details["rowDigestBefore"] = digest
    if str(payload.get("digest") or "") != digest:
        return {"error": "row_changed",
                "detail": "The row changed since it was read; reload it."}, 409
    existing_data = before.get("data")
    if "data" in payload and isinstance(existing_data, str) and existing_data:
        decoded = (json.loads(existing_data)
                   if _looks_plaintext_json(existing_data)
                   else await decrypt_row(env, existing_data))
        if decoded is None:
            return {"error": "data_unreadable",
                    "detail": "The stored blob does not decrypt; it cannot be "
                              "edited without losing it."}, 403
        if _admin_contains_wallet_key(decoded):
            return {"error": "wallet_custody_frozen",
                    "detail": "This row holds legacy wallet custody material; "
                              "use the offline migration."}, 403
    names, values = await _assignments(
        env, table, payload, column_names, existing_data)
    if not names:
        return {"error": "nothing_to_change"}, 400
    audit_details["columnCount"] = len(names)
    await d1_run(
        env,
        "UPDATE " + _qi(table) + " SET "
        + ",".join(_qi(n) + "=?" for n in names) + " WHERE rowid=?",
        *values, rowid,
    )
    audit_details["rowCount"] = 1
    return {"ok": True, "op": op, "table": table, "rowid": rowid}, 200


async def _apply_delete(env, actor, table, payload, audit_details):
    try:
        rowids = sorted({int(x) for x in (payload.get("rowids") or [])})
    except (TypeError, ValueError):
        return {"error": "rowids_must_be_integers"}, 400
    if not rowids:
        return {"error": "no_rows_selected"}, 400
    if len(rowids) > ADMIN_DATABASE_DELETE_MAX:
        return {"error": "too_many_rows"}, 400
    if (table == "repositories"
            and len(rowids) > ADMIN_DATABASE_REPOSITORY_DELETE_MAX):
        return {"error": "too_many_rows",
                "detail": "Delete at most %d repositories at a time."
                          % ADMIN_DATABASE_REPOSITORY_DELETE_MAX}, 400
    expected = str(payload.get("digest") or "")
    if len(rowids) == 1 and expected:
        before = await d1_first(
            env,
            "SELECT rowid AS _rowid_, * FROM " + _qi(table) + " WHERE rowid=?",
            rowids[0],
        )
        if not before:
            return {"error": "row_not_found"}, 404
        if row_digest(before) != expected:
            return {"error": "row_changed",
                    "detail": "The row changed since it was read; reload "
                              "it."}, 409
    if await _admin_selected_rows_have_wallet_keys(env, table, rowids):
        return {"error": "wallet_custody_frozen",
                "detail": "A selected row holds legacy wallet custody "
                          "material; use the offline migration."}, 403
    audit_details["rowCount"] = len(rowids)
    audit_details["rowDigestBefore"] = await _admin_selected_rows_digest(
        env, table, rowids)
    if table == "repositories":
        purged = await admin_purge_repository_rows(env, rowids, actor)
        return {"ok": True, "op": "delete", "table": table,
                "rowCount": purged}, 200
    for start in range(0, len(rowids), 90):
        batch = rowids[start:start + 90]
        await d1_run(
            env,
            "DELETE FROM " + _qi(table) + " WHERE rowid IN ("
            + ",".join(["?"] * len(batch)) + ")",
            *batch,
        )
    return {"ok": True, "op": "delete", "table": table,
            "rowCount": len(rowids)}, 200


async def _repository_row_names(env, row):
    try:
        record = await decrypt_row(env, row.get("data") or "")
    except Exception:
        record = None
    if not isinstance(record, dict):
        return "", ""
    owner = safe_segment(str(record.get("owner") or ""))
    name = safe_segment(str(record.get("name") or ""))
    if not owner or not name:
        return "", ""
    if await blind_index(env, owner + "/" + name) != row.get("key_bi"):
        return "", ""
    return owner, name


async def admin_purge_repository_rows(env, rowids, actor):
    purged = 0
    for start in range(0, len(rowids), 90):
        batch = rowids[start:start + 90]
        rows = await d1_all(
            env,
            "SELECT key_bi, owner_bi, data FROM repositories WHERE rowid IN ("
            + ",".join(["?"] * len(batch)) + ")",
            *batch,
        )
        for row in rows or []:
            key_bi = str(row.get("key_bi") or "")
            if not key_bi:
                continue
            owner, name = await _repository_row_names(env, row)
            target = owner + "/" + name if owner else key_bi
            if owner:
                await invalidate_public_catalog_snapshots(env, owner, name)
            await _record_repo_delete_tombstone(
                env, key_bi, str(row.get("owner_bi") or ""))
            await _delete_repo_scoped_state(env, key_bi)
            if owner:
                await _delete_repo_named_state(env, owner, name)
            await d1_run(
                env, "DELETE FROM repositories WHERE key_bi=?", key_bi)
            if owner:
                await _delete_bounties_namespace(env, owner, name)
                await d1_run(
                    env, "DELETE FROM chat_history WHERE room_key LIKE ?",
                    "repo:" + owner + "/" + name + ":room:%")
                await d1_run(
                    env,
                    "DELETE FROM funds_received WHERE scope='project' "
                    "AND key=?",
                    owner + "/" + name)
                await schedule_public_catalog_republish(env, owner, name)
            await _audit_sensitive_action(
                env, actor, "repository.delete", "repository", target,
                "success", {"deleted": True, "admin": True,
                            "via": "database"})
            purged += 1
    if purged:
        await purge_catalog_related_caches()
        await _bump_catalog_rev(env)
    return purged
